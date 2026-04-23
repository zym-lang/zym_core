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

void freeModuleLoadResult(ZymVM* vm, ModuleLoadResult* result);

#ifdef __cplusplus
}
#endif
