#pragma once

#include "./scanner.h"
#include "./ast.h"
#include "./linemap.h"
#include "./sourcemap.h"

typedef struct VM VM;

typedef struct {
    Stmt** statements;
    int capacity;
} AstResult;

AstResult parse(VM* vm, const char* source, const LineMap* line_map,
                const SourceMap* source_map, const char* entry_file,
                ZymFileId file_id);
