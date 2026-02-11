#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct VM VM;
typedef struct LineMap LineMap;

typedef struct {
    char* source;
    LineMap* line_map;
} ModuleReadResult;

typedef ModuleReadResult (*ModuleReadCallback)(const char* path, void* user_data);

typedef struct {
    char* combined_source;
    LineMap* line_map;
    char** module_paths;
    int module_count;
    bool has_error;
    char* error_message;
} ModuleLoadResult;

ModuleLoadResult* loadModules(
    VM* vm,
    const char* entry_source,
    LineMap* entry_line_map,
    const char* entry_path,
    ModuleReadCallback read_callback,
    void* user_data,
    bool debug_names,
    bool write_debug_output,
    const char* debug_output_path
);

void freeModuleLoadResult(VM* vm, ModuleLoadResult* result);
