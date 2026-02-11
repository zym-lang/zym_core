#pragma once

#include "./common.h"
#include "./opcode.h"
#include "./value.h"

typedef struct VM VM;

typedef struct Chunk {
    int count;
    int capacity;
    uint32_t* code;
    int* lines;
    ValueArray constants;
} Chunk;

void initChunk(Chunk* chunk);
void freeChunk(VM* vm, Chunk* chunk);
void writeInstruction(VM* vm, Chunk* chunk, uint32_t instruction, int line);
void write64BitLiteral(VM* vm, Chunk* chunk, double value, int line);
int addConstant(VM* vm, Chunk* chunk, Value value);