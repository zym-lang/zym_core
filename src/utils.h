#pragma once

#include <stdlib.h>
#include <stdbool.h>

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

// Returns allocated string with escapes processed, or NULL on error. Caller must free().
char* processEscapeSequences(const char* input, int input_len, int* out_len,
                             const char** error_msg, int* error_pos);

// Decodes module identifier to file path: "src_slash_math_dot_zym" -> "src/math.zym"
char* decodeModulePath(const char* encoded, int length);
