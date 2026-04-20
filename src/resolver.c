#include "./config.h"

#if ZYM_HAS_SYMBOL_TABLE

#include "./resolver.h"
#include "./ast.h"
#include "./memory.h"
#include "./node_span.h"
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
    int base;    // index into table->symbols of the first symbol in this scope
    int depth;   // logical depth (top-level == 0)
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
} Ctx;

static void ctx_init(Ctx* ctx, VM* vm, ZymSymbolTable* table)
{
    ctx->vm            = vm;
    ctx->table         = table;
    ctx->scope_count   = 1;
    ctx->scopes[0].base  = 0;
    ctx->scopes[0].depth = 0;
    ctx->visible_end   = 0;
}

static int ctx_depth(const Ctx* ctx)
{
    return ctx->scopes[ctx->scope_count - 1].depth;
}

static void ctx_push_scope(Ctx* ctx)
{
    if (ctx->scope_count >= MAX_SCOPES) return;  // overflow: flatten into outermost
    int base = ctx->visible_end;
    int depth = ctx->scopes[ctx->scope_count - 1].depth + 1;
    ctx->scopes[ctx->scope_count].base  = base;
    ctx->scopes[ctx->scope_count].depth = depth;
    ctx->scope_count++;
}

static void ctx_pop_scope(Ctx* ctx)
{
    if (ctx->scope_count <= 1) return;
    // Roll visibility back so symbols declared in the popped scope are
    // no longer findable by ctx_lookup. The symbols remain in
    // `table->symbols` for consumers to read out after the walk.
    ctx->visible_end = ctx->scopes[ctx->scope_count - 1].base;
    ctx->scope_count--;
}

// Innermost-first lookup for an identifier-use: walks scopes from the
// deepest open scope down to the top-level and scans for a symbol
// whose `name` matches `nm`. Returns the symbol index, or -1 if no
// match is in scope.
static int ctx_lookup(const Ctx* ctx, const Token* nm)
{
    if (nm == NULL || nm->length <= 0) return -1;
    const char* nb = lookup_name_bytes(ctx->vm, nm);
    if (nb == NULL) return -1;
    int need = nm->length;

    const ZymSymbolTable* table = ctx->table;
    for (int s = ctx->scope_count - 1; s >= 0; s--) {
        int base = ctx->scopes[s].base;
        int end  = (s + 1 < ctx->scope_count) ? ctx->scopes[s + 1].base
                                              : ctx->visible_end;
        // innermost-first within a scope: iterate in reverse so the
        // most-recently declared symbol wins on redeclaration.
        for (int i = end - 1; i >= base; i--) {
            const Symbol* sy = &table->symbols[i];
            if (sy->name_length == need && sy->name != NULL &&
                memcmp(sy->name, nb, (size_t)need) == 0) {
                return i;
            }
        }
    }
    return -1;
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
    int idx = push_symbol(ctx->vm, ctx->table, s);
    ctx->visible_end = ctx->table->count;
    return idx;
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

// AssignExpr.target is an Expr*; recognize the common shapes for
// binding purposes: a VariableExpr means a write-ref to a name; a
// SetExpr.object/GetExpr chain means a write to a field (field name
// is unresolved at 4.1b).
static void walk_assign_target(Ctx* ctx, const Expr* target)
{
    if (target == NULL) return;
    switch (target->type) {
        case EXPR_VARIABLE: {
            const Token* nm = &target->as.variable.name;
            int idx = ctx_lookup(ctx, nm);
            record_ref(ctx, nm, /*is_write=*/true, idx);
            return;
        }
        case EXPR_GET: {
            walk_expr(ctx, target->as.get.object);
            // Field name itself — unresolved at 4.1b.
            record_ref(ctx, &target->as.get.name, /*is_write=*/true, -1);
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
            int idx = ctx_lookup(ctx, nm);
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
        case EXPR_GET:
            walk_expr(ctx, e->as.get.object);
            record_ref(ctx, &e->as.get.name, /*is_write=*/false, -1);
            break;
        case EXPR_SET:
            walk_expr(ctx, e->as.set.object);
            record_ref(ctx, &e->as.set.name, /*is_write=*/true, -1);
            walk_expr(ctx, e->as.set.value);
            break;
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
            // Anonymous function — params form a new scope.
            ctx_push_scope(ctx);
            for (int i = 0; i < e->as.function.param_count; i++) {
                emit_param(ctx, &e->as.function.params[i].name);
            }
            walk_stmt(ctx, e->as.function.body);
            ctx_pop_scope(ctx);
            break;
        }
        case EXPR_STRUCT_INST:
            // struct_name is a type reference; field names are
            // unresolved at 4.1b.
            record_ref(ctx, &e->as.struct_inst.struct_name,
                       /*is_write=*/false,
                       ctx_lookup(ctx, &e->as.struct_inst.struct_name));
            for (int i = 0; i < e->as.struct_inst.field_count; i++) {
                walk_expr(ctx, e->as.struct_inst.field_values[i]);
            }
            break;
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
            // The function name is declared in the enclosing scope;
            // params + body live in a fresh inner scope.
            SymbolKind fn_kind = SYMBOL_KIND_FUNC;
            emit_decl(ctx, fn_kind, &s->as.func_declaration.name, s);
            ctx_push_scope(ctx);
            for (int i = 0; i < s->as.func_declaration.param_count; i++) {
                emit_param(ctx, &s->as.func_declaration.params[i].name);
            }
            walk_stmt(ctx, s->as.func_declaration.body);
            ctx_pop_scope(ctx);
            break;
        }
        case STMT_RETURN:
            walk_expr(ctx, s->as.return_stmt.value);
            break;
        case STMT_STRUCT_DECLARATION:
            emit_decl(ctx, SYMBOL_KIND_STRUCT,
                      &s->as.struct_declaration.name, s);
            break;
        case STMT_ENUM_DECLARATION:
            emit_decl(ctx, SYMBOL_KIND_ENUM,
                      &s->as.enum_declaration.name, s);
            break;
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
// Entry point
// -----------------------------------------------------------------------------
bool resolver_resolve_top_level(VM* vm, const ZymParseTree* tree,
                                ZymSymbolTable* table)
{
    if (tree == NULL || table == NULL) return false;
    if (tree->ast.statements == NULL) return true;

    Ctx ctx;
    ctx_init(&ctx, vm, table);

    for (int i = 0; i < tree->ast.capacity; i++) {
        Stmt* s = tree->ast.statements[i];
        if (s == NULL) break;  // NULL sentinel
        walk_stmt(&ctx, s);
    }

    return true;
}

#endif // ZYM_HAS_SYMBOL_TABLE
