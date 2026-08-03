#pragma once

#include "./scanner.h"
#include "./ast.h"
#include "./sourcemap.h"

typedef struct VM VM;
typedef struct TriviaBuffer TriviaBuffer;

typedef struct {
    Stmt** statements;
    int capacity;
} AstResult;

// Phase 2.3: `trivia` is an optional side buffer. When non-NULL, every
// comment (including doc comments) the scanner encounters is appended
// as a TriviaPiece. Pass NULL for the legacy no-retention path — the
// scanner does zero extra work.
AstResult parse(VM* vm, const char* source, const SourceMap* source_map,
                const char* entry_file, ZymFileId file_id,
                TriviaBuffer* trivia);
