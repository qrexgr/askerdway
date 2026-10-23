#pragma once
#include <Windows.h>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include "interface_fonts.h"

namespace tile_raster {
// Worker-owned scratch surface. Reuse DIB/DC/fonts instead of asking GDI to
// allocate and destroy them for every changing health pixel or timer label.
class surface {
public:
    HDC dc=nullptr;
    void* pixels=nullptr;
    HFONT hud_font=nullptr,label_font=nullptr;
    HPEN border=nullptr;
    unsigned builds=0;
    surface()=default;
    surface(const surface&)=delete;
    surface& operator=(const surface&)=delete;
    ~surface(){reset();}
    void reset() {
        if(mask_dc&&old_mask)SelectObject(mask_dc,old_mask);
        if(mask_bitmap)DeleteObject(mask_bitmap);
        if(mask_dc)DeleteDC(mask_dc);
        if(dc&&old_bitmap)SelectObject(dc,old_bitmap);
        if(bitmap)DeleteObject(bitmap);
        if(hud_font)DeleteObject(hud_font);
        if(label_font)DeleteObject(label_font);
        if(border)DeleteObject(border);
        if(dc)DeleteDC(dc);
        dc=nullptr;bitmap=nullptr;old_bitmap=nullptr;pixels=nullptr;
        mask_dc=nullptr;mask_bitmap=nullptr;old_mask=nullptr;mask_pixels=nullptr;
        hud_font=label_font=nullptr;border=nullptr;width=height=0;
    }
    bool begin(int w,int h) {
        if(!dc||width!=w||height!=h) {
            reset();dc=CreateCompatibleDC(nullptr);
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;
            info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            bitmap=dc?CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;
            if(dc&&bitmap)old_bitmap=SelectObject(dc,bitmap);
            mask_dc=CreateCompatibleDC(nullptr);
            mask_bitmap=mask_dc?CreateDIBSection(mask_dc,&info,DIB_RGB_COLORS,&mask_pixels,nullptr,0):nullptr;
            if(mask_dc&&mask_bitmap)old_mask=SelectObject(mask_dc,mask_bitmap);
            hud_font=font(-11,FW_BOLD);label_font=font(-12,FW_NORMAL);
            border=CreatePen(PS_SOLID,1,RGB(101,114,126));
            if(!dc||!bitmap||!pixels||!mask_dc||!mask_bitmap||!mask_pixels||!hud_font||!label_font||!border) {reset();return false;}
            width=w;height=h;++builds;
        }
        // Finish pending commands before touching the reused DIB from CPU.
        GdiFlush();std::memset(pixels,0,std::size_t(w)*h*4);SetBkMode(dc,TRANSPARENT);
        return true;
    }
    void finish_gdi() {
        GdiFlush();auto* p=static_cast<std::uint8_t*>(pixels);
        for(int i=0;i<width*height;++i,p+=4)
            if(!p[3]&&(p[0]||p[1]||p[2]))p[3]=255;
    }
    void text(const char* label,RECT bounds,UINT format,COLORREF color,HFONT face) {
        // GDI draws coverage into a black mask. Turning every nonblack RGB
        // pixel opaque loses that coverage and leaves a dark crawling fringe.
        GdiFlush();std::memset(mask_pixels,0,std::size_t(width)*height*4);
        SetBkMode(mask_dc,TRANSPARENT);SetTextColor(mask_dc,RGB(255,255,255));
        const auto previous=SelectObject(mask_dc,face);
        DrawTextA(mask_dc,label,-1,&bounds,format);
        SelectObject(mask_dc,previous);GdiFlush();
        const unsigned channels[]{GetBValue(color),GetGValue(color),GetRValue(color)};
        const auto* mask=static_cast<const std::uint8_t*>(mask_pixels);
        auto* dst=static_cast<std::uint8_t*>(pixels);
        for(int i=0;i<width*height;++i,mask+=4,dst+=4) {
            const unsigned a=std::max({mask[0],mask[1],mask[2]});if(!a)continue;
            const unsigned old_a=dst[3],out_a=a+(old_a*(255-a)+127)/255;
            for(int c=0;c<3;++c)
                dst[c]=static_cast<std::uint8_t>((channels[c]*a*255+dst[c]*old_a*(255-a)+out_a*127)/(out_a*255));
            dst[3]=static_cast<std::uint8_t>(out_a);
        }
    }
private:
    HDC mask_dc=nullptr;
    HBITMAP mask_bitmap=nullptr;
    HGDIOBJ old_mask=nullptr;
    void* mask_pixels=nullptr;
    HBITMAP bitmap=nullptr;
    HGDIOBJ old_bitmap=nullptr;
    int width=0,height=0;
    static HFONT font(int height,int weight) {
        return CreateFontW(height,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,
            FIXED_PITCH|FF_MODERN,interface_fonts::family);
    }
};
}
