#pragma once

#include "./chunk.h"
#include "./value.h"
#include "./table.h"
#include <stdint.h>
#include "./config.h"
#include "zym/config.h" /* ZYM_HAS_* feature flags */
#include "./allocator.h"
#include "./source_file.h"
#include "./diagnostics.h"
#include <signal.h> /* sig_atomic_t for compile cancellation flag */

/*
 * VM Configuration Limits
 *
 * These arrays are pre-allocated in the VM struct. Memory usage on 64-bit:
 *
 *   CallFrame:    32 bytes each (closure, ip, stack_base, caller_chunk + padding)
 *   PromptEntry:  16 bytes each (tag, frame_index, stack_base)
 *   ResumeContext: 8 bytes each (frame_boundary, result_slot)
 *   PreemptEntry: 24 bytes each (slice, remaining, callback, id, flags + padding)
 *
 * ┌─────────────┬─────────────────────────────────────────────────┐
 * │   Count     │  8       16      32      64      256            │
 * ├─────────────┼─────────────────────────────────────────────────┤
 * │ FRAMES_MAX  │  0.25 KB 0.5 KB  1 KB    2 KB    8 KB           │
 * │ MAX_PROMPTS │  128 B   256 B   0.5 KB  1 KB    4 KB           │
 * │ RESUME_DEPTH│  64 B    128 B   256 B   0.5 KB  2 KB           │
 * │ PREEMPT_MAX │  192 B   384 B   768 B   1.5 KB  6 KB           │
 * └─────────────┴─────────────────────────────────────────────────┘
 *
 * Notes:
 *   - FRAMES_MAX limits active call depth (recursion, and resuming continuations)
 *   - MAX_PROMPTS limits concurrent prompt boundaries (bookmarks for continuations)
 *   - Captured continuations are heap-allocated, not limited by these values
 *   - Value stack is dynamic (STACK_INITIAL to STACK_MAX), 8 bytes per Value
 *   - ZYM_PREEMPT_MAX_ENTRIES is one table shared by host and script entries.
 *     handlePreemption also builds a scratch array of one pointer per slot on
 *     the C stack, so a slot costs 24 bytes of VM plus 8 bytes of stack during
 *     an expiry. Scanning is O(slots) on the cold path only; the dispatch loop
 *     stays one decrement and one branch whatever the size.
 */
#ifndef FRAMES_MAX
#define FRAMES_MAX 256
#endif
#ifndef STACK_MAX
#define STACK_MAX 65536
#endif
#define STACK_INITIAL 256
#define MAX_PROMPTS 64
#define DEFAULT_TIMESLICE 10000
#define MAX_RESUME_DEPTH 64
#define MAX_WITH_PROMPT_DEPTH 64
// ---- Preemption entry table ---------------------------------------------
// Fixed size, no allocation: registration fails when full. Keeps the MCU
// profile honest.
//
// Overridable at build time like FRAMES_MAX and STACK_MAX. The table lives
// inline in the VM struct, so each slot costs sizeof(PreemptEntry) (24 bytes)
// per VM plus one pointer of stack in handlePreemption's scratch array. The
// capacity is shared between host and script entries; hosts should read
// zym_preemptCapacity() rather than assume a number.
#ifndef ZYM_PREEMPT_MAX_ENTRIES
#define ZYM_PREEMPT_MAX_ENTRIES 8
#endif

#define ZYM_PREEMPT_F_MASKABLE  (1u << 0)  // suppressed by a script shield
#define ZYM_PREEMPT_F_ONESHOT   (1u << 1)  // do not rearm after firing

typedef struct {
    int32_t  slice;        // rearm value; 0 means the slot is free
    int32_t  remaining;    // counts down toward 0
    Value    callback;     // NULL_VAL => abort execution instead of calling
    uint32_t id;           // 0 is never a valid id
    uint8_t  flags;
    bool     owner_script; // false => host-owned
    bool     in_flight;    // this entry's callback is executing
} PreemptEntry;

#define FRAME_FLAG_PREEMPT 0x01
#define FRAME_FLAG_DISABLE_PREEMPT 0x02
// Re-entrant API boundary: this frame was pushed by a public API call
// (`zym_call`/`zym_callClosurev` -> `zym_call_execute`) into a VM that
// may already be mid-bytecode execution. When OP(RET) pops a frame
// carrying this flag, control must return to the C caller of
// `zym_call_execute` immediately, **without** falling through into the
// api_trampoline's RET (which would cascade-pop every suspended caller
// frame, NULL-ing their stack_base slots and corrupting their locals).
#define FRAME_FLAG_API_BOUNDARY 0x04

typedef struct ObjPromptTag ObjPromptTag;

struct CallFrame {
    ObjClosure* closure;
    uint32_t* ip;
    int stack_base;
    Chunk* caller_chunk;
    int flags;
    uint16_t arg_count;  // actual number of args passed to this call (for variadic PACK_REST)
    uint32_t preempt_id; // preemption entry whose callback this frame runs (0 = none)
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
    // Cursor used by the spread-call layout sequence
    // (CALL_ARG_PREP / CALL_ARG_SPREAD / CALL_VAR). Holds the absolute
    // stack index of the next free argument slot during a spread call.
    // Outside of that opcode triple it is meaningless.
    int call_arg_top;
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
    // Byte ceiling for this VM; 0 means unlimited. Checked in reallocate on
    // the growth path. Crossing it does NOT fail the allocation -- real memory
    // is still there, so the request is satisfied and the VM is suspended at
    // the next instruction boundary instead. That is what makes it
    // recoverable: the host can raise the limit, drop references, or tear the
    // VM down, where a failed allocation would leave nothing to decide.
    size_t memory_limit;
    // Sticky, like stop_requested: set when the ceiling is crossed, cleared
    // only by the host. Without stickiness a resume would run straight back
    // over the limit and the host would never regain control.
    bool oom_pending;
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

    // ---- Preemption -------------------------------------------------
    // One countdown drives an entry table. The dispatch loop only ever
    // decrements `preempt_counter`; all table work happens on expiry in
    // the cold handler, so the hot path stays at 1 decrement + 1 branch.
    //
    // `stop_requested` is the unmaskable hard stop. It is checked BEFORE
    // any masking so a shield, a running callback, or a table with no
    // entries can never suppress it, and it is never cleared by the VM.
    // Declared like `compile_cancelled` so an ISR or another thread can
    // set it safely.
    int32_t preempt_counter;
    int32_t preempt_armed;          // value the counter was armed with
    volatile sig_atomic_t stop_requested;
    // True only while execution is suspended and resumable (the last
    // run returned YIELD or ABORTED). `chunk`/`ip` remain set after a
    // completed run, so they cannot answer this on their own.
    bool execution_suspended;

    // ---- Reported state ---------------------------------------------
    // What the VM is, and why. `vm_state` tracks execution; `vm_cause`
    // latches the reason for the last transition out of RUNNING and
    // survives until the next run starts, so it stays readable after the
    // fact. The detail fields are only meaningful for their own cause.
    ZymVmState vm_state;
    ZymVmCause vm_cause;
    ZymPreemptId cause_preempt_id;   // entry that fired, for the preempt causes
    size_t cause_bytes_wanted;       // request that crossed the ceiling
    PreemptEntry preempt_table[ZYM_PREEMPT_MAX_ENTRIES];
    // Slots the host keeps for itself. Script's ceiling is
    // ZYM_PREEMPT_MAX_ENTRIES - host_preempt_reserve; the host itself may still
    // use any free slot. Settable only before the VM has ever executed, so a
    // script's budget cannot shrink under it mid-run: whatever capacity it sees
    // at the start is still bindable at the end.
    int host_preempt_reserve;
    // Latched on the first run/resume/call. Gates the reserve, which must be a
    // bring-up decision -- a host that discovers mid-run that it needs slots has
    // already lost the argument.
    bool has_executed;
    uint32_t preempt_next_id;
    int preempt_live_count;
    int preempt_shield_depth;       // script critical sections; masks
                                    // script-owned MASKABLE entries only
    Value on_preempt_callback;      // legacy single-callback shim
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

    // Phase 1.5: cooperative cancellation flag for the frontend pipeline.
    // `volatile sig_atomic_t` is the minimum C99 primitive that is safe
    // to read/write across threads without a full atomics dependency —
    // the parser and compiler only need to observe a monotonic 0 → 1
    // transition, never a tearing read. Flipped by zymRequestCancel()
    // from an arbitrary thread; reset to 0 by zymClearCancel() before
    // starting a new compile. Parser polls at every declaration boundary;
    // compiler polls at every statement-emit boundary.
    volatile sig_atomic_t compile_cancelled;

    // Module loader: pointer to the *currently active* ImportStack (an
    // opaque struct defined in module_loader.c) and its current depth, set
    // by `load_module_recursive` around each `read_callback` invocation
    // and cleared on return. Used by the public `zym_currentImport*`
    // accessors so an embedder's read_callback can answer "who asked?"
    // without changing the callback signature. NULL / 0 outside an active
    // read_callback frame. The pointer is borrowed (never owned by VM).
    void* current_import_stack;
    int current_import_count;

#if ZYM_HAS_BUILD_TESTING
    // Phase 4.5: compiler resolution-trace buffer used by the parity test
    // between `resolver.c` and `compiler.c`. Non-NULL only between
    // zym_compilerTraceBegin() and zym_compilerTraceEnd(). Shipping builds
    // do not have this field.
    struct ZymResolutionTrace* active_trace;
#endif
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
    // Execution paused at an instruction boundary and can be resumed. Why is
    // recorded in vm->vm_cause, never in this value: a watchdog, a stop, the
    // memory ceiling, and an unservable preempt callback are one state with
    // four causes. Deliberately NOT a runtime error -- no diagnostic is pushed
    // and no script-visible handler runs, so a sandboxed script cannot observe,
    // intercept, or loop inside its own termination.
    INTERPRET_SUSPENDED
} InterpretResult;

static inline Chunk* currentChunk(VM* vm) {
    return vm->chunk;
}

void initVM(VM* vm);
void freeVM(VM* vm);
void runtimeError(VM* vm, const char* format, ...);

void updateStackReferences(VM* vm, Value* old_stack, Value* new_stack);
void closeUpvalues(VM* vm, Value* last);
// Grow the value stack so `needed_top` slots are addressable. Exposed so
// native modules that push their own call frames can use it.
bool growStackForCall(VM* vm, int needed_top, Value** old_stack_out);
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