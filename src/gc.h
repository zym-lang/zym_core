#pragma once

#include "./vm.h"
#include "./object.h"
#include "./value.h"

void collectGarbage(VM* vm);

void markValue(VM* vm, Value value);
void markObject(VM* vm, Obj* object);
void markTable(VM* vm, Table* table);

void pushTempRoot(VM* vm, Obj* object);
void popTempRoot(VM* vm);

void freeObject(VM* vm, Obj* object);

#define GC_HEAP_GROW_FACTOR 2

//#define GC_DEBUG
//#define GC_DEBUG_FULL
//#define DEBUG_STRESS_GC