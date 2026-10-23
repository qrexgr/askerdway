#pragma once
#include <cstdint>
#include <cstring>

// Read-only VEngineCvar007 layout, checked against the installed tier0 getter.
// Names and boolean type are revalidated on every cached read. No console
// commands, registrations or callbacks are invoked.
namespace local_convar {
template<class Read>
bool active_server(Read read,std::uintptr_t service) {
    std::uintptr_t table=0,getter=0,active=0,server=0;
    std::uint8_t code[8]{},check[21]{};
    if(!read(service,&table,8)||!read(table+23*8,&getter,8)||!read(getter,code,sizeof(code))||
        code[0]!=0x48||code[1]!=0x8b||code[2]!=0x81||code[7]!=0xc3||
        !read(table+24*8,&active,8)||!read(active,check,sizeof(check)))return false;
    // GetIGameServer and IsActiveInGame must agree about the same pointer.
    constexpr std::uint8_t tail[]{0x48,0x85,0xc0,0x75,0x01,0xc3,0x83,0x78,0x38,0x03,0x0f,0x9d,0xc0,0xc3};
    if(std::memcmp(code,check,7)||std::memcmp(check+7,tail,sizeof(tail)))return false;
    std::uint32_t offset=0;std::memcpy(&offset,code+3,sizeof(offset));
    int state=0;
    return offset>=0x10&&offset<=0x1000&&read(service+offset,&server,8)&&server&&
        read(server+0x38,&state,sizeof(state))&&(state==3||state==4);
}

template<class Read>
bool boolean(Read read, std::uintptr_t cvar, const char* name,
    std::uintptr_t& cached, bool& value, std::uint32_t* revision=nullptr) {
    const auto sample=[&](std::uintptr_t entry) {
        std::uintptr_t text=0;std::int16_t type=-1;std::uint8_t raw=0;
        std::uint32_t before=0,after=0;
        char actual[64]{};const auto length=std::strlen(name)+1;
        if(length>sizeof(actual)||!read(entry,&text,sizeof(text))||
            !read(text,actual,length)||std::strcmp(actual,name)!=0||
            !read(entry+0x28,&type,sizeof(type))||type!=0||
            (revision&&!read(entry+0x2c,&before,sizeof(before)))||
            !read(entry+0x58,&raw,sizeof(raw))||raw>1)return false;
        if(revision&&(!read(entry+0x2c,&after,sizeof(after))||before!=after))return false;
        value=raw!=0;if(revision)*revision=after;return true;
    };
    if(cached&&sample(cached))return true;
    cached=0;
    std::uintptr_t entries=0;std::uint16_t count=0;
    if(!read(cvar+0x50,&entries,sizeof(entries))||
        !read(cvar+0x5e,&count,sizeof(count))||!entries||count>16384)return false;
    for(unsigned i=0;i<count;++i) {
        std::uintptr_t entry=0;
        if(read(entries+i*16,&entry,sizeof(entry))&&entry&&sample(entry)){
            cached=entry;return true;
        }
    }
    return false;
}
}
