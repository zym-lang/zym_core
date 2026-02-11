#include <stdlib.h>
#include <stdio.h>

#include "./memory.h"
#include "./vm.h"
#include "./gc.h"

void* reallocate(VM* vm, void* pointer, size_t oldSize, size_t newSize) {
    vm->bytes_allocated += newSize - oldSize;

    if (vm->gc_enabled && newSize > oldSize) {
        #ifdef DEBUG_STRESS_GC
            collectGarbage(vm);
        #else
            if (vm->bytes_allocated > vm->next_gc) {
                collectGarbage(vm);
            }
        #endif
    }

    if (newSize == 0) {
        free(pointer);
        return NULL;
    }

    void* result = realloc(pointer, newSize);
    if (result == NULL) {
        // Try GC before OOM
        if (vm->gc_enabled) {
            collectGarbage(vm);
            result = realloc(pointer, newSize);
        }
        if (result == NULL) {
            fprintf(stderr, "Fatal: Out of memory\n");
            exit(1);
        }
    }
    return result;
}