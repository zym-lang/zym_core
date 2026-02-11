#pragma once

#include "./common.h"
#include "./value.h"
#include "compiler.h"

typedef struct VM VM;
typedef struct ObjFunction ObjFunction;
typedef struct Table Table;
typedef struct ObjPromptTag ObjPromptTag;
typedef struct ObjContinuation ObjContinuation;
typedef struct CallFrame CallFrame;

#define isObjType(value, objectType) (IS_OBJ(value) && AS_OBJ(value)->type == objectType)
#define OBJ_TYPE(value)     (AS_OBJ(value)->type)

#define IS_STRING(value)      isObjType(value, OBJ_STRING)
#define IS_FUNCTION(value)    isObjType(value, OBJ_FUNCTION)
#define IS_NATIVE_FUNCTION(value) isObjType(value, OBJ_NATIVE_FUNCTION)
#define IS_NATIVE_CONTEXT(value) isObjType(value, OBJ_NATIVE_CONTEXT)
#define IS_NATIVE_CLOSURE(value) isObjType(value, OBJ_NATIVE_CLOSURE)
#define IS_NATIVE_REFERENCE(value) isObjType(value, OBJ_NATIVE_REFERENCE)
#define IS_CLOSURE(value)     isObjType(value, OBJ_CLOSURE)
#define IS_UPVALUE(value)     isObjType(value, OBJ_UPVALUE)
#define IS_LIST(value)        isObjType(value, OBJ_LIST)
#define IS_MAP(value)         isObjType(value, OBJ_MAP)
#define IS_DISPATCHER(value)  isObjType(value, OBJ_DISPATCHER)
#define IS_REFERENCE(value)   isObjType(value, OBJ_REFERENCE)
#define IS_STRUCT_SCHEMA(value) isObjType(value, OBJ_STRUCT_SCHEMA)
#define IS_STRUCT_INSTANCE(value) isObjType(value, OBJ_STRUCT_INSTANCE)
#define IS_ENUM_SCHEMA(value) isObjType(value, OBJ_ENUM_SCHEMA)
#define IS_PROMPT_TAG(value)  isObjType(value, OBJ_PROMPT_TAG)
#define IS_CONTINUATION(value) isObjType(value, OBJ_CONTINUATION)

#define AS_STRING(value)      ((ObjString*)AS_OBJ(value))
#define AS_FUNCTION(value)    ((ObjFunction*)AS_OBJ(value))
#define AS_NATIVE_FUNCTION(value) ((ObjNativeFunction*)AS_OBJ(value))
#define AS_NATIVE_CONTEXT(value) ((ObjNativeContext*)AS_OBJ(value))
#define AS_NATIVE_CLOSURE(value) ((ObjNativeClosure*)AS_OBJ(value))
#define AS_NATIVE_REFERENCE(value) ((ObjNativeReference*)AS_OBJ(value))
#define AS_CLOSURE(value)     ((ObjClosure*)AS_OBJ(value))
#define AS_UPVALUE(value)     ((ObjUpvalue*)AS_OBJ(value))
#define AS_CSTRING(value)     (((ObjString*)AS_OBJ(value))->chars)
#define AS_LIST(value)        ((ObjList*)AS_OBJ(value))
#define AS_MAP(value)         ((ObjMap*)AS_OBJ(value))
#define AS_DISPATCHER(value)  ((ObjDispatcher*)AS_OBJ(value))
#define AS_REFERENCE(value)   ((ObjReference*)AS_OBJ(value))
#define AS_STRUCT_SCHEMA(value) ((ObjStructSchema*)AS_OBJ(value))
#define AS_STRUCT_INSTANCE(value) ((ObjStructInstance*)AS_OBJ(value))
#define AS_ENUM_SCHEMA(value) ((ObjEnumSchema*)AS_OBJ(value))
#define AS_PROMPT_TAG(value)  ((ObjPromptTag*)AS_OBJ(value))
#define AS_CONTINUATION(value) ((ObjContinuation*)AS_OBJ(value))

typedef enum {
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_FUNCTION,
    OBJ_NATIVE_FUNCTION,
    OBJ_NATIVE_CONTEXT,
    OBJ_NATIVE_CLOSURE,
    OBJ_NATIVE_REFERENCE,
    OBJ_INT64,
    OBJ_STRING,
    OBJ_LIST,
    OBJ_MAP,
    OBJ_DISPATCHER,
    OBJ_REFERENCE,
    OBJ_STRUCT_SCHEMA,
    OBJ_STRUCT_INSTANCE,
    OBJ_ENUM_SCHEMA,
    OBJ_PROMPT_TAG,
    OBJ_CONTINUATION,
} ObjType;

struct Obj {
    ObjType type;
    bool is_marked;
    struct Obj* next;
};

typedef struct {
    Obj obj;
    int64_t value;
} ObjInt64;

typedef struct ObjString {
    Obj obj;
    int length;
    int byte_length;
    char* chars;
    uint32_t hash;
} ObjString;

// Qualifier signature for fast-path call optimization
typedef enum {
    QUAL_SIG_ALL_NORMAL_NO_REFS = 0,  // Fastest: skip all qualifier processing
    QUAL_SIG_ALL_NORMAL = 1,           // Fast: only check for references to deref
    QUAL_SIG_HAS_QUALIFIERS = 2        // Slow: full qualifier processing needed
} QualifierSignature;

typedef struct ObjFunction {
    Obj obj;
    int arity;
    int max_regs;
    Chunk* chunk;
    ObjString* name;
    ObjString* module_name;
    Upvalue upvalues[MAX_LOCALS];
    int upvalue_count;
    uint8_t* param_qualifiers;
    uint8_t qualifier_sig;  // QualifierSignature for call fast-path
} ObjFunction;

typedef Value (*NativeDispatcher)(VM* vm, Value* args, void* func_ptr);

typedef struct ObjNativeFunction {
    Obj obj;
    ObjString* name;
    int arity;
    uint8_t* param_qualifiers;
    uint8_t qualifier_sig;  // QualifierSignature for call fast-path
    void* func_ptr;
    NativeDispatcher dispatcher;
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
    uint8_t* param_qualifiers;
    uint8_t qualifier_sig;  // QualifierSignature for call fast-path
    void* func_ptr;
    NativeDispatcher dispatcher;
    Value context;
} ObjNativeClosure;

typedef Value (*NativeRefGetHook)(VM* vm, Value context, Value current_value);
typedef void (*NativeRefSetHook)(VM* vm, Value context, Value new_value);

typedef struct {
    Obj obj;
    Value context;
    size_t value_offset;
    NativeRefGetHook get_hook;
    NativeRefSetHook set_hook;
} ObjNativeReference;

typedef struct ObjUpvalue {
    Obj obj;
    Value* location;
    Value closed;
    struct ObjUpvalue* next;
} ObjUpvalue;

typedef struct {
    Obj obj;
    ObjFunction* function;
    ObjUpvalue** upvalues;
    int upvalue_count;
} ObjClosure;

typedef struct {
    Obj obj;
    ValueArray items;
} ObjList;

typedef struct {
    Obj obj;
    struct Table* table;
} ObjMap;

#define MAX_OVERLOADS 16
typedef struct {
    Obj obj;
    Obj* overloads[MAX_OVERLOADS];
    int count;
} ObjDispatcher;

typedef enum {
    REF_LOCAL,
    REF_GLOBAL,
    REF_INDEX,
    REF_PROPERTY,
    REF_UPVALUE
} RefType;

typedef struct {
    Obj obj;
    RefType ref_type;
    union {
        struct {
            Value* location;
        } local;
        struct {
            ObjString* global_name;
        } global;
        struct {
            Value container;
            Value index;
        } index;
        struct {
            Value container;
            Value key;
        } property;
        struct {
            ObjUpvalue* upvalue;
        } upvalue;
    } as;
} ObjReference;

typedef struct ObjStructSchema {
    Obj obj;
    ObjString* name;
    int field_count;
    ObjString** field_names;
    Table* field_to_index;
} ObjStructSchema;

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

typedef struct ObjPromptTag {
    Obj obj;
    uint32_t id;
    ObjString* name;
} ObjPromptTag;

typedef enum {
    CONT_VALID,
    CONT_CONSUMED,
    CONT_INVALID
} ContinuationState;

typedef struct ObjContinuation {
    Obj obj;
    CallFrame* frames;
    int frame_count;
    Value* stack;
    int stack_size;
    int stack_base_offset;
    uint32_t* saved_ip;
    Chunk* saved_chunk;
    ObjPromptTag* prompt_tag;
    ContinuationState state;
    int return_slot;
} ObjContinuation;

ObjFunction* newFunction(VM* vm);
ObjNativeFunction* newNativeFunction(VM* vm, ObjString* name, int arity, void* func_ptr, NativeDispatcher dispatcher);
ObjNativeContext* newNativeContext(VM* vm, void* native_data, NativeFinalizerFunc finalizer);
ObjNativeClosure* newNativeClosure(VM* vm, ObjString* name, int arity, void* func_ptr, NativeDispatcher dispatcher, Value context);
ObjNativeReference* newNativeReference(VM* vm, Value context, size_t value_offset, NativeRefGetHook get_hook, NativeRefSetHook set_hook);
ObjString* takeString(VM* vm, char* chars, int length);
ObjString* copyString(VM* vm, const char* chars, int length);
void printObject(Value value);
Obj* allocateObject(VM* vm, size_t size, ObjType type);
ObjClosure* newClosure(VM* vm, ObjFunction* function);
ObjList* newList(VM* vm);
ObjMap* newMap(VM* vm);
ObjDispatcher* newDispatcher(VM* vm);
ObjReference* newReference(VM* vm, Value* location);
ObjReference* newStackSlotReference(VM* vm, int slot_index);
ObjReference* newGlobalReference(VM* vm, ObjString* global_name);
ObjReference* newIndexReference(VM* vm, Value container, Value index);
ObjReference* newPropertyReference(VM* vm, Value container, Value key);
ObjReference* newUpvalueReference(VM* vm, ObjUpvalue* upvalue);
ObjStructSchema* newStructSchema(VM* vm, ObjString* name, ObjString** field_names, int field_count);
ObjStructInstance* newStructInstance(VM* vm, ObjStructSchema* schema);
ObjEnumSchema* newEnumSchema(VM* vm, ObjString* name, ObjString** variant_names, int variant_count);
ObjPromptTag* newPromptTag(VM* vm, ObjString* name);
ObjContinuation* newContinuation(VM* vm);