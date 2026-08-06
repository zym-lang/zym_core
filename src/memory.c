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
        bool just_collected = false;
        int32_t delta = (int32_t)((newSize - oldSize) > (size_t)INT32_MAX ? (size_t)INT32_MAX : (newSize - oldSize));
        vm->gc_debt -= delta;
        #ifdef DEBUG_STRESS_GC
            if (vm->gc_enabled) { collectGarbage(vm); just_collected = true; }
        #else
            if (__builtin_expect(vm->gc_debt <= 0, 0)) {
                if (vm->gc_enabled) {
                    collectGarbage(vm);
                    just_collected = true;
                } else {
                    // GC disabled but debt wrapped — reset to prevent repeated triggers
                    vm->gc_debt = INT32_MAX;
                }
            }
        #endif

        // Memory ceiling. Deliberately checked AFTER any collection above, so
        // a program that merely churns garbage is never charged for it.
        if (__builtin_expect(vm->memory_limit > 0 && !vm->oom_pending &&
                             vm->bytes_allocated > vm->memory_limit, 0)) {
            // One more collection before giving up -- the debt heuristic may
            // simply not have come due yet. gc_enabled is false while a
            // collection is in progress, so this cannot re-enter.
            if (!just_collected && vm->gc_enabled) {
                collectGarbage(vm);
            }

            if (vm->bytes_allocated > vm->memory_limit) {
                // Do NOT fail the allocation: the host allocator still has
                // memory, and returning NULL here would strand every caller
                // that assumes success. Satisfy this request, then suspend at
                // the next instruction boundary. The overshoot is bounded by
                // one allocation, plus whatever the current opcode still does
                // before DISPATCH runs again.
                vm->oom_pending = true;
                vm->preempt_counter = 0;   // observed by DISPATCH -> handlePreemption
            }
        }
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
