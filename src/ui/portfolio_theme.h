/**
 * @file
 * @brief Тема ImGui.
 *
 * Централизует палитру, размеры и настройку стиля. Изменения здесь должны влиять на оформление,
 * не на состояние игрового runtime.
 */

#pragma once

#include "imgui.h"

namespace portfolio_theme {

inline constexpr ImVec4 accent{169.0f / 255.0f, 128.0f / 255.0f, 1.0f, 1.0f};
inline constexpr ImVec4 background{20.0f / 255.0f, 20.0f / 255.0f, 23.0f / 255.0f, 1.0f};
inline constexpr ImVec4 sidebar{24.0f / 255.0f, 24.0f / 255.0f, 28.0f / 255.0f, 1.0f};
inline constexpr ImVec4 panel{24.0f / 255.0f, 24.0f / 255.0f, 28.0f / 255.0f, 1.0f};
inline constexpr ImVec4 control{33.0f / 255.0f, 33.0f / 255.0f, 39.0f / 255.0f, 1.0f};
inline constexpr ImVec4 control_hover{44.0f / 255.0f, 44.0f / 255.0f, 52.0f / 255.0f, 1.0f};
inline constexpr ImVec4 text{0.92f, 0.92f, 0.94f, 1.0f};
inline constexpr ImVec4 text_muted{99.0f / 255.0f, 99.0f / 255.0f, 118.0f / 255.0f, 1.0f};
inline constexpr ImVec4 border{35.0f / 255.0f, 35.0f / 255.0f, 43.0f / 255.0f, 0.70f};
inline constexpr ImVec4 success{0.35f, 0.78f, 0.48f, 1.0f};
inline constexpr ImVec4 warning{1.0f, 0.68f, 0.20f, 1.0f};
inline constexpr ImVec4 danger{0.92f, 0.31f, 0.34f, 1.0f};

inline constexpr float sidebar_width = 76.0f;
inline constexpr float shell_rounding = 12.0f;
inline constexpr float card_rounding = 6.0f;

inline void apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.WindowRounding = shell_rounding;
    style.WindowBorderSize = 1.0f;
    style.ChildRounding = card_rounding;
    style.ChildBorderSize = 0.0f;
    style.FramePadding = ImVec2(12.0f, 8.0f);
    style.FrameRounding = 6.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(7.0f, 6.0f);
    style.ScrollbarSize = 5.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabMinSize = 8.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 4.0f;
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;
    style.CurveTessellationTol = 0.75f;
    style.CircleTessellationMaxError = 0.30f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = text;
    colors[ImGuiCol_TextDisabled] = text_muted;
    colors[ImGuiCol_WindowBg] = background;
    colors[ImGuiCol_TitleBg] = background;
    colors[ImGuiCol_TitleBgActive] = background;
    colors[ImGuiCol_TitleBgCollapsed] = background;
    colors[ImGuiCol_ChildBg] = panel;
    colors[ImGuiCol_PopupBg] = panel;
    colors[ImGuiCol_Border] = border;
    colors[ImGuiCol_Separator] = border;
    colors[ImGuiCol_FrameBg] = control;
    colors[ImGuiCol_FrameBgHovered] = control_hover;
    colors[ImGuiCol_FrameBgActive] = control_hover;
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = accent;
    colors[ImGuiCol_Button] = control;
    colors[ImGuiCol_ButtonHovered] = control_hover;
    colors[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    colors[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.16f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.24f);
    colors[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.32f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = control;
    colors[ImGuiCol_ScrollbarGrabHovered] = control_hover;
    colors[ImGuiCol_ScrollbarGrabActive] = accent;
    colors[ImGuiCol_PlotHistogram] = accent;
}

} // namespace portfolio_theme
