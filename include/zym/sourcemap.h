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

// Lifecycle for an opaque SourceMap handle. A SourceMap is populated by
// `zym_preprocessEx()` and consumed by `zym_compileEx()`; embedders
// should pair each `zym_newSourceMap` with a `zym_freeSourceMap`.
ZymSourceMap* zym_newSourceMap(ZymVM* vm);
void zym_freeSourceMap(ZymVM* vm, ZymSourceMap* map);
