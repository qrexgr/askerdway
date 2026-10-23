/**
 * @file
 * @brief Снимок разрешённых полей schema.
 *
 * Показывает найденные смещения полей и состояние разрешения. Нулевое значение ещё не означает
 * валидное поле; готовность определяется инициализацией и обязательностью запроса.
 */

#pragma once

#include <cstdint>

#include "askerdway/version.hpp"

// Snapshot of the schema fields currently used by the runtime. Zero means the
// field was not resolved. These are offsets, never absolute addresses.
//
// This structure is primarily a maintenance tool: after a patch, compare the
// resolved/missing fields without searching console output or stepping through
// initialize_schema_offsets().
struct askerdway_schema_snapshot_v1 {
    std::uint32_t struct_size = sizeof(askerdway_schema_snapshot_v1);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    std::uint32_t resolved_required = 0;
    std::uint32_t required_total = 0;

    std::int64_t entity_health = 0;
    std::int64_t entity_max_health = 0;
    std::int64_t entity_identity = 0;
    std::int64_t identity_designer_name = 0;
    std::int64_t entity_scene_node = 0;
    std::int64_t entity_team = 0;
    std::int64_t entity_owner_handle = 0;
    std::int64_t entity_velocity = 0;
    std::int64_t scene_node_origin = 0;

    std::int64_t npc_modifier_manager = 0;
    std::int64_t npc_inventory = 0;
    std::int64_t npc_player_owner_id = 0;
    std::int64_t npc_hull_radius = 0;
    std::int64_t npc_physical_armor = 0;
    std::int64_t npc_magical_resistance = 0;
    std::int64_t npc_mana = 0;
    std::int64_t npc_unit_state = 0;

    std::int64_t controller_player_id = 0;
    std::int64_t controller_assigned_hero = 0;
    std::int64_t ability_toggle_state = 0;
    std::int64_t ability_cooldown = 0;
    std::int64_t inventory_items = 0;
    std::int64_t item_charges = 0;
    std::int64_t item_enabled = 0;
    std::int64_t neutral_spawner_type = 0;

    // Optional diagnostic offsets: [0] rules neutral spawn boxes, [1] physical item handle.
    // Modifier manager calibration: [2] active count, [3] data pointer.
    // Ability observations: [4] vector, [5] phase, [6] cast start,
    // [7] channel start, [8] last cast click. Lane clock: [9] creep spawn
    // epoch, [10] game start, [11] spawning enabled, [12] NPC move speed,
    // [13] native GameRules phase (distinct from the VScript enum),
    // [14] per-team NPC visibility array, [15] model team visibility mask.
    // Zero = unavailable.
    std::int64_t reserved[16]{};
};
