cmake_minimum_required(VERSION 3.24)

project(askerdway VERSION 1.0.0 LANGUAGES C CXX RC)

option(ASKERDWAY_DIAGNOSTICS "Compile runtime performance counters (debug logging remains opt-in)" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(ASKERDWAY_OUTPUT_NAME "askerdway" CACHE STRING "DLL file name without extension")
set(ASKERDWAY_DLL_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH
    "Directory for the compiled DLL; use the build directory when the installed file is locked")
if(NOT ASKERDWAY_OUTPUT_NAME STREQUAL "askerdway")
    message(FATAL_ERROR "Keep the DLL name askerdway; versioned DLL copies are not supported.")
endif()
cmake_path(ABSOLUTE_PATH ASKERDWAY_DLL_OUTPUT_DIRECTORY NORMALIZE OUTPUT_VARIABLE _ASKERDWAY_DLL_DIRECTORY)
if(_ASKERDWAY_DLL_DIRECTORY STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(FATAL_ERROR "DLL output must be a build directory, not the project root. Use a separate build directory.")
endif()
include(FetchContent)
if(MSVC)
    add_compile_options(/utf-8 /Brepro /experimental:deterministic
        "/pathmap:${CMAKE_CURRENT_SOURCE_DIR}=askerdway"
        "/pathmap:${CMAKE_CURRENT_BINARY_DIR}=build"
        "/pathmap:$ENV{USERPROFILE}=user")
    add_link_options($<$<CONFIG:Release>:/DEBUG:NONE> /Brepro)
endif()

FetchContent_Declare(
    imgui
    URL https://codeload.github.com/ocornut/imgui/tar.gz/8936b58fe26e8c3da834b8f60b06511d537b4c63
    URL_HASH SHA256=a1e4443190d78976e0901039dd2207e1d59a4b5b818489232568bdb69ff79ce6
)
FetchContent_MakeAvailable(imgui)
FetchContent_Declare(minhook
    URL https://codeload.github.com/TsudaKageyu/minhook/tar.gz/c3fcafdc10146beb5919319d0683e44e3c30d537
    URL_HASH SHA256=31757833475be039c16359d4e276e718db6ba2ba61a6b870053a112106015822)
FetchContent_MakeAvailable(minhook)

set(ASKERDWAY_PUBLIC_HEADERS
    askerdway.hpp
    include/askerdway/api.hpp
    include/askerdway/config.hpp
    include/askerdway/comfort.hpp
    include/askerdway/dota_api.hpp
    include/askerdway/dota_sdk.hpp
    include/askerdway/export.hpp
    include/askerdway/legacy.hpp
    include/askerdway/render.hpp
    include/askerdway/render_snapshot.hpp
    include/askerdway/status.hpp
    include/askerdway/telemetry.hpp
    include/askerdway/version.hpp
    include/askerdway/engine/handles.hpp
    include/askerdway/engine/runtime.hpp
    include/askerdway/engine/schema.hpp
    include/askerdway/engine/types.hpp
)

set(ASKERDWAY_INTERNAL_HEADERS
    src/runtime/render_feature_state.h
    src/runtime/comfort_policy.h
    src/runtime/comfort_messages.h
    src/runtime/comfort_protobuf_bridge.h
    src/runtime/comfort_settings.h
    src/runtime/comfort_item_ids.h
    src/runtime/runtime_worker.h
    src/runtime/effect_activity.h
    src/runtime/effect_marks.h
    src/runtime/effect_validation.h
    src/runtime/neutral_spawn_boxes.h
    src/render/particle_source_catalog.h
    src/render/particle_ability_match.h
    src/render/particle_item_aliases.h
    src/render/world_frame_renderer.h
    src/ui/tile_raster_surface.h
    src/ui/hero_portrait_manifest.h
    src/ui/hero_portraits.h
    src/ui/ability_icons.h
    src/ui/ability_icon_manifest.h
    src/ui/embedded_image.h
    src/ui/interface_fonts.h
    src/ui/portfolio_theme.h
    src/ui/sidebar_icon_resources.h
    src/ui/sidebar_icons.h
)

set(ASKERDWAY_ASSETS
    assets/fonts/NordwandMono-Regular.ttf
    assets/fonts/NordwandMono-Bold.ttf
    assets/fonts/LICENSE-FONT.txt
    assets/icons/armlet_menu_64.png
    assets/icons/dota2_hero_portrait_atlas.png
    assets/icons/dota2_ability_atlas.png
)
set_source_files_properties(${ASKERDWAY_ASSETS} PROPERTIES HEADER_FILE_ONLY TRUE)

# Runtime source order is significant because these files share internal state.
set(ASKERDWAY_RUNTIME_SOURCES
    src/runtime/runtime_foundation.cpp
    src/runtime/render_feature_state.cpp
    src/runtime/engine_memory_readers.cpp
    src/runtime/incoming_damage_sources.cpp
    src/runtime/schema_and_inventory.cpp
    src/runtime/order_dispatch_and_game_window.cpp
    src/runtime/comfort_runtime.cpp
    src/ui/hero_portraits.cpp
    src/ui/ability_icons.cpp
    src/ui/interface_fonts.cpp
    src/ui/sidebar_icons.cpp
    src/runtime/overlay_window.cpp
    src/runtime/overlay_menu.cpp
    src/runtime/armlet_state_reader.cpp
    src/runtime/damage_safety_timeline.cpp
    src/runtime/armlet_automation.cpp
    src/runtime/runtime_worker.cpp
    src/api/public_api.cpp
    src/render/particle_visibility_hook.cpp
    src/render/world_frame_renderer.cpp
)

# The generated catalog is deliberately compiled separately: it contains every
# live SchemaSystem declaration and PE export from the installed Dota modules,
# and would otherwise make the runtime unity translation unit unnecessarily
# large and expensive to rebuild.
set(ASKERDWAY_GENERATED_API_SOURCES
    src/api/dota_api.cpp
)
set(ASKERDWAY_COMMON_RUNTIME_SOURCES ${ASKERDWAY_RUNTIME_SOURCES})
list(REMOVE_ITEM ASKERDWAY_COMMON_RUNTIME_SOURCES src/render/world_frame_renderer.cpp)
list(TRANSFORM ASKERDWAY_COMMON_RUNTIME_SOURCES
    PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
    OUTPUT_VARIABLE ASKERDWAY_UNITY_INPUT_DEPENDENCIES)

add_library(askerdway_core OBJECT
    dllmain.cpp
    askerdway_resources.rc
    ${ASKERDWAY_PUBLIC_HEADERS}
    ${ASKERDWAY_INTERNAL_HEADERS}
    ${ASKERDWAY_ASSETS}
    ${ASKERDWAY_COMMON_RUNTIME_SOURCES}
    ${ASKERDWAY_GENERATED_API_SOURCES}
    src/api/dota_sdk.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)


add_library(askerdway SHARED src/render/world_frame_renderer.cpp $<TARGET_OBJECTS:askerdway_core>)
set(ASKERDWAY_DLL_TARGETS askerdway)

# Keep dllmain and Dear ImGui outside the unity source.
set_target_properties(askerdway_core PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE 64
    UNITY_BUILD_RELOCATABLE ON
)
set_source_files_properties(
    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/askerdway_core.dir/Unity/unity_0_cxx.cxx"
    PROPERTIES OBJECT_DEPENDS
        "${ASKERDWAY_UNITY_INPUT_DEPENDENCIES}"
)
set_source_files_properties(
    dllmain.cpp
    src/ui/hero_portraits.cpp
    src/ui/ability_icons.cpp
    src/ui/sidebar_icons.cpp
    src/ui/interface_fonts.cpp
    src/api/dota_api.cpp
    src/api/dota_sdk.cpp
    src/render/world_frame_renderer.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    PREFIX "Public API" FILES ${ASKERDWAY_PUBLIC_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PREFIX "Internal headers" FILES ${ASKERDWAY_INTERNAL_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    PREFIX "Assets" FILES ${ASKERDWAY_ASSETS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PREFIX "Runtime" FILES ${ASKERDWAY_RUNTIME_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src/api"
    PREFIX "Generated API" FILES ${ASKERDWAY_GENERATED_API_SOURCES})
source_group("Entry point" FILES dllmain.cpp)
source_group("SDK" FILES src/api/dota_sdk.cpp)
source_group("Resources" FILES askerdway_resources.rc)

foreach(runtime_target IN ITEMS askerdway_core ${ASKERDWAY_DLL_TARGETS})
    target_compile_features(${runtime_target} PRIVATE cxx_std_20)
    target_compile_definitions(${runtime_target} PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        ASKERDWAY_COMBINED=1 ASKERDWAY_BUILDING_DLL=1 RENDER_TELEPORTATION_EMBEDDED=1)
    target_compile_definitions(${runtime_target} PRIVATE
        ASKERDWAY_DIAGNOSTICS=$<BOOL:${ASKERDWAY_DIAGNOSTICS}>)
    target_include_directories(${runtime_target} PRIVATE
        ${imgui_SOURCE_DIR} ${minhook_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(${runtime_target} PRIVATE /O2 /W4 /permissive- /EHsc /bigobj /utf-8)
    else()
        target_compile_options(${runtime_target} PRIVATE -O3 -Wall -Wextra -Wpedantic)
    endif()
endforeach()
foreach(dll_target IN LISTS ASKERDWAY_DLL_TARGETS)
    target_link_libraries(${dll_target} PRIVATE
        d3d11 d3dcompiler dxgi dwmapi gdi32 ole32 user32 windowscodecs minhook)
    if(MINGW)
        target_link_options(${dll_target} PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--gc-sections)
    endif()
    if(MSVC)
        target_link_options(${dll_target} PRIVATE $<$<CONFIG:Release>:/DEBUG:NONE>
            $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    endif()
endforeach()

if(NOT MSVC)
    message(FATAL_ERROR "The native bridge requires the MSVC Rust target")
endif()
find_program(ASKERDWAY_CARGO cargo REQUIRED)
set(_rust_root "${CMAKE_CURRENT_SOURCE_DIR}/src/intelligence")
set(_rust_target "${CMAKE_CURRENT_BINARY_DIR}/rust-target")
file(GLOB _rust_sources CONFIGURE_DEPENDS "${_rust_root}/src/*.rs")
add_custom_command(OUTPUT "${_rust_target}/release/askerdway_intelligence.lib"
    COMMAND "${CMAKE_COMMAND}" -E env
        "RUSTFLAGS=--remap-path-prefix=${CMAKE_CURRENT_SOURCE_DIR}=askerdway --remap-path-prefix=${CMAKE_CURRENT_BINARY_DIR}=build --remap-path-prefix=$ENV{USERPROFILE}=user"
        "${ASKERDWAY_CARGO}" build --release --locked
        --manifest-path "${_rust_root}/Cargo.toml" --target-dir "${_rust_target}"
    DEPENDS ${_rust_sources} "${_rust_root}/Cargo.toml" "${_rust_root}/Cargo.lock"
        "${_rust_root}/build.rs" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/particle_source_catalog.h"
    VERBATIM)
add_custom_target(askerdway_rust_core DEPENDS "${_rust_target}/release/askerdway_intelligence.lib")
foreach(dll_target IN LISTS ASKERDWAY_DLL_TARGETS)
    add_dependencies(${dll_target} askerdway_rust_core)
    target_link_libraries(${dll_target} PRIVATE
        "${_rust_target}/release/askerdway_intelligence.lib" userenv ws2_32 bcrypt ntdll dbghelp)
endforeach()

set_target_properties(askerdway PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${ASKERDWAY_OUTPUT_NAME}"
    RUNTIME_OUTPUT_DIRECTORY "${ASKERDWAY_DLL_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${ASKERDWAY_DLL_OUTPUT_DIRECTORY}"
)
