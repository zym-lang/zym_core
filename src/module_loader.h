#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "./sourcemap.h"
#include "./source_file.h"

typedef struct VM VM;

// Result produced by the embedder's per-module read callback. The module
// loader owns `source` and `source_map` after return; both are freed via
// the allocator on loader shutdown. `file_id` is the SourceFileRegistry
// id under which the embedder registered this module's bytes, used as
// the origin fileId for every segment of `source_map`.
typedef struct {
    char* source;
    SourceMap* source_map;
    ZymFileId file_id;
} ModuleReadResult;

typedef ModuleReadResult (*ModuleReadCallback)(const char* path, void* user_data);

// Optional resolve callback. See `include/zym/module_loader.h` for the
// full contract. Returns NULL to keep the loader's default behavior;
// otherwise a NUL-terminated borrowed string that the loader copies
// internally and uses as the canonical key for cycle detection,
// caching, and the subsequent `read_callback`.
typedef const char* (*ModuleResolveCallback)(const char* path, void* user_data);

typedef struct {
    char* combined_source;
    SourceMap* source_map;
    char** module_paths;
    int module_count;
    bool has_error;
    char* error_message;
} ModuleLoadResult;

ModuleLoadResult* loadModules(
    VM* vm,
    const char* entry_source,
    SourceMap* entry_source_map,
    const char* entry_path,
    ModuleReadCallback read_callback,
    void* user_data,
    bool debug_names,
    bool write_debug_output,
    const char* debug_output_path
);

// Extended entry point. `resolve_callback == NULL` is byte-identical to
// `loadModules`; the latter is now a thin wrapper that passes NULL.
ModuleLoadResult* loadModulesEx(
    VM* vm,
    const char* entry_source,
    SourceMap* entry_source_map,
    const char* entry_path,
    ModuleReadCallback read_callback,
    ModuleResolveCallback resolve_callback,
    void* user_data,
    bool debug_names,
    bool write_debug_output,
    const char* debug_output_path
);

void freeModuleLoadResult(VM* vm, ModuleLoadResult* result);

// =============================================================================
// Module-loader runtime introspection
// =============================================================================
//
// The module loader maintains an internal `ImportStack` that records the
// chain of in-flight `read_callback` invocations: the bottom of the stack
// is the entry module's resolved path, the top is the path that is
// currently being read. The following accessors expose that state to an
// embedder's `read_callback` (or any code reachable from one) without
// changing the `ModuleReadCallback` signature.
//
// All three are valid ONLY while the loader is actively dispatching a
// `read_callback` on `vm` (`zym_currentImportDepth(vm) > 0`). Outside
// that window:
//   - `zym_currentImportDepth(vm)` returns 0.
//   - `zym_currentImportPathAt(vm, i)` returns NULL.
//   - `zym_currentImportCaller(vm)` returns NULL.
//
// Higher-level binding code (e.g. `vm.moduleLoader.getCaller()` in the
// CLI's `Zym` native) is responsible for translating "depth == 0" into a
// language-level error when called outside an active callback.

// Number of modules currently on the active import stack, including the
// one whose `read_callback` is in flight. 0 when no callback is active.
int zym_currentImportDepth(VM* vm);

// Resolved module_path at index `i` of the active import stack
// (0 = entry, depth-1 = currently-loading module). NULL on OOB or when
// no callback is active. Pointer is owned by the loader and only valid
// for the duration of the current `read_callback` invocation.
const char* zym_currentImportPathAt(VM* vm, int i);

// Resolved module_path of the *immediate parent* of the currently-loading
// module — i.e. who issued the `import("...")` that triggered this
// callback. NULL when the entry module is being loaded (no caller) or
// when no callback is active.
const char* zym_currentImportCaller(VM* vm);
