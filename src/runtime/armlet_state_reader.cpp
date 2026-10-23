/**
 * @file
 * @brief Снимок Armlet и построение событий.
 *
 * Обновляет состояние предмета/героя и историю здоровья, рассчитывает события периодического
 * урона и проигрывает временную шкалу. Подтверждение модификатором и наблюдаемым здоровьем
 * отделено от самого факта отправки приказа.
 */

// Hero, Armlet and modifier polling.

/**
 * @brief Обновляет текущий hero/item snapshot и историю HP. Недоступный объект приводит к
 * сбросу состояния, а не к повторному использованию старого адреса.
 */
void poll_armlet(std::uintptr_t client_base, std::size_t client_size,
    poll_history& history) {
    const auto now = clock_type::now();
    const bool allow_controller_scan =
        history.last_hero_scan.time_since_epoch().count() == 0 ||
        now - history.last_hero_scan >= std::chrono::seconds(1);
    // Query the authoritative controller every poll.  After Aegis respawn or
    // reincarnation the old pawn can remain readable and pass the structural
    // sanity checks, while its inventory handles are permanently stale.
    std::uintptr_t hero = resolve_local_hero(client_base, client_size, false);
    if (hero == 0 && local_hero_is_owned(history.cached_hero))
        hero = history.cached_hero;
    if (hero == 0 && allow_controller_scan)
        hero = resolve_local_hero(client_base, client_size, true);
    if (allow_controller_scan)
        history.last_hero_scan = now;

    if (hero == 0) {
        if (history.previous_health != -1)
            log_message("INFO", "Local hero is unavailable.");
        history.cached_hero = 0;
        history.cached_armlet = 0;
        history.previous_health = -1;
        history.had_item = false;
        history.confirmation_pending = false;
        clear_runtime_state(armlet_status::waiting_for_hero);
        return;
    }

    if (history.cached_hero != 0 && hero != history.cached_hero) {
        log_message("HERO", "Local hero pawn changed; invalidating cached inventory state.");
        history.cached_armlet = 0;
        history.previous_health = -1;
        history.last_logged_health = -1;
        history.had_item = false;
        history.previous_toggle = false;
        history.previous_effect = false;
        history.confirmation_pending = false;
        history.recent_loss_amount = 0;
        history.similar_loss_samples = 0;
        history.recent_loss_interval_ms = 0;
        history.recent_loss_at = {};
        g_monitor.observed_loss_amount.store(0, std::memory_order_relaxed);
        g_monitor.observed_loss_interval_ms.store(0, std::memory_order_relaxed);
        g_monitor.observed_loss_samples.store(0, std::memory_order_relaxed);
        g_monitor.observed_loss_at.store(0, std::memory_order_relaxed);
        g_monitor.observed_loss_is_self_cost.store(false, std::memory_order_relaxed);
        g_monitor.burning_spear_health_cost.store(0, std::memory_order_relaxed);
        g_monitor.burning_spear_cost_at.store(0, std::memory_order_relaxed);
    }
    history.cached_hero = hero;

    int health = -1;
    int max_health = -1;
    if (!safe_read(hero + schema_fields::m_iHealth, health) ||
        !safe_read(hero + schema_fields::m_iMaxHealth, max_health) ||
        health < 0 || max_health <= 0 || health > max_health * 4) {
        g_monitor.health.store(-1, std::memory_order_relaxed);
        g_monitor.max_health.store(-1, std::memory_order_relaxed);
        g_monitor.status.store(armlet_status::read_error, std::memory_order_relaxed);
        return;
    }

    g_monitor.health.store(health, std::memory_order_relaxed);
    g_monitor.max_health.store(max_health, std::memory_order_relaxed);
    g_monitor.hero_address.store(hero, std::memory_order_relaxed);
    if (history.previous_health >= 0 && health < history.previous_health) {
        const int loss = history.previous_health - health;
        // HP magnitude does not identify its cause. In particular, an enemy
        // hit near an ability's health cost is still unclassified damage until
        // an independent source event proves otherwise.
        const int interval_ms = history.recent_loss_at.time_since_epoch().count() != 0
            ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                now - history.recent_loss_at).count())
            : 0;
        const int tolerance = std::max(5, history.recent_loss_amount / 4);
        if (interval_ms > 25 && interval_ms < 2500 &&
            std::abs(loss - history.recent_loss_amount) <= tolerance)
            ++history.similar_loss_samples;
        else
            history.similar_loss_samples = 1;
        history.recent_loss_amount = loss;
        history.recent_loss_interval_ms = interval_ms;
        history.recent_loss_at = now;
        g_monitor.observed_loss_amount.store(loss, std::memory_order_relaxed);
        g_monitor.observed_loss_interval_ms.store(interval_ms, std::memory_order_relaxed);
        g_monitor.observed_loss_samples.store(
            history.similar_loss_samples, std::memory_order_relaxed);
        g_monitor.observed_loss_at.store(GetTickCount64(), std::memory_order_relaxed);
        g_monitor.observed_loss_is_self_cost.store(false, std::memory_order_relaxed);
        g_monitor.burning_spear_health_cost.store(0, std::memory_order_relaxed);
        g_monitor.burning_spear_cost_at.store(0, std::memory_order_relaxed);
    }
    if (health != history.previous_health) {
        const bool first_sample = history.last_logged_health < 0;
        const bool material_change = !first_sample &&
            std::abs(health - history.last_logged_health) >= 100;
        const bool low_health_update = health <= 350 &&
            (history.last_health_log.time_since_epoch().count() == 0 ||
                now - history.last_health_log >= std::chrono::milliseconds(250));
        if (first_sample || material_change || low_health_update || health == 0) {
            log_health(health);
            history.last_logged_health = health;
            history.last_health_log = now;
        }
        history.previous_health = health;
    }

    const auto armlet = find_armlet_item(
        client_base, client_size, hero, history.cached_armlet);
    history.cached_armlet = armlet;

    bool toggle = false;
    float cooldown = 0.0f;
    const bool item_found = armlet != 0 &&
        safe_read(armlet + schema_fields::m_bToggleState, toggle);
    if (item_found && schema_fields::toggle_cooldown > 0)
        safe_read(armlet + schema_fields::toggle_cooldown, cooldown);
    observe_direct_order_confirmation(item_found, toggle, health);
    const int armlet_entity_index = item_found
        ? find_armlet_entity_index(client_base, hero, armlet)
        : -1;

    const auto modifier = find_armlet_modifier(hero, client_base, client_size);
    const bool effect = modifier != 0;
    std::uint64_t unit_state = 0;
    const bool unit_state_valid = safe_read(hero + schema_fields::m_nUnitState64, unit_state);
    constexpr std::uint64_t muted = 1ULL << 4;
    constexpr std::uint64_t stunned = 1ULL << 5;
    const bool command_blocked = !unit_state_valid || (unit_state & (muted | stunned)) != 0;
    bool game_paused = g_monitor.game_paused.load(std::memory_order_relaxed);
    const bool pause_state_valid = read_game_paused(client_base, game_paused);
    if (pause_state_valid)
        g_monitor.game_paused.store(game_paused, std::memory_order_relaxed);

    g_monitor.item_found.store(item_found, std::memory_order_relaxed);
    g_monitor.toggle_state.store(item_found && toggle, std::memory_order_relaxed);
    g_monitor.effect_confirmed.store(effect, std::memory_order_relaxed);
    g_monitor.armlet_address.store(armlet, std::memory_order_relaxed);
    g_monitor.modifier_address.store(modifier, std::memory_order_relaxed);
    g_monitor.toggle_cooldown.store(cooldown, std::memory_order_relaxed);
    g_monitor.armlet_entity_index.store(armlet_entity_index,
        std::memory_order_relaxed);
    g_monitor.stunned_or_muted.store(command_blocked, std::memory_order_relaxed);
    g_monitor.automation_ready.store(
        g_monitor.schema_ready.load(std::memory_order_acquire) &&
            armlet_entity_index >= 0 &&
            g_prepare_unit_orders.load(std::memory_order_acquire) != 0 &&
            plausible_pointer(g_local_controller_address.load(
                std::memory_order_acquire)) &&
            order_dispatch_installed() &&
            !command_blocked && pause_state_valid && !game_paused,
        std::memory_order_relaxed);

    if (effect)
        read_modifier_telemetry(modifier);

    if (item_found && !history.had_item) {
        char message[96]{};
        std::snprintf(message, sizeof(message), "Armlet found at 0x%llX.",
            static_cast<unsigned long long>(armlet));
        log_message("ITEM", message);
    } else if (!item_found && history.had_item) {
        log_message("ITEM", "Armlet is no longer in the local hero inventory.");
    }

    if (item_found && toggle != history.previous_toggle) {
        log_message("STATE", toggle ? "m_bToggleState -> ON" : "m_bToggleState -> OFF");
        history.confirmation_pending = true;
        history.confirmation_target = toggle;
        history.transition_started = now;
        g_monitor.transitions.fetch_add(1, std::memory_order_relaxed);
    }
    if (effect != history.previous_effect)
        log_message("BUFF", effect ? "Unholy Strength modifier APPLIED."
                                   : "Unholy Strength modifier REMOVED.");

    if (history.confirmation_pending && effect == history.confirmation_target) {
        const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - history.transition_started).count();
        g_monitor.confirmation_latency_ms.store(
            std::clamp(static_cast<int>(latency), 60, 1000),
            std::memory_order_relaxed);
        history.confirmation_pending = false;
    } else if (history.confirmation_pending &&
        now - history.transition_started >= std::chrono::milliseconds(1500)) {
        history.confirmation_pending = false;
        log_message("ERROR", "Armlet modifier confirmation timed out.");
    }

    if (!item_found)
        g_monitor.status.store(effect ? armlet_status::lingering : armlet_status::item_missing,
            std::memory_order_relaxed);
    else if (toggle && effect)
        g_monitor.status.store(armlet_status::active_confirmed, std::memory_order_relaxed);
    else if (toggle)
        g_monitor.status.store(armlet_status::applying, std::memory_order_relaxed);
    else if (effect)
        g_monitor.status.store(armlet_status::lingering, std::memory_order_relaxed);
    else
        g_monitor.status.store(armlet_status::off, std::memory_order_relaxed);

    history.had_item = item_found;
    history.previous_toggle = item_found && toggle;
    history.previous_effect = effect;
}

/**
 * @brief Возвращает управляющий интервал команд; его смысл отделён от измеренного RTT
 * телеметрии.
 */
std::chrono::milliseconds armlet_command_delay() {
    float cooldown = g_monitor.toggle_cooldown.load(std::memory_order_relaxed);
    if (!(cooldown >= 0.0f && cooldown <= 0.5f))
        cooldown = 0.10f;

    const int cooldown_ms = static_cast<int>(cooldown * 1000.0f);
    return std::chrono::milliseconds(std::max(25, cooldown_ms + 10));
}

/**
 * @brief Переносит результат оценки в атомарную диагностику для меню/API; диагностическое
 * чтение не управляет FSM.
 */
void publish_safety(const safety_result& result, bool telemetry_ready) {
    g_monitor.safety.store(result.state, std::memory_order_relaxed);
    g_monitor.expected_cycle_ms.store(result.cycle_ms, std::memory_order_relaxed);
    g_monitor.incoming_count.store(result.count, std::memory_order_relaxed);
    g_monitor.incoming_damage.store(result.damage, std::memory_order_relaxed);
    g_monitor.nearest_impact_ms.store(result.nearest_ms, std::memory_order_relaxed);
    g_monitor.predicted_minimum_hp.store(result.minimum_hp, std::memory_order_relaxed);
    g_monitor.modifier_tick_count.store(result.modifier_ticks, std::memory_order_relaxed);
    g_monitor.high_risk_detected.store(result.high_risk, std::memory_order_relaxed);
    g_monitor.high_risk_interval_ms.store(
        result.high_risk_interval_ms, std::memory_order_relaxed);
    g_monitor.projectile_telemetry_ready.store(telemetry_ready, std::memory_order_relaxed);
}

float conservative_damage(const incoming_projectile& incoming,
    float armor, float magic_resistance) {
    const float armor_factor = 1.0f -
        (0.06f * armor) / (1.0f + 0.06f * std::fabs(armor));
    const float magic_factor = 1.0f - std::clamp(magic_resistance, -1.0f, 0.95f);
    const float modeled = incoming.physical_damage * armor_factor +
        incoming.magical_damage * magic_factor + incoming.pure_damage +
        incoming.composite_damage;
    const float raw = std::max(incoming.damage, incoming.original_damage);
    // Raw damage remains the ceiling because barriers and blocks are not available
    // in this standalone monitor. Split damage is still modeled and used when higher.
    return std::max(raw, modeled);
}


int timeline_priority(timeline_kind kind) {
    switch (kind) {
    case timeline_kind::toggle_off: return 0;
    case timeline_kind::armlet_health_removal: return 1;
    case timeline_kind::instant_heal: return 2;
    case timeline_kind::incoming_damage: return 3;
    case timeline_kind::toggle_on: return 4;
    case timeline_kind::armlet_health_addition: return 5;
    }
    return 6;
}

template <std::size_t Size>
bool queue_event(std::array<damage_event, Size>& events, std::size_t& count,
    const damage_event& event) {
    if (count >= events.size())
        return false;
    events[count++] = event;
    return true;
}

struct timeline_prediction {
    float damage = 0.0f;
    float nearest_damage = -1.0f;
    float minimum_hp = -1.0f;
    float guarded_minimum_hp = -1.0f;
    bool saw_incoming_damage = false;
    bool only_high_risk_damage = true;
};

template <std::size_t Size>
/**
 * @brief Проигрывает упорядоченные события здоровья/урона; порядок событий с одинаковым
 * временем влияет на минимальный HP и безопасность окна.
 */
timeline_prediction simulate_timeline(
    const std::array<damage_event, Size>& timeline, std::size_t event_count,
    float starting_health, float starting_max_health, float armlet_health,
    float health_added_ms) {
    timeline_prediction prediction{};
    float predicted_hp = starting_health;
    float predicted_max_hp = starting_max_health;
    float minimum_hp = predicted_hp;
    float guarded_minimum_hp = std::numeric_limits<float>::infinity();
    float nearest_damage = std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < event_count; ++index) {
        const auto& event = timeline[index];
        switch (event.kind) {
        case timeline_kind::armlet_health_removal:
            predicted_max_hp = std::max(1.0f, predicted_max_hp - armlet_health);
            predicted_hp = std::min(predicted_max_hp,
                std::max(1.0f, predicted_hp - event.damage));
            break;
        case timeline_kind::instant_heal:
            predicted_hp = std::min(predicted_max_hp, predicted_hp + event.damage);
            break;
        case timeline_kind::incoming_damage:
            prediction.saw_incoming_damage = true;
            prediction.only_high_risk_damage =
                prediction.only_high_risk_damage && event.high_risk;
            predicted_hp -= event.damage;
            prediction.damage += event.damage;
            nearest_damage = std::min(nearest_damage, event.time_ms);
            if (event.time_ms >= health_added_ms || predicted_hp <= 0.0f)
                guarded_minimum_hp = std::min(guarded_minimum_hp, predicted_hp);
            break;
        case timeline_kind::armlet_health_addition:
            predicted_max_hp = std::min(starting_max_health,
                predicted_max_hp + event.damage);
            predicted_hp = std::min(predicted_max_hp, predicted_hp + event.damage);
            break;
        case timeline_kind::toggle_off:
        case timeline_kind::toggle_on:
            break;
        }
        minimum_hp = std::min(minimum_hp, predicted_hp);
    }
    guarded_minimum_hp = std::min(guarded_minimum_hp, predicted_hp);
    prediction.nearest_damage = std::isfinite(nearest_damage)
        ? std::max(0.0f, nearest_damage) : -1.0f;
    prediction.minimum_hp = minimum_hp;
    prediction.guarded_minimum_hp = guarded_minimum_hp;
    return prediction;
}

damage_type classify_damage(const incoming_projectile& incoming) {
    const int components = (incoming.physical_damage > 0.0f ? 1 : 0) +
        (incoming.magical_damage > 0.0f ? 1 : 0) +
        (incoming.pure_damage > 0.0f ? 1 : 0) +
        (incoming.composite_damage > 0.0f ? 1 : 0);
    if (components != 1)
        return damage_type::mixed;
    if (incoming.physical_damage > 0.0f)
        return damage_type::physical;
    if (incoming.magical_damage > 0.0f)
        return damage_type::magical;
    if (incoming.pure_damage > 0.0f)
        return damage_type::pure;
    return damage_type::composite;
}

/**
 * @brief Добавляет прогнозируемые периодические события с учётом доступных параметров;
 * неизвестные модификаторы не подменяются произвольным уроном.
 */
bool append_known_modifier_ticks(std::uintptr_t client_base,
    std::size_t client_size, std::uintptr_t hero, std::uintptr_t entity_system,
    std::uint8_t hero_team, float game_time, float horizon_ms,
    std::array<damage_event, 512>& events, std::size_t& event_count,
    bool& unknown_enemy_modifier, int& modifier_ticks,
    bool& high_risk_periodic, float& shortest_high_risk_interval_ms) {
    std::array<std::uintptr_t, 128> modifiers{};
    std::size_t count = 0;
    if (!collect_active_modifiers(
            hero, client_base, client_size, modifiers, count))
        return false;

    const float horizon_end = game_time + horizon_ms / 1000.0f;
    struct burning_spear_group {
        std::uint32_t caster_handle = 0xFFFFFFFFu;
        int counter_instances = 0;
        int damage_stacks = 0;
    };
    std::array<burning_spear_group, 16> burning_spear_groups{};
    std::size_t burning_spear_group_count = 0;
    const auto find_burning_spear_group = [&](std::uint32_t caster_handle)
        -> burning_spear_group* {
        for (std::size_t group_index = 0;
             group_index < burning_spear_group_count; ++group_index) {
            if (burning_spear_groups[group_index].caster_handle == caster_handle)
                return &burning_spear_groups[group_index];
        }
        if (burning_spear_group_count >= burning_spear_groups.size())
            return nullptr;
        auto& group = burning_spear_groups[burning_spear_group_count++];
        group.caster_handle = caster_handle;
        return &group;
    };
    for (std::size_t index = 0; index < count; ++index) {
        const auto modifier = modifiers[index];
        char class_name[192]{};
        if (!read_rtti_name(modifier, client_base, client_size, class_name))
            return false;

        if (std::strstr(class_name,
                "CDOTA_Modifier_Huskar_Burning_Spear_Counter@@") != nullptr) {
            std::uint32_t caster_handle = 0xFFFFFFFFu;
            if (!safe_read(modifier + private_modifier_layout::m_hCaster, caster_handle))
                return false;
            const auto caster = entity_by_handle(entity_system, caster_handle);
            std::uint8_t caster_team = 0;
            if (plausible_pointer(caster) &&
                safe_read(caster + schema_fields::m_iTeamNum, caster_team) &&
                caster_team != 0 && caster_team != hero_team) {
                auto* group = find_burning_spear_group(caster_handle);
                if (group == nullptr)
                    return false;
                ++group->counter_instances;
            }
            continue;
        }

        const known_modifier_spec* specification = nullptr;
        for (const auto& candidate : known_modifier_registry) {
            if (std::strstr(class_name, candidate.class_name) != nullptr) {
                specification = &candidate;
                break;
            }
        }
        const bool armlet = std::strstr(class_name,
            "CDOTA_Modifier_Armlet_UnholyStrength@@") != nullptr;
        const bool high_risk_marker =
            std::strstr(class_name,
                "CDOTA_Modifier_DeathProphet_SpiritSiphon") != nullptr ||
            std::strstr(class_name,
                "CDOTA_Modifier_QueenOfPain_SonicWave") != nullptr ||
            std::strstr(class_name,
                "CDOTA_Modifier_Phoenix_Sun_Ray_Blind") != nullptr;

        if (specification == nullptr) {
            if (armlet || std::strstr(class_name, "CDOTA_Modifier_Item_") != nullptr)
                continue;

            std::uint32_t caster_handle = 0xFFFFFFFFu;
            if (!safe_read(modifier + private_modifier_layout::m_hCaster, caster_handle))
                return false;
            const auto caster = entity_by_handle(entity_system, caster_handle);
            std::uint8_t caster_team = 0;
            if (plausible_pointer(caster) &&
                safe_read(caster + schema_fields::m_iTeamNum, caster_team) &&
                caster_team != 0 && caster_team != hero_team) {
                // Towers apply non-damaging service/aura/targeting modifiers
                // while the hero is in their influence radius. Treating every
                // unknown enemy-cast modifier as a possible DoT turned that
                // radius into a permanent Armlet-abuse lock. Tower damage is
                // already modeled by the real tracking projectile plus the
                // synthetic tower shot, including projectiles that remain in
                // flight after the hero leaves attack range.
                if (is_tower_entity(caster, client_base, client_size)) {
                    continue;
                } else if (high_risk_marker) {
                    high_risk_periodic = true;
                    shortest_high_risk_interval_ms = std::min(
                        shortest_high_risk_interval_ms, 250.0f);
                } else {
                    unknown_enemy_modifier = true;
                }
            }
            continue;
        }

        float creation_time = 0.0f;
        float duration = 0.0f;
        float die_time = 0.0f;
        float previous_tick = 0.0f;
        float interval = 0.0f;
        float damage = 0.0f;
        float exact_next_tick = 0.0f;
        float max_health_percent = 0.0f;
        int stack_count = 1;
        std::uint32_t caster_handle = 0xFFFFFFFFu;
        if (!safe_read(modifier + private_modifier_layout::m_flCreationTime, creation_time) ||
            !safe_read(modifier + private_modifier_layout::m_flDuration, duration) ||
            !safe_read(modifier + private_modifier_layout::m_flDieTime, die_time) ||
            !safe_read(modifier + private_modifier_layout::m_flPreviousTick, previous_tick) ||
            !safe_read(modifier + private_modifier_layout::m_hCaster, caster_handle))
            return false;

        if (specification->interval_offset != 0) {
            if (!safe_read(modifier + specification->interval_offset, interval))
                return false;
        } else {
            interval = specification->fixed_interval;
        }
        if (specification->damage_offset == 0)
            return false;
        if (!safe_read(modifier + specification->damage_offset, damage))
            return false;
        if (specification->exact_next_tick_offset != 0 &&
            !safe_read(modifier + specification->exact_next_tick_offset,
                exact_next_tick))
            return false;
        if (specification->max_health_percent_offset != 0 &&
            (!safe_read(modifier + specification->max_health_percent_offset,
                 max_health_percent) || !finite_nonnegative(max_health_percent) ||
             max_health_percent > 100.0f))
            return false;
        if (specification->multiply_by_stack_count &&
            (!safe_read(modifier + private_modifier_layout::m_iStackCount, stack_count) ||
             stack_count < 1 || stack_count > 128))
            return false;

        if (specification->max_health_percent_offset != 0) {
            const int target_max_health = g_monitor.max_health.load(
                std::memory_order_relaxed);
            if (target_max_health <= 0)
                return false;
            damage += static_cast<float>(target_max_health) *
                max_health_percent * 0.01f;
        }
        if (specification->mode == modifier_damage_mode::per_second)
            damage *= interval;
        damage *= static_cast<float>(stack_count);
        if (std::strstr(class_name,
                "CDOTA_Modifier_Necrolyte_Heartstopper_Aura_Effect@@") != nullptr) {
            static ULONGLONG next_heartstopper_log = 0;
            const ULONGLONG diagnostic_now = GetTickCount64();
            if (diagnostic_now >= next_heartstopper_log) {
                char message[160]{};
                std::snprintf(message, sizeof(message),
                    "Heartstopper modeled: %.1f HP per %.0f ms tick.",
                    damage, interval * 1000.0f);
                log_message("DOT", message);
                next_heartstopper_log = diagnostic_now + 2000;
            }
        }
        if (std::strstr(class_name,
                "CDOTA_Modifier_Huskar_Burning_Spear_Debuff@@") != nullptr) {
            const auto caster = entity_by_handle(entity_system, caster_handle);
            std::uint8_t caster_team = 0;
            if (!plausible_pointer(caster) ||
                !safe_read(caster + schema_fields::m_iTeamNum, caster_team) ||
                caster_team == 0)
                return false;
            if (caster_team != hero_team) {
                auto* group = find_burning_spear_group(caster_handle);
                if (group == nullptr)
                    return false;
                group->damage_stacks += stack_count;
            }
        }

        if (!(interval > 0.01f && interval <= 10.0f) ||
            !finite_nonnegative(damage) || damage <= 0.0f ||
            !std::isfinite(creation_time) || !std::isfinite(duration) ||
            !std::isfinite(die_time) || !std::isfinite(previous_tick))
            return false;

        const bool event_is_high_risk = interval <= 0.2501f;
        if (event_is_high_risk) {
            high_risk_periodic = true;
            shortest_high_risk_interval_ms = std::min(
                shortest_high_risk_interval_ms, interval * 1000.0f);
        }

        float effect_end = horizon_end;
        if (!specification->refreshable) {
            if (die_time > game_time - 0.05f) {
                effect_end = std::min(effect_end, die_time);
            } else if (duration > 0.0f && creation_time >= 0.0f) {
                effect_end = std::min(effect_end, creation_time + duration);
            }
        }
        if (effect_end < game_time)
            continue;

        float next_tick = 0.0f;
        float confidence = 0.65f;
        if (specification->exact_next_tick_offset != 0 &&
            std::isfinite(exact_next_tick) &&
            exact_next_tick >= game_time - interval &&
            exact_next_tick <= effect_end + interval) {
            next_tick = exact_next_tick;
            confidence = 1.0f;
        } else if (previous_tick > 0.0f && previous_tick <= game_time + interval) {
            next_tick = previous_tick + interval;
            confidence = 0.85f;
        } else if (creation_time >= 0.0f) {
            next_tick = creation_time;
        } else {
            return false;
        }

        if (next_tick <= game_time) {
            const float steps = std::floor((game_time - next_tick) / interval) + 1.0f;
            next_tick += steps * interval;
        }

        const float timing_error_ms = (1.0f - confidence) * 60.0f;
        const float conservative_tick_damage = damage *
            (1.0f + (1.0f - confidence) * 0.75f);
        for (float tick = next_tick;
             tick <= effect_end + 0.0001f && tick <= horizon_end + 0.0001f;
             tick += interval) {
            damage_event event{};
            event.time_ms = std::max(0.0f,
                (tick - game_time) * 1000.0f - timing_error_ms);
            event.damage = conservative_tick_damage;
            event.type = specification->type;
            event.source = entity_by_handle(entity_system, caster_handle);
            event.confidence = confidence;
            event.kind = timeline_kind::incoming_damage;
            event.high_risk = event_is_high_risk;
            if (!queue_event(events, event_count, event))
                return false;
            ++modifier_ticks;
        }
    }
    for (std::size_t group_index = 0;
         group_index < burning_spear_group_count; ++group_index) {
        const auto& group = burning_spear_groups[group_index];
        if (group.counter_instances > group.damage_stacks)
            return false;
    }
    return true;
}

void refresh_enemy_periodic_source_cache(std::uintptr_t entity_system,
    std::uint8_t hero_team, ULONGLONG now) {
    constexpr ULONGLONG refresh_interval_ms = 750;
    if (g_periodic_source_cache.local_team == hero_team &&
        g_periodic_source_cache.refreshed_at != 0 &&
        now >= g_periodic_source_cache.refreshed_at &&
        now - g_periodic_source_cache.refreshed_at < refresh_interval_ms)
        return;

    g_periodic_source_cache.count = 0;
    g_periodic_source_cache.local_team = hero_team;
    visit_entities(entity_system, [&](std::uintptr_t entity, int) {
        std::uint8_t team = 0;
        char designer_name[96]{};
        if (!safe_read(entity + schema_fields::m_iTeamNum, team) || team == 0 ||
            team == hero_team || !read_designer_name(entity, designer_name) ||
            std::strcmp(designer_name, "npc_dota_hero_leshrac") != 0)
            return false;
        if (g_periodic_source_cache.count <
            g_periodic_source_cache.entries.size())
            g_periodic_source_cache.entries[
                g_periodic_source_cache.count++] = entity;
        return false;
    });
    g_periodic_source_cache.refreshed_at = now;
}

/**
 * @brief Добавляет прогнозируемые периодические события с учётом доступных параметров;
 * неизвестные модификаторы не подменяются произвольным уроном.
 */
bool append_enemy_source_periodic_ticks(std::uintptr_t client_base,
    std::size_t client_size, std::uintptr_t entity_system,
    std::uint8_t hero_team, const armlet_vec3& hero_position,
    float game_time, float horizon_ms,
    std::array<damage_event, 512>& events, std::size_t& event_count,
    int& modifier_ticks) {
    // Pulse Nova's damaging modifier lives on Leshrac, not on its victim.
    // Current installed game data: 1.0 s interval, 500 base radius and up to
    // 180 + 30 talent magical damage.  A larger radius covers AoE bonuses.
    constexpr float pulse_interval = 1.0f;
    constexpr float conservative_pulse_radius = 800.0f;
    constexpr float conservative_pulse_damage = 210.0f;
    const ULONGLONG now = GetTickCount64();
    refresh_enemy_periodic_source_cache(entity_system, hero_team, now);
    const float horizon_end = game_time + horizon_ms / 1000.0f;

    for (std::size_t source_index = 0;
         source_index < g_periodic_source_cache.count; ++source_index) {
        const auto source = g_periodic_source_cache.entries[source_index];
        int source_health = 0;
        std::uint8_t source_team = 0;
        armlet_vec3 source_position{};
        if (!plausible_pointer(source) ||
            !safe_read(source + schema_fields::m_iHealth, source_health) ||
            source_health <= 0 ||
            !safe_read(source + schema_fields::m_iTeamNum, source_team) ||
            source_team == 0 || source_team == hero_team ||
            !read_world_position(source, source_position))
            continue;
        if (length(subtract(hero_position, source_position)) >
            conservative_pulse_radius)
            continue;

        std::array<std::uintptr_t, 128> modifiers{};
        std::size_t modifier_count = 0;
        if (!collect_active_modifiers(
                source, client_base, client_size, modifiers, modifier_count))
            return false;
        for (std::size_t index = 0; index < modifier_count; ++index) {
            char class_name[192]{};
            if (!read_rtti_name(
                    modifiers[index], client_base, client_size, class_name))
                return false;
            if (std::strstr(class_name,
                    "CDOTA_Modifier_Leshrac_Pulse_Nova@@") == nullptr)
                continue;

            float creation_time = 0.0f;
            float previous_tick = 0.0f;
            if (!safe_read(modifiers[index] + private_modifier_layout::m_flCreationTime,
                    creation_time) ||
                !safe_read(modifiers[index] + private_modifier_layout::m_flPreviousTick,
                    previous_tick) ||
                !std::isfinite(creation_time) || !std::isfinite(previous_tick))
                return false;
            float next_tick = previous_tick > 0.0f
                ? previous_tick + pulse_interval
                : creation_time + pulse_interval;
            if (next_tick <= game_time) {
                const float steps =
                    std::floor((game_time - next_tick) / pulse_interval) + 1.0f;
                next_tick += steps * pulse_interval;
            }
            for (float tick = next_tick; tick <= horizon_end + 0.0001f;
                 tick += pulse_interval) {
                damage_event event{};
                event.time_ms = std::max(
                    0.0f, (tick - game_time) * 1000.0f - 30.0f);
                event.damage = conservative_pulse_damage;
                event.type = damage_type::magical;
                event.source = source;
                event.confidence = 0.9f;
                event.kind = timeline_kind::incoming_damage;
                if (!queue_event(events, event_count, event))
                    return false;
                ++modifier_ticks;
            }
        }
    }
    return true;
}
