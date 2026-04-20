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

void freeModuleLoadResult(VM* vm, ModuleLoadResult* result);
