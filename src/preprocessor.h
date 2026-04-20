#pragma once

#include "./sourcemap.h"
#include "./source_file.h"

typedef struct VM VM;

// Expands preprocessor directives and macros in `source`. When
// `source_map` is non-NULL and `origin_file_id` is valid, the expanded
// buffer's per-line origin bytes are recorded so downstream consumers
// (scanner, diagnostics, LSP) can translate expanded positions back to
// user-visible source coordinates.
char* preprocess(VM* vm, const char* source,
                 SourceMap* source_map, ZymFileId origin_file_id);
