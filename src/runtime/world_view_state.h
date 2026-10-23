#pragma once
#include <array>
#include <cstdint>
#include <cstring>

// Client UI state is independent of GameRules: an ongoing match can be behind
// the dashboard. Read only; never toggle Panorama or infer UI state from keys.
namespace world_view_state {
inline constexpr auto signature=[] {
    std::array<std::uint8_t,70> bytes{};bytes.fill(0xcc);
    for(unsigned i:{0u,16u}) {bytes[i]=0x48;bytes[i+1]=0x8d;bytes[i+2]=0x05;
        for(unsigned j=3;j<7;++j)bytes[i+j]=0;bytes[i+7]=0xc3;}
    bytes[32]=0x8b;bytes[33]=0x81;for(unsigned i=34;i<38;++i)bytes[i]=0;
    bytes[38]=0xc3;
    for(unsigned i:{48u,64u}) {bytes[i]=0xb8;bytes[i+1]=i==48?2:5;
        bytes[i+2]=bytes[i+3]=bytes[i+4]=0;bytes[i+5]=0xc3;}
    return bytes;
}();
inline constexpr auto mask=[] {
    std::array<char,71> value{};value.fill('x');value.back()=0;
    for(unsigned i:{3u,19u,34u})for(unsigned j=0;j<4;++j)value[i+j]='?';
    return value;
}();
struct layout {std::uintptr_t object=0;std::uint32_t offset=0;};
inline bool decode(const std::uint8_t* code,std::uintptr_t instruction,layout& out) {
    out={};if(!code||!instruction)return false;
    for(unsigned i=0;i<signature.size();++i)
        if(mask[i]=='x'&&code[i]!=signature[i])return false;
    std::int32_t displacement=0;std::uint32_t offset=0;
    std::memcpy(&displacement,code+3,4);std::memcpy(&offset,code+34,4);
    if(!offset||offset>0x1000||offset%4)return false;
    out={static_cast<std::uintptr_t>(static_cast<std::intptr_t>(instruction)+7+displacement),offset};
    return true;
}
inline bool visible(int state) noexcept {return state==2;}
}
