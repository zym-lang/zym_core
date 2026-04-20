#ifndef ZYM_FRONTEND_H
#define ZYM_FRONTEND_H

// =============================================================================
// Zym frontend surface — LSP / tooling types
// =============================================================================
//
// This header exposes the *tooling* surface of zym_core: types and
// functions that let a host (LSP, editor, doc generator) inspect the
// results of the frontend pipeline as data, not just as a side effect
// of bytecode production.
//
// Every type in this header is gated at compile time behind one of the
// ZYM_HAS_* feature flags declared in <zym/config.h>. On minimum-
// footprint (MCU) builds these flags are 0 and the corresponding types
// and functions are not defined, so referring to them from host code is
// a compile error — intentionally, to prevent accidental dependency on
// LSP surface from size-constrained targets.
//
// **Stability:** EXPERIMENTAL. The shape of these types and the names
// of these functions will change while the LSP surface is being built
// out. Promoted to stable (and version-bumped) when the full roadmap
// ships (see roadmap "Done criteria").
//
// =============================================================================

#include "zym/config.h"

#ifdef __cplusplus
extern "C" {
#endif

// VM forward decl — same pattern as zym/sourcemap.h to coexist with
// zym/zym.h regardless of inclusion order.
#ifndef ZYM_VM_FWD_DECLARED
#define ZYM_VM_FWD_DECLARED
typedef struct VM ZymVM;
#endif

// Opaque handle to a retained parse tree. The typedef is always
// declared — so the zym_compile() signature (which takes
// `ZymParseTree**`) compiles on every profile — but only produced by
// the core when ZYM_HAS_PARSE_TREE_RETENTION=1. On MCU builds,
// zym_compile ignores the out-parameter and leaves *out_tree = NULL.
//
// EXPERIMENTAL: future phases will add query/introspection APIs
// (zymTreeFindNodeAt, zymTreeListTopLevelDeclarations, ...) in this
// header. The struct definition intentionally remains opaque so those
// phases can evolve the internal layout without churning the ABI.
typedef struct ZymParseTree ZymParseTree;

// ---------------------------------------------------------------------
// Phase 2.4 — Reconstructed node spans.
//
// A ZymSpan is the canonical (fileId, byteStart, length) rectangle into
// the scanned source bytes of a syntactic element, plus a pre-computed
// (startLine, startColumn, endLine, endColumn) convenience view that
// matches the Phase 1.1 scanner conventions (1-based lines, 0-based
// UTF-8 byte columns, columns reset on '\n').
//
// A "degraded" span is reported when the node's bracketing tokens are
// entirely synthetic (an empty container, an all-preprocessor-
// generated subtree, etc.): in that case `fileId == -1`, `startByte ==
// -1`, `length == 0`, columns are -1 and the line numbers fall back to
// the Expr/Stmt `line` field. Callers should treat `fileId == -1` as
// "no byte range available".
//
// Spans are available on every profile where a parse tree can exist
// (i.e. whenever ZYM_HAS_PARSE_TREE_RETENTION=1); on MCU builds the
// accessors below are simply not declared.
// ---------------------------------------------------------------------
typedef struct {
    int fileId;
    int startByte;
    int length;
    int startLine;
    int startColumn;
    int endLine;
    int endColumn;
} ZymSpan;

#if ZYM_HAS_PARSE_TREE_RETENTION

// Release a parse tree obtained from `zym_compile`. Frees every AST
// node the tree owns. Safe on NULL. Must be called on the same VM
// that produced the tree.
void zym_freeParseTree(ZymVM* vm, ZymParseTree* tree);

// Number of top-level statements in `tree`. Returns 0 for a NULL tree
// or a tree that parsed to an empty root (e.g. whitespace-only file).
int  zym_parseTreeTopLevelCount(const ZymParseTree* tree);

// Fill `*out` with the reconstructed span of the i-th top-level
// statement. Returns false (and leaves *out untouched) when `i` is out
// of range or `tree`/`out` is NULL.
//
// This is the minimum public surface needed to exercise the Phase 2.4
// helper; the richer query API (zymTreeFindNodeAt, ...) lands in
// Phase 2.5 and will re-use the same ZymSpan type.
#include <stdbool.h>
bool zym_parseTreeTopLevelSpan(const ZymParseTree* tree, int i, ZymSpan* out);

// ---------------------------------------------------------------------
// Phase 2.3 — Comment trivia side buffer.
//
// Trivia is recorded by the scanner as a separate stream, keyed by byte
// offset into the scanned (preprocessed) source. The parser never sees
// trivia — the token stream it consumes is unchanged. Tooling consumers
// (hover, doc comments, folding, semantic tokens) read the buffer by
// range.
//
// Only comments are recorded. Whitespace is reconstructable from the
// gap between adjacent tokens' byte ranges and is intentionally *not*
// stored — it would dominate the retained-tree footprint for no real
// LSP gain.
//
// Doc-comment convention (locked, see roadmap 2.3):
//   `///`         → ZYM_TRIVIA_DOC_LINE
//   `//`          → ZYM_TRIVIA_COMMENT_LINE
//   `/** … */`    → ZYM_TRIVIA_DOC_BLOCK   (unless `/**/`)
//   `/* … */`     → ZYM_TRIVIA_COMMENT_BLOCK
// ---------------------------------------------------------------------

#include <stdbool.h>

typedef enum {
    ZYM_TRIVIA_COMMENT_LINE = 0,
    ZYM_TRIVIA_COMMENT_BLOCK,
    ZYM_TRIVIA_DOC_LINE,
    ZYM_TRIVIA_DOC_BLOCK
} ZymTriviaKind;

typedef struct {
    ZymTriviaKind kind;
    int           fileId;
    int           startByte;
    int           length;
    int           startLine;
    int           startColumn;
} ZymTrivia;

// Number of trivia pieces recorded for `tree`. Returns 0 when the tree
// has no trivia buffer attached.
int  zym_parseTreeTriviaCount(const ZymParseTree* tree);

// Read the i-th trivia piece into `*out`. Returns false (leaving *out
// untouched) if `i` is out of range.
bool zym_parseTreeTriviaAt(const ZymParseTree* tree, int i, ZymTrivia* out);

// Find the trivia piece that contains `byteOffset`. Returns false if
// none cover the offset. O(log N). Useful for hover-at-cursor.
bool zym_parseTreeTriviaFindAt(const ZymParseTree* tree,
                               int byteOffset, ZymTrivia* out);
// ---------------------------------------------------------------------
// Phase 2.5 — Query API over the retained parse tree.
//
// The goal is to let tooling (LSP breadcrumbs, folding, outline view,
// go-to-definition scaffolding) answer structural questions about the
// tree without walking internal AST types. Every query returns data by
// value through `ZymNodeInfo` — there is no opaque handle that could
// outlive the tree. Callers pass a fixed-size output buffer and the
// function reports how many entries were filled (capped at `max_out`).
//
// `ZymNodeKind` is a *statement-level* classification. Expression-level
// queries are not part of this phase — hover-on-expression and
// semantic tokens land with the symbol table (Phase 4).
//
// All queries operate on byte offsets into the parsed (preprocessed)
// source, matching the Phase 1.1 `ZymSpan` conventions.
// ---------------------------------------------------------------------
typedef enum {
    ZYM_NODE_UNKNOWN = 0,
    ZYM_NODE_VAR_DECL,
    ZYM_NODE_FUNC_DECL,
    ZYM_NODE_STRUCT_DECL,
    ZYM_NODE_ENUM_DECL,
    ZYM_NODE_BLOCK,
    ZYM_NODE_IF,
    ZYM_NODE_WHILE,
    ZYM_NODE_DO_WHILE,
    ZYM_NODE_FOR,
    ZYM_NODE_SWITCH,
    ZYM_NODE_RETURN,
    ZYM_NODE_BREAK,
    ZYM_NODE_CONTINUE,
    ZYM_NODE_EXPRESSION,
    ZYM_NODE_LABEL,
    ZYM_NODE_GOTO,
    ZYM_NODE_DIRECTIVE,
    ZYM_NODE_ERROR
} ZymNodeKind;
typedef struct {
    ZymNodeKind kind;
    ZymSpan     span;
    // Name span for declarations (func/struct/enum/label = the declared
    // identifier; var = the first declared variable). All three fields
    // are (-1, 0, -1) when the node has no associated name.
    int         nameFileId;
    int         nameStartByte;
    int         nameLength;
} ZymNodeInfo;
// Find the innermost statement whose reconstructed span contains
// `byteOffset`. Returns false (leaving *out untouched) when no
// top-level statement covers the offset or when `tree`/`out` is NULL.
//
// Byte offsets with `fileId == -1` in the resulting span come from
// fully synthetic subtrees — unusual for Phase 2 but possible once
// preprocessor-synthesized nodes exist.
bool zymTreeFindNodeAt(const ZymParseTree* tree, int byteOffset,
                       ZymNodeInfo* out);
// Fill `out[0..min(n, max_out))` with every top-level declaration in
// the tree (VAR / FUNC / STRUCT / ENUM). Returns the real count of
// declarations present (which may exceed `max_out` — the caller can
// re-invoke with a larger buffer).
int  zymTreeListTopLevelDeclarations(const ZymParseTree* tree,
                                     ZymNodeInfo* out, int max_out);
// Fill `out` with the chain of block-bearing statements enclosing
// `byteOffset`, outermost first. "Block-bearing" = statements an LSP
// would surface in a breadcrumb: BLOCK, IF, WHILE, DO_WHILE, FOR,
// FUNC_DECL, STRUCT_DECL, ENUM_DECL, SWITCH. Returns the real chain
// length.
int  zymTreeEnclosingBlocks(const ZymParseTree* tree, int byteOffset,
                            ZymNodeInfo* out, int max_out);
// Fill `out` with every foldable range in the tree — the spans of
// block-bearing statements as defined above. Order is a pre-order
// walk (parent before children). Returns the real total count.
int  zymTreeFoldingRanges(const ZymParseTree* tree,
                          ZymNodeInfo* out, int max_out);
#endif // ZYM_HAS_PARSE_TREE_RETENTION

#ifdef __cplusplus
}
#endif

#endif // ZYM_FRONTEND_H
