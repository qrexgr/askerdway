/**
 * @file
 * @brief Версионированная конфигурация.
 *
 * update_mask выбирает изменяемые поля. Логические значения представлены uint32_t и принимают
 * 0/1. Поля reserved сохраняют место для расширения ABI; изменение порядка и типов ломает
 * совместимость.
 */

#pragma once

#include <cstdint>

#include "askerdway/version.hpp"

// SetConfig only modifies fields selected in update_mask.
enum askerdway_config_field : std::uint64_t {
    ASKERDWAY_CONFIG_ARMLET_ENABLED = 1ull << 0,
    ASKERDWAY_CONFIG_ARMLET_HOTKEY = 1ull << 1,
    ASKERDWAY_CONFIG_SAFETY_MARGIN = 1ull << 2,
    ASKERDWAY_CONFIG_DANGEROUS_ABUSE = 1ull << 3,
    ASKERDWAY_CONFIG_LOCAL_FALLBACK = 1ull << 4,
    ASKERDWAY_CONFIG_INSTANT_HEALS = 1ull << 5,
    ASKERDWAY_CONFIG_INSTANT_HEAL_MASK = 1ull << 6,
    ASKERDWAY_CONFIG_DEBUG_LOGGING = 1ull << 7,
    ASKERDWAY_CONFIG_PARTICLE_FOW = 1ull << 8,
    ASKERDWAY_CONFIG_HERO_TRAILS = 1ull << 9,
    ASKERDWAY_CONFIG_TRACE_LIFETIME = 1ull << 10,
    ASKERDWAY_CONFIG_TELEPORT_ARROWS = 1ull << 11,
    ASKERDWAY_CONFIG_CAMP_STATUS = 1ull << 12,
    ASKERDWAY_CONFIG_MINIMAP_RIGHT = 1ull << 13,
    ASKERDWAY_CONFIG_ALL = (1ull << 14) - 1,
    ASKERDWAY_CONFIG_DRAW_BYPASS = 1ull << 14,
    ASKERDWAY_CONFIG_CREEP_TRACKER = 1ull << 15,
};

enum askerdway_instant_heal_group : std::uint32_t {
    ASKERDWAY_HEAL_STICK_WAND = 1u << 0,
    ASKERDWAY_HEAL_FAERIE_FIRE = 1u << 1,
    ASKERDWAY_HEAL_LOTUS = 1u << 2,
    ASKERDWAY_HEAL_HOLY_LOCKET = 1u << 3,
    ASKERDWAY_HEAL_TEAM_ITEMS = 1u << 4,
    ASKERDWAY_HEAL_CHEESE = 1u << 5,
    ASKERDWAY_HEAL_ALL = (1u << 6) - 1,
};

#pragma pack(push, 8)
struct askerdway_config_v1 {
    std::uint32_t struct_size = sizeof(askerdway_config_v1);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    std::uint64_t update_mask = 0;

    std::uint32_t armlet_enabled = 0;
    std::int32_t armlet_hotkey = 0x75; // VK_F6 without requiring Windows.h.
    float safety_margin_hp = 325.0f;
    std::uint32_t allow_dangerous_abuse = 0;
    std::uint32_t allow_local_only_fallback = 0;
    std::uint32_t use_instant_heals = 1;
    std::uint32_t instant_heal_mask =
        ASKERDWAY_HEAL_STICK_WAND | ASKERDWAY_HEAL_FAERIE_FIRE;
    std::uint32_t debug_logging = 0;

    std::uint32_t render_particles_through_fog = 0;
    std::uint32_t hero_particle_trails = 0;
    std::uint32_t trace_lifetime_seconds = 10;
    std::uint32_t teleport_arrows = 1;
    std::uint32_t neutral_camp_status = 0;
    std::uint32_t minimap_on_right = 0;
    std::uint32_t diagnostic_draw_bypass = 0; // Reserved slot 0: 1 = bypass world_frame::render, 0 = draw
    std::uint32_t creep_tracker = 1; // Reserved slot 1; independent of hero trails.
    std::uint32_t reserved[6]{};
};
#pragma pack(pop)
