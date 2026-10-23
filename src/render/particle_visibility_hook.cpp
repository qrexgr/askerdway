/**
 * @file
 * @brief Хук частиц и кольцевой буфер.
 *
 * Проверяет layout и уникальную точку перехвата, вызывает оригинальный update и публикует
 * наблюдения без блокировок runtime. Кэш ограничивает стоимость обработки каждого кадра. Capture
 * и принудительная видимость являются разными режимами.
 */

// Particle visibility hook.

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include "particle_layout.h"
#include "../intelligence/include/askermap.h"
#include "particle_ability_match.h"
#include "particle_endcap_clock.h"

namespace {

askerdway::particles::layout g_particle_layout{};
std::size_t g_resource_name_field = 0;
std::uintptr_t g_resource_collection_vtable = 0;
std::size_t g_collection_resource_field = 0;
constexpr std::uint8_t kFogHiddenMask = 0x02;
constexpr std::uint32_t kTrackableParticleFlag = 1u << 31;
constexpr std::uint32_t kNamedParticleFlag = 1u << 30;
constexpr std::uint8_t kForceNoDrawMask = 0x01;
constexpr std::size_t kUpdateVtableIndex = 2;

// client.dll 22.08.2026: CNewParticleEffect::Update(CNewParticleEffect*, float).
// The address is resolved at runtime; the bytes also validate the expected ABI.
constexpr std::array<std::uint8_t, 26> kUpdatePattern{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9,
    0x0F, 0x29, 0x74, 0x24, 0x20, 0x48, 0x8B, 0x49, 0x20,
    0x48, 0x8D, 0x54, 0x24, 0x40, 0x0F, 0x28, 0xF1,
};

using particle_update_fn = void (*)(void*, float);
using set_particle_rendering_fn = void (*)(void*, bool);
std::uintptr_t g_render_collection_vtable = 0;
set_particle_rendering_fn g_collection_rendering = nullptr;
using get_control_point_fn = const armlet_vec3* (*)(void*, int);
get_control_point_fn g_get_control_point = nullptr;
std::uintptr_t g_control_point_collection_vtable=0;
std::size_t g_control_point_vtable_offset = 0;
std::atomic<void**> g_collection_render_slot{nullptr};
std::uintptr_t g_effect_vtable = 0;
std::array<std::uintptr_t,16> g_fog_setter_returns{};
std::size_t g_fog_setter_return_count = 0;
std::atomic<render_teleportation::particle_owner_validator> g_owner_validator{nullptr};
void hooked_collection_rendering(void*, bool) noexcept;

struct image_view {
    std::uint8_t* base{};
    std::size_t size{};
    IMAGE_NT_HEADERS64* nt{};
};

#if !defined(RENDER_TELEPORTATION_EMBEDDED)
HMODULE g_module{};
#endif
std::atomic<particle_update_fn> g_original_update{nullptr};
std::atomic<void**> g_update_slot{nullptr};
#if defined(RENDER_TELEPORTATION_EMBEDDED)
std::atomic<bool> g_enabled{false};
#else
std::atomic<bool> g_enabled{true};
#endif
std::atomic<bool> g_unloading{false};
std::atomic<bool> g_gameplay_context{true};
// CNewParticleEffect::Update runs on parallel engine jobs. Each thread owns a
// separate activity cache line; unload drains all jobs without a contended
// global RMW for every particle update.
struct alignas(64) active_worker_slot { std::atomic<bool> active{false}; };
std::array<active_worker_slot,64> g_active_workers{};
std::atomic<unsigned> g_worker_slots_used{0},g_active_worker_overflow{0};
thread_local unsigned g_worker_slot=~0u,g_worker_depth=0;
bool any_hook_active() noexcept {
    if(g_active_worker_overflow.load(std::memory_order_acquire))return true;
    for(const auto& worker:g_active_workers)if(worker.active.load(std::memory_order_acquire))return true;
    return false;
}
// Dota dispatches Update on parallel jobs. Serialize only our cache/publication
// work, never the original engine update, and install vtable slots under it.
SRWLOCK g_particle_state_lock=SRWLOCK_INIT;
struct particle_state_guard {
    particle_state_guard(){AcquireSRWLockExclusive(&g_particle_state_lock);}
    ~particle_state_guard(){ReleaseSRWLockExclusive(&g_particle_state_lock);}
};
std::atomic<std::uint64_t> g_updated_effects{0};
std::atomic<bool> g_trace_capture_enabled{false};

// Update is dispatched by one render thread. Accumulate the hot-path
// diagnostics there and publish snapshots occasionally; three relaxed stores
// per batch are considerably cheaper than atomic increments per callback.
constexpr std::uint32_t kDiagnosticPublishInterval = 256;
std::uint64_t g_hook_calls_local = 0;
std::uint64_t g_named_effects_local = 0;
std::uint64_t g_trackable_effects_local = 0;
std::uint64_t g_fow_hidden_effects_local = 0;
std::uint32_t g_diagnostic_calls_since_publish = 0;
std::atomic<std::uint64_t> g_hook_calls_published{0};
std::atomic<std::uint64_t> g_named_effects_published{0};
std::atomic<std::uint64_t> g_trackable_effects_published{0};
std::atomic<std::uint64_t> g_fow_hidden_effects_published{0};

struct particle_observation_slot {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<std::uint32_t> owner_handle{0xFFFFFFFFu};
    std::atomic<std::uintptr_t> owner_entity{0};
    std::atomic<std::uint32_t> flags{0};
    std::atomic<std::uint32_t> source_x_bits{0},source_y_bits{0},source_z_bits{0};
    std::atomic<std::uint32_t> x_bits{0};
    std::atomic<std::uint32_t> y_bits{0};
    std::atomic<std::uint32_t> z_bits{0};
    std::atomic<std::uint64_t> observed_at_ms{0};
    std::atomic<std::uintptr_t> effect{0}, collection{0};
    std::atomic<std::uint32_t> visibility{0};
    std::array<std::atomic<std::uint8_t>, 256> debug_name{};
};

constexpr std::size_t kObservationCapacity = 2048;
std::array<particle_observation_slot, kObservationCapacity> g_observations{};
std::atomic<std::uint64_t> g_observation_sequence{0};
std::atomic<std::uint64_t> g_observation_committed_sequence{0};
std::atomic<std::uint64_t> g_observation_epoch{1};
struct position_correction_slot {
    std::atomic<std::uint64_t> sequence{0},key{0},expires{0};
    std::array<std::atomic<float>,3> position{},reference{};
};
std::array<position_correction_slot,32> g_position_corrections{};
std::atomic<unsigned> g_position_correction_count{0};
std::atomic<render_teleportation::particle_attachment_reader> g_attachment_reader{nullptr};

struct particle_classification_cache_slot {
    std::atomic<std::uintptr_t> effect{0};
    std::atomic<std::uintptr_t> debug_name{0};
    // Only the render thread uses this resource identity; reset after drain.
    std::uintptr_t resource_binding = 0;
    std::uintptr_t collection = 0;
    std::uintptr_t classified_collection = 0;
    bool original_fog_policy = false;
    std::atomic<std::uint32_t> flags{0};
    // CNewParticleEffect::Update is dispatched by the render thread. Keeping
    // this cadence counter local avoids a locked RMW for every live particle
    // every frame; shutdown resets it only after the vtable slot is restored
    // and the active invocation has drained.
    std::uint32_t updates_since_probe = 0;
    std::atomic<std::uint64_t> last_capture_ms{0};
    std::atomic<std::uint64_t> visual_until{0};
    particle_endcap::clock endcap_clock{};
    char copied_name[256]{};
};

constexpr std::size_t kClassificationCacheCapacity = 32768;
std::array<particle_classification_cache_slot,
    kClassificationCacheCapacity> g_particle_classification_cache{};

struct collection_owner_slot {
    std::atomic<std::uintptr_t> collection{0}, effect{0};
    std::atomic<bool> world{false};
};
std::array<collection_owner_slot, 16384> g_collection_owners{};

std::size_t collection_owner_bucket(std::uintptr_t collection) noexcept {
    auto hash=collection>>4;
    hash^=hash>>30; hash*=0xbf58476d1ce4e5b9ULL; hash^=hash>>27;
    return (hash%(g_collection_owners.size()/4))*4;
}

void remember_collection(std::uintptr_t collection, std::uintptr_t effect, bool world) noexcept {
    if (collection==0) return;
    const auto begin=collection_owner_bucket(collection);
    auto* chosen=&g_collection_owners[begin];
    for (std::size_t i=begin; i<begin+4; ++i) {
        const auto key=g_collection_owners[i].collection.load(std::memory_order_relaxed);
        if (key==collection || key==0) { chosen=&g_collection_owners[i]; break; }
    }
    chosen->effect.store(effect,std::memory_order_relaxed);
    chosen->world.store(world,std::memory_order_relaxed);
    chosen->collection.store(collection,std::memory_order_release);
}

particle_classification_cache_slot& particle_cache_for(std::uintptr_t effect) noexcept {
    auto hash=effect>>4;hash^=hash>>30;hash*=0xbf58476d1ce4e5b9ULL;hash^=hash>>27;
    const auto begin=(hash%(g_particle_classification_cache.size()/4))*4;
    auto* chosen=&g_particle_classification_cache[begin];
    for(std::size_t i=begin;i<begin+4;++i) {
        auto& candidate=g_particle_classification_cache[i];
        const auto key=candidate.effect.load(std::memory_order_relaxed);
        if(key==effect)return candidate;
        if(key==0)return candidate;
        if(candidate.last_capture_ms.load()<chosen->last_capture_ms.load())chosen=&candidate;
    }
    return *chosen;
}

void debug_log(const char* level, const char* message) noexcept {
#if defined(ASKERDWAY_DIAGNOSTICS) && !ASKERDWAY_DIAGNOSTICS
    (void)level;(void)message;
#else
    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer),
                  "[render_teleportation] [%s] %s\n", level, message);
    OutputDebugStringA(buffer);
#endif
}

bool query_protection(const void* address, std::size_t size,
                      DWORD& protection) noexcept {
    if (address == nullptr || size == 0) {
        return false;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin > std::numeric_limits<std::uintptr_t>::max() - size) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 ||
        (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    const auto region_begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto region_end = region_begin + mbi.RegionSize;
    if (begin < region_begin || begin + size > region_end) {
        return false;
    }

    protection = mbi.Protect & 0xFFU;
    return true;
}

bool is_readable(const void* address, std::size_t size) noexcept {
    if (address == nullptr || size == 0) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin < 0x10000 || begin > 0x00007FFFFFFFFFFFULL - size) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        const auto* p = static_cast<const volatile char*>(address);
        volatile char dummy = p[0];
        dummy = p[size - 1];
        (void)dummy;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    DWORD protection{};
    if (!query_protection(address, size, protection)) {
        return false;
    }

    switch (protection) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
#endif
}

bool is_executable(const void* address) noexcept {
    DWORD protection{};
    if (!query_protection(address, 1, protection)) {
        return false;
    }

    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

/**
 * @brief Разбирает заголовки PE для границ образа; эти границы ограничивают дальнейший поиск и
 * проверку адресов.
 */
bool get_image_view(HMODULE module, image_view& image) noexcept {
    if (module == nullptr) {
        return false;
    }

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    if (!is_readable(base, sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!is_readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SizeOfImage == 0) {
        return false;
    }

    image = {base, nt->OptionalHeader.SizeOfImage, nt};
    return true;
}

/**
 * @brief Принимает результат поиска только при однозначном совпадении в проверенном диапазоне
 * образа; неоднозначность означает отказ от разрешения адреса.
 */
std::uint8_t* find_unique_update(const image_view& image) noexcept {
    auto* section = IMAGE_FIRST_SECTION(image.nt);
    std::uint8_t* match{};
    std::size_t matches{};

    for (unsigned index = 0; index < image.nt->FileHeader.NumberOfSections;
         ++index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        const std::size_t start = section->VirtualAddress;
        const std::size_t section_size = section->Misc.VirtualSize;
        if (start >= image.size || section_size < kUpdatePattern.size()) {
            continue;
        }

        const std::size_t available = image.size - start;
        const std::size_t scan_size = section_size < available ? section_size : available;
        auto* cursor = image.base + start;
        const auto last = scan_size - kUpdatePattern.size();
        for (std::size_t offset = 0; offset <= last; ++offset) {
            if (std::memcmp(cursor + offset, kUpdatePattern.data(),
                            kUpdatePattern.size()) == 0) {
                match = cursor + offset;
                if (++matches > 1) {
                    return nullptr;
                }
            }
        }
    }

    return matches == 1 ? match : nullptr;
}

template <std::size_t Size>
bool contains_bytes(const std::uint8_t* bytes, std::size_t byte_count,
                    const std::array<std::uint8_t, Size>& pattern) noexcept {
    if (bytes == nullptr || byte_count < Size) {
        return false;
    }
    for (std::size_t offset = 0; offset <= byte_count - Size; ++offset) {
        if (std::memcmp(bytes + offset, pattern.data(), Size) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Проверяет ожидаемую форму объекта/слота перед перехватом; несовпадение после патча
 * должно завершаться отказом, а не записью по старому смещению.
 */
bool validate_particle_layout(const image_view& image,
                              const std::uint8_t* update) noexcept {
    constexpr std::size_t evidence_size = 0x100;
    askerdway::particles::layout resolved{};
    if (!is_readable(update, evidence_size) ||
        !askerdway::particles::resolve_update({update, evidence_size}, resolved))
        return false;
    constexpr int render_init[]{0xC6,0x83,-1,-1,-1,-1,1};
    auto* section = IMAGE_FIRST_SECTION(image.nt);
    unsigned candidates = 0;
    for (unsigned index=0; index<image.nt->FileHeader.NumberOfSections;
         ++index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE)==0) continue;
        const std::size_t start=section->VirtualAddress;
        if (start>=image.size) continue;
        const std::size_t size=std::min<std::size_t>(section->Misc.VirtualSize,image.size-start);
        const std::span<const std::uint8_t> bytes{image.base+start,size};
        for (std::size_t at=0;at<size;++at) {
            if (!askerdway::particles::matches(bytes,at,render_init)) continue;
            auto candidate=resolved;
            if (askerdway::particles::resolve_constructor(
                    bytes.subspan(at,std::min<std::size_t>(0xA0,size-at)),candidate)) {
                resolved=candidate;
                ++candidates;
            }
        }
    }
    if (candidates!=1) return false;
    unsigned policy_candidates = 0;
    section = IMAGE_FIRST_SECTION(image.nt);
    for (unsigned index=0; index<image.nt->FileHeader.NumberOfSections;
         ++index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE)==0) continue;
        const auto start=static_cast<std::size_t>(section->VirtualAddress);
        if (start>=image.size) continue;
        const auto size=std::min<std::size_t>(section->Misc.VirtualSize,image.size-start);
        auto candidate=resolved;
        if (askerdway::particles::resolve_fog_policy({image.base+start,size},candidate)) {
            resolved.fog_check_mask=candidate.fog_check_mask;
            ++policy_candidates;
        }
    }
    if (policy_candidates!=1 || resolved.fog_check_mask==kFogHiddenMask ||
        resolved.fog_check_mask==kForceNoDrawMask) return false;
    unsigned dispatch_candidates=0;
    section=IMAGE_FIRST_SECTION(image.nt);
    for (unsigned index=0; index<image.nt->FileHeader.NumberOfSections; ++index,++section) {
        if ((section->Characteristics&IMAGE_SCN_MEM_EXECUTE)==0) continue;
        const auto start=static_cast<std::size_t>(section->VirtualAddress);
        if (start>=image.size) continue;
        const auto size=std::min<std::size_t>(section->Misc.VirtualSize,image.size-start);
        askerdway::particles::fog_dispatch dispatch{};
        if (!askerdway::particles::resolve_fog_dispatch({image.base+start,size},resolved,dispatch)) continue;
        g_fog_setter_return_count=dispatch.count;
        g_control_point_vtable_offset=dispatch.control_point_vtable_offset;
        for (std::size_t i=0;i<dispatch.count;++i)
            g_fog_setter_returns[i]=reinterpret_cast<std::uintptr_t>(image.base+start+dispatch.returns[i]);
        ++dispatch_candidates;
    }
    if (dispatch_candidates!=1) return false;
    // Update's owner-visibility branch is independently validated by
    // resolve_update. It disables the same collection before the world FoW
    // pass and must participate in the override too.
    if(g_fog_setter_return_count==g_fog_setter_returns.size())return false;
    g_fog_setter_returns[g_fog_setter_return_count++]=
        reinterpret_cast<std::uintptr_t>(update)+resolved.update_render_return;
    g_particle_layout=resolved;
    return true;
}

/**
 * @brief Проверяет ожидаемую форму объекта/слота перед перехватом; несовпадение после патча
 * должно завершаться отказом, а не записью по старому смещению.
 */
bool validate_update_slot(void** slot, const void* update) noexcept {
    if (slot == nullptr || reinterpret_cast<std::uintptr_t>(slot) % alignof(void*) != 0 ||
        !is_readable(slot - kUpdateVtableIndex, 5 * sizeof(void*))) {
        return false;
    }

    void* entries[5]{};
    std::memcpy(entries, slot - kUpdateVtableIndex, sizeof(entries));
    if (entries[kUpdateVtableIndex] != update) {
        return false;
    }

    for (void* entry : entries) {
        if (!is_executable(entry)) {
            return false;
        }
    }

    const auto* trivial_update = reinterpret_cast<const std::uint8_t*>(entries[3]);
    const auto* flags_getter = reinterpret_cast<const std::uint8_t*>(entries[4]);
    if (!is_readable(trivial_update, 3) || !is_readable(flags_getter, 4)) {
        return false;
    }

    const bool is_return = trivial_update[0] == 0xC3 ||
                           (trivial_update[0] == 0xC2 && trivial_update[1] == 0x00 &&
                            trivial_update[2] == 0x00);
    const bool is_flags_getter = flags_getter[0] == 0x0F && flags_getter[1] == 0xB6 &&
                                 flags_getter[2] == 0x41 && flags_getter[3] == 0x30;
    return is_return && is_flags_getter;
}

/**
 * @brief Принимает результат поиска только при однозначном совпадении в проверенном диапазоне
 * образа; неоднозначность означает отказ от разрешения адреса.
 */
void** find_unique_update_slot(const image_view& image, void* update) noexcept {
    auto* section = IMAGE_FIRST_SECTION(image.nt);
    void** match{};
    std::size_t matches{};

    for (unsigned index = 0; index < image.nt->FileHeader.NumberOfSections;
         ++index, ++section) {
        const auto characteristics = section->Characteristics;
        if ((characteristics & IMAGE_SCN_MEM_READ) == 0 ||
            (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
            continue;
        }

        const std::size_t start = section->VirtualAddress;
        const std::size_t section_size = section->Misc.VirtualSize;
        if (start >= image.size || section_size < sizeof(void*)) {
            continue;
        }

        const std::size_t available = image.size - start;
        const std::size_t scan_size = section_size < available ? section_size : available;
        auto begin = reinterpret_cast<std::uintptr_t>(image.base + start);
        const auto end = begin + scan_size;
        begin = (begin + alignof(void*) - 1) & ~(alignof(void*) - 1);

        for (auto address = begin; address + sizeof(void*) <= end;
             address += sizeof(void*)) {
            void* value{};
            std::memcpy(&value, reinterpret_cast<void*>(address), sizeof(value));
            if (value != update) {
                continue;
            }

            auto** candidate = reinterpret_cast<void**>(address);
            if (validate_update_slot(candidate, update)) {
                match = candidate;
                if (++matches > 1) {
                    return nullptr;
                }
            }
        }
    }

    return matches == 1 ? match : nullptr;
}

/**
 * @brief Меняет проверенный слот callback с контролем ожидаемого значения и защиты страницы;
 * не предполагает, что слот всё ещё принадлежит этому хуку.
 */
bool exchange_slot(void** slot, void* expected, void* replacement) noexcept {
    if (slot == nullptr || replacement == nullptr) {
        return false;
    }

    DWORD old_protection{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protection)) {
        return false;
    }

    auto* volatile_slot = reinterpret_cast<void* volatile*>(slot);
    void* previous = InterlockedCompareExchangePointer(
        volatile_slot, replacement, expected);

    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return previous == expected;
}

class active_hook_scope {
public:
    active_hook_scope() noexcept {
        if(g_worker_slot==~0u)g_worker_slot=g_worker_slots_used.fetch_add(1);
        if(g_worker_slot<g_active_workers.size()) {
            ++g_worker_depth;g_active_workers[g_worker_slot].active.store(true,std::memory_order_release);
        } else g_active_worker_overflow.fetch_add(1,std::memory_order_acq_rel);
    }

    ~active_hook_scope() {
        if(g_worker_slot<g_active_workers.size())
            g_active_workers[g_worker_slot].active.store(--g_worker_depth!=0,std::memory_order_release);
        else g_active_worker_overflow.fetch_sub(1,std::memory_order_release);
    }

    active_hook_scope(const active_hook_scope&) = delete;
    active_hook_scope& operator=(const active_hook_scope&) = delete;
};

bool ensure_control_point_accessor(void* effect) noexcept {
    auto* bytes=static_cast<std::uint8_t*>(effect);
    void* collection=nullptr;std::memcpy(&collection,bytes+g_particle_layout.collection,sizeof(collection));
    if(!collection||!g_control_point_vtable_offset)return false;
    void** table=nullptr;std::memcpy(&table,collection,sizeof(table));
    const auto key=reinterpret_cast<std::uintptr_t>(table);
    if(key==g_control_point_collection_vtable)return g_get_control_point!=nullptr;
    if(!is_readable(table+g_control_point_vtable_offset/sizeof(void*),sizeof(void*)))return false;
    void* getter=nullptr;std::memcpy(&getter,table+g_control_point_vtable_offset/sizeof(void*),sizeof(getter));
    g_control_point_collection_vtable=key;
    g_get_control_point=is_executable(getter)?reinterpret_cast<get_control_point_fn>(getter):nullptr;
    return g_get_control_point!=nullptr;
}
bool visual_generation_suppressed(std::uintptr_t effect,std::uintptr_t collection) noexcept {
    const auto& cache=particle_cache_for(effect);
    if(cache.effect.load(std::memory_order_acquire)==effect&&cache.classified_collection==collection&&
       cache.visual_until.load(std::memory_order_relaxed)>GetTickCount64())return true;
    const auto count=g_position_correction_count.load(std::memory_order_acquire);
    const auto key=effect^(collection<<1);
    for(unsigned i=0;i<count&&i<g_position_corrections.size();++i){
        const auto& slot=g_position_corrections[i];const auto before=slot.sequence.load(std::memory_order_acquire);
        if((before&1)||slot.key.load(std::memory_order_relaxed)!=key)continue;
        const auto expires=slot.expires.load(std::memory_order_relaxed);
        return before==slot.sequence.load(std::memory_order_acquire)&&expires>GetTickCount64();
    }
    return false;
}
bool resolve_creation_attachment(void* effect,const char* resource,armlet_vec3& reference,armlet_vec3& target) noexcept {
#if defined(_MSC_VER)
    const auto reader=g_attachment_reader.load(std::memory_order_acquire);
    if(!reader||!resource)return false;
    __try {
        if(!ensure_control_point_accessor(effect))return false;
        const auto* base=static_cast<const std::uint8_t*>(effect);std::uintptr_t collection=0,owner=0;
        std::memcpy(&collection,base+g_particle_layout.collection,sizeof(collection));
        std::memcpy(&owner,base+g_particle_layout.owner_pointer,sizeof(owner));
        const auto* point=g_get_control_point(reinterpret_cast<void*>(collection),0);
        if(!point)return false;
        reference=*point;std::memcpy(&target,base+g_particle_layout.sort_origin,sizeof(target));
        if(std::hypot(target.x-reference.x,target.y-reference.y)<128.f)return false;
        askermap::attachment_query q{};q.size=sizeof(q);q.version=askermap::abi;
        float model[3]{};
        if(!reader(owner,&q.actor,q.hero,sizeof(q.hero),model))return false;
        q.model={model[0],model[1],model[2]};q.attachment={reference.x,reference.y,reference.z};q.origin={target.x,target.y,target.z};
        std::snprintf(q.resource,sizeof(q.resource),"%s",resource);
        if(askermap::askermap_resolve_attachment(&q)!=0)return false;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
#else
    (void)effect;(void)resource;(void)reference;(void)target;return false;
#endif
}

bool ensure_collection_rendering_hook(void* effect) noexcept {
    auto* base = reinterpret_cast<std::uint8_t*>(effect);
    if ((base[g_particle_layout.visibility] & kForceNoDrawMask) != 0)
        return false;

    void* particles{};
    std::memcpy(&particles, base + g_particle_layout.collection, sizeof(particles));
    if (particles == nullptr)
        return false;

    void** vtable{};
    std::memcpy(&vtable, particles, sizeof(vtable));
    const auto render_slot = g_particle_layout.render_vtable_offset / sizeof(void*);
    const auto key = reinterpret_cast<std::uintptr_t>(vtable);
    if (key != g_render_collection_vtable) {
        // One verified CParticleCollection vtable. Unknown implementations
        // retain their original dispatch rather than sharing a stale setter.
        if (g_collection_render_slot.load(std::memory_order_acquire)!=nullptr) return false;
        if (vtable == nullptr || !is_readable(vtable + render_slot, sizeof(void*)))
            return false;
        void* function{};
        std::memcpy(&function, vtable + render_slot, sizeof(function));
        if (!is_executable(function)) return false;
        g_collection_rendering = reinterpret_cast<set_particle_rendering_fn>(function);
        g_render_collection_vtable = key;
        if (!exchange_slot(vtable+render_slot,function,
                reinterpret_cast<void*>(&hooked_collection_rendering))) {
            g_collection_rendering=nullptr;
            g_render_collection_vtable=0; return false;
        }
        g_collection_render_slot.store(vtable+render_slot,std::memory_order_release);
    }
    if (g_collection_rendering == nullptr) return false;
    return true;
}

bool live_world_collection(std::uintptr_t effect, std::uintptr_t collection) noexcept {
#if defined(_MSC_VER)
    __try {
#else
    if (!is_readable(reinterpret_cast<void*>(effect),g_particle_layout.visibility+1)) return false;
    {
#endif
        auto* bytes=reinterpret_cast<const std::uint8_t*>(effect);
        std::uintptr_t vtable=0, live_collection=0;
        std::uintptr_t owner=0;
        std::memcpy(&vtable,bytes,sizeof(vtable));
        std::memcpy(&live_collection,bytes+g_particle_layout.collection,sizeof(live_collection));
        std::memcpy(&owner,bytes+g_particle_layout.owner_pointer,sizeof(owner));
        const auto validate_owner=g_owner_validator.load(std::memory_order_acquire);
        return vtable==g_effect_vtable && live_collection==collection &&
            (bytes[g_particle_layout.visibility]&kForceNoDrawMask)==0 &&
            (owner==0 || owner==0xFFFFFFFFu || (validate_owner!=nullptr && validate_owner(owner)));
#if defined(_MSC_VER)
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    }
#endif
}

bool adjust_collection_rendering(void* particles, bool enabled, std::uintptr_t caller) noexcept {
    if (enabled || !g_enabled.load(std::memory_order_relaxed) || !g_gameplay_context.load(std::memory_order_relaxed) ||
        g_unloading.load(std::memory_order_acquire)) return enabled;
    bool fog_caller=false;
    for (std::size_t i=0;i<g_fog_setter_return_count;++i)
        if (g_fog_setter_returns[i]==caller) { fog_caller=true; break; }
    if (!fog_caller) return enabled;
    const auto collection=reinterpret_cast<std::uintptr_t>(particles);
    const auto begin=collection_owner_bucket(collection);
    for (std::size_t i=begin;i<begin+4;++i) {
        const auto& entry=g_collection_owners[i];
        if (entry.collection.load(std::memory_order_acquire)!=collection ||
            !entry.world.load(std::memory_order_relaxed)) continue;
        if (live_world_collection(entry.effect.load(std::memory_order_relaxed),collection))
            return true;
    }
    return enabled;
}

void hooked_collection_rendering(void* particles, bool enabled) noexcept {
    active_hook_scope active;
#if defined(_MSC_VER)
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#endif
    const auto original=g_collection_rendering;
    auto adjusted=adjust_collection_rendering(particles,enabled,caller);
    if(g_enabled.load(std::memory_order_relaxed)&&g_gameplay_context.load(std::memory_order_relaxed)&&
       !g_unloading.load(std::memory_order_acquire)) {
        const auto collection=reinterpret_cast<std::uintptr_t>(particles),begin=collection_owner_bucket(collection);
        for(std::size_t i=begin;i<begin+4;++i){const auto& entry=g_collection_owners[i];
            if(entry.collection.load(std::memory_order_acquire)!=collection||!entry.world.load(std::memory_order_relaxed))continue;
            const auto effect=entry.effect.load(std::memory_order_relaxed);
            if(visual_generation_suppressed(effect,collection)&&live_world_collection(effect,collection)){adjusted=false;break;}
        }
    }
    if (adjusted!=enabled) g_updated_effects.fetch_add(1,std::memory_order_relaxed);
    if (original!=nullptr) original(particles,adjusted);
}

void publish_hook_diagnostics() noexcept {
    g_hook_calls_published.store(g_hook_calls_local, std::memory_order_relaxed);
    g_named_effects_published.store(g_named_effects_local,
        std::memory_order_relaxed);
    g_trackable_effects_published.store(g_trackable_effects_local,
        std::memory_order_relaxed);
    g_fow_hidden_effects_published.store(g_fow_hidden_effects_local,
        std::memory_order_relaxed);
}

void record_hook_diagnostics(std::uint32_t flags) noexcept {
    ++g_hook_calls_local;
    if ((flags & kNamedParticleFlag) != 0)
        ++g_named_effects_local;
    if ((flags & (kTrackableParticleFlag |
            render_teleportation::particle_flag_teleport)) != 0)
        ++g_trackable_effects_local;

    if (++g_diagnostic_calls_since_publish >= kDiagnosticPublishInterval) {
        g_diagnostic_calls_since_publish = 0;
        publish_hook_diagnostics();
    }
}

void reset_hook_diagnostics() noexcept {
    g_hook_calls_local = 0;
    g_named_effects_local = 0;
    g_trackable_effects_local = 0;
    g_fow_hidden_effects_local = 0;
    g_diagnostic_calls_since_publish = 0;
    publish_hook_diagnostics();
}

/**
 * @brief Классифицирует имя ресурса и кэширует флаги по effect/name. Флаги направления TP
 * являются наблюдением, а не гарантией установленного caster.
 */
bool resolve_resource_name_field() noexcept {
    image_view image{};
    if (!get_image_view(GetModuleHandleW(L"resourcesystem.dll"), image)) return false;
    // ResourceSystem's name formatting path checks the interned name entry,
    // loads its string, then constructs a CUtlString. Extract its operand.
    constexpr int pattern[]{0x48,0x8B,0x41,-1,0x48,0x85,0xC0,0x74,-1,
        0x48,0x8B,0x00,0x48,0x8D,0x4D,-1,0x48,0x85,0xC0,
        0x48,0x8B,0xD6,0x48,0x0F,0x45,0xD0,0xE8};
    unsigned matches = 0;
    auto* section = IMAGE_FIRST_SECTION(image.nt);
    for (unsigned i=0;i<image.nt->FileHeader.NumberOfSections;++i,++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE)==0) continue;
        const auto start=section->VirtualAddress;
        if (start>=image.size) continue;
        const std::span<const std::uint8_t> code{image.base+start,
            std::min<std::size_t>(section->Misc.VirtualSize,image.size-start)};
        for (std::size_t at=0;at<code.size();++at)
            if (askerdway::particles::matches(code,at,pattern)) {
                g_resource_name_field=code[at+3]; ++matches;
            }
    }
    if (matches!=1 || g_resource_name_field<sizeof(void*) || g_resource_name_field>0x80) {
        g_resource_name_field=0; return false;
    }
    return true;
}

const char* particle_resource_name(void* effect) noexcept {
    const auto* base=static_cast<const std::uint8_t*>(effect);
    const auto effect_key=reinterpret_cast<std::uintptr_t>(effect);
    auto& cache=particle_cache_for(effect_key);
    void* collection=nullptr;
    std::memcpy(&collection,base+g_particle_layout.collection,sizeof(collection));
    const auto collection_key = reinterpret_cast<std::uintptr_t>(collection);
    const char* name=nullptr;
    std::memcpy(&name,base+g_particle_layout.debug_name,sizeof(name));
    if (name!=nullptr || g_resource_name_field==0) {
        cache.resource_binding=0;
        cache.collection=collection_key;
        return name;
    }
    if (collection==nullptr) return nullptr;
    // Update dispatches through this same live collection. Its lifetime is
    // protected by the engine while our callback is executing.
    std::uintptr_t vtable=0;
    std::memcpy(&vtable,collection,sizeof(vtable));
    if (vtable!=g_resource_collection_vtable) {
        if (!is_readable(reinterpret_cast<void*>(vtable+g_particle_layout.resource_vtable_offset),sizeof(void*))) return nullptr;
        const std::uint8_t* getter=nullptr;
        std::memcpy(&getter,reinterpret_cast<void*>(vtable+g_particle_layout.resource_vtable_offset),sizeof(getter));
        constexpr int pattern[]{0x48,0x8B,0x41,-1,0x48,0x89,0x02,0x48,0x8B,0xC2,0xC3};
        if (!is_readable(getter,sizeof(pattern)/sizeof(pattern[0])) ||
            !askerdway::particles::matches({getter,11},0,pattern)) return nullptr;
        g_collection_resource_field=getter[3];
        g_resource_collection_vtable=vtable;
    }
    std::uintptr_t binding=0,entry=0;
    std::memcpy(&binding,static_cast<std::uint8_t*>(collection)+g_collection_resource_field,sizeof(binding));
    if (binding!=0 && cache.effect.load(std::memory_order_acquire)==effect_key &&
        cache.collection==collection_key && cache.resource_binding==binding &&
        cache.debug_name.load(std::memory_order_relaxed)!=0)
        return reinterpret_cast<const char*>(cache.debug_name.load(std::memory_order_relaxed));
    if (!is_readable(reinterpret_cast<void*>(binding+g_resource_name_field),sizeof(entry))) return nullptr;
    std::memcpy(&entry,reinterpret_cast<void*>(binding+g_resource_name_field),sizeof(entry));
    if (!is_readable(reinterpret_cast<void*>(entry),sizeof(name))) return nullptr;
    std::memcpy(&name,reinterpret_cast<void*>(entry),sizeof(name));
    cache.resource_binding=binding;
    cache.collection=collection_key;
    return name;
}

const char* resolve_particle_name_lockfree(const void* effect) noexcept {
    if (!effect) return nullptr;
#if defined(_MSC_VER)
    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(effect);
        const char* name = nullptr;
        if (g_particle_layout.debug_name) {
            name = *reinterpret_cast<const char* const*>(bytes + g_particle_layout.debug_name);
        }
        if (!name || !*name) {
            // Engine jobs own the live collection during Update. Resolve its
            // validated resource getter without touching the shared mutable
            // classification cache or querying memory pages on every frame.
            const auto collection = *reinterpret_cast<const std::uintptr_t*>(bytes + g_particle_layout.collection);
            if(!collection || !g_resource_name_field)return nullptr;
            const auto vtable = *reinterpret_cast<const std::uintptr_t*>(collection);
            struct getter_layout { std::uintptr_t table=0;std::uint8_t field=0; };
            thread_local std::array<getter_layout,8> getters{};
            auto& layout=getters[(vtable>>4)%getters.size()];
            if(layout.table!=vtable) {
                const auto* getter=*reinterpret_cast<const std::uint8_t* const*>(vtable+g_particle_layout.resource_vtable_offset);
                constexpr int pattern[]{0x48,0x8B,0x41,-1,0x48,0x89,0x02,0x48,0x8B,0xC2,0xC3};
                if(!getter||!askerdway::particles::matches({getter,11},0,pattern))return nullptr;
                layout={vtable,getter[3]};
            }
            const auto binding=*reinterpret_cast<const std::uintptr_t*>(collection+layout.field);
            if(!binding)return nullptr;
            const auto entry=*reinterpret_cast<const std::uintptr_t*>(binding+g_resource_name_field);
            if(!entry)return nullptr;
            name=*reinterpret_cast<const char* const*>(entry);
        }
        return (name && *name) ? name : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#else
    const auto* bytes = static_cast<const std::uint8_t*>(effect);
    const char* name = g_particle_layout.debug_name
        ? *reinterpret_cast<const char* const*>(bytes + g_particle_layout.debug_name)
        : nullptr;
    if (!name || !*name) {
        name = particle_resource_name(const_cast<void*>(effect));
    }
    return (name && *name) ? name : nullptr;
#endif
}

bool is_alt_service_particle_lockfree(const char* name) noexcept {
    if (!name || !*name) return false;
#if defined(_MSC_VER)
    __try {
        return particle_ability_match::local_feedback(name) ||
               std::strstr(name, "camp_spawn") != nullptr ||
               std::strstr(name, "range_display") != nullptr ||
               std::strstr(name, "range_finder") != nullptr ||
               std::strstr(name, "bounding_box") != nullptr ||
               std::strstr(name, "ui_mouseactions") != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return particle_ability_match::local_feedback(name) ||
           std::strstr(name, "camp_spawn") != nullptr ||
           std::strstr(name, "range_display") != nullptr ||
           std::strstr(name, "range_finder") != nullptr ||
           std::strstr(name, "bounding_box") != nullptr ||
           std::strstr(name, "ui_mouseactions") != nullptr;
#endif
}

std::uint32_t classify_particle(void* effect) noexcept {
    if (effect == nullptr)
        return render_teleportation::particle_flag_none;
    // Update is already executing on this live effect object. Read its resource
    // pointer directly, then hit the cache before any VirtualQuery. The old
    // ordering queried a memory page for every particle on every rendered
    // frame even though the classification is stable for the effect lifetime.
    const char* debug_name = particle_resource_name(effect);
    const auto effect_key = reinterpret_cast<std::uintptr_t>(effect);
    const auto debug_name_key = reinterpret_cast<std::uintptr_t>(debug_name);
    auto& cache = particle_cache_for(effect_key);
    if (cache.effect.load(std::memory_order_acquire) == effect_key &&
        cache.debug_name.load(std::memory_order_relaxed) == debug_name_key &&
        cache.classified_collection == cache.collection)
        return cache.flags.load(std::memory_order_relaxed);
    cache.classified_collection = cache.collection;
    cache.updates_since_probe = 0;
    cache.last_capture_ms.store(0,std::memory_order_relaxed);
    cache.visual_until.store(0,std::memory_order_relaxed);
    cache.endcap_clock={};
    cache.copied_name[0] = '\0';
    if (debug_name == nullptr) {
        cache.flags.store(render_teleportation::particle_flag_none,
            std::memory_order_relaxed);
        cache.debug_name.store(0, std::memory_order_relaxed);
        cache.effect.store(effect_key, std::memory_order_release);
        return render_teleportation::particle_flag_none;
    }

    constexpr std::size_t maximum_name = 384;
    std::size_t readable_size = maximum_name;
    if (!is_readable(debug_name, readable_size)) {
        readable_size = 64;
        if (!is_readable(debug_name, readable_size)) {
            // A bad layout/pointer must not trigger VirtualQuery again for the
            // same effect on every frame. A pointer change invalidates this
            // negative cache entry automatically.
            cache.flags.store(render_teleportation::particle_flag_none,
                std::memory_order_relaxed);
            cache.debug_name.store(debug_name_key, std::memory_order_relaxed);
            cache.effect.store(effect_key, std::memory_order_release);
            return render_teleportation::particle_flag_none;
        }
    }

    char normalized[maximum_name + 1]{};
    std::memcpy(normalized, debug_name, readable_size);
    normalized[readable_size] = '\0';
    // A truncated resource prefix cannot prove identity or exclude UI assets.
    bool plausible_name = normalized[0] != '\0' &&
        std::memchr(normalized,'\0',readable_size)!=nullptr;
    for (std::size_t index = 0; index < readable_size && normalized[index] != '\0';
         ++index) {
        const unsigned char character = static_cast<unsigned char>(normalized[index]);
        if (character < 0x20 || character > 0x7E) {
            plausible_name = false;
            break;
        }
        if (character >= 'A' && character <= 'Z')
            normalized[index] = static_cast<char>(character - 'A' + 'a');
    }

    const auto resource=particle_ability_match::canonical_resource(normalized);
    const char* classification_name=resource.data();
    // These are local UI feedback, not effects emitted by the hero. Dota
    // attaches select_unit to the selected entity even while it is in fog.
    const bool local_feedback = plausible_name &&
        particle_ability_match::local_feedback(normalized);
    // Static map ambience is not emitted by a player. Forcing these thousands
    // of river/building effects through fog adds GPU work without useful data.
    const bool map_ambient = plausible_name &&
        (std::strstr(classification_name,"/world_environmental_fx/") != nullptr ||
         std::strstr(classification_name,"/radiant_fx/") != nullptr ||
         std::strstr(classification_name,"/dire_fx/") != nullptr ||
         std::strstr(classification_name,"/radiant_fx2/") != nullptr ||
         std::strstr(classification_name,"/dire_fx2/") != nullptr ||
         std::strstr(classification_name,"/base_static/") != nullptr ||
         std::strstr(classification_name,"/world_outpost/") != nullptr);
    std::uint32_t flags = plausible_name
        ? kNamedParticleFlag | ((local_feedback || map_ambient) ? 0 : kTrackableParticleFlag)
        : render_teleportation::particle_flag_none;
    if (plausible_name) {
        const auto length = resource.size();
        // Preserve the resource basename even for long cosmetic depot paths.
        const auto skip = length >= sizeof(cache.copied_name)
            ? length - (sizeof(cache.copied_name) - 1) : 0;
        std::snprintf(cache.copied_name, sizeof(cache.copied_name), "%.*s",
            static_cast<int>(length-skip), resource.data()+skip);
    }
    const bool twin_gate = plausible_name &&
        (std::strstr(classification_name, "twin_gate") != nullptr ||
            std::strstr(classification_name, "twins_gate") != nullptr);
    const bool keen_conveyance =
        plausible_name &&
        (std::strstr(classification_name, "keen_conveyance") != nullptr ||
            std::strstr(classification_name, "tinker_teleport") != nullptr);
    if (plausible_name && !local_feedback && !map_ambient &&
        (std::strstr(classification_name, "teleport") != nullptr || twin_gate ||
            keen_conveyance)) {
        // Teleports are still hero/item particles. Preserve the trackable bit
        // so the optional through-fog renderer does not discard the exact
        // effects that supply the route endpoints.
        flags |= render_teleportation::particle_flag_teleport;
        if (twin_gate)
            flags |= render_teleportation::particle_flag_twin_gate;
        if (keen_conveyance)
            flags |= render_teleportation::particle_flag_keen_conveyance;
        if (std::strstr(classification_name, "teleport_start") != nullptr ||
            std::strstr(classification_name, "teleport_begin") != nullptr ||
            std::strstr(classification_name, "channel_start") != nullptr ||
            std::strstr(classification_name, "departure") != nullptr ||
            std::strstr(classification_name, "_from") != nullptr)
            flags |= render_teleportation::particle_flag_teleport_start;
        if (std::strstr(classification_name, "teleport_end") != nullptr ||
            std::strstr(classification_name, "teleport_target") != nullptr ||
            std::strstr(classification_name, "teleport_destination") != nullptr ||
            std::strstr(classification_name, "arrival") != nullptr ||
            std::strstr(classification_name, "_to") != nullptr)
            flags |= render_teleportation::particle_flag_teleport_destination;
    }
    cache.flags.store(flags, std::memory_order_relaxed);
    cache.debug_name.store(debug_name_key, std::memory_order_relaxed);
    cache.effect.store(effect_key, std::memory_order_release);
    return flags;
}

/**
 * @brief Публикует ограниченный POD-снимок в ring buffer. sequence/epoch и committed watermark
 * отделяют зарезервированный слот от полностью записанного.
 */
void capture_particle_observation(void* effect,const armlet_vec3* creation_reference=nullptr) noexcept {
    if (effect == nullptr ||
        !g_trace_capture_enabled.load(std::memory_order_relaxed) ||
        !g_gameplay_context.load(std::memory_order_relaxed) ||
        g_unloading.load(std::memory_order_acquire))
        return;

    // Update is invoked for every live particle every rendered frame. Perform
    // the per-effect throttle before VirtualQuery/name classification: doing
    // those expensive checks before the old throttle hurt lobby FPS severely.
    const auto effect_key = reinterpret_cast<std::uintptr_t>(effect);
    auto& capture_cache = particle_cache_for(effect_key);
    const auto* base = reinterpret_cast<const std::uint8_t*>(effect);
    const char* debug_name = particle_resource_name(effect);
    // Update owns a live effect. Including the resource pointer in the key
    // prevents an address-reused effect from inheriting a stale zero class.
    const bool cached_effect = capture_cache.effect.load(
            std::memory_order_acquire) == effect_key &&
        capture_cache.debug_name.load(std::memory_order_relaxed) ==
            reinterpret_cast<std::uintptr_t>(debug_name) &&
        capture_cache.classified_collection == capture_cache.collection;
    const std::uint32_t cached_flags = cached_effect
        ? capture_cache.flags.load(std::memory_order_relaxed) : 0;
    std::uint32_t flags = cached_flags;
    if (!cached_effect)
        flags = classify_particle(effect);
    // The overwhelming majority of live effects are neither hero/item
    // traces nor teleports. Exit before the clock and two VirtualQuery calls.
    if ((flags & (kTrackableParticleFlag |
            render_teleportation::particle_flag_teleport)) == 0)
        return;
    // A long-lived ambient/buff effect is one event, not a fresh cast on each
    // Update. Re-publishing it kept the last-position timer permanently at 10s
    // and repeatedly traversed owners/modifiers. TP endpoints still stream.
    // Persistent gameplay effects publish state at a bounded cadence. The
    // consumer may annotate/move an existing trace, but never restart its TTL.
    const std::uint64_t observed_at = GetTickCount64();
    const std::uint64_t previous_capture = capture_cache.last_capture_ms.load(
        std::memory_order_relaxed);
    if (cached_effect && observed_at >= previous_capture &&
        observed_at - previous_capture <
            ((flags & render_teleportation::particle_flag_teleport) != 0 ? 100u : 500u))
        return;

    const std::uint64_t epoch = g_observation_epoch.load(std::memory_order_acquire);
#if defined(_MSC_VER)
    // Only the specific ordinary TP end resource has verified completion
    // semantics. Cancellation removes it without entering its endcap.
    if(capture_cache.collection&&std::strcmp(capture_cache.copied_name,"particles/items2_fx/teleport_end.vpcf")==0) {
        std::array<float,11> prefix{};bool readable=false;
        __try {std::memcpy(prefix.data(),reinterpret_cast<const void*>(capture_cache.collection+0x40),sizeof(prefix));readable=true;}
        __except(EXCEPTION_EXECUTE_HANDLER) {}
        if(readable&&capture_cache.endcap_clock.sample(prefix)) flags|=render_teleportation::particle_flag_teleport_endcap;
    }
#endif
    // Update owns this live object; the validated fields are inside it.

    std::uintptr_t owner_entity = 0;
    armlet_vec3 position{};
    std::memcpy(&owner_entity, base + g_particle_layout.owner_pointer, sizeof(owner_entity));
    std::memcpy(&position, base + g_particle_layout.sort_origin, sizeof(position));
    const armlet_vec3 source_position=position;
    bool control_point_position=false;
    ensure_control_point_accessor(effect);
    // SortOrigin is a culling anchor and can stay at the caster for a remote
    // effect. CP0 is the actual effect origin used by the collection.
    if (g_get_control_point && capture_cache.collection) {
#if defined(_MSC_VER)
        __try {
#endif
            std::uintptr_t live_vtable=0;
            std::memcpy(&live_vtable,reinterpret_cast<void*>(capture_cache.collection),sizeof(live_vtable));
            const auto* point=live_vtable==g_control_point_collection_vtable
                ? g_get_control_point(reinterpret_cast<void*>(capture_cache.collection),0) : nullptr;
            if (point && std::isfinite(point->x) && std::isfinite(point->y) &&
                std::isfinite(point->z) && (std::abs(point->x)>1 || std::abs(point->y)>1))
            {
                position=*point;
                control_point_position=true;
            }
#if defined(_MSC_VER)
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
#endif
    }
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z))
        return;

    if(creation_reference)position=*creation_reference;
    const bool visual_correction=creation_reference?true:visual_generation_suppressed(effect_key,capture_cache.collection);

    capture_cache.updates_since_probe = 0;
    capture_cache.last_capture_ms.store(observed_at, std::memory_order_relaxed);

    std::uint32_t x_bits = 0;
    std::uint32_t y_bits = 0;
    std::uint32_t z_bits = 0;
    std::memcpy(&x_bits, &position.x, sizeof(x_bits));
    std::memcpy(&y_bits, &position.y, sizeof(y_bits));
    std::memcpy(&z_bits, &position.z, sizeof(z_bits));

    const std::uint64_t sequence = g_observation_sequence.fetch_add(
        1, std::memory_order_acq_rel) + 1;
    auto& slot = g_observations[(sequence - 1) % kObservationCapacity];
    // Mark the wrapped slot as in-progress before replacing its payload. The
    // consumer otherwise could accept the old committed sequence together
    // with partially rewritten coordinates/name.
    slot.sequence.store(0, std::memory_order_release);
    // The visibility factory stores a pointer-sized user token. Current Dota
    // uses a zero-extended CEntityHandle here, not an entity address.
    const bool encoded_handle = owner_entity != 0 && owner_entity <= UINT32_MAX;
    slot.owner_handle.store(encoded_handle ? static_cast<std::uint32_t>(owner_entity)
        : 0xFFFFFFFFu, std::memory_order_relaxed);
    slot.owner_entity.store(encoded_handle ? 0 : owner_entity, std::memory_order_relaxed);
    slot.flags.store((flags & ~(kTrackableParticleFlag | kNamedParticleFlag)) |
        (previous_capture == 0 ? render_teleportation::particle_flag_new_effect : 0) |
        (control_point_position ? render_teleportation::particle_flag_control_point_position : 0) |
        (visual_correction ? render_teleportation::particle_flag_visual_correction : 0),
        std::memory_order_relaxed);
    slot.effect.store(effect_key, std::memory_order_relaxed);
    slot.collection.store(capture_cache.collection, std::memory_order_relaxed);
    slot.visibility.store(base[g_particle_layout.visibility], std::memory_order_relaxed);
    std::uint32_t sx=0,sy=0,sz=0;
    std::memcpy(&sx,&source_position.x,4);std::memcpy(&sy,&source_position.y,4);std::memcpy(&sz,&source_position.z,4);
    slot.source_x_bits.store(sx,std::memory_order_relaxed);slot.source_y_bits.store(sy,std::memory_order_relaxed);slot.source_z_bits.store(sz,std::memory_order_relaxed);
    slot.x_bits.store(x_bits, std::memory_order_relaxed);
    slot.y_bits.store(y_bits, std::memory_order_relaxed);
    slot.z_bits.store(z_bits, std::memory_order_relaxed);
    slot.observed_at_ms.store(observed_at, std::memory_order_relaxed);
    for (std::size_t index = 0; index < sizeof(capture_cache.copied_name); ++index)
        slot.debug_name[index].store(
            static_cast<std::uint8_t>(capture_cache.copied_name[index]),
            std::memory_order_relaxed);
    slot.epoch.store(epoch, std::memory_order_relaxed);
    slot.sequence.store(sequence, std::memory_order_release);
    // Reservation happens before the payload write. Publish a distinct high
    // water mark only after the slot is complete so the consumer cannot skip
    // a just-reserved observation and advance its cursor past it forever.
    g_observation_committed_sequence.store(sequence,
        std::memory_order_release);
}

/**
 * @brief Callback движка: учитывает активный вызов, сохраняет вызов оригинала и выполняет
 * разрешённый захват/видимость. Не обращаться здесь к runtime UI или его блокировкам.
 */
void hooked_particle_update(void* effect, float time) noexcept {
    active_hook_scope active;
    auto original = g_original_update.load(std::memory_order_acquire);
    if (original == nullptr) {
        return;
    }

    const bool visibility_override =
        g_enabled.load(std::memory_order_relaxed)&&g_gameplay_context.load(std::memory_order_relaxed);
    const bool inspect = effect != nullptr &&
        !g_unloading.load(std::memory_order_acquire) &&
        (visibility_override ||
            (g_gameplay_context.load(std::memory_order_relaxed)&&g_trace_capture_enabled.load(std::memory_order_relaxed)));
    // Disabled features must not touch the classification cache, particle
    // flags or observation diagnostics for every engine particle every frame.
    if (!inspect) {
        original(effect,time);
        return;
    }

    const char* resolved_name = resolve_particle_name_lockfree(effect);

    const auto name_key = reinterpret_cast<std::uintptr_t>(resolved_name);

    // Per-job scheduling avoids contending on the shared state for every
    // particle/frame. Engine Update always runs; identity/policy changes are
    // inspected immediately and stable particles retain their stream cadence.
    struct inspection_slot {
        std::uintptr_t effect=0,collection=0,name=0,owner=0;
        std::uint64_t at=0,epoch=0;
        std::uint32_t flags=0,probes=0;
        unsigned mode=0;std::uint8_t policy=0;bool service=false;
    };
    thread_local std::array<inspection_slot,4096> inspections{};
    const auto key=reinterpret_cast<std::uintptr_t>(effect);
    auto hash=key>>4;hash^=hash>>30;hash*=0xbf58476d1ce4e5b9ULL;hash^=hash>>27;
    const auto bucket=(hash%(inspections.size()/4))*4;
    auto* selected=&inspections[bucket];
    for(std::size_t i=bucket;i<bucket+4;++i) {
        auto& candidate=inspections[i];
        if(candidate.effect==key||candidate.effect==0){selected=&candidate;break;}
        if(candidate.at<selected->at)selected=&candidate;
    }
    auto& inspection=*selected;
    const auto* bytes=static_cast<const std::uint8_t*>(effect);
    std::uintptr_t collection_key=0,owner_key=0;
    std::memcpy(&collection_key,bytes+g_particle_layout.collection,sizeof(collection_key));
    std::memcpy(&owner_key,bytes+g_particle_layout.owner_pointer,sizeof(owner_key));
    const auto epoch=g_observation_epoch.load(std::memory_order_relaxed);
    const unsigned mode=(visibility_override?1u:0u)|(g_trace_capture_enabled.load()?2u:0u);
    // Engine fog culling toggles the hidden bit during each frame. It is an
    // outcome, not a change of emitter identity/policy: comparing it defeated
    // the cadence limit and serialized parallel jobs on every such toggle.
    const auto policy=static_cast<std::uint8_t>(bytes[g_particle_layout.visibility]&~kFogHiddenMask);
    const bool stable=inspection.effect==key&&inspection.collection==collection_key&&
        inspection.name==name_key&&inspection.owner==owner_key&&inspection.epoch==epoch&&
        inspection.mode==mode&&inspection.policy==policy;
    const bool same_name=inspection.effect==key&&inspection.collection==collection_key&&inspection.name==name_key;
    if((same_name&&inspection.service)||(!same_name&&is_alt_service_particle_lockfree(resolved_name))) {
        inspection={key,collection_key,name_key,owner_key,0,epoch,0,0,mode,policy,true};
        original(effect,time);return;
    }
    if(stable) {
        const bool teleport=(inspection.flags&render_teleportation::particle_flag_teleport)!=0;
        if(++inspection.probes<(teleport?4u:8u)) {original(effect,time);return;}
        inspection.probes=0;
        if(GetTickCount64()-inspection.at<(teleport?100u:500u)) {original(effect,time);return;}
    }
    std::uint32_t flags=0;
    bool checks_fog=false;
    checks_fog=(policy&g_particle_layout.fog_check_mask)!=0;
    armlet_vec3 creation_reference{},creation_target{};
    original(effect,time);
    const bool creation_corrected=!same_name&&visibility_override&&
        resolve_creation_attachment(effect,resolved_name,creation_reference,creation_target);
    particle_state_guard state;
    if(g_unloading.load(std::memory_order_acquire))return;
    flags=classify_particle(effect);
    if(creation_corrected)particle_cache_for(key).visual_until.store(GetTickCount64()+1000,std::memory_order_relaxed);
#if !defined(ASKERDWAY_DIAGNOSTICS) || ASKERDWAY_DIAGNOSTICS
    record_hook_diagnostics(flags);
    if(checks_fog)++g_fow_hidden_effects_local;
#endif
    particle_cache_for(key).original_fog_policy=checks_fog;
    inspection={key,collection_key,name_key,owner_key,GetTickCount64(),epoch,flags,0,mode,
        static_cast<std::uint8_t>(bytes[g_particle_layout.visibility]&~kFogHiddenMask)};
    if (visibility_override && effect != nullptr &&
        !g_unloading.load(std::memory_order_acquire)) {
        const auto* base = reinterpret_cast<const std::uint8_t*>(effect);
        std::uintptr_t collection=0;
        std::memcpy(&collection,base+g_particle_layout.collection,sizeof(collection));
        const bool world=(flags&kTrackableParticleFlag)!=0 ||
            ((flags&kNamedParticleFlag)==0 && checks_fog);
        remember_collection(collection,reinterpret_cast<std::uintptr_t>(effect),world);
        if (world && ensure_collection_rendering_hook(effect) &&
            live_world_collection(key,collection)) {
            // The first Update may have disabled the collection before its
            // setter was intercepted. Reconcile once here; subsequent engine
            // visibility calls go through the validated setter hook. Preserve
            // engine flags so turning the feature off restores normal policy.
            if(visual_generation_suppressed(key,collection))g_collection_rendering(reinterpret_cast<void*>(collection),false);
            else if(!stable&&(base[g_particle_layout.visibility]&kFogHiddenMask)!=0){
                g_collection_rendering(reinterpret_cast<void*>(collection),true);
                g_updated_effects.fetch_add(1,std::memory_order_relaxed);
            }
        }
    }
    capture_particle_observation(effect,creation_corrected?&creation_reference:nullptr);
}

/**
 * @brief Управляет перехватом вместе с активными callback. Неуспешное снятие не даёт права
 * выгружать исполняемый код DLL.
 */
bool install_hook() noexcept {
    HMODULE client = GetModuleHandleW(L"client.dll");
    image_view image{};
    if (client == nullptr || !get_image_view(client, image)) {
        return false;
    }

    void* update = find_unique_update(image);
    if (update == nullptr) {
        debug_log("ERROR", "CNewParticleEffect::Update signature is missing or ambiguous.");
        return false;
    }

    if (!validate_particle_layout(image,
                                  reinterpret_cast<const std::uint8_t*>(update))) {
        debug_log("ERROR", "CNewParticleEffect field/layout validation failed.");
        return false;
    }
    if (!resolve_resource_name_field())
        debug_log("WARN", "Particle resource-name signature unavailable; optional labels only.");

    void** slot = find_unique_update_slot(image, update);
    if (slot == nullptr) {
        debug_log("ERROR", "CNewParticleEffect vtable validation failed.");
        return false;
    }

    reset_hook_diagnostics();
    g_original_update.store(reinterpret_cast<particle_update_fn>(update),
                            std::memory_order_release);
    if (!exchange_slot(slot, update,
                       reinterpret_cast<void*>(&hooked_particle_update))) {
        g_original_update.store(nullptr, std::memory_order_release);
        debug_log("ERROR", "CNewParticleEffect::Update vtable slot is already modified.");
        return false;
    }

    g_update_slot.store(slot, std::memory_order_release);
    g_effect_vtable=reinterpret_cast<std::uintptr_t>(slot-kUpdateVtableIndex);
    debug_log("INFO", "CNewParticleEffect::Update hook installed.");
    return true;
}

/**
 * @brief Управляет перехватом вместе с активными callback. Неуспешное снятие не даёт права
 * выгружать исполняемый код DLL.
 */
bool remove_hook() noexcept {
    g_unloading.store(true, std::memory_order_release);
    g_enabled.store(false, std::memory_order_release);
    g_trace_capture_enabled.store(false, std::memory_order_release);
    g_observation_epoch.fetch_add(1, std::memory_order_acq_rel);

    {
    particle_state_guard state;
    void** slot = g_update_slot.load(std::memory_order_acquire);
    auto original = g_original_update.load(std::memory_order_acquire);
    if (slot != nullptr && original != nullptr) {
        if (!exchange_slot(slot, reinterpret_cast<void*>(&hooked_particle_update),
                           reinterpret_cast<void*>(original))) {
            debug_log("ERROR", "Could not restore the CNewParticleEffect vtable slot.");
            return false;
        }
        g_update_slot.store(nullptr, std::memory_order_release);
    }
    void** render_slot=g_collection_render_slot.load(std::memory_order_acquire);
    if (render_slot!=nullptr && g_collection_rendering!=nullptr) {
        if (!exchange_slot(render_slot,reinterpret_cast<void*>(&hooked_collection_rendering),
                reinterpret_cast<void*>(g_collection_rendering))) {
            debug_log("ERROR","Could not restore the particle collection rendering slot.");
            return false;
        }
        g_collection_render_slot.store(nullptr,std::memory_order_release);
    }
    }

    // Let a dispatch that fetched the old slot before the exchange enter, then
    // wait for all parallel engine jobs and nested setters to finish.
    Sleep(32);
    for (unsigned attempt = 0;
         attempt < 500 && any_hook_active();
         ++attempt) {
        Sleep(2);
    }

    if (any_hook_active()) {
        debug_log("ERROR", "An Update call is still active; keeping the DLL loaded.");
        return false;
    }

    publish_hook_diagnostics();
    g_original_update.store(nullptr, std::memory_order_release);
    g_updated_effects.store(0, std::memory_order_relaxed);
    for (auto& cache : g_particle_classification_cache) {
        cache.effect.store(0, std::memory_order_relaxed);
        cache.debug_name.store(0, std::memory_order_relaxed);
        cache.flags.store(0, std::memory_order_relaxed);
        cache.resource_binding = 0;
        cache.collection = 0;
        cache.classified_collection = 0;
        cache.original_fog_policy = false;
        cache.copied_name[0] = '\0';
        cache.updates_since_probe = 0;
        cache.last_capture_ms.store(0, std::memory_order_relaxed);
    }
    debug_log("INFO", "CNewParticleEffect::Update hook removed.");
    g_resource_collection_vtable = 0;
    g_collection_resource_field = 0;
    g_render_collection_vtable = 0;
    g_collection_rendering = nullptr;
    g_get_control_point = nullptr;
    g_control_point_collection_vtable=0;
    g_effect_vtable = 0;
    g_owner_validator.store(nullptr,std::memory_order_release);
    for (auto& owner:g_collection_owners) {
        owner.collection.store(0,std::memory_order_relaxed);
        owner.effect.store(0,std::memory_order_relaxed);
        owner.world.store(false,std::memory_order_relaxed);
    }
    return true;
}

#if !defined(RENDER_TELEPORTATION_EMBEDDED)
/**
 * @brief Управляет жизненным циклом соответствующего runtime: ожидание зависимостей, polling и
 * остановка. В combined target игровой worker реализован в runtime_worker.cpp.
 */
DWORD WINAPI worker_thread(void*) noexcept {
    bool installed = false;
    bool failure_reported = false;

    while (!g_unloading.load(std::memory_order_acquire)) {
        if ((GetAsyncKeyState(VK_END) & 1) != 0) {
            break;
        }

        if (!installed && GetModuleHandleW(L"client.dll") != nullptr) {
            installed = install_hook();
            if (!installed && !failure_reported) {
                debug_log("ERROR", "Hook installation failed; retrying every two seconds.");
                failure_reported = true;
            }
            if (!installed) {
                for (unsigned tick = 0; tick < 40; ++tick) {
                    if ((GetAsyncKeyState(VK_END) & 1) != 0) {
                        g_unloading.store(true, std::memory_order_release);
                        break;
                    }
                    Sleep(50);
                }
                continue;
            }
        }

        Sleep(50);
    }

    if (remove_hook()) {
        FreeLibraryAndExitThread(g_module, 0);
    }
    return 0;
}
#endif

}  // namespace

namespace render_teleportation {
void set_attachment_reader(particle_attachment_reader reader) noexcept {g_attachment_reader.store(reader,std::memory_order_release);}
void publish_position_corrections(const particle_position_correction* values,std::size_t count) noexcept {
    count=values?std::min(count,g_position_corrections.size()):0;
    for(std::size_t i=0;i<count;++i){
        auto& slot=g_position_corrections[i];slot.sequence.fetch_add(1,std::memory_order_acq_rel);
        slot.key.store(values[i].key,std::memory_order_relaxed);slot.expires.store(values[i].expires_at,std::memory_order_relaxed);
        for(int axis=0;axis<3;++axis){slot.position[axis].store(values[i].position[axis],std::memory_order_relaxed);slot.reference[axis].store(values[i].reference[axis],std::memory_order_relaxed);}
        slot.sequence.fetch_add(1,std::memory_order_release);
    }
    g_position_correction_count.store(static_cast<unsigned>(count),std::memory_order_release);
}

bool install() noexcept {
    if (g_unloading.load(std::memory_order_acquire)) {
        return false;
    }
    if (g_update_slot.load(std::memory_order_acquire) != nullptr) {
        return true;
    }
    return install_hook();
}

bool shutdown() noexcept {
    return remove_hook();
}

bool set_enabled(bool enabled) noexcept {
    if (g_unloading.load(std::memory_order_acquire)) {
        return false;
    }
    g_enabled.store(enabled, std::memory_order_release);
    return true;
}

void set_owner_validator(particle_owner_validator validator) noexcept {
    g_owner_validator.store(validator,std::memory_order_release);
}

bool set_trace_capture_enabled(bool enabled) noexcept {
    if (g_unloading.load(std::memory_order_acquire))
        return false;
    const bool previous=g_trace_capture_enabled.exchange(enabled,std::memory_order_acq_rel);
    if (previous&&!enabled)
        clear_observations();
    return true;
}

void clear_observations() noexcept {
    g_position_correction_count.store(0,std::memory_order_release);
    g_observation_epoch.fetch_add(1, std::memory_order_acq_rel);
}

bool is_enabled() noexcept {
    return g_enabled.load(std::memory_order_acquire);
}
void set_gameplay_context(bool active) noexcept {
    const bool previous=g_gameplay_context.exchange(active,std::memory_order_acq_rel);
    if(previous&&!active)clear_observations();
}

bool is_hook_installed() noexcept {
    return g_update_slot.load(std::memory_order_acquire) != nullptr;
}

std::uint64_t updated_effect_count() noexcept {
    return g_updated_effects.load(std::memory_order_relaxed);
}

std::uint64_t hook_call_count() noexcept {
    return g_hook_calls_published.load(std::memory_order_relaxed);
}

std::uint64_t named_effect_count() noexcept {
    return g_named_effects_published.load(std::memory_order_relaxed);
}

std::uint64_t trackable_effect_count() noexcept {
    return g_trackable_effects_published.load(std::memory_order_relaxed);
}

std::uint64_t fow_hidden_effect_count() noexcept {
    return g_fow_hidden_effects_published.load(std::memory_order_relaxed);
}

/**
 * @brief Копирует только согласованные записи кольца после cursor. Отставший читатель может
 * потерять перезаписанные записи; обновлённый cursor нужно сохранить.
 */
std::size_t read_observations(std::uint64_t& cursor,
    particle_observation* output, std::size_t capacity) noexcept {
    if (output == nullptr || capacity == 0)
        return 0;

    const std::uint64_t latest = g_observation_committed_sequence.load(
        std::memory_order_acquire);
    if (latest <= cursor)
        return 0;

    std::uint64_t first = cursor + 1;
    if (latest >= kObservationCapacity &&
        first <= latest - kObservationCapacity)
        first = latest - kObservationCapacity + 1;
    const std::uint64_t last = std::min<std::uint64_t>(latest,first+capacity-1);

    const std::uint64_t epoch = g_observation_epoch.load(std::memory_order_acquire);
    std::size_t written = 0;
    for (std::uint64_t sequence = first; sequence <= last; ++sequence) {
        auto& slot = g_observations[(sequence - 1) % kObservationCapacity];
        if (slot.sequence.load(std::memory_order_acquire) != sequence)
            continue;
        if (slot.epoch.load(std::memory_order_relaxed) != epoch)
            continue;

        particle_observation observation{};
        observation.sequence = sequence;
        observation.owner_handle = slot.owner_handle.load(std::memory_order_relaxed);
        observation.owner_entity = slot.owner_entity.load(std::memory_order_relaxed);
        observation.flags = slot.flags.load(std::memory_order_relaxed);
        observation.effect = slot.effect.load(std::memory_order_relaxed);
        observation.collection = slot.collection.load(std::memory_order_relaxed);
        observation.visibility = slot.visibility.load(std::memory_order_relaxed);
        const std::uint32_t x_bits = slot.x_bits.load(std::memory_order_relaxed);
        const std::uint32_t y_bits = slot.y_bits.load(std::memory_order_relaxed);
        const std::uint32_t z_bits = slot.z_bits.load(std::memory_order_relaxed);
        observation.observed_at_ms = slot.observed_at_ms.load(
            std::memory_order_relaxed);
        for (std::size_t name_index = 0;
             name_index + 1 < sizeof(observation.debug_name); ++name_index) {
            observation.debug_name[name_index] = static_cast<char>(
                slot.debug_name[name_index].load(std::memory_order_relaxed));
        }
        observation.debug_name[sizeof(observation.debug_name) - 1] = '\0';
        const auto sx=slot.source_x_bits.load(),sy=slot.source_y_bits.load(),sz=slot.source_z_bits.load();
        std::memcpy(&observation.source_x,&sx,4);std::memcpy(&observation.source_y,&sy,4);std::memcpy(&observation.source_z,&sz,4);
        std::memcpy(&observation.x, &x_bits, sizeof(observation.x));
        std::memcpy(&observation.y, &y_bits, sizeof(observation.y));
        std::memcpy(&observation.z, &z_bits, sizeof(observation.z));
        if (slot.sequence.load(std::memory_order_acquire) != sequence)
            continue;
        output[written++] = observation;
    }
    cursor = last;
    return written;
}

std::size_t read_recent_observations(particle_observation* output,
    std::size_t capacity) noexcept {
    const auto latest = g_observation_committed_sequence.load(std::memory_order_acquire);
    auto cursor = latest > capacity ? latest - capacity : 0;
    return read_observations(cursor, output, capacity);
}

const char* build_id() noexcept {
    return "1.0.0";
}

}  // namespace render_teleportation

#if !defined(RENDER_TELEPORTATION_EMBEDDED)

extern "C" __declspec(dllexport) BOOL
RenderTeleportation_SetEnabled(BOOL enabled) noexcept {
    return render_teleportation::set_enabled(enabled != FALSE) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL
RenderTeleportation_IsHookInstalled() noexcept {
    return render_teleportation::is_hook_installed() ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) unsigned long long
RenderTeleportation_GetUpdatedEffectCount() noexcept {
    return render_teleportation::updated_effect_count();
}

extern "C" __declspec(dllexport) const char*
RenderTeleportation_GetBuildId() noexcept {
    return render_teleportation::build_id();
}

/**
 * @brief При attach запускает worker и быстро возвращает управление загрузчику; ожидание игры
 * и создание интерфейса выполняются уже в worker.
 */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, &worker_thread, nullptr, 0, nullptr);
        if (thread == nullptr) {
            return FALSE;
        }
        CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_enabled.store(false, std::memory_order_release);
        g_unloading.store(true, std::memory_order_release);
    }
    return TRUE;
}
#endif
