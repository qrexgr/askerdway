/**
 * @file
 * @brief Публичные POD-снимки.
 *
 * Хранит копируемые значения векторов, сущностей, урона и render-событий. Не все компактные
 * структуры имеют v1-заголовок; ориентируйтесь на конкретное объявление, а не только на общее
 * правило ABI.
 */

#pragma once

// Engine-facing value types
// -------------------------
// These are snapshots, not binary overlays for Valve classes. Reading a C++
// game object by casting it to a public struct is intentionally unsupported:
// inheritance, padding and schema offsets change between patches. The runtime
// resolves fields and copies stable values into the structures below.

#include <cstdint>

#include "askerdway/engine/handles.hpp"
#include "askerdway/version.hpp"

struct askerdway_vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Historical name retained as a source-compatible alias.
using armlet_vec3 = askerdway_vec3;

struct askerdway_damage_components {
    float original = 0.0f;
    float physical = 0.0f;
    float magical = 0.0f;
    float pure = 0.0f;
    float composite = 0.0f;
};

// Identifies a networked entity without exposing an owning pointer. address is
// useful in diagnostics only; handle is the preferred short-lived identity.
struct askerdway_entity_ref {
    std::uint64_t address = 0;
    std::uint32_t handle = askerdway::engine::entity_handle::invalid_value;
    std::int32_t index = -1;
    std::uint8_t team = 0;
    std::uint8_t reserved[3]{};
};

struct askerdway_hero_snapshot {
    std::uint32_t struct_size = sizeof(askerdway_hero_snapshot);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    askerdway_entity_ref entity{};
    askerdway_vec3 position{};
    askerdway_vec3 velocity{};
    std::int32_t health = -1;
    std::int32_t max_health = -1;
    float mana = -1.0f;
    float physical_armor = 0.0f;
    float magical_resistance = 0.0f;
    std::uint64_t unit_state = 0;
    char designer_name[64]{};
};

struct askerdway_armlet_snapshot {
    std::uint32_t struct_size = sizeof(askerdway_armlet_snapshot);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    askerdway_entity_ref item{};
    std::uint64_t modifier_address = 0;
    std::uint32_t found = 0;
    std::uint32_t toggled_on = 0;
    std::uint32_t modifier_confirmed = 0;
    std::uint32_t item_enabled = 0;
    float cooldown_seconds = 0.0f;
    std::int32_t bonus_strength = 0;
    std::int32_t bonus_damage = 0;
    std::int32_t bonus_attack_speed = 0;
    std::int32_t bonus_armor = 0;
    std::int32_t health_drain_per_second = 0;
    float drain_tick_interval = 0.0f;
};

enum askerdway_projectile_kind : std::uint32_t {
    ASKERDWAY_PROJECTILE_TRACKING = 0,
    ASKERDWAY_PROJECTILE_LINEAR = 1,
    ASKERDWAY_PROJECTILE_RELEASED_ATTACK = 2,
    ASKERDWAY_PROJECTILE_INSTANT_ATTACK = 3,
    ASKERDWAY_PROJECTILE_MELEE_WINDUP = 4,
    ASKERDWAY_PROJECTILE_EXTERNAL_DAMAGE = 5,
    ASKERDWAY_PROJECTILE_TOWER_ATTACK = 6,
};

enum askerdway_projectile_flag : std::uint32_t {
    ASKERDWAY_PROJECTILE_POSITION_VALID = 1u << 0,
    ASKERDWAY_PROJECTILE_DAMAGE_COMPLETE = 1u << 1,
    ASKERDWAY_PROJECTILE_IS_ATTACK = 1u << 2,
    ASKERDWAY_PROJECTILE_RANGED_ATTACK = 1u << 3,
    ASKERDWAY_PROJECTILE_USES_PROJECTILE = 1u << 4,
};

struct askerdway_projectile_snapshot {
    std::uint64_t id = 0;
    std::uint32_t kind = ASKERDWAY_PROJECTILE_TRACKING;
    std::uint32_t source_handle = askerdway::engine::entity_handle::invalid_value;
    std::uint32_t target_handle = askerdway::engine::entity_handle::invalid_value;
    std::uint32_t flags = 0;
    std::uint64_t source_entity = 0;
    askerdway_vec3 position{};
    askerdway_vec3 velocity{};
    float speed = 0.0f;
    float radius = 0.0f;
    float remaining_distance = 0.0f;
    float impact_in_ms = -1.0f;
    askerdway_damage_components damage{};
    float confidence = 0.0f;
};

// Particle names are copied into caller-owned storage. `flags` uses the public
// ASKERDWAY_PARTICLE_* bits declared by render.hpp.
struct askerdway_particle_snapshot {
    std::uint64_t sequence = 0;
    std::uint32_t owner_handle = askerdway::engine::entity_handle::invalid_value;
    std::uint32_t flags = 0;
    askerdway_vec3 position{};
    std::uint64_t observed_at_ms = 0;
};

struct askerdway_hero_trace_snapshot {
    std::uint32_t owner_handle = askerdway::engine::entity_handle::invalid_value;
    char abbreviation[4]{};
    askerdway_vec3 position{};
    std::uint64_t expires_at_ms = 0;
};

struct askerdway_modifier_snapshot {
    std::uint64_t address = 0;
    std::uint32_t caster_handle = askerdway::engine::entity_handle::invalid_value;
    std::int32_t stack_count = 0;
    float creation_time = 0.0f;
    float duration = 0.0f;
    float previous_tick_time = 0.0f;
    float tick_interval = 0.0f;
    float estimated_damage_per_tick = 0.0f;
    std::uint32_t high_risk = 0;
    char rtti_name[96]{};
};

enum askerdway_camp_type : std::int32_t {
    ASKERDWAY_CAMP_SMALL = 0,
    ASKERDWAY_CAMP_LARGE = 1,
    ASKERDWAY_CAMP_ANCIENT = 2,
    ASKERDWAY_CAMP_UNKNOWN = -1,
};

struct askerdway_neutral_camp_snapshot {
    askerdway_vec3 position{};
    std::int32_t type = ASKERDWAY_CAMP_UNKNOWN;
    std::uint32_t blocked = 0;
    float approximated_block_radius = 0.0f;
};

struct askerdway_teleport_arrow_snapshot {
    std::uint32_t owner_handle = askerdway::engine::entity_handle::invalid_value;
    std::uint32_t complete = 0;
    askerdway_vec3 origin{};
    askerdway_vec3 destination{};
    std::uint64_t first_seen_at_ms = 0;
    std::uint64_t expires_at_ms = 0;
};
