#pragma once

#include "./chunk.h"
#include "./value.h"
#include "./table.h"
#include "./config.h"

/*
 * VM Configuration Limits
 *
 * These arrays are pre-allocated in the VM struct. Memory usage on 64-bit:
 *
 *   CallFrame:   32 bytes each (closure, ip, stack_base, caller_chunk + padding)
 *   PromptEntry: 16 bytes each (tag, frame_index, stack_base)
 *   ResumeContext: 8 bytes each (frame_boundary, result_slot)
 *
 * ┌─────────────┬─────────────────────────────────────────────────┐
 * │   Count     │  32      64      128     256     512            │
 * ├─────────────┼─────────────────────────────────────────────────┤
 * │ FRAMES_MAX  │  1 KB    2 KB    4 KB    8 KB    16 KB          │
 * │ MAX_PROMPTS │  0.5 KB  1 KB    2 KB    4 KB    8 KB           │
 * │ RESUME_DEPTH│  0.25 KB 0.5 KB  1 KB    2 KB    4 KB           │
 * └─────────────┴─────────────────────────────────────────────────┘
 *
 * Notes:
 *   - FRAMES_MAX limits active call depth (recursion, and resuming continuations)
 *   - MAX_PROMPTS limits concurrent prompt boundaries (bookmarks for continuations)
 *   - Captured continuations are heap-allocated, not limited by these values
 *   - Value stack is dynamic (STACK_INITIAL to STACK_MAX), 8 bytes per Value
 */
#define FRAMES_MAX 64
#define STACK_MAX 65536
#define STACK_INITIAL 256
#define MAX_PROMPTS 32
#define DEFAULT_TIMESLICE 10000
#define MAX_RESUME_DEPTH 16
#define MAX_WITH_PROMPT_DEPTH 16

typedef struct ObjPromptTag ObjPromptTag;

struct CallFrame {
    ObjClosure* closure;
    uint32_t* ip;
    int stack_base;
    Chunk* caller_chunk;
};
typedef struct CallFrame CallFrame;

typedef struct {
    ObjPromptTag* tag;
    int frame_index;
    int stack_base;
} PromptEntry;

typedef struct {
    int frame_boundary;
    int result_slot;
} ResumeContext;

typedef struct {
    int frame_boundary;
} WithPromptContext;

typedef struct VM {
    Chunk* chunk;
    uint32_t* ip;

    Value* stack;
    int stack_capacity;
    int stack_top;
    Table globals;
    ValueArray globalSlots;
    Table strings;

    CallFrame frames[FRAMES_MAX];
    int frame_count;

    Obj* objects;
    ObjUpvalue* open_upvalues;

    int api_stack_top;
    Chunk api_trampoline;

    int next_enum_type_id;
    ObjString* entry_file;

    // Garbage Collector
    size_t bytes_allocated;
    size_t next_gc;
    Obj** gray_stack;
    int gray_count;
    int gray_capacity;
    bool gc_enabled;
    struct Compiler* compiler;

    Obj** temp_roots;
    int temp_root_count;
    int temp_root_capacity;

    PromptEntry prompt_stack[MAX_PROMPTS];
    int prompt_count;
    uint32_t next_prompt_tag_id;

    int yield_budget;
    bool preempt_requested;
    bool preemption_enabled;
    int default_timeslice;

    ResumeContext resume_stack[MAX_RESUME_DEPTH];
    int resume_depth;

    WithPromptContext with_prompt_stack[MAX_WITH_PROMPT_DEPTH];
    int with_prompt_depth;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
    INTERPRET_YIELD
} InterpretResult;

static inline Chunk* currentChunk(VM* vm) {
    if (vm->frame_count > 0) {
        return vm->frames[vm->frame_count - 1].closure->function->chunk;
    }
    return vm->chunk;
}

void initVM(VM* vm);
void freeVM(VM* vm);
void runtimeError(VM* vm, const char* format, ...);

void updateStackReferences(VM* vm, Value* old_stack, Value* new_stack);
void closeUpvalues(VM* vm, Value* last);
void protectLocalRefsInValue(VM* vm, Value value, Value* frame_start);

bool globalGet(VM* vm, ObjString* name, Value* out_value);
bool globalSet(VM* vm, ObjString* name, Value value);

InterpretResult runChunk(VM* vm, Chunk* chunk);

bool zym_call_prepare(VM* vm, const char* functionName, int arity);

void zym_pushNumber(VM* vm, double number);
void zym_pushString(VM* vm, const char* string);
void zym_pushNull(VM* vm);
void zym_pushBool(VM* vm, bool value);

InterpretResult zym_call_execute(VM* vm, int argCount);

Value zym_call_getResult(VM* vm);