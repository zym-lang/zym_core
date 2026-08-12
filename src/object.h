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
typedef struct ObjPromptTag ObjPromptTag;
typedef struct ObjContinuation ObjContinuation;
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
#define IS_PROMPT_TAG(value)  isObjType(value, OBJ_PROMPT_TAG)
#define IS_CONTINUATION(value) isObjType(value, OBJ_CONTINUATION)

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
#define AS_PROMPT_TAG(value)  ((ObjPromptTag*)AS_OBJ(value))
#define AS_CONTINUATION(value) ((ObjContinuation*)AS_OBJ(value))

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
    ObjUpvalue** upvalues;
    int upvalue_count;
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
    // Snapshot of the captured frames' spilled locals, and the vm->spill_stack
    // offset it was taken from. Spilled locals do not live in the value stack
    // (see CallFrame.spill_base), so copying `stack` does not carry them: the
    // frames memcpy above brings each frame's spill_base along as a raw
    // integer, which means nothing once vm->spill_top has moved on. Without
    // this the resumed frame and the next callee pushed above it are handed the
    // same spill offsets, and nothing roots the values in the meantime.
    //
    // The extent is [frames[prompt_frame].spill_base, top-of-captured-range),
    // rebased on resume exactly as `stack` is rebased through
    // stack_base_offset. Freed with the other two buffers at resume and in
    // freeObject; marked alongside `stack` in blackenObject.
    Value* spill;
    int spill_size;
    int spill_base_offset;
    // Prompts that were live INSIDE the captured extent, i.e. everything the
    // capture's own prompt entry delimits from below. They are as much a part
    // of the extent as its frames: resuming puts the body back dynamically
    // inside `withPrompt(INNER, ...)`, so the INNER bookmark has to come back
    // with it or the resumed code cannot name a prompt it is demonstrably
    // standing in.
    //
    // Entries hold absolute frame/stack indices, meaningless once the VM has
    // moved on, so they are rebased on restore exactly as the frames are --
    // frame_index through `frame_base_offset`, stack_base through
    // `stack_base_offset`. The delimiting prompt itself is NOT in here: a
    // captured continuation is undelimited until somebody wraps it again.
    PromptEntry* prompts;
    int prompt_count;
    // Resume boundaries that were live INSIDE the captured extent. A boundary
    // is the only thing that tells RET where a restored frame's return value
    // goes: the bottom frame of a splice sits at a stack_base the restore
    // picked, not at its caller's result register, so the callee-writes-to-its-
    // own-stack_base convention RET otherwise uses lands the value in a dead
    // slot. vm->resume_stack carries that mapping while the computation runs.
    //
    // A continuation captured across a previous resume contains such a splice
    // point, and it is as much part of the extent as the frames on either side
    // of it. Dropping it made the defect invisible until the computation
    // actually ran to completion -- every re-suspension unwinds through capture
    // instead of returning, so only the final step exercises the linkage, and
    // it delivered whatever stale value the result register happened to hold
    // (in practice the callee: `Cont.resume` itself).
    //
    // Entries hold absolute indices and are rebased on restore exactly as the
    // prompts are: frame_boundary through `frame_base_offset`, result_slot
    // through `stack_base_offset`. The boundary belonging to the resume that is
    // splicing this extent back in is NOT in here -- that one exits the extent,
    // and Cont.resume pushes it for itself.
    ResumeContext* resumes;
    int resume_count;
    // Absolute frame index the captured frames started at (the delimiting
    // prompt's frame_index). The frame counterpart of stack_base_offset /
    // spill_base_offset: restored frame_index = vm->frame_count + (saved -
    // this). Needed even when frame_count is 0, so it is stored rather than
    // derived from frames[0].
    int frame_base_offset;
    uint32_t* saved_ip;
    Chunk* saved_chunk;
    // The ObjFunction whose embedded chunk `saved_chunk` is, or NULL when the
    // resume target lives in a host-owned chunk. A Chunk is not a GC object and
    // carries no back-pointer, so marking `saved_chunk` cannot keep its storage
    // alive -- only marking this owner can. NULL means no owner can be marked,
    // and the chunk is invalidated by zym_freeChunk instead.
    ObjFunction* saved_owner;
    ObjPromptTag* prompt_tag;
    ContinuationState state;
    int return_slot;
    int preempt_shield_depth;   // script critical-section depth at capture
} ObjContinuation;

ObjFunction* newFunction(VM* vm);
ObjNativeFunction* newNativeFunction(VM* vm, ObjString* name, int arity, void* func_ptr, NativeDispatcher dispatcher);
ObjNativeContext* newNativeContext(VM* vm, void* native_data, NativeFinalizerFunc finalizer);
ObjNativeClosure* newNativeClosure(VM* vm, ObjString* name, int arity, void* func_ptr, NativeDispatcher dispatcher, Value context);
ObjString* takeString(VM* vm, char* chars, int length);
ObjString* copyString(VM* vm, const char* chars, int length);
void printObject(Value value);
Obj* allocateObject(VM* vm, size_t size, ObjType type);
ObjClosure* newClosure(VM* vm, ObjFunction* function);
ObjList* newList(VM* vm);
ObjMap* newMap(VM* vm);
ObjDispatcher* newDispatcher(VM* vm);
ObjStructSchema* newStructSchema(VM* vm, ObjString* name, ObjString** field_names, int field_count);
ObjStructInstance* newStructInstance(VM* vm, ObjStructSchema* schema);
ObjEnumSchema* newEnumSchema(VM* vm, ObjString* name, ObjString** variant_names, int variant_count);
ObjPromptTag* newPromptTag(VM* vm, ObjString* name);
ObjContinuation* newContinuation(VM* vm);