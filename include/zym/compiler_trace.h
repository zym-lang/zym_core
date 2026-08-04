/*
 * zym/compiler_trace.h — test-only compiler resolution-trace hook.
 *
 * STATUS: TEST-ONLY. This entire header is guarded by `ZYM_HAS_BUILD_TESTING`
 * and is only linked in when the repo is configured with
 * `-DZYM_ENABLE_BUILD_TESTING=ON`. No shipping build profile turns this on.
 * If you are an embedder, you do not want this header.
 *
 * Purpose: drive the Phase 4.5 parity test. The parallel resolver
 * (`zym_core/src/resolver.c`, public API in `zym/frontend.h`) and the
 * bytecode compiler (`zym_core/src/compiler.c`) must agree on how every
 * identifier in the source is classified. When build testing is enabled,
 * `compiler.c` emits one `ZymResolutionEntry` for each identifier it
 * classifies as LOCAL/UPVALUE/GLOBAL/PROPERTY. The parity test cross-
 * references those entries against the resolver's `ZymSymbolTable` and
 * fails loudly on silent disagreement.
 */
#ifndef ZYM_COMPILER_TRACE_H
#define ZYM_COMPILER_TRACE_H

#include "zym/config.h"

#if ZYM_HAS_BUILD_TESTING

#include <stddef.h>

#include "zym/sourcemap.h" /* ZymFileId, ZymVM forward decl */

#ifdef __cplusplus
extern "C" {
#endif

/* Compiler's classification of an identifier at compile time.
 * Mirrors the resolution paths in `compiler.c`:
 *   LOCAL     — resolved via `resolve_local`   (slot = register index)
 *   UPVALUE   — resolved via `resolve_upvalue` (slot = upvalue index in
 *               the enclosing function's upvalue array; `isLocalUpvalue`
 *               distinguishes "capture-from-local" vs "capture-from-
 *               enclosing-upvalue")
 *   GLOBAL    — fell through to `identifier_constant` + global op
 *   PROPERTY  — member access name resolved via struct-schema lookup or
 *               treated as a string key (resolver kind FIELD/VARIANT)
 */
typedef enum {
    ZYM_RES_LOCAL    = 0,
    ZYM_RES_UPVALUE  = 1,
    ZYM_RES_GLOBAL   = 2,
    ZYM_RES_PROPERTY = 3,
} ZymResolutionKind;

typedef struct {
    ZymFileId         fileId;
    int               byteOffset;   /* start byte in the file */
    int               length;       /* lexeme length */
    ZymResolutionKind kind;
    int               slotOrIndex;  /* LOCAL: register; UPVALUE: upvalue
                                     * index; GLOBAL: name-constant index;
                                     * PROPERTY: -1 (not meaningful). */
    int               isLocalUpvalue; /* UPVALUE only: 1 if the upvalue
                                     * captures an enclosing LOCAL, 0 if it
                                     * captures an enclosing UPVALUE. -1
                                     * for non-UPVALUE kinds. */
} ZymResolutionEntry;

typedef struct ZymResolutionTrace ZymResolutionTrace;

#ifndef ZYM_VM_FWD_DECLARED
#define ZYM_VM_FWD_DECLARED
typedef struct VM ZymVM;
#endif

/* Begin recording. Attaches a fresh trace buffer to `vm`. The next
 * `zym_compile` call on this VM populates it. Pairs with `zym_compilerTraceEnd`.
 * Returns 0 on success, non-zero on failure. */
int zym_compilerTraceBegin(ZymVM* vm);

/* Stop recording and detach the buffer. Returns the buffer for inspection;
 * caller owns it and must call `zym_compilerTraceFree`. Returns NULL if no
 * trace was active. */
ZymResolutionTrace* zym_compilerTraceEnd(ZymVM* vm);

/* Free a trace previously returned by `zym_compilerTraceEnd`. */
void zym_compilerTraceFree(ZymVM* vm, ZymResolutionTrace* trace);

/* Accessor API: count + indexed get. */
int zym_compilerTraceCount(const ZymResolutionTrace* trace);
int zym_compilerTraceAt(const ZymResolutionTrace* trace,
                        int                         index,
                        ZymResolutionEntry*         out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZYM_HAS_BUILD_TESTING */

#endif /* ZYM_COMPILER_TRACE_H */
