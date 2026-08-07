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
    // How many registers this chunk's code actually uses. A chunk executed at
    // top level has no CallFrame, so this is the only thing that tells the GC
    // how far up the stack its live registers reach -- markRoots scans
    // [0, stack_top) and anything above it is invisible and gets swept while
    // still reachable from script. Set by the compiler and round-tripped
    // through bytecode; MAX_PHYSICAL_REGS is the safe fallback for a chunk
    // that predates the field or was built by hand.
    int max_regs;
} Chunk;

void initChunk(Chunk* chunk);
void freeChunk(VM* vm, Chunk* chunk);
void writeInstruction(VM* vm, Chunk* chunk, uint32_t instruction, int line);
void write64BitLiteral(VM* vm, Chunk* chunk, double value, int line);
int addConstant(VM* vm, Chunk* chunk, Value value);