#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "./memory.h"
#include "./vm.h"
#include "./gc.h"

// =============================================================================
// DEFAULT ALLOCATOR
// =============================================================================

static void* default_alloc(void* ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void* default_calloc(void* ctx, size_t count, size_t size) {
    (void)ctx;
    return calloc(count, size);
}

static void* default_realloc(void* ctx, void* ptr, size_t old_size, size_t new_size) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void default_free(void* ctx, void* ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

ZymAllocator zym_defaultAllocator(void) {
    return (ZymAllocator){
        .alloc   = default_alloc,
        .calloc  = default_calloc,
        .realloc = default_realloc,
        .free    = default_free,
        .ctx     = NULL
    };
}

// =============================================================================
// REALLOCATE (GC-aware, uses VM's allocator)
// =============================================================================

void* reallocate(VM* vm, void* pointer, size_t oldSize, size_t newSize) {
    vm->bytes_allocated += newSize - oldSize;

    if (newSize > oldSize) {
        int32_t delta = (int32_t)((newSize - oldSize) > (size_t)INT32_MAX ? (size_t)INT32_MAX : (newSize - oldSize));
        vm->gc_debt -= delta;
        #ifdef DEBUG_STRESS_GC
            if (vm->gc_enabled) collectGarbage(vm);
        #else
            if (__builtin_expect(vm->gc_debt <= 0, 0)) {
                if (vm->gc_enabled) {
                    collectGarbage(vm);
                } else {
                    // GC disabled but debt wrapped — reset to prevent repeated triggers
                    vm->gc_debt = INT32_MAX;
                }
            }
        #endif
    }

    if (newSize == 0) {
        ZYM_FREE(&vm->allocator, pointer, oldSize);
        return NULL;
    }

    void* result = ZYM_REALLOC(&vm->allocator, pointer, oldSize, newSize);
    if (result == NULL) {
        if (vm->gc_enabled) {
            collectGarbage(vm);
            result = ZYM_REALLOC(&vm->allocator, pointer, oldSize, newSize);
        }
        if (result == NULL) {
            fprintf(stderr, "Fatal: Out of memory\n");
            exit(1);
        }
    }
    return result;
}
