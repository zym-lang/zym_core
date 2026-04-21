#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <inttypes.h>

#include "./vm.h"
#include "./chunk.h"
#ifndef ZYM_RUNTIME_ONLY
#include "./preprocessor.h"
#include "./compiler.h"
#include "./parse_tree.h"
#include "./node_span.h"
#include "./trivia.h"
#include "./sourcemap.h"
#include "./source_file.h"
#endif
#include "./serializer.h"
#include "./value.h"
#include "./object.h"
#include "./native.h"
#include "./table.h"
#include "./gc.h"
#include "./memory.h"

#include "zym/zym.h"
#include "zym/diagnostics.h"

// =============================================================================
// VM LIFECYCLE
// =============================================================================

ZymVM* zym_newVM(ZymAllocator* allocator)
{
    ZymAllocator alloc = allocator ? *allocator : zym_defaultAllocator();
    ZymVM* vm = (ZymVM*)ZYM_ALLOC(&alloc, sizeof(ZymVM));
    if (vm == NULL) return NULL;
    vm->allocator = alloc;
    initVM(vm);
    return vm;
}

void zym_freeVM(ZymVM* vm)
{
    if (vm == NULL) return;
    ZymAllocator alloc = vm->allocator;
    freeVM(vm);
    ZYM_FREE(&alloc, vm, sizeof(ZymVM));
}

const ZymAllocator* zym_getAllocator(ZymVM* vm)
{
    if (vm == NULL) return NULL;
    return &vm->allocator;
}

// =============================================================================
// ERROR CALLBACK
// =============================================================================

void zym_setErrorCallback(ZymVM* vm, ZymErrorCallback callback, void* user_data)
{
    if (vm == NULL) return;
    vm->error_callback = callback;
    vm->error_user_data = user_data;
}

// =============================================================================
// STRUCTURED DIAGNOSTICS (Phase 1.3)
// =============================================================================

const ZymDiagnostic* zymGetDiagnostics(ZymVM* vm, size_t* count)
{
    if (vm == NULL) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = vm->diagnostics.count;
    return vm->diagnostics.count == 0 ? NULL : vm->diagnostics.items;
}

void zymClearDiagnostics(ZymVM* vm)
{
    if (vm == NULL) return;
    diagsink_clear(vm, &vm->diagnostics);
}

// =============================================================================
// COOPERATIVE CANCELLATION (Phase 1.5)
// =============================================================================

void zym_requestCancel(ZymVM* vm)
{
    if (vm == NULL) return;
    vm->compile_cancelled = 1;
}

void zym_clearCancel(ZymVM* vm)
{
    if (vm == NULL) return;
    vm->compile_cancelled = 0;
}

bool zym_wasCancelled(const ZymVM* vm)
{
    if (vm == NULL) return false;
    return vm->compile_cancelled != 0;
}

// =============================================================================
// COMPILATION AND EXECUTION
// =============================================================================

ZymChunk* zym_newChunk(ZymVM* vm)
{
    ZymChunk* chunk = (ZymChunk*)ZYM_ALLOC(&vm->allocator, sizeof(ZymChunk));
    if (chunk == NULL) return NULL;
    initChunk(chunk);
    return chunk;
}

void zym_freeChunk(ZymVM* vm, ZymChunk* chunk)
{
    if (chunk == NULL) return;
    if (vm->chunk == chunk) {
        vm->chunk = NULL;
    }
    freeChunk(vm, chunk);
    ZYM_FREE(&vm->allocator, chunk, sizeof(ZymChunk));
}

#ifndef ZYM_RUNTIME_ONLY
// ---- Phase 1.1 / 1.2 / 1.6: SourceFile registry + SourceMap public API ----
//
// Phase 1.6 collapsed LineMap into SourceMap; the Ex-suffixed variants
// are gone too (pre-1.0: no transitional shim is kept, per the roadmap's
// "changelog, not a runway" policy). SourceMap is the sole origin-
// tracking primitive consumed by the scanner / diagnostics / LSP.

ZymFileId zym_registerSourceFile(ZymVM* vm, const char* path,
                                 const char* bytes, size_t length)
{
    if (vm == NULL) return ZYM_FILE_ID_INVALID;
    return sfr_register(vm, &vm->source_files, path, bytes, length);
}

int zym_getSourceFile(ZymVM* vm, ZymFileId fileId, ZymSourceFileInfo* out)
{
    if (vm == NULL || out == NULL) return 0;
    const SourceFile* sf = sfr_get(&vm->source_files, fileId);
    if (sf == NULL) return 0;
    out->id     = sf->id;
    out->path   = sf->path;
    out->bytes  = sf->bytes;
    out->length = sf->length;
    return 1;
}

ZymSourceMap* zym_newSourceMap(ZymVM* vm)
{
    if (vm == NULL) return NULL;
    ZymSourceMap* map = (ZymSourceMap*)ZYM_ALLOC(&vm->allocator, sizeof(ZymSourceMap));
    if (map == NULL) return NULL;
    initSourceMap(map);
    return map;
}

void zym_freeSourceMap(ZymVM* vm, ZymSourceMap* map)
{
    if (vm == NULL || map == NULL) return;
    freeSourceMap(vm, map);
    ZYM_FREE(&vm->allocator, map, sizeof(ZymSourceMap));
}

ZymStatus zym_preprocess(ZymVM* vm, const char* source,
                         ZymSourceMap* source_map, ZymFileId origin_file_id,
                         const char** processedSource)
{
    if (source == NULL || processedSource == NULL) return ZYM_STATUS_COMPILE_ERROR;

    char* processed = preprocess(vm, source, source_map, origin_file_id);
    if (processed == NULL) {
        *processedSource = NULL;
        return ZYM_STATUS_COMPILE_ERROR;
    }

    *processedSource = processed;
    return ZYM_STATUS_OK;
}

void zym_freeProcessedSource(ZymVM* vm, const char* processedSource)
{
    if (processedSource == NULL) return;
    ZYM_FREE_STR(&vm->allocator, (char*)processedSource);
}

ZymStatus zym_compile(ZymVM* vm, const char* source, ZymChunk* chunk,
                      const ZymSourceMap* source_map,
                      const char* entry_file, ZymCompilerConfig config,
                      ZymParseTree** out_tree)
{
    if (out_tree) *out_tree = NULL;
    if (source == NULL || chunk == NULL) return ZYM_STATUS_COMPILE_ERROR;
    bool success = compile(vm, source, chunk, source_map, entry_file, config, out_tree);
    return success ? ZYM_STATUS_OK : ZYM_STATUS_COMPILE_ERROR;
}

#if ZYM_HAS_PARSE_TREE_RETENTION
ZymStatus zym_parseOnly(ZymVM* vm, const char* source,
                        const ZymSourceMap* source_map,
                        const char* entry_file,
                        ZymParseTree** out_tree)
{
    if (out_tree == NULL) return ZYM_STATUS_COMPILE_ERROR;
    *out_tree = NULL;
    if (vm == NULL || source == NULL) return ZYM_STATUS_COMPILE_ERROR;
    bool success = parseOnly(vm, source, source_map, entry_file, out_tree);
    return success ? ZYM_STATUS_OK : ZYM_STATUS_COMPILE_ERROR;
}

void zym_freeParseTree(ZymVM* vm, ZymParseTree* tree)
{
    parse_tree_free(vm, tree);
}

// Phase 2.3 trivia accessors. `ZymTrivia` / `ZymTriviaKind` are the
// public mirrors of the internal `TriviaPiece` / `TriviaKind`; the
// enum values are kept 1:1 so this is a field-wise copy, not a
// translation.
static void trivia_piece_to_public(const TriviaPiece* src, ZymTrivia* dst)
{
    dst->kind        = (ZymTriviaKind)src->kind;
    dst->fileId      = src->fileId;
    dst->startByte   = src->startByte;
    dst->length      = src->length;
    dst->startLine   = src->startLine;
    dst->startColumn = src->startColumn;
}

int zym_parseTreeTriviaCount(const ZymParseTree* tree)
{
    if (tree == NULL || tree->trivia == NULL) return 0;
    return tree->trivia->count;
}

bool zym_parseTreeTriviaAt(const ZymParseTree* tree, int i, ZymTrivia* out)
{
    if (out == NULL) return false;
    if (tree == NULL || tree->trivia == NULL) return false;
    if (i < 0 || i >= tree->trivia->count) return false;
    trivia_piece_to_public(&tree->trivia->pieces[i], out);
    return true;
}

bool zym_parseTreeTriviaFindAt(const ZymParseTree* tree,
                               int byteOffset, ZymTrivia* out)
{
    if (out == NULL) return false;
    if (tree == NULL || tree->trivia == NULL) return false;
    const TriviaPiece* p = trivia_find_at(tree->trivia, byteOffset);
    if (p == NULL) return false;
    trivia_piece_to_public(p, out);
    return true;
}

// Phase 2.4 — top-level node span accessors. Thin wrappers over the
// internal `nodeSpanOfStmt` helper; the richer query API (Phase 2.5)
// will expose node spans for arbitrary depths.
// AstResult stores `capacity` (the backing buffer size) and terminates
// the valid-stmt range with a NULL sentinel (see parse() in parser.c).
// We count up to the first NULL to report the *real* number of parsed
// top-level statements.
static int parse_tree_real_count(const ZymParseTree* tree)
{
    if (tree == NULL || tree->ast.statements == NULL) return 0;
    int cap = tree->ast.capacity;
    int n = 0;
    while (n < cap && tree->ast.statements[n] != NULL) n++;
    return n;
}

int zym_parseTreeTopLevelCount(const ZymParseTree* tree)
{
    return parse_tree_real_count(tree);
}

bool zym_parseTreeTopLevelSpan(const ZymParseTree* tree, int i, ZymSpan* out)
{
    if (out == NULL || tree == NULL) return false;
    int n = parse_tree_real_count(tree);
    if (i < 0 || i >= n) return false;
    Stmt* stmt = tree->ast.statements[i];
    *out = nodeSpanOfStmt(stmt);
    return true;
}
// =====================================================================
// Phase 2.5 — Query API implementation
// =====================================================================
static ZymNodeKind stmt_to_node_kind(const Stmt* s)
{
    switch (s->type) {
        case STMT_EXPRESSION:         return ZYM_NODE_EXPRESSION;
        case STMT_VAR_DECLARATION:    return ZYM_NODE_VAR_DECL;
        case STMT_BLOCK:              return ZYM_NODE_BLOCK;
        case STMT_IF:                 return ZYM_NODE_IF;
        case STMT_WHILE:              return ZYM_NODE_WHILE;
        case STMT_DO_WHILE:           return ZYM_NODE_DO_WHILE;
        case STMT_FOR:                return ZYM_NODE_FOR;
        case STMT_BREAK:              return ZYM_NODE_BREAK;
        case STMT_CONTINUE:           return ZYM_NODE_CONTINUE;
        case STMT_FUNC_DECLARATION:   return ZYM_NODE_FUNC_DECL;
        case STMT_RETURN:             return ZYM_NODE_RETURN;
        case STMT_COMPILER_DIRECTIVE: return ZYM_NODE_DIRECTIVE;
        case STMT_STRUCT_DECLARATION: return ZYM_NODE_STRUCT_DECL;
        case STMT_ENUM_DECLARATION:   return ZYM_NODE_ENUM_DECL;
        case STMT_LABEL:              return ZYM_NODE_LABEL;
        case STMT_GOTO:               return ZYM_NODE_GOTO;
        case STMT_SWITCH:             return ZYM_NODE_SWITCH;
        case STMT_ERROR:              return ZYM_NODE_ERROR;
    }
    return ZYM_NODE_UNKNOWN;
}
static void fill_node_info(const Stmt* s, ZymNodeInfo* info)
{
    info->kind           = stmt_to_node_kind(s);
    info->span           = nodeSpanOfStmt(s);
    info->nameFileId     = -1;
    info->nameStartByte  = -1;
    info->nameLength     = 0;
    const Token* nm = NULL;
    switch (s->type) {
        case STMT_FUNC_DECLARATION:   nm = &s->as.func_declaration.name;    break;
        case STMT_STRUCT_DECLARATION: nm = &s->as.struct_declaration.name;  break;
        case STMT_ENUM_DECLARATION:   nm = &s->as.enum_declaration.name;    break;
        case STMT_LABEL:              nm = &s->as.label.label_name;         break;
        case STMT_VAR_DECLARATION:
            if (s->as.var_declaration.count > 0) {
                nm = &s->as.var_declaration.variables[0].name;
            }
            break;
        default: break;
    }
    if (nm != NULL && nm->length > 0) {
        info->nameFileId    = nm->fileId;
        info->nameStartByte = nm->startByte;
        info->nameLength    = nm->length;
    }
}
static bool span_contains_offset(const ZymSpan* sp, int byteOffset)
{
    if (sp->fileId < 0 || sp->startByte < 0 || sp->length <= 0) return false;
    return byteOffset >= sp->startByte &&
           byteOffset <  sp->startByte + sp->length;
}
static bool is_block_bearing(StmtType t)
{
    switch (t) {
        case STMT_BLOCK:
        case STMT_IF:
        case STMT_WHILE:
        case STMT_DO_WHILE:
        case STMT_FOR:
        case STMT_FUNC_DECLARATION:
        case STMT_STRUCT_DECLARATION:
        case STMT_ENUM_DECLARATION:
        case STMT_SWITCH:
            return true;
        default:
            return false;
    }
}
// Visit every *direct* statement child of `s` in source order.
typedef void (*stmt_visitor_fn)(const Stmt* child, void* ctx);
static void for_each_child_stmt(const Stmt* s,
                                stmt_visitor_fn fn, void* ctx)
{
    if (s == NULL) return;
    switch (s->type) {
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.count; i++) {
                if (s->as.block.statements[i]) fn(s->as.block.statements[i], ctx);
            }
            break;
        case STMT_IF:
            if (s->as.if_stmt.then_branch) fn(s->as.if_stmt.then_branch, ctx);
            if (s->as.if_stmt.else_branch) fn(s->as.if_stmt.else_branch, ctx);
            break;
        case STMT_WHILE:
            if (s->as.while_stmt.body) fn(s->as.while_stmt.body, ctx);
            break;
        case STMT_DO_WHILE:
            if (s->as.do_while_stmt.body) fn(s->as.do_while_stmt.body, ctx);
            break;
        case STMT_FOR:
            if (s->as.for_stmt.initializer) fn(s->as.for_stmt.initializer, ctx);
            if (s->as.for_stmt.body)        fn(s->as.for_stmt.body, ctx);
            break;
        case STMT_FUNC_DECLARATION:
            if (s->as.func_declaration.body) fn(s->as.func_declaration.body, ctx);
            break;
        case STMT_SWITCH:
            for (int i = 0; i < s->as.switch_stmt.case_count; i++) {
                const CaseClause* c = &s->as.switch_stmt.cases[i];
                for (int j = 0; j < c->statement_count; j++) {
                    if (c->statements[j]) fn(c->statements[j], ctx);
                }
            }
            break;
        default: break;
    }
}
// --- zymTreeFindNodeAt -------------------------------------------------
typedef struct { int byteOffset; const Stmt* deepest; } FindCtx;
static void find_visit(const Stmt* s, void* ctxp)
{
    FindCtx* ctx = (FindCtx*)ctxp;
    ZymSpan sp = nodeSpanOfStmt(s);
    if (!span_contains_offset(&sp, ctx->byteOffset)) return;
    ctx->deepest = s;
    for_each_child_stmt(s, find_visit, ctx);
}
bool zymTreeFindNodeAt(const ZymParseTree* tree, int byteOffset,
                       ZymNodeInfo* out)
{
    if (tree == NULL || out == NULL) return false;
    int n = parse_tree_real_count(tree);
    FindCtx ctx = { byteOffset, NULL };
    for (int i = 0; i < n; i++) {
        find_visit(tree->ast.statements[i], &ctx);
        if (ctx.deepest != NULL) break;  // top-level spans do not overlap
    }
    if (ctx.deepest == NULL) return false;
    fill_node_info(ctx.deepest, out);
    return true;
}
// --- zymTreeListTopLevelDeclarations -----------------------------------
int zymTreeListTopLevelDeclarations(const ZymParseTree* tree,
                                    ZymNodeInfo* out, int max_out)
{
    if (tree == NULL) return 0;
    int n = parse_tree_real_count(tree);
    int total = 0;
    for (int i = 0; i < n; i++) {
        const Stmt* s = tree->ast.statements[i];
        if (s == NULL) continue;
        switch (s->type) {
            case STMT_VAR_DECLARATION:
            case STMT_FUNC_DECLARATION:
            case STMT_STRUCT_DECLARATION:
            case STMT_ENUM_DECLARATION:
                if (out != NULL && total < max_out) {
                    fill_node_info(s, &out[total]);
                }
                total++;
                break;
            default: break;
        }
    }
    return total;
}
// --- zymTreeEnclosingBlocks --------------------------------------------
typedef struct {
    int          byteOffset;
    ZymNodeInfo* out;
    int          max_out;
    int          total;
} EnclosingCtx;
static void enclosing_visit(const Stmt* s, void* ctxp)
{
    EnclosingCtx* ctx = (EnclosingCtx*)ctxp;
    ZymSpan sp = nodeSpanOfStmt(s);
    if (!span_contains_offset(&sp, ctx->byteOffset)) return;
    if (is_block_bearing(s->type)) {
        if (ctx->out != NULL && ctx->total < ctx->max_out) {
            fill_node_info(s, &ctx->out[ctx->total]);
        }
        ctx->total++;
    }
    for_each_child_stmt(s, enclosing_visit, ctx);
}
int zymTreeEnclosingBlocks(const ZymParseTree* tree, int byteOffset,
                           ZymNodeInfo* out, int max_out)
{
    if (tree == NULL) return 0;
    int n = parse_tree_real_count(tree);
    EnclosingCtx ctx = { byteOffset, out, max_out, 0 };
    for (int i = 0; i < n; i++) {
        enclosing_visit(tree->ast.statements[i], &ctx);
        if (ctx.total > 0) break;  // top-level spans do not overlap
    }
    return ctx.total;
}
// --- zymTreeFoldingRanges ----------------------------------------------
typedef struct {
    ZymNodeInfo* out;
    int          max_out;
    int          total;
} FoldCtx;
static void folding_visit(const Stmt* s, void* ctxp)
{
    FoldCtx* ctx = (FoldCtx*)ctxp;
    if (s == NULL) return;
    if (is_block_bearing(s->type)) {
        if (ctx->out != NULL && ctx->total < ctx->max_out) {
            fill_node_info(s, &ctx->out[ctx->total]);
        }
        ctx->total++;
    }
    for_each_child_stmt(s, folding_visit, ctx);
}
int zymTreeFoldingRanges(const ZymParseTree* tree,
                         ZymNodeInfo* out, int max_out)
{
    if (tree == NULL) return 0;
    int n = parse_tree_real_count(tree);
    FoldCtx ctx = { out, max_out, 0 };
    for (int i = 0; i < n; i++) {
        folding_visit(tree->ast.statements[i], &ctx);
    }
    return ctx.total;
}
#endif

#if ZYM_HAS_SYMBOL_TABLE
#include "./resolver.h"

ZymStatus zym_check(ZymVM* vm, const char* source,
                    const ZymSourceMap* source_map,
                    const char* entry_file,
                    ZymParseTree** out_tree,
                    ZymSymbolTable** out_table)
{
    if (out_tree == NULL || out_table == NULL) return ZYM_STATUS_COMPILE_ERROR;
    *out_tree = NULL;
    *out_table = NULL;
    if (vm == NULL || source == NULL) return ZYM_STATUS_COMPILE_ERROR;
    bool success = checkCompile(vm, source, source_map, entry_file, out_tree, out_table);
    return success ? ZYM_STATUS_OK : ZYM_STATUS_COMPILE_ERROR;
}

void zym_freeSymbolTable(ZymVM* vm, ZymSymbolTable* table)
{
    symbol_table_free(vm, table);
}

int zym_symbolTableSymbolCount(const ZymSymbolTable* table)
{
    if (table == NULL) return 0;
    return table->count;
}

bool zym_symbolTableSymbolAt(const ZymSymbolTable* table, int i,
                             ZymSymbolInfo* out)
{
    if (out == NULL || table == NULL) return false;
    if (i < 0 || i >= table->count) return false;
    const Symbol* s = &table->symbols[i];
    out->kind          = (ZymSymbolKind)s->kind;
    out->name          = s->name;
    out->nameLength    = s->name_length;
    out->nameFileId    = s->name_file_id;
    out->nameStartByte = s->name_start_byte;
    out->defSpan       = s->def_span;
    out->scopeDepth    = s->scope_depth;
    out->parentIndex   = s->parent_index;
    return true;
}

int zym_symbolTableReferenceCount(const ZymSymbolTable* table)
{
    if (table == NULL) return 0;
    return table->ref_count;
}

bool zym_symbolTableReferenceAt(const ZymSymbolTable* table, int i,
                                ZymReferenceInfo* out)
{
    if (out == NULL || table == NULL) return false;
    if (i < 0 || i >= table->ref_count) return false;
    const Reference* r = &table->references[i];
    out->name          = r->name;
    out->nameLength    = r->name_length;
    out->nameFileId    = r->name_file_id;
    out->nameStartByte = r->name_start_byte;
    out->useSpan       = r->use_span;
    out->symbolIndex   = r->symbol_index;
    out->isWrite       = r->is_write ? 1 : 0;
    return true;
}

bool zym_symbolTableFindSymbolAt(const ZymSymbolTable* table,
                                 int fileId, int byteOffset,
                                 ZymSymbolInfo* out)
{
    if (table == NULL || out == NULL) return false;
    for (int i = 0; i < table->count; i++) {
        const Symbol* s = &table->symbols[i];
        if (s->name_file_id != fileId) continue;
        if (byteOffset < s->name_start_byte) continue;
        if (byteOffset >= s->name_start_byte + s->name_length) continue;
        out->kind          = (ZymSymbolKind)s->kind;
        out->name          = s->name;
        out->nameLength    = s->name_length;
        out->nameFileId    = s->name_file_id;
        out->nameStartByte = s->name_start_byte;
        out->defSpan       = s->def_span;
        out->scopeDepth    = s->scope_depth;
        out->parentIndex   = s->parent_index;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------
// Phase 4.3a — public query API
// ---------------------------------------------------------------------

static void fill_symbol_info(const Symbol* s, ZymSymbolInfo* out) {
    out->kind          = (ZymSymbolKind)s->kind;
    out->name          = s->name;
    out->nameLength    = s->name_length;
    out->nameFileId    = s->name_file_id;
    out->nameStartByte = s->name_start_byte;
    out->defSpan       = s->def_span;
    out->scopeDepth    = s->scope_depth;
    out->parentIndex   = s->parent_index;
}

bool zymSymbolAtPosition(const ZymSymbolTable* table,
                         int fileId, int byteOffset,
                         ZymSymbolInfo* out)
{
    if (table == NULL || out == NULL) return false;
    // Pass 1: declarations (name span).
    for (int i = 0; i < table->count; i++) {
        const Symbol* s = &table->symbols[i];
        if (s->name_file_id != fileId) continue;
        if (byteOffset < s->name_start_byte) continue;
        if (byteOffset >= s->name_start_byte + s->name_length) continue;
        fill_symbol_info(s, out);
        return true;
    }
    // Pass 2: references (use span). Must bind to a real symbol.
    for (int i = 0; i < table->ref_count; i++) {
        const Reference* r = &table->references[i];
        if (r->name_file_id != fileId) continue;
        if (byteOffset < r->name_start_byte) continue;
        if (byteOffset >= r->name_start_byte + r->name_length) continue;
        if (r->symbol_index < 0 || r->symbol_index >= table->count) return false;
        fill_symbol_info(&table->symbols[r->symbol_index], out);
        return true;
    }
    return false;
}

int zymSymbolReferences(const ZymSymbolTable* table,
                        int symbolIndex,
                        ZymReferenceInfo* out_buf, int max_out)
{
    if (table == NULL) return -1;
    if (symbolIndex < 0 || symbolIndex >= table->count) return -1;
    int total = 0;
    for (int i = 0; i < table->ref_count; i++) {
        const Reference* r = &table->references[i];
        if (r->symbol_index != symbolIndex) continue;
        if (out_buf != NULL && total < max_out) {
            out_buf[total].name          = r->name;
            out_buf[total].nameLength    = r->name_length;
            out_buf[total].nameFileId    = r->name_file_id;
            out_buf[total].nameStartByte = r->name_start_byte;
            out_buf[total].useSpan       = r->use_span;
            out_buf[total].symbolIndex   = r->symbol_index;
            out_buf[total].isWrite       = r->is_write ? 1 : 0;
        }
        total++;
    }
    return total;
}

int zymListFileSymbols(const ZymSymbolTable* table,
                       int fileId,
                       ZymSymbolInfo* out_buf, int max_out)
{
    if (table == NULL) return -1;
    int total = 0;
    for (int i = 0; i < table->count; i++) {
        const Symbol* s = &table->symbols[i];
        if (s->name_file_id != fileId) continue;
        if (s->scope_depth != 0) continue;
        if (s->kind != SYMBOL_KIND_VAR &&
            s->kind != SYMBOL_KIND_FUNC &&
            s->kind != SYMBOL_KIND_STRUCT &&
            s->kind != SYMBOL_KIND_ENUM) continue;
        if (out_buf != NULL && total < max_out) {
            fill_symbol_info(s, &out_buf[total]);
        }
        total++;
    }
    return total;
}

// Returns true if every byte in [start, end) of `bytes` consists only
// of ASCII whitespace and/or identifier characters (letters, digits,
// underscore).
//
// The allowance for identifier characters is deliberate: the AST's
// `nodeSpan(stmt)` sometimes starts at the declaration's *name* token
// rather than its leading keyword (e.g. `func foo` starts at `foo`),
// which leaves `func ` sitting between a preceding trivia piece and
// `def_start`. Treating keywords + identifiers as "gap" content keeps
// the doc-attachment logic robust to that difference without forcing a
// nodeSpan refactor.
//
// This is safe because any non-whitespace non-identifier byte in the
// gap (punctuation, braces, operators) would necessarily indicate a
// real statement sitting between the trivia and the declaration, in
// which case the trivia shouldn't attach anyway.
static bool ws_or_keyword_only(const char* bytes, int length,
                               int start, int end) {
    if (start < 0 || end > length || start > end) return false;
    for (int i = start; i < end; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') continue;
        return false;
    }
    return true;
}

ZymDocumentationKind zymSymbolDocumentation(const ZymSymbolTable* table,
                                            const ZymParseTree* tree,
                                            int symbolIndex,
                                            int* out_fileId,
                                            int* out_startByte,
                                            int* out_length)
{
    if (table == NULL || tree == NULL) return ZYM_DOC_NONE;
    if (symbolIndex < 0 || symbolIndex >= table->count) return ZYM_DOC_NONE;
    const TriviaBuffer* tb = tree->trivia;
    if (tb == NULL || tb->count == 0) return ZYM_DOC_NONE;

    const Symbol* s = &table->symbols[symbolIndex];
    if (s->name == NULL || s->name_length <= 0) return ZYM_DOC_NONE;
    // Recover source-file base pointer + length. The symbol's name is
    // borrowed from SourceFile bytes, so bytes == name - name_start_byte.
    const char* source_bytes = s->name - s->name_start_byte;
    // Def begins at defSpan.startByte (preferred) or nameStartByte as fallback.
    int def_start = (s->def_span.fileId == s->name_file_id &&
                     s->def_span.startByte >= 0)
                        ? s->def_span.startByte
                        : s->name_start_byte;
    int fileId = s->name_file_id;

    // Find the trivia piece immediately before def_start (in this file).
    // Walk backwards from trivia_lower_bound(def_start); the previous
    // piece (if any) is the candidate.
    int lb = trivia_lower_bound(tb, def_start);
    int last_idx = -1;
    for (int i = lb - 1; i >= 0; i--) {
        const TriviaPiece* p = &tb->pieces[i];
        if (p->fileId != fileId) continue;
        if (p->startByte + p->length > def_start) continue;
        last_idx = i;
        break;
    }
    if (last_idx < 0) return ZYM_DOC_NONE;
    const TriviaPiece* last = &tb->pieces[last_idx];

    // Conservative source length bound: scan from the end of the last
    // trivia piece up to def_start — must be only whitespace. We don't
    // know the exact source buffer length here, but since both
    // last->startByte+length and def_start are valid offsets into the
    // same source file, we can safely read bytes in that range.
    int scan_len = def_start; // upper bound for only_whitespace's length guard
    int gap_start = last->startByte + last->length;
    if (!ws_or_keyword_only(source_bytes, scan_len, gap_start, def_start))
        return ZYM_DOC_NONE;

    if (last->kind == TRIVIA_DOC_BLOCK) {
        if (out_fileId)    *out_fileId = fileId;
        if (out_startByte) *out_startByte = last->startByte;
        if (out_length)    *out_length = last->length;
        return ZYM_DOC_BLOCK;
    }
    if (last->kind == TRIVIA_DOC_LINE) {
        // Walk backwards collecting a contiguous run of DOC_LINE pieces,
        // each separated from the next only by whitespace.
        int first_idx = last_idx;
        for (int i = last_idx - 1; i >= 0; i--) {
            const TriviaPiece* p = &tb->pieces[i];
            if (p->fileId != fileId) break;
            if (p->kind != TRIVIA_DOC_LINE) break;
            int this_end = p->startByte + p->length;
            int next_start = tb->pieces[first_idx].startByte;
            if (this_end > next_start) break;
            if (!ws_or_keyword_only(source_bytes, scan_len, this_end, next_start)) break;
            first_idx = i;
        }
        const TriviaPiece* first = &tb->pieces[first_idx];
        int total_len = (last->startByte + last->length) - first->startByte;
        if (out_fileId)    *out_fileId = fileId;
        if (out_startByte) *out_startByte = first->startByte;
        if (out_length)    *out_length = total_len;
        return ZYM_DOC_LINE;
    }
    // last was a plain comment — not a doc comment.
    return ZYM_DOC_NONE;
}
// ---------------------------------------------------------------------
// Phase 4.3b — zymSemanticTokens
// ---------------------------------------------------------------------
#include "./scanner.h"
#include "./trivia.h"
static ZymSemanticTokenKind classify_token_type(TokenType t) {
    switch (t) {
        // Punctuation.
        case TOKEN_LEFT_PAREN: case TOKEN_RIGHT_PAREN:
        case TOKEN_LEFT_BRACE: case TOKEN_RIGHT_BRACE:
        case TOKEN_LEFT_BRACKET: case TOKEN_RIGHT_BRACKET:
        case TOKEN_COLON: case TOKEN_COMMA: case TOKEN_SEMICOLON:
        case TOKEN_DOT: case TOKEN_DOT_DOT_DOT: case TOKEN_AT:
        case TOKEN_QUESTION:
            return ZYM_SEMTOK_PUNCTUATION;
        // Literals.
        case TOKEN_NUMBER: return ZYM_SEMTOK_NUMBER;
        case TOKEN_STRING: return ZYM_SEMTOK_STRING;
        case TOKEN_TRUE: case TOKEN_FALSE: case TOKEN_NULL:
            return ZYM_SEMTOK_BOOL_NULL;
        // Keywords.
        case TOKEN_AND: case TOKEN_BITWISE: case TOKEN_BREAK:
        case TOKEN_CASE: case TOKEN_CONTINUE: case TOKEN_DEFAULT:
        case TOKEN_DO: case TOKEN_ELSE: case TOKEN_ENUM:
        case TOKEN_FOR: case TOKEN_FUNC: case TOKEN_GOTO:
        case TOKEN_IF: case TOKEN_OR: case TOKEN_RETURN:
        case TOKEN_STRUCT: case TOKEN_SWITCH: case TOKEN_VAR:
        case TOKEN_WHILE:
            return ZYM_SEMTOK_KEYWORD;
        default:
            // Everything else (arithmetic, comparison, bitwise,
            // compound-assign, shifts, arrows, bang, equals) is an
            // operator.
            return ZYM_SEMTOK_OPERATOR;
    }
}
static ZymSemanticTokenKind symbol_kind_to_semtok(SymbolKind k) {
    switch (k) {
        case SYMBOL_KIND_FUNC:    return ZYM_SEMTOK_FUNCTION;
        case SYMBOL_KIND_VAR:     return ZYM_SEMTOK_VARIABLE;
        case SYMBOL_KIND_PARAM:   return ZYM_SEMTOK_PARAMETER;
        case SYMBOL_KIND_LOCAL:   return ZYM_SEMTOK_LOCAL;
        case SYMBOL_KIND_UPVALUE: return ZYM_SEMTOK_UPVALUE;
        case SYMBOL_KIND_STRUCT:  return ZYM_SEMTOK_STRUCT;
        case SYMBOL_KIND_ENUM:    return ZYM_SEMTOK_ENUM;
        case SYMBOL_KIND_FIELD:   return ZYM_SEMTOK_FIELD;
        case SYMBOL_KIND_VARIANT: return ZYM_SEMTOK_VARIANT;
        default:                  return ZYM_SEMTOK_IDENTIFIER;
    }
}
// Classify an identifier token by looking it up in the symbol table's
// declaring spans first, then its reference use spans. An identifier
// that matches neither is left as ZYM_SEMTOK_IDENTIFIER with modifiers
// == 0 (unresolved — e.g. member of a dynamic receiver, or an
// undeclared name).
static void classify_identifier_token(const ZymSymbolTable* table,
                                      int fileId, int startByte, int length,
                                      ZymSemanticTokenKind* out_kind,
                                      unsigned int* out_mods)
{
    *out_kind = ZYM_SEMTOK_IDENTIFIER;
    *out_mods = 0u;
    // Declaring span first: this caret is on the decl's identifier.
    for (int i = 0; i < table->count; i++) {
        const Symbol* s = &table->symbols[i];
        if (s->name_file_id == fileId &&
            s->name_start_byte == startByte &&
            s->name_length == length) {
            *out_kind = symbol_kind_to_semtok(s->kind);
            *out_mods |= ZYM_SEMTOK_MOD_DECLARATION;
            return;
        }
    }
    // Reference use span: a reader or writer of some bound symbol.
    for (int i = 0; i < table->ref_count; i++) {
        const Reference* r = &table->references[i];
        if (r->name_file_id != fileId) continue;
        if (r->name_start_byte != startByte) continue;
        if (r->name_length != length) continue;
        if (r->is_write) *out_mods |= ZYM_SEMTOK_MOD_WRITE;
        if (r->symbol_index >= 0 && r->symbol_index < table->count) {
            *out_kind = symbol_kind_to_semtok(table->symbols[r->symbol_index].kind);
        }
        return; // unresolved refs stay ZYM_SEMTOK_IDENTIFIER but keep the write bit
    }
}
// Compute end line/column of a trivia piece by walking its bytes. UTF-8
// bytes map 1:1 onto columns (scanner does the same — Phase 1.1 decision).
static void trivia_end_linecol(const char* bytes, int fileLen,
                               const TriviaPiece* p,
                               int* out_endLine, int* out_endCol)
{
    int line = p->startLine;
    int col  = p->startColumn;
    int end  = p->startByte + p->length;
    if (end > fileLen) end = fileLen;
    for (int i = p->startByte; i < end; i++) {
        if (bytes[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    *out_endLine = line;
    *out_endCol  = col;
}
static void emit_trivia(const TriviaPiece* p, const char* bytes, int fileLen,
                        ZymSemanticTokenInfo* out_buf, int max_out,
                        int* total)
{
    if (out_buf != NULL && *total < max_out) {
        ZymSemanticTokenInfo* e = &out_buf[*total];
        e->fileId      = p->fileId;
        e->startByte   = p->startByte;
        e->length      = p->length;
        e->startLine   = p->startLine;
        e->startColumn = p->startColumn;
        trivia_end_linecol(bytes, fileLen, p, &e->endLine, &e->endColumn);
        switch (p->kind) {
            case TRIVIA_DOC_LINE:
            case TRIVIA_DOC_BLOCK:
                e->kind = ZYM_SEMTOK_DOC_COMMENT;
                break;
            default:
                e->kind = ZYM_SEMTOK_COMMENT;
                break;
        }
        e->modifiers = 0u;
    }
    (*total)++;
}
int zymSemanticTokens(const ZymVM* vm,
                      const ZymSymbolTable* table,
                      const ZymParseTree* tree,
                      int fileId,
                      ZymSemanticTokenInfo* out_buf, int max_out)
{
    if (vm == NULL || table == NULL || tree == NULL) return -1;
    const SourceFile* sf = sfr_get(&((VM*)vm)->source_files, fileId);
    if (sf == NULL) return -1;
    Scanner scanner;
    initScanner(&scanner, sf->bytes, NULL, fileId);
    const TriviaBuffer* trivia = tree->trivia;
    // Skip any trivia pieces that belong to a different file, which can
    // happen when the parse tree was produced for a multi-file compile.
    int trivia_idx = 0;
    if (trivia != NULL) {
        while (trivia_idx < trivia->count &&
               trivia->pieces[trivia_idx].fileId != fileId) {
            trivia_idx++;
        }
    }
    int total = 0;
    for (;;) {
        Token tok = scanToken(&scanner);
        if (tok.type == TOKEN_EOF) break;
        // Emit all trivia pieces that begin before this token, in byte
        // order. We only emit pieces for the requested fileId; pieces
        // belonging to other files are skipped silently.
        if (trivia != NULL) {
            while (trivia_idx < trivia->count) {
                const TriviaPiece* p = &trivia->pieces[trivia_idx];
                if (p->fileId != fileId) { trivia_idx++; continue; }
                if (p->startByte >= tok.startByte) break;
                emit_trivia(p, sf->bytes, (int)sf->length,
                            out_buf, max_out, &total);
                trivia_idx++;
            }
        }
        if (tok.type == TOKEN_ERROR) continue;
        // Classify the scanner token.
        ZymSemanticTokenKind kind;
        unsigned int mods = 0u;
        if (tok.type == TOKEN_IDENTIFIER) {
            classify_identifier_token(table, fileId, tok.startByte,
                                      tok.length, &kind, &mods);
        } else {
            kind = classify_token_type(tok.type);
        }
        if (out_buf != NULL && total < max_out) {
            ZymSemanticTokenInfo* e = &out_buf[total];
            e->fileId      = fileId;
            e->startByte   = tok.startByte;
            e->length      = tok.length;
            e->startLine   = tok.startLine;
            e->startColumn = tok.startColumn;
            e->endLine     = tok.endLine;
            e->endColumn   = tok.endColumn;
            e->kind        = kind;
            e->modifiers   = mods;
        }
        total++;
    }
    // Emit any trailing trivia pieces (after the last scanner token).
    if (trivia != NULL) {
        while (trivia_idx < trivia->count) {
            const TriviaPiece* p = &trivia->pieces[trivia_idx];
            if (p->fileId == fileId) {
                emit_trivia(p, sf->bytes, (int)sf->length,
                            out_buf, max_out, &total);
            }
            trivia_idx++;
        }
    }
    return total;
}
#endif
#else

// ZYM_RUNTIME_ONLY stubs — registry/sourcemap are compile-surface only.
ZymFileId zym_registerSourceFile(ZymVM* vm, const char* path,
                                 const char* bytes, size_t length)
{
    (void)vm; (void)path; (void)bytes; (void)length;
    return ZYM_FILE_ID_INVALID;
}

int zym_getSourceFile(ZymVM* vm, ZymFileId fileId, ZymSourceFileInfo* out)
{
    (void)vm; (void)fileId; (void)out;
    return 0;
}

ZymSourceMap* zym_newSourceMap(ZymVM* vm) { (void)vm; return NULL; }
void zym_freeSourceMap(ZymVM* vm, ZymSourceMap* map) { (void)vm; (void)map; }

#endif

ZymStatus zym_runChunk(ZymVM* vm, ZymChunk* chunk)
{
    if (vm == NULL || chunk == NULL) return ZYM_STATUS_RUNTIME_ERROR;

    InterpretResult result = runChunk(vm, chunk);
    switch (result) {
        case INTERPRET_OK: return ZYM_STATUS_OK;
        case INTERPRET_RUNTIME_ERROR: return ZYM_STATUS_RUNTIME_ERROR;
        case INTERPRET_COMPILE_ERROR: return ZYM_STATUS_COMPILE_ERROR;
        case INTERPRET_YIELD: return ZYM_STATUS_YIELD;
        default: return ZYM_STATUS_RUNTIME_ERROR;
    }
}

ZymStatus zym_resume(ZymVM* vm)
{
    if (vm == NULL) return ZYM_STATUS_RUNTIME_ERROR;

    InterpretResult result = runVM(vm);
    switch (result) {
        case INTERPRET_OK: return ZYM_STATUS_OK;
        case INTERPRET_RUNTIME_ERROR: return ZYM_STATUS_RUNTIME_ERROR;
        case INTERPRET_COMPILE_ERROR: return ZYM_STATUS_COMPILE_ERROR;
        case INTERPRET_YIELD: return ZYM_STATUS_YIELD;
        default: return ZYM_STATUS_RUNTIME_ERROR;
    }
}

void zym_setPreemptCallback(ZymVM* vm, ZymValue callback)
{
    if (vm == NULL) return;
    vm->on_preempt_callback = callback;
}

#ifndef ZYM_RUNTIME_ONLY
ZymStatus zym_serializeChunk(ZymVM* vm, ZymCompilerConfig config, ZymChunk* chunk, char** out_buffer, size_t* out_size)
{
    if (vm == NULL || chunk == NULL || out_buffer == NULL || out_size == NULL) return ZYM_STATUS_COMPILE_ERROR;

    OutputBuffer temp_buffer;
    initOutputBuffer(&temp_buffer);
    serializeChunk(vm, chunk, config, &temp_buffer);
/*
    printf(" *** BYTECODE : %zu bytes ***\n", temp_buffer.count);
    printf(" *** BYTECODE START : %zu bytes ***\n", temp_buffer.count);
    for (size_t i = 0; i < temp_buffer.count; i++) {
        printf("%02x ", temp_buffer.buffer[i]);
    }
    printf("\n");
    printf(" *** BYTECODE END ***\n\n");
*/
    char* host_buffer = (char*)ZYM_ALLOC(&vm->allocator, temp_buffer.count);
    if (host_buffer == NULL) {
        freeOutputBuffer(vm, &temp_buffer);
        *out_buffer = NULL;
        *out_size = 0;
        return ZYM_STATUS_COMPILE_ERROR;
    }

    memcpy(host_buffer, temp_buffer.buffer, temp_buffer.count);

    *out_buffer = host_buffer;
    *out_size = temp_buffer.count;

    freeOutputBuffer(vm, &temp_buffer);

    return ZYM_STATUS_OK;
}
#endif

ZymStatus zym_deserializeChunk(ZymVM* vm, ZymChunk* chunk, const char* buffer, size_t size)
{
    if (vm == NULL || chunk == NULL || buffer == NULL) return ZYM_STATUS_COMPILE_ERROR;

    vm->chunk = chunk;

    bool success = deserializeChunk(vm, chunk, (const uint8_t*)buffer, size);
    return success ? ZYM_STATUS_OK : ZYM_STATUS_COMPILE_ERROR;
}

// =============================================================================
// NATIVE FUNCTION REGISTRATION
// =============================================================================

ZymStatus zym_defineNative(ZymVM* vm, const char* signature, void* func_ptr) {
    if (!vm || !signature || !func_ptr) {
        return ZYM_STATUS_COMPILE_ERROR;
    }

    bool success = registerNativeFunction(vm, signature, func_ptr);
    return success ? ZYM_STATUS_OK : ZYM_STATUS_COMPILE_ERROR;
}

ZymStatus zym_defineNativeVariadic(ZymVM* vm, const char* signature, void* func_ptr) {
    if (!vm || !signature || !func_ptr) {
        return ZYM_STATUS_COMPILE_ERROR;
    }

    bool success = registerNativeVariadicFunction(vm, signature, func_ptr);
    return success ? ZYM_STATUS_OK : ZYM_STATUS_COMPILE_ERROR;
}

ZymStatus zym_defineGlobal(ZymVM* vm, const char* name, ZymValue value) {
    if (!vm || !name) {
        return ZYM_STATUS_COMPILE_ERROR;
    }

    if (IS_OBJ(value)) pushTempRoot(vm, AS_OBJ(value));
    ObjString* nameStr = copyString(vm, name, (int)strlen(name));
    pushTempRoot(vm, (Obj*)nameStr);

    // GET_GLOBAL/SET_GLOBAL use a slot-based optimization where the
    // globals table stores a slot index (as DOUBLE_VAL) and the actual
    // value lives in vm->globalSlots. The direct-value path in vm.c is
    // reserved for native functions (registered under mangled names like
    // "print@1") and treats any direct-stored global as non-assignable.
    // To keep user-defined globals (numbers, strings, lists, maps, ...)
    // assignable from script, we always route them through globalSlots.
    Value existing;
    if (tableGet(&vm->globals, nameStr, &existing) && IS_DOUBLE(existing)) {
        // Existing slot-based global: update in place (matches SET_GLOBAL).
        int slot_index = (int)AS_DOUBLE(existing);
        vm->globalSlots.values[slot_index] = value;
    } else {
        // New global: allocate a fresh slot and store the slot index.
        int slot_index = vm->globalSlots.count;
        writeValueArray(vm, &vm->globalSlots, value);
        tableSet(vm, &vm->globals, nameStr, DOUBLE_VAL((double)slot_index));
    }

    popTempRoot(vm);
    if (IS_OBJ(value)) popTempRoot(vm);

    return ZYM_STATUS_OK;
}

// =============================================================================
// NATIVE CLOSURES
// =============================================================================

ZymValue zym_createNativeContext(ZymVM* vm, void* native_data, void (*finalizer)(ZymVM*, void*)) {
    if (!vm) return NULL_VAL;

    ObjNativeContext* context = newNativeContext(vm, native_data, finalizer);
    return OBJ_VAL(context);
}

void* zym_getNativeData(ZymValue context) {
    if (!IS_NATIVE_CONTEXT(context)) {
        return NULL;
    }

    ObjNativeContext* ctx = AS_NATIVE_CONTEXT(context);
    return ctx->native_data;
}

ZymValue zym_createNativeClosure(ZymVM* vm, const char* signature, void* func_ptr, ZymValue context) {
    if (!vm || !signature || !func_ptr) {
        return NULL_VAL;
    }

    if (!IS_NATIVE_CONTEXT(context)) {
        fprintf(stderr, "zym_createNativeClosure: context must be a native context\n");
        return NULL_VAL;
    }

    char func_name[256];
    int arity;

    if (!parseNativeSignature(signature, func_name, &arity)) {
        return NULL_VAL;
    }

    if (arity > MAX_NATIVE_ARITY) {
        fprintf(stderr, "Native closure '%s' has too many parameters (max %d)\n", func_name, MAX_NATIVE_ARITY);
        return NULL_VAL;
    }

    NativeDispatcher dispatcher = getNativeClosureDispatcher(arity);
    if (!dispatcher) {
        fprintf(stderr, "No closure dispatcher available for arity %d\n", arity);
        return NULL_VAL;
    }

    pushTempRoot(vm, AS_OBJ(context));
    ObjString* name_obj = copyString(vm, func_name, (int)strlen(func_name));
    pushTempRoot(vm, (Obj*)name_obj);
    ObjNativeClosure* closure = newNativeClosure(vm, name_obj, arity, func_ptr, dispatcher, context);
    popTempRoot(vm);
    popTempRoot(vm);

    return OBJ_VAL(closure);
}

ZymValue zym_createNativeClosureVariadic(ZymVM* vm, const char* signature, void* func_ptr, ZymValue context) {
    if (!vm || !signature || !func_ptr) {
        return NULL_VAL;
    }

    if (!IS_NATIVE_CONTEXT(context)) {
        fprintf(stderr, "zym_createNativeClosureVariadic: context must be a native context\n");
        return NULL_VAL;
    }

    char func_name[256];
    int fixed_arity;
    bool is_variadic;

    if (!parseNativeSignatureEx(signature, func_name, &fixed_arity, &is_variadic)) {
        return NULL_VAL;
    }

    if (!is_variadic) {
        fprintf(stderr, "zym_createNativeClosureVariadic: signature must contain '...'\n");
        return NULL_VAL;
    }

    NativeVariadicDispatcher vdispatcher = getNativeVariadicClosureDispatcher(fixed_arity);
    if (!vdispatcher) {
        fprintf(stderr, "No variadic closure dispatcher available for fixed arity %d\n", fixed_arity);
        return NULL_VAL;
    }

    pushTempRoot(vm, AS_OBJ(context));
    ObjString* name_obj = copyString(vm, func_name, (int)strlen(func_name));
    pushTempRoot(vm, (Obj*)name_obj);
    ObjNativeClosure* closure = newNativeClosure(vm, name_obj, fixed_arity, func_ptr, NULL, context);
    closure->variadic_dispatcher = vdispatcher;
    closure->is_variadic = true;
    popTempRoot(vm);
    popTempRoot(vm);

    return OBJ_VAL(closure);
}

ZymValue zym_getClosureContext(ZymValue closure) {
    if (!IS_OBJ(closure)) {
        return NULL_VAL;
    }

    Obj* obj = AS_OBJ(closure);
    if (obj->type != OBJ_NATIVE_CLOSURE) {
        return NULL_VAL;
    }

    ObjNativeClosure* native_closure = (ObjNativeClosure*)obj;
    return native_closure->context;
}

// =============================================================================
// FUNCTION OVERLOADING (DISPATCHER)
// =============================================================================

ZymValue zym_createDispatcher(ZymVM* vm) {
    if (!vm) {
        return NULL_VAL;
    }

    ObjDispatcher* dispatcher = newDispatcher(vm);
    return OBJ_VAL(dispatcher);
}

bool zym_addOverload(ZymVM* vm, ZymValue dispatcher, ZymValue closure) {
    if (!vm || !IS_DISPATCHER(dispatcher)) {
        return false;
    }

    if (!IS_OBJ(closure)) {
        return false;
    }

    Obj* obj = AS_OBJ(closure);
    if (obj->type != OBJ_CLOSURE && obj->type != OBJ_NATIVE_CLOSURE) {
        return false;
    }

    ObjDispatcher* disp = AS_DISPATCHER(dispatcher);

    if (disp->count >= MAX_OVERLOADS) {
        return false;
    }

    disp->overloads[disp->count++] = obj;
    return true;
}

bool zym_setVariadicFallback(ZymVM* vm, ZymValue dispatcher, ZymValue closure, int min_arity) {
    if (!vm || !IS_DISPATCHER(dispatcher)) {
        return false;
    }

    if (!IS_OBJ(closure)) {
        return false;
    }

    Obj* obj = AS_OBJ(closure);
    if (obj->type != OBJ_CLOSURE && obj->type != OBJ_NATIVE_CLOSURE) {
        return false;
    }

    ObjDispatcher* disp = AS_DISPATCHER(dispatcher);
    disp->variadic_fallback = obj;
    disp->variadic_min_arity = min_arity;
    return true;
}


// =============================================================================
// VALUE TYPE CHECKING
// =============================================================================

bool zym_isNull(ZymValue value) { return IS_NULL(value); }
bool zym_isBool(ZymValue value) { return IS_BOOL(value); }
bool zym_isNumber(ZymValue value) { return IS_DOUBLE(value); }
bool zym_isString(ZymValue value) { return IS_STRING(value); }
bool zym_isList(ZymValue value) { return IS_LIST(value); }
bool zym_isMap(ZymValue value) { return IS_MAP(value); }
bool zym_isStruct(ZymValue value) { return IS_STRUCT_INSTANCE(value); }
bool zym_isEnum(ZymValue value) { return IS_ENUM(value); }
bool zym_isFunction(ZymValue value) {
    return IS_FUNCTION(value) || IS_CLOSURE(value) || IS_NATIVE_FUNCTION(value) || IS_NATIVE_CLOSURE(value);
}
bool zym_isClosure(ZymValue value) { return IS_CLOSURE(value); }
bool zym_isPromptTag(ZymValue value) { return IS_OBJ(value) && IS_PROMPT_TAG(value); }
bool zym_isContinuation(ZymValue value) { return IS_OBJ(value) && IS_CONTINUATION(value); }

// =============================================================================
// VALUE EXTRACTION (SAFE)
// =============================================================================

bool zym_toBool(ZymValue value, bool* out) {
    if (!IS_BOOL(value)) return false;
    if (out) *out = AS_BOOL(value);
    return true;
}

bool zym_toNumber(ZymValue value, double* out) {
    if (!IS_DOUBLE(value)) return false;
    if (out) *out = AS_DOUBLE(value);
    return true;
}

bool zym_toString(ZymValue value, const char** out, int* length) {
    if (!IS_STRING(value)) return false;
    ObjString* str = AS_STRING(value);
    if (out) *out = str->chars;
    if (length) *length = str->length;  // UTF-8 character count
    return true;
}

bool zym_toStringBytes(ZymValue value, const char** out, int* byte_length) {
    if (!IS_STRING(value)) return false;
    ObjString* str = AS_STRING(value);
    if (out) *out = str->chars;
    if (byte_length) *byte_length = str->byte_length;  // Byte length
    return true;
}

// =============================================================================
// VALUE EXTRACTION (UNSAFE)
// =============================================================================

double zym_asNumber(ZymValue value) { return AS_DOUBLE(value); }
bool zym_asBool(ZymValue value) { return AS_BOOL(value); }
const char* zym_asCString(ZymValue value) { return AS_CSTRING(value); }

// =============================================================================
// VALUE INSPECTION
// =============================================================================

const char* zym_typeName(ZymValue value) {
    if (IS_NULL(value)) return "null";
    if (IS_BOOL(value)) return "bool";
    if (IS_DOUBLE(value)) return "number";
    if (IS_ENUM(value)) return "enum";
    if (IS_OBJ(value)) {
        Obj* obj = AS_OBJ(value);
        switch (obj->type) {
            case OBJ_STRING: return "string";
            case OBJ_LIST: return "list";
            case OBJ_MAP: return "map";
            case OBJ_FUNCTION: return "function";
            case OBJ_CLOSURE: return "closure";
            case OBJ_NATIVE_FUNCTION: return "native_function";
            case OBJ_NATIVE_CLOSURE: return "native_closure";
            case OBJ_NATIVE_CONTEXT: return "native_context";
            case OBJ_PROMPT_TAG: return "prompt_tag";
            case OBJ_CONTINUATION: return "continuation";
            case OBJ_STRUCT_SCHEMA: return "struct_schema";
            case OBJ_STRUCT_INSTANCE: return "struct";
            case OBJ_ENUM_SCHEMA: return "enum_schema";
            case OBJ_DISPATCHER: return "dispatcher";
            default: return "unknown";
        }
    }
    return "unknown";
}

int zym_stringLength(ZymValue value) {
    if (!IS_STRING(value)) return 0;
    return AS_STRING(value)->length;
}

int zym_stringByteLength(ZymValue value) {
    if (!IS_STRING(value)) return 0;
    return AS_STRING(value)->byte_length;
}

// =============================================================================
// VALUE DISPLAY
// =============================================================================

// Internal helper: write value to a dynamically growing buffer
static bool valueToStringHelper(VM* vm, Value value, char** buffer, size_t* buf_size, size_t* pos, Obj** visited, int depth) {
    char temp[256];

    if (depth >= 100) {
        size_t need = 3;
        while (*pos + need >= *buf_size) { size_t old = *buf_size; *buf_size *= 2; *buffer = ZYM_REALLOC(&vm->allocator, *buffer, old, *buf_size); if (!*buffer) return false; }
        memcpy(*buffer + *pos, "...", 3); *pos += 3;
        return true;
    }

#define APPEND(s, n) do { \
    size_t _n = (n); \
    while (*pos + _n >= *buf_size) { size_t _old = *buf_size; *buf_size *= 2; *buffer = ZYM_REALLOC(&vm->allocator, *buffer, _old, *buf_size); if (!*buffer) return false; } \
    memcpy(*buffer + *pos, (s), _n); *pos += _n; \
} while(0)

    if (IS_NULL(value)) {
        APPEND("null", 4);
    } else if (IS_BOOL(value)) {
        const char* s = AS_BOOL(value) ? "true" : "false";
        APPEND(s, strlen(s));
    } else if (IS_ENUM(value)) {
        int type_id = ENUM_TYPE_ID(value);
        int variant_idx = ENUM_VARIANT(value);
        if (vm != NULL) {
            ObjEnumSchema* schema = NULL;
            for (int i = 0; i < vm->globals.capacity; i++) {
                Entry* entry = &vm->globals.entries[i];
                if (entry->key == NULL) continue;
                Value ev = entry->value;
                if (IS_DOUBLE(ev)) {
                    int slot = (int)AS_DOUBLE(ev);
                    if (slot < 0 || slot >= vm->globalSlots.count) continue;
                    ev = vm->globalSlots.values[slot];
                }
                if (IS_OBJ(ev) && IS_ENUM_SCHEMA(ev)) {
                    ObjEnumSchema* candidate = AS_ENUM_SCHEMA(ev);
                    if (candidate->type_id == type_id) { schema = candidate; break; }
                }
            }
            if (schema != NULL && variant_idx >= 0 && variant_idx < schema->variant_count) {
                ObjString* vname = schema->variant_names[variant_idx];
                int len = snprintf(temp, sizeof(temp), "%.*s.%.*s",
                    schema->name->length, schema->name->chars,
                    vname->length, vname->chars);
                APPEND(temp, len);
            } else {
                int len = snprintf(temp, sizeof(temp), "<enum#%d.%d>", type_id, variant_idx);
                APPEND(temp, len);
            }
        } else {
            int len = snprintf(temp, sizeof(temp), "<enum#%d.%d>", type_id, variant_idx);
            APPEND(temp, len);
        }
    } else if (IS_DOUBLE(value)) {
        double num = AS_DOUBLE(value);
        int len;
        if (num == (long long)num && num >= -1e15 && num <= 1e15) {
            len = snprintf(temp, sizeof(temp), "%.0f", num);
        } else {
            len = snprintf(temp, sizeof(temp), "%g", num);
        }
        APPEND(temp, len);
    } else if (IS_OBJ(value)) {
        Obj* obj = AS_OBJ(value);

        for (int i = 0; i < depth; i++) {
            if (visited[i] == obj) { APPEND("...", 3); return true; }
        }
        visited[depth] = obj;

        switch (obj->type) {
            case OBJ_STRING: {
                ObjString* str = AS_STRING(value);
                APPEND(str->chars, str->byte_length);
                break;
            }
            case OBJ_LIST: {
                ObjList* list = AS_LIST(value);
                APPEND("[", 1);
                for (int i = 0; i < list->items.count; i++) {
                    if (i > 0) APPEND(", ", 2);
                    if (!valueToStringHelper(vm, list->items.values[i], buffer, buf_size, pos, visited, depth + 1)) return false;
                }
                APPEND("]", 1);
                break;
            }
            case OBJ_MAP: {
                ObjMap* map = AS_MAP(value);
                APPEND("{", 1);
                int printed = 0;
                for (int i = 0; i < map->table.capacity; i++) {
                    Entry* entry = &map->table.entries[i];
                    if (entry->key != NULL) {
                        if (printed > 0) APPEND(", ", 2);
                        APPEND("\"", 1);
                        APPEND(entry->key->chars, entry->key->byte_length);
                        APPEND("\": ", 3);
                        if (!valueToStringHelper(vm, entry->value, buffer, buf_size, pos, visited, depth + 1)) return false;
                        printed++;
                    }
                }
                APPEND("}", 1);
                break;
            }
            case OBJ_FUNCTION: {
                ObjFunction* fn = AS_FUNCTION(value);
                int len;
                if (fn->name) len = snprintf(temp, sizeof(temp), "<fn %.*s/%d>", fn->name->length, fn->name->chars, fn->arity);
                else len = snprintf(temp, sizeof(temp), "<fn /%d>", fn->arity);
                APPEND(temp, len);
                break;
            }
            case OBJ_CLOSURE: {
                ObjFunction* fn = AS_CLOSURE(value)->function;
                int len;
                if (fn->name) len = snprintf(temp, sizeof(temp), "<closure %.*s/%d>", fn->name->length, fn->name->chars, fn->arity);
                else len = snprintf(temp, sizeof(temp), "<closure /%d>", fn->arity);
                APPEND(temp, len);
                break;
            }
            case OBJ_NATIVE_FUNCTION: {
                ObjNativeFunction* native = AS_NATIVE_FUNCTION(value);
                int len;
                if (native->name) len = snprintf(temp, sizeof(temp), "<native fn %.*s/%d>", native->name->length, native->name->chars, native->arity);
                else len = snprintf(temp, sizeof(temp), "<native fn /%d>", native->arity);
                APPEND(temp, len);
                break;
            }
            case OBJ_NATIVE_CONTEXT: {
                APPEND("<native context>", 16);
                break;
            }
            case OBJ_NATIVE_CLOSURE: {
                ObjNativeClosure* closure = AS_NATIVE_CLOSURE(value);
                int len;
                if (closure->name) len = snprintf(temp, sizeof(temp), "<native closure %.*s/%d>", closure->name->length, closure->name->chars, closure->arity);
                else len = snprintf(temp, sizeof(temp), "<native closure /%d>", closure->arity);
                APPEND(temp, len);
                break;
            }
            case OBJ_STRUCT_INSTANCE: {
                ObjStructInstance* inst = AS_STRUCT_INSTANCE(value);
                ObjStructSchema* schema = inst->schema;
                APPEND(schema->name->chars, schema->name->byte_length);
                APPEND(" { ", 3);
                for (int i = 0; i < schema->field_count; i++) {
                    if (i > 0) APPEND(", ", 2);
                    APPEND(schema->field_names[i]->chars, schema->field_names[i]->byte_length);
                    APPEND(": ", 2);
                    if (!valueToStringHelper(vm, inst->fields[i], buffer, buf_size, pos, visited, depth + 1)) return false;
                }
                APPEND(" }", 2);
                break;
            }
            case OBJ_PROMPT_TAG: {
                ObjPromptTag* tag = AS_PROMPT_TAG(value);
                int len;
                if (tag->name != NULL) len = snprintf(temp, sizeof(temp), "<prompt_tag: %.*s>", tag->name->length, tag->name->chars);
                else len = snprintf(temp, sizeof(temp), "<prompt_tag #%" PRIu32 ">", tag->id);
                APPEND(temp, len);
                break;
            }
            case OBJ_CONTINUATION: {
                ObjContinuation* cont = AS_CONTINUATION(value);
                const char* state_str = "valid";
                if (cont->state == CONT_CONSUMED) state_str = "consumed";
                else if (cont->state == CONT_INVALID) state_str = "invalid";
                int len = snprintf(temp, sizeof(temp), "<continuation: %s>", state_str);
                APPEND(temp, len);
                break;
            }
            case OBJ_DISPATCHER: {
                APPEND("<dispatcher>", 12);
                break;
            }
            default: {
                int len = snprintf(temp, sizeof(temp), "<object>");
                APPEND(temp, len);
                break;
            }
        }
    } else {
        APPEND("<unknown>", 9);
    }

#undef APPEND
    return true;
}

ZymValue zym_valueToString(ZymVM* vm, ZymValue value) {
    if (vm == NULL) return ZYM_ERROR;

    size_t buf_size = 256;
    char* buffer = ZYM_ALLOC(&vm->allocator, buf_size);
    if (buffer == NULL) return ZYM_ERROR;

    size_t pos = 0;
    Obj* visited[100] = {0};

    if (!valueToStringHelper(vm, value, &buffer, &buf_size, &pos, visited, 0)) {
        ZYM_FREE(&vm->allocator, buffer, buf_size);
        return ZYM_ERROR;
    }

    buffer[pos] = '\0';
    ZymValue result = zym_newString(vm, buffer);
    ZYM_FREE(&vm->allocator, buffer, buf_size);
    return result;
}

void zym_printValue(ZymVM* vm, ZymValue value) {
    printValue(vm, value);
}

// =============================================================================
// VALUE CREATION
// =============================================================================

ZymValue zym_newNull(void) { return NULL_VAL; }
ZymValue zym_newBool(bool value) { return BOOL_VAL(value); }
ZymValue zym_newNumber(double value) { return DOUBLE_VAL(value); }

ZymValue zym_newString(ZymVM* vm, const char* str) {
    if (!vm || !str) return NULL_VAL;
    ObjString* obj = copyString(vm, str, (int)strlen(str));
    return OBJ_VAL(obj);
}

ZymValue zym_newStringN(ZymVM* vm, const char* str, int len) {
    if (!vm || !str || len < 0) return NULL_VAL;
    ObjString* obj = copyString(vm, str, len);
    return OBJ_VAL(obj);
}

ZymValue zym_newList(ZymVM* vm) {
    if (!vm) return NULL_VAL;
    ObjList* list = newList(vm);
    return OBJ_VAL(list);
}

ZymValue zym_newMap(ZymVM* vm) {
    if (!vm) return NULL_VAL;
    ObjMap* map = newMap(vm);
    return OBJ_VAL(map);
}

ZymValue zym_newStruct(ZymVM* vm, const char* structName) {
    if (!vm || !structName) return NULL_VAL;

    // Look up the struct schema in globals
    ObjString* name = copyString(vm, structName, (int)strlen(structName));
    Value schemaVal;
    if (!globalGet(vm, name, &schemaVal) || !IS_STRUCT_SCHEMA(schemaVal)) {
        return NULL_VAL;  // Schema not found
    }

    ObjStructSchema* schema = AS_STRUCT_SCHEMA(schemaVal);
    ObjStructInstance* instance = newStructInstance(vm, schema);
    return OBJ_VAL(instance);
}

ZymValue zym_newEnum(ZymVM* vm, const char* enumName, const char* variantName) {
    if (!vm || !enumName || !variantName) return NULL_VAL;

    // Look up the enum schema in globals
    ObjString* name = copyString(vm, enumName, (int)strlen(enumName));
    Value schemaVal;
    if (!globalGet(vm, name, &schemaVal) || !IS_ENUM_SCHEMA(schemaVal)) {
        return NULL_VAL;  // Schema not found
    }

    ObjEnumSchema* schema = AS_ENUM_SCHEMA(schemaVal);

    // Find variant index
    int variantIndex = -1;
    for (int i = 0; i < schema->variant_count; i++) {
        if (strcmp(schema->variant_names[i]->chars, variantName) == 0) {
            variantIndex = i;
            break;
        }
    }

    if (variantIndex == -1) {
        return NULL_VAL;  // Variant not found
    }

    return ENUM_VAL(schema->type_id, variantIndex);
}

// =============================================================================
// LIST OPERATIONS
// =============================================================================

int zym_listLength(ZymValue list) {
    if (!IS_LIST(list)) return -1;
    ObjList* lst = AS_LIST(list);
    return lst->items.count;
}

ZymValue zym_listGet(ZymVM* vm, ZymValue list, int index) {
    if (!IS_LIST(list)) return ZYM_ERROR;
    ObjList* lst = AS_LIST(list);
    if (index < 0 || index >= lst->items.count) return ZYM_ERROR;
    return lst->items.values[index];
}

bool zym_listSet(ZymVM* vm, ZymValue list, int index, ZymValue val) {
    if (!IS_LIST(list)) return false;
    ObjList* lst = AS_LIST(list);
    if (index < 0 || index >= lst->items.count) return false;
    lst->items.values[index] = val;
    return true;
}

bool zym_listAppend(ZymVM* vm, ZymValue list, ZymValue val) {
    if (!IS_LIST(list)) return false;
    ObjList* lst = AS_LIST(list);
    writeValueArray(vm, &lst->items, val);
    return true;
}

bool zym_listInsert(ZymVM* vm, ZymValue list, int index, ZymValue val) {
    if (!IS_LIST(list)) return false;
    ObjList* lst = AS_LIST(list);
    if (index < 0 || index > lst->items.count) return false;

    if (lst->items.count >= lst->items.capacity) {
        int oldCapacity = lst->items.capacity;
        int newCapacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        lst->items.values = (Value*)reallocate(vm, lst->items.values,
                                               sizeof(Value) * oldCapacity,
                                               sizeof(Value) * newCapacity);
        lst->items.capacity = newCapacity;
    }

    for (int i = lst->items.count; i > index; i--) {
        lst->items.values[i] = lst->items.values[i - 1];
    }

    lst->items.values[index] = val;
    lst->items.count++;
    return true;
}

bool zym_listRemove(ZymVM* vm, ZymValue list, int index) {
    if (!IS_LIST(list)) return false;
    ObjList* lst = AS_LIST(list);
    if (index < 0 || index >= lst->items.count) return false;

    for (int i = index; i < lst->items.count - 1; i++) {
        lst->items.values[i] = lst->items.values[i + 1];
    }

    lst->items.count--;
    return true;
}

// =============================================================================
// MAP OPERATIONS
// =============================================================================

int zym_mapSize(ZymValue map) {
    if (!IS_MAP(map)) return -1;
    ObjMap* m = AS_MAP(map);
    return m->table.count;
}

ZymValue zym_mapGet(ZymVM* vm, ZymValue map, const char* key) {
    if (!IS_MAP(map) || !key) return ZYM_ERROR;
    ObjMap* m = AS_MAP(map);
    ObjString* keyStr = copyString(vm, key, (int)strlen(key));

    Value result;
    if (!tableGet(&m->table, keyStr, &result)) {
        return ZYM_ERROR;
    }
    return result;
}

bool zym_mapSet(ZymVM* vm, ZymValue map, const char* key, ZymValue val) {
    if (!IS_MAP(map) || !key) return false;
    ObjMap* m = AS_MAP(map);
    ObjString* keyStr = copyString(vm, key, (int)strlen(key));
    tableSet(vm, &m->table, keyStr, val);
    return true;
}

bool zym_mapHas(ZymValue map, const char* key) {
    if (!IS_MAP(map) || !key) return false;
    ObjMap* m = AS_MAP(map);

    uint32_t hash = 0;
    int len = (int)strlen(key);
    for (int i = 0; i < len; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }

    ObjString* keyStr = tableFindString(&m->table, key, len, hash);
    if (!keyStr) return false;

    Value dummy;
    return tableGet(&m->table, keyStr, &dummy);
}

bool zym_mapDelete(ZymVM* vm, ZymValue map, const char* key) {
    if (!IS_MAP(map) || !key) return false;
    ObjMap* m = AS_MAP(map);

    uint32_t hash = 0;
    int len = (int)strlen(key);
    for (int i = 0; i < len; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }

    ObjString* keyStr = tableFindString(&m->table, key, len, hash);
    if (!keyStr) return false;

    return tableDelete(&m->table, keyStr);
}

void zym_mapForEach(ZymVM* vm, ZymValue map, ZymMapIterFunc func, void* userdata) {
    if (!IS_MAP(map) || !func) return;
    ObjMap* m = AS_MAP(map);

    for (int i = 0; i < m->table.capacity; i++) {
        Entry* entry = &m->table.entries[i];
        if (entry->key != NULL) {
            bool shouldContinue = func(vm, entry->key->chars, entry->value, userdata);
            if (!shouldContinue) break;
        }
    }
}

// =============================================================================
// STRUCT OPERATIONS
// =============================================================================

ZymValue zym_structGet(ZymVM* vm, ZymValue structVal, const char* fieldName) {
    if (!IS_STRUCT_INSTANCE(structVal) || !fieldName) return ZYM_ERROR;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);
    ObjString* fieldStr = copyString(vm, fieldName, (int)strlen(fieldName));

    int index = find_field_index(inst->schema, fieldStr);
    if (index < 0 || index >= inst->field_count) return ZYM_ERROR;

    return inst->fields[index];
}

bool zym_structSet(ZymVM* vm, ZymValue structVal, const char* fieldName, ZymValue val) {
    if (!IS_STRUCT_INSTANCE(structVal) || !fieldName) return false;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);
    ObjString* fieldStr = copyString(vm, fieldName, (int)strlen(fieldName));

    int index = find_field_index(inst->schema, fieldStr);
    if (index < 0 || index >= inst->field_count) return false;

    inst->fields[index] = val;
    return true;
}

bool zym_structHasField(ZymValue structVal, const char* fieldName) {
    if (!IS_STRUCT_INSTANCE(structVal) || !fieldName) return false;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);

    for (int i = 0; i < inst->schema->field_count; i++) {
        if (strcmp(inst->schema->field_names[i]->chars, fieldName) == 0) {
            return true;
        }
    }
    return false;
}

const char* zym_structGetName(ZymValue structVal) {
    if (!IS_STRUCT_INSTANCE(structVal)) return NULL;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);
    return inst->schema->name->chars;
}

int zym_structFieldCount(ZymValue structVal) {
    if (!IS_STRUCT_INSTANCE(structVal)) return -1;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);
    return inst->field_count;
}

const char* zym_structFieldNameAt(ZymValue structVal, int index) {
    if (!IS_STRUCT_INSTANCE(structVal)) return NULL;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);
    if (index < 0 || index >= inst->field_count) return NULL;
    return inst->schema->field_names[index]->chars;
}

// =============================================================================
// ENUM OPERATIONS
// =============================================================================

const char* zym_enumGetName(ZymVM* vm, ZymValue enumVal) {
    if (!IS_ENUM(enumVal)) return NULL;
    int type_id = ENUM_TYPE_ID(enumVal);

    for (int i = 0; i < vm->globals.capacity; i++) {
        Entry* entry = &vm->globals.entries[i];
        if (entry->key == NULL) continue;
        Value v = entry->value;
        if (IS_DOUBLE(v)) {
            int slot = (int)AS_DOUBLE(v);
            if (slot < 0 || slot >= vm->globalSlots.count) continue;
            v = vm->globalSlots.values[slot];
        }
        if (IS_OBJ(v) && IS_ENUM_SCHEMA(v)) {
            ObjEnumSchema* schema = AS_ENUM_SCHEMA(v);
            if (schema->type_id == type_id) {
                return schema->name->chars;
            }
        }
    }
    return NULL;
}

const char* zym_enumGetVariant(ZymVM* vm, ZymValue enumVal) {
    if (!IS_ENUM(enumVal)) return NULL;
    int type_id = ENUM_TYPE_ID(enumVal);
    int variant = ENUM_VARIANT(enumVal);

    for (int i = 0; i < vm->globals.capacity; i++) {
        Entry* entry = &vm->globals.entries[i];
        if (entry->key == NULL) continue;
        Value v = entry->value;
        if (IS_DOUBLE(v)) {
            int slot = (int)AS_DOUBLE(v);
            if (slot < 0 || slot >= vm->globalSlots.count) continue;
            v = vm->globalSlots.values[slot];
        }
        if (IS_OBJ(v) && IS_ENUM_SCHEMA(v)) {
            ObjEnumSchema* schema = AS_ENUM_SCHEMA(v);
            if (schema->type_id == type_id) {
                if (variant >= 0 && variant < schema->variant_count) {
                    return schema->variant_names[variant]->chars;
                }
                return NULL;
            }
        }
    }
    return NULL;
}

bool zym_enumEquals(ZymValue a, ZymValue b) {
    if (!IS_ENUM(a) || !IS_ENUM(b)) return false;
    return ENUM_TYPE_ID(a) == ENUM_TYPE_ID(b) && ENUM_VARIANT(a) == ENUM_VARIANT(b);
}

int zym_enumVariantIndex(ZymVM* vm, ZymValue enumVal) {
    if (!IS_ENUM(enumVal)) return -1;
    return ENUM_VARIANT(enumVal);
}


// =============================================================================
// CALLING SCRIPT FUNCTIONS FROM C
// =============================================================================

bool zym_hasFunction(ZymVM* vm, const char* funcName, int arity) {
    if (!vm || !funcName) return false;

    char mangled[256];
    snprintf(mangled, sizeof(mangled), "%s@%d", funcName, arity);

    ObjString* nameObj = copyString(vm, mangled, (int)strlen(mangled));
    Value funcVal;

    return globalGet(vm, nameObj, &funcVal) &&
           (IS_CLOSURE(funcVal) || IS_NATIVE_FUNCTION(funcVal));
}

ZymStatus zym_callv(ZymVM* vm, const char* funcName, int argc, ZymValue* argv) {
    if (!vm || !funcName) return ZYM_STATUS_RUNTIME_ERROR;

    bool success = zym_call_prepare(vm, funcName, argc);
    if (!success) return ZYM_STATUS_RUNTIME_ERROR;

    for (int i = 0; i < argc; i++) {
        vm->stack[vm->api_stack_top + 1 + i] = argv[i];
    }
    vm->api_stack_top += argc;

    InterpretResult result = zym_call_execute(vm, argc);
    switch (result) {
        case INTERPRET_OK: return ZYM_STATUS_OK;
        case INTERPRET_RUNTIME_ERROR: return ZYM_STATUS_RUNTIME_ERROR;
        case INTERPRET_YIELD: return ZYM_STATUS_YIELD;
        default: return ZYM_STATUS_RUNTIME_ERROR;
    }
}

ZymStatus zym_callClosurev(ZymVM* vm, ZymValue closure, int argc, ZymValue* argv) {
    if (!vm) return ZYM_STATUS_RUNTIME_ERROR;

    // Mirror zym_callv's convention: the host API call frame always starts at
    // stack slot 0. Using vm->stack_top as the base would leak upward every
    // invocation (stack_top never shrinks), eventually exhausting the stack.
    vm->api_stack_top = 0;
    vm->stack[0] = closure;
    for (int i = 0; i < argc; i++) {
        vm->stack[1 + i] = argv[i];
    }
    vm->api_stack_top = argc;

    InterpretResult result = zym_call_execute(vm, argc);
    // zym_call_execute leaves the result at stack[0] and sets api_stack_top=0
    // so zym_getCallResult can read it.

    switch (result) {
        case INTERPRET_OK: return ZYM_STATUS_OK;
        case INTERPRET_RUNTIME_ERROR: return ZYM_STATUS_RUNTIME_ERROR;
        case INTERPRET_YIELD: return ZYM_STATUS_YIELD;
        default: return ZYM_STATUS_RUNTIME_ERROR;
    }
}

ZymStatus zym_call(ZymVM* vm, const char* funcName, int argc, ...) {
    if (!vm || !funcName) return ZYM_STATUS_RUNTIME_ERROR;

    ZymValue* args = NULL;
    if (argc > 0) {
        args = (ZymValue*)ZYM_ALLOC(&vm->allocator, sizeof(ZymValue) * argc);
        if (!args) return ZYM_STATUS_RUNTIME_ERROR;

        va_list ap;
        va_start(ap, argc);
        for (int i = 0; i < argc; i++) {
            args[i] = va_arg(ap, ZymValue);
        }
        va_end(ap);
    }

    ZymStatus result = zym_callv(vm, funcName, argc, args);

    if (args) ZYM_FREE(&vm->allocator, args, sizeof(ZymValue) * argc);
    return result;
}

ZymValue zym_getCallResult(ZymVM* vm) {
    if (!vm) return NULL_VAL;
    return zym_call_getResult(vm);
}

// =============================================================================
// GC PROTECTION (TEMPORARY ROOTS)
// =============================================================================

void zym_pushRoot(ZymVM* vm, ZymValue val) {
    if (!vm || !IS_OBJ(val)) return;
    pushTempRoot(vm, AS_OBJ(val));
}

void zym_popRoot(ZymVM* vm) {
    if (!vm) return;
    popTempRoot(vm);
}

ZymValue zym_peekRoot(ZymVM* vm, int depth) {
    if (!vm || depth < 0 || depth >= vm->temp_root_count) return NULL_VAL;
    int index = vm->temp_root_count - 1 - depth;
    return OBJ_VAL(vm->temp_roots[index]);
}

// =============================================================================
// ERROR HANDLING
// =============================================================================

void zym_runtimeError(ZymVM* vm, const char* format, ...) {
    if (!vm || !format) return;

    va_list args;
    va_start(args, format);
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    runtimeError((VM*)vm, "%s", buffer);
}

