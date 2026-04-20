/*
 * _config_report.c — zymConfigSummary() implementation.
 *
 * Returns a human-readable one-line summary of the active ZYM_HAS_* flags.
 * Hosts embed this in --version output or an LSP handshake so a bug report
 * can unambiguously identify which tooling surface was compiled in.
 */
#include "zym/config.h"

/* Assemble the summary as a single compile-time-concatenated string literal
 * so the result lives in rodata and no dynamic allocation is required. The
 * five flag tokens are expanded with the preprocessor trick `X(a) #a` to
 * produce "0" or "1" at build time. */
#define ZYM__STR_INNER(x) #x
#define ZYM__STR(x)       ZYM__STR_INNER(x)

static const char kZymConfigSummary[] =
    "zym_core features: "
    "LSP_SURFACE="          ZYM__STR(ZYM_HAS_LSP_SURFACE)          ", "
    "PARSE_TREE_RETENTION=" ZYM__STR(ZYM_HAS_PARSE_TREE_RETENTION) ", "
    "SYMBOL_TABLE="         ZYM__STR(ZYM_HAS_SYMBOL_TABLE)         ", "
    "NATIVE_METADATA="      ZYM__STR(ZYM_HAS_NATIVE_METADATA)      ", "
    "DIAGNOSTIC_CODES="     ZYM__STR(ZYM_HAS_DIAGNOSTIC_CODES);

const char* zymConfigSummary(void) {
    return kZymConfigSummary;
}
