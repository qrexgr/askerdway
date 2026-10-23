/** @file
 * @brief Типизированные представления schema-объектов и вызываемые функции Dota.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include "askerdway/dota_api.hpp"
#include "askerdway/engine/types.hpp"

#if defined(_WIN32)
#if defined(ASKERDWAY_BUILDING_DLL)
#define ASKERDWAY_SDK_API __declspec(dllexport)
#else
#define ASKERDWAY_SDK_API __declspec(dllimport)
#endif
#else
#define ASKERDWAY_SDK_API
#endif

namespace askerdway::dota {

/** Ошибка разрешения, типа или доступа. Не перехватывает ошибки внутри движка. */
class error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct vector2 { float x{}, y{}; };
using vector3 = askerdway_vec3;
struct vector4 { float x{}, y{}, z{}, w{}; };
struct color { std::uint8_t r{}, g{}, b{}, a{}; };
static_assert(sizeof(vector2) == 8 && sizeof(vector3) == 12 && sizeof(vector4) == 16);

/** Callback-контекст позволяет тестировать views без живого игрового объекта.
 * owner удерживает ресурсы backend; callback не должен бросать исключения.
 */
struct memory_access {
    using reader = bool (*)(std::uintptr_t, void*, std::size_t) noexcept;
    using writer = bool (*)(std::uintptr_t, const void*, std::size_t) noexcept;
    reader read = nullptr;
    writer write = nullptr;
    std::shared_ptr<void> owner{};
};

[[nodiscard]] ASKERDWAY_SDK_API memory_access process_memory(bool writable = false);

/** Адрес + backend, а не overlay игрового C++ layout. Нулевой extent означает
 * неизвестный размер объекта; backend всё равно проверяет доступность памяти.
 */
class object_view {
public:
    object_view() = default;
    explicit object_view(std::uintptr_t address, memory_access access,
                         std::size_t extent = 0)
        : address_(address), access_(std::move(access)), extent_(extent), bounded_(extent != 0) {}
    [[nodiscard]] std::uintptr_t address() const noexcept { return address_; }
    [[nodiscard]] explicit operator bool() const noexcept { return address_ != 0; }
    [[nodiscard]] const memory_access& access() const noexcept { return access_; }

    /** Проверяет overflow и известную границу локального буфера. */
    [[nodiscard]] object_view subview(std::size_t offset, std::size_t size = 0) const {
        if (!address_ || offset > std::numeric_limits<std::uintptr_t>::max() - address_ ||
            size > std::numeric_limits<std::uintptr_t>::max() - (address_ + offset) ||
            (bounded_ && (offset > extent_ || size > extent_ - offset)))
            throw error("Object field is outside the available address range");
        object_view result(address_ + offset, access_, bounded_ ? extent_ - offset : 0);
        result.bounded_ = bounded_;
        return result;
    }

    /** Возвращает смещение из каталога и проверяет тип, ожидаемый заголовком SDK.
     * Никаких предположений по префиксу m_i/m_fl и неизвестным typedef.
     */
    [[nodiscard]] object_view schema_field(std::string_view module,
        std::string_view owner, std::string_view name, std::string_view type,
        std::size_t size = 0) const {
        const auto* field = dota_api::find_field(module, owner, name);
        if (!field || field->offset < 0 || field->type_name != type)
            throw error("SDK/catalog field mismatch: " + std::string(owner) + "::" + std::string(name));
        return subview(static_cast<std::size_t>(field->offset), size);
    }

private:
    std::uintptr_t address_ = 0;
    memory_access access_{};
    std::size_t extent_ = 0;
    bool bounded_ = false;
};

/** Значение поля читается при get/conversion, записывается при set/assignment.
 * Копирование proxy не копирует значение игрового объекта.
 */
template<class T> class field_ref {
    static_assert(std::is_trivially_copyable_v<T>);
public:
    explicit field_ref(object_view memory) : memory_(std::move(memory)) {}
    field_ref(const field_ref&) = default;
    [[nodiscard]] T get() const {
        T value{};
        const auto bounded = memory_.subview(0, sizeof(T));
        if (!bounded.access().read ||
            !bounded.access().read(bounded.address(), &value, sizeof(T)))
            throw error("Cannot read engine field");
        return value;
    }
    operator T() const { return get(); }
    void set(const T& value) const {
        const auto bounded = memory_.subview(0, sizeof(T));
        if (!bounded.access().write ||
            !bounded.access().write(bounded.address(), &value, sizeof(T)))
            throw error("Engine field is read-only or unavailable");
    }
    const field_ref& operator=(const T& value) const { set(value); return *this; }
    // A field-to-field assignment copies the value, not the proxy address.
    const field_ref& operator=(const field_ref& value) const { return *this = value.get(); }
    [[nodiscard]] std::uintptr_t address() const noexcept { return memory_.address(); }
private:
    object_view memory_;
};

/** Неизвестный контейнер/typedef/bitfield не превращается в угаданный POD.
 * as<T> — явное утверждение caller о проверенном layout, не автоопределение.
 */
class opaque_field {
public:
    explicit opaque_field(object_view memory) : memory_(std::move(memory)) {}
    [[nodiscard]] std::uintptr_t address() const noexcept { return memory_.address(); }
    template<class T> [[nodiscard]] field_ref<T> as() const {
        return field_ref<T>(memory_.subview(0, sizeof(T)));
    }
private:
    object_view memory_;
};

struct resolved_function {
    std::uintptr_t address = 0;
    std::shared_ptr<void> module_owner{};
};

enum class symbol_source { signature, pe_export };

/** Разрешает уже загруженный модуль, сверяет PE с каталогом и удерживает DLL.
 * prototype проверяется для заранее известных signature bindings.
 */
[[nodiscard]] ASKERDWAY_SDK_API resolved_function resolve_function(
    std::string_view module, std::string_view name, symbol_source source,
    std::string_view prototype = {}, std::uint16_t ordinal = 0);

/** Вызывается как обычная функция: fn(arg1, arg2). ABI берётся из Signature.
 * Обёртка не переводит вызов на игровой поток и не подтверждает server action.
 */
template<class Signature> class function;
template<class R, class... Args> class function<R(Args...)> {
public:
    using pointer = R (*)(Args...);
    function() = default;
    explicit function(resolved_function resolved) : resolved_(std::move(resolved)) {}
    // Explicit trusted pointer path for tests and caller-verified native ABIs.
    explicit function(pointer native) : resolved_{reinterpret_cast<std::uintptr_t>(native), {}} {}
    [[nodiscard]] explicit operator bool() const noexcept { return resolved_.address != 0; }
    R operator()(Args... args) const {
        if (!resolved_.address) throw error("Function is not bound");
        return reinterpret_cast<pointer>(resolved_.address)(std::forward<Args>(args)...);
    }
private:
    resolved_function resolved_{};
};

/** C-varargs ABI: дополнительные аргументы проходят обычные default promotions.
 * Формат/смысл varargs остаётся контрактом конкретной функции движка.
 */
template<class R, class... Args> class function<R(Args..., ...)> {
public:
    using pointer = R (*)(Args..., ...);
    function() = default;
    explicit function(resolved_function resolved) : resolved_(std::move(resolved)) {}
    explicit function(pointer native) : resolved_{reinterpret_cast<std::uintptr_t>(native), {}} {}
    [[nodiscard]] explicit operator bool() const noexcept { return resolved_.address != 0; }
    template<class... Extra> R operator()(Args... args, Extra... extra) const {
        if (!resolved_.address) throw error("Function is not bound");
        return reinterpret_cast<pointer>(resolved_.address)(std::forward<Args>(args)..., extra...);
    }
private:
    resolved_function resolved_{};
};

/** Для экспортов без прототипа caller обязан явно выбрать проверенный ABI. */
struct export_symbol {
    std::string_view module;
    std::string_view name;
    std::uint16_t ordinal = 0;
    template<class Signature> [[nodiscard]] function<Signature> bind_as() const {
        return function<Signature>(resolve_function(module, name, symbol_source::pe_export, {}, ordinal));
    }
};

template<class Signature>
[[nodiscard]] function<Signature> bind_export(std::string_view module, std::string_view name) {
    return export_symbol{module, name}.template bind_as<Signature>();
}

/** Указывает, что ABI внутренней byte signature проверен вызывающим кодом.
 * Для типизированных готовых bindings используется дополнительная сверка prototype.
 */
template<class Signature>
[[nodiscard]] function<Signature> bind_signature(std::string_view module, std::string_view name,
                                                std::string_view expected_prototype = {}) {
    return function<Signature>(resolve_function(module, name, symbol_source::signature, expected_prototype));
}

/** Проверенная сессия загруженного модуля. Удерживает DLL, но не игровые entities.
 * Используйте object<T> вместо reinterpret_cast<T*>: T — view, не native layout.
 */
class ASKERDWAY_SDK_API module_view {
public:
    explicit module_view(std::string_view name, bool writable = false);
    template<class T> [[nodiscard]] T object(std::uintptr_t address) const {
        if (T::module_name != name_) throw error("View belongs to another module");
        if (!address) throw error("Null engine object");
        return T(object_view(address, access_));
    }
private:
    std::string name_;
    memory_access access_;
};

} // namespace askerdway::dota
#undef ASKERDWAY_SDK_API
