//! Versioned numeric-only C boundary. No engine pointers or Rust containers cross it.
pub const ABI: u32 = 4;
pub const MAX_EVENTS: usize = 4096;
pub const ENTITY: u32 = 1;
pub const BINDING: u32 = 2;
pub const SOURCE: u32 = 3;
pub const PARTICLE: u32 = 4;
pub const MODIFIER: u32 = 5;
pub const TELEPORT: u32 = 6;
pub const CAMP: u32 = 7;
pub const DROP: u32 = 8;
pub const SCROLL: u32 = 9;
/// Raw scroll channel fields: values = [channel_start, cast_start, base, extra].
pub const TELEPORT_ITEM: u32 = 10;
/// Native linear projectile: exact caster, launch/current positions, age in ms.
pub const PROJECTILE: u32 = 11;
pub const OBSTACLE: u32 = 12;
/// Raw spawn-epoch/start seconds and GameRules phase. KNOWN validates the clock;
/// ACTIVE = creep spawning enabled. Forecasts require phase 5 (after the horn).
/// key = local spawn-configuration revision + 1, or zero when unavailable.
pub const WAVE_CLOCK: u32 = 13;
pub const LANE_CREEP: u32 = 1 << 26;
pub const VISUAL_CORRECTION: u32 = 1 << 27;
/// ENTITY lane creep: auxiliary.x is the networked entity creation time (seconds).
/// It identifies the prepared timer cohort; hidden coordinates are not motion evidence.
pub const CREATION_TIME_KNOWN: u32 = 1 << 28;
pub const VISIBLE: u32 = 1;
pub const HIDDEN: u32 = 2;
pub const ALIVE: u32 = 4;
pub const POSITION: u32 = 8;
pub const ACTIVE: u32 = 16;
pub const KNOWN: u32 = 32;
pub const ORIGIN: u32 = 64;
pub const DESTINATION: u32 = 128;
pub const NEW: u32 = 256;
pub const NEUTRAL: u32 = 512;
pub const HERO: u32 = 1024;
pub const CONTROL_POINT: u32 = 2048;
// SCROLL payload validity is independent of presence (ACTIVE) and inventory
// completeness (KNOWN). Failed reads must never become zero charges/cooldown.
pub const CHARGES_KNOWN: u32 = 4096;
pub const COOLDOWN_KNOWN: u32 = 8192;
pub const PARTICLE_TELEPORT: u32 = 16384;
pub const TWIN_GATE: u32 = 32768;
pub const KEEN_CONVEYANCE: u32 = 65536;
pub const CHANNEL_TIME_KNOWN: u32 = 1 << 17;
pub const CAST_TIME_KNOWN: u32 = 1 << 18;
pub const DURATION_KNOWN: u32 = 1 << 19;
pub const COOLDOWN_LENGTH_KNOWN: u32 = 1 << 20;
pub const ITEM_TELEPORT: u32 = 1 << 21;
pub const ESTIMATED: u32 = 1 << 22;
pub const TRAVEL_BOOTS: u32 = 1 << 23;
/// Resource-validated TP destination with a cast-origin correlation anchor.
pub const ENDPOINT_ANCHOR: u32 = 1 << 24;
pub const ENDCAP: u32 = 1 << 25;
pub const TRACES: u32 = 1;
pub const CREEP_TRACKER: u32 = 8;
/// Visible wave stays in inference snapshots but has no overlay presentation.
pub const WAVE_VISIBLE: u32 = 11;
pub const ROUTES: u32 = 2;
pub const CAMPS: u32 = 4;
pub type Name = [u8; 96];

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Vec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}
impl Vec3 {
    pub fn valid(self) -> bool {
        [self.x, self.y, self.z]
            .iter()
            .all(|v| v.is_finite() && v.abs() <= 20000.)
    }
    pub fn inside(self, low: Self, high: Self) -> bool {
        self.x >= low.x
            && self.x <= high.x
            && self.y >= low.y
            && self.y <= high.y
            && self.z >= low.z
            && self.z <= high.z
    }
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Frame {
    pub size: u32,
    pub version: u32,
    pub session: u64,
    pub wall_ms: u64,
    pub game_ms: u64,
    pub flags: u32,
    pub local_team: u32,
    pub trace_ttl_ms: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Observation {
    pub kind: u32,
    pub flags: u32,
    pub actor: u32,
    pub subject: u32,
    pub key: u64,
    pub at_ms: u64,
    pub position: Vec3,
    pub auxiliary: Vec3,
    pub values: [f32; 4],
    pub name: Name,
    pub resource: [u8; 256],
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Marker {
    pub owner: u32,
    pub kind: u32,
    pub key: u64,
    pub position: Vec3,
    pub evidence: u32,
    pub created_ms: u64,
    pub expires_ms: u64,
    pub hero: Name,
    pub icon: Name,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Route {
    pub owner: u32,
    pub flags: u32,
    pub key: u64,
    pub origin: Vec3,
    pub destination: Vec3,
    pub started_ms: u64,
    pub ends_ms: u64,
    pub expires_ms: u64,
    pub hero: Name,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Camp {
    pub id: u32,
    pub kind: u32,
    pub minimum: Vec3,
    pub maximum: Vec3,
    pub state: u32,
    pub neutrals: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Enemy {
    pub handle: u32,
    pub player: i32,
    pub team: u32,
    pub flags: u32,
    pub health: i32,
    pub max_health: i32,
    pub hero: Name,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Scroll {
    pub owner: u32,
    pub flags: u32,
    pub charges: i32,
    pub reserved: u32,
    pub cooldown_ms: u64,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Snapshot {
    pub size: u32,
    pub version: u32,
    pub session: u64,
    pub game_ms: u64,
    pub marker_count: u32,
    pub route_count: u32,
    pub camp_count: u32,
    pub enemy_count: u32,
    pub scroll_count: u32,
    pub rejected: u32,
    pub dropped: u32,
    pub correction_count: u32,
    pub markers: [Marker; 64],
    pub routes: [Route; 16],
    pub camps: [Camp; 64],
    pub enemies: [Enemy; 32],
    pub scrolls: [Scroll; 32],
    pub corrections: [PositionCorrection; 32],
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PositionCorrection {
    pub key: u64,
    pub position: Vec3,
    pub reference: Vec3,
    pub created_ms: u64,
    pub expires_ms: u64,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AttachmentQuery {
    pub size: u32,
    pub version: u32,
    pub actor: u32,
    pub model: Vec3,
    pub attachment: Vec3,
    pub origin: Vec3,
    pub hero: Name,
    pub resource: [u8; 256],
}
macro_rules! zero_default { ($($ty:ty),*) => {$ (
    impl Default for $ty { fn default() -> Self {
        // SAFETY: these ABI structs contain only integers, floats and arrays;
        // every all-zero bit pattern is valid. No references/bools/enums exist.
        unsafe { std::mem::zeroed() }
    }}
)*}; }
zero_default!(
    Frame,
    Observation,
    Marker,
    Route,
    Camp,
    Enemy,
    Scroll,
    Snapshot,
    PositionCorrection,
    AttachmentQuery
);
pub fn name(text: &str) -> Name {
    let mut out = [0; 96];
    let n = text.len().min(95);
    out[..n].copy_from_slice(&text.as_bytes()[..n]);
    out
}
pub fn text(bytes: &[u8]) -> &str {
    std::str::from_utf8(&bytes[..bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len())])
        .unwrap_or("")
}
