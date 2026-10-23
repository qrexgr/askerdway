#pragma once
#include <array>
#include <cstdint>

namespace player_colors {
// The ten ordinary player slots. Mix a small neutral component into the
// saturated HUD colors so thin route lines remain readable without glare.
inline constexpr std::array<std::uint32_t,10> palette{
    0xff7533,0xbfff66,0xbf00bf,0x0bf0f3,0x006bff,
    0xc286fe,0x47b4a1,0xf7d965,0x218300,0x0069a4};
inline constexpr std::uint32_t muted(int player_id) noexcept {
    if(player_id<0||player_id>=static_cast<int>(palette.size()))return 0xc0b38d;
    const auto color=palette[player_id];
    std::uint32_t result=0;
    for(unsigned shift=0;shift<24;shift+=8)
        result|=((((color>>shift)&255)*4+128)/5)<<shift;
    return result;
}
} // namespace player_colors
