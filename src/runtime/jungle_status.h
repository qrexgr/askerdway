#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace jungle_status {
struct unit_snapshot {
    struct point {float x=0,y=0,z=0;} position;
    std::uint32_t index=0;
    int health=0,gold_min=-1,gold_max=-1;
    bool visible=true;
};
struct entry {
    float x=0,y=0;
    int count=0,count_max=0,health=0,gold_min=0,gold_max=0;
    bool known=false,current=false,gold_known=false,possibly_cleared=false,empty_confirmed=false;
    std::uint64_t changed_at=0,farm_at=0;
    std::array<std::uint32_t,32> members{};
    unsigned member_count=0;
    char hero[96]{};
};
struct store {
    std::array<entry,64> entries{};
    std::uintptr_t session=0;
    float game_time=0;
    void world(std::uintptr_t rules,float time){
        if(rules!=session||time+5<game_time)entries={};
        else if(int(time/60)>int(game_time/60))for(auto& e:entries)if(e.empty_confirmed){
            e.empty_confirmed=false;e.possibly_cleared=true;
        }
        session=rules;game_time=time;
    }
    template<class Camps,class Units> void update(const Camps& camps,unsigned count,const Units& units,unsigned unit_count,std::uint64_t now) {
        for(unsigned i=0;i<count&&i<entries.size();++i){
            auto& status=entries[i];const auto& camp=camps[i];
            if(std::abs(status.x-camp.position.x)>8||std::abs(status.y-camp.position.y)>8)status={};
            status.x=camp.position.x;status.y=camp.position.y;
            int n=0,dead=0,hp=0,low=0,high=0;bool gold=true;
            std::array<std::uint32_t,32> members{};unsigned member_count=0;
            for(unsigned j=0;j<unit_count;++j){const auto& unit=units[j];
                // Hidden replication retains old HP/origins after a neutral dies.
                // It can preserve history, but cannot count as a live observation.
                if constexpr(requires {unit.visible;})if(!unit.visible&&unit.health>0)continue;
                const auto dx=unit.position.x-status.x,dy=unit.position.y-status.y;
                if(dx*dx+dy*dy>1050.f*1050.f)continue;
                // Assign each unit to its nearest camp, avoiding overlapping ranges.
                bool nearest=true;
                for(unsigned k=0;k<count;++k)if(k!=i){
                    const auto x=unit.position.x-camps[k].position.x,y=unit.position.y-camps[k].position.y;
                    if(x*x+y*y<dx*dx+dy*dy||(x*x+y*y==dx*dx+dy*dy&&k<i)){nearest=false;break;}
                }
                if(!nearest)continue;
                std::uint32_t key=j+1;
                if constexpr(requires {unit.index;})key=unit.index?unit.index:j+1;
                else if constexpr(requires {unit.entity_index;})key=unit.entity_index;
                if(unit.health<=0){
                    for(unsigned k=0;k<status.member_count;++k)if(status.members[k]==key){++dead;break;}
                    continue;
                }
                ++n;hp+=unit.health;
                if(member_count<members.size())members[member_count++]=key;
                if(unit.gold_min<0||unit.gold_max<0)gold=false;
                else{low+=unit.gold_min;high+=unit.gold_max;}
            }
            status.current=n>0;
            if(n){
                if(!status.known||status.count!=n||status.health!=hp||status.gold_min!=low||status.gold_max!=high)
                    status.changed_at=now;
                status.known=true;status.count=n;status.health=hp;status.gold_min=low;status.gold_max=high;
                status.members=members;status.member_count=member_count;
                status.gold_known=gold;status.possibly_cleared=false;status.empty_confirmed=false;
            }else if(status.known&&status.count>0&&dead>=status.count){
                status.empty_confirmed=true;status.possibly_cleared=false;
                status.count=status.health=status.gold_min=status.gold_max=0;status.changed_at=now;
            }else if(status.known&&(dead>0||(status.farm_at&&now>=status.farm_at&&now-status.farm_at<15000)))
                status.possibly_cleared=true;
        }
    }
    template<class Position> void farming(const Position& position,const char* hero,std::uint64_t at,unsigned count) {
        float distance=1050.f*1050.f;entry* selected=nullptr;
        for(unsigned i=0;i<count&&i<entries.size();++i){auto& e=entries[i];
            const auto dx=position.x-e.x,dy=position.y-e.y;
            if(dx*dx+dy*dy<distance){distance=dx*dx+dy*dy;selected=&e;}
        }
        if(selected&&at>=selected->farm_at){selected->farm_at=at;std::snprintf(selected->hero,sizeof(selected->hero),"%s",hero);}
    }
};
}
