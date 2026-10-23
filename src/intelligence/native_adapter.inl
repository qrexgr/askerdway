// Included by the ordered runtime unity unit after native read helpers.
// This adapter has no cast history, trace lifetimes, attribution heuristics,
// teleport lifecycle or camp inference. Those belong exclusively to Rust.
#include "include/askermap.h"
#include <vector>

// The ordered runtime unit defines this reader after poll_render_intelligence.
// Declare its shared native contract before the Rust sensor bridge uses it.
bool resolve_modifier_vector(std::uintptr_t manager, std::uintptr_t client_base,
    std::size_t client_size, int& count, std::uintptr_t& data);
bool initialize_internal_projectile_feed(std::uintptr_t client_base,std::size_t client_size);

namespace rust_map_adapter {
struct body {std::uintptr_t address=0;std::uint32_t handle=UINT32_MAX;char name[96]{};bool hero=false,neutral=false,lane=false;std::size_t modifier_slot=0;};
struct source_member {std::uint32_t source=UINT32_MAX,hero=UINT32_MAX;std::uintptr_t address=0;bool item=false;};
struct sensor_state {
    std::vector<body> bodies;
    std::vector<askermap::observation> events;
    std::vector<askermap::observation> source_events;
    std::vector<source_member> members;
    std::vector<std::pair<std::uint32_t,armlet_vec3>> trees;
    std::uint64_t cursor=0,discovered=0,session=0,last_game=0;
    std::uint64_t jungle_sampled=0;
    std::size_t hero_modifier_cursor=0,neutral_modifier_cursor=0;
    std::uint32_t lost=0;
};
sensor_state sensors;
void neutral_lifecycle(std::uintptr_t address,int& health,bool& hidden){
    bool waiting=false;std::uint8_t life=0;std::uint32_t effects=0;
    if(schema_fields::npc_waiting_to_spawn>0)safe_read(address+schema_fields::npc_waiting_to_spawn,waiting);
    if(schema_fields::entity_life_state>0)safe_read(address+schema_fields::entity_life_state,life);
    if(schema_fields::entity_effects>0)safe_read(address+schema_fields::entity_effects,effects);
    if(waiting||life)health=0;
    hidden=waiting||(effects&0x20u)!=0; // EF_NODRAW, including the inactive neutral pool.
}
bool handle_of(std::uintptr_t system,std::uintptr_t entity,std::uint32_t& handle) {
    std::uintptr_t identity=0;handle=UINT32_MAX;
    return plausible_pointer(entity)&&safe_read(entity+schema_fields::m_pEntityIdentity,identity)&&
        plausible_pointer(identity)&&safe_read(identity+0x10,handle)&&handle!=UINT32_MAX&&
        entity_by_handle(system,handle)==entity;
}
askermap::vec3 vector(const armlet_vec3& p){return {p.x,p.y,p.z};}
bool read_attachment_owner(std::uintptr_t entity,std::uint32_t* result_handle,char* hero,std::size_t capacity,float* point) noexcept {
    char name[96]{};armlet_vec3 position{};std::uint32_t handle=UINT32_MAX;
    std::uintptr_t system=0;
    if(!read_game_entity_system(g_client_base_address.load(std::memory_order_acquire),system))return false;
    // The factory's user token may be a zero-extended full CHandle rather
    // than an address. Resolve and revalidate its generation in either form.
    if(entity<=UINT32_MAX)entity=entity_by_handle(system,static_cast<std::uint32_t>(entity));
    if(!handle_of(system,entity,handle)||!entity_is_hero(entity,name)||!read_world_position(entity,position))return false;
    *result_handle=handle;std::snprintf(hero,capacity,"%s",name);point[0]=position.x;point[1]=position.y;point[2]=position.z;return true;
}
void push(const askermap::observation& e){
    // Repeated scalar source samples cannot evict a short-lived endpoint or
    // modifier event before it reaches Rust. Rust processes kinds in phases.
    auto& out=e.kind==askermap::source?sensors.source_events:sensors.events;
    const std::size_t capacity=e.kind==askermap::source?2048:4096;
    if(out.size()<capacity)out.push_back(e);else ++sensors.lost;
}
void append_sources(){
    for(const auto& e:sensors.source_events)if(sensors.events.size()<4096)sensors.events.push_back(e);else ++sensors.lost;
    sensors.source_events.clear();
}
void bind(std::uint32_t actor,std::uint32_t parent,std::uint64_t game){
    askermap::observation e{};e.kind=askermap::binding;e.actor=actor;e.subject=parent;e.at_ms=game;push(e);
}
void discover(std::uintptr_t system,std::uint64_t now,std::uint64_t game_ms) {
    if(sensors.discovered&&now>=sensors.discovered&&now-sensors.discovered<1000)return;
    auto previous=std::move(sensors.bodies);
    std::vector<body> others;
    sensors.bodies.clear();
    sensors.trees.clear();
    visit_entities(system,[&](std::uintptr_t entity,int){
        body b{};b.address=entity;
        if(!read_designer_name(entity,b.name)||!handle_of(system,entity,b.handle))return false;
        if(std::strcmp(b.name,"ent_dota_tree")==0||std::strcmp(b.name,"ent_dota_temp_tree")==0){
            armlet_vec3 point{};
            if(sensors.trees.size()<4096&&read_world_position(entity,point))sensors.trees.emplace_back(b.handle,point);
            return false;
        }
        // entity_is_hero clears its output on nonheroes. Keep the designer
        // name used to classify neutral and physical-item recipients intact.
        char hero_name[96]{};b.hero=entity_is_hero(entity,hero_name);
        b.neutral=(std::strstr(b.name,"npc_dota_neutral_")||std::strstr(b.name,"npc_dota_creep_neutral"))&&
            std::strcmp(b.name,"npc_dota_neutral_spawner")!=0;
        b.lane=std::strstr(b.name,"npc_dota_creep_goodguys_")||std::strstr(b.name,"npc_dota_creep_badguys_")||std::strcmp(b.name,"npc_dota_creep_lane")==0;
        if(!b.hero&&!b.neutral&&!b.lane&&std::strstr(b.name,"item_drop")==nullptr)return false;
        for(const auto& old:previous)if(old.handle==b.handle&&old.address==b.address){b.modifier_slot=old.modifier_slot;break;}
        // A large neutral population must not evict later heroes from the
        // discovery cap. The two modifier queues also keep separate budgets.
        if(b.hero){if(sensors.bodies.size()<64)sensors.bodies.push_back(b);else ++sensors.lost;}
        else if(others.size()<512)others.push_back(b);else ++sensors.lost;
        return false;
    });
    for(const auto& b:others)if(sensors.bodies.size()<512)sensors.bodies.push_back(b);else ++sensors.lost;
    // A vanished full generation must release its cohort even when its last
    // replicated health was positive in fog. A discovery cap alone is not loss.
    for(const auto& b:previous)if(b.lane&&entity_by_handle(system,b.handle)!=b.address){
        askermap::observation e{};e.kind=askermap::entity;e.actor=b.handle;
        e.flags=askermap::lane_creep;e.at_ms=game_ms;push(e);
    }
    sensors.discovered=now;
}
void sample_sources(std::uintptr_t system,const body& b,std::uint64_t game,std::uint32_t flags=7){
    std::array<std::uint32_t,32> spells{},items{};std::size_t spell_count=0,item_count=0;
    const bool evidence=(flags&5)!=0,teleports=(flags&2)!=0;
    if(evidence)read_enemy_ability_handles(b.address,spells,spell_count);
    bool inventory_known=(evidence||teleports)&&read_hero_inventory_handles(b.address,items,item_count);
    askermap::observation scroll{};scroll.kind=askermap::scroll;scroll.actor=b.handle;scroll.at_ms=game;
    bool travel=false;
    for(std::size_t i=0;i<spell_count+item_count;++i){
        const auto handle=i<spell_count?spells[i]:items[i-spell_count];
        if(!handle||handle==UINT32_MAX)continue;
        const auto item=entity_by_handle(system,handle);
        askermap::observation e{};e.kind=askermap::source;e.actor=handle;e.subject=b.handle;e.at_ms=game;
        if(!plausible_pointer(item)||!read_designer_name(item,e.name)){if(i>=spell_count)inventory_known=false;continue;}
        if(std::strcmp(e.name,"item_travel_boots")==0||std::strcmp(e.name,"item_travel_boots_2")==0)travel=true;
        sensors.members.push_back({handle,b.handle,b.address,i>=spell_count});
        if(std::strncmp(e.name,"special_bonus_",14)==0)continue;
        if(evidence){
            armlet_vec3 caster{};
            if(read_world_position(b.address,caster)){e.flags|=askermap::position;e.position=vector(caster);}
            bool phase=false;
            safe_read(item+schema_fields::m_bInAbilityPhase,phase);if(phase)e.flags|=askermap::active;
            safe_read(item+schema_fields::m_flCastStartTime,e.values[0]);
            safe_read(item+schema_fields::m_flChannelStartTime,e.values[1]);
            safe_read(item+schema_fields::m_flLastCastClickTime,e.values[2]);
            float remaining=0;
            if(schema_fields::m_fCooldown>0&&safe_read(item+schema_fields::m_fCooldown,remaining)&&std::isfinite(remaining)&&remaining>=0&&remaining<=300){e.flags|=askermap::cooldown_known;e.values[3]=remaining;}
            push(e);
        }
        if(teleports&&std::strcmp(e.name,"item_tpscroll")==0){
            scroll.flags=askermap::known|askermap::active;scroll.subject=handle;
            int charges=0;float cooldown=0;
            if(schema_fields::m_iCurrentCharges>0&&safe_read(item+schema_fields::m_iCurrentCharges,charges)&&charges>=0&&charges<=999){scroll.flags|=askermap::charges_known;scroll.values[0]=static_cast<float>(charges);}
            if(schema_fields::m_fCooldown>0&&safe_read(item+schema_fields::m_fCooldown,cooldown)&&std::isfinite(cooldown)&&cooldown>=0&&cooldown<=300){scroll.flags|=askermap::cooldown_known;scroll.values[1]=cooldown;}
            float length=0;
            if(schema_fields::m_flCooldownLength>0&&safe_read(item+schema_fields::m_flCooldownLength,length)&&std::isfinite(length)&&length>0&&length<=300){scroll.flags|=askermap::cooldown_length_known;scroll.values[2]=length;}
            // Submit the item's raw channel facts. Rust owns cast identity,
            // active-window validation, endpoint merging and consumption.
            askermap::observation t{};t.kind=askermap::teleport_item;t.actor=b.handle;t.subject=handle;t.at_ms=game;
            if(schema_fields::m_flChannelStartTime>0&&safe_read(item+schema_fields::m_flChannelStartTime,t.values[0])&&std::isfinite(t.values[0]))t.flags|=askermap::channel_time_known;
            if(schema_fields::m_flCastStartTime>0&&safe_read(item+schema_fields::m_flCastStartTime,t.values[1])&&std::isfinite(t.values[1]))t.flags|=askermap::cast_time_known;
            int fx=0;
            for(const auto offset:{schema_fields::teleport_m_nFxCaster,schema_fields::teleport_m_nFxOrigin,schema_fields::teleport_m_nFxDestination})
                if(offset>0&&safe_read(item+offset,fx)&&fx>0)t.flags|=askermap::active;
            if(schema_fields::teleport_m_flBaseTime>0&&schema_fields::teleport_m_flExtraTime>0&&
                safe_read(item+schema_fields::teleport_m_flBaseTime,t.values[2])&&safe_read(item+schema_fields::teleport_m_flExtraTime,t.values[3])&&
                std::isfinite(t.values[2])&&std::isfinite(t.values[3]))t.flags|=askermap::duration_known;
            armlet_vec3 origin{},destination{};
            if(read_world_position(b.address,origin)){t.flags|=askermap::origin;t.position=vector(origin);}
            bool destination_known=schema_fields::teleport_m_vDestination>0&&safe_read(item+schema_fields::teleport_m_vDestination,destination)&&
                std::isfinite(destination.x)&&std::isfinite(destination.y)&&std::isfinite(destination.z)&&
                (std::abs(destination.x)>1.f||std::abs(destination.y)>1.f||std::abs(destination.z)>1.f);
            if(!destination_known&&schema_fields::teleport_m_hTarget>0){
                std::uint32_t target=UINT32_MAX;
                destination_known=safe_read(item+schema_fields::teleport_m_hTarget,target)&&
                    plausible_pointer(entity_by_handle(system,target))&&read_world_position(entity_by_handle(system,target),destination);
            }
            if(destination_known){t.flags|=askermap::destination;t.auxiliary=vector(destination);}
            push(t);
        }
    }
    // A missing item in a complete inventory is an explicit absence. Failed
    // reads stay unknown and cannot turn into a fabricated ready scroll.
    if(teleports){if(inventory_known)scroll.flags|=askermap::known;if(travel)scroll.flags|=askermap::travel_boots;push(scroll);}
}
void sample_bodies(std::uintptr_t system,std::uint32_t local_team,const askermap::frame& frame){
    float game_seconds=static_cast<float>(frame.game_ms)/1000.f;
    for(const auto& b:sensors.bodies){
        if(entity_by_handle(system,b.handle)!=b.address)continue;
        if(std::strstr(b.name,"item_drop")&&schema_fields::physical_item_m_hItem>0){
            armlet_vec3 p{};std::uint32_t item=UINT32_MAX;askermap::observation d{};
            if(read_world_position(b.address,p)&&safe_read(b.address+schema_fields::physical_item_m_hItem,item)&&read_designer_name(entity_by_handle(system,item),d.name)&&
               (std::strcmp(d.name,"item_madstone_bundle")==0||(std::strncmp(d.name,"item_tier",9)==0&&std::strstr(d.name,"_token")))){
                d.kind=askermap::drop;d.key=b.handle;d.at_ms=frame.game_ms;d.position=vector(p);push(d);
            }
            continue;
        }
        askermap::observation e{};e.kind=askermap::entity;e.actor=b.handle;e.at_ms=frame.game_ms;
        std::memcpy(e.name,b.name,sizeof(e.name));
        std::uint8_t team=0;int hp=0,maxhp=0,player=-1;armlet_vec3 p{};
        if(!safe_read(b.address+schema_fields::m_iTeamNum,team)||!safe_read(b.address+schema_fields::m_iHealth,hp)||
            !safe_read(b.address+schema_fields::m_iMaxHealth,maxhp)||!read_world_position(b.address,p))continue;
        float attack_time=0;if(b.hero && safe_read(b.address+schema_fields::m_flLastAttackTime,attack_time) && std::isfinite(attack_time) && attack_time>0)e.key=static_cast<std::uint64_t>(attack_time*1000.);
        bool neutral_hidden=false;if(b.neutral)neutral_lifecycle(b.address,hp,neutral_hidden);
        e.position=vector(p);e.flags|=askermap::position;if(hp>0)e.flags|=askermap::alive;
        if(b.hero)e.flags|=askermap::hero;if(b.neutral)e.flags|=askermap::neutral;
        if(b.lane)e.flags|=askermap::lane_creep;
        bool hidden=true;
        if(b.neutral&&neutral_hidden)e.flags|=askermap::hidden;
        else if(b.neutral||team==local_team)e.flags|=askermap::visible;
        else if(read_hero_hidden_from_local_team(system,b.address,static_cast<int>(b.handle&0x7fffu),static_cast<std::uint8_t>(local_team),hidden))e.flags|=hidden?askermap::hidden:askermap::visible;
        if(schema_fields::hero_m_iPlayerID>0&&b.hero)safe_read(b.address+schema_fields::hero_m_iPlayerID,player);
        e.values[0]=static_cast<float>(team);e.values[1]=static_cast<float>(player);e.values[2]=static_cast<float>(hp);e.values[3]=static_cast<float>(maxhp);
        if(b.lane&&schema_fields::npc_m_iMoveSpeed>0) {
            int speed=0;if(safe_read(b.address+schema_fields::npc_m_iMoveSpeed,speed)&&speed>=200&&speed<=650)e.values[1]=static_cast<float>(speed);
        }
        if(b.lane&&schema_fields::entity_m_flCreateTime>0){
            float created=0;
            if(safe_read(b.address+schema_fields::entity_m_flCreateTime,created)&&std::isfinite(created)&&created>=0&&created<=game_seconds+.1f){
                e.flags|=askermap::creation_time_known;e.auxiliary.x=created;
            }
        }
        push(e);
        if(b.hero&&team!=local_team&&(team==2||team==3)){
            sample_sources(system,b,frame.game_ms,frame.flags);
            if((frame.flags&2)==0)continue;
            active_teleport_snapshot tp{};bool known=false;
            const bool active=read_active_teleport_snapshot(b.address,game_seconds,tp,&known);
            askermap::observation t{};t.kind=askermap::teleport;t.actor=b.handle;t.at_ms=frame.game_ms;
            t.key=tp.cast_key;t.flags=known?askermap::known:0;if(active)t.flags|=askermap::active;
            if(tp.twin_gate)t.flags|=askermap::twin_gate;
            if(e.flags&askermap::hidden)t.flags|=askermap::hidden;
            if(tp.has_origin)t.flags|=askermap::origin;if(tp.has_destination)t.flags|=askermap::destination;
            t.position=vector(tp.origin);t.auxiliary=vector(tp.destination);t.values[0]=static_cast<float>(tp.duration_ms);t.values[1]=static_cast<float>(tp.elapsed_ms);push(t);
        }
    }
}
void sample_modifiers(std::uintptr_t system,const askermap::frame& frame){
    if(sensors.bodies.empty()||(frame.flags&1)==0)return;
    const auto client=g_client_base_address.load(std::memory_order_acquire);const auto size=image_size(client);
    if(!client||!size||schema_fields::m_ModifierManager<=0)return;
    // The vector resolver validates up to 256 entries; the old 128-element
    // collection rejected larger live vectors in their entirety. Resume at a
    // per-generation slot and rotate recipients after a 32-slot slice.
    const auto sample_queue=[&](bool heroes,std::size_t& cursor,std::size_t budget){
        std::size_t visited=0;
        while(budget&&visited<sensors.bodies.size()){
            auto& b=sensors.bodies[cursor++%sensors.bodies.size()];++visited;
            if(heroes?!b.hero:!b.neutral)continue;
            if(entity_by_handle(system,b.handle)!=b.address){b.modifier_slot=0;continue;}
            armlet_vec3 p{};if(!read_world_position(b.address,p))continue;
            std::uintptr_t data=0;int count=0;
            if(!resolve_modifier_vector(b.address+schema_fields::m_ModifierManager,client,size,count,data)||count<0||count>256)continue;
            if(!count){b.modifier_slot=0;continue;}
            if(b.modifier_slot>=static_cast<std::size_t>(count))b.modifier_slot=0;
            const auto slice=std::min<std::size_t>({budget,32,static_cast<std::size_t>(count)-b.modifier_slot});
            const auto end=b.modifier_slot+slice;
            const auto start_events=sensors.events.size();
            for(;b.modifier_slot<end;++b.modifier_slot,--budget){
                std::uintptr_t modifier=0;char type[192]{};std::uint32_t caster=UINT32_MAX;
                if(!safe_read(data+sizeof(void*)*b.modifier_slot,modifier)||!plausible_pointer(modifier)||
                   !read_rtti_name(modifier,client,size,type)||!safe_read(modifier+private_modifier_layout::m_hCaster,caster)||
                   !plausible_pointer(entity_by_handle(system,caster)))continue;
                askermap::observation e{};e.kind=askermap::modifier;e.actor=caster;e.subject=b.handle;e.key=modifier;e.at_ms=frame.game_ms;
                e.position=vector(p);std::snprintf(e.name,sizeof(e.name),"%s",type);
                if(!safe_read(modifier+private_modifier_layout::m_flCreationTime,e.values[0])||!safe_read(modifier+private_modifier_layout::m_flPreviousTick,e.values[1]))continue;
                push(e);
            }
            std::uintptr_t final_data=0;int final_count=0;
            if(entity_by_handle(system,b.handle)!=b.address||
               !resolve_modifier_vector(b.address+schema_fields::m_ModifierManager,client,size,final_count,final_data)||
               final_data!=data||final_count!=count){sensors.lost+=static_cast<std::uint32_t>(sensors.events.size()-start_events);sensors.events.resize(start_events);b.modifier_slot=0;continue;}
            if(b.modifier_slot>=static_cast<std::size_t>(count))b.modifier_slot=0;
        }
    };
    // Neutrals cannot consume the hero/friendly budget. Each modifier record
    // costs at most RTTI + caster + two timing reads, 256 records per poll.
    sample_queue(true,sensors.hero_modifier_cursor,192);
    sample_queue(false,sensors.neutral_modifier_cursor,64);
}
void sample_particles(std::uintptr_t system,const askermap::frame& frame){
    std::array<render_teleportation::particle_observation,512> observations{};
    const auto previous_cursor=sensors.cursor;
    const auto count=render_teleportation::read_observations(sensors.cursor,observations.data(),observations.size());
    // The ring advances across overwritten/incoherent slots as well. Include
    // those gaps in the same numeric dropped-observation counter as caps.
    if(sensors.cursor>=previous_cursor&&sensors.cursor-previous_cursor>count){
        const auto skipped=sensors.cursor-previous_cursor-count;
        sensors.lost+=static_cast<std::uint32_t>(std::min<std::uint64_t>(skipped,UINT32_MAX-sensors.lost));
    }
    g_render_diagnostics.particle_observations.fetch_add(count,std::memory_order_relaxed);
    for(std::size_t i=0;i<count;++i){
        const auto& raw=observations[i];if(raw.observed_at_ms>frame.wall_ms||frame.wall_ms-raw.observed_at_ms>1500)continue;
        askermap::observation e{};e.kind=askermap::particle;e.actor=raw.owner_handle;
        if(raw.owner_entity){
            if(!handle_of(system,raw.owner_entity,e.actor))continue;
            if(raw.owner_handle!=0&&raw.owner_handle!=UINT32_MAX&&raw.owner_handle!=e.actor)continue;
        }
        auto owner=entity_by_handle(system,e.actor);auto owner_handle=e.actor;
        for(unsigned depth=0;depth<4&&plausible_pointer(owner)&&schema_fields::m_hOwnerEntity>0;++depth){
            char hero_name[96]{};if(entity_is_hero(owner,hero_name))break;
            std::uint32_t parent=UINT32_MAX;if(!safe_read(owner+schema_fields::m_hOwnerEntity,parent)||parent==0||parent==UINT32_MAX||parent==owner_handle)break;
            const auto next=entity_by_handle(system,parent);if(!plausible_pointer(next))break;
            bind(owner_handle,parent,frame.game_ms);owner=next;owner_handle=parent;
        }
        e.key=static_cast<std::uint64_t>(raw.effect)^static_cast<std::uint64_t>(raw.collection<<1);
        if(!e.key)e.key=raw.sequence;
        e.at_ms=frame.game_ms-std::min(frame.game_ms,frame.wall_ms-raw.observed_at_ms);
        e.position={raw.x,raw.y,raw.z};e.auxiliary={raw.source_x,raw.source_y,raw.source_z};e.flags=askermap::position;
        if(raw.flags&render_teleportation::particle_flag_control_point_position)e.flags|=askermap::control_point;
        if(raw.flags&render_teleportation::particle_flag_new_effect)e.flags|=askermap::new_effect;
        if(raw.flags&render_teleportation::particle_flag_visual_correction)e.flags|=askermap::visual_correction;
        std::memcpy(e.resource,raw.debug_name,sizeof(e.resource));e.resource[sizeof(e.resource)-1]=0;
        if(frame.flags&5)push(e);
        if((frame.flags&2)==0||(raw.flags&render_teleportation::particle_flag_teleport)==0)continue;
        if(!plausible_pointer(entity_by_handle(system,e.actor)))continue;
        const auto endpoints=raw.flags&(render_teleportation::particle_flag_teleport_start|render_teleportation::particle_flag_teleport_destination);
        if(!endpoints)continue;
        // A player controller is a delivery recipient, not necessarily the
        // caster (Full Map Demo delivers enemy TP ends to local player 0).
        // teleport_end carries CP0=destination and SortOrigin=cast origin.
        // Submit this pair for unique temporal/spatial route correlation in
        // Rust. This rule is scoped to this resource, never generic impacts.
        char carrier_name[96]{};
        const bool recipient=read_designer_name(owner,carrier_name)&&std::strcmp(carrier_name,"dota_player_controller")==0;
        if(recipient&&std::strcmp(raw.debug_name,"particles/items2_fx/teleport_end.vpcf")==0&&
            (raw.flags&render_teleportation::particle_flag_control_point_position)&&
            (std::abs(raw.source_x)>1.f||std::abs(raw.source_y)>1.f)){
            askermap::observation endpoint{};endpoint.kind=askermap::teleport;endpoint.actor=UINT32_MAX;
            endpoint.key=e.key;endpoint.at_ms=e.at_ms;
            endpoint.flags=askermap::known|askermap::active|askermap::particle_teleport|askermap::destination|askermap::endpoint_anchor;
            if(raw.flags&render_teleportation::particle_flag_new_effect)endpoint.flags|=askermap::new_effect;
            if(raw.flags&render_teleportation::particle_flag_teleport_endcap)endpoint.flags|=askermap::endcap;
            endpoint.position=e.auxiliary;endpoint.auxiliary=e.position;endpoint.values[0]=3000.f;push(endpoint);
            continue;
        }
        char hero_name[96]{};
        if(!entity_is_hero(owner,hero_name)){
            std::uint32_t candidate=UINT32_MAX;std::uintptr_t candidate_address=0;bool ambiguous=false;
            for(const auto& member:sensors.members)if(member.source==e.actor&&entity_by_handle(system,member.hero)==member.address){
                std::array<std::uint32_t,32> live{};std::size_t live_count=0;
                if(!(member.item?read_hero_inventory_handles(member.address,live,live_count):read_enemy_ability_handles(member.address,live,live_count))||
                   std::find(live.begin(),live.begin()+live_count,e.actor)==live.begin()+live_count)continue;
                if(candidate!=UINT32_MAX&&candidate!=member.hero){ambiguous=true;break;}
                candidate=member.hero;candidate_address=member.address;
            }
            if(ambiguous||candidate==UINT32_MAX||!entity_is_hero(candidate_address,hero_name))continue;
            owner=candidate_address;owner_handle=candidate;
        }
        std::uint8_t team=0;
        if(!safe_read(owner+schema_fields::m_iTeamNum,team)||team==frame.local_team||(team!=2&&team!=3))continue;
        askermap::observation tp{};tp.kind=askermap::teleport;tp.actor=owner_handle;tp.key=e.key;tp.at_ms=e.at_ms;
        tp.flags=askermap::known|askermap::active|askermap::particle_teleport;
        if(raw.flags&render_teleportation::particle_flag_new_effect)tp.flags|=askermap::new_effect;
        if(raw.flags&render_teleportation::particle_flag_teleport_endcap)tp.flags|=askermap::endcap;
        if(raw.flags&render_teleportation::particle_flag_twin_gate)tp.flags|=askermap::twin_gate;
        if(raw.flags&render_teleportation::particle_flag_keen_conveyance)tp.flags|=askermap::keen_conveyance;
        // Only the endpoint explicitly named by the particle is evidence.
        // SortOrigin (source_x/y/z) is never the opposite end of a teleport.
        if(endpoints&render_teleportation::particle_flag_teleport_start){tp.flags|=askermap::origin;tp.position=e.position;}
        if(endpoints&render_teleportation::particle_flag_teleport_destination){tp.flags|=askermap::destination;tp.auxiliary=e.position;}
        tp.values[0]=3000.f;push(tp);
    }
}
// Read the engine's launch facts independently of Armlet/damage automation.
// No damage inference or target filtering belongs in this small sensor.
void sample_projectiles(std::uintptr_t system,const askermap::frame& frame){
    if((frame.flags&1)==0)return;
    const auto client=g_client_base_address.load(std::memory_order_acquire);
    const auto size=image_size(client);
    static std::uint64_t retry_at=0;
    if(!g_projectile_manager_global.load(std::memory_order_acquire)&&frame.wall_ms>=retry_at){
        retry_at=frame.wall_ms+1000;initialize_internal_projectile_feed(client,size);
    }
    const auto global=g_projectile_manager_global.load(std::memory_order_acquire);
    std::uintptr_t manager=0,table=0;
    std::array<std::uintptr_t,512> slots{};
    if(!plausible_pointer(global)||!safe_read(global,manager)||!plausible_pointer(manager)||
       !safe_read(manager,table)||table<client||table>=client+size||!safe_read(manager+0x2038,slots))return;
    for(std::size_t i=0;i<slots.size();++i){
        const auto p=slots[i];if(!plausible_pointer(p))continue;
        std::uint32_t id=0,check=0,owner=UINT32_MAX;std::uintptr_t current=0;
        armlet_vec3 origin{},position{},velocity{};
        if(!safe_read(p+0x40,id)||!id||!safe_read(p+0x44,origin)||!safe_read(p+0x50,position)||
           !safe_read(p+0x68,velocity)||!safe_read(p+0x90,owner)||!safe_read(p+0x40,check)||id!=check||
           !safe_read(manager+0x2038+i*sizeof(void*),current)||current!=p||
           !plausible_pointer(entity_by_handle(system,owner)))continue;
        const float speed=std::hypot(velocity.x,velocity.y),distance=std::hypot(position.x-origin.x,position.y-origin.y);
        if(!std::isfinite(speed)||speed<100||speed>10000||!std::isfinite(distance)||distance>speed*.75f)continue;
        askermap::observation e{};e.kind=askermap::projectile;e.actor=owner;e.key=(std::uint64_t(i)<<32)|id;
        e.at_ms=frame.game_ms;e.position=vector(origin);e.auxiliary=vector(position);e.values[0]=distance/speed*1000.f;push(e);
    }
}
void sample_tree_obstacles(const askermap::frame& frame){
    if((frame.flags&1)==0)return;
    const auto count=sensors.events.size();unsigned casts=0,submitted=0;
    for(std::size_t i=0;i<count&&casts<16&&submitted<256;++i){
        const auto fact=sensors.events[i];
        if(fact.kind!=askermap::particle||(fact.flags&askermap::new_effect)==0)continue;
        ++casts;
        for(const auto& [handle,point]:sensors.trees){
            const float dx=point.x-fact.position.x,dy=point.y-fact.position.y;
            if(dx*dx+dy*dy>900.f*900.f)continue;
            askermap::observation e{};e.kind=askermap::obstacle;e.actor=handle;e.at_ms=frame.game_ms;e.position=vector(point);push(e);
            if(++submitted>=256)break;
        }
    }
}
void sample_camps(const askermap::frame& frame){
    if((frame.flags&5)==0||schema_fields::game_rules_m_NeutralSpawnBoxes<=0)return;
    std::uintptr_t rules=0,data=0;int count=0;
    if(!safe_read(g_game_rules_global.load(std::memory_order_acquire),rules)||!plausible_pointer(rules))return;
    const auto vector=rules+schema_fields::game_rules_m_NeutralSpawnBoxes;
    if(!safe_read(vector,count)||count<0||count>64||!safe_read(vector+8,data)||!plausible_pointer(data))return;
    std::array<std::uint8_t,neutral_spawn_boxes::stride*64> bytes{};
    if(!safe_copy_memory(data,bytes.data(),count*neutral_spawn_boxes::stride))return;
    for(int i=0;i<count;++i){
        neutral_spawn_boxes::box box{};
        if(!neutral_spawn_boxes::decode({bytes.data()+i*neutral_spawn_boxes::stride,neutral_spawn_boxes::stride},box))continue;
        askermap::observation e{};e.kind=askermap::camp;e.actor=box.spawner;e.at_ms=frame.game_ms;
        e.position={box.minimum[0],box.minimum[1],box.minimum[2]};e.auxiliary={box.maximum[0],box.maximum[1],box.maximum[2]};e.values[0]=static_cast<float>(box.type);push(e);
    }
}
ULONGLONG wall_deadline(std::uint64_t event,const askermap::frame& f){return f.wall_ms+(event>f.game_ms?event-f.game_ms:0);}
void publish(const askermap::snapshot& snapshot,const askermap::frame& frame,std::uintptr_t system){
    std::array<render_teleportation::particle_position_correction,32> corrections{};
    const auto correction_count=std::min<std::size_t>(snapshot.correction_count,corrections.size());
    for(std::size_t i=0;i<correction_count;++i){const auto& source=snapshot.corrections[i];auto& d=corrections[i];
        d.key=source.key;d.expires_at=wall_deadline(source.expires_ms,frame);
        d.position[0]=source.position.x;d.position[1]=source.position.y;d.position[2]=source.position.z;
        d.reference[0]=source.reference.x;d.reference[1]=source.reference.y;d.reference[2]=source.reference.z;}
    render_teleportation::publish_position_corrections(corrections.data(),correction_count);
    render_intelligence_exclusive_guard guard;
    auto& dst=g_render_intelligence;dst.trace_count=0;dst.teleport_count=0;dst.camp_count=0;dst.enemy_visibility_count=0;dst.enemy_teleport_count=0;dst.pulse_count=0;dst.marks={};
    for(std::size_t i=0;i<snapshot.correction_count&&dst.pulse_count<dst.pulses.size();++i){
        const auto& c=snapshot.corrections[i];if(c.created_ms+1200<=frame.game_ms)continue;
        bool duplicate=false;
        for(std::size_t j=0;j<dst.pulse_count;++j)if(std::hypot(dst.pulses[j].position.x-c.position.x,dst.pulses[j].position.y-c.position.y)<64.)duplicate=true;
        if(duplicate)continue;
        auto& pulse=dst.pulses[dst.pulse_count++];pulse.position={c.position.x,c.position.y,c.position.z};
        pulse.started_at=frame.wall_ms-std::min(frame.wall_ms,frame.game_ms-std::min(frame.game_ms,c.created_ms));
        pulse.expires_at=wall_deadline(c.created_ms+1200,frame);
    }
    for(std::size_t i=0;i<snapshot.marker_count;++i){
        const auto& m=snapshot.markers[i];
        // Rust keeps the cohort alive while visible, but neither world nor
        // minimap needs a duplicate overlay over already visible lane creeps.
        if(m.kind==6&&m.evidence==askermap::wave_visible)continue;
        if(m.kind==4){dst.marks.observe(m.key,effect_marks::kind::drop,{m.position.x,m.position.y,m.position.z},m.icon,"Neutral drop",frame.wall_ms);continue;}
        if(dst.trace_count>=dst.traces.size())break;
        auto& t=dst.traces[dst.trace_count++];t={};t.owner_handle=m.owner;t.position={m.position.x,m.position.y,m.position.z};
        t.estimated_position=m.kind==5;t.creep_wave=m.kind==6;
        if(m.kind!=3)copy_designer_name(t.designer_name,m.hero);copy_designer_name(t.ability_icon,m.icon);
        copy_detector_trigger(t.detector_trigger,m.kind==1?"Last seen":m.kind==3?"Remote effect":m.kind==5?"Estimated farm":m.kind==6?"Predicted wave":"Cast evidence");
        t.created_at=frame.wall_ms-std::min(frame.wall_ms,frame.game_ms-std::min(frame.game_ms,m.created_ms));t.event_started_at=t.created_at;t.expires_at=wall_deadline(m.expires_ms,frame);t.source_event_key=m.key;t.source_priority=static_cast<std::uint8_t>(m.evidence);t.particle_position=m.kind==3;
    }
    for(std::size_t i=0;i<snapshot.route_count&&dst.teleport_count<dst.teleports.size();++i){
        const auto& r=snapshot.routes[i];auto& t=dst.teleports[dst.teleport_count++];t={};
        t.owner_handle=r.owner;t.cast_key=r.key;t.origin={r.origin.x,r.origin.y,r.origin.z};t.destination={r.destination.x,r.destination.y,r.destination.z};
        t.effect_flags=render_teleportation::particle_flag_teleport;
        if(r.flags&askermap::twin_gate)t.effect_flags|=render_teleportation::particle_flag_twin_gate;
        if(r.flags&askermap::keen_conveyance)t.effect_flags|=render_teleportation::particle_flag_keen_conveyance;
        t.has_origin=(r.flags&1)!=0;t.has_destination=(r.flags&2)!=0;t.canceled=(r.flags&4)!=0;t.channel_finished=(r.flags&8)!=0;t.channel_observed=true;t.enemy_confirmed=true;
        t.duration_ms=static_cast<std::uint32_t>(r.ends_ms-r.started_ms);t.cast_started_at=frame.wall_ms-std::min(frame.wall_ms,frame.game_ms-std::min(frame.game_ms,r.started_ms));t.first_seen_at=t.cast_started_at;t.expires_at=wall_deadline(r.expires_ms,frame);copy_designer_name(t.designer_name,r.hero);
    }
    for(std::size_t i=0;i<snapshot.camp_count&&dst.camp_count<dst.camps.size();++i){
        const auto& c=snapshot.camps[i];auto& t=dst.camps[dst.camp_count++];t={};t.minimum={c.minimum.x,c.minimum.y,c.minimum.z};t.maximum={c.maximum.x,c.maximum.y,c.maximum.z};
        t.position={(c.minimum.x+c.maximum.x)*.5f,(c.minimum.y+c.maximum.y)*.5f,c.minimum.z};t.type=static_cast<int>(c.kind);t.blocked=c.state==2;t.estimated=c.state==0;t.has_native_bounds=true;
    }
    for(std::size_t i=0;i<snapshot.enemy_count&&dst.enemy_visibility_count<dst.enemy_visibility.size();++i){
        const auto& e=snapshot.enemies[i];auto& t=dst.enemy_visibility[dst.enemy_visibility_count++];t={};t.entity_index=e.handle&0x7fffu;t.entity_generation=e.handle;t.player_id=e.player;t.team=static_cast<std::uint8_t>(e.team);t.dormant=(e.flags&askermap::hidden)!=0;t.initialized=true;t.last_observed_at=frame.wall_ms;t.last_health=e.health;
        copy_designer_name(t.designer_name,e.hero);
        auto& s=dst.enemy_teleport_count<dst.enemy_teleports.size()?dst.enemy_teleports[dst.enemy_teleport_count++]:dst.enemy_teleports.back();s={};s.player_id=e.player;s.team=static_cast<std::uint8_t>(e.team);s.health=e.health;s.max_health=e.max_health;s.visible=(e.flags&askermap::visible)!=0;hero_abbreviation(e.hero,s.hero_abbreviation);
        s.charges=-1;s.cooldown_seconds=-1.f;
        for(std::size_t j=0;j<snapshot.scroll_count;++j)if(snapshot.scrolls[j].owner==e.handle){const auto& raw=snapshot.scrolls[j];s.inventory_known=(raw.flags&askermap::known)!=0;s.has_scroll=(raw.flags&askermap::active)!=0;s.charges=(raw.flags&askermap::charges_known)||!s.has_scroll?raw.charges:-1;s.cooldown_seconds=(raw.flags&askermap::cooldown_known)||!s.has_scroll?static_cast<float>(raw.cooldown_ms>frame.game_ms?raw.cooldown_ms-frame.game_ms:0)/1000.f:-1.f;s.cooldown_estimated=(raw.flags&askermap::estimated)!=0&&s.cooldown_seconds>0;s.cooldown_observed_at=frame.wall_ms;}
    }
    if((frame.flags&4)&&(!sensors.jungle_sampled||frame.wall_ms>=sensors.jungle_sampled+250)){
        sensors.jungle_sampled=frame.wall_ms;
        std::array<jungle_status::unit_snapshot,192> units{};std::size_t count=0;
        for(const auto& body:sensors.bodies){
            if(!body.neutral||count>=units.size()||entity_by_handle(system,body.handle)!=body.address)continue;
            armlet_vec3 position{};int health=0;
            if(!read_world_position(body.address,position)||!safe_read(body.address+schema_fields::m_iHealth,health))continue;
            auto& unit=units[count++];unit.position={position.x,position.y,position.z};unit.health=health;unit.index=body.handle;
            bool hidden=true,pooled=false;neutral_lifecycle(body.address,unit.health,pooled);
            unit.visible=!pooled&&read_hero_hidden_from_local_team(system,body.address,static_cast<int>(body.handle&0x7fffu),
                static_cast<std::uint8_t>(frame.local_team),hidden)&&!hidden;
            if(schema_fields::npc_gold_min>0)safe_read(body.address+schema_fields::npc_gold_min,unit.gold_min);
            if(schema_fields::npc_gold_max>0)safe_read(body.address+schema_fields::npc_gold_max,unit.gold_max);
            if(unit.gold_min<0||unit.gold_max<unit.gold_min||unit.gold_max>10000)unit.gold_min=unit.gold_max=-1;
        }
        // Presentation history of raw neutral samples; Rust still owns event attribution.
        commit_jungle_sample({units.data(),count},frame.session,float(frame.game_ms)*.001f,frame.wall_ms);
    }
    refresh_minimap_bounds(system,frame.wall_ms); // geometry-only infrastructure
    publish_render_intelligence_counts();
}
void poll(std::uintptr_t system){
    static const bool abi_valid=askermap::askermap_layout(0)==askermap::abi&&askermap::askermap_layout(1)==sizeof(askermap::frame)&&askermap::askermap_layout(2)==sizeof(askermap::observation)&&askermap::askermap_layout(3)==sizeof(askermap::snapshot);
    if(!abi_valid)return;
    const auto now=GetTickCount64();float game=0,tick_interval=0;std::uint8_t team=0;
    const auto hero=g_monitor.hero_address.load(std::memory_order_relaxed);
    if(!read_game_time(g_client_base_address.load(std::memory_order_acquire),game,tick_interval)||!std::isfinite(game)||game<0||!safe_read(hero+schema_fields::m_iTeamNum,team)||(team!=2&&team!=3))return;
    const auto session=static_cast<std::uint64_t>(system);
    auto game_ms=static_cast<std::uint64_t>(game*1000.f);
    // CGlobalVars::curtime alternates between interpolated render time and
    // packet/prediction tick time. A backward step within one measured engine
    // tick is the same simulation instant, not a match rewind. Normalize only
    // this phase uncertainty; larger rewinds retain the existing reset path.
    if(sensors.session==session&&game_ms<sensors.last_game&&
        std::isfinite(tick_interval)&&tick_interval>0&&
        sensors.last_game-game_ms<=static_cast<std::uint64_t>(std::ceil(tick_interval*1000.f)))
        game_ms=sensors.last_game;
    if(sensors.session!=session||game_ms<sensors.last_game){sensors={};sensors.session=session;}
    sensors.last_game=game_ms;
    askermap::frame frame{sizeof(askermap::frame),askermap::abi,session,now,game_ms,0,team,static_cast<std::uint32_t>(g_render_trace_lifetime_seconds.load(std::memory_order_relaxed)*1000),0};
    if(g_render_trace_icons.load(std::memory_order_relaxed))frame.flags|=1;if(g_render_teleport_arrows.load(std::memory_order_relaxed))frame.flags|=2;if(g_render_camp_status.load(std::memory_order_relaxed))frame.flags|=4;
    if(g_render_creep_tracker.load(std::memory_order_relaxed))frame.flags|=8;
    render_teleportation::set_owner_validator(&particle_owner_is_live);render_teleportation::set_trace_capture_enabled((frame.flags&7)!=0);
    render_teleportation::set_attachment_reader(&read_attachment_owner);
    sensors.events.clear();sensors.source_events.clear();sensors.members.clear();sensors.lost=0;
    if(frame.flags){
        // Always publish an explicit clock admission result. Missing metadata,
        // pre-game (4), or post-game cannot leave old predicted waves alive.
        std::uintptr_t rules=0;float next=0,start=0;bool spawning=false;int phase=0;
        const bool clock_known=schema_fields::game_rules_m_flGameStartTime>0&&
            schema_fields::game_rules_m_bCreepSpawningEnabled>0&&schema_fields::game_rules_m_nGameState>0&&
            safe_read(g_game_rules_global.load(std::memory_order_acquire),rules)&&plausible_pointer(rules)&&
            safe_read(rules+schema_fields::game_rules_m_flGameStartTime,start)&&std::isfinite(start)&&
            safe_read(rules+schema_fields::game_rules_m_bCreepSpawningEnabled,spawning)&&
            safe_read(rules+schema_fields::game_rules_m_nGameState,phase);
        bool local_spawning=true;std::uint32_t spawn_revision=0;
        const bool local_schedule=read_local_creep_spawning(local_spawning,spawn_revision);
        if(local_schedule&&!local_spawning)spawning=false;
        // Optional raw epoch is diagnostic only. :00/:30 comes from match zero.
        if(clock_known&&schema_fields::game_rules_m_flCreepSpawntime>0&&
            (!safe_read(rules+schema_fields::game_rules_m_flCreepSpawntime,next)||!std::isfinite(next)))next=0;
        askermap::observation wave_clock{};wave_clock.kind=askermap::wave_clock;wave_clock.at_ms=game_ms;
        // Engine change count also detects off/on pairs between two polls.
        // Zero means no local revision source; revision zero itself is valid.
        wave_clock.key=local_schedule?std::uint64_t(spawn_revision)+1:0;
        wave_clock.flags=(clock_known?askermap::known:0)|(spawning?askermap::active:0);
        wave_clock.values[0]=clock_known?next:0;wave_clock.values[1]=clock_known?start:0;
        wave_clock.values[2]=static_cast<float>(phase);push(wave_clock);
        discover(system,now,game_ms);sample_bodies(system,team,frame);sample_projectiles(system,frame);sample_modifiers(system,frame);sample_particles(system,frame);sample_camps(frame);sample_tree_obstacles(frame);
    }
    append_sources();
    frame.reserved=sensors.lost;
    askermap::snapshot snapshot{};
    const auto status=askermap::askermap_process(&frame,sensors.events.data(),sensors.events.size(),&snapshot);
    if(status==0)publish(snapshot,frame,system);
    else {render_intelligence_exclusive_guard guard;g_render_intelligence={};publish_render_intelligence_counts();log_message("RUST","Intelligence frame rejected; presentation cleared.");}
}
}
