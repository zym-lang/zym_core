#pragma once

#include "./common.h"
#include "./object.h"
#include "./value.h"

typedef struct VM VM;

void initTable(Table* table);
void freeTable(VM* vm, Table* table);
bool tableGet(Table* table, ObjString* key, Value* value);
bool tableSet(VM* vm, Table* table, ObjString* key, Value value);
bool tableDelete(Table* table, ObjString* key);
ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash);
// Two-segment variant for buffer-free concat interning: matches a key
// whose content equals a_chars[0..a_len) followed by b_chars[0..b_len).
ObjString* tableFindStringPair(Table* table,
                               const char* a_chars, int a_len,
                               const char* b_chars, int b_len,
                               uint32_t hash);
// N-segment variant for CONCAT_N interning: matches a key whose content is
// the concatenation of the given string parts, in order.
ObjString* tableFindStringParts(Table* table, ObjString** parts, int count,
                                int total_len, uint32_t hash);
void tableRemoveWhite(Table* table);