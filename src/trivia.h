#pragma once
// Trivia side buffer — Phase 2.3 of the LSP roadmap.
//
// Trivia (comments + whitespace) is *not* part of the parser's token
// stream. The scanner records each trivia piece into this side buffer,
// keyed by byte offset, and then skips past it exactly as before.
// Downstream tooling (hover, doc-comments, folding, semantic tokens)
// looks the buffer up by range.
//
// Whitespace is intentionally *not* recorded: it is trivially
// reconstructable from the gap between any two adjacent tokens' byte
// ranges, and capturing every space/tab/newline would dominate the
// memory footprint of a retained parse tree for no real LSP gain.
// Only comment trivia (including doc comments) is stored.
//
// Pieces are appended in byte-offset order (scanner walks forward),
// which makes range lookups a binary search over `pieces[]`.
//
// Lifetime: owned by `ZymParseTree`. Allocated via the VM's tracked
// reallocate(); freed when the parse tree is freed.

#include <stdbool.h>

#include "./source_file.h"

typedef struct VM VM;

typedef enum {
    TRIVIA_COMMENT_LINE = 0, // // ...  (regular line comment)
    TRIVIA_COMMENT_BLOCK,    // /* ... */
    TRIVIA_DOC_LINE,         // /// ... (doc line comment)
    TRIVIA_DOC_BLOCK         // /** ... */
} TriviaKind;

typedef struct {
    TriviaKind kind;
    ZymFileId  fileId;
    int        startByte;   // absolute offset into the scanned file's buffer
    int        length;      // in bytes
    int        startLine;   // 1-based, in the scanned (preprocessed) buffer
    int        startColumn; // 1-based UTF-8 byte column
} TriviaPiece;

typedef struct TriviaBuffer {
    TriviaPiece* pieces;
    int          count;
    int          capacity;
} TriviaBuffer;

void trivia_init(TriviaBuffer* tb);
void trivia_free(VM* vm, TriviaBuffer* tb);
void trivia_append(VM* vm, TriviaBuffer* tb, const TriviaPiece* piece);

// Return the index of the first piece whose startByte >= `byteOffset`,
// or `tb->count` if none. Binary search; O(log N).
int  trivia_lower_bound(const TriviaBuffer* tb, int byteOffset);

// Return the piece that covers `byteOffset` (i.e. startByte <= off <
// startByte + length), or NULL if none. Useful for hover.
const TriviaPiece* trivia_find_at(const TriviaBuffer* tb, int byteOffset);
