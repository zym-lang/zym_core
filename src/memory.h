#pragma once

#include "./common.h"

typedef struct VM VM;

void* reallocate(VM* vm, void* pointer, size_t oldSize, size_t newSize);

#define GROW_CAPACITY(capacity) ((capacity) < 8 ? 8 : (capacity) * 2)
#define GROW_ARRAY(vm, type, pointer, oldCount, newCount) (type*)reallocate(vm, pointer, sizeof(type) * (oldCount), sizeof(type) * (newCount))
#define FREE_ARRAY(vm, type, pointer, oldCapacity) reallocate(vm, pointer, sizeof(type) * (oldCapacity), 0)
#define ALLOCATE(vm, type, count) (type*)reallocate(vm, NULL, 0, sizeof(type) * (count))
#define FREE(vm, type, pointer) reallocate(vm, pointer, sizeof(type), 0)