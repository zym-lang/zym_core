#include <stdio.h>

#include "fiber.h"
#include "zym/zym.h"
#include "../object.h"
#include "../gc.h"
#include "../table.h"
#include "../memory.h"

// ============================================================================
// FIBER MODULE -- script surface over the vm.c switch machinery
// ============================================================================
//
// Thin by design: every native validates its arguments and hands off to
// fiberResume / fiberYield, then maps the FiberOpResult onto the native
// return protocol (ZYM_ERROR / ZYM_FIBER_SWITCH sentinel / direct value).
// Names are PROVISIONAL pending the design doc's decision-10 stamp; they
// are registration strings, renaming is free.

typedef struct { int dummy; } FiberModuleData;

static void fiber_cleanup(ZymVM* vm, void* ptr) {
    const ZymAllocator* alloc = zym_getAllocator(vm);
    ZYM_FREE((ZymAllocator*)alloc, (FiberModuleData*)ptr, sizeof(FiberModuleData));
}

static ZymValue fiber_new(ZymVM* vm, ZymValue ctx, ZymValue fnv) {
    (void)zym_getNativeData(ctx);
    if (!IS_CLOSURE(fnv)) {
        zym_runtimeError(vm, "Fiber.new(fn): fn must be a function.");
        return ZYM_ERROR;
    }
    ObjClosure* closure = AS_CLOSURE(fnv);
    if (closure->function->is_variadic) {
        zym_runtimeError(vm, "Fiber.new(fn): fn must not be variadic.");
        return ZYM_ERROR;
    }
    if (closure->function->arity > 1) {
        zym_runtimeError(vm, "Fiber.new(fn): fn takes at most 1 argument (the first resume value), got %d.",
                         closure->function->arity);
        return ZYM_ERROR;
    }
    ObjFiber* fiber = newFiber(vm, closure);
    return OBJ_VAL(fiber);
}

static ZymValue fiber_do_call(ZymVM* vm, ZymValue fv, ZymValue value,
                              bool has_value, bool as_try) {
    if (!IS_FIBER(fv)) {
        zym_runtimeError(vm, "Fiber.call: first argument must be a fiber.");
        return ZYM_ERROR;
    }
    Value direct = NULL_VAL;
    switch (fiberResume(vm, AS_FIBER(fv), value, has_value, as_try, &direct)) {
        case FIBER_OP_ERROR:    return ZYM_ERROR;
        case FIBER_OP_SWITCHED: return ZYM_FIBER_SWITCH;
        case FIBER_OP_DIRECT:   return direct;
    }
    return NULL_VAL; // unreachable
}

static ZymValue fiber_call_1(ZymVM* vm, ZymValue ctx, ZymValue fv) {
    (void)zym_getNativeData(ctx);
    return fiber_do_call(vm, fv, NULL_VAL, false, false);
}

static ZymValue fiber_call_2(ZymVM* vm, ZymValue ctx, ZymValue fv, ZymValue value) {
    (void)zym_getNativeData(ctx);
    return fiber_do_call(vm, fv, value, true, false);
}

// try: identical to call, but marks the resume edge catchable -- an error
// in the fiber (or anything it plainly resumes) kills the chain and lands
// HERE: try returns null and Fiber.error(f) holds the error. On success,
// try returns the yielded/returned value exactly like call; check
// Fiber.error(f) to distinguish. (Deviation from Wren, where try returns
// the error itself: a single-return language cannot disambiguate an error
// string from a legitimate string result.)
static ZymValue fiber_try_1(ZymVM* vm, ZymValue ctx, ZymValue fv) {
    (void)zym_getNativeData(ctx);
    return fiber_do_call(vm, fv, NULL_VAL, false, true);
}

static ZymValue fiber_try_2(ZymVM* vm, ZymValue ctx, ZymValue fv, ZymValue value) {
    (void)zym_getNativeData(ctx);
    return fiber_do_call(vm, fv, value, true, true);
}

static ZymValue fiber_error(ZymVM* vm, ZymValue ctx, ZymValue fv) {
    (void)zym_getNativeData(ctx);
    if (!IS_FIBER(fv)) {
        zym_runtimeError(vm, "Fiber.error: argument must be a fiber.");
        return ZYM_ERROR;
    }
    return AS_FIBER(fv)->error;
}

static ZymValue fiber_yield_0(ZymVM* vm, ZymValue ctx) {
    (void)zym_getNativeData(ctx);
    switch (fiberYield(vm, NULL_VAL)) {
        case FIBER_OP_ERROR:    return ZYM_ERROR;
        case FIBER_OP_SWITCHED: return ZYM_FIBER_SWITCH;
        default:                return NULL_VAL; // unreachable
    }
}

static ZymValue fiber_yield_1(ZymVM* vm, ZymValue ctx, ZymValue value) {
    (void)zym_getNativeData(ctx);
    switch (fiberYield(vm, value)) {
        case FIBER_OP_ERROR:    return ZYM_ERROR;
        case FIBER_OP_SWITCHED: return ZYM_FIBER_SWITCH;
        default:                return NULL_VAL; // unreachable
    }
}

static ZymValue fiber_status(ZymVM* vm, ZymValue ctx, ZymValue fv) {
    (void)zym_getNativeData(ctx);
    if (!IS_FIBER(fv)) {
        zym_runtimeError(vm, "Fiber.status: argument must be a fiber.");
        return ZYM_ERROR;
    }
    switch (AS_FIBER(fv)->status) {
        case FIBER_NEW:       return zym_newString(vm, "new");
        case FIBER_RUNNING:   return zym_newString(vm, "running");
        case FIBER_SUSPENDED: return zym_newString(vm, "suspended");
        case FIBER_PREEMPTED: return zym_newString(vm, "preempted");
        case FIBER_DEAD:      return zym_newString(vm, "dead");
    }
    return zym_newString(vm, "unknown"); // unreachable
}

static ZymValue fiber_isDone(ZymVM* vm, ZymValue ctx, ZymValue fv) {
    (void)zym_getNativeData(ctx);
    if (!IS_FIBER(fv)) {
        zym_runtimeError(vm, "Fiber.isDone: argument must be a fiber.");
        return ZYM_ERROR;
    }
    return zym_newBool(AS_FIBER(fv)->status == FIBER_DEAD);
}

static ZymValue fiber_current(ZymVM* vm, ZymValue ctx) {
    (void)zym_getNativeData(ctx);
    return vm->current_fiber != NULL ? OBJ_VAL(vm->current_fiber) : zym_newNull();
}

// ============================================================================
// Module Factory + Registration
// ============================================================================

ZymValue nativeFiber_create(ZymVM* vm) {
    const ZymAllocator* alloc = zym_getAllocator(vm);
    FiberModuleData* data = ZYM_CALLOC((ZymAllocator*)alloc, 1, sizeof(FiberModuleData));
    if (!data) {
        zym_runtimeError(vm, "Out of memory");
        return ZYM_ERROR;
    }

    ZymValue context = zym_createNativeContext(vm, data, fiber_cleanup);
    zym_pushRoot(vm, context);

#define MK(sig, fn) zym_createNativeClosure(vm, sig, (void*)fn, context)
    ZymValue mNew     = MK("new(fn)", fiber_new);              zym_pushRoot(vm, mNew);

    // call and yield dispatch on arity: with and without a value.
    ZymValue call1    = MK("call(f)", fiber_call_1);           zym_pushRoot(vm, call1);
    ZymValue call2    = MK("call(f, v)", fiber_call_2);        zym_pushRoot(vm, call2);
    ZymValue mCall    = zym_createDispatcher(vm);              zym_pushRoot(vm, mCall);
    zym_addOverload(vm, mCall, call1);
    zym_addOverload(vm, mCall, call2);

    ZymValue yield0   = MK("yield()", fiber_yield_0);          zym_pushRoot(vm, yield0);
    ZymValue yield1   = MK("yield(v)", fiber_yield_1);         zym_pushRoot(vm, yield1);
    ZymValue mYield   = zym_createDispatcher(vm);              zym_pushRoot(vm, mYield);
    zym_addOverload(vm, mYield, yield0);
    zym_addOverload(vm, mYield, yield1);

    ZymValue try1     = MK("try(f)", fiber_try_1);             zym_pushRoot(vm, try1);
    ZymValue try2     = MK("try(f, v)", fiber_try_2);          zym_pushRoot(vm, try2);
    ZymValue mTry     = zym_createDispatcher(vm);              zym_pushRoot(vm, mTry);
    zym_addOverload(vm, mTry, try1);
    zym_addOverload(vm, mTry, try2);

    ZymValue mError   = MK("error(f)", fiber_error);           zym_pushRoot(vm, mError);
    ZymValue mStatus  = MK("status(f)", fiber_status);         zym_pushRoot(vm, mStatus);
    ZymValue mIsDone  = MK("isDone(f)", fiber_isDone);         zym_pushRoot(vm, mIsDone);
    ZymValue mCurrent = MK("current()", fiber_current);        zym_pushRoot(vm, mCurrent);
#undef MK

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "new",     mNew);
    zym_mapSet(vm, obj, "call",    mCall);
    zym_mapSet(vm, obj, "yield",   mYield);
    zym_mapSet(vm, obj, "try",     mTry);
    zym_mapSet(vm, obj, "error",   mError);
    zym_mapSet(vm, obj, "status",  mStatus);
    zym_mapSet(vm, obj, "isDone",  mIsDone);
    zym_mapSet(vm, obj, "current", mCurrent);

    for (int i = 0; i < 15; i++) zym_popRoot(vm);   // context + 13 values + obj
    return obj;
}

void registerFiberModule(VM* vm) {
    ZymValue fiberModule = nativeFiber_create(vm);
    zym_pushRoot(vm, fiberModule);

    ObjString* name = copyString(vm, "Fiber", 5);
    pushTempRoot(vm, (Obj*)name);
    tableSet(vm, &vm->globals, name, fiberModule);
    popTempRoot(vm);

    zym_popRoot(vm);
}
