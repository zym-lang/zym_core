#pragma once

#include <stdio.h>
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

// JavaScript ToInt32 (ECMA-262 7.1.6): truncate toward zero, reduce modulo
// 2^32, reinterpret as signed. A bare (int32_t) cast is undefined outside
// int32 range - x86 hands back INT32_MIN, wasm's trunc_sat clamps - so
// out-of-range doubles have to be wrapped explicitly.
static inline int32_t double_to_int32(double num) {
    // Fast path: anything representable as an int64 wraps correctly under
    // truncation, which covers every value short of 2^63.
    if (num >= -9223372036854775808.0 && num < 9223372036854775808.0) {
        return (int32_t)(uint32_t)(uint64_t)(int64_t)num;
    }
    // What's left is NaN, an infinity, or |num| >= 2^63 - all integral, so the
    // low 32 bits can be read straight off the significand. The exponent field
    // is at least 1086 here, making the shift below land in [11, 31].
    uint64_t bits = double_to_value(num);
    int shift = (int)((bits >> 52) & 0x7FF) - 1075;  // 1023 exponent bias + 52 significand bits
    if (shift >= 32) return 0;                       // NaN, infinity, or a multiple of 2^32
    uint32_t low = (uint32_t)(((bits & 0xFFFFFFFFFFFFFULL) | 0x10000000000000ULL) << shift);
    return (int32_t)((bits & SIGN_BIT) ? (uint32_t)0 - low : low);
}

// JavaScript ToUint32 (ECMA-262 7.1.7): same reduction, unsigned reading.
static inline uint32_t double_to_uint32(double num) {
    return (uint32_t)double_to_int32(num);
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
void fprintValue(VM* vm, FILE* file, Value value);
Value cloneValue(VM* vm, Value value);
Value deepCloneValue(VM* vm, Value value);