if(DEFINED ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/esp-idf.cmake)
    return()
endif()

cmake_minimum_required(VERSION 3.20)
project(zym_core C)

set(CMAKE_C_STANDARD 11)

option(ZYM_RUNTIME_ONLY "Build runtime-only (no compiler)" OFF)

# --- Feature flags (LSP-facing surface gating) -----------------------------
# Declares ZYM_ENABLE_* options and ZYM_HAS_* compile definitions. See
# compile-time feature flags for the LSP surface.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/Features.cmake)
zym_core_declare_features()
zym_core_configure_feature_header(ZYM_CORE_GENERATED_INCLUDE_DIR)

# --- Runtime sources (always built) ---
set(ZYM_CORE_SOURCES
    src/_config_assert.c
    src/_config_report.c
    src/value.c
    src/chunk.c
    src/vm.c
    src/object.c
    src/memory.c
    src/table.c
    src/serializer.c
    src/utils.c
    src/zym.c
    src/gc.c
    src/native.c
    src/utf8.c
    src/source_file.c
    src/diagnostics.c
    src/sourcemap.c
    src/modules/core_modules.c
    src/modules/continuation.c
    src/modules/preemption.c
    src/natives/core_natives.c
    src/natives/gc_native.c
    src/natives/conversions.c
    src/natives/error.c
    src/natives/list.c
    src/natives/map.c
    src/natives/shared.c
    src/natives/string_natives.c
    src/natives/math.c
    src/natives/typeof.c
)

# --- Compiler sources (only for full build) ---
if(NOT ZYM_RUNTIME_ONLY)
    list(APPEND ZYM_CORE_SOURCES
        src/scanner.c
        src/preprocessor.c
        src/ast.c
        src/parser.c
        src/compiler.c
        src/debug.c
        src/module_loader.c
    )
endif()

# --- Build as a static library ---
add_library(zym_core STATIC ${ZYM_CORE_SOURCES})

# Public headers: consumers of this library get include/ on their include path
# Private: internal .c files can use relative includes within src/
target_include_directories(zym_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${ZYM_CORE_GENERATED_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Publish ZYM_HAS_* compile definitions on the PUBLIC surface so every TU
# and every embedder sees identical values.
zym_core_apply_features(zym_core)

if(ZYM_RUNTIME_ONLY)
    target_compile_definitions(zym_core PUBLIC ZYM_RUNTIME_ONLY)
endif()

# Platform libraries
if(EMSCRIPTEN)
    # Emscripten provides libm built-in; no platform libs needed
elseif(WIN32)
    target_link_libraries(zym_core PUBLIC m ws2_32 userenv iphlpapi)
else()
    target_link_libraries(zym_core PUBLIC m)
endif()