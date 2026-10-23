/**
 * @file
 * @brief Реализация публичного ABI.
 *
 * Проверяет версии, размеры и диапазоны аргументов, переводит внутренние состояния в публичные
 * снимки. Составные render/projectile-данные копируются под соответствующей блокировкой;
 * отдельные атомарные поля не образуют транзакционный снимок всего процесса.
 */

// Public API implementation.

static_assert(std::is_standard_layout_v<askerdway_config_v1>);
static_assert(std::is_trivially_copyable_v<askerdway_config_v1>);
static_assert(std::is_standard_layout_v<askerdway_status_v1>);
static_assert(std::is_trivially_copyable_v<askerdway_status_v1>);
static_assert(offsetof(askerdway_config_v1, struct_size) == 0);
static_assert(offsetof(askerdway_config_v1, abi_version) == sizeof(std::uint32_t));
static_assert(offsetof(askerdway_status_v1, struct_size) == 0);
static_assert(offsetof(askerdway_status_v1, abi_version) == sizeof(std::uint32_t));
static_assert(std::is_standard_layout_v<askerdway_render_snapshot_v2>);
static_assert(std::is_trivially_copyable_v<askerdway_render_snapshot_v2>);
static_assert(offsetof(askerdway_render_snapshot_v2, struct_size) == 0);
static_assert(offsetof(askerdway_render_snapshot_v2, abi_version) == sizeof(std::uint32_t));

namespace {

constexpr std::uint64_t askerdway_capabilities =
    ASKERDWAY_CAP_ARMLET_AUTOMATION |
    ASKERDWAY_CAP_EXTERNAL_DAMAGE_TIMELINE |
    ASKERDWAY_CAP_INSTANT_HEALS |
    ASKERDWAY_CAP_PARTICLE_FOW_RENDER |
    ASKERDWAY_CAP_HERO_PARTICLE_TRAILS |
    ASKERDWAY_CAP_TELEPORT_ARROWS |
    ASKERDWAY_CAP_NEUTRAL_CAMP_STATUS |
    ASKERDWAY_CAP_ENGINE_SNAPSHOTS |
    ASKERDWAY_CAP_NATIVE_WORLD_RENDER |
    ASKERDWAY_CAP_RENDER_DIAGNOSTICS;

template <typename Structure>
/**
 * @brief Проверяет ненулевой указатель, минимальный sizeof и major ABI. Это проверка
 * контракта, а не защита от произвольного недоступного адреса внешнего caller.
 */
askerdway_result validate_api_structure(const Structure* structure) noexcept {
    if (structure == nullptr)
        return ASKERDWAY_INVALID_ARGUMENT;
    if (structure->struct_size < sizeof(Structure))
        return ASKERDWAY_BUFFER_TOO_SMALL;
    if ((structure->abi_version & ASKERDWAY_ABI_MAJOR_MASK) !=
        (ASKERDWAY_ABI_VERSION & ASKERDWAY_ABI_MAJOR_MASK))
        return ASKERDWAY_INCOMPATIBLE_ABI;
    return ASKERDWAY_OK;
}

bool api_boolean_is_valid(std::uint32_t value) noexcept {
    return value == 0 || value == 1;
}

bool api_damage_is_valid(const askerdway_damage_components& damage) noexcept {
    return finite_nonnegative(damage.original) &&
        finite_nonnegative(damage.physical) &&
        finite_nonnegative(damage.magical) &&
        finite_nonnegative(damage.pure) &&
        finite_nonnegative(damage.composite);
}

bool api_vector_is_finite(const askerdway_vec3& vector) noexcept {
    return std::isfinite(vector.x) && std::isfinite(vector.y) &&
        std::isfinite(vector.z);
}

std::int32_t public_armlet_state(armlet_status value) noexcept {
    switch (value) {
    case armlet_status::waiting_for_client: return ASKERDWAY_ARMLET_WAITING_FOR_CLIENT;
    case armlet_status::waiting_for_hero: return ASKERDWAY_ARMLET_WAITING_FOR_HERO;
    case armlet_status::item_missing: return ASKERDWAY_ARMLET_ITEM_MISSING;
    case armlet_status::off: return ASKERDWAY_ARMLET_OFF;
    case armlet_status::active_confirmed: return ASKERDWAY_ARMLET_ACTIVE_CONFIRMED;
    case armlet_status::applying: return ASKERDWAY_ARMLET_APPLYING;
    case armlet_status::lingering: return ASKERDWAY_ARMLET_LINGERING;
    case armlet_status::read_error: return ASKERDWAY_ARMLET_READ_ERROR;
    }
    return ASKERDWAY_ARMLET_READ_ERROR;
}

std::int32_t public_cycle_state(cycle_state value) noexcept {
    switch (value) {
    case cycle_state::idle: return ASKERDWAY_CYCLE_IDLE;
    case cycle_state::on: return ASKERDWAY_CYCLE_ON;
    case cycle_state::disabling: return ASKERDWAY_CYCLE_DISABLING;
    case cycle_state::off: return ASKERDWAY_CYCLE_OFF;
    case cycle_state::enabling: return ASKERDWAY_CYCLE_ENABLING;
    }
    return ASKERDWAY_CYCLE_IDLE;
}

std::int32_t public_safety_state(safety_state value) noexcept {
    switch (value) {
    case safety_state::unavailable: return ASKERDWAY_SAFETY_UNAVAILABLE;
    case safety_state::abuse_disabled: return ASKERDWAY_SAFETY_ABUSE_DISABLED;
    case safety_state::health_above_threshold:
        return ASKERDWAY_SAFETY_HEALTH_ABOVE_THRESHOLD;
    case safety_state::safe: return ASKERDWAY_SAFETY_SAFE;
    case safety_state::telemetry_stale: return ASKERDWAY_SAFETY_TELEMETRY_STALE;
    case safety_state::state_unconfirmed: return ASKERDWAY_SAFETY_STATE_UNCONFIRMED;
    case safety_state::stunned_or_muted: return ASKERDWAY_SAFETY_STUNNED_OR_MUTED;
    case safety_state::cooldown_wait: return ASKERDWAY_SAFETY_COOLDOWN_WAIT;
    case safety_state::command_unavailable:
        return ASKERDWAY_SAFETY_COMMAND_UNAVAILABLE;
    case safety_state::incomplete_event: return ASKERDWAY_SAFETY_INCOMPLETE_EVENT;
    case safety_state::unknown_modifier: return ASKERDWAY_SAFETY_UNKNOWN_MODIFIER;
    case safety_state::threat_pending: return ASKERDWAY_SAFETY_THREAT_PENDING;
    case safety_state::lethal_cluster: return ASKERDWAY_SAFETY_LETHAL_CLUSTER;
    case safety_state::high_risk_blocked: return ASKERDWAY_SAFETY_HIGH_RISK_BLOCKED;
    case safety_state::high_risk_allowed: return ASKERDWAY_SAFETY_HIGH_RISK_ALLOWED;
    case safety_state::game_paused: return ASKERDWAY_SAFETY_GAME_PAUSED;
    case safety_state::safe_with_instant_heal:
        return ASKERDWAY_SAFETY_SAFE_WITH_INSTANT_HEAL;
    }
    return ASKERDWAY_SAFETY_UNAVAILABLE;
}

std::uint64_t collect_public_state_flags() noexcept {
    std::uint64_t flags = 0;
    if (g_running.load(std::memory_order_acquire))
        flags |= ASKERDWAY_STATE_RUNNING;
    if (g_monitor.schema_ready.load(std::memory_order_acquire))
        flags |= ASKERDWAY_STATE_SCHEMA_READY;
    if (g_monitor.automation_ready.load(std::memory_order_relaxed))
        flags |= ASKERDWAY_STATE_AUTOMATION_READY;
    if (g_monitor.item_found.load(std::memory_order_relaxed))
        flags |= ASKERDWAY_STATE_ARMLET_FOUND;
    if (g_monitor.toggle_state.load(std::memory_order_relaxed))
        flags |= ASKERDWAY_STATE_ARMLET_TOGGLED_ON;
    if (g_monitor.effect_confirmed.load(std::memory_order_relaxed))
        flags |= ASKERDWAY_STATE_MODIFIER_CONFIRMED;
    if (g_monitor.projectile_telemetry_ready.load(std::memory_order_relaxed))
        flags |= ASKERDWAY_STATE_PROJECTILE_FEED_READY;
    if (g_monitor.game_paused.load(std::memory_order_relaxed))
        flags |= ASKERDWAY_STATE_GAME_PAUSED;
    if (g_monitor.stunned_or_muted.load(std::memory_order_relaxed))
        flags |= ASKERDWAY_STATE_STUNNED_OR_MUTED;
    if (g_monitor.high_risk_detected.load(std::memory_order_relaxed))
        flags |= ASKERDWAY_STATE_HIGH_RISK;
    if (render_teleportation::is_hook_installed())
        flags |= ASKERDWAY_STATE_PARTICLE_HOOK_READY;
    if (g_render_diagnostics.view_matrix_ready.load(std::memory_order_relaxed))
        flags |= ASKERDWAY_STATE_NATIVE_VIEW_MATRIX_READY;
    if (world_frame::is_draw_bypassed())
        flags |= ASKERDWAY_STATE_WORLD_DRAW_BYPASS;
    return flags;
}

}  // namespace

std::int32_t AskerdwayApi_GetComfortConfig(askerdway_comfort_config_v1* config) noexcept {
    const auto result = validate_api_structure(config);
    if (result != ASKERDWAY_OK) return result;
    std::lock_guard guard(comfort_runtime::lock);
    *config = comfort_runtime::rules.config;
    return ASKERDWAY_OK;
}
std::int32_t AskerdwayApi_SetComfortConfig(const askerdway_comfort_config_v1* config) noexcept {
    const auto result = validate_api_structure(config);
    if (result != ASKERDWAY_OK) return result;
    if (!g_running.load(std::memory_order_acquire)) return ASKERDWAY_SHUTTING_DOWN;
    const int key = config->silence_hotkey;
    if (!askerdway::comfort::valid_config(*config) || (key && (key < VK_BACK || key == VK_INSERT ||
        key == VK_END || key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU ||
        key == VK_LWIN || key == VK_RWIN || key == g_monitor.abuse_hotkey.load()))) return ASKERDWAY_VALUE_OUT_OF_RANGE;
    std::lock_guard guard(comfort_runtime::lock);
    comfort_runtime::config_changed(*config);
    return ASKERDWAY_OK;
}
std::int32_t AskerdwayApi_GetComfortStatus(askerdway_comfort_status_v1* status) noexcept {
    const auto result = validate_api_structure(status);
    if (result != ASKERDWAY_OK) return result;
    askerdway_comfort_status_v1 value{};
    value.available_features = comfort_runtime::available.load(std::memory_order_acquire);
    value.settings_saved = comfort_runtime::saved.load();
    std::lock_guard guard(comfort_runtime::lock);
    const auto& rules = comfort_runtime::rules;
    value.silence_requested = rules.silence;
    value.full_silence_active = rules.silence && comfort_runtime::context_ready.load() &&
        value.available_features == ASKERDWAY_COMFORT_FEATURE_ALL &&
        askerdway::comfort::valid_player(rules.current_context().local_player);
    value.blocked_players = rules.blocked;
    value.session = comfort_runtime::context_ready.load() ? rules.current_context().session : 0;
    *status = value;
    return ASKERDWAY_OK;
}
std::int32_t AskerdwayApi_SetComfortSilence(std::uint32_t enabled) noexcept {
    if (enabled > 1) return ASKERDWAY_VALUE_OUT_OF_RANGE;
    if (!g_running.load(std::memory_order_acquire)) return ASKERDWAY_SHUTTING_DOWN;
    std::lock_guard guard(comfort_runtime::lock);
    comfort_runtime::rules.silence = enabled != 0;
    comfort_runtime::rules.clear_history();
    return ASKERDWAY_OK;
}
std::int32_t AskerdwayApi_SetComfortPlayerBlocked(std::uint32_t id, std::uint32_t blocked) noexcept {
    if (id >= 64 || blocked > 1) return ASKERDWAY_VALUE_OUT_OF_RANGE;
    if (!g_running.load(std::memory_order_acquire)) return ASKERDWAY_SHUTTING_DOWN;
    std::lock_guard guard(comfort_runtime::lock);
    if (int(id) == comfort_runtime::rules.current_context().local_player) return ASKERDWAY_INVALID_ARGUMENT;
    if (!comfort_runtime::context_ready.load() || comfort_runtime::players[id].entity < 0) return ASKERDWAY_NOT_READY;
    auto& mask = comfort_runtime::rules.blocked;
    mask = blocked ? (mask | (1ull << id)) : (mask & ~(1ull << id));
    comfort_runtime::rules.clear_history();
    return ASKERDWAY_OK;
}

/**
 * @brief Возвращает числовую версию бинарного интерфейса.
 */
std::uint32_t AskerdwayApi_GetAbiVersion() noexcept {
    return ASKERDWAY_ABI_VERSION;
}

/**
 * @brief Возвращает битовую маску реализованных возможностей; готовность в текущей игре
 * проверяется отдельно через GetStatus.
 */
std::uint64_t AskerdwayApi_GetCapabilities() noexcept {
    return askerdway_capabilities;
}

/**
 * @brief Возвращает статическую строку результата; память не освобождается вызывающим кодом.
 */
const char* AskerdwayApi_ResultString(askerdway_result result) noexcept {
    switch (result) {
    case ASKERDWAY_OK: return "ok";
    case ASKERDWAY_INVALID_ARGUMENT: return "invalid argument";
    case ASKERDWAY_INCOMPATIBLE_ABI: return "incompatible ABI major version";
    case ASKERDWAY_BUFFER_TOO_SMALL: return "structure buffer is too small";
    case ASKERDWAY_NOT_READY: return "runtime is not ready";
    case ASKERDWAY_SHUTTING_DOWN: return "DLL is shutting down";
    case ASKERDWAY_VALUE_OUT_OF_RANGE: return "value is out of range";
    }
    return "unknown result";
}

/**
 * @brief Заполняет всю структуру значениями по умолчанию, включая размер и ABI. Передайте
 * доступный буфер размером не меньше sizeof(askerdway_config_v1).
 */
askerdway_result AskerdwayApi_InitConfig(askerdway_config_v1* config) noexcept {
    if (config == nullptr)
        return ASKERDWAY_INVALID_ARGUMENT;
    *config = askerdway_config_v1{};
    config->update_mask = ASKERDWAY_CONFIG_ALL;
    return ASKERDWAY_OK;
}

/**
 * @brief Проверяет v1-заголовок и копирует текущие настройки. Независимые атомарные поля не
 * являются общей транзакцией.
 */
askerdway_result AskerdwayApi_GetConfig(askerdway_config_v1* config) noexcept {
    const auto validation = validate_api_structure(config);
    if (validation != ASKERDWAY_OK)
        return validation;

    askerdway_config_v1 snapshot{};
    snapshot.update_mask = ASKERDWAY_CONFIG_ALL;
    snapshot.armlet_enabled = g_monitor.abuse_enabled.load(std::memory_order_relaxed);
    snapshot.armlet_hotkey = g_monitor.abuse_hotkey.load(std::memory_order_relaxed);
    snapshot.safety_margin_hp = g_monitor.safety_margin.load(std::memory_order_relaxed);
    snapshot.allow_dangerous_abuse =
        g_monitor.allow_high_risk_abuse.load(std::memory_order_relaxed);
    snapshot.allow_local_only_fallback =
        g_monitor.allow_local_only_fallback.load(std::memory_order_relaxed);
    snapshot.use_instant_heals =
        g_monitor.use_instant_heals.load(std::memory_order_relaxed);
    snapshot.instant_heal_mask =
        g_monitor.instant_heal_mask.load(std::memory_order_relaxed);
    snapshot.debug_logging = g_monitor.debug_logging.load(std::memory_order_relaxed);
    snapshot.render_particles_through_fog = render_teleportation::is_enabled();
    snapshot.hero_particle_trails =
        g_render_trace_icons.load(std::memory_order_relaxed);
    snapshot.trace_lifetime_seconds = static_cast<std::uint32_t>(
        g_render_trace_lifetime_seconds.load(std::memory_order_relaxed));
    snapshot.teleport_arrows =
        g_render_teleport_arrows.load(std::memory_order_relaxed);
    snapshot.creep_tracker = g_render_creep_tracker.load(std::memory_order_relaxed);
    snapshot.neutral_camp_status =
        g_render_camp_status.load(std::memory_order_relaxed);
    snapshot.minimap_on_right =
        g_render_minimap_right.load(std::memory_order_relaxed);
    snapshot.diagnostic_draw_bypass = world_frame::is_draw_bypassed() ? 1 : 0;
    *config = snapshot;
    return ASKERDWAY_OK;
}

/**
 * @brief Проверяет маску/диапазоны до применения и изменяет только выбранные update_mask поля;
 * setters обслуживают зависимые буферы.
 */
askerdway_result AskerdwayApi_SetConfig(
    const askerdway_config_v1* config) noexcept {
    const auto validation = validate_api_structure(config);
    if (validation != ASKERDWAY_OK)
        return validation;
    if (!g_running.load(std::memory_order_acquire))
        return ASKERDWAY_SHUTTING_DOWN;
    if ((config->update_mask & ~(ASKERDWAY_CONFIG_ALL | ASKERDWAY_CONFIG_DRAW_BYPASS | ASKERDWAY_CONFIG_CREEP_TRACKER)) != 0)
        return ASKERDWAY_INVALID_ARGUMENT;

    const auto boolean_is_invalid = [&](std::uint64_t bit, std::uint32_t value) {
        return (config->update_mask & bit) != 0 && !api_boolean_is_valid(value);
    };
    if (boolean_is_invalid(ASKERDWAY_CONFIG_ARMLET_ENABLED,
            config->armlet_enabled) ||
        boolean_is_invalid(ASKERDWAY_CONFIG_DANGEROUS_ABUSE,
            config->allow_dangerous_abuse) ||
        boolean_is_invalid(ASKERDWAY_CONFIG_LOCAL_FALLBACK,
            config->allow_local_only_fallback) ||
        boolean_is_invalid(ASKERDWAY_CONFIG_INSTANT_HEALS,
            config->use_instant_heals) ||
        boolean_is_invalid(ASKERDWAY_CONFIG_DEBUG_LOGGING,
            config->debug_logging) ||
        boolean_is_invalid(ASKERDWAY_CONFIG_PARTICLE_FOW,
            config->render_particles_through_fog) ||
        boolean_is_invalid(ASKERDWAY_CONFIG_HERO_TRAILS,
            config->hero_particle_trails) ||
        boolean_is_invalid(ASKERDWAY_CONFIG_TELEPORT_ARROWS,
            config->teleport_arrows) ||
        boolean_is_invalid(ASKERDWAY_CONFIG_CAMP_STATUS,
            config->neutral_camp_status) ||
        boolean_is_invalid(ASKERDWAY_CONFIG_MINIMAP_RIGHT,
            config->minimap_on_right) ||
        boolean_is_invalid(ASKERDWAY_CONFIG_CREEP_TRACKER, config->creep_tracker) ||
        boolean_is_invalid(ASKERDWAY_CONFIG_DRAW_BYPASS,
            config->diagnostic_draw_bypass))
        return ASKERDWAY_VALUE_OUT_OF_RANGE;

    if ((config->update_mask & ASKERDWAY_CONFIG_ARMLET_HOTKEY) != 0 &&
        (config->armlet_hotkey < 0 || config->armlet_hotkey > 0xFF ||
            (config->armlet_hotkey != 0 && config->armlet_hotkey == comfort_runtime::silence_key.load())))
        return ASKERDWAY_VALUE_OUT_OF_RANGE;
    if ((config->update_mask & ASKERDWAY_CONFIG_SAFETY_MARGIN) != 0 &&
        (!std::isfinite(config->safety_margin_hp) ||
            config->safety_margin_hp < 0.0f || config->safety_margin_hp > 500.0f))
        return ASKERDWAY_VALUE_OUT_OF_RANGE;
    if ((config->update_mask & ASKERDWAY_CONFIG_TRACE_LIFETIME) != 0 &&
        (config->trace_lifetime_seconds < 3 || config->trace_lifetime_seconds > 30))
        return ASKERDWAY_VALUE_OUT_OF_RANGE;
    if ((config->update_mask & ASKERDWAY_CONFIG_INSTANT_HEAL_MASK) != 0 &&
        (config->instant_heal_mask & ~ASKERDWAY_HEAL_ALL) != 0)
        return ASKERDWAY_VALUE_OUT_OF_RANGE;

    // The particle subsystem has an independent unload guard. Probe/apply it
    // before changing any other field so a teardown race cannot produce a
    // partially applied request followed by ASKERDWAY_NOT_READY.
    if ((config->update_mask & ASKERDWAY_CONFIG_PARTICLE_FOW) != 0 &&
        !set_particle_visibility_enabled(
            config->render_particles_through_fog != 0))
        return ASKERDWAY_NOT_READY;

    // Validation is intentionally complete before the first write: an invalid
    // request cannot leave half of a configuration applied.
    if ((config->update_mask & ASKERDWAY_CONFIG_ARMLET_ENABLED) != 0)
        set_abuse_enabled(config->armlet_enabled != 0, "public API");
    if ((config->update_mask & ASKERDWAY_CONFIG_ARMLET_HOTKEY) != 0)
        g_monitor.abuse_hotkey.store(config->armlet_hotkey, std::memory_order_relaxed);
    if ((config->update_mask & ASKERDWAY_CONFIG_SAFETY_MARGIN) != 0)
        g_monitor.safety_margin.store(config->safety_margin_hp,
            std::memory_order_relaxed);
    if ((config->update_mask & ASKERDWAY_CONFIG_DANGEROUS_ABUSE) != 0)
        g_monitor.allow_high_risk_abuse.store(
            config->allow_dangerous_abuse != 0, std::memory_order_relaxed);
    if ((config->update_mask & ASKERDWAY_CONFIG_LOCAL_FALLBACK) != 0)
        g_monitor.allow_local_only_fallback.store(
            config->allow_local_only_fallback != 0, std::memory_order_relaxed);
    if ((config->update_mask & ASKERDWAY_CONFIG_INSTANT_HEALS) != 0)
        g_monitor.use_instant_heals.store(config->use_instant_heals != 0,
            std::memory_order_relaxed);
    if ((config->update_mask & ASKERDWAY_CONFIG_INSTANT_HEAL_MASK) != 0)
        g_monitor.instant_heal_mask.store(config->instant_heal_mask,
            std::memory_order_relaxed);
    if ((config->update_mask & ASKERDWAY_CONFIG_DEBUG_LOGGING) != 0)
        g_monitor.debug_logging.store(config->debug_logging != 0,
            std::memory_order_relaxed);
    if ((config->update_mask & ASKERDWAY_CONFIG_HERO_TRAILS) != 0)
        set_hero_trails_enabled(config->hero_particle_trails != 0);
    if ((config->update_mask & ASKERDWAY_CONFIG_TRACE_LIFETIME) != 0)
        set_trace_lifetime_seconds(static_cast<int>(config->trace_lifetime_seconds));
    if ((config->update_mask & ASKERDWAY_CONFIG_TELEPORT_ARROWS) != 0)
        set_teleport_arrows_enabled(config->teleport_arrows != 0);
    if ((config->update_mask & ASKERDWAY_CONFIG_CAMP_STATUS) != 0)
        set_camp_status_enabled(config->neutral_camp_status != 0);
    if ((config->update_mask & ASKERDWAY_CONFIG_MINIMAP_RIGHT) != 0)
        set_minimap_side_right(config->minimap_on_right != 0);
    if ((config->update_mask & ASKERDWAY_CONFIG_CREEP_TRACKER) != 0)
        set_creep_tracker_enabled(config->creep_tracker != 0);
    if ((config->update_mask & ASKERDWAY_CONFIG_DRAW_BYPASS) != 0)
        world_frame::set_draw_bypass(config->diagnostic_draw_bypass != 0);
    return ASKERDWAY_OK;
}

/**
 * @brief Возвращает текущее состояние диагностического байпаса отрисовки world_frame.
 */
std::uint32_t AskerdwayApi_GetDiagnosticDrawBypass() noexcept {
    return world_frame::is_draw_bypassed() ? 1 : 0;
}

/**
 * @brief Управляет диагностическим байпасом отрисовки world_frame без изменения остальных подсистем.
 */
askerdway_result AskerdwayApi_SetDiagnosticDrawBypass(
    std::uint32_t bypass) noexcept {
    if (!api_boolean_is_valid(bypass))
        return ASKERDWAY_VALUE_OUT_OF_RANGE;
    if (!g_running.load(std::memory_order_acquire))
        return ASKERDWAY_SHUTTING_DOWN;
    world_frame::set_draw_bypass(bypass != 0);
    return ASKERDWAY_OK;
}


namespace {

void populate_render_snapshot(askerdway_render_snapshot_v2& snapshot) noexcept {
    snapshot = askerdway_render_snapshot_v2{};
    snapshot.captured_at_ms = GetTickCount64();
    snapshot.hook_calls = render_teleportation::hook_call_count();
    snapshot.fow_hidden_calls = render_teleportation::fow_hidden_effect_count();
    snapshot.forced_updates = render_teleportation::updated_effect_count();
    const auto frame = world_frame::status();
    snapshot.world_marker_frames = frame.drawn_frames;
    snapshot.game_present_calls = frame.presents;
    snapshot.frame_render_us = frame.render_us;
    snapshot.frame_visible_markers = frame.visible;
    snapshot.frame_hook_installed = frame.installed;
    snapshot.frame_reentrant_calls = frame.reentrant_calls;
    snapshot.frame_object_hook = frame.object_hook;
    snapshot.game_present_us = frame.game_present_us;
    snapshot.game_present_peak_us = frame.game_present_peak_us;
    snapshot.draw_bypassed = frame.draw_bypassed ? 1 : 0;
    const auto intervals=world_frame::read_frame_sample();
    snapshot.reserved[0]=intervals.count;snapshot.reserved[1]=intervals.p50_us;
    snapshot.reserved[2]=intervals.p95_us;snapshot.reserved[3]=intervals.p99_us;snapshot.reserved[4]=intervals.maximum_us;
    const auto clock_client = g_client_base_address.load(std::memory_order_acquire);
    read_game_time(clock_client, snapshot.game_time);
    bool clock_paused = false;
    if (read_game_paused(clock_client, clock_paused)) snapshot.game_paused = clock_paused ? 1 : 0;
    snapshot.scan_us = g_render_diagnostics.intelligence_poll_us.load(std::memory_order_relaxed);
    snapshot.render_us = g_render_diagnostics.native_render_us.load(std::memory_order_relaxed);
    render_teleportation::particle_observation observations[32]{};
    snapshot.particle_count = static_cast<std::uint32_t>(
        render_teleportation::read_recent_observations(observations, 32));
    for (std::size_t i = 0; i < snapshot.particle_count; ++i) {
        const auto& p = observations[i];
        auto& result = snapshot.particles[i];
        result.effect = p.effect; result.collection = p.collection;
        result.observed_at = p.observed_at_ms; result.owner = p.owner_handle;
        result.flags = p.flags; result.visibility = p.visibility;
        result.position[0] = p.x; result.position[1] = p.y; result.position[2] = p.z;
        result.source_position[0] = p.source_x; result.source_position[1] = p.source_y; result.source_position[2] = p.source_z;
        copy_designer_name(result.name, p.debug_name);
    }
    render_intelligence_shared_guard guard;
    snapshot.trace_count = static_cast<std::uint32_t>(g_render_intelligence.trace_count);
    snapshot.route_count = static_cast<std::uint32_t>(g_render_intelligence.teleport_count);
    snapshot.enemy_count = static_cast<std::uint32_t>(g_render_intelligence.enemy_visibility_count);
    snapshot.scroll_count = static_cast<std::uint32_t>(g_render_intelligence.enemy_teleport_count);
    for (std::size_t i = 0; i < snapshot.trace_count; ++i) {
        const auto& trace = g_render_intelligence.traces[i];
        auto& result = snapshot.traces[i];
        result.owner = trace.owner_handle;
        std::memcpy(result.position, &trace.position, sizeof(result.position));
        result.created_at_ms = trace.created_at;
        result.expires_at_ms = trace.expires_at;
        copy_designer_name(result.hero, trace.designer_name);
        copy_detector_trigger(result.trigger, trace.detector_trigger);
        copy_designer_name(result.ability, trace.ability_icon);
    }
    for (std::size_t i = 0; i < snapshot.route_count; ++i) {
        const auto& route = g_render_intelligence.teleports[i];
        auto& result = snapshot.routes[i];
        result.owner = route.owner_handle;
        std::memcpy(result.origin, &route.origin, sizeof(result.origin));
        std::memcpy(result.destination, &route.destination, sizeof(result.destination));
        result.has_origin = route.has_origin;
        result.has_destination = route.has_destination;
        result.observed = route.channel_observed;
        result.finished = route.channel_finished;
        result.canceled = route.canceled;
        result.expires_at_ms = route.expires_at;
    }
    for (std::size_t i = 0; i < snapshot.enemy_count; ++i) {
        const auto& enemy = g_render_intelligence.enemy_visibility[i];
        auto& result = snapshot.enemies[i];
        result.address = enemy.entity_address;
        result.owner = enemy.entity_index;
        result.ability_count = static_cast<std::uint32_t>(enemy.ability_count);
        result.hidden = enemy.dormant;
        copy_designer_name(result.hero, enemy.designer_name);
    }
    for (std::size_t i = 0; i < snapshot.scroll_count; ++i) {
        const auto& scroll = g_render_intelligence.enemy_teleports[i];
        snapshot.scrolls[i] = {scroll.player_id, scroll.charges, scroll.team,
            scroll.inventory_known, scroll.has_scroll, scroll.cooldown_seconds};
    }
}

}  // namespace

/**
 * @brief Возвращает агрегированную диагностику runtime в заранее инициализированную
 * v1-структуру; ASKERDWAY_OK не означает, что автоматика сейчас разрешена.
 */
askerdway_result AskerdwayApi_GetRenderSnapshot(
    askerdway_render_snapshot_v1* output) noexcept {
    const auto validation = validate_api_structure(output);
    if (validation != ASKERDWAY_OK) return validation;
    askerdway_render_snapshot_v2 full{};
    populate_render_snapshot(full);
    const std::uint32_t user_size = output->struct_size;
    full.struct_size = user_size;
    const std::size_t copy_size = (user_size >= sizeof(askerdway_render_snapshot_v2))
        ? sizeof(askerdway_render_snapshot_v2)
        : sizeof(askerdway_render_snapshot_v1);
    std::memcpy(output, &full, copy_size);
    return ASKERDWAY_OK;
}

/**
 * @brief Возвращает расширенную диагностику runtime v2, включая телеметрию Present и байпас.
 */
askerdway_result AskerdwayApi_GetRenderSnapshotV2(
    askerdway_render_snapshot_v2* output) noexcept {
    const auto validation = validate_api_structure(output);
    if (validation != ASKERDWAY_OK) return validation;
    const std::uint32_t user_size = output->struct_size;
    populate_render_snapshot(*output);
    output->struct_size = user_size;
    return ASKERDWAY_OK;
}


askerdway_result AskerdwayApi_GetStatus(askerdway_status_v1* status) noexcept {
    const auto validation = validate_api_structure(status);
    if (validation != ASKERDWAY_OK)
        return validation;

    askerdway_status_v1 snapshot{};
    snapshot.capabilities = askerdway_capabilities;
    snapshot.state_flags = collect_public_state_flags();
    snapshot.armlet_state = public_armlet_state(
        g_monitor.status.load(std::memory_order_relaxed));
    snapshot.cycle_state = public_cycle_state(
        g_monitor.cycle.load(std::memory_order_relaxed));
    snapshot.safety_state = public_safety_state(
        g_monitor.safety.load(std::memory_order_relaxed));
    snapshot.health = g_monitor.health.load(std::memory_order_relaxed);
    snapshot.max_health = g_monitor.max_health.load(std::memory_order_relaxed);
    snapshot.armlet_entity_index =
        g_monitor.armlet_entity_index.load(std::memory_order_relaxed);
    snapshot.confirmation_latency_ms =
        g_monitor.confirmation_latency_ms.load(std::memory_order_relaxed);
    snapshot.expected_cycle_ms =
        g_monitor.expected_cycle_ms.load(std::memory_order_relaxed);
    snapshot.safety_margin_hp = g_monitor.safety_margin.load(std::memory_order_relaxed);
    snapshot.network_latency_ms =
        g_monitor.network_latency_ms.load(std::memory_order_relaxed);
    snapshot.incoming_damage = g_monitor.incoming_damage.load(std::memory_order_relaxed);
    snapshot.nearest_impact_ms =
        g_monitor.nearest_impact_ms.load(std::memory_order_relaxed);
    snapshot.predicted_minimum_hp =
        g_monitor.predicted_minimum_hp.load(std::memory_order_relaxed);
    snapshot.high_risk_interval_ms =
        g_monitor.high_risk_interval_ms.load(std::memory_order_relaxed);
    snapshot.incoming_event_count =
        g_monitor.incoming_count.load(std::memory_order_relaxed);
    snapshot.tracking_projectile_count =
        g_monitor.internal_tracking_count.load(std::memory_order_relaxed);
    snapshot.linear_projectile_count =
        g_monitor.internal_linear_count.load(std::memory_order_relaxed);
    snapshot.tower_projectile_count =
        g_monitor.internal_tower_count.load(std::memory_order_relaxed);
    snapshot.melee_windup_count =
        g_monitor.internal_melee_count.load(std::memory_order_relaxed);
    snapshot.modifier_tick_count =
        g_monitor.modifier_tick_count.load(std::memory_order_relaxed);
    snapshot.available_heal_amount =
        g_monitor.instant_heal_available_amount.load(std::memory_order_relaxed);
    snapshot.available_heal_charges =
        g_monitor.instant_heal_available_charges.load(std::memory_order_relaxed);
    snapshot.planned_heal_amount =
        g_monitor.instant_heal_planned_amount.load(std::memory_order_relaxed);
    snapshot.orders_queued = g_monitor.orders_queued.load(std::memory_order_relaxed);
    snapshot.orders_dispatched =
        g_monitor.orders_dispatched.load(std::memory_order_relaxed);
    snapshot.orders_confirmed =
        g_monitor.orders_confirmed.load(std::memory_order_relaxed);
    snapshot.orders_timed_out =
        g_monitor.orders_timed_out.load(std::memory_order_relaxed);
    snapshot.state_transitions =
        g_monitor.transitions.load(std::memory_order_relaxed);
    snapshot.instant_heal_uses =
        g_monitor.instant_heal_uses.load(std::memory_order_relaxed);
    snapshot.particle_updates_forced = render_teleportation::updated_effect_count();
    snapshot.active_hero_traces =
        g_render_trace_count_snapshot.load(std::memory_order_acquire);
    snapshot.active_teleport_arrows =
        g_render_teleport_count_snapshot.load(std::memory_order_acquire);
    snapshot.neutral_camps =
        g_render_camp_count_snapshot.load(std::memory_order_acquire);
    snapshot.schema_fields_resolved = static_cast<std::uint32_t>(std::max(0,
        g_monitor.schema_fields_resolved.load(std::memory_order_relaxed)));
    snapshot.schema_fields_required = static_cast<std::uint32_t>(std::max(0,
        g_monitor.schema_fields_required.load(std::memory_order_relaxed)));
    snapshot.reserved_count = ASKERDWAY_DIAGNOSTIC_SLOT_COUNT;
    snapshot.reserved[ASKERDWAY_DIAG_PARTICLE_OBSERVATIONS] =
        g_render_diagnostics.particle_observations.load(std::memory_order_relaxed);
    snapshot.reserved[ASKERDWAY_DIAG_RESOLVED_PARTICLE_OWNERS] =
        g_render_diagnostics.resolved_particle_owners.load(
            std::memory_order_relaxed);
    snapshot.reserved[ASKERDWAY_DIAG_FOG_ABILITY_EVENTS] =
        g_render_diagnostics.fog_ability_events.load(std::memory_order_relaxed);
    snapshot.reserved[ASKERDWAY_DIAG_FOG_FARMING_EVENTS] =
        g_render_diagnostics.fog_farming_events.load(std::memory_order_relaxed);
    snapshot.reserved[ASKERDWAY_DIAG_COMPLETE_TELEPORT_ROUTES] =
        g_render_diagnostics.teleport_routes.load(std::memory_order_relaxed);
    snapshot.reserved[ASKERDWAY_DIAG_TOWER_SHOTS] =
        g_render_diagnostics.tower_shots.load(std::memory_order_relaxed);
    snapshot.reserved[ASKERDWAY_DIAG_NATIVE_TIMINGS_US] =
        static_cast<std::uint64_t>(g_render_diagnostics.intelligence_poll_us.load(
            std::memory_order_relaxed)) |
        (static_cast<std::uint64_t>(g_render_diagnostics.native_render_us.load(
            std::memory_order_relaxed)) << 32);
    snapshot.reserved[ASKERDWAY_DIAG_VIEW_MATRIX_ADDRESS] =
        g_view_matrix_address.load(std::memory_order_acquire);
    snapshot.hero_address = g_monitor.hero_address.load(std::memory_order_relaxed);
    snapshot.armlet_address = g_monitor.armlet_address.load(std::memory_order_relaxed);
    snapshot.modifier_address =
        g_monitor.modifier_address.load(std::memory_order_relaxed);
    *status = snapshot;
    return ASKERDWAY_OK;
}

/**
 * @brief Заполняет versioned сведения о сборке; строковые поля принадлежат выходной структуре.
 */
askerdway_result AskerdwayApi_GetBuildInfo(
    askerdway_build_info_v1* info) noexcept {
    const auto validation = validate_api_structure(info);
    if (validation != ASKERDWAY_OK)
        return validation;
    askerdway_build_info_v1 snapshot{};
    std::snprintf(snapshot.product, sizeof(snapshot.product), "askerdway");
    std::snprintf(snapshot.build_date, sizeof(snapshot.build_date), "%s", __DATE__);
    std::snprintf(snapshot.build_time, sizeof(snapshot.build_time), "%s", __TIME__);
#if defined(__clang__)
    std::snprintf(snapshot.compiler, sizeof(snapshot.compiler), "Clang %d.%d",
        __clang_major__, __clang_minor__);
#elif defined(__GNUC__)
    std::snprintf(snapshot.compiler, sizeof(snapshot.compiler), "GCC %d.%d",
        __GNUC__, __GNUC_MINOR__);
#elif defined(_MSC_VER)
    std::snprintf(snapshot.compiler, sizeof(snapshot.compiler), "MSVC %d", _MSC_VER);
#else
    std::snprintf(snapshot.compiler, sizeof(snapshot.compiler), "unknown");
#endif
    std::snprintf(snapshot.compatibility_note,
        sizeof(snapshot.compatibility_note),
        "ABI 1; check SchemaSnapshot after every Dota client update");
    *info = snapshot;
    return ASKERDWAY_OK;
}

/**
 * @brief Копирует опубликованные данные героя в POD-снимок; сохранённый адрес нельзя считать
 * долговечным указателем.
 */
askerdway_result AskerdwayApi_GetHeroSnapshot(
    askerdway_hero_snapshot* hero) noexcept {
    const auto validation = validate_api_structure(hero);
    if (validation != ASKERDWAY_OK)
        return validation;
    askerdway_hero_snapshot snapshot{};
    snapshot.abi_version = ASKERDWAY_ABI_VERSION;
    const auto address = g_monitor.hero_address.load(std::memory_order_relaxed);
    snapshot.entity.address = address;
    snapshot.health = g_monitor.health.load(std::memory_order_relaxed);
    snapshot.max_health = g_monitor.max_health.load(std::memory_order_relaxed);
    if (!plausible_pointer(address)) {
        *hero = snapshot;
        return ASKERDWAY_NOT_READY;
    }
    if (!g_monitor.schema_ready.load(std::memory_order_acquire)) {
        *hero = snapshot;
        return ASKERDWAY_NOT_READY;
    }
    read_world_position(address, snapshot.position);
    if (schema_fields::m_vecAbsVelocity > 0)
        safe_read(address + schema_fields::m_vecAbsVelocity, snapshot.velocity);
    if (schema_fields::m_iTeamNum > 0)
        safe_read(address + schema_fields::m_iTeamNum, snapshot.entity.team);
    if (schema_fields::m_flMana > 0)
        safe_read(address + schema_fields::m_flMana, snapshot.mana);
    if (schema_fields::m_flPhysicalArmorValue > 0)
        safe_read(address + schema_fields::m_flPhysicalArmorValue,
            snapshot.physical_armor);
    if (schema_fields::m_flMagicalResistanceValue > 0)
        safe_read(address + schema_fields::m_flMagicalResistanceValue,
            snapshot.magical_resistance);
    if (schema_fields::m_nUnitState64 > 0)
        safe_read(address + schema_fields::m_nUnitState64, snapshot.unit_state);
    char name[96]{};
    if (read_designer_name(address, name))
        std::snprintf(snapshot.designer_name, sizeof(snapshot.designer_name),
            "%s", name);
    *hero = snapshot;
    return ASKERDWAY_OK;
}

/**
 * @brief Копирует наблюдаемое состояние предмета и подтверждений; отправленный приказ и
 * применённое состояние различаются.
 */
askerdway_result AskerdwayApi_GetArmletSnapshot(
    askerdway_armlet_snapshot* armlet) noexcept {
    const auto validation = validate_api_structure(armlet);
    if (validation != ASKERDWAY_OK)
        return validation;
    askerdway_armlet_snapshot snapshot{};
    snapshot.abi_version = ASKERDWAY_ABI_VERSION;
    snapshot.item.address = g_monitor.armlet_address.load(std::memory_order_relaxed);
    snapshot.item.index = g_monitor.armlet_entity_index.load(std::memory_order_relaxed);
    snapshot.modifier_address =
        g_monitor.modifier_address.load(std::memory_order_relaxed);
    snapshot.found = g_monitor.item_found.load(std::memory_order_relaxed);
    snapshot.toggled_on = g_monitor.toggle_state.load(std::memory_order_relaxed);
    snapshot.modifier_confirmed =
        g_monitor.effect_confirmed.load(std::memory_order_relaxed);
    snapshot.cooldown_seconds =
        g_monitor.toggle_cooldown.load(std::memory_order_relaxed);
    snapshot.bonus_strength = g_monitor.bonus_strength.load(std::memory_order_relaxed);
    snapshot.bonus_damage = g_monitor.bonus_damage.load(std::memory_order_relaxed);
    snapshot.bonus_attack_speed =
        g_monitor.bonus_attack_speed.load(std::memory_order_relaxed);
    snapshot.bonus_armor = g_monitor.bonus_armor.load(std::memory_order_relaxed);
    snapshot.health_drain_per_second =
        g_monitor.health_drain_per_second.load(std::memory_order_relaxed);
    snapshot.drain_tick_interval =
        g_monitor.tick_interval.load(std::memory_order_relaxed);
    bool enabled = false;
    if (snapshot.item.address != 0 && schema_fields::m_bItemEnabled > 0 &&
        safe_read(snapshot.item.address + schema_fields::m_bItemEnabled, enabled))
        snapshot.item_enabled = enabled;
    *armlet = snapshot;
    return snapshot.found ? ASKERDWAY_OK : ASKERDWAY_NOT_READY;
}

/**
 * @brief Заполняет versioned снимок разрешённых смещений для диагностики текущего клиента.
 */
askerdway_result AskerdwayApi_GetSchemaSnapshot(
    askerdway_schema_snapshot_v1* schema) noexcept {
    const auto validation = validate_api_structure(schema);
    if (validation != ASKERDWAY_OK)
        return validation;
    askerdway_schema_snapshot_v1 snapshot{};
    snapshot.resolved_required = static_cast<std::uint32_t>(std::max(0,
        g_monitor.schema_fields_resolved.load(std::memory_order_relaxed)));
    snapshot.required_total = static_cast<std::uint32_t>(std::max(0,
        g_monitor.schema_fields_required.load(std::memory_order_relaxed)));
    snapshot.entity_health = schema_fields::m_iHealth;
    snapshot.entity_max_health = schema_fields::m_iMaxHealth;
    snapshot.entity_identity = schema_fields::m_pEntityIdentity;
    snapshot.identity_designer_name = schema_fields::m_designerName;
    snapshot.entity_scene_node = schema_fields::m_pGameSceneNode;
    snapshot.entity_team = schema_fields::m_iTeamNum;
    snapshot.entity_owner_handle = schema_fields::m_hOwnerEntity;
    snapshot.entity_velocity = schema_fields::m_vecAbsVelocity;
    snapshot.scene_node_origin = schema_fields::m_vecAbsOrigin;
    snapshot.npc_modifier_manager = schema_fields::m_ModifierManager;
    snapshot.npc_inventory = schema_fields::m_Inventory;
    snapshot.npc_player_owner_id = schema_fields::m_nPlayerOwnerID;
    snapshot.npc_hull_radius = schema_fields::m_flHullRadius;
    snapshot.npc_physical_armor = schema_fields::m_flPhysicalArmorValue;
    snapshot.npc_magical_resistance = schema_fields::m_flMagicalResistanceValue;
    snapshot.npc_mana = schema_fields::m_flMana;
    snapshot.npc_unit_state = schema_fields::m_nUnitState64;
    snapshot.controller_player_id = schema_fields::controller_m_nPlayerID;
    snapshot.controller_assigned_hero = schema_fields::controller_m_hAssignedHero;
    snapshot.ability_toggle_state = schema_fields::m_bToggleState;
    snapshot.ability_cooldown = schema_fields::m_fCooldown;
    snapshot.inventory_items = schema_fields::m_hItems;
    snapshot.item_charges = schema_fields::m_iCurrentCharges;
    snapshot.item_enabled = schema_fields::m_bItemEnabled;
    snapshot.neutral_spawner_type = schema_fields::neutral_spawner_m_Type;
    snapshot.reserved[0] = schema_fields::game_rules_m_NeutralSpawnBoxes;
    snapshot.reserved[1] = schema_fields::physical_item_m_hItem;
    snapshot.reserved[2] = g_modifier_count_offset.load(std::memory_order_acquire);
    snapshot.reserved[3] = g_modifier_data_offset.load(std::memory_order_acquire);
    snapshot.reserved[4] = schema_fields::m_vecAbilities;
    snapshot.reserved[5] = schema_fields::m_bInAbilityPhase;
    snapshot.reserved[6] = schema_fields::m_flCastStartTime;
    snapshot.reserved[7] = schema_fields::m_flChannelStartTime;
    snapshot.reserved[8] = schema_fields::m_flLastCastClickTime;
    snapshot.reserved[9] = schema_fields::game_rules_m_flCreepSpawntime;
    snapshot.reserved[10] = schema_fields::game_rules_m_flGameStartTime;
    snapshot.reserved[11] = schema_fields::game_rules_m_bCreepSpawningEnabled;
    snapshot.reserved[12] = schema_fields::npc_m_iMoveSpeed;
    snapshot.reserved[13] = schema_fields::game_rules_m_nGameState;
    snapshot.reserved[14] = schema_fields::m_bNPCVisibleState;
    snapshot.reserved[15] = schema_fields::m_iTeamVisibilityBitmask;
    *schema = snapshot;
    return g_monitor.schema_ready.load(std::memory_order_acquire)
        ? ASKERDWAY_OK : ASKERDWAY_NOT_READY;
}

/**
 * @brief Возвращает versioned снимок адресов/калибровки текущего процесса; не сохраняйте
 * адреса для следующего запуска игры.
 */
askerdway_result AskerdwayApi_GetRuntimeGlobals(
    askerdway_runtime_globals_v1* globals) noexcept {
    const auto validation = validate_api_structure(globals);
    if (validation != ASKERDWAY_OK)
        return validation;
    askerdway_runtime_globals_v1 snapshot{};
    snapshot.client_base = g_client_base_address.load(std::memory_order_acquire);
    snapshot.local_controller =
        g_local_controller_address.load(std::memory_order_acquire);
    snapshot.prepare_unit_orders =
        g_prepare_unit_orders.load(std::memory_order_acquire);
    snapshot.entity_chunk_table_offset =
        g_entity_chunk_table_offset.load(std::memory_order_acquire);
    snapshot.entity_entry_stride =
        g_entity_entry_stride.load(std::memory_order_acquire);
    const auto read_indirect = [](const std::atomic<std::uintptr_t>& storage) {
        std::uintptr_t value = 0;
        const auto address = storage.load(std::memory_order_acquire);
        safe_read(address, value);
        return value;
    };
    snapshot.game_entity_system = read_indirect(g_game_entity_system_global);
    snapshot.global_vars = read_indirect(g_global_vars_global);
    snapshot.game_rules = read_indirect(g_game_rules_global);
    snapshot.local_player_pawn = read_indirect(g_local_player_pawn_global);
    snapshot.projectile_manager = read_indirect(g_projectile_manager_global);
    snapshot.reserved[0] = g_world_ui_state_address.load(std::memory_order_acquire);
    *globals = snapshot;
    return snapshot.client_base != 0 ? ASKERDWAY_OK : ASKERDWAY_NOT_READY;
}

/**
 * @brief Разрешает handle и копирует данные ссылки на текущую сущность; результат может
 * устареть после возврата.
 */
askerdway_result AskerdwayApi_ResolveEntityHandle(
    std::uint32_t handle, askerdway_entity_ref* entity) noexcept {
    if (entity == nullptr || handle == 0 || handle == 0xFFFFFFFFu)
        return ASKERDWAY_INVALID_ARGUMENT;
    *entity = {};
    entity->handle = handle;
    entity->index = static_cast<std::int32_t>(handle & 0x7FFFu);

    const auto global = g_game_entity_system_global.load(std::memory_order_acquire);
    std::uintptr_t entity_system = 0;
    if (!safe_read(global, entity_system) || !plausible_pointer(entity_system))
        return ASKERDWAY_NOT_READY;
    const auto address = entity_by_handle(entity_system, handle);
    if (!plausible_pointer(address))
        return ASKERDWAY_NOT_READY;
    entity->address = address;
    if (schema_fields::m_iTeamNum > 0)
        safe_read(address + schema_fields::m_iTeamNum, entity->team);
    return ASKERDWAY_OK;
}

/**
 * @brief Проверяет чтение мировой позиции по адресу сущности; caller обрабатывает ошибку
 * вместо использования прежних координат.
 */
askerdway_result AskerdwayApi_GetEntityWorldPosition(
    std::uint64_t entity_address, askerdway_vec3* position) noexcept {
    if (position == nullptr ||
        !plausible_pointer(static_cast<std::uintptr_t>(entity_address)))
        return ASKERDWAY_INVALID_ARGUMENT;
    *position = {};
    if (!g_monitor.schema_ready.load(std::memory_order_acquire))
        return ASKERDWAY_NOT_READY;
    return read_world_position(static_cast<std::uintptr_t>(entity_address), *position)
        ? ASKERDWAY_OK : ASKERDWAY_NOT_READY;
}

/**
 * @brief Читает ring buffer по изменяемому cursor; written обязателен, output может быть
 * nullptr только при capacity == 0.
 */
askerdway_result AskerdwayApi_ReadParticleObservations(
    std::uint64_t* cursor, askerdway_particle_snapshot* output,
    std::uint32_t capacity, std::uint32_t* written) noexcept {
    if (cursor == nullptr || written == nullptr ||
        (capacity != 0 && output == nullptr))
        return ASKERDWAY_INVALID_ARGUMENT;
    *written = 0;
    if (capacity == 0)
        return ASKERDWAY_OK;
    if (capacity > 512)
        return ASKERDWAY_VALUE_OUT_OF_RANGE;

    std::array<render_teleportation::particle_observation, 512> internal{};
    const std::size_t count = render_teleportation::read_observations(
        *cursor, internal.data(), capacity);
    for (std::size_t index = 0; index < count; ++index) {
        output[index].sequence = internal[index].sequence;
        output[index].owner_handle = internal[index].owner_handle;
        output[index].flags = internal[index].flags;
        output[index].position = {
            internal[index].x, internal[index].y, internal[index].z};
        output[index].observed_at_ms = internal[index].observed_at_ms;
    }
    *written = static_cast<std::uint32_t>(count);
    return ASKERDWAY_OK;
}

/**
 * @brief Копирует активные события под projectile shared-lock до capacity записей; written
 * сообщает фактическое число.
 */
askerdway_result AskerdwayApi_ReadProjectiles(
    askerdway_projectile_snapshot* output, std::uint32_t capacity,
    std::uint32_t* written) noexcept {
    if (written == nullptr || (capacity != 0 && output == nullptr))
        return ASKERDWAY_INVALID_ARGUMENT;
    *written = 0;
    if (capacity == 0)
        return ASKERDWAY_OK;

    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_projectiles.lock);
    std::uint32_t count = 0;
    for (const auto& incoming : g_projectiles.entries) {
        if (!incoming.active || count >= capacity)
            continue;
        auto& snapshot = output[count++];
        snapshot = {};
        snapshot.id = incoming.projectile_id;
        snapshot.kind = static_cast<std::uint32_t>(incoming.kind);
        snapshot.source_handle = incoming.source_handle;
        snapshot.target_handle = incoming.target_handle;
        if (incoming.position_valid)
            snapshot.flags |= ASKERDWAY_PROJECTILE_POSITION_VALID;
        if (incoming.damage_complete)
            snapshot.flags |= ASKERDWAY_PROJECTILE_DAMAGE_COMPLETE;
        if (incoming.is_attack)
            snapshot.flags |= ASKERDWAY_PROJECTILE_IS_ATTACK;
        if (incoming.ranged_attack)
            snapshot.flags |= ASKERDWAY_PROJECTILE_RANGED_ATTACK;
        if (incoming.use_projectile)
            snapshot.flags |= ASKERDWAY_PROJECTILE_USES_PROJECTILE;
        snapshot.source_entity = incoming.source_entity;
        snapshot.position = incoming.position;
        snapshot.velocity = incoming.velocity;
        snapshot.speed = incoming.speed;
        snapshot.radius = incoming.radius;
        snapshot.remaining_distance = incoming.remaining_distance;
        snapshot.impact_in_ms = incoming.impact_at == 0 ? -1.0f :
            static_cast<float>(incoming.impact_at > now ? incoming.impact_at - now : 0);
        snapshot.damage = {incoming.original_damage, incoming.physical_damage,
            incoming.magical_damage, incoming.pure_damage,
            incoming.composite_damage};
        snapshot.confidence = incoming.confidence;
    }
    ReleaseSRWLockShared(&g_projectiles.lock);
    *written = count;
    return ASKERDWAY_OK;
}

/**
 * @brief Копирует trace-массив под render shared-lock; output == nullptr допустим только при
 * capacity == 0.
 */
askerdway_result AskerdwayApi_ReadHeroTraces(
    askerdway_hero_trace_snapshot* output, std::uint32_t capacity,
    std::uint32_t* written) noexcept {
    if (written == nullptr || (capacity != 0 && output == nullptr))
        return ASKERDWAY_INVALID_ARGUMENT;
    *written = 0;
    render_intelligence_shared_guard read_guard;
    const std::uint32_t count = std::min(capacity,
        static_cast<std::uint32_t>(g_render_intelligence.trace_count));
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& source = g_render_intelligence.traces[index];
        output[index] = {};
        output[index].owner_handle = source.owner_handle;
        hero_abbreviation(source.designer_name, output[index].abbreviation);
        output[index].position = source.position;
        output[index].expires_at_ms = source.expires_at;
    }
    *written = count;
    return ASKERDWAY_OK;
}

/**
 * @brief Копирует внутренние маршруты, включая неполные; complete означает наличие обоих
 * endpoints, а не завершение каста.
 */
askerdway_result AskerdwayApi_ReadTeleportArrows(
    askerdway_teleport_arrow_snapshot* output, std::uint32_t capacity,
    std::uint32_t* written) noexcept {
    if (written == nullptr || (capacity != 0 && output == nullptr))
        return ASKERDWAY_INVALID_ARGUMENT;
    *written = 0;
    render_intelligence_shared_guard read_guard;
    const auto now = GetTickCount64();
    std::uint32_t count = 0;
    for (std::uint32_t index = 0; index < g_render_intelligence.teleport_count && count < capacity; ++index) {
        const auto& source = g_render_intelligence.teleports[index];
        if (source.canceled || source.expires_at <= now) continue;
        auto& target = output[count++];
        target = {};
        target.owner_handle = source.owner_handle;
        target.complete = source.has_origin && source.has_destination;
        target.origin = source.origin;
        target.destination = source.destination;
        target.first_seen_at_ms = source.first_seen_at;
        target.expires_at_ms = source.expires_at;
    }
    *written = count;
    return ASKERDWAY_OK;
}

/**
 * @brief Копирует записи лагерей под render shared-lock и публикует размер области блокировки.
 */
askerdway_result AskerdwayApi_ReadNeutralCamps(
    askerdway_neutral_camp_snapshot* output, std::uint32_t capacity,
    std::uint32_t* written) noexcept {
    if (written == nullptr || (capacity != 0 && output == nullptr))
        return ASKERDWAY_INVALID_ARGUMENT;
    *written = 0;
    render_intelligence_shared_guard read_guard;
    const std::uint32_t count = std::min(capacity,
        static_cast<std::uint32_t>(g_render_intelligence.camp_count));
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& source = g_render_intelligence.camps[index];
        output[index] = {};
        output[index].position = source.position;
        output[index].type = source.type;
        output[index].blocked = source.blocked;
        output[index].approximated_block_radius = source.has_native_bounds
            ? std::max((source.maximum.x-source.minimum.x)*.5f,(source.maximum.y-source.minimum.y)*.5f)
            : camp_block_radius(source.type);
    }
    *written = count;
    return ASKERDWAY_OK;
}

/**
 * @brief Обновляет heartbeat с измеренной задержкой 0..5000 мс. Не удаляет все ранее
 * переданные события.
 */
askerdway_result AskerdwayApi_BeginTelemetryFrame(
    float network_latency_ms) noexcept {
    if (!std::isfinite(network_latency_ms) || network_latency_ms < 0.0f ||
        network_latency_ms > 5000.0f)
        return ASKERDWAY_VALUE_OUT_OF_RANGE;
    projectile_heartbeat(network_latency_ms);
    return ASKERDWAY_OK;
}

/**
 * @brief Проверяет v1-событие, ненулевой ID, источник, конечный урон, delay 0..10 секунд и
 * confidence 0..1; затем копирует событие.
 */
askerdway_result AskerdwayApi_SubmitDamageEvent(
    const askerdway_damage_event_v1* event) noexcept {
    const auto validation = validate_api_structure(event);
    if (validation != ASKERDWAY_OK)
        return validation;
    if (event->event_id == 0 ||
        !plausible_pointer(static_cast<std::uintptr_t>(event->source_entity)) ||
        !std::isfinite(event->delay_seconds) ||
        event->delay_seconds < 0.0f || event->delay_seconds > 10.0f ||
        !api_damage_is_valid(event->damage) ||
        !std::isfinite(event->confidence) || event->confidence < 0.0f ||
        event->confidence > 1.0f)
        return ASKERDWAY_VALUE_OUT_OF_RANGE;
    ingest_external_damage(event->event_id,
        static_cast<std::uintptr_t>(event->source_entity), event->delay_seconds,
        event->damage.original, event->damage.physical,
        event->damage.magical, event->damage.pure,
        event->damage.composite, event->confidence);
    return ASKERDWAY_OK;
}

/**
 * @brief Проверяет v1-снаряд, ID, источник, конечные координаты/скорость и неотрицательную
 * геометрию; затем копирует данные в буфер.
 */
askerdway_result AskerdwayApi_SubmitLinearProjectile(
    const askerdway_linear_projectile_v1* projectile) noexcept {
    const auto validation = validate_api_structure(projectile);
    if (validation != ASKERDWAY_OK)
        return validation;
    if (projectile->projectile_id == 0 ||
        !plausible_pointer(static_cast<std::uintptr_t>(projectile->source_entity)) ||
        !api_vector_is_finite(projectile->current_position) ||
        !api_vector_is_finite(projectile->velocity) ||
        !api_damage_is_valid(projectile->damage) ||
        !std::isfinite(projectile->projectile_radius) ||
        projectile->projectile_radius < 0.0f ||
        !std::isfinite(projectile->remaining_distance) ||
        projectile->remaining_distance < 0.0f)
        return ASKERDWAY_VALUE_OUT_OF_RANGE;
    ingest_linear_projectile(projectile->projectile_id,
        static_cast<std::uintptr_t>(projectile->source_entity),
        projectile->damage.original, projectile->damage.physical,
        projectile->damage.magical, projectile->damage.pure,
        projectile->damage.composite, projectile->current_position,
        projectile->velocity, projectile->projectile_radius,
        projectile->remaining_distance);
    return ASKERDWAY_OK;
}

/**
 * @brief Удаляет событие по ненулевому ID; повторное удаление отсутствующего ID не требует
 * существования записи.
 */
askerdway_result AskerdwayApi_RemoveTelemetryEvent(
    std::uint64_t event_id) noexcept {
    if (event_id == 0)
        return ASKERDWAY_INVALID_ARGUMENT;
    remove_projectile(event_id);
    return ASKERDWAY_OK;
}
