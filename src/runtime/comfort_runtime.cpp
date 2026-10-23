#include "comfort_client_profile.h"
// This file follows schema/order readers in the existing runtime unity unit.
// Communication presentation hooks never alter decoded network/gameplay state.
namespace comfort_runtime {
using namespace ::askerdway::comfort;
std::mutex lock;
policy rules;
std::atomic<unsigned> available{0};
std::atomic<int> silence_key{0};
std::atomic<bool> bind_capture{false};
std::atomic<unsigned> key_presses{0};
std::atomic<bool> saved{false};
std::atomic<bool> alive{false};
std::atomic<bool> context_ready{false};
std::atomic<unsigned> calls{0};
std::uint64_t settings_revision = 0;
bool settings_dirty = false;
std::wstring settings_path;
struct player { std::uint64_t steam = 0; int team = 0; int entity = -1; std::uint32_t generation = UINT32_MAX; char name[128]{}; };
std::array<player, 64> players{};
std::array<std::uint64_t, 64> known_steam{};
std::array<std::uint32_t, 64> known_generation{};
std::atomic<std::uintptr_t> voice_map{0};
struct call_guard {
    call_guard() { calls.fetch_add(1, std::memory_order_acq_rel); }
    ~call_guard() { calls.fetch_sub(1, std::memory_order_release); }
};

void config_changed(const askerdway_comfort_config_v1& c) {
    rules.config = c;
    rules.clear_history();
    silence_key.store(c.silence_hotkey, std::memory_order_release);
    settings_dirty = true; ++settings_revision; saved.store(false);
}
void load_settings() {
    wchar_t directory[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, DWORD(std::size(directory)));
    if (!length || length >= std::size(directory)) return;
    settings_path = std::wstring(directory, length) + L"\\Askerdway";
    if (!CreateDirectoryW(settings_path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        settings_path.clear(); return;
    }
    settings_path += L"\\comfort.ini";
    const HANDLE file = CreateFileW(settings_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char data[513]{}; DWORD read = 0;
    const bool ok = ReadFile(file, data, 512, &read, nullptr) && read < 512;
    CloseHandle(file);
    askerdway_comfort_config_v1 c{};
    if (ok && decode_settings(std::string_view(data, read), c)) {
        if (c.silence_hotkey == g_monitor.abuse_hotkey.load()) c.silence_hotkey = 0;
        std::lock_guard guard(lock); rules.config = c;
        silence_key.store(c.silence_hotkey); saved.store(true);
    }
}
void flush_settings() {
    askerdway_comfort_config_v1 c{}; std::uint64_t revision = 0;
    {
        std::lock_guard guard(lock);
        if (!settings_dirty || settings_path.empty()) return;
        c = rules.config; revision = settings_revision;
    }
    const auto data = encode_settings(c);
    const HANDLE file = CreateFileW(settings_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), DWORD(data.size()), &written, nullptr) &&
        written == data.size() && FlushFileBuffers(file);
    CloseHandle(file);
    std::lock_guard guard(lock);
    if (revision == settings_revision) { settings_dirty = !ok; saved.store(ok); }
}

// Build-specific bridges are guarded by PE identity AND their instruction bytes.
// The dispatch/vtable and protobuf types themselves are found through MSVC RTTI.
// Evidence and method contracts: docs/COMFORT_RU.md.
struct presentation_hook {
    const char* name;
    unsigned id;
    std::uintptr_t table = 0, protobuf_table = 0;
    void* entry = nullptr;
    using function = void(*)(void*, const void*);
    function original = nullptr;
    bool installed = false;
};
std::array hooks{
    presentation_hook{"CUserMessageSayText", 117},
    presentation_hook{"CUserMessageSayText2", 118},
    presentation_hook{"CDOTAUserMsg_ChatMessage", 612},
    presentation_hook{"CDOTAUserMsg_BotChat", 490},
    presentation_hook{"CDOTAUserMsg_AbilityPing", 523},
    presentation_hook{"CDOTAUserMsg_ItemAlert", 499},
    presentation_hook{"CDOTAUserMsg_EnemyItemAlert", 534},
    presentation_hook{"CDOTAUserMsg_EmptyItemSlotAlert", 607},
    presentation_hook{"CDOTAUserMsg_HPManaAlert", 544},
    presentation_hook{"CDOTAUserMsg_SalutePlayer", 576},
    presentation_hook{"CDOTAUserMsg_LocationPing", 477},
    presentation_hook{"CDOTAUserMsg_MapLine", 478},
    presentation_hook{"CDOTAUserMsg_WorldLine", 497},
    presentation_hook{"CDOTAUserMsg_ChatWheel", 501},
    presentation_hook{"CDOTAUserMsg_WillPurchaseAlert", 529},
    presentation_hook{"CDOTAUserMsg_BuyBackStateAlert", 537},
    presentation_hook{"CDOTAUserMsg_QuickBuyAlert", 540},
    presentation_hook{"CDOTAUserMsg_ModifierAlert", 543},
    presentation_hook{"CDOTAUserMsg_GlyphAlert", 545},
    presentation_hook{"CDOTAUserMsg_XPAlert", 555},
    presentation_hook{"CDOTAUserMsg_SuggestHeroPick", 560},
    presentation_hook{"CDOTAUserMsg_SuggestHeroRole", 561},
    presentation_hook{"CDOTAUserMsg_DamageReport", 575},
    presentation_hook{"CDOTAUserMsg_EmptyTeleportAlert", 579},
    presentation_hook{"CDOTAUserMsg_RadarAlert", 587},
    presentation_hook{"CDOTAUserMsg_TalentTreeAlert", 589},
    presentation_hook{"CDOTAUserMsg_AghsStatusAlert", 608},
    presentation_hook{"CDOTAUserMsg_NeutralCampAlert", 613},
    presentation_hook{"CDOTAUserMsg_FacetPing", 624},
    presentation_hook{"CDOTAUserMsg_InnatePing", 625},
    presentation_hook{"CDOTAUserMsg_RoshanTimer", 626},
    presentation_hook{"CDOTAUserMsg_TimerAlert", 628},
    presentation_hook{"CDOTAUserMsg_MadstoneAlert", 629},
    presentation_hook{"CDOTAUserMsg_TormentorTimer", 634},
};
struct extra_hook { void* entry = nullptr; void* original = nullptr; bool installed = false; };
extra_hook voice, wheel_callback, wheel_format, wheel_audio, receive_sound;
std::uintptr_t client_base = 0;
std::size_t client_size = 0;
std::uintptr_t voice_type_table = 0;
bool owns_minhook = false;
thread_local unsigned wheel_hidden = 0;

int entity_player(std::uintptr_t entity) {
    char name[96]{};
    if (!plausible_pointer(entity) || !read_designer_name(entity, name)) return -1;
    int id = -1;
    if (std::strcmp(name, "dota_player_controller") == 0 && schema_fields::controller_m_nPlayerID > 0)
        safe_read(entity + schema_fields::controller_m_nPlayerID, id);
    else if (std::strncmp(name, "npc_dota_hero_", 14) == 0 && schema_fields::hero_m_iPlayerID > 0)
        safe_read(entity + schema_fields::hero_m_iPlayerID, id);
    else if (std::strncmp(name, "item_", 5) == 0 && schema_fields::m_iPlayerOwnerID > 0)
        safe_read(entity + schema_fields::m_iPlayerOwnerID, id);
    return valid_player(id) ? id : -1;
}
unsigned decision(presentation_hook& hook, const void* wrapper, const void* consumer) {
    if (!alive.load(std::memory_order_acquire) || !context_ready.load(std::memory_order_acquire) || !wrapper) return 0;
    {
        std::lock_guard guard(lock);
        if (!rules.enabled() && !rules.silence) return 0;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(wrapper) + 0x30;
    std::uintptr_t table = 0;
    if (!safe_read(address, table) || table != hook.protobuf_table) return 0;
    std::array<std::uintptr_t, 12> methods{};
    if (!safe_read(table, methods)) return 0;
    std::array<std::byte, 16448> wire{}; std::size_t size = 0;
    if (!serialize_checked(reinterpret_cast<const void*>(address),
            reinterpret_cast<size_function>(methods[7]), reinterpret_cast<serialize_function>(methods[11]),
            wire.data(), size)) return 0;
    auto decoded = decode(hook.id, std::span(wire.data(), size));
    auto& e = decoded.value;
    e.consumer = reinterpret_cast<std::uintptr_t>(consumer);
    if (e.kind == category::unknown) return 0;
    std::uintptr_t system = 0;
    read_game_entity_system(client_base, system);
    if (decoded.source_entity >= 0) e.sender = entity_player(entity_by_index(system, decoded.source_entity));
    if (decoded.target_entity >= 0) e.target = entity_player(entity_by_index(system, decoded.target_entity));
    if (decoded.needs_item_identity) {
        const auto item = entity_by_index(system, decoded.ability_entity);
        if (is_item_ability(decoded.ability_id)) {
            e.kind = category::item;
            // entity_id may be the owner hero rather than an item entity.
            const int owner = entity_player(item);
            if (owner >= 0) e.target = owner;
        }
    }
    if (decoded.needs_death_state && schema_fields::comfort_life_state > 0) {
        const auto target = entity_by_index(system, decoded.target_entity);
        std::uint8_t state = 0;
        if (valid_player(e.target) && safe_read(target + schema_fields::comfort_life_state, state) &&
            state > 0 && state <= 2) e.kind = category::respawn;
    }
    // Header identity includes the delivery tick/metadata, rather than only a reused address.
    std::array<std::byte, 40> metadata{};
    if (safe_copy_memory(reinterpret_cast<std::uintptr_t>(wrapper) + 8, metadata.data(), metadata.size()))
        e.delivery = hash(metadata) ^ reinterpret_cast<std::uintptr_t>(wrapper) ^ e.fingerprint;
    std::lock_guard guard(lock);
    if (valid_player(e.sender)) e.sender_team = players[e.sender].team;
    return rules.evaluate(e, GetTickCount64());
}

template<std::size_t I> void dispatch(void* self, const void* message) {
    call_guard guard;
    auto& hook = hooks[I];
    // Wheel's real callback is hooked too: its deferred delivery needs a scoped output decision.
    if (hook.id != 501 && decision(hook, message, self) == all_surfaces) return;
    hook.original(self, message);
}
template<std::size_t... I> constexpr auto dispatchers(std::index_sequence<I...>) {
    return std::array<presentation_hook::function, sizeof...(I)>{&dispatch<I>...};
}
constexpr auto detours = dispatchers(std::make_index_sequence<hooks.size()>{});

void on_wheel(void* self, const void* message) {
    call_guard guard;
    auto it = std::find_if(hooks.begin(), hooks.end(), [](auto& h) { return h.id == 501; });
    const unsigned hidden = decision(*it, message, self);
    if (hidden == all_surfaces) return;
    const unsigned previous = wheel_hidden;
    wheel_hidden = hidden;
    reinterpret_cast<presentation_hook::function>(wheel_callback.original)(self, message);
    wheel_hidden = previous;
}
std::uintptr_t format_wheel(void* self, void* parameters, void* output) {
    call_guard guard;
    using fn = std::uintptr_t(*)(void*, void*, void*);
    const auto result = reinterpret_cast<fn>(wheel_format.original)(self, parameters, output);
    if (wheel_hidden & text) {
        // The inspected output is CUtlStringBuilder: length, capacity flags, inline/pointer storage.
        auto* words = static_cast<std::uint32_t*>(output);
        const auto capacity = words[1];
        char* chars = (capacity & (1u << 30)) ? reinterpret_cast<char*>(words + 2) :
            *reinterpret_cast<char**>(words + 2);
        if ((capacity & 0x3fffffffu) && chars) *chars = '\0';
        words[0] &= 0xc0000000u;
    }
    return result;
}
void play_wheel(void* self, int* duration, void* definition, void* hero, int player_id) {
    call_guard guard;
    if (wheel_hidden & sound) { if (duration) *duration = 0; return; }
    using fn = void(*)(void*, int*, void*, void*, int);
    reinterpret_cast<fn>(wheel_audio.original)(self, duration, definition, hero, player_id);
}
std::uintptr_t play_receive(std::uintptr_t result, void* recipients, int entity, const char* name,
    float volume, void* data) {
    call_guard guard;
    if ((wheel_hidden & sound) && result) { *reinterpret_cast<std::uint64_t*>(result) = 0; return result; }
    using fn = std::uintptr_t(*)(std::uintptr_t, void*, int, const char*, float, void*);
    return reinterpret_cast<fn>(receive_sound.original)(result, recipients, entity, name, volume, data);
}
unsigned voice_flags(void* map, std::uint64_t steam_id) {
    call_guard guard;
    using fn = unsigned(*)(void*, std::uint64_t);
    const unsigned original = reinterpret_cast<fn>(voice.original)(map, steam_id);
    if (!alive.load(std::memory_order_acquire) || !steam_id) return original;
    const auto object = reinterpret_cast<std::uintptr_t>(map) - 0x18;
    std::uintptr_t type = 0;
    if (!voice_type_table || !safe_read(object, type) || type != voice_type_table) return original;
    voice_map.store(reinterpret_cast<std::uintptr_t>(map), std::memory_order_release);
    if (!context_ready.load(std::memory_order_acquire)) return original;
    std::lock_guard state_guard(lock);
    for (int i = 0; i < 64; ++i)
        if (players[i].steam == steam_id && rules.player_hidden(i)) return original | 1u;
    return original;
}

bool add_hook(extra_hook& h, std::uintptr_t rva, std::initializer_list<unsigned char> prologue, void* detour) {
    if (rva + prologue.size() >= client_size) return false;
    std::array<unsigned char, 32> code{};
    if (prologue.size() > code.size() || !safe_copy_memory(client_base + rva, code.data(), prologue.size()) ||
        !std::equal(prologue.begin(), prologue.end(), code.begin())) return false;
    h.entry = reinterpret_cast<void*>(client_base + rva);
    if (MH_CreateHook(h.entry, detour, &h.original) != MH_OK) return false;
    if (MH_EnableHook(h.entry) != MH_OK) { MH_RemoveHook(h.entry); return false; }
    h.installed = true; return true;
}

void install(std::uintptr_t base, std::size_t size) {
    client_base = base; client_size = size;
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!safe_read(base, dos) || !safe_read(base + dos.e_lfanew, nt))return;
    const auto* profile=find_client_profile(nt.FileHeader.TimeDateStamp,unsigned(size));
    if (!profile) {
        log_message("COMFORT", "Client presentation ABI is not supported; Comfort filters are unavailable."); return;
    }
    const auto mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) return;
    owns_minhook = mh == MH_OK; // Renderer initialization may have failed before Comfort starts.
    std::vector<std::byte> image(size);
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    const auto section_address = base + dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    if (!safe_copy_memory(section_address, sections.data(), sections.size() * sizeof(IMAGE_SECTION_HEADER))) return;
    for (const auto& s : sections) {
        if (std::size_t(s.VirtualAddress) + s.Misc.VirtualSize > size) return;
        // RTTI lives in initialized non-executable data. Ignore inaccessible padding/gaps.
        if ((s.Characteristics & IMAGE_SCN_MEM_READ) && !(s.Characteristics & IMAGE_SCN_MEM_EXECUTE))
            safe_copy_memory(base + s.VirtualAddress, image.data() + s.VirtualAddress, s.Misc.VirtualSize);
    }
    const auto u32 = [&](std::size_t o) { std::uint32_t v = 0; if (o + 4 <= size) std::memcpy(&v, image.data()+o, 4); return v; };
    const auto u64 = [&](std::size_t o) { std::uint64_t v = 0; if (o + 8 <= size) std::memcpy(&v, image.data()+o, 8); return v; };
    std::unordered_map<std::string, std::uintptr_t> tables;
    for (const auto& section : sections) {
        if (!(section.Characteristics & IMAGE_SCN_MEM_READ) || (section.Characteristics & IMAGE_SCN_MEM_WRITE) ||
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        for (std::size_t o = section.VirtualAddress; o + 56 < section.VirtualAddress + section.Misc.VirtualSize; o += 8) {
            const auto pointer = u64(o);
            if (pointer < base || pointer + 24 >= base + size) continue;
            const auto col = std::size_t(pointer - base);
            if (u32(col) != 1 || u32(col+4) != 0 || u32(col+20) != col) continue;
            const auto td = std::size_t(u32(col+12));
            if (td + 256 >= size) continue;
            const char* name = reinterpret_cast<const char*>(image.data() + td + 16);
            const auto length = strnlen(name, 240);
            if (length < 4 || length >= 240 || std::strncmp(name, ".?AV", 4)) continue;
            tables.emplace(std::string(name, length), base + o + 8);
        }
    }
    for (std::size_t i = 0; i < hooks.size(); ++i) {
        auto& h = hooks[i];
        auto dt = tables.find(std::string(".?AV?$CGameMessageDelegateHook@V") + h.name + "_t@@@@");
        auto pt = tables.find(std::string(".?AV") + h.name + "@@");
        if (dt == tables.end() || pt == tables.end()) continue;
        h.table = dt->second; h.protobuf_table = pt->second;
        std::array<std::uintptr_t, 6> methods{}; std::uintptr_t size_getter = 0;
        std::array<unsigned char, 6> id_code{}; std::array<unsigned char, 4> cached_size{};
        if (!safe_read(h.table, methods) || !safe_read(methods[2], id_code) || id_code[0] != 0xb8 ||
            id_code[5] != 0xc3 || !safe_read(h.protobuf_table + 8*8, size_getter) ||
            !safe_read(size_getter, cached_size) || cached_size != std::array<unsigned char,4>{0x8b,0x41,0x14,0xc3}) continue;
        unsigned id = 0; std::memcpy(&id, id_code.data()+1, 4); if (id != h.id) continue;
        h.entry = reinterpret_cast<void*>(methods[5]);
        if (MH_CreateHook(h.entry, reinterpret_cast<void*>(detours[i]), reinterpret_cast<void**>(&h.original)) != MH_OK) continue;
        if (MH_EnableHook(h.entry) != MH_OK) { MH_RemoveHook(h.entry); continue; }
        h.installed = true;
    }
    // These RVAs are used only after both PE checks above and exact ABI prologue validation.
    if (const auto type = tables.find(".?AVCVoiceStatus@@"); type != tables.end()) {
        voice_type_table = type->second;
        add_hook(voice, profile->voice, {0x48,0x89,0x54,0x24,0x10,0x53,0x48,0x83,0xec,0x20}, reinterpret_cast<void*>(&voice_flags));
    }
    const bool wheel_ok = add_hook(wheel_callback, profile->wheel,
        {0x48,0x8b,0xc4,0x55,0x41,0x56,0x41,0x57}, reinterpret_cast<void*>(&on_wheel));
    const bool format_ok = add_hook(wheel_format, profile->format,
        {0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x4c,0x24,0x08}, reinterpret_cast<void*>(&format_wheel));
    const bool audio_ok = add_hook(wheel_audio, profile->audio,
        {0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x4c,0x24,0x08}, reinterpret_cast<void*>(&play_wheel));
    const bool receive_ok = add_hook(receive_sound, profile->receive,
        {0x40,0x53,0x48,0x83,0xec,0x60,0x48,0x8b,0xd9}, reinterpret_cast<void*>(&play_receive));
    unsigned features = 0;
    const auto has = [](unsigned id) { for (auto& h : hooks) if (h.id == id) return h.installed; return false; };
    if (has(529)) features |= ASKERDWAY_COMFORT_FEATURE_ITEMS;
    if (has(544)) features |= ASKERDWAY_COMFORT_FEATURE_RESPAWN;
    if (has(576)) features |= ASKERDWAY_COMFORT_FEATURE_TIPS;
    if (has(117) && has(118) && has(612) && has(490)) features |= ASKERDWAY_COMFORT_FEATURE_TEXT;
    const bool signals_ready = std::all_of(hooks.begin(), hooks.end(), [](const auto& h) {
        switch (h.id) {
        case 117: case 118: case 612: case 490: case 501: case 576: case 529: case 544: return true;
        default: return h.installed;
        }
    });
    if (signals_ready) features |= ASKERDWAY_COMFORT_FEATURE_PINGS;
    if (wheel_ok && format_ok) features |= ASKERDWAY_COMFORT_FEATURE_WHEEL_TEXT;
    if (wheel_ok && audio_ok && receive_ok) features |= ASKERDWAY_COMFORT_FEATURE_WHEEL_SOUND;
    // Voice readiness additionally requires an observed CVoiceStatus lookup and player identities.
    available.store(features, std::memory_order_release);
    alive.store(true, std::memory_order_release);
    char summary[96]{}; unsigned count = 0; for (auto& h : hooks) count += h.installed;
    std::snprintf(summary, sizeof(summary), "Presentation delegates installed: %u/%zu, features=0x%X.", count, hooks.size(), features);
    log_message("COMFORT", summary);
}

void poll() {
    static ULONGLONG next = 0;
    const auto now = GetTickCount64();
    if (now < next) return;
    next = now + 500;
    flush_settings();
    {
        std::lock_guard guard(lock);
        if (!rules.enabled() && !g_native_menu_open.load()) { context_ready.store(false); return; }
    }
    std::uintptr_t system = 0, game_rules = 0;
    if (!read_game_entity_system(client_base, system) ||
        !safe_read(g_game_rules_global.load(), game_rules) || !plausible_pointer(game_rules)) {
        context_ready.store(false, std::memory_order_release); return;
    }
    const int local = read_local_player_id();
    if (!valid_player(local)) { context_ready.store(false, std::memory_order_release); return; }
    int phase = 0;
    if (schema_fields::game_rules_m_nGameState > 0) safe_read(game_rules + schema_fields::game_rules_m_nGameState, phase);
    if (phase >= 6) {
        context_ready.store(false, std::memory_order_release);
        std::lock_guard guard(lock); rules.set_context({-1,0,0}); players = {}; known_steam = {}; known_generation = {};
        return;
    }
    std::uint64_t match = 0;
    if (schema_fields::comfort_match_id > 0) safe_read(game_rules + schema_fields::comfort_match_id, match);
    if (!match) {
        // Offline/Demo matches have no GC match ID. Detect a new session independently of hero respawns.
        static std::uintptr_t previous_rules = 0;
        static int previous_phase = 0;
        static float previous_time = 0;
        static std::uint64_t generation = 0;
        float game_time = 0; const bool has_time = read_game_time(client_base, game_time);
        if (game_rules != previous_rules || (previous_phase >= 4 && phase > 0 && phase < 4) ||
            (has_time && game_time + 5 < previous_time)) ++generation;
        previous_rules = game_rules; previous_phase = phase;
        if (has_time) previous_time = game_time;
        match = game_rules ^ (generation << 32);
    }
    std::array<player, 64> roster{};
    static std::array<std::uint32_t, 64> controller_handles{};
    static std::uint64_t roster_session = 0;
    static ULONGLONG next_discovery = 0;
    const auto collect = [&](std::uintptr_t entity, int index) {
        char name[96]{};
        if (!read_designer_name(entity, name) || std::strcmp(name, "dota_player_controller") != 0) return false;
        const int id = entity_player(entity); if (!valid_player(id)) return false;
        auto& p = roster[id]; p.entity = index;
        std::uintptr_t identity = 0;
        if (schema_fields::m_pEntityIdentity > 0 && safe_read(entity + schema_fields::m_pEntityIdentity, identity))
            safe_read(identity + 0x10, p.generation);
        std::uint8_t team = 0; safe_read(entity + schema_fields::m_iTeamNum, team); p.team = team;
        if (schema_fields::comfort_steam_id > 0) safe_read(entity + schema_fields::comfort_steam_id, p.steam);
        if (schema_fields::comfort_player_name > 0) safe_read(entity + schema_fields::comfort_player_name, p.name);
        p.name[127] = 0;
        if (!p.name[0]) std::snprintf(p.name, sizeof(p.name), "Player %d", id+1);
        controller_handles[id] = p.generation;
        return false;
    };
    if (roster_session != match || now >= next_discovery) {
        controller_handles.fill(UINT32_MAX); roster_session = match; next_discovery = now + 5000;
        visit_entities(system, collect);
    } else {
        for (const auto handle : controller_handles) if (handle != UINT32_MAX) {
            const auto entity = entity_by_handle(system, handle);
            if (entity) collect(entity, int(handle & 0x7fffu));
        }
        if (roster[local].entity < 0) next_discovery = 0;
    }
    {
        std::lock_guard guard(lock);
        if (rules.current_context().session != match) { known_steam = {}; known_generation = {}; }
        rules.set_context({local, roster[local].team, match});
        for (unsigned i = 0; i < 64; ++i) {
            if (roster[i].entity < 0) continue;
            if ((known_steam[i] && roster[i].steam && known_steam[i] != roster[i].steam) ||
                (!roster[i].steam && known_generation[i] && roster[i].generation != UINT32_MAX &&
                    known_generation[i] != roster[i].generation))
                rules.blocked &= ~(1ull << i);
            if (roster[i].steam) known_steam[i] = roster[i].steam;
            if (roster[i].generation != UINT32_MAX) known_generation[i] = roster[i].generation;
        }
        players = roster;
    }
    context_ready.store(roster[local].entity >= 0 && roster[local].team > 0, std::memory_order_release);
    unsigned features = available.load();
    if (schema_fields::comfort_life_state <= 0) features &= ~ASKERDWAY_COMFORT_FEATURE_RESPAWN;
    if (voice.installed && voice_map.load() && schema_fields::comfort_steam_id > 0)
        features |= ASKERDWAY_COMFORT_FEATURE_VOICE;
    available.store(features, std::memory_order_release);
}
void toggle_silence() { std::lock_guard guard(lock); rules.silence = !rules.silence; rules.clear_history(); }
bool shutdown() {
    alive.store(false, std::memory_order_release); context_ready.store(false); available.store(0);
    flush_settings();
    bool ok = true;
    for (auto& h : hooks) if (h.installed && MH_DisableHook(h.entry) != MH_OK) ok = false;
    for (auto* h : {&voice, &wheel_callback, &wheel_format, &wheel_audio, &receive_sound})
        if (h->installed && MH_DisableHook(h->entry) != MH_OK) ok = false;
    for (unsigned i = 0; calls.load(std::memory_order_acquire) && i < 100; ++i) Sleep(5);
    if (!ok || calls.load(std::memory_order_acquire)) return false;
    for (auto& h : hooks) if (h.installed) {
        if (MH_RemoveHook(h.entry) != MH_OK) return false;
        h.installed = false;
    }
    for (auto* h : {&voice, &wheel_callback, &wheel_format, &wheel_audio, &receive_sound}) if (h->installed) {
        if (MH_RemoveHook(h->entry) != MH_OK) return false;
        h->installed = false;
    }
    // When Present exists its renderer owns MinHook and uninitializes it later.
    // With no renderer, release the library state initialized by Comfort itself.
    if (owns_minhook) {
        if (MH_Uninitialize() != MH_OK) return false;
        owns_minhook = false;
    }
    return true;
}
} // namespace comfort_runtime
