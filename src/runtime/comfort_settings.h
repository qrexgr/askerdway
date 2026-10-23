#pragma once
#include "comfort_policy.h"
#include <charconv>
#include <string>
#include <string_view>

namespace askerdway::comfort {
inline std::string encode_settings(const askerdway_comfort_config_v1& c) {
    auto result = "version=2\nflags=" + std::to_string(c.flags) + "\nitems=" +
        std::to_string(c.item_mode) + "\nhotkey=" + std::to_string(c.silence_hotkey) + "\nids=";
    for (unsigned i = 0; i < c.suggestion_item_count; ++i) {
        if (i) result += ',';
        result += std::to_string(c.suggestion_items[i]);
    }
    return result + "\n";
}
inline bool decode_settings(std::string_view data, askerdway_comfort_config_v1& output) noexcept {
    askerdway_comfort_config_v1 c{};
    c.suggestion_item_count = 0;
    for (auto& id : c.suggestion_items) id = 0;
    unsigned seen = 0;
    while (!data.empty()) {
        auto end = data.find('\n');
        auto line = data.substr(0, end);
        data = end == data.npos ? std::string_view{} : data.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == line.npos) return false;
        auto key = line.substr(0, eq), number = line.substr(eq + 1);
        if (key == "ids") {
            if (seen & 16) return false;
            seen |= 16;
            while (!number.empty()) {
                if (c.suggestion_item_count == 16) return false;
                const auto comma = number.find(',');
                const auto token = number.substr(0, comma);
                unsigned id = 0;
                const auto parsed = std::from_chars(token.data(), token.data()+token.size(), id);
                if (parsed.ec != std::errc{} || parsed.ptr != token.data()+token.size()) return false;
                c.suggestion_items[c.suggestion_item_count++] = id;
                if (comma == number.npos) break;
                number.remove_prefix(comma+1);
                if (number.empty()) return false;
            }
            continue;
        }
        std::uint32_t value = 0;
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) return false;
        unsigned bit = 0;
        if (key == "version") { bit = 1; if (value != 2) return false; }
        else if (key == "flags") { bit = 2; c.flags = value; }
        else if (key == "items") { bit = 4; c.item_mode = value; }
        else if (key == "hotkey") { bit = 8; if (value > 255) return false; c.silence_hotkey = int(value); }
        else return false;
        if (seen & bit) return false;
        seen |= bit;
    }
    if (seen != 31 || !valid_config(c)) return false;
    output = c;
    return true;
}
} // namespace askerdway::comfort
