#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace minimap_hud {
struct rectangle {float x=0,y=0,width=0,height=0,scale=1;};
template<class Read> bool sample(Read read,std::uintptr_t panel,float viewport_width,float viewport_height,rectangle& out) {
    std::uintptr_t name=0,client=0,back=0;
    char label[8]{};std::array<float,4> local{},absolute{};float scale=0;
    if(!read(panel+0x10,&name,8)||!read(name,label,sizeof(label))||std::strcmp(label,"minimap")||
       !read(panel+8,&client,8)||!read(client+8,&back,8)||back!=panel||
       !read(panel+0x48,local.data(),sizeof(local))||!read(panel+0x1c8,absolute.data(),sizeof(absolute))||
       !read(panel+0x1f0,&scale,4))return false;
    for(float v:local)if(!std::isfinite(v))return false;
    for(float v:absolute)if(!std::isfinite(v))return false;
    const float x=absolute[0]+local[0],y=absolute[1]+local[1];
    if(!std::isfinite(scale)||scale<.5f||scale>3||local[2]<120||local[2]>viewport_width*.45f||
       local[3]<120||std::abs(local[2]-local[3])>3||x< -2||y<viewport_height*.4f||
       x+local[2]>viewport_width+2||y+local[3]>viewport_height+2)return false;
    out={x,y,local[2],local[3],scale};return true;
}
// Panorama stores panels in a sparse tree: count is NOT the highest live slot.
template<class Read> std::uintptr_t find(Read read,std::uintptr_t engine,float width,float height) {
    std::uintptr_t entries=0;std::uint32_t count=0,last=0;
    if(!read(engine+0x218,&entries,8)||!read(engine+0x220,&count,4)||
       !read(engine+0x224,&last,4)||!entries||!count||count>100000||last>=100000)return 0;
    for(unsigned i=0;i<=last;++i){
        std::uintptr_t panel=0;rectangle bounds{};
        if(read(entries+i*32+16,&panel,8)&&panel&&sample(read,panel,width,height,bounds))return panel;
    }
    return 0;
}
}
