#include "./source_file.h"

#include "./memory.h"

#include <stddef.h>

void sfr_init(SourceFileRegistry* reg) {
    reg->files = NULL;
    reg->count = 0;
    reg->capacity = 0;
}

void sfr_free(VM* vm, SourceFileRegistry* reg) {
    if (reg->files != NULL) {
        FREE_ARRAY(vm, SourceFile, reg->files, reg->capacity);
    }
    reg->files = NULL;
    reg->count = 0;
    reg->capacity = 0;
}

ZymFileId sfr_register(VM* vm, SourceFileRegistry* reg,
                       const char* path, const char* bytes, size_t length) {
    if (reg->count + 1 > reg->capacity) {
        int old_capacity = reg->capacity;
        reg->capacity = GROW_CAPACITY(old_capacity);
        reg->files = GROW_ARRAY(vm, SourceFile, reg->files, old_capacity, reg->capacity);
    }

    ZymFileId id = (ZymFileId)reg->count;
    SourceFile* sf = &reg->files[id];
    sf->id = id;
    sf->path = path;
    sf->bytes = bytes;
    sf->length = length;
    reg->count++;
    return id;
}

const SourceFile* sfr_get(const SourceFileRegistry* reg, ZymFileId id) {
    if (id < 0 || id >= reg->count) return NULL;
    return &reg->files[id];
}

void sfr_reset(SourceFileRegistry* reg) {
    reg->count = 0;
}
