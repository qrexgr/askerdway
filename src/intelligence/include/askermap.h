#pragma once
#include <cstddef>
#include <cstdint>
namespace askermap {
constexpr std::uint32_t abi=4;
// wave_clock.key: local spawn change revision + 1, zero if unavailable.
enum kind: std::uint32_t {entity=1,binding=2,source=3,particle=4,modifier=5,teleport=6,camp=7,drop=8,scroll=9,teleport_item=10,projectile=11,obstacle=12,wave_clock=13};
enum flag: std::uint32_t {visible=1,hidden=2,alive=4,position=8,active=16,known=32,origin=64,destination=128,new_effect=256,neutral=512,hero=1024,control_point=2048,
    charges_known=4096,cooldown_known=8192,particle_teleport=16384,twin_gate=32768,keen_conveyance=65536,
    channel_time_known=1<<17,cast_time_known=1<<18,duration_known=1<<19,cooldown_length_known=1<<20,
    item_teleport=1<<21,estimated=1<<22,travel_boots=1<<23,endpoint_anchor=1<<24,endcap=1<<25,lane_creep=1<<26,visual_correction=1<<27,creation_time_known=1<<28};
struct vec3 {float x,y,z;};
struct frame {std::uint32_t size,version;std::uint64_t session,wall_ms,game_ms;std::uint32_t flags,local_team,trace_ttl_ms,reserved;};
struct observation {std::uint32_t kind,flags,actor,subject;std::uint64_t key,at_ms;vec3 position,auxiliary;float values[4];char name[96],resource[256];};
inline constexpr std::uint32_t wave_visible = 11;
struct marker {std::uint32_t owner,kind;std::uint64_t key;vec3 position;std::uint32_t evidence;std::uint64_t created_ms,expires_ms;char hero[96],icon[96];};
struct route {std::uint32_t owner,flags;std::uint64_t key;vec3 origin,destination;std::uint64_t started_ms,ends_ms,expires_ms;char hero[96];};
struct camp_state {std::uint32_t id,kind;vec3 minimum,maximum;std::uint32_t state,neutrals;};
struct enemy {std::uint32_t handle;std::int32_t player;std::uint32_t team,flags;std::int32_t health,max_health;char hero[96];};
struct scroll_state {std::uint32_t owner,flags;std::int32_t charges;std::uint32_t reserved;std::uint64_t cooldown_ms;};
struct position_correction {std::uint64_t key;vec3 position,reference;std::uint64_t created_ms,expires_ms;};
struct attachment_query {std::uint32_t size,version,actor;vec3 model,attachment,origin;char hero[96],resource[256];};
struct snapshot {
    std::uint32_t size,version;std::uint64_t session,game_ms;
    std::uint32_t marker_count,route_count,camp_count,enemy_count,scroll_count,rejected,dropped,correction_count;
    marker markers[64];route routes[16];camp_state camps[64];enemy enemies[32];scroll_state scrolls[32];position_correction corrections[32];
};
extern "C" std::uint32_t askermap_layout(std::uint32_t which);
extern "C" std::uint32_t askermap_resolve_attachment(const attachment_query*);
// Host owns pointer validity, alignment, lifetime and output storage. Calls
// are serialized by Rust; every frame uses a bounded numeric-only ABI.
extern "C" std::uint32_t askermap_process(const frame*,const observation*,std::size_t,snapshot*);
}
