#include "./node_span.h"

#include <stddef.h>
#include <string.h>

// A Token is considered "real" (i.e. usable for span reconstruction)
// when the scanner populated its byte-offset span. Tokens
// zero-initialized by implicit struct layout — or synthesized by a
// node that never received a real bracketing token — carry fileId=0
// and length=0, which is indistinguishable from a scanned empty
// token. We therefore require a non-zero length to accept a token as
// "real", and additionally require startByte >= 0.
static bool token_is_real(const Token* t) {
    return t != NULL && t->length > 0 && t->startByte >= 0;
}

static Token zero_token(void) {
    Token t;
    memset(&t, 0, sizeof(Token));
    t.fileId          = -1;
    t.startByte       = -1;
    t.startLine       = -1;
    t.startColumn     = -1;
    t.endLine         = -1;
    t.endColumn       = -1;
    t.originFileId    = -1;
    t.originStartByte = -1;
    t.originLength    = 0;
    return t;
}

// ---- Expr: first/last token -----------------------------------------

Token firstTokenOfExpr(const Expr* e, bool* ok) {
    if (ok) *ok = false;
    if (e == NULL) return zero_token();

    Token t = zero_token();
    bool  child_ok = false;

    switch (e->type) {
    case EXPR_ASSIGN:
        t = firstTokenOfExpr(e->as.assign.target, &child_ok);
        break;
    case EXPR_BINARY:
        t = firstTokenOfExpr(e->as.binary.left, &child_ok);
        if (!child_ok) { t = e->as.binary.operator; child_ok = token_is_real(&t); }
        break;
    case EXPR_CALL:
        t = firstTokenOfExpr(e->as.call.callee, &child_ok);
        if (!child_ok) { t = e->as.call.paren; child_ok = token_is_real(&t); }
        break;
    case EXPR_GET:
        t = firstTokenOfExpr(e->as.get.object, &child_ok);
        if (!child_ok) { t = e->as.get.name; child_ok = token_is_real(&t); }
        break;
    case EXPR_SET:
        t = firstTokenOfExpr(e->as.set.object, &child_ok);
        if (!child_ok) { t = e->as.set.name; child_ok = token_is_real(&t); }
        break;
    case EXPR_UNARY:
        t = e->as.unary.operator; child_ok = token_is_real(&t);
        if (!child_ok) t = firstTokenOfExpr(e->as.unary.right, &child_ok);
        break;
    case EXPR_LITERAL:
        t = e->as.literal.literal; child_ok = token_is_real(&t);
        break;
    case EXPR_GROUPING:
        // Opening paren isn't stored — recurse into inner expression.
        t = firstTokenOfExpr(e->as.grouping.expression, &child_ok);
        break;
    case EXPR_VARIABLE:
        t = e->as.variable.name; child_ok = token_is_real(&t);
        break;
    case EXPR_LIST:
        // Opening bracket isn't stored — recurse into first element.
        if (e->as.list.count > 0) {
            t = firstTokenOfExpr(e->as.list.elements[0], &child_ok);
        }
        break;
    case EXPR_SUBSCRIPT:
        t = firstTokenOfExpr(e->as.subscript.object, &child_ok);
        if (!child_ok) { t = e->as.subscript.bracket; child_ok = token_is_real(&t); }
        break;
    case EXPR_MAP:
        if (e->as.map.count > 0) {
            t = firstTokenOfExpr(e->as.map.keys[0], &child_ok);
        }
        break;
    case EXPR_FUNCTION:
        // `func` keyword token isn't stored on FunctionExpr; best we can
        // do is recurse into the first param or body.
        if (e->as.function.param_count > 0) {
            t = e->as.function.params[0].name;
            child_ok = token_is_real(&t);
        }
        if (!child_ok && e->as.function.body != NULL) {
            t = firstTokenOfStmt(e->as.function.body, &child_ok);
        }
        break;
    case EXPR_STRUCT_INST:
        t = e->as.struct_inst.struct_name; child_ok = token_is_real(&t);
        break;
    case EXPR_TERNARY:
        t = firstTokenOfExpr(e->as.ternary.condition, &child_ok);
        break;
    case EXPR_PRE_INC:
    case EXPR_PRE_DEC:
        // ++/-- token isn't stored on the node; recurse into target and
        // the caller gets at worst a span starting at the target — which
        // is what degraded-but-useful looks like for these prefix ops.
        t = firstTokenOfExpr(
                e->type == EXPR_PRE_INC ? e->as.pre_inc.target
                                        : e->as.pre_dec.target,
                &child_ok);
        break;
    case EXPR_POST_INC:
    case EXPR_POST_DEC:
        t = firstTokenOfExpr(
                e->type == EXPR_POST_INC ? e->as.post_inc.target
                                         : e->as.post_dec.target,
                &child_ok);
        break;
    case EXPR_SPREAD:
        t = firstTokenOfExpr(e->as.spread.expression, &child_ok);
        break;
    case EXPR_ERROR:
        t = e->as.error.start_token; child_ok = token_is_real(&t);
        break;
    }

    if (ok) *ok = child_ok;
    return t;
}

Token lastTokenOfExpr(const Expr* e, bool* ok) {
    if (ok) *ok = false;
    if (e == NULL) return zero_token();

    Token t = zero_token();
    bool  child_ok = false;

    switch (e->type) {
    case EXPR_ASSIGN:
        t = lastTokenOfExpr(e->as.assign.value, &child_ok);
        if (!child_ok) t = lastTokenOfExpr(e->as.assign.target, &child_ok);
        break;
    case EXPR_BINARY:
        t = lastTokenOfExpr(e->as.binary.right, &child_ok);
        if (!child_ok) { t = e->as.binary.operator; child_ok = token_is_real(&t); }
        break;
    case EXPR_CALL:
        // `paren` on CallExpr is the closing paren — ideal end.
        t = e->as.call.paren; child_ok = token_is_real(&t);
        if (!child_ok) t = lastTokenOfExpr(e->as.call.callee, &child_ok);
        break;
    case EXPR_GET:
        t = e->as.get.name; child_ok = token_is_real(&t);
        break;
    case EXPR_SET:
        t = lastTokenOfExpr(e->as.set.value, &child_ok);
        if (!child_ok) { t = e->as.set.name; child_ok = token_is_real(&t); }
        break;
    case EXPR_UNARY:
        t = lastTokenOfExpr(e->as.unary.right, &child_ok);
        if (!child_ok) { t = e->as.unary.operator; child_ok = token_is_real(&t); }
        break;
    case EXPR_LITERAL:
        t = e->as.literal.literal; child_ok = token_is_real(&t);
        break;
    case EXPR_GROUPING:
        t = lastTokenOfExpr(e->as.grouping.expression, &child_ok);
        break;
    case EXPR_VARIABLE:
        t = e->as.variable.name; child_ok = token_is_real(&t);
        break;
    case EXPR_LIST:
        if (e->as.list.count > 0) {
            t = lastTokenOfExpr(e->as.list.elements[e->as.list.count - 1],
                                &child_ok);
        }
        break;
    case EXPR_SUBSCRIPT:
        // `bracket` on SubscriptExpr is the closing ']'.
        t = e->as.subscript.bracket; child_ok = token_is_real(&t);
        if (!child_ok) t = lastTokenOfExpr(e->as.subscript.index, &child_ok);
        break;
    case EXPR_MAP:
        if (e->as.map.count > 0) {
            t = lastTokenOfExpr(e->as.map.values[e->as.map.count - 1],
                                &child_ok);
        }
        break;
    case EXPR_FUNCTION:
        if (e->as.function.body != NULL) {
            t = lastTokenOfStmt(e->as.function.body, &child_ok);
        }
        break;
    case EXPR_STRUCT_INST:
        if (e->as.struct_inst.field_count > 0) {
            t = lastTokenOfExpr(
                    e->as.struct_inst.field_values
                        [e->as.struct_inst.field_count - 1],
                    &child_ok);
        }
        if (!child_ok) { t = e->as.struct_inst.struct_name; child_ok = token_is_real(&t); }
        break;
    case EXPR_TERNARY:
        t = lastTokenOfExpr(e->as.ternary.else_expr, &child_ok);
        break;
    case EXPR_PRE_INC:
    case EXPR_PRE_DEC:
        t = lastTokenOfExpr(
                e->type == EXPR_PRE_INC ? e->as.pre_inc.target
                                        : e->as.pre_dec.target,
                &child_ok);
        break;
    case EXPR_POST_INC:
    case EXPR_POST_DEC:
        // ++/-- token isn't stored — recurse into target. Caller may
        // under-report the trailing two bytes but never over-reports.
        t = lastTokenOfExpr(
                e->type == EXPR_POST_INC ? e->as.post_inc.target
                                         : e->as.post_dec.target,
                &child_ok);
        break;
    case EXPR_SPREAD:
        t = lastTokenOfExpr(e->as.spread.expression, &child_ok);
        break;
    case EXPR_ERROR:
        t = e->as.error.end_token; child_ok = token_is_real(&t);
        if (!child_ok) { t = e->as.error.start_token; child_ok = token_is_real(&t); }
        break;
    }

    if (ok) *ok = child_ok;
    return t;
}

// ---- Stmt: first/last token -----------------------------------------

static Token first_stmt_array(Stmt** stmts, int count, bool* ok) {
    for (int i = 0; i < count; i++) {
        bool child_ok = false;
        Token t = firstTokenOfStmt(stmts[i], &child_ok);
        if (child_ok) { if (ok) *ok = true; return t; }
    }
    if (ok) *ok = false;
    return zero_token();
}

static Token last_stmt_array(Stmt** stmts, int count, bool* ok) {
    for (int i = count - 1; i >= 0; i--) {
        bool child_ok = false;
        Token t = lastTokenOfStmt(stmts[i], &child_ok);
        if (child_ok) { if (ok) *ok = true; return t; }
    }
    if (ok) *ok = false;
    return zero_token();
}

Token firstTokenOfStmt(const Stmt* s, bool* ok) {
    if (ok) *ok = false;
    if (s == NULL) return zero_token();

    Token t = zero_token();
    bool  child_ok = false;

    // For most statement kinds the scanner-populated `keyword` field is
    // the cleanest start — `var`, `if`, `while`, `for`, `return`,
    // `break`, `continue`, `goto`, `struct`, `enum`, `switch`. Fall back
    // to per-kind children when keyword is zero-initialized.
    if (s->type != STMT_EXPRESSION && s->type != STMT_ERROR) {
        t = s->keyword;
        child_ok = token_is_real(&t);
    }

    if (!child_ok) {
        switch (s->type) {
        case STMT_EXPRESSION:
            t = firstTokenOfExpr(s->as.expression.expression, &child_ok);
            break;
        case STMT_VAR_DECLARATION:
            if (s->as.var_declaration.count > 0) {
                t = s->as.var_declaration.variables[0].name;
                child_ok = token_is_real(&t);
            }
            break;
        case STMT_BLOCK:
            t = first_stmt_array(s->as.block.statements,
                                 s->as.block.count, &child_ok);
            break;
        case STMT_IF:
            t = firstTokenOfExpr(s->as.if_stmt.condition, &child_ok);
            break;
        case STMT_WHILE:
            t = firstTokenOfExpr(s->as.while_stmt.condition, &child_ok);
            break;
        case STMT_DO_WHILE:
            t = firstTokenOfStmt(s->as.do_while_stmt.body, &child_ok);
            break;
        case STMT_FOR:
            t = firstTokenOfStmt(s->as.for_stmt.initializer, &child_ok);
            break;
        case STMT_FUNC_DECLARATION:
            t = s->as.func_declaration.name;
            child_ok = token_is_real(&t);
            break;
        case STMT_RETURN:
            t = firstTokenOfExpr(s->as.return_stmt.value, &child_ok);
            break;
        case STMT_COMPILER_DIRECTIVE:
            t = s->as.compiler_directive.argument;
            child_ok = token_is_real(&t);
            break;
        case STMT_STRUCT_DECLARATION:
            t = s->as.struct_declaration.name;
            child_ok = token_is_real(&t);
            break;
        case STMT_ENUM_DECLARATION:
            t = s->as.enum_declaration.name;
            child_ok = token_is_real(&t);
            break;
        case STMT_LABEL:
            t = s->as.label.label_name;
            child_ok = token_is_real(&t);
            break;
        case STMT_GOTO:
            t = s->as.goto_stmt.target_label;
            child_ok = token_is_real(&t);
            break;
        case STMT_SWITCH:
            t = firstTokenOfExpr(s->as.switch_stmt.expression, &child_ok);
            break;
        case STMT_ERROR:
            t = s->as.error.start_token;
            child_ok = token_is_real(&t);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;  // keyword-only; already tried above
        }
    }

    if (ok) *ok = child_ok;
    return t;
}

Token lastTokenOfStmt(const Stmt* s, bool* ok) {
    if (ok) *ok = false;
    if (s == NULL) return zero_token();

    Token t = zero_token();
    bool  child_ok = false;

    switch (s->type) {
    case STMT_EXPRESSION:
        t = lastTokenOfExpr(s->as.expression.expression, &child_ok);
        break;
    case STMT_VAR_DECLARATION: {
        int n = s->as.var_declaration.count;
        if (n > 0) {
            VarDecl* last = &s->as.var_declaration.variables[n - 1];
            if (last->initializer != NULL) {
                t = lastTokenOfExpr(last->initializer, &child_ok);
            }
            if (!child_ok) { t = last->name; child_ok = token_is_real(&t); }
        }
        break;
    }
    case STMT_BLOCK:
        t = last_stmt_array(s->as.block.statements,
                            s->as.block.count, &child_ok);
        break;
    case STMT_IF:
        if (s->as.if_stmt.else_branch != NULL) {
            t = lastTokenOfStmt(s->as.if_stmt.else_branch, &child_ok);
        }
        if (!child_ok) t = lastTokenOfStmt(s->as.if_stmt.then_branch, &child_ok);
        if (!child_ok) t = lastTokenOfExpr(s->as.if_stmt.condition, &child_ok);
        break;
    case STMT_WHILE:
        t = lastTokenOfStmt(s->as.while_stmt.body, &child_ok);
        if (!child_ok) t = lastTokenOfExpr(s->as.while_stmt.condition, &child_ok);
        break;
    case STMT_DO_WHILE:
        t = lastTokenOfExpr(s->as.do_while_stmt.condition, &child_ok);
        if (!child_ok) t = lastTokenOfStmt(s->as.do_while_stmt.body, &child_ok);
        break;
    case STMT_FOR:
        t = lastTokenOfStmt(s->as.for_stmt.body, &child_ok);
        if (!child_ok) t = lastTokenOfExpr(s->as.for_stmt.increment, &child_ok);
        if (!child_ok) t = lastTokenOfExpr(s->as.for_stmt.condition, &child_ok);
        if (!child_ok) t = lastTokenOfStmt(s->as.for_stmt.initializer, &child_ok);
        break;
    case STMT_FUNC_DECLARATION:
        t = lastTokenOfStmt(s->as.func_declaration.body, &child_ok);
        if (!child_ok) { t = s->as.func_declaration.name; child_ok = token_is_real(&t); }
        break;
    case STMT_RETURN:
        t = lastTokenOfExpr(s->as.return_stmt.value, &child_ok);
        if (!child_ok) { t = s->keyword; child_ok = token_is_real(&t); }
        break;
    case STMT_COMPILER_DIRECTIVE:
        t = s->as.compiler_directive.argument;
        child_ok = token_is_real(&t);
        break;
    case STMT_STRUCT_DECLARATION: {
        int n = s->as.struct_declaration.field_count;
        if (n > 0) {
            t = s->as.struct_declaration.fields[n - 1];
            child_ok = token_is_real(&t);
        }
        if (!child_ok) {
            t = s->as.struct_declaration.name; child_ok = token_is_real(&t);
        }
        break;
    }
    case STMT_ENUM_DECLARATION: {
        int n = s->as.enum_declaration.variant_count;
        if (n > 0) {
            t = s->as.enum_declaration.variants[n - 1];
            child_ok = token_is_real(&t);
        }
        if (!child_ok) {
            t = s->as.enum_declaration.name; child_ok = token_is_real(&t);
        }
        break;
    }
    case STMT_LABEL:
        t = s->as.label.label_name;
        child_ok = token_is_real(&t);
        break;
    case STMT_GOTO:
        t = s->as.goto_stmt.target_label;
        child_ok = token_is_real(&t);
        break;
    case STMT_SWITCH: {
        int n = s->as.switch_stmt.case_count;
        if (n > 0) {
            CaseClause* last = &s->as.switch_stmt.cases[n - 1];
            t = last_stmt_array(last->statements, last->statement_count,
                                &child_ok);
        }
        if (!child_ok) t = lastTokenOfExpr(s->as.switch_stmt.expression, &child_ok);
        break;
    }
    case STMT_ERROR:
        t = s->as.error.end_token;
        child_ok = token_is_real(&t);
        if (!child_ok) { t = s->as.error.start_token; child_ok = token_is_real(&t); }
        break;
    case STMT_BREAK:
    case STMT_CONTINUE:
        t = s->keyword;
        child_ok = token_is_real(&t);
        break;
    }

    if (ok) *ok = child_ok;
    return t;
}

// ---- ZymSpan constructors -------------------------------------------

static ZymSpan span_from_tokens(const Token* first, const Token* last,
                                int fallback_line) {
    ZymSpan s;
    if (first != NULL && last != NULL
        && token_is_real(first) && token_is_real(last)
        && first->fileId == last->fileId) {
        int end_byte = last->startByte + last->length;
        s.fileId      = first->fileId;
        s.startByte   = first->startByte;
        s.length      = end_byte - first->startByte;
        if (s.length < 0) s.length = first->length;  // defensive
        s.startLine   = first->startLine;
        s.startColumn = first->startColumn;
        s.endLine     = last->endLine;
        s.endColumn   = last->endColumn;
    } else {
        s.fileId      = -1;
        s.startByte   = -1;
        s.length      = 0;
        s.startLine   = fallback_line;
        s.startColumn = -1;
        s.endLine     = fallback_line;
        s.endColumn   = -1;
    }
    return s;
}

ZymSpan nodeSpanOfExpr(const Expr* expr) {
    if (expr == NULL) return span_from_tokens(NULL, NULL, 0);
    bool fok = false, lok = false;
    Token first = firstTokenOfExpr(expr, &fok);
    Token last  = lastTokenOfExpr(expr, &lok);
    return span_from_tokens(fok ? &first : NULL,
                            lok ? &last  : NULL,
                            expr->line);
}

ZymSpan nodeSpanOfStmt(const Stmt* stmt) {
    if (stmt == NULL) return span_from_tokens(NULL, NULL, 0);
    bool fok = false, lok = false;
    Token first = firstTokenOfStmt(stmt, &fok);
    Token last  = lastTokenOfStmt(stmt, &lok);
    return span_from_tokens(fok ? &first : NULL,
                            lok ? &last  : NULL,
                            stmt->line);
}
