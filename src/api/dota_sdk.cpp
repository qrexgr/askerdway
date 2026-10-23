/** @file
 * @brief Windows backend SDK: guarded memory access, PE identity and symbol resolution.
 * Separate translation unit; does not depend on private unity runtime state.
 */
#include "askerdway/dota_sdk.hpp"
#include <Windows.h>
#include <algorithm>
#include <cctype>

namespace askerdway::dota {
namespace {
bool read_process(std::uintptr_t address, void* output, std::size_t size) noexcept {
    SIZE_T count = 0;
    return address && output && size && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address), output, size, &count) && count == size;
}
bool write_process(std::uintptr_t address, const void* input, std::size_t size) noexcept {
    SIZE_T count = 0;
    return address && input && size && WriteProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(address), input, size, &count) && count == size;
}
std::string module_key(std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos ||
        name.find_first_of("/\\") != std::string_view::npos)
        throw error("Expected a module basename");
    std::string result(name);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
struct loaded_module {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    std::shared_ptr<void> owner;
};
loaded_module checked_module(std::string_view name) {
    static_assert(sizeof(void*) == 8, "Dota SDK requires the Windows x64 ABI");
    const auto key = module_key(name);
    const auto* descriptor = dota_api::find_module(key);
    if (!descriptor || !descriptor->image_size || !descriptor->pe_timestamp)
        throw error("No verifiable PE identity in catalog: " + key);
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(0, key.c_str(), &module))
        throw error("Module is not loaded: " + key);
    std::shared_ptr<void> owner(module, [](void* value) { FreeLibrary(static_cast<HMODULE>(value)); });
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read_process(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || static_cast<std::size_t>(dos.e_lfanew) > descriptor->image_size ||
        sizeof(nt) > descriptor->image_size - static_cast<std::size_t>(dos.e_lfanew) ||
        !read_process(base + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.SizeOfImage != descriptor->image_size ||
        nt.FileHeader.TimeDateStamp != descriptor->pe_timestamp)
        throw error("Loaded module differs from SDK catalog: " + key);
    return {base, descriptor->image_size, std::move(owner)};
}
bool executable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &page, sizeof(page)) ||
        page.State != MEM_COMMIT || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto protection = page.Protect & 0xFFu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}
} // namespace

memory_access process_memory(bool writable) {
    return {read_process, writable ? write_process : nullptr, {}};
}
module_view::module_view(std::string_view name, bool writable) : name_(module_key(name)) {
    auto loaded = checked_module(name_);
    access_ = process_memory(writable);
    access_.owner = std::move(loaded.owner);
}
resolved_function resolve_function(std::string_view module, std::string_view name,
    symbol_source source, std::string_view prototype, std::uint16_t ordinal) {
    if (name.find('\0') != std::string_view::npos || (name.empty() && !ordinal))
        throw error("Invalid function name or ordinal");
    const auto key = module_key(module);
    auto loaded = checked_module(key);
    std::uintptr_t address = 0;
    if (source == symbol_source::signature) {
        const auto* descriptor = dota_api::find_signature(key, name);
        if (!descriptor || descriptor->result != dota_api::pattern_result_kind::match_address ||
            !descriptor->verified_for_dump ||
            (!prototype.empty() && descriptor->prototype != prototype))
            throw error("Missing, unverified or incompatible function signature: " + std::string(name));
        address = dota_api::resolve_signature(loaded.base, loaded.size, *descriptor);
        if (address < loaded.base || address - loaded.base >= loaded.size)
            throw error("Signature did not resolve inside its module");
    } else {
        const dota_api::export_descriptor* descriptor = nullptr;
        if (!name.empty()) descriptor = dota_api::find_export(key, name);
        else {
            for (const auto& entry : dota_api::exports()) {
                if (entry.module == key && entry.ordinal == ordinal) { descriptor = &entry; break; }
            }
        }
        if (!descriptor) throw error("Export is absent from catalog: " + std::string(name));
        address = dota_api::resolve_export(loaded.base, *descriptor);
    }
    if (!address || !executable(address)) throw error("Symbol is not executable");
    // Forwarded exports can live in a different module. Retain the actual target,
    // not just the forwarding DLL, for the entire lifetime of the bound function.
    HMODULE target = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCSTR>(address), &target)) throw error("Cannot retain function module");
    auto owners = std::make_shared<std::pair<std::shared_ptr<void>, std::shared_ptr<void>>>(
        std::move(loaded.owner), std::shared_ptr<void>(target,
            [](void* value) { FreeLibrary(static_cast<HMODULE>(value)); }));
    return {address, std::move(owners)};
}
} // namespace askerdway::dota
