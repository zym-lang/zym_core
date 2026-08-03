/*
 * compiler_trace.c — test-only compiler resolution-trace implementation.
 *
 * Compiled into zym_core only when `ZYM_ENABLE_BUILD_TESTING=ON`. All
 * symbols in this file live behind `#if ZYM_HAS_BUILD_TESTING`; shipping
 * builds link an empty TU.
 *
 * Storage model: one ZymResolutionTrace per VM, lifetime owned by the
 * caller via zym_compilerTraceBegin/End/Free. The VM only holds a borrowed
 * pointer (`vm->active_trace`) that is set by Begin and cleared by End.
 * The compiler records through that pointer; if it is NULL (the shipping
 * case or no trace is active), recording is a no-op.
 */
#include "zym/config.h"

#if ZYM_HAS_BUILD_TESTING

#include "zym/compiler_trace.h"

#include <stdlib.h>
#include <string.h>

#include "./vm.h"
#include "./memory.h"
#include "./token.h"

struct ZymResolutionTrace {
    ZymResolutionEntry* entries;
    int                 count;
    int                 capacity;
};

static ZymResolutionTrace* trace_new(VM* vm) {
    ZymResolutionTrace* t = (ZymResolutionTrace*)reallocate(
        vm, NULL, 0, sizeof(ZymResolutionTrace));
    if (!t) return NULL;
    t->entries  = NULL;
    t->count    = 0;
    t->capacity = 0;
    return t;
}

static void trace_free_internal(VM* vm, ZymResolutionTrace* t) {
    if (!t) return;
    if (t->entries) {
        reallocate(vm, t->entries,
                   sizeof(ZymResolutionEntry) * (size_t)t->capacity, 0);
    }
    reallocate(vm, t, sizeof(ZymResolutionTrace), 0);
}

int zym_compilerTraceBegin(ZymVM* vm) {
    if (!vm) return 1;
    if (vm->active_trace != NULL) {
        /* Already recording; refuse to stomp on an in-flight trace. */
        return 2;
    }
    ZymResolutionTrace* t = trace_new(vm);
    if (!t) return 3;
    vm->active_trace = t;
    return 0;
}

ZymResolutionTrace* zym_compilerTraceEnd(ZymVM* vm) {
    if (!vm) return NULL;
    ZymResolutionTrace* t = vm->active_trace;
    vm->active_trace = NULL;
    return t;
}

void zym_compilerTraceFree(ZymVM* vm, ZymResolutionTrace* trace) {
    if (!vm) return;
    trace_free_internal(vm, trace);
}

int zym_compilerTraceCount(const ZymResolutionTrace* trace) {
    if (!trace) return 0;
    return trace->count;
}

int zym_compilerTraceAt(const ZymResolutionTrace* trace,
                        int                         index,
                        ZymResolutionEntry*         out) {
    if (!trace || !out) return -1;
    if (index < 0 || index >= trace->count) return -1;
    *out = trace->entries[index];
    return 0;
}

/* -------------------------------------------------------------------------
 * Internal: recording helper called by compiler.c through the
 * compiler_trace_record() wrapper. Not part of the public test header; must
 * remain a plain-C symbol for compiler.c to forward-declare.
 * ------------------------------------------------------------------------- */
void zym_compilerTrace_record(VM* vm,
                              ZymFileId fileId,
                              int byteOffset,
                              int length,
                              ZymResolutionKind kind,
                              int slotOrIndex,
                              int isLocalUpvalue) {
    if (!vm) return;
    ZymResolutionTrace* t = vm->active_trace;
    if (!t) return;
    /* Synthetic tokens (mangled names built at compile time) have no byte
     * offset; reject them so the parity test keys cleanly on real source. */
    if (byteOffset < 0 || length <= 0) return;
    if (t->count >= t->capacity) {
        int new_cap = t->capacity < 8 ? 8 : t->capacity * 2;
        ZymResolutionEntry* new_entries = (ZymResolutionEntry*)reallocate(
            vm, t->entries,
            sizeof(ZymResolutionEntry) * (size_t)t->capacity,
            sizeof(ZymResolutionEntry) * (size_t)new_cap);
        if (!new_entries) return;
        t->entries  = new_entries;
        t->capacity = new_cap;
    }
    ZymResolutionEntry* e = &t->entries[t->count++];
    e->fileId         = fileId;
    e->byteOffset     = byteOffset;
    e->length         = length;
    e->kind           = kind;
    e->slotOrIndex    = slotOrIndex;
    e->isLocalUpvalue = isLocalUpvalue;
}

#else  /* !ZYM_HAS_BUILD_TESTING */

/* Shipping build: emit a unique external symbol so the linker doesn't warn
 * about an empty translation unit on strict toolchains. */
extern const int zym_compiler_trace_disabled_marker;
const int zym_compiler_trace_disabled_marker = 0;

#endif /* ZYM_HAS_BUILD_TESTING */
