#pragma once

#include "./chunk.h"
#include "./value.h"
#include "./table.h"
#include <stdint.h>
#include "./config.h"
#include "./allocator.h"
#include "./source_file.h"
#include "./diagnostics.h"

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
#define FRAMES_MAX 512
#define STACK_MAX 65536
#define STACK_INITIAL 256
#define MAX_PROMPTS 64
#define DEFAULT_TIMESLICE 10000
#define MAX_RESUME_DEPTH 64
#define MAX_WITH_PROMPT_DEPTH 64
#define FRAME_FLAG_PREEMPT 0x01
#define FRAME_FLAG_DISABLE_PREEMPT 0x02

typedef struct ObjPromptTag ObjPromptTag;

struct CallFrame {
    ObjClosure* closure;
    uint32_t* ip;
    int stack_base;
    Chunk* caller_chunk;
    int flags;
    uint16_t arg_count;  // actual number of args passed to this call (for variadic PACK_REST)
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

// Error callback: if set, error messages are routed here instead of stderr.
// type: ZYM_STATUS_COMPILE_ERROR or ZYM_STATUS_RUNTIME_ERROR
typedef void (*ErrorCallback)(struct VM* vm, ZymStatus type, const char* file,
                              int line, const char* message, void* user_data);

typedef struct VM {
    ZymAllocator allocator;

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
    int cur_base;
    int active_boundaries;
    CallFrame* current_frame;

    Obj* objects;
    ObjUpvalue* open_upvalues;

    int api_stack_top;
    Chunk api_trampoline;

    int next_enum_type_id;
    ObjString* entry_file;

    // Garbage Collector
    size_t bytes_allocated;
    size_t next_gc;
    int32_t gc_debt;  // Allocation debt counter: triggers GC when <= 0; INT32_MAX when GC disabled
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

    int32_t preempt_counter;
    int32_t saved_budget;
    bool preempt_requested;
    bool preemption_enabled;
    int preemption_disable_depth;
    Value on_preempt_callback;
    int default_timeslice;

    ResumeContext resume_stack[MAX_RESUME_DEPTH];
    int resume_depth;

    WithPromptContext with_prompt_stack[MAX_WITH_PROMPT_DEPTH];
    int with_prompt_depth;

    // Cached: active_boundaries = with_prompt_depth + resume_depth
    // Used for a single fast check in RET/TAIL_CALL instead of two separate checks

    // Error callback (NULL = default fprintf to stderr)
    ErrorCallback error_callback;
    void* error_user_data;

    // Phase 1.1: per-VM registry of source files whose bytes scanner tokens
    // (and future diagnostics / parse tree / symbol table) reference by id.
    SourceFileRegistry source_files;

    // Phase 1.3: structured diagnostics sink. Populated by the frontend
    // (parser/compiler/…) via pushDiagnostic(); drained by embedders via
    // zymGetDiagnostics() / zymClearDiagnostics().
    DiagnosticSink diagnostics;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
    INTERPRET_YIELD
} InterpretResult;

static inline Chunk* currentChunk(VM* vm) {
    return vm->chunk;
}

void initVM(VM* vm);
void freeVM(VM* vm);
void runtimeError(VM* vm, const char* format, ...);

void updateStackReferences(VM* vm, Value* old_stack, Value* new_stack);
void closeUpvalues(VM* vm, Value* last);
void unwindFrames(VM* vm, int new_frame_count);
void protectLocalRefsInValue(VM* vm, Value value, Value* frame_start);

bool globalGet(VM* vm, ObjString* name, Value* out_value);
bool globalSet(VM* vm, ObjString* name, Value value);

InterpretResult runVM(VM* vm);
InterpretResult runChunk(VM* vm, Chunk* chunk);

bool zym_call_prepare(VM* vm, const char* functionName, int arity);

void zym_pushNumber(VM* vm, double number);
void zym_pushString(VM* vm, const char* string);
void zym_pushNull(VM* vm);
void zym_pushBool(VM* vm, bool value);

InterpretResult zym_call_execute(VM* vm, int argCount);

Value zym_call_getResult(VM* vm);