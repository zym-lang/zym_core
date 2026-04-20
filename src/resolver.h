#pragma once

// =============================================================================
// Zym parallel resolver — Phase 4 of the LSP roadmap.
// =============================================================================
//
// The resolver walks a retained `ZymParseTree` and produces a
// `SymbolTable` describing the declarations in that tree — keyed by the
// declaring identifier's byte span — along with a flat list of
// identifier-use references, each bound (where possible) to the index
// of the declaring symbol.
//
// It is a *parallel* pass: it never touches `compiler.c`'s
// bytecode-producing resolution logic, and `compiler.c` is never
// rewired to consume this table. The two resolution paths coexist;
// this one exists purely for tooling consumers (LSP, docs, outline,
// hover).
//
// Phase split (see `future/lsp_roadmap.md` §4):
//
//   4.1a — Top-level declarations only: var / func / struct / enum.
//   4.1b — THIS FILE. Lexical scopes + function params + local vars +
//          shadowing + reference lists for `VariableExpr`, `AssignExpr`
//          targets, `GetExpr` / `SetExpr` object walks. Nested func
//          decls record themselves but their bodies still participate
//          in the scope chain.
//   4.1c — Closures, struct fields, enum variants.
//
// Entire file is compiled in only when `ZYM_HAS_SYMBOL_TABLE=1`; on
// `mcu-min` the TU is not linked and the types below do not exist.
//
// =============================================================================

#include "./config.h"

#if ZYM_HAS_SYMBOL_TABLE

#include "./parse_tree.h"
#include "./source_file.h"

#include <zym/frontend.h>  // for ZymSpan

typedef struct VM VM;

// What kind of thing a symbol names. Matches the set of declarations
// the Phase 4.1b walker recognizes; struct fields and enum variants
// land in 4.1c.
typedef enum {
    SYMBOL_KIND_UNKNOWN = 0,
    SYMBOL_KIND_VAR,     // top-level var
    SYMBOL_KIND_FUNC,    // func declaration (top-level or nested)
    SYMBOL_KIND_STRUCT,
    SYMBOL_KIND_ENUM,
    SYMBOL_KIND_PARAM,   // function parameter (4.1b)
    SYMBOL_KIND_LOCAL,   // block-scoped var declaration (4.1b)
    SYMBOL_KIND_FIELD,   // struct field (4.1c) — parent_index -> enclosing STRUCT
    SYMBOL_KIND_VARIANT  // enum variant (4.1c) — parent_index -> enclosing ENUM
} SymbolKind;

// A single resolved declaration.
//
//   name_*      — byte span of the declaring identifier token (what the
//                 host clicks on in the editor).
//   def_span    — span of the entire declaration (decl + body if any).
//   scope_depth — 0 at top level; incremented per nested block scope.
//                 Function bodies, `if`/`while`/`do_while`/`for`
//                 bodies, explicit `{ ... }` blocks, and the for-init
//                 clause each push a scope.
//
// `name` points into the originating source buffer's byte range and is
// NOT null-terminated; use `name_length` to bound reads. The pointer
// is stable for the lifetime of the source file (registered with the
// VM's `SourceFileRegistry`, which owns the bytes).
typedef struct {
    SymbolKind kind;
    const char* name;       // borrowed from SourceFile bytes; NOT null-terminated
    int         name_length;
    ZymFileId   name_file_id;
    int         name_start_byte;
    ZymSpan     def_span;
    int         scope_depth;
    // Phase 4.1c: for FIELD/VARIANT symbols, index into `symbols` of
    // the enclosing STRUCT/ENUM declaration. -1 for all other kinds.
    // Hosts use this to walk struct→field / enum→variant edges.
    int         parent_index;
} Symbol;

// A single resolved (or attempted) identifier use.
//
//   use_span       — byte span of the identifier token at the use site.
//   symbol_index   — index into the table's `symbols` array of the
//                    declaration this use resolves to, or -1 when the
//                    use did not resolve (undeclared, or a member name
//                    like `GetExpr.name` where 4.1b does not yet know
//                    the struct type).
//   is_write       — true for assignment targets (`AssignExpr`) and
//                    compound ops; false for read uses. Useful for
//                    highlight-read vs highlight-write semantic tokens.
typedef struct {
    const char* name;
    int         name_length;
    ZymFileId   name_file_id;
    int         name_start_byte;
    ZymSpan     use_span;
    int         symbol_index;
    bool        is_write;
} Reference;

// The table owns its symbol and reference arrays. `name` pointers in
// both `Symbol` and `Reference` reference SourceFile bytes and remain
// valid for as long as the file stays registered on the VM.
//
// Design note: a dense array is fine for 4.1b — a symbol table per
// file is modest. A `(fileId, byteOffset)` hit-test index can be
// layered on top when an LSP consumer demands it.
struct ZymSymbolTable {
    Symbol*    symbols;
    int        count;
    int        capacity;
    Reference* references;
    int        ref_count;
    int        ref_capacity;
};
typedef struct ZymSymbolTable ZymSymbolTable;

// Allocate a new, empty symbol table. Caller owns the returned pointer
// and must release it via `symbol_table_free` (or the public
// `zym_freeSymbolTable`).
ZymSymbolTable* symbol_table_new(VM* vm);

// Release the table and its symbol/reference arrays. Safe on NULL.
void symbol_table_free(VM* vm, ZymSymbolTable* table);

// Walk `tree` and populate `table` with every declaration reachable
// from the top-level statement list (recursively into function bodies,
// blocks, and control-flow bodies), plus a reference entry for every
// identifier-use the walker recognizes.
//
// Idempotent per `(tree, table)` pair — calling it twice appends
// twice, so callers should resolve into a fresh table. Returns `true`
// on success; parse failures are caught upstream in `zym_check` before
// this runs.
bool resolver_resolve_top_level(VM* vm, const ZymParseTree* tree,
                                ZymSymbolTable* table);

#endif // ZYM_HAS_SYMBOL_TABLE
