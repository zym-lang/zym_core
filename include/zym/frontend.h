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

// ---------------------------------------------------------------------
// Phase 3 — Compile modes.
//
// `ZymCompileMode` lets a host request less than a full compile. Today:
//
//   ZYM_COMPILE_EXECUTE    — today's behavior; produces bytecode into
//                            a caller-owned Chunk. Always available.
//                            Reach via zym_compile().
//   ZYM_COMPILE_PARSE_ONLY — scan + preprocess + parse. Produces a
//                            retained parse tree via out_tree; never
//                            touches codegen; never allocates a Chunk.
//                            Available only when
//                            ZYM_HAS_PARSE_TREE_RETENTION=1. Reach via
//                            zym_parseOnly().
//
// A future ZYM_COMPILE_CHECK mode (Phase 4) will additionally run the
// parallel resolver to populate a SymbolTable.
//
// The enum itself is always defined so host code can switch on it
// uniformly across profiles; the PARSE_ONLY *entry point*
// (`zym_parseOnly`, declared in `zym/zym.h`) is retention-gated.
// ---------------------------------------------------------------------
typedef enum {
    ZYM_COMPILE_EXECUTE = 0,
    ZYM_COMPILE_PARSE_ONLY = 1,
    ZYM_COMPILE_CHECK = 2  // Phase 4 — parse + parallel resolver (requires ZYM_HAS_SYMBOL_TABLE)
} ZymCompileMode;

#endif // ZYM_HAS_PARSE_TREE_RETENTION

// ---------------------------------------------------------------------
// Phase 4 — Symbol table and parallel resolver.
//
// The symbol table is produced by the resolver pass (Phase 4), which
// walks a retained parse tree and records one entry per declaration
// keyed by the declaring identifier's span. The resolver is a
// *parallel* pass — `compiler.c`'s existing name resolution is never
// rewired to consume this table.
//
// 4.1a scope (current): top-level declarations only (var / func /
// struct / enum). Scope chains, references, and closures arrive in
// 4.1b / 4.1c.
//
// Entire block is gated on ZYM_HAS_SYMBOL_TABLE; on MCU builds the
// types and functions below are not declared.
// ---------------------------------------------------------------------
#if ZYM_HAS_SYMBOL_TABLE

typedef enum {
    ZYM_SYMBOL_UNKNOWN = 0,
    ZYM_SYMBOL_VAR,     // top-level var declaration
    ZYM_SYMBOL_FUNC,    // function declaration (top-level or nested)
    ZYM_SYMBOL_STRUCT,
    ZYM_SYMBOL_ENUM,
    ZYM_SYMBOL_PARAM,   // function parameter (Phase 4.1b)
    ZYM_SYMBOL_LOCAL,   // block-scoped var declaration (Phase 4.1b)
    ZYM_SYMBOL_FIELD,   // struct field (Phase 4.1c) — parentIndex -> enclosing STRUCT
    ZYM_SYMBOL_VARIANT, // enum variant (Phase 4.1c) — parentIndex -> enclosing ENUM
    ZYM_SYMBOL_UPVALUE  // captured binding (Phase 4.1d) — parentIndex -> origin
                        // symbol (LOCAL / PARAM / another UPVALUE in the
                        // next enclosing function frame).
} ZymSymbolKind;

// A single resolved declaration, copied out of the symbol table by
// value. `name` points into the source file bytes registered with the
// VM and is NOT null-terminated; read at most `nameLength` bytes. The
// pointer is valid for as long as the originating SourceFile stays
// registered on the VM. May be NULL for synthetic / preprocessor-
// generated declarations.
typedef struct {
    ZymSymbolKind kind;
    const char*   name;
    int           nameLength;
    int           nameFileId;
    int           nameStartByte;
    ZymSpan       defSpan;
    int           scopeDepth;
    // Phase 4.1c: for FIELD/VARIANT kinds, index into the symbol table
    // of the enclosing STRUCT/ENUM declaration. -1 for all other kinds.
    int           parentIndex;
} ZymSymbolInfo;

// Opaque handle to a symbol table produced by `zym_check`. Owned by
// the caller; release via `zym_freeSymbolTable`.
typedef struct ZymSymbolTable ZymSymbolTable;

// Release a symbol table obtained from `zym_check`. Safe on NULL. Must
// be called on the same VM that produced the table.
void zym_freeSymbolTable(ZymVM* vm, ZymSymbolTable* table);

// Total number of symbols recorded in the table. At 4.1a this is the
// number of top-level declarations; later phases grow this to include
// locals, params, fields, and variants.
int  zym_symbolTableSymbolCount(const ZymSymbolTable* table);

// Fill `*out` with the i-th symbol. Returns false (leaving *out
// untouched) if `i` is out of range or `table`/`out` is NULL.
bool zym_symbolTableSymbolAt(const ZymSymbolTable* table, int i,
                             ZymSymbolInfo* out);

// A single identifier-use recorded by the resolver (Phase 4.1b). Each
// reference is bound (where possible) to the symbol it resolves to via
// `symbolIndex`. An index of -1 means the use did not resolve — either
// the name is undeclared, or it's a member access (`obj.field`) whose
// field name is not yet resolvable until Phase 4.1c supplies struct
// type information.
//
// `name` lifetime matches `ZymSymbolInfo.name` — borrowed from the
// SourceFile bytes, NOT null-terminated.
typedef struct {
    const char* name;
    int         nameLength;
    int         nameFileId;
    int         nameStartByte;
    ZymSpan     useSpan;
    int         symbolIndex;   // index into the symbol array, or -1 if unresolved
    int         isWrite;       // 1 for assignment targets (including ++/-- lvalues), 0 for reads
} ZymReferenceInfo;

// Total number of identifier-use references recorded by the resolver.
int  zym_symbolTableReferenceCount(const ZymSymbolTable* table);

// Fill `*out` with the i-th reference. Returns false (leaving *out
// untouched) if `i` is out of range or `table`/`out` is NULL.
bool zym_symbolTableReferenceAt(const ZymSymbolTable* table, int i,
                                ZymReferenceInfo* out);

// Find the symbol whose declaring identifier span covers
// `(fileId, byteOffset)` and fill `*out`. Returns false if no symbol
// matches. Useful for "go to definition on click" — the host passes
// the caret position and gets the enclosing declaration, if any.
bool zym_symbolTableFindSymbolAt(const ZymSymbolTable* table,
                                 int fileId, int byteOffset,
                                 ZymSymbolInfo* out);

// ---------------------------------------------------------------------
// Phase 4.3a — public query API.
//
// The four accessors below are thin filters over the data already
// populated by the resolver; they give LSP-style hosts the operations
// they actually want (hover, go-to-def, find-references, outline,
// doc-comment hover) without forcing each host to re-implement array
// filtering.
//
// Note: `zymSymbolDefinition` is intentionally NOT a separate
// function. Use `ZymSymbolInfo.defSpan` directly — every symbol
// already carries its def span.
// ---------------------------------------------------------------------

// Hit-test at a caret position. Matches either a declaring identifier
// span (the symbol's `nameStart..nameStart+nameLength`) or a reference
// use span (`useSpan`). Preferred over `zym_symbolTableFindSymbolAt`
// for hover / go-to-definition because it also resolves clicks on
// identifier *uses*, not just declarations. When the caret is on a
// reference whose `symbolIndex == -1` (unresolved — e.g. member of a
// dynamically-typed receiver), returns false.
//
// Fills `*out` with the resolved symbol and returns true on hit;
// returns false (leaving `*out` untouched) on miss.
bool zymSymbolAtPosition(const ZymSymbolTable* table,
                         int fileId, int byteOffset,
                         ZymSymbolInfo* out);

// Collect up to `max_out` references that resolve to `symbolIndex`
// (i.e. `ZymReferenceInfo.symbolIndex == symbolIndex`). Writes them to
// `out_buf[0..min(N, max_out))` and returns the total N (which may
// exceed `max_out`); callers that care about unbounded results
// re-invoke with a larger buffer.
//
// Returns -1 on bad inputs (NULL table, out-of-range symbolIndex).
int zymSymbolReferences(const ZymSymbolTable* table,
                        int symbolIndex,
                        ZymReferenceInfo* out_buf, int max_out);

// Collect the file-level outline for `fileId`: top-level declarations
// (kind ∈ {VAR, FUNC, STRUCT, ENUM}, `scopeDepth == 0`) whose
// declaring identifier lives in that file. Order matches table
// insertion order, which is source order for top-level declarations.
//
// Returns the total count; fills up to `max_out` entries. Returns -1
// on NULL table.
int zymListFileSymbols(const ZymSymbolTable* table,
                       int fileId,
                       ZymSymbolInfo* out_buf, int max_out);

// Which kind of trivia piece `zymSymbolDocumentation` returned.
// `ZYM_DOC_NONE` means no doc comment was found and `*out_text` /
// `*out_len` are left untouched.
typedef enum {
    ZYM_DOC_NONE  = 0,
    ZYM_DOC_LINE  = 1,  // one or more contiguous `///` lines
    ZYM_DOC_BLOCK = 2   // a single `/** ... */` block
} ZymDocumentationKind;

// Look up the doc-comment attached to `symbolIndex`: the trivia
// piece(s) immediately preceding the symbol's `defSpan.startByte` with
// only whitespace (spaces, tabs, newlines) between.
//
// For `///` line comments, a *run* of contiguous doc-line pieces
// (pieces whose end + whitespace is the next piece's start) is
// returned as a single span — the caller can slice the source bytes
// `(tree-registered-source)[*out_start .. *out_start + *out_length)`
// to recover the raw text. For `/** ... */` blocks, exactly one piece
// is returned.
//
// On no-match (no trivia buffer, no adjacent doc piece, mismatched
// file, or the gap between trivia and def contains non-whitespace):
// returns `ZYM_DOC_NONE` and leaves out-params untouched.
//
// Signature:
//   kind          - which flavour of doc comment, or ZYM_DOC_NONE.
//   *out_fileId   - the file the doc span lives in (same as the symbol's).
//   *out_startByte- byte offset into that file where the doc comment run begins.
//   *out_length   - byte length of the doc comment run (includes the
//                   leading `///` or `/**` markers; host may strip as
//                   appropriate for presentation).
ZymDocumentationKind zymSymbolDocumentation(const ZymSymbolTable* table,
                                            const ZymParseTree* tree,
                                            int symbolIndex,
                                            int* out_fileId,
                                            int* out_startByte,
                                            int* out_length);
// ---------------------------------------------------------------------
// Phase 4.3b — semantic tokens.
//
// Classified-token stream for an entire file: every scanner token
// (keyword, operator, literal, identifier, ...) plus every comment
// trivia piece, emitted in source byte order with its resolver-derived
// kind and modifier bitmask.
//
// Hosts consume this for semantic highlighting ("this identifier is a
// function / parameter / struct field"). The public API is byte- and
// line/column-based; hosts convert to LSP's delta-line / delta-start /
// relative-token-index form on their side.
// ---------------------------------------------------------------------
typedef enum {
    // Syntactic / non-identifier tokens.
    ZYM_SEMTOK_KEYWORD       = 0,  // var, func, if, while, return, and, or, ...
    ZYM_SEMTOK_OPERATOR      = 1,  // +, -, =, ==, <<, &=, ->, ...
    ZYM_SEMTOK_PUNCTUATION   = 2,  // (, ), {, }, [, ], :, ;, ,, ., ..., @
    ZYM_SEMTOK_NUMBER        = 3,  // numeric literal
    ZYM_SEMTOK_STRING        = 4,  // string literal
    ZYM_SEMTOK_BOOL_NULL     = 5,  // true / false / null
    ZYM_SEMTOK_COMMENT       = 6,  // // ...  or /* ... */
    ZYM_SEMTOK_DOC_COMMENT   = 7,  // /// ...  or /** ... */
    // Identifier tokens classified via the resolver's symbol table.
    // When an identifier cannot be bound (undeclared use, or a member
    // name with dynamic receiver), its kind is ZYM_SEMTOK_IDENTIFIER.
    ZYM_SEMTOK_FUNCTION      = 8,
    ZYM_SEMTOK_VARIABLE      = 9,  // top-level var
    ZYM_SEMTOK_PARAMETER     = 10,
    ZYM_SEMTOK_LOCAL         = 11,
    ZYM_SEMTOK_UPVALUE       = 12,
    ZYM_SEMTOK_STRUCT        = 13,
    ZYM_SEMTOK_ENUM          = 14,
    ZYM_SEMTOK_FIELD         = 15,
    ZYM_SEMTOK_VARIANT       = 16,
    ZYM_SEMTOK_IDENTIFIER    = 17  // unresolved / unknown
} ZymSemanticTokenKind;
// Bitmask modifiers on a semantic token. LSP semantic-token modifiers
// are a superset; hosts pick which bits to project into the LSP list.
#define ZYM_SEMTOK_MOD_NONE        0u
#define ZYM_SEMTOK_MOD_DECLARATION 1u  // token is the declaring identifier span of a symbol
#define ZYM_SEMTOK_MOD_WRITE       2u  // reference use recorded as an assignment target
typedef struct {
    int           fileId;
    int           startByte;
    int           length;
    int           startLine;
    int           startColumn;
    int           endLine;
    int           endColumn;
    ZymSemanticTokenKind kind;
    unsigned int  modifiers;   // bitmask of ZYM_SEMTOK_MOD_*
} ZymSemanticTokenInfo;
// Produce the semantic-token stream for `fileId`: re-scans the file's
// canonical byte buffer (from the VM's SourceFile registry) and
// classifies each token via the resolver's symbol table, interleaved
// with comment trivia pieces recorded on `tree`.
//
// Writes up to `max_out` entries into `out_buf` (may be NULL to query
// the total). Returns the total number of tokens produced; callers
// that care about unbounded results re-invoke with a larger buffer.
//
// Returns -1 on bad inputs (NULL vm/table/tree, unknown fileId).
int zymSemanticTokens(const ZymVM* vm,
                      const ZymSymbolTable* table,
                      const ZymParseTree* tree,
                      int fileId,
                      ZymSemanticTokenInfo* out_buf, int max_out);
#endif // ZYM_HAS_SYMBOL_TABLE

#ifdef __cplusplus
}
#endif

#endif // ZYM_FRONTEND_H
