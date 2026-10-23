#pragma once
#include "tile_raster_surface.h"
#include "../runtime/jungle_status.h"
#include <cstdio>

namespace camp_world_tile {
struct options {bool count=true,gold=true,background=false;};
inline bool draw(tile_raster::surface& surface,const jungle_status::entry& camp,options visible={}) {
    if(!camp.known||camp.empty_confirmed||camp.possibly_cleared||
       (!visible.count&&!visible.gold)||!surface.begin(112,68))return false;
    auto* pixels=static_cast<std::uint8_t*>(surface.pixels);
    const int rows=int(visible.count)+int(visible.gold);
    const auto pixel=[&](int x,int y,unsigned color,unsigned alpha=255){
        if(x<0||x>=112||y<0||y>=68)return;
        auto* p=pixels+(y*112+x)*4;
        p[0]=std::uint8_t(color&255);p[1]=std::uint8_t((color>>8)&255);p[2]=std::uint8_t((color>>16)&255);p[3]=std::uint8_t(alpha);
    };
    if(visible.background)for(int y=3;y<7+rows*17;++y)for(int x=2;x<110;++x)pixel(x,y,0x17191d,210);
    int row=0;
    const auto label=[&](const char* value,COLORREF color){
        if(!visible.background)surface.text(value,{26,6+row*17,109,22+row*17},DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX,
            RGB(0,0,0),surface.label_font);
        surface.text(value,{25,5+row*17,108,21+row*17},DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX,
            color,surface.label_font);
    };
    char value[32]{};
    const char* stale=camp.current?"":"~";
    if(visible.count){
        // A small neutral head, with cut-out eyes and horns; no font glyphs.
        for(int y=0;y<11;++y)for(int x=0;x<13;++x){
            bool head=(y>=2&&y<=8&&x>=2&&x<=10)||(y>=8&&x>=4&&x<=8)||
                (y<=3&&(x==0||x==12));
            if(y==5&&(x==4||x==8))head=false;
            if(head)pixel(9+x,8+y,0xffffff);
        }
        if(camp.count_max>camp.count)std::snprintf(value,sizeof(value),"%s%d-%d",stale,camp.count,camp.count_max);
        else std::snprintf(value,sizeof(value),"%s%d",stale,camp.count);
        label(value,RGB(255,255,255));++row;
    }
    if(visible.gold){
        for(int y=-5;y<=5;++y)for(int x=-5;x<=5;++x)if(x*x+y*y<=25)
            pixel(15+x,13+row*17+y,x==0&&std::abs(y)<4?0x17191d:0xffcd4a);
        if(camp.gold_known)std::snprintf(value,sizeof(value),"%s%d-%d",stale,camp.gold_min,camp.gold_max);
        else std::snprintf(value,sizeof(value),"?");
        label(value,RGB(255,205,74));
    }
    // Lost visibility retains explicitly approximate observations.
    if(!camp.current)for(int i=0;i<112*68;++i)pixels[i*4+3]=std::uint8_t(unsigned(pixels[i*4+3])*180/255);
    return true;
}
}
