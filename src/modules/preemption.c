#include <stdio.h>
#include <stdlib.h>

#include "preemption.h"
#include "zym/zym.h"
#include "../gc.h"
#include "../object.h"
#include "../table.h"
#include "../memory.h"

// ============================================================================
// Preemption entry table
// ============================================================================
//
// A single countdown (`vm->preempt_counter`) is armed to the nearest
// deadline in the table. The dispatch loop only decrements it. Everything
// below runs on the cold path, either when the countdown expires or when a
// caller mutates the table.

static PreemptEntry* find_entry(VM* vm, uint32_t id) {
    if (id == 0) return NULL;
    for (int i = 0; i < ZYM_PREEMPT_MAX_ENTRIES; i++) {
        PreemptEntry* e = &vm->preempt_table[i];
        if (e->slice != 0 && e->id == id) return e;
    }
    return NULL;
}

// True when an entry is currently excluded from the countdown: either its
// own callback is running (structural, prevents self-recursion) or a script
// shield is masking script-owned maskable entries (advisory).
static bool entry_masked(const VM* vm, const PreemptEntry* e) {
    if (e->in_flight) return true;
    if ((e->flags & ZYM_PREEMPT_F_MASKABLE) && vm->preempt_shield_depth > 0) {
        return true;
    }
    return false;
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
        if (e->slice == 0 || entry_masked(vm, e)) continue;
        if (e->remaining < best) best = e->remaining;
    }
    if (best < 1) best = 1;   // always make forward progress
    vm->preempt_counter = best;
    vm->preempt_armed   = best;
}

int preemptCount(const VM* vm, bool script_owned_only) {
    int n = 0;
    for (int i = 0; i < ZYM_PREEMPT_MAX_ENTRIES; i++) {
        const PreemptEntry* e = &vm->preempt_table[i];
        if (e->slice == 0) continue;
        if (script_owned_only && !e->owner_script) continue;
        n++;
    }
    return n;
}

// How many entries script may hold in total. Fixed for the life of the run:
// the reserve is locked once the VM executes, so whatever script sees at the
// start is still bindable at the end.
int preemptScriptCapacity(const VM* vm) {
    int cap = ZYM_PREEMPT_MAX_ENTRIES - vm->host_preempt_reserve;
    return cap < 0 ? 0 : cap;
}

// Free slots script could actually take right now. Bounded by BOTH its own
// remaining budget and the real free slots -- the host may be using more than
// its reserve, and promising script room that does not exist would turn the
// next register into a surprise.
int preemptScriptAvailable(const VM* vm) {
    int budget = preemptScriptCapacity(vm) - preemptCount(vm, true);
    int free_slots = ZYM_PREEMPT_MAX_ENTRIES - preemptCount(vm, false);
    int n = budget < free_slots ? budget : free_slots;
    return n < 0 ? 0 : n;
}

int preemptIds(const VM* vm, uint32_t* out, int max, bool script_owned_only) {
    int n = 0;
    for (int i = 0; i < ZYM_PREEMPT_MAX_ENTRIES; i++) {
        const PreemptEntry* e = &vm->preempt_table[i];
        if (e->slice == 0) continue;
        if (script_owned_only && !e->owner_script) continue;
        if (out != NULL && n < max) out[n] = e->id;
        n++;
    }
    return n;
}

uint32_t preemptRegister(VM* vm, int slice, Value callback,
                         uint8_t flags, bool owner_script) {
    if (slice < 1) slice = 1;

    // A callback the VM could never invoke must not be registrable. pushPreemptFrame
    // requires arity 0, so a callback taking arguments would occupy a slot, fire on
    // every slice, and never run -- silently. Reject it here, where both the script
    // natives and zym_preemptRegister funnel through, so neither path can hand back
    // an id for an entry that cannot do its job. A NULL callback is the watchdog
    // shape and is deliberately allowed.
    if (IS_CLOSURE(callback) && AS_CLOSURE(callback)->function->arity != 0) {
        return 0;
    }

    // Script may not spend the host's reserve. The host itself is unrestricted:
    // the reserve is a floor for the host, a ceiling for script.
    if (owner_script && preemptCount(vm, true) >= preemptScriptCapacity(vm)) {
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
        e->owner_script = owner_script;
        e->in_flight    = false;
        vm->preempt_live_count++;
        preemptArm(vm);
        return id;
    }
    return 0;   // table full
}

// `owner_script` gates authority: script may only touch entries it owns, so
// it can never cancel or retune a host watchdog.
static bool may_touch(const PreemptEntry* e, bool owner_script) {
    return !owner_script || e->owner_script;
}

bool preemptUnregister(VM* vm, uint32_t id, bool owner_script) {
    PreemptEntry* e = find_entry(vm, id);
    if (!e || !may_touch(e, owner_script)) return false;
    e->slice = 0;
    e->callback = NULL_VAL;
    e->in_flight = false;
    vm->preempt_live_count--;
    preemptArm(vm);
    return true;
}

bool preemptSetSlice(VM* vm, uint32_t id, int slice, bool owner_script) {
    PreemptEntry* e = find_entry(vm, id);
    if (!e || !may_touch(e, owner_script)) return false;
    if (slice < 1) slice = 1;
    // Restart the countdown: "fire `slice` instructions from now". The old
    // clamp could only lower `remaining`, which made it impossible to give
    // an exhausted entry more budget after an abort.
    e->slice = slice;
    e->remaining = slice;
    preemptArm(vm);
    return true;
}

int preemptEntryRemaining(VM* vm, uint32_t id, bool owner_script) {
    PreemptEntry* e = find_entry(vm, id);
    // Ownership-gated like the mutators. Ids are handed out sequentially, so an
    // ungated read would let script probe 1, 2, 3... and map every host entry --
    // which would make gating Preempt.ids() pointless. A host that wants script
    // to see one of its deadlines can expose it through its own native.
    if (!e || !may_touch(e, owner_script)) return -1;
    return e->remaining;
}

bool preemptTrigger(VM* vm, uint32_t id, bool owner_script) {
    PreemptEntry* e = find_entry(vm, id);
    if (!e || !may_touch(e, owner_script)) return false;
    e->remaining = 0;
    vm->preempt_counter = 0;   // enter the cold handler on the next dispatch
    return true;
}

void preemptShieldPush(VM* vm) {
    vm->preempt_shield_depth++;
    preemptArm(vm);
}

void preemptShieldPop(VM* vm) {
    if (vm->preempt_shield_depth > 0) vm->preempt_shield_depth--;
    preemptArm(vm);
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

// ============================================================================
// Legacy single-target shims
// ============================================================================

void preemptionSetTimeslice(VM* vm, int instructions) {
    if (instructions < 1) instructions = 1;
    vm->default_timeslice = instructions;
}

int preemptionGetTimeslice(VM* vm) {
    return vm->default_timeslice;
}

void preemptionRequest(VM* vm) {
    vm->preempt_counter = 0;
}

void preemptionReset(VM* vm) {
    preemptArm(vm);
}

int preemptionRemaining(VM* vm) {
    return vm->preempt_counter;
}

// ============================================================================
// PREEMPT MODULE -- script-facing surface
// ============================================================================
//
// Script may only create, retune, and cancel entries it owns, and every
// entry it creates is MASKABLE. It cannot reach host entries, cannot clear
// the hard stop, and cannot disable the machinery globally -- there is no
// longer a global switch to reach.

typedef struct { int dummy; } PreemptData;

static void preempt_cleanup(ZymVM* vm, void* ptr) {
    const ZymAllocator* alloc = zym_getAllocator(vm);
    ZYM_FREE((ZymAllocator*)alloc, (PreemptData*)ptr, sizeof(PreemptData));
}

static ZymValue preempt_every(ZymVM* vm, ZymValue ctx, ZymValue slice, ZymValue cb) {
    (void)zym_getNativeData(ctx);
    if (!zym_isNumber(slice)) {
        zym_runtimeError(vm, "Preempt.every(slice, fn): slice must be a number.");
        return ZYM_ERROR;
    }
    if (!zym_isClosure(cb)) {
        zym_runtimeError(vm, "Preempt.every(slice, fn): fn must be a function.");
        return ZYM_ERROR;
    }
    // Checked here as well as in preemptRegister so the script gets the real
    // reason rather than the table-full message.
    if (AS_CLOSURE(cb)->function->arity != 0) {
        zym_runtimeError(vm, "Preempt.every(slice, fn): fn must take 0 arguments, got %d.",
                         AS_CLOSURE(cb)->function->arity);
        return ZYM_ERROR;
    }
    uint32_t id = preemptRegister(vm, (int)zym_asNumber(slice), cb,
                                  ZYM_PREEMPT_F_MASKABLE, true);
    if (id == 0) {
        zym_runtimeError(vm, "Preempt.every: no free preemption slots (max %d).",
                         ZYM_PREEMPT_MAX_ENTRIES);
        return ZYM_ERROR;
    }
    return zym_newNumber((double)id);
}

static ZymValue preempt_once(ZymVM* vm, ZymValue ctx, ZymValue slice, ZymValue cb) {
    (void)zym_getNativeData(ctx);
    if (!zym_isNumber(slice) || !zym_isClosure(cb)) {
        zym_runtimeError(vm, "Preempt.once(slice, fn): expected (number, function).");
        return ZYM_ERROR;
    }
    if (AS_CLOSURE(cb)->function->arity != 0) {
        zym_runtimeError(vm, "Preempt.once(slice, fn): fn must take 0 arguments, got %d.",
                         AS_CLOSURE(cb)->function->arity);
        return ZYM_ERROR;
    }
    uint32_t id = preemptRegister(vm, (int)zym_asNumber(slice), cb,
                                  ZYM_PREEMPT_F_MASKABLE | ZYM_PREEMPT_F_ONESHOT, true);
    if (id == 0) {
        zym_runtimeError(vm, "Preempt.once: no free preemption slots (max %d).",
                         ZYM_PREEMPT_MAX_ENTRIES);
        return ZYM_ERROR;
    }
    return zym_newNumber((double)id);
}

static ZymValue preempt_cancel(ZymVM* vm, ZymValue ctx, ZymValue idv) {
    (void)zym_getNativeData(ctx);
    if (!zym_isNumber(idv)) return zym_newBool(false);
    return zym_newBool(preemptUnregister(vm, (uint32_t)zym_asNumber(idv), true));
}

static ZymValue preempt_setSlice(ZymVM* vm, ZymValue ctx, ZymValue idv, ZymValue slice) {
    (void)zym_getNativeData(ctx);
    if (!zym_isNumber(idv) || !zym_isNumber(slice)) return zym_newBool(false);
    return zym_newBool(preemptSetSlice(vm, (uint32_t)zym_asNumber(idv),
                                       (int)zym_asNumber(slice), true));
}

static ZymValue preempt_remaining(ZymVM* vm, ZymValue ctx, ZymValue idv) {
    (void)zym_getNativeData(ctx);
    if (!zym_isNumber(idv)) return zym_newNumber(-1);
    return zym_newNumber((double)preemptEntryRemaining(vm, (uint32_t)zym_asNumber(idv), true));
}

static ZymValue preempt_request(ZymVM* vm, ZymValue ctx, ZymValue idv) {
    (void)zym_getNativeData(ctx);
    if (!zym_isNumber(idv)) return zym_newBool(false);
    return zym_newBool(preemptTrigger(vm, (uint32_t)zym_asNumber(idv), true));
}

// Run fn with this script's maskable entries suppressed. A non-maskable
// host entry (a watchdog) fires straight through, which is the point.
static ZymValue preempt_shield(ZymVM* vm, ZymValue ctx, ZymValue fn) {
    (void)zym_getNativeData(ctx);
    if (!zym_isClosure(fn)) {
        zym_runtimeError(vm, "Preempt.shield(fn): argument must be a function.");
        return ZYM_ERROR;
    }
    ObjClosure* closure = AS_CLOSURE(fn);
    ObjFunction* function = closure->function;
    if (function->arity != 0) {
        zym_runtimeError(vm, "Preempt.shield(fn): function must take 0 arguments, got %d.",
                         function->arity);
        return ZYM_ERROR;
    }

    int callee_slot = -1;
    if (vm->chunk != NULL && vm->ip > vm->chunk->code) {
        uint32_t prev_instr = *(vm->ip - 1);
        int opcode = prev_instr & 0xFF;
        if (opcode == CALL || opcode == CALL_SELF ||
            opcode == TAIL_CALL || opcode == TAIL_CALL_SELF) {
            int result_reg = (prev_instr >> 8) & 0xFF;
            int frame_base = (vm->frame_count > 0)
                           ? vm->frames[vm->frame_count - 1].stack_base : 0;
            callee_slot = frame_base + result_reg;
        }
    }
    if (callee_slot < 0) {
        zym_runtimeError(vm, "Preempt.shield: could not determine call context.");
        return ZYM_ERROR;
    }
    if (vm->frame_count >= FRAMES_MAX) {
        zym_runtimeError(vm, "Preempt.shield: stack overflow (max call depth reached).");
        return ZYM_ERROR;
    }

    int needed_top = callee_slot + function->max_regs;
    if (!growStackForCall(vm, needed_top, NULL)) {
        zym_runtimeError(vm, "Preempt.shield: stack overflow.");
        return ZYM_ERROR;
    }

    vm->stack[callee_slot] = fn;

    CallFrame* frame = &vm->frames[vm->frame_count++];
    frame->closure      = closure;
    frame->ip           = vm->ip;
    frame->stack_base   = callee_slot;
    frame->spill_base   = reserveSpillSlots(vm, function->spill_count);
    frame->caller_chunk = vm->chunk;
    frame->flags        = FRAME_FLAG_DISABLE_PREEMPT;
    frame->arg_count    = 0;
    frame->preempt_id   = 0;

    vm->current_frame = frame;
    vm->cur_base = callee_slot;
    vm->chunk = &function->chunk;
    vm->ip = function->chunk.code;
    if (needed_top > vm->stack_top) vm->stack_top = needed_top;

    preemptShieldPush(vm);
    return ZYM_CONTROL_TRANSFER;
}

// Script sees its own budget, not the machine's: `capacity` is what the host
// left it, and `ids` lists only entries it owns. Enumerating host entries would
// hand script a map of its own supervision that it could not act on anyway.
static ZymValue preempt_capacity(ZymVM* vm, ZymValue ctx) {
    (void)zym_getNativeData(ctx);
    return zym_newNumber((double)preemptScriptCapacity(vm));
}

static ZymValue preempt_available(ZymVM* vm, ZymValue ctx) {
    (void)zym_getNativeData(ctx);
    return zym_newNumber((double)preemptScriptAvailable(vm));
}

static ZymValue preempt_ids(ZymVM* vm, ZymValue ctx) {
    (void)zym_getNativeData(ctx);
    uint32_t ids[ZYM_PREEMPT_MAX_ENTRIES];
    int n = preemptIds(vm, ids, ZYM_PREEMPT_MAX_ENTRIES, /*script_owned_only=*/true);

    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);
    for (int i = 0; i < n; i++) {
        zym_listAppend(vm, list, zym_newNumber((double)ids[i]));
    }
    zym_popRoot(vm);
    return list;
}

static ZymValue preempt_shieldDepth(ZymVM* vm, ZymValue ctx) {
    (void)zym_getNativeData(ctx);
    return zym_newNumber((double)vm->preempt_shield_depth);
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

#define MK(sig, fn) zym_createNativeClosure(vm, sig, (void*)fn, context)
    ZymValue every       = MK("every(slice, fn)", preempt_every);        zym_pushRoot(vm, every);
    ZymValue once        = MK("once(slice, fn)", preempt_once);          zym_pushRoot(vm, once);
    ZymValue cancel      = MK("cancel(id)", preempt_cancel);             zym_pushRoot(vm, cancel);
    ZymValue setSlice    = MK("setSlice(id, n)", preempt_setSlice);      zym_pushRoot(vm, setSlice);
    ZymValue remaining   = MK("remaining(id)", preempt_remaining);       zym_pushRoot(vm, remaining);
    ZymValue request     = MK("request(id)", preempt_request);           zym_pushRoot(vm, request);
    ZymValue shield      = MK("shield(fn)", preempt_shield);             zym_pushRoot(vm, shield);
    ZymValue shieldDepth = MK("shieldDepth()", preempt_shieldDepth);     zym_pushRoot(vm, shieldDepth);
    ZymValue capacity    = MK("capacity()", preempt_capacity);           zym_pushRoot(vm, capacity);
    ZymValue available   = MK("available()", preempt_available);         zym_pushRoot(vm, available);
    ZymValue ids         = MK("ids()", preempt_ids);                     zym_pushRoot(vm, ids);
#undef MK

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "every", every);
    zym_mapSet(vm, obj, "once", once);
    zym_mapSet(vm, obj, "cancel", cancel);
    zym_mapSet(vm, obj, "setSlice", setSlice);
    zym_mapSet(vm, obj, "remaining", remaining);
    zym_mapSet(vm, obj, "request", request);
    zym_mapSet(vm, obj, "shield", shield);
    zym_mapSet(vm, obj, "shieldDepth", shieldDepth);
    zym_mapSet(vm, obj, "capacity", capacity);
    zym_mapSet(vm, obj, "available", available);
    zym_mapSet(vm, obj, "ids", ids);

    for (int i = 0; i < 13; i++) zym_popRoot(vm);   // context + 11 methods + obj
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
