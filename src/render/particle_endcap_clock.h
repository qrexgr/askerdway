#pragma once
#include <array>
#include <cmath>
#include <limits>

namespace particle_endcap {
// Experimental particles.dll collection prefix observed on 2026-10-19.
// The reader copies [0x40,0x6c) only while Update owns the live collection.
// Reject changed layouts: replicated clock lanes, previous time, dt and the
// sentinel->endcap transition must agree. Never interpret a timer alone as TP.
struct clock {
    bool armed=false, emitted=false;
    float previous=0;
    bool sample(const std::array<float,11>& p) noexcept {
        const float now=p[0], prior=p[5], dt=p[9], end=p[7];
        if(!std::isfinite(now)||now<0||now>20||!std::isfinite(prior)||prior<0||prior>now+.001f||
            !std::isfinite(dt)||dt<0||dt>1||std::abs((now-prior)-dt)>.02f||
            p[1]!=now||p[2]!=now||p[3]!=now||p[4]!=now||p[6]!=now||
            p[10]!=dt||now+.001f<previous) {
            armed=false;return false;
        }
        previous=now;
        if(end==-std::numeric_limits<float>::max()) {armed=true;return false;}
        if(!armed||emitted||!std::isfinite(end)||end<.5f||end>10.f||end>now||now-end>.5f)return false;
        emitted=true;return true;
    }
};
}
