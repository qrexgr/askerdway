#include "ability_icons.h"
#include "ability_icon_manifest.h"
#include "sidebar_icon_resources.h"
#include "embedded_image.h"
#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_map>

namespace ability_icons {
namespace {
embedded_image::rgba_image atlas;
std::unordered_map<std::string_view, const ability_icon_manifest::entry*> entries;
}
bool initialize(void* module) {
    shutdown();
    if (!embedded_image::load(static_cast<HMODULE>(module), IDR_ABILITY_ICON_ATLAS, atlas))
        return false;
    for (const auto& entry : ability_icon_manifest::entries) entries.emplace(entry.name, &entry);
    return true;
}
void shutdown() { entries.clear(); atlas = {}; }
bool is_available(const char* name) { return name && entries.contains(name); }
bool blit_bgra(std::uint8_t* destination, int width, int height, int stride,
    int center_x, int center_y, int size, const char* name, float opacity) {
    if (!destination || !name || width <= 0 || height <= 0 || size <= 0 || stride < width * 4)
        return false;
    const auto found = entries.find(name);
    if (found == entries.end()) return false;
    const auto& entry = *found->second;
    constexpr auto tile = ability_icon_manifest::tile_size;
    const int left = center_x - size / 2, top = center_y - size / 2;
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const int dx = left + x, dy = top + y;
        if (dx < 0 || dy < 0 || dx >= width || dy >= height) continue;
        // Sample pixel centers with bilinear filtering inside this atlas tile.
        // Resampling occurs only when a cached CPU icon changes, never per frame.
        const float fx=std::clamp((x+.5f)*tile/size-.5f,0.f,float(tile-1));
        const float fy=std::clamp((y+.5f)*tile/size-.5f,0.f,float(tile-1));
        const int sx=int(fx),sy=int(fy);
        const float tx=fx-sx,ty=fy-sy;
        float source[4]{};
        for(int iy=0;iy<2;++iy)for(int ix=0;ix<2;++ix) {
            const auto px=entry.column*tile+std::min(sx+ix,tile-1);
            const auto py=entry.row*tile+std::min(sy+iy,tile-1);
            const auto* sample=atlas.pixels.data()+(static_cast<std::size_t>(py)*atlas.width+px)*4;
            const float weight=(ix?tx:1-tx)*(iy?ty:1-ty);
            for(int channel=0;channel<4;++channel)source[channel]+=sample[channel]*weight;
        }
        auto* target = destination + static_cast<std::size_t>(dy) * stride + dx * 4;
        const float alpha = source[3] / 255.0f * std::clamp(opacity, 0.0f, 1.0f);
        for (int channel = 0; channel < 3; ++channel)
            target[channel] = static_cast<std::uint8_t>(source[2-channel] * alpha + target[channel] * (1.0f-alpha));
        if (target[0] == 0 && target[1] == 0 && target[2] == 0) target[0] = 1;
        target[3] = 255;
    }
    return true;
}
}
