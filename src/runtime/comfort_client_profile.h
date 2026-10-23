#pragma once
#include <cstdint>

namespace askerdway::comfort {
struct client_profile {
    unsigned timestamp, image_size;
    std::uintptr_t voice, wheel, format, audio, receive, wheel_table, wheel_default;
};
// Validated presentation ABI; independent of Steam and the local account.
inline constexpr client_profile client_profiles[]{
    {1792097557u,115580928u,0x19d4240,0x2244fa0,0x22659c0,0x23a8bd0,0x19e2830,0x3dfa6c8,0x59a41d0},
    {1792686196u,115580928u,0x19d4210,0x2244f70,0x2265990,0x23a8ba0,0x19e2800,0x3dfa6c8,0x59a41d0},
};
inline const client_profile* find_client_profile(unsigned timestamp,unsigned image_size) {
    for(const auto& profile:client_profiles)
        if(profile.timestamp==timestamp&&profile.image_size==image_size)return &profile;
    return nullptr;
}
}
