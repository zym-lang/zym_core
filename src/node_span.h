#pragma once

// Phase 2.4 (LSP roadmap) — reconstructed node spans.
//
// `nodeSpanOfExpr` / `nodeSpanOfStmt` return a ZymSpan covering the
// source bytes of an AST node. Spans are *reconstructed* on demand
// from the node's bracketing tokens rather than stored per-node, so
// the on-disk footprint of the AST is unchanged. Synthetic / error
// nodes already carry explicit `start_token` / `end_token` fields
// (see ErrorExpr, ErrorStmt in ast.h) and are honored directly.
//
// This header is internal. The public surface (ZymSpan + accessors on
// ZymParseTree) lives in <zym/frontend.h> and is gated on
// ZYM_HAS_PARSE_TREE_RETENTION. These helpers stay always-linked so
// the compiler / debug paths can use them for diagnostics too if
// ever needed.

#include "zym/frontend.h"   // ZymSpan
#include "./ast.h"
#include "./token.h"

// Returns the span of an expression/statement. On a NULL node or a
// node whose tokens are entirely synthetic (no fileId), the returned
// span is the degraded form {fileId=-1, startByte=-1, length=0,
// startLine=node->line, startColumn=-1, endLine=node->line,
// endColumn=-1}.
ZymSpan nodeSpanOfExpr(const Expr* expr);
ZymSpan nodeSpanOfStmt(const Stmt* stmt);

// Exposed for the query API (Phase 2.5) and for golden dumps (2.6).
// Return the first / last scanned Token that belongs to `expr`/`stmt`.
// `ok` (when non-NULL) is set to false if no Token could be located
// (entirely synthetic subtree); the returned Token is then zero-filled.
Token firstTokenOfExpr(const Expr* expr, bool* ok);
Token lastTokenOfExpr(const Expr* expr, bool* ok);
Token firstTokenOfStmt(const Stmt* stmt, bool* ok);
Token lastTokenOfStmt(const Stmt* stmt, bool* ok);
