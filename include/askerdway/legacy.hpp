/**
 * @file
 * @brief Совместимость со старой телеметрией.
 *
 * Сохраняет старые точки входа, адаптирующие данные к текущему буферу событий. Для новой
 * интеграции предпочтителен проверяемый AskerdwayApi_* интерфейс.
 */

#pragma once

// Compatibility exports for older integrations.

#include <cstdint>

#include "askerdway/engine/types.hpp"
#include "askerdway/export.hpp"

ASKERDWAY_API void ArmletProjectile_BeginFrame(float network_latency_ms);
ASKERDWAY_API void ArmletTimeline_BeginFrame(float network_latency_ms);
ASKERDWAY_API void ArmletTimeline_SetSafetyMargin(float health);
ASKERDWAY_API void ArmletProjectile_AttackReleased(const void* attack_record);
ASKERDWAY_API void ArmletProjectile_Tracking(
    std::uint64_t projectile_id,
    const void* attack_record,
    armlet_vec3 current_position);
ASKERDWAY_API void ArmletProjectile_Linear(
    std::uint64_t projectile_id,
    std::uintptr_t source_entity,
    float original_damage,
    float physical_damage,
    float magical_damage,
    float pure_damage,
    float composite_damage,
    armlet_vec3 current_position,
    armlet_vec3 velocity,
    float projectile_radius,
    float remaining_distance);
ASKERDWAY_API void ArmletProjectile_Remove(std::uint64_t projectile_id);
ASKERDWAY_API void ArmletTimeline_DamageEvent(
    std::uint64_t event_id,
    std::uintptr_t source_entity,
    float delay_seconds,
    float original_damage,
    float physical_damage,
    float magical_damage,
    float pure_damage,
    float composite_damage,
    float confidence);
ASKERDWAY_API void ArmletTimeline_RemoveEvent(std::uint64_t event_id);
