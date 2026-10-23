#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <array>

namespace askerdway::particles {

// Private fields are operands of validated instructions, not a versioned
// table of offsets. Unknown instruction shapes disable the hook.
struct layout {
    std::size_t collection = 0;
    std::size_t debug_name = 0;
    std::size_t visibility = 0;
    std::size_t sort_origin = 0;
    std::size_t owner_pointer = 0;
    std::size_t rendering_enabled = 0;
    std::size_t render_vtable_offset = 0;
    std::size_t update_render_return = 0;
    std::size_t resource_vtable_offset = 0;
    std::uint8_t fog_check_mask = 0;
};

inline bool matches(std::span<const std::uint8_t> bytes, std::size_t at,
    std::span<const int> pattern) noexcept {
    if (at > bytes.size() || pattern.size() > bytes.size() - at) return false;
    for (std::size_t i = 0; i < pattern.size(); ++i)
        if (pattern[i] >= 0 && bytes[at + i] != pattern[i]) return false;
    return true;
}

inline bool resolve_update(std::span<const std::uint8_t> code,
    layout& out) noexcept {
    constexpr int collection_load[]{0x48,0x8B,0x49,-1,0x48,0x8D,0x54,0x24,0x40};
    constexpr int owner_load[]{0x48,0x8B,0x53,-1,0x32,0xC0,0x48,0x85,0xD2,0x74,-1,0x48,0x8B,0x4B,-1,0xE8};
    constexpr int fog_write[]{0x80,0xE2,0xFD,0x02,0xC0,0x0A,0xD0,0x88,0x53,-1};
    constexpr int dispatch[]{0xF6,0xC2,0x03,0x0F,0x94,0xC2,0xFF,0x90,-1,-1,-1,-1};
    constexpr int resource[]{0x48,0x8B,0x01,0xFF,0x50,-1,0x48,0x8B,0x08,0x48,0x85,0xC9};
    unsigned loads = 0, owners = 0, writes = 0, calls = 0, resources = 0;
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (matches(code,i,collection_load)) { out.collection=code[i+3]; ++loads; }
        if (matches(code,i,owner_load)) { out.owner_pointer=code[i+3]; ++owners; }
        if (matches(code,i,fog_write)) { out.visibility=code[i+9]; ++writes; }
        if (matches(code,i,dispatch)) {
            std::uint32_t value = 0;
            std::memcpy(&value,code.data()+i+8,sizeof(value));
            out.render_vtable_offset=value; out.update_render_return=i+12; ++calls;
        }
        if (matches(code,i,resource)) { out.resource_vtable_offset=code[i+5]; ++resources; }
    }
    return loads==1 && owners==1 && writes==1 && calls==1 && resources==1 &&
        out.collection>=8 && out.owner_pointer>=8 && out.visibility>0 &&
        out.render_vtable_offset>=sizeof(void*) &&
        out.render_vtable_offset<=0x1000 && out.render_vtable_offset%sizeof(void*)==0;
}

inline bool resolve_constructor(std::span<const std::uint8_t> code,
    layout& out) noexcept {
    // Contiguous initializers for collection, name, flags, scalar, owner and
    // factory context. Cross-check collection with the independent Update.
    constexpr int fields[]{0x4C,0x89,0x6B,-1,0x4C,0x89,0x6B,-1,
        0xC6,0x43,-1,0x68,0xC7,0x43,-1,0,0,0x80,0x3F,
        0x4C,0x89,0x6B,-1,0x4C,0x89,0x6B,-1,0x4C,0x89,0x6B,-1,0x4C,0x89,0x6B,-1};
    constexpr int origin[]{0x4C,0x89,0x6B,-1,0x44,0x89,0x6B,-1};
    constexpr int render[]{0xC6,0x83,-1,-1,-1,-1,1};
    unsigned sets=0, origins=0, renders=0;
    for (std::size_t i=0;i<code.size();++i) {
        if (matches(code,i,fields) && code[i+3]==out.collection &&
            code[i+22]==out.owner_pointer && static_cast<std::size_t>(code[i+10])+1==out.visibility) {
            out.debug_name=code[i+7]; ++sets;
        }
        if (matches(code,i,origin) && code[i+7]==code[i+3]+8) {
            out.sort_origin=code[i+3]; ++origins;
        }
        if (matches(code,i,render)) {
            std::uint32_t value=0;
            std::memcpy(&value,code.data()+i+2,sizeof(value));
            out.rendering_enabled=value; ++renders;
        }
    }
    return sets==1 && origins==1 && renders==1 && out.debug_name>=8 &&
        out.rendering_enabled>out.owner_pointer && out.rendering_enabled<0x400;
}

inline bool resolve_fog_policy(std::span<const std::uint8_t> code,
    layout& out) noexcept {
    // SetParticleShouldCheckFoW: preserve unrelated bits, shift the supplied
    // bool into the policy bit and store back to the same effect flags byte.
    constexpr int setter[]{0x0F,0xB6,0x41,-1,0x24,-1,0x41,0xC0,0xE3,-1,
        0x41,0x0A,0xC3,0x88,0x41,-1};
    unsigned candidates = 0;
    for (std::size_t i=0; i<code.size(); ++i) {
        if (!matches(code,i,setter) || code[i+3]!=out.visibility ||
            code[i+15]!=out.visibility || code[i+9]>=8) continue;
        const auto mask=static_cast<std::uint8_t>(1u<<code[i+9]);
        if (code[i+5]!=static_cast<std::uint8_t>(~mask)) continue;
        out.fog_check_mask=mask; ++candidates;
    }
    return candidates==1;
}

struct fog_dispatch {
    std::array<std::size_t, 16> returns{};
    std::size_t count = 0;
    std::size_t control_point_vtable_offset = 0;
};

inline bool resolve_fog_dispatch(std::span<const std::uint8_t> code,
    const layout& fields, fog_dispatch& out) noexcept {
    // Independent Dota visibility pass: owner visibility precedes world FOW.
    // Resolve its setter call sites so unrelated rendering/lifecycle calls
    // continue to receive the engine's original bool.
    constexpr int policy[]{0x41,0xF6,0x45,-1,-1,0x0F,0x28,0xF0,0x0F,0x84,-1,-1,-1,-1};
    constexpr int prologue[]{0x48,0x89,0x4C,0x24,0x08,0x55,0x41,0x55,0x41,0x56,0x41,0x57,
        0x48,0x8D,0xAC,0x24,-1,-1,-1,-1,0x48,0x81,0xEC,-1,-1,-1,-1,
        0x4C,0x8B,0x71,-1,0x4C,0x8B,0xE9};
    constexpr int visible[]{0x49,0x8B,0x06,0xB2,0x01,0x49,0x8B,0xCE,0xFF,0x90,-1,-1,-1,-1,0xE9,-1,-1,-1,-1};
    unsigned candidates = 0;
    for (std::size_t at=0; at<code.size(); ++at) {
        if (!matches(code,at,policy) || code[at+3]!=fields.visibility ||
            code[at+4]!=fields.fog_check_mask) continue;
        std::int32_t relative=0;
        std::memcpy(&relative,code.data()+at+10,4);
        const auto target=static_cast<std::int64_t>(at)+14+relative;
        if (target<0 || !matches(code,static_cast<std::size_t>(target),visible)) continue;
        std::uint32_t setter_slot=0;
        std::memcpy(&setter_slot,code.data()+target+10,4);
        if (setter_slot!=fields.render_vtable_offset) continue;
        std::size_t begin=0; unsigned starts=0;
        for (std::size_t i=at>4096?at-4096:0; i<at; ++i)
            if (matches(code,i,prologue) && code[i+30]==fields.collection) { begin=i; ++starts; }
        if (starts!=1 || target<=static_cast<std::int64_t>(at) || target-begin>8192) continue;
        const auto end=static_cast<std::size_t>(target)+19;
        std::size_t owner_begin=0; unsigned owners=0;
        for (std::size_t i=begin; i+4<=at; ++i)
            if (code[i]==0x49 && code[i+1]==0x8B && code[i+2]==0x55 &&
                code[i+3]==fields.owner_pointer) { owner_begin=i; ++owners; }
        if (owners!=1) continue;
        fog_dispatch result{};
        // This world-visibility consumer reads a Vector pointer from the
        // collection's selected control point. Validate the complete ABI use.
        constexpr int control_point[]{0x49,0x8B,0x06,0x49,0x8B,0xCE,
            0x41,0x8B,0x54,0x24,0x08,0xFF,0x90,-1,-1,-1,-1,
            0x41,0x8B,0x54,0x24,0x0C,0xF2,0x0F,0x10,0x00};
        unsigned getters=0;
        for (std::size_t i=at;i<end;++i) {
            if (!matches(code,i,control_point)) continue;
            std::uint32_t slot=0; std::memcpy(&slot,code.data()+i+13,4);
            if (!slot || slot>0x1000 || slot%sizeof(void*)) continue;
            result.control_point_vtable_offset=slot; ++getters;
        }
        if (getters!=1) continue;
        for (std::size_t i=owner_begin; i+6<=end; ++i) {
            if (code[i]!=0xFF || code[i+1]!=0x90) continue;
            std::uint32_t slot=0; std::memcpy(&slot,code.data()+i+2,4);
            if (slot!=fields.render_vtable_offset) continue;
            if (result.count==result.returns.size()) return false;
            result.returns[result.count++]=i+6;
        }
        if (result.count<3) continue;
        out=result; ++candidates;
    }
    return candidates==1;
}
} // namespace askerdway::particles
