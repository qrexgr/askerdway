/**
 * @file
 * @brief Меню и диагностические представления.
 *
 * Рисует страницы ImGui, преобразует внутренние статусы в подписи и принимает настройки
 * пользователя. clear_runtime_state сбрасывает недействительный снимок героя; меню не вызывает
 * PrepareUnitOrders напрямую.
 */

// Overlay menu.

#include "../ui/portfolio_theme.h"
#include "../ui/sidebar_icons.h"
#include "../ui/menu_preferences.h"

enum class overlay_page : std::uint8_t {
    armlet,
    render,
    comfort,
    diagnostics,
};

/**
 * @brief Единая точка включения автоматики из UI/горячей клавиши; обновляет связанное
 * состояние и диагностическое сообщение.
 */
void set_abuse_enabled(bool enabled, const char* source) {
    const bool previous = g_monitor.abuse_enabled.exchange(
        enabled, std::memory_order_relaxed);
    if (previous == enabled)
        return;
    if (!enabled)
        cancel_queued_armlet_order();
    g_enable_notification.enabled.store(enabled, std::memory_order_relaxed);
    g_enable_notification.shown_at.store(GetTickCount64(), std::memory_order_relaxed);
    g_enable_notification.serial.fetch_add(1, std::memory_order_release);
    char message[96]{};
    std::snprintf(message, sizeof(message), "Armlet Abuse %s (%s).",
        enabled ? "ENABLED" : "DISABLED", source);
    log_message("BIND", message);
}

const char* hotkey_name(int virtual_key, char (&buffer)[48]) {
    if (virtual_key == 0)
        return "NONE";
    switch (virtual_key) {
    case VK_LBUTTON: return "MOUSE 1";
    case VK_RBUTTON: return "MOUSE 2";
    case VK_MBUTTON: return "MOUSE 3";
    case VK_XBUTTON1: return "MOUSE 4";
    case VK_XBUTTON2: return "MOUSE 5";
    case VK_BACK: return "BACKSPACE";
    case VK_TAB: return "TAB";
    case VK_RETURN: return "ENTER";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "SHIFT";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "CTRL";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return "ALT";
    case VK_PAUSE: return "PAUSE";
    case VK_CAPITAL: return "CAPS LOCK";
    case VK_ESCAPE: return "ESC";
    case VK_SPACE: return "SPACE";
    case VK_PRIOR: return "PAGE UP";
    case VK_NEXT: return "PAGE DOWN";
    case VK_END: return "END";
    case VK_HOME: return "HOME";
    case VK_LEFT: return "LEFT";
    case VK_UP: return "UP";
    case VK_RIGHT: return "RIGHT";
    case VK_DOWN: return "DOWN";
    case VK_SNAPSHOT: return "PRINT SCREEN";
    case VK_INSERT: return "INSERT";
    case VK_DELETE: return "DELETE";
    case VK_LWIN: case VK_RWIN: return "WIN";
    case VK_APPS: return "MENU";
    case VK_MULTIPLY: return "NUM *";
    case VK_ADD: return "NUM +";
    case VK_SUBTRACT: return "NUM -";
    case VK_DECIMAL: return "NUM .";
    case VK_DIVIDE: return "NUM /";
    case VK_NUMLOCK: return "NUM LOCK";
    case VK_SCROLL: return "SCROLL LOCK";
    case VK_OEM_1: return ";";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_3: return "`";
    case VK_OEM_4: return "[";
    case VK_OEM_5: case VK_OEM_102: return "\\";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "'";
    default: break;
    }
    // Windows key names follow the OS language. Keep UI labels deterministic
    // even when Windows or the active keyboard layout is Russian.
    if ((virtual_key >= 'A' && virtual_key <= 'Z') ||
        (virtual_key >= '0' && virtual_key <= '9'))
        std::snprintf(buffer, sizeof(buffer), "%c", virtual_key);
    else if (virtual_key >= VK_F1 && virtual_key <= VK_F24)
        std::snprintf(buffer, sizeof(buffer), "F%d", virtual_key - VK_F1 + 1);
    else if (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9)
        std::snprintf(buffer, sizeof(buffer), "NUM %d", virtual_key - VK_NUMPAD0);
    else
        std::snprintf(buffer, sizeof(buffer), "VK_%02X", virtual_key);
    return buffer;
}

const char* status_text(armlet_status status) {
    switch (status) {
    case armlet_status::waiting_for_client: return "Waiting for client.dll";
    case armlet_status::waiting_for_hero: return "No local hero";
    case armlet_status::item_missing: return "Armlet not found";
    case armlet_status::off: return "Armlet is OFF";
    case armlet_status::active_confirmed: return "Armlet active";
    case armlet_status::applying: return "Enabling";
    case armlet_status::lingering: return "Disabling";
    case armlet_status::read_error: return "Memory read error";
    }
    return "Unknown";
}

const char* cycle_text(cycle_state state) {
    switch (state) {
    case cycle_state::idle: return "IDLE";
    case cycle_state::on: return "ON";
    case cycle_state::disabling: return "DISABLING";
    case cycle_state::off: return "OFF";
    case cycle_state::enabling: return "ENABLING";
    }
    return "UNKNOWN";
}

const char* safety_text(safety_state state) {
    switch (state) {
    case safety_state::unavailable: return "No Armlet or hero";
    case safety_state::abuse_disabled: return "Disabled";
    case safety_state::health_above_threshold: return "HP above threshold";
    case safety_state::safe: return "Safe";
    case safety_state::telemetry_stale: return "Telemetry stale";
    case safety_state::state_unconfirmed: return "State not confirmed";
    case safety_state::stunned_or_muted: return "Stunned or muted";
    case safety_state::cooldown_wait: return "Cooldown";
    case safety_state::command_unavailable: return "Command unavailable";
    case safety_state::incomplete_event: return "Incomplete damage event";
    case safety_state::unknown_modifier: return "Unknown modifier";
    case safety_state::threat_pending: return "Incoming damage";
    case safety_state::lethal_cluster: return "Unsafe damage";
    case safety_state::high_risk_blocked: return "High-risk timing";
    case safety_state::high_risk_allowed: return "High risk allowed";
    case safety_state::game_paused: return "Game paused";
    case safety_state::safe_with_instant_heal: return "Safe with heal";
    }
    return "unknown";
}

ImVec4 status_color(armlet_status status) {
    switch (status) {
    case armlet_status::active_confirmed: return portfolio_theme::success;
    case armlet_status::applying:
    case armlet_status::lingering: return portfolio_theme::warning;
    case armlet_status::read_error: return portfolio_theme::danger;
    default: return portfolio_theme::text_muted;
    }
}

ImVec4 health_color(int health, int max_health) {
    const float fraction = max_health > 0
        ? static_cast<float>(health) / static_cast<float>(max_health)
        : 0.0f;
    if (fraction > 0.60f)
        return portfolio_theme::success;
    if (fraction > 0.25f)
        return portfolio_theme::warning;
    return portfolio_theme::danger;
}

void draw_section_heading(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_Text, portfolio_theme::text_muted);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void draw_value_row(const char* label, const char* value, ImVec4 color = portfolio_theme::text) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    const float value_width = ImGui::CalcTextSize(value).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
        ImGui::GetWindowWidth() - value_width - 14.0f));
    ImGui::TextColored(color, "%s", value);
}

// State belongs to the current ImGui window/ID; no per-frame allocations or
// worker-thread animation state. First appearance starts at the real value.
float menu_animate(const char* id, float target, float speed = 20.0f) {
    auto* state = ImGui::GetStateStorage();
    const auto key = ImGui::GetID(id);
    const float previous = state->GetFloat(key, target);
    const float step = 1.0f - std::exp(-speed * std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.05f));
    const float value = previous + (target - previous) * step;
    state->SetFloat(key, value);
    return value;
}

ImVec4 menu_mix(ImVec4 from, ImVec4 to, float t) {
    return ImVec4(from.x+(to.x-from.x)*t, from.y+(to.y-from.y)*t,
        from.z+(to.z-from.z)*t, from.w+(to.w-from.w)*t);
}

bool begin_settings_popup(const char* id,float preferred_width=360.f) {
    const float width=std::min(preferred_width,std::max(160.f,ImGui::GetIO().DisplaySize.x-32));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width,0),ImVec2(width,ImGui::GetIO().DisplaySize.y-32));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(16,14));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8,8));
    if(ImGui::BeginPopup(id))return true;
    ImGui::PopStyleVar(2);return false;
}
void end_settings_popup(){ImGui::EndPopup();ImGui::PopStyleVar(2);}

bool portfolio_toggle(const char* label, bool* value, const char* popup = nullptr) {
    ImGui::PushID(label);
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = float(menu_preferences::current.row_height);
    const auto minimum = ImGui::GetCursorScreenPos();
    const ImVec2 maximum(minimum.x+width,minimum.y+height);
    const float label_width=std::max(1.f,width-(popup?78.f:46.f));
    bool changed=ImGui::InvisibleButton("##label",ImVec2(label_width,height));
    bool settings=false;
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddText(ImVec2(minimum.x+2,minimum.y+(height-ImGui::GetFontSize())*.5f),
        ImGui::GetColorU32(portfolio_theme::text),label);
    if(popup) {
        ImGui::SameLine(0,4);
        settings=ImGui::InvisibleButton("##settings",ImVec2(24,height));
        const ImVec2 center(minimum.x+width-60,minimum.y+height*.5f);
        const auto color=ImGui::GetColorU32(ImGui::IsItemHovered()?portfolio_theme::text:portfolio_theme::text_muted);
        ImVec2 outline[32]{};
        for(int i=0;i<32;++i){
            const float angle=(i-1)*3.14159265f/16.f;
            const float radius=(i%4==1||i%4==2)?6.7f:4.8f;
            outline[i]=ImVec2(center.x+radius*std::cos(angle),center.y+radius*std::sin(angle));
        }
        draw->AddConcavePolyFilled(outline,32,color);
        draw->AddCircleFilled(center,2.4f,ImGui::GetColorU32(portfolio_theme::panel),16);
    }
    ImGui::SameLine(0,4);
    changed|=ImGui::InvisibleButton("##switch",ImVec2(42,height));
    if(changed)*value=!*value;
    const float on=menu_animate("##on",*value?1.f:0.f);
    const ImVec2 track(maximum.x-42,minimum.y+(height-20)*.5f);
    draw->AddRectFilled(track,ImVec2(track.x+40,track.y+20),ImGui::GetColorU32(menu_mix(
        portfolio_theme::control,ImVec4(portfolio_theme::accent.x,portfolio_theme::accent.y,portfolio_theme::accent.z,.72f),on)),10);
    draw->AddCircleFilled(ImVec2(track.x+10+20*on,track.y+10),7,
        ImGui::GetColorU32(menu_mix(portfolio_theme::text_muted,portfolio_theme::text,on)),24);
    draw->AddLine(ImVec2(minimum.x,maximum.y),maximum,ImGui::GetColorU32(portfolio_theme::border));
    ImGui::PopID();
    if(settings)ImGui::OpenPopup(popup);
    return changed;
}

bool sidebar_button(const char* id, sidebar_icons::icon icon, bool selected) {
    ImGui::PushID(id);
    ImGui::SetCursorPosX((portfolio_theme::sidebar_width-48)*.5f);
    const bool pressed=ImGui::InvisibleButton("##nav",ImVec2(48,48));
    const float active=menu_animate("##active",selected?1.f:0.f,18);
    const float hover=menu_animate("##hover",ImGui::IsItemHovered()?1.f:0.f);
    const auto minimum=ImGui::GetItemRectMin(),maximum=ImGui::GetItemRectMax();
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(minimum,maximum,ImGui::GetColorU32(ImVec4(
        portfolio_theme::accent.x,portfolio_theme::accent.y,portfolio_theme::accent.z,.12f*active+.04f*hover)),9);
    if(active>.005f)draw->AddRectFilled(ImVec2(minimum.x-9,minimum.y+12),ImVec2(minimum.x-6,maximum.y-12),
        ImGui::GetColorU32(ImVec4(portfolio_theme::accent.x,portfolio_theme::accent.y,portfolio_theme::accent.z,active)),2);
    sidebar_icons::draw(draw,icon,ImVec2(minimum.x+10,minimum.y+10),ImVec2(maximum.x-10,maximum.y-10),
        ImGui::GetColorU32(menu_mix(portfolio_theme::text_muted,portfolio_theme::accent,std::max(active,hover))));
    ImGui::PopID();
    return pressed;
}

void draw_brand_logo() {
    // Brand only: not a navigation item, never selected or clickable.
    ImGui::Dummy(ImVec2(0,18));
    const auto p=ImGui::GetCursorScreenPos();
    const ImVec2 center(p.x+portfolio_theme::sidebar_width*.5f,p.y+18);
    auto* draw=ImGui::GetWindowDrawList();
    const ImVec2 diamond[]{ImVec2(center.x,center.y-16),ImVec2(center.x+15,center.y),
        ImVec2(center.x,center.y+16),ImVec2(center.x-15,center.y)};
    draw->AddPolyline(diamond,4,ImGui::GetColorU32(portfolio_theme::accent),ImDrawFlags_Closed,1.8f);
    const ImVec2 core[]{ImVec2(center.x,center.y-6),ImVec2(center.x+5,center.y),
        ImVec2(center.x,center.y+6),ImVec2(center.x-5,center.y)};
    draw->AddConvexPolyFilled(core,4,ImGui::GetColorU32(portfolio_theme::text));
    ImGui::Dummy(ImVec2(portfolio_theme::sidebar_width,56));
    ImGui::SetCursorPosX(18);
    const auto line=ImGui::GetCursorScreenPos();
    draw->AddLine(line,ImVec2(line.x+portfolio_theme::sidebar_width-36,line.y),ImGui::GetColorU32(portfolio_theme::border));
    ImGui::Dummy(ImVec2(0,18));
}

bool heal_choice(const char* label, bool selected) {
    ImGui::PushID(label);
    const bool pressed=ImGui::InvisibleButton("##choice",ImVec2(ImGui::GetContentRegionAvail().x,42));
    const float on=menu_animate("##on",selected?1.f:0.f);
    const float hover=menu_animate("##hover",ImGui::IsItemHovered()?1.f:0.f);
    const auto p=ImGui::GetItemRectMin(),q=ImGui::GetItemRectMax();
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(p,q,ImGui::GetColorU32(menu_mix(portfolio_theme::panel,portfolio_theme::control_hover,hover*.5f)),6);
    draw->AddRect(p,q,ImGui::GetColorU32(menu_mix(portfolio_theme::border,
        ImVec4(portfolio_theme::accent.x,portfolio_theme::accent.y,portfolio_theme::accent.z,.5f),on)),6);
    const ImVec2 box(p.x+12,p.y+11);
    draw->AddRectFilled(box,ImVec2(box.x+20,box.y+20),ImGui::GetColorU32(menu_mix(portfolio_theme::control,
        ImVec4(portfolio_theme::accent.x,portfolio_theme::accent.y,portfolio_theme::accent.z,.55f),on)),4);
    if(on>.005f) {
        const auto color=ImGui::GetColorU32(ImVec4(portfolio_theme::text.x,portfolio_theme::text.y,portfolio_theme::text.z,on));
        draw->AddLine(ImVec2(box.x+5,box.y+10),ImVec2(box.x+9,box.y+14),color,2);
        draw->AddLine(ImVec2(box.x+9,box.y+14),ImVec2(box.x+16,box.y+6),color,2);
    }
    draw->AddText(ImVec2(p.x+42,p.y+(42-ImGui::GetFontSize())*.5f),ImGui::GetColorU32(portfolio_theme::text),label);
    ImGui::PopID();return pressed;
}

void begin_card(const char* id, bool automatic_height = false) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, portfolio_theme::panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
    ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding | (automatic_height ?
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize : ImGuiChildFlags_None));
}

void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void draw_keybind_control() {
    bool capturing = g_bind_capture_active.load(std::memory_order_acquire) && g_bind_capture_target.load() == 0;
    static std::array<bool, 256> key_was_down{};
    char key_buffer[48]{};
    const int configured_hotkey = g_monitor.abuse_hotkey.load(std::memory_order_relaxed);

    ImGui::TextDisabled("Activation key");
    ImGui::SameLine();
    const char* label = capturing ? "PRESS A KEY..." : hotkey_name(configured_hotkey, key_buffer);
    const float button_width = 160.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
        ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - button_width));
    if (ImGui::Button(label, ImVec2(button_width, 34.0f))) {
        capturing = true;
        g_bind_capture_target.store(0);
        g_bind_capture_key.store(0,std::memory_order_relaxed);
        g_bind_capture_active.store(true,std::memory_order_release);
        for (int key = 0; key < 256; ++key)
            key_was_down[static_cast<std::size_t>(key)] =
                (GetAsyncKeyState(key) & 0x8000) != 0;
    }

    if (!capturing)
        return;
    const int event_key=g_bind_capture_key.exchange(0,std::memory_order_acq_rel);
    for (int key = 1; key < 256; ++key) {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool pressed = key==event_key||(down && !key_was_down[static_cast<std::size_t>(key)]);
        key_was_down[static_cast<std::size_t>(key)] = down;
        if (!pressed)
            continue;
        const int next_key = key == VK_ESCAPE ? 0 : key;
        if (next_key && next_key == comfort_runtime::silence_key.load()) continue;
        g_monitor.abuse_hotkey.store(next_key, std::memory_order_relaxed);
        capturing = false;
        g_bind_capture_active.store(false,std::memory_order_release);
        log_message("BIND", next_key == 0
            ? "Armlet Abuse hotkey cleared."
            : "Armlet Abuse hotkey changed from ImGui.");
        break;
    }
}

/**
 * @brief Рисует соответствующую страницу текущего ImGui-контекста. Настройки передаются
 * setter-функциям, а диагностические поля читаются из опубликованного состояния.
 */
void draw_comfort_keybind(askerdway_comfort_config_v1& config, bool& changed) {
    static std::array<bool, 256> down{};
    bool capturing = g_bind_capture_active.load() && g_bind_capture_target.load() == 1;
    char key_buffer[48]{};
    ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Silence key");
    ImGui::SameLine();ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),ImGui::GetWindowWidth()-148));
    const char* label = capturing ? "PRESS A KEY..." : hotkey_name(config.silence_hotkey, key_buffer);
    if (ImGui::Button(label, ImVec2(132, 26))) {
        g_bind_capture_target.store(1); g_bind_capture_key.store(0);
        g_bind_capture_active.store(true); capturing = true;
        for (int key = 0; key < 256; ++key) down[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    }
    if (!capturing) return;
    const int event_key = g_bind_capture_key.exchange(0);
    for (int key = VK_BACK; key < 256; ++key) {
        const bool pressed = (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool edge = (pressed && !down[key]) || key == event_key;
        down[key] = pressed;
        if (!edge) continue;
        if (key == VK_INSERT || key == VK_END || key == VK_SHIFT || key == VK_CONTROL ||
            key == VK_MENU || key == VK_LWIN || key == VK_RWIN ||
            (key >= VK_LSHIFT && key <= VK_RMENU) || key == g_monitor.abuse_hotkey.load()) continue;
        config.silence_hotkey = key == VK_ESCAPE ? 0 : key;
        changed = true; g_bind_capture_active.store(false); break;
    }
}

void draw_comfort_page() {
    askerdway_comfort_config_v1 config{};
    askerdway_comfort_status_v1 status{};
    AskerdwayApi_GetComfortConfig(&config);AskerdwayApi_GetComfortStatus(&status);
    bool changed=false;
    begin_card("##utility",true);
    const auto toggle=[&](const char* label,unsigned flag,const char* popup=nullptr){
        bool value=(config.flags&flag)!=0;
        if(portfolio_toggle(label,&value,popup)){
            config.flags=value?config.flags|flag:config.flags&~flag;changed=true;
            if(flag==ASKERDWAY_COMFORT_ENABLED&&!value)AskerdwayApi_SetComfortSilence(0);
        }
    };
    draw_section_heading("General");
    toggle("Enabled",ASKERDWAY_COMFORT_ENABLED,"##utility_profile");
    if(begin_settings_popup("##utility_profile")){
        if(ImGui::Button("Default")){const int key=config.silence_hotkey;
            config={};config.silence_hotkey=key;config.flags|=ASKERDWAY_COMFORT_ENABLED;changed=true;}
        ImGui::SameLine();
        if(ImGui::Button("Only essentials")){const int key=config.silence_hotkey;
            config={};config.silence_hotkey=key;config.flags|=ASKERDWAY_COMFORT_ENABLED|ASKERDWAY_COMFORT_ALLY_TEXT;changed=true;}
        end_settings_popup();
    }
    bool silence=status.silence_requested!=0;
    if(portfolio_toggle("Full silence",&silence,"##silence_settings"))AskerdwayApi_SetComfortSilence(silence);
    if(begin_settings_popup("##silence_settings")){draw_comfort_keybind(config,changed);end_settings_popup();}
    ImGui::Spacing();draw_section_heading("Signals");
    bool suggestions=config.item_mode!=ASKERDWAY_COMFORT_ITEMS_OFF;
    if(portfolio_toggle("Hide purchase suggestions",&suggestions,"##suggestion_settings")){
        config.item_mode=suggestions?ASKERDWAY_COMFORT_ITEMS_SELECTED_SUGGESTIONS:ASKERDWAY_COMFORT_ITEMS_OFF;changed=true;
    }
    if(begin_settings_popup("##suggestion_settings")){
        const auto item=[&](const char* label,unsigned id){
            bool selected=false;
            for(unsigned i=0;i<config.suggestion_item_count;++i)selected|=config.suggestion_items[i]==id;
            if(ImGui::Checkbox(label,&selected)){
                if(selected&&config.suggestion_item_count<16)config.suggestion_items[config.suggestion_item_count++]=id;
                else if(!selected){unsigned count=0;
                    for(unsigned i=0;i<config.suggestion_item_count;++i)if(config.suggestion_items[i]!=id)config.suggestion_items[count++]=config.suggestion_items[i];
                    config.suggestion_item_count=count;
                    for(unsigned i=count;i<16;++i)config.suggestion_items[i]=0;
                }changed=true;
            }
        };
        item("Mantle of Intelligence",15);item("Urn of Shadows",92);item("Bloodstone",121);
        end_settings_popup();
    }
    toggle("Hide respawn pings",ASKERDWAY_COMFORT_RESPAWN);
    toggle("Hide tips",ASKERDWAY_COMFORT_TIPS);
    toggle("Anti-spam",ASKERDWAY_COMFORT_ANTISPAM);
    ImGui::Spacing();draw_section_heading("Chat");
    toggle("Hide allied text",ASKERDWAY_COMFORT_ALLY_TEXT);
    toggle("Hide enemy text",ASKERDWAY_COMFORT_ENEMY_TEXT);
    toggle("Hide wheel text",ASKERDWAY_COMFORT_WHEEL_TEXT);
    toggle("Mute wheel sounds",ASKERDWAY_COMFORT_WHEEL_SOUND);
    if(changed)AskerdwayApi_SetComfortConfig(&config);
    end_card();
}

void draw_armlet_page() {
    begin_card("##armlet_controls");
    bool enabled = g_monitor.abuse_enabled.load(std::memory_order_relaxed);
    if (portfolio_toggle("Enabled", &enabled,"##armlet_settings"))
        set_abuse_enabled(enabled, "ImGui");
    if(begin_settings_popup("##armlet_settings")){draw_keybind_control();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    draw_section_heading("Safety");
    float margin = g_monitor.safety_margin.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##safety_margin", &margin, 0.0f, 500.0f,
            "Safety margin: %.0f HP"))
        g_monitor.safety_margin.store(margin, std::memory_order_relaxed);

    bool high_risk = g_monitor.allow_high_risk_abuse.load(std::memory_order_relaxed);
    if (portfolio_toggle("Dangerous timing", &high_risk))
        g_monitor.allow_high_risk_abuse.store(high_risk, std::memory_order_relaxed);
    end_settings_popup();
    }

    bool use_heals = g_monitor.use_instant_heals.load(std::memory_order_relaxed);
    if (portfolio_toggle("Use instant heal", &use_heals,"##heal_settings"))
        g_monitor.use_instant_heals.store(use_heals, std::memory_order_relaxed);

    if(begin_settings_popup("##heal_settings")) {
    ImGui::BeginDisabled(!use_heals);
    {
        std::uint32_t mask = g_monitor.instant_heal_mask.load(std::memory_order_relaxed);
        const auto heal_checkbox = [&](const char* label, std::uint32_t group) {
            bool selected = (mask & group) != 0;
            if (heal_choice(label, selected)) {
                selected = !selected;
                if (selected) mask |= group;
                else mask &= ~group;
            }
        };
        if (ImGui::BeginTable("##heal_options", ImGui::GetContentRegionAvail().x < 560 ? 2 : 3, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn(); heal_checkbox("Stick / Wand", heal_group_stick_wand);
            ImGui::TableNextColumn(); heal_checkbox("Faerie Fire", heal_group_faerie_fire);
            ImGui::TableNextColumn(); heal_checkbox("Lotus", heal_group_lotus);
            ImGui::TableNextColumn(); heal_checkbox("Holy Locket", heal_group_holy_locket);
            ImGui::TableNextColumn(); heal_checkbox("Mek / Greaves", heal_group_team_items);
            ImGui::TableNextColumn(); heal_checkbox("Cheese", heal_group_cheese);
            ImGui::EndTable();
        }
        g_monitor.instant_heal_mask.store(mask, std::memory_order_relaxed);
    }
    ImGui::EndDisabled();
    end_settings_popup();
    }
    end_card();
}

/**
 * @brief Рисует соответствующую страницу текущего ImGui-контекста. Настройки передаются
 * setter-функциям, а диагностические поля читаются из опубликованного состояния.
 */
void draw_jungle_status() {
    std::array<jungle_status::entry,64> camps{};unsigned count=0;
    {
        render_intelligence_shared_guard guard;
        camps=g_render_intelligence.jungle.entries;
        count=unsigned(g_render_intelligence.camp_count);
    }
    const auto now=GetTickCount64();
    if(ImGui::BeginTable("##camp_status",6,ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,ImVec2(0,300))){
        for(const char* heading:{"Camp","State","Units","HP ~","Gold ~","Enemy"})ImGui::TableSetupColumn(heading);
        ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
        for(unsigned i=0;i<count&&i<camps.size();++i){const auto& camp=camps[i];
            ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%u",i+1);
            ImGui::TableNextColumn();ImGui::TextUnformatted(!camp.known?"Unknown":camp.empty_confirmed?"Empty":camp.possibly_cleared?"Cleared?":camp.current?"Seen":"Last seen");
            ImGui::TableNextColumn();if(camp.known)ImGui::Text("%d",camp.count);else ImGui::TextUnformatted("?");
            ImGui::TableNextColumn();if(camp.known)ImGui::Text("%d",camp.health);else ImGui::TextUnformatted("?");
            ImGui::TableNextColumn();if(camp.gold_known)ImGui::Text("%d-%d",camp.gold_min,camp.gold_max);else ImGui::TextUnformatted("?");
            ImGui::TableNextColumn();
            if(camp.farm_at){const char* name=std::strncmp(camp.hero,"npc_dota_hero_",14)==0?camp.hero+14:camp.hero;
                ImGui::Text("%s / %.0fs",name,now>=camp.farm_at?float(now-camp.farm_at)/1000.f:0.f);}
            else ImGui::TextUnformatted("-");
            if(camp.known&&ImGui::IsItemHovered())ImGui::SetTooltip("Last change: %.0f s",float(now-camp.changed_at)/1000.f);
        }
        ImGui::EndTable();
    }
}

void draw_render_page() {
    if (!ImGui::BeginTable("##render_columns", 2, ImGuiTableFlags_SizingStretchSame))
        return;
    ImGui::TableNextColumn();
    begin_card("##visual_controls",true);
    draw_section_heading("Visuals");
    bool particle_fow = render_teleportation::is_enabled();
    const bool hook_ready = render_teleportation::is_hook_installed();
    ImGui::BeginDisabled(!hook_ready);
    if (portfolio_toggle("Particles through fog", &particle_fow))
        set_particle_visibility_enabled(particle_fow);
    ImGui::EndDisabled();

    bool arrows = g_render_teleport_arrows.load(std::memory_order_relaxed);
    if (portfolio_toggle("Threat direction", &arrows))
        set_teleport_arrows_enabled(arrows);

    bool creeps = g_render_creep_tracker.load(std::memory_order_relaxed);
    if (portfolio_toggle("Creep tracker", &creeps))
        set_creep_tracker_enabled(creeps);

    bool trails = g_render_trace_icons.load(std::memory_order_relaxed);
    if (portfolio_toggle("Last enemy position", &trails,"##trail_settings"))
        set_hero_trails_enabled(trails);

    if(begin_settings_popup("##trail_settings")){
    int lifetime = g_render_trace_lifetime_seconds.load(std::memory_order_relaxed);
    ImGui::BeginDisabled(!trails);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##trace_lifetime", &lifetime, 3, 30,
            "Duration: %d s"))
        set_trace_lifetime_seconds(lifetime);
    ImGui::EndDisabled();

    int icon_scale = g_render_hero_icon_scale_percent.load(
        std::memory_order_relaxed);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##hero_icon_scale", &icon_scale, 0, 100,
            "Icon scale: %d%%"))
        set_hero_icon_scale_percent(icon_scale);
    end_settings_popup();
    }
    end_card();

    ImGui::TableNextColumn();
    begin_card("##minimap_controls",true);
    draw_section_heading("Minimap");
    bool camps = g_render_camp_status.load(std::memory_order_relaxed);
    if (portfolio_toggle("Jungle camps", &camps,"##camp_settings"))
        set_camp_status_enabled(camps);
    if(begin_settings_popup("##camp_settings")){
        bool changed=false;
        bool count=g_render_camp_count.load(),gold=g_render_camp_gold.load();
        if(ImGui::Checkbox("Creep count",&count)){g_render_camp_count.store(count);changed=true;}
        if(ImGui::Checkbox("Gold bounty",&gold)){g_render_camp_gold.store(gold);changed=true;}
        bool estimates=g_render_camp_estimates.load(),background=g_render_camp_background.load();
        if(ImGui::Checkbox("Unseen camp estimates",&estimates)){g_render_camp_estimates.store(estimates);changed=true;}
        if(ImGui::Checkbox("Background",&background)){g_render_camp_background.store(background);changed=true;}
        if(changed){auto& prefs=menu_preferences::current;prefs.camp_count=count;prefs.camp_gold=gold;
            prefs.camp_estimates=estimates;prefs.camp_background=background;prefs.save();}
        ImGui::Separator();
        bool boxes=g_render_camp_boxes.load(),alerts=g_render_camp_alerts.load();
        if(ImGui::Checkbox("Spawn boxes on ALT",&boxes))g_render_camp_boxes.store(boxes);
        if(ImGui::Checkbox("Block alerts",&alerts))g_render_camp_alerts.store(alerts);
        end_settings_popup();
    }

    const int detected_pixels=g_minimap_hud_pixels.load();
    if(detected_pixels)ImGui::TextDisabled("HUD: %d px / %d%%",detected_pixels,g_minimap_hud_scale.load());
    bool automatic=g_minimap_auto.load();
    if(portfolio_toggle("Auto layout",&automatic,"##map_settings"))g_minimap_auto.store(automatic);
    if(begin_settings_popup("##map_settings")){
    bool minimap_right = g_render_minimap_right.load(std::memory_order_relaxed);
    ImGui::BeginDisabled(automatic);
    if (portfolio_toggle("Minimap on right", &minimap_right))
        set_minimap_side_right(minimap_right);
    ImGui::EndDisabled();

    int minimap_scale = g_render_minimap_size_percent.load(
        std::memory_order_relaxed);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##native_minimap_scale", &minimap_scale, 60, 150,
            "Scale correction: %d%%"))
        g_render_minimap_size_percent.store(minimap_scale,
            std::memory_order_relaxed);

    int offset_x = g_render_minimap_offset_x.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##native_minimap_x", &offset_x, -120, 120,
            "Map offset X: %d px"))
        g_render_minimap_offset_x.store(offset_x, std::memory_order_relaxed);

    int offset_y = g_render_minimap_offset_y.load(std::memory_order_relaxed);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##native_minimap_y", &offset_y, -120, 120,
            "Map offset Y: %d px"))
        g_render_minimap_offset_y.store(offset_y, std::memory_order_relaxed);
    if(ImGui::Button("Reset correction")){g_render_minimap_size_percent.store(100);g_render_minimap_offset_x.store(0);g_render_minimap_offset_y.store(0);}
    end_settings_popup();
    }
    end_card();

    ImGui::EndTable();
}

/**
 * @brief Рисует соответствующую страницу текущего ImGui-контекста. Настройки передаются
 * setter-функциям, а диагностические поля читаются из опубликованного состояния.
 */
const char* menu_account_name() {
    static char name[128]="Player";
    static ULONGLONG next=0;
    const auto now=GetTickCount64();
    if(now<next)return name;
    next=now+5000;
    bool found=false;
    if(const auto steam=GetModuleHandleW(L"steam_api64.dll")){
        using running_fn=bool(*)();using friends_fn=void*(*)();using name_fn=const char*(*)(void*);
        const auto running=reinterpret_cast<running_fn>(GetProcAddress(steam,"SteamAPI_IsSteamRunning"));
        const auto friends=reinterpret_cast<friends_fn>(GetProcAddress(steam,"SteamAPI_SteamFriends_v018"));
        const auto persona=reinterpret_cast<name_fn>(GetProcAddress(steam,"SteamAPI_ISteamFriends_GetPersonaName"));
        if(running&&friends&&persona&&running())if(void* api=friends())if(const char* text=persona(api)){
            if(text[0]){std::snprintf(name,sizeof(name),"%s",text);found=true;}
        }
    }
    if(!found){
        std::lock_guard guard(comfort_runtime::lock);
        const int local=comfort_runtime::rules.current_context().local_player;
        if(local>=0&&local<64&&comfort_runtime::players[local].name[0])
            std::snprintf(name,sizeof(name),"%s",comfort_runtime::players[local].name);
        else std::snprintf(name,sizeof(name),"Player");
    }
    return name;
}

void draw_interface_settings() {
    auto& prefs=menu_preferences::current;
    static bool dirty=false;
    draw_section_heading("Interface");
    dirty|=ImGui::Checkbox("Nickname",&prefs.nickname);ImGui::SameLine();
    dirty|=ImGui::Checkbox("Date",&prefs.date);ImGui::SameLine();
    dirty|=ImGui::Checkbox("Time",&prefs.time);
    ImGui::SetNextItemWidth(-1);
    dirty|=ImGui::InputTextWithHint("##alias","Nickname override",prefs.alias,sizeof(prefs.alias));
    dirty|=ImGui::Checkbox("Seconds",&prefs.seconds);ImGui::SameLine();
    dirty|=ImGui::Checkbox("ISO date",&prefs.iso_date);
    ImGui::SetNextItemWidth(-1);
    dirty|=ImGui::SliderInt("##scroll_ms",&prefs.scroll_ms,0,250,"Scroll animation: %d ms");
    ImGui::SetNextItemWidth(-1);
    dirty|=ImGui::SliderInt("##row_height",&prefs.row_height,28,44,"Row height: %d px");
    if(dirty&&!ImGui::IsMouseDown(0)){prefs.save();dirty=false;}
}

void smooth_menu_scroll() {
    auto* storage=ImGui::GetStateStorage();
    const auto key=ImGui::GetID("##scroll_frame");
    static std::array<menu_preferences::scroll_motion,4> motions{};
    const auto index=std::clamp(storage->GetInt(ImGui::GetID("##page_index"),0),0,3);
    auto& motion=motions[index];
    if(storage->GetInt(key,-2)!=ImGui::GetFrameCount()-1)motion.initialized=false;
    storage->SetInt(key,ImGui::GetFrameCount());
    const auto& io=ImGui::GetIO();
    const float wheel=ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)?io.MouseWheel:0.f;
    const bool direct=io.MouseDown[0]||ImGui::IsKeyDown(ImGuiKey_PageDown)||ImGui::IsKeyDown(ImGuiKey_PageUp)||ImGui::IsKeyDown(ImGuiKey_Home)||ImGui::IsKeyDown(ImGuiKey_End);
    const float next=motion.step(ImGui::GetScrollY(),ImGui::GetScrollMaxY(),wheel,
        ImGui::GetFontSize()*5,io.DeltaTime,menu_preferences::current.scroll_ms,direct);
    if(!direct)ImGui::SetScrollY(next);
}

void draw_diagnostics_page() {
    begin_card("##diagnostics",true);
    draw_interface_settings();
    ImGui::Spacing();draw_section_heading("Runtime");
    const bool automation_ready = g_monitor.automation_ready.load(std::memory_order_relaxed);
    draw_value_row("Automation", automation_ready ? "READY" : "WAITING",
        automation_ready ? portfolio_theme::success : portfolio_theme::warning);
    draw_value_row("Armlet",status_text(g_monitor.status.load(std::memory_order_relaxed)));
    draw_value_row("Decision",safety_text(g_monitor.safety.load(std::memory_order_relaxed)));
    constexpr const char* rejection_names[]{"None","Pause unavailable / paused","Entity / toggle changed",
        "Hero ownership changed","Feature disabled","Dead / HP unreadable","Muted / stunned / state unreadable",
        "Item not in active inventory","Prediction expired","HP changed after prediction","Required heal unavailable"};
    const auto rejection=std::clamp(g_monitor.order_rejection.load(std::memory_order_relaxed),0,10);
    draw_value_row("Last dispatch rejection",rejection_names[rejection],rejection?portfolio_theme::warning:portfolio_theme::text);
    char orders[128]{};
    std::snprintf(orders,sizeof(orders),"%llu / %llu / %llu",
        static_cast<unsigned long long>(g_monitor.orders_queued.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_monitor.orders_dispatched.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_monitor.orders_rejected.load(std::memory_order_relaxed)));
    draw_value_row("Queued / sent / rejected",orders);
    bool logging=g_monitor.debug_logging.load(std::memory_order_relaxed);
    if(portfolio_toggle("Debug logging",&logging))g_monitor.debug_logging.store(logging,std::memory_order_relaxed);
    if(logging&&ImGui::CollapsingHeader("Jungle samples"))draw_jungle_status();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Unload DLL", ImVec2(-1.0f, 32.0f)))
        g_running.store(false, std::memory_order_relaxed);
    end_card();
}

void draw_page_header(overlay_page page) {
    const char* title="ARMLET";
    if(page==overlay_page::render)title="RENDER";
    else if(page==overlay_page::comfort)title="UTILITY";
    else if(page==overlay_page::diagnostics)title="DIAGNOSTICS";
    const auto p=ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),19,p,ImGui::GetColorU32(portfolio_theme::text),title);
    ImGui::Dummy(ImVec2(0,32));
}

void render_armlet_panel() {
    menu_preferences::current.load();
    static overlay_page selected_page=overlay_page::armlet;
    static int last_frame=-2;
    static double appeared=0,page_changed=0;
    const int frame=ImGui::GetFrameCount();
    const double now=ImGui::GetTime();
    if(last_frame!=frame-1)appeared=now;
    last_frame=frame;
    const auto previous_page=selected_page;
    const auto display=ImGui::GetIO().DisplaySize;
    const ImVec2 size(std::min(820.f,std::max(300.f,display.x-32)),
        std::min(610.f,std::max(250.f,display.y-32)));
    ImGui::SetNextWindowPos(ImVec2(24,24),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size,ImGuiCond_Always);
    const float reveal=float(std::clamp((now-appeared)/.16,0.,1.));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,.72f+.28f*reveal);
    // Keep a dedicated title-bar drag area; the body and child panels cannot move the shell.
    constexpr ImGuiWindowFlags flags=ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoResize|
        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing;
    SYSTEMTIME clock{};GetLocalTime(&clock);
    const auto title=menu_preferences::current.title(menu_account_name(),clock);
    if(ImGui::Begin(title.c_str(),nullptr,flags)) {
        const auto position=ImGui::GetWindowPos();
        // Clamp a dragged shell after resolution changes without resetting its position.
        const ImVec2 clamped(std::clamp(position.x,0.f,std::max(0.f,display.x-size.x)),
            std::clamp(position.y,0.f,std::max(0.f,display.y-size.y)));
        if(clamped.x!=position.x||clamped.y!=position.y)ImGui::SetWindowPos(clamped);
        const auto p=ImGui::GetWindowPos(),s=ImGui::GetWindowSize();
        const auto pack=[](ImVec2 v){return (std::uint64_t(std::uint32_t(int(v.y)))<<32)|std::uint32_t(int(v.x));};
        g_native_menu_minimum.store(pack(p),std::memory_order_relaxed);
        g_native_menu_maximum.store(pack(ImVec2(p.x+s.x,p.y+s.y)),std::memory_order_relaxed);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,portfolio_theme::sidebar);
        ImGui::BeginChild("##sidebar",ImVec2(portfolio_theme::sidebar_width,0));
        draw_brand_logo();
        if(sidebar_button("armlet",sidebar_icons::icon::armlet,selected_page==overlay_page::armlet))selected_page=overlay_page::armlet;
        ImGui::Spacing();
        if(sidebar_button("render",sidebar_icons::icon::render,selected_page==overlay_page::render))selected_page=overlay_page::render;
        ImGui::Spacing();
        if(sidebar_button("Utility",sidebar_icons::icon::comfort,selected_page==overlay_page::comfort))selected_page=overlay_page::comfort;
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY()+18,ImGui::GetWindowHeight()-68));
        if(sidebar_button("diagnostics",sidebar_icons::icon::diagnostics,selected_page==overlay_page::diagnostics))selected_page=overlay_page::diagnostics;
        ImGui::EndChild();ImGui::PopStyleColor();
        auto* draw=ImGui::GetWindowDrawList();
        draw->AddLine(ImVec2(p.x+portfolio_theme::sidebar_width,p.y+ImGui::GetFrameHeight()),ImVec2(p.x+portfolio_theme::sidebar_width,p.y+s.y-1),
            ImGui::GetColorU32(portfolio_theme::border));
        if(selected_page!=previous_page) {
            g_bind_capture_active.store(false,std::memory_order_release);
            page_changed=now;
        }
        ImGui::SameLine(0,0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(24,22));
        ImGui::BeginChild("##content",ImVec2(0,0),ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        const float transition=float(std::clamp((now-page_changed)/.14,0.,1.));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,ImGui::GetStyle().Alpha*(.8f+.2f*transition));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY()+4*(1-transition));
        draw_page_header(selected_page);ImGui::Separator();
        // A separate ID keeps each page's controls and animation state independent.
        ImGui::PushID(int(selected_page));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
        ImGui::BeginChild("##page",ImVec2(0,0),ImGuiChildFlags_AlwaysUseWindowPadding,ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("##page_index"),int(selected_page));
        smooth_menu_scroll();
        if(selected_page==overlay_page::armlet)draw_armlet_page();
        else if(selected_page==overlay_page::render)draw_render_page();
        else if(selected_page==overlay_page::comfort)draw_comfort_page();
        else draw_diagnostics_page();
        ImGui::EndChild();ImGui::PopStyleVar();ImGui::PopID();ImGui::PopStyleVar();
        ImGui::EndChild();ImGui::PopStyleVar();
    }
    ImGui::End();ImGui::PopStyleVar();
}

// The game window is the sole producer; Present is the sole consumer. No
// ImGui operation is performed on the WndProc/worker threads after startup.
bool queue_native_menu_input(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_KILLFOCUS||message==WM_SETFOCUS||
        (message==WM_ACTIVATEAPP&&!wparam)) {
        // A focus epoch owns its queued input and captures. Neither a held
        // mouse button nor a binding edit may survive an Alt+Tab round trip.
        g_native_menu_epoch.fetch_add(1,std::memory_order_acq_rel);
        g_native_menu_reset.store(true,std::memory_order_release);
        g_native_menu_mouse.store(false,std::memory_order_relaxed);
        g_native_menu_keyboard.store(false,std::memory_order_relaxed);
        g_bind_capture_active.store(false,std::memory_order_release);
        g_bind_capture_key.store(0,std::memory_order_release);
    }
    if(!g_native_menu_open.load(std::memory_order_acquire))return false;
    const bool mouse=message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST;
    const bool keyboard=message>=WM_KEYFIRST&&message<=WM_KEYLAST;
    if(mouse||keyboard||message==WM_SETFOCUS||message==WM_KILLFOCUS) {
        const unsigned head=g_native_menu_head.load(std::memory_order_relaxed);
        const unsigned next=(head+1)%g_native_menu_messages.size();
        if(next==g_native_menu_tail.load(std::memory_order_acquire))
            g_native_menu_overflow.store(true,std::memory_order_release);
        else {
            g_native_menu_messages[head]={message,wparam,lparam,g_native_menu_epoch.load(std::memory_order_acquire)};
            g_native_menu_head.store(next,std::memory_order_release);
        }
    }
    POINT cursor{};GetCursorPos(&cursor);ScreenToClient(window,&cursor);
    const auto low=g_native_menu_minimum.load(std::memory_order_relaxed);
    const auto high=g_native_menu_maximum.load(std::memory_order_relaxed);
    const bool over=cursor.x>=int(std::uint32_t(low))&&cursor.y>=int(std::uint32_t(low>>32))&&
        cursor.x<int(std::uint32_t(high))&&cursor.y<int(std::uint32_t(high>>32));
    if(message==WM_INPUT) {
        RAWINPUTHEADER header{};UINT bytes=sizeof(header);
        if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam),RID_HEADER,&header,&bytes,
                sizeof(header))!=sizeof(header))return false;
        // SDL uses raw keyboard input too. Cursor position can only decide
        // ownership of a mouse packet, never of A/Q or another game key.
        if(header.dwType==RIM_TYPEKEYBOARD)
            return g_native_menu_keyboard.load(std::memory_order_relaxed);
        if(header.dwType!=RIM_TYPEMOUSE)return false;
        return over||g_native_menu_mouse.load(std::memory_order_relaxed);
    }
    if(mouse)return over||g_native_menu_mouse.load(std::memory_order_relaxed);
    // Insert/End must remain available to the worker even during keyboard capture.
    if(keyboard&&wparam!=VK_INSERT&&wparam!=VK_END)
        return g_native_menu_keyboard.load(std::memory_order_relaxed);
    return false;
}

struct native_menu_gpu_state {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    IDXGISwapChain* chain=nullptr;
    UINT width=0,height=0;
}g_native_menu_gpu;
SRWLOCK g_native_menu_render_lock=SRWLOCK_INIT;

void invalidate_native_menu_target() {
    AcquireSRWLockExclusive(&g_native_menu_render_lock);
    g_native_menu_gpu.target.Reset();
    g_native_menu_gpu.chain=nullptr;
    ReleaseSRWLockExclusive(&g_native_menu_render_lock);
}

void destroy_native_menu_resources() {
    invalidate_native_menu_target();
    if(g_overlay.imgui_dx11_initialized) {
        ImGui_ImplDX11_Shutdown();g_overlay.imgui_dx11_initialized=false;
    }
    g_native_menu_gpu.context.Reset();g_native_menu_gpu.device.Reset();
}

void drain_native_menu_input(ImGuiIO& io) {
    if(g_native_menu_reset.exchange(false,std::memory_order_acq_rel)) {
        io.ClearEventsQueue();
        io.ClearInputKeys();io.ClearInputMouse();
    }
    if(g_native_menu_overflow.exchange(false,std::memory_order_acq_rel)) {
        // A full queue cannot leave buttons held indefinitely.
        io.ClearEventsQueue();io.ClearInputKeys();io.ClearInputMouse();
        g_native_menu_tail.store(g_native_menu_head.load(std::memory_order_acquire),std::memory_order_release);
    }
    unsigned tail=g_native_menu_tail.load(std::memory_order_relaxed);
    const unsigned head=g_native_menu_head.load(std::memory_order_acquire);
    const unsigned epoch=g_native_menu_epoch.load(std::memory_order_acquire);
    while(tail!=head) {
        const auto m=g_native_menu_messages[tail];
        if(m.epoch!=epoch){tail=(tail+1)%g_native_menu_messages.size();continue;}
        // Win32 backend's button path calls SetCapture on the caller thread.
        // Present is not the game window thread: enqueue button state directly.
        int button=-1;bool down=false;
        switch(m.message) {
        case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:button=0;down=true;break;
        case WM_LBUTTONUP:button=0;break;
        case WM_RBUTTONDOWN:case WM_RBUTTONDBLCLK:button=1;down=true;break;
        case WM_RBUTTONUP:button=1;break;
        case WM_MBUTTONDOWN:case WM_MBUTTONDBLCLK:button=2;down=true;break;
        case WM_MBUTTONUP:button=2;break;
        case WM_XBUTTONDOWN:case WM_XBUTTONDBLCLK:button=GET_XBUTTON_WPARAM(m.wparam)==XBUTTON1?3:4;down=true;break;
        case WM_XBUTTONUP:button=GET_XBUTTON_WPARAM(m.wparam)==XBUTTON1?3:4;break;
        }
        if(button>=0) {
            io.AddMousePosEvent(float(GET_X_LPARAM(m.lparam)),float(GET_Y_LPARAM(m.lparam)));
            io.AddMouseButtonEvent(button,down);
        }
        else ImGui_ImplWin32_WndProcHandler(g_overlay.game_window,m.message,m.wparam,m.lparam);
        tail=(tail+1)%g_native_menu_messages.size();
    }
    g_native_menu_tail.store(tail,std::memory_order_release);
}

void render_native_menu(IDXGISwapChain* chain) {
    if(!g_native_menu_open.load(std::memory_order_acquire)||!ImGui::GetCurrentContext()||
        IsIconic(g_overlay.game_window))return;
    if(!TryAcquireSRWLockExclusive(&g_native_menu_render_lock))return;
    struct release_lock {~release_lock(){
        g_native_menu_gpu.target.Reset();
        ReleaseSRWLockExclusive(&g_native_menu_render_lock);
    }}lock;
    auto& gpu=g_native_menu_gpu;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if(FAILED(chain->GetDevice(IID_PPV_ARGS(&device))))return;
    if(device.Get()!=gpu.device.Get()) {
        gpu.target.Reset();gpu.chain=nullptr;
        if(g_overlay.imgui_dx11_initialized)ImGui_ImplDX11_Shutdown();
        g_overlay.imgui_dx11_initialized=false;
        sidebar_icons::shutdown();
        gpu.device=device;device->GetImmediateContext(&gpu.context);
        if(!ImGui_ImplDX11_Init(device.Get(),gpu.context.Get())){gpu.device.Reset();gpu.context.Reset();return;}
        g_overlay.imgui_dx11_initialized=true;
        sidebar_icons::initialize(g_overlay.module,device.Get());
    }
    if(!g_overlay.imgui_dx11_initialized)return;
    {
        gpu.target.Reset();
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
        if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&back))))return;
        D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);gpu.width=desc.Width;gpu.height=desc.Height;
        gpu.context->OMGetRenderTargets(1,&gpu.target,nullptr);
        if(gpu.target) {
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;gpu.target->GetResource(&resource);
            if(resource.Get()!=back.Get())gpu.target.Reset();
        }
        if(!gpu.target&&FAILED(device->CreateRenderTargetView(back.Get(),nullptr,&gpu.target)))return;
        gpu.chain=chain;
    }
    auto& io=ImGui::GetIO();
    drain_native_menu_input(io);
    // Dota already draws its cursor; do not hide/replace it through Win32.
    io.MouseDrawCursor=false;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Win32 coordinates own input; the current backbuffer owns output size.
    // Alt+Tab/fullscreen transitions may temporarily give different sizes.
    io.DisplayFramebufferScale=ImVec2(io.DisplaySize.x>0?gpu.width/io.DisplaySize.x:1.f,
        io.DisplaySize.y>0?gpu.height/io.DisplaySize.y:1.f);
    ImGui::NewFrame();
    render_armlet_panel();
    ImGui::Render();

    g_native_menu_mouse.store(io.WantCaptureMouse,std::memory_order_relaxed);
    // Focusing a settings panel sets WantCaptureKeyboard for navigation. It
    // must not disable gameplay keys while no text/binding editor is active.
    g_native_menu_keyboard.store(io.WantTextInput||g_bind_capture_active.load(std::memory_order_acquire),std::memory_order_relaxed);
    ID3D11RenderTargetView* previous[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* depth=nullptr;
    gpu.context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,previous,&depth);
    auto* target=gpu.target.Get();
    gpu.context->OMSetRenderTargets(1,&target,nullptr);
    // ImGui restores shader/input/raster state. Restore the game's targets too;
    // never clear the native backbuffer or perform a second Present.
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    gpu.context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,previous,depth);
    for(auto* value:previous)if(value)value->Release();
    if(depth)depth->Release();
}

/**
 * @brief Сбрасывает зависимые от героя данные при его отсутствии/ошибке чтения, чтобы FSM не
 * действовал по снимку предыдущего героя.
 */
void clear_runtime_state(armlet_status status) {
    g_monitor.health.store(-1, std::memory_order_relaxed);
    g_monitor.max_health.store(-1, std::memory_order_relaxed);
    g_monitor.item_found.store(false, std::memory_order_relaxed);
    g_monitor.toggle_state.store(false, std::memory_order_relaxed);
    g_monitor.effect_confirmed.store(false, std::memory_order_relaxed);
    g_monitor.armlet_address.store(0, std::memory_order_relaxed);
    g_monitor.modifier_address.store(0, std::memory_order_relaxed);
    g_monitor.armlet_entity_index.store(-1, std::memory_order_relaxed);
    g_monitor.automation_ready.store(false, std::memory_order_relaxed);
    g_monitor.stunned_or_muted.store(false, std::memory_order_relaxed);
    g_monitor.safety.store(safety_state::unavailable, std::memory_order_relaxed);
    g_monitor.expected_cycle_ms.store(0, std::memory_order_relaxed);
    g_monitor.incoming_count.store(0, std::memory_order_relaxed);
    g_monitor.incoming_damage.store(0.0f, std::memory_order_relaxed);
    g_monitor.nearest_impact_ms.store(-1.0f, std::memory_order_relaxed);
    g_monitor.predicted_minimum_hp.store(-1.0f, std::memory_order_relaxed);
    g_monitor.modifier_tick_count.store(0, std::memory_order_relaxed);
    g_monitor.internal_tower_count.store(0, std::memory_order_relaxed);
    g_monitor.internal_melee_count.store(0, std::memory_order_relaxed);
    g_monitor.high_risk_detected.store(false, std::memory_order_relaxed);
    g_monitor.high_risk_interval_ms.store(-1.0f, std::memory_order_relaxed);
    g_monitor.game_paused.store(false, std::memory_order_relaxed);
    g_monitor.observed_loss_is_self_cost.store(false, std::memory_order_relaxed);
    g_monitor.burning_spear_health_cost.store(0, std::memory_order_relaxed);
    g_monitor.burning_spear_cost_at.store(0, std::memory_order_relaxed);
    g_monitor.hero_address.store(0, std::memory_order_relaxed);
    g_monitor.status.store(status, std::memory_order_relaxed);
}

void read_modifier_telemetry(std::uintptr_t modifier) {
    int value = 0;
    float float_value = 0.0f;
#define READ_INT_FIELD(field, destination) \
    do { if (schema_fields::field > 0 && \
        safe_read(modifier + schema_fields::field, value)) \
        g_monitor.destination.store(value, std::memory_order_relaxed); } while (false)
#define READ_FLOAT_FIELD(field, destination) \
    do { if (schema_fields::field > 0 && \
        safe_read(modifier + schema_fields::field, float_value)) \
        g_monitor.destination.store(float_value, std::memory_order_relaxed); } while (false)

    READ_INT_FIELD(unholy_bonus_damage, bonus_damage);
    READ_INT_FIELD(unholy_bonus_attack_speed, bonus_attack_speed);
    READ_INT_FIELD(unholy_bonus_strength, bonus_strength);
    READ_INT_FIELD(unholy_bonus_armor, bonus_armor);
    READ_INT_FIELD(unholy_health_drain_per_second, health_drain_per_second);
    READ_INT_FIELD(str_tick_count, tick_count);
    READ_INT_FIELD(unholy_bonus_slow_resistance, slow_resistance);
    READ_FLOAT_FIELD(tick_interval, tick_interval);
    READ_FLOAT_FIELD(m_flDamageRemainder, damage_remainder);

#undef READ_FLOAT_FIELD
#undef READ_INT_FIELD
}
