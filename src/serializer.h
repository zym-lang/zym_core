#pragma once

#include "./chunk.h"
#include "./compiler.h"
#include "./utils.h"

void serializeChunk(VM* vm, Chunk* chunk, CompilerConfig config, OutputBuffer* out);
bool deserializeChunk(VM* vm, Chunk* chunk, const uint8_t* buffer, size_t size);