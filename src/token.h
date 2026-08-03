#pragma once

#include <stdio.h>

#include "./source_file.h"

typedef enum {
    // Single-character tokens.
    TOKEN_LEFT_PAREN,
    TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE,
    TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_BRACKET,
    TOKEN_RIGHT_BRACKET,
    TOKEN_COLON,
    TOKEN_COMMA,
    TOKEN_DOT,
    TOKEN_DOT_DOT_DOT,
    TOKEN_MINUS,
    TOKEN_PERCENT,
    TOKEN_PLUS,
    TOKEN_QUESTION,
    TOKEN_SEMICOLON,
    TOKEN_SLASH,
    TOKEN_STAR,
    TOKEN_AT,

    // One or two character tokens.
    TOKEN_BANG,
    TOKEN_BANG_EQUAL,
    TOKEN_EQUAL,
    TOKEN_EQUAL_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_FAT_ARROW,
    TOKEN_PLUS_PLUS,
    TOKEN_MINUS_MINUS,
    TOKEN_PLUS_EQUAL,
    TOKEN_MINUS_EQUAL,
    TOKEN_STAR_EQUAL,
    TOKEN_SLASH_EQUAL,
    TOKEN_PERCENT_EQUAL,
    TOKEN_BINARY_AND_EQUAL,
    TOKEN_BINARY_OR_EQUAL,
    TOKEN_BINARY_XOR_EQUAL,
    TOKEN_LEFT_SHIFT_EQUAL,
    TOKEN_RIGHT_SHIFT_EQUAL,
    TOKEN_UNSIGNED_RIGHT_SHIFT_EQUAL,
    TOKEN_LEFT_SHIFT,
    TOKEN_RIGHT_SHIFT,
    TOKEN_UNSIGNED_RIGHT_SHIFT,

    // Literals.
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,

    // Bitwise operators.
    TOKEN_BINARY_AND,
    TOKEN_BINARY_OR,
    TOKEN_BINARY_XOR,
    TOKEN_BINARY_NOT,

    // Zym Keywords (alphabetical)
    TOKEN_AND,
    TOKEN_BITWISE,
    TOKEN_BREAK,
    TOKEN_CASE,
    TOKEN_CONTINUE,
    TOKEN_DEFAULT,
    TOKEN_DO,
    TOKEN_ELSE,
    TOKEN_ENUM,
    TOKEN_FALSE,
    TOKEN_FOR,
    TOKEN_FUNC,
    TOKEN_GOTO,
    TOKEN_IF,
    TOKEN_NULL,
    TOKEN_OR,
    TOKEN_RETURN,
    TOKEN_STRUCT,
    TOKEN_SWITCH,
    TOKEN_TRUE,
    TOKEN_VAR,
    TOKEN_WHILE,

    // Special tokens.
    TOKEN_ERROR,
    TOKEN_EOF
  } TokenType;

typedef struct {
    TokenType type;

    // Legacy fields (kept for this PR so scanner/parser/compiler/debug
    // consumers do not need to be rewritten in lockstep with 1.1). These
    // are a derived view of (fileId, startByte, length, startLine):
    //   start  == sfr_get(&vm->source_files, fileId)->bytes + startByte
    //   line   == startLine
    // The scanner populates both the legacy and new fields; the parser /
    // compiler / debug consumers continue reading the legacy fields.
    const char* start;
    int length;
    int line;

    // Phase 1.1: canonical byte-offset span into the file identified by
    // fileId. Columns are counted in UTF-8 bytes (not code points);
    // position-encoding translation is done on demand by the LSP layer.
    ZymFileId fileId;
    int startByte;
    int startLine;
    int startColumn;
    int endLine;
    int endColumn;

    // Phase 1.1: best-effort origin mapping. For tokens scanned directly
    // from user source these are equal to (fileId, startByte, length).
    // For tokens synthesized by the preprocessor (macro expansions,
    // pasted #include content) these point to the user-visible origin
    // bytes that should be reported in diagnostics and hover. Byte-
    // granular origin tracking arrives with SourceMap in Phase 1.2; in
    // 1.1 the preprocessor may leave originStartByte/originLength as -1
    // when no exact mapping is available.
    ZymFileId originFileId;
    int originStartByte;
    int originLength;
} Token;

// Derived accessor for the token's lexeme bytes. In this PR it is
// equivalent to `tok->start`, but consumers are encouraged to use this
// helper so a later PR can retire the raw `start` pointer field without
// another sweep.
static inline const char* tokenLexeme(const Token* tok) {
    return tok->start;
}
