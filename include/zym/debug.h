#pragma once
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Chunk ZymChunk;

void disassembleChunk(ZymChunk* chunk, const char* name);
void disassembleChunkToFile(ZymChunk* chunk, const char* name, FILE* file);
int disassembleInstruction(ZymChunk* chunk, int offset);
int disassembleInstructionToFile(ZymChunk* chunk, int offset, FILE* file);

#ifdef __cplusplus
}
#endif
