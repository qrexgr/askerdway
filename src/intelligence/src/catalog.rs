include!(concat!(env!("OUT_DIR"), "/catalog.rs"));
pub fn normalized(s: &str) -> String {
    s.bytes()
        .filter(u8::is_ascii_alphanumeric)
        .map(|b| b.to_ascii_lowercase() as char)
        .collect()
}
pub fn behavior(source: &str) -> u32 {
    SOURCES
        .binary_search_by_key(&source, |p| p.0)
        .map(|i| SOURCES[i].1)
        .unwrap_or(0)
}
pub fn feedback(resource: &str) -> bool {
    [
        "selection",
        "range_finder",
        "mouse",
        "cursor",
        "ping_world",
        "ui_mouseactions",
    ]
    .iter()
    .any(|v| resource.contains(v))
}
pub fn canonical(s: &str) -> &str {
    let s = s.strip_suffix("_c").unwrap_or(s);
    ALIASES
        .binary_search_by_key(&s, |p| p.0)
        .map(|i| ALIASES[i].1)
        .unwrap_or(s)
}
pub struct ResourceMatch {
    entries: &'static [(&'static str, &'static str)],
    normalized: String,
    basename: String,
}
impl ResourceMatch {
    pub fn new(resource: &str) -> Self {
        let resource = canonical(resource);
        let first = RESOURCES.partition_point(|p| p.0 < resource);
        let last = RESOURCES.partition_point(|p| p.0 <= resource);
        Self {
            entries: &RESOURCES[first..last],
            normalized: normalized(resource.rsplit('/').next().unwrap_or(resource)),
            basename: resource.rsplit('/').next().unwrap_or(resource).to_owned(),
        }
    }
    pub fn score(&self, source: &str, key: &str) -> usize {
        if self.entries.iter().any(|p| p.1 == source) {
            1000
        } else if key.len() >= 6 && self.normalized.contains(key) {
            100 + key.len()
        } else if key.len() >= 6 && {
            // Resource names insert words such as "spell" between the hero
            // and ability. Match complete ordered tokens, not a hero-specific
            // alias or a single generic word. The caller resolves ties.
            let mut resource = self.basename.split(|c: char| !c.is_ascii_alphanumeric());
            let mut tokens = source.split('_').filter(|p| !p.is_empty());
            let first = tokens.next().unwrap_or("");
            tokens.next().is_some_and(|second| {
                resource.any(|p| p == first)
                    && resource.any(|p| p == second)
                    && tokens.all(|token| resource.any(|p| p == token))
            })
        } {
            80 + key.len().min(19)
        } else {
            0
        }
    }
}
pub fn resource_hero(resource: &str) -> Option<&'static str> {
    let resource = canonical(resource);
    HERO_FOLDERS
        .iter()
        .filter(|p| {
            resource
                .strip_prefix(p.0)
                .is_some_and(|s| s.starts_with('/'))
        })
        .max_by_key(|p| p.0.len())
        .map(|p| p.1)
}
// A deliberately weaker match for a newly observed point effect accompanying
// a confirmed cast. Names can retain an old compound root after an ability is
// renamed. The caller must require fresh activation, exact ownership and CP0;
// this identifies an event, never the caster's body or an ownerless particle.
pub fn recent_point_score(resource: &str, source: &str, hero: &str) -> usize {
    let resource = canonical(resource);
    if behavior(source) & 8 == 0 || resource_hero(resource) != Some(hero) {
        return 0;
    }
    let Some(hero) = hero.strip_prefix("npc_dota_hero_") else {
        return 0;
    };
    let Some(suffix) = source.strip_prefix(hero).and_then(|s| s.strip_prefix('_')) else {
        return 0;
    };
    if !suffix.contains('_') {
        return 0;
    }
    let base = resource.rsplit('/').next().unwrap_or(resource);
    let Some(base) = base.strip_prefix(hero).and_then(|s| s.strip_prefix('_')) else {
        return 0;
    };
    let parts: Vec<_> = base
        .strip_suffix(".vpcf")
        .unwrap_or(base)
        .split('_')
        .collect();
    if !parts
        .iter()
        .any(|p| ["ring", "circle", "impact", "ground", "aoe"].contains(p))
        || parts.iter().any(|p| {
            [
                "ambient",
                "portrait",
                "loadout",
                "preview",
                "selection",
                "hover",
                "range",
                "buff",
                "body",
                "trail",
                "weapon",
                "attack",
            ]
            .contains(p)
        })
    {
        return 0;
    }
    suffix
        .split('_')
        .filter(|word| {
            word.len() >= 5
                && !["attack", "damage", "bonus", "passive", "effect", "target"].contains(word)
        })
        .filter(|word| parts.iter().any(|p| p.starts_with(word)))
        .map(|word| 32 + word.len().min(20))
        .max()
        .unwrap_or(0)
}
fn remote_emitter(resource: &str) -> bool {
    let base = canonical(resource).rsplit('/').next().unwrap_or(resource);
    [
        "target",
        "enemy",
        "friend",
        "debuff",
        "impact",
        "hit",
        "projectile",
        "attack",
        "trail",
        "launch",
    ]
    .iter()
    .any(|v| base.contains(v))
}
pub fn self_cast(resource: &str, ability: &str) -> bool {
    let flags = behavior(ability);
    flags & 4 != 0 && flags & (1 | 8 | 16) == 0 && !remote_emitter(resource)
}
pub fn caster_emitter(resource: &str, ability: &str) -> bool {
    let flags = behavior(ability);
    let base = canonical(resource).rsplit('/').next().unwrap_or(resource);
    if remote_emitter(resource) {
        return false;
    }
    if flags & 32 != 0 || (flags & 2 != 0 && flags & 4 != 0) {
        return true;
    }
    flags & 4 != 0
        && flags & (1 | 8 | 16) == 0
        && ["buff", "shield", "orbit", "_active", "_self", "_body"]
            .iter()
            .any(|v| base.contains(v))
}
