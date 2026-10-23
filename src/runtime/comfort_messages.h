#pragma once

#include "comfort_policy.h"
#include <cstring>
#include <string_view>

namespace askerdway::comfort {
// Read protobuf wire data, never depend on generated C++ member offsets.
class wire_message {
    struct field { unsigned number = 0, type = 0; std::uint64_t value = 0; std::span<const std::byte> bytes{}; };
    std::array<field, 96> fields_{};
    unsigned count_ = 0;
    bool valid_ = true;
    static bool varint(std::span<const std::byte> b, std::size_t& i, std::uint64_t& v) noexcept {
        v = 0;
        for (unsigned shift = 0; shift < 70; shift += 7) {
            if (i == b.size()) return false;
            const auto c = std::to_integer<unsigned char>(b[i++]);
            if (shift == 63 && c > 1) return false;
            v |= std::uint64_t(c & 127) << shift;
            if (!(c & 128)) return true;
        }
        return false;
    }
public:
    explicit wire_message(std::span<const std::byte> data) noexcept {
        std::size_t i = 0;
        while (i < data.size()) {
            std::uint64_t tag = 0, value = 0;
            if (count_ == fields_.size() || !varint(data, i, tag) || !tag || (tag >> 3) > 0x1fffffff) {
                valid_ = false; break;
            }
            field f{unsigned(tag >> 3), unsigned(tag & 7)};
            if (!f.number) { valid_ = false; break; }
            if (f.type == 0) {
                if (!varint(data, i, f.value)) { valid_ = false; break; }
            } else if (f.type == 2) {
                if (!varint(data, i, value) || value > data.size() - i) { valid_ = false; break; }
                f.bytes = data.subspan(i, std::size_t(value)); i += std::size_t(value);
            } else if (f.type == 1 || f.type == 5) {
                const std::size_t size = f.type == 1 ? 8 : 4;
                if (size > data.size() - i) { valid_ = false; break; }
                f.bytes = data.subspan(i, size); i += size;
            } else { valid_ = false; break; }
            fields_[count_++] = f;
        }
    }
    bool valid() const noexcept { return valid_; }
    bool has(unsigned n, unsigned type = 0) const noexcept {
        if (!valid_) return false;
        for (unsigned i = 0; i < count_; ++i) if (fields_[i].number == n && fields_[i].type == type) return true;
        return false;
    }
    std::int32_t integer(unsigned n, std::int32_t fallback = -1) const noexcept {
        if (!valid_) return fallback;
        for (unsigned i = count_; i > 0; --i)
            if (fields_[i-1].number == n && fields_[i-1].type == 0)
                return std::int32_t(std::uint32_t(fields_[i-1].value));
        return fallback;
    }
    std::span<const std::byte> bytes(unsigned n) const noexcept {
        if (!valid_) return {};
        for (unsigned i = count_; i > 0; --i)
            if (fields_[i-1].number == n && fields_[i-1].type == 2) return fields_[i-1].bytes;
        return {};
    }
};

struct decoded_event {
    event value{};
    int source_entity = -1;
    int target_entity = -1;
    int ability_entity = -1;
    int ability_id = -1;
    bool needs_item_identity = false;
    bool needs_death_state = false;
};

inline decoded_event decode(unsigned message_id, std::span<const std::byte> bytes) noexcept {
    decoded_event result{};
    wire_message m(bytes);
    if (!m.valid()) return result;
    auto& e = result.value;
    e.fingerprint = hash(bytes) ^ (std::uint64_t(message_id) << 32);
    switch (message_id) {
    case 490: // BotChat uses the same source/text fields; useful in native local Demo too.
    case 612: // CDOTAUserMsg_ChatMessage (free text, not ChatEvent/system notifications)
        if (m.has(1) && m.has(3, 2)) { e.kind = category::text; e.sender = m.integer(1); }
        break;
    case 117: // CUserMessageSayText
    case 118: // CUserMessageSayText2
        if (m.integer(2, 0) == 1 && m.has(1)) {
            e.kind = category::text; result.source_entity = m.integer(1);
        }
        break;
    case 501: // ChatWheel
        if (m.has(1) && m.has(2)) { e.kind = category::wheel; e.sender = m.integer(2); }
        break;
    case 576: // SalutePlayer has source, recipient, tip_amount. TipAlert (577) is NOT tipping.
        if (m.has(1) && m.has(2) && m.has(3) && m.integer(3, 0) > 0) {
            e.kind = category::tip; e.sender = m.integer(1); e.target = m.integer(2);
        }
        break;
    case 523: // AbilityPing also carries inventory item pings.
        if (m.has(1) && m.has(2)) {
            e.kind = category::other_signal; e.sender = m.integer(1);
            result.ability_id = m.integer(2); result.ability_entity = m.integer(8);
            result.target_entity = m.integer(14); result.needs_item_identity = true;
        }
        break;
    case 499: // ItemAlert is a ground-item/location alert, not proof of inventory ownership.
        if (m.has(1) && m.has(2, 2)) { e.kind = category::item; e.sender = m.integer(1); }
        break;
    case 534: // EnemyItemAlert
    case 607: // EmptyItemSlotAlert
        if (m.has(1) && m.has(2)) { e.kind = category::item; e.sender = m.integer(1); e.target = m.integer(2); }
        break;
    case 544: // HPManaAlert: the HUD formats the respawn timer when the target is dead.
        if (m.has(1) && m.has(2)) {
            e.kind = category::other_signal; e.sender = m.integer(1);
            result.target_entity = m.integer(2); result.needs_death_state = true;
        }
        break;
    case 477: // LocationPing
    case 478: // MapLine
    case 497: // WorldLine
        if (m.has(1) && m.has(2, 2)) { e.kind = category::map_ping; e.sender = m.integer(1); }
        break;
    // Presentation-only communications with a source player in field 1.
    case 529:
        if (m.has(1) && m.has(2)) {
            e.kind = m.has(4) ? category::purchase_suggestion : category::other_signal;
            e.sender = m.integer(2); e.target = m.integer(4); e.item_id = m.integer(1);
        }
        break;
    case 537: case 540: case 543: case 545: case 555: case 560:
    case 561: case 575: case 579: case 587: case 589: case 608: case 613:
    case 624: case 625: case 626: case 628: case 629: case 634:
        if (m.has(1)) { e.kind = category::other_signal; e.sender = m.integer(1); }
        break;
    default: break;
    }
    return result;
}
} // namespace askerdway::comfort
