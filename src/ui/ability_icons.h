#pragma once
#include <cstdint>
namespace ability_icons {
bool initialize(void* module);
void shutdown();
bool is_available(const char* name);
bool blit_bgra(std::uint8_t* destination, int width, int height, int stride,
    int center_x, int center_y, int size, const char* name, float opacity = 1.0f);
}
