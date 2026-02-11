#pragma once

#include "./common.h"
#include "./object.h"
#include "./value.h"

typedef struct VM VM;

typedef struct {
    ObjString* key;
    Value value;
} Entry;

typedef struct Table {
    int count;
    int capacity;
    Entry* entries;
} Table;

void initTable(Table* table);
void freeTable(VM* vm, Table* table);
bool tableGet(Table* table, ObjString* key, Value* value);
bool tableSet(VM* vm, Table* table, ObjString* key, Value value);
bool tableDelete(Table* table, ObjString* key);
ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash);
void tableRemoveWhite(Table* table);