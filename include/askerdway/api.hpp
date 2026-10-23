/**
 * @file
 * @brief Публичные функции DLL.
 *
 * Точка входа для конфигурации, диагностики, снимков и телеметрии. Для v1-структур перед вызовом
 * нужны struct_size и abi_version; массивные Read-функции используют capacity и written.
 * Контракты описаны в docs/API_REFERENCE_RU.md.
 */

#pragma once

// Public API.

#include <cstddef>
#include <cstdint>

#include "askerdway/config.hpp"
#include "askerdway/comfort.hpp"
#include "askerdway/engine/runtime.hpp"
#include "askerdway/engine/schema.hpp"
#include "askerdway/engine/types.hpp"
#include "askerdway/export.hpp"
#include "askerdway/status.hpp"
#include "askerdway/render_snapshot.hpp"
#include "askerdway/telemetry.hpp"

enum askerdway_result : std::int32_t {
    ASKERDWAY_OK = 0,
    ASKERDWAY_INVALID_ARGUMENT = 1,
    ASKERDWAY_INCOMPATIBLE_ABI = 2,
    ASKERDWAY_BUFFER_TOO_SMALL = 3,
    ASKERDWAY_NOT_READY = 4,
    ASKERDWAY_SHUTTING_DOWN = 5,
    ASKERDWAY_VALUE_OUT_OF_RANGE = 6,
};

#pragma pack(push, 8)
struct askerdway_build_info_v1 {
    std::uint32_t struct_size = sizeof(askerdway_build_info_v1);
    std::uint32_t abi_version = ASKERDWAY_ABI_VERSION;
    std::uint32_t version_major = ASKERDWAY_VERSION_MAJOR;
    std::uint32_t version_minor = ASKERDWAY_VERSION_MINOR;
    std::uint32_t version_patch = ASKERDWAY_VERSION_PATCH;
    std::uint32_t reserved_number = 0;
    char product[32]{};
    char build_date[24]{};
    char build_time[16]{};
    char compiler[32]{};
    char compatibility_note[96]{};
};
#pragma pack(pop)

/**
 * @brief Возвращает числовую версию бинарного интерфейса.
 */
ASKERDWAY_API std::uint32_t AskerdwayApi_GetAbiVersion() noexcept;
/**
 * @brief Возвращает битовую маску реализованных возможностей; готовность в текущей игре
 * проверяется отдельно через GetStatus.
 */
ASKERDWAY_API std::uint64_t AskerdwayApi_GetCapabilities() noexcept;
/**
 * @brief Возвращает статическую строку результата; память не освобождается вызывающим кодом.
 */
ASKERDWAY_API const char* AskerdwayApi_ResultString(
    askerdway_result result) noexcept;

// SetConfig applies fields selected by update_mask.
/**
 * @brief Заполняет всю структуру значениями по умолчанию, включая размер и ABI. Передайте
 * доступный буфер размером не меньше sizeof(askerdway_config_v1).
 */
ASKERDWAY_API askerdway_result AskerdwayApi_InitConfig(
    askerdway_config_v1* config) noexcept;
/**
 * @brief Проверяет v1-заголовок и копирует текущие настройки. Независимые атомарные поля не
 * являются общей транзакцией.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_GetConfig(
    askerdway_config_v1* config) noexcept;
/**
 * @brief Проверяет маску/диапазоны до применения и изменяет только выбранные update_mask поля;
 * setters обслуживают зависимые буферы.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_SetConfig(
    const askerdway_config_v1* config) noexcept;

// Initialize struct_size and abi_version before reading a snapshot.
/**
 * @brief Возвращает агрегированную диагностику runtime в заранее инициализированную
 * v1-структуру; ASKERDWAY_OK не означает, что автоматика сейчас разрешена.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_GetStatus(
    askerdway_status_v1* status) noexcept;
ASKERDWAY_API askerdway_result AskerdwayApi_GetRenderSnapshot(
    askerdway_render_snapshot_v1* snapshot) noexcept;
ASKERDWAY_API askerdway_result AskerdwayApi_GetRenderSnapshotV2(
    askerdway_render_snapshot_v2* snapshot) noexcept;
ASKERDWAY_API std::uint32_t AskerdwayApi_GetDiagnosticDrawBypass() noexcept;
ASKERDWAY_API askerdway_result AskerdwayApi_SetDiagnosticDrawBypass(
    std::uint32_t bypass) noexcept;
/**
 * @brief Заполняет versioned сведения о сборке; строковые поля принадлежат выходной структуре.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_GetBuildInfo(
    askerdway_build_info_v1* info) noexcept;
/**
 * @brief Копирует опубликованные данные героя в POD-снимок; сохранённый адрес нельзя считать
 * долговечным указателем.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_GetHeroSnapshot(
    askerdway_hero_snapshot* hero) noexcept;
/**
 * @brief Копирует наблюдаемое состояние предмета и подтверждений; отправленный приказ и
 * применённое состояние различаются.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_GetArmletSnapshot(
    askerdway_armlet_snapshot* armlet) noexcept;
/**
 * @brief Заполняет versioned снимок разрешённых смещений для диагностики текущего клиента.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_GetSchemaSnapshot(
    askerdway_schema_snapshot_v1* schema) noexcept;
/**
 * @brief Возвращает versioned снимок адресов/калибровки текущего процесса; не сохраняйте
 * адреса для следующего запуска игры.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_GetRuntimeGlobals(
    askerdway_runtime_globals_v1* globals) noexcept;

// Returned entity data is a snapshot.
/**
 * @brief Разрешает handle и копирует данные ссылки на текущую сущность; результат может
 * устареть после возврата.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_ResolveEntityHandle(
    std::uint32_t handle, askerdway_entity_ref* entity) noexcept;
/**
 * @brief Проверяет чтение мировой позиции по адресу сущности; caller обрабатывает ошибку
 * вместо использования прежних координат.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_GetEntityWorldPosition(
    std::uint64_t entity_address, askerdway_vec3* position) noexcept;

// Start the particle cursor at zero and reuse its returned value.
/**
 * @brief Читает ring buffer по изменяемому cursor; written обязателен, output может быть
 * nullptr только при capacity == 0.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_ReadParticleObservations(
    std::uint64_t* cursor,
    askerdway_particle_snapshot* output,
    std::uint32_t capacity,
    std::uint32_t* written) noexcept;
/**
 * @brief Копирует активные события под projectile shared-lock до capacity записей; written
 * сообщает фактическое число.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_ReadProjectiles(
    askerdway_projectile_snapshot* output,
    std::uint32_t capacity,
    std::uint32_t* written) noexcept;
/**
 * @brief Копирует trace-массив под render shared-lock; output == nullptr допустим только при
 * capacity == 0.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_ReadHeroTraces(
    askerdway_hero_trace_snapshot* output,
    std::uint32_t capacity,
    std::uint32_t* written) noexcept;
/**
 * @brief Копирует внутренние маршруты, включая неполные; complete означает наличие обоих
 * endpoints, а не завершение каста.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_ReadTeleportArrows(
    askerdway_teleport_arrow_snapshot* output,
    std::uint32_t capacity,
    std::uint32_t* written) noexcept;
/**
 * @brief Копирует записи лагерей под render shared-lock и публикует размер области блокировки.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_ReadNeutralCamps(
    askerdway_neutral_camp_snapshot* output,
    std::uint32_t capacity,
    std::uint32_t* written) noexcept;

// External telemetry is optional. network_latency_ms is the current measured
// round-trip/feedback latency, not an OFF -> ON delay; it only aligns incoming
// damage with server time. Values from 0 through 5000 ms are accepted.
/**
 * @brief Обновляет heartbeat с измеренной задержкой 0..5000 мс. Не удаляет все ранее
 * переданные события.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_BeginTelemetryFrame(
    float network_latency_ms) noexcept;
/**
 * @brief Проверяет v1-событие, ненулевой ID, источник, конечный урон, delay 0..10 секунд и
 * confidence 0..1; затем копирует событие.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_SubmitDamageEvent(
    const askerdway_damage_event_v1* event) noexcept;
/**
 * @brief Проверяет v1-снаряд, ID, источник, конечные координаты/скорость и неотрицательную
 * геометрию; затем копирует данные в буфер.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_SubmitLinearProjectile(
    const askerdway_linear_projectile_v1* projectile) noexcept;
/**
 * @brief Удаляет событие по ненулевому ID; повторное удаление отсутствующего ID не требует
 * существования записи.
 */
ASKERDWAY_API askerdway_result AskerdwayApi_RemoveTelemetryEvent(
    std::uint64_t event_id) noexcept;
