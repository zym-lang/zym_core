#pragma once

#include <stddef.h>

typedef struct ZymAllocator {
    void* (*alloc)(void* ctx, size_t size);
    void* (*calloc)(void* ctx, size_t count, size_t size);
    void* (*realloc)(void* ctx, void* ptr, size_t old_size, size_t new_size);
    void  (*free)(void* ctx, void* ptr, size_t size);
    void* ctx;
} ZymAllocator;

ZymAllocator zym_defaultAllocator(void);
