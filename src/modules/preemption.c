#include <stdio.h>
#include <stdlib.h>

#include "preemption.h"
#include "../zym.h"
#include "../gc.h"
#include "../object.h"
#include "../table.h"
#include "../memory.h"

// ============================================================================
// Preemption Control Functions
// ============================================================================

void preemptionEnable(VM* vm) {
    vm->preemption_enabled = true;
}

void preemptionDisable(VM* vm) {
    vm->preemption_enabled = false;
}

void preemptionPushDisable(VM* vm) {
    vm->preemption_disable_depth++;
}

void preemptionPopDisable(VM* vm) {
    if (vm->preemption_disable_depth > 0) {
        vm->preemption_disable_depth--;
    }
}

bool preemptionIsEnabled(VM* vm) {
    return vm->preemption_enabled && vm->preemption_disable_depth == 0;
}

void preemptionSetTimeslice(VM* vm, int instructions) {
    if (instructions < 1) {
        instructions = 1;
    }
    vm->default_timeslice = instructions;
}

int preemptionGetTimeslice(VM* vm) {
    return vm->default_timeslice;
}

void preemptionRequest(VM* vm) {
    vm->preempt_requested = true;
}

void preemptionReset(VM* vm) {
    vm->yield_budget = vm->default_timeslice;
    vm->preempt_requested = false;
}

int preemptionRemaining(VM* vm) {
    return vm->yield_budget;
}

// ============================================================================
// PREEMPT MODULE - NATIVE CLOSURE IMPLEMENTATION
// ============================================================================

typedef struct {
    int dummy;
} PreemptData;

static void preempt_cleanup(ZymVM* vm, void* ptr) {
    PreemptData* data = (PreemptData*)ptr;
    const ZymAllocator* alloc = zym_getAllocator(vm);
    ZYM_FREE((ZymAllocator*)alloc, data, sizeof(PreemptData));
}

static ZymValue preempt_enable(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    preemptionEnable(vm);
    return zym_newNull();
}

static ZymValue preempt_disable(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    preemptionDisable(vm);
    return zym_newNull();
}

static ZymValue preempt_pushDisable(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    preemptionPushDisable(vm);
    return zym_newNull();
}

static ZymValue preempt_popDisable(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    preemptionPopDisable(vm);
    return zym_newNull();
}

static ZymValue preempt_getDisableDepth(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    return zym_newNumber((double)vm->preemption_disable_depth);
}

static ZymValue preempt_setCallback(ZymVM* vm, ZymValue context, ZymValue callback) {
    (void)zym_getNativeData(context);
    zym_setPreemptCallback(vm, callback);
    return zym_newNull();
}

static ZymValue preempt_withDisabled(ZymVM* vm, ZymValue context, ZymValue fn) {
    (void)zym_getNativeData(context);
    if (!zym_isClosure(fn)) {
        zym_runtimeError(vm, "Preempt.withDisabled: argument must be a function.");
        return ZYM_ERROR;
    }

    ObjClosure* closure = AS_CLOSURE(fn);
    ObjFunction* function = closure->function;

    if (function->arity != 0) {
        zym_runtimeError(vm, "Preempt.withDisabled: function must take 0 arguments, got %d.", function->arity);
        return ZYM_ERROR;
    }

    int callee_slot = -1;
    if (vm->chunk != NULL && vm->ip > vm->chunk->code) {
        uint32_t prev_instr = *(vm->ip - 1);
        int opcode = prev_instr & 0xFF;

        if (opcode == CALL || opcode == CALL_SELF || opcode == TAIL_CALL ||
            opcode == TAIL_CALL_SELF) {
            int result_reg = (prev_instr >> 8) & 0xFF;
            int frame_base = (vm->frame_count > 0) ? vm->frames[vm->frame_count - 1].stack_base : 0;
            callee_slot = frame_base + result_reg;
        }
    }

    if (callee_slot < 0) {
        zym_runtimeError(vm, "Preempt.withDisabled: could not determine call context.");
        return ZYM_ERROR;
    }

    if (vm->frame_count >= FRAMES_MAX) {
        zym_runtimeError(vm, "Preempt.withDisabled: stack overflow (max call depth reached).");
        return ZYM_ERROR;
    }

    int needed_top = callee_slot + function->max_regs;
    if (needed_top > STACK_MAX) {
        zym_runtimeError(vm, "Preempt.withDisabled: stack overflow.");
        return ZYM_ERROR;
    }
    
    if (needed_top > vm->stack_capacity) {
        int new_capacity = vm->stack_capacity;
        while (new_capacity < needed_top) {
            new_capacity *= 2;
            if (new_capacity > STACK_MAX) {
                new_capacity = STACK_MAX;
                break;
            }
        }
        Value* old_stack = vm->stack;
        vm->stack = GROW_ARRAY(vm, Value, vm->stack, vm->stack_capacity, new_capacity);
        vm->stack_capacity = new_capacity;
        if (old_stack != vm->stack) {
            updateStackReferences(vm, old_stack, vm->stack);
        }
    }

    vm->stack[callee_slot] = fn;

    CallFrame* frame = &vm->frames[vm->frame_count++];
    frame->closure = closure;
    frame->ip = vm->ip;
    frame->stack_base = callee_slot;
    frame->caller_chunk = vm->chunk;
    frame->flags = FRAME_FLAG_DISABLE_PREEMPT;

    vm->chunk = function->chunk;
    vm->ip = function->chunk->code;

    if (needed_top > vm->stack_top) {
        vm->stack_top = needed_top;
    }

    vm->preemption_disable_depth++;

    return ZYM_CONTROL_TRANSFER;
}

static ZymValue preempt_isEnabled(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    return zym_newBool(preemptionIsEnabled(vm));
}

static ZymValue preempt_setTimeslice(ZymVM* vm, ZymValue context, ZymValue instructions) {
    (void)zym_getNativeData(context);

    if (!zym_isNumber(instructions)) {
        zym_runtimeError(vm, "Preempt.setTimeslice: argument must be a number.");
        return ZYM_ERROR;
    }

    int value = (int)zym_asNumber(instructions);
    preemptionSetTimeslice(vm, value);
    return zym_newNull();
}

static ZymValue preempt_getTimeslice(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    return zym_newNumber((double)preemptionGetTimeslice(vm));
}

static ZymValue preempt_request(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    preemptionRequest(vm);
    return zym_newNull();
}

static ZymValue preempt_reset(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    preemptionReset(vm);
    return zym_newNull();
}

static ZymValue preempt_remaining(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    return zym_newNumber((double)preemptionRemaining(vm));
}

static ZymValue preempt_yield(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    vm->preempt_requested = true;
    vm->yield_budget = 0;
    return zym_newNull();
}

// ============================================================================
// Module Factory
// ============================================================================

ZymValue nativePreempt_create(ZymVM* vm) {
    const ZymAllocator* alloc = zym_getAllocator(vm);
    PreemptData* data = ZYM_CALLOC((ZymAllocator*)alloc, 1, sizeof(PreemptData));
    if (!data) {
        zym_runtimeError(vm, "Out of memory");
        return ZYM_ERROR;
    }

    ZymValue context = zym_createNativeContext(vm, data, preempt_cleanup);
    zym_pushRoot(vm, context);

    ZymValue enable = zym_createNativeClosure(vm, "enable()", (void*)preempt_enable, context);
    zym_pushRoot(vm, enable);

    ZymValue disable = zym_createNativeClosure(vm, "disable()", (void*)preempt_disable, context);
    zym_pushRoot(vm, disable);

    ZymValue pushDisable = zym_createNativeClosure(vm, "pushDisable()", (void*)preempt_pushDisable, context);
    zym_pushRoot(vm, pushDisable);

    ZymValue popDisable = zym_createNativeClosure(vm, "popDisable()", (void*)preempt_popDisable, context);
    zym_pushRoot(vm, popDisable);

    ZymValue getDisableDepth = zym_createNativeClosure(vm, "getDisableDepth()", (void*)preempt_getDisableDepth, context);
    zym_pushRoot(vm, getDisableDepth);

    ZymValue setCallback = zym_createNativeClosure(vm, "setCallback(fn)", (void*)preempt_setCallback, context);
    zym_pushRoot(vm, setCallback);

    ZymValue withDisabled = zym_createNativeClosure(vm, "withDisabled(fn)", (void*)preempt_withDisabled, context);
    zym_pushRoot(vm, withDisabled);

    ZymValue isEnabled = zym_createNativeClosure(vm, "isEnabled()", (void*)preempt_isEnabled, context);
    zym_pushRoot(vm, isEnabled);

    ZymValue setTimeslice = zym_createNativeClosure(vm, "setTimeslice(n)", (void*)preempt_setTimeslice, context);
    zym_pushRoot(vm, setTimeslice);

    ZymValue getTimeslice = zym_createNativeClosure(vm, "getTimeslice()", (void*)preempt_getTimeslice, context);
    zym_pushRoot(vm, getTimeslice);

    ZymValue request = zym_createNativeClosure(vm, "request()", (void*)preempt_request, context);
    zym_pushRoot(vm, request);

    ZymValue reset = zym_createNativeClosure(vm, "reset()", (void*)preempt_reset, context);
    zym_pushRoot(vm, reset);

    ZymValue remaining = zym_createNativeClosure(vm, "remaining()", (void*)preempt_remaining, context);
    zym_pushRoot(vm, remaining);

    ZymValue yield_closure = zym_createNativeClosure(vm, "yield()", (void*)preempt_yield, context);
    zym_pushRoot(vm, yield_closure);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "enable", enable);
    zym_mapSet(vm, obj, "disable", disable);
    zym_mapSet(vm, obj, "pushDisable", pushDisable);
    zym_mapSet(vm, obj, "popDisable", popDisable);
    zym_mapSet(vm, obj, "getDisableDepth", getDisableDepth);
    zym_mapSet(vm, obj, "setCallback", setCallback);
    zym_mapSet(vm, obj, "withDisabled", withDisabled);
    zym_mapSet(vm, obj, "isEnabled", isEnabled);
    zym_mapSet(vm, obj, "setTimeslice", setTimeslice);
    zym_mapSet(vm, obj, "getTimeslice", getTimeslice);
    zym_mapSet(vm, obj, "request", request);
    zym_mapSet(vm, obj, "reset", reset);
    zym_mapSet(vm, obj, "remaining", remaining);
    zym_mapSet(vm, obj, "yield", yield_closure);

    for (int i = 0; i < 16; i++) {
        zym_popRoot(vm);
    }

    return obj;
}

// ============================================================================
// Module Registration
// ============================================================================

void registerPreemptionModule(VM* vm) {
    ZymValue preemptModule = nativePreempt_create(vm);
    zym_pushRoot(vm, preemptModule);

    ObjString* name = copyString(vm, "Preempt", 7);
    pushTempRoot(vm, (Obj*)name);
    tableSet(vm, &vm->globals, name, preemptModule);
    popTempRoot(vm);

    zym_popRoot(vm);
}
