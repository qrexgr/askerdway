#pragma once
#include <cstdint>
#include "askerdway/version.hpp"

// Stable POD diagnostics: callers never depend on private C++ container layout.
#pragma pack(push, 8)
struct askerdway_render_trace_v1 {
    std::uint32_t owner = 0;
    float position[3]{};
    std::uint64_t created_at_ms = 0, expires_at_ms = 0;
    char hero[96]{}, trigger[64]{}, ability[96]{};
};
struct askerdway_render_route_v1 {
    std::uint32_t owner = 0;
    float origin[3]{}, destination[3]{};
    std::uint32_t has_origin = 0, has_destination = 0;
    std::uint32_t observed = 0, finished = 0, canceled = 0;
    std::uint64_t expires_at_ms = 0;
};
struct askerdway_render_enemy_v1 {
    std::uint64_t address = 0;
    std::uint32_t owner = 0, ability_count = 0, hidden = 0;
    char hero[96]{};
};
struct askerdway_render_scroll_v1 {
    std::int32_t player = -1, charges = -1;
    std::uint32_t team = 0, known = 0, has_scroll = 0;
    float cooldown = 0;
};
struct askerdway_render_snapshot_v1 {
    std::uint32_t struct_size = sizeof(askerdway_render_snapshot_v1);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    std::uint64_t captured_at_ms = 0;
    std::uint64_t hook_calls = 0, fow_hidden_calls = 0, forced_updates = 0;
    std::uint32_t trace_count = 0, route_count = 0, enemy_count = 0, scroll_count = 0;
    std::uint32_t scan_us = 0, render_us = 0;
    askerdway_render_trace_v1 traces[64]{};
    askerdway_render_route_v1 routes[16]{};
    askerdway_render_enemy_v1 enemies[16]{};
    askerdway_render_scroll_v1 scrolls[10]{};
    struct particle {
        std::uint64_t effect = 0, collection = 0, observed_at = 0;
        std::uint32_t owner = 0, flags = 0, visibility = 0;
        float position[3]{}, source_position[3]{};
        char name[96]{};
    } particles[32]{};
    std::uint32_t particle_count = 0;
    std::uint64_t world_marker_frames = 0;
    float game_time = -1;
    std::uint32_t game_paused = 0;
    std::uint64_t game_present_calls = 0;
    std::uint32_t frame_render_us = 0, frame_visible_markers = 0, frame_hook_installed = 0;
    std::uint64_t frame_reentrant_calls = 0;
    std::uint32_t frame_object_hook = 0;
};
struct askerdway_render_snapshot_v2 {
    std::uint32_t struct_size = sizeof(askerdway_render_snapshot_v2);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    std::uint64_t captured_at_ms = 0;
    std::uint64_t hook_calls = 0, fow_hidden_calls = 0, forced_updates = 0;
    std::uint32_t trace_count = 0, route_count = 0, enemy_count = 0, scroll_count = 0;
    std::uint32_t scan_us = 0, render_us = 0;
    askerdway_render_trace_v1 traces[64]{};
    askerdway_render_route_v1 routes[16]{};
    askerdway_render_enemy_v1 enemies[16]{};
    askerdway_render_scroll_v1 scrolls[10]{};
    struct particle {
        std::uint64_t effect = 0, collection = 0, observed_at = 0;
        std::uint32_t owner = 0, flags = 0, visibility = 0;
        float position[3]{}, source_position[3]{};
        char name[96]{};
    } particles[32]{};
    std::uint32_t particle_count = 0;
    std::uint64_t world_marker_frames = 0;
    float game_time = -1;
    std::uint32_t game_paused = 0;
    std::uint64_t game_present_calls = 0;
    std::uint32_t frame_render_us = 0, frame_visible_markers = 0, frame_hook_installed = 0;
    std::uint64_t frame_reentrant_calls = 0;
    std::uint32_t frame_object_hook = 0;
    std::uint32_t game_present_us = 0;
    std::uint32_t game_present_peak_us = 0;
    std::uint32_t draw_bypassed = 0;
    // Bounded diagnostic Present interval sample: count, p50/p95/p99/max in us.
    std::uint32_t reserved[5]{};
};
#pragma pack(pop)
