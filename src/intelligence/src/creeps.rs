//! One state per spawn cohort and lane. A map route and the engine spawn
//! clock drive forecasts; visible units correct their own team, never a mirror.
use crate::{abi::*, lanes};
use std::collections::{BTreeMap, BTreeSet};
type PackKey = (u32, u32, u64);
const PERIOD: u64 = 30000;
const SPEED: f32 = 325.;
#[derive(Clone, Copy)]
struct Unit {
    position: Vec3,
    at: u64,
    pack: Option<PackKey>,
    visible: bool,
    visibility_at: u64,
}
#[derive(Clone, Copy)]
struct Pack {
    depart_at: u64,
    progress: f32,
    at: u64,
    seen: u64,
    speed: f32,
    contact: Option<u64>,
    center: Vec3,
    anchor_progress: f32,
    direct: bool,
    stationary_since: Option<u64>,
    motion: Option<(Vec3, u64)>,
}
impl Pack {
    fn position(&self, team: u32, lane: u32, now: u64) -> Vec3 {
        let dt = now.saturating_sub(self.at.max(self.depart_at)).min(2000) as f32 / 1000.;
        let travel = if self.contact.is_some() || self.stationary_since.is_some() {
            0.
        } else {
            self.speed * dt
        };
        let mut position = lanes::point(team, lane, self.progress + travel);
        if self.direct {
            // The formation offset is relative to the observed route position,
            // never to the advancing prediction. Otherwise blending pulls the
            // entire wave back toward stale coordinates after losing vision.
            let anchor = lanes::point(team, lane, self.anchor_progress);
            let weight = if self.stationary_since.is_some() {
                1.
            } else {
                (1. - now.saturating_sub(self.seen) as f32 / 4000.).clamp(0., 1.)
            };
            position.x += (self.center.x - anchor.x) * weight;
            position.y += (self.center.y - anchor.y) * weight;
            position.z += (self.center.z - anchor.z) * weight;
        }
        position
    }
}
#[derive(Clone, Copy)]
struct Clock {
    start: u64,
    at: u64,
    enabled: bool,
    spawn_disabled: bool,
    generation: u64,
    forecast_from: u64,
    units_from: u64,
}
#[derive(Default)]
pub struct Creeps {
    units: BTreeMap<u32, Unit>,
    packs: BTreeMap<PackKey, Pack>,
    retired: BTreeMap<PackKey, u64>,
    clock: Option<Clock>,
    speeds: BTreeMap<(u32, u32), f32>,
    base_speeds: BTreeMap<(u32, u32), f32>,
    epoch: u64,
}
#[derive(Clone, Copy)]
pub struct Wave {
    pub position: Vec3,
    pub key: u64,
    pub observed: u64,
    pub direct: bool,
    pub visible: bool,
}
impl Creeps {
    fn speed(&self, team: u32, lane: u32) -> f32 {
        self.speeds
            .get(&(team, lane))
            .or_else(|| self.speeds.get(&(5 - team, lane)))
            .copied()
            .unwrap_or(SPEED)
    }
    pub fn clock(&mut self, e: &Observation) {
        if self.clock.is_some_and(|c| c.at > e.at_ms) {
            return;
        }
        // Match 0:00 is the schedule origin. The native spawn field can be
        // a negative epoch or drift slightly; it must not delay :00/:30 waves.
        let start = (e.values[1] * 1000.).round() as u64;
        let valid_start = e.values[1].is_finite() && e.values[1] >= 0. && start <= e.at_ms + 120000;
        let enabled =
            valid_start && e.flags & (ACTIVE | KNOWN) == (ACTIVE | KNOWN) && e.values[2] == 5.;
        let restart = enabled
            && self.clock.is_some_and(|c| {
                c.spawn_disabled
                    || (c.enabled && c.generation != 0 && e.key != 0 && c.generation != e.key)
            });
        let previous = self.clock;
        if !enabled || restart {
            self.units.clear();
            self.packs.clear();
            self.retired.clear();
            self.speeds.clear();
            self.base_speeds.clear();
        }
        if restart {
            self.epoch = self.epoch.wrapping_add(1);
        }
        self.clock = Some(Clock {
            start,
            at: e.at_ms,
            enabled,
            spawn_disabled: valid_start
                && e.flags & KNOWN != 0
                && e.flags & ACTIVE == 0
                && e.values[2] == 5.,
            // A transient unavailable read cannot erase the last revision and
            // hide a change when the next consistent sample arrives.
            generation: if e.key != 0 {
                e.key
            } else {
                previous.map_or(0, |c| c.generation)
            },
            // No timer can prove a wave from before the schedule resumed.
            forecast_from: if restart {
                e.at_ms
            } else {
                previous.map_or(start, |c| c.forecast_from)
            },
            // The transition happened after the preceding sample. Accept newly
            // created physical generations, but reject stale replicated bodies.
            units_from: if restart {
                previous.unwrap().at
            } else {
                previous.map_or(0, |c| c.units_from)
            },
        });
    }
    fn phase(&self, at: u64) -> u64 {
        let Some(c) = self.clock else {
            return at / PERIOD * PERIOD;
        };
        c.start + at.saturating_sub(c.start) / PERIOD * PERIOD
    }

    pub fn simulate(&mut self, now: u64) {
        if let Some(c) = self
            .clock
            .filter(|c| c.enabled && now.saturating_sub(c.at) < 3000 && now >= c.start)
        {
            let last = self.phase(now);
            for age in 0..5 {
                let Some(born) = last.checked_sub(age * PERIOD) else {
                    break;
                };
                if born < c.start || born < c.forecast_from {
                    continue;
                }
                for team in 2..=3 {
                    for lane in 0..3 {
                        let key = (team, lane, born);
                        if self.retired.contains_key(&key) {
                            continue;
                        }
                        let speed = self.speed(team, lane);
                        // Do not invent all waves from the last two minutes when
                        // attaching mid-match. Unobserved cohorts have a short
                        // survival budget after the nominal first meeting.
                        if (now - born) as f32 > lanes::length(team, lane) * 500. / speed + 12000. {
                            continue;
                        }
                        self.packs.entry(key).or_insert(Pack {
                            depart_at: born + lanes::departure_delay(team, lane),
                            progress: now.saturating_sub(born + lanes::departure_delay(team, lane))
                                as f32
                                / 1000.
                                * speed,
                            at: now,
                            seen: 0,
                            speed,
                            contact: None,
                            center: Vec3::default(),
                            anchor_progress: 0.,
                            direct: false,
                            stationary_since: None,
                            motion: None,
                        });
                    }
                }
            }
        }
        // Catch up simulation time after throttled/background frames with
        // bounded substeps. A per-frame dt cap silently lost Alt+Tab time.
        let speeds: Vec<_> = (2..=3)
            .flat_map(|team| (0..3).map(move |lane| (team, lane)))
            .map(|(team, lane)| ((team, lane), self.speed(team, lane)))
            .collect();
        for (&(team, lane, _), p) in &mut self.packs {
            p.speed = speeds.iter().find(|(k, _)| *k == (team, lane)).unwrap().1;
        }
        let mut tick = self
            .packs
            .values()
            .map(|p| p.at)
            .min()
            .unwrap_or(now)
            .max(now.saturating_sub(150000));
        while tick < now {
            tick = (tick + 250).min(now);
            for (&(team, lane, born), p) in &mut self.packs {
                if born > tick || p.at >= tick {
                    continue;
                }
                let dt = tick.saturating_sub(p.at.max(p.depart_at)) as f32 / 1000.;
                if p.contact.is_none() && p.stationary_since.is_none() {
                    p.progress += p.speed * dt;
                }
                p.progress = p.progress.min(lanes::length(team, lane));
                p.at = tick;
            }
            self.contacts(tick);
        }
    }
    fn contacts(&mut self, now: u64) {
        let positions: Vec<_> = self
            .packs
            .iter()
            .filter(|(k, p)| k.2 <= now && p.at <= now)
            .map(|(k, p)| (*k, p.position(k.0, k.1, now)))
            .collect();
        for (key, pos) in &positions {
            let engaged = positions.iter().any(|(other, q)| {
                other.0 != key.0 && other.1 == key.1 && lanes::distance(*pos, *q) < 700.
            });
            if let Some(p) = self.packs.get_mut(key) {
                if engaged {
                    p.contact.get_or_insert(now);
                } else {
                    // Contact is a relationship, not a permanent stop flag.
                    // A dead/retired/moving opposing cohort releases this one.
                    p.contact = None;
                }
            }
        }
    }

    pub fn observe(&mut self, e: &Observation) {
        if self.clock.is_some_and(|c| !c.enabled || e.at_ms < c.start) {
            return;
        }
        if e.flags & CREATION_TIME_KNOWN != 0
            && e.auxiliary.x.is_finite()
            && self
                .clock
                .is_some_and(|c| e.auxiliary.x * 1000. < c.units_from as f32)
        {
            return;
        }
        if e.actor == 0
            || e.actor == u32::MAX
            || self
                .units
                .get(&e.actor)
                .is_some_and(|u| u.visibility_at > e.at_ms)
        {
            return;
        }
        if !self.units.contains_key(&e.actor)
            && let Some((&old, u)) = self
                .units
                .iter()
                .find(|(h, _)| **h & 0x7fff == e.actor & 0x7fff)
        {
            if u.at >= e.at_ms {
                return;
            }
            if let Some(old_unit) = self.units.remove(&old) {
                self.retire_member(old_unit.pack, e.at_ms);
            }
        }
        if e.flags & ALIVE == 0 {
            if let Some(u) = self.units.remove(&e.actor) {
                self.retire_member(u.pack, e.at_ms);
            }
            return;
        }
        let team = e.values[0] as u32;
        if !(2..=3).contains(&team) {
            return;
        }
        let old = self.units.get(&e.actor).copied();
        let lane = old
            .and_then(|u| u.pack)
            .map_or_else(|| lanes::classify(team, e.position), |pack| pack.1);
        // Creation time is spawn provenance, not a current position. The engine
        // prepares the next cohort during the preceding 30-second interval.
        // The first batch is created just after the horn (one server tick/unit).
        let prepared = self.clock.filter(|c| c.enabled).and_then(|c| {
            let created = (e.auxiliary.x * 1000.).round() as u64;
            (e.flags & CREATION_TIME_KNOWN != 0
                && e.auxiliary.x.is_finite()
                && e.auxiliary.x >= 0.
                && created <= e.at_ms + 100
                && created + PERIOD >= c.start)
                .then(|| {
                    (
                        team,
                        lane,
                        c.start + created.saturating_sub(c.start + 500).div_ceil(PERIOD) * PERIOD,
                    )
                })
        });
        let near_spawn = lanes::distance(e.position, lanes::route(team, lane)[0]) < 300.;
        // Bind hidden identities only at their known spawn formation. Frozen
        // coordinates never correct progress, speed, centroid or combat state.
        if e.flags & (POSITION | VISIBLE) != (POSITION | VISIBLE) {
            if let Some(unit) = self.units.get_mut(&e.actor) {
                unit.visible = false;
                unit.visibility_at = e.at_ms;
            } else if near_spawn && let Some(pack) = prepared {
                self.units.insert(
                    e.actor,
                    Unit {
                        position: Vec3::default(),
                        at: 0,
                        pack: Some(pack),
                        visible: false,
                        visibility_at: e.at_ms,
                    },
                );
            }
            return;
        }
        if old.is_some_and(|u| u.at >= e.at_ms) {
            return;
        }
        let staged = self.clock.is_some()
            && near_spawn
            && old
                .and_then(|u| u.pack)
                .is_none_or(|key| self.packs.get(&key).is_none_or(|p| !p.direct));
        if staged {
            self.units.insert(
                e.actor,
                Unit {
                    position: e.position,
                    at: e.at_ms,
                    pack: prepared.or_else(|| old.and_then(|u| u.pack)),
                    visible: true,
                    visibility_at: e.at_ms,
                },
            );
            return;
        }
        if (200. ..=650.).contains(&e.values[1]) {
            // The base stat is not measured transit speed. Reset the latter
            // only when the stat actually changes, not for every unit/poll.
            let previous = self.base_speeds.insert((team, lane), e.values[1]);
            if previous.is_none_or(|old| (old - e.values[1]).abs() > 1.) {
                self.speeds.insert((team, lane), e.values[1]);
            }
        }
        let (progress, _) = lanes::project(team, lane, e.position);
        let released = old.is_some_and(|u| u.pack.is_none());
        let key = old
            .and_then(|u| u.pack)
            .filter(|pack| pack.2 <= e.at_ms)
            .or(prepared.filter(|pack| pack.2 <= e.at_ms))
            .or_else(|| released.then(|| (team, lane, self.phase(e.at_ms))))
            .unwrap_or_else(|| {
                // All melee, ranged and siege members attach to an existing lane
                // cohort, even when discovery spans a second or they stretch out.
                self.packs
                    .iter()
                    .filter(|((t, l, _), _)| *t == team && *l == lane)
                    .map(|(k, p)| (*k, (p.progress - progress).abs()))
                    .filter(|(_, d)| *d < 2400.)
                    .min_by(|a, b| a.1.total_cmp(&b.1))
                    .map(|v| v.0)
                    .unwrap_or((
                        team,
                        lane,
                        self.phase(e.at_ms.saturating_sub((progress / SPEED * 1000.) as u64)),
                    ))
            });
        let speed = self.speed(team, lane);
        self.packs.entry(key).or_insert(Pack {
            depart_at: 0,
            progress,
            at: e.at_ms,
            seen: 0,
            speed,
            contact: None,
            center: e.position,
            anchor_progress: progress,
            direct: true,
            stationary_since: None,
            motion: None,
        });
        self.retired.remove(&key); // A real visible generation outranks an estimate of death.
        self.units.insert(
            e.actor,
            Unit {
                position: e.position,
                at: e.at_ms,
                pack: Some(key),
                visible: true,
                visibility_at: e.at_ms,
            },
        );
    }
    fn retire_member(&mut self, pack: Option<PackKey>, now: u64) {
        if let Some(pack) = pack
            && !self.units.values().any(|u| u.pack == Some(pack))
        {
            self.packs.remove(&pack);
            self.retired.insert(pack, now + 150000);
        }
    }
    pub fn reconcile(&mut self, now: u64) {
        // Recompute the centroid once for the complete batch, not once per
        // creep. A straggler can neither create its own dot nor reset the pack.
        let mut samples: BTreeMap<PackKey, Vec<(Vec3, u64)>> = BTreeMap::new();
        for u in self
            .units
            .values()
            .filter(|u| u.visible && now.saturating_sub(u.at) < 750)
        {
            if let Some(pack) = u.pack {
                samples.entry(pack).or_default().push((u.position, u.at));
            }
        }
        for (key, points) in samples {
            let Some(p) = self.packs.get_mut(&key) else {
                continue;
            };
            let mut center = Vec3::default();
            let mut at = 0;
            for (pos, t) in &points {
                center.x += pos.x;
                center.y += pos.y;
                center.z += pos.z;
                at = at.max(*t);
            }
            let n = points.len() as f32;
            center.x /= n;
            center.y /= n;
            center.z /= n;
            let progress = lanes::project(key.0, key.1, center).0;
            if at > p.seen {
                // Replication can repeat a position across several 50 ms polls.
                // Confirm motion over a time window, not a single equal sample.
                match p.motion {
                    Some((origin, started)) if at.saturating_sub(started) >= 400 => {
                        let dt = at.saturating_sub(started) as f32 / 1000.;
                        if lanes::distance(center, origin) < 40. * dt {
                            p.stationary_since.get_or_insert(at);
                        } else {
                            p.stationary_since = None;
                        }
                        // Include unchanged replication samples in the elapsed
                        // time. Learning only from positive per-unit jumps
                        // overestimates speed when positions arrive in bursts.
                        let measured = (progress - lanes::project(key.0, key.1, origin).0) / dt;
                        let base = self
                            .base_speeds
                            .get(&(key.0, key.1))
                            .copied()
                            .unwrap_or(SPEED);
                        if dt <= 1.5 && (base * 0.75..=base * 1.15).contains(&measured) {
                            let estimate = self.speeds.entry((key.0, key.1)).or_insert(base);
                            *estimate = *estimate * 0.8 + measured * 0.2;
                        }
                        p.motion = Some((center, at));
                    }
                    None => p.motion = Some((center, at)),
                    _ => {}
                }
                p.depart_at = 0;
                p.progress = progress;
                p.center = center;
                p.anchor_progress = progress;
                p.at = now;
                p.seen = at;
                p.direct = true;
                p.speed = self.speeds.get(&(key.0, key.1)).copied().unwrap_or(SPEED);
            }
        }
        self.contacts(now);
        let retired: Vec<_> = self
            .packs
            .iter()
            .filter_map(|(key, p)| {
                let gone = now.saturating_sub(p.seen) > 12000
                    && (p.contact.is_some_and(|at| now.saturating_sub(at) > 12000)
                        || p.stationary_since
                            .is_some_and(|at| now.saturating_sub(at) > 12000));
                (gone
                    || now.saturating_sub(key.2) > 150000
                    || p.progress >= lanes::length(key.0, key.1))
                .then_some(*key)
            })
            .collect();
        for key in retired {
            self.packs.remove(&key);
            self.retired.insert(key, now + 150000);
        }
    }
    pub fn expire(&mut self, now: u64) {
        self.units
            .retain(|_, u| now.saturating_sub(u.visibility_at) <= 20000);
        self.retired.retain(|_, until| *until > now);
        while self.units.len() > 512 {
            let k = *self.units.iter().min_by_key(|(_, u)| u.at).unwrap().0;
            self.units.remove(&k);
        }
    }
    pub fn waves(&self, now: u64, local_team: u32) -> Vec<Wave> {
        let mut groups: Vec<(PackKey, Vec3, u32, u64, bool, bool)> = Vec::new();
        for (&key, p) in self
            .packs
            .iter()
            .filter(|(key, _)| key.0 != local_team && key.2 <= now)
        {
            let position = p.position(key.0, key.1, now);
            let visible = self.units.values().any(|unit| {
                unit.pack == Some(key)
                    && unit.visible
                    && now.saturating_sub(unit.visibility_at) <= 500
            });
            if let Some(g) = groups
                .iter_mut()
                .find(|(k, pos, _, _, _, _)| k.1 == key.1 && lanes::distance(*pos, position) < 750.)
            {
                let n = g.2 as f32;
                g.1.x = (g.1.x * n + position.x) / (n + 1.);
                g.1.y = (g.1.y * n + position.y) / (n + 1.);
                g.1.z = (g.1.z * n + position.z) / (n + 1.);
                g.2 += 1;
                g.3 = g.3.max(p.seen);
                g.4 |= p.direct;
                g.5 |= visible;
            } else {
                groups.push((key, position, 1, p.seen, p.direct, visible));
            }
        }
        let mut used = BTreeSet::new();
        groups
            .into_iter()
            .filter_map(|(key, position, _, observed, direct, visible)| {
                let id = 0xc000000000000000
                    | (u64::from(key.0) << 56)
                    | (u64::from(key.1) << 48)
                    | (key.2 ^ (self.epoch.wrapping_mul(0x9e3779b97f4a7c15) & 0x0000ffffffffffff));
                used.insert(id).then_some(Wave {
                    position,
                    key: id,
                    observed,
                    direct,
                    visible,
                })
            })
            .take(18)
            .collect()
    }
}
