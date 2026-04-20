#pragma once

#include "./linemap.h"
#include "./sourcemap.h"
#include "./source_file.h"

typedef struct VM VM;

// Legacy entry point: expands `source`, populates `line_map` with
// per-expanded-line origin line numbers. Semantics unchanged since the
// pre-1.2 frontend; equivalent to `preprocessEx(vm, source, line_map,
// NULL, ZYM_FILE_ID_INVALID)`.
char* preprocess(VM* vm, const char* source, LineMap* line_map);

// Phase 1.2 entry point: same as `preprocess()` but additionally
// populates a SourceMap with per-expanded-line origin byte ranges
// resolved against `origin_file_id`. Pass `source_map == NULL` and
// `origin_file_id == ZYM_FILE_ID_INVALID` to get the legacy behavior.
char* preprocessEx(VM* vm, const char* source, LineMap* line_map,
                   SourceMap* source_map, ZymFileId origin_file_id);
