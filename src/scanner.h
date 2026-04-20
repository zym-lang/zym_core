#pragma once

#include <stdbool.h>

#include "./token.h"
#include "./linemap.h"
#include "./sourcemap.h"
#include "./source_file.h"

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

    const LineMap* line_map;

    // Phase 1.2: optional per-expanded-line origin table. When present,
    // every emitted token's origin{FileId,StartByte,Length} is resolved
    // via sourcemap_lookup() on the token's startByte. NULL for callers
    // that don't run through the preprocessor (e.g. module_loader's
    // pre-combined buffers).
    const SourceMap* source_map;
} Scanner;

void initScanner(Scanner* scanner, const char* source, const LineMap* line_map,
                 const SourceMap* source_map, ZymFileId file_id);
Token scanToken(Scanner* scanner);
bool isAlpha(char c);
