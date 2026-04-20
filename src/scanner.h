#pragma once

#include <stdbool.h>

#include "./token.h"
#include "./sourcemap.h"
#include "./source_file.h"

typedef struct TriviaBuffer TriviaBuffer;
typedef struct VM VM;

typedef struct {
    // Canonical base pointer: scanner->base points at byte 0 of the file
    // identified by file_id. All byte offsets on emitted Tokens are
    // measured from this base.
    const char* base;
    ZymFileId file_id;

    // Legacy scan cursors (kept for the existing scanning logic which
    // walks pointers). `start` is advanced to `current` at the top of
    // every scanToken() call.
    const char* start;
    const char* current;

    // Phase 1.1: canonical position state, updated in lockstep with
    // start/current. Columns are counted in UTF-8 bytes.
    int start_byte;
    int start_line;
    int start_column;
    int current_line;   // 1-based, mirrors legacy `line`
    int current_column; // 1-based byte column on current_line

    // Retained for compatibility with existing callers / tokens that
    // still dereference `scanner->line`. Equals current_line.
    int line;

    // Phase 1.6: SourceMap is the sole origin-tracking primitive. When
    // present, every emitted token's `line` and origin{FileId,StartByte,
    // Length} are resolved via sourcemap_lookup() on the token's
    // startByte. NULL for callers that don't run through the preprocessor
    // (e.g. scanning of a raw unpreprocessed buffer); in that case the
    // scanner reports its own expanded-buffer line number verbatim.
    const SourceMap* source_map;

    // Phase 2.3: optional trivia side buffer. When non-NULL, every
    // comment (regular and doc) consumed by skipWhitespace() is
    // recorded as a TriviaPiece keyed by byte offset. The parser
    // never sees trivia — scanToken() still returns only non-trivia
    // tokens. NULL on MCU builds / whenever the caller hasn't opted
    // into retention; in that case skipWhitespace() has no extra cost.
    TriviaBuffer* trivia;
    VM*           vm; // needed to grow the trivia buffer; NULL when trivia is NULL

    // Phase 1.5: per-scanner scratch buffer used by scanToken() to format
    // the "Unexpected character ..." error message. Previously a
    // function-local `static char[64]` — moving it onto the scanner
    // removes the last piece of mutable static state from the frontend
    // and makes the scanner trivially re-entrant across concurrent
    // instances.
    char error_buf[64];
} Scanner;

void initScanner(Scanner* scanner, const char* source,
                 const SourceMap* source_map, ZymFileId file_id);
void scannerAttachTrivia(Scanner* scanner, VM* vm, TriviaBuffer* trivia);
Token scanToken(Scanner* scanner);
bool isAlpha(char c);
