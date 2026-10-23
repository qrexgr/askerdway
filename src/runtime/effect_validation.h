#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace effect_validation {
// Per-poll limits of refresh_modifier_effect_events. The read budget counts
// modifier-field reads (caster, creation, tick) shared by validation and both
// discovery queues; local/friendly discovery keeps a capped share of it.
inline constexpr unsigned read_budget=256;
inline constexpr unsigned priority_visits=64;
inline constexpr unsigned priority_targets=2;
inline constexpr unsigned neutral_targets=6;

// Resume point of one bounded discovery queue: target index, slot inside the
// target's modifier vector and the entity the slot belongs to.
struct queue {
    std::size_t target=0,modifier=0;
    std::uintptr_t address=0;
};

// Exact identity of a proven incoming target tag. A cached address alone is
// never proof: validation requires live vector membership on the same
// recipient generation, the same full caster handle/address and creation time.
struct entry {
    std::uintptr_t modifier=0,target=0,caster=0;
    std::uint32_t target_handle=0xffffffffu,caster_handle=0xffffffffu;
    float creation=0;
    std::uint64_t validated=0;
    char icon[96]{},rtti[192]{};
};

template<std::size_t Size>
inline void assign(char (&out)[Size],const char* in) {
    std::size_t i=0;
    if(in)for(;i+1<sizeof(out)&&in[i];++i)out[i]=in[i];
    for(;i<sizeof(out);++i)out[i]=0;
}

struct cache {
    static constexpr std::size_t capacity=32,per_poll=8;
    std::array<entry,capacity> entries{};

    entry* find(std::uintptr_t modifier,std::uintptr_t target) {
        if(!modifier)return nullptr;
        for(auto& value:entries)if(value.modifier==modifier&&value.target==target)return &value;
        return nullptr;
    }

    // Insert or replace the identity of one tag. A full cache evicts the least
    // recently validated entry; its mark then only lives out its own TTL.
    void track(const entry& value) {
        if(!value.modifier||!value.target)return;
        entry* slot=find(value.modifier,value.target);
        if(!slot)for(auto& candidate:entries)if(!candidate.modifier){slot=&candidate;break;}
        if(!slot) {
            slot=&entries[0];
            for(auto& candidate:entries)if(candidate.validated<slot->validated)slot=&candidate;
        }
        *slot=value;
    }

    // Up to per_poll entries, least recently validated first, then grouped by
    // recipient so one live modifier vector read serves all its tags.
    std::size_t select(std::array<entry*,per_poll>& out) {
        std::array<bool,capacity> taken{};
        std::size_t count=0;
        while(count<out.size()) {
            std::size_t best=capacity;
            for(std::size_t i=0;i<capacity;++i)
                if(entries[i].modifier&&!taken[i]&&
                    (best==capacity||entries[i].validated<entries[best].validated))best=i;
            if(best==capacity)break;
            taken[best]=true;out[count++]=&entries[best];
        }
        for(std::size_t i=1;i<count;++i)
            for(std::size_t j=i;j&&out[j-1]->target>out[j]->target;--j)std::swap(out[j-1],out[j]);
        return count;
    }
};
}
