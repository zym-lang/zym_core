#include <stdio.h>
#include <stdlib.h>

#include "preemption.h"
#include "../zym.h"
#include "../gc.h"
#include "../object.h"
#include "../table.h"

// ============================================================================
// Preemption Control Functions
// ============================================================================

void preemptionEnable(VM* vm) {
    vm->preemption_enabled = true;
}

void preemptionDisable(VM* vm) {
    vm->preemption_enabled = false;
}

bool preemptionIsEnabled(VM* vm) {
    return vm->preemption_enabled;
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
    (void)vm;
    PreemptData* data = (PreemptData*)ptr;
    free(data);
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
    PreemptData* data = calloc(1, sizeof(PreemptData));
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
    zym_mapSet(vm, obj, "isEnabled", isEnabled);
    zym_mapSet(vm, obj, "setTimeslice", setTimeslice);
    zym_mapSet(vm, obj, "getTimeslice", getTimeslice);
    zym_mapSet(vm, obj, "request", request);
    zym_mapSet(vm, obj, "reset", reset);
    zym_mapSet(vm, obj, "remaining", remaining);
    zym_mapSet(vm, obj, "yield", yield_closure);

    for (int i = 0; i < 11; i++) {
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
