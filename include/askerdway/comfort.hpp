#pragma once

#include <cstdint>
#include "askerdway/export.hpp"
#include "askerdway/version.hpp"

enum askerdway_comfort_flag : std::uint32_t {
    ASKERDWAY_COMFORT_ENABLED = 1u << 0,
    ASKERDWAY_COMFORT_RESPAWN = 1u << 1,
    ASKERDWAY_COMFORT_TIPS = 1u << 2,
    ASKERDWAY_COMFORT_ALLY_TEXT = 1u << 3,
    ASKERDWAY_COMFORT_ENEMY_TEXT = 1u << 4,
    ASKERDWAY_COMFORT_WHEEL_TEXT = 1u << 5,
    ASKERDWAY_COMFORT_WHEEL_SOUND = 1u << 6,
    ASKERDWAY_COMFORT_ANTISPAM = 1u << 7,
    ASKERDWAY_COMFORT_FLAGS_ALL = (1u << 8) - 1,
};

enum askerdway_comfort_feature : std::uint32_t {
    ASKERDWAY_COMFORT_FEATURE_ITEMS = 1u << 0,
    ASKERDWAY_COMFORT_FEATURE_RESPAWN = 1u << 1,
    ASKERDWAY_COMFORT_FEATURE_TIPS = 1u << 2,
    ASKERDWAY_COMFORT_FEATURE_TEXT = 1u << 3,
    ASKERDWAY_COMFORT_FEATURE_WHEEL_TEXT = 1u << 4,
    ASKERDWAY_COMFORT_FEATURE_WHEEL_SOUND = 1u << 5,
    ASKERDWAY_COMFORT_FEATURE_PINGS = 1u << 6,
    ASKERDWAY_COMFORT_FEATURE_VOICE = 1u << 7,
    ASKERDWAY_COMFORT_FEATURE_ALL = (1u << 8) - 1,
};

enum askerdway_comfort_item_mode : std::uint32_t {
    ASKERDWAY_COMFORT_ITEMS_OFF = 0,
    ASKERDWAY_COMFORT_ITEMS_SELECTED_SUGGESTIONS = 1,
};

// Separate structures/exports: the existing config/status v1 layout is unchanged.
#pragma pack(push, 8)
struct askerdway_comfort_config_v1 {
    std::uint32_t struct_size = sizeof(askerdway_comfort_config_v1);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    std::uint32_t flags = ASKERDWAY_COMFORT_RESPAWN | ASKERDWAY_COMFORT_TIPS |
        ASKERDWAY_COMFORT_ENEMY_TEXT | ASKERDWAY_COMFORT_WHEEL_TEXT |
        ASKERDWAY_COMFORT_WHEEL_SOUND | ASKERDWAY_COMFORT_ANTISPAM;
    std::uint32_t item_mode = ASKERDWAY_COMFORT_ITEMS_SELECTED_SUGGESTIONS;
    std::int32_t silence_hotkey = 0;
    // Selected purchase suggestions only. Ordinary item/readiness messages are retained.
    std::uint32_t suggestion_item_count = 2;
    std::uint32_t suggestion_items[16]{15, 92}; // Mantle of Intelligence, Urn of Shadows (installed catalog).
    std::uint32_t reserved[2]{};
};

struct askerdway_comfort_status_v1 {
    std::uint32_t struct_size = sizeof(askerdway_comfort_status_v1);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    std::uint32_t available_features = 0;
    std::uint32_t silence_requested = 0;
    std::uint32_t full_silence_active = 0;
    std::uint32_t settings_saved = 0;
    std::uint64_t blocked_players = 0;
    std::uint64_t session = 0;
    std::uint32_t reserved[4]{};
};
#pragma pack(pop)

// All setters return askerdway_result (declared in api.hpp).
ASKERDWAY_API std::int32_t AskerdwayApi_GetComfortConfig(askerdway_comfort_config_v1*) noexcept;
ASKERDWAY_API std::int32_t AskerdwayApi_SetComfortConfig(const askerdway_comfort_config_v1*) noexcept;
ASKERDWAY_API std::int32_t AskerdwayApi_GetComfortStatus(askerdway_comfort_status_v1*) noexcept;
ASKERDWAY_API std::int32_t AskerdwayApi_SetComfortSilence(std::uint32_t enabled) noexcept;
ASKERDWAY_API std::int32_t AskerdwayApi_SetComfortPlayerBlocked(std::uint32_t player_id,
    std::uint32_t blocked) noexcept;
