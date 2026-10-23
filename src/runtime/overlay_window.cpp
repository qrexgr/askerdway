/**
 * @file
 * @brief Нативная отрисовка игрового оверлея.
 *
 * Создаёт и освобождает окна/ресурсы, рисует миникарту, строку телепортов и мировые отметки.
 * Мировая проекция зависит от view matrix; миникарта использует собственное преобразование
 * координат. Временные GDI-объекты освобождаются после рисования.
 */

// DX11 overlay windows.

#include "../ui/portfolio_theme.h"
#include "../ui/interface_fonts.h"
#include "../ui/hero_portraits.h"
#include "../ui/sidebar_icons.h"
#include "../render/player_colors.h"
#include "../render/topbar_slots.h"
#include "../ui/topbar_health_pixels.h"
COLORREF teleport_player_color(std::uint32_t owner);

/**
 * @brief Создаёт ресурсы Windows/Direct3D на worker-потоке. При частичном отказе caller обязан
 * выполнить общий путь очистки.
 */
bool create_overlay_window(HMODULE module) {
    g_overlay.module = module;
    menu_preferences::current.load();
    g_render_camp_count.store(menu_preferences::current.camp_count);
    g_render_camp_gold.store(menu_preferences::current.camp_gold);
    g_render_camp_estimates.store(menu_preferences::current.camp_estimates);
    g_render_camp_background.store(menu_preferences::current.camp_background);
    g_overlay.game_window = find_game_window();
    if (g_overlay.game_window == nullptr)
        return false;

    RECT bounds{};
    if (!get_game_client_bounds(bounds))
        return false;
    bounds = compact_overlay_bounds(bounds);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_CLASSDC;
    window_class.lpfnWndProc = overlay_window_proc;
    window_class.hInstance = module;
#if defined(ASKERDWAY_COMBINED)
    window_class.lpszClassName = L"AskerdwayOverlay";
#else
    window_class.lpszClassName = L"DotaArmletMonitorOverlay";
#endif
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    g_overlay.width = static_cast<UINT>(bounds.right - bounds.left);
    g_overlay.height = static_cast<UINT>(bounds.bottom - bounds.top);
    g_overlay.x = bounds.left;
    g_overlay.y = bounds.top;
    g_overlay.window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
#if defined(ASKERDWAY_COMBINED)
        window_class.lpszClassName, L"askerdway", WS_POPUP,
#else
        window_class.lpszClassName, L"Dota 2 Armlet overlay", WS_POPUP,
#endif
        bounds.left, bounds.top, static_cast<int>(g_overlay.width), static_cast<int>(g_overlay.height),
        g_overlay.game_window, nullptr, module, nullptr);

    if (g_overlay.window == nullptr)
        return false;

    // ImGui fills this compact swap-chain surface completely. Do not use a
    // color key with DXGI: on some DWM paths it makes hit testing/composition
    // inconsistent without providing per-pixel transparency.
    SetLayeredWindowAttributes(g_overlay.window, RGB(0, 0, 0), 255, LWA_ALPHA);

    // This compact layered window is independent of the ImGui panel, allowing
    // an enable/disable message to sit near the screen centre without making a
    // full-screen transparent DX surface (which can render as black).
    g_overlay.notification_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
#if defined(ASKERDWAY_COMBINED)
        window_class.lpszClassName, L"askerdway Armlet status", WS_POPUP,
#else
        window_class.lpszClassName, L"Dota 2 Armlet status", WS_POPUP,
#endif
        0, 0, 360, 88, g_overlay.game_window, nullptr, module, nullptr);
    if (g_overlay.notification_window == nullptr)
        return false;
    SetLayeredWindowAttributes(g_overlay.notification_window, RGB(0, 0, 0), 191,
        LWA_COLORKEY | LWA_ALPHA);

    // Native, click-through color-keyed surfaces keep gameplay intelligence
    // independent of Panorama/JavaScript and avoid a full-screen DXGI surface.
    g_overlay.minimap_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED |
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        window_class.lpszClassName, L"askerdway minimap", WS_POPUP,
        0, 0, 320, 320, g_overlay.game_window, nullptr, module, nullptr);
    if (g_overlay.minimap_window == nullptr)
        return false;
    SetLayeredWindowAttributes(g_overlay.minimap_window,
        RGB(0, 0, 0), 255, LWA_COLORKEY);

    const MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g_overlay.window, &margins);

    if (!create_device_and_swap_chain(g_overlay.window, g_overlay.width, g_overlay.height))
        return false;

    if (!hero_portraits::initialize(module))
        log_message("ERROR", "Could not load the embedded hero portrait atlas.");
    if (!ability_icons::initialize(module))
        log_message("ERROR", "Could not load the embedded ability icon atlas.");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    if (!interface_fonts::load(module)) {
        log_message("ERROR", "Could not load embedded Nordwand Mono fonts.");
        return false;
    }
    portfolio_theme::apply();

    if (!ImGui_ImplWin32_Init(g_overlay.game_window))
        return false;
    g_overlay.imgui_win32_initialized = true;

    world_frame::set_menu_callbacks(&render_native_menu,&invalidate_native_menu_target);
    if (!world_frame::install(g_overlay.swap_chain,g_overlay.game_window))
        log_message("ERROR","Game Present hook unavailable; world markers remain disabled.");

    return true;
}

/**
 * @brief Освобождает принадлежащие оверлею графические ресурсы; не уничтожает само окно игры.
 */
void shutdown_world_tile_surface();

void destroy_overlay(HMODULE module) {
    g_native_menu_open.store(false,std::memory_order_release);
    world_frame::set_menu_callbacks(nullptr,nullptr);
    destroy_native_menu_resources();
    shutdown_world_tile_surface();
    uninstall_game_window_order_dispatch();

    if (g_overlay.imgui_dx11_initialized) {
        ImGui_ImplDX11_Shutdown();
        g_overlay.imgui_dx11_initialized = false;
    }
    if (g_overlay.imgui_win32_initialized) {
        ImGui_ImplWin32_Shutdown();
        g_overlay.imgui_win32_initialized = false;
    }
    if (ImGui::GetCurrentContext() != nullptr)
        ImGui::DestroyContext();
    interface_fonts::shutdown();

    sidebar_icons::shutdown();
    hero_portraits::shutdown();
    ability_icons::shutdown();

    destroy_render_target();
    if (g_overlay.swap_chain != nullptr) {
        g_overlay.swap_chain->Release();
        g_overlay.swap_chain = nullptr;
    }
    if (g_overlay.context != nullptr) {
        g_overlay.context->Release();
        g_overlay.context = nullptr;
    }
    if (g_overlay.device != nullptr) {
        g_overlay.device->Release();
        g_overlay.device = nullptr;
    }
    if (g_overlay.window != nullptr) {
        DestroyWindow(g_overlay.window);
        g_overlay.window = nullptr;
    }
    if (g_overlay.notification_window != nullptr) {
        DestroyWindow(g_overlay.notification_window);
        g_overlay.notification_window = nullptr;
    }
    if (g_overlay.minimap_window != nullptr) {
        DestroyWindow(g_overlay.minimap_window);
        g_overlay.minimap_window = nullptr;
    }
    g_overlay.module = nullptr;

#if defined(ASKERDWAY_COMBINED)
    UnregisterClassW(L"AskerdwayOverlay", module);
#else
    UnregisterClassW(L"DotaArmletMonitorOverlay", module);
#endif
}

/**
 * @brief Синхронизирует положение native-окон с клиентской областью игры; возвращает
 * доступность корректных границ для текущего кадра.
 */
bool resize_and_position_overlay() {
    RECT game_bounds{};
    if (!get_game_client_bounds(game_bounds))
        return false;
    RECT bounds = compact_overlay_bounds(game_bounds);

    const LONG width_long = bounds.right - bounds.left;
    const LONG height_long = bounds.bottom - bounds.top;
    if (g_overlay.user_positioned) {
        LONG desired_x = bounds.left + g_overlay.drag_offset_x;
        LONG desired_y = bounds.top + g_overlay.drag_offset_y;
        const LONG maximum_x = std::max(game_bounds.left,
            game_bounds.right - width_long);
        const LONG maximum_y = std::max(game_bounds.top,
            game_bounds.bottom - height_long);
        desired_x = std::clamp(desired_x, game_bounds.left, maximum_x);
        desired_y = std::clamp(desired_y, game_bounds.top, maximum_y);
        g_overlay.drag_offset_x = desired_x - bounds.left;
        g_overlay.drag_offset_y = desired_y - bounds.top;
        bounds = {desired_x, desired_y,
            desired_x + width_long, desired_y + height_long};
    }

    const UINT width = static_cast<UINT>(bounds.right - bounds.left);
    const UINT height = static_cast<UINT>(bounds.bottom - bounds.top);
    const bool size_changed = width != g_overlay.width || height != g_overlay.height;
    const bool position_changed = bounds.left != g_overlay.x || bounds.top != g_overlay.y;
    if (!size_changed && !position_changed)
        return true;

    SetWindowPos(g_overlay.window, HWND_TOPMOST, bounds.left, bounds.top,
        static_cast<int>(width), static_cast<int>(height),
        SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    g_overlay.x = bounds.left;
    g_overlay.y = bounds.top;
    if (!size_changed)
        return true;

    g_overlay.width = width;
    g_overlay.height = height;
    destroy_render_target();
    if (FAILED(g_overlay.swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)) ||
        !create_render_target()) {
        log_message("ERROR", "Could not resize the overlay swap chain.");
        return false;
    }

    return true;
}

/**
 * @brief Переключает приём ввода меню; игровые информационные окна сохраняют отдельную click-
 * through модель.
 */
void set_overlay_interactive(bool interactive) {
    if (g_overlay.window == nullptr)
        return;
    g_overlay.interactive = interactive;
    if(!interactive) {
        g_overlay.native_moving=false;
        if(GetCapture()==g_overlay.window)ReleaseCapture();
    }
    world_frame::set_interactive_window(interactive?g_overlay.window:nullptr);
    LONG_PTR style = GetWindowLongPtrW(g_overlay.window, GWL_EXSTYLE);
    if (interactive)
        style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT|WS_EX_NOACTIVATE);
    else
        style |= WS_EX_TRANSPARENT|WS_EX_NOACTIVATE;
    SetWindowLongPtrW(g_overlay.window, GWL_EXSTYLE, style);
    SetWindowPos(g_overlay.window, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if(!interactive&&GetForegroundWindow()==g_overlay.window)SetForegroundWindow(g_overlay.game_window);
}

void update_enable_notification(bool game_is_foreground) {
    constexpr ULONGLONG display_duration_ms = 1800;
    constexpr int notification_width = 360;
    constexpr int notification_height = 88;

    if (g_overlay.notification_window == nullptr)
        return;

    const ULONGLONG shown_at = g_enable_notification.shown_at.load(
        std::memory_order_relaxed);
    const bool active = game_is_foreground && shown_at != 0 &&
        GetTickCount64() - shown_at < display_duration_ms;
    if (!active) {
        ShowWindow(g_overlay.notification_window, SW_HIDE);
        return;
    }

    RECT game_bounds{};
    if (!get_game_client_bounds(game_bounds))
        return;

    const int game_width = game_bounds.right - game_bounds.left;
    const int game_height = game_bounds.bottom - game_bounds.top;
    const int x = game_bounds.left + (game_width - notification_width) / 2;
    // Place the text a little above the exact vertical centre of the game.
    const int y = game_bounds.top + game_height / 2 - notification_height / 2 - 70;
    SetWindowPos(g_overlay.notification_window, HWND_TOPMOST, x, y,
        notification_width, notification_height, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    const std::uint64_t serial = g_enable_notification.serial.load(
        std::memory_order_acquire);
    if (serial == g_overlay.displayed_notification)
        return;

    const bool enabled = g_enable_notification.enabled.load(std::memory_order_relaxed);
    HDC dc = GetDC(g_overlay.notification_window);
    if (dc == nullptr)
        return;

    RECT bounds{0, 0, notification_width, notification_height};
    const HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &bounds, background);
    DeleteObject(background);

    HFONT font = CreateFontW(-56, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, interface_fonts::family);
    HGDIOBJ previous_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? RGB(64, 220, 96) : RGB(255, 75, 72));
    DrawTextW(dc, enabled ? L"Enabled" : L"Disabled", -1, &bounds,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, previous_font);
    DeleteObject(font);
    ReleaseDC(g_overlay.notification_window, dc);
    g_overlay.displayed_notification = serial;
}

/**
 * @brief Преобразует мировые координаты в пиксели миникарты с её bounds/размером; это не
 * перспективная проекция.
 */
bool read_minimap_hud_rectangle(const RECT& viewport,minimap_hud::rectangle& result) {
    static std::uintptr_t panel=0;
    static ULONGLONG next=0;
    static minimap_hud::rectangle cached{};
    static bool valid=false;
    const auto now=GetTickCount64();
    if(now<next){if(valid)result=cached;return valid;}
    next=now+500;
    const auto read=[](std::uintptr_t address,void* data,std::size_t size){return safe_copy_memory(address,data,size);};
    const float width=float(viewport.right-viewport.left),height=float(viewport.bottom-viewport.top);
    if(panel&&minimap_hud::sample(read,panel,width,height,cached))valid=true;
    else {
        panel=0;valid=false;
        const auto panorama=GetModuleHandleW(L"panorama.dll");
        using factory_fn=void*(*)(const char*,int*);
        const auto factory=panorama?reinterpret_cast<factory_fn>(GetProcAddress(panorama,"CreateInterface")):nullptr;
        const auto object=factory?reinterpret_cast<std::uintptr_t>(factory("PanoramaUIEngine001",nullptr)):0;
        std::uintptr_t table=0,getter=0,engine=0;std::array<unsigned char,5> code{};
        // A verified getter is a plain load. Do not invoke arbitrary UI methods on the worker.
        if(object&&safe_read(object,table)&&safe_read(table+13*8,getter)&&safe_read(getter,code)&&
           code==std::array<unsigned char,5>{0x48,0x8b,0x41,0x28,0xc3}&&safe_read(object+0x28,engine)){
            panel=minimap_hud::find(read,engine,width,height);
            valid=panel&&minimap_hud::sample(read,panel,width,height,cached);
        }
        if(!valid)next=now+2000;
    }
    g_minimap_hud_pixels.store(valid?int(std::lround(cached.width)):0);
    g_minimap_hud_scale.store(valid?int(std::lround(cached.scale*100)):100);
    if(valid)result=cached;
    return valid;
}

POINT world_to_minimap(const armlet_vec3& position, int size,
    const armlet_vec3& map_minimum, const armlet_vec3& map_maximum) {
    // The two live dota_minimap_boundary entities define the exact transform
    // used by the current map. Clamping keeps transient particle positions
    // outside that envelope from drawing outside the window.
    armlet_vec3 minimum = map_minimum;
    armlet_vec3 maximum = map_maximum;
    const float boundary_span_x = maximum.x - minimum.x;
    const float boundary_span_y = maximum.y - minimum.y;
    if (boundary_span_x > 16384.0f && boundary_span_x < 17000.0f) {
        const float inset = (boundary_span_x - 16384.0f) * 0.5f;
        minimum.x += inset;
        maximum.x -= inset;
    }
    if (boundary_span_y > 16384.0f && boundary_span_y < 17000.0f) {
        const float inset = (boundary_span_y - 16384.0f) * 0.5f;
        minimum.y += inset;
        maximum.y -= inset;
    }
    const float world_span_x = std::max(1.0f, maximum.x - minimum.x);
    const float world_span_y = std::max(1.0f, maximum.y - minimum.y);
    const float normalized_x = std::clamp(
        (position.x - minimum.x) / world_span_x,
        0.0f, 1.0f);
    const float normalized_y = std::clamp(
        (position.y - minimum.y) / world_span_y,
        0.0f, 1.0f);
    return {
        static_cast<LONG>(std::lround(normalized_x * (size - 1))),
        static_cast<LONG>(std::lround((1.0f - normalized_y) * (size - 1))),
    };
}

/**
 * @brief Рисует trace/camp/teleport snapshot на native-поверхности. Кольца TP используют
 * cast_started_at и duration_ms, а не время последней частицы.
 */
void draw_render_minimap_overlay(bool game_is_foreground) {
    const auto frame=world_frame::status();
    if(frame.object_hook&&frame.gpu_ready) {
        if(g_overlay.minimap_window)ShowWindow(g_overlay.minimap_window,SW_HIDE);
        return;
    }
    const bool show_all_traces = g_render_trace_icons.load(std::memory_order_relaxed);
    const bool show_waves=g_render_creep_tracker.load(std::memory_order_relaxed);
    const bool show_traces=show_all_traces||show_waves||g_render_camp_status.load(std::memory_order_relaxed);
    const bool show_camps = g_render_camp_status.load(std::memory_order_relaxed);
    const bool show_teleports = g_render_teleport_arrows.load(
        std::memory_order_relaxed);
    const bool has_content =
        (show_traces && g_render_trace_count_snapshot.load(
            std::memory_order_acquire) != 0) ||
        (show_teleports && g_render_teleport_count_snapshot.load(
            std::memory_order_acquire) != 0) ||
        (show_camps && g_render_diagnostics.blocked_camps.load(
            std::memory_order_acquire) != 0) ||
        g_render_effect_mark_count_snapshot.load(std::memory_order_acquire)!=0;
    if (g_overlay.minimap_window == nullptr || !game_is_foreground ||
        (!show_traces && !show_camps && !show_teleports) || !has_content ||
        IsIconic(g_overlay.game_window)) {
        if (g_overlay.minimap_window != nullptr)
            ShowWindow(g_overlay.minimap_window, SW_HIDE);
        return;
    }

    RECT game_bounds{};
    if (!get_game_client_bounds(game_bounds)) {
        ShowWindow(g_overlay.minimap_window, SW_HIDE);
        return;
    }
    const int game_height = game_bounds.bottom - game_bounds.top;
    const int game_width = game_bounds.right - game_bounds.left;
    const float native_scale = static_cast<float>(std::clamp(
        g_render_minimap_size_percent.load(std::memory_order_relaxed),
        60, 150)) / 100.0f;
    const int maximum_minimap_size = std::max(1,
        std::min(520, std::min(game_height, game_width / 2)));
    const int minimum_minimap_size = std::min(160, maximum_minimap_size);
    const int minimap_size = std::clamp(
        static_cast<int>(std::lround(
            320.0f * static_cast<float>(game_height) / 1080.0f * native_scale)),
        minimum_minimap_size, maximum_minimap_size);
    const bool on_right = g_render_minimap_right.load(std::memory_order_relaxed);
    const int map_offset_x = std::clamp(
        g_render_minimap_offset_x.load(std::memory_order_relaxed), -120, 120);
    const int map_offset_y = std::clamp(
        g_render_minimap_offset_y.load(std::memory_order_relaxed), -120, 120);
    // The observed 1080p HUD uses a 320 px minimap. Size/side/calibration
    // remain configurable for alternate HUD layouts and minimap settings.
    const LONG minimap_extent = static_cast<LONG>(minimap_size);
    const LONG requested_x =
        (on_right ? game_bounds.right - minimap_size : game_bounds.left) +
        map_offset_x;
    const LONG requested_y = game_bounds.bottom - minimap_extent + map_offset_y;
    const LONG x = std::clamp(requested_x, game_bounds.left,
        game_bounds.right - minimap_extent);
    const LONG y = std::clamp(requested_y, game_bounds.top,
        game_bounds.bottom - minimap_extent);
    static HWND positioned_window = nullptr;
    static RECT positioned_bounds{};
    const RECT requested_bounds{x, y, x + minimap_size, y + minimap_size};
    const bool geometry_changed = positioned_window != g_overlay.minimap_window ||
        std::memcmp(&positioned_bounds, &requested_bounds,
            sizeof(requested_bounds)) != 0;
    if (geometry_changed || !IsWindowVisible(g_overlay.minimap_window)) {
        SetWindowPos(g_overlay.minimap_window, HWND_TOPMOST, x, y,
            minimap_size, minimap_size, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        positioned_window = g_overlay.minimap_window;
        positioned_bounds = requested_bounds;
    }

    HDC window_dc = GetDC(g_overlay.minimap_window);
    if (window_dc == nullptr)
        return;
    HDC memory_dc = CreateCompatibleDC(window_dc);
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = minimap_size;
    bitmap_info.bmiHeader.biHeight = -minimap_size;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* bitmap_storage = nullptr;
    HBITMAP bitmap = CreateDIBSection(window_dc, &bitmap_info,
        DIB_RGB_COLORS, &bitmap_storage, nullptr, 0);
    if (memory_dc == nullptr || bitmap == nullptr) {
        if (bitmap != nullptr)
            DeleteObject(bitmap);
        if (memory_dc != nullptr)
            DeleteDC(memory_dc);
        ReleaseDC(g_overlay.minimap_window, window_dc);
        return;
    }

    HGDIOBJ previous_bitmap = SelectObject(memory_dc, bitmap);
    RECT surface{0, 0, minimap_size, minimap_size};
    HBRUSH transparent_key = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(memory_dc, &surface, transparent_key);
    DeleteObject(transparent_key);
    SetBkMode(memory_dc, TRANSPARENT);

    auto* bitmap_pixels = static_cast<std::uint8_t*>(bitmap_storage);
    const int bitmap_stride = minimap_size * 4;
    const int icon_scale = std::clamp(
        g_render_hero_icon_scale_percent.load(std::memory_order_relaxed), 0, 100);
    const int hero_icon_diameter = std::max(10,
        static_cast<int>(std::lround(
            static_cast<float>(minimap_size) *
            (1.0f / 13.0f + (1.0f / 10.0f - 1.0f / 13.0f) *
                static_cast<float>(icon_scale) / 100.0f))));
    const int hero_icon_radius = std::max(5, hero_icon_diameter / 2);
    const auto draw_hero_portrait = [&](const POINT& point,
        const char* designer_name, bool blue_tint, float opacity,
        COLORREF fallback_color) {
        if (bitmap_pixels == nullptr)
            return false;
        // GDI may batch previous vector drawing. Flush before direct access to
        // the DIB so the portrait is composed onto the completed surface.
        GdiFlush();
        const bool rendered = hero_portraits::blit_bgra(bitmap_pixels, minimap_size,
            minimap_size, bitmap_stride, point.x, point.y,
            hero_icon_diameter, designer_name, blue_tint, opacity);
        if (rendered)
            return true;

        // Entity names can be temporarily unavailable while a hero is already
        // dormant. Never turn a valid event into an invisible marker merely
        // because the portrait atlas lookup failed.
        HPEN border = CreatePen(PS_SOLID, 2, fallback_color);
        HBRUSH fill = CreateSolidBrush(RGB(24, 29, 36));
        HGDIOBJ previous_pen = SelectObject(memory_dc, border);
        HGDIOBJ previous_brush = SelectObject(memory_dc, fill);
        Ellipse(memory_dc, point.x - hero_icon_radius,
            point.y - hero_icon_radius, point.x + hero_icon_radius + 1,
            point.y + hero_icon_radius + 1);
        SelectObject(memory_dc, previous_brush);
        SelectObject(memory_dc, previous_pen);
        DeleteObject(fill);
        DeleteObject(border);

        char abbreviation[4]{};
        hero_abbreviation(designer_name, abbreviation);
        HFONT font = CreateFontW(-std::max(10, hero_icon_diameter / 2),
            0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            FIXED_PITCH | FF_MODERN, interface_fonts::family);
        HGDIOBJ previous_font = SelectObject(memory_dc, font);
        RECT label{point.x - hero_icon_radius, point.y - hero_icon_radius,
            point.x + hero_icon_radius + 1, point.y + hero_icon_radius + 1};
        SetTextColor(memory_dc, RGB(245, 248, 252));
        DrawTextA(memory_dc, abbreviation, -1, &label,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(memory_dc, previous_font);
        DeleteObject(font);
        return true;
    };

    // External API readers use the same shared lock. The worker only mutates
    // these arrays during poll_render_intelligence under an exclusive lock.
    render_intelligence_shared_guard intelligence_guard;
    const armlet_vec3 map_minimum = g_render_intelligence.has_minimap_bounds
        ? g_render_intelligence.minimap_minimum
        : armlet_vec3{-8288.0f, -8288.0f, 0.0f};
    const armlet_vec3 map_maximum = g_render_intelligence.has_minimap_bounds
        ? g_render_intelligence.minimap_maximum
        : armlet_vec3{8288.0f, 8288.0f, 0.0f};

    if (show_camps) {
        HGDIOBJ previous_brush = SelectObject(memory_dc, GetStockObject(HOLLOW_BRUSH));
        for (std::size_t index = 0; index < g_render_intelligence.camp_count; ++index) {
            const auto& camp = g_render_intelligence.camps[index];
            if(!camp.blocked)continue;
            const POINT point = world_to_minimap(
                camp.position, minimap_size, map_minimum, map_maximum);
            const int radius = camp.type == 2 ? 8 : (camp.type == 1 ? 7 : 6);
            HPEN pen = CreatePen(PS_SOLID, 2,
                camp.blocked ? RGB(255, 45, 45) : RGB(245, 205, 45));
            HGDIOBJ previous_pen = SelectObject(memory_dc, pen);
            Rectangle(memory_dc, point.x - radius, point.y - radius,
                point.x + radius + 1, point.y + radius + 1);
            SelectObject(memory_dc, previous_pen);
            DeleteObject(pen);
        }
        SelectObject(memory_dc, previous_brush);
    }

    if (show_teleports) {
        const ULONGLONG now = GetTickCount64();
        for (std::size_t index = 0;
             index < g_render_intelligence.teleport_count; ++index) {
            const auto& teleport = g_render_intelligence.teleports[index];
            if (!teleport.enemy_confirmed || teleport.canceled ||
                teleport.expires_at <= now)
                continue;
            POINT origin{}, destination{};
            if (teleport.has_origin)
                origin = world_to_minimap(teleport.origin, minimap_size, map_minimum, map_maximum);
            if (teleport.has_destination)
                destination = world_to_minimap(teleport.destination, minimap_size, map_minimum, map_maximum);
            if (!teleport.channel_finished && teleport.has_origin && teleport.has_destination) {
                HPEN pen = CreatePen(PS_SOLID, 2, teleport_player_color(teleport.owner_handle));
                const auto old_pen = SelectObject(memory_dc, pen);
                MoveToEx(memory_dc, origin.x, origin.y, nullptr);
                LineTo(memory_dc, destination.x, destination.y);
                const float length=std::hypot(float(destination.x-origin.x),float(destination.y-origin.y));
                if(length>20) {
                    const float dx=(destination.x-origin.x)/length,dy=(destination.y-origin.y)/length;
                    // End at the destination portrait's edge, keeping the head visible.
                    const float tip_x=destination.x-dx*18.f;
                    const float tip_y=destination.y-dy*18.f;
                    MoveToEx(memory_dc,int(tip_x-dx*8-dy*5),int(tip_y-dy*8+dx*5),nullptr);
                    LineTo(memory_dc,int(tip_x),int(tip_y));
                    LineTo(memory_dc,int(tip_x-dx*8+dy*5),int(tip_y-dy*8-dx*5));
                }
                SelectObject(memory_dc, old_pen);
                DeleteObject(pen);
            }
            if (!teleport.channel_finished && teleport.has_origin)
                draw_hero_portrait(origin, teleport.designer_name, false, 0.80f, RGB(104, 139, 149));
            if (teleport.has_destination)
                draw_hero_portrait(destination, teleport.designer_name, false, 0.95f, RGB(137, 160, 157));
        }
    }

    if (show_traces) {
        for (std::size_t index = 0; index < g_render_intelligence.trace_count; ++index) {
            const auto& trace = g_render_intelligence.traces[index];
            if(trace.creep_wave?!show_waves:(!show_all_traces&&!position_is_near_neutral_farm(trace.position)))continue;
            bool teleport_marker=false;
            if(show_teleports)for(std::size_t j=0;j<g_render_intelligence.teleport_count;++j) {
                const auto& route=g_render_intelligence.teleports[j];
                if(route.enemy_confirmed&&!route.canceled&&route.expires_at>GetTickCount64()&&
                    same_entity_handle(route.owner_handle,trace.owner_handle))teleport_marker=true;
            }
            if(teleport_marker)continue;
            const POINT point = world_to_minimap(
                trace.position, minimap_size, map_minimum, map_maximum);
            if(trace.creep_wave) {
                const auto pen=CreatePen(PS_SOLID,2,RGB(232,220,73));
                const auto old_pen=SelectObject(memory_dc,pen),old_brush=SelectObject(memory_dc,GetStockObject(HOLLOW_BRUSH));
                Ellipse(memory_dc,point.x-6,point.y-6,point.x+7,point.y+7);
                SelectObject(memory_dc,old_pen);SelectObject(memory_dc,old_brush);DeleteObject(pen);
                continue;
            }
            if (!draw_hero_portrait(point, trace.designer_name, false, 0.96f,
                    RGB(147, 155, 166)))
                continue;
            HGDIOBJ previous_brush = SelectObject(
                memory_dc, GetStockObject(HOLLOW_BRUSH));
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(147, 155, 166));
            HGDIOBJ previous_pen = SelectObject(memory_dc, pen);
            Ellipse(memory_dc, point.x - hero_icon_radius,
                point.y - hero_icon_radius,
                point.x + hero_icon_radius + 1,
                point.y + hero_icon_radius + 1);
            SelectObject(memory_dc, previous_pen);
            SelectObject(memory_dc, previous_brush);
            DeleteObject(pen);
            if (trace.ability_icon[0] != '\0') {
                GdiFlush();
                ability_icons::blit_bgra(bitmap_pixels, minimap_size, minimap_size,
                    bitmap_stride, point.x + hero_icon_radius - 1,
                    point.y + hero_icon_radius - 1, 15, trace.ability_icon);
                if(trace.estimated_position){
                    HPEN border=CreatePen(PS_SOLID,2,RGB(255,255,255));
                    const auto old_pen=SelectObject(memory_dc,border),old_brush=SelectObject(memory_dc,GetStockObject(HOLLOW_BRUSH));
                    const int ax=point.x+hero_icon_radius-1,ay=point.y+hero_icon_radius-1;
                    Ellipse(memory_dc,ax-8,ay-8,ax+9,ay+9);
                    SelectObject(memory_dc,old_pen);SelectObject(memory_dc,old_brush);DeleteObject(border);
                }
            }
        }
    }

    const auto mark_time=GetTickCount64();
    for(const auto& mark:g_render_intelligence.marks.entries) {
        if(mark.expires<=mark_time||(mark.type==effect_marks::kind::drop?!show_camps:!show_all_traces))continue;
        const auto point=world_to_minimap({mark.position[0],mark.position[1],mark.position[2]},minimap_size,map_minimum,map_maximum);
        GdiFlush();ability_icons::blit_bgra(bitmap_pixels,minimap_size,minimap_size,bitmap_stride,
            point.x,point.y,20,mark.icon);
    }

    BitBlt(window_dc, 0, 0, minimap_size, minimap_size,
        memory_dc, 0, 0, SRCCOPY);
    SelectObject(memory_dc, previous_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(g_overlay.minimap_window, window_dc);
    g_render_diagnostics.minimap_frames.fetch_add(
        1, std::memory_order_relaxed);
}


// Called only while holding the intelligence lock. No game-memory reads or
// entity scans in either renderer's draw path.
COLORREF teleport_player_color(std::uint32_t owner) {
    for(std::size_t i=0;i<g_render_intelligence.enemy_visibility_count;++i) {
        const auto& hero=g_render_intelligence.enemy_visibility[i];
        if(same_entity_handle(hero.entity_index,owner))return player_colors::muted(hero.player_id);
    }
    return player_colors::muted(-1);
}

// All world marks share one game-device atlas. The worker only updates tile
// contents when hero/ability/timer changes, and publishes world coordinates.
struct world_tile_cache {
    int diameter=0;
    bool estimated=false,creep=false;
    char hero[96]{},ability[96]{},label[64]{};
    std::uint64_t version=0;
};
std::array<world_tile_cache,world_frame::capacity> g_world_tile_cache{};
std::array<world_frame::tile_pixels,world_frame::capacity> g_world_tiles{};
std::uint64_t g_world_tile_version=0;
tile_raster::surface g_world_surface;
void shutdown_world_tile_surface(){g_world_surface.reset();}

bool rasterize_enemy_hud_tile(unsigned index,const enemy_teleport_status_entry& status,ULONGLONG /*now*/) {
    const bool health_known=status.health>=0&&status.max_health>0&&status.max_health<1000000;
    const int health_width=health_known?std::clamp(int(60.0*status.health/status.max_health),0,60):-1;
    // Worker already extrapolates using pause-aware game time. Wall-clock
    // subtraction here made ALT flicker across second boundaries while paused.
    const int cooldown=status.cooldown_seconds>=0?
        int(std::ceil(std::max(0.f,status.cooldown_seconds))):-1;
    char key[64]{};
    std::snprintf(key,sizeof(key),"HUD %d %d %d %d %d %d %d",health_width,status.visible,
        status.inventory_known,status.has_scroll,status.charges,cooldown,status.cooldown_estimated);
    auto& cache=g_world_tile_cache[index];
    if(cache.version&&std::strcmp(cache.label,key)==0)return true;
    constexpr int w=world_frame::tile_width,h=world_frame::tile_height,cx=w/2;
    if(!g_world_surface.begin(w,h))return false;
    void* pixels=g_world_surface.pixels;
    auto* bgra=static_cast<std::uint8_t*>(pixels);
    if(health_known)for(int y=0;y<topbar_health::height;++y)for(int x=cx-30;x<cx+30;++x) {
        auto* p=bgra+(y*w+x)*4;
        const bool filled=x<cx-30+health_width;
        const std::uint32_t pixel=filled?topbar_health::bgra[y*60+x-(cx-30)]:
            (y==0||y==7?0xff17200du:y==1?0xff222e13u:0xff1e2911u);
        std::memcpy(p,&pixel,sizeof(pixel));
    }
    if(status.inventory_known&&status.has_scroll)
        ability_icons::blit_bgra(bgra,w,h,w*4,cx,51,20,"item_tpscroll");
    HFONT font=g_world_surface.hud_font;
    auto text=[&](const char* label,RECT bounds) {
        RECT shadow=bounds;OffsetRect(&shadow,1,1);
        constexpr auto format=DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX;
        g_world_surface.text(label,shadow,format,RGB(8,10,12),font);
        g_world_surface.text(label,bounds,format,RGB(224,229,230),font);
    };
    if(!status.inventory_known)text("?",{cx-10,41,cx+10,61});
    else if(!status.has_scroll)text("-",{cx-10,41,cx+10,61});
    else {
        char count[16]{};std::snprintf(count,sizeof(count),status.charges>=0?"%d":"?",status.charges);
        text(count,{cx+4,53,cx+22,67});
        if(cooldown>0){char seconds[16]{};std::snprintf(seconds,sizeof(seconds),status.cooldown_estimated?"~%d":"%d",cooldown);text(seconds,{cx-16,41,cx+16,60});}
    }
    if(!status.visible)for(int i=0;i<w*h;++i)bgra[i*4+3]=static_cast<std::uint8_t>((unsigned(bgra[i*4+3])*160+127)/255);
    std::memcpy(g_world_tiles[index].data(),pixels,w*h*4);
    cache={};copy_detector_trigger(cache.label,key);cache.version=++g_world_tile_version;
    return true;
}

bool rasterize_world_tile(unsigned index,const render_trace_entry& trace,
    const char* label,int diameter) {
    auto& cache=g_world_tile_cache[index];
    if(cache.version&&cache.diameter==diameter&&cache.estimated==trace.estimated_position&&cache.creep==trace.creep_wave&&std::strcmp(cache.hero,trace.designer_name)==0&&
        std::strcmp(cache.ability,trace.ability_icon)==0&&std::strcmp(cache.label,label)==0) return true;
    constexpr int w=world_frame::tile_width,h=world_frame::tile_height,cx=w/2,cy=26;
    if(!g_world_surface.begin(w,h))return false;
    HDC dc=g_world_surface.dc;void* pixels=g_world_surface.pixels;
    if(trace.creep_wave) {
        auto* bgra=static_cast<std::uint8_t*>(pixels);
        for(int y=cy-6;y<=cy+6;++y)for(int x=cx-6;x<=cx+6;++x)if((x-cx)*(x-cx)+(y-cy)*(y-cy)<=36) {
            auto* p=bgra+(y*w+x)*4;p[0]=55;p[1]=55;p[2]=240;p[3]=255;
        }
    }
    else if(trace.designer_name[0])hero_portraits::blit_bgra(static_cast<std::uint8_t*>(pixels),w,h,w*4,cx,cy,diameter,trace.designer_name,false,1);
    else ability_icons::blit_bgra(static_cast<std::uint8_t*>(pixels),w,h,w*4,cx,cy,diameter,trace.ability_icon,1);
    const int radius=diameter/2;
    HPEN border=g_world_surface.border;
    const auto old_pen=SelectObject(dc,border),old_brush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
    if(!trace.creep_wave)Ellipse(dc,cx-radius-1,cy-radius-1,cx+radius+2,cy+radius+2);
    SelectObject(dc,old_pen);SelectObject(dc,old_brush);GdiFlush();
    if(trace.designer_name[0]&&trace.ability_icon[0])ability_icons::blit_bgra(static_cast<std::uint8_t*>(pixels),w,h,w*4,
        cx+radius+3,cy+radius-8,24,trace.ability_icon,1);
    if(trace.estimated_position) {
        GdiFlush();auto* bgra=static_cast<std::uint8_t*>(pixels);
        const int bx=trace.designer_name[0]?cx+radius+3:cx,by=trace.designer_name[0]?cy+radius-8:cy;
        const int r=trace.designer_name[0]?13:radius+2;
        for(int y=std::max(0,by-r);y<std::min(h,by+r+1);++y)for(int x=std::max(0,bx-r);x<std::min(w,bx+r+1);++x) {
            const int d=(x-bx)*(x-bx)+(y-by)*(y-by);
            if(d<=r*r&&d>=(r-2)*(r-2)){auto* p=bgra+(y*w+x)*4;p[0]=p[1]=p[2]=p[3]=255;}
        }
    }
    HFONT font=g_world_surface.label_font;
    g_world_surface.finish_gdi();
    RECT bounds{3,cy+radius+4,w-3,h-2},shadow=bounds;OffsetRect(&shadow,1,1);
    constexpr auto format=DT_CENTER|DT_TOP|DT_SINGLELINE|DT_NOPREFIX;
    g_world_surface.text(label,shadow,format,RGB(12,15,19),font);
    g_world_surface.text(label,bounds,format,RGB(193,203,211),font);
    std::memcpy(g_world_tiles[index].data(),pixels,w*h*4);
    cache.diameter=diameter;cache.estimated=trace.estimated_position;cache.creep=trace.creep_wave;copy_designer_name(cache.hero,trace.designer_name);
    copy_designer_name(cache.ability,trace.ability_icon);copy_detector_trigger(cache.label,label);
    cache.version=++g_world_tile_version;return true;
}

bool rasterize_camp_world_tile(unsigned index,const jungle_status::entry& camp,camp_world_tile::options visible) {
    auto& cache=g_world_tile_cache[index];
    char key[64]{};
    std::snprintf(key,sizeof(key),"CAMP %d %d %d %d %d %d %d %d %d",camp.count,camp.count_max,
        camp.gold_min,camp.gold_max,camp.gold_known,camp.current,visible.count,visible.gold,visible.background);
    if(cache.version&&std::strcmp(cache.label,key)==0)return true;
    if(!camp_world_tile::draw(g_world_surface,camp,visible))return false;
    std::memcpy(g_world_tiles[index].data(),g_world_surface.pixels,world_frame::tile_width*world_frame::tile_height*4);
    cache={};copy_detector_trigger(cache.label,key);cache.version=++g_world_tile_version;
    return true;
}

void draw_render_world_traces(bool world_visible) {
    // Camera projection happens in Present; tile publication needs only 20Hz.
    // Dashboard/minimization retire the scene; focus loss does not.
    static ULONGLONG next_publish=0;
    const auto tick=GetTickCount64();
    if(world_visible&&tick<next_publish)return;
    next_publish=tick+50;
    world_frame::scene scene{};
    scene.enabled=world_visible;
    scene.matrix_address=g_view_matrix_address.load(std::memory_order_acquire);
    const auto now=GetTickCount64();
    std::array<render_trace_entry,64> traces{};
    std::array<int,64> trace_health{};trace_health.fill(-1);
    std::array<jungle_status::entry,64> camp_statuses{};
    std::array<armlet_vec3,64> camp_positions{};
    std::array<int,64> camp_types{};
    float camp_time=0;
    std::size_t camp_count=0;
    std::array<render_teleport_entry,16> routes{};
    std::array<COLORREF,16> route_colors{};
    std::array<enemy_teleport_status_entry,16> hud{};
    std::array<effect_marks::entry,32> marks{};
    std::size_t hud_count=0;
    std::size_t trace_count=0,route_count=0;
    // Do not hold intelligence_lock across GDI work or renderer publication.
    if(scene.enabled) {
        render_intelligence_shared_guard guard;
        hud_count=std::min(hud.size(),g_render_intelligence.enemy_teleport_count);
        std::copy_n(g_render_intelligence.enemy_teleports.begin(),hud_count,hud.begin());
        marks=g_render_intelligence.marks.entries;
        for(std::size_t i=0;i<g_render_intelligence.pulse_count&&scene.pulse_count<scene.pulses.size();++i){
            const auto& source=g_render_intelligence.pulses[i];if(source.expires_at<=now)continue;
            auto& pulse=scene.pulses[scene.pulse_count++];pulse.center[0]=source.position.x;pulse.center[1]=source.position.y;pulse.center[2]=source.position.z+8;
            pulse.started=source.started_at;pulse.expires=source.expires_at;
        }
        scene.minimap.enabled=true;
        RECT viewport{};minimap_hud::rectangle bounds{};
        if(get_game_client_bounds(viewport)&&read_minimap_hud_rectangle(viewport,bounds)&&g_minimap_auto.load()){
            scene.minimap.automatic=true;
            scene.minimap.viewport_width=float(viewport.right-viewport.left);
            scene.minimap.viewport_height=float(viewport.bottom-viewport.top);
            scene.minimap.x=bounds.x;scene.minimap.y=bounds.y;
            scene.minimap.width=bounds.width;scene.minimap.height=bounds.height;
        }
        scene.minimap.on_right=g_render_minimap_right.load(std::memory_order_relaxed);
        scene.minimap.size_percent=g_render_minimap_size_percent.load(std::memory_order_relaxed);
        scene.minimap.offset_x=g_render_minimap_offset_x.load(std::memory_order_relaxed);
        scene.minimap.offset_y=g_render_minimap_offset_y.load(std::memory_order_relaxed);
        if(g_render_intelligence.has_minimap_bounds) {
            scene.minimap.minimum[0]=g_render_intelligence.minimap_minimum.x;
            scene.minimap.minimum[1]=g_render_intelligence.minimap_minimum.y;
            scene.minimap.maximum[0]=g_render_intelligence.minimap_maximum.x;
            scene.minimap.maximum[1]=g_render_intelligence.minimap_maximum.y;
        }
        if(g_render_camp_status.load(std::memory_order_relaxed)) {
            camp_count=std::min(camp_statuses.size(),g_render_intelligence.camp_count);
            std::copy_n(g_render_intelligence.jungle.entries.begin(),camp_count,camp_statuses.begin());
            for(std::size_t i=0;i<camp_count;++i){camp_positions[i]=g_render_intelligence.camps[i].position;camp_types[i]=g_render_intelligence.camps[i].type;}
            camp_time=g_render_intelligence.jungle.game_time;
            for(std::size_t i=0;i<g_render_intelligence.camp_count&&scene.area_count<scene.areas.size();++i){
                const auto& status=g_render_intelligence.jungle.entries[i];
                if(!status.possibly_cleared&&!status.empty_confirmed)continue;
                const auto& camp=g_render_intelligence.camps[i];
                auto& area=scene.areas[scene.area_count++];
                area.center[0]=camp.position.x;area.center[1]=camp.position.y;area.center[2]=camp.position.z+8;
                area.expires=now+750;area.cleared=true;area.uncertain=!status.empty_confirmed;
            }
            for(std::size_t i=0;i<g_render_intelligence.camp_count&&scene.area_count<scene.areas.size();++i) {
                const auto& camp=g_render_intelligence.camps[i];
                if(camp.blocked?!g_render_camp_alerts.load():!g_render_camp_boxes.load())continue;
                const auto half=camp.has_native_bounds
                    ? armlet_vec3{(camp.maximum.x-camp.minimum.x)*.5f,(camp.maximum.y-camp.minimum.y)*.5f,0}
                    : camp_block_half_extents(camp.type);
                auto& area=scene.areas[scene.area_count++];
                area.center[0]=camp.position.x;area.center[1]=camp.position.y;area.center[2]=camp.position.z+8;
                area.half[0]=half.x;area.half[1]=half.y;
                area.expires=g_render_intelligence.camps_refreshed_at+750;
                area.estimated=camp.estimated;
                area.blocked=camp.blocked;area.alt_only=!camp.blocked;
            }
        }
        if(g_render_trace_icons.load(std::memory_order_relaxed)||g_render_camp_status.load(std::memory_order_relaxed)||g_render_creep_tracker.load(std::memory_order_relaxed)) {
            for(std::size_t i=0;i<g_render_intelligence.trace_count;++i) {
                const auto& trace=g_render_intelligence.traces[i];
                if(trace.creep_wave?g_render_creep_tracker.load(std::memory_order_relaxed):
                    (g_render_trace_icons.load(std::memory_order_relaxed)||(g_render_camp_status.load(std::memory_order_relaxed)&&position_is_near_neutral_farm(trace.position))))
                {
                    for(std::size_t h=0;h<g_render_intelligence.enemy_visibility_count;++h){
                        const auto& hero=g_render_intelligence.enemy_visibility[h];
                        if(same_entity_handle(hero.entity_index,trace.owner_handle))trace_health[trace_count]=hero.last_health;
                    }
                    traces[trace_count++]=trace;
                }
            }
        }
        if(g_render_teleport_arrows.load(std::memory_order_relaxed)) {
            route_count=g_render_intelligence.teleport_count;
            std::copy_n(g_render_intelligence.teleports.begin(),route_count,routes.begin());
            for(std::size_t i=0;i<route_count;++i)route_colors[i]=teleport_player_color(routes[i].owner_handle);
        }
    }
    const int scale=std::clamp(g_render_hero_icon_scale_percent.load(std::memory_order_relaxed),0,100);
    const int diameter=32+static_cast<int>(std::lround(10.f*scale/100.f));
    if(g_render_teleport_arrows.load(std::memory_order_relaxed)) {
        const auto hud_slots=topbar_slots::assign(std::span<const enemy_teleport_status_entry>(hud.data(),hud_count));
        for(std::size_t i=0;i<hud_count&&scene.count<world_frame::capacity;++i) {
            const auto& status=hud[i];
            const int slot=hud_slots[i];
            if(slot<0)continue;
            const unsigned index=scene.count;
            if(!rasterize_enemy_hud_tile(index,status,now))continue;
            auto& marker=scene.markers[index];marker.screen_space=true;marker.alt_only=true;
            // Installed dota_hud_top_bar.vcss: 66px slot with -4px margin,
            // 62px score + 40px gap, 60x8 health at y39, TP indicator at y80.
            marker.position[0]=status.team==2?-102.f-62.f*(5-slot)+32.f:102.f+62.f*slot+30.f;
            marker.position[1]=65.f;marker.expires=now+500;
            marker.version=g_world_tile_cache[index].version;++scene.count;
        }
    }
    auto add=[&](const render_trace_entry& trace,const char* label) {
        if(scene.count>=world_frame::capacity||trace.expires_at<=now)return;
        const unsigned index=scene.count;
        if(!trace.creep_wave&&!rasterize_world_tile(index,trace,label,diameter))return;
        auto& marker=scene.markers[index];
        marker.position[0]=trace.position.x;marker.position[1]=trace.position.y;marker.position[2]=trace.position.z;
        marker.expires=trace.expires_at;marker.version=trace.creep_wave?0:g_world_tile_cache[index].version;++scene.count;
        marker.wave=trace.creep_wave;
        marker.minimap_icon=true;marker.icon_diameter=static_cast<float>(diameter);
        marker.minimap_source_icon=trace.designer_name[0]&&trace.ability_icon[0];
        marker.motion_key=trace.creep_wave?trace.source_event_key:0;
    };
    for(const auto& effect:marks) {
        if(effect.expires<=now||(effect.type==effect_marks::kind::drop
            ? !g_render_camp_status.load(std::memory_order_relaxed)
            : !g_render_trace_icons.load(std::memory_order_relaxed)))continue;
        render_trace_entry mark{};mark.expires_at=effect.expires;
        mark.position={effect.position[0],effect.position[1],effect.position[2]};
        copy_designer_name(mark.ability_icon,effect.icon);add(mark,effect.label);
    }
    for(std::size_t i=0;i<trace_count;++i) {
        bool teleport_marker=false;
        for(std::size_t j=0;j<route_count;++j)
            if(routes[j].enemy_confirmed&&!routes[j].canceled&&routes[j].expires_at>now&&
               same_entity_handle(routes[j].owner_handle,traces[i].owner_handle))teleport_marker=true;
        if(teleport_marker)continue;
        char label[64]{};
        if(traces[i].creep_wave)label[0]=0;
        else if(g_render_camp_status.load()&&trace_health[i]>0&&(std::strcmp(traces[i].detector_trigger,"Jungle farming")==0||std::strcmp(traces[i].detector_trigger,"Estimated farm")==0))
            std::snprintf(label,sizeof(label),"~%d HP",trace_health[i]);
        else if(traces[i].continuous)std::snprintf(label,sizeof(label),"Aura");
        else std::snprintf(label,sizeof(label),"%.1fs",now>=traces[i].created_at?float(now-traces[i].created_at)/1000.f:0.f);
        add(traces[i],label);
    }
    for(std::size_t i=0;i<route_count;++i) {
        const auto& tp=routes[i];
        if(!tp.enemy_confirmed||tp.canceled||tp.expires_at<=now)continue;
        if(tp.has_origin&&tp.has_destination&&!tp.channel_finished&&scene.direction_count<scene.directions.size()) {
            auto& d=scene.directions[scene.direction_count++];
            d.origin[0]=tp.origin.x;d.origin[1]=tp.origin.y;d.origin[2]=tp.origin.z;
            d.destination[0]=tp.destination.x;d.destination[1]=tp.destination.y;d.destination[2]=tp.destination.z;
            d.expires=tp.expires_at;
            d.color=0xde000000u|route_colors[i];
        }
        for(int endpoint=0;endpoint<2;++endpoint) {
            if(endpoint==0?(!tp.has_origin||tp.channel_finished):!tp.has_destination)continue;
            render_trace_entry marker{};marker.position=endpoint==0?tp.origin:tp.destination;
            marker.created_at=tp.cast_started_at;marker.expires_at=tp.expires_at;
            copy_designer_name(marker.designer_name,tp.designer_name);
            copy_designer_name(marker.ability_icon,
                (tp.effect_flags&render_teleportation::particle_flag_twin_gate)?"twin_gate_portal_warp":
                (tp.effect_flags&render_teleportation::particle_flag_keen_conveyance)?"tinker_keen_teleport":"item_tpscroll");
            const auto elapsed=now>tp.cast_started_at?now-tp.cast_started_at:0;
            const float remaining=elapsed<tp.duration_ms?float(tp.duration_ms-elapsed)/1000.f:0.f;
            char label[64]{};
            std::snprintf(label,sizeof(label),tp.channel_finished||remaining<=0?"TP":"TP %.1fs",remaining);
            add(marker,label);
        }
    }
    const camp_world_tile::options camp_info{g_render_camp_count.load(),g_render_camp_gold.load(),g_render_camp_background.load()};
    if(camp_info.count||camp_info.gold)
        for(std::size_t i=0;i<camp_count&&scene.count<world_frame::capacity;++i){
            auto camp=camp_statuses[i];
            if(!camp.known&&g_render_camp_estimates.load()){
                const auto estimate=camp_estimates::get(camp_types[i],camp_time);
                if(estimate.count_min){camp.known=true;camp.count=estimate.count_min;camp.count_max=estimate.count_max;
                    camp.gold_min=estimate.gold_min;camp.gold_max=estimate.gold_max;camp.gold_known=true;}
            }
            if(!camp.known||camp.empty_confirmed||camp.possibly_cleared)continue;
            const unsigned index=scene.count;
            if(!rasterize_camp_world_tile(index,camp,camp_info))continue;
            auto& marker=scene.markers[scene.count++];
            marker.position[0]=camp_positions[i].x;marker.position[1]=camp_positions[i].y;marker.position[2]=camp_positions[i].z+96;
            marker.expires=now+750;marker.version=g_world_tile_cache[index].version;
        }
    world_frame::publish(scene,g_world_tiles);
    const auto stats=world_frame::status();
    g_render_diagnostics.visible_world_markers.store(static_cast<int>(stats.visible),std::memory_order_relaxed);
    g_render_diagnostics.world_marker_frames.store(stats.drawn_frames,std::memory_order_relaxed);
    g_render_diagnostics.view_matrix_ready.store(scene.matrix_address!=0,std::memory_order_relaxed);
}
