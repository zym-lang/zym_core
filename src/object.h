#pragma once

#include "./common.h"
#include "./value.h"
#include "compiler.h"

typedef struct VM VM;
typedef struct ObjFunction ObjFunction;
typedef struct {
    ObjString* key;
    Value value;
} Entry;

typedef struct Table {
    int count;
    int capacity;
    Entry* entries;
} Table;
typedef struct CallFrame CallFrame;
typedef struct PromptEntry PromptEntry;
typedef struct ResumeContext ResumeContext;

#define isObjType(value, objectType) (IS_OBJ(value) && AS_OBJ(value)->type == objectType)
#define OBJ_TYPE(value)     (AS_OBJ(value)->type)

#define IS_STRING(value)      isObjType(value, OBJ_STRING)
#define IS_FUNCTION(value)    isObjType(value, OBJ_FUNCTION)
#define IS_NATIVE_FUNCTION(value) isObjType(value, OBJ_NATIVE_FUNCTION)
#define IS_NATIVE_CONTEXT(value) isObjType(value, OBJ_NATIVE_CONTEXT)
#define IS_NATIVE_CLOSURE(value) isObjType(value, OBJ_NATIVE_CLOSURE)
#define IS_CLOSURE(value)     isObjType(value, OBJ_CLOSURE)
#define IS_UPVALUE(value)     isObjType(value, OBJ_UPVALUE)
#define IS_LIST(value)        isObjType(value, OBJ_LIST)
#define IS_MAP(value)         isObjType(value, OBJ_MAP)
#define IS_DISPATCHER(value)  isObjType(value, OBJ_DISPATCHER)
#define IS_STRUCT_SCHEMA(value) isObjType(value, OBJ_STRUCT_SCHEMA)
#define IS_STRUCT_INSTANCE(value) isObjType(value, OBJ_STRUCT_INSTANCE)
#define IS_ENUM_SCHEMA(value) isObjType(value, OBJ_ENUM_SCHEMA)

#define AS_STRING(value)      ((ObjString*)AS_OBJ(value))
#define AS_FUNCTION(value)    ((ObjFunction*)AS_OBJ(value))
#define AS_NATIVE_FUNCTION(value) ((ObjNativeFunction*)AS_OBJ(value))
#define AS_NATIVE_CONTEXT(value) ((ObjNativeContext*)AS_OBJ(value))
#define AS_NATIVE_CLOSURE(value) ((ObjNativeClosure*)AS_OBJ(value))
#define AS_CLOSURE(value)     ((ObjClosure*)AS_OBJ(value))
#define AS_UPVALUE(value)     ((ObjUpvalue*)AS_OBJ(value))
#define AS_CSTRING(value)     (((ObjString*)AS_OBJ(value))->chars)
#define AS_LIST(value)        ((ObjList*)AS_OBJ(value))
#define AS_MAP(value)         ((ObjMap*)AS_OBJ(value))
#define AS_DISPATCHER(value)  ((ObjDispatcher*)AS_OBJ(value))
#define AS_STRUCT_SCHEMA(value) ((ObjStructSchema*)AS_OBJ(value))
#define AS_STRUCT_INSTANCE(value) ((ObjStructInstance*)AS_OBJ(value))
#define AS_ENUM_SCHEMA(value) ((ObjEnumSchema*)AS_OBJ(value))

typedef enum {
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_FUNCTION,
    OBJ_NATIVE_FUNCTION,
    OBJ_NATIVE_CONTEXT,
    OBJ_NATIVE_CLOSURE,
    OBJ_INT64,
    OBJ_STRING,
    OBJ_LIST,
    OBJ_MAP,
    OBJ_DISPATCHER,
    OBJ_STRUCT_SCHEMA,
    OBJ_STRUCT_INSTANCE,
    OBJ_ENUM_SCHEMA,
} ObjType;

struct Obj {
    ObjType type;
    bool is_marked;
    struct Obj* next;
#ifdef ZYM_HEAP_CENSUS
    // Census builds only (never shipped): GC epoch at birth, for death-age
    // histograms. See zymCensus* in memory.c.
    uint32_t census_birth;
#endif
};

typedef struct {
    Obj obj;
    int64_t value;
} ObjInt64;

typedef struct ObjString {
    Obj obj;
    int length;
    int byte_length;
    uint32_t hash;
    // Single allocation: content lives on the string's tail
    // (NUL-terminated). Strings are immutable and interned, so nothing
    // ever resizes or re-points this. Header, hash, and short-string
    // content share a cache line; one malloc per string instead of two.
    char chars[];
} ObjString;

typedef struct ObjFunction {
    Obj obj;
    int arity;          // total param count (including rest param)
    int fixed_arity;    // number of fixed params before rest param (== arity if not variadic)
    bool is_variadic;   // true if function has a rest parameter (...args)
    int max_regs;
    // Number of spill slots this function needs. Spill slots do NOT live in
    // the value stack: they are bump-allocated on vm->spill_stack, a parallel
    // array, and the frame records its base in CallFrame.spill_base. They used
    // to sit at [bp+max_regs ..], which put them exactly where a callee's frame
    // is based -- so every call from a spilling function overwrote its own
    // spilled locals. Accessed only via SPILL_LOAD / SPILL_STORE, which take a
    // uint16 slot index relative to the frame's spill base.
    int spill_count;
    Chunk chunk;
    ObjString* name;
    ObjString* module_name;
    Upvalue* upvalues;
    int upvalue_count;
    int upvalue_capacity;
} ObjFunction;

typedef Value (*NativeDispatcher)(VM* vm, Value* args, void* func_ptr);
typedef Value (*NativeVariadicDispatcher)(VM* vm, Value* args, void* func_ptr, int argc);

typedef struct ObjNativeFunction {
    Obj obj;
    ObjString* name;
    int arity;
    void* func_ptr;
    NativeDispatcher dispatcher;              // for fixed-arity natives
    NativeVariadicDispatcher variadic_dispatcher;  // for variadic natives
    bool is_variadic;
} ObjNativeFunction;

typedef void (*NativeFinalizerFunc)(VM* vm, void* native_data);

typedef struct {
    Obj obj;
    void* native_data;
    NativeFinalizerFunc finalizer;
} ObjNativeContext;

typedef struct {
    Obj obj;
    ObjString* name;
    int arity;
    void* func_ptr;
    NativeDispatcher dispatcher;              // for fixed-arity closures
    NativeVariadicDispatcher variadic_dispatcher;  // for variadic closures
    Value context;
    bool is_variadic;
} ObjNativeClosure;


typedef struct ObjUpvalue {
    Obj obj;
    Value* location;
    Value closed;
    struct ObjUpvalue* next;
} ObjUpvalue;

typedef struct {
    Obj obj;
    ObjFunction* function;
    int upvalue_count;
    // Single allocation: the upvalue pointer array lives on the tail of the
    // closure itself (upvalue_count is fixed at creation, so nothing ever
    // resizes it). One malloc instead of two per closure, one less pointer
    // chase per upvalue access, and closure creation can no longer trigger
    // GC between an array and an object that must stay consistent.
    ObjUpvalue* upvalues[];
} ObjClosure;

typedef struct {
    Obj obj;
    ValueArray items;
} ObjList;

typedef struct {
    Obj obj;
    Table table;
} ObjMap;

#define MAX_OVERLOADS 16
typedef struct {
    Obj obj;
    Obj* overloads[MAX_OVERLOADS];
    int count;
    Obj* variadic_fallback;     // closure/native for variadic fallback (NULL if none)
    int variadic_min_arity;     // minimum args required by the variadic fallback
} ObjDispatcher;


typedef struct ObjStructSchema {
    Obj obj;
    ObjString* name;
    int field_count;
    ObjString** field_names;
} ObjStructSchema;

// Fast field index lookup using interned string pointer comparison.
// For typical struct sizes (2-10 fields), a linear scan of pointer
// comparisons is faster than a hash table lookup.
static inline int find_field_index(ObjStructSchema* schema, ObjString* name) {
    for (int i = 0; i < schema->field_count; i++) {
        if (schema->field_names[i] == name) return i;
    }
    return -1;
}

typedef struct ObjStructInstance {
    Obj obj;
    ObjStructSchema* schema;
    int field_count;
    Value* fields;
} ObjStructInstance;

typedef struct ObjEnumSchema {
    Obj obj;
    ObjString* name;
    int type_id;
    int variant_count;
    ObjString** variant_names;
} ObjEnumSchema;

ObjFunction* newFunction(VM* vm);
ObjNativeFunction* newNativeFunction(VM* vm, ObjString* name, int arity, void* func_ptr, NativeDispatcher dispatcher);
ObjNativeContext* newNativeContext(VM* vm, void* native_data, NativeFinalizerFunc finalizer);
ObjNativeClosure* newNativeClosure(VM* vm, ObjString* name, int arity, void* func_ptr, NativeDispatcher dispatcher, Value context);
// ---- String hashing ------------------------------------------------------
// ONE hash function per content, everywhere a string enters or probes the
// intern table (copy/take/concat/concatN, and zym_mapHas from the C API).
// Length-dispatched: FNV-1a for short keys (< ZYM_HASH_BULK_MIN bytes),
// where its two-instruction step beats any wide mixer's setup cost and it
// spreads 1-3 byte keys slightly better; an 8-bytes-per-step multiplicative
// mixer above that, where FNV's byte-serial dependent multiply chain is the
// bottleneck (measured 6x on 80-byte concat results, equal collision
// behavior at the birthday floor). Length is part of the content, so equal
// strings always take the same branch -- deterministic, no dual-hash risk.
//
// The streaming form exists so concatenation can hash across segment
// boundaries without materializing the joined bytes: it stages bytes into
// an 8-byte word and mixes each full word, and its result is bit-identical
// to zymHashString over the joined content (the invariant interning needs).
#define ZYM_HASH_BULK_MIN 8

typedef struct {
    uint64_t seed;      // running mixer state (bulk path)
    uint64_t stage;     // partial word being filled
    int      staged;    // bytes currently in `stage` (0..7)
    uint32_t fnv;       // running FNV state (short path)
    int      total;     // total length, decides the path at finish
    int      expected;  // declared total length (fixed at init)
} ZymHashStream;

uint32_t zymHashString(const char* key, int length);
void     zymHashInit(ZymHashStream* h, int total_length);
void     zymHashFeed(ZymHashStream* h, const char* bytes, int length);
uint32_t zymHashFinish(ZymHashStream* h);

ObjString* takeString(VM* vm, char* chars, int length);
ObjString* copyString(VM* vm, const char* chars, int length);
// Concatenation without a temporary buffer: hash streams across both
// halves, the intern probe compares in two segments, and on a miss the
// content is written directly into the new string's tail.
ObjString* concatStrings(VM* vm, ObjString* a, ObjString* b);
// N-ary form for CONCAT_N: `parts[0..count)` are all strings (the caller
// checked). One sized allocation, hash streamed across every piece, and an
// n-segment intern probe -- no intermediates. Sizes are summed in size_t and
// bounded to the int byte_length range before allocating.
ObjString* concatStringsN(VM* vm, Value* parts, int count);
void printObject(Value value);
Obj* allocateObject(VM* vm, size_t size, ObjType type);
ObjClosure* newClosure(VM* vm, ObjFunction* function);
ObjList* newList(VM* vm);
ObjMap* newMap(VM* vm);
ObjDispatcher* newDispatcher(VM* vm);
ObjStructSchema* newStructSchema(VM* vm, ObjString* name, ObjString** field_names, int field_count);
ObjStructInstance* newStructInstance(VM* vm, ObjStructSchema* schema);
ObjEnumSchema* newEnumSchema(VM* vm, ObjString* name, ObjString** variant_names, int variant_count);
