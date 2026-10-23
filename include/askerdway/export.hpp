/**
 * @file
 * @brief Экспорт символов.
 *
 * Централизует соглашение об экспорте публичных функций DLL. Не переносите платформенные
 * атрибуты в каждое объявление вручную.
 */

#pragma once

// Public symbol visibility.
#if defined(_WIN32)
#define ASKERDWAY_API extern "C" __declspec(dllexport)
#else
#define ASKERDWAY_API extern "C"
#endif
