#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace askerdway {
// Replication can stop while an enemy is hidden. Anchor remaining duration to
// game time only when the replicated sample changes or visibility confirms it.
struct replicated_cooldown {
    std::uint32_t item_handle = UINT32_MAX;
    float last_raw = 0, deadline = 0, last_game_time = -1;
    bool initialized = false;
    bool estimated = false;
    std::uint64_t last_cast = 0;
    float sample_changed_at = -1;

    // A validated TP event can arrive while the replicated item remains at 0.
    // Keep its generation separately: streaming endpoints never restart a CD.
    void infer_cast(std::uint64_t cast, float cast_game_time, float duration) noexcept {
        if(!initialized||!cast||cast<=last_cast||!std::isfinite(cast_game_time)||
            cast_game_time<0||!std::isfinite(duration)||duration<=0||duration>300)return;
        last_cast=cast;
        if(last_raw>0&&sample_changed_at>=cast_game_time-.2f)return;
        deadline=cast_game_time+duration;
        estimated=true;
    }

    float observe(std::uint32_t handle, float raw, float game_time,
                  bool visible) noexcept {
        if (!std::isfinite(raw) || raw < 0 || raw > 300)
            return initialized && game_time >= 0 ? std::max(0.0f, deadline-game_time) : -1;
        if (!std::isfinite(game_time) || game_time < 0) return raw;
        if (!initialized || item_handle != handle || game_time < last_game_time ||
            visible || std::abs(raw-last_raw) > 0.001f) {
            if(!initialized||item_handle!=handle||game_time<last_game_time)last_cast=0;
            item_handle=handle;
            last_raw=raw;
            deadline=game_time+raw;
            initialized=true;
            sample_changed_at=game_time;
            estimated=false;
        }
        last_game_time=game_time;
        return std::max(0.0f,deadline-game_time);
    }
};
}
