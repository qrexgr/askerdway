#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>

namespace particle_owners {
// A bounded reverse index over the current enemy ability/inventory handles.
// Exact handles retain generation bits; proximity never establishes ownership.
struct binding {
    std::uint32_t source=UINT32_MAX,hero=UINT32_MAX;
    std::uintptr_t hero_entity=0;
    bool item=false;
};
class index {
    std::array<binding,16*64> entries{};
    std::size_t count=0;
    bool overflow=false;
public:
    void add(std::uint32_t source,std::uint32_t hero,std::uintptr_t hero_entity,bool item=false) {
        if(source==0||source==UINT32_MAX||!hero_entity)return;
        // A dropped entry could be a conflicting owner of an existing handle.
        if(count==entries.size()){overflow=true;return;}
        entries[count++]={source,hero,hero_entity,item};
    }
    void finish() {
        std::sort(entries.begin(),entries.begin()+count,
            [](const auto& a,const auto& b){return a.source<b.source;});
    }
    const binding* find(std::uint32_t source) const {
        if(overflow)return nullptr;
        const auto end=entries.begin()+count;
        auto found=std::lower_bound(entries.begin(),end,source,
            [](const auto& entry,std::uint32_t key){return entry.source<key;});
        if(found==end||found->source!=source)return nullptr;
        for(auto next=found+1;next!=end&&next->source==source;++next)
            if(next->hero!=found->hero||next->hero_entity!=found->hero_entity||next->item!=found->item)
                return nullptr;
        return &*found;
    }
};

// Keep both nearest candidates: ambiguity must not depend on iteration order.
// This is used only for ownerless TP source particles, never arbitrary effects.
class proximity_choice {
    float first=std::numeric_limits<float>::infinity();
    float second=std::numeric_limits<float>::infinity();
    binding selected{};
public:
    void add(float distance_squared,std::uint32_t hero,std::uintptr_t entity) {
        if(!entity||!std::isfinite(distance_squared)||distance_squared<0||
            distance_squared>900.0f*900.0f)return;
        if(distance_squared<first) {
            second=first;first=distance_squared;
            selected={UINT32_MAX,hero,entity,false};
        } else if(distance_squared<second)second=distance_squared;
    }
    const binding* unique() const {
        return selected.hero_entity && second-first>=40000.0f ? &selected : nullptr;
    }
};
} // namespace particle_owners
