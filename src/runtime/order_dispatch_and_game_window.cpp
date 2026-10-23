/**
 * @file
 * @brief Диспетчер приказов и инфраструктура окна.
 *
 * Находит функции движка по уникальным сигнатурам, публикует запрос через атомарное состояние и
 * PostMessageW. PrepareUnitOrders вызывается обработчиком игрового окна; worker лишь ставит
 * запрос. Здесь также находятся базовые операции окна и Direct3D.
 */

// Signatures, order dispatch and game window handling.

void open_console() {
    // Lifecycle hook retained for the worker flow. AllocConsole is deliberately
    // omitted because it creates a distracting extra window in the game session.
}

/**
 * @brief Принимает результат поиска только при однозначном совпадении в проверенном диапазоне
 * образа; неоднозначность означает отказ от разрешения адреса.
 */
std::uintptr_t find_unique_executable_pattern(std::uintptr_t module_base,
    std::size_t module_size, const std::uint8_t* pattern, const char* mask,
    std::size_t pattern_size) {
    if (!plausible_pointer(module_base) || pattern == nullptr || mask == nullptr ||
        pattern_size == 0 || module_size < pattern_size)
        return 0;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        module_base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    std::uintptr_t result = 0;
    std::size_t matches = 0;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned section_index = 0;
         section_index < nt->FileHeader.NumberOfSections; ++section_index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        const std::size_t section_offset = section->VirtualAddress;
        const std::size_t section_size = section->Misc.VirtualSize;
        if (section_offset >= module_size || section_size < pattern_size)
            continue;
        const std::size_t bounded_size = std::min(
            section_size, module_size - section_offset);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(
            module_base + section_offset);
        for (std::size_t offset = 0; offset + pattern_size <= bounded_size; ++offset) {
            if (mask[0] != '?' && bytes[offset] != pattern[0])
                continue;
            bool equal = true;
            for (std::size_t byte = 1; byte < pattern_size; ++byte) {
                if (mask[byte] != '?' && bytes[offset + byte] != pattern[byte]) {
                    equal = false;
                    break;
                }
            }
            if (!equal)
                continue;
            result = module_base + section_offset + offset;
            if (++matches > 1)
                return 0;
        }
    }
    return matches == 1 ? result : 0;
}

/**
 * @brief Преобразует RIP-relative displacement инструкции в адрес глобального значения;
 * применяется только к предварительно проверенной сигнатуре.
 */
std::uintptr_t resolve_rip_relative_global(std::uintptr_t instruction,
    std::size_t displacement_offset, std::size_t instruction_size,
    std::uintptr_t module_base, std::size_t module_size) {
    std::int32_t displacement = 0;
    // x86-64 instructions are byte-aligned. Do not apply the data-pointer
    // alignment check here: the current EntitySystem signature starts at an
    // unaligned RVA (0x17EB2CB), which previously made every runtime global
    // rescan fail even though the pattern was unique and correct.
    if (instruction < module_base ||
        instruction + instruction_size > module_base + module_size ||
        displacement_offset + sizeof(displacement) > instruction_size ||
        !safe_read(instruction + displacement_offset, displacement))
        return 0;
    const auto resolved = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(instruction + instruction_size) + displacement);
    return resolved >= module_base && resolved + sizeof(void*) <= module_base + module_size
        ? resolved : 0;
}

/**
 * @brief Разрешает адреса своей подсистемы по текущему образу клиента. false оставляет
 * зависимую функцию недоступной до повторной инициализации.
 */
bool initialize_runtime_globals(std::uintptr_t client_base,
    std::size_t client_size) {
    constexpr std::array<std::uint8_t, 17> entity_system_pattern{
        0x48, 0x8B, 0x1D, 0, 0, 0, 0,
        0x48, 0x89, 0x1D, 0, 0, 0, 0,
        0x4C, 0x63, 0xB3,
    };
    constexpr char entity_system_mask[] = "xxx????xxx????xxx";
    constexpr std::array<std::uint8_t, 10> global_vars_pattern{
        0x48, 0x89, 0x15, 0, 0, 0, 0, 0x48, 0x89, 0x42,
    };
    constexpr char global_vars_mask[] = "xxx????xxx";
    constexpr std::array<std::uint8_t, 19> game_rules_pattern{
        0xF6, 0xC1, 0x01, 0x0F, 0x85, 0, 0, 0, 0,
        0x4C, 0x8B, 0x05, 0, 0, 0, 0, 0x4D, 0x85, 0xC0,
    };
    constexpr char game_rules_mask[] = "xxxxx????xxx????xxx";
    constexpr std::array<std::uint8_t, 21> prediction_pattern{
        0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3,
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
        0x40, 0x53, 0x56, 0x41, 0x54,
    };
    constexpr char prediction_mask[] = "xxx????xxxxxxxxxxxxxx";
    constexpr std::array<std::uint8_t, 12> local_pawn_reference_pattern{
        0x4C, 0x39, 0xB6, 0, 0, 0, 0, 0x74, 0, 0x44, 0x88, 0xBE,
    };
    constexpr char local_pawn_reference_mask[] = "xxx????x?xxx";
    // This is the same RIP-relative signature used by the local Source 2
    // dumper for dwViewMatrix; no patch-specific RVA is embedded.
    constexpr std::array<std::uint8_t, 11> view_matrix_pattern{
        0x48, 0x8D, 0x0D, 0, 0, 0, 0, 0x48, 0xC1, 0xE0, 0x06,
    };
    constexpr char view_matrix_mask[] = "xxx????xxxx";

    const auto entity_instruction = find_unique_executable_pattern(
        client_base, client_size, entity_system_pattern.data(),
        entity_system_mask, entity_system_pattern.size());
    const auto globals_instruction = find_unique_executable_pattern(
        client_base, client_size, global_vars_pattern.data(),
        global_vars_mask, global_vars_pattern.size());
    const auto game_rules_instruction = find_unique_executable_pattern(
        client_base, client_size, game_rules_pattern.data(),
        game_rules_mask, game_rules_pattern.size());
    const auto prediction_instruction = find_unique_executable_pattern(
        client_base, client_size, prediction_pattern.data(),
        prediction_mask, prediction_pattern.size());
    const auto local_pawn_reference = find_unique_executable_pattern(
        client_base, client_size, local_pawn_reference_pattern.data(),
        local_pawn_reference_mask, local_pawn_reference_pattern.size());
    const auto view_matrix_instruction = find_unique_executable_pattern(
        client_base, client_size, view_matrix_pattern.data(),
        view_matrix_mask, view_matrix_pattern.size());
    const auto entity_global = resolve_rip_relative_global(
        entity_instruction, 3, 7, client_base, client_size);
    const auto vars_global = resolve_rip_relative_global(
        globals_instruction, 3, 7, client_base, client_size);
    const auto rules_global = resolve_rip_relative_global(
        game_rules_instruction, 12, 16, client_base, client_size);
    const auto prediction_global = resolve_rip_relative_global(
        prediction_instruction, 3, 7, client_base, client_size);
    const auto view_matrix = resolve_rip_relative_global(
        view_matrix_instruction, 3, 7, client_base, client_size);
    std::uint32_t local_pawn_displacement = 0;
    std::uintptr_t local_pawn_global = 0;
    if (prediction_global != 0 && local_pawn_reference != 0 &&
        safe_read(local_pawn_reference + 3, local_pawn_displacement) &&
        local_pawn_displacement <= 0x10000) {
        const auto candidate = prediction_global + local_pawn_displacement;
        if (candidate >= client_base &&
            candidate + sizeof(void*) <= client_base + client_size)
            local_pawn_global = candidate;
    }

    const auto previous_entity_global = g_game_entity_system_global.exchange(
        entity_global, std::memory_order_acq_rel);
    if (previous_entity_global != entity_global) {
        g_entity_chunk_table_offset.store(0, std::memory_order_release);
        g_entity_entry_stride.store(0, std::memory_order_release);
    }
    g_global_vars_global.store(vars_global, std::memory_order_release);
    g_game_rules_global.store(rules_global, std::memory_order_release);
    g_local_player_pawn_global.store(local_pawn_global, std::memory_order_release);
    g_view_matrix_address.store(view_matrix, std::memory_order_release);
    // Resolve the native UI-manager getter + adjacent state getter as one
    // verified layout. No build RVA, panel scan or engine call in Present.
    const auto ui_getter=find_unique_executable_pattern(client_base,client_size,
        world_view_state::signature.data(),world_view_state::mask.data(),world_view_state::signature.size());
    world_view_state::layout ui{};std::uintptr_t ui_table=0;int ui_state=-1;
    const bool ui_ready=ui_getter&&world_view_state::decode(reinterpret_cast<const std::uint8_t*>(ui_getter),ui_getter,ui)&&
        ui.object>=client_base&&ui.object+ui.offset+sizeof(int)<=client_base+client_size&&
        safe_read(ui.object,ui_table)&&ui_table>=client_base&&ui_table<client_base+client_size&&
        safe_read(ui.object+ui.offset,ui_state)&&ui_state>=0&&ui_state<=3;
    g_world_ui_state_address.store(ui_ready?ui.object+ui.offset:0,std::memory_order_release);
    g_render_diagnostics.view_matrix_ready.store(
        view_matrix != 0, std::memory_order_relaxed);
    return entity_global != 0 && vars_global != 0;
}

/**
 * @brief Принимает результат поиска только при однозначном совпадении в проверенном диапазоне
 * образа; неоднозначность означает отказ от разрешения адреса.
 */
std::uintptr_t find_unique_readable_string(std::uintptr_t module_base,
    std::size_t module_size, const char* text) {
    if (!plausible_pointer(module_base) || module_size == 0 || text == nullptr)
        return 0;

    const std::size_t text_size = std::strlen(text) + 1;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        module_base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    std::uintptr_t result = 0;
    std::size_t matches = 0;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned section_index = 0;
         section_index < nt->FileHeader.NumberOfSections; ++section_index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0)
            continue;
        const std::size_t section_offset = section->VirtualAddress;
        const std::size_t section_size = section->Misc.VirtualSize;
        if (section_offset >= module_size || section_size < text_size)
            continue;
        const std::size_t bounded_size = std::min(
            section_size, module_size - section_offset);
        const auto* bytes = reinterpret_cast<const char*>(module_base + section_offset);
        for (std::size_t offset = 0; offset + text_size <= bounded_size; ++offset) {
            if (bytes[offset] != text[0] ||
                std::memcmp(bytes + offset, text, text_size) != 0)
                continue;
            result = module_base + section_offset + offset;
            if (++matches > 1)
                return 0;
        }
    }
    return matches == 1 ? result : 0;
}

/**
 * @brief Разрешает адреса своей подсистемы по текущему образу клиента. false оставляет
 * зависимую функцию недоступной до повторной инициализации.
 */
bool initialize_internal_projectile_feed(std::uintptr_t client_base,
    std::size_t client_size) {
    const auto manager_name = find_unique_readable_string(
        client_base, client_size, "C_DOTA_ProjectileManager");
    if (manager_name == 0)
        return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(client_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        client_base + static_cast<std::uintptr_t>(dos->e_lfanew));
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    std::uintptr_t initializer = 0;
    std::size_t matches = 0;
    for (unsigned section_index = 0;
         section_index < nt->FileHeader.NumberOfSections; ++section_index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        const std::size_t section_offset = section->VirtualAddress;
        const std::size_t section_size = section->Misc.VirtualSize;
        if (section_offset >= client_size || section_size < 0x38)
            continue;
        const std::size_t bounded_size = std::min(
            section_size, client_size - section_offset);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(
            client_base + section_offset);
        for (std::size_t offset = 4; offset + 0x34 < bounded_size; ++offset) {
            if (bytes[offset] != 0x48 || bytes[offset + 1] != 0x8D ||
                bytes[offset + 2] != 0x15)
                continue;
            std::int32_t displacement = 0;
            std::memcpy(&displacement, bytes + offset + 3, sizeof(displacement));
            const auto instruction = client_base + section_offset + offset;
            const auto referenced = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(instruction + 7) + displacement);
            if (referenced != manager_name)
                continue;

            const auto start = bytes + offset - 4;
            const bool structure_matches =
                start[0] == 0x48 && start[1] == 0x83 &&
                start[2] == 0xEC && start[3] == 0x28 &&
                start[0x25] == 0x48 && start[0x26] == 0x8D &&
                start[0x27] == 0x05 && start[0x2C] == 0x48 &&
                start[0x2D] == 0x89 && start[0x2E] == 0x05 &&
                start[0x33] == 0x48 && start[0x34] == 0x83 &&
                start[0x35] == 0xC4 && start[0x36] == 0x28 &&
                start[0x37] == 0xC3;
            if (!structure_matches)
                continue;
            initializer = instruction - 4;
            if (++matches > 1)
                return false;
        }
    }
    if (matches != 1 || initializer == 0)
        return false;

    const auto global_instruction = initializer + 0x25;
    std::int32_t global_displacement = 0;
    if (!safe_read(global_instruction + 3, global_displacement))
        return false;
    const auto manager_global = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(global_instruction + 7) + global_displacement);
    if (manager_global < client_base || manager_global + sizeof(void*) >
        client_base + client_size)
        return false;

    g_projectile_manager_global.store(manager_global, std::memory_order_release);
    return true;
}

/**
 * @brief Разрешает адреса своей подсистемы по текущему образу клиента. false оставляет
 * зависимую функцию недоступной до повторной инициализации.
 */
bool initialize_direct_order_path(std::uintptr_t client_base,
    std::size_t client_size) {
    // Current C_DOTAPlayerController::PrepareUnitOrders prologue. This is
    // intentionally resolved by a unique pattern instead of a patch-specific RVA.
    constexpr std::array<std::uint8_t, 37> prepare_pattern{
        0x4C, 0x89, 0x4C, 0x24, 0x20, 0x44, 0x89, 0x44, 0x24, 0x18,
        0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55,
        0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xE0,
        0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00,
    };
    constexpr char prepare_mask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

    // Accessor used by Dota's own item/ability input handlers. Passing zero
    // returns the local C_DOTAPlayerController for split-screen slot zero.
    constexpr std::array<std::uint8_t, 23> controller_pattern{
        0x33, 0xC0, 0x83, 0xF9, 0xFF, 0x0F, 0x44, 0xC8, 0x48, 0x63,
        0xC1, 0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B,
        0x04, 0xC1, 0xC3,
    };
    constexpr char controller_mask[] = "xxxxxxxxxxxxxx????xxxxx";

    const auto prepare = find_unique_executable_pattern(client_base, client_size,
        prepare_pattern.data(), prepare_mask, prepare_pattern.size());
    const auto controller = find_unique_executable_pattern(client_base, client_size,
        controller_pattern.data(), controller_mask, controller_pattern.size());

    std::uintptr_t controller_global = 0;
    if (controller != 0) {
        std::int32_t displacement = 0;
        // 48 8D 0D xx xx xx xx at accessor + 0x0B; RIP is accessor + 0x12.
        if (safe_read(controller + 0x0E, displacement)) {
            const auto candidate = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(controller + 0x12) + displacement);
            if (candidate >= client_base &&
                candidate + sizeof(void*) <= client_base + client_size)
                controller_global = candidate;
        }
    }
    g_prepare_unit_orders.store(prepare, std::memory_order_release);
    g_local_controller_global.store(controller_global, std::memory_order_release);
    const auto previous_accessor = g_local_controller_accessor.exchange(
        controller, std::memory_order_acq_rel);
    if (previous_accessor != controller) {
        g_local_controller_address.store(0, std::memory_order_release);
        controller_from_accessor();
    }
    return prepare != 0 && controller != 0;
}

/**
 * @brief Выполняется в потоке игрового окна: повторно проверяет контекст запроса и лишь затем
 * вызывает engine order path.
 */
// Stable internal diagnostic codes, visible on the Diagnostics page. These
// do not change the versioned public status ABI.
void reject_armlet_order(int reason, const char* detail) {
    g_monitor.order_rejection.store(reason,std::memory_order_relaxed);
    g_monitor.orders_rejected.fetch_add(1,std::memory_order_relaxed);
    g_pending_order_state.store(pending_order_state::empty,std::memory_order_release);
    log_message("ORDER",detail);
}

void execute_pending_armlet_order() {
    pending_order_state expected = pending_order_state::queued;
    if (!g_pending_order_state.compare_exchange_strong(expected,
            pending_order_state::dispatching, std::memory_order_acq_rel) ||
        !g_running.load(std::memory_order_relaxed)) {
        if (expected == pending_order_state::dispatching)
            return;
        g_pending_order_state.store(pending_order_state::empty,
            std::memory_order_release);
        return;
    }

    const auto client_base = g_client_base_address.load(std::memory_order_acquire);
    bool game_paused = false;
    if (!read_game_paused(client_base, game_paused) || game_paused ||
        g_monitor.game_paused.load(std::memory_order_relaxed)) {
        reject_armlet_order(1,"Queued Armlet order cancelled: pause unavailable or paused.");
        return;
    }
    const auto armlet = g_pending_armlet.load(std::memory_order_relaxed);
    const auto hero = g_pending_hero.load(std::memory_order_relaxed);
    const int ability_index = g_pending_ability_index.load(std::memory_order_relaxed);
    const bool expected_toggle = g_pending_expected_toggle.load(
        std::memory_order_relaxed);
    const auto order_kind = g_pending_order_kind.load(
        std::memory_order_relaxed);
    instant_heal_candidate pending_heal{};
    pending_heal.item = g_pending_heal_item.load(std::memory_order_relaxed);
    pending_heal.entity_index = g_pending_heal_entity_index.load(
        std::memory_order_relaxed);
    pending_heal.kind = static_cast<instant_heal_kind>(
        g_pending_heal_kind.load(std::memory_order_relaxed));
    pending_heal.cast_order = g_pending_heal_order.load(std::memory_order_relaxed);
    pending_heal.target_index = g_pending_heal_target_index.load(
        std::memory_order_relaxed);
    pending_heal.heal = static_cast<float>(g_pending_heal_amount.load(
        std::memory_order_relaxed));
    const auto prepare_address = g_prepare_unit_orders.load(std::memory_order_acquire);
    auto controller = g_local_controller_address.load(std::memory_order_acquire);
    if (!plausible_pointer(controller))
        controller = controller_from_accessor();

    bool current_toggle = false;
    armlet_vec3 order_position{};
    std::uintptr_t entity_system = 0;
    const bool valid_order = plausible_pointer(client_base) && plausible_pointer(armlet) &&
        looks_like_local_hero(hero) && ability_index >= 0 && ability_index <= 0x7FFF &&
        plausible_pointer(prepare_address) && plausible_pointer(controller) &&
        read_world_position(hero, order_position) &&
        safe_read(armlet + schema_fields::m_bToggleState, current_toggle) &&
        current_toggle == expected_toggle &&
        read_game_entity_system(client_base, entity_system) &&
        entity_by_index(entity_system, ability_index) == armlet;
    if (!valid_order) {
        reject_armlet_order(2,"Direct CAST_TOGGLE cancelled: entity/toggle context changed.");
        return;
    }

    if (hero_from_controller(controller, entity_system) != hero) {
        reject_armlet_order(3,"Local C_DOTAPlayerController does not own this hero.");
        return;
    }

    using prepare_unit_orders_fn = void (*)(void*, int, int, const armlet_vec3*,
        int, int, void*, int, bool);
    const bool rearm_begin =
        order_kind == pending_armlet_order_kind::rearm_begin;
    int live_health = 0;
    int live_max_health = 0;
    std::uint64_t live_unit_state = 0;
    constexpr std::uint64_t blocks_items = (1ULL << 4) | (1ULL << 5);
    int rejection = 0;
    if (!g_monitor.abuse_enabled.load(std::memory_order_relaxed)) rejection=4;
    else if (!safe_read(hero+schema_fields::m_iHealth,live_health)||live_health<=0) rejection=5;
    else if (!safe_read(hero+schema_fields::m_nUnitState64,live_unit_state)||(live_unit_state&blocks_items)!=0) rejection=6;
    else if (find_usable_item_entity_index(client_base,hero,armlet)!=ability_index) rejection=7;
    if (rearm_begin) {
        const auto evaluated_at = g_pending_safety_evaluated_at.load(std::memory_order_relaxed);
        const auto dispatch_at = GetTickCount64();
        if(!rejection&&(evaluated_at==0||dispatch_at<evaluated_at||dispatch_at-evaluated_at>armlet_plan_lifetime_ms)) rejection=8;
        if(!rejection&&(live_health<g_pending_health.load(std::memory_order_relaxed)||
            !safe_read(hero+schema_fields::m_iMaxHealth,live_max_health)||
            live_max_health!=g_pending_max_health.load(std::memory_order_relaxed))) rejection=9;
    }
    int current_charges = 0;
    const bool heal_still_ready = pending_heal &&
        find_usable_item_entity_index(client_base, hero, pending_heal.item) ==
            pending_heal.entity_index &&
        entity_by_index(entity_system, pending_heal.entity_index) == pending_heal.item &&
        instant_heal_item_ready(pending_heal.item, pending_heal.kind, hero, current_charges) &&
        instant_heal_amount(pending_heal.kind, current_charges) + 0.5f >= pending_heal.heal &&
        (pending_heal.cast_order != 6 ||
            entity_by_index(entity_system, pending_heal.target_index) == hero);
    if(!rejection&&rearm_begin&&pending_heal&&!heal_still_ready) rejection=10;
    if (rejection) {
        reject_armlet_order(rejection,"Armlet preflight cancelled; see Diagnostics for the exact reason.");
        return;
    }
    g_monitor.order_rejection.store(0,std::memory_order_relaxed);
    constexpr int cast_toggle_order = 9;
    // Never use issuer DEFAULT here: Dota resolves DEFAULT through the current
    // selection, so selecting an enemy/controlled unit makes the game search
    // that unit's inventory for Armlet. Bind every automated item order to the
    // controller-assigned local hero explicitly. Native item call sites use
    // PASSED_UNIT_ONLY with a C_DOTA_BaseNPC pointer for this exact path.
    constexpr int issuer_passed_unit_only = 3;
    constexpr int queue_never = 1;
    constexpr bool show_effects = true;
    // Neither OFF nor its recovery ON may inherit Shift's order queue.
    const int queue_behavior = queue_never;
    const int rearm_health_before = rearm_begin ? live_health : -1;
    const auto submit_armlet_toggle = [&] {
        reinterpret_cast<prepare_unit_orders_fn>(prepare_address)(
            reinterpret_cast<void*>(controller), cast_toggle_order, 0,
            &order_position, ability_index,
            issuer_passed_unit_only, reinterpret_cast<void*>(hero),
            queue_behavior, show_effects);
    };

    bool target_toggle=!expected_toggle;
    unsigned submitted_toggles=0;
    if (rearm_begin) {
        // A function call is not an engine state transition. Send OFF once;
        // only a freshly observed OFF permits the inverse command. If native
        // prediction applies OFF synchronously, keep the zero-extra-frame path.
        submit_armlet_toggle();
        ++submitted_toggles;
        target_toggle=false;
        g_last_rearm_started_at.store(GetTickCount64(),std::memory_order_release);
        bool after_off=true;
        if(safe_read(armlet+schema_fields::m_bToggleState,after_off) && !after_off) {
            submit_armlet_toggle();
            ++submitted_toggles;
            target_toggle=true;
            g_last_rearm_submitted_at.store(GetTickCount64(),std::memory_order_release);
        }
    }

    if (target_toggle && pending_heal) {
        if (heal_still_ready) {
            reinterpret_cast<prepare_unit_orders_fn>(prepare_address)(
                reinterpret_cast<void*>(controller), pending_heal.cast_order,
                pending_heal.target_index,
                &order_position, pending_heal.entity_index,
                issuer_passed_unit_only, reinterpret_cast<void*>(hero),
                queue_behavior, show_effects);
            g_monitor.instant_heal_uses.fetch_add(1, std::memory_order_relaxed);
            char heal_message[160]{};
            std::snprintf(heal_message, sizeof(heal_message),
                rearm_begin
                    ? "Submitted %s (%d HP) immediately after state-confirmed Armlet rearm."
                    : "Submitted %s (%d HP) immediately before Armlet ON.",
                instant_heal_name(pending_heal.kind),
                static_cast<int>(std::lround(pending_heal.heal)));
            log_message("HEAL", heal_message);
        } else {
            // Never leave Armlet OFF because a consumable changed after the
            // safety snapshot. Skip the heal and still send ON immediately.
            log_message("HEAL",
                rearm_begin
                    ? "Planned instant heal became unavailable after state-confirmed Armlet rearm."
                    : "Planned instant heal became unavailable; forcing Armlet ON without delay.");
        }
    }
    if (!rearm_begin) {
        submit_armlet_toggle();
        ++submitted_toggles;
    }

    if(!rearm_begin && target_toggle)
        g_last_rearm_submitted_at.store(GetTickCount64(), std::memory_order_release);
    g_last_order_target_toggle.store(target_toggle, std::memory_order_relaxed);
    g_last_order_kind.store(order_kind, std::memory_order_relaxed);
    g_last_rearm_health_before.store(rearm_health_before,
        std::memory_order_relaxed);
    g_last_order_dispatched_at.store(GetTickCount64(), std::memory_order_release);
    g_monitor.orders_dispatched.fetch_add(
        submitted_toggles, std::memory_order_relaxed);

    char message[160]{};
    if (rearm_begin) {
        std::snprintf(message, sizeof(message),
            "Submitted Armlet rearm step for entity %d: %u toggle(s), target=%s (queue=NEVER).",
            ability_index,submitted_toggles,target_toggle?"ON":"OFF");
    } else {
        std::snprintf(message, sizeof(message),
            "Submitted direct CAST_TOGGLE for Armlet entity %d (issuer=LOCAL_HERO, queue=NEVER, target=%s).",
            ability_index, expected_toggle ? "OFF" : "ON");
    }
    log_message("ORDER", message);
    g_pending_order_state.store(pending_order_state::empty,
        std::memory_order_release);
}

/**
 * @brief Обрабатывает приватное сообщение диспетчера и сохраняет цепочку оригинального
 * WndProc; глубина callback участвует в корректном снятии перехвата.
 */
// SDL's text-input queries are main-thread-only. Query on Dota's window
// thread, publish an atomic result; the worker must never call SDL itself.
void refresh_game_text_input_state() {
    using focus_fn = void* (*)();
    using active_fn = bool (*)(void*);
    static const auto sdl = GetModuleHandleW(L"SDL3.dll");
    static const auto focus = sdl ? reinterpret_cast<focus_fn>(GetProcAddress(sdl,"SDL_GetKeyboardFocus")) : nullptr;
    static const auto active = sdl ? reinterpret_cast<active_fn>(GetProcAddress(sdl,"SDL_TextInputActive")) : nullptr;
    void* focused = focus ? focus() : nullptr;
    g_game_text_input_active.store(focused && active && active(focused),std::memory_order_release);
}

LRESULT CALLBACK game_window_dispatch_proc(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam) {
    g_game_window_proc_depth.fetch_add(1, std::memory_order_acq_rel);
    refresh_game_text_input_state();
    LRESULT result = 0;
    bool hotkey_consumed=false;
    if(message==WM_KILLFOCUS || message==WM_SETFOCUS || message==WM_ACTIVATE) {
        g_game_abuse_presses.store(0,std::memory_order_release);
        comfort_runtime::key_presses.store(0,std::memory_order_release);
        g_game_insert_presses.store(0,std::memory_order_release);
        g_game_hotkey_focus_time.store(static_cast<DWORD>(GetMessageTime()),std::memory_order_release);
    }
    if((message==WM_KEYDOWN||message==WM_SYSKEYDOWN)&&
        !(lparam&((1LL<<30)|(1LL<<29)))&&GetForegroundWindow()==window&&
        static_cast<std::int32_t>(static_cast<DWORD>(GetMessageTime())-
            g_game_hotkey_focus_time.load(std::memory_order_acquire))>=0&&
        // The dashboard may keep SDL text input active without an editor.
        // Insert is a reserved UI command and must remain available there.
        (wparam==VK_INSERT || !g_game_text_input_active.load(std::memory_order_acquire))) {
        if(wparam==VK_INSERT) {g_game_insert_presses.fetch_add(1,std::memory_order_release);hotkey_consumed=true;}
        else if(wparam==VK_END) {g_running.store(false,std::memory_order_relaxed);hotkey_consumed=true;}
        else if(!g_game_text_input_active.load(std::memory_order_acquire)&&g_bind_capture_active.load(std::memory_order_acquire)) {
            g_bind_capture_key.store(static_cast<int>(wparam),std::memory_order_release);
            hotkey_consumed=true;
        } else if(wparam != 0 && static_cast<int>(wparam) == comfort_runtime::silence_key.load(std::memory_order_acquire)) {
            comfort_runtime::key_presses.fetch_add(1,std::memory_order_release); hotkey_consumed=true;
        } else if(!g_game_text_input_active.load(std::memory_order_acquire)&&wparam!=0&&static_cast<int>(wparam)==g_monitor.abuse_hotkey.load(std::memory_order_relaxed)) {
            g_game_abuse_presses.fetch_add(1,std::memory_order_release);hotkey_consumed=true;
        }
    }
    const auto menu=world_frame::active_menu_window();
    const bool menu_visible=menu&&IsWindowVisible(menu);
    const bool owned_focus_transfer=menu_visible&&
        ((message==WM_ACTIVATE&&LOWORD(wparam)==WA_INACTIVE&&reinterpret_cast<HWND>(lparam)==menu)||
         (message==WM_KILLFOCUS&&reinterpret_cast<HWND>(wparam)==menu));
    if(hotkey_consumed) {
        result=0;
    } else if (message == execute_armlet_order_message) {
        execute_pending_armlet_order();
    } else if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_KEYMENU) {
        result = 0;
    } else if (message == WM_SYSKEYDOWN && wparam == VK_MENU && ((lparam & (1LL << 30)) != 0)) {
        result = 0;
    } else if(queue_native_menu_input(window,message,wparam,lparam)) {
        // Raw input still requires DefWindowProc cleanup even when consumed.
        result=message==WM_INPUT?DefWindowProcW(window,message,wparam,lparam):0;
    } else if(owned_focus_transfer) {
        // An owned settings window is not leaving the game. Do not let SDL's
        // focus-loss path minimize an exclusive-fullscreen game beneath it.
        result=0;
    } else if(menu_visible&&message==WM_INPUT&&GetForegroundWindow()==menu) {
        // The game may register background raw input. It must not process the
        // menu's clicks or reclaim its capture; DefWindowProc performs cleanup.
        result=DefWindowProcW(window,message,wparam,lparam);
    } else {
        const WNDPROC original = g_original_game_window_proc.load(
            std::memory_order_acquire);
        result = original != nullptr
            ? CallWindowProcW(original, window, message, wparam, lparam)
            : DefWindowProcW(window, message, wparam, lparam);
    }
    // Enter/Escape can change the SDL text-input mode in the original proc.
    refresh_game_text_input_state();
    g_game_window_proc_depth.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

/**
 * @brief Устанавливает/снимает обработчик игрового окна. Жизненный цикл должен завершаться до
 * выгрузки кода DLL.
 */
bool install_game_window_order_dispatch(HWND window) {
    if (!IsWindow(window))
        return false;
    const auto hook = reinterpret_cast<WNDPROC>(&game_window_dispatch_proc);
    const auto existing = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(window, GWLP_WNDPROC));
    if (existing == nullptr || existing == hook)
        return existing == hook;

    g_original_game_window_proc.store(existing, std::memory_order_release);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(hook));
    if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
        g_original_game_window_proc.store(nullptr, std::memory_order_release);
        return false;
    }
    g_original_game_window_proc.store(
        reinterpret_cast<WNDPROC>(previous), std::memory_order_release);
    g_order_dispatch_window = window;
    return true;
}

bool order_dispatch_installed() {
    return IsWindow(g_order_dispatch_window) &&
        g_original_game_window_proc.load(std::memory_order_acquire) != nullptr &&
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(
            g_order_dispatch_window, GWLP_WNDPROC)) ==
            reinterpret_cast<WNDPROC>(&game_window_dispatch_proc);
}

/**
 * @brief Отменяет ещё ожидающий запрос через состояние очереди; уже начатое выполнение
 * является отдельным состоянием dispatching.
 */
bool cancel_queued_armlet_order() {
    pending_order_state queued = pending_order_state::queued;
    return g_pending_order_state.compare_exchange_strong(
        queued, pending_order_state::empty, std::memory_order_acq_rel);
}

/**
 * @brief Устанавливает/снимает обработчик игрового окна. Жизненный цикл должен завершаться до
 * выгрузки кода DLL.
 */
void uninstall_game_window_order_dispatch() {
    g_pending_order_state.store(pending_order_state::empty,
        std::memory_order_release);
    const WNDPROC hook = reinterpret_cast<WNDPROC>(&game_window_dispatch_proc);
    const WNDPROC original = g_original_game_window_proc.load(
        std::memory_order_acquire);
    if (IsWindow(g_order_dispatch_window) && original != nullptr &&
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(
            g_order_dispatch_window, GWLP_WNDPROC)) == hook) {
        SetWindowLongPtrW(g_order_dispatch_window, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(original));
    }
    g_original_game_window_proc.store(nullptr, std::memory_order_release);
    g_order_dispatch_window = nullptr;
    for (int wait = 0; wait < 100 &&
         g_game_window_proc_depth.load(std::memory_order_acquire) != 0; ++wait)
        Sleep(1);
}

/**
 * @brief Worker публикует запрос и будит игровое окно через PostMessageW. true означает
 * постановку в очередь, не выполнение и не подтверждение toggle.
 */
bool queue_direct_armlet_order(std::uintptr_t armlet, std::uintptr_t hero,
    int ability_index, bool expected_toggle,
    pending_armlet_order_kind order_kind,
    const instant_heal_candidate* instant_heal = nullptr,
    ULONGLONG safety_evaluated_at = 0) {
    if (g_monitor.game_paused.load(std::memory_order_relaxed)) {
        log_message("PAUSE", "Direct CAST_TOGGLE is suspended while the game is paused.");
        return false;
    }
    if (!plausible_pointer(hero)) {
        log_message("ERROR", "Direct CAST_TOGGLE is not ready: local hero is unresolved.");
        return false;
    }
    if (!plausible_pointer(armlet) || ability_index < 0 || ability_index > 0x7FFF) {
        log_message("ERROR", "Direct CAST_TOGGLE is not ready: Armlet entity is unresolved.");
        return false;
    }
    auto controller = g_local_controller_address.load(std::memory_order_acquire);
    if (!plausible_pointer(controller))
        controller = controller_from_accessor();
    if (g_prepare_unit_orders.load(std::memory_order_acquire) == 0 ||
        !plausible_pointer(controller)) {
        log_message("ERROR", "Direct CAST_TOGGLE is not ready: native signatures are unavailable.");
        return false;
    }
    if (g_original_game_window_proc.load(std::memory_order_acquire) == nullptr ||
        !IsWindow(g_order_dispatch_window)) {
        log_message("ERROR", "Direct CAST_TOGGLE is not ready: game-thread dispatcher is unavailable.");
        return false;
    }

    const bool accepts_heal = !expected_toggle ||
        order_kind == pending_armlet_order_kind::rearm_begin;
    const instant_heal_candidate normalized_heal =
        accepts_heal && instant_heal != nullptr ? *instant_heal
                                                : instant_heal_candidate{};

    // A single producer feeds the game-window consumer. Reuse only the exact
    // same queued order; a lost window message is released after a short timeout.
    constexpr ULONGLONG dispatch_timeout_ms = 500;
    const ULONGLONG now = GetTickCount64();
    auto state = g_pending_order_state.load(std::memory_order_acquire);
    if (state == pending_order_state::queued) {
        const ULONGLONG queued_at = g_pending_order_queued_at.load(
            std::memory_order_relaxed);
        if (now >= queued_at && now - queued_at > dispatch_timeout_ms) {
            pending_order_state queued = pending_order_state::queued;
            if (g_pending_order_state.compare_exchange_strong(queued,
                    pending_order_state::empty, std::memory_order_acq_rel)) {
                log_message("ERROR", "Timed out queued Armlet order; releasing it for retry.");
                state = pending_order_state::empty;
            } else {
                state = queued;
            }
        }
    }
    if (state != pending_order_state::empty) {
        return state == pending_order_state::queued &&
            g_pending_armlet.load(std::memory_order_relaxed) == armlet &&
            g_pending_hero.load(std::memory_order_relaxed) == hero &&
            g_pending_ability_index.load(std::memory_order_relaxed) == ability_index &&
            g_pending_expected_toggle.load(std::memory_order_relaxed) == expected_toggle &&
            g_pending_order_kind.load(std::memory_order_relaxed) == order_kind &&
            g_pending_heal_item.load(std::memory_order_relaxed) == normalized_heal.item &&
            g_pending_heal_entity_index.load(std::memory_order_relaxed) ==
                normalized_heal.entity_index;
    }

    g_pending_armlet.store(armlet, std::memory_order_relaxed);
    g_pending_hero.store(hero, std::memory_order_relaxed);
    g_pending_ability_index.store(ability_index, std::memory_order_relaxed);
    g_pending_expected_toggle.store(expected_toggle, std::memory_order_relaxed);
    g_pending_order_kind.store(order_kind, std::memory_order_relaxed);
    g_pending_heal_item.store(normalized_heal.item, std::memory_order_relaxed);
    g_pending_heal_entity_index.store(
        normalized_heal.entity_index, std::memory_order_relaxed);
    g_pending_heal_kind.store(
        static_cast<int>(normalized_heal.kind), std::memory_order_relaxed);
    g_pending_heal_order.store(normalized_heal.cast_order, std::memory_order_relaxed);
    g_pending_heal_target_index.store(
        normalized_heal.target_index, std::memory_order_relaxed);
    g_pending_heal_amount.store(static_cast<int>(std::lround(normalized_heal.heal)),
        std::memory_order_relaxed);
    g_pending_order_queued_at.store(now, std::memory_order_relaxed);
    g_pending_safety_evaluated_at.store(safety_evaluated_at, std::memory_order_relaxed);
    g_pending_health.store(g_monitor.health.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_pending_max_health.store(g_monitor.max_health.load(std::memory_order_relaxed), std::memory_order_relaxed);
    pending_order_state empty = pending_order_state::empty;
    if (!g_pending_order_state.compare_exchange_strong(empty,
            pending_order_state::queued, std::memory_order_release,
            std::memory_order_relaxed))
        return false;
    if (!PostMessageW(g_order_dispatch_window, execute_armlet_order_message, 0, 0)) {
        g_pending_order_state.store(pending_order_state::empty,
            std::memory_order_release);
        return false;
    }
    g_monitor.orders_queued.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/**
 * @brief Worker публикует запрос и будит игровое окно через PostMessageW. true означает
 * постановку в очередь, не выполнение и не подтверждение toggle.
 */
bool queue_direct_armlet_toggle(std::uintptr_t armlet, std::uintptr_t hero,
    int ability_index, bool expected_toggle,
    const instant_heal_candidate* instant_heal = nullptr) {
    return queue_direct_armlet_order(armlet, hero, ability_index,
        expected_toggle, pending_armlet_order_kind::single_toggle,
        instant_heal);
}

/**
 * @brief Worker публикует запрос и будит игровое окно через PostMessageW. true означает
 * постановку в очередь, не выполнение и не подтверждение toggle.
 */
bool queue_direct_armlet_rearm(std::uintptr_t armlet, std::uintptr_t hero,
    int ability_index,
    const instant_heal_candidate* instant_heal = nullptr,
    ULONGLONG safety_evaluated_at = 0) {
    return queue_direct_armlet_order(armlet, hero, ability_index,
        true, pending_armlet_order_kind::rearm_begin, instant_heal, safety_evaluated_at);
}

void close_console() {
    // Logging uses OutputDebugStringA and therefore has no console to release.
}

BOOL CALLBACK find_game_window_callback(HWND window, LPARAM parameter) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != GetCurrentProcessId() || window == GetConsoleWindow() ||
        window == g_overlay.window || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
        return TRUE;

    const LONG_PTR extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extended_style & WS_EX_TOOLWINDOW) != 0)
        return TRUE;

    RECT client{};
    if (!GetClientRect(window, &client))
        return TRUE;

    const auto area = static_cast<std::int64_t>(client.right - client.left) *
        static_cast<std::int64_t>(client.bottom - client.top);
    if (area <= 0)
        return TRUE;

    auto* best = reinterpret_cast<std::pair<HWND, std::int64_t>*>(parameter);
    if (area > best->second)
        *best = {window, area};

    return TRUE;
}

HWND find_game_window() {
    std::pair<HWND, std::int64_t> best{nullptr, 0};
    EnumWindows(find_game_window_callback, reinterpret_cast<LPARAM>(&best));
    return best.first;
}

/**
 * @brief Освобождает принадлежащие оверлею графические ресурсы; не уничтожает само окно игры.
 */
void destroy_render_target() {
    if (g_overlay.render_target != nullptr) {
        g_overlay.render_target->Release();
        g_overlay.render_target = nullptr;
    }
}

bool create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(g_overlay.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
        return false;

    const HRESULT result = g_overlay.device->CreateRenderTargetView(
        back_buffer, nullptr, &g_overlay.render_target);
    back_buffer->Release();
    return SUCCEEDED(result);
}

bool get_game_client_bounds(RECT& bounds);
RECT compact_overlay_bounds(const RECT& game_bounds);

LRESULT WINAPI overlay_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    static POINT drag_anchor{};
    // ImGui's Win32 backend expects an active window for reliable mouse and
    // keyboard capture. The worker accepts this owned menu as game context.
    if(message==WM_MOUSEACTIVATE && window==g_overlay.window && g_overlay.interactive)
        return MA_ACTIVATE;
    // Avoid DefWindowProc's modal caption-drag loop: it blocks the worker and
    // its polling/rendering until the button is released.
    if(window==g_overlay.window&&g_overlay.interactive) {
        if(message==WM_NCLBUTTONDOWN&&wparam==HTCAPTION) {
            POINT cursor{};RECT bounds{};
            if(GetCursorPos(&cursor)&&GetWindowRect(window,&bounds)) {
                drag_anchor={cursor.x-bounds.left,cursor.y-bounds.top};
                g_overlay.native_moving=true;SetCapture(window);
            }
            return 0;
        }
        if(message==WM_MOUSEMOVE&&g_overlay.native_moving&&GetCapture()==window) {
            POINT cursor{};RECT bounds{},game_bounds{};
            if(GetCursorPos(&cursor)&&GetWindowRect(window,&bounds)&&get_game_client_bounds(game_bounds)) {
                const LONG x=std::clamp(cursor.x-drag_anchor.x,game_bounds.left,
                    std::max(game_bounds.left,game_bounds.right-(bounds.right-bounds.left)));
                const LONG y=std::clamp(cursor.y-drag_anchor.y,game_bounds.top,
                    std::max(game_bounds.top,game_bounds.bottom-(bounds.bottom-bounds.top)));
                SetWindowPos(window,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            }
            return 0;
        }
        if(message==WM_LBUTTONUP&&g_overlay.native_moving) {
            g_overlay.native_moving=false;if(GetCapture()==window)ReleaseCapture();return 0;
        }
        if(message==WM_CAPTURECHANGED||message==WM_CANCELMODE) {
            g_overlay.native_moving=false;
            if(message==WM_CANCELMODE&&GetCapture()==window)ReleaseCapture();
        }
        if(message==WM_NCLBUTTONDBLCLK&&wparam==HTCAPTION)return 0;
    }
    if (window == g_overlay.window && g_overlay.interactive && ImGui::GetCurrentContext() != nullptr &&
        ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam))
        return 1;

    switch (message) {
    case WM_NCHITTEST: {
        if (window == g_overlay.window && g_overlay.interactive) {
            POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (ScreenToClient(window, &cursor) && cursor.y >= 0 && cursor.y < 34)
                return HTCAPTION;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    case WM_ENTERSIZEMOVE:
        if (window == g_overlay.window)
            g_overlay.native_moving = true;
        return 0;
    case WM_MOVE:
        if (window == g_overlay.window && g_overlay.native_moving) {
            RECT window_bounds{};
            RECT game_bounds{};
            if (GetWindowRect(window, &window_bounds) &&
                get_game_client_bounds(game_bounds)) {
                const RECT default_bounds = compact_overlay_bounds(game_bounds);
                g_overlay.x = window_bounds.left;
                g_overlay.y = window_bounds.top;
                g_overlay.drag_offset_x = window_bounds.left - default_bounds.left;
                g_overlay.drag_offset_y = window_bounds.top - default_bounds.top;
                g_overlay.user_positioned = true;
            }
        }
        return 0;
    case WM_EXITSIZEMOVE:
        if (window == g_overlay.window)
            g_overlay.native_moving = false;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        if (window == g_overlay.window)
            PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

/**
 * @brief Создаёт ресурсы Windows/Direct3D на worker-потоке. При частичном отказе caller обязан
 * выполнить общий путь очистки.
 */
bool create_device_and_swap_chain(HWND window, UINT width, UINT height) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Width = width;
    description.BufferDesc.Height = height;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL requested_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL created_level{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        requested_levels, static_cast<UINT>(std::size(requested_levels)), D3D11_SDK_VERSION,
        &description, &g_overlay.swap_chain, &g_overlay.device, &created_level,
        &g_overlay.context);

    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            requested_levels, static_cast<UINT>(std::size(requested_levels)), D3D11_SDK_VERSION,
            &description, &g_overlay.swap_chain, &g_overlay.device, &created_level,
            &g_overlay.context);
    }

    return SUCCEEDED(result) && create_render_target();
}

bool get_game_client_bounds(RECT& bounds) {
    RECT client{};
    if (g_overlay.game_window == nullptr || IsIconic(g_overlay.game_window) ||
        !GetClientRect(g_overlay.game_window, &client))
        return false;

    POINT top_left{client.left, client.top};
    POINT bottom_right{client.right, client.bottom};
    if (!ClientToScreen(g_overlay.game_window, &top_left) ||
        !ClientToScreen(g_overlay.game_window, &bottom_right))
        return false;

    bounds = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    return bounds.right > bounds.left && bounds.bottom > bounds.top;
}

RECT compact_overlay_bounds(const RECT& game_bounds) {
    // A layered DX11 surface cannot reliably use per-pixel transparent clearing
    // with this swap-chain setup: a full-screen surface makes untouched pixels
    // appear black on some Windows configurations. Keep the native surface no
    // larger than the panel instead. The ImGui panel itself is frameless, so the
    // old "window inside a window" appearance does not return.
    constexpr LONG desired_width = 760;
    constexpr LONG desired_height = 540;
    constexpr LONG inset = 16;
    const LONG game_width = game_bounds.right - game_bounds.left;
    const LONG game_height = game_bounds.bottom - game_bounds.top;
    const LONG width = std::min(desired_width, std::max<LONG>(1, game_width - 2 * inset));
    const LONG height = std::min(desired_height, std::max<LONG>(1, game_height - 2 * inset));
    return {
        game_bounds.left + inset,
        game_bounds.top + inset,
        game_bounds.left + inset + width,
        game_bounds.top + inset + height,
    };
}
