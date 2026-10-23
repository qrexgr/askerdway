/**
 * @file
 * @brief Оценка безопасного окна.
 *
 * Копирует источники урона под shared-lock и оценивает окно переключения вне блокировки.
 * Возвращает safety_result с причиной решения; нехватка или устаревание данных обрабатываются
 * явно. Помощники в конце файла публикуют состояние цикла и ставят приказы.
 */

// Damage timeline and cycle checks.

/**
 * @brief Копирует буфер угроз под shared-lock, проверяет свежесть и моделирует окно
 * переключения. Возвращает решение и причину; сам по себе вызов не отправляет приказ.
 */
safety_result evaluate_timeline_safety(std::uintptr_t client_base,
    std::size_t client_size, const cycle_context& cycle) {
    constexpr int health_threshold = 300;
    constexpr int post_cycle_window_ms = 250;
    constexpr ULONGLONG telemetry_timeout_ms = 250;
    constexpr ULONGLONG projectile_timeout_ms = 350;
    constexpr ULONGLONG released_attack_timeout_ms = 450;

    safety_result result{};
    g_monitor.instant_heal_planned.store(
        instant_heal_kind::none, std::memory_order_relaxed);
    g_monitor.instant_heal_planned_amount.store(0, std::memory_order_relaxed);
    std::array<incoming_projectile, 128> incoming_snapshot{};
    ULONGLONG heartbeat = 0;
    ULONGLONG latency_updated_at = 0;
    float network_latency_ms = -1.0f;
    AcquireSRWLockShared(&g_projectiles.lock);
    incoming_snapshot = g_projectiles.entries;
    heartbeat = g_projectiles.heartbeat;
    latency_updated_at = g_projectiles.latency_updated_at;
    network_latency_ms = g_projectiles.network_latency_ms;
    ReleaseSRWLockShared(&g_projectiles.lock);

    const ULONGLONG now = GetTickCount64();
    result.evaluated_at = now;
    const bool telemetry_ready = heartbeat != 0 && now >= heartbeat &&
        now - heartbeat <= telemetry_timeout_ms;
    const bool latency_ready = latency_updated_at != 0 &&
        now >= latency_updated_at &&
        now - latency_updated_at <= 1000 &&
        std::isfinite(network_latency_ms) && network_latency_ms >= 0.0f &&
        network_latency_ms <= 5000.0f;
    g_monitor.network_latency_ms.store(
        latency_ready ? network_latency_ms : -1.0f,
        std::memory_order_relaxed);

    // Dispatch freshness and OFF acknowledgement are distinct delays. Even a
    // zero-ping lobby has frame/tick scheduling between observed OFF and ON.
    const float reaction_lag_ms = (latency_ready ? network_latency_ms : 0.0f) +
        static_cast<float>(armlet_plan_lifetime_ms);
    const float reenable_gap_ms=static_cast<float>(armlet_plan_lifetime_ms) +
        (latency_ready ? network_latency_ms : 0.0f);
    constexpr int armlet_ramp_ticks = 6;
    constexpr float armlet_ramp_tick_ms = 100.0f;
    const float health_added_ms =
        reenable_gap_ms + armlet_ramp_ticks * armlet_ramp_tick_ms;
    result.cycle_ms = static_cast<int>(std::ceil(health_added_ms)) + 50;
    result.minimum_hp = static_cast<float>(
        g_monitor.health.load(std::memory_order_relaxed));

    const int health = g_monitor.health.load(std::memory_order_relaxed);
    if (!g_monitor.abuse_enabled.load(std::memory_order_relaxed)) {
        result.state = safety_state::abuse_disabled;
        publish_safety(result, telemetry_ready);
        return result;
    }
    if (health > health_threshold) {
        result.state = safety_state::health_above_threshold;
        publish_safety(result, telemetry_ready);
        return result;
    }
    if (!g_monitor.toggle_state.load(std::memory_order_relaxed) ||
        !g_monitor.effect_confirmed.load(std::memory_order_relaxed)) {
        result.state = safety_state::state_unconfirmed;
        publish_safety(result, telemetry_ready);
        return result;
    }
    if (g_monitor.stunned_or_muted.load(std::memory_order_relaxed)) {
        result.state = safety_state::stunned_or_muted;
        publish_safety(result, telemetry_ready);
        return result;
    }
    if (!g_monitor.automation_ready.load(std::memory_order_relaxed)) {
        result.state = safety_state::command_unavailable;
        publish_safety(result, telemetry_ready);
        return result;
    }
    if (clock_type::now() < cycle.retry_after) {
        result.state = safety_state::command_unavailable;
        publish_safety(result, telemetry_ready);
        return result;
    }
    if (clock_type::now() - cycle.state_since < armlet_command_delay()) {
        result.state = safety_state::cooldown_wait;
        publish_safety(result, telemetry_ready);
        return result;
    }
    const bool local_only_fallback = g_monitor.allow_local_only_fallback.load(
        std::memory_order_relaxed);
    if (!latency_ready) {
        result.state = safety_state::telemetry_stale;
        publish_safety(result, false);
        return result;
    }
    if (!telemetry_ready && !local_only_fallback) {
        result.state = safety_state::telemetry_stale;
        publish_safety(result, false);
        return result;
    }

    const auto hero = g_monitor.hero_address.load(std::memory_order_relaxed);
    armlet_vec3 hero_position{};
    armlet_vec3 hero_velocity{};
    float hero_radius = 0.0f;
    float armor = 0.0f;
    float magic_resistance = 0.0f;
    float game_time = 0.0f;
    std::uint8_t hero_team = 0;
    std::uintptr_t entity_system = 0;
    if (!plausible_pointer(hero) || !read_world_position(hero, hero_position) ||
        !safe_read(hero + schema_fields::m_vecAbsVelocity, hero_velocity) ||
        !safe_read(hero + schema_fields::m_flHullRadius, hero_radius) || hero_radius < 0.0f ||
        !safe_read(hero + schema_fields::m_flPhysicalArmorValue, armor) ||
        !safe_read(hero + schema_fields::m_flMagicalResistanceValue, magic_resistance) ||
        !safe_read(hero + schema_fields::m_iTeamNum, hero_team) || hero_team == 0 ||
        !read_game_entity_system(client_base, entity_system) ||
        !read_game_time(client_base, game_time)) {
        result.state = safety_state::incomplete_event;
        publish_safety(result, telemetry_ready);
        return result;
    }

    std::array<damage_event, 512> timeline{};
    std::size_t event_count = 0;
    bool incomplete = false;
    bool unknown_enemy_modifier = false;
    bool high_risk_periodic = false;
    float shortest_high_risk_interval_ms =
        std::numeric_limits<float>::infinity();
    const float horizon_ms =
        static_cast<float>(result.cycle_ms + post_cycle_window_ms);
    // Client projectile positions, replicated modifier timers and the HP sample
    // are old by the time the order is processed by the server. Collect an
    // extra latency-width slice, then shift hostile events left exactly once.
    const float collection_horizon_ms = horizon_ms + reaction_lag_ms;

    const int bonus_strength = g_monitor.bonus_strength.load(std::memory_order_relaxed);
    if (bonus_strength <= 0) {
        result.state = safety_state::incomplete_event;
        publish_safety(result, telemetry_ready);
        return result;
    }
    const float armlet_health = static_cast<float>(bonus_strength) * 22.0f;
    // OFF removes health at t=0; ON and its ramp may follow only after the
    // acknowledgement gap. Hostile events must be allowed inside that gap.
    incomplete |= !queue_event(timeline, event_count,
        {0.0f, armlet_health, damage_type::pure, 0, 1.0f,
            timeline_kind::armlet_health_removal});
    const int strength_per_tick = bonus_strength / armlet_ramp_ticks;
    const int strength_remainder = bonus_strength % armlet_ramp_ticks;
    for (int tick = 1; tick <= armlet_ramp_ticks; ++tick) {
        const int strength_this_tick = strength_per_tick +
            (tick > armlet_ramp_ticks - strength_remainder ? 1 : 0);
        incomplete |= !queue_event(timeline, event_count,
            {reenable_gap_ms + tick * armlet_ramp_tick_ms,
                static_cast<float>(strength_this_tick) * 22.0f,
                damage_type::pure, 0, 1.0f,
                timeline_kind::armlet_health_addition});
    }

    if (!append_known_modifier_ticks(client_base, client_size, hero, entity_system,
            hero_team, game_time, collection_horizon_ms, timeline, event_count,
            unknown_enemy_modifier, result.modifier_ticks,
            high_risk_periodic, shortest_high_risk_interval_ms))
        incomplete = true;
    if (!append_enemy_source_periodic_ticks(client_base, client_size,
            entity_system, hero_team, hero_position, game_time,
            collection_horizon_ms,
            timeline, event_count, result.modifier_ticks))
        incomplete = true;
    const auto scheduled_impact_is_live = [now](
        ULONGLONG impact_at, ULONGLONG grace_ms) {
        return impact_at != 0 &&
            (impact_at >= now || now - impact_at <= grace_ms);
    };
    const auto scheduled_grace_ms = static_cast<ULONGLONG>(
        std::ceil(100.0f + reaction_lag_ms));

    // Fuse ProjectileManager observations with synthetic tower shots one to
    // one. The real object owns position/speed/ETA; the synthetic record only
    // supplies conservative damage when the tracking object lacks it.
    std::array<int, 128> synthetic_tower_for_real{};
    std::array<int, 128> real_projectile_for_synthetic{};
    synthetic_tower_for_real.fill(-1);
    real_projectile_for_synthetic.fill(-1);
    constexpr float tower_fusion_eta_tolerance_ms = 450.0f;
    for (std::size_t real_index = 0;
         real_index < incoming_snapshot.size(); ++real_index) {
        const auto& real = incoming_snapshot[real_index];
        const ULONGLONG real_timeout =
            real.kind == projectile_kind::released_attack
            ? released_attack_timeout_ms : projectile_timeout_ms;
        if (!real.active || now < real.updated_at ||
            (real.kind != projectile_kind::tracking &&
                real.kind != projectile_kind::released_attack) ||
            now - real.updated_at > real_timeout ||
            entity_by_handle(entity_system, real.target_handle) != hero)
            continue;

        const auto real_source =
            entity_by_handle(entity_system, real.source_handle);
        armlet_vec3 start = real.position;
        if (!plausible_pointer(real_source) || real.speed <= 1.0f ||
            (real.kind == projectile_kind::released_attack &&
                !real.position_valid &&
                !read_world_position(real_source, start)))
            continue;
        const float first_eta =
            length(subtract(hero_position, start)) / real.speed;
        const armlet_vec3 predicted_position{
            hero_position.x + hero_velocity.x * first_eta,
            hero_position.y + hero_velocity.y * first_eta,
            hero_position.z + hero_velocity.z * first_eta,
        };
        const float real_eta_ms =
            length(subtract(predicted_position, start)) /
            real.speed * 1000.0f;
        if (!std::isfinite(real_eta_ms) || real_eta_ms < 0.0f)
            continue;
        const float real_impact_ms = std::max(0.0f,
            real_eta_ms - static_cast<float>(now - real.updated_at));

        int best_synthetic = -1;
        float best_eta_delta = tower_fusion_eta_tolerance_ms + 1.0f;
        for (std::size_t synthetic_index = 0;
             synthetic_index < incoming_snapshot.size(); ++synthetic_index) {
            const auto& synthetic = incoming_snapshot[synthetic_index];
            if (!synthetic.active ||
                synthetic.kind != projectile_kind::tower_attack ||
                synthetic.source_entity != real_source ||
                real_projectile_for_synthetic[synthetic_index] >= 0 ||
                !same_entity_handle(
                    synthetic.target_handle, real.target_handle) ||
                !scheduled_impact_is_live(
                    synthetic.impact_at, scheduled_grace_ms))
                continue;
            const float synthetic_impact_ms = synthetic.impact_at > now
                ? static_cast<float>(synthetic.impact_at - now) : 0.0f;
            const float eta_delta =
                std::abs(synthetic_impact_ms - real_impact_ms);
            if (eta_delta <= tower_fusion_eta_tolerance_ms &&
                eta_delta < best_eta_delta) {
                best_eta_delta = eta_delta;
                best_synthetic = static_cast<int>(synthetic_index);
            }
        }
        if (best_synthetic >= 0) {
            synthetic_tower_for_real[real_index] = best_synthetic;
            real_projectile_for_synthetic[
                static_cast<std::size_t>(best_synthetic)] =
                static_cast<int>(real_index);
        }
    }

    for (std::size_t incoming_index = 0;
         incoming_index < incoming_snapshot.size(); ++incoming_index) {
        const auto& incoming = incoming_snapshot[incoming_index];
        if (incoming.kind == projectile_kind::tower_attack &&
            real_projectile_for_synthetic[incoming_index] >= 0)
            continue;
        if (!incoming.active || now < incoming.updated_at)
            continue;
        const ULONGLONG age = now - incoming.updated_at;
        if (incoming.kind == projectile_kind::external_damage ||
            incoming.kind == projectile_kind::melee_animation ||
            incoming.kind == projectile_kind::tower_attack) {
            if (!scheduled_impact_is_live(
                    incoming.impact_at, scheduled_grace_ms))
                continue;
        } else {
            const ULONGLONG timeout = incoming.kind == projectile_kind::released_attack
                ? released_attack_timeout_ms
                : (incoming.kind == projectile_kind::instant_attack
                    ? 150 : projectile_timeout_ms);
            if (age > timeout)
                continue;
        }

        std::uintptr_t source = incoming.source_entity;
        if (incoming.kind != projectile_kind::linear &&
            incoming.kind != projectile_kind::external_damage &&
            incoming.kind != projectile_kind::melee_animation &&
            incoming.kind != projectile_kind::tower_attack) {
            if (entity_by_handle(entity_system, incoming.target_handle) != hero)
                continue;
            source = entity_by_handle(entity_system, incoming.source_handle);
        }

        std::uint8_t source_team = 0;
        if (!plausible_pointer(source) ||
            !safe_read(source + schema_fields::m_iTeamNum, source_team) || source_team == 0) {
            incomplete = true;
            continue;
        }
        if (source == hero || source_team == hero_team)
            continue;

        float impact_ms = -1.0f;
        if (incoming.kind == projectile_kind::external_damage ||
            incoming.kind == projectile_kind::melee_animation ||
            incoming.kind == projectile_kind::tower_attack) {
            impact_ms = incoming.impact_at > now
                ? static_cast<float>(incoming.impact_at - now) : 0.0f;
        } else if (incoming.kind == projectile_kind::instant_attack) {
            impact_ms = 0.0f;
        } else if (incoming.kind == projectile_kind::linear) {
            const armlet_vec3 relative_velocity{
                incoming.velocity.x - hero_velocity.x,
                incoming.velocity.y - hero_velocity.y,
                incoming.velocity.z - hero_velocity.z,
            };
            const float relative_speed_sq = dot(relative_velocity, relative_velocity);
            if (relative_speed_sq <= 1.0f || incoming.remaining_distance <= 0.0f) {
                incomplete = true;
                continue;
            }
            const armlet_vec3 to_hero = subtract(hero_position, incoming.position);
            const float impact_seconds = dot(to_hero, relative_velocity) /
                relative_speed_sq;
            if (impact_seconds < 0.0f ||
                impact_seconds * incoming.speed > incoming.remaining_distance)
                continue;
            const armlet_vec3 separation{
                to_hero.x - relative_velocity.x * impact_seconds,
                to_hero.y - relative_velocity.y * impact_seconds,
                to_hero.z - relative_velocity.z * impact_seconds,
            };
            if (length(separation) > incoming.radius + hero_radius)
                continue;
            impact_ms = impact_seconds * 1000.0f;
        } else {
            armlet_vec3 start = incoming.position;
            if (incoming.kind == projectile_kind::released_attack &&
                !incoming.position_valid && !read_world_position(source, start)) {
                incomplete = true;
                continue;
            }
            if (incoming.speed <= 1.0f) {
                incomplete = true;
                continue;
            }
            const float first_eta = length(subtract(hero_position, start)) / incoming.speed;
            const armlet_vec3 predicted_position{
                hero_position.x + hero_velocity.x * first_eta,
                hero_position.y + hero_velocity.y * first_eta,
                hero_position.z + hero_velocity.z * first_eta,
            };
            impact_ms = length(subtract(predicted_position, start)) /
                incoming.speed * 1000.0f;
        }

        if (!std::isfinite(impact_ms) || impact_ms < 0.0f) {
            incomplete = true;
            continue;
        }
        // Position-derived ETA is relative to the observation, not this
        // evaluation. An otherwise fresh heartbeat cannot refresh an older
        // projectile record. Absolute impact_at records already use `now`.
        if (incoming.kind == projectile_kind::tracking ||
            incoming.kind == projectile_kind::linear ||
            incoming.kind == projectile_kind::released_attack)
            impact_ms = std::max(0.0f, impact_ms - static_cast<float>(age));
        const int synthetic_index = synthetic_tower_for_real[incoming_index];
        const auto& damage_input = synthetic_index >= 0
            ? incoming_snapshot[static_cast<std::size_t>(synthetic_index)]
            : incoming;
        const float confidence = std::clamp(
            std::max(incoming.confidence, damage_input.confidence),
            0.0f, 1.0f);
        impact_ms = std::max(0.0f, impact_ms - (1.0f - confidence) * 75.0f);

        if (impact_ms > collection_horizon_ms)
            continue;

        if (!damage_input.damage_complete) {
            incomplete = true;
            continue;
        }

        float damage = conservative_damage(
            damage_input, armor, magic_resistance);
        damage *= 1.0f + (1.0f - confidence) * 0.75f;
        if (!(damage > 0.0f) || !std::isfinite(damage)) {
            incomplete = true;
            continue;
        }
        damage_event event{};
        event.time_ms = impact_ms;
        event.damage = damage;
        event.type = classify_damage(damage_input);
        event.source = source;
        event.confidence = confidence;
        event.kind = timeline_kind::incoming_damage;
        if (!queue_event(timeline, event_count, event))
            incomplete = true;
    }

    const ULONGLONG last_loss_at = g_monitor.observed_loss_at.load(
        std::memory_order_relaxed);
    const int loss_samples = g_monitor.observed_loss_samples.load(
        std::memory_order_relaxed);
    const int loss_interval_ms = g_monitor.observed_loss_interval_ms.load(
        std::memory_order_relaxed);
    const int loss_amount = g_monitor.observed_loss_amount.load(
        std::memory_order_relaxed);
    const bool observed_loss_is_self_cost =
        g_monitor.observed_loss_is_self_cost.load(std::memory_order_relaxed);

    // The observed-health fallback is meant for periodic damage whose modifier
    // cannot be decoded. Confirmed ordinary attacks are not a second DoT.
    // Match a landed contact and a sufficient damage bound, never an unresolved
    // future swing or merely the size of a hero-specific ability cost. Towers
    // retain a wider tolerance for their conservative travel estimate.
    bool observed_loss_matches_known_attack = false;
    if (last_loss_at != 0 && last_loss_at <= now) {
        for (const auto& incoming : incoming_snapshot) {
            const bool tower=incoming.kind==projectile_kind::tower_attack;
            const bool confirmed_melee=incoming.kind==projectile_kind::melee_animation ||
                incoming.kind==projectile_kind::instant_attack;
            if (!incoming.active || (!tower&&!confirmed_melee) ||
                incoming.impact_at==0 || now<incoming.updated_at ||
                now-incoming.updated_at>projectile_timeout_ms ||
                (confirmed_melee&&incoming.impact_at>now))continue;
            if(confirmed_melee && static_cast<float>(loss_amount)>
                conservative_damage(incoming,armor,magic_resistance)+5.f)continue;
            const ULONGLONG impact_tolerance_ms=tower?650:150;
            const ULONGLONG delta = last_loss_at >= incoming.impact_at
                ? last_loss_at - incoming.impact_at
                : incoming.impact_at - last_loss_at;
            if (delta <= impact_tolerance_ms) {
                observed_loss_matches_known_attack = true;
                break;
            }
        }
    }
    if (result.modifier_ticks == 0 && loss_samples >= 2 && loss_interval_ms >= 30 &&
        loss_interval_ms <= 2000 && last_loss_at <= now && now - last_loss_at < 1500 &&
        !observed_loss_is_self_cost && !observed_loss_matches_known_attack) {
        // Health samples also contain Armlet's own periodic drain. Treat only the
        // unexplained remainder as incoming damage; otherwise the fallback turns
        // the item's drain into a fake lethal hit exactly when the hero reaches
        // the normal 1 HP Armlet floor. One extra Armlet tick covers sampling on
        // either side of a drain boundary.
        const int armlet_drain_per_second = std::max(0,
            g_monitor.health_drain_per_second.load(std::memory_order_relaxed));
        const float armlet_tick_seconds = std::clamp(
            g_monitor.tick_interval.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float expected_armlet_drain =
            static_cast<float>(armlet_drain_per_second) *
            (static_cast<float>(loss_interval_ms) / 1000.0f + armlet_tick_seconds);
        const float unexplained_loss = std::max(
            0.0f, static_cast<float>(loss_amount) - expected_armlet_drain);
        if (unexplained_loss > 0.5f) {
            const bool observed_high_risk = loss_interval_ms <= 250;
            if (observed_high_risk) {
                high_risk_periodic = true;
                shortest_high_risk_interval_ms = std::min(
                    shortest_high_risk_interval_ms,
                    static_cast<float>(loss_interval_ms));
            }
            float next_loss = static_cast<float>(loss_interval_ms -
                static_cast<int>((now - last_loss_at) % loss_interval_ms));
            for (; next_loss <= collection_horizon_ms;
                 next_loss += loss_interval_ms) {
                damage_event event{};
                event.time_ms = std::max(0.0f, next_loss - 60.0f);
                event.damage = unexplained_loss * 1.45f;
                event.type = damage_type::mixed;
                event.confidence = 0.4f;
                event.kind = timeline_kind::incoming_damage;
                event.high_risk = observed_high_risk;
                if (!queue_event(timeline, event_count, event)) {
                    incomplete = true;
                    break;
                }
            }
        }
    }

    // Move only hostile events into the server-time coordinate system. Keep
    // negative timestamps: they are damage that can reach the server before
    // this order while still being absent from the delayed client HP sample.
    // OFF remains at t=0; ON recovery includes its acknowledgement gap.
    for (std::size_t index = 0; index < event_count; ++index) {
        if (timeline[index].kind == timeline_kind::incoming_damage)
            timeline[index].time_ms -= reaction_lag_ms;
    }

    // Equal timestamps are deterministic: health removal, a planned heal,
    // then real damage/strength ramp. The
    // heal is only inserted after a usable item has been resolved.
    std::sort(timeline.begin(), timeline.begin() + event_count,
        [](const damage_event& lhs, const damage_event& rhs) {
            if (lhs.time_ms != rhs.time_ms)
                return lhs.time_ms < rhs.time_ms;
            return timeline_priority(lhs.kind) < timeline_priority(rhs.kind);
        });

    const float max_health = static_cast<float>(
        g_monitor.max_health.load(std::memory_order_relaxed));
    // Accept the full range exposed by the UI and public API. Zero is a valid
    // diagnostic setting that shows the unbuffered timeline prediction.
    const float safety_margin = std::clamp(
        g_monitor.safety_margin.load(std::memory_order_relaxed), 0.0f, 500.0f);
    timeline_prediction prediction = simulate_timeline(timeline, event_count,
        static_cast<float>(health), max_health, armlet_health, health_added_ms);
    result.high_risk = high_risk_periodic;
    result.high_risk_interval_ms = std::isfinite(shortest_high_risk_interval_ms)
        ? shortest_high_risk_interval_ms : -1.0f;
    const bool allow_high_risk = g_monitor.allow_high_risk_abuse.load(
        std::memory_order_relaxed);
    bool modeled_lethal = !(prediction.minimum_hp > 0.0f) ||
        !(prediction.guarded_minimum_hp > safety_margin);

    std::array<instant_heal_candidate, 12> heal_candidates{};
    const std::size_t heal_count = collect_instant_heals(
        client_base, hero, heal_candidates);
    const instant_heal_candidate available_heal =
        heal_count > 0 ? heal_candidates[0] : instant_heal_candidate{};
    g_monitor.instant_heal_available.store(
        available_heal.kind, std::memory_order_relaxed);
    g_monitor.instant_heal_available_amount.store(
        static_cast<int>(std::lround(available_heal.heal)), std::memory_order_relaxed);
    g_monitor.instant_heal_available_charges.store(
        available_heal.charges, std::memory_order_relaxed);

    // An unknown enemy-cast modifier is not evidence of damage by itself.
    // Control auras, vision markers, attack-state helpers and illusion/clone
    // bookkeeping are all represented as modifiers too. The old blanket gate
    // kept Armlet blocked around towers and Monkey King's soldiers even when
    // the timeline contained no incoming damage. Unknown periodic damage is
    // still caught by the observed-health fallback above; known periodic
    // modifiers, projectiles and released attacks remain modeled explicitly.
    if (unknown_enemy_modifier) {
        static ULONGLONG next_unknown_modifier_log = 0;
        if (now >= next_unknown_modifier_log) {
            log_message("MODIFIER",
                "Unknown enemy modifier observed without proven damage; not blocking Armlet.");
            next_unknown_modifier_log = now + 2000;
        }
    }

    const bool needs_instant_heal = modeled_lethal ||
        (high_risk_periodic && !allow_high_risk);
    if (!incomplete && needs_instant_heal) {
        for (std::size_t index = 0; index < heal_count; ++index) {
            auto healed_timeline = timeline;
            std::size_t healed_count = event_count;
            if (!queue_event(healed_timeline, healed_count,
                    {reenable_gap_ms, heal_candidates[index].heal,
                        damage_type::pure,
                        heal_candidates[index].item, 1.0f,
                        timeline_kind::instant_heal}))
                continue;
            std::sort(healed_timeline.begin(), healed_timeline.begin() + healed_count,
                [](const damage_event& lhs, const damage_event& rhs) {
                    if (lhs.time_ms != rhs.time_ms)
                        return lhs.time_ms < rhs.time_ms;
                    return timeline_priority(lhs.kind) < timeline_priority(rhs.kind);
                });
            const timeline_prediction healed_prediction = simulate_timeline(
                healed_timeline, healed_count, static_cast<float>(health),
                max_health, armlet_health, health_added_ms);
            if (!(healed_prediction.minimum_hp > 0.0f) ||
                !(healed_prediction.guarded_minimum_hp > safety_margin))
                continue;
            result.instant_heal = heal_candidates[index];
            prediction = healed_prediction;
            event_count = healed_count;
            modeled_lethal = false;
            break;
        }
    }

    result.count = static_cast<int>(event_count);
    result.damage = prediction.damage;
    result.nearest_ms = prediction.nearest_damage;
    result.minimum_hp = prediction.minimum_hp;
    g_monitor.instant_heal_planned.store(
        result.instant_heal.kind, std::memory_order_relaxed);
    g_monitor.instant_heal_planned_amount.store(
        static_cast<int>(std::lround(result.instant_heal.heal)),
        std::memory_order_relaxed);
    if (incomplete) {
        result.state = safety_state::incomplete_event;
    } else if (high_risk_periodic && !allow_high_risk && !result.instant_heal) {
        result.state = safety_state::high_risk_blocked;
    } else if (modeled_lethal && !(allow_high_risk && high_risk_periodic &&
                   (!prediction.saw_incoming_damage ||
                       prediction.only_high_risk_damage))) {
        result.state = safety_state::lethal_cluster;
    } else {
        result.safe = true;
        result.state = result.instant_heal
            ? safety_state::safe_with_instant_heal
            : (high_risk_periodic ? safety_state::high_risk_allowed
                                  : safety_state::safe);
    }
    publish_safety(result, telemetry_ready);
    return result;
}

/**
 * @brief Публикует переход FSM и время входа в состояние; таймауты дальнейших шагов считаются
 * от этого момента.
 */
void enter_cycle_state(cycle_context& cycle, cycle_state next,
    clock_type::time_point now) {
    cycle.state = next;
    cycle.state_since = now;
    cycle.command_attempts = 0;

    g_monitor.cycle.store(next, std::memory_order_relaxed);

    char message[64]{};
    std::snprintf(message, sizeof(message), "FSM -> %s", cycle_text(next));
    log_message("FSM", message);
}

/**
 * @brief Ставит действие FSM через диспетчер с учётом повторов и временных ограничений;
 * подтверждение проверяется в последующих polling-проходах.
 */
bool issue_cycle_order(cycle_context& cycle,
    bool expected_current_toggle,
    clock_type::time_point now) {
    const auto armlet =
        g_monitor.armlet_address.load(std::memory_order_relaxed);

    const auto hero =
        g_monitor.hero_address.load(std::memory_order_relaxed);

    const int ability_index =
        g_monitor.armlet_entity_index.load(std::memory_order_relaxed);

    const instant_heal_candidate* heal =
        !expected_current_toggle && cycle.instant_heal
            ? &cycle.instant_heal : nullptr;
    if (!queue_direct_armlet_toggle(
            armlet,
            hero,
            ability_index,
            expected_current_toggle,
            heal)) {
        log_message(
            "ERROR",
            "Could not queue the direct Armlet CAST_TOGGLE order.");

        return false;
    }

    cycle.rearm_queued_at=expected_current_toggle?0:g_pending_order_queued_at.load(std::memory_order_relaxed);
    cycle.rearm_pause_ms=0;
    if(!expected_current_toggle&&cycle.command_attempts==0)
        cycle.health_before_enable=g_monitor.health.load(std::memory_order_relaxed);
    cycle.last_command=now;
    ++cycle.command_attempts;
    if (!expected_current_toggle)
        cycle.instant_heal = {};

    return true;
}

/**
 * @brief Ставит действие FSM через диспетчер с учётом повторов и временных ограничений;
 * подтверждение проверяется в последующих polling-проходах.
 */
bool issue_cycle_rearm(cycle_context& cycle,
    clock_type::time_point now) {
    const auto armlet =
        g_monitor.armlet_address.load(std::memory_order_relaxed);
    const auto hero =
        g_monitor.hero_address.load(std::memory_order_relaxed);
    const int ability_index =
        g_monitor.armlet_entity_index.load(std::memory_order_relaxed);
    const instant_heal_candidate* heal = cycle.instant_heal
        ? &cycle.instant_heal : nullptr;

    if (!queue_direct_armlet_rearm(
            armlet, hero, ability_index, heal, cycle.safety_evaluated_at)) {
        log_message("ERROR",
            "Could not queue the state-confirmed Armlet rearm.");
        return false;
    }

    cycle.rearm_queued_at=g_pending_order_queued_at.load(std::memory_order_relaxed);
    cycle.rearm_pause_ms=0;
    cycle.health_before_enable=g_monitor.health.load(std::memory_order_relaxed);
    cycle.last_command=now;
    ++cycle.command_attempts;
    // Keep the planned heal until the OFF -> ON continuation is submitted.
    return true;
}
