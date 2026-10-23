#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// A contact belongs to one live body's attack epoch, never its player/hero name.
struct melee_contact_history {
    float attack_at = -1.f;
    float contact_at = -1.f;
    bool confirmed = false;
    bool observe(float attack, float source_damage, float target_damage, float game_time) noexcept {
        if (!std::isfinite(attack) || !std::isfinite(source_damage) ||
            !std::isfinite(target_damage) || !std::isfinite(game_time) ||
            attack <= 0 || attack > game_time + .10f) return false;
        if (attack != attack_at) { attack_at = attack; confirmed = false; contact_at = -1.f; }
        if (confirmed) return true;
        confirmed = source_damage >= attack && target_damage >= attack &&
            source_damage <= game_time && target_damage <= game_time &&
            std::abs(source_damage - target_damage) <= .15f;
        if (confirmed) contact_at = std::max(source_damage, target_damage);
        return confirmed;
    }
};

inline bool instant_contact_covers_swing(std::uint64_t contact_at,
    std::uint64_t swing_started_at, std::uint64_t now) noexcept {
    return swing_started_at != 0 && contact_at >= swing_started_at && contact_at <= now;
}
