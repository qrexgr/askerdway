#pragma once

#include "askerdway/comfort.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace askerdway::comfort {

enum class category : std::uint8_t {
    unknown, text, item, respawn, tip, wheel, map_ping, other_signal, voice, purchase_suggestion
};
enum surface : unsigned { text = 1, sound = 2, marker = 4, all_surfaces = 7 };
struct event {
    category kind = category::unknown;
    int sender = -1;
    int target = -1;
    int sender_team = 0;
    std::uint64_t fingerprint = 0;
    // Stable per delivery, shared by all presentation delegates. Zero disables caching.
    std::uint64_t delivery = 0;
    int item_id = -1;
    // Presentation subscriber identity distinguishes another subscriber from a reused message buffer.
    std::uintptr_t consumer = 0;
};
struct context {
    int local_player = -1;
    int local_team = 0;
    std::uint64_t session = 0;
};

inline std::uint64_t hash(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value = 14695981039346656037ull;
    for (auto c : bytes) { value ^= std::to_integer<unsigned char>(c); value *= 1099511628211ull; }
    return value;
}
inline bool valid_player(int id) noexcept { return id >= 0 && id < 64; }
inline bool valid_config(const askerdway_comfort_config_v1& c) noexcept {
    if ((c.flags & ~ASKERDWAY_COMFORT_FLAGS_ALL) || c.item_mode > ASKERDWAY_COMFORT_ITEMS_SELECTED_SUGGESTIONS ||
        c.silence_hotkey < 0 || c.silence_hotkey > 255) return false;
    const int k = c.silence_hotkey;
    if (k && (k < 8 || k == 0x10 || k == 0x11 || k == 0x12 || k == 0x1b ||
        k == 0x23 || k == 0x2d || k == 0x5b || k == 0x5c || (k >= 0xa0 && k <= 0xa5))) return false;
    for (auto r : c.reserved) if (r) return false;
    if (c.suggestion_item_count > 16) return false;
    for (unsigned i = 0; i < 16; ++i) {
        if (i >= c.suggestion_item_count) { if (c.suggestion_items[i]) return false; continue; }
        if (!c.suggestion_items[i] || c.suggestion_items[i] > 1000000) return false;
        for (unsigned j = 0; j < i; ++j) if (c.suggestion_items[j] == c.suggestion_items[i]) return false;
    }
    return true;
}

class policy {
    struct seen { std::uint64_t key = 0, at = 0; bool used = false; };
    struct delivery_result {
        std::uint64_t id = 0, key = 0, at = 0; unsigned hidden = 0; bool used = false;
        std::array<std::uintptr_t, 8> consumers{};
        unsigned consumer_count = 0;
    };
    struct sender_history {
        std::array<seen, 64> duplicates{};
        std::array<std::uint64_t, 3> pings{};
        unsigned ping_count = 0;
        unsigned next = 0;
    };
    std::array<sender_history, 64> history_{};
    std::array<delivery_result, 128> deliveries_{};
    unsigned next_delivery_ = 0;
    std::uint64_t last_time_ = 0;
    context context_{};
public:
    askerdway_comfort_config_v1 config{};
    bool silence = false;
    std::uint64_t blocked = 0;

    void clear_history() noexcept {
        history_ = {}; deliveries_ = {}; next_delivery_ = 0; last_time_ = 0;
    }
    void set_context(context c) noexcept {
        if (c.session != context_.session || c.local_player != context_.local_player) {
            blocked = 0; clear_history();
        }
        context_ = c;
    }
    const context& current_context() const noexcept { return context_; }
    bool enabled() const noexcept { return (config.flags & ASKERDWAY_COMFORT_ENABLED) != 0 || silence; }
    bool player_hidden(int player) const noexcept {
        return enabled() && valid_player(player) && player != context_.local_player &&
            (silence || ((blocked >> player) & 1));
    }

    unsigned evaluate(const event& e, std::uint64_t now) noexcept {
        if (!enabled() || e.kind == category::unknown || !valid_player(e.sender) ||
            !valid_player(context_.local_player) || e.sender == context_.local_player) return 0;
        if (now < last_time_) clear_history();
        last_time_ = now;
        const auto key = e.fingerprint ^ (std::uint64_t(e.kind) << 56) ^
            (std::uint64_t(e.sender) << 48) ^ (std::uint64_t(std::uint32_t(e.target)) << 16);
        if (e.delivery) {
            for (unsigned i = 0; i < deliveries_.size(); ++i) {
                auto& d = deliveries_[(next_delivery_ + deliveries_.size() - 1 - i) % deliveries_.size()];
                if (!d.used || d.id != e.delivery || d.key != key || now - d.at >= 100) continue;
                if (!e.consumer) return d.hidden;
                bool repeated_consumer = false;
                for (unsigned j = 0; j < d.consumer_count; ++j) repeated_consumer |= d.consumers[j] == e.consumer;
                // The same subscriber cannot receive one event twice. A pool may reuse the
                // wrapper within the same server tick; that repeat must still count as spam.
                if (repeated_consumer || d.consumer_count == d.consumers.size()) break;
                d.consumers[d.consumer_count++] = e.consumer;
                return d.hidden;
            }
        }
        unsigned hidden = 0;
        const auto flags = config.flags;
        if (player_hidden(e.sender)) hidden = all_surfaces;
        else switch (e.kind) {
        case category::text:
            // Unknown teams are never guessed to be enemies.
            if (context_.local_team > 0 && e.sender_team > 0 &&
                (flags & (e.sender_team == context_.local_team ?
                    ASKERDWAY_COMFORT_ALLY_TEXT : ASKERDWAY_COMFORT_ENEMY_TEXT))) hidden = all_surfaces;
            break;
        case category::purchase_suggestion:
            if (config.item_mode == ASKERDWAY_COMFORT_ITEMS_SELECTED_SUGGESTIONS)
                for (unsigned i = 0; i < config.suggestion_item_count; ++i)
                    if (int(config.suggestion_items[i]) == e.item_id) { hidden = all_surfaces; break; }
            break;
        case category::respawn:
            if ((flags & ASKERDWAY_COMFORT_RESPAWN) && e.target == context_.local_player) hidden = all_surfaces;
            break;
        case category::tip:
            if (flags & ASKERDWAY_COMFORT_TIPS) hidden = all_surfaces;
            break;
        case category::wheel:
            if (flags & ASKERDWAY_COMFORT_WHEEL_TEXT) hidden |= text | marker;
            if (flags & ASKERDWAY_COMFORT_WHEEL_SOUND) hidden |= sound;
            break;
        default: break;
        }
        if ((flags & ASKERDWAY_COMFORT_ANTISPAM) && e.kind != category::voice && hidden != all_surfaces) {
            auto& h = history_[e.sender];
            bool duplicate = false;
            for (auto& s : h.duplicates)
                if (s.used && s.key == key && now - s.at < 3000) { duplicate = true; break; }
            if (duplicate) hidden = all_surfaces;
            else if (e.kind == category::map_ping) {
                unsigned live = 0;
                for (unsigned i = 0; i < h.ping_count; ++i)
                    if (now - h.pings[i] < 5000) h.pings[live++] = h.pings[i];
                h.ping_count = live;
                if (live == 3) hidden = all_surfaces;
                else h.pings[h.ping_count++] = now;
            }
            // Suppressed repeats do not extend the window indefinitely.
            if (hidden != all_surfaces) h.duplicates[h.next++ % h.duplicates.size()] = {key, now, true};
        }
        if (e.delivery) {
            auto& d = deliveries_[next_delivery_++ % deliveries_.size()];
            d = {e.delivery, key, now, hidden, true};
            if (e.consumer) d.consumers[d.consumer_count++] = e.consumer;
        }
        return hidden;
    }
};
} // namespace askerdway::comfort
