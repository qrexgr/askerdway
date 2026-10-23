//! The only owner of inference state. Native code submits facts, never markers.
use crate::{abi::*, catalog};
use std::collections::BTreeMap;

#[derive(Clone, Default)]
struct Actor {
    enemy: Enemy,
    visible: bool,
    alive: bool,
    neutral: bool,
    seen: u64,
    body: Option<(Vec3, u64)>,
    last_visible: Option<(Vec3, u64)>,
    attack_at: u64,
    sampled_position: Option<(Vec3, u64)>,
    position_changed_at: u64,
    // The displayed age belongs to a position episode, not a polling tick.
    history_anchor: Option<(Vec3, u64, Name)>,
    body_episode: Option<(Name, u64)>,
}
struct Source {
    owner: u32,
    name: Name,
    stamps: [f32; 3],
    activated: Option<u64>,
    phase: bool,
    seen: u64,
    cooldown: Option<f32>,
    normalized: String,
    behavior: u32,
}
#[derive(Default)]
struct ModifierClock {
    owner: u32,
    creation: f32,
    tick: f32,
    seen: u64,
}
#[derive(Default)]
struct ScrollClock {
    value: Scroll,
    item: u32,
    seen: u64,
    charges_at: Option<u64>,
    cooldown_at: Option<u64>,
    last_remaining: Option<f32>,
    charges_changed_at: Option<u64>,
    cooldown_length: Option<u64>,
    travel_boots: bool,
    consumed_cast: u64,
    estimated_charges: Option<i32>,
    estimated_cooldown: Option<u64>,
}
#[derive(Default)]
struct TeleportRoute {
    value: Route,
    subtype: u32,
    authoritative: bool,
    last_at: u64,
    event_key: u64,
    position_published: bool,
    endpoint_key: u64,
    endcap_at: Option<u64>,
}
struct Emitter {
    source: u32,
    seen: u64,
    started: u64,
    correction: Option<(Vec3, Vec3)>,
}
#[derive(Default)]
pub struct World {
    session: u64,
    now: u64,
    team: u32,
    initialized: bool,
    actors: BTreeMap<u32, Actor>,
    bindings: BTreeMap<u32, (u32, u64)>,
    sources: BTreeMap<u32, Source>,
    source_index: BTreeMap<u32, Vec<u32>>,
    modifiers: BTreeMap<(u32, u64), ModifierClock>,
    markers: BTreeMap<(u32, u64), Marker>,
    routes: BTreeMap<u32, TeleportRoute>,
    camps: BTreeMap<u32, Camp>,
    scrolls: BTreeMap<u32, ScrollClock>,
    dedup: BTreeMap<(u32, u64), u64>,
    rejected: u32,
    dropped: u32,
    terminal: BTreeMap<(u32, u64), u64>,
    particle_terminal: BTreeMap<u32, u64>,
    stopped_channels: BTreeMap<u32, (u64, u64)>,
    emitters: BTreeMap<(u32, u64), Emitter>,
    launches: BTreeMap<(u32, u64), (Vec3, u64)>,
    creeps: crate::creeps::Creeps,
    obstacles: BTreeMap<u32, (Vec3, u64)>,
}
impl World {
    pub fn attachment_is_frozen(&self, actor: u32, model: Vec3) -> bool {
        self.actors.get(&actor).is_some_and(|a| {
            a.alive
                && !a.visible
                && a.enemy.team != self.team
                && self.now.saturating_sub(a.seen) <= 1500
                && self.now.saturating_sub(a.position_changed_at) >= 200
                && a.sampled_position
                    .is_some_and(|(p, _)| (p.x - model.x).hypot(p.y - model.y) < 16.)
        })
    }
    pub fn advance(&mut self, f: &Frame, events: &[Observation]) -> Result<Snapshot, u32> {
        if f.size != std::mem::size_of::<Frame>() as u32
            || f.version != ABI
            || events.len() > MAX_EVENTS
            || f.game_ms > 10_000_000_000
            || !(2..=3).contains(&f.local_team)
        {
            return Err(2);
        }
        if !self.initialized
            || self.session != f.session
            || self.team != f.local_team
            || f.game_ms < self.now
        {
            *self = Self::default();
            self.session = f.session;
            self.team = f.local_team;
            self.initialized = true;
        }
        self.now = f.game_ms;
        self.dropped = self.dropped.saturating_add(f.reserved);
        self.expire();
        self.emitters
            .retain(|_, v| self.now.saturating_sub(v.seen) <= 1500);
        // A batch is a fact set, not an order-dependent call script. Resolve
        // identities first, then membership, then events, then presentation.
        // Within each phase, replay capture order so a delayed baseline cannot
        // overwrite newer state or hide a real transition in the same batch.
        // The input cap bounds this scratch allocation to MAX_EVENTS pointers.
        let mut ordered: Vec<_> = events.iter().collect();
        ordered.sort_by_key(|e| e.at_ms);
        let events = ordered;
        for e in events.iter().filter(|e| e.kind == WAVE_CLOCK) {
            if self.valid(e) {
                self.creeps.clock(e);
            }
        }
        self.creeps.simulate(self.now);
        for e in events.iter().filter(|e| e.kind == ENTITY) {
            if self.valid(e) {
                self.entity(e, f);
            }
        }
        self.creeps.reconcile(self.now);
        for e in events.iter().filter(|e| e.kind == BINDING) {
            if self.valid(e) {
                self.binding(e);
            }
        }
        for e in events.iter().filter(|e| e.kind == SOURCE) {
            if self.valid(e) {
                self.source(e, f);
            }
        }
        // Reuse per-owner vectors. New effects should inspect only this
        // actor's sources, not the entire world's ability population.
        self.source_index.retain(|owner, handles| {
            handles.clear();
            self.actors.contains_key(owner)
        });
        for (&handle, source) in &self.sources {
            self.source_index
                .entry(source.owner)
                .or_default()
                .push(handle);
        }
        // A readable stopped channel wins over late particle endpoints, and a
        // structured active channel wins over its particle aliases regardless
        // of input order. Cancellation must be terminal within this fact set.
        for phase in 0..5 {
            for e in events.iter().filter(|e| {
                if phase == 2 {
                    return e.kind == TELEPORT_ITEM;
                }
                e.kind == TELEPORT
                    && match phase {
                        0 => e.flags & ACTIVE == 0,
                        1 => e.flags & ACTIVE != 0 && e.flags & PARTICLE_TELEPORT == 0,
                        3 => {
                            e.flags & ACTIVE != 0
                                && e.flags & PARTICLE_TELEPORT != 0
                                && e.flags & ENDPOINT_ANCHOR == 0
                        }
                        _ => e.flags & ACTIVE != 0 && e.flags & ENDPOINT_ANCHOR != 0,
                    }
            }) {
                if self.valid(e) {
                    if e.kind == TELEPORT_ITEM {
                        self.teleport_item(e);
                    } else if e.flags & ENDPOINT_ANCHOR != 0 {
                        self.correlate_endpoint(e);
                    } else {
                        self.teleport(e);
                    }
                }
            }
        }
        // Projectile identity is an independent engine fact. Make launches
        // available before resolving ownerless particle aliases in this batch.
        for e in events.iter().filter(|e| e.kind == OBSTACLE) {
            if self.valid(e) {
                self.obstacles.insert(e.actor, (e.position, e.at_ms));
            }
        }
        for e in events.iter().filter(|e| e.kind == PROJECTILE) {
            if !self.valid(e) || e.values[0] < 0. || e.values[0] > 750. {
                continue;
            }
            let Some(owner) = self.owner(e.actor) else {
                continue;
            };
            let at = e.at_ms.saturating_sub(e.values[0] as u64);
            if self.launches.contains_key(&(owner, e.key)) {
                continue;
            }
            self.launches.insert((owner, e.key), (e.position, at));
            self.record_position(owner, e.position, at, name(""), 8, f);
        }
        for e in events.iter().filter(|e| {
            !matches!(
                e.kind,
                ENTITY | BINDING | SOURCE | TELEPORT | TELEPORT_ITEM | PROJECTILE | OBSTACLE
            )
        }) {
            if !self.valid(e) {
                continue;
            }
            match e.kind {
                PARTICLE => self.particle(e, f),
                MODIFIER => self.modifier(e, f),
                CAMP => {
                    if e.position.valid()
                        && e.auxiliary.valid()
                        && e.position.x < e.auxiliary.x
                        && e.position.y < e.auxiliary.y
                        && e.position.z < e.auxiliary.z
                    {
                        self.camps.insert(
                            e.actor,
                            Camp {
                                id: e.actor,
                                kind: e.values[0] as u32,
                                minimum: e.position,
                                maximum: e.auxiliary,
                                ..Camp::default()
                            },
                        );
                    }
                }
                DROP => {
                    if f.flags & CAMPS != 0 && e.position.valid() {
                        self.mark(
                            0,
                            e.key,
                            e.position,
                            4,
                            0,
                            self.now,
                            350,
                            name(""),
                            e.name,
                            true,
                        );
                    }
                }
                SCROLL => self.scroll(e),
                _ => self.rejected = self.rejected.saturating_add(1),
            }
        }
        self.reconcile_teleports(f);
        self.infer_scroll_casts();
        if f.flags & TRACES == 0 {
            self.markers.retain(|_, v| v.kind == 4);
        } else {
            self.markers.retain(|_, v| {
                v.owner == 0
                    || self
                        .actors
                        .get(&v.owner)
                        .is_some_and(|a| !a.visible && a.alive)
            });
        }
        if f.flags & CAMPS == 0 {
            self.markers.retain(|_, v| v.kind != 4);
        }
        self.bound();
        Ok(self.snapshot(f))
    }
    fn valid(&mut self, e: &Observation) -> bool {
        let valid = e.at_ms <= self.now
            && self.now - e.at_ms <= 3000
            && e.values.iter().all(|v| v.is_finite())
            && e.position.valid()
            && e.auxiliary.valid();
        if !valid {
            self.rejected = self.rejected.saturating_add(1);
        }
        valid
    }
    fn expire(&mut self) {
        let now = self.now;
        self.creeps.expire(now);
        self.obstacles
            .retain(|_, v| now.saturating_sub(v.1) < 10000);
        self.markers.retain(|_, v| v.expires_ms > now);
        self.routes.retain(|_, v| {
            v.value.expires_ms > now
                || (v.endpoint_key != 0
                    && v.value.flags & 12 == 0
                    && v.value.started_ms.saturating_add(10500) > now)
        });
        let expired: Vec<_> = self
            .actors
            .iter()
            .filter(|(_, a)| now.saturating_sub(a.seen) > 3000)
            .map(|(handle, _)| *handle)
            .collect();
        for owner in expired {
            self.forget_actor(owner);
        }
        self.sources
            .retain(|_, v| now.saturating_sub(v.seen) <= 1500);
        self.bindings.retain(|_, v| v.1 >= now);
        self.modifiers
            .retain(|_, v| now.saturating_sub(v.seen) <= 3000);
        self.dedup.retain(|_, v| *v > now);
        self.terminal.retain(|_, v| *v > now);
        self.particle_terminal.retain(|_, v| *v > now);
        self.stopped_channels
            .retain(|_, v| now.saturating_sub(v.0) <= 3000);
        self.scrolls
            .retain(|_, v| now.saturating_sub(v.seen) <= 1500);
        self.launches
            .retain(|_, v| now.saturating_sub(v.1) <= 30000);
    }
    fn entity(&mut self, e: &Observation, f: &Frame) {
        if e.actor == 0 || e.actor == u32::MAX {
            self.rejected = self.rejected.saturating_add(1);
            return;
        }
        if e.flags & LANE_CREEP != 0 {
            self.creeps.observe(e);
            return;
        }
        let index = e.actor & 0x7fff;
        let existing = self.actors.get(&e.actor);
        if existing.is_some_and(|a| a.seen > e.at_ms) {
            self.rejected = self.rejected.saturating_add(1);
            return;
        }
        if existing.is_none() {
            // At most one generation per index is retained. Ordinary samples
            // use the map lookup above instead of scanning every actor.
            if let Some((&old, a)) = self.actors.iter().find(|(h, _)| **h & 0x7fff == index) {
                if a.seen >= e.at_ms {
                    self.rejected = self.rejected.saturating_add(1);
                    return;
                }
                self.forget_actor(old);
            }
        }
        let alive = e.flags & ALIVE != 0;
        let visible = e.flags & VISIBLE != 0;
        let known = e.flags & (VISIBLE | HIDDEN) != 0;
        let mut a = self.actors.remove(&e.actor).unwrap_or_default();
        let disappeared = a.visible && known && !visible;
        a.enemy = Enemy {
            handle: e.actor,
            player: e.values[1] as i32,
            team: e.values[0] as u32,
            flags: e.flags,
            health: e.values[2] as i32,
            max_health: e.values[3] as i32,
            hero: e.name,
        };
        a.seen = e.at_ms;
        a.attack_at = e.key;
        a.alive = alive;
        a.neutral = e.flags & NEUTRAL != 0;
        if e.flags & POSITION != 0 {
            if a.sampled_position.is_none_or(|(p, _)| p != e.position) {
                a.position_changed_at = e.at_ms;
            }
            a.sampled_position = Some((e.position, e.at_ms));
        }
        if known {
            a.visible = visible;
        }
        if visible && e.flags & POSITION != 0 && alive {
            a.last_visible = Some((e.position, e.at_ms));
            if a.body.is_none_or(|(_, at)| at <= e.at_ms) {
                a.body = Some((e.position, e.at_ms));
            }
        }
        if !alive {
            a.body = None;
            a.last_visible = None;
            self.markers.retain(|k, _| k.0 != e.actor);
            self.routes.remove(&e.actor);
            self.dedup.remove(&(e.actor, 0));
        }
        if visible {
            a.history_anchor = None;
            a.body_episode = None;
            self.markers.retain(|k, _| k.0 != e.actor);
            // Key zero represents the current visibility episode, not a
            // deduplicated cast. A new reveal ends that episode immediately.
            self.dedup.remove(&(e.actor, 0));
        }
        if disappeared
            && alive
            && f.flags & TRACES != 0
            && a.enemy.team != self.team
            && let Some((position, at)) = a.last_visible
        {
            self.mark(
                e.actor,
                0,
                position,
                1,
                1,
                at,
                u64::from(f.trace_ttl_ms.clamp(3000, 30000)),
                a.enemy.hero,
                name(""),
                false,
            );
        }
        self.actors.insert(e.actor, a);
    }
    fn binding(&mut self, e: &Observation) {
        if e.actor != 0 && e.actor != u32::MAX && e.actor != e.subject && e.subject != u32::MAX {
            let expires = e.at_ms + 1500;
            if expires >= self.now
                && self
                    .bindings
                    .get(&e.actor)
                    .is_none_or(|old| old.1 <= expires)
            {
                self.bindings.insert(e.actor, (e.subject, expires));
            }
        }
    }
    fn owner(&self, mut handle: u32) -> Option<u32> {
        let mut seen = [u32::MAX; 5];
        for i in 0..5 {
            if seen[..i].contains(&handle) {
                return None;
            }
            seen[i] = handle;
            if let Some(a) = self.actors.get(&handle) {
                return (a.enemy.team != self.team
                    && a.enemy.team != 4
                    && a.alive
                    && !a.neutral
                    && a.enemy.flags & HERO != 0)
                    .then_some(handle);
            }
            handle = self.bindings.get(&handle).filter(|v| v.1 >= self.now)?.0;
        }
        None
    }
    fn source(&mut self, e: &Observation, f: &Frame) {
        if e.actor == 0 || e.actor == u32::MAX || self.now - e.at_ms > 1500 {
            self.rejected = self.rejected.saturating_add(1);
            return;
        }
        if self
            .bindings
            .get(&e.actor)
            .is_some_and(|b| b.1 > e.at_ms + 1500)
        {
            self.rejected = self.rejected.saturating_add(1);
            return;
        }
        // Source handles have generations too. A recycled slot must not leave
        // its old membership as a competing attribution candidate.
        let index = e.actor & 0x7fff;
        let existing = self.sources.get(&e.actor);
        if existing.is_some_and(|s| s.seen > e.at_ms) {
            self.rejected = self.rejected.saturating_add(1);
            return;
        }
        if existing.is_none()
            && let Some((&old, s)) = self.sources.iter().find(|(h, _)| **h & 0x7fff == index)
        {
            if s.seen >= e.at_ms {
                self.rejected = self.rejected.saturating_add(1);
                return;
            }
            self.forget_source(old);
        }
        let Some(owner) = self.owner(e.subject) else {
            self.forget_source(e.actor);
            return;
        };
        if self
            .sources
            .get(&e.actor)
            .is_some_and(|s| s.owner != owner || s.name != e.name)
        {
            self.forget_source(e.actor);
        }
        self.bindings.insert(e.actor, (owner, e.at_ms + 1500));
        let stamps = [e.values[0], e.values[1], e.values[2]];
        let phase = e.flags & ACTIVE != 0;
        let previous = self.sources.remove(&e.actor);
        let cooldown = if e.flags & COOLDOWN_KNOWN != 0 {
            Some(e.values[3])
        } else {
            previous.as_ref().and_then(|s| s.cooldown)
        };
        let changed = previous.as_ref().is_some_and(|s| {
            s.owner == owner
                && s.name == e.name
                && ((phase && !s.phase)
                    || (e.flags & COOLDOWN_KNOWN != 0
                        && s.cooldown.is_some_and(|old| e.values[3] > old + 0.05))
                    || stamps.iter().zip(s.stamps).any(|(a, b)| {
                        *a > b + 0.001
                            && *a > 0.
                            && (*a * 1000.) as u64 <= e.at_ms
                            && e.at_ms - (*a * 1000.) as u64 <= 1500
                    }))
        });
        let activated = if changed {
            Some(e.at_ms)
        } else {
            previous.as_ref().and_then(|s| s.activated)
        };
        let behavior = previous
            .as_ref()
            .map_or_else(|| catalog::behavior(text(&e.name)), |s| s.behavior);
        let normalized =
            previous.map_or_else(|| catalog::normalized(text(&e.name)), |s| s.normalized);
        self.sources.insert(
            e.actor,
            Source {
                owner,
                name: e.name,
                stamps,
                activated,
                phase,
                seen: e.at_ms,
                cooldown,
                normalized,
                behavior,
            },
        );
        if changed && f.flags & TRACES != 0 {
            if e.flags & POSITION != 0 {
                // A cast edge authorizes one positional estimate from this
                // exact actor sample. Polling hidden coordinates alone never
                // moves LastPos; remote effect coordinates are not used here.
                self.record_position(owner, e.position, e.at_ms, e.name, 5, f);
                return;
            }
            let a = self.actors[&owner].clone();
            if !a.visible
                && let Some((position, at)) = a.body
                && at <= e.at_ms
                && e.at_ms - at <= 500
            {
                let stamp = stamps.iter().copied().fold(0., f32::max).to_bits();
                self.mark(
                    owner,
                    ((stamp as u64) << 32) | u64::from(e.actor),
                    position,
                    2,
                    2,
                    e.at_ms,
                    2500,
                    a.enemy.hero,
                    e.name,
                    false,
                );
            }
        }
    }
    fn choose_source(
        &self,
        owner: u32,
        resource: &str,
        raw_source: u32,
        at: u64,
        new_control_point: bool,
    ) -> Option<(Name, u32)> {
        let mut selected = None;
        let mut best = 0;
        let mut tied = false;
        let mut fallback = None;
        let mut fallback_best = 0;
        let mut fallback_tied = false;
        let resource_match = catalog::ResourceMatch::new(resource);
        for handle in self.source_index.get(&owner).into_iter().flatten() {
            let Some(s) = self.sources.get(handle) else {
                continue;
            };
            let flags = s.behavior;
            let proc = flags & (1 | 64) != 0 && flags & 32 == 0;
            let attack = self.actors[&owner].attack_at;
            let exact = *handle == raw_source;
            if proc && !exact && (attack == 0 || attack > self.now || self.now - attack > 1500) {
                continue;
            }
            let mut score = resource_match.score(text(&s.name), &s.normalized);
            let activated = !proc && s.activated.is_some_and(|v| v.abs_diff(at) <= 750);
            if score == 0 && activated && new_control_point {
                let candidate = catalog::recent_point_score(
                    resource,
                    text(&s.name),
                    text(&self.actors[&owner].enemy.hero),
                );
                if candidate > fallback_best {
                    fallback_best = candidate;
                    fallback = Some((s.name, *handle));
                    fallback_tied = false;
                } else if candidate > 0 && candidate == fallback_best {
                    fallback_tied = true;
                }
            }
            if activated
                && (score > 0
                    || (catalog::resource_hero(resource)
                        == Some(text(&self.actors[&owner].enemy.hero))
                        && catalog::caster_emitter(resource, text(&s.name))))
            {
                score += 2000;
            }
            if exact {
                score += 4000;
            }
            if score > best {
                best = score;
                selected = Some((s.name, *handle));
                tied = false;
            } else if score > 0 && score == best {
                tied = true;
            }
        }
        if tied {
            None
        } else {
            // Partial names may explain a previously unresolved event, but
            // never overrule an exact source, explicit binding or full name.
            selected.or(if fallback_tied { None } else { fallback })
        }
    }
    fn unique_resource_owner(&self, resource: &str) -> Option<u32> {
        let hero = catalog::resource_hero(resource)?;
        // All observed heroes participate, including allies. Duplicate heroes
        // and illusions make a resource-only identity ambiguous; distance must
        // never break this tie.
        let mut candidates = self
            .actors
            .iter()
            .filter(|(_, a)| a.alive && a.enemy.flags & HERO != 0 && text(&a.enemy.hero) == hero);
        let (&handle, _) = candidates.next()?;
        if candidates.next().is_some() {
            return None;
        }
        self.owner(handle)
    }
    fn particle(&mut self, e: &Observation, f: &Frame) {
        if f.flags & (TRACES | CAMPS) == 0 {
            return;
        }
        let resource = text(&e.resource);
        if catalog::feedback(resource) {
            return;
        }
        let inferred_owner = if matches!(e.actor, 0 | u32::MAX) && e.flags & NEW != 0 {
            self.unique_resource_owner(resource)
        } else {
            None
        };
        let Some(owner) = self.owner(e.actor).or(inferred_owner) else {
            self.rejected = self.rejected.saturating_add(1);
            return;
        };
        let a = self.actors[&owner].clone();
        if a.visible {
            return;
        }
        let cached = if e.flags & NEW == 0
            && let Some(emitter) = self.emitters.get(&(owner, e.key))
            && let Some(s) = self.sources.get(&emitter.source)
            && s.owner == owner
        {
            Some((s.name, emitter.source))
        } else {
            None
        };
        let selected = cached.or_else(|| {
            self.choose_source(
                owner,
                resource,
                e.actor,
                e.at_ms,
                e.flags & (NEW | CONTROL_POINT) == (NEW | CONTROL_POINT),
            )
        });
        let Some((icon, source)) = selected.or_else(|| {
            let resource = catalog::canonical(resource);
            catalog::ATTACKS
                .iter()
                .any(|p| p.0 == resource && p.1 == text(&a.enemy.hero))
                .then_some((name(""), owner))
        }) else {
            return;
        };
        let started = self
            .emitters
            .get(&(owner, e.key))
            .map_or(e.at_ms, |v| v.started);
        let correction = self
            .emitters
            .get(&(owner, e.key))
            .and_then(|v| v.correction);
        self.emitters.insert(
            (owner, e.key),
            Emitter {
                source,
                seen: e.at_ms,
                started,
                correction,
            },
        );
        if e.flags & VISUAL_CORRECTION != 0 && e.flags & NEW == 0 {
            return;
        }
        if e.flags & (NEW | CONTROL_POINT) == (NEW | CONTROL_POINT)
            && let Some((position, at)) = self
                .launches
                .iter()
                .filter(|((h, _), (p, at))| {
                    *h == owner
                        && at.abs_diff(e.at_ms) <= 750
                        && (p.x - e.position.x).hypot(p.y - e.position.y) < 128.
                })
                .map(|(_, v)| *v)
                .max_by_key(|v| v.1)
        {
            self.record_position(owner, position, at, icon, 8, f);
            return;
        }
        let exact_carrier =
            e.actor == owner || self.sources.get(&e.actor).is_some_and(|s| s.owner == owner);
        let frozen_attachment = a
            .sampled_position
            .is_some_and(|(p, _)| (p.x - e.position.x).hypot(p.y - e.position.y) <= 64.);
        // At creation the delivered origin can precede CP attachment to a
        // dormant client model. Only a new exact-carrier self cast can use
        // this anchor, and only when CP0 matches that frozen model. Remote
        // casts, target effects and continued culling updates cannot do so.
        if exact_carrier
            && e.flags & (NEW | CONTROL_POINT) == (NEW | CONTROL_POINT)
            && catalog::self_cast(resource, text(&icon))
            && frozen_attachment
            && (e.auxiliary.x.abs() > 1. || e.auxiliary.y.abs() > 1.)
            && (e.position.x - e.auxiliary.x).hypot(e.position.y - e.auxiliary.y) > 128.
        {
            self.emitters.get_mut(&(owner, e.key)).unwrap().correction =
                Some((e.auxiliary, e.position));
            self.record_position(owner, e.auxiliary, e.at_ms, icon, 5, f);
            return;
        }
        let self_cast = catalog::self_cast(resource, text(&icon));
        let body = exact_carrier
            && (self_cast || catalog::caster_emitter(resource, text(&icon)))
            && e.flags & CONTROL_POINT != 0;
        let position = e.position;
        if body {
            if a.body.is_some_and(|(_, at)| at > e.at_ms)
                || (frozen_attachment
                    && a.last_visible.is_some_and(|(p, at)| {
                        at > a.position_changed_at
                            && (p.x - position.x).hypot(p.y - position.y) > 64.
                    }))
            {
                return;
            }
            // A single positional history is shared by visible samples,
            // body observations, cast evidence and teleport arrivals. A body
            // effect must replace the old LastPos, and its disappearance must
            // leave the last observation with its original age.
            let state = self.actors.get_mut(&owner).unwrap();
            let fresh = e.flags & NEW != 0
                && started == e.at_ms
                && state
                    .body_episode
                    .is_none_or(|(old, at)| old != icon || e.at_ms.saturating_sub(at) > 750);
            if fresh {
                state.history_anchor = None;
                state.body_episode = Some((icon, e.at_ms));
            }
            let (_, episode) = *state.body_episode.get_or_insert((icon, started));
            if started + 750 < episode {
                return;
            }
            self.record_position(owner, position, e.at_ms, icon, 4, f);
            if let Some(m) = self.markers.get_mut(&(owner, 0))
                && m.evidence == 4
            {
                m.created_ms = episode;
                m.expires_ms = episode + u64::from(f.trace_ttl_ms.clamp(3000, 30000));
            }
            return;
        }
        // Self buffs and their ambient children describe the same carrier.
        // A child without an independent position must never become a second
        // hero icon at a stale culling/attachment origin.
        if self_cast {
            return;
        }
        if f.flags & TRACES == 0 {
            return;
        }
        if !body && e.flags & NEW == 0 {
            return;
        }
        if catalog::behavior(text(&icon)) & (8 | 16) != 0 && self.estimate_cast(owner, e, icon, f) {
            return;
        }
        // Cast/attack generation gives fixed dedup anchors. Child particles
        // cannot indefinitely extend one action; a genuine aura can refresh.
        let key = self
            .sources
            .get(&source)
            .and_then(|s| {
                s.stamps
                    .iter()
                    .copied()
                    .filter(|v| {
                        *v > 0.
                            && (*v * 1000.) as u64 <= self.now
                            && self.now - (*v * 1000.) as u64 <= 1500
                    })
                    .max_by(f32::total_cmp)
            })
            .map(|s| ((s.to_bits() as u64) << 32) | u64::from(source))
            .unwrap_or(e.key);
        self.mark(
            owner,
            key,
            position,
            if body { 2 } else { 3 },
            if body { 4 } else { 3 },
            e.at_ms,
            // The capture hook samples continuing effects every 500 ms.
            // Match the same finite evidence horizon used for camp bodies.
            if body { 1200 } else { 3000 },
            a.enemy.hero,
            icon,
            body,
        );
    }
    fn modifier(&mut self, e: &Observation, f: &Frame) {
        let Some(owner) = self.owner(e.actor) else {
            return;
        };
        let creation = e.values[0];
        let tick = e.values[1];
        if creation < 0.
            || tick < 0.
            || creation * 1000. > self.now as f32 + 500.
            || tick * 1000. > self.now as f32 + 500.
        {
            self.rejected = self.rejected.saturating_add(1);
            return;
        }
        if self
            .modifiers
            .get(&(e.subject, e.key))
            .is_some_and(|p| p.seen > e.at_ms)
        {
            self.rejected = self.rejected.saturating_add(1);
            return;
        }
        let prior = self.modifiers.insert(
            (e.subject, e.key),
            ModifierClock {
                owner,
                creation,
                tick,
                seen: e.at_ms,
            },
        );
        let recent =
            |v: f32| v > 0. && v * 1000. <= self.now as f32 && self.now as f32 - v * 1000. <= 1500.;
        let changed = prior
            .as_ref()
            .filter(|p| p.owner == owner)
            .map_or(recent(creation) || recent(tick), |p| {
                p.creation != creation || tick > p.tick + 0.001
            });
        if !changed || (!recent(creation) && !recent(tick)) || f.flags & TRACES == 0 {
            return;
        }
        let key = catalog::normalized(text(&e.name));
        let mut candidates: Vec<_> = self
            .source_index
            .get(&owner)
            .into_iter()
            .flatten()
            .filter_map(|h| self.sources.get(h))
            .filter(|s| s.normalized.len() >= 6 && key.contains(&s.normalized))
            .map(|s| s.name)
            .collect();
        candidates.sort();
        candidates.dedup();
        if candidates.len() != 1 {
            return;
        }
        let a = &self.actors[&owner];
        if a.visible {
            return;
        }
        self.mark(
            owner,
            e.key ^ u64::from(tick.to_bits()),
            e.position,
            3,
            3,
            e.at_ms,
            2500,
            a.enemy.hero,
            candidates[0],
            false,
        );
    }
    fn estimate_cast(&mut self, owner: u32, e: &Observation, icon: Name, f: &Frame) -> bool {
        // This is a contextual hypothesis, never body evidence. A point cast
        // may target combat, a wave or an empty location. Require a nearby
        // observed wave/neutral or a camp box; keep the white outline even if
        // the spatial prior looks convincing.
        if self
            .dedup
            .get(&(owner, e.key))
            .is_some_and(|t| *t > self.now)
        {
            return true;
        }
        let target = e.position;
        let mut contexts: Vec<Vec3> = self
            .creeps
            .waves(self.now, self.team)
            .into_iter()
            .map(|w| w.position)
            .collect();
        contexts.extend(
            self.actors
                .values()
                .filter(|a| a.neutral && a.alive)
                .filter_map(|a| {
                    a.body
                        .filter(|(_, at)| self.now.saturating_sub(*at) < 1500)
                        .map(|v| v.0)
                }),
        );
        // The spawn area is also a useful weak prior when its units are not
        // currently replicated. Do not claim it is occupied or blocked.
        contexts.extend(self.camps.values().map(|c| Vec3 {
            x: (c.minimum.x + c.maximum.x) * 0.5,
            y: (c.minimum.y + c.maximum.y) * 0.5,
            z: c.minimum.z,
        }));
        let Some(context) = contexts
            .into_iter()
            .filter(|p| (p.x - target.x).hypot(p.y - target.y) < 650.)
            .min_by(|a, b| {
                (a.x - target.x)
                    .hypot(a.y - target.y)
                    .total_cmp(&(b.x - target.x).hypot(b.y - target.y))
            })
        else {
            return false;
        };
        let a = &self.actors[&owner];
        let prior = a.last_visible.map(|v| v.0);
        let (dx, dy) = prior.map_or((context.x - target.x, context.y - target.y), |p| {
            (p.x - target.x, p.y - target.y)
        });
        let norm = dx.hypot(dy);
        // With no directional information, show the ability's effect only.
        if norm < 1. {
            return false;
        }
        // Search around the farm target, preferring the prior's side and
        // avoiding sampled tree trunks. Camp geometry is only a prior; this
        // is not a navigation/pathfinding proof.
        let base = dy.atan2(dx);
        let mut points = Vec::new();
        for radius in [200., 350., 500.] {
            for step in -8i32..8 {
                let angle = base + step as f32 * std::f32::consts::PI / 8.;
                let p = Vec3 {
                    x: target.x + angle.cos() * radius,
                    y: target.y + angle.sin() * radius,
                    z: target.z,
                };
                if self
                    .obstacles
                    .values()
                    .any(|(t, _)| (t.x - p.x).hypot(t.y - p.y) < 100.)
                {
                    continue;
                }
                let score = (p.x - context.x).hypot(p.y - context.y) + step.abs() as f32 * 60.;
                points.push((p, score));
            }
        }
        let Some((p, _)) = points.into_iter().min_by(|a, b| a.1.total_cmp(&b.1)) else {
            return false;
        };
        let hero = a.enemy.hero;
        self.markers
            .retain(|(h, _), m| *h != owner || !matches!(m.kind, 1 | 2 | 5));
        self.dedup.insert((owner, e.key), self.now + 30000);
        self.dedup.remove(&(owner, 0));
        self.mark(
            owner,
            0,
            p,
            5,
            7,
            e.at_ms,
            u64::from(f.trace_ttl_ms.clamp(3000, 30000)),
            hero,
            icon,
            false,
        );
        true
    }
    #[allow(clippy::too_many_arguments)]
    fn mark(
        &mut self,
        owner: u32,
        key: u64,
        position: Vec3,
        kind: u32,
        evidence: u32,
        at: u64,
        ttl: u64,
        hero: Name,
        icon: Name,
        continuous: bool,
    ) {
        if !position.valid() {
            return;
        }
        let id = (owner, key);
        if !continuous && self.dedup.get(&id).is_some_and(|v| *v > self.now) {
            return;
        }
        self.dedup.insert(id, self.now + 30000);
        self.markers.insert(
            id,
            Marker {
                owner,
                kind,
                key,
                position,
                evidence,
                created_ms: at,
                expires_ms: at.saturating_add(ttl),
                hero,
                icon,
            },
        );
    }
    fn scroll(&mut self, e: &Observation) {
        if self.owner(e.actor) != Some(e.actor) || e.flags & KNOWN == 0 {
            return;
        }
        let visible = self.actors[&e.actor].visible;
        let state = self.scrolls.entry(e.actor).or_default();
        if e.at_ms < state.seen {
            return;
        }
        if e.flags & ACTIVE == 0 {
            *state = ScrollClock {
                value: Scroll {
                    owner: e.actor,
                    flags: KNOWN,
                    ..Scroll::default()
                },
                seen: e.at_ms,
                ..ScrollClock::default()
            };
            return;
        }
        if state.value.flags & ACTIVE == 0 || state.item != e.subject {
            *state = ScrollClock {
                value: Scroll {
                    owner: e.actor,
                    flags: KNOWN | ACTIVE,
                    charges: -1,
                    ..Scroll::default()
                },
                item: e.subject,
                ..ScrollClock::default()
            };
        }
        state.seen = e.at_ms;
        state.travel_boots = e.flags & TRAVEL_BOOTS != 0;
        if e.flags & COOLDOWN_LENGTH_KNOWN != 0 && (0.001..=300.).contains(&e.values[2]) {
            state.cooldown_length = Some((e.values[2] * 1000.) as u64);
        }
        if e.flags & CHARGES_KNOWN != 0 && e.values[0] >= 0. {
            if state.charges_at.is_none() || state.value.charges != e.values[0] as i32 {
                state.charges_changed_at = Some(e.at_ms);
                state.estimated_charges = None;
            }
            if visible {
                state.estimated_charges = None;
            }
            state.value.charges = e.values[0] as i32;
            state.value.flags |= CHARGES_KNOWN;
            state.charges_at = Some(e.at_ms);
        }
        if e.flags & COOLDOWN_KNOWN != 0 && (0. ..=300.).contains(&e.values[1]) {
            let raw = e.values[1];
            // Replicated remaining time freezes in fog. Only a changed sample,
            // a different item or confirmed visibility can re-anchor it.
            if visible
                || state
                    .last_remaining
                    .is_none_or(|last| (raw - last).abs() > 0.001)
            {
                state.value.cooldown_ms = e.at_ms.saturating_add((raw * 1000.) as u64);
                state.estimated_cooldown = None;
            }
            state.last_remaining = Some(raw);
            state.cooldown_at = Some(e.at_ms);
            state.value.flags |= COOLDOWN_KNOWN;
        }
    }
    fn correlate_endpoint(&mut self, e: &Observation) {
        if e.flags & ENDCAP != 0 {
            let candidates: Vec<_> = self
                .routes
                .iter()
                .filter(|(_, r)| r.endpoint_key == e.key && r.value.flags & 12 == 0)
                .map(|(o, _)| *o)
                .collect();
            if candidates.len() == 1 {
                self.accept_endcap(candidates[0], e);
            }
            return;
        }
        let required = KNOWN | ACTIVE | PARTICLE_TELEPORT | ENDPOINT_ANCHOR | DESTINATION | NEW;
        if e.flags & required != required {
            return;
        }
        let mut candidates = self
            .routes
            .iter()
            .filter(|(_, r)| {
                let route = &r.value;
                route.flags & 1 != 0
                    && route.flags & 12 == 0
                    && route.started_ms.abs_diff(e.at_ms) <= 750
                    && (route.origin.x - e.position.x).hypot(route.origin.y - e.position.y) <= 64.
                    && (route.origin.z - e.position.z).abs() <= 128.
                    && (route.flags & 2 == 0
                        || (route.destination.x - e.auxiliary.x)
                            .hypot(route.destination.y - e.auxiliary.y)
                            <= 64.)
            })
            .map(|(owner, _)| *owner);
        let Some(owner) = candidates.next() else {
            return;
        };
        if candidates.next().is_some() {
            return;
        }
        let route = self.routes.get_mut(&owner).unwrap();
        if route.value.flags & 2 == 0 {
            route.value.destination = e.auxiliary;
            route.value.flags |= 2;
        }
        route.last_at = route.last_at.max(e.at_ms);
        if route.endpoint_key == 0 {
            route.endpoint_key = e.key;
        }
    }
    fn accept_endcap(&mut self, owner: u32, e: &Observation) {
        let Some(r) = self.routes.get_mut(&owner) else {
            return;
        };
        let elapsed = e.at_ms.saturating_sub(r.value.started_ms);
        if r.endpoint_key != e.key
            || r.value.flags & 12 != 0
            || r.value.flags & 2 == 0
            || !(500..=10000).contains(&elapsed)
            || (r.value.destination.x - e.auxiliary.x).hypot(r.value.destination.y - e.auxiliary.y)
                > 64.
        {
            return;
        }
        r.endcap_at = Some(e.at_ms);
    }
    fn teleport(&mut self, e: &Observation) {
        let Some(owner) = self.owner(e.actor) else {
            return;
        };
        if e.flags & KNOWN == 0 {
            return;
        }
        if e.flags & ENDCAP != 0 {
            self.accept_endcap(owner, e);
            return;
        }
        if e.flags & ACTIVE == 0 {
            if e.flags & PARTICLE_TELEPORT != 0 {
                return;
            }
            // An empty hidden modifier vector cannot disprove a channel
            // which has only been seen through item/particle evidence.
            if e.flags & HIDDEN != 0 && self.routes.get(&owner).is_none_or(|r| !r.authoritative) {
                return;
            }
            if self.routes.get(&owner).is_some_and(|r| r.last_at > e.at_ms) {
                return;
            }
            if self
                .routes
                .get(&owner)
                .is_some_and(|r| r.value.flags & 12 != 0)
            {
                // Repeated absence describes the same stopped cast. Moving
                // its timestamp forward would suppress a new item-only cast.
                return;
            }
            self.stopped_channels
                .entry(owner)
                .and_modify(|prior| {
                    if e.at_ms > prior.0 || (e.at_ms == prior.0 && e.key == 0) {
                        *prior = (e.at_ms, e.key);
                    }
                })
                .or_insert((e.at_ms, e.key));
            if e.key != 0 {
                self.terminal.insert((owner, e.key), self.now + 10000);
            }
            let arrived = self
                .routes
                .get(&owner)
                .is_some_and(|r| self.arrived(owner, r));
            if let Some(state) = self.routes.get_mut(&owner) {
                let route = &mut state.value;
                if e.key != 0 && e.key != route.key {
                    return;
                }
                if route.flags & 12 != 0 {
                    return;
                }
                route.flags |= if arrived { 8 } else { 4 };
                route.expires_ms = self.now + 350;
                state.last_at = e.at_ms;
                self.terminal.insert((owner, route.key), self.now + 10000);
                // Particle collection keys need not equal the modifier key.
                // During the terminated cast's remaining window they cannot
                // establish a distinguishable new cast without channel facts.
                self.particle_terminal
                    .insert(owner, route.ends_ms.max(self.now) + 350);
            } else {
                self.particle_terminal.insert(owner, self.now + 350);
            }
            return;
        }
        if e.key == 0
            || e.values[0] < 500.
            || e.values[0] > 10000.
            || e.values[1] < 0.
            || e.values[1] > e.values[0]
        {
            return;
        }
        if self.terminal.contains_key(&(owner, e.key)) {
            return;
        }
        let particle = e.flags & PARTICLE_TELEPORT != 0;
        let item = e.flags & ITEM_TELEPORT != 0;
        if particle
            && e.flags & NEW == 0
            && self.routes.get(&owner).is_none_or(|r| r.value.key != e.key)
        {
            return;
        }
        if !particle
            && self
                .stopped_channels
                .get(&owner)
                .is_some_and(|(at, key)| *at >= e.at_ms && (*key == 0 || *key == e.key))
        {
            return;
        }
        let subtype = e.flags & (TWIN_GATE | KEEN_CONVEYANCE);
        if subtype == (TWIN_GATE | KEEN_CONVEYANCE)
            || (particle && e.flags & (ORIGIN | DESTINATION) == 0)
            || (particle && self.particle_terminal.contains_key(&owner))
        {
            return;
        }
        let started = e.at_ms.saturating_sub(e.values[1] as u64);
        let ends = started.saturating_add(e.values[0] as u64);
        // A frozen item timestamp must not resurrect a cancelled channel
        // after its short presentation grace has expired. Item and modifier
        // keys differ, so compare the logical cast time with the stop fact.
        if item
            && self
                .stopped_channels
                .get(&owner)
                .is_some_and(|(at, _)| *at >= started)
        {
            return;
        }
        if ends + 350 <= self.now {
            return;
        }
        if let Some(state) = self.routes.get(&owner) {
            if item
                && state.authoritative
                && !(state.value.flags & 12 != 0
                    && state.value.key != e.key
                    && started > state.last_at)
            {
                // Item endpoints can fill a missing modifier endpoint, but
                // cannot replace its clock or establish a second cast.
                if state.value.flags & 12 != 0 || state.value.started_ms.abs_diff(started) > 750 {
                    return;
                }
                let state = self.routes.get_mut(&owner).unwrap();
                if state.value.flags & 1 == 0 && e.flags & ORIGIN != 0 {
                    state.value.origin = e.position;
                    state.value.flags |= 1;
                }
                if state.value.flags & 2 == 0 && e.flags & DESTINATION != 0 {
                    state.value.destination = e.auxiliary;
                    state.value.flags |= 2;
                }
                return;
            }
            if particle && state.subtype != subtype {
                return;
            }
            if state.value.flags & 12 != 0 && (particle || state.value.key == e.key) {
                return;
            }
            if e.at_ms < state.last_at && state.value.key == e.key {
                return;
            }
            if particle && state.value.key != e.key {
                // Exact ownership alone does not establish cast identity. A
                // complementary endpoint may fill one unambiguous cast window;
                // conflicting same-side geometry is never silently replaced.
                if state.value.started_ms.abs_diff(started) > 750 {
                    return;
                }
                let compatible = |old: Vec3, new: Vec3| {
                    (old.x - new.x).abs() <= 64.
                        && (old.y - new.y).abs() <= 64.
                        && (old.z - new.z).abs() <= 64.
                };
                if (e.flags & ORIGIN != 0
                    && state.value.flags & 1 != 0
                    && !compatible(state.value.origin, e.position))
                    || (e.flags & DESTINATION != 0
                        && state.value.flags & 2 != 0
                        && !compatible(state.value.destination, e.auxiliary))
                {
                    return;
                }
            }
        }
        let state = self.routes.entry(owner).or_default();
        if state.value.key != e.key && !particle {
            let upgrade = !state.authoritative
                && state.subtype == subtype
                && state.value.started_ms.abs_diff(started) <= 750
                && state.value.flags & 12 == 0;
            let old = state.value;
            if !upgrade {
                state.event_key = e.key;
                state.position_published = false;
                state.endpoint_key = 0;
                state.endcap_at = None;
            }
            state.value = Route {
                owner,
                key: e.key,
                started_ms: started,
                ends_ms: ends,
                hero: self.actors[&owner].enemy.hero,
                ..Route::default()
            };
            if upgrade {
                state.value.origin = old.origin;
                state.value.destination = old.destination;
                state.value.flags = old.flags & 3;
            }
            state.subtype = subtype;
            state.authoritative = !item;
        } else if state.value.key == 0 {
            state.value = Route {
                owner,
                key: e.key,
                started_ms: started,
                ends_ms: ends,
                hero: self.actors[&owner].enemy.hero,
                ..Route::default()
            };
            state.subtype = subtype;
            state.authoritative = !particle && !item;
            state.event_key = e.key;
        } else if !particle {
            // Structured channel data can replace an estimated fallback timer.
            state.value.started_ms = started;
            state.value.ends_ms = ends;
            state.subtype = subtype;
            state.authoritative = !item;
        }
        let route = &mut state.value;
        if e.flags & ORIGIN != 0 && ((!particle && !item) || route.flags & 1 == 0) {
            route.origin = e.position;
            route.flags |= 1;
        }
        if e.flags & DESTINATION != 0 && ((!particle && !item) || route.flags & 2 == 0) {
            route.destination = e.auxiliary;
            route.flags |= 2;
        }
        if particle && e.flags & DESTINATION != 0 && state.endpoint_key == 0 {
            state.endpoint_key = e.key;
        }
        state.last_at = state.last_at.max(e.at_ms);
        route.expires_ms = route.ends_ms + 350;
    }
    fn teleport_item(&mut self, e: &Observation) {
        if e.subject == 0 || e.subject == u32::MAX {
            return;
        }
        let channel_stamp = if e.flags & CHANNEL_TIME_KNOWN != 0 {
            e.values[0]
        } else {
            0.
        };
        let cast_stamp = if e.flags & (CAST_TIME_KNOWN | ACTIVE) == (CAST_TIME_KNOWN | ACTIVE) {
            e.values[1]
        } else {
            0.
        };
        let stamp = channel_stamp.max(cast_stamp);
        if stamp <= 0. {
            return;
        }
        let started = (f64::from(stamp) * 1000.).round() as u64;
        if started > e.at_ms {
            return;
        }
        let duration = if e.flags & DURATION_KNOWN != 0 {
            if !(0.5..=10.).contains(&e.values[2]) || !(0. ..=5.).contains(&e.values[3]) {
                return;
            }
            ((e.values[2] + e.values[3]) * 1000.)
                .round()
                .clamp(500., 10000.) as u64
        } else {
            3000
        };
        if e.at_ms - started > duration {
            return;
        }
        let mut channel = *e;
        channel.kind = TELEPORT;
        channel.key = (u64::from(e.subject) << 32) | u64::from(stamp.to_bits());
        channel.flags |= KNOWN | ACTIVE | ITEM_TELEPORT;
        channel.values = [duration as f32, (e.at_ms - started) as f32, 0., 0.];
        self.teleport(&channel);
    }
    fn arrived(&self, owner: u32, state: &TeleportRoute) -> bool {
        let r = &state.value;
        if r.flags & 2 == 0 || self.now + 150 < r.ends_ms {
            return false;
        }
        let Some(a) = self.actors.get(&owner) else {
            return false;
        };
        let Some((p, at)) = a.sampled_position else {
            return false;
        };
        at + 150 >= r.ends_ms
            && self.now.saturating_sub(at) <= 500
            && (p.x - r.destination.x).hypot(p.y - r.destination.y) <= 200.
            && (a.visible || a.position_changed_at + 250 >= r.ends_ms)
    }
    fn record_position(
        &mut self,
        owner: u32,
        position: Vec3,
        at: u64,
        icon: Name,
        evidence: u32,
        f: &Frame,
    ) {
        let confirmed = evidence != 5;
        let Some(a) = self.actors.get_mut(&owner) else {
            return;
        };
        if !a.alive || a.last_visible.is_some_and(|(_, time)| time > at) {
            return;
        }
        if !confirmed
            // Only a projection of the frozen model can contradict a newer
            // body observation. A new effect's independent creation anchor
            // must not be vetoed by repeated visible samples of that model.
            && a.sampled_position.is_some_and(|(p, _)| {
                (p.x - position.x).hypot(p.y - position.y) <= 64.
            })
            && a.body.is_some_and(|(p, time)| {
                time > a.position_changed_at && (p.x - position.x).hypot(p.y - position.y) > 64.
            })
        {
            return;
        }
        // Stationary streams confirm freshness for camp inference, but do
        // not restart the age/TTL of a remembered position every 500 ms.
        // Keep this anchor after the marker expires, otherwise the next poll
        // would resurrect that same stationary emitter indefinitely.
        let unchanged = evidence == 4
            && a.history_anchor.is_some_and(|(p, _, old_icon)| {
                (p.x - position.x).hypot(p.y - position.y) < 16. && old_icon == icon
            });
        if confirmed {
            a.body = Some((position, at));
        }
        if unchanged {
            return;
        }
        a.history_anchor = Some((position, at, icon));
        if evidence != 4 {
            a.body_episode = Some((icon, at));
        }
        a.last_visible = Some((position, at));
        let hero = a.enemy.hero;
        let visible = a.visible;
        self.markers
            .retain(|(h, _), m| *h != owner || !matches!(m.kind, 1 | 2 | 5));
        self.dedup.remove(&(owner, 0));
        if !visible && f.flags & TRACES != 0 {
            self.mark(
                owner,
                0,
                position,
                if evidence == 6 { 1 } else { 2 },
                evidence,
                at,
                u64::from(f.trace_ttl_ms.clamp(3000, 30000)),
                hero,
                icon,
                true,
            );
        }
    }
    fn reconcile_teleports(&mut self, f: &Frame) {
        let arrivals: Vec<_> = self
            .routes
            .iter()
            .filter(|(owner, r)| {
                !r.position_published
                    && r.value.flags & 4 == 0
                    && (r.endcap_at.is_some() || r.value.flags & 8 != 0 || self.arrived(**owner, r))
            })
            .map(|(owner, r)| (*owner, r.value))
            .collect();
        for (owner, r) in arrivals {
            let state = self.routes.get_mut(&owner).unwrap();
            state.value.flags |= 8;
            state.value.expires_ms = self.now + 350;
            state.last_at = self.now;
            state.position_published = true;
            self.terminal.insert((owner, r.key), self.now + 10000);
            self.particle_terminal.insert(owner, self.now + 350);
            let (position, at) = if let Some(at) = state.endcap_at {
                (r.destination, at)
            } else {
                self.actors[&owner].sampled_position.unwrap()
            };
            self.record_position(owner, position, at, name(""), 6, f);
        }
    }
    fn infer_scroll_casts(&mut self) {
        for (&owner, route) in &self.routes {
            let r = &route.value;
            if route.subtype != 0 || r.flags & 1 == 0 {
                continue;
            }
            let Some(s) = self.scrolls.get_mut(&owner) else {
                continue;
            };
            if s.value.flags & (KNOWN | ACTIVE) != (KNOWN | ACTIVE)
                || s.consumed_cast == route.event_key
            {
                continue;
            }
            s.consumed_cast = route.event_key;
            if self.actors.get(&owner).is_none_or(|a| a.visible) {
                continue;
            }
            // Frozen replicated values are reconciled once per logical cast.
            // Real changed/visible samples win in scroll(); route aliases keep
            // event_key across particle -> item -> modifier upgrades.
            if s.value.flags & CHARGES_KNOWN != 0
                && s.charges_changed_at.is_some_and(|at| at < r.started_ms)
            {
                s.estimated_charges =
                    Some((s.estimated_charges.unwrap_or(s.value.charges) - 1).max(0));
            }
            if s.last_remaining.is_some_and(|v| v <= 0.001) {
                let duration =
                    s.cooldown_length
                        .unwrap_or(if s.travel_boots { 40000 } else { 80000 });
                s.estimated_cooldown = Some(r.started_ms.saturating_add(duration));
            }
        }
    }
    fn forget_source(&mut self, handle: u32) {
        self.sources.remove(&handle);
        self.bindings.retain(|k, v| *k != handle && v.0 != handle);
        self.emitters.retain(|_, v| v.source != handle);
    }
    fn forget_actor(&mut self, owner: u32) {
        self.actors.remove(&owner);
        self.markers.retain(|k, _| k.0 != owner);
        self.routes.remove(&owner);
        self.scrolls.remove(&owner);
        self.particle_terminal.remove(&owner);
        self.stopped_channels.remove(&owner);
        self.bindings.retain(|k, v| *k != owner && v.0 != owner);
        self.sources.retain(|_, v| v.owner != owner);
        self.modifiers
            .retain(|k, v| k.0 != owner && v.owner != owner);
        self.emitters.retain(|k, _| k.0 != owner);
        self.dedup.retain(|k, _| k.0 != owner);
        self.terminal.retain(|k, _| k.0 != owner);
        self.launches.retain(|k, _| k.0 != owner);
    }
    fn bound(&mut self) {
        macro_rules! cap {
            ($map:expr,$limit:expr) => {
                while $map.len() > $limit {
                    $map.pop_first();
                    self.dropped = self.dropped.saturating_add(1);
                }
            };
        }
        while self.actors.len() > 256 {
            // Neutral population must not evict a fresh hero merely because
            // that hero has a smaller numeric handle and lose fog continuity.
            let owner = *self
                .actors
                .iter()
                .min_by_key(|(_, a)| (a.enemy.flags & HERO != 0, a.seen))
                .unwrap()
                .0;
            self.forget_actor(owner);
            self.dropped = self.dropped.saturating_add(1);
        }
        cap!(self.sources, 1024);
        cap!(self.emitters, 1024);
        cap!(self.launches, 512);
        cap!(self.obstacles, 512);
        cap!(self.bindings, 2048);
        cap!(self.modifiers, 1024);
        cap!(self.dedup, 4096);
        while self.markers.len() > 64 {
            let key = *self
                .markers
                .iter()
                .min_by_key(|(_, v)| v.expires_ms)
                .unwrap()
                .0;
            self.markers.remove(&key);
            self.dropped = self.dropped.saturating_add(1);
        }
        cap!(self.routes, 16);
        cap!(self.camps, 64);
        cap!(self.scrolls, 32);
        cap!(self.terminal, 1024);
        cap!(self.particle_terminal, 256);
        cap!(self.stopped_channels, 256);
    }
    fn snapshot(&self, f: &Frame) -> Snapshot {
        let mut out = Snapshot {
            size: std::mem::size_of::<Snapshot>() as u32,
            version: ABI,
            session: self.session,
            game_ms: self.now,
            rejected: self.rejected,
            dropped: self.dropped,
            ..Snapshot::default()
        };
        for ((_, key), emitter) in &self.emitters {
            if out.correction_count >= 32 {
                break;
            }
            let Some((position, reference)) = emitter.correction else {
                continue;
            };
            let expires = (emitter.seen + 750).min(emitter.started + 15000);
            if expires <= self.now {
                continue;
            }
            out.corrections[out.correction_count as usize] = PositionCorrection {
                key: *key,
                position,
                reference,
                created_ms: emitter.started,
                expires_ms: expires,
            };
            out.correction_count += 1;
        }
        for m in self.markers.values().filter(|m| m.expires_ms > self.now) {
            out.markers[out.marker_count as usize] = *m;
            out.marker_count += 1;
        }
        if f.flags & ROUTES != 0 {
            for r in self
                .routes
                .values()
                .filter(|r| r.value.expires_ms > self.now)
            {
                let mut value = r.value;
                value.flags |= r.subtype;
                out.routes[out.route_count as usize] = value;
                out.route_count += 1;
            }
        }
        if f.flags & CREEP_TRACKER != 0 {
            for wave in self.creeps.waves(self.now, self.team) {
                if out.marker_count as usize >= out.markers.len() {
                    break;
                }
                out.markers[out.marker_count as usize] = Marker {
                    kind: 6,
                    key: wave.key,
                    position: wave.position,
                    evidence: if wave.visible {
                        WAVE_VISIBLE
                    } else if wave.direct {
                        9
                    } else {
                        10
                    },
                    created_ms: wave.observed,
                    expires_ms: self.now + 250,
                    ..Marker::default()
                };
                out.marker_count += 1;
            }
        }
        if f.flags & CAMPS != 0 {
            for c in self.camps.values() {
                let mut camp = *c;
                for a in self.actors.values().filter(|a| a.alive) {
                    if a.neutral {
                        if let Some((p, at)) = a.body
                            && self.now.saturating_sub(at) <= 1200
                            && p.inside(c.minimum, c.maximum)
                        {
                            camp.neutrals += 1;
                        }
                    } else if a.enemy.team != self.team
                        && let Some((p, at)) = a.body
                        && self.now.saturating_sub(at) <= 1200
                        && p.inside(c.minimum, c.maximum)
                    {
                        camp.state = 2;
                    }
                }
                out.camps[out.camp_count as usize] = camp;
                out.camp_count += 1;
            }
        }
        for a in self
            .actors
            .values()
            .filter(|a| a.enemy.team != self.team && !a.neutral && a.enemy.flags & HERO != 0)
            .take(32)
        {
            out.enemies[out.enemy_count as usize] = a.enemy;
            out.enemy_count += 1;
        }
        for state in self
            .scrolls
            .values()
            .filter(|s| self.now.saturating_sub(s.seen) <= 1500)
        {
            let mut value = state.value;
            if let Some(charges) = state.estimated_charges {
                value.charges = charges;
                value.flags |= ESTIMATED | CHARGES_KNOWN;
            }
            if let Some(deadline) = state.estimated_cooldown {
                value.cooldown_ms = deadline;
                value.flags |= ESTIMATED | COOLDOWN_KNOWN;
            }
            if state.estimated_charges.is_none()
                && state
                    .charges_at
                    .is_none_or(|at| self.now.saturating_sub(at) > 1500)
                && value.flags & ACTIVE != 0
            {
                value.flags &= !CHARGES_KNOWN;
                value.charges = -1;
            }
            if state.estimated_cooldown.is_none()
                && state
                    .cooldown_at
                    .is_none_or(|at| self.now.saturating_sub(at) > 1500)
            {
                value.flags &= !COOLDOWN_KNOWN;
                value.cooldown_ms = 0;
            }
            out.scrolls[out.scroll_count as usize] = value;
            out.scroll_count += 1;
        }
        out
    }
}
