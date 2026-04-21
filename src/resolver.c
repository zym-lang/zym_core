#include "./config.h"

#if ZYM_HAS_SYMBOL_TABLE

#include "./resolver.h"
#include "./ast.h"
#include "./memory.h"
#include "./node_span.h"
#include "./object.h"
#include "./value.h"
#include "./vm.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// =============================================================================
// Phase 4.1b — scope-aware resolver.
//
// Data structures:
//   - ZymSymbolTable (in resolver.h) — dense array of declarations
//     (Symbol) + dense array of identifier uses (Reference).
//   - ScopeStack (local to this file) — the currently-open scopes
//     during the walk. Each scope records the base index into
//     `table->symbols` where its members start; a lookup walks the
//     stack innermost-first and scans [base, count) for a name match.
//     Top-level (scope_depth 0) is implicit: any symbol with
//     scope_depth 0 is visible everywhere unless shadowed by a
//     deeper scope's entry.
//
// Scope push/pop sites:
//   - Function body (params pushed into the body scope).
//   - Explicit { ... } block statement.
//   - If / while / do-while / for body (each body is a scope).
//   - For-init clause (a scope that wraps the whole for: init
//     vars are visible in the condition, increment, and body).
//   - Switch body — NOT a scope today (case clauses share a scope).
//
// Unresolved references (including member names in GetExpr/SetExpr —
// 4.1c's job) are recorded with symbol_index = -1 so tooling can still
// highlight them.
// =============================================================================

// -----------------------------------------------------------------------------
// Table lifecycle
// -----------------------------------------------------------------------------

ZymSymbolTable* symbol_table_new(VM* vm)
{
    ZymSymbolTable* t = ALLOCATE(vm, ZymSymbolTable, 1);
    t->symbols      = NULL;
    t->count        = 0;
    t->capacity     = 0;
    t->references   = NULL;
    t->ref_count    = 0;
    t->ref_capacity = 0;
    return t;
}

void symbol_table_free(VM* vm, ZymSymbolTable* table)
{
    if (table == NULL) return;
    if (table->symbols != NULL) {
        FREE_ARRAY(vm, Symbol, table->symbols, table->capacity);
    }
    if (table->references != NULL) {
        FREE_ARRAY(vm, Reference, table->references, table->ref_capacity);
    }
    FREE(vm, ZymSymbolTable, table);
}

static int push_symbol(VM* vm, ZymSymbolTable* table, Symbol sym)
{
    if (table->count + 1 > table->capacity) {
        int old_cap = table->capacity;
        int new_cap = GROW_CAPACITY(old_cap);
        table->symbols = GROW_ARRAY(vm, Symbol, table->symbols, old_cap, new_cap);
        table->capacity = new_cap;
    }
    int idx = table->count;
    table->symbols[table->count++] = sym;
    return idx;
}

static void push_reference(VM* vm, ZymSymbolTable* table, Reference ref)
{
    if (table->ref_count + 1 > table->ref_capacity) {
        int old_cap = table->ref_capacity;
        int new_cap = GROW_CAPACITY(old_cap);
        table->references = GROW_ARRAY(vm, Reference, table->references, old_cap, new_cap);
        table->ref_capacity = new_cap;
    }
    table->references[table->ref_count++] = ref;
}

// -----------------------------------------------------------------------------
// Name span -> borrowed byte pointer into the SourceFile registry.
// Returns NULL when the token has no file-backed span.
// -----------------------------------------------------------------------------
static const char* lookup_name_bytes(VM* vm, const Token* nm)
{
    if (nm == NULL || nm->length <= 0) return NULL;
    if (nm->fileId == ZYM_FILE_ID_INVALID || nm->startByte < 0) return NULL;
    const SourceFile* sf = sfr_get(&vm->source_files, nm->fileId);
    if (sf == NULL || sf->bytes == NULL) return NULL;
    if ((size_t)nm->startByte + (size_t)nm->length > sf->length) return NULL;
    return sf->bytes + nm->startByte;
}

// -----------------------------------------------------------------------------
// Scope stack — tracks where each scope's members start in
// table->symbols. scope_depth is indexed from 0 (top-level).
// -----------------------------------------------------------------------------
#define MAX_SCOPES 128

typedef struct {
    int  base;    // index into table->symbols of the first symbol in this scope
    int  depth;   // logical depth (top-level == 0)
    bool is_function_frame; // 4.1d: push at func decl / anon-func body
    // 4.1d: per-function-frame dedup map. A use inside this frame that
    // captures an outer binding synthesizes one UPVALUE symbol; every
    // subsequent capture of the same origin reuses it. Dynamically
    // allocated for function frames; NULL for plain block scopes.
    int* upvalue_origins;    // origin symbol indices (LOCAL/PARAM/UPVALUE)
    int* upvalue_symbols;    // corresponding UPVALUE symbol indices
    int  upvalue_count;
    int  upvalue_capacity;
    // Phase 4: mirrors compiler.c's per-block func pre-declaration
    // (see the BLOCK handler in compile_statement_impl that walks its
    // direct `STMT_FUNC_DECLARATION` children before compiling them).
    // When a scope is pushed for a BLOCK / loop body, we pre-emit the
    // `SYMBOL_KIND_FUNC` entries for every direct `func` child into
    // that scope, then set this flag. walk_stmt's STMT_FUNC_DECLARATION
    // case then skips the redundant emit, leaving body-walk behaviour
    // (param emit, body traversal) intact. This gives block-local
    // hoisting (e.g. `helper()` called above `func helper() {...}`
    // inside a for-loop body) without scoping leakage into the
    // enclosing function frame.
    bool funcs_hoisted;
} Scope;

typedef struct {
    VM*              vm;
    ZymSymbolTable*  table;
    Scope            scopes[MAX_SCOPES];
    int              scope_count;
    // `visible_end` is the index one-past-the-last currently-visible
    // symbol. Grows when a new decl is emitted; rolls back to the
    // popped scope's base on ctx_pop_scope. Decoupled from
    // `table->count` so that popped-but-retained symbols (the table
    // never deletes) stop participating in lookups.
    int              visible_end;
    // Phase 4: two-pass top-level. Mirrors compiler.c's Pass 1 hoist of
    // `FUNC`/`STRUCT`/`ENUM` declarations so that forward references
    // inside function bodies (e.g. a call to `hoisted_factory(...)`
    // above its declaration) bind to the real symbol instead of
    // silently dropping to -1. The pre-pass sets this flag, then the
    // full walk guards against re-emitting the same top-level
    // declaration when `ctx_depth(ctx) == 0`.
    bool             top_level_hoisted;
} Ctx;

static void ctx_init(Ctx* ctx, VM* vm, ZymSymbolTable* table)
{
    ctx->vm            = vm;
    ctx->table         = table;
    ctx->scope_count   = 1;
    ctx->scopes[0].base              = 0;
    ctx->scopes[0].depth             = 0;
    ctx->scopes[0].is_function_frame = false;
    ctx->scopes[0].upvalue_origins   = NULL;
    ctx->scopes[0].upvalue_symbols   = NULL;
    ctx->scopes[0].upvalue_count     = 0;
    ctx->scopes[0].upvalue_capacity  = 0;
    ctx->scopes[0].funcs_hoisted     = false;
    ctx->visible_end       = 0;
    ctx->top_level_hoisted = false;
}

static int ctx_depth(const Ctx* ctx)
{
    return ctx->scopes[ctx->scope_count - 1].depth;
}

// 4.1d: index in the scopes[] array of the innermost function frame, or
// -1 when the current scope is not inside any function (top-level).
static int ctx_innermost_function_scope(const Ctx* ctx)
{
    for (int s = ctx->scope_count - 1; s >= 0; s--) {
        if (ctx->scopes[s].is_function_frame) return s;
    }
    return -1;
}

static void ctx_push_scope_ex(Ctx* ctx, bool is_function_frame)
{
    if (ctx->scope_count >= MAX_SCOPES) return;  // overflow: flatten into outermost
    int base = ctx->visible_end;
    int depth = ctx->scopes[ctx->scope_count - 1].depth + 1;
    Scope* sc = &ctx->scopes[ctx->scope_count];
    sc->base              = base;
    sc->depth             = depth;
    sc->is_function_frame = is_function_frame;
    sc->upvalue_origins   = NULL;
    sc->upvalue_symbols   = NULL;
    sc->upvalue_count     = 0;
    sc->upvalue_capacity  = 0;
    sc->funcs_hoisted     = false;
    ctx->scope_count++;
}

static void ctx_push_scope(Ctx* ctx)
{
    ctx_push_scope_ex(ctx, /*is_function_frame=*/false);
}

static void ctx_push_function_frame(Ctx* ctx)
{
    ctx_push_scope_ex(ctx, /*is_function_frame=*/true);
}

static void ctx_pop_scope(Ctx* ctx)
{
    if (ctx->scope_count <= 1) return;
    // Roll visibility back so symbols declared in the popped scope are
    // no longer findable by ctx_lookup. The symbols remain in
    // `table->symbols` for consumers to read out after the walk.
    Scope* sc = &ctx->scopes[ctx->scope_count - 1];
    if (sc->is_function_frame && sc->upvalue_origins != NULL) {
        FREE_ARRAY(ctx->vm, int, sc->upvalue_origins, sc->upvalue_capacity);
        FREE_ARRAY(ctx->vm, int, sc->upvalue_symbols, sc->upvalue_capacity);
        sc->upvalue_origins  = NULL;
        sc->upvalue_symbols  = NULL;
    }
    ctx->visible_end = sc->base;
    ctx->scope_count--;
}

// Innermost-first lookup for an identifier-use: walks scopes from the
// deepest open scope down to the top-level and scans for a symbol
// whose `name` matches `nm`. Returns the symbol index, or -1 if no
// match is in scope. When `out_scope_idx` is non-NULL, also yields
// the scopes[] index where the symbol was found (needed by 4.1d's
// capture logic to count crossed function frames).
static int ctx_lookup_ex(const Ctx* ctx, const Token* nm, int* out_scope_idx)
{
    if (out_scope_idx) *out_scope_idx = -1;
    if (nm == NULL || nm->length <= 0) return -1;
    const char* nb = lookup_name_bytes(ctx->vm, nm);
    if (nb == NULL) return -1;
    int need = nm->length;

    const ZymSymbolTable* table = ctx->table;
    for (int s = ctx->scope_count - 1; s >= 0; s--) {
        int base = ctx->scopes[s].base;
        int end  = (s + 1 < ctx->scope_count) ? ctx->scopes[s + 1].base
                                              : ctx->visible_end;
        int scope_depth = ctx->scopes[s].depth;
        // innermost-first within a scope: iterate in reverse so the
        // most-recently declared symbol wins on redeclaration.
        for (int i = end - 1; i >= base; i--) {
            const Symbol* sy = &table->symbols[i];
            // Skip struct fields / enum variants: those are looked up
            // through `find_child_by_name(parent, name)` on member
            // access, never by a bare identifier. Including them in
            // the plain scope walk would let e.g. `x` resolve to a
            // `Point.x` field declared at the same scope depth.
            // Skip UPVALUE symbols too: they are consumed via the
            // per-frame dedup map + `parent_index`, never by bare-name
            // lookup. Leaving them visible here would let an outer-
            // scope use site re-resolve to a stale UPVALUE synthesized
            // by a now-popped inner function frame (e.g. writing
            // `a = 10` in the outer function AFTER an inner closure
            // captured `a`).
            if (sy->kind == SYMBOL_KIND_FIELD ||
                sy->kind == SYMBOL_KIND_VARIANT ||
                sy->kind == SYMBOL_KIND_UPVALUE) continue;
            // Phase 4.5c: symbols beyond the current scope's live range
            // but still occupying indices < visible_end are orphans
            // from already-popped deeper scopes (the table retains
            // every symbol for host inspection; we never compact). An
            // orphan has scope_depth greater than this scope's own
            // depth — skip it so its popped declaration can't be
            // re-exposed when a later outer-scope emit bumps
            // visible_end past it.
            if (sy->scope_depth > scope_depth) continue;
            if (sy->name_length == need && sy->name != NULL &&
                memcmp(sy->name, nb, (size_t)need) == 0) {
                if (out_scope_idx) *out_scope_idx = s;
                return i;
            }
        }
    }
    return -1;
}

static int ctx_lookup(const Ctx* ctx, const Token* nm)
{
    return ctx_lookup_ex(ctx, nm, NULL);
}

// -----------------------------------------------------------------------------
// Declaration emission.
// -----------------------------------------------------------------------------
static int emit_decl(Ctx* ctx, SymbolKind kind, const Token* nm, const Stmt* decl)
{
    if (nm == NULL || nm->length <= 0) return -1;  // synthetic decl
    Symbol s;
    s.kind            = kind;
    s.name            = lookup_name_bytes(ctx->vm, nm);
    s.name_length     = nm->length;
    s.name_file_id    = nm->fileId;
    s.name_start_byte = nm->startByte;
    s.def_span        = nodeSpanOfStmt(decl);
    s.scope_depth     = ctx_depth(ctx);
    s.parent_index    = -1;
    int idx = push_symbol(ctx->vm, ctx->table, s);
    ctx->visible_end = ctx->table->count;
    return idx;
}

// Used for params, which don't have a Stmt wrapper — synthesize a
// span from the name token alone.
static int emit_param(Ctx* ctx, const Token* nm)
{
    if (nm == NULL || nm->length <= 0) return -1;
    Symbol s;
    s.kind                  = SYMBOL_KIND_PARAM;
    s.name                  = lookup_name_bytes(ctx->vm, nm);
    s.name_length           = nm->length;
    s.name_file_id          = nm->fileId;
    s.name_start_byte       = nm->startByte;
    s.def_span.fileId       = nm->fileId;
    s.def_span.startByte    = nm->startByte;
    s.def_span.length       = nm->length;
    s.def_span.startLine    = nm->startLine;
    s.def_span.startColumn  = nm->startColumn;
    s.def_span.endLine      = nm->endLine;
    s.def_span.endColumn    = nm->endColumn;
    s.scope_depth           = ctx_depth(ctx);
    s.parent_index          = -1;
    int idx = push_symbol(ctx->vm, ctx->table, s);
    ctx->visible_end = ctx->table->count;
    return idx;
}

// 4.1d: emit a new UPVALUE symbol into the given function frame scope,
// with parent_index linking to the origin (either the ultimate
// LOCAL/PARAM declaration, or the previous-level UPVALUE for transitive
// captures). Does NOT bump visible_end — upvalues are resolved via the
// scope's dedup map, not through name-walks. scope_depth is set to the
// function frame's depth so hosts can tell which function owns the
// capture.
static int emit_upvalue_into_scope(Ctx* ctx, int frame_scope_idx,
                                   const Token* nm, int parent_origin_idx)
{
    if (nm == NULL || nm->length <= 0) return -1;
    Symbol s;
    s.kind                  = SYMBOL_KIND_UPVALUE;
    s.name                  = lookup_name_bytes(ctx->vm, nm);
    s.name_length           = nm->length;
    s.name_file_id          = nm->fileId;
    s.name_start_byte       = nm->startByte;
    s.def_span.fileId       = nm->fileId;
    s.def_span.startByte    = nm->startByte;
    s.def_span.length       = nm->length;
    s.def_span.startLine    = nm->startLine;
    s.def_span.startColumn  = nm->startColumn;
    s.def_span.endLine      = nm->endLine;
    s.def_span.endColumn    = nm->endColumn;
    s.scope_depth           = ctx->scopes[frame_scope_idx].depth;
    s.parent_index          = parent_origin_idx;
    return push_symbol(ctx->vm, ctx->table, s);
}

// 4.1d: look in frame scope's dedup map for an existing upvalue whose
// parent_index matches the given origin. Returns the upvalue symbol idx
// or -1 if not yet synthesized.
static int find_upvalue_in_frame(const Ctx* ctx, int frame_scope_idx, int origin_idx)
{
    const Scope* sc = &ctx->scopes[frame_scope_idx];
    for (int i = 0; i < sc->upvalue_count; i++) {
        if (sc->upvalue_origins[i] == origin_idx) {
            return sc->upvalue_symbols[i];
        }
    }
    return -1;
}

// 4.1d: record the (origin -> upvalue) mapping in a function frame's
// dedup map. Grows the map on demand via the VM allocator.
static void record_upvalue_in_frame(Ctx* ctx, int frame_scope_idx,
                                    int origin_idx, int upvalue_idx)
{
    Scope* sc = &ctx->scopes[frame_scope_idx];
    if (sc->upvalue_count + 1 > sc->upvalue_capacity) {
        int old_cap = sc->upvalue_capacity;
        int new_cap = (old_cap < 4) ? 4 : old_cap * 2;
        sc->upvalue_origins = GROW_ARRAY(ctx->vm, int, sc->upvalue_origins, old_cap, new_cap);
        sc->upvalue_symbols = GROW_ARRAY(ctx->vm, int, sc->upvalue_symbols, old_cap, new_cap);
        sc->upvalue_capacity = new_cap;
    }
    sc->upvalue_origins[sc->upvalue_count] = origin_idx;
    sc->upvalue_symbols[sc->upvalue_count] = upvalue_idx;
    sc->upvalue_count++;
}

// 4.1d: given an origin lookup that crossed function frames, synthesize
// (or reuse) an UPVALUE symbol in each crossed function frame, chaining
// parent_index from the origin outward to the innermost use site.
// Mirrors `compiler.c`'s recursive `resolve_upvalue`: the outermost
// crossed frame captures the origin directly (is_local=true); each
// inner frame re-captures the previous frame's upvalue
// (is_local=false). Returns the innermost upvalue symbol index that
// the use site should bind to; returns `origin_idx` unchanged when no
// function frames were crossed.
//
// `origin_scope_idx` is the scope where the origin lives;
// `use_scope_idx` is the current (innermost) scope at the use site.
static int synthesize_upvalue_chain(Ctx* ctx, const Token* nm,
                                    int origin_idx,
                                    int origin_scope_idx,
                                    int use_scope_idx)
{
    if (origin_idx < 0) return -1;
    // Collect the function-frame scopes strictly between origin and use
    // (i.e. scopes s where origin_scope_idx < s <= use_scope_idx AND
    // s is a function frame), outermost-first.
    int frames[MAX_SCOPES];
    int frame_count = 0;
    for (int s = origin_scope_idx + 1; s <= use_scope_idx; s++) {
        if (ctx->scopes[s].is_function_frame) {
            if (frame_count < MAX_SCOPES) frames[frame_count++] = s;
        }
    }
    if (frame_count == 0) return origin_idx;  // no capture needed

    // Only capturable kinds traverse the upvalue chain. Globals
    // (VAR/FUNC/STRUCT/ENUM at scope_depth 0) and nested func names
    // are resolved through ordinary name lookup at the use site;
    // compiler.c would fall through to global resolution for them.
    const Symbol* origin = &ctx->table->symbols[origin_idx];
    if (origin->kind != SYMBOL_KIND_LOCAL &&
        origin->kind != SYMBOL_KIND_PARAM &&
        origin->kind != SYMBOL_KIND_UPVALUE) {
        return origin_idx;
    }

    int prev_idx = origin_idx;
    for (int i = 0; i < frame_count; i++) {
        int frame_sc = frames[i];
        int existing = find_upvalue_in_frame(ctx, frame_sc, prev_idx);
        if (existing >= 0) {
            prev_idx = existing;
            continue;
        }
        int upv = emit_upvalue_into_scope(ctx, frame_sc, nm, prev_idx);
        if (upv < 0) return origin_idx;  // synthesis failure: fall back
        record_upvalue_in_frame(ctx, frame_sc, prev_idx, upv);
        prev_idx = upv;
    }
    return prev_idx;
}

// 4.1d: name resolution for a capturable use site. Looks up the name,
// and if it crossed function frames to reach a capturable origin,
// synthesizes/reuses the upvalue chain and returns the innermost
// upvalue symbol index. Returns the origin directly when no capture
// is needed (same-frame use, or origin is a global/struct/enum/func).
static int resolve_name_with_capture(Ctx* ctx, const Token* nm)
{
    int origin_scope = -1;
    int origin_idx = ctx_lookup_ex(ctx, nm, &origin_scope);
    if (origin_idx < 0) return -1;
    int use_scope = ctx->scope_count - 1;
    return synthesize_upvalue_chain(ctx, nm, origin_idx, origin_scope, use_scope);
}

// Phase 4.1c: emit one FIELD or VARIANT child symbol. Children are
// recorded with scope_depth matching the parent (they don't push a
// scope — they're looked up through parent_index, not via the scope
// stack) and link back to the parent via parent_index. They do NOT
// bump ctx->visible_end so ordinary name lookups won't resolve
// `x` to a struct field when `Point.x` is what was intended; field
// resolution goes through `find_child_by_name`.
static int emit_child(Ctx* ctx, SymbolKind kind, const Token* nm, int parent_idx)
{
    if (nm == NULL || nm->length <= 0) return -1;
    Symbol s;
    s.kind                  = kind;
    s.name                  = lookup_name_bytes(ctx->vm, nm);
    s.name_length           = nm->length;
    s.name_file_id          = nm->fileId;
    s.name_start_byte       = nm->startByte;
    s.def_span.fileId       = nm->fileId;
    s.def_span.startByte    = nm->startByte;
    s.def_span.length       = nm->length;
    s.def_span.startLine    = nm->startLine;
    s.def_span.startColumn  = nm->startColumn;
    s.def_span.endLine      = nm->endLine;
    s.def_span.endColumn    = nm->endColumn;
    s.scope_depth           = ctx_depth(ctx);
    s.parent_index          = parent_idx;
    // Children are appended to `symbols` but NOT made visible through
    // the scope stack (we do not advance visible_end). Member access
    // resolves via find_child_by_name(parent_idx, ...) below.
    int idx = push_symbol(ctx->vm, ctx->table, s);
    return idx;
}

// Scan `symbols[]` for a child whose `parent_index == parent_idx` and
// whose name matches `nm`. Returns the child's symbol index or -1.
// O(total_symbols); acceptable today (tables are small) and trivially
// replaceable with a parent→children side index later.
static int find_child_by_name(const Ctx* ctx, int parent_idx, const Token* nm)
{
    if (parent_idx < 0 || nm == NULL || nm->length <= 0) return -1;
    const char* nb = lookup_name_bytes(ctx->vm, nm);
    if (nb == NULL) return -1;
    int need = nm->length;
    const ZymSymbolTable* table = ctx->table;
    for (int i = 0; i < table->count; i++) {
        const Symbol* sy = &table->symbols[i];
        if (sy->parent_index != parent_idx) continue;
        if (sy->name_length == need && sy->name != NULL &&
            memcmp(sy->name, nb, (size_t)need) == 0) {
            return i;
        }
    }
    return -1;
}

// -----------------------------------------------------------------------------
// Reference recording.
// -----------------------------------------------------------------------------
static void record_ref(Ctx* ctx, const Token* nm, bool is_write, int resolved_index)
{
    if (nm == NULL || nm->length <= 0) return;
    Reference r;
    r.name            = lookup_name_bytes(ctx->vm, nm);
    r.name_length     = nm->length;
    r.name_file_id    = nm->fileId;
    r.name_start_byte = nm->startByte;
    r.use_span.fileId      = nm->fileId;
    r.use_span.startByte   = nm->startByte;
    r.use_span.length      = nm->length;
    r.use_span.startLine   = nm->startLine;
    r.use_span.startColumn = nm->startColumn;
    r.use_span.endLine     = nm->endLine;
    r.use_span.endColumn   = nm->endColumn;
    r.symbol_index    = resolved_index;
    r.is_write        = is_write;
    push_reference(ctx->vm, ctx->table, r);
}

// -----------------------------------------------------------------------------
// Expression and statement walkers.
// -----------------------------------------------------------------------------
static void walk_expr(Ctx* ctx, const Expr* e);
static void walk_stmt(Ctx* ctx, const Stmt* s);
static void hoist_block_funcs(Ctx* ctx, Stmt** statements, int count);

// Phase 4.1c: resolve a member-access name (GetExpr.name / SetExpr.name)
// when the receiver is a direct VariableExpr whose name resolves to a
// STRUCT (field lookup) or ENUM (variant lookup). Returns the child
// symbol index or -1 if the receiver is dynamic / unresolved / not a
// type-bearing kind.
static int resolve_member_name(Ctx* ctx, const Expr* object, const Token* member)
{
    if (object == NULL || object->type != EXPR_VARIABLE) return -1;
    int recv = ctx_lookup(ctx, &object->as.variable.name);
    if (recv < 0) return -1;
    SymbolKind rk = ctx->table->symbols[recv].kind;
    if (rk != SYMBOL_KIND_STRUCT && rk != SYMBOL_KIND_ENUM) return -1;
    return find_child_by_name(ctx, recv, member);
}

// AssignExpr.target is an Expr*; recognize the common shapes for
// binding purposes: a VariableExpr means a write-ref to a name; a
// SetExpr.object/GetExpr chain means a write to a field. Field names
// are resolved via `resolve_member_name` (Phase 4.1c).
static void walk_assign_target(Ctx* ctx, const Expr* target)
{
    if (target == NULL) return;
    switch (target->type) {
        case EXPR_VARIABLE: {
            const Token* nm = &target->as.variable.name;
            int idx = resolve_name_with_capture(ctx, nm);
            record_ref(ctx, nm, /*is_write=*/true, idx);
            return;
        }
        case EXPR_GET: {
            walk_expr(ctx, target->as.get.object);
            int midx = resolve_member_name(ctx, target->as.get.object,
                                           &target->as.get.name);
            record_ref(ctx, &target->as.get.name, /*is_write=*/true, midx);
            return;
        }
        case EXPR_SUBSCRIPT:
            walk_expr(ctx, target->as.subscript.object);
            walk_expr(ctx, target->as.subscript.index);
            return;
        default:
            walk_expr(ctx, target);
            return;
    }
}

static void walk_expr(Ctx* ctx, const Expr* e)
{
    if (e == NULL) return;
    switch (e->type) {
        case EXPR_VARIABLE: {
            const Token* nm = &e->as.variable.name;
            int idx = resolve_name_with_capture(ctx, nm);
            record_ref(ctx, nm, /*is_write=*/false, idx);
            break;
        }
        case EXPR_ASSIGN:
            walk_assign_target(ctx, e->as.assign.target);
            walk_expr(ctx, e->as.assign.value);
            break;
        case EXPR_BINARY:
            walk_expr(ctx, e->as.binary.left);
            walk_expr(ctx, e->as.binary.right);
            break;
        case EXPR_CALL:
            walk_expr(ctx, e->as.call.callee);
            for (int i = 0; i < e->as.call.arg_count; i++) {
                walk_expr(ctx, e->as.call.args[i]);
            }
            break;
        case EXPR_GET: {
            walk_expr(ctx, e->as.get.object);
            int midx = resolve_member_name(ctx, e->as.get.object, &e->as.get.name);
            record_ref(ctx, &e->as.get.name, /*is_write=*/false, midx);
            break;
        }
        case EXPR_SET: {
            walk_expr(ctx, e->as.set.object);
            int midx = resolve_member_name(ctx, e->as.set.object, &e->as.set.name);
            record_ref(ctx, &e->as.set.name, /*is_write=*/true, midx);
            walk_expr(ctx, e->as.set.value);
            break;
        }
        case EXPR_UNARY:
            walk_expr(ctx, e->as.unary.right);
            break;
        case EXPR_GROUPING:
            walk_expr(ctx, e->as.grouping.expression);
            break;
        case EXPR_LIST:
            for (int i = 0; i < e->as.list.count; i++) {
                walk_expr(ctx, e->as.list.elements[i]);
            }
            break;
        case EXPR_SUBSCRIPT:
            walk_expr(ctx, e->as.subscript.object);
            walk_expr(ctx, e->as.subscript.index);
            break;
        case EXPR_MAP:
            for (int i = 0; i < e->as.map.count; i++) {
                walk_expr(ctx, e->as.map.keys[i]);
                walk_expr(ctx, e->as.map.values[i]);
            }
            break;
        case EXPR_FUNCTION: {
            // Anonymous function — a real function frame so outer
            // locals captured here become UPVALUE symbols (Phase 4.1d).
            ctx_push_function_frame(ctx);
            for (int i = 0; i < e->as.function.param_count; i++) {
                emit_param(ctx, &e->as.function.params[i].name);
            }
            // Body is a STMT_BLOCK whose scope-push handler pre-hoists
            // direct `func` children, so forward calls to nested funcs
            // inside the body resolve correctly.
            walk_stmt(ctx, e->as.function.body);
            ctx_pop_scope(ctx);
            break;
        }
        case EXPR_STRUCT_INST: {
            // struct_name is a type reference; when it resolves to a
            // STRUCT symbol, bind each field_names[i] to the struct's
            // corresponding field (Phase 4.1c).
            int struct_idx = ctx_lookup(ctx, &e->as.struct_inst.struct_name);
            record_ref(ctx, &e->as.struct_inst.struct_name,
                       /*is_write=*/false, struct_idx);
            bool receiver_is_struct =
                (struct_idx >= 0 &&
                 ctx->table->symbols[struct_idx].kind == SYMBOL_KIND_STRUCT);
            for (int i = 0; i < e->as.struct_inst.field_count; i++) {
                int fidx = receiver_is_struct
                    ? find_child_by_name(ctx, struct_idx,
                                         &e->as.struct_inst.field_names[i])
                    : -1;
                record_ref(ctx, &e->as.struct_inst.field_names[i],
                           /*is_write=*/false, fidx);
                walk_expr(ctx, e->as.struct_inst.field_values[i]);
            }
            break;
        }
        case EXPR_TERNARY:
            walk_expr(ctx, e->as.ternary.condition);
            walk_expr(ctx, e->as.ternary.then_expr);
            walk_expr(ctx, e->as.ternary.else_expr);
            break;
        case EXPR_PRE_INC:  walk_assign_target(ctx, e->as.pre_inc.target);  break;
        case EXPR_POST_INC: walk_assign_target(ctx, e->as.post_inc.target); break;
        case EXPR_PRE_DEC:  walk_assign_target(ctx, e->as.pre_dec.target);  break;
        case EXPR_POST_DEC: walk_assign_target(ctx, e->as.post_dec.target); break;
        case EXPR_SPREAD:
            walk_expr(ctx, e->as.spread.expression);
            break;
        case EXPR_LITERAL:
        case EXPR_ERROR:
            break;
    }
}

static void walk_var_decl(Ctx* ctx, const Stmt* s)
{
    const VarDeclStmt* vd = &s->as.var_declaration;
    SymbolKind kind = (ctx_depth(ctx) == 0) ? SYMBOL_KIND_VAR : SYMBOL_KIND_LOCAL;
    for (int j = 0; j < vd->count; j++) {
        // Walk the initializer FIRST in the current scope so that
        // `var x = x;` resolves the RHS `x` to the enclosing scope's
        // definition (if any) before the new binding shadows it. This
        // matches standard block-scoping semantics and what hosts
        // expect.
        walk_expr(ctx, vd->variables[j].initializer);
        emit_decl(ctx, kind, &vd->variables[j].name, s);
    }
}

static void walk_stmt(Ctx* ctx, const Stmt* s)
{
    if (s == NULL) return;
    switch (s->type) {
        case STMT_EXPRESSION:
            walk_expr(ctx, s->as.expression.expression);
            break;
        case STMT_VAR_DECLARATION:
            walk_var_decl(ctx, s);
            break;
        case STMT_BLOCK: {
            ctx_push_scope(ctx);
            const BlockStmt* b = &s->as.block;
            // Per-block func hoist — mirrors compiler.c's BLOCK handler
            // which pre-declares direct `STMT_FUNC_DECLARATION` children
            // in the block scope before compiling body statements. This
            // is what makes forward calls like `helper()` above its
            // `func helper()` sibling resolve, whether the block is a
            // top-level loop body, a function body, or nested.
            hoist_block_funcs(ctx, b->statements, b->count);
            for (int i = 0; i < b->count; i++) walk_stmt(ctx, b->statements[i]);
            ctx_pop_scope(ctx);
            break;
        }
        case STMT_IF:
            walk_expr(ctx, s->as.if_stmt.condition);
            ctx_push_scope(ctx);
            walk_stmt(ctx, s->as.if_stmt.then_branch);
            ctx_pop_scope(ctx);
            if (s->as.if_stmt.else_branch) {
                ctx_push_scope(ctx);
                walk_stmt(ctx, s->as.if_stmt.else_branch);
                ctx_pop_scope(ctx);
            }
            break;
        case STMT_WHILE:
            walk_expr(ctx, s->as.while_stmt.condition);
            ctx_push_scope(ctx);
            walk_stmt(ctx, s->as.while_stmt.body);
            ctx_pop_scope(ctx);
            break;
        case STMT_DO_WHILE:
            ctx_push_scope(ctx);
            walk_stmt(ctx, s->as.do_while_stmt.body);
            ctx_pop_scope(ctx);
            walk_expr(ctx, s->as.do_while_stmt.condition);
            break;
        case STMT_FOR: {
            // For-init gets its own scope that wraps the whole for,
            // so `var i = 0` in the init is visible in cond/incr/body.
            ctx_push_scope(ctx);
            walk_stmt(ctx, s->as.for_stmt.initializer);
            walk_expr(ctx, s->as.for_stmt.condition);
            walk_expr(ctx, s->as.for_stmt.increment);
            walk_stmt(ctx, s->as.for_stmt.body);
            ctx_pop_scope(ctx);
            break;
        }
        case STMT_FUNC_DECLARATION: {
            // Function name is declared in the enclosing scope; params
            // + body live in a fresh function frame (Phase 4.1d) so
            // references to outer locals become UPVALUE symbols.
            //
            // When the pre-pass already hoisted this top-level func
            // (mirrors compiler.c's Pass 1), skip the redundant
            // emit_decl to avoid a duplicate SYMBOL_KIND_FUNC entry at
            // depth 0. Nested (non-top-level) func decls still emit
            // here normally.
            SymbolKind fn_kind = SYMBOL_KIND_FUNC;
            // Skip emit_decl when this name is already present in the
            // current scope thanks to a pre-pass:
            //   (a) top-level pre-pass (mirrors compiler.c Pass 1), or
            //   (b) enclosing BLOCK's per-block pre-hoist (mirrors
            //       compiler.c's BLOCK handler pre-declaration).
            // In both cases re-emitting would duplicate the symbol.
            bool enclosing_hoisted = ctx->scopes[ctx->scope_count - 1].funcs_hoisted;
            bool top_hoisted = (ctx->top_level_hoisted && ctx_depth(ctx) == 0);
            if (!top_hoisted && !enclosing_hoisted) {
                emit_decl(ctx, fn_kind, &s->as.func_declaration.name, s);
            }
            ctx_push_function_frame(ctx);
            for (int i = 0; i < s->as.func_declaration.param_count; i++) {
                emit_param(ctx, &s->as.func_declaration.params[i].name);
            }
            // Body is a STMT_BLOCK whose scope-push handler pre-hoists
            // direct `func` children, so forward calls to nested funcs
            // inside the body resolve correctly.
            walk_stmt(ctx, s->as.func_declaration.body);
            ctx_pop_scope(ctx);
            break;
        }
        case STMT_RETURN:
            walk_expr(ctx, s->as.return_stmt.value);
            break;
        case STMT_STRUCT_DECLARATION: {
            // Top-level structs are fully registered (decl + fields) by
            // the pre-pass; nothing to walk in the body here.
            if (ctx->top_level_hoisted && ctx_depth(ctx) == 0) break;
            int idx = emit_decl(ctx, SYMBOL_KIND_STRUCT,
                                &s->as.struct_declaration.name, s);
            if (idx >= 0) {
                const StructDeclStmt* sd = &s->as.struct_declaration;
                for (int i = 0; i < sd->field_count; i++) {
                    emit_child(ctx, SYMBOL_KIND_FIELD, &sd->fields[i], idx);
                }
            }
            break;
        }
        case STMT_ENUM_DECLARATION: {
            // Top-level enums are fully registered (decl + variants) by
            // the pre-pass; nothing to walk in the body here.
            if (ctx->top_level_hoisted && ctx_depth(ctx) == 0) break;
            int idx = emit_decl(ctx, SYMBOL_KIND_ENUM,
                                &s->as.enum_declaration.name, s);
            if (idx >= 0) {
                const EnumDeclStmt* ed = &s->as.enum_declaration;
                for (int i = 0; i < ed->variant_count; i++) {
                    emit_child(ctx, SYMBOL_KIND_VARIANT, &ed->variants[i], idx);
                }
            }
            break;
        }
        case STMT_SWITCH: {
            walk_expr(ctx, s->as.switch_stmt.expression);
            for (int i = 0; i < s->as.switch_stmt.case_count; i++) {
                const CaseClause* c = &s->as.switch_stmt.cases[i];
                walk_expr(ctx, c->value);
                for (int j = 0; j < c->statement_count; j++) {
                    walk_stmt(ctx, c->statements[j]);
                }
            }
            break;
        }
        case STMT_LABEL:
        case STMT_GOTO:
        case STMT_BREAK:
        case STMT_CONTINUE:
        case STMT_COMPILER_DIRECTIVE:
        case STMT_ERROR:
            break;
    }
}

// -----------------------------------------------------------------------------
// Host-registered natives pre-seed (Phase 4.5c)
// -----------------------------------------------------------------------------
// The compiler classifies every bare-name use of a host-registered
// native (`print`, `length`, `str`, `assert`, ...) as a GLOBAL and
// emits `GET_GLOBAL` for it. The runtime then resolves the call
// through the mangled key (`print@<arity>` / `print@v<fixed>`) stored
// in `vm->globals`.
//
// For LSP parity the resolver must do the same classification: a
// call-site to `print(...)` has to resolve to a real symbol instead
// of dropping to -1. We pre-seed the top-level scope with one
// `SYMBOL_KIND_NATIVE` symbol per distinct bare name found in
// `vm->globals` whose value is an `ObjNativeFunction`.
//
// Naming: the stored key is mangled (e.g. "print@1"). We borrow the
// `ObjString.chars` pointer directly and set `name_length` to the
// position of the '@' separator so hosts see only the user-facing
// name. Lifetime is fine — global-table entries are GC roots and
// their `ObjString`s outlive the resolver call.
//
// Span: natives have no editor-visible declaration. `name_file_id =
// ZYM_FILE_ID_INVALID`, `name_start_byte = -1`, and `def_span` is
// zeroed so position-keyed queries (`findSymbolAt`, `listFileSymbols`)
// filter them out naturally.
//
// Dedup: multiple arities of the same bare name (e.g. `Random()` and
// `Random(seed)`) collapse to one NATIVE symbol.
static int find_native_bare(const ZymSymbolTable* table,
                            const char* name, int name_length)
{
    for (int i = 0; i < table->count; i++) {
        const Symbol* s = &table->symbols[i];
        if (s->kind != SYMBOL_KIND_NATIVE) continue;
        if (s->name_length != name_length) continue;
        if (s->name == NULL || name == NULL) continue;
        if (memcmp(s->name, name, (size_t)name_length) == 0) return i;
    }
    return -1;
}

static void seed_natives_from_globals(Ctx* ctx)
{
    if (ctx == NULL || ctx->vm == NULL) return;
    const Table* globals = &ctx->vm->globals;
    if (globals->entries == NULL) return;

    for (int i = 0; i < globals->capacity; i++) {
        const Entry* e = &globals->entries[i];
        if (e->key == NULL) continue;
        // Slot-based user globals (see zym_defineGlobal) store a
        // DOUBLE_VAL slot index. Real native bindings store an
        // ObjNativeFunction; that's our filter.
        if (!IS_OBJ(e->value)) continue;
        if (AS_OBJ(e->value)->type != OBJ_NATIVE_FUNCTION) continue;

        const char* chars = e->key->chars;
        int         len   = e->key->length;
        if (chars == NULL || len <= 0) continue;

        // Strip the "@<arity>" / "@v<fixed>" suffix.
        int bare_len = len;
        for (int j = 0; j < len; j++) {
            if (chars[j] == '@') { bare_len = j; break; }
        }
        if (bare_len <= 0) continue;

        // Dedup: overloaded natives (`Random()` / `Random(seed)`) get
        // one NATIVE symbol.
        if (find_native_bare(ctx->table, chars, bare_len) >= 0) continue;

        Symbol s;
        s.kind                  = SYMBOL_KIND_NATIVE;
        s.name                  = chars;
        s.name_length           = bare_len;
        s.name_file_id          = ZYM_FILE_ID_INVALID;
        s.name_start_byte       = -1;
        s.def_span.fileId       = ZYM_FILE_ID_INVALID;
        s.def_span.startByte    = -1;
        s.def_span.length       = 0;
        s.def_span.startLine    = 0;
        s.def_span.startColumn  = 0;
        s.def_span.endLine      = 0;
        s.def_span.endColumn    = 0;
        s.scope_depth           = 0;
        s.parent_index          = -1;
        (void)push_symbol(ctx->vm, ctx->table, s);
        ctx->visible_end = ctx->table->count;
    }
}

// -----------------------------------------------------------------------------
// Per-block func pre-hoist (mirrors compiler.c's BLOCK handler).
// -----------------------------------------------------------------------------
// Scans a block's direct statement list for `STMT_FUNC_DECLARATION`
// children and emits a `SYMBOL_KIND_FUNC` entry for each into the
// current (top) scope. Does NOT recurse into nested blocks, loop
// bodies, or if-branches — each such construct pushes its own scope
// and runs its own per-block hoist on scope entry. Also does not
// recurse into function bodies (those are separate frames). This
// uniform per-block model handles every forward-call case the
// compiler handles: top-level blocks, function bodies, loop bodies,
// if-branches, and their nestings.
static void hoist_block_funcs(Ctx* ctx, Stmt** statements, int count)
{
    if (statements == NULL) return;
    for (int i = 0; i < count; i++) {
        const Stmt* s = statements[i];
        if (s == NULL) continue;
        if (s->type == STMT_FUNC_DECLARATION) {
            emit_decl(ctx, SYMBOL_KIND_FUNC,
                      &s->as.func_declaration.name, s);
        }
    }
    ctx->scopes[ctx->scope_count - 1].funcs_hoisted = true;
}

// -----------------------------------------------------------------------------
// Top-level pre-pass (mirrors compiler.c Pass 1)
// -----------------------------------------------------------------------------
// Registers declarations that the compiler hoists so forward references
// inside later function bodies (e.g. `hoisted_factory(1000)` appearing
// above `func hoisted_factory(...) {...}`) resolve to the real symbol
// instead of silently dropping to -1. Only `FUNC`, `STRUCT`, and `ENUM`
// are hoisted — this matches exactly what `compile()` Pass 1 registers
// at the top level. Top-level `var`s remain globals that bind by name
// at runtime, so we don't hoist them here either (matches compiler).
static void hoist_top_level_decl(Ctx* ctx, const Stmt* s)
{
    if (s == NULL) return;
    switch (s->type) {
        case STMT_FUNC_DECLARATION:
            emit_decl(ctx, SYMBOL_KIND_FUNC,
                      &s->as.func_declaration.name, s);
            break;
        case STMT_STRUCT_DECLARATION: {
            int idx = emit_decl(ctx, SYMBOL_KIND_STRUCT,
                                &s->as.struct_declaration.name, s);
            if (idx >= 0) {
                const StructDeclStmt* sd = &s->as.struct_declaration;
                for (int i = 0; i < sd->field_count; i++) {
                    emit_child(ctx, SYMBOL_KIND_FIELD, &sd->fields[i], idx);
                }
            }
            break;
        }
        case STMT_ENUM_DECLARATION: {
            int idx = emit_decl(ctx, SYMBOL_KIND_ENUM,
                                &s->as.enum_declaration.name, s);
            if (idx >= 0) {
                const EnumDeclStmt* ed = &s->as.enum_declaration;
                for (int i = 0; i < ed->variant_count; i++) {
                    emit_child(ctx, SYMBOL_KIND_VARIANT,
                               &ed->variants[i], idx);
                }
            }
            break;
        }
        default:
            break;
    }
}

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------
bool resolver_resolve_top_level(VM* vm, const ZymParseTree* tree,
                                ZymSymbolTable* table)
{
    if (tree == NULL || table == NULL) return false;
    if (tree->ast.statements == NULL) return true;

    Ctx ctx;
    ctx_init(&ctx, vm, table);

    // Seed host-registered natives into the top-level scope so bare-name
    // uses like `print(...)` resolve to a real symbol (Phase 4.5c).
    // Runs before hoisting so user top-level decls with the same name
    // (e.g. `var print = ...`) shadow the native on innermost-first
    // lookup, matching how the compiler's slot-based globals supersede
    // mangled native bindings.
    seed_natives_from_globals(&ctx);

    // Pre-pass: hoist top-level FUNC/STRUCT/ENUM declarations so
    // forward references inside function bodies resolve correctly.
    // Mirrors compiler.c Pass 1 (`declare_function` + early struct/enum
    // registration). Runs at depth 0 with no body walk, no initializer
    // walk — just symbol emission.
    for (int i = 0; i < tree->ast.capacity; i++) {
        Stmt* s = tree->ast.statements[i];
        if (s == NULL) break;  // NULL sentinel
        hoist_top_level_decl(&ctx, s);
    }
    ctx.top_level_hoisted = true;

    // Full walk: resolve references and register remaining decls. The
    // `top_level_hoisted` flag tells `walk_stmt` to skip re-emitting
    // the symbols the pre-pass already registered at depth 0.
    for (int i = 0; i < tree->ast.capacity; i++) {
        Stmt* s = tree->ast.statements[i];
        if (s == NULL) break;  // NULL sentinel
        walk_stmt(&ctx, s);
    }

    return true;
}

#endif // ZYM_HAS_SYMBOL_TABLE
