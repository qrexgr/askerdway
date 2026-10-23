/**
 * @file
 * @brief Архивный интерфейс Panorama.
 *
 * Объявления альтернативного HUD, не входящего в текущий target. Перед повторным включением
 * требуется отдельно проверить ABI Panorama, потоки callback и teardown.
 */

#pragma once

#include <Windows.h>

// Only registration is marshalled through the window procedure. JavaScript is
// executed later by Panorama's own per-frame callback list.
constexpr UINT register_panorama_frame_callback_message = WM_APP + 0x4A2;

bool queue_panorama_hud_setup(HWND game_window);
bool queue_panorama_hud_snapshot(HWND game_window);
bool install_panorama_frame_dispatch(HWND game_window);
bool panorama_frame_dispatch_installed();
void uninstall_panorama_frame_dispatch();
void execute_pending_panorama_hud_script();
void shutdown_panorama_hud(HWND game_window);
