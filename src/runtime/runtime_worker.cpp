/**
 * @file
 * @brief Жизненный цикл и планировщик.
 *
 * Ожидает игровые модули, инициализирует подсистемы, выполняет polling и отрисовку с разными
 * периодами. Завершение сначала отменяет работу и снимает хуки; при небезопасном снятии
 * particle-hook DLL остаётся загруженной. В конце расположены legacy-адаптеры.
 */

// Runtime worker and legacy API adapters.
#include "hotkey_edge.h"

/**
 * @brief Управляет жизненным циклом соответствующего runtime: ожидание зависимостей, polling и
 * остановка. В combined target игровой worker реализован в runtime_worker.cpp.
 */
DWORD WINAPI worker_thread(LPVOID parameter) {
    const auto module = static_cast<HMODULE>(parameter);
    open_console();
    comfort_runtime::load_settings();

    log_message("INFO", "Waiting for client.dll...");
    HMODULE client = nullptr;
    while (g_running.load(std::memory_order_relaxed) &&
        (client = GetModuleHandleW(L"client.dll")) == nullptr) {
        if ((GetAsyncKeyState(VK_END) & 1) != 0)
            g_running.store(false, std::memory_order_relaxed);
        Sleep(100);
    }

    if (!g_running.load(std::memory_order_relaxed)) {
        close_console();
        FreeLibraryAndExitThread(module, 0);
    }

    const auto client_base = reinterpret_cast<std::uintptr_t>(client);
    g_client_base_address.store(client_base, std::memory_order_relaxed);
    const auto client_size = image_size(client_base);
    if (client_size == 0)
        log_message("ERROR", "Could not read the client.dll image size.");
#if defined(ASKERDWAY_COMBINED)
    if (render_teleportation::install())
        log_message("INFO", "Teleport particle hook installed (disabled until enabled in menu).");
    else
        log_message("ERROR", "Teleport particle hook could not be installed.");
#endif
    if (initialize_schema_offsets())
        log_message("INFO", "ISchemaSystem resolved every required runtime field.");
    else
        log_message("ERROR", "ISchemaSystem is incomplete; automation remains fail-closed and will retry.");
    if (initialize_runtime_globals(client_base, client_size))
        log_message("INFO", "Runtime globals resolved dynamically without patch RVAs.");
    else
        log_message("ERROR", "Runtime global signatures failed; automation remains fail-closed.");
    world_frame::set_world_view_reader(&read_world_view_active);
    if (initialize_direct_order_path(client_base, client_size))
        log_message("INFO", "Direct C_DOTAPlayerController::PrepareUnitOrders path resolved.");
    else
        log_message("ERROR", "Direct PrepareUnitOrders signatures are unavailable; automation is blocked.");
    if (initialize_internal_projectile_feed(client_base, client_size))
        log_message("INFO", "Internal Dota projectile timeline resolved.");
    else
        log_message("ERROR", "Internal projectile manager signature is unavailable; safety remains fail-closed.");
    log_message("INFO", "client.dll found. Waiting for the game window...");

    while (g_running.load(std::memory_order_relaxed) && !create_overlay_window(module)) {
        if (g_overlay.window != nullptr || g_overlay.notification_window != nullptr ||
            g_overlay.minimap_window != nullptr ||
            g_overlay.device != nullptr)
            destroy_overlay(module);
        if ((GetAsyncKeyState(VK_END) & 1) != 0)
            g_running.store(false, std::memory_order_relaxed);
        Sleep(250);
    }

    // Present owns the shared MinHook lifecycle. Install Comfort only after overlay
    // creation/retries finish: a renderer initialization failure may uninitialize MinHook.
    if (g_running.load(std::memory_order_relaxed))
        comfort_runtime::install(client_base, client_size);

    if (g_running.load(std::memory_order_relaxed) &&
        install_game_window_order_dispatch(g_overlay.game_window))
        log_message("INFO", "Direct order dispatcher installed on the Dota window thread.");
    else if (g_running.load(std::memory_order_relaxed))
        log_message("ERROR", "Could not install the game-thread order dispatcher.");

    if (g_running.load(std::memory_order_relaxed))
        log_message("INFO", "ImGui overlay initialized.");

    if (g_running.load(std::memory_order_relaxed)) {
        log_message("INFO", g_view_matrix_address.load(std::memory_order_acquire) != 0
            ? "Native minimap/world renderer initialized; Panorama and JavaScript are disabled."
            : "Native minimap renderer initialized; world view-matrix signature is unavailable.");
    }

    bool gameplay_ready = false;
    hotkey_edge abuse_edge{};
    hotkey_edge unload_edge{};
    set_overlay_interactive(false);
    ShowWindow(g_overlay.window, SW_HIDE);
    poll_history history{};
    cycle_context cycle{};
    auto next_health_poll = clock_type::now();
    auto next_render_intelligence_poll = clock_type::now();
    auto next_native_render = clock_type::now();
    auto next_window_sync = clock_type::now();
    auto next_diagnostic_report = clock_type::now() + std::chrono::seconds(2);
    auto next_schema_retry = clock_type::now() + std::chrono::seconds(1);
    auto next_runtime_retry = clock_type::now() + std::chrono::seconds(1);
    MSG message{};

    while (g_running.load(std::memory_order_relaxed)) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT)
                g_running.store(false, std::memory_order_relaxed);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        const HWND foreground=GetForegroundWindow();
        // Focus owns input and external HWNDs. Native gameplay presentation
        // follows visible Dota frames even when another app has focus.
        bool game_is_foreground = foreground == g_overlay.game_window;
        bool game_is_active = game_is_foreground && !IsIconic(g_overlay.game_window);
        bool game_is_visible = IsWindowVisible(g_overlay.game_window) && !IsIconic(g_overlay.game_window);
        const bool world_view_active=read_world_view_active();

        const SHORT end_sample=GetAsyncKeyState(VK_END);
        if (unload_edge.pressed(VK_END,(end_sample&0x8000)!=0,game_is_active,
                (end_sample&1)!=0,g_game_text_input_active.load(std::memory_order_acquire) ||
                    g_bind_capture_active.load(std::memory_order_acquire))) {
            g_running.store(false, std::memory_order_relaxed);
            continue;
        }
        const auto insert_messages=g_game_insert_presses.exchange(0,std::memory_order_acq_rel);
        if (game_is_active && (insert_messages&1u)) {
            const bool menu_open = !g_native_menu_open.load(std::memory_order_relaxed);
            g_native_menu_epoch.fetch_add(1,std::memory_order_acq_rel);
            g_native_menu_open.store(menu_open,std::memory_order_release);
            if(!menu_open)g_bind_capture_active.store(false,std::memory_order_release);
            g_native_menu_mouse.store(false,std::memory_order_relaxed);
            g_native_menu_keyboard.store(false,std::memory_order_relaxed);
            g_native_menu_reset.store(true,std::memory_order_release);
            log_message("INFO", menu_open ? "ImGui menu opened." : "ImGui menu closed.");
        }

        const int configured_hotkey = g_monitor.abuse_hotkey.load(
            std::memory_order_relaxed);
        const SHORT key_sample=configured_hotkey!=0?GetAsyncKeyState(configured_hotkey):0;
        const bool hotkey_down = (key_sample&0x8000)!=0;
        const bool text_input = g_game_text_input_active.load(std::memory_order_acquire) ||
            g_native_menu_keyboard.load(std::memory_order_relaxed) ||
            g_bind_capture_active.load(std::memory_order_acquire);
        const auto comfort_presses = comfort_runtime::key_presses.exchange(0, std::memory_order_acq_rel);
        if (!text_input && game_is_active && (comfort_presses & 1u) &&
            GetForegroundWindow() == g_overlay.game_window) comfort_runtime::toggle_silence();
        comfort_runtime::poll();
        // Drain the process-global async latch, but only a focused window
        // message is evidence that this press belongs to the game.
        abuse_edge.pressed(configured_hotkey, hotkey_down,
            game_is_active,(key_sample&1)!=0,text_input);
        const auto abuse_messages=g_game_abuse_presses.exchange(0,std::memory_order_acq_rel);
        if (!text_input && game_is_active && (abuse_messages&1u) &&
            GetForegroundWindow()==g_overlay.game_window) {
                const bool next = !g_monitor.abuse_enabled.load(std::memory_order_relaxed);
                set_abuse_enabled(next, "hotkey");
        }

        const auto now = clock_type::now();
        if (now >= next_runtime_retry) {
            std::uintptr_t entity_system = 0;
            std::uintptr_t global_vars = 0;
            const bool runtime_globals_ready =
                read_game_entity_system(client_base, entity_system) &&
                read_global_vars(client_base, global_vars) &&
                g_game_rules_global.load(std::memory_order_acquire) != 0 &&
                g_local_player_pawn_global.load(std::memory_order_acquire) != 0;
            if (!runtime_globals_ready &&
                initialize_runtime_globals(client_base, client_size))
                log_message("INFO", "Runtime globals recovered by signature rescan.");

            if (g_prepare_unit_orders.load(std::memory_order_acquire) == 0 ||
                g_local_controller_accessor.load(std::memory_order_acquire) == 0) {
                if (initialize_direct_order_path(client_base, client_size))
                    log_message("INFO", "Direct order path recovered by signature rescan.");
            }
            if (g_monitor.abuse_enabled.load(std::memory_order_relaxed) &&
                g_projectile_manager_global.load(std::memory_order_acquire) == 0)
                initialize_internal_projectile_feed(client_base, client_size);
            if (!order_dispatch_installed())
                install_game_window_order_dispatch(g_overlay.game_window);
            next_runtime_retry = now + std::chrono::seconds(1);
        }

        if (now >= next_health_poll) {
            if (!g_monitor.schema_ready.load(std::memory_order_acquire)) {
                if (now >= next_schema_retry) {
                    if (initialize_schema_offsets())
                        log_message("INFO", "ISchemaSystem runtime fields are now ready.");
                    next_schema_retry = now + std::chrono::seconds(1);
                }
                if (!g_monitor.schema_ready.load(std::memory_order_acquire))
                    clear_runtime_state(armlet_status::read_error);
            } else {
                poll_armlet(client_base, client_size, history);
                if (plausible_pointer(g_monitor.hero_address.load(
                        std::memory_order_relaxed))) {
                    // The damage feeds are only consumed by Armlet automation.
                    // Scanning them every 5 ms while the feature is disabled
                    // wasted a full core in normal play. On enable they are
                    // refreshed in this same pass before the fail-closed FSM.
                    if (g_monitor.abuse_enabled.load(
                            std::memory_order_relaxed)) {
                        poll_internal_network_latency();
                        poll_internal_projectile_feed(client_base, client_size);
                        poll_internal_tower_attack_feed(client_base, client_size);
                        poll_internal_melee_attack_feed(client_base);
                    }
                    update_armlet_cycle(client_base, client_size, cycle);
                }
            }
            const bool hero_active = plausible_pointer(g_monitor.hero_address.load(
                std::memory_order_relaxed));
            const bool next_gameplay=hero_active&&read_gameplay_session_active();
            if(gameplay_ready&&!next_gameplay) {
                render_intelligence_exclusive_guard guard;
                g_render_intelligence={};
                render_teleportation::clear_observations();
                publish_render_intelligence_counts();
            }
            gameplay_ready=next_gameplay;
            render_teleportation::set_gameplay_context(gameplay_ready);
            const bool automation_active = g_monitor.abuse_enabled.load(
                std::memory_order_relaxed);
            next_health_poll = now + std::chrono::milliseconds(
                hero_active ? (automation_active ? 5 : 50) : 100);
        }

        if (now >= next_render_intelligence_poll) {
            std::uintptr_t render_entity_system = 0;
            const bool hero_active = plausible_pointer(g_monitor.hero_address.load(
                std::memory_order_relaxed));
            if (hero_active && gameplay_ready &&
                g_monitor.schema_ready.load(std::memory_order_acquire) &&
                read_game_entity_system(client_base, render_entity_system)) {
#if ASKERDWAY_DIAGNOSTICS
                const auto started = clock_type::now();
#endif
                poll_render_intelligence(render_entity_system);
#if ASKERDWAY_DIAGNOSTICS
                const auto elapsed = std::chrono::duration_cast<
                    std::chrono::microseconds>(clock_type::now() - started).count();
                g_render_diagnostics.intelligence_poll_us.store(
                    static_cast<std::uint32_t>(std::clamp<long long>(
                        elapsed, 0, 1000000)), std::memory_order_relaxed);
#endif
            } else {
                render_teleportation::set_trace_capture_enabled(false);
            }
            next_render_intelligence_poll = now + std::chrono::milliseconds(
                hero_active ? 50 : 250);
        }

        if (now >= next_window_sync) {
            if (!IsWindow(g_overlay.game_window)) {
                log_message("ERROR", "The game window was closed.");
                g_running.store(false, std::memory_order_relaxed);
                continue;
            }
            if (!resize_and_position_overlay()) {
                game_is_foreground = false;
                game_is_active = false;
                game_is_visible = false;
            }
            next_window_sync = now + std::chrono::milliseconds(250);
        }

        if (now >= next_native_render) {
#if ASKERDWAY_DIAGNOSTICS
            const auto started = clock_type::now();
#endif
            const bool hero_active = plausible_pointer(g_monitor.hero_address.load(
                std::memory_order_relaxed));
            const bool gameplay_visible = game_is_active && gameplay_ready && hero_active && world_view_active;
            draw_render_minimap_overlay(gameplay_visible);
#if ASKERDWAY_DIAGNOSTICS
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::microseconds>(clock_type::now() - started).count();
            g_render_diagnostics.native_render_us.store(
                static_cast<std::uint32_t>(std::clamp<long long>(
                    elapsed, 0, 1000000)), std::memory_order_relaxed);
#endif
            const bool active_route =
                g_render_teleport_count_snapshot.load(
                    std::memory_order_relaxed) != 0;
            const bool active_static_overlay =
                g_render_trace_count_snapshot.load(
                    std::memory_order_relaxed) != 0 ||
                g_render_camp_count_snapshot.load(
                    std::memory_order_relaxed) != 0;
            next_native_render = now + std::chrono::milliseconds(
                !gameplay_visible ? 250
                : active_route ? 100
                : active_static_overlay ? 200
                : 250);
        }

        // Publish events/cached tiles; the game Present callback projects them
        // against the current matrix at the actual game-frame cadence.
        // Hide external windows on every worker pass, not only the minimap's
        // 100-250ms drawing cadence. Background Dota still has WS_VISIBLE.
        if(!game_is_active||!gameplay_ready||!world_view_active) {
            ShowWindow(g_overlay.minimap_window,SW_HIDE);
            ShowWindow(g_overlay.notification_window,SW_HIDE);
        }
        draw_render_world_traces(game_is_visible && gameplay_ready && world_view_active && plausible_pointer(
            g_monitor.hero_address.load(std::memory_order_relaxed)));

#if ASKERDWAY_DIAGNOSTICS
        if (now >= next_diagnostic_report) {
            if (g_monitor.debug_logging.load(std::memory_order_relaxed)) {
                char summary[512]{};
                std::snprintf(summary, sizeof(summary),
                    "scan=%uus render=%uus particles=%llu resolved=%llu "
                    "fog=%llu farm=%llu tp=%llu/%llu towers=%d/%d shots=%llu "
                    "camps=%u blocked=%d world=%d matrix=%s",
                    g_render_diagnostics.intelligence_poll_us.load(
                        std::memory_order_relaxed),
                    g_render_diagnostics.native_render_us.load(
                        std::memory_order_relaxed),
                    static_cast<unsigned long long>(
                        g_render_diagnostics.particle_observations.load(
                            std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        g_render_diagnostics.resolved_particle_owners.load(
                            std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        g_render_diagnostics.fog_ability_events.load(
                            std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        g_render_diagnostics.fog_farming_events.load(
                            std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        g_render_diagnostics.teleport_origins.load(
                            std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        g_render_diagnostics.teleport_routes.load(
                            std::memory_order_relaxed)),
                    g_render_diagnostics.cached_towers.load(
                        std::memory_order_relaxed),
                    g_render_diagnostics.targeted_towers.load(
                        std::memory_order_relaxed),
                    static_cast<unsigned long long>(
                        g_render_diagnostics.tower_shots.load(
                            std::memory_order_relaxed)),
                    g_render_camp_count_snapshot.load(std::memory_order_relaxed),
                    g_render_diagnostics.blocked_camps.load(
                        std::memory_order_relaxed),
                    g_render_diagnostics.visible_world_markers.load(
                        std::memory_order_relaxed),
                    g_render_diagnostics.view_matrix_ready.load(
                        std::memory_order_relaxed) ? "ready" : "missing");
                log_message("METRIC", summary);
            }
            next_diagnostic_report = now + std::chrono::seconds(2);
        }
#endif

        // ImGui is drawn only by Dota's Present hook. The discovery window
        // remains hidden throughout its lifetime, including Alt+Tab.
        update_enable_notification(game_is_active&&gameplay_ready);
        // Pump input without busy-spinning or coupling menu FPS to vsync.
        Sleep(g_monitor.abuse_enabled.load(std::memory_order_relaxed)?2:8);
    }

    log_message("INFO", "Shutting down.");
    g_monitor.abuse_enabled.store(false, std::memory_order_relaxed);
    cancel_queued_armlet_order();
    reset_render_features();
    const bool comfort_safe_to_unload = comfort_runtime::shutdown();
    if (!comfort_safe_to_unload) {
        log_message("ERROR", "Comfort callback is still active; module will remain loaded for safety.");
        return 0;
    }
    const bool world_safe_to_unload=world_frame::shutdown();
    if (!world_safe_to_unload)
        log_message("ERROR","Present hook still active; module will remain loaded.");
#if defined(ASKERDWAY_COMBINED)
    const bool teleport_safe_to_unload = render_teleportation::shutdown();
    if (!teleport_safe_to_unload || !world_safe_to_unload)
        log_message("ERROR", "Teleport hook is still active; module will remain loaded for safety.");
#endif
    // A live callback may still access all of these resources. Do not destroy
    // them or unload code when either hook could not be detached safely.
    if(!world_safe_to_unload)return 0;
#if defined(ASKERDWAY_COMBINED)
    if(!teleport_safe_to_unload)return 0;
#endif
    g_game_entity_system_global.store(0, std::memory_order_release);
    g_global_vars_global.store(0, std::memory_order_release);
    g_game_rules_global.store(0, std::memory_order_release);
    g_local_player_pawn_global.store(0, std::memory_order_release);
    g_prepare_unit_orders.store(0, std::memory_order_release);
    g_local_controller_accessor.store(0, std::memory_order_release);
    g_local_controller_global.store(0, std::memory_order_release);
    g_local_controller_address.store(0, std::memory_order_release);
    g_projectile_manager_global.store(0, std::memory_order_release);
    g_view_matrix_address.store(0, std::memory_order_release);
    g_entity_chunk_table_offset.store(0, std::memory_order_release);
    g_entity_entry_stride.store(0, std::memory_order_release);
    g_modifier_count_offset.store(0, std::memory_order_release);
    g_modifier_data_offset.store(0, std::memory_order_release);
    g_client_base_address.store(0, std::memory_order_relaxed);
    destroy_overlay(module);
    close_console();
#if defined(ASKERDWAY_COMBINED)
    if (!teleport_safe_to_unload)
        return 0;
#endif
    FreeLibraryAndExitThread(module, 0);
}

} // namespace

namespace askerdway::runtime {

/**
 * @brief Управляет жизненным циклом соответствующего runtime: ожидание зависимостей, polling и
 * остановка. В combined target игровой worker реализован в runtime_worker.cpp.
 */
DWORD WINAPI run(LPVOID module_handle) {
    return worker_thread(module_handle);
}

} // namespace askerdway::runtime

void ArmletProjectile_BeginFrame(float network_latency_ms) {
    projectile_heartbeat(network_latency_ms);
}

void ArmletTimeline_BeginFrame(float network_latency_ms) {
    projectile_heartbeat(network_latency_ms);
}

void ArmletTimeline_SetSafetyMargin(float health) {
    // Keep the public API in lockstep with the on-screen control.
    if (std::isfinite(health) && health >= 0.0f && health <= 500.0f)
        g_monitor.safety_margin.store(health, std::memory_order_relaxed);
}

void ArmletProjectile_AttackReleased(const void* attack_record) {
    ingest_released_attack(attack_record);
}

void ArmletProjectile_Tracking(std::uint64_t projectile_id,
    const void* attack_record, armlet_vec3 current_position) {
    ingest_tracking_projectile(projectile_id, attack_record, current_position);
}

void ArmletProjectile_Linear(std::uint64_t projectile_id,
    std::uintptr_t source_entity, float original_damage, float physical_damage,
    float magical_damage, float pure_damage, float composite_damage,
    armlet_vec3 current_position, armlet_vec3 velocity, float projectile_radius,
    float remaining_distance) {
    ingest_linear_projectile(projectile_id, source_entity, original_damage,
        physical_damage, magical_damage, pure_damage, composite_damage,
        current_position, velocity, projectile_radius, remaining_distance);
}

void ArmletProjectile_Remove(std::uint64_t projectile_id) {
    remove_projectile(projectile_id);
}

void ArmletTimeline_DamageEvent(std::uint64_t event_id,
    std::uintptr_t source_entity, float delay_seconds, float original_damage,
    float physical_damage, float magical_damage, float pure_damage,
    float composite_damage, float confidence) {
    ingest_external_damage(event_id, source_entity, delay_seconds, original_damage,
        physical_damage, magical_damage, pure_damage, composite_damage, confidence);
}

void ArmletTimeline_RemoveEvent(std::uint64_t event_id) {
    remove_projectile(event_id);
}
