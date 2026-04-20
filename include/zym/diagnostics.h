#pragma once

// =============================================================================
// Structured diagnostics — public API.
//
// Replaces the old "fprintf(stderr, …)" path in the frontend. Every parse /
// compile error, warning, note or hint is recorded as a ZymDiagnostic on the
// VM's internal sink. Embedders drain the sink via zymGetDiagnostics() after
// a compile and render them however they want (CLI caret UI, LSP Diagnostic,
// red squiggles in an editor, silent accumulation for batch tooling, …).
//
// The existing ZymErrorCallback is still fired in lockstep for back-compat.
// New code should prefer draining diagnostics — the sink carries byte-granular
// span information that the legacy callback does not.
//
// The `code` and `hint` fields are compiled out on builds where
// ZYM_HAS_DIAGNOSTIC_CODES == 0 so size-constrained (MCU) hosts pay nothing
// for stable diagnostic-code bookkeeping.
// =============================================================================

#include <stdint.h>
#include <stddef.h>

#include "zym/config.h"
#include "zym/sourcemap.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ZYM_VM_FWD_DECLARED
#define ZYM_VM_FWD_DECLARED
typedef struct VM ZymVM;
#endif

typedef enum {
    ZYM_DIAG_ERROR   = 0,
    ZYM_DIAG_WARNING = 1,
    ZYM_DIAG_INFO    = 2,
    ZYM_DIAG_HINT    = 3
} ZymDiagSeverity;

// A single structured diagnostic. All string fields (message / code / hint)
// are owned by the VM's DiagnosticSink and remain valid until the next call
// to zymClearDiagnostics() or VM teardown. Embedders that need to keep a
// diagnostic beyond that must copy the strings.
typedef struct {
    ZymDiagSeverity severity;

    // Source location (best effort — older frontend paths still emit
    // line-only spans where byte granularity is not yet wired through).
    ZymFileId fileId;       // ZYM_FILE_ID_INVALID when unknown.
    int       startByte;    // -1 when unknown.
    int       length;       // 0 when unknown.
    int       line;         // 1-based; -1 when unknown.
    int       column;       // 1-based byte column; -1 when unknown.

    const char* message;    // always present, NUL-terminated.

#if ZYM_HAS_DIAGNOSTIC_CODES
    const char* code;       // e.g. "E0001"; may be NULL.
    const char* hint;       // optional "did you mean …?" suggestion.
#endif
} ZymDiagnostic;

// Returns a pointer to the VM's current diagnostic buffer and writes the
// count out. The pointer is valid until the next push / clear / VM free.
// Returns NULL when the buffer is empty (count is still written).
const ZymDiagnostic* zymGetDiagnostics(ZymVM* vm, size_t* count);

// Drop every currently-recorded diagnostic. Frees the message / code / hint
// storage owned by each entry. Does not shrink the underlying capacity.
void zymClearDiagnostics(ZymVM* vm);

#ifdef __cplusplus
}
#endif
