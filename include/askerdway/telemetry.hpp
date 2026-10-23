/**
 * @file
 * @brief Внешние события урона.
 *
 * Описывает запланированный урон и линейные снаряды. Задержка события задаётся в секундах,
 * задержка сети в BeginTelemetryFrame — в миллисекундах. Владелец переданных структур остаётся
 * вызывающей стороной.
 */

#pragma once

#include <cstdint>

#include "askerdway/engine/types.hpp"
#include "askerdway/version.hpp"

#pragma pack(push, 8)

// Scheduled external damage.
struct askerdway_damage_event_v1 {
    std::uint32_t struct_size = sizeof(askerdway_damage_event_v1);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    // Ненулевой ID для последующего обновления/удаления события.
    std::uint64_t event_id = 0;
    std::uint64_t source_entity = 0;
    // Относительная задержка попадания: 0..10 секунд, не абсолютный game time.
    float delay_seconds = 0.0f;
    askerdway_damage_components damage{};
    // Уверенность источника 0..1; не является вероятностью безопасного переключения.
    float confidence = 1.0f;
    std::uint32_t reserved[4]{};
};

// External linear projectile data.
struct askerdway_linear_projectile_v1 {
    std::uint32_t struct_size = sizeof(askerdway_linear_projectile_v1);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    std::uint64_t projectile_id = 0;
    std::uint64_t source_entity = 0;
    askerdway_vec3 current_position{};
    // Мировые единицы в секунду; длина/радиус используют те же мировые единицы.
    askerdway_vec3 velocity{};
    float projectile_radius = 0.0f;
    float remaining_distance = 0.0f;
    askerdway_damage_components damage{};
    std::uint32_t reserved[4]{};
};

#pragma pack(pop)
