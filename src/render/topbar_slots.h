#pragma once
#include <array>
#include <cstddef>
#include <span>

namespace topbar_slots {
// Preserve native 0..4 / 5..9 placement in regular matches. Demo/custom player
// IDs can span both ranges; never stack two enemies on the same portrait.
template<class Status>
std::array<int,16> assign(std::span<const Status> statuses) {
    std::array<int,16> slots;slots.fill(-1);
    for(const unsigned team:{2u,3u}) {
        std::array<bool,5> occupied{};bool canonical=true;
        for(std::size_t i=0;i<statuses.size()&&i<slots.size();++i) {
            const auto& s=statuses[i];if(s.team!=team)continue;
            const int slot=s.player_id-(team==3?5:0);
            if(slot<0||slot>=5||occupied[slot])canonical=false;
            else occupied[slot]=true;
        }
        for(std::size_t i=0;i<statuses.size()&&i<slots.size();++i) {
            const auto& s=statuses[i];if(s.team!=team)continue;
            if(canonical){slots[i]=s.player_id-(team==3?5:0);continue;}
            int before=0;
            for(std::size_t j=0;j<statuses.size();++j) {
                const auto& other=statuses[j];if(other.team!=team)continue;
                // Unknown/equal ids keep snapshot order after known players.
                if((other.player_id>=0&&(s.player_id<0||other.player_id<s.player_id))||
                   (other.player_id==s.player_id&&j<i))++before;
            }
            if(before<5)slots[i]=before;
        }
    }
    return slots;
}
}
