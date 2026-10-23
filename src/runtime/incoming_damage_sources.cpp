/**
 * @file
 * @brief Источники входящего урона.
 *
 * Приводит внутренние снаряды, атаки башен/героев и внешнюю телеметрию к incoming_projectile.
 * Общий буфер защищён g_projectiles.lock; время наблюдения и полнота урона участвуют в
 * последующей оценке безопасности.
 */

// Projectile and attack inputs.
#include "melee_contact.h"

/**
 * @brief Обновляет свежесть внешней телеметрии и сетевую задержку под блокировкой буфера; это
 * не очистка всех ранее добавленных событий.
 */
void projectile_heartbeat(float network_latency_ms) {
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_projectiles.lock);
    g_projectiles.heartbeat = now;
    if (std::isfinite(network_latency_ms) && network_latency_ms >= 0.0f &&
        network_latency_ms <= 5000.0f) {
        g_projectiles.network_latency_ms = network_latency_ms;
        g_projectiles.latency_updated_at = now;
        g_projectiles.external_latency_updated_at = now;
    }
    ReleaseSRWLockExclusive(&g_projectiles.lock);
}

/**
 * @brief Считывает внутреннюю сетевую задержку для временного выравнивания угроз, не для
 * задания задержки OFF→ON.
 */
bool poll_internal_network_latency() {
    // `m_flLastUnitOrdersTotalLatency` is an order acknowledgement/processing
    // duration. It includes local frame and tick scheduling, so it reports
    // roughly 20-30 ms even for an in-process loopback server and must not be
    // used as network ping.
    //
    // Source 2's own cl_net_printsummary reads two 0xE0-byte direction records
    // from CNetworkGameClient. Resolve the outer object and the build-dependent
    // record/loopback offsets from code signatures, then consume its P50 ping.
    struct network_layout_cache {
        std::uintptr_t engine_base = 0;
        std::size_t engine_size = 0;
        std::uintptr_t network_client_global = 0;
        std::ptrdiff_t summary_base = 0;
        std::ptrdiff_t loopback_flag = 0;
        ULONGLONG retry_at = 0;
    };
    static network_layout_cache cache{};

    constexpr std::array<std::uint8_t, 9> network_client_pattern{
        0x48, 0x89, 0x3D, 0, 0, 0, 0, 0xFF, 0x87,
    };
    constexpr char network_client_mask[] = "xxx????xx";
    constexpr std::array<std::uint8_t, 29> summary_layout_pattern{
        0x8B, 0x9E, 0, 0, 0, 0,
        0xBA, 0x02, 0x00, 0x00, 0x00,
        0x8B, 0x0D, 0, 0, 0, 0,
        0x85, 0xDB, 0x75, 0,
        0x38, 0x9E, 0, 0, 0, 0,
        0x74, 0,
    };
    constexpr char summary_layout_mask[] =
        "xx????xxxxxxx????xxx?xx????x?";
    constexpr std::ptrdiff_t summary_duration_within_record = 0x28;
    constexpr std::ptrdiff_t summary_stride = 0xE0;
    constexpr std::ptrdiff_t summary_flags = 0x14;
    constexpr std::ptrdiff_t summary_ping_p50 = 0xD4;
    constexpr std::uint32_t summary_has_ping_p50 = 1u << 10;

    const ULONGLONG now = GetTickCount64();
    const auto engine = reinterpret_cast<std::uintptr_t>(
        GetModuleHandleW(L"engine2.dll"));
    if (!plausible_pointer(engine))
        return false;
    if (cache.engine_base != engine) {
        cache = {};
        cache.engine_base = engine;
        cache.engine_size = image_size(engine);
    }
    if (cache.engine_size == 0)
        return false;

    if (cache.network_client_global == 0 || cache.summary_base <= 0 ||
        cache.loopback_flag <= 0) {
        if (now < cache.retry_at)
            return false;
        cache.retry_at = now + 1000;

        const auto client_instruction = find_unique_executable_pattern(
            engine, cache.engine_size, network_client_pattern.data(),
            network_client_mask, network_client_pattern.size());
        const auto layout_instruction = find_unique_executable_pattern(
            engine, cache.engine_size, summary_layout_pattern.data(),
            summary_layout_mask, summary_layout_pattern.size());
        std::uint32_t duration_offset = 0;
        std::uint32_t loopback_offset = 0;
        const auto client_global = resolve_rip_relative_global(
            client_instruction, 3, 7, engine, cache.engine_size);
        if (client_global == 0 || layout_instruction == 0 ||
            !safe_read(layout_instruction + 2, duration_offset) ||
            !safe_read(layout_instruction + 23, loopback_offset) ||
            duration_offset <= summary_duration_within_record ||
            duration_offset > 0x10000000 || loopback_offset == 0 ||
            loopback_offset > 0x10000000) {
            cache.network_client_global = 0;
            cache.summary_base = 0;
            cache.loopback_flag = 0;
            return false;
        }
        cache.network_client_global = client_global;
        cache.summary_base = static_cast<std::ptrdiff_t>(duration_offset) -
            summary_duration_within_record;
        cache.loopback_flag = static_cast<std::ptrdiff_t>(loopback_offset);
    }

    std::uintptr_t network_client = 0;
    bool loopback = false;
    if (!safe_read(cache.network_client_global, network_client) ||
        !plausible_pointer(network_client) ||
        !safe_read(network_client + cache.loopback_flag, loopback))
        return false;

    float latency_ms = -1.0f;
    {
        std::uint32_t summary_seconds = 0;
        const bool summary_valid = safe_read(network_client + cache.summary_base +
                summary_duration_within_record, summary_seconds) &&
            summary_seconds > 0 && summary_seconds <= 86400;

        std::uint32_t maximum_p50 = 0;
        bool found_p50 = false;
        for (int direction = 0; summary_valid && direction < 2; ++direction) {
            const auto record = network_client + cache.summary_base +
                static_cast<std::ptrdiff_t>(direction) * summary_stride;
            std::uint32_t flags = 0;
            std::uint32_t ping_p50 = 0;
            if (!safe_read(record + summary_flags, flags) ||
                (flags & summary_has_ping_p50) == 0 ||
                !safe_read(record + summary_ping_p50, ping_p50) ||
                ping_p50 > 5000)
                continue;
            maximum_p50 = std::max(maximum_p50, ping_p50);
            found_p50 = true;
        }
        if (!found_p50 && !loopback)
            return false;
        // Read measured latency even for a local server: loopback alone does
        // not prove zero delay when the engine's packet emulator is enabled.
        // A local server without a summary still has a valid zero baseline.
        latency_ms = static_cast<float>(maximum_p50);
    }

    if (!std::isfinite(latency_ms) || latency_ms < 0.0f || latency_ms > 5000.0f)
        return false;

    constexpr ULONGLONG external_override_ms = 1000;
    AcquireSRWLockExclusive(&g_projectiles.lock);
    const bool external_is_fresh =
        g_projectiles.external_latency_updated_at != 0 &&
        now >= g_projectiles.external_latency_updated_at &&
        now - g_projectiles.external_latency_updated_at <= external_override_ms;
    if (!external_is_fresh) {
        g_projectiles.network_latency_ms = latency_ms;
        g_projectiles.latency_updated_at = now;
    }
    ReleaseSRWLockExclusive(&g_projectiles.lock);
    return true;
}

/**
 * @brief Нормализует входное событие в ограниченный общий буфер угроз под exclusive-lock.
 * Идентификатор используется для обновления/удаления события.
 */
void ingest_released_attack(const void* attack_record) {
    incoming_projectile incoming{};
    if (!read_attack_record(attack_record, incoming, false))
        return;

    const ULONGLONG now = GetTickCount64();
    incoming.active = true;
    incoming.kind = incoming.ranged_attack && incoming.use_projectile
        ? projectile_kind::released_attack : projectile_kind::instant_attack;
    incoming.projectile_id = (static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(incoming.attack_record)) << 32) ^
        reinterpret_cast<std::uintptr_t>(attack_record);
    incoming.updated_at = now;
    if (incoming.kind == projectile_kind::instant_attack)
        incoming.impact_at = now;
    std::uintptr_t entity_system = 0;
    const auto client_base = g_client_base_address.load(std::memory_order_relaxed);
    if (client_base != 0 && read_game_entity_system(client_base, entity_system)) {
        incoming.source_entity = entity_by_handle(entity_system, incoming.source_handle);
        incoming.position_valid = read_world_position(
            incoming.source_entity, incoming.position);
    }

    AcquireSRWLockExclusive(&g_projectiles.lock);
    g_projectiles.heartbeat = now;
    if (incoming.kind == projectile_kind::instant_attack) {
        for (auto& entry : g_projectiles.entries) {
            if (entry.active && entry.kind == projectile_kind::melee_animation &&
                entry.source_entity == incoming.source_entity)
                entry.active = false;
        }
    }
    auto* slot = select_projectile_slot(incoming.projectile_id, incoming.kind, now);
    *slot = incoming;
    ReleaseSRWLockExclusive(&g_projectiles.lock);
}

/**
 * @brief Нормализует входное событие в ограниченный общий буфер угроз под exclusive-lock.
 * Идентификатор используется для обновления/удаления события.
 */
void ingest_tracking_projectile(std::uint64_t projectile_id,
    const void* attack_record, armlet_vec3 current_position) {
    incoming_projectile incoming{};
    if (!read_attack_record(attack_record, incoming, true) ||
        !std::isfinite(current_position.x) || !std::isfinite(current_position.y) ||
        !std::isfinite(current_position.z))
        return;

    const ULONGLONG now = GetTickCount64();
    incoming.active = true;
    incoming.kind = projectile_kind::tracking;
    incoming.projectile_id = projectile_id;
    incoming.position = current_position;
    incoming.position_valid = true;
    incoming.updated_at = now;

    AcquireSRWLockExclusive(&g_projectiles.lock);
    g_projectiles.heartbeat = now;
    for (auto& entry : g_projectiles.entries) {
        if (entry.active && entry.kind == projectile_kind::released_attack &&
            entry.attack_record == incoming.attack_record)
            entry.active = false;
    }
    auto* slot = select_projectile_slot(projectile_id, incoming.kind, now);
    *slot = incoming;
    ReleaseSRWLockExclusive(&g_projectiles.lock);
}

/**
 * @brief Нормализует входное событие в ограниченный общий буфер угроз под exclusive-lock.
 * Идентификатор используется для обновления/удаления события.
 */
void ingest_linear_projectile(std::uint64_t projectile_id,
    std::uintptr_t source_entity, float original_damage, float physical_damage,
    float magical_damage, float pure_damage, float composite_damage,
    armlet_vec3 current_position, armlet_vec3 velocity, float projectile_radius,
    float remaining_distance) {
    const float speed = length(velocity);
    if (!plausible_pointer(source_entity) || speed <= 1.0f ||
        !finite_nonnegative(original_damage) || !finite_nonnegative(physical_damage) ||
        !finite_nonnegative(magical_damage) || !finite_nonnegative(pure_damage) ||
        !finite_nonnegative(composite_damage) || !finite_nonnegative(projectile_radius) ||
        !finite_nonnegative(remaining_distance))
        return;

    const ULONGLONG now = GetTickCount64();
    incoming_projectile incoming{};
    incoming.active = true;
    incoming.kind = projectile_kind::linear;
    incoming.projectile_id = projectile_id;
    incoming.source_entity = source_entity;
    incoming.position = current_position;
    incoming.position_valid = true;
    incoming.velocity = velocity;
    incoming.speed = speed;
    incoming.radius = projectile_radius;
    incoming.remaining_distance = remaining_distance;
    incoming.original_damage = original_damage;
    incoming.physical_damage = physical_damage;
    incoming.magical_damage = magical_damage;
    incoming.pure_damage = pure_damage;
    incoming.composite_damage = composite_damage;
    incoming.damage_complete = original_damage > 0.0f || physical_damage > 0.0f ||
        magical_damage > 0.0f || pure_damage > 0.0f || composite_damage > 0.0f;
    incoming.updated_at = now;

    AcquireSRWLockExclusive(&g_projectiles.lock);
    g_projectiles.heartbeat = now;
    auto* slot = select_projectile_slot(projectile_id, incoming.kind, now);
    *slot = incoming;
    ReleaseSRWLockExclusive(&g_projectiles.lock);
}

/**
 * @brief Нормализует входное событие в ограниченный общий буфер угроз под exclusive-lock.
 * Идентификатор используется для обновления/удаления события.
 */
void ingest_external_damage(std::uint64_t event_id, std::uintptr_t source_entity,
    float delay_seconds, float original_damage, float physical_damage,
    float magical_damage, float pure_damage, float composite_damage,
    float confidence) {
    if (!plausible_pointer(source_entity) || !finite_nonnegative(delay_seconds) ||
        delay_seconds > 10.0f || !finite_nonnegative(original_damage) ||
        !finite_nonnegative(physical_damage) || !finite_nonnegative(magical_damage) ||
        !finite_nonnegative(pure_damage) || !finite_nonnegative(composite_damage) ||
        !std::isfinite(confidence))
        return;

    const ULONGLONG now = GetTickCount64();
    incoming_projectile incoming{};
    incoming.active = true;
    incoming.kind = projectile_kind::external_damage;
    incoming.projectile_id = event_id;
    incoming.source_entity = source_entity;
    incoming.original_damage = original_damage;
    incoming.physical_damage = physical_damage;
    incoming.magical_damage = magical_damage;
    incoming.pure_damage = pure_damage;
    incoming.composite_damage = composite_damage;
    incoming.damage_complete = original_damage > 0.0f || physical_damage > 0.0f ||
        magical_damage > 0.0f || pure_damage > 0.0f || composite_damage > 0.0f;
    incoming.confidence = std::clamp(confidence, 0.0f, 1.0f);
    incoming.impact_at = now + static_cast<ULONGLONG>(delay_seconds * 1000.0f);
    incoming.updated_at = now;

    AcquireSRWLockExclusive(&g_projectiles.lock);
    g_projectiles.heartbeat = now;
    auto* slot = select_projectile_slot(event_id, incoming.kind, now);
    *slot = incoming;
    ReleaseSRWLockExclusive(&g_projectiles.lock);
}

/**
 * @brief Удаляет событие по его ID из общего буфера; вызывается также адаптерами внешней
 * телеметрии.
 */
void remove_projectile(std::uint64_t projectile_id) {
    AcquireSRWLockExclusive(&g_projectiles.lock);
    g_projectiles.heartbeat = GetTickCount64();
    for (auto& entry : g_projectiles.entries) {
        if (entry.active && entry.projectile_id == projectile_id)
            entry.active = false;
    }
    ReleaseSRWLockExclusive(&g_projectiles.lock);
}

bool read_conservative_source_attack_damage(std::uintptr_t source,
    float& estimated_damage) {
    int damage_max = 0;
    int base_damage_max = 0;
    int damage_bonus = 0;
    if (!plausible_pointer(source) ||
        !safe_read(source + schema_fields::m_iDamageMax, damage_max) ||
        !safe_read(source + schema_fields::m_iBaseDamageMax, base_damage_max) ||
        !safe_read(source + schema_fields::m_iDamageBonus, damage_bonus) ||
        damage_max < 0 || damage_max > 100000 ||
        base_damage_max < 0 || base_damage_max > 100000 ||
        damage_bonus < -100000 || damage_bonus > 100000)
        return false;

    const int raw = std::max(damage_max, base_damage_max) +
        std::max(0, damage_bonus);
    if (raw <= 0 || raw > 100000)
        return false;

    // Crit/orb/proc state is not available in the tracking object. Keep the
    // raw pre-mitigation maximum and inflate it instead of pretending that the
    // armor-reduced estimate is exact.
    estimated_damage = static_cast<float>(raw) * 1.45f;
    return finite_nonnegative(estimated_damage) && estimated_damage > 0.0f;
}

void refresh_enemy_melee_source_cache(std::uintptr_t entity_system,
    std::uint8_t hero_team, ULONGLONG now) {
    constexpr ULONGLONG refresh_interval_ms = 500;
    if (g_melee_source_cache.local_team == hero_team &&
        g_melee_source_cache.refreshed_at != 0 &&
        now >= g_melee_source_cache.refreshed_at &&
        now - g_melee_source_cache.refreshed_at < refresh_interval_ms)
        return;

    g_melee_source_cache.count = 0;
    g_melee_source_cache.local_team = hero_team;
    visit_entities(entity_system, [&](std::uintptr_t entity, int) {
        std::uint8_t team = 0;
        int health = 0;
        int max_health = 0;
        int attack_capabilities = 0;
        int attack_range = 0;
        float base_attack_time = 0.0f;
        char designer_name[96]{};
        if (!safe_read(entity + schema_fields::m_iTeamNum, team) || team == 0 ||
            team == hero_team ||
            !safe_read(entity + schema_fields::m_iHealth, health) || health <= 0 ||
            !safe_read(entity + schema_fields::m_iMaxHealth, max_health) || max_health <= 0 ||
            !safe_read(entity + schema_fields::m_iAttackCapabilities, attack_capabilities) ||
            attack_capabilities != 1 ||
            !safe_read(entity + schema_fields::m_iAttackRange, attack_range) ||
            attack_range < 0 || attack_range > 800 ||
            !safe_read(entity + schema_fields::m_flBaseAttackTime, base_attack_time) ||
            !std::isfinite(base_attack_time) || base_attack_time < 0.1f ||
            base_attack_time > 10.0f ||
            !read_designer_name(entity, designer_name) ||
            std::strncmp(designer_name, "npc_dota_", 9) != 0)
            return false;

        if (g_melee_source_cache.count < g_melee_source_cache.entries.size())
            g_melee_source_cache.entries[g_melee_source_cache.count++] = entity;
        return false;
    });
    g_melee_source_cache.refreshed_at = now;
}

/**
 * @brief Собирает соответствующий внутренний источник угроз. Свежесть и полнота наблюдения
 * передаются оценщику; отсутствие валидного feed не является доказательством безопасности.
 */
int poll_internal_melee_attack_feed(std::uintptr_t client_base) {
    // Melee attack records are normally published at release/impact, which is
    // too late for an Armlet cycle. m_flLastAttackTime changes at the start of
    // the replicated attack gesture, so keep a conservative synthetic hit in
    // the timeline for the whole possible wind-up.
    constexpr float melee_reach_grace = 96.0f;
    constexpr float post_windup_grace_seconds = 0.20f;
    struct source_history {
        std::uintptr_t entity=0;
        std::uint32_t handle=UINT32_MAX;
        ULONGLONG seen=0;
        melee_contact_history contact;
    };
    static std::array<source_history,128> histories{};
    static std::uintptr_t history_hero=0;
    static float history_time=0;

    const auto hero = g_monitor.hero_address.load(std::memory_order_relaxed);
    std::uintptr_t entity_system = 0;
    std::uint8_t hero_team = 0;
    float game_time = 0.0f;
    float hero_last_damage_time = 0.0f;
    armlet_vec3 hero_position{};
    float hero_radius = 0.0f;
    if (!plausible_pointer(hero) ||
        !safe_read(hero + schema_fields::m_iTeamNum, hero_team) || hero_team == 0 ||
        !safe_read(hero + schema_fields::m_flHullRadius, hero_radius) ||
        !finite_nonnegative(hero_radius) ||
        !read_game_entity_system(client_base, entity_system) ||
        !read_game_time(client_base, game_time) ||
        !safe_read(hero + schema_fields::m_flLastDamageTime,
            hero_last_damage_time) ||
        !std::isfinite(hero_last_damage_time) ||
        !read_world_position(hero, hero_position)) {
        g_melee_source_cache.count = 0;
        g_melee_source_cache.refreshed_at = 0;
        g_monitor.internal_melee_count.store(0, std::memory_order_relaxed);
        return 0;
    }

    const ULONGLONG now = GetTickCount64();
    if(history_hero!=hero || game_time<history_time) {
        histories={};history_hero=hero;
    }
    history_time=game_time;
    refresh_enemy_melee_source_cache(entity_system, hero_team, now);

    std::array<incoming_projectile, 128> updates{};
    std::size_t update_count = 0;
    for (std::size_t index = 0; index < g_melee_source_cache.count; ++index) {
        const auto source = g_melee_source_cache.entries[index];
        std::uint8_t source_team = 0;
        int health = 0;
        int attack_capabilities = 0;
        int attack_range = 0;
        float base_attack_time = 0.0f;
        float last_attack_time = 0.0f;
        float last_dealt_damage_time = 0.0f;
        float source_radius = 0.0f;
        armlet_vec3 source_position{};
        if (!plausible_pointer(source) ||
            !safe_read(source + schema_fields::m_iTeamNum, source_team) ||
            source_team == 0 || source_team == hero_team ||
            !safe_read(source + schema_fields::m_iHealth, health) || health <= 0 ||
            !safe_read(source + schema_fields::m_iAttackCapabilities,
                attack_capabilities) || attack_capabilities != 1 ||
            !safe_read(source + schema_fields::m_iAttackRange, attack_range) ||
            attack_range < 0 || attack_range > 800 ||
            !safe_read(source + schema_fields::m_flBaseAttackTime, base_attack_time) ||
            !std::isfinite(base_attack_time) || base_attack_time < 0.1f ||
            base_attack_time > 10.0f ||
            !safe_read(source + schema_fields::m_flLastAttackTime, last_attack_time) ||
            !std::isfinite(last_attack_time) || last_attack_time <= 0.0f ||
            !safe_read(source + schema_fields::m_flLastDealtDamageTime,
                last_dealt_damage_time) ||
            !std::isfinite(last_dealt_damage_time) ||
            !safe_read(source + schema_fields::m_flHullRadius, source_radius) ||
            !finite_nonnegative(source_radius) ||
            !read_world_position(source, source_position))
            continue;

        const float age_seconds = game_time - last_attack_time;
        // BAT is useful only as a lifetime bound. It cannot identify the hit:
        // current attack speed scales the animation (Roshan has 200 AS), so a
        // BAT-derived impact opens the window close to the next swing. Confirm
        // contact by matching the attacker's last dealt-damage timestamp with
        // the local hero's last received-damage timestamp instead.
        const float fallback_attack_point = std::clamp(
            base_attack_time * 0.45f, 0.35f, 0.85f);
        const float windup_window = fallback_attack_point +
            post_windup_grace_seconds;
        if (!std::isfinite(age_seconds) || age_seconds < -0.10f ||
            age_seconds > windup_window)
            continue;

        const float distance = length(subtract(hero_position, source_position));
        const float melee_reach = static_cast<float>(attack_range) +
            source_radius + hero_radius + melee_reach_grace;
        if (!finite_nonnegative(distance) || distance > melee_reach)
            continue;

        float estimated_damage = 0.0f;
        if (!read_conservative_source_attack_damage(source, estimated_damage))
            estimated_damage = 1000.0f;

        std::uint32_t attack_stamp = 0;
        std::memcpy(&attack_stamp, &last_attack_time, sizeof(attack_stamp));
        incoming_projectile incoming{};
        incoming.active = true;
        incoming.damage_complete = true;
        incoming.is_attack = true;
        incoming.kind = projectile_kind::melee_animation;
        incoming.projectile_id = 0x4D454C4500000000ULL ^
            static_cast<std::uint64_t>(source) ^
            (static_cast<std::uint64_t>(attack_stamp) << 21);
        incoming.source_entity = source;
        incoming.position = source_position;
        incoming.position_valid = true;
        incoming.original_damage = estimated_damage;
        incoming.physical_damage = estimated_damage;
        incoming.confidence = 0.60f;
        incoming.source_attack_at=now-static_cast<ULONGLONG>(std::min<double>(
            static_cast<double>(now),std::max(0.,double(age_seconds)*1000.)));
        std::uint32_t source_handle=UINT32_MAX;
        if(!effect_target_handle(entity_system,source,source_handle))continue;
        source_history* history=nullptr;
        for(auto& entry:histories)if(entry.entity==source && entry.handle==source_handle) {
            history=&entry;break;
        }
        if(!history) {
            history=&*std::min_element(histories.begin(),histories.end(),
                [](const auto& a,const auto& b){return a.seen<b.seen;});
            *history={};history->entity=source;history->handle=source_handle;
        }
        history->seen=now;
        const bool impact_confirmed=history->contact.observe(last_attack_time,
            last_dealt_damage_time,hero_last_damage_time,game_time);
        const float confirmed_impact_time=history->contact.contact_at;
        if (!impact_confirmed) {
            // Keep an unresolved swing immediately ahead of server time. This
            // is intentionally refreshed until damage is confirmed; unlike the
            // old ETA guess it cannot expire while the weapon is still moving.
            incoming.impact_at = now + 50;
        } else {
            const float seconds_after_impact = std::max(
                0.0f, game_time - confirmed_impact_time);
            const auto elapsed_after_impact = static_cast<ULONGLONG>(
                std::ceil(seconds_after_impact * 1000.0f));
            incoming.impact_at = elapsed_after_impact < now
                ? now - elapsed_after_impact : 0;
        }
        incoming.updated_at = now;
        updates[update_count++] = incoming;
    }

    int inserted_count = 0;
    AcquireSRWLockExclusive(&g_projectiles.lock);
    for (std::size_t index = 0; index < update_count; ++index) {
        bool confirmed = false;
        for (const auto& entry : g_projectiles.entries) {
            if (entry.active && entry.kind == projectile_kind::instant_attack &&
                entry.source_entity == updates[index].source_entity &&
                instant_contact_covers_swing(entry.updated_at,
                    updates[index].source_attack_at,now)) {
                confirmed = true;
                break;
            }
        }
        if (confirmed)
            continue;
        auto* slot = select_projectile_slot(
            updates[index].projectile_id, updates[index].kind, now);
        *slot = updates[index];
        ++inserted_count;
    }
    ReleaseSRWLockExclusive(&g_projectiles.lock);

    g_monitor.internal_melee_count.store(inserted_count,
        std::memory_order_relaxed);
    return inserted_count;
}

bool is_tower_entity(std::uintptr_t entity, std::uintptr_t client_base,
    std::size_t client_size) {
    if (!plausible_pointer(entity))
        return false;

    char designer_name[96]{};
    if (read_designer_name(entity, designer_name) &&
        std::strstr(designer_name, "tower") != nullptr)
        return true;

    return client_size != 0 &&
        rtti_name_contains(entity, client_base, client_size,
            "C_DOTA_BaseNPC_Tower");
}

void refresh_enemy_tower_cache(std::uintptr_t client_base,
    std::size_t client_size, std::uintptr_t entity_system,
    std::uint8_t hero_team, ULONGLONG now) {
    constexpr ULONGLONG refresh_interval_ms = 1000;
    if (g_tower_cache.entity_system == entity_system &&
        g_tower_cache.local_team == hero_team &&
        g_tower_cache.refreshed_at != 0 && now >= g_tower_cache.refreshed_at &&
        now - g_tower_cache.refreshed_at < refresh_interval_ms)
        return;

    g_tower_cache.count = 0;
    g_tower_cache.entity_system = entity_system;
    g_tower_cache.local_team = hero_team;
    int rtti_tower_count = 0;
    visit_entities(entity_system, [&](std::uintptr_t entity, int) {
        std::uint8_t team = 0;
        char designer_name[96]{};
        if (!safe_read(entity + schema_fields::m_iTeamNum, team) || team == 0 ||
            team == hero_team)
            return false;

        const bool named_tower = read_designer_name(entity, designer_name) &&
            std::strstr(designer_name, "tower") != nullptr;
        const bool typed_tower = !named_tower && client_size != 0 &&
            rtti_name_contains(entity, client_base, client_size,
                "C_DOTA_BaseNPC_Tower");
        if (!named_tower && !typed_tower)
            return false;
        if (typed_tower)
            ++rtti_tower_count;

        // Cosmetic/custom towers may expose prop_dynamic_clientside as their
        // designer name. Runtime type identity keeps the tower feed working.

        if (g_tower_cache.count < g_tower_cache.entries.size())
            g_tower_cache.entries[g_tower_cache.count++] = entity;
        return false;
    });
    g_tower_cache.refreshed_at = now;
    g_render_diagnostics.cached_towers.store(
        static_cast<int>(g_tower_cache.count), std::memory_order_relaxed);
    g_render_diagnostics.rtti_towers.store(
        rtti_tower_count, std::memory_order_relaxed);
}

/**
 * @brief Собирает соответствующий внутренний источник угроз. Свежесть и полнота наблюдения
 * передаются оценщику; отсутствие валидного feed не является доказательством безопасности.
 */
int poll_internal_tower_attack_feed(std::uintptr_t client_base,
    std::size_t client_size) {
    // Basic attacks from lane towers are not guaranteed to appear in
    // C_DOTA_ProjectileManager's tracking array. Reconstruct their in-flight
    // window from the tower's networked target and last-attack timestamp.
    constexpr float tower_projectile_speed = 750.0f;
    constexpr float tower_attack_point_and_network_grace = 0.55f;

    if (schema_fields::m_hTowerAttackTarget <= 0 &&
        schema_fields::m_hPrevTowerAttackTarget <= 0) {
        g_monitor.internal_tower_count.store(0, std::memory_order_relaxed);
        g_render_diagnostics.targeted_towers.store(0, std::memory_order_relaxed);
        return 0;
    }

    const auto hero = g_monitor.hero_address.load(std::memory_order_relaxed);
    std::uint32_t hero_handle = 0xFFFFFFFFu;
    const auto controller = controller_from_accessor();
    if (plausible_pointer(controller))
        safe_read(controller + schema_fields::controller_m_hAssignedHero,
            hero_handle);
    std::uintptr_t entity_system = 0;
    std::uint8_t hero_team = 0;
    float game_time = 0.0f;
    if (!plausible_pointer(hero) ||
        !safe_read(hero + schema_fields::m_iTeamNum, hero_team) || hero_team == 0 ||
        !read_game_entity_system(client_base, entity_system) ||
        !read_game_time(client_base, game_time)) {
        g_tower_cache = {};
        g_monitor.internal_tower_count.store(0, std::memory_order_relaxed);
        g_render_diagnostics.cached_towers.store(0, std::memory_order_relaxed);
        g_render_diagnostics.targeted_towers.store(0, std::memory_order_relaxed);
        return 0;
    }

    const ULONGLONG now = GetTickCount64();
    refresh_enemy_tower_cache(
        client_base, client_size, entity_system, hero_team, now);

    armlet_vec3 hero_position{};
    if (!read_world_position(hero, hero_position)) {
        g_monitor.internal_tower_count.store(0, std::memory_order_relaxed);
        return 0;
    }

    int count = 0;
    int targeted_count = 0;
    for (std::size_t index = 0; index < g_tower_cache.count; ++index) {
        const auto tower = g_tower_cache.entries[index];
        std::uint32_t current_target = 0xFFFFFFFFu;
        std::uint32_t previous_target = 0xFFFFFFFFu;
        float last_attack_time = 0.0f;
        armlet_vec3 tower_position{};
        const bool current_readable = schema_fields::m_hTowerAttackTarget > 0 &&
            safe_read(tower + schema_fields::m_hTowerAttackTarget, current_target);
        const bool previous_readable = schema_fields::m_hPrevTowerAttackTarget > 0 &&
            safe_read(tower + schema_fields::m_hPrevTowerAttackTarget, previous_target);
        if (!plausible_pointer(tower) ||
            (!current_readable && !previous_readable) ||
            !safe_read(tower + schema_fields::m_flLastAttackTime, last_attack_time) ||
            !std::isfinite(last_attack_time) ||
            !read_world_position(tower, tower_position))
            continue;

        std::uint32_t target_handle = 0xFFFFFFFFu;
        const auto current_target_entity =
            entity_by_handle(entity_system, current_target);
        const bool current_is_hero =
            (hero_handle != 0xFFFFFFFFu &&
                same_entity_handle(current_target, hero_handle)) ||
            current_target_entity == hero;
        const bool previous_is_hero =
            (hero_handle != 0xFFFFFFFFu &&
                same_entity_handle(previous_target, hero_handle)) ||
            entity_by_handle(entity_system, previous_target) == hero;
        if (current_is_hero)
            target_handle = current_target;
        // m_hPrevTowerAttackTarget may still name the hero while the tower is
        // already attacking a creep. Use it only after the current target has
        // actually cleared; otherwise a new creep shot is fabricated as a hero
        // projectile and recreates the old "inside tower radius" anchor.
        else if (!plausible_pointer(current_target_entity) && previous_is_hero)
            target_handle = previous_target;
        if (target_handle == 0xFFFFFFFFu)
            continue;
        ++targeted_count;

        const float age_seconds = game_time - last_attack_time;
        const float distance = length(subtract(hero_position, tower_position));
        if (!std::isfinite(age_seconds) || age_seconds < -0.10f ||
            !finite_nonnegative(distance))
            continue;
        const float in_flight_window = std::clamp(
            distance / tower_projectile_speed +
                tower_attack_point_and_network_grace,
            0.20f, 2.50f);
        if (age_seconds > in_flight_window)
            continue;

        float estimated_damage = 0.0f;
        if (!read_conservative_source_attack_damage(tower, estimated_damage)) {
            char tower_name[96]{};
            if (!read_designer_name(tower, tower_name))
                tower_name[0] = '\0';
            if (std::strstr(tower_name, "tower4") != nullptr)
                estimated_damage = 340.0f;
            else if (std::strstr(tower_name, "tower3") != nullptr)
                estimated_damage = 285.0f;
            else if (std::strstr(tower_name, "tower2") != nullptr)
                estimated_damage = 230.0f;
            else
                estimated_damage = 165.0f;
        }

        std::uint32_t attack_stamp = 0;
        std::memcpy(&attack_stamp, &last_attack_time, sizeof(attack_stamp));
        incoming_projectile incoming{};
        incoming.active = true;
        incoming.kind = projectile_kind::tower_attack;
        incoming.projectile_id =
            (static_cast<std::uint64_t>(tower) << 17) ^ attack_stamp;
        incoming.source_entity = tower;
        incoming.target_handle = target_handle;
        incoming.position = tower_position;
        incoming.position_valid = true;
        incoming.speed = tower_projectile_speed;
        incoming.original_damage = estimated_damage;
        incoming.physical_damage = estimated_damage;
        incoming.damage_complete = true;
        incoming.confidence = 0.70f;
        // Use the actual travel estimate for safety decisions.  The extra
        // grace above only keeps the synthetic record alive while the target
        // handle changes; counting it as travel time delayed abuse by ~550 ms.
        // Subtracting the full attack age is deliberately conservative when
        // m_flLastAttackTime denotes wind-up rather than projectile release.
        // m_flLastAttackTime is the beginning of the tower's attack gesture,
        // not projectile release. Omitting its attack point expired the record
        // roughly half a second before the visible projectile actually landed.
        constexpr float tower_attack_point = 0.50f;
        const float seconds_until_impact = tower_attack_point +
            distance / tower_projectile_speed - std::max(0.0f, age_seconds);
        if (seconds_until_impact >= 0.0f) {
            incoming.impact_at = now + static_cast<ULONGLONG>(
                std::ceil(seconds_until_impact * 1000.0f));
        } else {
            // Keep the calculated impact timestamp fixed in the past. Clamping
            // to `now` here made every 5 ms poll extend an already landed tower
            // projectile and erased the safe gap between consecutive attacks.
            const auto elapsed_after_impact = static_cast<ULONGLONG>(
                std::ceil(-seconds_until_impact * 1000.0f));
            incoming.impact_at = elapsed_after_impact < now
                ? now - elapsed_after_impact : 0;
        }
        incoming.updated_at = now;

        AcquireSRWLockExclusive(&g_projectiles.lock);
        auto* slot = select_projectile_slot(
            incoming.projectile_id, incoming.kind, now);
        const bool new_shot = !slot->active ||
            slot->projectile_id != incoming.projectile_id ||
            slot->kind != incoming.kind;
        *slot = incoming;
        ReleaseSRWLockExclusive(&g_projectiles.lock);
        if (new_shot)
            g_render_diagnostics.tower_shots.fetch_add(
                1, std::memory_order_relaxed);
        ++count;
    }

    g_monitor.internal_tower_count.store(count, std::memory_order_relaxed);
    g_render_diagnostics.targeted_towers.store(
        targeted_count, std::memory_order_relaxed);
    static int last_logged_tower_count = -1;
    if (count != last_logged_tower_count) {
        char message[96]{};
        std::snprintf(message, sizeof(message),
            "Tower threat feed: %d active projectile(s).", count);
        log_message("TOWER", message);
        last_logged_tower_count = count;
    }
    return count;
}

/**
 * @brief Собирает соответствующий внутренний источник угроз. Свежесть и полнота наблюдения
 * передаются оценщику; отсутствие валидного feed не является доказательством безопасности.
 */
bool poll_internal_projectile_feed(std::uintptr_t client_base,
    std::size_t client_size) {
    constexpr std::ptrdiff_t tracking_slots = 0x38;
    constexpr std::ptrdiff_t linear_slots = 0x2038;
    constexpr std::size_t slot_count = 1024;
    constexpr float conservative_linear_radius = 150.0f;

    const auto manager_global = g_projectile_manager_global.load(
        std::memory_order_acquire);
    std::uintptr_t manager = 0;
    std::uintptr_t manager_vtable = 0;
    std::uintptr_t entity_system = 0;
    if (!plausible_pointer(manager_global) || !safe_read(manager_global, manager) ||
        !plausible_pointer(manager) || !safe_read(manager, manager_vtable) ||
        manager_vtable < client_base || manager_vtable >= client_base + client_size ||
        !read_game_entity_system(client_base, entity_system)) {
        g_monitor.internal_projectile_feed_ready.store(false,
            std::memory_order_relaxed);
        g_monitor.internal_tracking_count.store(0, std::memory_order_relaxed);
        g_monitor.internal_linear_count.store(0, std::memory_order_relaxed);
        return false;
    }

    const auto hero = g_monitor.hero_address.load(std::memory_order_relaxed);
    std::uint8_t hero_team = 0;
    const bool hero_ready = plausible_pointer(hero) &&
        safe_read(hero + schema_fields::m_iTeamNum, hero_team) && hero_team != 0;

    std::array<incoming_projectile, 128> updates{};
    std::array<std::uintptr_t, slot_count> tracking_snapshot{};
    std::array<std::uintptr_t, slot_count> linear_snapshot{};
    std::size_t update_count = 0;
    int tracking_count = 0;
    int linear_count = 0;
    bool overflow = false;

    if (!safe_read(manager + tracking_slots, tracking_snapshot) ||
        !safe_read(manager + linear_slots, linear_snapshot)) {
        g_monitor.internal_projectile_feed_ready.store(false,
            std::memory_order_relaxed);
        return false;
    }

    for (std::size_t index = 0; index < slot_count; ++index) {
        const auto slot_address = manager + tracking_slots + index * sizeof(void*);
        const auto projectile_first = tracking_snapshot[index];
        if (!plausible_pointer(projectile_first))
            continue;

        std::uint32_t id_first = 0;
        std::uint32_t id_second = 0;
        std::uint32_t source_handle = 0xFFFFFFFFu;
        std::uint32_t target_handle = 0xFFFFFFFFu;
        std::uint32_t ability_handle = 0xFFFFFFFFu;
        int original_speed = 0;
        int current_speed = 0;
        armlet_vec3 position{};
        std::uint8_t is_attack = 0;
        if (!safe_read(projectile_first + 0x00, id_first) || id_first == 0 ||
            !safe_read(projectile_first + 0x04, original_speed) ||
            !safe_read(projectile_first + 0x08, current_speed) ||
            !safe_read(projectile_first + 0x0C, position) ||
            !safe_read(projectile_first + 0x18, source_handle) ||
            !safe_read(projectile_first + 0x1C, target_handle) ||
            !safe_read(projectile_first + 0x20, ability_handle) ||
            !safe_read(projectile_first + 0x35, is_attack) ||
            !safe_read(projectile_first + 0x00, id_second))
            continue;
        std::uintptr_t projectile_second = 0;
        if (!safe_read(slot_address, projectile_second) ||
            projectile_first != projectile_second || id_first != id_second)
            continue;

        const int speed = std::max(original_speed, current_speed);
        if (speed <= 1 || speed >= 100000 ||
            !std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z) || !hero_ready ||
            entity_by_handle(entity_system, target_handle) != hero)
            continue;

        const auto source = entity_by_handle(entity_system, source_handle);
        std::uint8_t source_team = 0;
        if (!plausible_pointer(source) ||
            !safe_read(source + schema_fields::m_iTeamNum, source_team) ||
            source_team == 0 || source_team == hero_team)
            continue;

        if (update_count >= updates.size()) {
            overflow = true;
            break;
        }

        incoming_projectile incoming{};
        incoming.active = true;
        incoming.position_valid = true;
        incoming.is_attack = is_attack != 0;
        incoming.ranged_attack = incoming.is_attack;
        incoming.use_projectile = incoming.is_attack;
        incoming.kind = projectile_kind::tracking;
        incoming.internally_polled = true;
        incoming.projectile_id =
            (static_cast<std::uint64_t>(index) << 32) | id_first;
        incoming.source_handle = source_handle;
        incoming.target_handle = target_handle;
        incoming.source_entity = source;
        incoming.position = position;
        incoming.speed = static_cast<float>(speed);
        incoming.updated_at = GetTickCount64();

        float estimated_damage = 0.0f;
        if (incoming.is_attack && ability_handle == 0xFFFFFFFFu &&
            read_conservative_source_attack_damage(source, estimated_damage)) {
            incoming.damage = estimated_damage;
            incoming.original_damage = estimated_damage;
            incoming.physical_damage = estimated_damage;
            incoming.damage_complete = true;
            incoming.confidence = 0.70f;
        } else {
            // The timeline evaluator treats zero/unknown damage on an intersecting
            // enemy projectile as incomplete and blocks the cycle.
            incoming.confidence = 0.0f;
        }
        updates[update_count++] = incoming;
        ++tracking_count;
    }

    if (!overflow) {
        for (std::size_t index = 0; index < slot_count; ++index) {
            const auto slot_address = manager + linear_slots + index * sizeof(void*);
            const auto projectile_first = linear_snapshot[index];
            if (!plausible_pointer(projectile_first))
                continue;

            std::uint32_t id_first = 0;
            std::uint32_t id_second = 0;
            std::uint32_t source_handle = 0xFFFFFFFFu;
            armlet_vec3 origin{};
            armlet_vec3 position{};
            armlet_vec3 velocity{};
            float max_distance = 0.0f;
            if (!safe_read(projectile_first + 0x40, id_first) || id_first == 0 ||
                !safe_read(projectile_first + 0x44, origin) ||
                !safe_read(projectile_first + 0x50, position) ||
                !safe_read(projectile_first + 0x68, velocity) ||
                !safe_read(projectile_first + 0x8C, max_distance) ||
                !safe_read(projectile_first + 0x90, source_handle) ||
                !safe_read(projectile_first + 0x40, id_second))
                continue;
            std::uintptr_t projectile_second = 0;
            if (!safe_read(slot_address, projectile_second) ||
                projectile_first != projectile_second || id_first != id_second)
                continue;

            const float speed = length(velocity);
            const float travelled = length(subtract(position, origin));
            if (!(speed > 1.0f && speed < 100000.0f) ||
                !finite_nonnegative(max_distance) || max_distance <= travelled ||
                !std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z))
                continue;

            const auto source = entity_by_handle(entity_system, source_handle);
            std::uint8_t source_team = 0;
            if (!hero_ready || !plausible_pointer(source) ||
                !safe_read(source + schema_fields::m_iTeamNum, source_team) ||
                source_team == 0 || source_team == hero_team)
                continue;
            if (update_count >= updates.size()) {
                overflow = true;
                break;
            }

            incoming_projectile incoming{};
            incoming.active = true;
            incoming.position_valid = true;
            incoming.kind = projectile_kind::linear;
            incoming.internally_polled = true;
            incoming.projectile_id = (1ULL << 63) |
                (static_cast<std::uint64_t>(index) << 32) | id_first;
            incoming.source_handle = source_handle;
            incoming.source_entity = source;
            incoming.position = position;
            incoming.velocity = velocity;
            incoming.speed = speed;
            incoming.radius = conservative_linear_radius;
            incoming.remaining_distance = max_distance - travelled;
            incoming.confidence = 0.0f;
            incoming.updated_at = GetTickCount64();
            updates[update_count++] = incoming;
            ++linear_count;
        }
    }

    if (overflow) {
        g_monitor.internal_projectile_feed_ready.store(false,
            std::memory_order_relaxed);
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_projectiles.lock);
    // A complete manager snapshot is authoritative for entries that came from
    // this same feed. Retire missing objects in this poll rather than letting a
    // vanished/landed projectile survive for the generic 350 ms API timeout.
    for (auto& entry : g_projectiles.entries) {
        if (!entry.active || !entry.internally_polled ||
            (entry.kind != projectile_kind::tracking &&
                entry.kind != projectile_kind::linear))
            continue;
        bool still_present = false;
        for (std::size_t index = 0; index < update_count; ++index) {
            if (updates[index].kind == entry.kind &&
                updates[index].projectile_id == entry.projectile_id) {
                still_present = true;
                break;
            }
        }
        if (!still_present)
            entry.active = false;
    }
    g_projectiles.heartbeat = now;
    for (std::size_t index = 0; index < update_count; ++index) {
        auto* slot = select_projectile_slot(
            updates[index].projectile_id, updates[index].kind, now);
        *slot = updates[index];
        slot->updated_at = now;
    }
    ReleaseSRWLockExclusive(&g_projectiles.lock);

    g_monitor.internal_projectile_feed_ready.store(true,
        std::memory_order_relaxed);
    g_monitor.internal_tracking_count.store(tracking_count,
        std::memory_order_relaxed);
    g_monitor.internal_linear_count.store(linear_count,
        std::memory_order_relaxed);
    return true;
}

bool is_owned_armlet(std::uintptr_t entity, int local_player_id,
    std::uintptr_t client_base, std::size_t client_size) {
    if (!plausible_pointer(entity))
        return false;

    int owner_id = -1;
    if (!safe_read(entity + schema_fields::m_iPlayerOwnerID, owner_id) ||
        owner_id != local_player_id)
        return false;

    char designer_name[96]{};
    if (read_designer_name(entity, designer_name) &&
        std::strcmp(designer_name, "item_armlet") == 0)
        return true;

    return rtti_name_contains(entity, client_base, client_size, "C_DOTA_Item_Armlet@@");
}

bool is_armlet_entity(std::uintptr_t entity, std::uintptr_t client_base,
    std::size_t client_size) {
    if (!plausible_pointer(entity))
        return false;

    char designer_name[96]{};
    if (read_designer_name(entity, designer_name) &&
        std::strcmp(designer_name, "item_armlet") == 0)
        return true;
    return rtti_name_contains(entity, client_base, client_size,
        "C_DOTA_Item_Armlet@@");
}

/**
 * @brief Находит принадлежащий герою Armlet и его индекс для приказа; повторная проверка нужна
 * перед фактическим выполнением команды.
 */
std::uintptr_t find_armlet_item(std::uintptr_t client_base, std::size_t client_size,
    std::uintptr_t hero, std::uintptr_t cached_armlet) {
    std::uintptr_t entity_system = 0;
    if (!read_game_entity_system(client_base, entity_system))
        return 0;

    // The hero inventory is authoritative. Demo/controlled heroes may expose
    // m_nPlayerOwnerID == -1 even though their item entities correctly belong
    // to the local player, so owner filtering must not precede this lookup.
    std::array<std::uint32_t, 32> handles{};
    std::size_t slot_count = 0;
    read_hero_inventory_handles(hero, handles, slot_count);
    for (std::size_t slot = 0; slot < std::min(slot_count, std::size_t{17}); ++slot) {
        const auto handle = handles[slot];
        if (handle == 0 || handle == 0xFFFFFFFFu)
            continue;
        const auto item = entity_by_handle(entity_system, handle);
        if (is_armlet_entity(item, client_base, client_size))
            return item;
    }

    int local_player_id = -1;
    if (!safe_read(hero + schema_fields::m_nPlayerOwnerID, local_player_id) ||
        local_player_id < 0 || local_player_id > 63)
        local_player_id = read_local_player_id();
    if (local_player_id < 0 || local_player_id > 63)
        return 0;

    if (is_owned_armlet(cached_armlet, local_player_id, client_base, client_size))
        return cached_armlet;

    std::uintptr_t first_match = 0;
    std::uintptr_t active_match = 0;
    visit_entities(entity_system, [&](std::uintptr_t entity, int) {
        if (!is_owned_armlet(entity, local_player_id, client_base, client_size))
            return false;

        bool toggled = false;
        if (safe_read(entity + schema_fields::m_bToggleState, toggled) && toggled) {
            active_match = entity;
            return true;
        }
        if (first_match == 0)
            first_match = entity;
        return false;
    });

    return active_match != 0 ? active_match : first_match;
}

/**
 * @brief Находит принадлежащий герою Armlet и его индекс для приказа; повторная проверка нужна
 * перед фактическим выполнением команды.
 */
int find_usable_item_entity_index(std::uintptr_t client_base, std::uintptr_t hero,
    std::uintptr_t item) {
    if (!plausible_pointer(item))
        return -1;

    std::uintptr_t entity_system = 0;
    if (!read_game_entity_system(client_base, entity_system))
        return -1;

    std::array<std::uint32_t, 32> handles{};
    std::size_t slot_count = 0;
    if (!read_hero_inventory_handles(hero, handles, slot_count))
        return -1;
    // Only main inventory items can be used. Resolve the full CHandle so a
    // replaced item cannot inherit the old entity's index.
    for (std::size_t slot = 0; slot < std::min(slot_count, std::size_t{6}); ++slot) {
        const auto handle = handles[slot];
        if (handle == 0 || handle == 0xFFFFFFFFu)
            continue;

        const int entity_index = static_cast<int>(handle & 0x7FFFu);
        if (entity_by_handle(entity_system, handle) == item)
            return entity_index;
    }

    return -1;
}

int find_armlet_entity_index(std::uintptr_t client_base, std::uintptr_t hero,
    std::uintptr_t armlet) {
    return find_usable_item_entity_index(client_base, hero, armlet);
}
