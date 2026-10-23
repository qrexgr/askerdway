/**
 * @file
 * @brief Дескриптор сущности Source 2.
 *
 * Разделяет 15-битный индекс слота и serial. Индекс пригоден для поиска сейчас; сравнение
 * полного handle необходимо для различения повторного использования слота.
 */

#pragma once

#include <cstdint>

namespace askerdway::engine {

// Source 2 entity handles use a 15-bit entry index. The upper bits are a serial
// number that changes when an entity slot is reused. Always compare the complete
// raw handle when tracking identity; use index() only for an immediate lookup.
struct entity_handle {
    static constexpr std::uint32_t invalid_value = 0xFFFFFFFFu;
    static constexpr std::uint32_t index_mask = 0x7FFFu;

    std::uint32_t value = invalid_value;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value != 0 && value != invalid_value;
    }

    [[nodiscard]] constexpr std::uint16_t index() const noexcept {
        return static_cast<std::uint16_t>(value & index_mask);
    }

    [[nodiscard]] constexpr std::uint32_t serial() const noexcept {
        return value >> 15;
    }

    [[nodiscard]] static constexpr entity_handle from_parts(
        std::uint16_t entry_index, std::uint32_t serial_number) noexcept {
        return {static_cast<std::uint32_t>(entry_index & index_mask) |
            (serial_number << 15)};
    }
};

}  // namespace askerdway::engine
