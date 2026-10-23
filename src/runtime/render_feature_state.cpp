/**
 * @file
 * @brief Переключение функций отрисовки.
 *
 * Связывает флаги меню/API с захватом частиц, очисткой снимков и видимостью native-окон.
 * Изменение флага может иметь побочные эффекты, поэтому вызывайте setter, а не только store в
 * атомарное поле.
 */

#include "render_feature_state.h"

namespace {

/**
 * @brief Согласует захват частиц с потребностями trace/teleport-функций; принудительная
 * видимость частиц управляется отдельно.
 */
void update_particle_capture() {
    // Trace markers also consume the particle name that explains which
    // ability/item animation coincided with the fog transition.
    const bool enabled = g_render_trace_icons.load(std::memory_order_relaxed) ||
        g_render_teleport_arrows.load(std::memory_order_relaxed) ||
        g_render_camp_status.load(std::memory_order_relaxed);
    render_teleportation::set_trace_capture_enabled(enabled);
}

void hide_minimap_when_unused() {
    const bool used = g_render_creep_tracker.load(std::memory_order_relaxed) || g_render_trace_icons.load(std::memory_order_relaxed) ||
        g_render_teleport_arrows.load(std::memory_order_relaxed) ||
        g_render_camp_status.load(std::memory_order_relaxed);
    if (!used && g_overlay.minimap_window != nullptr)
        ShowWindow(g_overlay.minimap_window, SW_HIDE);
}

} // namespace

bool set_particle_visibility_enabled(bool enabled) {
    return render_teleportation::set_enabled(enabled);
}

/**
 * @brief Изменяет флаг функции и обслуживает зависимый render-снимок/захват. Снятие флага не
 * должно оставлять старые отметки активными.
 */
void set_hero_trails_enabled(bool enabled) {
    g_render_trace_icons.store(enabled, std::memory_order_relaxed);
    if (!enabled&&!g_render_camp_status.load(std::memory_order_relaxed)) {
        render_intelligence_exclusive_guard guard;
        auto& state=g_render_intelligence;
        std::size_t kept=0;
        for(std::size_t i=0;i<state.trace_count;++i)
            if(state.traces[i].creep_wave)state.traces[kept++]=state.traces[i];
        state.trace_count=kept;
        g_render_trace_count_snapshot.store(static_cast<std::uint32_t>(kept), std::memory_order_release);
    }
    update_particle_capture();
    hide_minimap_when_unused();
}

/**
 * @brief Изменяет флаг функции и обслуживает зависимый render-снимок/захват. Снятие флага не
 * должно оставлять старые отметки активными.
 */
void set_creep_tracker_enabled(bool enabled) {
    g_render_creep_tracker.store(enabled,std::memory_order_relaxed);
    if(!enabled) {
        render_intelligence_exclusive_guard guard;
        auto& state=g_render_intelligence;
        std::size_t kept=0;
        for(std::size_t i=0;i<state.trace_count;++i)
            if(!state.traces[i].creep_wave)state.traces[kept++]=state.traces[i];
        state.trace_count=kept;
        g_render_trace_count_snapshot.store(static_cast<std::uint32_t>(kept),std::memory_order_release);
    }
    hide_minimap_when_unused();
}

void set_teleport_arrows_enabled(bool enabled) {
    g_render_teleport_arrows.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        render_intelligence_exclusive_guard guard;
        g_render_intelligence.teleport_count = 0;
        g_render_intelligence.enemy_teleport_count = 0;
        g_render_teleport_count_snapshot.store(0, std::memory_order_release);
    }
    update_particle_capture();
    hide_minimap_when_unused();
}

/**
 * @brief Изменяет флаг функции и обслуживает зависимый render-снимок/захват. Снятие флага не
 * должно оставлять старые отметки активными.
 */
void set_camp_status_enabled(bool enabled) {
    g_render_camp_status.store(enabled, std::memory_order_relaxed);
    {
        render_intelligence_exclusive_guard guard;
        if (enabled)
            g_render_intelligence.camps_refreshed_at = 0;
        else
            g_render_intelligence.camp_count = 0;
    }
    if (!enabled)
        g_render_camp_count_snapshot.store(0, std::memory_order_release);
    update_particle_capture();
    hide_minimap_when_unused();
}

void set_minimap_side_right(bool enabled) {
    g_render_minimap_right.store(enabled, std::memory_order_relaxed);
}

void set_trace_lifetime_seconds(int seconds) {
    g_render_trace_lifetime_seconds.store(
        std::clamp(seconds, 3, 30), std::memory_order_relaxed);
}

void set_hero_icon_scale_percent(int percent) {
    g_render_hero_icon_scale_percent.store(
        std::clamp(percent, 0, 100), std::memory_order_relaxed);
}

/**
 * @brief Сбрасывает функции, буферы и видимые окна при завершении; используется до
 * освобождения графических ресурсов.
 */
void reset_render_features() {
    set_creep_tracker_enabled(false);
    set_particle_visibility_enabled(false);
    g_render_trace_icons.store(false, std::memory_order_relaxed);
    g_render_teleport_arrows.store(false, std::memory_order_relaxed);
    g_render_camp_status.store(false, std::memory_order_relaxed);
    g_render_minimap_right.store(false, std::memory_order_relaxed);
    g_render_minimap_size_percent.store(100, std::memory_order_relaxed);
    g_render_minimap_offset_x.store(0, std::memory_order_relaxed);
    g_render_minimap_offset_y.store(0, std::memory_order_relaxed);
    g_render_trace_lifetime_seconds.store(10, std::memory_order_relaxed);
    g_render_hero_icon_scale_percent.store(50, std::memory_order_relaxed);
    render_teleportation::set_trace_capture_enabled(false);

    {
        render_intelligence_exclusive_guard guard;
        g_render_intelligence = {};
    }
    g_render_trace_count_snapshot.store(0, std::memory_order_release);
    g_render_teleport_count_snapshot.store(0, std::memory_order_release);
    g_render_camp_count_snapshot.store(0, std::memory_order_release);

    if (g_overlay.minimap_window != nullptr)
        ShowWindow(g_overlay.minimap_window, SW_HIDE);
    if (g_overlay.notification_window != nullptr)
        ShowWindow(g_overlay.notification_window, SW_HIDE);
}
