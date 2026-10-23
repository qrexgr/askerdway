/**
 * @file
 * @brief Интерфейс управления отрисовкой.
 *
 * Объявляет операции включения, масштабирования и сброса render-функций. Реализация обслуживает
 * связанные буферы и окна.
 */

#pragma once

bool set_particle_visibility_enabled(bool enabled);
/**
 * @brief Изменяет флаг функции и обслуживает зависимый render-снимок/захват. Снятие флага не
 * должно оставлять старые отметки активными.
 */
void set_hero_trails_enabled(bool enabled);
void set_creep_tracker_enabled(bool enabled);
/**
 * @brief Изменяет флаг функции и обслуживает зависимый render-снимок/захват. Снятие флага не
 * должно оставлять старые отметки активными.
 */
void set_teleport_arrows_enabled(bool enabled);
/**
 * @brief Изменяет флаг функции и обслуживает зависимый render-снимок/захват. Снятие флага не
 * должно оставлять старые отметки активными.
 */
void set_camp_status_enabled(bool enabled);
void set_minimap_side_right(bool enabled);
void set_trace_lifetime_seconds(int seconds);
void set_hero_icon_scale_percent(int percent);
/**
 * @brief Сбрасывает функции, буферы и видимые окна при завершении; используется до
 * освобождения графических ресурсов.
 */
void reset_render_features();
