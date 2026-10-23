#pragma once
#include <Windows.h>
#include <dxgi.h>
#include <array>
#include <cstdint>

// Worker publishes bounded data and cached pixels; Present only projects and
// draws. No inventory scans, GDI, allocations or blocking worker locks there.
namespace world_frame {
inline constexpr unsigned capacity=64, tile_width=112, tile_height=68;
using tile_pixels=std::array<std::uint8_t,tile_width*tile_height*4>;
struct marker {
    float position[3]{};
    std::uint64_t expires=0, version=0;
    std::uint64_t motion_key=0;
    bool screen_space=false,alt_only=false;
    bool minimap_icon=false,minimap_source_icon=false,wave=false;
    float icon_diameter=32;
};
struct direction { float origin[3]{}, destination[3]{}; std::uint64_t expires=0;
    std::uint32_t color=0xded9c19d; };
struct camp_area {
    float center[3]{},half[2]{};
    std::uint64_t expires=0;
    bool estimated=true;
    bool blocked=true,alt_only=false;
    bool cleared=false,uncertain=false;
};
struct activation_pulse {float center[3]{};std::uint64_t started=0,expires=0;};
struct scene {
    std::array<marker,capacity> markers{};
    std::array<direction,16> directions{};
    std::array<camp_area,32> areas{};
    std::array<activation_pulse,16> pulses{};
    unsigned count=0, direction_count=0;
    unsigned area_count=0;
    unsigned pulse_count=0;
    std::uintptr_t matrix_address=0;
    bool enabled=false;
    struct minimap_layout {
        bool enabled=false,on_right=false;
        float minimum[2]{-8288,-8288},maximum[2]{8288,8288};
        int size_percent=100,offset_x=0,offset_y=0;
        bool automatic=false;
        float viewport_width=0,viewport_height=0;
        float x=0,y=0,width=0,height=0;
    } minimap;
};
struct metrics { std::uint64_t presents=0, drawn_frames=0; unsigned visible=0, render_us=0; bool installed=false;
    std::uint64_t reentrant_calls=0; bool object_hook=false;
    std::uint64_t gpu_builds=0,tile_uploads=0; bool gpu_ready=false;
    unsigned game_present_us=0, game_present_peak_us=0; bool draw_bypassed=false; };
const char* backend();
bool install(IDXGISwapChain* discovery,HWND game);
// The menu owns its immediate-context resources and input queue. These callbacks
// run inside the guarded native swapchain hooks, independently of world markers.
using menu_draw_fn=void (*)(IDXGISwapChain*);
using menu_resize_fn=void (*)();
void set_menu_callbacks(menu_draw_fn draw,menu_resize_fn resize);
using world_view_reader=bool (*)();
void set_world_view_reader(world_view_reader);
void set_interactive_window(HWND menu);
HWND active_menu_window();
bool shutdown();
void publish(const scene&,const std::array<tile_pixels,capacity>&);
metrics status();
void set_draw_bypass(bool bypass);
bool is_draw_bypassed();
// A bounded opt-in Present interval sample, started by a diagnostic bypass change.
struct frame_sample {unsigned count=0,p50_us=0,p95_us=0,p99_us=0,maximum_us=0;};
frame_sample read_frame_sample();
}
