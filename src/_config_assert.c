/*
 * _config_assert.c — compile-time sanity checks for the ZYM_HAS_* flags.
 *
 * This translation unit is compiled into every build of zym_core and emits
 * no runtime code or data. Its only job is to catch inconsistent flag
 * combinations at build time so a broken CMake config fails loudly instead
 * of producing a silently-wrong binary.
 */
#include "zym/config.h"

/* Every flag must be defined to 0 or 1 by zym/config.h. */
#if !defined(ZYM_HAS_LSP_SURFACE)          \
 || !defined(ZYM_HAS_PARSE_TREE_RETENTION) \
 || !defined(ZYM_HAS_SYMBOL_TABLE)         \
 || !defined(ZYM_HAS_NATIVE_METADATA)      \
 || !defined(ZYM_HAS_DIAGNOSTIC_CODES)     \
 || !defined(ZYM_HAS_BUILD_TESTING)
# error "zym/config.h did not define all ZYM_HAS_* flags. Regenerate CMake."
#endif

/* SYMBOL_TABLE on implies PARSE_TREE_RETENTION on. The resolver walks a
 * retained parse tree; it cannot run without one. This is also enforced in
 * Features.cmake, but re-asserted here so a hand-edited config.h cannot
 * slip past. */
_Static_assert(!(ZYM_HAS_SYMBOL_TABLE) || (ZYM_HAS_PARSE_TREE_RETENTION),
    "ZYM_HAS_SYMBOL_TABLE=1 requires ZYM_HAS_PARSE_TREE_RETENTION=1.");

/* Flags must be 0 or 1, not anything truthy. This catches a config.h.in
 * template expansion that accidentally produced the empty string or
 * something like 'ON'. */
_Static_assert((ZYM_HAS_LSP_SURFACE)          == 0 || (ZYM_HAS_LSP_SURFACE)          == 1,
    "ZYM_HAS_LSP_SURFACE must be 0 or 1.");
_Static_assert((ZYM_HAS_PARSE_TREE_RETENTION) == 0 || (ZYM_HAS_PARSE_TREE_RETENTION) == 1,
    "ZYM_HAS_PARSE_TREE_RETENTION must be 0 or 1.");
_Static_assert((ZYM_HAS_SYMBOL_TABLE)         == 0 || (ZYM_HAS_SYMBOL_TABLE)         == 1,
    "ZYM_HAS_SYMBOL_TABLE must be 0 or 1.");
_Static_assert((ZYM_HAS_NATIVE_METADATA)      == 0 || (ZYM_HAS_NATIVE_METADATA)      == 1,
    "ZYM_HAS_NATIVE_METADATA must be 0 or 1.");
_Static_assert((ZYM_HAS_DIAGNOSTIC_CODES)     == 0 || (ZYM_HAS_DIAGNOSTIC_CODES)     == 1,
    "ZYM_HAS_DIAGNOSTIC_CODES must be 0 or 1.");
_Static_assert((ZYM_HAS_BUILD_TESTING)        == 0 || (ZYM_HAS_BUILD_TESTING)        == 1,
    "ZYM_HAS_BUILD_TESTING must be 0 or 1.");

/* Silence -Wempty-translation-unit on strict toolchains (MSVC, some clang
 * configurations) that warn when a TU contains no external declarations. */
extern const int zym_config_assert_marker;
const int zym_config_assert_marker = 0;
