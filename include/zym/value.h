#pragma once

#include <string.h>
#include "./common.h"

typedef struct Chunk Chunk;
typedef struct VM VM;

typedef struct Obj Obj;
typedef uint64_t Value;

#define QNAN     ((uint64_t)0x7ff8000000000000)
#define SIGN_BIT ((uint64_t)0x8000000000000000)
#define TAG_NULL    1
#define TAG_FALSE   2
#define TAG_TRUE    3
#define TAG_ENUM    4

#define IS_DOUBLE(value) (((value) & QNAN) != QNAN)
#define IS_OBJ(value)    (((value) & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT))
#define IS_NULL(value)   ((value) == NULL_VAL)
#define IS_BOOL(value)   (((value) | 1) == TRUE_VAL)
#define IS_ENUM(value)   (((value) & (QNAN | 0xFF)) == (QNAN | TAG_ENUM))

#define AS_DOUBLE(value) value_to_double(value)
#define AS_OBJ(value)    ((Obj*)(uintptr_t)((value) & ~(SIGN_BIT | QNAN)))
#define AS_BOOL(value)   ((value) == TRUE_VAL)
#define ENUM_TYPE_ID(value) ((int)(((value) >> 32) & 0xFFFF))
#define ENUM_VARIANT(value) ((int)(((value) >> 16) & 0xFFFF))

#define DOUBLE_VAL(num)  double_to_value(num)
#define OBJ_VAL(obj)     (Value)(SIGN_BIT | QNAN | (uint64_t)(uintptr_t)(obj))
#define NULL_VAL         ((Value)(uint64_t)(QNAN | TAG_NULL))
#define FALSE_VAL        ((Value)(uint64_t)(QNAN | TAG_FALSE))
#define TRUE_VAL         ((Value)(uint64_t)(QNAN | TAG_TRUE))
#define ENUM_VAL(type_id, variant) ((Value)(QNAN | TAG_ENUM | ((uint64_t)(type_id) << 32) | ((uint64_t)(variant) << 16)))

#define BOOL_VAL(b)      ((b) ? TRUE_VAL : FALSE_VAL)

static inline double value_to_double(Value value) {
    /*double num;
    memcpy(&num, &value, sizeof(Value));
    return num;*/
    union { Value u; double d; } x;
    x.u = value;
    return x.d;
}
static inline Value double_to_value(double num) {
    /*Value value;
    memcpy(&value, &num, sizeof(double));
    return value;*/
    union { double d; Value u; } x;
    x.d = num;
    return x.u;
}

typedef struct {
    int capacity;
    int count;
    Value* values;
} ValueArray;

void initValueArray(ValueArray* array);
void writeValueArray(VM* vm, ValueArray* array, Value value);
void freeValueArray(VM* vm, ValueArray* array);
void printValue(VM* vm, Value value);
Value cloneValue(VM* vm, Value value);
Value deepCloneValue(VM* vm, Value value);
bool dereferenceValue(VM* vm, Value refValue, Value* out);
bool writeReferenceValue(VM* vm, Value refValue, Value new_value);