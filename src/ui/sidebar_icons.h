/**
 * @file
 * @brief Интерфейс значков меню.
 *
 * Перечисляет значки и общий вызов их отрисовки в ImGui. Реализация инкапсулирует геометрию
 * конкретных пиктограмм.
 */

#pragma once

#include "imgui.h"

struct ID3D11Device;

namespace sidebar_icons {

enum class icon {
    armlet,
    render,
    comfort,
    diagnostics,
};

bool initialize(void* module_handle, ID3D11Device* device);
void shutdown();

void draw(ImDrawList* draw_list, icon value, ImVec2 minimum, ImVec2 maximum,
    ImU32 color);

} // namespace sidebar_icons
