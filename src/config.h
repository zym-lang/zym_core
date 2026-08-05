#pragma once

#include <stdbool.h>

typedef struct CompilerConfig {
    bool include_line_info;
    // Symbol stripping: globals DEFINED by the compilation unit are
    // renamed to compact symbols ("s0", "s1", ... in definition order)
    // in the emitted bytecode. Names listed in keep_names, names that
    // collide with a registered native, and everything data-bearing
    // (map keys, struct fields, enum variants, string literals) are
    // left untouched. keep_names entries are base names (no @arity).
    bool strip_symbols;
    const char* const* keep_names;
    int keep_name_count;
} CompilerConfig;

typedef CompilerConfig ZymCompilerConfig;

typedef enum {
    ZYM_STATUS_OK,
    ZYM_STATUS_COMPILE_ERROR,
    ZYM_STATUS_RUNTIME_ERROR,
    ZYM_STATUS_YIELD,
    // Host-initiated stop. Appended so existing numeric values are stable.
    ZYM_STATUS_ABORTED
} ZymStatus;