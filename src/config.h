#pragma once

#include <stdbool.h>

typedef struct CompilerConfig {
    bool include_line_info;
} CompilerConfig;

typedef CompilerConfig ZymCompilerConfig;

typedef enum {
    ZYM_STATUS_OK,
    ZYM_STATUS_COMPILE_ERROR,
    ZYM_STATUS_RUNTIME_ERROR,
    ZYM_STATUS_YIELD
} ZymStatus;