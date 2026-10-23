#pragma once
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace menu_preferences {
struct settings {
    bool nickname=true, date=true, time=true, seconds=false, iso_date=false;
    int scroll_ms=100, row_height=32;
    bool camp_count=true,camp_gold=true,camp_estimates=true,camp_background=false;
    char alias[128]{};
    std::string path;
    bool loaded=false;
    void load() {
        if (loaded) return;
        loaded=true;
        char directory[32768]{};
        if (!GetEnvironmentVariableA("LOCALAPPDATA",directory,DWORD(sizeof(directory)))) return;
        path=std::string(directory)+"\\Askerdway";
        CreateDirectoryA(path.c_str(),nullptr);
        path+="\\interface.ini";
        nickname=GetPrivateProfileIntA("Header","Nickname",1,path.c_str())!=0;
        date=GetPrivateProfileIntA("Header","Date",1,path.c_str())!=0;
        time=GetPrivateProfileIntA("Header","Time",1,path.c_str())!=0;
        seconds=GetPrivateProfileIntA("Header","Seconds",0,path.c_str())!=0;
        iso_date=GetPrivateProfileIntA("Header","ISODate",0,path.c_str())!=0;
        scroll_ms=std::clamp(int(GetPrivateProfileIntA("Menu","ScrollMs",100,path.c_str())),0,250);
        row_height=std::clamp(int(GetPrivateProfileIntA("Menu","RowHeight",32,path.c_str())),28,44);
        GetPrivateProfileStringA("Header","Alias","",alias,sizeof(alias),path.c_str());
        camp_count=GetPrivateProfileIntA("Jungle","Count",1,path.c_str())!=0;
        camp_gold=GetPrivateProfileIntA("Jungle","Gold",1,path.c_str())!=0;
        camp_estimates=GetPrivateProfileIntA("Jungle","Estimates",1,path.c_str())!=0;
        camp_background=GetPrivateProfileIntA("Jungle","Background",0,path.c_str())!=0;
    }
    void save() const {
        if(path.empty())return;
        const auto write=[&](const char* key,bool value){WritePrivateProfileStringA("Header",key,value?"1":"0",path.c_str());};
        write("Nickname",nickname);write("Date",date);write("Time",time);write("Seconds",seconds);write("ISODate",iso_date);
        WritePrivateProfileStringA("Header","Alias",alias,path.c_str());
        char number[16]{};std::snprintf(number,sizeof(number),"%d",row_height);
        WritePrivateProfileStringA("Menu","RowHeight",number,path.c_str());std::snprintf(number,sizeof(number),"%d",scroll_ms);
        WritePrivateProfileStringA("Menu","ScrollMs",number,path.c_str());
        const auto jungle=[&](const char* key,bool value){WritePrivateProfileStringA("Jungle",key,value?"1":"0",path.c_str());};
        jungle("Count",camp_count);jungle("Gold",camp_gold);jungle("Estimates",camp_estimates);jungle("Background",camp_background);
    }
    std::string title(const char* name,const SYSTEMTIME& now) const {
        std::string text;
        const auto append=[&](const char* part){if(!text.empty())text+="  |  ";text+=part;};
        if(nickname)append(alias[0]?alias:name);
        char part[48]{};
        if(date){
            if(iso_date)std::snprintf(part,sizeof(part),"%04u-%02u-%02u",now.wYear,now.wMonth,now.wDay);
            else std::snprintf(part,sizeof(part),"%02u.%02u.%04u",now.wDay,now.wMonth,now.wYear);
            append(part);
        }
        if(time){
            std::snprintf(part,sizeof(part),seconds?"%02u:%02u:%02u":"%02u:%02u",now.wHour,now.wMinute,now.wSecond);
            append(part);
        }
        // Account names are data, not ImGui label/ID delimiters.
        for(char& c:text)if(c=='#')c=' ';
        return text+"###askerdway_portfolio";
    }
};
inline settings current;

struct scroll_motion {
    float position=0,target=0;
    bool initialized=false;
    float step(float actual,float maximum,float wheel,float distance,float dt,int milliseconds,bool direct) {
        if(!initialized||direct){position=target=actual;initialized=true;}
        target=std::clamp(target-wheel*distance,0.f,maximum);
        if(milliseconds<=0)position=target;
        else position+=(target-position)*(1.f-std::exp(-4.6f*std::clamp(dt,0.f,.1f)/(milliseconds*.001f)));
        if(std::abs(target-position)<.25f)position=target;
        position=std::clamp(position,0.f,maximum);
        return position;
    }
};
}
