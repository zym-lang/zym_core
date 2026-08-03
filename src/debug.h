#pragma once

#include <stdio.h>
#include "./chunk.h"

void disassembleChunk(Chunk* chunk, const char* name);
void disassembleChunkToFile(Chunk* chunk, const char* name, FILE* file);
int disassembleInstruction(Chunk* chunk, int offset);