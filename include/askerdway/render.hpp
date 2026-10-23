/**
 * @file
 * @brief Контракт particle-hook.
 *
 * Флаги отделяют телепорт, источник и назначение. Наблюдения копируются из ограниченного кольца
 * по курсору; читатель должен сохранять обновлённый cursor. Указатели движка не становятся
 * стабильными объектами из-за чтения снимка.
 */

#pragma once

#include <cstddef>
#include <cstdint>

// Particle classification bits.
enum askerdway_particle_flag : std::uint32_t {
    ASKERDWAY_PARTICLE_NONE = 0,
    ASKERDWAY_PARTICLE_TELEPORT = 1u << 0,
    ASKERDWAY_PARTICLE_TELEPORT_START = 1u << 1,
    ASKERDWAY_PARTICLE_TELEPORT_DESTINATION = 1u << 2,
    ASKERDWAY_PARTICLE_TWIN_GATE = 1u << 3,
    ASKERDWAY_PARTICLE_KEEN_CONVEYANCE = 1u << 4,
    ASKERDWAY_PARTICLE_NEW_EFFECT = 1u << 5,
    ASKERDWAY_PARTICLE_CONTROL_POINT_POSITION = 1u << 6,
    ASKERDWAY_PARTICLE_TELEPORT_ENDCAP = 1u << 7,
    ASKERDWAY_PARTICLE_VISUAL_CORRECTION = 1u << 8,
};

// Internal hook bridge. External code should use AskerdwayApi_*.
namespace render_teleportation {

struct particle_observation {
    std::uint64_t sequence;
    std::uint32_t owner_handle;
    std::uint32_t flags;
    float x;
    float y;
    float z;
    std::uint64_t observed_at_ms;
    char debug_name[256];
    // Private bridge: +owner is a pointer, never the low DWORD of a handle.
    std::uintptr_t owner_entity;
    std::uintptr_t effect, collection;
    std::uint32_t visibility;
    float source_x,source_y,source_z;
};

enum particle_observation_flags : std::uint32_t {
    particle_flag_none = ASKERDWAY_PARTICLE_NONE,
    particle_flag_teleport = ASKERDWAY_PARTICLE_TELEPORT,
    particle_flag_teleport_start = ASKERDWAY_PARTICLE_TELEPORT_START,
    particle_flag_teleport_destination = ASKERDWAY_PARTICLE_TELEPORT_DESTINATION,
    particle_flag_twin_gate = ASKERDWAY_PARTICLE_TWIN_GATE,
    particle_flag_keen_conveyance = ASKERDWAY_PARTICLE_KEEN_CONVEYANCE,
    particle_flag_new_effect = ASKERDWAY_PARTICLE_NEW_EFFECT,
    particle_flag_control_point_position = ASKERDWAY_PARTICLE_CONTROL_POINT_POSITION,
    particle_flag_teleport_endcap = ASKERDWAY_PARTICLE_TELEPORT_ENDCAP,
    particle_flag_visual_correction = ASKERDWAY_PARTICLE_VISUAL_CORRECTION,
};

bool install() noexcept;
bool shutdown() noexcept;
bool set_enabled(bool enabled) noexcept;
void set_gameplay_context(bool active) noexcept;
using particle_owner_validator = bool (*)(std::uintptr_t) noexcept;
void set_owner_validator(particle_owner_validator validator) noexcept;
bool set_trace_capture_enabled(bool enabled) noexcept;
void clear_observations() noexcept;
bool is_enabled() noexcept;
bool is_hook_installed() noexcept;
std::uint64_t updated_effect_count() noexcept;
// Cumulative Update-hook diagnostics. Values are published in small batches
// so observing the hook does not add an atomic RMW to every particle frame.
std::uint64_t hook_call_count() noexcept;
std::uint64_t named_effect_count() noexcept;
std::uint64_t trackable_effect_count() noexcept;
std::uint64_t fow_hidden_effect_count() noexcept;
/**
 * @brief Копирует только согласованные записи кольца после cursor. Отставший читатель может
 * потерять перезаписанные записи; обновлённый cursor нужно сохранить.
 */
std::size_t read_observations(std::uint64_t& cursor,
    particle_observation* output, std::size_t capacity) noexcept;
std::size_t read_recent_observations(particle_observation* output,
    std::size_t capacity) noexcept;
const char* build_id() noexcept;
// Rust-authorized fallback for a frozen self-cast attachment. The native
// collection is suppressed; presentation draws a bounded pulse at the cast.
struct particle_position_correction {
    std::uint64_t key=0,expires_at=0;
    float position[3]{},reference[3]{};
};
void publish_position_corrections(const particle_position_correction*,std::size_t count) noexcept;
using particle_attachment_reader=bool (*)(std::uintptr_t,std::uint32_t*,char*,std::size_t,float*) noexcept;
void set_attachment_reader(particle_attachment_reader) noexcept;

}  // namespace render_teleportation
