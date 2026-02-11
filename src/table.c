#include <stdlib.h>
#include <string.h>

#include "./memory.h"
#include "./object.h"
#include "./table.h"
#include "./vm.h"
#include "gc.h"

#define TABLE_MAX_LOAD 0.75

void initTable(Table* table) {
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
}

void freeTable(VM* vm, Table* table) {
    reallocate(vm, table->entries, sizeof(Entry) * table->capacity, 0);
    initTable(table);
}

static Entry* findEntry(Entry* entries, int capacity, ObjString* key) {
    uint32_t index = key->hash & (capacity - 1);
    Entry* tombstone = NULL;

    for (;;) {
        Entry* entry = &entries[index];
        if (entry->key == NULL) {
            if (IS_NULL(entry->value)) {
                return tombstone != NULL ? tombstone : entry;
            } else {
                if (tombstone == NULL) tombstone = entry;
            }
        } else if (entry->key == key) {
            return entry;
        }

        index = (index + 1) & (capacity - 1);
    }
}

ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash) {
    if (table->count == 0) return NULL;

    uint32_t index = hash & (table->capacity - 1);
    for (;;) {
        Entry* entry = &table->entries[index];
        if (entry->key == NULL) {
            if (IS_NULL(entry->value)) return NULL;
        } else if (entry->key->byte_length == length &&
                   entry->key->hash == hash &&
                   memcmp(entry->key->chars, chars, length) == 0) {
            return entry->key;
                   }

        index = (index + 1)  & (table->capacity - 1);
    }
}

bool tableGet(Table* table, ObjString* key, Value* value) {
    if (table->count == 0) return false;

    Entry* entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;

    *value = entry->value;
    return true;
}

static void adjustCapacity(VM* vm, Table* table, int capacity) {
    Entry* entries = (Entry*)reallocate(vm, NULL, 0, sizeof(Entry) * capacity);
    for (int i = 0; i < capacity; i++) {
        entries[i].key = NULL;
        entries[i].value = NULL_VAL;
    }

    table->count = 0;
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key == NULL) continue;

        Entry* dest = findEntry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }

    reallocate(vm, table->entries, sizeof(Entry) * table->capacity, 0);
    table->entries = entries;
    table->capacity = capacity;
}

bool tableSet(VM* vm, Table* table, ObjString* key, Value value) {
    // Protect key and value from GC during adjustCapacity (which can trigger GC)
    pushTempRoot(vm, (Obj*)key);
    if (IS_OBJ(value)) pushTempRoot(vm, AS_OBJ(value));

    if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
        int capacity = table->capacity < 8 ? 8 : table->capacity * 2;
        adjustCapacity(vm, table, capacity);
    }

    Entry* entry = findEntry(table->entries, table->capacity, key);
    bool isNewKey = entry->key == NULL;
    if (isNewKey && IS_NULL(entry->value)) table->count++;

    entry->key = key;
    entry->value = value;

    if (IS_OBJ(value)) popTempRoot(vm);
    popTempRoot(vm);
    return isNewKey;
}

bool tableDelete(Table* table, ObjString* key) {
    if (table->count == 0) return false;

    Entry* entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;

    entry->key = NULL;
    entry->value = BOOL_VAL(true);
    return true;
}