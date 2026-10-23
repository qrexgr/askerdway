/**
 * @file
 * @brief Автомат состояний Armlet.
 *
 * Один вызов выполняет один шаг FSM. Переходы учитывают актуальность данных, подтверждение
 * toggle/модификатора, восстановление здоровья, лимиты повторов и паузу. Не заменять
 * подтверждение состояния успешным PostMessageW.
 */

// Armlet state machine.

/**
 * @brief Выполняет один шаг автомата idle/on/disabling/off/enabling. Пауза останавливает
 * действия и корректирует monotonic-дедлайны после возобновления.
 */
void update_armlet_cycle(std::uintptr_t client_base,
    std::size_t client_size,
    cycle_context& cycle) {
    constexpr int health_threshold = 300;
    constexpr int max_command_attempts = 3;

    constexpr auto modifier_confirmation_timeout =
        std::chrono::milliseconds(750);

    constexpr ULONGLONG strength_ramp_ms = 650;

    constexpr auto disable_phase_timeout =
        std::chrono::milliseconds(2200);

    constexpr auto failure_backoff =
        std::chrono::milliseconds(750);

    constexpr auto emergency_reenable_retry =
        std::chrono::milliseconds(150);

    const auto now = clock_type::now();
    const auto command_delay = armlet_command_delay();
    const auto retry_delay = std::max(
        command_delay + std::chrono::milliseconds(25),
        std::chrono::milliseconds(125));

    bool game_paused = g_monitor.game_paused.load(std::memory_order_relaxed);
    if (read_game_paused(client_base, game_paused))
        g_monitor.game_paused.store(game_paused, std::memory_order_relaxed);
    if (game_paused) {
        g_pending_order_state.store(
            pending_order_state::empty, std::memory_order_release);
        if (!cycle.pause_latched) {
            cycle.pause_latched = true;
            cycle.pause_started = now;
            log_message("PAUSE", "Game paused; Armlet FSM and its timers are suspended.");
        }
        g_monitor.safety.store(
            safety_state::game_paused, std::memory_order_relaxed);
        return;
    }
    if (cycle.pause_latched) {
        const auto paused_for = now - cycle.pause_started;
        if(cycle.rearm_queued_at != 0)
            cycle.rearm_pause_ms += static_cast<ULONGLONG>(
                std::chrono::duration_cast<std::chrono::milliseconds>(paused_for).count());
        if (cycle.state_since.time_since_epoch().count() != 0)
            cycle.state_since += paused_for;
        if (cycle.last_command.time_since_epoch().count() != 0)
            cycle.last_command += paused_for;
        if (cycle.retry_after.time_since_epoch().count() != 0)
            cycle.retry_after += paused_for;
        const bool order_still_needed =
            (cycle.state == cycle_state::enabling &&
                !g_monitor.toggle_state.load(std::memory_order_relaxed)) ||
            (cycle.state == cycle_state::disabling &&
                g_monitor.toggle_state.load(std::memory_order_relaxed));
        if (order_still_needed) {
            // A queued window message may have been cancelled by the pause.
            // Make the first post-resume pass retry it immediately.
            cycle.last_command = {};
            cycle.command_attempts = 0;
        }
        cycle.pause_latched = false;
        cycle.pause_started = {};
        log_message("PAUSE", "Game resumed; Armlet FSM timers were restored.");
    }

    const bool script_enabled =
        g_monitor.abuse_enabled.load(std::memory_order_relaxed);

    const bool item_found =
        g_monitor.item_found.load(std::memory_order_relaxed);

    const bool ready =
        g_monitor.automation_ready.load(std::memory_order_relaxed);

    const bool toggle =
        g_monitor.toggle_state.load(std::memory_order_relaxed);

    const bool effect =
        g_monitor.effect_confirmed.load(std::memory_order_relaxed);

    const int health =
        g_monitor.health.load(std::memory_order_relaxed);

    const bool observed_on = toggle && effect;
    const bool observed_off = !toggle && !effect;

    if (!item_found || health <= 0) {
        cancel_queued_armlet_order();
        g_monitor.safety.store(safety_state::unavailable,std::memory_order_relaxed);
        cycle = {};

        g_monitor.cycle.store(
            cycle_state::idle,
            std::memory_order_relaxed);

        return;
    }

    if (!script_enabled) {
        // Stop at the current real state.  Do not complete or roll back an
        // automatic OFF/ON transition after the user disables automation.
        cancel_queued_armlet_order();
        cycle.cycle_armed = false;
        cycle.instant_heal = {};
        cycle.retry_after = {};
        cycle.rearm_queued_at = 0;
        cycle.rearm_pause_ms = 0;

        g_monitor.safety.store(
            safety_state::abuse_disabled,
            std::memory_order_relaxed);

        if (observed_on && cycle.state != cycle_state::on) {
            enter_cycle_state(
                cycle,
                cycle_state::on,
                now);
        } else if (observed_off &&
            cycle.state != cycle_state::off) {
            enter_cycle_state(
                cycle,
                cycle_state::off,
                now);
        } else if (!observed_on &&
            !observed_off &&
            cycle.state != cycle_state::idle) {
            enter_cycle_state(
                cycle,
                cycle_state::idle,
                now);
        }

        return;
    }

    if (cycle.state == cycle_state::idle) {
        if (observed_on) {
            enter_cycle_state(
                cycle,
                cycle_state::on,
                now);
        } else if (observed_off) {
            enter_cycle_state(
                cycle,
                cycle_state::off,
                now);
        }

        return;
    }

    switch (cycle.state) {
    case cycle_state::idle:
        break;

    case cycle_state::on: {
        /*
         * Armlet был выключен вручную или внешней командой.
         */
        if (observed_off) {
            cycle.cycle_armed = false;
            cycle.instant_heal = {};

            enter_cycle_state(
                cycle,
                cycle_state::off,
                now);

            break;
        }

        if (!observed_on)
            break;

        const safety_result safety =
            evaluate_timeline_safety(
                client_base,
                client_size,
                cycle);

        /*
         * Правило:
         * Armlet включён;
         * HP <= 300;
         * атаки и способности безопасны;
         * выключаем Armlet.
         */
        if (health <= health_threshold &&
            ready &&
            safety.safe &&
            now >= cycle.retry_after &&
            now - cycle.state_since >= command_delay) {
            cycle.cycle_armed = true;
            cycle.instant_heal = safety.instant_heal;
            cycle.safety_evaluated_at = safety.evaluated_at;

            enter_cycle_state(
                cycle,
                cycle_state::disabling,
                now);

            // Observe actual OFF before issuing the inverse command. The
            // dispatcher can complete both steps immediately only if OFF was
            // applied synchronously by native prediction.
            if (!issue_cycle_rearm(cycle, now)) {
                cycle.cycle_armed = false;
                cycle.health_before_enable = -1;
                cycle.retry_after = now + failure_backoff;

                enter_cycle_state(
                    cycle,
                    cycle_state::on,
                    now);

                g_monitor.safety.store(
                    safety_state::command_unavailable,
                    std::memory_order_relaxed);
            }
        }

        break;
    }

    case cycle_state::disabling: {
        const auto started=g_last_rearm_started_at.load(std::memory_order_acquire);
        const auto submitted=g_last_rearm_submitted_at.load(std::memory_order_acquire);
        const bool off_sent=cycle.rearm_queued_at!=0 && started>=cycle.rearm_queued_at;
        const bool on_sent=cycle.rearm_queued_at!=0 && submitted>=cycle.rearm_queued_at;
        if(on_sent) {
            cycle.instant_heal={};
            enter_cycle_state(cycle,cycle_state::enabling,now);
            cycle.last_command=now;
            cycle.command_attempts=1;
            break;
        }
        const auto pending=g_pending_order_state.load(std::memory_order_acquire);
        // The toggle is authoritative for the inverse order. Waiting for the
        // OFF modifier to disappear used to add up to 750ms at the HP floor.
        if(!toggle) {
            // The same callback may still be completing the synchronous path.
            // Do not turn a temporary busy slot into a 125ms recovery delay.
            if(pending!=pending_order_state::empty)break;
            enter_cycle_state(cycle,cycle_state::enabling,now);
            if(ready && !issue_cycle_order(cycle,false,now))
                g_monitor.safety.store(safety_state::command_unavailable,std::memory_order_relaxed);
            break;
        }
        if((!off_sent && pending==pending_order_state::empty) ||
            now-cycle.state_since>=disable_phase_timeout) {
            // Never retry an unconfirmed OFF: a delayed first command could
            // otherwise undo a later ON. Re-evaluate while the item stays ON.
            cycle.cycle_armed=false;cycle.instant_heal={};cycle.rearm_queued_at=0;
            cycle.retry_after=now;
            enter_cycle_state(cycle,cycle_state::on,now);
        }
        break;
    }

    case cycle_state::off: {
        /*
         * Armlet уже включился.
         */
        if (observed_on) {
            cycle.cycle_armed = false;
            cycle.retry_after = {};

            enter_cycle_state(
                cycle,
                cycle_state::on,
                now);

            break;
        }

        /*
         * Включить Armlet необходимо в двух случаях:
         *
         * 1. FSM сама выключила Armlet.
         * 2. Armlet уже был выключен, а HP <= 300.
         */
        const bool should_enable =
            cycle.cycle_armed ||
            health <= health_threshold;

        if (!should_enable || !ready || now < cycle.retry_after)
            break;

        /*
         * Armlet полностью выключен.
         * Ждём один безопасный интервал переключения.
         */
        const bool automatic_reenable = cycle.cycle_armed;
        if (observed_off &&
            (automatic_reenable || now - cycle.state_since >= command_delay)) {
            cycle.cycle_armed = true;

            enter_cycle_state(
                cycle,
                cycle_state::enabling,
                now);

            /*
             * expected_current_toggle == false:
             * перед командой Armlet должен быть выключен.
             */
            if (!issue_cycle_order(cycle, false, now)) {
                cycle.retry_after = now + emergency_reenable_retry;

                enter_cycle_state(
                    cycle,
                    cycle_state::off,
                    now);

                g_monitor.safety.store(
                    safety_state::command_unavailable,
                    std::memory_order_relaxed);
            }

            break;
        }

        /*
         * Toggle уже OFF, но модификатор завис в памяти.
         * Всё равно пытаемся включить Armlet.
         */
        if (!toggle &&
            now - cycle.state_since >=
                modifier_confirmation_timeout) {
            cycle.cycle_armed = true;

            log_message(
                "ERROR",
                "Armlet remained OFF with stale modifier; forcing ON.");

            enter_cycle_state(
                cycle,
                cycle_state::enabling,
                now);

            if (!issue_cycle_order(cycle, false, now)) {
                g_monitor.safety.store(
                    safety_state::command_unavailable,
                    std::memory_order_relaxed);
            }
        }

        break;
    }

    case cycle_state::enabling:
        /*
         * Включение полностью подтверждено.
         */
        if (observed_on) {
            if (cycle.rearm_queued_at != 0) {
                const auto pending = g_pending_order_state.load(std::memory_order_acquire);
                const auto submitted = g_last_rearm_submitted_at.load(std::memory_order_acquire);
                const auto tick = GetTickCount64();
                if (submitted < cycle.rearm_queued_at &&
                    pending == pending_order_state::empty) {
                    // Dispatch rejected a stale plan without touching the
                    // item. Re-evaluate next poll instead of waiting 2200 ms.
                    cycle.rearm_queued_at = 0;
                    cycle.cycle_armed = false;
                    cycle.instant_heal = {};
                    cycle.retry_after = now;
                    enter_cycle_state(cycle, cycle_state::on, now);
                    break;
                }
                // Damage can consume every gained HP between samples. Wait for
                // the actually dispatched ON and the strength ramp, not an HP
                // increase that may never be observable under sustained hits.
                if (submitted < cycle.rearm_queued_at || tick < submitted ||
                    tick - submitted < strength_ramp_ms + cycle.rearm_pause_ms) {
                    if (now - cycle.state_since < disable_phase_timeout)break;
                    cycle.rearm_queued_at = 0;
                    cycle.cycle_armed = false;
                    cycle.retry_after = now + failure_backoff;
                    enter_cycle_state(cycle, cycle_state::on, now);
                    break;
                }
            }
            cycle.cycle_armed = false;
            cycle.health_before_enable = -1;
            cycle.rearm_queued_at = 0;
            cycle.retry_after = now;

            enter_cycle_state(
                cycle,
                cycle_state::on,
                now);

            break;
        }

        /*
         * Toggle уже ON, но модификатор ещё применяется.
         * Повторную команду отправлять нельзя.
         */
        if (toggle) {
            if (now - cycle.state_since >=
                modifier_confirmation_timeout) {
                cycle.cycle_armed = false;

                enter_cycle_state(
                    cycle,
                    cycle_state::on,
                    now);

                g_monitor.safety.store(
                    safety_state::state_unconfirmed,
                    std::memory_order_relaxed);

                log_message(
                    "ERROR",
                    "Armlet ON modifier confirmation timed out.");
            }

            break;
        }

        /*
         * После нескольких неудачных команд выдерживаем паузу.
         */
        if (cycle.command_attempts >= max_command_attempts &&
            now - cycle.last_command >=
                std::max(command_delay, retry_delay)) {
            cycle.command_attempts = 0;
            cycle.retry_after = now + emergency_reenable_retry;

            g_monitor.safety.store(
                safety_state::command_unavailable,
                std::memory_order_relaxed);

            log_message(
                "ERROR",
                "Armlet ON recovery is backing off.");

            break;
        }

        /*
         * Повтор команды включения.
         */
        if (ready &&
            now >= cycle.retry_after &&
            now - cycle.last_command >=
                std::max(command_delay, retry_delay)) {
            if (!issue_cycle_order(cycle, false, now)) {
                g_monitor.safety.store(
                    safety_state::command_unavailable,
                    std::memory_order_relaxed);
            }
        }

        break;
    }
}
