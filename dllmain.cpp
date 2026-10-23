/**
 * @file
 * @brief Точка входа DLL.
 *
 * Обрабатывает DLL_PROCESS_ATTACH и запускает отдельный worker. Тяжёлая инициализация вынесена
 * из DllMain; дескриптор созданного потока закрывается здесь, сам поток продолжает работу.
 */

#include <Windows.h>

#include "src/runtime/runtime_worker.h"

/**
 * @brief При attach запускает worker и быстро возвращает управление загрузчику; ожидание игры
 * и создание интерфейса выполняются уже в worker.
 */
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(module);
    const HANDLE worker = CreateThread(
        nullptr, 0, askerdway::runtime::run, module, 0, nullptr);
    if (worker != nullptr)
        CloseHandle(worker);

    return TRUE;
}
