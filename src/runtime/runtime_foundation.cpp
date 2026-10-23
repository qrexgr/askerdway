/**
 * @file
 * @brief Общее состояние runtime.
 *
 * Объявляет schema offsets, внутренние структуры, атомарные снимки, блокировки и защищённое
 * чтение памяти. Открывает анонимное пространство имён, которое продолжается в последующих
 * файлах unity-сборки.
 */

// Shared runtime state and helpers.

#include <Windows.h>
#include "replicated_cooldown.h"
#include "effect_activity.h"
#include "neutral_spawn_boxes.h"
#include "effect_marks.h"
#include "effect_validation.h"
#include "../render/world_frame_renderer.h"
#include "../render/creep_badge.h"
#include "world_view_state.h"
#include "local_convar.h"
#include "minimap_hud.h"
#include "jungle_status.h"
#include "camp_estimates.h"
#include <d3d11.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <mutex>
#include <vector>
#include "comfort_policy.h"
#include "comfort_messages.h"
#include "comfort_protobuf_bridge.h"
#include "comfort_item_ids.h"
#include "comfort_settings.h"
#include "comfort_client_profile.h"
#include "../ui/menu_preferences.h"
#include <MinHook.h>

#include "askerdway.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "../ui/hero_portraits.h"
#include "../ui/ability_icons.h"
#include "../ui/tile_raster_surface.h"
#include "../ui/camp_world_tile.h"
#include "../render/particle_ability_match.h"
#include "../render/particle_owner_index.h"
#include "src/ui/sidebar_icons.h"


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace schema_fields {
    inline std::ptrdiff_t comfort_life_state = 0;
    inline std::ptrdiff_t comfort_steam_id = 0;
    inline std::ptrdiff_t comfort_player_name = 0;
    inline std::ptrdiff_t comfort_match_id = 0;
    // Every field below is resolved from the live SchemaSystem_001 type scope
    // and deliberately starts at zero (fail closed). There are no patch RVAs
    // in this table.
    inline std::ptrdiff_t game_rules_m_bGamePaused = 0;
    inline std::ptrdiff_t game_rules_m_nGameState = 0;
    inline std::ptrdiff_t game_rules_m_NeutralSpawnBoxes = 0;
    inline std::ptrdiff_t game_rules_m_flCreepSpawntime = 0;
    inline std::ptrdiff_t game_rules_m_flGameStartTime = 0;
    inline std::ptrdiff_t game_rules_m_bCreepSpawningEnabled = 0;
    inline std::ptrdiff_t npc_m_iMoveSpeed = 0;
    inline std::ptrdiff_t entity_m_flCreateTime = 0;
    inline std::ptrdiff_t physical_item_m_hItem = 0;
    inline std::ptrdiff_t game_rules_m_nPauseStartTick = 0;
    inline std::ptrdiff_t game_rules_m_nTotalPausedTicks = 0;

    inline std::ptrdiff_t m_iMaxHealth = 0;
    inline std::ptrdiff_t m_iHealth = 0;
    inline std::ptrdiff_t m_pEntityIdentity = 0;
    inline std::ptrdiff_t m_designerName = 0;
    inline std::ptrdiff_t m_pGameSceneNode = 0;
    inline std::ptrdiff_t m_bDormant = 0;
    inline std::ptrdiff_t m_iTeamVisibilityBitmask = 0;
    inline std::ptrdiff_t m_bNPCVisibleState = 0;
    inline std::ptrdiff_t m_iTeamNum = 0;
    inline std::ptrdiff_t m_hOwnerEntity = 0;
    inline std::ptrdiff_t m_vecAbsVelocity = 0;
    inline std::ptrdiff_t m_vecAbsOrigin = 0;
    inline std::ptrdiff_t m_ModifierManager = 0;
    inline std::ptrdiff_t m_Inventory = 0;
    inline std::ptrdiff_t m_nPlayerOwnerID = 0;
    inline std::ptrdiff_t m_hItems = 0;
    inline std::ptrdiff_t m_hTransientCastItem = 0;
    inline std::ptrdiff_t m_flHullRadius = 0;
    inline std::ptrdiff_t m_flPhysicalArmorValue = 0;
    inline std::ptrdiff_t m_flMagicalResistanceValue = 0;
    inline std::ptrdiff_t m_flMana = 0;
    inline std::ptrdiff_t m_nUnitState64 = 0;
    inline std::ptrdiff_t m_vecAbilities = 0;

    inline std::ptrdiff_t controller_m_nPlayerID = 0;
    inline std::ptrdiff_t controller_m_hAssignedHero = 0;

    inline std::ptrdiff_t m_bToggleState = 0;
    inline std::ptrdiff_t m_bInAbilityPhase = 0;
    inline std::ptrdiff_t m_fCooldown = 0;
    inline std::ptrdiff_t m_flCooldownLength = 0;
    inline std::ptrdiff_t m_flCastStartTime = 0;
    inline std::ptrdiff_t m_flChannelStartTime = 0;
    inline std::ptrdiff_t m_flLastCastClickTime = 0;
    inline std::ptrdiff_t m_iManaCost = 0;
    inline std::ptrdiff_t m_iAbilityLevel = 0;
    inline std::ptrdiff_t m_iEnemyAbilityLevel = 0;
    inline std::ptrdiff_t hero_m_iPlayerID = 0;
    inline std::ptrdiff_t m_iPlayerOwnerID = 0;
    inline std::ptrdiff_t m_iCurrentCharges = 0;
    inline std::ptrdiff_t m_bItemEnabled = 0;
    inline std::ptrdiff_t teleport_m_hTarget = 0;
    inline std::ptrdiff_t teleport_m_nFxCaster = 0;
    inline std::ptrdiff_t teleport_m_nFxOrigin = 0;
    inline std::ptrdiff_t teleport_m_nFxDestination = 0;
    inline std::ptrdiff_t teleport_m_vDestination = 0;
    inline std::ptrdiff_t teleport_m_flBaseTime = 0;
    inline std::ptrdiff_t teleport_m_flExtraTime = 0;
    inline std::ptrdiff_t teleport_modifier_m_fChannelTime = 0;
    inline std::ptrdiff_t teleport_modifier_m_vStart = 0;
    inline std::ptrdiff_t teleport_modifier_m_vEnd = 0;
    inline std::ptrdiff_t toggle_cooldown = 0;

    inline std::ptrdiff_t unholy_bonus_damage = 0;
    inline std::ptrdiff_t unholy_bonus_attack_speed = 0;
    inline std::ptrdiff_t unholy_bonus_strength = 0;
    inline std::ptrdiff_t unholy_bonus_armor = 0;
    inline std::ptrdiff_t unholy_health_drain_per_second = 0;
    inline std::ptrdiff_t str_tick_count = 0;
    inline std::ptrdiff_t unholy_bonus_slow_resistance = 0;
    inline std::ptrdiff_t tick_interval = 0;
    inline std::ptrdiff_t m_flDamageRemainder = 0;

    inline std::ptrdiff_t chains_tick_interval = 0;
    inline std::ptrdiff_t chains_tick_damage = 0;
    inline std::ptrdiff_t ice_wall_damage_per_second = 0;
    inline std::ptrdiff_t ice_wall_tick_interval = 0;
    inline std::ptrdiff_t ice_wall_next_dps_time = 0;
    inline std::ptrdiff_t burning_spear_damage = 0;
    inline std::ptrdiff_t burning_spear_max_health_pct = 0;
    inline std::ptrdiff_t sonic_wave_damage_per_tick = 0;
    inline std::ptrdiff_t frostbite_damage_per_second = 0;
    inline std::ptrdiff_t frostbite_tick_interval = 0;
    inline std::ptrdiff_t sun_ray_damage_per_second = 0;
    inline std::ptrdiff_t sun_ray_tick_interval = 0;
    inline std::ptrdiff_t poison_attack_damage = 0;
    inline std::ptrdiff_t heartstopper_aura_damage = 0;

    inline std::ptrdiff_t m_iDamageMin = 0;
    inline std::ptrdiff_t m_iDamageMax = 0;
    inline std::ptrdiff_t m_iDamageBonus = 0;
    inline std::ptrdiff_t m_iBaseDamageMin = 0;
    inline std::ptrdiff_t m_iBaseDamageMax = 0;
    inline std::ptrdiff_t m_iAttackRange = 0;
    inline std::ptrdiff_t m_flBaseAttackTime = 0;
    inline std::ptrdiff_t m_iAttackCapabilities = 0;
    inline std::ptrdiff_t m_flLastAttackTime = 0;
    inline std::ptrdiff_t m_flLastDealtDamageTime = 0;
    inline std::ptrdiff_t m_flLastDamageTime = 0;
    inline std::ptrdiff_t npc_gold_min=0,npc_gold_max=0;
    inline std::ptrdiff_t npc_waiting_to_spawn=0,entity_effects=0,entity_life_state=0;
    inline std::ptrdiff_t m_bIsNeutralUnitType = 0;
    inline std::ptrdiff_t m_hNeutralSpawner = 0;
    inline std::ptrdiff_t m_hPrevTowerAttackTarget = 0;
    inline std::ptrdiff_t m_hTowerAttackTarget = 0;
    inline std::ptrdiff_t neutral_spawner_m_Type = 0;
}

namespace private_modifier_layout {
    // CDOTA_Buff is intentionally absent from the current live ISchemaSystem
    // registrations. These are private modifier ABI fields, kept separate from
    // runtime schema offsets. m_flThinkInterval is never treated as a generic
    // damage interval.
    constexpr std::ptrdiff_t m_flCreationTime = 0x54;
    constexpr std::ptrdiff_t m_flDuration = 0x60;
    constexpr std::ptrdiff_t m_flDieTime = 0x64;
    constexpr std::ptrdiff_t m_hCaster = 0x68;
    constexpr std::ptrdiff_t m_iStackCount = 0x84;
    constexpr std::ptrdiff_t m_flPreviousTick = 0xA0;
    constexpr std::ptrdiff_t m_flThinkInterval = 0xA4;
}

namespace attack_record_offsets {
    constexpr std::ptrdiff_t flags = 0x00;
    constexpr std::ptrdiff_t source = 0x08;
    constexpr std::ptrdiff_t target = 0x10;
    constexpr std::ptrdiff_t damage = 0x1C;
    constexpr std::ptrdiff_t original_damage = 0x20;
    constexpr std::ptrdiff_t physical_damage = 0x24;
    constexpr std::ptrdiff_t magical_damage = 0x2C;
    constexpr std::ptrdiff_t composite_damage = 0x30;
    constexpr std::ptrdiff_t pure_damage = 0x34;
    constexpr std::ptrdiff_t record = 0x38;
    constexpr std::ptrdiff_t projectile_speed = 0x7C;
    constexpr std::ptrdiff_t damage_impacted = 0x11D;
}

namespace {
namespace comfort_runtime {
    extern std::atomic<int> silence_key;
    extern std::atomic<unsigned> key_presses;
}

void log_message(const char* level, const char* message);

// Defined later in the ordered runtime unity source. Damage feeds use the same
// validated PE scanner to resolve private engine networking state without
// embedding patch-specific RVAs.
/**
 * @brief Принимает результат поиска только при однозначном совпадении в проверенном диапазоне
 * образа; неоднозначность означает отказ от разрешения адреса.
 */
std::uintptr_t find_unique_executable_pattern(std::uintptr_t module_base,
    std::size_t module_size, const std::uint8_t* pattern, const char* mask,
    std::size_t pattern_size);
/**
 * @brief Преобразует RIP-relative displacement инструкции в адрес глобального значения;
 * применяется только к предварительно проверенной сигнатуре.
 */
std::uintptr_t resolve_rip_relative_global(std::uintptr_t instruction,
    std::size_t displacement_offset, std::size_t instruction_size,
    std::uintptr_t module_base, std::size_t module_size);

using clock_type = std::chrono::steady_clock;

// Наблюдаемое состояние предмета; не путать с управляющим cycle_state.
enum class armlet_status : int {
    waiting_for_client,
    waiting_for_hero,
    item_missing,
    off,
    active_confirmed,
    applying,
    lingering,
    read_error,
};

// Стадия FSM. Переходы выполняет update_armlet_cycle, а не UI/API-читатели.
enum class cycle_state : int {
    idle,
    on,
    disabling,
    off,
    enabling,
};

// Причина решения оценщика: нужен отдельный код для отсутствующих данных,
// паузы и реальной угрозы, чтобы диагностика не сводилась к одному bool.
enum class safety_state : int {
    unavailable,
    abuse_disabled,
    health_above_threshold,
    safe,
    telemetry_stale,
    state_unconfirmed,
    stunned_or_muted,
    cooldown_wait,
    command_unavailable,
    incomplete_event,
    unknown_modifier,
    threat_pending,
    lethal_cluster,
    high_risk_blocked,
    high_risk_allowed,
    game_paused,
    safe_with_instant_heal,
};

enum class instant_heal_kind : std::uint8_t {
    none,
    magic_stick,
    magic_wand,
    faerie_fire,
    healing_lotus,
    great_healing_lotus,
    greater_healing_lotus,
    holy_locket,
    mekansm,
    guardian_greaves,
    cheese,
};

enum instant_heal_group : std::uint32_t {
    heal_group_stick_wand = 1u << 0,
    heal_group_faerie_fire = 1u << 1,
    heal_group_lotus = 1u << 2,
    heal_group_holy_locket = 1u << 3,
    heal_group_team_items = 1u << 4,
    heal_group_cheese = 1u << 5,
};

constexpr std::uint32_t default_instant_heal_mask =
    heal_group_stick_wand | heal_group_faerie_fire;

struct instant_heal_candidate {
    instant_heal_kind kind = instant_heal_kind::none;
    std::uintptr_t item = 0;
    int entity_index = -1;
    int cast_order = 8;
    int target_index = 0;
    int charges = 0;
    float heal = 0.0f;

    explicit operator bool() const {
        return kind != instant_heal_kind::none && item != 0 &&
            entity_index >= 0 && entity_index <= 0x7FFF && heal > 0.0f;
    }
};

struct monitor_state {
    std::atomic<int> order_rejection{0};
    std::atomic<std::uint64_t> orders_rejected{0};
    std::atomic<int> health{-1};
    std::atomic<int> max_health{-1};
    std::atomic<armlet_status> status{armlet_status::waiting_for_client};
    std::atomic<bool> item_found{false};
    std::atomic<bool> toggle_state{false};
    std::atomic<bool> effect_confirmed{false};
    std::atomic<int> bonus_damage{0};
    std::atomic<int> bonus_attack_speed{0};
    std::atomic<int> bonus_strength{0};
    std::atomic<int> bonus_armor{0};
    std::atomic<int> health_drain_per_second{0};
    std::atomic<int> tick_count{0};
    std::atomic<int> slow_resistance{0};
    std::atomic<float> tick_interval{0.0f};
    std::atomic<float> damage_remainder{0.0f};
    std::atomic<float> toggle_cooldown{0.0f};
    std::atomic<int> confirmation_latency_ms{-1};
    std::atomic<std::uint64_t> transitions{0};
    std::atomic<std::uint64_t> orders_queued{0};
    std::atomic<std::uint64_t> orders_dispatched{0};
    std::atomic<std::uint64_t> orders_confirmed{0};
    std::atomic<std::uint64_t> orders_timed_out{0};
    std::atomic<cycle_state> cycle{cycle_state::idle};
    std::atomic<int> armlet_entity_index{-1};
    std::atomic<bool> abuse_enabled{false};
    // Keep the automation enable/disable bind separate from gameplay hotkeys.
    std::atomic<int> abuse_hotkey{VK_F6};
    std::atomic<bool> automation_ready{false};
    std::atomic<bool> schema_ready{false};
    std::atomic<int> schema_fields_resolved{0};
    std::atomic<int> schema_fields_required{0};
    std::atomic<bool> projectile_telemetry_ready{false};
    std::atomic<bool> internal_projectile_feed_ready{false};
    std::atomic<int> internal_tracking_count{0};
    std::atomic<int> internal_linear_count{0};
    std::atomic<int> internal_tower_count{0};
    std::atomic<int> internal_melee_count{0};
    std::atomic<bool> allow_local_only_fallback{false};
    std::atomic<bool> allow_high_risk_abuse{false};
    std::atomic<bool> use_instant_heals{true};
    std::atomic<std::uint32_t> instant_heal_mask{default_instant_heal_mask};
    std::atomic<instant_heal_kind> instant_heal_available{instant_heal_kind::none};
    std::atomic<int> instant_heal_available_amount{0};
    std::atomic<int> instant_heal_available_charges{0};
    std::atomic<instant_heal_kind> instant_heal_planned{instant_heal_kind::none};
    std::atomic<int> instant_heal_planned_amount{0};
    std::atomic<std::uint64_t> instant_heal_uses{0};
    std::atomic<bool> high_risk_detected{false};
    std::atomic<float> high_risk_interval_ms{-1.0f};
    std::atomic<bool> game_paused{false};
    std::atomic<bool> stunned_or_muted{false};
    std::atomic<safety_state> safety{safety_state::unavailable};
    std::atomic<int> expected_cycle_ms{0};
    std::atomic<int> incoming_count{0};
    std::atomic<float> incoming_damage{0.0f};
    std::atomic<float> nearest_impact_ms{-1.0f};
    std::atomic<float> predicted_minimum_hp{-1.0f};
    // Extra HP that must remain after all scheduled damage events. This value is
    // shared by the menu and exported API so there is one authoritative setting.
    std::atomic<float> safety_margin{325.0f};
    // Verbose diagnostics are opt-in. A game overlay should remain quiet during
    // normal use, while bug reports can enable a detailed debug trace.
    std::atomic<bool> debug_logging{false};
    std::atomic<int> modifier_tick_count{0};
    std::atomic<int> observed_loss_amount{0};
    std::atomic<int> observed_loss_interval_ms{0};
    std::atomic<int> observed_loss_samples{0};
    std::atomic<ULONGLONG> observed_loss_at{0};
    std::atomic<bool> observed_loss_is_self_cost{false};
    std::atomic<int> burning_spear_health_cost{0};
    std::atomic<ULONGLONG> burning_spear_cost_at{0};
    std::atomic<float> network_latency_ms{-1.0f};
    std::atomic<std::uintptr_t> hero_address{0};
    std::atomic<std::uintptr_t> armlet_address{0};
    std::atomic<std::uintptr_t> modifier_address{0};
};

struct poll_history {
    int previous_health = -1;
    int last_logged_health = -1;
    clock_type::time_point last_health_log{};
    bool had_item = false;
    bool previous_toggle = false;
    bool previous_effect = false;
    bool confirmation_pending = false;
    bool confirmation_target = false;
    clock_type::time_point transition_started{};
    std::uintptr_t cached_armlet = 0;
    std::uintptr_t cached_hero = 0;
    clock_type::time_point last_hero_scan{};
    int recent_loss_amount = 0;
    int similar_loss_samples = 0;
    int recent_loss_interval_ms = 0;
    clock_type::time_point recent_loss_at{};
};

// Worker-owned контекст одной последовательности. steady_clock timestamps
// сдвигаются после игровой паузы; их нельзя смешивать с секундами game time.
constexpr ULONGLONG armlet_plan_lifetime_ms = 50;

struct cycle_context {
    cycle_state state = cycle_state::idle;
    bool cycle_armed = false;
    int command_attempts = 0;
    int health_before_enable = -1;
    ULONGLONG rearm_queued_at = 0;
    ULONGLONG rearm_pause_ms = 0;
    ULONGLONG safety_evaluated_at = 0;
    clock_type::time_point state_since{};
    clock_type::time_point last_command{};
    clock_type::time_point retry_after{};
    bool pause_latched = false;
    clock_type::time_point pause_started{};
    instant_heal_candidate instant_heal{};
};

enum class projectile_kind : std::uint8_t {
    tracking,
    linear,
    released_attack,
    instant_attack,
    melee_animation,
    external_damage,
    tower_attack,
};

// Нормализованная запись внутреннего feed или внешней телеметрии.
// Доступ к массиву записей защищён g_projectiles.lock; поля valid/complete
// отделяют неизвестное значение от измеренного нуля.
struct incoming_projectile {
    bool active = false;
    // True only for objects sampled directly from C_DOTA_ProjectileManager.
    // It lets the next complete manager snapshot retire vanished objects
    // immediately without touching entries supplied through the public API.
    bool internally_polled = false;
    bool position_valid = false;
    bool damage_complete = false;
    bool is_attack = false;
    bool ranged_attack = false;
    bool use_projectile = false;
    projectile_kind kind = projectile_kind::tracking;
    std::uint64_t projectile_id = 0;
    std::int32_t attack_record = -1;
    std::uint32_t source_handle = 0xFFFFFFFFu;
    std::uint32_t target_handle = 0xFFFFFFFFu;
    std::uintptr_t source_entity = 0;
    armlet_vec3 position{};
    armlet_vec3 velocity{};
    float speed = 0.0f;
    float radius = 0.0f;
    float remaining_distance = 0.0f;
    float damage = 0.0f;
    float original_damage = 0.0f;
    float physical_damage = 0.0f;
    float magical_damage = 0.0f;
    float pure_damage = 0.0f;
    float composite_damage = 0.0f;
    float confidence = 1.0f;
    ULONGLONG impact_at = 0;
    ULONGLONG source_attack_at = 0;
    ULONGLONG updated_at = 0;
};

enum class timeline_kind : std::uint8_t {
    toggle_off,
    armlet_health_removal,
    instant_heal,
    incoming_damage,
    toggle_on,
    armlet_health_addition,
};

enum class damage_type : std::uint8_t {
    physical,
    magical,
    pure,
    composite,
    mixed,
};

enum class modifier_damage_mode : std::uint8_t {
    per_tick,
    per_second,
};

struct known_modifier_spec {
    const char* class_name;
    std::ptrdiff_t damage_offset;
    std::ptrdiff_t interval_offset;
    std::ptrdiff_t exact_next_tick_offset;
    std::ptrdiff_t max_health_percent_offset;
    float fixed_interval;
    bool multiply_by_stack_count;
    bool refreshable;
    modifier_damage_mode mode;
    damage_type type;
};

std::array known_modifier_registry{
    known_modifier_spec{
        "CDOTA_Modifier_EmberSpirit_SearingChains@@",
        schema_fields::chains_tick_damage,
        schema_fields::chains_tick_interval,
        0,
        0,
        0.0f,
        false,
        false,
        modifier_damage_mode::per_tick,
        damage_type::magical,
    },
    known_modifier_spec{
        "CDOTA_Modifier_Invoker_IceWall_SlowDebuff@@",
        schema_fields::ice_wall_damage_per_second,
        schema_fields::ice_wall_tick_interval,
        schema_fields::ice_wall_next_dps_time,
        0,
        0.0f,
        false,
        false,
        modifier_damage_mode::per_second,
        damage_type::magical,
    },
    known_modifier_spec{
        "CDOTA_Modifier_Huskar_Burning_Spear_Debuff@@",
        schema_fields::burning_spear_damage,
        0,
        0,
        schema_fields::burning_spear_max_health_pct,
        1.0f,
        true,
        true,
        modifier_damage_mode::per_tick,
        damage_type::magical,
    },
    known_modifier_spec{
        "CDOTA_Modifier_QueenOfPain_SonicWave_Damage@@",
        schema_fields::sonic_wave_damage_per_tick,
        0,
        0,
        0,
        0.10f,
        false,
        false,
        modifier_damage_mode::per_tick,
        damage_type::pure,
    },
    known_modifier_spec{
        "CDOTA_Modifier_CrystalMaiden_Frostbite@@",
        schema_fields::frostbite_damage_per_second,
        schema_fields::frostbite_tick_interval,
        0,
        0,
        0.0f,
        false,
        false,
        modifier_damage_mode::per_second,
        damage_type::magical,
    },
    known_modifier_spec{
        "CDOTA_Modifier_Phoenix_Sun_Debuff@@",
        schema_fields::sun_ray_damage_per_second,
        schema_fields::sun_ray_tick_interval,
        0,
        0,
        0.0f,
        false,
        false,
        modifier_damage_mode::per_second,
        damage_type::magical,
    },
    known_modifier_spec{
        "CDOTA_Modifier_Viper_PoisonAttack_Slow@@",
        schema_fields::poison_attack_damage,
        0,
        0,
        0,
        0.50f,
        true,
        true,
        modifier_damage_mode::per_tick,
        damage_type::magical,
    },
    known_modifier_spec{
        "CDOTA_Modifier_Necrolyte_Heartstopper_Aura_Effect@@",
        schema_fields::heartstopper_aura_damage,
        0,
        0,
        schema_fields::heartstopper_aura_damage,
        0.50f,
        false,
        true,
        modifier_damage_mode::per_second,
        damage_type::pure,
    },
};

struct damage_event {
    float time_ms = 0.0f;
    float damage = 0.0f;
    damage_type type = damage_type::mixed;
    std::uintptr_t source = 0;
    float confidence = 1.0f;
    timeline_kind kind = timeline_kind::incoming_damage;
    bool high_risk = false;
};

struct projectile_store {
    SRWLOCK lock = SRWLOCK_INIT;
    std::array<incoming_projectile, 128> entries{};
    ULONGLONG heartbeat = 0;
    ULONGLONG latency_updated_at = 0;
    ULONGLONG external_latency_updated_at = 0;
    float network_latency_ms = -1.0f;
};

struct safety_result {
    bool safe = false;
    ULONGLONG evaluated_at = 0;
    safety_state state = safety_state::unavailable;
    int cycle_ms = 0;
    int count = 0;
    float damage = 0.0f;
    float nearest_ms = -1.0f;
    float minimum_hp = -1.0f;
    int modifier_ticks = 0;
    bool high_risk = false;
    float high_risk_interval_ms = -1.0f;
    instant_heal_candidate instant_heal{};
};

struct overlay_state {
    HMODULE module = nullptr;
    HWND window = nullptr;
    HWND notification_window = nullptr;
    HWND minimap_window = nullptr;
    HWND game_window = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* render_target = nullptr;
    bool imgui_win32_initialized = false;
    bool imgui_dx11_initialized = false;
    UINT width = 0;
    UINT height = 0;
    LONG x = 0;
    LONG y = 0;
    LONG drag_offset_x = 0;
    LONG drag_offset_y = 0;
    bool native_moving = false;
    bool interactive = false;
    bool user_positioned = false;
    std::uint64_t displayed_notification = 0;
};

struct enable_notification_state {
    std::atomic<bool> enabled{false};
    std::atomic<ULONGLONG> shown_at{0};
    std::atomic<std::uint64_t> serial{0};
};

monitor_state g_monitor;
overlay_state g_overlay;
std::atomic<bool> g_native_menu_open{false};
std::atomic<int> g_minimap_hud_pixels{0},g_minimap_hud_scale{100};
std::atomic<bool> g_minimap_auto{true},g_render_camp_boxes{false},g_render_camp_alerts{false};
std::atomic<unsigned> g_game_insert_presses{0},g_game_abuse_presses{0};
std::atomic<bool> g_game_text_input_active{false};
std::atomic<DWORD> g_game_hotkey_focus_time{0};
std::atomic<bool> g_bind_capture_active{false};
std::atomic<int> g_bind_capture_key{0};
std::atomic<int> g_bind_capture_target{0}; // 0 Armlet, 1 Comfort; one shared capture at a time.
std::atomic<bool> g_native_menu_mouse{false},g_native_menu_keyboard{false};
struct native_menu_message {UINT message;WPARAM wparam;LPARAM lparam;unsigned epoch;};
std::array<native_menu_message,256> g_native_menu_messages{};
std::atomic<unsigned> g_native_menu_head{0},g_native_menu_tail{0};
std::atomic<bool> g_native_menu_overflow{false};
std::atomic<bool> g_native_menu_reset{true};
std::atomic<unsigned> g_native_menu_epoch{0};
std::atomic<std::uint64_t> g_native_menu_minimum{(std::uint64_t(16)<<32)|16};
std::atomic<std::uint64_t> g_native_menu_maximum{(std::uint64_t(556)<<32)|776};
void render_native_menu(IDXGISwapChain*);
void invalidate_native_menu_target();
void destroy_native_menu_resources();
bool queue_native_menu_input(HWND,UINT,WPARAM,LPARAM);
enable_notification_state g_enable_notification;
std::atomic<bool> g_running{true};
projectile_store g_projectiles;

std::atomic<bool> g_render_trace_icons{true};
std::atomic<bool> g_render_creep_tracker{true};
std::atomic<bool> g_render_camp_status{false};
std::atomic<bool> g_render_camp_count{true},g_render_camp_gold{true};
std::atomic<bool> g_render_camp_estimates{true},g_render_camp_background{false};
std::atomic<bool> g_render_teleport_arrows{true};
std::atomic<bool> g_render_minimap_right{false};
std::atomic<int> g_render_minimap_size_percent{100};
std::atomic<int> g_render_minimap_offset_x{0};
std::atomic<int> g_render_minimap_offset_y{0};
std::atomic<int> g_render_trace_lifetime_seconds{10};
std::atomic<int> g_render_hero_icon_scale_percent{50};

// Published counts are separate atomics rather than direct reads of the worker-
// owned arrays below. External API calls can occur on arbitrary helper threads;
// exposing vector/array mutation would introduce a data race in diagnostics.
std::atomic<std::uint32_t> g_render_trace_count_snapshot{0};
std::atomic<std::uint32_t> g_render_teleport_count_snapshot{0};
std::atomic<std::uint32_t> g_render_camp_count_snapshot{0};

struct render_diagnostics_state {
    std::atomic<std::uint64_t> particle_observations{0};
    std::atomic<std::uint64_t> resolved_particle_owners{0};
    std::atomic<std::uint64_t> path_particle_owners{0};
    std::atomic<std::uint64_t> proximity_particle_owners{0};
    std::atomic<std::uint64_t> unresolved_particle_owners{0};
    std::atomic<std::uint64_t> fog_ability_events{0};
    std::atomic<std::uint64_t> fog_farming_events{0};
    std::atomic<std::uint64_t> visibility_transitions{0};
    std::atomic<std::uint64_t> teleport_origins{0};
    std::atomic<std::uint64_t> teleport_routes{0};
    std::atomic<std::uint64_t> minimap_frames{0};
    std::atomic<std::uint64_t> world_marker_frames{0};
    std::atomic<std::uint64_t> tower_shots{0};
    std::atomic<std::uint32_t> intelligence_poll_us{0};
    std::atomic<std::uint32_t> native_render_us{0};
    std::atomic<int> enemy_heroes{0};
    std::atomic<int> neutral_creeps{0};
    std::atomic<int> blocked_camps{0};
    std::atomic<int> cached_towers{0};
    std::atomic<int> rtti_towers{0};
    std::atomic<int> rtti_camps{0};
    std::atomic<int> targeted_towers{0};
    std::atomic<int> visible_world_markers{0};
    std::atomic<bool> view_matrix_ready{false};
};

render_diagnostics_state g_render_diagnostics;

struct render_trace_entry {
    std::uint32_t owner_handle = 0xFFFFFFFFu;
    armlet_vec3 position{};
    char designer_name[96]{};
    char detector_trigger[64]{"Fog of War"};
    char ability_icon[96]{};
    ULONGLONG created_at = 0;
    // Fixed correlation anchor: child particles cannot prolong an attack burst.
    ULONGLONG event_started_at = 0;
    std::uint64_t source_event_key = 0;
    ULONGLONG expires_at = 0;
    std::uint8_t source_priority = 0;
    bool particle_position = false;
    bool continuous = false;
    bool estimated_position = false;
    bool creep_wave = false;
    ULONGLONG particle_observed_at = 0;
    std::uintptr_t particle_effect = 0;
    std::uintptr_t particle_collection = 0;
};

struct enemy_ability_entry {
    std::uint32_t entity_handle = 0xFFFFFFFFu;
    bool initialized = false;
    bool in_ability_phase = false;
    bool toggle_state = false;
    float cooldown_end = 0.0f;
    float cast_start_time = 0.0f;
    float channel_start_time = 0.0f;
    float last_cast_click_time = 0.0f;
    char designer_name[96]{};
    ULONGLONG last_event_at = 0;
    ULONGLONG activated_at = 0;
    ULONGLONG last_observed_at = 0;
};

struct enemy_visibility_entry {
    std::uint32_t entity_index = 0xFFFFFFFFu;
    std::uint32_t entity_generation = 0xFFFFFFFFu;
    std::uintptr_t entity_address = 0;
    int player_id = -1;
    std::uint8_t team = 0;
    bool initialized = false;
    bool dormant = false;
    ULONGLONG visibility_confirmed_at = 0;
    bool has_last_visible_position = false;
    armlet_vec3 last_visible_position{};
    // Independent of the chosen icon: only validated caster-carried emitters
    // provide fresh body evidence for camp occupancy while hidden.
    bool has_effect_body_position = false;
    armlet_vec3 effect_body_position{};
    ULONGLONG effect_body_observed_at = 0;
    bool has_network_position = false;
    armlet_vec3 network_position{};
    ULONGLONG network_position_observed_at = 0;
    ULONGLONG network_position_stable_since = 0;
    bool has_network_position_stability_anchor = false;
    armlet_vec3 network_position_stability_anchor{};
    bool has_pending_teleport_jump = false;
    armlet_vec3 pending_teleport_origin{};
    armlet_vec3 pending_teleport_destination{};
    ULONGLONG pending_teleport_detected_at = 0;
    ULONGLONG inferred_teleport_at = 0;
    int last_health = -1;
    float last_attack_time = 0.0f;
    float last_dealt_damage_time = 0.0f;
    float last_mana = -1.0f;
    bool activity_initialized = false;
    ULONGLONG last_attack_event_at = 0;
    std::array<enemy_ability_entry, 32> abilities{};
    std::size_t ability_count = 0;
    ULONGLONG abilities_discovered_at = 0;
    std::array<enemy_ability_entry, 32> items{};
    std::size_t item_count = 0;
    std::array<std::uint32_t,32> inventory_handles{};
    std::size_t inventory_count=0;
    ULONGLONG items_discovered_at = 0;
    askerdway::replicated_cooldown scroll_cooldown{};
    char designer_name[96]{};
    ULONGLONG last_observed_at = 0;
};

struct render_camp_entry {
    armlet_vec3 position{};
    int type = 0;
    bool blocked = false;
    bool estimated = true;
    bool has_native_bounds = false;
    armlet_vec3 minimum{}, maximum{};
};

struct neutral_creep_entry {
    std::uint32_t entity_index = 0xFFFFFFFFu;
    armlet_vec3 position{};
    int health = 0;
    float last_damage_time = 0.0f;
    int gold_min=-1,gold_max=-1;
};
struct render_pulse_entry {armlet_vec3 position{};ULONGLONG started_at=0,expires_at=0;};

// Запись render-маршрута под g_render_intelligence_lock. Полный маршрут
// означает две известные точки, а не подтверждённое прибытие героя.
struct render_teleport_entry {
    std::uint32_t owner_handle = 0xFFFFFFFFu;
    // Ненулевой ключ связывает наблюдения одного поколения каста.
    std::uint64_t cast_key = 0;
    armlet_vec3 origin{};
    armlet_vec3 destination{};
    bool has_origin = false;
    bool has_destination = false;
    std::uint8_t origin_confidence = 0;
    std::uint8_t destination_confidence = 0;
    // Слабая частица не должна сокращать длительность точного modifier snapshot.
    std::uint8_t timing_confidence = 0;
    bool enemy_confirmed = false;
    bool debug_origin_reported = false;
    bool debug_complete_reported = false;
    std::uint32_t effect_flags = 0;
    std::uint32_t duration_ms = 0;
    char designer_name[96]{};
    ULONGLONG first_seen_at = 0;
    // GetTickCount64 milliseconds, скорректированные по игровому elapsed_ms.
    ULONGLONG cast_started_at = 0;
    ULONGLONG completed_at = 0;
    // A terminal channel cannot be revived by queued particle observations.
    bool channel_observed = false;
    bool channel_finished = false;
    bool canceled = false;
    ULONGLONG expires_at = 0;
};

struct enemy_teleport_status_entry {
    int player_id = -1;
    std::uint8_t team = 0;
    bool has_scroll = false;
    bool inventory_known = false;
    float cooldown_seconds = 0.0f;
    ULONGLONG cooldown_observed_at = 0;
    int charges = -1;
    int health=-1,max_health=-1;
    bool visible=false;
    bool cooldown_estimated=false;
    char hero_abbreviation[4]{};
};

struct render_intelligence_state {
    std::array<render_pulse_entry,16> pulses{};
    std::size_t pulse_count=0;
    effect_marks::store marks{};
    std::array<effect_activity::clock,512> modifier_events{};
    // Known incoming target tags are revalidated independently of discovery.
    effect_validation::cache effect_validations{};
    // Local/friendly heroes and neutral creeps are discovered by separate queues.
    effect_validation::queue effect_priority_queue{},effect_neutral_queue{};
    std::array<render_trace_entry, 64> traces{};
    std::size_t trace_count = 0;
    std::array<enemy_visibility_entry, 16> enemy_visibility{};
    std::size_t enemy_visibility_count = 0;
    ULONGLONG enemy_visibility_refreshed_at = 0;
    ULONGLONG enemy_visibility_discovered_at = 0;
    std::array<render_camp_entry, 32> camps{};
    std::size_t camp_count = 0;
    std::array<neutral_creep_entry, 192> neutral_creeps{};
    jungle_status::store jungle;
    std::size_t neutral_creep_count = 0;
    std::array<std::uint32_t,32> friendly_effect_targets{};
    std::size_t friendly_effect_target_count=0;
    std::array<render_teleport_entry, 16> teleports{};
    std::size_t teleport_count = 0;
    std::array<enemy_teleport_status_entry, 10> enemy_teleports{};
    std::size_t enemy_teleport_count = 0;
    ULONGLONG enemy_teleports_refreshed_at = 0;
    std::uint64_t observation_cursor = 0;
    ULONGLONG camps_refreshed_at = 0;
    ULONGLONG camp_entities_discovered_at = 0;
    struct camp_entity { std::uint32_t index=0; std::uintptr_t address=0; };
    std::array<camp_entity,512> camp_entities{};
    std::size_t camp_entity_count=0;
    ULONGLONG neutrals_sampled_at = 0;
    armlet_vec3 minimap_minimum{-8288.0f, -8288.0f, 0.0f};
    armlet_vec3 minimap_maximum{8288.0f, 8288.0f, 0.0f};
    bool has_minimap_bounds = false;
    ULONGLONG minimap_bounds_refreshed_at = 0;
};

render_intelligence_state g_render_intelligence;
SRWLOCK g_render_intelligence_lock = SRWLOCK_INIT;

// Small RAII guards make every return path release the Windows slim lock. The
// render worker is the only writer; public snapshot functions may be readers.
class render_intelligence_exclusive_guard {
public:
    render_intelligence_exclusive_guard() noexcept {
        AcquireSRWLockExclusive(&g_render_intelligence_lock);
    }
    ~render_intelligence_exclusive_guard() {
        ReleaseSRWLockExclusive(&g_render_intelligence_lock);
    }
    render_intelligence_exclusive_guard(
        const render_intelligence_exclusive_guard&) = delete;
    render_intelligence_exclusive_guard& operator=(
        const render_intelligence_exclusive_guard&) = delete;
};

class render_intelligence_shared_guard {
public:
    render_intelligence_shared_guard() noexcept {
        AcquireSRWLockShared(&g_render_intelligence_lock);
    }
    ~render_intelligence_shared_guard() {
        ReleaseSRWLockShared(&g_render_intelligence_lock);
    }
    render_intelligence_shared_guard(const render_intelligence_shared_guard&) = delete;
    render_intelligence_shared_guard& operator=(
        const render_intelligence_shared_guard&) = delete;
};

struct tower_cache_state {
    std::array<std::uintptr_t, 32> entries{};
    std::size_t count = 0;
    ULONGLONG refreshed_at = 0;
    std::uintptr_t entity_system = 0;
    std::uint8_t local_team = 0;
};

struct melee_source_cache_state {
    std::array<std::uintptr_t, 128> entries{};
    std::size_t count = 0;
    ULONGLONG refreshed_at = 0;
    std::uint8_t local_team = 0;
};

struct periodic_source_cache_state {
    std::array<std::uintptr_t, 16> entries{};
    std::size_t count = 0;
    ULONGLONG refreshed_at = 0;
    std::uint8_t local_team = 0;
};

tower_cache_state g_tower_cache;
melee_source_cache_state g_melee_source_cache;
periodic_source_cache_state g_periodic_source_cache;
std::atomic<std::uintptr_t> g_client_base_address{0};
std::atomic<std::uintptr_t> g_game_entity_system_global{0};
std::atomic<std::uintptr_t> g_global_vars_global{0};
std::atomic<std::uintptr_t> g_game_rules_global{0};
std::atomic<std::uintptr_t> g_local_player_pawn_global{0};
std::atomic<std::uintptr_t> g_prepare_unit_orders{0};
std::atomic<std::uintptr_t> g_local_controller_accessor{0};
std::atomic<std::uintptr_t> g_local_controller_global{0};
std::atomic<std::uintptr_t> g_local_controller_address{0};
std::atomic<std::uintptr_t> g_projectile_manager_global{0};
std::atomic<std::uintptr_t> g_view_matrix_address{0};
std::atomic<std::uintptr_t> g_world_ui_state_address{0};
std::atomic<std::size_t> g_entity_chunk_table_offset{0};
std::atomic<std::uint32_t> g_render_effect_mark_count_snapshot{0};
std::atomic<std::size_t> g_entity_entry_stride{0};
std::atomic<std::size_t> g_modifier_count_offset{0};
std::atomic<std::size_t> g_modifier_data_offset{0};
// Протокол единственного pending request: worker публикует queued через release,
// игровой WndProc забирает запрос для dispatch. Это не очередь нескольких producers.
enum class pending_order_state : std::uint8_t {
    empty,
    queued,
    dispatching,
};
enum class pending_armlet_order_kind : std::uint8_t {
    single_toggle,
    rearm_begin,
};
std::atomic<pending_order_state> g_pending_order_state{pending_order_state::empty};
std::atomic<ULONGLONG> g_pending_order_queued_at{0};
std::atomic<ULONGLONG> g_pending_safety_evaluated_at{0};
std::atomic<int> g_pending_health{0};
std::atomic<int> g_pending_max_health{0};
std::atomic<std::uintptr_t> g_pending_armlet{0};
std::atomic<std::uintptr_t> g_pending_hero{0};
std::atomic<int> g_pending_ability_index{-1};
std::atomic<bool> g_pending_expected_toggle{false};
std::atomic<pending_armlet_order_kind> g_pending_order_kind{
    pending_armlet_order_kind::single_toggle};
std::atomic<std::uintptr_t> g_pending_heal_item{0};
std::atomic<int> g_pending_heal_entity_index{-1};
std::atomic<int> g_pending_heal_kind{0};
std::atomic<int> g_pending_heal_order{8};
std::atomic<int> g_pending_heal_target_index{0};
std::atomic<int> g_pending_heal_amount{0};
std::atomic<bool> g_last_order_target_toggle{false};
std::atomic<pending_armlet_order_kind> g_last_order_kind{
    pending_armlet_order_kind::single_toggle};
std::atomic<int> g_last_rearm_health_before{-1};
std::atomic<ULONGLONG> g_last_order_dispatched_at{0};
// Operational acknowledgement, independent of optional order diagnostics.
std::atomic<ULONGLONG> g_last_rearm_submitted_at{0};
std::atomic<ULONGLONG> g_last_rearm_started_at{0};
std::atomic<WNDPROC> g_original_game_window_proc{nullptr};
std::atomic<int> g_game_window_proc_depth{0};
HWND g_order_dispatch_window = nullptr;

constexpr UINT execute_armlet_order_message = WM_APP + 0x2FA8;
#if defined(ASKERDWAY_COMBINED)
constexpr char askerdway_version[] = "1.0.0";
#endif

/**
 * @brief Защищённо копирует память; false означает отсутствие достоверного результата.
 * Проверка доступности не закрепляет время жизни игрового объекта.
 */
bool safe_copy_memory(std::uintptr_t address, void* destination,
    std::size_t size) noexcept {
    if (address == 0 || destination == nullptr || size == 0 ||
        address > std::numeric_limits<std::uintptr_t>::max() - size)
        return false;

#if defined(_MSC_VER)
    // The DLL lives in the Dota process. ReadProcessMemory on our own process
    // turns every schema field into a kernel transition and was the dominant
    // cost of the 20 Hz intelligence scan. SEH keeps stale entity handles
    // fail-closed while ordinary reads remain a direct memcpy.
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    SIZE_T bytes_read = 0;
    return ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address), destination, size,
        &bytes_read) != FALSE && bytes_read == size;
#endif
}

template <typename T>
/**
 * @brief Защищённо копирует память; false означает отсутствие достоверного результата.
 * Проверка доступности не закрепляет время жизни игрового объекта.
 */
bool safe_read(std::uintptr_t address, T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    return safe_copy_memory(address, &value, sizeof(value));
}

/**
 * @brief Проверяет только правдоподобие адреса, не его доступность и не тип объекта;
 * фактическое чтение всё равно выполняется через safe_read.
 */
bool plausible_pointer(std::uintptr_t address) {
    return address >= 0x10000 && address < 0x00007FFFFFFFFFFFULL &&
        (address & (alignof(void*) - 1)) == 0;
}

/**
 * @brief Разыменовывает ранее разрешённый глобальный адрес; false требует пропустить зависимое
 * чтение, а не продолжать с нулевой базой.
 */
bool read_game_entity_system(std::uintptr_t client_base,
    std::uintptr_t& entity_system) {
    (void)client_base;
    const auto global = g_game_entity_system_global.load(std::memory_order_acquire);
    return safe_read(global, entity_system) && plausible_pointer(entity_system);
}

/**
 * @brief Разыменовывает ранее разрешённый глобальный адрес; false требует пропустить зависимое
 * чтение, а не продолжать с нулевой базой.
 */
bool read_global_vars(std::uintptr_t client_base, std::uintptr_t& global_vars) {
    (void)client_base;
    const auto global = g_global_vars_global.load(std::memory_order_acquire);
    return safe_read(global, global_vars) && plausible_pointer(global_vars);
}

template <std::size_t Size>
/**
 * @brief Читает ограниченную строку из памяти движка. Выходной буфер принадлежит вызывающей
 * стороне; ошибка чтения не означает совпадение имени.
 */
bool read_c_string(std::uintptr_t address, char (&buffer)[Size]) {
    static_assert(Size > 1);
    std::memset(buffer, 0, Size);
    // Character data is byte-aligned. Requiring pointer-size alignment here
    // rejected valid Source 2 designer names such as "item_tpscroll".
    const bool readable_address = address >= 0x10000 &&
        address < 0x00007FFFFFFFFFFFULL;
    if (!readable_address ||
        !safe_copy_memory(address, buffer, Size - 1))
        return false;

    buffer[Size - 1] = '\0';
    return true;
}

/**
 * @brief Разбирает заголовки PE для границ образа; эти границы ограничивают дальнейший поиск и
 * проверку адресов.
 */
std::size_t image_size(std::uintptr_t module_base) {
    IMAGE_DOS_HEADER dos{};
    if (!safe_read(module_base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    IMAGE_NT_HEADERS64 nt{};
    if (!safe_read(module_base + static_cast<std::uintptr_t>(dos.e_lfanew), nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE)
        return 0;

    return nt.OptionalHeader.SizeOfImage;
}

struct rtti_complete_object_locator {
    std::uint32_t signature;
    std::uint32_t offset;
    std::uint32_t constructor_displacement;
    std::int32_t type_descriptor_rva;
    std::int32_t class_descriptor_rva;
    std::int32_t self_rva;
};

template <std::size_t Size>
/**
 * @brief Проверяет RTTI относительно загруженного client image. Используется для идентификации
 * типа там, где одного designer name недостаточно.
 */
bool read_rtti_name(std::uintptr_t object, std::uintptr_t client_base,
    std::size_t client_size, char (&name)[Size]) {
    if (!plausible_pointer(object) || client_size == 0)
        return false;

    std::uintptr_t vtable = 0;
    if (!safe_read(object, vtable) || vtable < client_base + sizeof(void*) ||
        vtable >= client_base + client_size)
        return false;

    std::uintptr_t locator_address = 0;
    if (!safe_read(vtable - sizeof(void*), locator_address) ||
        locator_address < client_base || locator_address >= client_base + client_size)
        return false;

    rtti_complete_object_locator locator{};
    if (!safe_read(locator_address, locator) || locator.signature != 1 ||
        locator.type_descriptor_rva <= 0)
        return false;

    const auto type_descriptor = client_base +
        static_cast<std::uint32_t>(locator.type_descriptor_rva);
    if (!read_c_string(type_descriptor + 2 * sizeof(void*), name))
        return false;

    return true;
}

/**
 * @brief Проверяет RTTI относительно загруженного client image. Используется для идентификации
 * типа там, где одного designer name недостаточно.
 */
bool rtti_name_contains(std::uintptr_t object, std::uintptr_t client_base,
    std::size_t client_size, const char* expected) {
    char name[192]{};
    if (!read_rtti_name(object, client_base, client_size, name))
        return false;

    return std::strstr(name, expected) != nullptr;
}
