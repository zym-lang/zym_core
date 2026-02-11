#include <stdlib.h>
#include <string.h>

#include "./chunk.h"
#include "./memory.h"
#include "./vm.h"
#include "gc.h"

void initChunk(Chunk* chunk) {
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    initValueArray(&chunk->constants);
}

void freeChunk(VM* vm, Chunk* chunk) {
    FREE_ARRAY(vm, uint32_t, chunk->code, chunk->capacity);
    FREE_ARRAY(vm, int, chunk->lines, chunk->capacity);
    freeValueArray(vm, &chunk->constants);
    initChunk(chunk);
}

void writeInstruction(VM* vm, Chunk* chunk, uint32_t instruction, int line) {
    if (chunk->capacity < chunk->count + 1) {
        int oldCapacity = chunk->capacity;
        chunk->capacity = GROW_CAPACITY(oldCapacity);
        chunk->code = GROW_ARRAY(vm, uint32_t, chunk->code, oldCapacity, chunk->capacity);
        chunk->lines = GROW_ARRAY(vm, int, chunk->lines, oldCapacity, chunk->capacity);
        if (chunk->code == NULL || chunk->lines == NULL) {
            exit(1);
        }
    }
    chunk->code[chunk->count] = instruction;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

void write64BitLiteral(VM* vm, Chunk* chunk, double value, int line) {
    if (chunk->capacity < chunk->count + 2) {
        int oldCapacity = chunk->capacity;
        chunk->capacity = GROW_CAPACITY(oldCapacity);
        chunk->code = GROW_ARRAY(vm, uint32_t, chunk->code, oldCapacity, chunk->capacity);
        chunk->lines = GROW_ARRAY(vm, int, chunk->lines, oldCapacity, chunk->capacity);
        if (chunk->code == NULL || chunk->lines == NULL) {
            exit(1);
        }
    }

    uint64_t bits;
    memcpy(&bits, &value, sizeof(double));

    chunk->code[chunk->count] = (uint32_t)(bits & 0xFFFFFFFF);
    chunk->lines[chunk->count] = line;
    chunk->count++;

    chunk->code[chunk->count] = (uint32_t)((bits >> 32) & 0xFFFFFFFF);
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

int addConstant(VM* vm, Chunk* chunk, Value value) {
    if (IS_OBJ(value)) {
        pushTempRoot(vm, AS_OBJ(value));
    }
    writeValueArray(vm, &chunk->constants, value);
    if (IS_OBJ(value)) {
        popTempRoot(vm);
    }
    return chunk->constants.count - 1;
}