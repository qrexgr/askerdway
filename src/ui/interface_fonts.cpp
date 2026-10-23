/**
 * @file
 * @brief Шрифты интерфейса.
 *
 * Настраивает шрифты ImGui для текущего контекста. Вызывается при создании интерфейса, а не при
 * каждом кадре.
 */

#include "interface_fonts.h"
#include "sidebar_icon_resources.h"

#include <Windows.h>

#include "imgui.h"

namespace interface_fonts {
namespace {
HANDLE regular_handle = nullptr, bold_handle = nullptr;
void* resource_bytes(HMODULE module, int id, DWORD& size) {
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return nullptr;
    size = SizeofResource(module, resource);
    return LockResource(LoadResource(module, resource));
}
}

bool load(HMODULE module) {
    DWORD regular_size = 0, bold_size = 0, count = 0;
    void* regular = resource_bytes(module, IDR_NORDWAND_REGULAR, regular_size);
    void* bold = resource_bytes(module, IDR_NORDWAND_BOLD, bold_size);
    if (!regular || !bold || !regular_size || !bold_size) return false;
    // Private, process-local registration also supplies the cached GDI HUD.
    if (!regular_handle)
        regular_handle = AddFontMemResourceEx(regular, regular_size, nullptr, &count);
    if (!bold_handle)
        bold_handle = AddFontMemResourceEx(bold, bold_size, nullptr, &count);
    if (!regular_handle || !bold_handle) { shutdown(); return false; }
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig config{};
    config.FontDataOwnedByAtlas = false; // Resource memory belongs to the DLL.
    // Nordwand has fine horizontal strokes. Vertical oversampling matters as
    // much as horizontal coverage at menu sizes; geometry AA cannot fix an
    // undersampled glyph atlas. Keep fractional advances for the mono face.
    config.OversampleH = 3;
    config.OversampleV = 3;
    config.RasterizerMultiply = 1.10f;
    config.PixelSnapH = false;
    io.FontDefault = io.Fonts->AddFontFromMemoryTTF(
        regular, static_cast<int>(regular_size), 18.0f, &config,
        io.Fonts->GetGlyphRangesCyrillic()); // Preserve player names.
    ImGui::GetStyle().FontSizeBase = 18.0f;
    return io.FontDefault != nullptr;
}

void shutdown() {
    if (bold_handle) RemoveFontMemResourceEx(bold_handle);
    if (regular_handle) RemoveFontMemResourceEx(regular_handle);
    regular_handle = bold_handle = nullptr;
}

} // namespace interface_fonts
