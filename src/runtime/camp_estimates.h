// Generated from installed npc_units.txt and npc_abilities.txt.
// CRC32: 372ae8e0, 41815067.
// Unseen estimate assumes one initial pack, with no observed stacks.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
namespace camp_estimates {
struct range {int count_min=0,count_max=0,gold_min=0,gold_max=0;};
inline constexpr std::array<range,5> initial{{{0,0,0,0},
{3,5,40,61},
{2,4,38,87},
{2,3,74,123},
{3,3,150,178},
}};
inline range get(int kind,float game_seconds){
    if(kind<1||kind>=int(initial.size()))return {};
    auto value=initial[kind];
    const int level=std::clamp(std::isfinite(game_seconds)?int(std::max(0.f,game_seconds)/450.f):0,0,30);
    value.gold_min+=value.count_min*level*1;
    value.gold_max+=value.count_max*level*1;
    return value;
}
}
