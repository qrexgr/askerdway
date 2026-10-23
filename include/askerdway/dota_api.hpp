/**
 * @file
 * @brief C++ API каталога Dota / Source 2.
 *
 * Даёт доступ к дескрипторам schema, интерфейсов, экспортов и сигнатур. Это отдельный source-
 * level C++ контракт со span/string_view, а не POD ABI AskerdwayApi. Каталог не доказывает время
 * жизни объектов или прототип неизвестной функции.
 */

#pragma once

// Generated Dota / Source 2 discovery catalog.
//
// This is a source-level C++ framework, not a set of binary overlays for Valve
// classes. Schema classes expose field names, live offsets and declared types;
// callers must still read memory defensively because object lifetime and class
// layout can change after every game update.
//
// `dota_api.cpp` is generated from a live dota2-dumper snapshot plus the PE
// export tables in game/bin/win64 and game/dota/bin/win64. Unknown function
// prototypes are deliberately left unknown instead of being guessed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#if defined(_WIN32)
#if defined(ASKERDWAY_BUILDING_DLL)
#define ASKERDWAY_DOTA_API __declspec(dllexport)
#else
#define ASKERDWAY_DOTA_API __declspec(dllimport)
#endif
#else
#define ASKERDWAY_DOTA_API
#endif

namespace askerdway::dota_api {

// Half-open range into one of the catalog arrays: [begin, begin + count).
struct descriptor_range {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
};

// Broad purpose inferred from a schema field's declared type and Source 2 name.
// The original name/type/offset remain authoritative; this category is only a
// navigation aid for tools and inspectors.
enum class field_category : std::uint8_t {
    unknown,
    boolean_flag,
    entity_handle,
    pointer,
    string,
    vector_or_container,
    position_or_direction,
    time_or_duration,
    health,
    damage,
    count_or_index,
    identifier,
    state_or_flags,
};

// What an offset descriptor points to. `member_offset` is relative to an
// object; all other address-like values are module-relative RVAs.
enum class symbol_kind : std::uint8_t {
    unknown,
    global_address,
    function,
    member_offset,
};

// How resolve_signature() converts the unique byte-pattern match into a value.
enum class pattern_result_kind : std::uint8_t {
    match_address,
    rip_relative_32,
    immediate_u32,
    immediate_u8,
};

// One loaded PE module. Ranges point into the global descriptor arrays below.
struct module_descriptor {
    std::string_view name;
    std::string_view relative_path;
    std::string_view responsibility;
    std::uint32_t image_size = 0;
    std::uint32_t pe_timestamp = 0;
    descriptor_range interfaces{};
    descriptor_range offsets{};
    descriptor_range schema_classes{};
    descriptor_range schema_enums{};
    descriptor_range exports{};
};

// Source 2 CreateInterface registration. `instance_rva` points to the live
// interface instance resolved by the dumper, not to its factory function.
struct interface_descriptor {
    std::string_view module;
    std::string_view name;
    std::uintptr_t instance_rva = 0;
};

// Named global/function/member offset found by the Dota-specific dumper.
struct offset_descriptor {
    std::string_view module;
    std::string_view name;
    std::uintptr_t value = 0;
    symbol_kind kind = symbol_kind::unknown;
};

// A build-dependent internal byte signature. `pattern` uses IDA-style bytes
// (`48 8B ? ?`). `prototype` is empty when the real ABI is not proven.
struct signature_descriptor {
    std::string_view module;
    std::string_view name;
    std::string_view pattern;
    std::string_view prototype;
    pattern_result_kind result = pattern_result_kind::match_address;
    std::uint16_t operand_offset = 0;
    std::uint16_t instruction_size = 0;
    std::intptr_t addend = 0;
    bool verified_for_dump = false;
};

// Schema class/struct declaration. Field and metadata ranges refer to the
// flattened catalogs returned by schema_fields() and schema_metadata().
struct schema_class_descriptor {
    std::string_view module;
    std::string_view name;
    std::string_view parent;
    descriptor_range fields{};
    descriptor_range metadata{};
};

// One runtime field. `offset` is relative to the class object. `type_name` is
// the exact type string reported by SchemaSystem for this dump.
struct schema_field_descriptor {
    std::string_view module;
    std::string_view owner;
    std::string_view name;
    std::string_view type_name;
    std::ptrdiff_t offset = 0;
    field_category category = field_category::unknown;
};

// Schema metadata such as network-variable names and change callbacks.
struct schema_metadata_descriptor {
    std::string_view module;
    std::string_view owner;
    std::string_view kind;
    std::string_view name;
    std::string_view type_name;
};

struct schema_enum_descriptor {
    std::string_view module;
    std::string_view name;
    std::uint8_t alignment = 0;
    descriptor_range values{};
};

struct schema_enum_value_descriptor {
    std::string_view module;
    std::string_view owner;
    std::string_view name;
    std::int64_t value = 0;
};

// Named or ordinal PE export. PE files do not encode C++ prototypes; `name`
// therefore identifies the callable export, while ABI must come from official
// headers or verified reverse engineering. Forwarded exports name their target.
struct export_descriptor {
    std::string_view module;
    std::string_view name;
    std::string_view forwarder;
    std::uint32_t rva = 0;
    std::uint16_t ordinal = 0;
    bool forwarded = false;
};

struct catalog_info {
    std::string_view generated_at_utc;
    std::uint32_t engine_build_number = 0;
    std::uint32_t module_count = 0;
    std::uint32_t interface_count = 0;
    std::uint32_t offset_count = 0;
    std::uint32_t signature_count = 0;
    std::uint32_t class_count = 0;
    std::uint32_t field_count = 0;
    std::uint32_t metadata_count = 0;
    std::uint32_t enum_count = 0;
    std::uint32_t enum_value_count = 0;
    std::uint32_t export_count = 0;
};

[[nodiscard]] ASKERDWAY_DOTA_API const catalog_info& info() noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const module_descriptor> modules() noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const interface_descriptor> interfaces() noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const offset_descriptor> offsets() noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const signature_descriptor> signatures() noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const schema_class_descriptor> schema_classes() noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const schema_field_descriptor> schema_fields() noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const schema_metadata_descriptor> schema_metadata() noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const schema_enum_descriptor> schema_enums() noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const schema_enum_value_descriptor> schema_enum_values() noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const export_descriptor> exports() noexcept;

[[nodiscard]] ASKERDWAY_DOTA_API const module_descriptor* find_module(
    std::string_view module) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API const interface_descriptor* find_interface(
    std::string_view module, std::string_view name) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API const offset_descriptor* find_offset(
    std::string_view module, std::string_view name) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API const signature_descriptor* find_signature(
    std::string_view module, std::string_view name) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API const schema_class_descriptor* find_class(
    std::string_view module, std::string_view name) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API const schema_field_descriptor* find_field(
    std::string_view module, std::string_view owner,
    std::string_view name) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API const schema_enum_descriptor* find_enum(
    std::string_view module, std::string_view name) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API const export_descriptor* find_export(
    std::string_view module, std::string_view name) noexcept;

[[nodiscard]] ASKERDWAY_DOTA_API std::span<const interface_descriptor> interfaces(
    const module_descriptor& module) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const offset_descriptor> offsets(
    const module_descriptor& module) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const schema_class_descriptor> schema_classes(
    const module_descriptor& module) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const schema_enum_descriptor> schema_enums(
    const module_descriptor& module) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const export_descriptor> exports(
    const module_descriptor& module) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const schema_field_descriptor> fields(
    const schema_class_descriptor& owner) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const schema_metadata_descriptor> metadata(
    const schema_class_descriptor& owner) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::span<const schema_enum_value_descriptor> values(
    const schema_enum_descriptor& owner) noexcept;

// Convert a catalog RVA into a process-local address after validating it
// against the loaded module image. Returns zero on invalid input.
[[nodiscard]] ASKERDWAY_DOTA_API std::uintptr_t resolve_rva(
    std::uintptr_t module_base, std::uintptr_t rva) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::uintptr_t resolve_interface(
    std::uintptr_t module_base,
    const interface_descriptor& interface_entry) noexcept;

// Scan executable PE sections for exactly one signature match. The overload
// with a descriptor also applies its RIP/immediate extraction rule.
[[nodiscard]] ASKERDWAY_DOTA_API std::uintptr_t scan_signature(
    std::uintptr_t module_base, std::size_t image_size,
    std::string_view ida_pattern) noexcept;
[[nodiscard]] ASKERDWAY_DOTA_API std::uintptr_t resolve_signature(
    std::uintptr_t module_base, std::size_t image_size,
    const signature_descriptor& signature) noexcept;

// Resolve a named/ordinal export from an already loaded module. This wrapper is
// intentionally untyped: cast only after verifying the function's real ABI.
[[nodiscard]] ASKERDWAY_DOTA_API std::uintptr_t resolve_export(
    std::uintptr_t module_base, const export_descriptor& export_entry) noexcept;

} // namespace askerdway::dota_api

#undef ASKERDWAY_DOTA_API
