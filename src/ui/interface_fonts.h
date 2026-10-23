/**
 * @file
 * @brief Интерфейс загрузки шрифтов.
 *
 * Описывает настройку шрифтов существующего ImGui-контекста. Жизненный цикл атласа управляется
 * ImGui.
 */

#pragma once
#include <Windows.h>

namespace interface_fonts {

inline constexpr wchar_t family[] = L"Nordwand Mono";
bool load(HMODULE module);
void shutdown();

} // namespace interface_fonts
