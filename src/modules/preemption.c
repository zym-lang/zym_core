#include <stdio.h>
#include <stdlib.h>

#include "preemption.h"
#include "zym/zym.h"
#include "../gc.h"
#include "../object.h"
#include "../table.h"
#include "../memory.h"

// ============================================================================
// Preemption entry table — host-only (0.4.0)
// ============================================================================
//
// A single countdown (`vm->preempt_counter`) is armed to the nearest
// deadline in the table. The dispatch loop only decrements it. Everything
// below runs on the cold path, either when the countdown expires or when a
// caller mutates the table.
//
// Script has no surface here: every entry is host-owned, nothing can be
// masked from script, and there are no shields. The guard exists for the
// embedder to bound script, never for script to observe.

static PreemptEntry* find_entry(VM* vm, uint32_t id) {
    if (id == 0) return NULL;
    for (int i = 0; i < ZYM_PREEMPT_MAX_ENTRIES; i++) {
        PreemptEntry* e = &vm->preempt_table[i];
        if (e->slice != 0 && e->id == id) return e;
    }
    return NULL;
}

void preemptArm(VM* vm) {
    // A pending hard stop outranks every deadline. Without this, clearing the
    // last table entry re-arms the counter to INT32_MAX and the stop is never
    // observed again -- the VM would run free with a stop still set.
    if (vm->stop_requested || vm->oom_pending) {
        vm->preempt_counter = 0;
        vm->preempt_armed   = 0;
        return;
    }
    int32_t best = INT32_MAX;
    for (int i = 0; i < ZYM_PREEMPT_MAX_ENTRIES; i++) {
        PreemptEntry* e = &vm->preempt_table[i];
        // An entry whose callback is in flight is excluded from the countdown
        // (structural: prevents self-recursion).
        if (e->slice == 0 || e->in_flight) continue;
        if (e->remaining < best) best = e->remaining;
    }
    if (best < 1) best = 1;   // always make forward progress
    vm->preempt_counter = best;
    vm->preempt_armed   = best;
}

int preemptCount(const VM* vm) {
    int n = 0;
    for (int i = 0; i < ZYM_PREEMPT_MAX_ENTRIES; i++) {
        if (vm->preempt_table[i].slice != 0) n++;
    }
    return n;
}

int preemptIds(const VM* vm, uint32_t* out, int max) {
    int n = 0;
    for (int i = 0; i < ZYM_PREEMPT_MAX_ENTRIES; i++) {
        const PreemptEntry* e = &vm->preempt_table[i];
        if (e->slice == 0) continue;
        if (out != NULL && n < max) out[n] = e->id;
        n++;
    }
    return n;
}

uint32_t preemptRegister(VM* vm, int slice, Value callback, uint8_t flags) {
    if (slice < 1) slice = 1;

    // A callback the VM could never invoke must not be registrable.
    // pushPreemptFrame requires arity 0, so a callback taking arguments would
    // occupy a slot, fire on every slice, and never run -- silently. A NULL
    // callback is the watchdog shape and is deliberately allowed.
    if (IS_CLOSURE(callback) && AS_CLOSURE(callback)->function->arity != 0) {
        return 0;
    }

    for (int i = 0; i < ZYM_PREEMPT_MAX_ENTRIES; i++) {
        PreemptEntry* e = &vm->preempt_table[i];
        if (e->slice != 0) continue;
        uint32_t id = ++vm->preempt_next_id;
        if (id == 0) id = ++vm->preempt_next_id;   // never hand out 0
        e->slice        = slice;
        e->remaining    = slice;
        e->callback     = callback;
        e->id           = id;
        e->flags        = flags;
        e->in_flight    = false;
        vm->preempt_live_count++;
        preemptArm(vm);
        return id;
    }
    return 0;   // table full
}

bool preemptUnregister(VM* vm, uint32_t id) {
    PreemptEntry* e = find_entry(vm, id);
    if (!e) return false;
    e->slice = 0;
    e->callback = NULL_VAL;
    e->in_flight = false;
    vm->preempt_live_count--;
    preemptArm(vm);
    return true;
}

bool preemptSetSlice(VM* vm, uint32_t id, int slice) {
    PreemptEntry* e = find_entry(vm, id);
    if (!e) return false;
    if (slice < 1) slice = 1;
    // Restart the countdown: "fire `slice` instructions from now". The old
    // clamp could only lower `remaining`, which made it impossible to give
    // an exhausted entry more budget after an abort.
    e->slice = slice;
    e->remaining = slice;
    preemptArm(vm);
    return true;
}

int preemptEntryRemaining(VM* vm, uint32_t id) {
    PreemptEntry* e = find_entry(vm, id);
    if (!e) return -1;
    return e->remaining;
}

bool preemptTrigger(VM* vm, uint32_t id) {
    PreemptEntry* e = find_entry(vm, id);
    if (!e) return false;
    e->remaining = 0;
    vm->preempt_counter = 0;   // enter the cold handler on the next dispatch
    return true;
}

// ============================================================================
// Unmaskable hard stop
// ============================================================================

void preemptRequestStop(VM* vm) {
    vm->stop_requested = 1;
    vm->preempt_counter = 0;   // notice it on the very next dispatch
}

void preemptClearStop(VM* vm) {
    vm->stop_requested = 0;
    preemptArm(vm);
}

bool preemptStopRequested(const VM* vm) {
    return vm->stop_requested != 0;
}
