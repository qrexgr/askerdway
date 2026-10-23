/**
 * @file
 * @brief Общий публичный заголовок.
 *
 * Объединяет поддерживаемые публичные заголовки. Внешнему потребителю не требуется включать
 * внутренние runtime-файлы.
 */

#pragma once

// Backward-compatible umbrella header.
//
// Existing projects may continue to include "askerdway.hpp". New projects can
// include only the smaller module they use, for example:
//   #include <askerdway/api.hpp>
//   #include <askerdway/engine/types.hpp>

#include "include/askerdway/api.hpp"
#include "include/askerdway/dota_api.hpp"
#include "include/askerdway/dota_sdk.hpp"
#include "include/askerdway/legacy.hpp"
#include "include/askerdway/render.hpp"
