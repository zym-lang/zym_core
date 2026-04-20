#pragma once

// Phase 1.1 frontend hygiene: SourceFile registry and ZymFileId.
//
// A SourceFile names a canonical byte buffer that the scanner tokens'
// byte offsets refer to. The registry lives on the VM so that downstream
// tooling (parse tree, symbol table, diagnostics) can resolve a fileId
// back to its bytes and path.
//
// For 1.1 we register two files per compile: the original user source
// and the post-preprocess buffer that the scanner actually walks. Token
// fields key off the latter; byte-granular origin mapping to the former
// arrives in Phase 1.2 (SourceMap).

#include <stddef.h>

typedef struct VM VM;

typedef int ZymFileId;
#define ZYM_FILE_ID_INVALID ((ZymFileId)-1)

typedef struct SourceFile {
    ZymFileId id;
    const char* path;   // borrowed, may be NULL
    const char* bytes;  // borrowed, not NUL-owned by registry
    size_t length;
} SourceFile;

typedef struct SourceFileRegistry {
    SourceFile* files;
    int count;
    int capacity;
} SourceFileRegistry;

void sfr_init(SourceFileRegistry* reg);
void sfr_free(VM* vm, SourceFileRegistry* reg);

// Registers a new source file and returns its id. path/bytes are borrowed;
// the caller must keep them alive for as long as the fileId is referenced.
ZymFileId sfr_register(VM* vm, SourceFileRegistry* reg,
                       const char* path, const char* bytes, size_t length);

// Returns NULL if id is out of range.
const SourceFile* sfr_get(const SourceFileRegistry* reg, ZymFileId id);

// Resets the registry to empty without releasing capacity; used after a
// compile completes to drop borrowed pointers.
void sfr_reset(SourceFileRegistry* reg);
