/**
 * @file
 * @brief Статусы, возможности и диагностика.
 *
 * Числовые значения предназначены для внешних потребителей. Capability сообщает наличие
 * реализации, а текущие state flags и safety reason — её состояние. Счётчики в reserved имеют
 * именованные диагностические индексы.
 */

#pragma once

#include <cstdint>

#include "askerdway/version.hpp"

enum askerdway_capability : std::uint64_t {
    ASKERDWAY_CAP_ARMLET_AUTOMATION = 1ull << 0,
    ASKERDWAY_CAP_EXTERNAL_DAMAGE_TIMELINE = 1ull << 1,
    ASKERDWAY_CAP_INSTANT_HEALS = 1ull << 2,
    ASKERDWAY_CAP_PARTICLE_FOW_RENDER = 1ull << 3,
    ASKERDWAY_CAP_HERO_PARTICLE_TRAILS = 1ull << 4,
    ASKERDWAY_CAP_TELEPORT_ARROWS = 1ull << 5,
    ASKERDWAY_CAP_NEUTRAL_CAMP_STATUS = 1ull << 6,
    ASKERDWAY_CAP_ENGINE_SNAPSHOTS = 1ull << 7,
    ASKERDWAY_CAP_NATIVE_WORLD_RENDER = 1ull << 8,
    ASKERDWAY_CAP_RENDER_DIAGNOSTICS = 1ull << 9,
};

// Public enum values never depend on the ordering of private implementation
// enums. Translation at the API boundary is required when either side changes.
enum askerdway_armlet_state : std::int32_t {
    ASKERDWAY_ARMLET_WAITING_FOR_CLIENT = 0,
    ASKERDWAY_ARMLET_WAITING_FOR_HERO = 1,
    ASKERDWAY_ARMLET_ITEM_MISSING = 2,
    ASKERDWAY_ARMLET_OFF = 3,
    ASKERDWAY_ARMLET_ACTIVE_CONFIRMED = 4,
    ASKERDWAY_ARMLET_APPLYING = 5,
    ASKERDWAY_ARMLET_LINGERING = 6,
    ASKERDWAY_ARMLET_READ_ERROR = 7,
};

enum askerdway_cycle_state : std::int32_t {
    ASKERDWAY_CYCLE_IDLE = 0,
    ASKERDWAY_CYCLE_ON = 1,
    ASKERDWAY_CYCLE_DISABLING = 2,
    ASKERDWAY_CYCLE_OFF = 3,
    ASKERDWAY_CYCLE_ENABLING = 4,
};

enum askerdway_safety_state : std::int32_t {
    ASKERDWAY_SAFETY_UNAVAILABLE = 0,
    ASKERDWAY_SAFETY_ABUSE_DISABLED = 1,
    ASKERDWAY_SAFETY_HEALTH_ABOVE_THRESHOLD = 2,
    ASKERDWAY_SAFETY_SAFE = 3,
    ASKERDWAY_SAFETY_TELEMETRY_STALE = 4,
    ASKERDWAY_SAFETY_STATE_UNCONFIRMED = 5,
    ASKERDWAY_SAFETY_STUNNED_OR_MUTED = 6,
    ASKERDWAY_SAFETY_COOLDOWN_WAIT = 7,
    ASKERDWAY_SAFETY_COMMAND_UNAVAILABLE = 8,
    ASKERDWAY_SAFETY_INCOMPLETE_EVENT = 9,
    ASKERDWAY_SAFETY_UNKNOWN_MODIFIER = 10,
    ASKERDWAY_SAFETY_THREAT_PENDING = 11,
    ASKERDWAY_SAFETY_LETHAL_CLUSTER = 12,
    ASKERDWAY_SAFETY_HIGH_RISK_BLOCKED = 13,
    ASKERDWAY_SAFETY_HIGH_RISK_ALLOWED = 14,
    ASKERDWAY_SAFETY_GAME_PAUSED = 15,
    ASKERDWAY_SAFETY_SAFE_WITH_INSTANT_HEAL = 16,
};

enum askerdway_state_flag : std::uint64_t {
    ASKERDWAY_STATE_RUNNING = 1ull << 0,
    ASKERDWAY_STATE_SCHEMA_READY = 1ull << 1,
    ASKERDWAY_STATE_AUTOMATION_READY = 1ull << 2,
    ASKERDWAY_STATE_ARMLET_FOUND = 1ull << 3,
    ASKERDWAY_STATE_ARMLET_TOGGLED_ON = 1ull << 4,
    ASKERDWAY_STATE_MODIFIER_CONFIRMED = 1ull << 5,
    ASKERDWAY_STATE_PROJECTILE_FEED_READY = 1ull << 6,
    ASKERDWAY_STATE_GAME_PAUSED = 1ull << 7,
    ASKERDWAY_STATE_STUNNED_OR_MUTED = 1ull << 8,
    ASKERDWAY_STATE_HIGH_RISK = 1ull << 9,
    ASKERDWAY_STATE_PARTICLE_HOOK_READY = 1ull << 10,
    ASKERDWAY_STATE_NATIVE_VIEW_MATRIX_READY = 1ull << 11,
    ASKERDWAY_STATE_WORLD_DRAW_BYPASS = 1ull << 12,
};

// ABI-compatible diagnostics published through the v1 status reserve. The
// timings slot stores scan microseconds in its low 32 bits and native-render
// microseconds in its high 32 bits.
enum askerdway_diagnostic_slot : std::uint32_t {
    ASKERDWAY_DIAG_PARTICLE_OBSERVATIONS = 0,
    ASKERDWAY_DIAG_RESOLVED_PARTICLE_OWNERS = 1,
    ASKERDWAY_DIAG_FOG_ABILITY_EVENTS = 2,
    ASKERDWAY_DIAG_FOG_FARMING_EVENTS = 3,
    ASKERDWAY_DIAG_COMPLETE_TELEPORT_ROUTES = 4,
    ASKERDWAY_DIAG_TOWER_SHOTS = 5,
    ASKERDWAY_DIAG_NATIVE_TIMINGS_US = 6,
    ASKERDWAY_DIAG_VIEW_MATRIX_ADDRESS = 7,
    ASKERDWAY_DIAGNOSTIC_SLOT_COUNT = 8,
};

#pragma pack(push, 8)
struct askerdway_status_v1 {
    std::uint32_t struct_size = sizeof(askerdway_status_v1);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    std::uint64_t capabilities = 0;
    std::uint64_t state_flags = 0;

    std::int32_t armlet_state = ASKERDWAY_ARMLET_WAITING_FOR_CLIENT;
    std::int32_t cycle_state = ASKERDWAY_CYCLE_IDLE;
    std::int32_t safety_state = ASKERDWAY_SAFETY_UNAVAILABLE;
    std::int32_t health = -1;
    std::int32_t max_health = -1;
    std::int32_t armlet_entity_index = -1;
    std::int32_t confirmation_latency_ms = -1;
    std::int32_t expected_cycle_ms = 0;

    float safety_margin_hp = 0.0f;
    float network_latency_ms = -1.0f;
    float incoming_damage = 0.0f;
    float nearest_impact_ms = -1.0f;
    float predicted_minimum_hp = -1.0f;
    float high_risk_interval_ms = -1.0f;

    std::int32_t incoming_event_count = 0;
    std::int32_t tracking_projectile_count = 0;
    std::int32_t linear_projectile_count = 0;
    std::int32_t tower_projectile_count = 0;
    std::int32_t melee_windup_count = 0;
    std::int32_t modifier_tick_count = 0;
    std::int32_t available_heal_amount = 0;
    std::int32_t available_heal_charges = 0;
    std::int32_t planned_heal_amount = 0;

    std::uint64_t orders_queued = 0;
    std::uint64_t orders_dispatched = 0;
    std::uint64_t orders_confirmed = 0;
    std::uint64_t orders_timed_out = 0;
    std::uint64_t state_transitions = 0;
    std::uint64_t instant_heal_uses = 0;
    std::uint64_t particle_updates_forced = 0;

    std::uint32_t active_hero_traces = 0;
    std::uint32_t active_teleport_arrows = 0;
    std::uint32_t neutral_camps = 0;
    std::uint32_t schema_fields_resolved = 0;
    std::uint32_t schema_fields_required = 0;
    std::uint32_t reserved_count = 0; // Number of populated diagnostic slots.

    std::uint64_t hero_address = 0;
    std::uint64_t armlet_address = 0;
    std::uint64_t modifier_address = 0;
    std::uint64_t reserved[8]{}; // Indexed by askerdway_diagnostic_slot.
};
#pragma pack(pop)
