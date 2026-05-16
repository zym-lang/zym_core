#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "zym/sourcemap.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ZYM_VM_FWD_DECLARED
#define ZYM_VM_FWD_DECLARED
typedef struct VM ZymVM;
#endif

typedef struct {
    char* source;
    ZymSourceMap* source_map;
    ZymFileId file_id;
} ModuleReadResult;

typedef ModuleReadResult (*ModuleReadCallback)(const char* path, void* user_data);

// Optional resolve callback. Invoked by the loader BEFORE any internal
// path math (no `resolve_module_path` join, no `normalize_path`) and
// BEFORE the cycle detector and module cache run. The resolver is the
// authority over the canonical module key.
//
// Arguments:
//   - `spec`     : the raw import spec exactly as it appeared in source
//                  (e.g. `"@/foo.zym"`, `"./bar.zym"`, `"std/json"`).
//                  The loader does NOT pre-join this with the importer's
//                  directory before calling the resolver.
//   - `importer` : the resolved canonical path of the module that issued
//                  the import, or NULL when resolving the entry module
//                  (which has no importer).
//   - `user_data`: opaque pointer from `loadModulesEx`.
//
// Return value:
//   - a NUL-terminated C string (BORROWED — the loader copies it into
//     its own allocator-owned storage immediately on return; the
//     callback's pointer does NOT need to outlive the call) to use as
//     the canonical key for cycle detection, caching, the
//     `read_callback` path argument, and as the `importer` of any
//     transitive imports, OR
//   - NULL to fall back to the loader's default `resolve_module_path`
//     (directory join + `normalize_path`) — identical to not installing
//     a resolve callback at all for that spec.
//
// The callback may also read the active import frame via
// `zym_currentImport*` accessors.
typedef const char* (*ModuleResolveCallback)(const char* spec,
                                             const char* importer,
                                             void* user_data);

typedef struct {
    char* combined_source;
    ZymSourceMap* source_map;
    char** module_paths;
    int module_count;
    bool has_error;
    char* error_message;
} ModuleLoadResult;

ModuleLoadResult* loadModules(
    ZymVM* vm,
    const char* entry_source,
    ZymSourceMap* entry_source_map,
    const char* entry_path,
    ModuleReadCallback read_callback,
    void* user_data,
    bool debug_names,
    bool write_debug_output,
    const char* debug_output_path
);

// Extended entry point. Identical to `loadModules` plus an optional
// `resolve_callback`. Pass NULL for `resolve_callback` for byte-identical
// behavior to `loadModules` (which is now a thin wrapper around this).
ModuleLoadResult* loadModulesEx(
    ZymVM* vm,
    const char* entry_source,
    ZymSourceMap* entry_source_map,
    const char* entry_path,
    ModuleReadCallback read_callback,
    ModuleResolveCallback resolve_callback,
    void* user_data,
    bool debug_names,
    bool write_debug_output,
    const char* debug_output_path
);

void freeModuleLoadResult(ZymVM* vm, ModuleLoadResult* result);

// =============================================================================
// Module-loader runtime introspection
// =============================================================================
//
// The module loader maintains an internal `ImportStack` that records the
// chain of in-flight `read_callback` invocations: the bottom is the entry
// module's resolved path, the top is the path currently being read. The
// following accessors expose that state to an embedder's `read_callback`
// (or anything reachable from one) WITHOUT changing the callback signature.
//
// All three are valid ONLY while the loader is actively dispatching a
// `read_callback` on `vm` (`zym_currentImportDepth(vm) > 0`). Outside
// that window depth is 0 and the path accessors return NULL.
//
// Higher-level binding code (e.g. `vm.moduleLoader.getCaller()` in the
// CLI's `Zym` native) is responsible for translating "depth == 0" into a
// language-level error when called outside an active callback.

int zym_currentImportDepth(ZymVM* vm);
const char* zym_currentImportPathAt(ZymVM* vm, int i);
const char* zym_currentImportCaller(ZymVM* vm);

#ifdef __cplusplus
}
#endif
