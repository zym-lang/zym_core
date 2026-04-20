#pragma once

// Internal diagnostics sink — the implementation backing the public
// zym/diagnostics.h API. Every frontend (parser, compiler, …) routes its
// errors through pushDiagnostic(...) instead of fprintf(stderr, …) so that
// embedders (CLI, LSP, wasm playground, …) can drain structured records
// rather than scrape a shared stderr.

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "zym/diagnostics.h"

typedef struct VM VM;

// Per-VM diagnostic buffer. All message / code / hint strings are duplicated
// into allocator-owned storage so callers may pass formatted temporaries.
typedef struct {
    ZymDiagnostic* items;
    size_t count;
    size_t capacity;
    bool has_error;  // true once any ZYM_DIAG_ERROR has been pushed
} DiagnosticSink;

void diagsink_init(DiagnosticSink* sink);
void diagsink_free(VM* vm, DiagnosticSink* sink);
void diagsink_clear(VM* vm, DiagnosticSink* sink);

// Core push helper. file/startByte/length default to ZYM_FILE_ID_INVALID/-1/0
// when the caller does not yet have byte-granular span information.
// `line` and `column` are 1-based; pass -1 when unknown.
void pushDiagnostic(VM* vm,
                    ZymDiagSeverity severity,
                    ZymFileId fileId,
                    int startByte,
                    int length,
                    int line,
                    int column,
                    const char* fmt,
                    ...);

void pushDiagnosticV(VM* vm,
                     ZymDiagSeverity severity,
                     ZymFileId fileId,
                     int startByte,
                     int length,
                     int line,
                     int column,
                     const char* fmt,
                     va_list args);
