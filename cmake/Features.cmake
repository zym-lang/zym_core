# zym_core/cmake/Features.cmake
#
# Declares the compile-time feature flags that gate the LSP-facing surface
# of `zym_core`.
#
# Usage:
#   include(${CMAKE_CURRENT_LIST_DIR}/cmake/Features.cmake)
#   zym_core_declare_features()
#   zym_core_apply_features(<target>)  # after the target is created
#
# The flags are build-time, not runtime. A build with a flag OFF must not
# link in the corresponding types, functions, or strings. An on-device
# (MCU) build can safely set `ZYM_ENABLE_LSP_SURFACE=OFF` and pay nothing
# for the tooling surface.

# Guard against multiple inclusion from different entry points
# (root CMakeLists.txt vs. esp-idf.cmake).
if(DEFINED _ZYM_CORE_FEATURES_INCLUDED)
    return()
endif()
set(_ZYM_CORE_FEATURES_INCLUDED TRUE)

# Capture this file's directory at include time. `CMAKE_CURRENT_LIST_DIR`
# reflects the currently-executing listfile, which shifts once control
# returns to the caller and re-enters this file via a function call — so
# record it once, here, and use the absolute path from then on.
set(_ZYM_CORE_FEATURES_DIR "${CMAKE_CURRENT_LIST_DIR}")

# ---------------------------------------------------------------------------
# zym_core_declare_features()
#
# Declares the ZYM_ENABLE_* cache options and enforces their implications.
# Must be called before `zym_core_apply_features`.
# ---------------------------------------------------------------------------
function(zym_core_declare_features)
    option(ZYM_ENABLE_LSP_SURFACE
        "Umbrella: parse tree + symbols + native metadata + diag codes" ON)

    option(ZYM_ENABLE_PARSE_TREE_RETENTION
        "Retain AST past codegen (required for LSP queries)"
        ${ZYM_ENABLE_LSP_SURFACE})

    option(ZYM_ENABLE_SYMBOL_TABLE
        "Run resolver and expose symbol table API"
        ${ZYM_ENABLE_LSP_SURFACE})

    option(ZYM_ENABLE_NATIVE_METADATA
        "Store extended ZymNativeInfo fields (summary, docs, params, since, deprecated)"
        ${ZYM_ENABLE_LSP_SURFACE})

    option(ZYM_ENABLE_DIAGNOSTIC_CODES
        "Ship stable diagnostic-code table and code/hint fields on ZymDiagnostic"
        ${ZYM_ENABLE_LSP_SURFACE})

    # Enforce: SYMBOL_TABLE on => PARSE_TREE_RETENTION on.
    # The resolver walks a retained parse tree; it cannot run without one.
    if(ZYM_ENABLE_SYMBOL_TABLE AND NOT ZYM_ENABLE_PARSE_TREE_RETENTION)
        message(FATAL_ERROR
            "zym_core: ZYM_ENABLE_SYMBOL_TABLE=ON requires "
            "ZYM_ENABLE_PARSE_TREE_RETENTION=ON. The resolver consumes a "
            "retained parse tree.")
    endif()

    # Promote to cache as BOOL for clean reporting.
    set(ZYM_ENABLE_LSP_SURFACE         ${ZYM_ENABLE_LSP_SURFACE}         PARENT_SCOPE)
    set(ZYM_ENABLE_PARSE_TREE_RETENTION ${ZYM_ENABLE_PARSE_TREE_RETENTION} PARENT_SCOPE)
    set(ZYM_ENABLE_SYMBOL_TABLE        ${ZYM_ENABLE_SYMBOL_TABLE}        PARENT_SCOPE)
    set(ZYM_ENABLE_NATIVE_METADATA     ${ZYM_ENABLE_NATIVE_METADATA}     PARENT_SCOPE)
    set(ZYM_ENABLE_DIAGNOSTIC_CODES    ${ZYM_ENABLE_DIAGNOSTIC_CODES}    PARENT_SCOPE)

    message(STATUS "zym_core features:")
    message(STATUS "  ZYM_ENABLE_LSP_SURFACE          = ${ZYM_ENABLE_LSP_SURFACE}")
    message(STATUS "  ZYM_ENABLE_PARSE_TREE_RETENTION = ${ZYM_ENABLE_PARSE_TREE_RETENTION}")
    message(STATUS "  ZYM_ENABLE_SYMBOL_TABLE         = ${ZYM_ENABLE_SYMBOL_TABLE}")
    message(STATUS "  ZYM_ENABLE_NATIVE_METADATA      = ${ZYM_ENABLE_NATIVE_METADATA}")
    message(STATUS "  ZYM_ENABLE_DIAGNOSTIC_CODES     = ${ZYM_ENABLE_DIAGNOSTIC_CODES}")
endfunction()

# ---------------------------------------------------------------------------
# zym_core_configure_feature_header(<out_include_dir_var>)
#
# Runs `configure_file` on `include/zym/config.h.in` into the build tree and
# writes the absolute path of the generated include directory into the named
# variable in the caller's scope. The caller is responsible for adding that
# directory to the target's include path.
# ---------------------------------------------------------------------------
function(zym_core_configure_feature_header out_include_dir_var)
    # Map CMake booleans to 0/1 integer macros. configure_file with #cmakedefine01
    # handles truthy-to-1, but we set explicit 0/1 vars for @VAR@ substitution
    # clarity in the header template.
    foreach(_flag
            ZYM_ENABLE_LSP_SURFACE
            ZYM_ENABLE_PARSE_TREE_RETENTION
            ZYM_ENABLE_SYMBOL_TABLE
            ZYM_ENABLE_NATIVE_METADATA
            ZYM_ENABLE_DIAGNOSTIC_CODES)
        if(${_flag})
            set(_${_flag}_01 1)
        else()
            set(_${_flag}_01 0)
        endif()
    endforeach()

    set(_in  "${_ZYM_CORE_FEATURES_DIR}/../include/zym/config.h.in")
    get_filename_component(_in "${_in}" REALPATH)
    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(_out "${_gen_dir}/zym/config.h")

    configure_file("${_in}" "${_out}" @ONLY)

    set(${out_include_dir_var} "${_gen_dir}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# zym_core_apply_features(<target>)
#
# Attaches the ZYM_HAS_* compile definitions to <target> as PUBLIC so every
# TU and every embedder sees identical values.
# ---------------------------------------------------------------------------
function(zym_core_apply_features tgt)
    foreach(_pair
            "ZYM_HAS_LSP_SURFACE=$<BOOL:${ZYM_ENABLE_LSP_SURFACE}>"
            "ZYM_HAS_PARSE_TREE_RETENTION=$<BOOL:${ZYM_ENABLE_PARSE_TREE_RETENTION}>"
            "ZYM_HAS_SYMBOL_TABLE=$<BOOL:${ZYM_ENABLE_SYMBOL_TABLE}>"
            "ZYM_HAS_NATIVE_METADATA=$<BOOL:${ZYM_ENABLE_NATIVE_METADATA}>"
            "ZYM_HAS_DIAGNOSTIC_CODES=$<BOOL:${ZYM_ENABLE_DIAGNOSTIC_CODES}>")
        target_compile_definitions(${tgt} PUBLIC ${_pair})
    endforeach()
endfunction()
