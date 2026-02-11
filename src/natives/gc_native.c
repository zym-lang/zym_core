#include <stdio.h>
#include <stdlib.h>
#include "gc_native.h"
#include "../memory.h"
#include "../gc.h"
#include "../zym.h"
#include "../table.h"
#include "../object.h"

// =============================================================================
// GC CONTROL NATIVE
// =============================================================================
// Provides script-level control over the garbage collector for performance
// tuning and memory management in performance-critical applications.
//
// Features:
// - Pause/resume garbage collection
// - Force garbage collection cycles
// - Query GC state (paused, bytes tracked, threshold)
// - Configure GC threshold
// =============================================================================

// GC context data (empty, but required for native closure pattern)
typedef struct {
    int dummy;  // Placeholder - we don't actually need state
} GCData;

// =============================================================================
// GC CLEANUP
// =============================================================================

void gc_cleanup(ZymVM* vm, void* ptr) {
    (void)vm;  // Unused
    GCData* gc = (GCData*)ptr;
    free(gc);
}

// =============================================================================
// GC CONTROL METHODS
// =============================================================================

// Pause garbage collection
ZymValue gc_pause(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);  // Verify context is valid
    vm->gc_enabled = false;
    return context;
}

// Resume garbage collection
ZymValue gc_resume(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);  // Verify context is valid
    vm->gc_enabled = true;
    return context;
}

// Check if GC is currently paused
ZymValue gc_isPaused(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);  // Verify context is valid
    return zym_newBool(!vm->gc_enabled);
}

// Force a garbage collection cycle
ZymValue gc_cycle(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);  // Verify context is valid

    // Temporarily enable GC if it's paused (to allow forced collection)
    bool was_enabled = vm->gc_enabled;
    vm->gc_enabled = true;

    // Run garbage collection
    collectGarbage(vm);

    // Restore previous state
    vm->gc_enabled = was_enabled;

    return context;
}

// Get current bytes allocated
ZymValue gc_getBytesTracked(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);  // Verify context is valid
    return zym_newNumber((double)vm->bytes_allocated);
}

// Get current GC threshold
ZymValue gc_getBytesThreshold(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);  // Verify context is valid
    return zym_newNumber((double)vm->next_gc);
}

// Set GC threshold
ZymValue gc_setBytesThreshold(ZymVM* vm, ZymValue context, ZymValue thresholdVal) {
    (void)zym_getNativeData(context);  // Verify context is valid

    if (!zym_isNumber(thresholdVal)) {
        zym_runtimeError(vm, "setBytesThreshold() requires a number argument");
        return ZYM_ERROR;
    }

    double threshold = zym_asNumber(thresholdVal);
    if (threshold < 0) {
        zym_runtimeError(vm, "GC threshold must be non-negative");
        return ZYM_ERROR;
    }

    // Set the threshold (minimum 1024 bytes to prevent thrashing)
    size_t new_threshold = (size_t)threshold;
    if (new_threshold < 1024) {
        new_threshold = 1024;
    }

    vm->next_gc = new_threshold;
    return context;
}

// =============================================================================
// GC FACTORY
// =============================================================================

ZymValue nativeGC_create(ZymVM* vm) {
    // Allocate GC context (minimal, just a placeholder)
    GCData* gc = calloc(1, sizeof(GCData));
    if (!gc) {
        zym_runtimeError(vm, "Out of memory");
        return ZYM_ERROR;
    }

    // Create context with finalizer
    ZymValue context = zym_createNativeContext(vm, gc, gc_cleanup);
    zym_pushRoot(vm, context);

    // Create method closures
    #define CREATE_METHOD_0(name, func) \
        ZymValue name = zym_createNativeClosure(vm, #func "()", func, context); \
        zym_pushRoot(vm, name);

    #define CREATE_METHOD_1(name, func) \
        ZymValue name = zym_createNativeClosure(vm, #func "(arg)", func, context); \
        zym_pushRoot(vm, name);

    CREATE_METHOD_0(pause, gc_pause);
    CREATE_METHOD_0(resume, gc_resume);
    CREATE_METHOD_0(isPaused, gc_isPaused);
    CREATE_METHOD_0(cycle, gc_cycle);
    CREATE_METHOD_0(getBytesTracked, gc_getBytesTracked);
    CREATE_METHOD_0(getBytesThreshold, gc_getBytesThreshold);
    CREATE_METHOD_1(setBytesThreshold, gc_setBytesThreshold);

    #undef CREATE_METHOD_0
    #undef CREATE_METHOD_1

    // Create GC object
    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    // Add methods
    zym_mapSet(vm, obj, "pause", pause);
    zym_mapSet(vm, obj, "resume", resume);
    zym_mapSet(vm, obj, "isPaused", isPaused);
    zym_mapSet(vm, obj, "cycle", cycle);
    zym_mapSet(vm, obj, "getBytesTracked", getBytesTracked);
    zym_mapSet(vm, obj, "getBytesThreshold", getBytesThreshold);
    zym_mapSet(vm, obj, "setBytesThreshold", setBytesThreshold);

    // Pop all roots (context + 7 methods + obj = 9 total)
    for (int i = 0; i < 9; i++) {
        zym_popRoot(vm);
    }

    return obj;
}

// =============================================================================
// Module Registration (Singleton - loaded at VM startup)
// =============================================================================

void registerGCModule(VM* vm) {
    ZymValue gcModule = nativeGC_create(vm);
    zym_pushRoot(vm, gcModule);

    ObjString* name = copyString(vm, "GC", 2);
    pushTempRoot(vm, (Obj*)name);
    tableSet(vm, &vm->globals, name, gcModule);
    popTempRoot(vm);

    zym_popRoot(vm);
}
