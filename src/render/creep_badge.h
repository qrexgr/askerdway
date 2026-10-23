#pragma once
#include <cmath>
#include <cstdint>

namespace creep_badge {
// World badge background. The original game emoticon is a static atlas sprite.
template<class Triangle>
void draw(float x,float y,float radius,Triangle triangle) {
    const auto disc=[&](float r,std::uint32_t color) {
        for(int i=0;i<24;++i) {
            const float a=i*6.2831853f/24,b=(i+1)*6.2831853f/24;
            triangle(x,y,x+std::cos(a)*r,y+std::sin(a)*r,
                x+std::cos(b)*r,y+std::sin(b)*r,color);
        }
    };
    disc(radius+1.5f,0xd008080du);
    disc(radius,0xff4242e9u);
    disc(radius*.80f,0xff141523u);
}
inline constexpr unsigned vertices=3*24*3;
}
