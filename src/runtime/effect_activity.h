#pragma once
#include <cmath>
#include <cstdint>

namespace effect_activity {
// A resident modifier is state, not a new action. Only a recent creation or
// advancing engine tick can publish an effect event; unchanged samples stay silent.
struct clock {
    std::uintptr_t modifier=0;
    std::uint32_t caster=0xffffffffu;
    float creation=0,previous_tick=0;
    bool initialized=false;
    bool observe(std::uintptr_t address,std::uint32_t owner,float created,float tick,float game_time) {
        if(!std::isfinite(game_time)||game_time<0||!std::isfinite(created)||created<0||
            created>game_time+.5f||!std::isfinite(tick)||tick<0||tick>game_time+.5f)return false;
        const bool same=initialized&&modifier==address&&caster==owner&&creation==created;
        const bool recent_tick=tick>0&&tick>=game_time-1.5f;
        const bool event=same?(recent_tick&&tick>previous_tick+.001f):
            (recent_tick||(created>0&&created>=game_time-1.5f));
        modifier=address;caster=owner;creation=created;previous_tick=tick;initialized=true;
        return event;
    }
};
}
