/**
 * @file
 * @brief Граница входа в runtime.
 *
 * Объявляет Win32 thread entry для dllmain.cpp. Детали общего unity-состояния через этот
 * заголовок не экспортируются.
 */

#pragma once

#include <Windows.h>

namespace askerdway::runtime {

/**
 * @brief Управляет жизненным циклом соответствующего runtime: ожидание зависимостей, polling и
 * остановка. В combined target игровой worker реализован в runtime_worker.cpp.
 */
DWORD WINAPI run(LPVOID module_handle);

} // namespace askerdway::runtime
