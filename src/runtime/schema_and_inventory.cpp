/**
 * @file
 * @brief SchemaSystem, модификаторы и инвентарь.
 *
 * Разрешает поля по имени класса и поля, отличает обязательные поля от необязательных, находит
 * Armlet и мгновенное лечение. Отсутствие обязательного поля блокирует готовность schema;
 * частичные дополнительные данные не должны подменяться нулевыми фактами.
 */

// Schema fields and inventory access.

const char* instant_heal_name(instant_heal_kind kind) {
    switch (kind) {
    case instant_heal_kind::magic_stick: return "Magic Stick";
    case instant_heal_kind::magic_wand: return "Magic Wand";
    case instant_heal_kind::faerie_fire: return "Faerie Fire";
    case instant_heal_kind::healing_lotus: return "Healing Lotus";
    case instant_heal_kind::great_healing_lotus: return "Great Healing Lotus";
    case instant_heal_kind::greater_healing_lotus: return "Greater Healing Lotus";
    case instant_heal_kind::holy_locket: return "Holy Locket";
    case instant_heal_kind::mekansm: return "Mekansm";
    case instant_heal_kind::guardian_greaves: return "Guardian Greaves";
    case instant_heal_kind::cheese: return "Cheese";
    case instant_heal_kind::none: return "none";
    }
    return "none";
}

instant_heal_kind instant_heal_kind_from_name(const char* name) {
    if (std::strcmp(name, "item_magic_stick") == 0)
        return instant_heal_kind::magic_stick;
    if (std::strcmp(name, "item_magic_wand") == 0)
        return instant_heal_kind::magic_wand;
    if (std::strcmp(name, "item_faerie_fire") == 0)
        return instant_heal_kind::faerie_fire;
    if (std::strcmp(name, "item_healing_lotus") == 0 ||
        std::strcmp(name, "item_lesser_healing_lotus") == 0)
        return instant_heal_kind::healing_lotus;
    if (std::strcmp(name, "item_great_healing_lotus") == 0)
        return instant_heal_kind::great_healing_lotus;
    if (std::strcmp(name, "item_greater_healing_lotus") == 0)
        return instant_heal_kind::greater_healing_lotus;
    if (std::strcmp(name, "item_holy_locket") == 0)
        return instant_heal_kind::holy_locket;
    if (std::strcmp(name, "item_mekansm") == 0)
        return instant_heal_kind::mekansm;
    if (std::strcmp(name, "item_guardian_greaves") == 0)
        return instant_heal_kind::guardian_greaves;
    if (std::strcmp(name, "item_cheese") == 0)
        return instant_heal_kind::cheese;
    return instant_heal_kind::none;
}

std::uint32_t instant_heal_group_for(instant_heal_kind kind) {
    switch (kind) {
    case instant_heal_kind::magic_stick:
    case instant_heal_kind::magic_wand:
        return heal_group_stick_wand;
    case instant_heal_kind::faerie_fire:
        return heal_group_faerie_fire;
    case instant_heal_kind::healing_lotus:
    case instant_heal_kind::great_healing_lotus:
    case instant_heal_kind::greater_healing_lotus:
        return heal_group_lotus;
    case instant_heal_kind::holy_locket:
        return heal_group_holy_locket;
    case instant_heal_kind::mekansm:
    case instant_heal_kind::guardian_greaves:
        return heal_group_team_items;
    case instant_heal_kind::cheese:
        return heal_group_cheese;
    case instant_heal_kind::none:
        return 0;
    }
    return 0;
}

float instant_heal_amount(instant_heal_kind kind, int charges) {
    switch (kind) {
    case instant_heal_kind::magic_stick:
    case instant_heal_kind::magic_wand: return 15.0f * charges;
    case instant_heal_kind::faerie_fire: return 85.0f;
    case instant_heal_kind::healing_lotus: return 125.0f;
    case instant_heal_kind::great_healing_lotus: return 400.0f;
    case instant_heal_kind::greater_healing_lotus: return 900.0f;
    case instant_heal_kind::holy_locket: return 17.0f * charges;
    case instant_heal_kind::mekansm: return 250.0f;
    case instant_heal_kind::guardian_greaves: return 325.0f;
    case instant_heal_kind::cheese: return 2500.0f;
    case instant_heal_kind::none: return 0.0f;
    }
    return 0.0f;
}

bool instant_heal_requires_charges(instant_heal_kind kind) {
    return kind == instant_heal_kind::magic_stick ||
        kind == instant_heal_kind::magic_wand ||
        kind == instant_heal_kind::faerie_fire ||
        kind == instant_heal_kind::healing_lotus ||
        kind == instant_heal_kind::great_healing_lotus ||
        kind == instant_heal_kind::greater_healing_lotus ||
        kind == instant_heal_kind::holy_locket ||
        kind == instant_heal_kind::cheese;
}

int find_entity_index(std::uintptr_t entity_system, std::uintptr_t target) {
    int found = -1;
    visit_entities(entity_system, [&](std::uintptr_t entity, int index) {
        if (entity != target)
            return false;
        found = index;
        return true;
    });
    return found;
}

/**
 * @brief Проверяет доступность предметов мгновенного лечения: готовность, заряды и ограничения
 * использования учитываются до включения в план.
 */
bool instant_heal_item_ready(std::uintptr_t item, instant_heal_kind expected,
    std::uintptr_t hero, int& charges) {
    if (!plausible_pointer(item) || !plausible_pointer(hero))
        return false;
    char designer_name[96]{};
    if (!read_designer_name(item, designer_name) ||
        instant_heal_kind_from_name(designer_name) != expected)
        return false;
    bool enabled = false;
    float cooldown = 0.0f;
    int mana_cost = 0;
    float mana = 0.0f;
    if (!safe_read(item + schema_fields::m_bItemEnabled, enabled) || !enabled ||
        !safe_read(item + schema_fields::m_fCooldown, cooldown) ||
        !std::isfinite(cooldown) || cooldown > 0.05f ||
        !safe_read(item + schema_fields::m_iManaCost, mana_cost) ||
        !safe_read(hero + schema_fields::m_flMana, mana) ||
        !std::isfinite(mana) || mana + 0.5f < static_cast<float>(mana_cost))
        return false;
    charges = 0;
    safe_read(item + schema_fields::m_iCurrentCharges, charges);
    return !instant_heal_requires_charges(expected) || charges > 0;
}

/**
 * @brief Проверяет доступность предметов мгновенного лечения: готовность, заряды и ограничения
 * использования учитываются до включения в план.
 */
std::size_t collect_instant_heals(std::uintptr_t client_base,
    std::uintptr_t hero, std::array<instant_heal_candidate, 12>& candidates) {
    if (!g_monitor.use_instant_heals.load(std::memory_order_relaxed))
        return 0;
    std::uintptr_t entity_system = 0;
    if (!read_game_entity_system(client_base, entity_system))
        return 0;
    const std::uint32_t allowed = g_monitor.instant_heal_mask.load(
        std::memory_order_relaxed);
    int hero_index = -1;
    bool hero_index_resolved = false;
    std::array<std::uint32_t, 32> handles{};
    std::size_t slot_count = 0;
    if (!read_hero_inventory_handles(hero, handles, slot_count))
        return 0;
    std::size_t count = 0;
    // Slots 0..5 are the usable main inventory. Backpack/stash items cannot be
    // activated in the critical OFF window and are deliberately ignored.
    for (std::size_t slot = 0; slot < std::min(slot_count, std::size_t{6}) &&
         count < candidates.size(); ++slot) {
        const auto handle = handles[slot];
        if (handle == 0 || handle == 0xFFFFFFFFu)
            continue;
        const auto item = entity_by_handle(entity_system, handle);
        char designer_name[96]{};
        if (!read_designer_name(item, designer_name))
            continue;
        const instant_heal_kind kind = instant_heal_kind_from_name(designer_name);
        if (kind == instant_heal_kind::none ||
            (allowed & instant_heal_group_for(kind)) == 0)
            continue;
        int charges = 0;
        if (!instant_heal_item_ready(item, kind, hero, charges))
            continue;
        instant_heal_candidate candidate{};
        candidate.kind = kind;
        candidate.item = item;
        candidate.entity_index = static_cast<int>(handle & 0x7FFFu);
        candidate.charges = charges;
        candidate.heal = instant_heal_amount(kind, charges);
        if (kind == instant_heal_kind::holy_locket) {
            if (!hero_index_resolved) {
                hero_index = find_entity_index(entity_system, hero);
                hero_index_resolved = true;
            }
            if (hero_index < 0)
                continue;
            candidate.cast_order = 6; // CAST_TARGET
            candidate.target_index = hero_index;
        }
        if (candidate.heal > 0.0f)
            candidates[count++] = candidate;
    }
    std::sort(candidates.begin(), candidates.begin() + count,
        [](const instant_heal_candidate& lhs, const instant_heal_candidate& rhs) {
            if (lhs.heal != rhs.heal)
                return lhs.heal < rhs.heal;
            return lhs.entity_index < rhs.entity_index;
        });
    return count;
}

/**
 * @brief Подтверждает modifier Armlet по проверяемому объекту, а не по одному toggle-флагу
 * предмета.
 */
bool is_armlet_modifier(std::uintptr_t object, std::uintptr_t client_base,
    std::size_t client_size) {
    return rtti_name_contains(object, client_base, client_size,
        "CDOTA_Modifier_Armlet_UnholyStrength@@");
}

/**
 * @brief Подтверждает modifier Armlet по проверяемому объекту, а не по одному toggle-флагу
 * предмета.
 */
std::uintptr_t scan_modifier_pointer_array(std::uintptr_t data, int count,
    std::uintptr_t client_base, std::size_t client_size) {
    if (!plausible_pointer(data) || count <= 0 || count > 256)
        return 0;

    for (int index = 0; index < count; ++index) {
        std::uintptr_t modifier = 0;
        if (safe_read(data + sizeof(void*) * static_cast<std::size_t>(index), modifier) &&
            is_armlet_modifier(modifier, client_base, client_size))
            return modifier;
    }
    return 0;
}

/**
 * @brief Подтверждает modifier Armlet по проверяемому объекту, а не по одному toggle-флагу
 * предмета.
 */
std::uintptr_t find_armlet_modifier(std::uintptr_t hero, std::uintptr_t client_base,
    std::size_t client_size) {
    const auto manager = hero + schema_fields::m_ModifierManager;

    std::uintptr_t data = 0;
    int count = 0;
    if (resolve_modifier_vector(
            manager, client_base, client_size, count, data)) {
        if (const auto modifier = scan_modifier_pointer_array(
                data, count, client_base, client_size); modifier != 0)
            return modifier;
    }

    // Fallback for layout changes: inspect direct pointers and small pointer containers
    // before the first stable schema field at manager + 0x28.
    for (std::size_t offset = 0; offset < 0x380; offset += sizeof(void*)) {
        std::uintptr_t candidate = 0;
        if (safe_read(manager + offset, candidate) &&
            is_armlet_modifier(candidate, client_base, client_size))
            return candidate;
    }

    for (std::size_t offset = 0; offset < 0x28; offset += sizeof(void*)) {
        std::uintptr_t candidate_data = 0;
        if (!safe_read(manager + offset, candidate_data) || !plausible_pointer(candidate_data))
            continue;
        if (const auto modifier = scan_modifier_pointer_array(
                candidate_data, 128, client_base, client_size); modifier != 0)
            return modifier;
    }

    return 0;
}

void log_message(const char* level, const char* message) {
    // Do not format or emit diagnostic lines until the user has explicitly
    // enabled logging from the advanced panel.
    if (!g_monitor.debug_logging.load(std::memory_order_relaxed))
        return;

    SYSTEMTIME now{};
    GetLocalTime(&now);

    char line[512]{};
    std::snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] [%-5s] %s\n",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, level, message);
    // The injected module stays silent and never opens a visible console. A
    // debugger may still collect diagnostics through the Windows debug channel.
    OutputDebugStringA(line);
}

struct schema_field_request {
    const char* module_name;
    const char* class_name;
    const char* field_name;
    std::ptrdiff_t* destination;
};

/**
 * @brief Принимает результат поиска только при однозначном совпадении в проверенном диапазоне
 * образа; неоднозначность означает отказ от разрешения адреса.
 */
std::uintptr_t find_unique_executable_pattern(std::uintptr_t module_base,
    std::size_t module_size, const std::uint8_t* pattern, const char* mask,
    std::size_t pattern_size);

// These layouts are the stable public SchemaSystem data layouts, not game
// entity offsets. Field offsets themselves always come from the live type
// scopes and are never backed by patch-specific constants.
constexpr std::ptrdiff_t schema_class_field_count = 0x24;
constexpr std::ptrdiff_t schema_class_fields = 0x30;
constexpr std::size_t schema_field_stride = 0x20;
constexpr std::ptrdiff_t schema_field_name = 0x00;
constexpr std::ptrdiff_t schema_field_offset = 0x10;
constexpr std::size_t schema_find_type_scope_vtable_index = 13;
constexpr std::size_t schema_find_declared_class_vtable_index = 25;

std::array schema_requests{
    schema_field_request{"client.dll", "C_BaseEntity", "m_lifeState", &schema_fields::comfort_life_state},
    schema_field_request{"client.dll", "CBasePlayerController", "m_steamID", &schema_fields::comfort_steam_id},
    schema_field_request{"client.dll", "CBasePlayerController", "m_iszPlayerName", &schema_fields::comfort_player_name},
    schema_field_request{"client.dll", "C_DOTAGamerules", "m_unMatchID64", &schema_fields::comfort_match_id},
    schema_field_request{"client.dll", "C_DOTAGamerules", "m_nGameState", &schema_fields::game_rules_m_nGameState},
    schema_field_request{"client.dll", "C_DOTAGamerules", "m_NeutralSpawnBoxes", &schema_fields::game_rules_m_NeutralSpawnBoxes},
    schema_field_request{"client.dll", "C_DOTAGamerules", "m_flCreepSpawntime", &schema_fields::game_rules_m_flCreepSpawntime},
    schema_field_request{"client.dll", "C_DOTAGamerules", "m_flGameStartTime", &schema_fields::game_rules_m_flGameStartTime},
    schema_field_request{"client.dll", "C_DOTAGamerules", "m_bCreepSpawningEnabled", &schema_fields::game_rules_m_bCreepSpawningEnabled},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_iMoveSpeed", &schema_fields::npc_m_iMoveSpeed},
    schema_field_request{"client.dll", "C_BaseEntity", "m_flCreateTime", &schema_fields::entity_m_flCreateTime},
    schema_field_request{"client.dll", "C_DOTA_Item_Physical", "m_hItem", &schema_fields::physical_item_m_hItem},
    schema_field_request{"client.dll", "C_GameRules", "m_bGamePaused", &schema_fields::game_rules_m_bGamePaused},
    schema_field_request{"client.dll", "C_GameRules", "m_nPauseStartTick", &schema_fields::game_rules_m_nPauseStartTick},
    schema_field_request{"client.dll", "C_GameRules", "m_nTotalPausedTicks", &schema_fields::game_rules_m_nTotalPausedTicks},

    schema_field_request{"client.dll", "C_BaseEntity", "m_iMaxHealth", &schema_fields::m_iMaxHealth},
    schema_field_request{"client.dll", "C_BaseEntity", "m_iHealth", &schema_fields::m_iHealth},
    schema_field_request{"client.dll", "CEntityInstance", "m_pEntity", &schema_fields::m_pEntityIdentity},
    schema_field_request{"client.dll", "CEntityIdentity", "m_designerName", &schema_fields::m_designerName},
    schema_field_request{"client.dll", "C_BaseEntity", "m_pGameSceneNode", &schema_fields::m_pGameSceneNode},
    schema_field_request{"client.dll", "C_BaseEntity", "m_iTeamNum", &schema_fields::m_iTeamNum},
    schema_field_request{"client.dll", "C_BaseEntity", "m_hOwnerEntity", &schema_fields::m_hOwnerEntity},
    schema_field_request{"client.dll", "C_BaseEntity", "m_vecAbsVelocity", &schema_fields::m_vecAbsVelocity},
    schema_field_request{"client.dll", "CGameSceneNode", "m_vecAbsOrigin", &schema_fields::m_vecAbsOrigin},
    schema_field_request{"client.dll", "CGameSceneNode", "m_bDormant", &schema_fields::m_bDormant},
    schema_field_request{"client.dll", "C_BaseModelEntity", "m_iTeamVisibilityBitmask", &schema_fields::m_iTeamVisibilityBitmask},
    schema_field_request{"client.dll", "C_DOTA_DataNonSpectator", "m_bNPCVisibleState", &schema_fields::m_bNPCVisibleState},

    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_ModifierManager", &schema_fields::m_ModifierManager},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_Inventory", &schema_fields::m_Inventory},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_nPlayerOwnerID", &schema_fields::m_nPlayerOwnerID},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_flHullRadius", &schema_fields::m_flHullRadius},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_flPhysicalArmorValue", &schema_fields::m_flPhysicalArmorValue},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_flMagicalResistanceValue", &schema_fields::m_flMagicalResistanceValue},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_flMana", &schema_fields::m_flMana},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_nUnitState64", &schema_fields::m_nUnitState64},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_vecAbilities", &schema_fields::m_vecAbilities},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_iDamageMin", &schema_fields::m_iDamageMin},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_iDamageMax", &schema_fields::m_iDamageMax},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_iDamageBonus", &schema_fields::m_iDamageBonus},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_iBaseDamageMin", &schema_fields::m_iBaseDamageMin},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_iBaseDamageMax", &schema_fields::m_iBaseDamageMax},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_iAttackRange", &schema_fields::m_iAttackRange},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_flBaseAttackTime", &schema_fields::m_flBaseAttackTime},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_iAttackCapabilities", &schema_fields::m_iAttackCapabilities},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_flLastAttackTime", &schema_fields::m_flLastAttackTime},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_flLastDealtDamageTime", &schema_fields::m_flLastDealtDamageTime},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_flLastDamageTime", &schema_fields::m_flLastDamageTime},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_iGoldBountyMin", &schema_fields::npc_gold_min},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_iGoldBountyMax", &schema_fields::npc_gold_max},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_bIsWaitingToSpawn", &schema_fields::npc_waiting_to_spawn},
    schema_field_request{"client.dll", "C_BaseEntity", "m_fEffects", &schema_fields::entity_effects},
    schema_field_request{"client.dll", "C_BaseEntity", "m_lifeState", &schema_fields::entity_life_state},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_bIsNeutralUnitType", &schema_fields::m_bIsNeutralUnitType},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC", "m_hNeutralSpawner", &schema_fields::m_hNeutralSpawner},
    schema_field_request{"client.dll", "C_DOTA_UnitInventory", "m_hItems", &schema_fields::m_hItems},
    schema_field_request{"client.dll", "C_DOTA_UnitInventory", "m_hTransientCastItem", &schema_fields::m_hTransientCastItem},

    schema_field_request{"client.dll", "C_DOTAPlayerController", "m_nPlayerID", &schema_fields::controller_m_nPlayerID},
    schema_field_request{"client.dll", "C_DOTAPlayerController", "m_hAssignedHero", &schema_fields::controller_m_hAssignedHero},
    schema_field_request{"client.dll", "C_DOTABaseAbility", "m_bToggleState", &schema_fields::m_bToggleState},
    schema_field_request{"client.dll", "C_DOTABaseAbility", "m_bInAbilityPhase", &schema_fields::m_bInAbilityPhase},
    schema_field_request{"client.dll", "C_DOTABaseAbility", "m_fCooldown", &schema_fields::m_fCooldown},
    schema_field_request{"client.dll", "C_DOTABaseAbility", "m_flCooldownLength", &schema_fields::m_flCooldownLength},
    schema_field_request{"client.dll", "C_DOTABaseAbility", "m_flCastStartTime", &schema_fields::m_flCastStartTime},
    schema_field_request{"client.dll", "C_DOTABaseAbility", "m_flChannelStartTime", &schema_fields::m_flChannelStartTime},
    schema_field_request{"client.dll", "C_DOTABaseAbility", "m_flLastCastClickTime", &schema_fields::m_flLastCastClickTime},
    schema_field_request{"client.dll", "C_DOTABaseAbility", "m_iManaCost", &schema_fields::m_iManaCost},
    schema_field_request{"client.dll", "C_DOTABaseAbility", "m_iLevel", &schema_fields::m_iAbilityLevel},
    schema_field_request{"client.dll", "C_DOTABaseAbility", "m_iEnemyLevel", &schema_fields::m_iEnemyAbilityLevel},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC_Hero", "m_iPlayerID", &schema_fields::hero_m_iPlayerID},
    schema_field_request{"client.dll", "C_DOTA_Item", "m_iPlayerOwnerID", &schema_fields::m_iPlayerOwnerID},
    schema_field_request{"client.dll", "C_DOTA_Item", "m_iCurrentCharges", &schema_fields::m_iCurrentCharges},
    schema_field_request{"client.dll", "C_DOTA_Item", "m_bItemEnabled", &schema_fields::m_bItemEnabled},
    schema_field_request{"client.dll", "C_DOTA_Item_TeleportScroll", "m_hTeleportTarget", &schema_fields::teleport_m_hTarget},
    schema_field_request{"client.dll", "C_DOTA_Item_TeleportScroll", "m_nFXCaster", &schema_fields::teleport_m_nFxCaster},
    schema_field_request{"client.dll", "C_DOTA_Item_TeleportScroll", "m_nFXOrigin", &schema_fields::teleport_m_nFxOrigin},
    schema_field_request{"client.dll", "C_DOTA_Item_TeleportScroll", "m_nFXDestination", &schema_fields::teleport_m_nFxDestination},
    schema_field_request{"client.dll", "C_DOTA_Item_TeleportScroll", "m_vDestination", &schema_fields::teleport_m_vDestination},
    schema_field_request{"client.dll", "C_DOTA_Item_TeleportScroll", "m_flBaseTeleportTime", &schema_fields::teleport_m_flBaseTime},
    schema_field_request{"client.dll", "C_DOTA_Item_TeleportScroll", "m_flExtraTeleportTime", &schema_fields::teleport_m_flExtraTime},
    schema_field_request{"client.dll", "CDOTA_Modifier_Teleporting", "m_fChannelTime", &schema_fields::teleport_modifier_m_fChannelTime},
    schema_field_request{"client.dll", "CDOTA_Modifier_Teleporting", "m_vStart", &schema_fields::teleport_modifier_m_vStart},
    schema_field_request{"client.dll", "CDOTA_Modifier_Teleporting", "m_vEnd", &schema_fields::teleport_modifier_m_vEnd},
    schema_field_request{"client.dll", "C_DOTA_Item_Armlet", "toggle_cooldown", &schema_fields::toggle_cooldown},

    schema_field_request{"client.dll", "CDOTA_Modifier_Armlet_UnholyStrength", "unholy_bonus_damage", &schema_fields::unholy_bonus_damage},
    schema_field_request{"client.dll", "CDOTA_Modifier_Armlet_UnholyStrength", "unholy_bonus_attack_speed", &schema_fields::unholy_bonus_attack_speed},
    schema_field_request{"client.dll", "CDOTA_Modifier_Armlet_UnholyStrength", "unholy_bonus_strength", &schema_fields::unholy_bonus_strength},
    schema_field_request{"client.dll", "CDOTA_Modifier_Armlet_UnholyStrength", "unholy_bonus_armor", &schema_fields::unholy_bonus_armor},
    schema_field_request{"client.dll", "CDOTA_Modifier_Armlet_UnholyStrength", "unholy_health_drain_per_second", &schema_fields::unholy_health_drain_per_second},
    schema_field_request{"client.dll", "CDOTA_Modifier_Armlet_UnholyStrength", "str_tick_count", &schema_fields::str_tick_count},
    schema_field_request{"client.dll", "CDOTA_Modifier_Armlet_UnholyStrength", "unholy_bonus_slow_resistance", &schema_fields::unholy_bonus_slow_resistance},
    schema_field_request{"client.dll", "CDOTA_Modifier_Armlet_UnholyStrength", "tick_interval", &schema_fields::tick_interval},
    schema_field_request{"client.dll", "CDOTA_Modifier_Armlet_UnholyStrength", "m_flDamageRemainder", &schema_fields::m_flDamageRemainder},

    schema_field_request{"client.dll", "CDOTA_Modifier_EmberSpirit_SearingChains", "tick_interval", &schema_fields::chains_tick_interval},
    schema_field_request{"client.dll", "CDOTA_Modifier_EmberSpirit_SearingChains", "tick_damage", &schema_fields::chains_tick_damage},
    schema_field_request{"client.dll", "CDOTA_Modifier_Invoker_IceWall_SlowDebuff", "damage_per_second", &schema_fields::ice_wall_damage_per_second},
    schema_field_request{"client.dll", "CDOTA_Modifier_Invoker_IceWall_SlowDebuff", "tick_interval", &schema_fields::ice_wall_tick_interval},
    schema_field_request{"client.dll", "CDOTA_Modifier_Invoker_IceWall_SlowDebuff", "m_NextDPSTime", &schema_fields::ice_wall_next_dps_time},
    schema_field_request{"client.dll", "CDOTA_Modifier_Huskar_Burning_Spear_Debuff", "burn_damage", &schema_fields::burning_spear_damage},
    schema_field_request{"client.dll", "CDOTA_Modifier_Huskar_Burning_Spear_Debuff", "burn_damage_max_pct", &schema_fields::burning_spear_max_health_pct},
    schema_field_request{"client.dll", "CDOTA_Modifier_QueenOfPain_SonicWave_Damage", "m_flDamagePerTick", &schema_fields::sonic_wave_damage_per_tick},
    schema_field_request{"client.dll", "CDOTA_Modifier_CrystalMaiden_Frostbite", "damage_per_second", &schema_fields::frostbite_damage_per_second},
    schema_field_request{"client.dll", "CDOTA_Modifier_CrystalMaiden_Frostbite", "tick_interval", &schema_fields::frostbite_tick_interval},
    schema_field_request{"client.dll", "CDOTA_Modifier_Phoenix_Sun_Debuff", "damage_per_sec", &schema_fields::sun_ray_damage_per_second},
    schema_field_request{"client.dll", "CDOTA_Modifier_Phoenix_Sun_Debuff", "tick_interval", &schema_fields::sun_ray_tick_interval},
    schema_field_request{"client.dll", "CDOTA_Modifier_Viper_PoisonAttack_Slow", "damage", &schema_fields::poison_attack_damage},
    schema_field_request{"client.dll", "CDOTA_Modifier_Necrolyte_Heartstopper_Aura_Effect", "aura_damage", &schema_fields::heartstopper_aura_damage},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC_Tower", "m_hPrevTowerAttackTarget", &schema_fields::m_hPrevTowerAttackTarget},
    schema_field_request{"client.dll", "C_DOTA_BaseNPC_Tower", "m_hTowerAttackTarget", &schema_fields::m_hTowerAttackTarget},
    schema_field_request{"client.dll", "C_DOTA_NeutralSpawner", "m_Type", &schema_fields::neutral_spawner_m_Type},
};

/**
 * @brief Определяет, блокирует ли отсутствие поля весь runtime. Новое необязательное render-
 * поле следует явно включать в список исключений.
 */
bool schema_field_is_required(const schema_field_request& request) {
    if(request.destination==&schema_fields::npc_gold_min||request.destination==&schema_fields::npc_gold_max||
       request.destination==&schema_fields::npc_waiting_to_spawn||request.destination==&schema_fields::entity_effects||
       request.destination==&schema_fields::entity_life_state)return false;
    if (request.destination == &schema_fields::comfort_life_state ||
        request.destination == &schema_fields::comfort_steam_id ||
        request.destination == &schema_fields::comfort_player_name ||
        request.destination == &schema_fields::comfort_match_id) return false;
    // Optional threat-model extensions must never take the local hero and
    // Armlet offline when Valve removes or renames one specialized modifier.
    if (std::strncmp(request.class_name, "CDOTA_Modifier_", 15) == 0 ||
        request.destination == &schema_fields::game_rules_m_nGameState ||
        request.destination == &schema_fields::game_rules_m_NeutralSpawnBoxes ||
        request.destination == &schema_fields::game_rules_m_flCreepSpawntime ||
        request.destination == &schema_fields::game_rules_m_flGameStartTime ||
        request.destination == &schema_fields::game_rules_m_bCreepSpawningEnabled ||
        request.destination == &schema_fields::npc_m_iMoveSpeed ||
        request.destination == &schema_fields::entity_m_flCreateTime ||
        request.destination == &schema_fields::physical_item_m_hItem ||
        std::strcmp(request.class_name, "C_DOTA_BaseNPC_Tower") == 0 ||
        request.destination == &schema_fields::toggle_cooldown ||
        request.destination == &schema_fields::m_pEntityIdentity ||
        request.destination == &schema_fields::m_designerName ||
        request.destination == &schema_fields::m_bDormant ||
        request.destination == &schema_fields::m_iTeamVisibilityBitmask ||
        request.destination == &schema_fields::m_bNPCVisibleState ||
        request.destination == &schema_fields::m_hOwnerEntity ||
        request.destination == &schema_fields::m_flLastDealtDamageTime ||
        request.destination == &schema_fields::m_flLastDamageTime ||
        request.destination == &schema_fields::m_bIsNeutralUnitType ||
        request.destination == &schema_fields::m_hNeutralSpawner ||
        request.destination == &schema_fields::m_hTransientCastItem ||
        request.destination == &schema_fields::m_vecAbilities ||
        request.destination == &schema_fields::m_bInAbilityPhase ||
        request.destination == &schema_fields::m_flCooldownLength ||
        request.destination == &schema_fields::m_flCastStartTime ||
        request.destination == &schema_fields::m_flChannelStartTime ||
        request.destination == &schema_fields::m_flLastCastClickTime ||
        request.destination == &schema_fields::m_iAbilityLevel ||
        request.destination == &schema_fields::m_iEnemyAbilityLevel ||
        request.destination == &schema_fields::hero_m_iPlayerID ||
        request.destination == &schema_fields::teleport_m_hTarget ||
        request.destination == &schema_fields::teleport_m_nFxCaster ||
        request.destination == &schema_fields::teleport_m_nFxOrigin ||
        request.destination == &schema_fields::teleport_m_nFxDestination ||
        request.destination == &schema_fields::teleport_m_vDestination ||
        request.destination == &schema_fields::teleport_m_flBaseTime ||
        request.destination == &schema_fields::teleport_m_flExtraTime ||
        request.destination == &schema_fields::teleport_modifier_m_fChannelTime ||
        request.destination == &schema_fields::teleport_modifier_m_vStart ||
        request.destination == &schema_fields::teleport_modifier_m_vEnd ||
        request.destination == &schema_fields::neutral_spawner_m_Type)
        return false;
    return true;
}

/**
 * @brief Ищет именованный элемент live SchemaSystem; lookup выполняется при
 * инициализации/повторной попытке, не вместо каждого чтения HP.
 */
std::uintptr_t find_schema_type_scope(std::uintptr_t schema_system,
    const char* module_name, std::uintptr_t schema_module,
    std::size_t schema_module_size) {
    std::uintptr_t vtable = 0;
    std::uintptr_t function = 0;
    if (!safe_read(schema_system, vtable) || !plausible_pointer(vtable) ||
        !safe_read(vtable + schema_find_type_scope_vtable_index * sizeof(void*), function) ||
        function < schema_module || function >= schema_module + schema_module_size)
        return 0;

    std::array<std::uint8_t, 5> prologue{};
    constexpr std::array<std::uint8_t, 5> expected{
        0x48, 0x89, 0x5C, 0x24, 0x10,
    };
    if (!safe_read(function, prologue) || prologue != expected)
        return 0;

    using find_type_scope_fn = std::uintptr_t(__fastcall*)(
        std::uintptr_t, const char*, const char**);
    return reinterpret_cast<find_type_scope_fn>(function)(
        schema_system, module_name, nullptr);
}

/**
 * @brief Ищет именованный элемент live SchemaSystem; lookup выполняется при
 * инициализации/повторной попытке, не вместо каждого чтения HP.
 */
std::uintptr_t find_schema_class(std::uintptr_t scope, const char* class_name,
    std::uintptr_t schema_module, std::size_t schema_module_size) {
    std::uintptr_t vtable = 0;
    std::uintptr_t function = 0;
    if (!safe_read(scope, vtable) || !plausible_pointer(vtable) ||
        !safe_read(vtable + schema_find_declared_class_vtable_index * sizeof(void*), function) ||
        function < schema_module || function >= schema_module + schema_module_size)
        return 0;

    // Do not call a stale vtable index after a SchemaSystem ABI change. The
    // current FindDeclaredClass entry starts by preserving RCX and validating
    // the class-name pointer; an unrelated method must fail closed here.
    std::array<std::uint8_t, 6> prologue{};
    constexpr std::array<std::uint8_t, 6> expected{
        0x4C, 0x8B, 0xD9, 0x48, 0x85, 0xD2,
    };
    if (!safe_read(function, prologue) || prologue != expected)
        return 0;

    using find_declared_class_fn = std::uintptr_t(__fastcall*)(
        std::uintptr_t, const char*);
    return reinterpret_cast<find_declared_class_fn>(function)(scope, class_name);
}

/**
 * @brief Ищет именованный элемент live SchemaSystem; lookup выполняется при
 * инициализации/повторной попытке, не вместо каждого чтения HP.
 */
bool find_schema_field_offset(std::uintptr_t class_info, const char* field_name,
    std::ptrdiff_t& result) {
    std::int16_t field_count = 0;
    std::uintptr_t fields = 0;
    if (!plausible_pointer(class_info) ||
        !safe_read(class_info + schema_class_field_count, field_count) ||
        !safe_read(class_info + schema_class_fields, fields) ||
        field_count <= 0 || field_count > 4096 || !plausible_pointer(fields))
        return false;

    for (std::int16_t index = 0; index < field_count; ++index) {
        const auto field = fields + static_cast<std::uintptr_t>(index) *
            schema_field_stride;
        std::uintptr_t name_address = 0;
        char name[192]{};
        std::int32_t offset = 0;
        if (!safe_read(field + schema_field_name, name_address) ||
            !read_c_string(name_address, name) || std::strcmp(name, field_name) != 0)
            continue;
        if (!safe_read(field + schema_field_offset, offset) ||
            offset <= 0 || offset > 0x100000)
            return false;
        result = offset;
        return true;
    }
    return false;
}

void refresh_modifier_registry_offsets() {
    known_modifier_registry[0].damage_offset = schema_fields::chains_tick_damage;
    known_modifier_registry[0].interval_offset = schema_fields::chains_tick_interval;
    known_modifier_registry[1].damage_offset = schema_fields::ice_wall_damage_per_second;
    known_modifier_registry[1].interval_offset = schema_fields::ice_wall_tick_interval;
    known_modifier_registry[1].exact_next_tick_offset = schema_fields::ice_wall_next_dps_time;
    known_modifier_registry[2].damage_offset = schema_fields::burning_spear_damage;
    known_modifier_registry[2].max_health_percent_offset =
        schema_fields::burning_spear_max_health_pct;
    known_modifier_registry[3].damage_offset = schema_fields::sonic_wave_damage_per_tick;
    known_modifier_registry[4].damage_offset = schema_fields::frostbite_damage_per_second;
    known_modifier_registry[4].interval_offset = schema_fields::frostbite_tick_interval;
    known_modifier_registry[5].damage_offset = schema_fields::sun_ray_damage_per_second;
    known_modifier_registry[5].interval_offset = schema_fields::sun_ray_tick_interval;
    known_modifier_registry[6].damage_offset = schema_fields::poison_attack_damage;
    known_modifier_registry[7].damage_offset =
        schema_fields::heartstopper_aura_damage;
    known_modifier_registry[7].max_health_percent_offset =
        schema_fields::heartstopper_aura_damage;
}

/**
 * @brief Разрешает запросы schema и публикует готовность только при наличии обязательных
 * полей. Необязательные расширения могут оставаться недоступными.
 */
bool initialize_schema_offsets() {
    g_monitor.schema_ready.store(false, std::memory_order_release);
    int required = 0;
    for (const auto& request : schema_requests) {
        if (schema_field_is_required(request))
            ++required;
    }
    g_monitor.schema_fields_required.store(
        required, std::memory_order_relaxed);
    g_monitor.schema_fields_resolved.store(0, std::memory_order_relaxed);
    for (const auto& request : schema_requests)
        *request.destination = 0;

    const HMODULE schema_module_handle = GetModuleHandleW(L"schemasystem.dll");
    if (schema_module_handle == nullptr)
        return false;
    const FARPROC create_interface_address = GetProcAddress(
        schema_module_handle, "CreateInterface");
    if (create_interface_address == nullptr)
        return false;
    using create_interface_fn = void*(*)(const char*, int*);
    static_assert(sizeof(create_interface_fn) == sizeof(create_interface_address));
    create_interface_fn create_interface = nullptr;
    std::memcpy(&create_interface, &create_interface_address,
        sizeof(create_interface));

    const auto schema_interface = reinterpret_cast<std::uintptr_t>(
        create_interface("SchemaSystem_001", nullptr));
    const auto schema_module = reinterpret_cast<std::uintptr_t>(schema_module_handle);
    const auto schema_module_size = image_size(schema_module);
    if (!plausible_pointer(schema_interface) || schema_module_size == 0)
        return false;

    // The exported interface is a subobject in current Source 2 builds. Resolve
    // the owning CSchemaSystem storage from its unique internal registration
    // site instead of assuming a fixed negative displacement from the interface.
    constexpr std::array<std::uint8_t, 10> backing_pattern{
        0x4C, 0x8D, 0x35, 0, 0, 0, 0, 0x0F, 0x28, 0x45,
    };
    constexpr char backing_mask[] = "xxx????xxx";
    const auto backing_instruction = find_unique_executable_pattern(
        schema_module, schema_module_size, backing_pattern.data(),
        backing_mask, backing_pattern.size());
    std::int32_t backing_displacement = 0;
    if (backing_instruction == 0 ||
        !safe_read(backing_instruction + 3, backing_displacement))
        return false;
    const auto schema_system = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(backing_instruction + 7) +
        backing_displacement);
    if (schema_system < schema_module ||
        schema_system + 0x284 > schema_module + schema_module_size)
        return false;

    int registration_count = 0;
    if (!safe_read(schema_system + 0x280, registration_count) ||
        registration_count <= 0)
        return false;

    const auto client_scope = find_schema_type_scope(schema_system, "client.dll",
        schema_module, schema_module_size);
    if (!plausible_pointer(client_scope))
        return false;

    int resolved_required = 0;
    int missing_required = 0;
    for (const auto& request : schema_requests) {
        if (_stricmp(request.module_name, "client.dll") != 0)
            return false;
        const auto scope = client_scope;
        const auto class_info = find_schema_class(scope, request.class_name,
            schema_module, schema_module_size);
        std::ptrdiff_t field_offset = 0;
        if (!find_schema_field_offset(class_info, request.field_name, field_offset)) {
            char message[320]{};
            std::snprintf(message, sizeof(message),
                "ISchemaSystem missing %s%s!%s::%s",
                schema_field_is_required(request) ? "REQUIRED " : "optional ",
                request.module_name, request.class_name, request.field_name);
            log_message("SCHEMA", message);
            if (schema_field_is_required(request))
                ++missing_required;
            continue;
        }
        *request.destination = field_offset;
        if (schema_field_is_required(request))
            ++resolved_required;
    }

    g_monitor.schema_fields_resolved.store(
        resolved_required, std::memory_order_relaxed);
    if (missing_required != 0 || resolved_required != required)
        return false;

    refresh_modifier_registry_offsets();
    g_monitor.schema_ready.store(true, std::memory_order_release);
    return true;
}

void log_health(int health) {
    char message[64]{};
    std::snprintf(message, sizeof(message), "Health changed: %d HP", health);
    log_message("HP", message);
}

/**
 * @brief Сопоставляет отправленный приказ с последующим наблюдением состояния. Dispatch и
 * server-side подтверждение намеренно разделены.
 */
void observe_direct_order_confirmation(bool item_found, bool toggle,
    int health) {
    const ULONGLONG dispatched_at = g_last_order_dispatched_at.load(
        std::memory_order_acquire);
    if (dispatched_at == 0)
        return;

    const ULONGLONG now = GetTickCount64();
    if (now < dispatched_at)
        return;

    const bool target_toggle = g_last_order_target_toggle.load(
        std::memory_order_relaxed);
    // Each submitted step has a known inverse starting toggle. An ON health
    // gain is not required: concurrent damage can consume it between samples.
    const bool confirmed = health>0 && item_found && toggle == target_toggle;
    constexpr ULONGLONG confirmation_timeout_ms = 2500;
    if (health>0 && !confirmed && now - dispatched_at <= confirmation_timeout_ms)
        return;

    ULONGLONG expected = dispatched_at;
    if (!g_last_order_dispatched_at.compare_exchange_strong(expected, 0,
            std::memory_order_acq_rel, std::memory_order_relaxed))
        return;

    if (confirmed) {
        g_monitor.orders_confirmed.fetch_add(1, std::memory_order_relaxed);
        log_message("ORDER", target_toggle
            ? "Direct CAST_TOGGLE confirmed: Armlet is ON."
            : "Direct CAST_TOGGLE confirmed: Armlet is OFF.");
    } else {
        g_monitor.orders_timed_out.fetch_add(1, std::memory_order_relaxed);
        log_message("ERROR", health<=0
            ? "Hero died before the Armlet order was confirmed."
            : "Direct CAST_TOGGLE was not confirmed within 2500 ms.");
    }
}
