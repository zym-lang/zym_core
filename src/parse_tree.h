#pragma once

// Internal definition of the retained parse tree artifact.
//
// Phase 2 (LSP roadmap) — gated on ZYM_HAS_PARSE_TREE_RETENTION.
//
// Lifetime model (locked in roadmap §2 open questions):
//   - The tree *owns* the AST produced by `parse()` + the fileId it was
//     parsed under. The compiler *borrows* the tree during codegen and
//     never retains raw pointers past the compile call.
//   - On host `zymFreeParseTree`, the tree walks the AST via the existing
//     `free_stmt` machinery and releases the root `Stmt**` array.
//
// Implementation note: today's AST allocations still flow through the
// VM's tracked `ALLOCATE` / `GROW_ARRAY` helpers. A true block-bump arena
// is a later perf optimization orthogonal to the lifetime contract this
// type provides — every property Phase 2 needs (stable pointers into the
// tree, host-controlled lifetime, no consumer in the compiler holds a
// pointer past `zym_compile`) is already satisfied by deferring the
// `ast_free` call until the host drops the tree.

#include "./ast.h"
#include "./parser.h"
#include "./source_file.h"
#include "./trivia.h"

typedef struct VM VM;

struct ZymParseTree {
    AstResult     ast;       // owned — freed via free_stmt loop on destroy
    ZymFileId     file_id;   // which source file this tree was parsed from
    // Phase 2.3: comment side buffer (doc + plain comments). Owned by
    // the tree; NULL when the caller opted out of trivia capture or
    // PARSE_TREE_RETENTION is disabled at build time.
    TriviaBuffer* trivia;
};
typedef struct ZymParseTree ZymParseTree;

// Construct a heap-allocated ZymParseTree that owns `ast` and the
// optional trivia buffer. Passing NULL for `trivia` is valid — the
// tree is simply produced without a comment side buffer. The tree's
// lifetime is handed to the caller; `zym_freeParseTree` releases both
// the tree struct and every AST / trivia node it references.
ZymParseTree* parse_tree_new(VM* vm, AstResult ast, ZymFileId file_id,
                             TriviaBuffer* trivia);

// Free the tree and the AST it owns. Safe on NULL.
void parse_tree_free(VM* vm, ZymParseTree* tree);
