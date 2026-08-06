#pragma once

#include "../vm.h"
#include "../value.h"
#include "zym/zym.h"

// ---- Entry table ---------------------------------------------------------
// Registration returns an opaque id (0 on failure). `callback` may be
// NULL_VAL, which means "abort execution on expiry" -- the watchdog shape,
// with nothing for script to intercept.
int  preemptCount(const VM* vm, bool script_owned_only);
int  preemptScriptCapacity(const VM* vm);
int  preemptScriptAvailable(const VM* vm);
int  preemptIds(const VM* vm, uint32_t* out, int max, bool script_owned_only);

uint32_t preemptRegister(VM* vm, int slice, Value callback,
                         uint8_t flags, bool owner_script);
bool     preemptUnregister(VM* vm, uint32_t id, bool owner_script);
bool     preemptSetSlice(VM* vm, uint32_t id, int slice, bool owner_script);
int      preemptEntryRemaining(VM* vm, uint32_t id, bool owner_script);
bool     preemptTrigger(VM* vm, uint32_t id, bool owner_script);

// Recompute the countdown from the table. Call after any mutation.
void     preemptArm(VM* vm);

// Script critical sections. Masks script-owned MASKABLE entries only; a
// non-maskable host entry fires straight through.
void     preemptShieldPush(VM* vm);
void     preemptShieldPop(VM* vm);

// ---- Unmaskable hard stop ------------------------------------------------
// Safe to call from an ISR, a signal handler, or another thread.
void     preemptRequestStop(VM* vm);
void     preemptClearStop(VM* vm);
bool     preemptStopRequested(const VM* vm);

// ---- Legacy single-target shims (kept so existing callers build) ---------
void preemptionSetTimeslice(VM* vm, int instructions);
int preemptionGetTimeslice(VM* vm);
void preemptionRequest(VM* vm);
void preemptionReset(VM* vm);
int preemptionRemaining(VM* vm);

ZymValue nativePreempt_create(ZymVM* vm);
void registerPreemptionModule(VM* vm);
