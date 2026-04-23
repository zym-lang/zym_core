#pragma once

// Public forward declaration for the Zym source map type.
//
// The SourceMap records per-expanded-line origin information for a
// preprocessed buffer: given a byte offset into the expanded source
// the scanner walked, it answers "which file did this come from, and
// what range in that file does it correspond to?". Embedders that want
// to render diagnostics against the user-visible source (LSP, doc
// tooling, richer error UIs) consult the map through the frontend API
// that Phase 2 exposes.
//
// The concrete layout is private to zym_core; embedders only ever hold
// an opaque pointer.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SourceMap ZymSourceMap;

#ifndef ZYM_VM_FWD_DECLARED
#define ZYM_VM_FWD_DECLARED
typedef struct VM ZymVM;
#endif

// A handle identifying a single source buffer registered with the VM.
// -1 (ZYM_FILE_ID_INVALID) means "no file" — used when an embedder just
// wants the pre-1.1 behavior (no origin tracking).
typedef int ZymFileId;
#define ZYM_FILE_ID_INVALID ((ZymFileId)-1)

// Register a source buffer with the VM's file registry. `path` and
// `bytes` are borrowed — the caller must keep them alive until the
// compile using this id completes. Returns ZYM_FILE_ID_INVALID on
// allocation failure.
ZymFileId zym_registerSourceFile(ZymVM* vm, const char* path,
                                 const char* bytes, size_t length);

// Read-only view onto a previously registered source file. `path` and
// `bytes` are borrowed from the registry and remain valid until the
// corresponding compile completes (after which they may be reset).
typedef struct ZymSourceFileInfo {
    ZymFileId   id;
    const char* path;   // may be NULL for synthetic/unnamed buffers
    const char* bytes;  // may be NULL if the registry was reset
    size_t      length;
} ZymSourceFileInfo;

// Resolves `fileId` back to the registered buffer + path. Returns true on
// success; returns false (and leaves *out untouched) if the id is invalid
// or out of range. Primarily used by diagnostic renderers to fetch the
// source line for a caret display.
int zym_getSourceFile(ZymVM* vm, ZymFileId fileId, ZymSourceFileInfo* out);

// Lifecycle for an opaque SourceMap handle. A SourceMap is populated by
// `zym_preprocess()` and consumed by `zym_compile()`; embedders should
// pair each `zym_newSourceMap` with a `zym_freeSourceMap`.
ZymSourceMap* zym_newSourceMap(ZymVM* vm);
void zym_freeSourceMap(ZymVM* vm, ZymSourceMap* map);

#ifdef __cplusplus
}
#endif
