#pragma once

#include <stdlib.h>
#include <stdbool.h>
#include "./allocator.h"

typedef struct VM VM;

typedef struct {
    char* buffer;
    int count;
    int capacity;
} OutputBuffer;

void initOutputBuffer(OutputBuffer* ob);
void appendToOutputBuffer(VM* vm, OutputBuffer* ob, const char* text, size_t length);
void freeOutputBuffer(VM* vm, OutputBuffer* ob);

typedef struct {
    bool condition_met;
    bool branch_taken;
} IfState;

typedef struct {
    IfState* states;
    int count;
    int capacity;
} ConditionalStack;

void initConditionalStack(ConditionalStack* stack);
void pushConditionalStack(VM* vm, ConditionalStack* stack, IfState state);
IfState* peekConditionalStack(ConditionalStack* stack);
void popConditionalStack(ConditionalStack* stack);
void freeConditionalStack(VM* vm, ConditionalStack* stack);

// Returns allocated string with escapes processed, or NULL on error.
// Caller must free with the same allocator.
char* processEscapeSequences(ZymAllocator* alloc, const char* input, int input_len, int* out_len,
                             const char** error_msg, int* error_pos);

// Decodes module identifier to file path: "src_slash_math_dot_zym" -> "src/math.zym"
// Caller must free with the same allocator.
char* decodeModulePath(ZymAllocator* alloc, const char* encoded, int length);

// Single source of truth for the module-path <-> identifier escape table.
// Defined in module_loader.c; consumed by both the encoder there and the
// decoder in utils.c so they cannot drift out of sync.
typedef struct {
    char        ch;         // raw character (e.g. '/')
    const char* token;      // its identifier-safe escape (e.g. "_slash_")
    size_t      token_len;  // strlen(token), cached
} ModulePathEscape;

extern const ModulePathEscape MODULE_PATH_ESCAPES[];
extern const size_t MODULE_PATH_ESCAPES_COUNT;
