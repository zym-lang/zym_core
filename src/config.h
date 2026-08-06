#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct CompilerConfig {
    bool include_line_info;
    // Symbol stripping: globals DEFINED by the compilation unit are
    // renamed to compact symbols ("s0", "s1", ... in definition order)
    // in the emitted bytecode. Names listed in keep_names, names that
    // collide with a registered native, and everything data-bearing
    // (map keys, struct fields, enum variants, string literals) are
    // left untouched. keep_names entries are base names (no @arity).
    bool strip_symbols;
    const char* const* keep_names;
    int keep_name_count;
} CompilerConfig;

typedef CompilerConfig ZymCompilerConfig;

typedef enum {
    ZYM_STATUS_OK,
    ZYM_STATUS_COMPILE_ERROR,
    ZYM_STATUS_RUNTIME_ERROR,
    ZYM_STATUS_YIELD,
    // Host-initiated stop. Appended so existing numeric values are stable.
    ZYM_STATUS_ABORTED
} ZymStatus;

// -----------------------------------------------------------------------------
// VM STATE AND CAUSE
// -----------------------------------------------------------------------------
// A ZymStatus describes what one call returned. These describe what the VM
// *is*, and why, and can be read at any time -- including from inside a native
// while the VM is running, or from another context.
//
// The two answer different questions and are deliberately separate axes: state
// says whether execution can continue, cause says what put it there. New
// reasons to stop are added to the cause enum; the state enum should not need
// to grow again.

typedef enum {
    ZYM_STATE_IDLE,       // never ran, or the last run finished
    ZYM_STATE_RUNNING,    // inside dispatch (observable from natives / other threads)
    ZYM_STATE_SUSPENDED,  // stopped mid-execution, frames intact
    ZYM_STATE_FAILED,     // last run ended in an error
} ZymVmState;

typedef enum {
    ZYM_CAUSE_NONE,
    // Reserved. There is no script-visible yield today -- no opcode and no
    // native produces one -- so nothing sets this yet. It is here so that
    // adding a cooperative yield later is a new cause rather than a new state.
    ZYM_CAUSE_SCRIPT_YIELD,
    ZYM_CAUSE_PREEMPT,          // a preempt entry expired
    // An entry came due but its callback frame could not be pushed (call depth
    // or stack exhausted). Control returns to the host rather than silently
    // dropping the callback, so this is a preemption that could not be served.
    ZYM_CAUSE_PREEMPT_BLOCKED,
    ZYM_CAUSE_HOST_STOP,        // zym_requestStop
    ZYM_CAUSE_MEMORY_LIMIT,     // ceiling crossed
    ZYM_CAUSE_RUNTIME_ERROR,
    ZYM_CAUSE_COMPILE_ERROR,
} ZymVmCause;

typedef uint32_t ZymPreemptId;   // 0 is never a valid id

typedef struct {
    ZymVmState   state;
    ZymVmCause   cause;
    bool         resumable;     // can zym_resume succeed *right now*
    ZymPreemptId preempt_id;    // which entry, when cause is a preemption
    size_t       bytes_wanted;  // the allocation that crossed, when MEMORY_LIMIT
    size_t       memory_limit;
    size_t       memory_used;
} ZymVmInfo;