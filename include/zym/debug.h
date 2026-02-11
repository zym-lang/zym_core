#pragma once
#include <stdio.h>

typedef struct Chunk ZymChunk;

void disassembleChunk(ZymChunk* chunk, const char* name);
void disassembleChunkToFile(ZymChunk* chunk, const char* name, FILE* file);
int disassembleInstruction(ZymChunk* chunk, int offset);
