/**
 * @file
 * @brief Снимок разрешённых адресов.
 *
 * Экспортирует диагностику runtime globals и калиброванного layout. Значение адреса
 * действительно только в соответствующем загруженном процессе и не является переносимым RVA.
 */

#pragma once

#include <cstdint>

#include "askerdway/version.hpp"

// Runtime addresses are diagnostic and process-local. They become invalid as
// soon as Dota unloads a module, reconnects or recycles the corresponding
// entity system. Prefer high-level snapshots for feature code.
struct askerdway_runtime_globals_v1 {
    std::uint32_t struct_size = sizeof(askerdway_runtime_globals_v1);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    std::uint64_t client_base = 0;
    std::uint64_t game_entity_system = 0;
    std::uint64_t global_vars = 0;
    std::uint64_t game_rules = 0;
    std::uint64_t local_player_pawn = 0;
    std::uint64_t local_controller = 0;
    std::uint64_t projectile_manager = 0;
    std::uint64_t prepare_unit_orders = 0;
    std::uint64_t entity_chunk_table_offset = 0;
    std::uint64_t entity_entry_stride = 0;
    // [0] native client UI-state address: 0 invalid, 1 loading, 2 map, 3 dashboard.
    std::uint64_t reserved[8]{};
};
