/**
 * @file
 * @brief Чтение мира и сбор данных для оверлея.
 *
 * Разрешает сущности и локального героя, отслеживает видимость, способности, телепорты и лагеря.
 * poll_render_intelligence обновляет составной снимок под эксклюзивной блокировкой; bool-
 * результаты чтения обязательны к проверке.
 */

// Entity and render data readers.

/**
 * @brief Читает время/паузу движка. Game time нельзя напрямую вычитать из GetTickCount64 или
 * steady_clock без преобразования единиц и начала отсчёта.
 */
bool read_game_time(std::uintptr_t client_base, float& game_time);
bool read_game_time(std::uintptr_t client_base, float& game_time, float& tick_interval);
bool read_local_creep_spawning(bool& spawning,std::uint32_t& revision);

/**
 * @brief Читает ограниченную строку из памяти движка. Выходной буфер принадлежит вызывающей
 * стороне; ошибка чтения не означает совпадение имени.
 */
bool read_designer_name(std::uintptr_t entity, char (&name)[96]) {
    std::uintptr_t identity = 0;
    std::uintptr_t name_address = 0;
    return schema_fields::m_pEntityIdentity > 0 &&
        schema_fields::m_designerName > 0 &&
        safe_read(entity + schema_fields::m_pEntityIdentity, identity) &&
        plausible_pointer(identity) &&
        safe_read(identity + schema_fields::m_designerName, name_address) &&
        read_c_string(name_address, name);
}

/**
 * @brief Ищет сущность по индексу через chunk layout. Полученный адрес нужно использовать
 * кратковременно и повторно проверять при следующем polling.
 */
std::uintptr_t entity_by_index_with_layout(std::uintptr_t entity_system, int index,
    std::size_t chunk_table_offset, std::size_t entry_stride) {
    if (!plausible_pointer(entity_system) || index < 0 || index > 0x7FFF ||
        chunk_table_offset == 0 || entry_stride < sizeof(void*))
        return 0;

    std::uintptr_t chunk = 0;
    if (!safe_read(entity_system + chunk_table_offset +
            sizeof(void*) * (static_cast<unsigned>(index) >> 9), chunk) ||
        !plausible_pointer(chunk))
        return 0;

    std::uintptr_t entity = 0;
    if (!safe_read(chunk + entry_stride *
            (static_cast<unsigned>(index) & 0x1FF), entity))
        return 0;
    return entity;
}

/**
 * @brief Ищет сущность по индексу через chunk layout. Полученный адрес нужно использовать
 * кратковременно и повторно проверять при следующем polling.
 */
std::uintptr_t entity_by_index(std::uintptr_t entity_system, int index) {
    return entity_by_index_with_layout(entity_system, index,
        g_entity_chunk_table_offset.load(std::memory_order_acquire),
        g_entity_entry_stride.load(std::memory_order_acquire));
}

template <typename Visitor>
/**
 * @brief Обходит таблицу сущностей пакетами по chunk, снижая число отдельных чтений памяти;
 * callback работает с текущим снимком перечисления.
 */
bool visit_entities(std::uintptr_t entity_system, Visitor&& visitor) {
    if (!plausible_pointer(entity_system))
        return false;

    const auto chunk_table_offset = g_entity_chunk_table_offset.load(
        std::memory_order_acquire);
    const auto entry_stride = g_entity_entry_stride.load(std::memory_order_acquire);
    if (chunk_table_offset == 0 || entry_stride < sizeof(void*))
        return false;

    // CEntityHandle uses a 15-bit entry index: 64 chunks with 512 entries.
    // Reading one pointer per slot used up to 32,768 ReadProcessMemory calls
    // for every hero/camp/tower refresh. Snapshot each populated chunk once;
    // the private entry stride is already calibrated against a live handle.
    constexpr std::size_t maximum_entry_stride = 0x100;
    std::array<std::byte, 512 * maximum_entry_stride> chunk_snapshot{};
    for (unsigned chunk_index = 0; chunk_index < 64; ++chunk_index) {
        std::uintptr_t chunk = 0;
        if (!safe_read(entity_system + chunk_table_offset +
                sizeof(void*) * chunk_index, chunk) ||
            !plausible_pointer(chunk))
            continue;

        const std::size_t requested_bytes = entry_stride <= maximum_entry_stride
            ? 512 * entry_stride : 0;
        const bool bulk_read = requested_bytes != 0 &&
            safe_copy_memory(chunk, chunk_snapshot.data(), requested_bytes);
        for (unsigned entry_index = 0; entry_index < 512; ++entry_index) {
            std::uintptr_t entity = 0;
            if (bulk_read) {
                std::memcpy(&entity, chunk_snapshot.data() +
                    entry_stride * entry_index, sizeof(entity));
            } else if (!safe_read(chunk + entry_stride * entry_index, entity)) {
                continue;
            }
            if (!plausible_pointer(entity) ||
                entity == chunk)
                continue;
            if (visitor(entity, static_cast<int>(chunk_index * 512 + entry_index)))
                return true;
        }
    }
    return false;
}

/**
 * @brief Разрешает handle в текущей таблице сущностей; не превращает адрес результата в
 * долговечную ссылку.
 */
bool entity_layout_matches_handle(std::uintptr_t entity_system, int index,
    std::uint32_t handle, std::size_t chunk_table_offset,
    std::size_t entry_stride);

std::uintptr_t entity_by_handle(std::uintptr_t entity_system, std::uint32_t handle) {
    if (handle == 0xFFFFFFFFu)
        return 0;
    const auto index = static_cast<int>(handle & 0x7FFFu);
    if (handle > 0x7FFFu && !entity_layout_matches_handle(entity_system, index,
            handle, g_entity_chunk_table_offset.load(std::memory_order_acquire),
            g_entity_entry_stride.load(std::memory_order_acquire)))
        return 0;
    return entity_by_index(entity_system, index);
}

bool looks_like_local_hero(std::uintptr_t entity) {
    int health = 0;
    int max_health = 0;
    std::uintptr_t scene_node = 0;
    return plausible_pointer(entity) &&
        safe_read(entity + schema_fields::m_iHealth, health) &&
        safe_read(entity + schema_fields::m_iMaxHealth, max_health) &&
        safe_read(entity + schema_fields::m_pGameSceneNode, scene_node) &&
        health >= 0 && max_health > 0 && health <= max_health * 4 &&
        plausible_pointer(scene_node);
}

bool has_hero_rtti(std::uintptr_t entity) {
    const auto client_base = g_client_base_address.load(std::memory_order_acquire);
    if (!plausible_pointer(client_base))
        return false;
    const auto client_size = image_size(client_base);
    char type_name[192]{};
    return client_size != 0 &&
        read_rtti_name(entity, client_base, client_size, type_name) &&
        std::strstr(type_name, "C_DOTA_Unit_Hero_") != nullptr;
}

bool entity_layout_matches_handle(std::uintptr_t entity_system, int index,
    std::uint32_t handle, std::size_t chunk_table_offset,
    std::size_t entry_stride) {
    if (!plausible_pointer(entity_system) || index < 0 || index > 0x7FFF ||
        chunk_table_offset == 0 || entry_stride < 0x18)
        return false;

    std::uintptr_t chunk = 0;
    if (!safe_read(entity_system + chunk_table_offset +
            sizeof(void*) * (static_cast<unsigned>(index) >> 9), chunk) ||
        !plausible_pointer(chunk))
        return false;

    const auto entry = chunk + entry_stride *
        (static_cast<unsigned>(index) & 0x1FF);
    std::uintptr_t entity = 0;
    std::uint32_t stored_handle = 0xFFFFFFFFu;
    return safe_read(entry, entity) && plausible_pointer(entity) &&
        safe_read(entry + 0x10, stored_handle) && stored_handle == handle;
}

/**
 * @brief Использует известный hero handle для проверки/калибровки layout таблицы; неверная
 * раскладка не должна становиться рабочим смещением.
 */
std::uintptr_t resolve_hero_handle_and_calibrate(std::uintptr_t entity_system,
    std::uint32_t handle) {
    const int index = static_cast<int>(handle & 0x7FFFu);
    const auto current_table = g_entity_chunk_table_offset.load(
        std::memory_order_acquire);
    const auto current_stride = g_entity_entry_stride.load(
        std::memory_order_acquire);
    if (entity_layout_matches_handle(entity_system, index, handle,
            current_table, current_stride)) {
        const auto current = entity_by_index_with_layout(entity_system, index,
            current_table, current_stride);
        if (looks_like_local_hero(current) || has_hero_rtti(current))
            return current;
    }

    // Source 2's entity storage is private and not exported through Schema.
    // Discover its two small layout parameters from the assigned-hero handle
    // and validate the candidate with live schema fields. This removes the
    // patch-specific 0x10/0x70 dependency that previously broke all handles.
    constexpr std::array<std::size_t, 5> table_candidates{
        0x10, 0x18, 0x20, 0x28, 0x30,
    };
    constexpr std::array<std::size_t, 9> stride_candidates{
        0x70, 0x78, 0x68, 0x80, 0x60, 0x88, 0x90, 0x58, 0x98,
    };
    for (const auto table_offset : table_candidates) {
        for (const auto stride : stride_candidates) {
            const auto candidate = entity_by_index_with_layout(
                entity_system, index, table_offset, stride);
            if (!entity_layout_matches_handle(entity_system, index, handle,
                    table_offset, stride) ||
                (!looks_like_local_hero(candidate) && !has_hero_rtti(candidate)))
                continue;
            g_entity_chunk_table_offset.store(table_offset,
                std::memory_order_release);
            g_entity_entry_stride.store(stride, std::memory_order_release);
            return candidate;
        }
    }
    return 0;
}

std::uintptr_t hero_from_controller(std::uintptr_t controller,
    std::uintptr_t entity_system) {
    std::uint32_t assigned_hero = 0xFFFFFFFFu;
    if (!plausible_pointer(controller) ||
        !safe_read(controller + schema_fields::controller_m_hAssignedHero, assigned_hero))
        return 0;
    if (assigned_hero == 0 || assigned_hero == 0xFFFFFFFFu)
        return 0;
    const auto hero = resolve_hero_handle_and_calibrate(entity_system, assigned_hero);
    return looks_like_local_hero(hero) || has_hero_rtti(hero) ? hero : 0;
}

std::uintptr_t controller_from_accessor() {
    // The accessor is a tiny wrapper over a RIP-relative controller array.
    // Reading slot zero directly avoids invoking game code from the polling
    // thread (some client builds only return a controller on their input
    // thread). The callable accessor remains a compatibility fallback.
    const auto controller_global = g_local_controller_global.load(
        std::memory_order_acquire);
    std::uintptr_t controller = 0;
    if (plausible_pointer(controller_global) &&
        safe_read(controller_global, controller) &&
        plausible_pointer(controller)) {
        g_local_controller_address.store(controller, std::memory_order_release);
        return controller;
    }

    const auto accessor = g_local_controller_accessor.load(std::memory_order_acquire);
    if (!plausible_pointer(accessor))
        return 0;
    using local_controller_fn = void* (*)(int);
    controller = reinterpret_cast<std::uintptr_t>(
        reinterpret_cast<local_controller_fn>(accessor)(0));
    if (plausible_pointer(controller)) {
        g_local_controller_address.store(controller, std::memory_order_release);
        return controller;
    }
    return 0;
}

int read_local_player_id() {
    auto controller = g_local_controller_address.load(std::memory_order_acquire);
    if (!plausible_pointer(controller))
        controller = controller_from_accessor();
    int player_id = -1;
    if (plausible_pointer(controller) &&
        safe_read(controller + schema_fields::controller_m_nPlayerID, player_id) &&
        player_id >= 0 && player_id < 64)
        return player_id;
    return -1;
}

bool local_hero_is_owned(std::uintptr_t hero) {
    if(!looks_like_local_hero(hero)||!has_hero_rtti(hero))return false;
    const int local_player=read_local_player_id();
    if(local_player<0)return false;
    int owner=-1;
    return (schema_fields::hero_m_iPlayerID>0&&
        safe_read(hero+schema_fields::hero_m_iPlayerID,owner)&&owner==local_player)||
        (schema_fields::m_nPlayerOwnerID>0&&
        safe_read(hero+schema_fields::m_nPlayerOwnerID,owner)&&owner==local_player);
}

/**
 * @brief Сводит доступные пути поиска локального героя и проверяет кандидата перед публикацией
 * в runtime.
 */
std::uintptr_t resolve_local_hero(std::uintptr_t client_base,
    std::size_t client_size, bool allow_controller_scan) {
    (void)client_size;

    std::uintptr_t entity_system = 0;
    if (!read_game_entity_system(client_base, entity_system))
        return 0;

    const auto controller = controller_from_accessor();
    // Prediction/input can follow the selected unit. It is not authoritative
    // for the local player's hero or the team used by FoW attribution.
    if (const auto hero = hero_from_controller(controller, entity_system); hero != 0)
        return hero;

    const auto pawn_global=g_local_player_pawn_global.load(std::memory_order_acquire);
    std::uintptr_t direct=0;
    if(safe_read(pawn_global,direct)&&local_hero_is_owned(direct))return direct;

    if (!allow_controller_scan)
        return 0;
    const int local_player_id = read_local_player_id();
    if (local_player_id < 0)
        return 0;

    std::uintptr_t resolved_hero = 0;
    visit_entities(entity_system, [&](std::uintptr_t candidate_controller, int) {
        int player_id = -1;
        if (!safe_read(candidate_controller + schema_fields::controller_m_nPlayerID,
                player_id) || player_id != local_player_id)
            return false;
        if (const auto hero = hero_from_controller(
                candidate_controller, entity_system); hero != 0) {
            g_local_controller_address.store(candidate_controller,
                std::memory_order_release);
            resolved_hero = hero;
        }
        return resolved_hero != 0;
    });
    return resolved_hero;
}

/**
 * @brief Проверяет видимость игровой карты независимо от фазы матча.
 */
bool read_world_view_active() {
    int state=-1;
    return safe_read(g_world_ui_state_address.load(std::memory_order_acquire),state)&&world_view_state::visible(state);
}

bool read_gameplay_session_active() {
    std::uintptr_t rules=0;
    if(!safe_read(g_game_rules_global.load(std::memory_order_acquire),rules)||!plausible_pointer(rules))return false;
    int state=0;
    if(schema_fields::game_rules_m_nGameState>0&&
        safe_read(rules+schema_fields::game_rules_m_nGameState,state)) {
        // DOTA_GameState, also recorded in the generated server SDK enum.
        constexpr int pre_game=4,game_in_progress=5;
        return state==pre_game||state==game_in_progress;
    }
    // Optional state metadata can be absent on a new game build. Require the
    // assigned controller hero then; an old cached/prediction pawn is insufficient.
    std::uintptr_t system=0;
    return read_game_entity_system(0,system)&&
        hero_from_controller(controller_from_accessor(),system)!=0;
}

/**
 * @brief Возвращает мировую позицию сущности; caller обязан проверить результат до проекции
 * или сопоставления маршрутов.
 */
bool read_world_position(std::uintptr_t entity, armlet_vec3& position) {
    std::uintptr_t scene_node = 0;
    return safe_read(entity + schema_fields::m_pGameSceneNode, scene_node) &&
        plausible_pointer(scene_node) &&
        safe_read(scene_node + schema_fields::m_vecAbsOrigin, position) &&
        std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

/**
 * @brief Получает видимость относительно локальной команды. Отсутствие данных о видимости не
 * равно подтверждённой невидимости.
 */
std::uintptr_t resolve_team_visibility_data(std::uintptr_t entity_system,
    std::uint8_t local_team) {
    if (schema_fields::m_bNPCVisibleState <= 0 ||
        (local_team != 2 && local_team != 3))
        return 0;

    static std::uintptr_t cached_data = 0;
    static std::uintptr_t cached_system = 0;
    static std::uint32_t cached_generation = UINT32_MAX;
    static std::uint8_t cached_team = 0;
    static ULONGLONG last_scan_at = 0;
    const auto client_base = g_client_base_address.load(std::memory_order_acquire);
    if(!plausible_pointer(client_base))return 0;
    const char* requested_type = local_team == 2
        ? "C_DOTA_DataRadiant" : "C_DOTA_DataDire";
    if(cached_system==entity_system&&cached_team==local_team&&plausible_pointer(cached_data)) {
        // Classify once, then validate the exact entity generation. Hundreds
        // of hero/particle visibility queries must not repeat RTTI traversal.
        if(cached_generation!=UINT32_MAX) {
            if(entity_by_handle(entity_system,cached_generation)==cached_data)return cached_data;
        }
    }
    const auto client_size=image_size(client_base);
    if(!client_size)return 0;
    if(cached_system==entity_system&&cached_team==local_team&&cached_generation==UINT32_MAX&&
        plausible_pointer(cached_data)&&rtti_name_contains(cached_data,client_base,client_size,requested_type))return cached_data;
    const ULONGLONG now = GetTickCount64();
    if (cached_system==entity_system&&cached_team == local_team && !plausible_pointer(cached_data) &&
        now - last_scan_at < 2000)
        return 0;

    cached_data = 0;
    cached_system=entity_system;cached_generation=UINT32_MAX;
    cached_team = local_team;
    last_scan_at = now;
    visit_entities(entity_system, [&](std::uintptr_t candidate, int index) {
        if (!rtti_name_contains(candidate, client_base, client_size,
                requested_type))
            return false;
        cached_data = candidate;
        std::uintptr_t identity=0;std::uint32_t handle=UINT32_MAX;
        if(schema_fields::m_pEntityIdentity>0&&
            safe_read(candidate+schema_fields::m_pEntityIdentity,identity)&&plausible_pointer(identity)&&
            safe_read(identity+0x10,handle)&&(handle&0x7FFFu)==static_cast<std::uint32_t>(index)&&
            entity_by_handle(entity_system,handle)==candidate)cached_generation=handle;
        return true;
    });
    return cached_data;
}

/**
 * @brief Получает видимость относительно локальной команды. Отсутствие данных о видимости не
 * равно подтверждённой невидимости.
 */
bool read_hero_hidden_from_local_team(std::uintptr_t entity_system,
    std::uintptr_t entity, int entity_index, std::uint8_t local_team,
    bool& hidden) {
    // Dota keeps hero entities networked while they are in fog, so the generic
    // scene-node dormant flag often never changes. Prefer the gameplay team
    // bitset to model/selection/culling state whenever the bitset is available.
    std::uint32_t team_visibility = 0;
    const bool model_visibility_valid = local_team < 32 &&
        schema_fields::m_iTeamVisibilityBitmask > 0 &&
        safe_read(entity + schema_fields::m_iTeamVisibilityBitmask,
            team_visibility);

    // m_iTeamVisibilityBitmask is legitimately zero for many Dota hero
    // entities. The team data entity carries the authoritative per-NPC bitset
    // used by the client's fog renderer and remains available while the hero
    // itself stays networked in fog.
    constexpr int npc_visibility_bit_count = 256 * 64;
    if (schema_fields::m_bNPCVisibleState>0&&entity_index >= 0 && entity_index < npc_visibility_bit_count) {
        const auto visibility_data = resolve_team_visibility_data(
            entity_system, local_team);
        std::uint64_t visible_word = 0;
        const auto word_index = static_cast<std::size_t>(entity_index) / 64;
        const auto bit_index = static_cast<unsigned>(entity_index) % 64;
        if (plausible_pointer(visibility_data) &&
            safe_read(visibility_data + schema_fields::m_bNPCVisibleState +
                    word_index * sizeof(visible_word), visible_word)) {
            hidden = (visible_word & (1ull << bit_index)) == 0;
            return true;
        }
    }

    // A zero model mask means no team currently renders this model. Prefer the
    // team bitset when available, but do not discard the enemy completely when
    // the per-team data entity is temporarily absent during reconnect/respawn.
    if (model_visibility_valid) {
        hidden = (team_visibility & (1u << local_team)) == 0;
        return true;
    }
    std::uintptr_t scene_node = 0;
    bool dormant = false;
    if (schema_fields::m_bDormant > 0 &&
        safe_read(entity + schema_fields::m_pGameSceneNode, scene_node) &&
        plausible_pointer(scene_node) &&
        safe_read(scene_node + schema_fields::m_bDormant, dormant)) {
        hidden = dormant;
        return true;
    }
    return false;
}

/**
 * @brief Сравнивает только младшие 15 бит для сопоставления raw handle с сохранённым индексом.
 * Не подходит для проверки идентичности сущности между повторными использованиями слота.
 */
bool same_entity_handle(std::uint32_t left, std::uint32_t right) {
    return (left & 0x7FFFu) == (right & 0x7FFFu);
}

void copy_designer_name(char (&destination)[96], const char* source) {
    destination[0] = '\0';
    if (source == nullptr)
        return;
    std::snprintf(destination, sizeof(destination), "%s", source);
}

void copy_detector_trigger(char (&destination)[64], const char* source) {
    if (source == nullptr || *source == '\0')
        return;
    std::snprintf(destination, sizeof(destination), "%s", source);
}

void describe_particle_trigger(
    const render_teleportation::particle_observation& observation,
    const char* source_designer_name, char (&output)[64]) {
    if ((observation.flags & render_teleportation::particle_flag_twin_gate) != 0) {
        std::snprintf(output, sizeof(output), "Twin Gate");
        return;
    }
    if ((observation.flags &
            render_teleportation::particle_flag_keen_conveyance) != 0) {
        std::snprintf(output, sizeof(output), "Keen Conveyance");
        return;
    }
    if ((observation.flags & render_teleportation::particle_flag_teleport) != 0) {
        std::snprintf(output, sizeof(output), "Town Portal Scroll");
        return;
    }

    const char* name = source_designer_name;
    if (name != nullptr && std::strncmp(name, "item_", 5) == 0) {
        name += 5;
    } else if (name != nullptr && std::strncmp(name, "ability_", 8) == 0) {
        name += 8;
    } else if (name == nullptr || *name == '\0' ||
        std::strncmp(name, "npc_dota_", 9) == 0 ||
        std::strncmp(name, "dota_", 5) == 0) {
        // Real ability entities are commonly named necrolyte_death_pulse,
        // never ability_*. Keep that exact parent when it is available.
        name = observation.debug_name;
    }
    if (name == nullptr || *name == '\0')
        return;
    if (const char* slash = std::strrchr(name, '/'); slash != nullptr)
        name = slash + 1;
    if (const char* slash = std::strrchr(name, '\\'); slash != nullptr)
        name = slash + 1;

    std::size_t write = 0;
    for (; name[write] != '\0' && name[write] != '.' &&
         write + 1 < sizeof(output); ++write) {
        const char value = name[write] == '_' ? ' ' : name[write];
        output[write] = value;
    }
    output[write] = '\0';
}

float planar_distance_squared(const armlet_vec3& left, const armlet_vec3& right) {
    const float dx = left.x - right.x;
    const float dy = left.y - right.y;
    return dx * dx + dy * dy;
}

void hero_abbreviation(const char* designer_name, char (&output)[4]) {
    output[0] = '?';
    output[1] = '?';
    output[2] = '\0';
    if (designer_name == nullptr)
        return;

    constexpr const char prefix[] = "npc_dota_hero_";
    const char* name = std::strncmp(designer_name, prefix, sizeof(prefix) - 1) == 0
        ? designer_name + sizeof(prefix) - 1 : designer_name;
    while (*name == '_')
        ++name;
    if (*name == '\0')
        return;

    output[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(*name)));
    const char* separator = std::strchr(name, '_');
    if (separator != nullptr && separator[1] != '\0')
        output[1] = static_cast<char>(std::toupper(
            static_cast<unsigned char>(separator[1])));
    else if (name[1] != '\0')
        output[1] = static_cast<char>(std::toupper(
            static_cast<unsigned char>(name[1])));
    else
        output[1] = output[0];
}

bool entity_is_hero(std::uintptr_t entity, char (&designer_name)[96]) {
    if (!plausible_pointer(entity))
        return false;
    if (read_designer_name(entity, designer_name) &&
        std::strncmp(designer_name, "npc_dota_hero_", 14) == 0 &&
        has_hero_rtti(entity))
        return true;
    designer_name[0] = '\0';
    return has_hero_rtti(entity);
}

/**
 * @brief Обновляет запись render trace в общем снимке. Вызывающая сторона должна удерживать
 * эксклюзивный render guard.
 */
void add_or_refresh_render_trace(std::uint32_t owner_handle,
    const armlet_vec3& position, const char* designer_name,
    const char* detector_trigger, ULONGLONG now,
    bool update_existing_position = true, const char* ability_icon = nullptr,
    bool particle_position = false, std::uintptr_t particle_effect = 0,
    std::uintptr_t particle_collection = 0, std::uint64_t source_event_key = 0) {
    const std::uint8_t priority = ability_icon!=nullptr && *ability_icon!='\0'
        ? (std::strncmp(ability_icon,"item_",5)==0 ? 3 : 2)
        : (detector_trigger!=nullptr && std::strcmp(detector_trigger,"Fog of War")!=0 ? 1 : 0);
    const ULONGLONG expires_at = now + static_cast<ULONGLONG>(
        std::clamp(g_render_trace_lifetime_seconds.load(std::memory_order_relaxed),
            3, 30)) * 1000ULL;

    std::size_t nearest = g_render_intelligence.trace_count;
    for (std::size_t index = 0; index < g_render_intelligence.trace_count; ++index) {
        auto& trace = g_render_intelligence.traces[index];
        if (trace.expires_at <= now)
            continue;
        if (same_entity_handle(trace.owner_handle, owner_handle)) {
            nearest = index;
            break;
        }
    }

    if (nearest < g_render_intelligence.trace_count) {
        auto& trace = g_render_intelligence.traces[nearest];
        // Stale replicated attack/cast coordinates cannot replace an effect point.
        if(trace.particle_position&&!particle_position)return;
        // Coalesce one impact's particle children. An identified item proc
        // wins over a simultaneous passive spell; a later event is independent.
        const auto anchor=trace.event_started_at?trace.event_started_at:trace.created_at;
        const auto delta=now>=anchor ? now-anchor : anchor-now;
        const bool same_event=source_event_key&&source_event_key==trace.source_event_key;
        const bool burst=same_event||((!source_event_key||!trace.source_event_key)&&delta<=150);
        if ((now<trace.created_at && delta>150) ||
            (priority<2 && priority<trace.source_priority) ||
            (burst && priority<trace.source_priority))
            return;
        // Equal-strength children annotate the same event, never choose its
        // position by callback order. A stronger item proc may replace it.
        if(burst && priority==trace.source_priority &&
            trace.particle_position && particle_position)return;
        if(!burst)trace.event_started_at=now;
        trace.source_event_key=source_event_key;
        // A particle event establishes the location of that event. Both world
        // and minimap markers use it rather than annotating an old lastpos.
        if (update_existing_position)
            trace.position = position;
        trace.created_at = std::max(trace.created_at,now);
        trace.expires_at = trace.created_at + (expires_at-now);
        trace.source_priority = priority;
        trace.particle_position = particle_position;
        trace.particle_effect = particle_effect;
        trace.particle_collection = particle_collection;
        trace.continuous = false;
        trace.particle_observed_at = particle_position ? now : 0;
        if (designer_name != nullptr && *designer_name != '\0')
            copy_designer_name(trace.designer_name, designer_name);
        copy_detector_trigger(trace.detector_trigger, detector_trigger);
        copy_designer_name(trace.ability_icon, ability_icon);
        return;
    }

    std::size_t slot = g_render_intelligence.trace_count;
    if (slot < g_render_intelligence.traces.size()) {
        ++g_render_intelligence.trace_count;
    } else {
        slot = 0;
        for (std::size_t index = 1; index < g_render_intelligence.trace_count; ++index) {
            if (g_render_intelligence.traces[index].expires_at <
                g_render_intelligence.traces[slot].expires_at)
                slot = index;
        }
    }

    auto& trace = g_render_intelligence.traces[slot];
    trace = {};
    trace.owner_handle = owner_handle;
    trace.position = position;
    trace.created_at = now;
    trace.event_started_at = now;
    trace.source_event_key = source_event_key;
    trace.expires_at = expires_at;
    trace.source_priority = priority;
    trace.particle_position = particle_position;
    trace.particle_effect = particle_effect;
    trace.particle_collection = particle_collection;
    trace.particle_observed_at = particle_position ? now : 0;
    copy_designer_name(trace.designer_name, designer_name);
    copy_detector_trigger(trace.detector_trigger, detector_trigger);
    copy_designer_name(trace.ability_icon, ability_icon);
}

/**
 * @brief Обновляет запись render trace в общем снимке. Вызывающая сторона должна удерживать
 * эксклюзивный render guard.
 */
bool update_persistent_render_trace(std::uint32_t owner_handle,
    const armlet_vec3& position,const char* ability_icon,ULONGLONG observed_at,
    std::uintptr_t effect, std::uintptr_t collection, bool caster_attached,
    bool active_continuous=false) {
    const std::uint8_t priority=ability_icon&&*ability_icon
        ? (std::strncmp(ability_icon,"item_",5)==0?3:2):1;
    for(std::size_t i=0;i<g_render_intelligence.trace_count;++i) {
        auto& trace=g_render_intelligence.traces[i];
        if(!same_entity_handle(trace.owner_handle,owner_handle)||trace.expires_at<=observed_at)continue;
        if(observed_at<trace.particle_observed_at)return true;
        // Body evidence is independent of the selected event/icon. A newer
        // caster emitter may move the body even when its icon has lower priority.
        if(caster_attached) {
            trace.position=position;
            trace.particle_observed_at=observed_at;
        }
        // A different lingering particle must never move another event's marker.
        if(trace.particle_effect && (trace.particle_effect!=effect ||
            trace.particle_collection!=collection)&&
            (!caster_attached||priority<=trace.source_priority))return true;
        if(!caster_attached&&!active_continuous)return true;
        if(priority<trace.source_priority || (priority==trace.source_priority &&
            trace.ability_icon[0]&&ability_icon&&std::strcmp(trace.ability_icon,ability_icon)!=0))return true;
        if(caster_attached)trace.position=position;
        trace.particle_position=true;
        trace.particle_effect=effect;trace.particle_collection=collection;
        if(ability_icon&&*ability_icon){copy_designer_name(trace.ability_icon,ability_icon);trace.source_priority=priority;}
        // Fresh emitter observations sustain an aura, without inventing casts.
        // Once they stop, its normal finite lifetime expires automatically.
        trace.continuous=true;
        trace.particle_observed_at=observed_at;
        trace.expires_at=observed_at+static_cast<ULONGLONG>(std::clamp(
            g_render_trace_lifetime_seconds.load(std::memory_order_relaxed),3,30))*1000;
        return true;
    }
    return false;
}

/**
 * @brief Удаляет больше не актуальные записи с уплотнением массива; teleport/trace count после
 * вызова задаёт допустимый диапазон индексов.
 */
void remove_render_traces_for_owner(std::uint32_t owner_handle) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < g_render_intelligence.trace_count; ++read) {
        if (same_entity_handle(
                g_render_intelligence.traces[read].owner_handle, owner_handle))
            continue;
        if (write != read)
            g_render_intelligence.traces[write] =
                g_render_intelligence.traces[read];
        ++write;
    }
    g_render_intelligence.trace_count = write;
}

/**
 * @brief Удаляет больше не актуальные записи с уплотнением массива; teleport/trace count после
 * вызова задаёт допустимый диапазон индексов.
 */
void prune_render_traces(ULONGLONG now) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < g_render_intelligence.trace_count; ++read) {
        if (g_render_intelligence.traces[read].expires_at <= now)
            continue;
        if (write != read)
            g_render_intelligence.traces[write] = g_render_intelligence.traces[read];
        ++write;
    }
    g_render_intelligence.trace_count = write;
}

bool position_is_near_neutral_farm(const armlet_vec3& position) {
    constexpr float neutral_distance_squared = 1050.0f * 1050.0f;
    for (std::size_t index = 0;
         index < g_render_intelligence.neutral_creep_count; ++index) {
        if (planar_distance_squared(position,
                g_render_intelligence.neutral_creeps[index].position) <=
            neutral_distance_squared)
            return true;
    }
    constexpr float camp_distance_squared = 900.0f * 900.0f;
    for (std::size_t index = 0;
         index < g_render_intelligence.camp_count; ++index) {
        if (planar_distance_squared(position,
                g_render_intelligence.camps[index].position) <=
            camp_distance_squared)
            return true;
    }
    return false;
}

bool read_enemy_ability_handles(std::uintptr_t hero,
    std::array<std::uint32_t, 32>& handles, std::size_t& count) {
    count = 0;
    if (!plausible_pointer(hero) || schema_fields::m_vecAbilities <= 0)
        return false;

    const auto vector = hero + schema_fields::m_vecAbilities;
    std::uintptr_t data = 0;
    int vector_size = 0;
    // C_NetworkUtlVectorBase: count, alignment padding, element pointer.
    // The enclosing field is resolved from live schema, not a patch offset.
    if (!safe_read(vector, vector_size) ||
        !safe_read(vector + sizeof(std::uint64_t), data) ||
        vector_size < 0 || vector_size > 64)
        return false;
    if (vector_size == 0)
        return true;
    if (!plausible_pointer(data))
        return false;

    count = std::min<std::size_t>(
        static_cast<std::size_t>(vector_size), handles.size());
    if (!safe_copy_memory(data, handles.data(),
            count * sizeof(handles.front()))) {
        count = 0;
        return false;
    }
    return true;
}

bool ability_name_is_trackable(const char* name) {
    if (name == nullptr || *name == '\0')
        return true;
    return std::strncmp(name, "special_bonus_", 14) != 0 &&
        std::strcmp(name, "attribute_bonus") != 0 &&
        std::strstr(name, "empty") == nullptr;
}

void describe_ability_trigger(const char* ability_name, char (&output)[64]) {
    constexpr char prefix[] = "Ability: ";
    std::snprintf(output, sizeof(output), "%s", prefix);
    std::size_t write = sizeof(prefix) - 1;
    const char* read = ability_name;
    if (read == nullptr || *read == '\0') {
        std::snprintf(output, sizeof(output), "Ability used");
        return;
    }
    if (std::strncmp(read, "ability_", 8) == 0)
        read += 8;
    for (; *read != '\0' && write + 1 < sizeof(output); ++read) {
        output[write++] = *read == '_' ? ' ' : *read;
    }
    output[write] = '\0';
}

bool recent_game_timestamp(float timestamp, float game_time) {
    if (!std::isfinite(timestamp) || timestamp <= 0.0f ||
        timestamp > 10000000.0f)
        return false;
    return !std::isfinite(game_time) || game_time < 0.0f ||
        (timestamp >= game_time - 2.5f && timestamp <= game_time + 0.5f);
}

bool read_hero_inventory_handles(std::uintptr_t hero,
    std::array<std::uint32_t,32>& handles, std::size_t& count);

// Ability and item identity/state share the same bounded cache. Preserve
// history only while an exact handle is still in the hero's live container.
void refresh_enemy_source_cache(std::uintptr_t entity_system,std::uintptr_t hero,
    enemy_visibility_entry& visibility,ULONGLONG now) {
    auto refresh=[&](auto& entries,std::size_t& count,ULONGLONG& discovered,bool items) {
        if(discovered && now>=discovered && now-discovered<250)return;
        std::array<std::uint32_t,32> handles{};std::size_t size=0;
        if(!(items?read_hero_inventory_handles(hero,handles,size):
            read_enemy_ability_handles(hero,handles,size)))return;
        const auto previous=entries;const auto previous_count=count;
        entries={};count=0;discovered=now;
        if(items){visibility.inventory_handles=handles;visibility.inventory_count=size;}
        for(std::size_t i=0;i<size;++i) {
            const auto handle=handles[i];
            if(!handle||handle==UINT32_MAX)continue;
            const auto entity=entity_by_handle(entity_system,handle);
            char name[96]{};
            if(!read_designer_name(entity,name)||!ability_name_is_trackable(name))continue;
            // m_bItemEnabled is not reliably replicated for hidden enemies.
            // Live session evidence: BF/Radiance emit particles while this
            // field is zero. Container membership proves source identity;
            // actual cast/particle/modifier activity proves use separately.
            if(items && std::strncmp(name,"item_",5)!=0)continue;
            // Duplicate inventory references are one source, not two events.
            bool duplicate=false;
            for(std::size_t j=0;j<count;++j)duplicate|=entries[j].entity_handle==handle;
            if(duplicate)continue;
            enemy_ability_entry candidate{};candidate.entity_handle=handle;
            for(std::size_t j=0;j<previous_count;++j)
                if(previous[j].entity_handle==handle){candidate=previous[j];break;}
            copy_designer_name(candidate.designer_name,name);
            if(count<entries.size())entries[count++]=candidate;
        }
    };
    refresh(visibility.abilities,visibility.ability_count,visibility.abilities_discovered_at,false);
    refresh(visibility.items,visibility.item_count,visibility.items_discovered_at,true);
}

/**
 * @brief Сопоставляет изменения реплицированного состояния способностей и предметов с событиями;
 * это резервный источник отметок помимо частиц.
 */
bool refresh_enemy_abilities(std::uintptr_t entity_system,
    std::uintptr_t hero, enemy_visibility_entry& visibility,
    bool hidden, const armlet_vec3* event_position,
    float game_time, ULONGLONG now) {
    refresh_enemy_source_cache(entity_system,hero,visibility,now);

    bool event_emitted = false;
    for (std::size_t index = 0; index < visibility.ability_count+visibility.item_count; ++index) {
        auto& state = index<visibility.ability_count?visibility.abilities[index]:
            visibility.items[index-visibility.ability_count];
        // TP channel lifecycle is already handled by the route detector.
        if(std::strcmp(state.designer_name,"item_tpscroll")==0)continue;
        const auto ability = entity_by_handle(entity_system, state.entity_handle);
        if (!plausible_pointer(ability))
            continue;

        // Do not filter by m_iLevel/m_iEnemyLevel here. Live bot/enemy
        // replication can keep both at zero while cast/cooldown fields still
        // advance. Unlearned abilities are naturally silent, so a level gate
        // only creates false negatives and adds no event validation.

        float cooldown_end = 0.0f;
        float cast_start = 0.0f;
        float channel_start = 0.0f;
        float last_click = 0.0f;
        bool in_phase = false;
        bool toggle_state = false;
        const bool cooldown_valid = schema_fields::m_fCooldown > 0 &&
            safe_read(ability + schema_fields::m_fCooldown, cooldown_end) &&
            std::isfinite(cooldown_end) && cooldown_end >= 0.0f &&
            cooldown_end < 10000000.0f;
        const bool cast_valid = schema_fields::m_flCastStartTime > 0 &&
            safe_read(ability + schema_fields::m_flCastStartTime, cast_start) &&
            std::isfinite(cast_start) && cast_start >= 0.0f &&
            cast_start < 10000000.0f;
        const bool channel_valid = schema_fields::m_flChannelStartTime > 0 &&
            safe_read(ability + schema_fields::m_flChannelStartTime,
                channel_start) && std::isfinite(channel_start) &&
            channel_start >= 0.0f && channel_start < 10000000.0f;
        const bool click_valid = schema_fields::m_flLastCastClickTime > 0 &&
            safe_read(ability + schema_fields::m_flLastCastClickTime,
                last_click) && std::isfinite(last_click) &&
            last_click >= 0.0f && last_click < 10000000.0f;
        const bool phase_valid = schema_fields::m_bInAbilityPhase > 0 &&
            safe_read(ability + schema_fields::m_bInAbilityPhase, in_phase);
        const bool toggle_valid = schema_fields::m_bToggleState > 0 &&
            safe_read(ability + schema_fields::m_bToggleState, toggle_state);

        bool cast_detected = false;
        if (state.initialized) {
            const bool cast_stamp_advanced = cast_valid &&
                cast_start > state.cast_start_time + 0.001f &&
                recent_game_timestamp(cast_start, game_time);
            const bool channel_stamp_advanced = channel_valid &&
                channel_start > state.channel_start_time + 0.001f &&
                recent_game_timestamp(channel_start, game_time);
            const bool click_stamp_advanced = click_valid &&
                last_click > state.last_cast_click_time + 0.001f &&
                recent_game_timestamp(last_click, game_time);
            const bool cooldown_started = cooldown_valid &&
                cooldown_end > state.cooldown_end + 0.05f;
            const bool phase_started = phase_valid && in_phase &&
                !state.in_ability_phase;
            const bool toggle_changed = toggle_valid &&
                toggle_state != state.toggle_state;
            cast_detected = cast_stamp_advanced || channel_stamp_advanced ||
                click_stamp_advanced || cooldown_started || phase_started ||
                toggle_changed;
        } else {
            // A newly granted/replaced ability (Rubick, Invoker, ability draft)
            // can first enter the vector during its cast. Baseline historical
            // cooldowns, but accept only a genuinely recent timestamp or a
            // currently active phase on that first observation.
            const bool game_clock_valid = std::isfinite(game_time) &&
                game_time >= 0.0f;
            cast_detected = (game_clock_valid &&
                ((cast_valid && recent_game_timestamp(cast_start, game_time)) ||
                 (channel_valid && recent_game_timestamp(
                    channel_start, game_time)) ||
                 (click_valid && recent_game_timestamp(last_click, game_time)))) ||
                (phase_valid && in_phase);
        }

        if (cooldown_valid)
            state.cooldown_end = cooldown_end;
        if (cast_valid)
            state.cast_start_time = cast_start;
        if (channel_valid)
            state.channel_start_time = channel_start;
        if (click_valid)
            state.last_cast_click_time = last_click;
        if (phase_valid)
            state.in_ability_phase = in_phase;
        if (toggle_valid)
            state.toggle_state = toggle_state;
        state.initialized = state.initialized || cooldown_valid || cast_valid ||
            channel_valid || click_valid || phase_valid || toggle_valid;
        state.last_observed_at = now;
        if(cast_detected)state.activated_at=now;

        if (!cast_detected || !hidden || event_position == nullptr ||
            (state.last_event_at != 0 && now - state.last_event_at < 750))
            continue;

        char trigger[64]{};
        describe_ability_trigger(state.designer_name, trigger);
        add_or_refresh_render_trace(visibility.entity_index, *event_position,
            visibility.designer_name, trigger, now, true, state.designer_name);
        state.last_event_at = now;
        event_emitted = true;
        g_render_diagnostics.fog_ability_events.fetch_add(
            1, std::memory_order_relaxed);
    }
    return event_emitted;
}

/**
 * @brief Объединяет наблюдение с маршрутом по владельцу, поколению каста и геометрии.
 * Confidence 1/2/3 задаёт приоритет частиц, резервных данных и modifier endpoints; вызов
 * требует render exclusive guard.
 */
void update_render_teleport(std::uint32_t owner_handle,
    const armlet_vec3& position, std::uint32_t flags,
    bool enemy_confirmed, const char* designer_name,
    std::uint32_t duration_ms, ULONGLONG now,
    std::uint32_t already_elapsed_ms,
    std::uint8_t endpoint_confidence,
    std::uint64_t cast_key, bool allow_create);
/**
 * @brief Отбрасывает NaN/Inf, координаты вне допустимых границ и почти нулевую
 * неинициализированную позицию до изменения маршрута.
 */
bool valid_teleport_endpoint(const armlet_vec3& position);

/**
 * @brief Обновляет кэш героев, переходы видимости и кандидатов перемещения. Кэш также
 * используется телепортами и атрибуцией render-событий.
 */
void refresh_enemy_visibility(std::uintptr_t entity_system, ULONGLONG now) {
    const bool trace_icons = g_render_trace_icons.load(std::memory_order_relaxed)||
        g_render_camp_status.load(std::memory_order_relaxed);
    const bool teleport_arrows = g_render_teleport_arrows.load(
        std::memory_order_relaxed);
    if (!trace_icons && !teleport_arrows) {
        g_render_intelligence.trace_count = 0;
        g_render_intelligence.enemy_visibility_count = 0;
        g_render_intelligence.enemy_visibility_refreshed_at = now;
        g_render_intelligence.enemy_visibility_discovered_at = 0;
        return;
    }
    if (now - g_render_intelligence.enemy_visibility_refreshed_at < 50)
        return;
    g_render_intelligence.enemy_visibility_refreshed_at = now;

    std::uint8_t local_team = 0;
    const auto local_hero = g_monitor.hero_address.load(std::memory_order_relaxed);
    if (!plausible_pointer(local_hero) ||
        !safe_read(local_hero + schema_fields::m_iTeamNum, local_team) ||
        local_team == 0)
        return;
    float game_time = -1.0f;
    read_game_time(g_client_base_address.load(std::memory_order_acquire),
        game_time);

    const auto process_enemy = [&](std::uintptr_t entity, int entity_index) {
        char designer_name[96]{};
        std::uint8_t team = 0;
        if (!entity_is_hero(entity, designer_name) ||
            !safe_read(entity + schema_fields::m_iTeamNum, team) ||
            (team != 2 && team != 3) || team == local_team)
            return false;

        std::size_t slot = g_render_intelligence.enemy_visibility_count;
        for (std::size_t index = 0;
             index < g_render_intelligence.enemy_visibility_count; ++index) {
            if (g_render_intelligence.enemy_visibility[index].entity_index ==
                static_cast<std::uint32_t>(entity_index)) {
                slot = index;
                break;
            }
        }
        if (slot == g_render_intelligence.enemy_visibility_count) {
            if (slot < g_render_intelligence.enemy_visibility.size()) {
                ++g_render_intelligence.enemy_visibility_count;
            } else {
                slot = 0;
                for (std::size_t index = 1;
                     index < g_render_intelligence.enemy_visibility_count; ++index) {
                    if (g_render_intelligence.enemy_visibility[index].last_observed_at <
                        g_render_intelligence.enemy_visibility[slot].last_observed_at)
                        slot = index;
                }
                remove_render_traces_for_owner(
                    g_render_intelligence.enemy_visibility[slot].entity_index);
            }
            g_render_intelligence.enemy_visibility[slot] = {};
            g_render_intelligence.enemy_visibility[slot].entity_index =
                static_cast<std::uint32_t>(entity_index);
            g_render_intelligence.enemy_visibility[slot].entity_address =
                entity;
        }

        auto& visibility = g_render_intelligence.enemy_visibility[slot];
        std::uintptr_t identity=0;std::uint32_t generation=UINT32_MAX;
        if(schema_fields::m_pEntityIdentity>0&&safe_read(entity+schema_fields::m_pEntityIdentity,identity)&&
            plausible_pointer(identity))safe_read(identity+0x10,generation);
        if(generation!=UINT32_MAX&&
            (!same_entity_handle(generation,static_cast<std::uint32_t>(entity_index))||
             entity_by_handle(entity_system,generation)!=entity))return false;
        const bool replaced_generation=visibility.entity_generation!=UINT32_MAX&&
            generation!=UINT32_MAX&&visibility.entity_generation!=generation;
        if (visibility.entity_address != 0 &&
            (visibility.entity_address != entity||replaced_generation)) {
            remove_render_traces_for_owner(visibility.entity_index);
            visibility = {};
            visibility.entity_index = static_cast<std::uint32_t>(entity_index);
            visibility.entity_address = entity;
        } else if (visibility.entity_address == 0) {
            visibility.entity_address = entity;
        }
        if(generation!=UINT32_MAX)visibility.entity_generation=generation;
        visibility.last_observed_at = now;
        visibility.team = team;
        if (designer_name[0] != '\0')
            copy_designer_name(visibility.designer_name, designer_name);
        int player_id = -1;
        if (schema_fields::hero_m_iPlayerID > 0)
            safe_read(entity + schema_fields::hero_m_iPlayerID, player_id);
        if ((player_id < 0 || player_id > 31) &&
            schema_fields::m_nPlayerOwnerID > 0)
            safe_read(entity + schema_fields::m_nPlayerOwnerID, player_id);
        if (player_id >= 0 && player_id <= 31)
            visibility.player_id = player_id;

        const armlet_vec3 previous_network_position =
            visibility.network_position;
        const ULONGLONG previous_position_observed_at =
            visibility.network_position_observed_at;
        const bool had_stability_anchor =
            visibility.has_network_position_stability_anchor;
        const armlet_vec3 previous_stability_anchor =
            visibility.network_position_stability_anchor;

        armlet_vec3 current_position{};
        const bool current_position_valid =
            read_world_position(entity, current_position);
        int current_health = -1;
        const bool current_health_valid = schema_fields::m_iHealth > 0 &&
            safe_read(entity + schema_fields::m_iHealth, current_health) &&
            current_health >= 0;
        if (current_position_valid) {
            visibility.network_position = current_position;
            visibility.has_network_position = true;
            visibility.network_position_observed_at = now;
            constexpr float stationary_radius_squared = 96.0f * 96.0f;
            if (!had_stability_anchor ||
                planar_distance_squared(previous_stability_anchor,
                    current_position) > stationary_radius_squared) {
                visibility.network_position_stability_anchor = current_position;
                visibility.has_network_position_stability_anchor = true;
                visibility.network_position_stable_since = now;
            } else if (visibility.network_position_stable_since == 0) {
                visibility.network_position_stable_since =
                    previous_position_observed_at != 0
                        ? previous_position_observed_at : now;
            }
        }
        if (current_health_valid)
            visibility.last_health = current_health;

        bool hidden = visibility.initialized ? visibility.dormant : false;
        const bool visibility_valid = read_hero_hidden_from_local_team(
            entity_system, entity, entity_index, local_team, hidden);
        constexpr ULONGLONG hidden_state_grace_ms = 250;
        const bool recently_confirmed_hidden = visibility.initialized &&
            visibility.dormant && visibility.visibility_confirmed_at != 0 &&
            now >= visibility.visibility_confirmed_at &&
            now - visibility.visibility_confirmed_at <= hidden_state_grace_ms;
        const bool hidden_event_context = hidden &&
            (visibility_valid || recently_confirmed_hidden);
        const bool disappeared = visibility_valid && visibility.initialized &&
            !visibility.dormant && hidden;

        // A network-origin discontinuity is not evidence of a channel. Do not
        // synthesize TP timers/routes after arrival (Blink/reveal also jump).
        visibility.has_pending_teleport_jump = false;

        if (!trace_icons) {
            if (visibility_valid) {
                visibility.dormant = hidden;
                visibility.initialized = true;
                visibility.visibility_confirmed_at = now;
            }
            return false;
        }

        float attack_time = 0.0f;
        float dealt_damage_time = 0.0f;
        float mana = -1.0f;
        const bool attack_valid = schema_fields::m_flLastAttackTime > 0 &&
            safe_read(entity + schema_fields::m_flLastAttackTime, attack_time) &&
            std::isfinite(attack_time) && attack_time > 0.0f;
        const bool dealt_valid = schema_fields::m_flLastDealtDamageTime > 0 &&
            safe_read(entity + schema_fields::m_flLastDealtDamageTime,
                dealt_damage_time) && std::isfinite(dealt_damage_time) &&
            dealt_damage_time > 0.0f;
        const bool mana_valid = schema_fields::m_flMana > 0 &&
            safe_read(entity + schema_fields::m_flMana, mana) &&
            std::isfinite(mana) && mana >= 0.0f;
        const bool attack_changed = visibility.activity_initialized && attack_valid&&
            attack_time>visibility.last_attack_time+.001f&&recent_game_timestamp(attack_time,game_time);
        if(attack_changed)visibility.last_attack_event_at=now;

        const armlet_vec3* activity_position = current_position_valid
            ? &current_position
            : visibility.has_network_position
                ? &visibility.network_position : nullptr;
        refresh_enemy_abilities(
            entity_system, entity, visibility, hidden_event_context,
            activity_position,
            game_time, now);

        if (!hidden) {
            if (current_position_valid) {
                visibility.last_visible_position = current_position;
                visibility.has_last_visible_position = true;
            }
            // A trail is historical fog information, never a second marker
            // attached to a hero that the player can currently see.
            remove_render_traces_for_owner(
                static_cast<std::uint32_t>(entity_index));
        } else if (disappeared && visibility.has_last_visible_position &&
                   (!current_health_valid || current_health > 0)) {
            // Freeze the last origin sampled while the hero was actually
            // visible. Once the team-visibility bit clears, Source 2 is free
            // to expose a stale, predicted, or newly hidden scene-node origin;
            // reading it here produced the large minimap jump seen after even
            // a one-frame step into fog.
            add_or_refresh_render_trace(static_cast<std::uint32_t>(entity_index),
                visibility.last_visible_position, visibility.designer_name,
                "Fog of War", now);
            g_render_diagnostics.visibility_transitions.fetch_add(
                1, std::memory_order_relaxed);
        }

        if (hidden_event_context && attack_changed) {
            if (activity_position != nullptr) {
                if (attack_changed &&
                    position_is_near_neutral_farm(*activity_position)) {
                    add_or_refresh_render_trace(
                        static_cast<std::uint32_t>(entity_index),
                        *activity_position, visibility.designer_name,
                        "Jungle farming", now);
                    visibility.last_attack_event_at = now;
                    g_render_diagnostics.fog_farming_events.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }

        if (attack_valid)
            visibility.last_attack_time = attack_time;
        if (dealt_valid)
            visibility.last_dealt_damage_time = dealt_damage_time;
        if (mana_valid)
            visibility.last_mana = mana;
        visibility.activity_initialized =
            visibility.activity_initialized || attack_valid || dealt_valid || mana_valid;

        if (visibility_valid) {
            visibility.dormant = hidden;
            visibility.initialized = true;
            visibility.visibility_confirmed_at = now;
        }
        return false;
    };

    // Hero handles are stable for a match. Discover the small enemy set every
    // two seconds, but sample those cached handles every 50 ms for the exact
    // visibility transition. This replaces a 32K-slot entity scan each tick.
    if (now - g_render_intelligence.enemy_visibility_discovered_at >= 2000) {
        g_render_intelligence.enemy_visibility_discovered_at = now;
        visit_entities(entity_system, [&](std::uintptr_t entity, int index) {
            return process_enemy(entity,index);
        });
    } else {
        std::array<std::uint32_t, 16> cached_handles{};
        const std::size_t cached_count =
            g_render_intelligence.enemy_visibility_count;
        for (std::size_t index = 0; index < cached_count; ++index)
            cached_handles[index] =
                g_render_intelligence.enemy_visibility[index].entity_index;
        for (std::size_t index = 0; index < cached_count; ++index) {
            const int entity_index = static_cast<int>(cached_handles[index]);
            process_enemy(entity_by_index(entity_system, entity_index),
                entity_index);
        }
    }

    std::size_t write = 0;
    for (std::size_t read = 0;
         read < g_render_intelligence.enemy_visibility_count; ++read) {
        auto& visibility = g_render_intelligence.enemy_visibility[read];
        // Removal/replacement is not a transition into fog. Retire historical
        // marks immediately once the calibrated live table loses this entity.
        const auto live_entity=entity_by_index(entity_system,
            static_cast<int>(visibility.entity_index));
        if (live_entity != visibility.entity_address ||
            (visibility.entity_generation!=UINT32_MAX&&
             entity_by_handle(entity_system,visibility.entity_generation)!=live_entity)||
            now - visibility.last_observed_at > 2500) {
            remove_render_traces_for_owner(visibility.entity_index);
            continue;
        }
        if (write != read)
            g_render_intelligence.enemy_visibility[write] = visibility;
        ++write;
    }
    g_render_intelligence.enemy_visibility_count = write;
    g_render_diagnostics.enemy_heroes.store(
        static_cast<int>(write), std::memory_order_relaxed);
}

/**
 * @brief Удаляет больше не актуальные записи с уплотнением массива; teleport/trace count после
 * вызова задаёт допустимый диапазон индексов.
 */
void prune_render_teleports(ULONGLONG now) {
    std::size_t write = 0;
    for (std::size_t read = 0;
         read < g_render_intelligence.teleport_count; ++read) {
        if (g_render_intelligence.teleports[read].expires_at <= now)
            continue;
        if (write != read)
            g_render_intelligence.teleports[write] =
                g_render_intelligence.teleports[read];
        ++write;
    }
    g_render_intelligence.teleport_count = write;
}

/**
 * @brief Проверяет transient cast item, затем обычные слоты. Такая последовательность
 * сохраняет обнаружение TP при переходном состоянии инвентаря.
 */
bool read_hero_inventory_handles(std::uintptr_t hero,
    std::array<std::uint32_t, 32>& handles, std::size_t& count) {
    count = 0;
    handles.fill(0xFFFFFFFFu);
    if (!plausible_pointer(hero) || schema_fields::m_Inventory <= 0 ||
        schema_fields::m_hItems <= 0)
        return false;
    const auto vector = hero + schema_fields::m_Inventory + schema_fields::m_hItems;
    // C_NetworkUtlVectorBase serializes its count followed by inline handles.
    // The live vector contains 25 slots; reading the count as a handle shifted
    // every inventory slot by one. Never walk beyond the replicated count.
    std::uint32_t size = 0;
    if (!safe_read(vector, size) || size > handles.size())
        return false;
    if (size != 0 && !safe_copy_memory(vector + sizeof(size), handles.data(),
            size * sizeof(handles[0])))
        return false;
    std::uint32_t final_size = 0;
    if (!safe_read(vector, final_size) || final_size != size)
        return false;
    count = size;
    return true;
}

std::uintptr_t find_hero_inventory_item(std::uintptr_t entity_system,
    std::uintptr_t hero, const char* requested_name,
    std::uint32_t* found_handle = nullptr) {
    if (!plausible_pointer(hero) || requested_name == nullptr)
        return 0;

    const auto match_handle = [&](std::uint32_t item_handle) -> std::uintptr_t {
        if (item_handle == 0 || item_handle == 0xFFFFFFFFu)
            return 0;
        const auto item = entity_by_handle(entity_system, item_handle);
        char item_name[96]{};
        if (!plausible_pointer(item))
            return 0;
        bool matches = read_designer_name(item, item_name) &&
            std::strcmp(item_name, requested_name) == 0;
        if (!matches) {
            const auto client_base = g_client_base_address.load(
                std::memory_order_acquire);
            const auto client_size = plausible_pointer(client_base)
                ? image_size(client_base) : 0;
            if (client_size != 0 &&
                std::strcmp(requested_name, "item_tpscroll") == 0) {
                matches = rtti_name_contains(item, client_base, client_size,
                    "C_DOTA_Item_TeleportScroll");
            }
        }
        if (!matches)
            return 0;
        if (found_handle != nullptr)
            *found_handle = item_handle;
        return item;
    };

    // A channelled item is temporarily mirrored here. Checking it first keeps
    // TP detection alive even while the regular inventory slot is transitioning.
    if (schema_fields::m_hTransientCastItem > 0) {
        std::uint32_t transient_handle = 0xFFFFFFFFu;
        if (safe_read(hero + schema_fields::m_Inventory +
                schema_fields::m_hTransientCastItem, transient_handle)) {
            if (const auto item = match_handle(transient_handle); item != 0)
                return item;
        }
    }

    std::array<std::uint32_t, 32> handles{};
    std::size_t count = 0;
    if (!read_hero_inventory_handles(hero, handles, count))
        return 0;
    for (std::size_t slot = 0; slot < count; ++slot) {
        const auto item_handle = handles[slot];
        if (item_handle == 0 || item_handle == 0xFFFFFFFFu)
            continue;
        if (const auto item = match_handle(item_handle); item != 0)
            return item;
    }
    return 0;
}

bool hero_has_inventory_item(std::uintptr_t entity_system,
    std::uintptr_t hero, const char* requested_name) {
    return find_hero_inventory_item(entity_system, hero, requested_name) != 0;
}

/**
 * @brief Проверяет/собирает массив указателей модификаторов с ограничением вместимости.
 * Отсутствие корректного массива обрабатывается как ошибка чтения.
 */
bool collect_active_modifiers(std::uintptr_t hero, std::uintptr_t client_base,
    std::size_t client_size, std::array<std::uintptr_t, 128>& modifiers,
    std::size_t& modifier_count);

struct active_teleport_snapshot {
    std::uint32_t duration_ms = 3000;
    std::uint32_t elapsed_ms = 0;
    std::uint64_t cast_key = 0;
    bool has_origin = false;
    bool has_destination = false;
    bool twin_gate = false;
    armlet_vec3 origin{};
    armlet_vec3 destination{};
};

/**
 * @brief Отбрасывает NaN/Inf, координаты вне допустимых границ и почти нулевую
 * неинициализированную позицию до изменения маршрута.
 */
bool valid_teleport_endpoint(const armlet_vec3& position) {
    return std::isfinite(position.x) && std::isfinite(position.y) &&
        std::isfinite(position.z) && std::abs(position.x) <= 20000.0f &&
        std::abs(position.y) <= 20000.0f && std::abs(position.z) <= 10000.0f &&
        (std::abs(position.x) > 1.0f || std::abs(position.y) > 1.0f ||
            std::abs(position.z) > 1.0f);
}

/**
 * @brief Собирает время, ключ каста и доступные endpoints активного TP-модификатора. game_time
 * нужен для elapsed_ms; отсутствие endpoint отражается отдельным has_* флагом.
 */
bool read_active_teleport_snapshot(std::uintptr_t hero, float game_time,
    active_teleport_snapshot& snapshot, bool* state_known = nullptr) {
    snapshot = {};
    if (state_known != nullptr) *state_known = false;
    const auto client_base = g_client_base_address.load(std::memory_order_acquire);
    const auto client_size = plausible_pointer(client_base)
        ? image_size(client_base) : 0;
    std::array<std::uintptr_t, 128> modifiers{};
    std::size_t modifier_count = 0;
    if (!plausible_pointer(hero) || client_size == 0 ||
        !collect_active_modifiers(hero, client_base, client_size,
            modifiers, modifier_count))
        return false;

    bool names_known = true;
    for (std::size_t index = 0; index < modifier_count; ++index) {
        char class_name[192]{};
        if (!read_rtti_name(modifiers[index], client_base, client_size,
                class_name)) {
            names_known = false;
            continue;
        }
        // Match the complete MSVC RTTI class token. A substring match also
        // accepted CDOTA_Modifier_Teleporting_Root_Logic and could keep a
        // phantom route alive after the real channel modifier disappeared.
        const bool standard_teleport = std::strstr(class_name,
            "CDOTA_Modifier_Teleporting@@") != nullptr;
        const bool twin_gate_channel = std::strstr(class_name,
            "CDOTA_Modifier_Twin_Gate_Warp_Channel@@") != nullptr;
        if (!standard_teleport && !twin_gate_channel)
            continue;
        snapshot.twin_gate = twin_gate_channel;

        float duration = 0.0f;
        bool duration_valid = standard_teleport &&
            schema_fields::teleport_modifier_m_fChannelTime > 0 &&
            safe_read(modifiers[index] +
                schema_fields::teleport_modifier_m_fChannelTime, duration) &&
            std::isfinite(duration) && duration >= 1.0f && duration <= 8.0f;
        if (!duration_valid) {
            duration_valid = safe_read(modifiers[index] +
                    private_modifier_layout::m_flDuration, duration) &&
                std::isfinite(duration) && duration >= 1.0f && duration <= 8.0f;
        }
        if (!duration_valid) {
            names_known = false;
            continue;
        }
        snapshot.duration_ms = static_cast<std::uint32_t>(
            std::lround(duration * 1000.0f));

        float creation_time = 0.0f;
        const bool creation_time_valid = safe_read(modifiers[index] +
                private_modifier_layout::m_flCreationTime, creation_time) &&
            std::isfinite(creation_time);
        std::uint32_t creation_bits = 0;
        if (creation_time_valid)
            std::memcpy(&creation_bits, &creation_time, sizeof(creation_bits));
        snapshot.cast_key =
            (static_cast<std::uint64_t>(modifiers[index]) >> 4) ^
            (static_cast<std::uint64_t>(creation_bits) << 32);
        if (snapshot.cast_key == 0)
            snapshot.cast_key = 1;
        if (std::isfinite(game_time) && game_time >= 0.0f &&
            creation_time_valid) {
            snapshot.elapsed_ms = static_cast<std::uint32_t>(std::lround(
                std::clamp(game_time - creation_time, 0.0f, duration) *
                1000.0f));
        }

        // The live schema exposes authoritative endpoints on the active TP
        // modifier. This is the primary route source; item state and particles
        // remain fallbacks for Twin Gate or temporary schema loss.
        if (standard_teleport &&
            schema_fields::teleport_modifier_m_vStart > 0 &&
            safe_read(modifiers[index] +
                schema_fields::teleport_modifier_m_vStart, snapshot.origin) &&
            valid_teleport_endpoint(snapshot.origin)) {
            snapshot.has_origin = true;
        }
        if (standard_teleport &&
            schema_fields::teleport_modifier_m_vEnd > 0 &&
            safe_read(modifiers[index] +
                schema_fields::teleport_modifier_m_vEnd, snapshot.destination) &&
            valid_teleport_endpoint(snapshot.destination) &&
            (!snapshot.has_origin || planar_distance_squared(
                snapshot.origin, snapshot.destination) > 4096.0f)) {
            snapshot.has_destination = true;
        }
        if (state_known != nullptr) *state_known = true;
        return true;
    }
    if (state_known != nullptr) *state_known = names_known;
    return false;
}

/**
 * @brief Объединяет наблюдение с маршрутом по владельцу, поколению каста и геометрии.
 * Confidence 1/2/3 задаёт приоритет частиц, резервных данных и modifier endpoints; вызов
 * требует render exclusive guard.
 */
void update_render_teleport(std::uint32_t owner_handle,
    const armlet_vec3& position, std::uint32_t flags,
    bool enemy_confirmed, const char* designer_name,
    std::uint32_t duration_ms, ULONGLONG now,
    std::uint32_t already_elapsed_ms = 0,
    std::uint8_t endpoint_confidence = 1,
    std::uint64_t cast_key = 0, bool allow_create = true) {
    if ((flags & render_teleportation::particle_flag_teleport) == 0 ||
        !valid_teleport_endpoint(position))
        return;

    const auto applied_duration = std::clamp(duration_ms, 1000u, 8000u);
    for (std::size_t i = 0; i < g_render_intelligence.teleport_count; ++i) {
        const auto& route = g_render_intelligence.teleports[i];
        if (route.channel_finished && route.expires_at > now &&
            same_entity_handle(route.owner_handle, owner_handle) &&
            (endpoint_confidence < 3 || cast_key == 0 || cast_key == route.cast_key))
            return;
    }
    const ULONGLONG observed_cast_start = already_elapsed_ms > 0
        ? now - std::min<ULONGLONG>(now,
            std::min<ULONGLONG>(already_elapsed_ms, applied_duration))
        : 0;
    const auto cast_generation_matches =
        [&](const render_teleport_entry& route) {
            if (cast_key != 0 && route.cast_key != 0)
                return cast_key == route.cast_key;
            if (cast_key != 0 && route.cast_key == 0 &&
                route.completed_at != 0)
                return false;
            if (cast_key != 0 && route.cast_key == 0 &&
                observed_cast_start != 0 && route.cast_started_at != 0) {
                const ULONGLONG delta = observed_cast_start >
                        route.cast_started_at
                    ? observed_cast_start - route.cast_started_at
                    : route.cast_started_at - observed_cast_start;
                return delta <= 750;
            }
            return true;
        };
    const auto route_accepts_updates = [&](const render_teleport_entry& route) {
        const ULONGLONG start = route.cast_started_at != 0
            ? route.cast_started_at : route.first_seen_at;
        const ULONGLONG active_duration = route.duration_ms != 0
            ? route.duration_ms : applied_duration;
        return !route.channel_finished && start != 0 && now >= start &&
            now - start <= active_duration + 350ULL;
    };
    const bool identified_hero = designer_name != nullptr &&
        std::strncmp(designer_name, "npc_dota_hero_", 14) == 0;
    const auto owner_is_compatible = [&](const render_teleport_entry& route) {
        // Spatial fallbacks may join an unowned/building endpoint to a hero,
        // but never join two known, different heroes at the same tower.
        return !identified_hero || route.designer_name[0] == '\0' ||
            same_entity_handle(route.owner_handle, owner_handle);
    };

    std::size_t slot = g_render_intelligence.teleport_count;
    for (std::size_t index = 0;
         index < g_render_intelligence.teleport_count; ++index) {
        auto& teleport = g_render_intelligence.teleports[index];
        if (owner_handle == 0 || owner_handle == 0xFFFFFFFFu ||
            !same_entity_handle(teleport.owner_handle, owner_handle) ||
            now < teleport.first_seen_at ||
            (teleport.expires_at != 0 && now > teleport.expires_at))
            continue;
        if (!cast_generation_matches(teleport))
            continue;
        // Unversioned particles may complete the currently active route, but
        // must never attach a second cast to the six-second display tail.
        if (cast_key == 0 && !route_accepts_updates(teleport))
            continue;
        if (slot == g_render_intelligence.teleport_count ||
            teleport.first_seen_at >
                g_render_intelligence.teleports[slot].first_seen_at)
            slot = index;
    }

    // Some destination effects are owned by the target building instead of
    // the casting hero. Pair that endpoint only when there is exactly one
    // recent incomplete opposite endpoint, avoiding ambiguous simultaneous TPs.
    bool created = false;
    if (slot == g_render_intelligence.teleport_count) {
        const bool updates_origin =
            (flags & render_teleportation::particle_flag_teleport_start) != 0;
        const bool updates_destination =
            (flags & render_teleportation::particle_flag_teleport_destination) != 0;
        const bool generic_endpoint = !updates_origin && !updates_destination;
        for (std::size_t index = 0;
             index < g_render_intelligence.teleport_count; ++index) {
            const auto& teleport = g_render_intelligence.teleports[index];
            if (now < teleport.first_seen_at ||
                !route_accepts_updates(teleport) ||
                !cast_generation_matches(teleport) ||
                !owner_is_compatible(teleport))
                continue;
            if ((updates_origin && teleport.has_origin &&
                    planar_distance_squared(teleport.origin, position) < 40000.0f) ||
                (updates_destination && teleport.has_destination &&
                    planar_distance_squared(teleport.destination, position) < 40000.0f) ||
                (generic_endpoint && teleport.has_destination &&
                    planar_distance_squared(teleport.destination, position) < 40000.0f)) {
                slot = index;
                break;
            }
        }
    }

    if (slot == g_render_intelligence.teleport_count) {
        std::size_t fallback = g_render_intelligence.teleport_count;
        int candidates = 0;
        const bool needs_origin =
            (flags & render_teleportation::particle_flag_teleport_start) != 0;
        const bool needs_destination =
            (flags & render_teleportation::particle_flag_teleport_destination) != 0;
        for (std::size_t index = 0;
             index < g_render_intelligence.teleport_count; ++index) {
            const auto& teleport = g_render_intelligence.teleports[index];
            if (now < teleport.first_seen_at ||
                !route_accepts_updates(teleport) ||
                !cast_generation_matches(teleport) ||
                !owner_is_compatible(teleport))
                continue;
            if ((needs_origin && !teleport.has_origin && teleport.has_destination) ||
                (needs_destination && !teleport.has_destination && teleport.has_origin) ||
                (!needs_origin && !needs_destination && teleport.has_origin &&
                    !teleport.has_destination &&
                    planar_distance_squared(teleport.origin, position) > 40000.0f)) {
                fallback = index;
                ++candidates;
            }
        }
        if (candidates == 1)
            slot = fallback;
    }

    if (slot == g_render_intelligence.teleport_count) {
        if (!allow_create)return;
        if (slot < g_render_intelligence.teleports.size()) {
            ++g_render_intelligence.teleport_count;
        } else {
            slot = 0;
            for (std::size_t index = 1;
                 index < g_render_intelligence.teleport_count; ++index) {
                if (g_render_intelligence.teleports[index].expires_at <
                    g_render_intelligence.teleports[slot].expires_at)
                    slot = index;
            }
        }
        g_render_intelligence.teleports[slot] = {};
        g_render_intelligence.teleports[slot].owner_handle = owner_handle;
        g_render_intelligence.teleports[slot].cast_key = cast_key;
        g_render_intelligence.teleports[slot].first_seen_at = now;
        g_render_intelligence.teleports[slot].cast_started_at =
            observed_cast_start != 0 ? observed_cast_start : now;
        created = true;
    }

    auto& teleport = g_render_intelligence.teleports[slot];
    if (endpoint_confidence >= 3)
        teleport.channel_observed = true;
    if (identified_hero && teleport.designer_name[0] == '\0')
        teleport.owner_handle = owner_handle;
    if (teleport.cast_key == 0 && cast_key != 0)
        teleport.cast_key = cast_key;
    const bool was_enemy_confirmed = teleport.enemy_confirmed;
    teleport.enemy_confirmed = teleport.enemy_confirmed || enemy_confirmed;
    teleport.effect_flags |= flags;
    const bool authoritative_timing = created ||
        endpoint_confidence >= teleport.timing_confidence;
    if (authoritative_timing) {
        teleport.duration_ms = applied_duration;
        teleport.timing_confidence = endpoint_confidence;
    }
    if (authoritative_timing && enemy_confirmed && already_elapsed_ms > 0) {
        const ULONGLONG elapsed = std::min<ULONGLONG>(
            already_elapsed_ms, teleport.duration_ms);
        const ULONGLONG measured_start = now - std::min(now, elapsed);
        teleport.first_seen_at = std::min(
            teleport.first_seen_at, measured_start);
        if (teleport.cast_started_at == 0 || cast_key == 0 ||
            teleport.cast_key == cast_key)
            teleport.cast_started_at = measured_start;
    }
    if (designer_name != nullptr && *designer_name != '\0')
        copy_designer_name(teleport.designer_name, designer_name);

    const bool explicit_origin =
        (flags & render_teleportation::particle_flag_teleport_start) != 0;
    const bool explicit_destination =
        (flags & render_teleportation::particle_flag_teleport_destination) != 0;
    const bool active_channel = route_accepts_updates(teleport);
    if (explicit_origin && (!teleport.has_origin ||
            endpoint_confidence > teleport.origin_confidence ||
            (active_channel && endpoint_confidence >= 3 &&
                endpoint_confidence == teleport.origin_confidence))) {
        teleport.origin = position;
        teleport.has_origin = true;
        teleport.origin_confidence = endpoint_confidence;
    }
    if (explicit_destination &&
        (!teleport.has_destination || endpoint_confidence >
            teleport.destination_confidence ||
            (active_channel && endpoint_confidence ==
                teleport.destination_confidence)) &&
        (!teleport.has_origin || planar_distance_squared(
            teleport.origin, position) > 4096.0f)) {
        teleport.destination = position;
        teleport.has_destination = true;
        teleport.destination_confidence = endpoint_confidence;
    }
    if (!explicit_origin && !explicit_destination) {
        // Twin Gate and some Keen Conveyance effects use a generic particle
        // name. The first stable point is the source; a later remote point is
        // the destination. Repeated particles at the same point do not flip it.
        if (teleport.has_origin && !teleport.has_destination &&
            enemy_confirmed && !was_enemy_confirmed &&
            planar_distance_squared(teleport.origin, position) > 40000.0f) {
            // A building-owned destination particle can arrive before the
            // hero-owned channel particle. Once the enemy hero resolves,
            // correct the earlier provisional point into the destination.
            teleport.destination = teleport.origin;
            teleport.has_destination = true;
            teleport.destination_confidence = teleport.origin_confidence;
            teleport.origin = position;
            teleport.origin_confidence = endpoint_confidence;
        } else if (!teleport.has_origin) {
            teleport.origin = position;
            teleport.has_origin = true;
            teleport.origin_confidence = endpoint_confidence;
        } else if (!teleport.has_destination &&
            planar_distance_squared(teleport.origin, position) > 40000.0f) {
            teleport.destination = position;
            teleport.has_destination = true;
            teleport.destination_confidence = endpoint_confidence;
        }
    }

    if (teleport.enemy_confirmed && teleport.has_origin &&
        !teleport.debug_origin_reported) {
        char message[256]{};
        std::snprintf(message, sizeof(message),
            "Enemy teleport origin: hero=%s pos=(%.0f, %.0f), duration=%u ms.",
            teleport.designer_name[0] != '\0' ? teleport.designer_name : "unknown",
            teleport.origin.x, teleport.origin.y, teleport.duration_ms);
        log_message("TP", message);
        teleport.debug_origin_reported = true;
        g_render_diagnostics.teleport_origins.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (teleport.enemy_confirmed && teleport.has_origin &&
        teleport.has_destination && !teleport.debug_complete_reported) {
        char message[320]{};
        std::snprintf(message, sizeof(message),
            "Enemy teleport route complete: hero=%s from=(%.0f, %.0f) to=(%.0f, %.0f), duration=%u ms.",
            teleport.designer_name[0] != '\0' ? teleport.designer_name : "unknown",
            teleport.origin.x, teleport.origin.y,
            teleport.destination.x, teleport.destination.y,
            teleport.duration_ms);
        log_message("TP", message);
        teleport.debug_complete_reported = true;
        teleport.completed_at = now;
        g_render_diagnostics.teleport_routes.fetch_add(
            1, std::memory_order_relaxed);
    }

    const ULONGLONG animation_end = teleport.cast_started_at +
        static_cast<ULONGLONG>(teleport.duration_ms);
    teleport.expires_at = teleport.completed_at != 0
        ? std::max(animation_end, teleport.completed_at) + 6000ULL
        : std::max(now + 350ULL, animation_end + 350ULL);
}

/**
 * @brief Обновляет ALT-строку и routes по кэшу вражеских героев, модификатору и TP-предмету;
 * внутренний throttle отделён от частоты worker polling.
 */
void reconcile_teleport_channel(std::uint32_t owner_handle, bool active,
    bool state_known, const armlet_vec3* hero_position, ULONGLONG now) {
    if (active || !state_known)
        return;
    for (std::size_t i = 0; i < g_render_intelligence.teleport_count; ++i) {
        auto& route = g_render_intelligence.teleports[i];
        if (!same_entity_handle(route.owner_handle, owner_handle) ||
            !route.channel_observed || route.channel_finished)
            continue;
        const bool arrived = hero_position != nullptr && route.has_destination &&
            valid_teleport_endpoint(*hero_position) &&
            planar_distance_squared(*hero_position, route.destination) <= 40000.0f;
        route.channel_finished = true;
        route.canceled = !arrived;
        // Keep an invisible tombstone so late particles cannot resurrect a
        // canceled channel. A new authoritative cast key is accepted at once.
        route.expires_at = now + (arrived ? 450ULL : 1500ULL);
    }
}

void refresh_enemy_teleport_statuses(
    std::uintptr_t entity_system, ULONGLONG now) {
    if (now - g_render_intelligence.enemy_teleports_refreshed_at < 100)
        return;
    g_render_intelligence.enemy_teleports_refreshed_at = now;
    g_render_intelligence.enemy_teleport_count = 0;

    std::uint8_t local_team = 0;
    const auto local_hero = g_monitor.hero_address.load(std::memory_order_relaxed);
    if (!plausible_pointer(local_hero) ||
        !safe_read(local_hero + schema_fields::m_iTeamNum, local_team) ||
        local_team == 0)
        return;

    float game_time = -1.0f;
    const bool game_time_valid = read_game_time(
        g_client_base_address.load(std::memory_order_acquire), game_time);

    const auto process_enemy_hero = [&](enemy_visibility_entry& visibility, std::uintptr_t hero,
        std::uint32_t hero_handle, int player_id, std::uint8_t team,
        const char* hero_name) {
        enemy_teleport_status_entry candidate{};
        candidate.player_id = player_id;
        candidate.team = team;
        candidate.cooldown_observed_at = now;
        candidate.visible=visibility.initialized&&!visibility.dormant;
        if(schema_fields::m_iHealth>0)safe_read(hero+schema_fields::m_iHealth,candidate.health);
        if(schema_fields::m_iMaxHealth>0)safe_read(hero+schema_fields::m_iMaxHealth,candidate.max_health);
        hero_abbreviation(hero_name, candidate.hero_abbreviation);

        const bool teleport_arrows = g_render_teleport_arrows.load(
            std::memory_order_relaxed);
        active_teleport_snapshot modifier_teleport{};
        bool channel_state_known = false;
        const bool active_teleport_modifier = teleport_arrows &&
            read_active_teleport_snapshot(hero,
                game_time_valid ? game_time : -1.0f, modifier_teleport,
                &channel_state_known);
        armlet_vec3 current_position{};
        const bool position_known = read_world_position(hero, current_position);
        reconcile_teleport_channel(hero_handle, active_teleport_modifier,
            channel_state_known, position_known ? &current_position : nullptr, now);
        const std::uint32_t modifier_duration_ms =
            modifier_teleport.duration_ms;
        const std::uint32_t modifier_elapsed_ms =
            modifier_teleport.elapsed_ms;

        // Primary path: the live teleport modifier contains both exact
        // endpoints. Publish them before consulting item/particle fallbacks;
        // endpoint confidence prevents weaker observations from replacing
        // these values later in the same channel.
        if (active_teleport_modifier) {
            constexpr std::uint32_t teleport_flag =
                render_teleportation::particle_flag_teleport;
            constexpr std::uint8_t structured_endpoint_confidence = 3;
            if (modifier_teleport.has_origin) {
                update_render_teleport(hero_handle, modifier_teleport.origin,
                    teleport_flag |
                        render_teleportation::particle_flag_teleport_start,
                    true, hero_name, modifier_duration_ms, now,
                    modifier_elapsed_ms, structured_endpoint_confidence,
                    modifier_teleport.cast_key);
            }
            if (modifier_teleport.has_destination) {
                update_render_teleport(hero_handle,
                    modifier_teleport.destination,
                    teleport_flag |
                        render_teleportation::particle_flag_teleport_destination,
                    true, hero_name, modifier_duration_ms, now,
                    modifier_elapsed_ms, structured_endpoint_confidence,
                    modifier_teleport.cast_key);
            }
        }

        std::uint32_t scroll_handle = UINT32_MAX;
        const auto scroll = find_hero_inventory_item(entity_system, hero,
            "item_tpscroll", &scroll_handle);
        std::array<std::uint32_t, 32> inventory_handles{};
        std::size_t inventory_count = 0;
        candidate.inventory_known = read_hero_inventory_handles(hero,
            inventory_handles, inventory_count);
        if(candidate.inventory_known) {
            visibility.inventory_handles=inventory_handles;
            visibility.inventory_count=inventory_count;
        }
        if (plausible_pointer(scroll)) {
            float cooldown = 0.0f;
            int charges = -1;
            if (!safe_read(scroll + schema_fields::m_fCooldown, cooldown) ||
                !std::isfinite(cooldown) || cooldown < 0.0f)
                cooldown = -1.0f;
            // Live TP inspection confirms m_fCooldown is remaining duration.
            // An unchanged sample in fog is stale, not a new full countdown.
            cooldown = visibility.scroll_cooldown.observe(scroll_handle, cooldown,
                game_time_valid ? game_time : -1.0f, !visibility.dormant);
            if(visibility.dormant&&game_time_valid) {
                for(std::size_t t=0;t<g_render_intelligence.teleport_count;++t) {
                    const auto& route=g_render_intelligence.teleports[t];
                    if(!route.enemy_confirmed||!route.has_origin||
                        !same_entity_handle(route.owner_handle,hero_handle)||
                        (route.effect_flags&(render_teleportation::particle_flag_twin_gate|
                            render_teleportation::particle_flag_keen_conveyance))||
                        now<route.cast_started_at||now-route.cast_started_at>10000)continue;
                    // Defaults verified in installed scripts/npc/items.txt;
                    // live cooldown length takes precedence (reductions/custom rules).
                    float duration=hero_has_inventory_item(entity_system,hero,"item_travel_boots")||
                        hero_has_inventory_item(entity_system,hero,"item_travel_boots_2")?40.f:80.f;
                    float replicated_length=0;
                    if(schema_fields::m_flCooldownLength>0&&safe_read(scroll+
                        schema_fields::m_flCooldownLength,replicated_length)&&
                        std::isfinite(replicated_length)&&replicated_length>0&&replicated_length<=300)
                        duration=replicated_length;
                    visibility.scroll_cooldown.infer_cast(route.first_seen_at,
                        game_time-float(now-route.cast_started_at)/1000.f,duration);
                }
                if(visibility.scroll_cooldown.initialized)
                    cooldown=std::max(0.f,visibility.scroll_cooldown.deadline-game_time);
            }
            if (schema_fields::m_iCurrentCharges <= 0 ||
                !safe_read(scroll + schema_fields::m_iCurrentCharges, charges) ||
                charges < 0)
                charges = -1;
            candidate.has_scroll = true;
            candidate.inventory_known = true;
            candidate.cooldown_seconds = cooldown;
            candidate.cooldown_estimated=visibility.scroll_cooldown.estimated&&cooldown>0;
            candidate.charges = charges;

            // Town Portal Scroll publishes exact source/destination state on
            // the item itself. This path remains available when Valve changes
            // the particle update function or the destination effect is owned
            // by a tower rather than by the enemy hero.
            if (teleport_arrows && (active_teleport_modifier || !channel_state_known) &&
                schema_fields::teleport_m_vDestination > 0) {
                float channel_start = 0.0f;
                float cast_start = 0.0f;
                float base_time = 3.0f;
                float extra_time = 0.0f;
                bool in_ability_phase = false;
                if (schema_fields::m_bInAbilityPhase > 0)
                    safe_read(scroll + schema_fields::m_bInAbilityPhase,
                        in_ability_phase);
                if (schema_fields::teleport_m_flBaseTime > 0)
                    safe_read(scroll + schema_fields::teleport_m_flBaseTime,
                        base_time);
                if (schema_fields::teleport_m_flExtraTime > 0)
                    safe_read(scroll + schema_fields::teleport_m_flExtraTime,
                        extra_time);
                if (!std::isfinite(base_time) || base_time < 1.0f ||
                    base_time > 8.0f)
                    base_time = 3.0f;
                if (!std::isfinite(extra_time) || extra_time < 0.0f ||
                    extra_time > 5.0f)
                    extra_time = 0.0f;
                float duration = std::clamp(base_time + extra_time, 1.0f, 8.0f);
                if (active_teleport_modifier) {
                    duration = static_cast<float>(modifier_duration_ms) / 1000.0f;
                } else if (hero_has_inventory_item(
                        entity_system, hero, "item_travel_boots_2"))
                    duration = 2.0f;

                const bool channel_start_valid =
                    schema_fields::m_flChannelStartTime > 0 &&
                    safe_read(scroll + schema_fields::m_flChannelStartTime,
                        channel_start) && std::isfinite(channel_start);
                const bool cast_start_valid =
                    schema_fields::m_flCastStartTime > 0 &&
                    safe_read(scroll + schema_fields::m_flCastStartTime,
                        cast_start) && std::isfinite(cast_start);
                if (active_teleport_modifier || channel_start_valid ||
                    cast_start_valid || in_ability_phase) {
                    const float replicated_start = std::max(
                        channel_start_valid ? channel_start : 0.0f,
                        cast_start_valid ? cast_start : 0.0f);
                    std::uint64_t item_cast_key = active_teleport_modifier
                        ? modifier_teleport.cast_key : 0;
                    if (item_cast_key == 0 && replicated_start > 0.0f) {
                        std::uint32_t start_bits = 0;
                        std::memcpy(&start_bits, &replicated_start,
                            sizeof(start_bits));
                        item_cast_key =
                            (static_cast<std::uint64_t>(scroll) >> 4) ^
                            (static_cast<std::uint64_t>(start_bits) << 32);
                        if (item_cast_key == 0)
                            item_cast_key = 1;
                    }
                    const float elapsed = active_teleport_modifier
                        ? static_cast<float>(modifier_elapsed_ms) / 1000.0f
                        : game_time_valid && replicated_start > 0.0f
                            ? game_time - replicated_start : 0.0f;
                    int fx_caster = -1;
                    int fx_origin = -1;
                    int fx_destination = -1;
                    if (schema_fields::teleport_m_nFxCaster > 0)
                        safe_read(scroll + schema_fields::teleport_m_nFxCaster,
                            fx_caster);
                    if (schema_fields::teleport_m_nFxOrigin > 0)
                        safe_read(scroll + schema_fields::teleport_m_nFxOrigin,
                            fx_origin);
                    if (schema_fields::teleport_m_nFxDestination > 0)
                        safe_read(scroll + schema_fields::teleport_m_nFxDestination,
                            fx_destination);
                    const bool has_active_fx = fx_caster > 0 || fx_origin > 0 ||
                        fx_destination > 0;

                    // The time window filters out the previous cast timestamp;
                    // FX/ability-phase state tolerates the first replicated frame.
                    if ((active_teleport_modifier ||
                            (game_time_valid && replicated_start > 0.0f) ||
                            has_active_fx || in_ability_phase) &&
                        elapsed >= -0.35f &&
                        elapsed <= duration + 0.15f &&
                        (has_active_fx || in_ability_phase || elapsed >= 0.0f)) {
                        armlet_vec3 origin{};
                        armlet_vec3 destination{};
                        bool has_origin = read_world_position(hero, origin);
                        bool has_destination = safe_read(scroll +
                            schema_fields::teleport_m_vDestination, destination) &&
                            std::isfinite(destination.x) &&
                            std::isfinite(destination.y) &&
                            std::isfinite(destination.z) &&
                            std::abs(destination.x) <= 20000.0f &&
                            std::abs(destination.y) <= 20000.0f &&
                            (std::abs(destination.x) > 64.0f ||
                                std::abs(destination.y) > 64.0f) &&
                            (!has_origin || planar_distance_squared(
                                origin, destination) > 40000.0f);

                        if (!has_destination &&
                            schema_fields::teleport_m_hTarget > 0) {
                            std::uint32_t target_handle = 0xFFFFFFFFu;
                            if (safe_read(scroll + schema_fields::teleport_m_hTarget,
                                    target_handle)) {
                                const auto target = entity_by_handle(
                                    entity_system, target_handle);
                                has_destination = plausible_pointer(target) &&
                                    read_world_position(target, destination);
                            }
                        }
                        if (active_teleport_modifier && !has_destination) {
                            static ULONGLONG next_missing_destination_log = 0;
                            if (now >= next_missing_destination_log) {
                                log_message("TP",
                                    "Active enemy teleport has no replicated destination yet.");
                                next_missing_destination_log = now + 2000;
                            }
                        }

                        const auto duration_ms = static_cast<std::uint32_t>(
                            std::lround(duration * 1000.0f));
                        const auto elapsed_ms = static_cast<std::uint32_t>(
                            std::lround(std::clamp(elapsed, 0.0f, duration) *
                                1000.0f));
                        constexpr std::uint32_t teleport_flag =
                            render_teleportation::particle_flag_teleport;
                        if (has_origin)
                            update_render_teleport(hero_handle, origin,
                                teleport_flag |
                                    render_teleportation::particle_flag_teleport_start,
                                true, hero_name, duration_ms, now, elapsed_ms, 2,
                                item_cast_key);
                        if (has_destination)
                            update_render_teleport(hero_handle, destination,
                                teleport_flag |
                                    render_teleportation::particle_flag_teleport_destination,
                                true, hero_name, duration_ms, now, elapsed_ms, 2,
                                item_cast_key);
                    }
                }
            }
        }

        // The transient TP item and its destination fields are not guaranteed
        // to replicate for an enemy in fog. The channel modifier, however, is
        // attached to the hero, while the destination particle is often owned
        // by the target tower. Join those two sources so a route does not
        // disappear merely because find_hero_inventory_item() returned null.
        if (teleport_arrows && active_teleport_modifier) {
                armlet_vec3 origin = modifier_teleport.origin;
                armlet_vec3 destination = modifier_teleport.destination;
                bool has_origin = modifier_teleport.has_origin;
                bool has_destination = modifier_teleport.has_destination;
                std::uint8_t origin_confidence = has_origin ? 3 : 0;
                std::uint8_t destination_confidence =
                    has_destination ? 3 : 0;
                if (!has_origin && read_world_position(hero, origin)) {
                    has_origin = true;
                    origin_confidence = 2;
                }

                if (!has_destination && plausible_pointer(scroll) &&
                    schema_fields::teleport_m_vDestination > 0) {
                    has_destination = safe_read(scroll +
                            schema_fields::teleport_m_vDestination,
                            destination) &&
                        std::isfinite(destination.x) &&
                        std::isfinite(destination.y) &&
                        std::isfinite(destination.z) &&
                        std::abs(destination.x) <= 20000.0f &&
                        std::abs(destination.y) <= 20000.0f &&
                        (std::abs(destination.x) > 64.0f ||
                            std::abs(destination.y) > 64.0f) &&
                        (!has_origin || planar_distance_squared(
                            origin, destination) > 40000.0f);
                    if (has_destination)
                        destination_confidence = 2;
                }
                if (!has_destination && plausible_pointer(scroll) &&
                    schema_fields::teleport_m_hTarget > 0) {
                    std::uint32_t target_handle = 0xFFFFFFFFu;
                    if (safe_read(scroll + schema_fields::teleport_m_hTarget,
                            target_handle)) {
                        const auto target = entity_by_handle(
                            entity_system, target_handle);
                        has_destination = plausible_pointer(target) &&
                            read_world_position(target, destination) &&
                            (!has_origin || planar_distance_squared(
                                origin, destination) > 40000.0f);
                        if (has_destination)
                            destination_confidence = 2;
                    }
                }

                // Match one recent remote teleport endpoint. Multiple
                // particles at the same target form one cluster; distinct
                // simultaneous destinations remain ambiguous and are ignored.
                if (!has_destination && has_origin) {
                    armlet_vec3 particle_destination{};
                    bool particle_candidate = false;
                    bool particle_ambiguous = false;
                    for (std::size_t teleport_index = 0;
                         teleport_index < g_render_intelligence.teleport_count;
                         ++teleport_index) {
                        const auto& observed =
                            g_render_intelligence.teleports[teleport_index];
                        if (now < observed.first_seen_at ||
                            now - observed.first_seen_at > 4500 ||
                            (observed.designer_name[0] != '\0' &&
                                !same_entity_handle(observed.owner_handle,
                                    hero_handle)) ||
                            (observed.cast_key != 0 && observed.cast_key !=
                                modifier_teleport.cast_key))
                            continue;

                        armlet_vec3 endpoint{};
                        bool has_endpoint = false;
                        if (observed.has_destination) {
                            endpoint = observed.destination;
                            has_endpoint = true;
                        } else if (observed.has_origin &&
                            !same_entity_handle(observed.owner_handle,
                                hero_handle)) {
                            endpoint = observed.origin;
                            has_endpoint = true;
                        }
                        if (!has_endpoint || planar_distance_squared(
                                origin, endpoint) <= 4096.0f)
                            continue;
                        if (!particle_candidate) {
                            particle_destination = endpoint;
                            particle_candidate = true;
                        } else if (planar_distance_squared(
                                particle_destination, endpoint) > 40000.0f) {
                            particle_ambiguous = true;
                            break;
                        }
                    }
                    if (particle_candidate && !particle_ambiguous) {
                        destination = particle_destination;
                        has_destination = true;
                        destination_confidence = 1;
                    }
                }

                constexpr std::uint32_t teleport_flag =
                    render_teleportation::particle_flag_teleport;
                if (has_origin)
                    update_render_teleport(hero_handle, origin,
                        teleport_flag |
                            render_teleportation::particle_flag_teleport_start,
                        true, hero_name, modifier_duration_ms, now,
                        modifier_elapsed_ms, origin_confidence,
                        modifier_teleport.cast_key);
                if (has_destination)
                    update_render_teleport(hero_handle, destination,
                        teleport_flag |
                            render_teleportation::particle_flag_teleport_destination,
                        true, hero_name, modifier_duration_ms, now,
                        modifier_elapsed_ms, destination_confidence,
                        modifier_teleport.cast_key);
        }

        // Illusions can share a player id. Prefer the real hero snapshot that
        // owns a TP scroll, otherwise keep the first stable portrait entry.
        for (std::size_t index = 0;
             index < g_render_intelligence.enemy_teleport_count; ++index) {
            auto& current = g_render_intelligence.enemy_teleports[index];
            if (current.player_id != player_id)
                continue;
            if (!current.has_scroll && candidate.has_scroll)
                current = candidate;
            return;
        }

        if (g_render_intelligence.enemy_teleport_count <
            g_render_intelligence.enemy_teleports.size()) {
            g_render_intelligence.enemy_teleports[
                g_render_intelligence.enemy_teleport_count++] = candidate;
        }
    };

    // Enemy discovery already produced a stable five-hero cache. Reusing it
    // avoids a full 32K entity walk every 500 ms and also keeps TP detection
    // independent of controller replication in fog/demo modes.
    int fallback_radiant_slot = 0;
    int fallback_dire_slot = 0;
    for (std::size_t index = 0;
         index < g_render_intelligence.enemy_visibility_count; ++index) {
        auto& visibility = g_render_intelligence.enemy_visibility[index];
        const auto hero = entity_by_index(entity_system,
            static_cast<int>(visibility.entity_index));
        if (!plausible_pointer(hero) || visibility.team == 0 ||
            visibility.team == local_team)
            continue;

        int player_id = visibility.player_id;
        if (player_id < 0 || player_id > 31) {
            int& fallback_slot = visibility.team == 2
                ? fallback_radiant_slot : fallback_dire_slot;
            player_id = (visibility.team == 2 ? 0 : 5) +
                std::min(fallback_slot++, 4);
        }
        process_enemy_hero(visibility, hero, visibility.entity_index, player_id,
            visibility.team, visibility.designer_name);
    }

    std::sort(g_render_intelligence.enemy_teleports.begin(),
        g_render_intelligence.enemy_teleports.begin() +
            g_render_intelligence.enemy_teleport_count,
        [](const enemy_teleport_status_entry& lhs,
            const enemy_teleport_status_entry& rhs) {
            return lhs.player_id < rhs.player_id;
        });
}

armlet_vec3 camp_block_half_extents(int type) {
    // Spawn volumes are rectangular and considerably wider than the old
    // radial approximation. These conservative extents cover wards placed in
    // the corners of the stock camp boxes as well as units standing inside.
    switch (type) {
    case 4: return {720.0f, 610.0f, 0.0f}; // ancient
    case 3: return {680.0f, 580.0f, 0.0f}; // large
    case 2: return {640.0f, 550.0f, 0.0f}; // medium
    default: return {610.0f, 520.0f, 0.0f};
    }
}

float camp_block_radius(int type) {
    const auto extents = camp_block_half_extents(type);
    return std::max(extents.x, extents.y);
}

bool read_native_render_camps() {
    if(schema_fields::game_rules_m_NeutralSpawnBoxes<=0)return false;
    std::uintptr_t rules=0,data=0;int count=0;
    if(!safe_read(g_game_rules_global.load(std::memory_order_acquire),rules)||!plausible_pointer(rules))return false;
    const auto vector=rules+schema_fields::game_rules_m_NeutralSpawnBoxes;
    if(!safe_read(vector,count)||count<=0||count>static_cast<int>(g_render_intelligence.camps.size())||
        !safe_read(vector+8,data)||!plausible_pointer(data))return false;
    std::array<std::uint8_t,neutral_spawn_boxes::stride*64> bytes{};
    if(static_cast<std::size_t>(count)>bytes.size()/neutral_spawn_boxes::stride||
        !safe_copy_memory(data,bytes.data(),count*neutral_spawn_boxes::stride))return false;
    std::array<render_camp_entry,64> camps{};
    for(int i=0;i<count;++i) {
        neutral_spawn_boxes::box box{};
        if(!neutral_spawn_boxes::decode({bytes.data()+i*neutral_spawn_boxes::stride,neutral_spawn_boxes::stride},box))return false;
        auto& camp=camps[i];camp.type=box.type;camp.has_native_bounds=true;
        camp.minimum={box.minimum[0],box.minimum[1],box.minimum[2]};
        camp.maximum={box.maximum[0],box.maximum[1],box.maximum[2]};
        camp.position={(box.minimum[0]+box.maximum[0])*.5f,(box.minimum[1]+box.maximum[1])*.5f,box.origin[2]};
        // Geometry is exact; occupancy can still be inferred from fog evidence.
        camp.estimated=true;
    }
    std::copy_n(camps.begin(),count,g_render_intelligence.camps.begin());
    g_render_intelligence.camp_count=count;return true;
}

/**
 * @brief Собирает лагеря и кандидатов блокировки/урона; собственные нейтралы лагеря не должны
 * становиться его блокерами.
 */
void refresh_render_camps(std::uintptr_t entity_system, ULONGLONG now) {
    const bool camp_status = g_render_camp_status.load(std::memory_order_relaxed);
    // Neutral units also carry replicated effect modifiers. Keep their shared
    // bounded discovery cache available to FoW tracking when Jungle is hidden.
    if (!camp_status && !g_render_trace_icons.load(std::memory_order_relaxed)) {
        g_render_intelligence.camp_count = 0;
        g_render_intelligence.neutral_creep_count = 0;
        g_render_intelligence.camp_entity_count=0;
        g_render_intelligence.camp_entities_discovered_at=0;
        g_render_diagnostics.neutral_creeps.store(0, std::memory_order_relaxed);
        g_render_diagnostics.blocked_camps.store(0, std::memory_order_relaxed);
        return;
    }
    if (now - g_render_intelligence.camps_refreshed_at < 250)
        return;
    g_render_intelligence.camps_refreshed_at = now;

    const auto client_base = g_client_base_address.load(
        std::memory_order_acquire);
    const auto client_size = plausible_pointer(client_base)
        ? image_size(client_base) : 0;

    struct blocker_entry { armlet_vec3 position{}; };
    std::array<blocker_entry, 384> blockers{};
    std::size_t blocker_count = 0;
    g_render_intelligence.camp_count = 0;
    g_render_intelligence.neutral_creep_count = 0;
    g_render_intelligence.friendly_effect_target_count=0;

    const bool native_camps=read_native_render_camps();

    int rtti_camp_count = 0;
    std::uint8_t local_team=0;
    const auto local_hero=g_monitor.hero_address.load(std::memory_order_relaxed);
    if(!safe_read(local_hero+schema_fields::m_iTeamNum,local_team)||
        (local_team!=2&&local_team!=3))return;
    const bool discover=g_render_intelligence.camp_entities_discovered_at==0||
        now-g_render_intelligence.camp_entities_discovered_at>=3000;
    if(discover)g_render_intelligence.camp_entity_count=0;
    const auto process_camp_entity = [&](std::uintptr_t entity, int entity_index) {
        char name[96]{};
        const bool named=read_designer_name(entity,name);
        // Physical drops can have no designer-name pointer. Resolve their
        // live item through schema rather than treating the container as a hero.
        if(camp_status&&schema_fields::physical_item_m_hItem>0&&
            ((!named||!name[0])||std::strstr(name,"item_drop"))&&client_size&&
            rtti_name_contains(entity,client_base,client_size,"C_DOTA_Item_Physical")) {
            if(discover&&g_render_intelligence.camp_entity_count<g_render_intelligence.camp_entities.size())
                g_render_intelligence.camp_entities[g_render_intelligence.camp_entity_count++]={static_cast<std::uint32_t>(entity_index),entity};
            std::uint32_t handle=UINT32_MAX;char item_name[96]{};armlet_vec3 position{};
            if(safe_read(entity+schema_fields::physical_item_m_hItem,handle)&&handle&&handle!=UINT32_MAX&&
                read_designer_name(entity_by_handle(entity_system,handle),item_name)&&
                (std::strcmp(item_name,"item_madstone_bundle")==0||
                 (std::strncmp(item_name,"item_tier",9)==0&&std::strstr(item_name,"_token")))&&
                read_world_position(entity,position)&&valid_teleport_endpoint(position))
                g_render_intelligence.marks.observe(static_cast<std::uint64_t>(entity),effect_marks::kind::drop,
                    {position.x,position.y,position.z},item_name,"Neutral drop",now);
            return false;
        }
        if(!named)return false;

        const bool named_spawner =
            std::strcmp(name, "npc_dota_neutral_spawner") == 0;
        const bool typed_spawner = !named_spawner && client_size != 0 &&
            std::strncmp(name, "prop_dynamic", 12) == 0 &&
            rtti_name_contains(entity, client_base, client_size,
                "C_DOTA_NeutralSpawner");
        const bool explicit_ward = std::strstr(name, "ward") != nullptr;
        const bool npc=std::strncmp(name,"npc_dota_",9)==0;
        if(!named_spawner&&!typed_spawner&&!npc&&!explicit_ward)return false;
        if(discover&&g_render_intelligence.camp_entity_count<g_render_intelligence.camp_entities.size())
            g_render_intelligence.camp_entities[g_render_intelligence.camp_entity_count++]={
                static_cast<std::uint32_t>(entity_index),entity};
        if (named_spawner || typed_spawner) {
            if(native_camps)return false;
            if (typed_spawner)
                ++rtti_camp_count;
            if (g_render_intelligence.camp_count >=
                g_render_intelligence.camps.size())
                return false;
            render_camp_entry camp{};
            if (!read_world_position(entity, camp.position))
                return false;
            if (schema_fields::neutral_spawner_m_Type > 0)
                safe_read(entity + schema_fields::neutral_spawner_m_Type, camp.type);
            g_render_intelligence.camps[g_render_intelligence.camp_count++] = camp;
            return false;
        }

        if (std::strncmp(name, "npc_dota_", 9) != 0 && !explicit_ward)
            return false;
        int health = 0;
        int max_health = 0;
        armlet_vec3 position{};
        const bool living = safe_read(entity + schema_fields::m_iHealth, health) &&
            safe_read(entity + schema_fields::m_iMaxHealth, max_health) &&
            health > 0 && max_health > 0;
        if ((!living && !explicit_ward) || !read_world_position(entity, position))
            return false;

        bool neutral_unit = std::strstr(name, "npc_dota_neutral_") != nullptr ||
            std::strstr(name, "npc_dota_creep_neutral") != nullptr;
        if (!neutral_unit && schema_fields::m_bIsNeutralUnitType > 0) {
            bool networked_neutral = false;
            neutral_unit = safe_read(entity +
                    schema_fields::m_bIsNeutralUnitType, networked_neutral) &&
                networked_neutral;
        }
        if (!neutral_unit && schema_fields::m_hNeutralSpawner > 0) {
            std::uint32_t spawner = 0xFFFFFFFFu;
            neutral_unit = safe_read(entity + schema_fields::m_hNeutralSpawner,
                    spawner) && spawner != 0 && spawner != 0xFFFFFFFFu;
        }
        if (neutral_unit) {
            if (g_render_intelligence.neutral_creep_count <
                g_render_intelligence.neutral_creeps.size()) {
                auto& neutral = g_render_intelligence.neutral_creeps[
                    g_render_intelligence.neutral_creep_count++];
                neutral.entity_index = static_cast<std::uint32_t>(entity_index);
                neutral.position = position;
                neutral.health = health;
                neutral.gold_min=neutral.gold_max=-1;
                if(schema_fields::npc_gold_min>0)safe_read(entity+schema_fields::npc_gold_min,neutral.gold_min);
                if(schema_fields::npc_gold_max>0)safe_read(entity+schema_fields::npc_gold_max,neutral.gold_max);
                if(neutral.gold_min<0||neutral.gold_max<neutral.gold_min||neutral.gold_max>10000)
                    neutral.gold_min=neutral.gold_max=-1;
                if (schema_fields::m_flLastDamageTime > 0)
                    safe_read(entity + schema_fields::m_flLastDamageTime,
                        neutral.last_damage_time);
            }
            // A camp's own neutral inhabitants never block its next spawn.
            return false;
        }

        // Never mark a camp red for an ally, a creep or a stale hidden origin.
        // A recent, attributed hero event supports only an approximate body area.
        std::uint8_t team=0;
        if(!safe_read(entity+schema_fields::m_iTeamNum,team)||(team!=2&&team!=3))return false;
        char hero_name[96]{};
        if(!entity_is_hero(entity,hero_name))return false;
        if(team==local_team) {
            auto& targets=g_render_intelligence.friendly_effect_targets;
            auto& count=g_render_intelligence.friendly_effect_target_count;
            if(entity!=local_hero&&count<targets.size()) {
                std::uintptr_t identity=0;std::uint32_t handle=UINT32_MAX;
                if(safe_read(entity+schema_fields::m_pEntityIdentity,identity)&&plausible_pointer(identity)&&
                    safe_read(identity+0x10,handle)&&entity_by_handle(entity_system,handle)==entity)
                    targets[count++]=handle;
            }
            return false;
        }
        bool hidden=true;
        if(!read_hero_hidden_from_local_team(entity_system,entity,entity_index,local_team,hidden))return false;
        bool fresh=!hidden;
        if(hidden)for(std::size_t i=0;i<g_render_intelligence.enemy_visibility_count;++i) {
            const auto& evidence=g_render_intelligence.enemy_visibility[i];
            if(same_entity_handle(evidence.entity_index,static_cast<std::uint32_t>(entity_index))&&
                evidence.entity_address==entity&&evidence.has_effect_body_position&&
                now>=evidence.effect_body_observed_at&&now-evidence.effect_body_observed_at<=1200) {
                position=evidence.effect_body_position;fresh=true;break;
            }
        }
        if(hidden&&!fresh)for(std::size_t i=0;i<g_render_intelligence.trace_count;++i) {
            const auto& trace=g_render_intelligence.traces[i];
            // CP0 can be a remote spell impact: that is not the caster's body.
            if(!trace.particle_position&&
                same_entity_handle(trace.owner_handle,static_cast<std::uint32_t>(entity_index))&&
                now>=trace.created_at&&now-trace.created_at<=3000&&trace.expires_at>now) {
                position=trace.position;fresh=true;break;
            }
        }
        if(fresh&&blocker_count<blockers.size())blockers[blocker_count++].position=position;
        return false;
    };
    if(discover) {
        visit_entities(entity_system,process_camp_entity);
        g_render_intelligence.camp_entities_discovered_at=now;
    } else for(std::size_t i=0;i<g_render_intelligence.camp_entity_count;++i) {
        const auto& cached=g_render_intelligence.camp_entities[i];
        const auto entity=entity_by_index(entity_system,static_cast<int>(cached.index));
        if(entity==cached.address)process_camp_entity(entity,static_cast<int>(cached.index));
    }

    int blocked_count = 0;
    for (std::size_t camp_index = 0;
         camp_index < g_render_intelligence.camp_count; ++camp_index) {
        auto& camp = g_render_intelligence.camps[camp_index];
        const armlet_vec3 extents = camp_block_half_extents(camp.type);
        camp.blocked = false;
        if (!camp_status)
            continue;
        for (std::size_t blocker_index = 0; blocker_index < blocker_count;
             ++blocker_index) {
            const auto& blocker = blockers[blocker_index].position;
            const bool inside=camp.has_native_bounds
                ? blocker.x>=camp.minimum.x&&blocker.x<=camp.maximum.x&&
                  blocker.y>=camp.minimum.y&&blocker.y<=camp.maximum.y&&
                  blocker.z>=camp.minimum.z&&blocker.z<=camp.maximum.z
                : std::abs(camp.position.x-blocker.x)<=extents.x&&std::abs(camp.position.y-blocker.y)<=extents.y;
            if (inside) {
                camp.blocked = true;
                break;
            }
        }
        if (camp.blocked)
            ++blocked_count;
    }

    std::uintptr_t jungle_rules=0;float jungle_time=0;
    safe_read(g_game_rules_global.load(),jungle_rules);
    read_game_time(g_client_base_address.load(),jungle_time);
    g_render_intelligence.jungle.world(jungle_rules,jungle_time);
    g_render_intelligence.jungle.update(g_render_intelligence.camps,unsigned(g_render_intelligence.camp_count),
        g_render_intelligence.neutral_creeps,unsigned(g_render_intelligence.neutral_creep_count),now);
    for(std::size_t i=0;i<g_render_intelligence.trace_count;++i){const auto& trace=g_render_intelligence.traces[i];
        if(!trace.creep_wave&&(std::strcmp(trace.detector_trigger,"Jungle farming")==0||std::strcmp(trace.detector_trigger,"Estimated farm")==0))
            g_render_intelligence.jungle.farming(trace.position,trace.designer_name,trace.created_at,unsigned(g_render_intelligence.camp_count));
    }
    g_render_diagnostics.neutral_creeps.store(
        static_cast<int>(g_render_intelligence.neutral_creep_count),
        std::memory_order_relaxed);
    g_render_diagnostics.blocked_camps.store(
        blocked_count, std::memory_order_relaxed);
    g_render_diagnostics.rtti_camps.store(
        rtti_camp_count, std::memory_order_relaxed);
}

/**
 * @brief Обновляет геометрию мира для преобразования миникарты; это отдельный путь от view
 * matrix мировых отметок.
 */
void commit_jungle_sample(std::span<const jungle_status::unit_snapshot> units,std::uint64_t session,float game,ULONGLONG now){
    auto& dst=g_render_intelligence;dst.neutral_creep_count=0;
    for(const auto& unit:units){if(dst.neutral_creep_count>=dst.neutral_creeps.size())break;
        auto& n=dst.neutral_creeps[dst.neutral_creep_count++];n={};n.entity_index=unit.index&0x7fffu;
        n.position={unit.position.x,unit.position.y,unit.position.z};n.health=unit.health;
        n.gold_min=unit.gold_min;n.gold_max=unit.gold_max;
    }
    dst.jungle.world(std::uintptr_t(session),game);
    dst.jungle.update(dst.camps,unsigned(dst.camp_count),units,unsigned(units.size()),now);
    for(std::size_t i=0;i<dst.trace_count;++i){const auto& trace=dst.traces[i];
        if(!trace.creep_wave&&(std::strcmp(trace.detector_trigger,"Jungle farming")==0||std::strcmp(trace.detector_trigger,"Estimated farm")==0))
            dst.jungle.farming(trace.position,trace.designer_name,trace.created_at,unsigned(dst.camp_count));
    }
}

void refresh_minimap_bounds(std::uintptr_t entity_system, ULONGLONG now) {
    constexpr ULONGLONG refresh_interval_ms = 2000;
    if (g_render_intelligence.has_minimap_bounds)
        return;
    if (now - g_render_intelligence.minimap_bounds_refreshed_at <
        refresh_interval_ms)
        return;
    g_render_intelligence.minimap_bounds_refreshed_at = now;

    armlet_vec3 minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(), 0.0f};
    armlet_vec3 maximum{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(), 0.0f};
    std::size_t boundary_count = 0;
    visit_entities(entity_system, [&](std::uintptr_t entity, int) {
        char name[96]{};
        armlet_vec3 position{};
        if (!read_designer_name(entity, name) ||
            std::strcmp(name, "dota_minimap_boundary") != 0 ||
            !read_world_position(entity, position))
            return false;

        minimum.x = std::min(minimum.x, position.x);
        minimum.y = std::min(minimum.y, position.y);
        maximum.x = std::max(maximum.x, position.x);
        maximum.y = std::max(maximum.y, position.y);
        ++boundary_count;
        return false;
    });

    const float width = maximum.x - minimum.x;
    const float height = maximum.y - minimum.y;
    const bool valid = boundary_count >= 2 && std::isfinite(width) &&
        std::isfinite(height) && width >= 8000.0f && height >= 8000.0f &&
        width <= 40000.0f && height <= 40000.0f;
    g_render_intelligence.has_minimap_bounds = valid;
    if (valid) {
        g_render_intelligence.minimap_minimum = minimum;
        g_render_intelligence.minimap_maximum = maximum;
    }
}

/**
 * @brief Публикует компактные атомарные счётчики после изменения массивов; счётчик не заменяет
 * блокировку при чтении самих записей.
 */
void publish_render_intelligence_counts() {
    std::uint32_t effect_count=0;
    const auto mark_time=GetTickCount64();
    for(const auto& mark:g_render_intelligence.marks.entries)
        if(mark.expires>mark_time&&(mark.type==effect_marks::kind::drop
            ? g_render_camp_status.load(std::memory_order_relaxed)
            : g_render_trace_icons.load(std::memory_order_relaxed)))++effect_count;
    g_render_effect_mark_count_snapshot.store(effect_count,std::memory_order_release);
    // Release stores make every count independently safe for status readers.
    // The arrays themselves remain owned by the worker/overlay thread.
    g_render_trace_count_snapshot.store(
        static_cast<std::uint32_t>(g_render_intelligence.trace_count),
        std::memory_order_release);
    std::uint32_t visible_teleport_events = 0;
    for (std::size_t index = 0;
         index < g_render_intelligence.teleport_count; ++index) {
        const auto& teleport = g_render_intelligence.teleports[index];
        if (teleport.enemy_confirmed && !teleport.canceled &&
            (teleport.has_origin || teleport.has_destination))
            ++visible_teleport_events;
    }
    g_render_teleport_count_snapshot.store(
        visible_teleport_events, std::memory_order_release);
    g_render_camp_count_snapshot.store(
        g_render_camp_status.load(std::memory_order_relaxed)
            ? static_cast<std::uint32_t>(g_render_intelligence.camp_count)
            : 0,
        std::memory_order_release);
}

/**
 * @brief Единая write-транзакция render-снимка: очистка, polling мира, чтение particle ring,
 * атрибуция и публикация счётчиков.
 */
void resolve_particle_ability_icon(
    const render_teleportation::particle_observation& observation,
    const char* source_name, std::uint32_t hero_handle, std::uintptr_t entity_system,
    char (&output)[96], std::uint32_t raw_source_handle=UINT32_MAX) {
    output[0]='\0';
    if(particle_ability_match::local_feedback(observation.debug_name))return;
    if (observation.flags & render_teleportation::particle_flag_teleport) {
        copy_designer_name(output,
            (observation.flags & render_teleportation::particle_flag_twin_gate)
                ? "twin_gate_portal_warp"
                : (observation.flags & render_teleportation::particle_flag_keen_conveyance)
                    ? "tinker_keen_teleport" : "item_tpscroll");
        return;
    }
    // A live emitter retains its resolved source even after the activation
    // window ends. Validate membership/generation on every reuse.
    struct emitter_binding {
        std::uintptr_t effect=0,collection=0;
        std::uint32_t owner=UINT32_MAX,source=UINT32_MAX;
        std::uint64_t seen=0;
    };
    static std::array<emitter_binding,512> bindings{};
    const auto at=observation.observed_at_ms;
    auto recent=[at](std::uint64_t stamp,std::uint64_t age) {
        return stamp!=0 && (stamp>at ? stamp-at<=150 : at-stamp<=age);
    };
    for (std::size_t i=0;i<g_render_intelligence.enemy_visibility_count;++i) {
        auto& hero=g_render_intelligence.enemy_visibility[i];
        if(!same_entity_handle(hero.entity_index,hero_handle))continue;
        refresh_enemy_source_cache(entity_system,entity_by_handle(entity_system,hero_handle),hero,GetTickCount64());
        const bool hero_resource=particle_ability_match::resource_hero(observation.debug_name)==hero.designer_name;
        const enemy_ability_entry* selected=nullptr;
        int best=0;bool ambiguous=false;
        for(std::size_t j=0;j<hero.ability_count+hero.item_count;++j) {
            const auto& state=j<hero.ability_count?hero.abilities[j]:hero.items[j-hero.ability_count];
            if(!ability_icons::is_available(state.designer_name))continue;
            const auto flags=particle_ability_match::behavior(state.designer_name);
            const bool attack_proc=(flags&(particle_catalog::passive|particle_catalog::attack_proc)) &&
                !(flags&particle_catalog::aura);
            const bool exact=raw_source_handle!=UINT32_MAX && raw_source_handle==state.entity_handle;
            const bool activation=recent(state.activated_at,750)&&!attack_proc;
            int value=particle_ability_match::score(observation.debug_name,state.designer_name,hero.designer_name);
            if(attack_proc&&!recent(hero.last_attack_event_at,1500)&&!exact)continue;
            if(activation && (value>0 || (hero_resource &&
                particle_ability_match::caster_emitter(observation.debug_name,state.designer_name))))
                value+=1000;
            if(exact)value+=2000;
            // A source string has no identity proof. It is only a last fallback
            // when the resource is otherwise unknown and membership is live.
            if(!value && source_name && std::strcmp(source_name,state.designer_name)==0)value=1;
            if(value>best){best=value;selected=&state;ambiguous=false;}
            else if(value>0&&value==best&&selected && std::strcmp(selected->designer_name,state.designer_name)!=0)
                ambiguous=true;
        }
        emitter_binding* cached=nullptr;
        if(observation.effect&&observation.collection) {
            for(auto& entry:bindings)if(entry.effect==observation.effect&&entry.collection==observation.collection&&
                entry.owner==hero_handle && at>=entry.seen && at-entry.seen<=1500) {cached=&entry;break;}
            if(cached && best<1000 && !(observation.flags&render_teleportation::particle_flag_new_effect)) {
                for(std::size_t j=0;j<hero.ability_count+hero.item_count;++j) {
                    const auto& state=j<hero.ability_count?hero.abilities[j]:hero.items[j-hero.ability_count];
                    if(state.entity_handle==cached->source){selected=&state;ambiguous=false;break;}
                }
            }
        }
        if(!selected||ambiguous)return;
        copy_designer_name(output,selected->designer_name);
        if(observation.effect&&observation.collection) {
            if(!cached)cached=&*std::min_element(bindings.begin(),bindings.end(),
                [](const auto& a,const auto& b){return a.seen<b.seen;});
            *cached={observation.effect,observation.collection,hero_handle,selected->entity_handle,at};
        }
        return;
    }
}

bool particle_caster_body_position(const render_teleportation::particle_observation& observation,
    bool exact_carrier,const char* icon,armlet_vec3& position) {
    if(!exact_carrier || !(observation.flags&render_teleportation::particle_flag_control_point_position) ||
        !particle_ability_match::caster_emitter(observation.debug_name,icon))return false;
    const armlet_vec3 current{observation.x,observation.y,observation.z};
    if(!valid_teleport_endpoint(current))return false;
    position=current;
    return true;
}

bool particle_owner_is_live(std::uintptr_t token) noexcept {
    std::uintptr_t entity_system=0;
    if (!read_game_entity_system(0,entity_system)) return false;
    if (token<=0xFFFFFFFFu)
        return plausible_pointer(entity_by_handle(entity_system,static_cast<std::uint32_t>(token)));
    std::uintptr_t identity=0;
    std::uint32_t handle=0xFFFFFFFFu;
    // The same identity handle already cross-validates the calibrated entity
    // table. Pointer owners must still resolve to this exact live generation.
    return safe_read(token+schema_fields::m_pEntityIdentity,identity) &&
        plausible_pointer(identity) && safe_read(identity+0x10,handle) &&
        entity_by_handle(entity_system,handle)==token;
}

std::uint64_t particle_source_event_key(std::uint32_t hero_handle,const char* icon,float game_time) {
    for(std::size_t i=0;i<g_render_intelligence.enemy_visibility_count;++i) {
        const auto& hero=g_render_intelligence.enemy_visibility[i];
        if(!same_entity_handle(hero.entity_index,hero_handle))continue;
        const auto flags=particle_ability_match::behavior(icon);
        float source_time=0;std::uint32_t source_handle=0;
        for(std::size_t j=0;j<hero.ability_count+hero.item_count;++j) {
            const auto& source=j<hero.ability_count?hero.abilities[j]:hero.items[j-hero.ability_count];
            if(std::strcmp(source.designer_name,icon)!=0)continue;
            source_time=std::max(source.cast_start_time,source.channel_start_time);
            source_handle=source.entity_handle;break;
        }
        if((flags&(particle_catalog::passive|particle_catalog::attack_proc))&&
            !(flags&particle_catalog::aura)&&hero.last_attack_time>=source_time) {
            // Passive spell and item procs from one attack share this key.
            if(!recent_game_timestamp(hero.last_attack_time,game_time))return 0;
            std::uint32_t stamp=0;std::memcpy(&stamp,&hero.last_attack_time,sizeof(stamp));
            return (std::uint64_t(stamp)<<32)|hero.entity_index|(1ULL<<63);
        }
        if(source_handle&&recent_game_timestamp(source_time,game_time)) {
            std::uint32_t stamp=0;std::memcpy(&stamp,&source_time,sizeof(stamp));
            return (std::uint64_t(stamp)<<32)|source_handle;
        }
        return 0;
    }
    return 0;
}

// Full CEntityHandle (index and serial) of a live entity. The handle must
// resolve back to this exact address; a recycled slot never matches it.
bool effect_target_handle(std::uintptr_t entity_system,std::uintptr_t entity,std::uint32_t& handle) {
    handle=UINT32_MAX;
    std::uintptr_t identity=0;
    return schema_fields::m_pEntityIdentity>0&&plausible_pointer(entity)&&
        safe_read(entity+schema_fields::m_pEntityIdentity,identity)&&plausible_pointer(identity)&&
        safe_read(identity+0x10,handle)&&handle!=UINT32_MAX&&
        entity_by_handle(entity_system,handle)==entity;
}

// Some passive auras have no identifiable particle. Their active modifiers
// still carry the caster handle and an engine tick. Sample a small rotating
// target set; do not invent caster positions from victims or stale hero origins.
// Proven incoming target tags are revalidated first from a bounded cache, so
// their marks depend neither on the neutral queue length nor on vector order.
void refresh_modifier_effect_events(std::uintptr_t entity_system,float game_time,ULONGLONG now) {
    if(!std::isfinite(game_time)||game_time<0)return;
    const auto client=g_client_base_address.load(std::memory_order_acquire);
    const auto size=plausible_pointer(client)?image_size(client):0;
    if(!size)return;
    auto& known_tags=g_render_intelligence.effect_validations;
    // One budget of modifier-field reads per poll; a final timestamp pair may
    // overrun it by at most two reads.
    unsigned budget=effect_validation::read_budget;
    const auto charge=[&](unsigned reads){budget=budget>reads?budget-reads:0;};
    const auto find_owner=[&](std::uintptr_t caster,std::uint32_t caster_handle)->enemy_visibility_entry* {
        for(std::size_t i=0;i<g_render_intelligence.enemy_visibility_count;++i) {
            auto& enemy=g_render_intelligence.enemy_visibility[i];
            if(enemy.initialized&&enemy.entity_address==caster&&
                same_entity_handle(enemy.entity_index,caster_handle))return &enemy;
        }
        return nullptr;
    };
    const auto fresh_tag=[&](float creation,float tick) {
        return std::isfinite(creation)&&creation>=0&&creation<=game_time+.5f&&
            std::isfinite(tick)&&tick>=game_time-1.5f&&tick<=game_time+.5f;
    };
    const auto mark_alive=[&](std::uintptr_t modifier) {
        for(const auto& mark:g_render_intelligence.marks.entries)
            if(mark.key==modifier&&mark.type==effect_marks::kind::target)return mark.expires>now;
        return false;
    };

    // 1. Validation of known tags. Any missing proof drops the entry; only a
    // new discovery can track the modifier again. The TTL is never extended.
    std::array<effect_validation::entry*,effect_validation::cache::per_poll> due{};
    const auto due_count=known_tags.select(due);
    std::array<std::uintptr_t,128> live{};std::size_t live_count=0;
    std::uintptr_t live_target=0;bool live_valid=false;
    for(std::size_t i=0;i<due_count;++i) {
        auto& known=*due[i];
        // A lapsed mark is not resurrected from cache.
        if(!mark_alive(known.modifier)){known={};continue;}
        std::uint32_t target_handle=UINT32_MAX;
        if(!effect_target_handle(entity_system,known.target,target_handle)||
            target_handle!=known.target_handle){known={};continue;}
        if(known.target!=live_target) {
            live_target=known.target;
            live_valid=collect_active_modifiers(known.target,client,size,live,live_count);
        }
        if(!live_valid||std::find(live.begin(),live.begin()+live_count,known.modifier)==
            live.begin()+live_count){known={};continue;}
        std::uint32_t caster_handle=UINT32_MAX,live_caster_handle=UINT32_MAX;
        charge(1);
        if(!safe_read(known.modifier+private_modifier_layout::m_hCaster,caster_handle)||
            caster_handle!=known.caster_handle||
            entity_by_handle(entity_system,caster_handle)!=known.caster||
            !effect_target_handle(entity_system,known.caster,live_caster_handle)||
            live_caster_handle!=caster_handle||
            !find_owner(known.caster,caster_handle)){known={};continue;}
        float creation=0,tick=0;
        armlet_vec3 position{};
        char type[192]{};
        charge(2);
        if(!safe_read(known.modifier+private_modifier_layout::m_flCreationTime,creation)||
            creation!=known.creation||
            !safe_read(known.modifier+private_modifier_layout::m_flPreviousTick,tick)||
            !fresh_tag(creation,tick)||!known.rtti[0]||
            !read_rtti_name(known.modifier,client,size,type)||
            std::strcmp(type,known.rtti)!=0||
            !read_world_position(known.target,position)){known={};continue;}
        g_render_intelligence.marks.observe(known.modifier,effect_marks::kind::target,
            {position.x,position.y,position.z},known.icon,"Targeted",now);
        known.validated=now;
    }

    // 2. Discovery. Each queue resumes inside a target's modifier vector.
    const auto discover=[&](effect_validation::queue& cursor,std::size_t target_count,
        auto&& resolve,unsigned visits,unsigned target_limit) {
        unsigned targets=0;
        while(budget&&visits&&targets<target_limit&&targets<target_count) {
            cursor.target%=target_count;
            const auto target=resolve(cursor.target);
            if(cursor.address!=target){cursor.address=target;cursor.modifier=0;}
            std::array<std::uintptr_t,128> modifiers{};std::size_t count=0;
            armlet_vec3 position{};
            if(!plausible_pointer(target)||!read_world_position(target,position)||
                !collect_active_modifiers(target,client,size,modifiers,count)) {
                ++cursor.target;++targets;cursor.modifier=0;continue;
            }
            for(;cursor.modifier<count&&budget&&visits;++cursor.modifier) {
                const auto modifier=modifiers[cursor.modifier];
                if(known_tags.find(modifier,target))continue; // Validated above.
                charge(1);--visits;
                std::uint32_t caster_handle=UINT32_MAX;
                if(!safe_read(modifier+private_modifier_layout::m_hCaster,caster_handle))continue;
                const auto caster=entity_by_handle(entity_system,caster_handle);
                auto* owner=find_owner(caster,caster_handle);
                // entity_by_handle also supports plain indices, so serial-zero
                // modifier handles need the same exact identity proof here.
                std::uint32_t live_caster_handle=UINT32_MAX;
                if(!owner||!effect_target_handle(entity_system,caster,live_caster_handle)||
                    live_caster_handle!=caster_handle)continue;
                char type[192]{};
                if(!read_rtti_name(modifier,client,size,type))continue;
                const bool target_tag=target!=caster&&
                    (std::strstr(type,"Vision")||std::strstr(type,"_Target"))&&
                    std::strstr(type,"Intrinsic")==nullptr;
                const char* selected=nullptr;int best=0;bool ambiguous=false;
                for(std::size_t i=0;i<owner->ability_count+owner->item_count;++i) {
                    const auto& source=i<owner->ability_count?owner->abilities[i]:owner->items[i-owner->ability_count];
                    if(!ability_icons::is_available(source.designer_name))continue;
                    const auto flags=particle_ability_match::behavior(source.designer_name);
                    if(!(target_tag&&(flags&particle_catalog::unit_target))&&
                        !(flags&particle_catalog::aura)&&std::strstr(type,"Aura")==nullptr&&
                        std::strstr(type,"_aura")==nullptr)continue;
                    const int score=particle_ability_match::modifier_score(type,source.designer_name);
                    if(score>best){selected=source.designer_name;best=score;ambiguous=false;}
                    else if(score&&score==best&&std::strcmp(selected,source.designer_name)!=0)ambiguous=true;
                }
                if(!selected||ambiguous)continue;
                const bool incoming_target=target_tag&&
                    (particle_ability_match::behavior(selected)&particle_catalog::unit_target);
                if(!owner->dormant&&!incoming_target)continue;
                float creation=0,tick=0;
                charge(2);
                if(!safe_read(modifier+private_modifier_layout::m_flCreationTime,creation)||
                    !safe_read(modifier+private_modifier_layout::m_flPreviousTick,tick))continue;
                auto& activity=g_render_intelligence.modifier_events[(modifier>>4)%g_render_intelligence.modifier_events.size()];
                if(incoming_target) {
                    if(fresh_tag(creation,tick)) {
                        g_render_intelligence.marks.observe(modifier,effect_marks::kind::target,
                            {position.x,position.y,position.z},selected,"Targeted",now);
                        effect_validation::entry known{};
                        known.modifier=modifier;known.target=target;known.caster=caster;
                        known.caster_handle=caster_handle;known.creation=creation;known.validated=now;
                        effect_validation::assign(known.icon,selected);
                        effect_validation::assign(known.rtti,type);
                        // Without the recipient's full handle the tag stays discovery-only.
                        if(effect_target_handle(entity_system,target,known.target_handle))
                            known_tags.track(known);
                    }
                    continue;
                }
                if(!activity.observe(modifier,caster_handle,creation,tick,game_time))continue;
                char trigger[64]{};describe_ability_trigger(selected,trigger);
                const auto collection=target;
                if(!update_persistent_render_trace(caster_handle,position,selected,now,modifier,collection,false,true)) {
                    add_or_refresh_render_trace(caster_handle,position,owner->designer_name,trigger,now,
                        true,selected,true,modifier,collection);
                    update_persistent_render_trace(caster_handle,position,selected,now,modifier,collection,false,true);
                    g_render_diagnostics.fog_ability_events.fetch_add(1,std::memory_order_relaxed);
                }
            }
            if(cursor.modifier<count)break; // Resume this target next poll.
            cursor.modifier=0;++cursor.target;++targets;
        }
    };
    const auto local=g_monitor.hero_address.load(std::memory_order_relaxed);
    // Local and friendly heroes keep a capped share even with a full neutral queue.
    discover(g_render_intelligence.effect_priority_queue,1+g_render_intelligence.friendly_effect_target_count,
        [&](std::size_t index){return index==0?local:entity_by_handle(entity_system,
            g_render_intelligence.friendly_effect_targets[index-1]);},
        effect_validation::priority_visits,effect_validation::priority_targets);
    discover(g_render_intelligence.effect_neutral_queue,g_render_intelligence.neutral_creep_count,
        [&](std::size_t index){return entity_by_index(entity_system,
            static_cast<int>(g_render_intelligence.neutral_creeps[index].entity_index));},
        ~0u,effect_validation::neutral_targets);
}

#include "../intelligence/native_adapter.inl"

void poll_render_intelligence(std::uintptr_t entity_system) {
    rust_map_adapter::poll(entity_system);
}

/**
 * @brief Читает время/паузу движка. Game time нельзя напрямую вычитать из GetTickCount64 или
 * steady_clock без преобразования единиц и начала отсчёта.
 */
bool read_game_time(std::uintptr_t client_base, float& game_time, float& tick_interval) {
    std::uintptr_t global_vars = 0;
    if (!read_global_vars(client_base, global_vars))
        return false;
    if (schema_fields::game_rules_m_nPauseStartTick<=0 ||
        schema_fields::game_rules_m_nTotalPausedTicks<=0) return false;

    // CGlobalVars simulation time includes paused ticks. The engine's
    // C_GameRules time conversion subtracts total paused ticks and clamps to
    // pause-start time while paused. Resolve its interval operand and verify
    // the independent live Schema field used by that conversion.
    static std::atomic<std::uint32_t> interval_offset{0};
    static std::atomic<bool> resolution_attempted{false};
    auto interval_field=interval_offset.load(std::memory_order_acquire);
    if (!interval_field && !resolution_attempted.exchange(true)) {
        constexpr std::uint8_t pattern[]{0x66,0x0F,0x6E,0x47,0,
            0x48,0x8B,0x05,0,0,0,0,0x0F,0x5B,0xC0,
            0xF3,0x0F,0x59,0x40,0,0xF3,0x0F,0x5C,0xF0};
        constexpr char mask[]="xxxx?xxx????xxxxxxx?xxxx";
        static_assert(sizeof(mask)==sizeof(pattern)+1);
        const auto instruction=find_unique_executable_pattern(client_base,image_size(client_base),
            pattern,mask,sizeof(pattern));
        std::uint8_t paused_field=0, tick_field=0;
        std::int32_t globals_relative=0;
        if (instruction && safe_read(instruction+4,paused_field) &&
            paused_field==schema_fields::game_rules_m_nTotalPausedTicks &&
            safe_read(instruction+8,globals_relative) &&
            static_cast<std::uintptr_t>(static_cast<std::intptr_t>(instruction+12)+globals_relative)==
                g_global_vars_global.load(std::memory_order_acquire) &&
            safe_read(instruction+19,tick_field) && tick_field>0) {
            interval_field=tick_field;
            interval_offset.store(interval_field,std::memory_order_release);
        }
    }
    float simulation_time=0, interval=0;
    std::uintptr_t rules=0;
    int total_paused=0, pause_start=0;
    bool paused=false;
    if (!interval_field || schema_fields::game_rules_m_nPauseStartTick<=0 ||
        schema_fields::game_rules_m_nTotalPausedTicks<=0 ||
        !safe_read(global_vars+0x30,simulation_time) ||
        !safe_read(global_vars+interval_field,interval) ||
        !std::isfinite(simulation_time) || !std::isfinite(interval) ||
        interval<=0 || interval>0.1f ||
        !safe_read(g_game_rules_global.load(std::memory_order_acquire),rules) ||
        !plausible_pointer(rules) ||
        !safe_read(rules+schema_fields::game_rules_m_bGamePaused,paused) ||
        !safe_read(rules+schema_fields::game_rules_m_nPauseStartTick,pause_start) ||
        !safe_read(rules+schema_fields::game_rules_m_nTotalPausedTicks,total_paused) ||
        total_paused<0) return false;
    if (paused && pause_start>0)
        simulation_time=std::min(simulation_time,static_cast<float>(pause_start)*interval);
    game_time=simulation_time-static_cast<float>(total_paused)*interval;
    tick_interval=interval;
    return std::isfinite(game_time) && game_time>=0 && game_time<10000000.0f;
}

bool read_game_time(std::uintptr_t client_base, float& game_time) {
    float interval=0;
    return read_game_time(client_base,game_time,interval);
}

/**
 * @brief Читает время/паузу движка. Game time нельзя напрямую вычитать из GetTickCount64 или
 * steady_clock без преобразования единиц и начала отсчёта.
 */
bool read_game_paused(std::uintptr_t client_base, bool& paused) {
    (void)client_base;
    const auto global = g_game_rules_global.load(std::memory_order_acquire);
    std::uintptr_t game_rules = 0;
    return safe_read(global, game_rules) &&
        plausible_pointer(game_rules) &&
        safe_read(game_rules + schema_fields::game_rules_m_bGamePaused, paused);
}

// Hero Demo's checkbox changes dota_creeps_no_spawning, independently of
// C_DOTAGamerules::m_bCreepSpawningEnabled. Only an active local server makes
// this process's server convar authoritative; remote matches use GameRules.
bool read_local_creep_spawning(bool& spawning,std::uint32_t& revision) {
    if(!GetModuleHandleW(L"server.dll"))return false;
    const auto tier=GetModuleHandleW(L"tier0.dll");
    const auto engine=GetModuleHandleW(L"engine2.dll");
    if(!tier||!engine)return false;
    using factory_fn=void* (*)(const char*,int*);
    const auto factory=reinterpret_cast<factory_fn>(GetProcAddress(tier,"CreateInterface"));
    const auto engine_factory=reinterpret_cast<factory_fn>(GetProcAddress(engine,"CreateInterface"));
    if(!factory||!engine_factory)return false;
    std::uintptr_t cvar=0,service=0;
    __try {
        service=reinterpret_cast<std::uintptr_t>(engine_factory("NetworkServerService_001",nullptr));
        cvar=reinterpret_cast<std::uintptr_t>(factory("VEngineCvar007",nullptr));
    }
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    const auto read=[](std::uintptr_t at,void* to,std::size_t size){return safe_copy_memory(at,to,size);};
    if(!local_convar::active_server(read,service))return false;
    std::uintptr_t table=0,getter=0;
    if(!plausible_pointer(cvar)||!safe_read(cvar,table)||!safe_read(table+41*8,getter))return false;
    // GetConVarData's indexed load proves the list offset and 16-byte stride.
    // Reject an updated ABI instead of guessing another value field.
    constexpr std::uint8_t load[]{0x48,0x8b,0x4e,0x50,0x0f,0xb7,0xc3,0x48,0x03,0xc0,0x48,0x8b,0x1c,0xc1};
    std::array<std::uint8_t,192> code{};
    if(!safe_copy_memory(getter,code.data(),code.size())||
        std::search(code.begin(),code.end(),std::begin(load),std::end(load))==code.end())return false;
    static std::uintptr_t cached=0;
    bool disabled=false;
    if(!local_convar::boolean(read,cvar,"dota_creeps_no_spawning",cached,disabled,&revision))return false;
    spawning=!disabled;return true;
}

bool modifier_pointer_array_is_valid(std::uintptr_t data, int count,
    std::uintptr_t client_base, std::size_t client_size) {
    if (!plausible_pointer(data) || count <= 0 || count > 256)
        return false;
    const int sample_count = std::min(count, 8);
    for (int index = 0; index < sample_count; ++index) {
        std::uintptr_t modifier = 0;
        char class_name[192]{};
        if (safe_read(data + sizeof(void*) * static_cast<std::size_t>(index),
                modifier) &&
            read_rtti_name(modifier, client_base, client_size, class_name) &&
            std::strstr(class_name, "CDOTA_Modif") != nullptr)
            return true;
    }
    return false;
}

/**
 * @brief Проверяет/собирает массив указателей модификаторов с ограничением вместимости.
 * Отсутствие корректного массива обрабатывается как ошибка чтения.
 */
bool resolve_modifier_vector(std::uintptr_t manager,
    std::uintptr_t client_base, std::size_t client_size,
    int& count, std::uintptr_t& data) {
    const auto cached_count_offset = g_modifier_count_offset.load(
        std::memory_order_acquire);
    const auto cached_data_offset = g_modifier_data_offset.load(
        std::memory_order_acquire);
    // The live manager's vector is size @ 0x10, data @ 0x18, capacity @ 0x20.
    // A resident allocation with size zero is genuinely empty. Probing its
    // capacity as a second size resurrected old modifiers from unused slots.
    int canonical_count=0,capacity=0;std::uintptr_t canonical_data=0;
    if(safe_read(manager+0x10,canonical_count)&&safe_read(manager+0x18,canonical_data)&&
        safe_read(manager+0x20,capacity)&&canonical_count>=0&&capacity>=canonical_count&&capacity<=256&&
        (capacity==0||plausible_pointer(canonical_data))&&
        (canonical_count==0||modifier_pointer_array_is_valid(canonical_data,canonical_count,client_base,client_size))) {
        g_modifier_count_offset.store(0x10,std::memory_order_release);
        g_modifier_data_offset.store(0x18,std::memory_order_release);
        count=canonical_count;data=canonical_data;return true;
    }
    if (cached_count_offset != 0 && cached_data_offset != 0 &&
        safe_read(manager + cached_count_offset, count) &&
        safe_read(manager + cached_data_offset, data) && count >= 0 && count <= 256 &&
        (count == 0 || modifier_pointer_array_is_valid(
            data, count, client_base, client_size)))
        return true;

    // Source 2 CUtlVector-style layouts place the data pointer before size
    // (data @ +0, size @ +0x10). Older/private modifier layouts observed in
    // this project used the opposite order. Probe both orientations and keep
    // the result only after validating live CDOTA_Modifier RTTI entries.
    constexpr std::array<std::ptrdiff_t, 5> data_deltas{
        -0x18, -0x10, 0x08, 0x10, 0x18,
    };
    for (std::size_t count_offset = 0x08; count_offset <= 0x80;
         count_offset += sizeof(std::uint32_t)) {
        int candidate_count = 0;
        if (!safe_read(manager + count_offset, candidate_count) ||
            candidate_count <= 0 || candidate_count > 256)
            continue;
        for (const auto delta : data_deltas) {
            const auto signed_data_offset =
                static_cast<std::ptrdiff_t>(count_offset) + delta;
            if (signed_data_offset < 0)
                continue;
            const auto data_offset =
                static_cast<std::size_t>(signed_data_offset);
            std::uintptr_t candidate_data = 0;
            if (!safe_read(manager + data_offset, candidate_data) ||
                !modifier_pointer_array_is_valid(candidate_data, candidate_count,
                    client_base, client_size))
                continue;
            g_modifier_count_offset.store(count_offset, std::memory_order_release);
            g_modifier_data_offset.store(data_offset, std::memory_order_release);
            count = candidate_count;
            data = candidate_data;
            return true;
        }
    }
    return false;
}

/**
 * @brief Проверяет/собирает массив указателей модификаторов с ограничением вместимости.
 * Отсутствие корректного массива обрабатывается как ошибка чтения.
 */
bool collect_active_modifiers(std::uintptr_t hero, std::uintptr_t client_base,
    std::size_t client_size, std::array<std::uintptr_t, 128>& modifiers,
    std::size_t& modifier_count) {
    modifier_count = 0;
    const auto manager = hero + schema_fields::m_ModifierManager;
    std::uintptr_t data = 0;
    int count = 0;
    if (!resolve_modifier_vector(manager, client_base, client_size, count, data) ||
        count < 0 || count > static_cast<int>(modifiers.size()))
        return false;

    for (int index = 0; index < count; ++index) {
        std::uintptr_t modifier = 0;
        if (!safe_read(data + sizeof(void*) * static_cast<std::size_t>(index), modifier))
            return false;
        if (plausible_pointer(modifier))
            modifiers[modifier_count++] = modifier;
    }
    return true;
}

armlet_vec3 subtract(armlet_vec3 lhs, armlet_vec3 rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

float dot(armlet_vec3 lhs, armlet_vec3 rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

float length(armlet_vec3 value) {
    return std::sqrt(std::max(0.0f, dot(value, value)));
}

bool finite_nonnegative(float value) {
    return std::isfinite(value) && value >= 0.0f && value < 1000000.0f;
}

bool read_attack_record(const void* address, incoming_projectile& output,
    bool require_projectile) {
    const auto record = reinterpret_cast<std::uintptr_t>(address);
    std::uint16_t flags = 0;
    bool impacted = false;
    int projectile_speed = 0;
    if (!plausible_pointer(record) ||
        !safe_read(record + attack_record_offsets::flags, flags) ||
        !safe_read(record + attack_record_offsets::source, output.source_handle) ||
        !safe_read(record + attack_record_offsets::target, output.target_handle) ||
        !safe_read(record + attack_record_offsets::record, output.attack_record) ||
        !safe_read(record + attack_record_offsets::projectile_speed, projectile_speed) ||
        !safe_read(record + attack_record_offsets::damage_impacted, impacted))
        return false;

    constexpr std::uint16_t ranged_attack = 1u << 1;
    constexpr std::uint16_t use_projectile = 1u << 10;
    output.is_attack = (flags & 1u) != 0;
    output.ranged_attack = (flags & ranged_attack) != 0;
    output.use_projectile = (flags & use_projectile) != 0;
    if (!output.is_attack || impacted ||
        (require_projectile && (!output.ranged_attack || !output.use_projectile)))
        return false;

    output.speed = static_cast<float>(projectile_speed);
    safe_read(record + attack_record_offsets::damage, output.damage);
    safe_read(record + attack_record_offsets::original_damage, output.original_damage);
    safe_read(record + attack_record_offsets::physical_damage, output.physical_damage);
    safe_read(record + attack_record_offsets::magical_damage, output.magical_damage);
    safe_read(record + attack_record_offsets::composite_damage, output.composite_damage);
    safe_read(record + attack_record_offsets::pure_damage, output.pure_damage);

    const bool speed_valid = std::isfinite(output.speed) && output.speed >= 0.0f &&
        output.speed < 100000.0f;
    const bool projectile_speed_valid = !require_projectile || output.speed > 1.0f;
    const bool valid = output.source_handle != 0xFFFFFFFFu &&
        output.target_handle != 0xFFFFFFFFu && speed_valid && projectile_speed_valid &&
        finite_nonnegative(output.damage) && finite_nonnegative(output.original_damage) &&
        finite_nonnegative(output.physical_damage) &&
        finite_nonnegative(output.magical_damage) &&
        finite_nonnegative(output.composite_damage) && finite_nonnegative(output.pure_damage);
    output.damage_complete = valid;
    return valid;
}

incoming_projectile* select_projectile_slot(std::uint64_t projectile_id,
    projectile_kind kind, ULONGLONG now) {
    for (auto& entry : g_projectiles.entries) {
        if (entry.active && entry.projectile_id == projectile_id && entry.kind == kind)
            return &entry;
    }

    incoming_projectile* oldest = &g_projectiles.entries.front();
    for (auto& entry : g_projectiles.entries) {
        if (!entry.active)
            return &entry;
        if (entry.updated_at < oldest->updated_at)
            oldest = &entry;
    }
    (void)now;
    return oldest;
}
