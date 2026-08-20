#pragma once

#include "../vm.h"
#include "../value.h"
#include "zym/zym.h"

// ---- Entry table (host-only) ----------------------------------------------
// Registration returns an opaque id (0 on failure). `callback` may be
// NULL_VAL, which means "abort execution on expiry" -- the watchdog shape,
// with nothing for script to intercept. Script has no preemption surface:
// every entry is host-owned and unmaskable.
int  preemptCount(const VM* vm);
int  preemptIds(const VM* vm, uint32_t* out, int max);

uint32_t preemptRegister(VM* vm, int slice, Value callback, uint8_t flags);
bool     preemptUnregister(VM* vm, uint32_t id);
bool     preemptSetSlice(VM* vm, uint32_t id, int slice);
int      preemptEntryRemaining(VM* vm, uint32_t id);
bool     preemptTrigger(VM* vm, uint32_t id);

// Recompute the countdown from the table. Call after any mutation.
void     preemptArm(VM* vm);

// ---- Unmaskable hard stop ------------------------------------------------
// Safe to call from an ISR, a signal handler, or another thread.
void     preemptRequestStop(VM* vm);
void     preemptClearStop(VM* vm);
bool     preemptStopRequested(const VM* vm);
