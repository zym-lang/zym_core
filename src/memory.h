#pragma once

#include "./common.h"
#include "./allocator.h"
#include <string.h>

typedef struct VM VM;

// Raw allocator convenience macros (take ZymAllocator*)
// reallocate NEVER returns NULL for a non-zero size. On failure it collects,
// retries, and then calls zymOutOfMemory, which unwinds to the nearest armed
// API boundary or, with none armed, terminates. Callers must not NULL-check it:
// such a check is dead code and implies a contract that does not exist.
//
// Unrecoverable allocation failure. When a public entry point has armed a
// recovery boundary this does not return: it unwinds there and the call reports
// ZYM_STATUS_RUNTIME_ERROR with cause ZYM_CAUSE_OUT_OF_MEMORY. With nothing
// armed -- an allocation outside any VM operation, such as during zym_newVM --
// there is nowhere to go and the process still dies, which is the honest
// outcome for a host that cannot even construct a VM.
struct VM;
void zymOutOfMemory(struct VM* vm);

#define ZYM_ALLOC(a, size)              (a)->alloc((a)->ctx, (size))
#define ZYM_CALLOC(a, count, size)      (a)->calloc((a)->ctx, (count), (size))
#define ZYM_REALLOC(a, ptr, old, new_)  (a)->realloc((a)->ctx, (ptr), (old), (new_))
#define ZYM_FREE(a, ptr, size)          (a)->free((a)->ctx, (ptr), (size))

// strdup replacement using allocator
static inline char* zym_strdup(ZymAllocator* a, const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = (char*)ZYM_ALLOC(a, len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

// Free a string allocated with zym_strdup (size unknown, pass 0)
#define ZYM_FREE_STR(a, ptr) do { if (ptr) { ZYM_FREE((a), (ptr), strlen(ptr) + 1); } } while(0)

void* reallocate(VM* vm, void* pointer, size_t oldSize, size_t newSize);

#define GROW_CAPACITY(capacity) ((capacity) < 8 ? 8 : (capacity) * 2)
#define GROW_ARRAY(vm, type, pointer, oldCount, newCount) (type*)reallocate(vm, pointer, sizeof(type) * (oldCount), sizeof(type) * (newCount))
#define FREE_ARRAY(vm, type, pointer, oldCapacity) reallocate(vm, pointer, sizeof(type) * (oldCapacity), 0)
#define ALLOCATE(vm, type, count) (type*)reallocate(vm, NULL, 0, sizeof(type) * (count))
#define FREE(vm, type, pointer) reallocate(vm, pointer, sizeof(type), 0)