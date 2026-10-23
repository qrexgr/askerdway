#pragma once

// Physical key edges are sampled globally. A focus/binding transition only
// establishes a new baseline; returning to Dota cannot replay an old press.
struct hotkey_edge {
    int binding = 0;
    bool was_down = false;
    bool was_active = false;
    bool initialized = false;
    bool was_blocked = false;

    bool pressed(int key, bool down, bool active, bool tapped = false,
        bool blocked = false) noexcept {
        if (!active || blocked || was_blocked || !initialized || binding != key || was_active != active) {
            initialized = true;
            binding = key;
            was_down = down;
            was_active = active;
            was_blocked = blocked;
            return false;
        }
        const bool edge = key != 0 && !was_down && (down || tapped);
        was_down = down;
        return edge;
    }
};
