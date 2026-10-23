#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace neutral_spawn_boxes {
// Shared network value layout: NeutralSpawnBoxes_t in the current server
// schema. The client value array was independently read in the live session.
// No string/pointer field is dereferenced; every volume is validated first.
inline constexpr std::size_t stride=0x38;
struct box { std::array<float,3> minimum{},maximum{},origin{};int type=0;std::uint32_t spawner=0; };
inline bool decode(std::span<const std::uint8_t> bytes,box& output) {
    if(bytes.size()!=stride)return false;
    box candidate;
    std::memcpy(candidate.minimum.data(),bytes.data(),12);
    std::memcpy(candidate.maximum.data(),bytes.data()+12,12);
    std::memcpy(candidate.origin.data(),bytes.data()+24,12);
    std::memcpy(&candidate.spawner,bytes.data()+0x30,4);
    std::memcpy(&candidate.type,bytes.data()+0x34,4);
    if(candidate.type<0||candidate.type>4||candidate.spawner>=32768)return false;
    for(unsigned i=0;i<3;++i) {
        const auto low=candidate.minimum[i],high=candidate.maximum[i],origin=candidate.origin[i];
        if(!std::isfinite(low)||!std::isfinite(high)||!std::isfinite(origin)||
            std::abs(low)>20000||std::abs(high)>20000||std::abs(origin)>20000||
            high<=low||high-low>4096)return false;
    }
    output=candidate;return true;
}
}
