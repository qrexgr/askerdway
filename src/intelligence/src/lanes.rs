//! Static geometry surveyed from the installed Full Map Demo, 2026-10-19.
//! Spawners and path_corner positions are map data, not hidden unit samples.
//! Heights are terrain heights. Keep this map profile separate from inference;
//! a changed/custom map needs a new profile rather than mirrored coordinates.
use crate::abi::Vec3;
const fn p(x: f32, y: f32, z: f32) -> Vec3 {
    Vec3 { x, y, z }
}
const DIRE_TOP: &[Vec3] = &[
    p(3168., 5792., 256.),
    p(800., 5790., 128.),
    p(-1872., 6080., 0.),
    p(-5248., 5696., 128.),
    p(-6139., 5476., 128.),
    p(-6236., 4872., 128.),
    p(-6336., 3896., 128.),
    p(-6501., -872., 128.),
    p(-6576., -3720., 256.),
    p(-5591., -5024., 260.),
];
const DIRE_MID: &[Vec3] = &[
    p(4096., 3584., 256.),
    p(2520., 1728., 128.),
    p(864., 672., 128.),
    p(144., 152., 128.),
    p(-392., -224., 0.),
    p(-1008., -760., 128.),
    p(-3190., -2926., 128.),
    p(-4872., -4360., 256.),
    p(-5527., -5024., 260.),
];
const DIRE_BOT: &[Vec3] = &[
    p(6272., 3648., 256.),
    p(6160., 593., 128.),
    p(6480., -700., 128.),
    p(6720., -2112., 128.),
    p(6298., -3319., 128.),
    p(6315., -4055., 128.),
    p(6087., -5290., 128.),
    p(5711., -5784., 128.),
    p(5312., -6140., 128.),
    p(1036., -6428., 0.),
    p(-4224., -6096., 256.),
    p(-5527., -5088., 260.),
];
const RADIANT_TOP: &[Vec3] = &[
    p(-6608., -4064., 256.),
    p(-6080., 384., 128.),
    p(-6656., 1600., 128.),
    p(-6208., 4224., 128.),
    p(-6200., 5312., 128.),
    p(-5792., 5464., 128.),
    p(-5160., 5712., 128.),
    p(-4488., 5824., 128.),
    p(-3784., 5928., 128.),
    p(-2792., 6048., 128.),
    p(-1856., 6104., 0.),
    p(3856., 5784., 256.),
    p(5323., 4852., 256.),
];
const RADIANT_MID: &[Vec3] = &[
    p(-5008., -4488., 256.),
    p(-3148., -2694., 128.),
    p(-976., -824., 128.),
    p(-360., -256., 3.),
    p(176., 120., 128.),
    p(912., 624., 128.),
    p(2584., 1704., 128.),
    p(4472., 3944., 256.),
    p(5323., 4788., 256.),
];
const RADIANT_BOT: &[Vec3] = &[
    p(-3648., -6112., 256.),
    p(480., -6457., 128.),
    p(2048., -6336., 128.),
    p(3320., -6184., 128.),
    p(4028., -6140., 128.),
    p(4672., -6098., 128.),
    p(5743., -5781., 128.),
    p(6028., -5484., 128.),
    p(6133., -4708., 128.),
    p(6256., -3252., 128.),
    p(6336., 3328., 256.),
    p(5387., 4788., 256.),
];
pub fn route(team: u32, lane: u32) -> &'static [Vec3] {
    match (team, lane) {
        (2, 0) => RADIANT_TOP,
        (2, 1) => RADIANT_MID,
        (2, 2) => RADIANT_BOT,
        (3, 0) => DIRE_TOP,
        (3, 1) => DIRE_MID,
        (3, 2) => DIRE_BOT,
        _ => &[],
    }
}
pub fn distance(a: Vec3, b: Vec3) -> f32 {
    (a.x - b.x).hypot(a.y - b.y)
}
pub fn length(team: u32, lane: u32) -> f32 {
    route(team, lane)
        .windows(2)
        .map(|p| distance(p[0], p[1]))
        .sum()
}
pub fn point(team: u32, lane: u32, mut progress: f32) -> Vec3 {
    let path = route(team, lane);
    for pair in path.windows(2) {
        let len = distance(pair[0], pair[1]);
        if progress <= len {
            let f = (progress / len).clamp(0., 1.);
            return p(
                pair[0].x + (pair[1].x - pair[0].x) * f,
                pair[0].y + (pair[1].y - pair[0].y) * f,
                pair[0].z + (pair[1].z - pair[0].z) * f,
            );
        }
        progress -= len;
    }
    path.last().copied().unwrap_or_default()
}
pub fn project(team: u32, lane: u32, position: Vec3) -> (f32, f32) {
    let (mut along, mut best, mut result) = (0., f32::MAX, 0.);
    for pair in route(team, lane).windows(2) {
        let len = distance(pair[0], pair[1]);
        let f = (((position.x - pair[0].x) * (pair[1].x - pair[0].x)
            + (position.y - pair[0].y) * (pair[1].y - pair[0].y))
            / (len * len))
            .clamp(0., 1.);
        let d = distance(
            position,
            p(
                pair[0].x + (pair[1].x - pair[0].x) * f,
                pair[0].y + (pair[1].y - pair[0].y) * f,
                0.,
            ),
        );
        if d < best {
            best = d;
            result = along + len * f;
        }
        along += len;
    }
    (result, best)
}
pub fn classify(team: u32, position: Vec3) -> u32 {
    (0..3)
        .min_by(|a, b| {
            project(team, *a, position)
                .1
                .total_cmp(&project(team, *b, position).1)
        })
        .unwrap_or(1)
}

/// Spawn formation and the short base-exit detour delay the marching centroid.
/// Surveyed from complete, moving four-unit waves on this map; not a global
/// clock offset or a change to movement speed. Opposite exits have opposite lags.
/// Visible observations always replace this initial estimate for their own wave.
pub fn departure_delay(team: u32, lane: u32) -> u64 {
    match (team, lane) {
        (2, 0) => 2800,
        (2, 1) => 1700,
        (2, 2) => 650,
        (3, 0) => 600,
        (3, 1) => 1600,
        (3, 2) => 2650,
        _ => 0,
    }
}
