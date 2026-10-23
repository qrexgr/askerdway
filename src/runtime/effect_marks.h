#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
namespace effect_marks {
enum class kind { drop, target };
struct entry {
    std::uint64_t key=0,expires=0,observed=0;
    kind type=kind::drop;
    std::array<float,3> position{};
    char icon[96]{},label[32]{};
};
struct store {
    std::array<entry,32> entries{};
    void observe(std::uint64_t key,kind type,const std::array<float,3>& position,
        const char* icon,const char* label,std::uint64_t now) {
        if(!key||!icon||!*icon)return;
        for(const auto value:position)if(!std::isfinite(value)||std::abs(value)>20000)return;
        entry* selected=nullptr;
        for(auto& value:entries)if(value.key==key&&value.type==type){selected=&value;break;}
        if(!selected)selected=&*std::min_element(entries.begin(),entries.end(),
            [](const auto& a,const auto& b){return a.expires<b.expires;});
        selected->key=key;selected->type=type;selected->position=position;
        selected->observed=now;selected->expires=now+(type==kind::drop?8000:350);
        const auto copy=[](auto& out,const char* in){std::memset(out,0,sizeof(out));if(in)for(std::size_t i=0;i+1<sizeof(out)&&in[i];++i)out[i]=in[i];};
        copy(selected->icon,icon);copy(selected->label,label);
    }
};
}
