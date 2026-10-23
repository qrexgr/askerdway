/**
 * @file
 * @brief Интерфейс портретов.
 *
 * initialize/shutdown управляют ресурсами атласа. blit_bgra получает буфер вызывающей стороны;
 * размеры и stride описывают top-down BGRA поверхность.
 */

#pragma once

#include <cstdint>

namespace hero_portraits {

bool initialize(void* module_handle);
void shutdown();
bool is_available(const char* designer_name);

// Composites a circular hero portrait into a top-down BGRA DIB surface.
// Blue tinting is evaluated from the original RGBA pixels for every draw.
bool blit_bgra(
    std::uint8_t* destination,
    int destination_width,
    int destination_height,
    int destination_stride,
    int center_x,
    int center_y,
    int diameter,
    const char* designer_name,
    bool blue_tint,
    float opacity = 1.0f);

} // namespace hero_portraits
