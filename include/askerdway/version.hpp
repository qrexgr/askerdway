/**
 * @file
 * @brief Версия продукта и ABI.
 *
 * Разделяет версию продукта и бинарного контракта. Проверка совместимости использует major-маску
 * ABI; обновление строки версии не делает изменённую раскладку совместимой.
 */

#pragma once

#include <cstdint>

// API and product versions deliberately advance independently. ABI-compatible
// implementation fixes only change the product version.
inline constexpr std::uint32_t ASKERDWAY_ABI_VERSION = 0x00010000u;
inline constexpr std::uint32_t ASKERDWAY_ABI_MAJOR_MASK = 0xFFFF0000u;
inline constexpr std::uint32_t ASKERDWAY_VERSION_MAJOR = 3;
inline constexpr std::uint32_t ASKERDWAY_VERSION_MINOR = 1;
inline constexpr std::uint32_t ASKERDWAY_VERSION_PATCH = 0;
