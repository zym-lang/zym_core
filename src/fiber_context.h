#pragma once

// The complete per-execution-context state of the VM. Everything in here
// tears if two fibers share it (see future/fiber_design.md, "Mechanics").
// One context is ACTIVE at a time and lives flattened in the VM's own
// fields; a parked context lives in one of these structs (inside an
// ObjFiber, or in the VM's root_parked slot for the root program).
//
// Ownership invariant: the heap arrays (stack, spill_stack, frames) are
// owned by exactly one place at a time -- the VM's fields while the
// context is active, this struct while it is parked. The install/save
// helpers in vm.c move ownership and NULL the vacated side, so the free
// paths (freeVM, freeObject) can free whatever is non-NULL without ever
// double-freeing.

#include <stdint.h>
#include "./value.h"

struct CallFrame;
struct Chunk;
struct ObjUpvalue;

typedef struct FiberContext {
    Value* stack;
    int stack_top;
    int stack_capacity;

    Value* spill_stack;
    int spill_top;
    int spill_capacity;

    struct CallFrame* frames;
    int frame_count;
    int frame_capacity;

    int cur_base;

    struct Chunk* chunk;
    uint32_t* ip;

    // Per-context: the address-sorted open-upvalue list compares raw
    // stack addresses, which is only meaningful within ONE stack.
    struct ObjUpvalue* open_upvalues;

    // Per-context: CALL_ARG_PREP staging cursor.
    int call_arg_top;
} FiberContext;
