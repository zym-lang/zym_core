#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "continuation.h"
#include "../memory.h"
#include "../gc.h"
#include "zym/zym.h"
#include "../table.h"
#include "../opcode.h"

bool pushPrompt(VM* vm, ObjPromptTag* tag) {
    if (vm->prompt_count >= MAX_PROMPTS) {
        runtimeError(vm, "Prompt stack overflow (max %d nested prompts).", MAX_PROMPTS);
        return false;
    }

    PromptEntry* entry = &vm->prompt_stack[vm->prompt_count++];
    entry->tag = tag;
    entry->frame_index = vm->frame_count;
    entry->stack_base = vm->stack_top;
    // Cont.withPrompt overwrites this immediately after; every other pusher
    // (the Cont.pushPrompt native) leaves it false, which is the truth: it
    // pushes no with_prompt boundary and nothing will auto-pop the entry.
    entry->from_with_prompt = false;
    return true;
}

void popPrompt(VM* vm) {
    if (vm->prompt_count > 0) {
        vm->prompt_count--;
    }
}

// Discard the matched prompt AND every entry pushed inside its extent.
//
// findPrompt scans from the top, so it can legitimately match an OUTER tag
// through inner prompts. The unwind that follows destroys the frames those
// inner prompts bookmark, so they are dead either way -- but a single
// popPrompt() removes only the topmost, i.e. an INNER entry, and leaves the
// entry we actually matched behind. That survivor is a ghost: still findable,
// pointing at a frame index and stack base that no longer exist, and holding a
// MAX_PROMPTS slot forever. Truncating to the matched index is the whole fix,
// and it is the same fix for capture, abort and shift because all three follow
// findPrompt with exactly one unwind.
static void popPromptsThrough(VM* vm, int prompt_index) {
    if (prompt_index >= 0 && vm->prompt_count > prompt_index) {
        vm->prompt_count = prompt_index;
    }
}

PromptEntry* findPrompt(VM* vm, ObjPromptTag* tag) {
    for (int i = vm->prompt_count - 1; i >= 0; i--) {
        if (vm->prompt_stack[i].tag->id == tag->id) {
            return &vm->prompt_stack[i];
        }
    }
    return NULL;
}

// Which ObjFunction embeds this chunk, or NULL if it is host-owned.
//
// A Chunk has no back-pointer to its owner, so we recover it by scanning the
// live frames: whatever chunk a capture saves is either the currently executing
// function's or its caller's, and both are on the frame stack at capture time.
// The result is what the GC marks -- without it, nothing keeps the resume
// target's bytecode alive once the capturing function goes out of scope.
static ObjFunction* ownerOfChunk(VM* vm, Chunk* chunk) {
    if (chunk == NULL) return NULL;
    for (int i = 0; i < vm->frame_count; i++) {
        ObjClosure* cl = vm->frames[i].closure;
        if (cl != NULL && cl->function != NULL && &cl->function->chunk == chunk) {
            return cl->function;
        }
    }
    return NULL;   // top-level/host chunk: nothing to mark
}

// Does this resume boundary belong to the extent being captured?
//
// A boundary at F fires when frames[F] returns, so it is ours only if frames[F]
// is ours: prompt_frame <= F < end_frame. The lower bound is strict on top of
// that -- F == prompt_frame is the boundary that carries the value OUT of the
// extent, into whoever resumed it, and that is the resumer's to push
// (Cont.resume does exactly that, for itself) rather than something this
// continuation should carry around. Above end_frame is the truncated-off
// preempt region, excluded for the same reason its frames and prompts are.
//
// The two slot bounds hold by construction -- result_slot is in
// frames[F - 1]'s window and saved_stack_top is the mark that frame's splice
// was based at, and both frames are captured whenever the boundary is. They
// are checked because each is rebased and written through on resume: an
// out-of-range result_slot is a stack write past the restored slice, and an
// out-of-range saved_stack_top moves the mark that bounds the GC's scan of the
// value stack. `<=` on the saved_stack_top upper bound because a mark may
// legitimately sit at the top of the slice; result_slot is a real slot and may
// not.
//
// Shared by the counting pass and the copying pass so the two cannot drift.
static inline bool resumeEntryInExtent(const ResumeContext* ctx, int prompt_frame,
                                       int end_frame, int stack_base, int stack_top) {
    if (ctx->frame_boundary <= prompt_frame) return false;
    if (ctx->frame_boundary >= end_frame) return false;
    if (ctx->result_slot < stack_base || ctx->result_slot >= stack_top) return false;
    if (ctx->saved_stack_top < stack_base || ctx->saved_stack_top > stack_top) return false;
    return true;
}

ObjContinuation* captureContinuation(VM* vm, ObjPromptTag* tag, int return_slot) {
    PromptEntry* prompt = findPrompt(vm, tag);
    if (prompt == NULL) {
        runtimeError(vm, "Cannot capture: prompt tag not found.");
        return NULL;
    }

    int prompt_index = (int)(prompt - vm->prompt_stack);
    int prompt_frame = prompt->frame_index;
    int capture_frame_count = vm->frame_count - prompt_frame;
    int capture_stack_top = vm->stack_top;

    uint32_t* preempt_saved_ip = NULL;
    Chunk* preempt_saved_chunk = NULL;
    for (int i = prompt_frame; i < vm->frame_count; i++) {
        // A host call boundary cannot be part of a continuation. The frames
        // above it only exist because native C code re-entered the VM, and
        // that C frame is not ours to save: resuming would have to return
        // through a caller whose stack has long since unwound. Natives are
        // atomic with respect to capture, and this is where that is enforced.
        //
        // Checked before the preempt case and in frame order on purpose. A
        // preempt frame truncates the capture, so a boundary *above* one is
        // outside the captured range and is none of our business -- only a
        // boundary we would actually have copied is an error.
        if (vm->frames[i].flags & FRAME_FLAG_API_BOUNDARY) {
            ObjClosure* cl = vm->frames[i].closure;
            ObjFunction* fn = (cl != NULL) ? cl->function : NULL;
            runtimeError(vm,
                "Cannot capture: a continuation cannot span a host call. "
                "'%.*s' was called from native code, and that native's frame "
                "cannot be captured or resumed. Capture before entering the "
                "native, or have it not call back into the VM.",
                fn && fn->name ? fn->name->length : 6,
                fn && fn->name ? fn->name->chars  : "<anon>");
            return NULL;
        }
        if (vm->frames[i].flags & FRAME_FLAG_PREEMPT) {
            capture_frame_count = i - prompt_frame;
            capture_stack_top = vm->frames[i].stack_base;
            preempt_saved_ip = vm->frames[i].ip;
            preempt_saved_chunk = vm->frames[i].caller_chunk;
            break;
        }
    }

    int capture_stack_base = prompt->stack_base;
    for (int i = 0; i < capture_frame_count; i++) {
        int frame_idx = prompt_frame + i;
        if (vm->frames[frame_idx].stack_base < capture_stack_base) {
            capture_stack_base = vm->frames[frame_idx].stack_base;
        }
    }

    int capture_stack_size = capture_stack_top - capture_stack_base;

    // Spilled locals are not in the window we just measured -- they live on
    // vm->spill_stack, bump-allocated per frame (see CallFrame.spill_base). The
    // frames memcpy below brings each frame's spill_base along as a raw
    // integer, which is meaningless on restore, so the values need a snapshot
    // of their own.
    //
    // The extent is exactly the captured frames' slice. Because the spill stack
    // is bump-allocated in frame order, the captured frames own
    // [frames[prompt_frame].spill_base, frames[end_frame].spill_base) -- and
    // when nothing above them was excluded, the upper bound is vm->spill_top.
    // Deriving the top from `end_frame` rather than always using spill_top is
    // what keeps a preempt-truncated capture honest: the frames from the
    // preempt frame upward are not part of this continuation, and neither are
    // their spills. It also degenerates correctly when nothing was captured at
    // all -- end_frame == prompt_frame makes the two bounds the same value.
    int end_frame = prompt_frame + capture_frame_count;
    int capture_spill_base = (prompt_frame < vm->frame_count)
        ? vm->frames[prompt_frame].spill_base
        : vm->spill_top;
    int capture_spill_top = (end_frame < vm->frame_count)
        ? vm->frames[end_frame].spill_base
        : vm->spill_top;
    int capture_spill_size = capture_spill_top - capture_spill_base;

    // Prompts that live inside the extent, i.e. everything above the delimiter.
    // The delimiter itself is excluded on purpose: the captured continuation is
    // undelimited until somebody re-wraps it, which is what makes
    // `Cont.resume` splice the body into the CALLER's prompt context rather
    // than silently reinstating the one it escaped.
    //
    // The upper bound is `end_frame`, not vm->prompt_count, for the same reason
    // the frame and spill slices use it: when a preempt frame truncates the
    // capture, prompts pushed by that callback are above the truncation and are
    // no more part of this continuation than its frames are. Entries are pushed
    // in frame order, so the qualifying ones are a contiguous run and the scan
    // can stop at the first that is out of range. `<= end_frame` rather than
    // `<`: a prompt pushed when frame_count == end_frame belongs to the body of
    // the last captured frame -- the preempt frame is what sits AT end_frame,
    // and anything it pushed is at end_frame + 1 or above.
    int capture_prompt_base = prompt_index + 1;
    int capture_prompt_count = 0;
    for (int i = capture_prompt_base; i < vm->prompt_count; i++) {
        if (vm->prompt_stack[i].frame_index > end_frame) break;
        capture_prompt_count++;
    }

    // Resume boundaries whose returning frame lives inside the extent. Same
    // argument as the prompts: the mapping is state the captured frames depend
    // on, it is not reconstructible from the frames themselves, and leaving it
    // behind silently breaks the extent the next time it is resumed.
    // resumeEntryInExtent above owns the membership test.
    //
    // Counted here and copied entry by entry further down rather than block-
    // copied from a base index. Entries are pushed in frame order, so the
    // qualifying ones ARE a contiguous run today and a memcpy would work -- but
    // it would work only for as long as that stays true. The membership test
    // includes two bounds checks that are meant never to fire, and if one ever
    // did, a base-and-count memcpy would not drop that entry: it would shift
    // the whole run by one and copy a neighbour in its place, turning a guard
    // into silent corruption. A filtered copy cannot do that.
    int capture_resume_count = 0;
    for (int i = 0; i < vm->resume_depth; i++) {
        if (vm->resume_stack[i].frame_boundary >= end_frame) break;
        if (resumeEntryInExtent(&vm->resume_stack[i], prompt_frame, end_frame,
                                capture_stack_base, capture_stack_top)) {
            capture_resume_count++;
        }
    }

    // Record every open upvalue pointing into the extent BEFORE closing
    // them: closeUpvalues removes them from the open list and seals their
    // locations, and the offsets have to be taken while the locations
    // still point at stack slots. Same boundary condition as
    // closeUpvalues, so the recorded set is exactly the closed set. See
    // ObjContinuation.upvalues for why resume needs this.
    int capture_upvalue_count = 0;
    for (ObjUpvalue* uv = vm->open_upvalues;
         uv != NULL && uv->location >= &vm->stack[capture_stack_base];
         uv = uv->next) {
        capture_upvalue_count++;
    }
    ContUpvalue* upvalues_buf = NULL;
    if (capture_upvalue_count > 0) {
        // The allocation can collect; the upvalue objects stay reachable
        // through vm->open_upvalues (a GC root) and the closures on the
        // still-intact stack, and a collection never mutates the list.
        upvalues_buf = ALLOCATE(vm, ContUpvalue, capture_upvalue_count);
        int ui = 0;
        for (ObjUpvalue* uv = vm->open_upvalues;
             uv != NULL && uv->location >= &vm->stack[capture_stack_base];
             uv = uv->next) {
            upvalues_buf[ui].upvalue = uv;
            upvalues_buf[ui].slot = (int)(uv->location - &vm->stack[capture_stack_base]);
            ui++;
        }
    }

    closeUpvalues(vm, &vm->stack[capture_stack_base]);

    ObjContinuation* cont = newContinuation(vm);
    pushTempRoot(vm, (Obj*)cont);

    CallFrame* frames_buf = NULL;
    if (capture_frame_count > 0) {
        frames_buf = ALLOCATE(vm, CallFrame, capture_frame_count);
        memset(frames_buf, 0, capture_frame_count * sizeof(CallFrame));
        memcpy(frames_buf, &vm->frames[prompt_frame],
               capture_frame_count * sizeof(CallFrame));
    }

    Value* stack_buf = NULL;
    if (capture_stack_size > 0) {
        stack_buf = ALLOCATE(vm, Value, capture_stack_size);
        for (int i = 0; i < capture_stack_size; i++) stack_buf[i] = NULL_VAL;
        memcpy(stack_buf, &vm->stack[capture_stack_base],
               capture_stack_size * sizeof(Value));
    }

    Value* spill_buf = NULL;
    if (capture_spill_size > 0) {
        spill_buf = ALLOCATE(vm, Value, capture_spill_size);
        for (int i = 0; i < capture_spill_size; i++) spill_buf[i] = NULL_VAL;
        memcpy(spill_buf, &vm->spill_stack[capture_spill_base],
               capture_spill_size * sizeof(Value));
    }

    // Safe to allocate before the entries are handed to `cont`: the tags are
    // still on vm->prompt_stack (the caller truncates it only after this
    // returns), and markRoots covers that. From the truncation onward
    // blackenObject's walk of cont->prompts is their only root.
    PromptEntry* prompts_buf = NULL;
    if (capture_prompt_count > 0) {
        prompts_buf = ALLOCATE(vm, PromptEntry, capture_prompt_count);
        memcpy(prompts_buf, &vm->prompt_stack[capture_prompt_base],
               capture_prompt_count * sizeof(PromptEntry));
    }

    // Plain ints, so unlike the three buffers above this one has no bearing on
    // the GC: nothing to mark, and blackenObject deliberately says nothing
    // about it.
    ResumeContext* resumes_buf = NULL;
    if (capture_resume_count > 0) {
        resumes_buf = ALLOCATE(vm, ResumeContext, capture_resume_count);
        int n = 0;
        for (int i = 0; i < vm->resume_depth && n < capture_resume_count; i++) {
            if (vm->resume_stack[i].frame_boundary >= end_frame) break;
            if (resumeEntryInExtent(&vm->resume_stack[i], prompt_frame, end_frame,
                                    capture_stack_base, capture_stack_top)) {
                resumes_buf[n++] = vm->resume_stack[i];
            }
        }
        capture_resume_count = n;   // the two passes agree, but do not assume it
    }

    cont->frames = frames_buf;
    cont->frame_count = capture_frame_count;

    cont->stack = stack_buf;
    cont->stack_size = capture_stack_size;

    cont->spill = spill_buf;
    cont->spill_size = capture_spill_size;
    cont->spill_base_offset = capture_spill_base;

    cont->prompts = prompts_buf;
    cont->prompt_count = capture_prompt_count;

    cont->resumes = resumes_buf;
    cont->resume_count = capture_resume_count;

    cont->upvalues = upvalues_buf;
    cont->upvalue_count = capture_upvalue_count;

    cont->frame_base_offset = prompt_frame;

    if (preempt_saved_ip != NULL) {
        cont->saved_ip = preempt_saved_ip;
        cont->saved_chunk = preempt_saved_chunk;
    } else {
        cont->saved_ip = vm->ip;
        cont->saved_chunk = vm->chunk;
    }
    // Resolve while the owning frame is still live -- after the unwind the
    // caller performs, the frame stack no longer names it.
    cont->saved_owner = ownerOfChunk(vm, cont->saved_chunk);
    cont->stack_base_offset = capture_stack_base;

    cont->prompt_tag = tag;
    cont->state = CONT_VALID;
    
    // The resumed continuation should run at the shield depth its OWN frames
    // account for, so subtract the shields owned by the frames we are not
    // capturing -- everything from the truncating preempt frame upward.
    //
    // Only FRAME_FLAG_DISABLE_PREEMPT owns a shield. preemptShieldPush() has
    // exactly one caller: the point where Preempt.shield pushes such a frame.
    // Every pop path (both RETs in run() and unwindFrames) decrements for
    // exactly that same flag, so depth == the number of live DISABLE_PREEMPT
    // frames, always. FRAME_FLAG_PREEMPT contributes nothing -- a preempt
    // callback is masked structurally by its entry's in_flight guard, not by
    // this counter -- so subtracting for one takes away a shield that was never
    // pushed and drives the saved depth negative. resumeContinuation() installs
    // the value verbatim, and from -1 the next Preempt.shield only reaches 0:
    // the script's critical section then runs completely unmasked.
    //
    // Because of that 1:1 correspondence the excluded shields are a subset of
    // the live ones, so this cannot go below zero by construction. The clamp
    // exists only to stop a future break in that invariant from escaping the
    // function, and it lives here, at the single place the field is written, so
    // that every reader -- resumeContinuation included -- can take
    // `cont->preempt_shield_depth >= 0` for granted rather than each re-testing
    // it and none of them owning the invariant.
    // Store what the CAPTURED FRAMES THEMSELVES contribute, not the capturer's
    // total. The two differ by every shield living below the prompt, which is
    // the resumer's business and not this continuation's: a continuation may be
    // resumed anywhere, including inside a shield the capturer never saw.
    // Recording the total and installing it verbatim made resume overwrite the
    // resumer's live shields with the capturer's -- the shield frames stayed on
    // the stack and still decremented on the way out, but the counter they
    // depend on had been replaced, so a supposedly masked section ran unmasked.
    // Counting only the captured extent makes the value compositional: resume
    // adds it to whatever the resumer already has.
    int shield_delta = 0;
    for (int i = prompt_frame; i < prompt_frame + capture_frame_count; i++) {
        if (vm->frames[i].flags & FRAME_FLAG_DISABLE_PREEMPT) {
            shield_delta++;
        }
    }
    cont->preempt_shield_depth = shield_delta;

    // return_slot is derived from the frame that called Cont.capture. That is
    // the right slot for an ordinary capture, where resuming re-enters that
    // call and the resume value is its result.
    //
    // It is meaningless for a preempt-truncated capture. There the captured
    // extent stops BELOW the preempt frame, so the calling frame -- the
    // callback's -- is not part of what gets restored, and its slot does not
    // exist in the restored slice. The continuation instead resumes the
    // interrupted computation at preempt_saved_ip, mid instruction stream,
    // with no call expression waiting on a value. Writing resume_value
    // anywhere would land outside the slice (an out-of-bounds stack write) or
    // clobber a live register of the resumed frame.
    //
    // -1 marks that: resume skips the result write entirely.
    cont->return_slot = (preempt_saved_ip != NULL)
        ? -1
        : return_slot + (prompt->stack_base - capture_stack_base);

    popTempRoot(vm);
    return cont;
}

bool resumeContinuation(VM* vm, ObjContinuation* cont, Value resume_value) {
    if (cont->state != CONT_VALID) {
        runtimeError(vm, "Cannot resume: continuation already consumed or invalid.");
        return false;
    }

    if (vm->frame_count + cont->frame_count > FRAMES_MAX) {
        runtimeError(vm, "Stack overflow: resuming continuation would exceed frame limit.");
        return false;
    }

    // The resume writes two things into the value stack: the captured slice
    // (stack_size slots from restore_base) and the resume value, at
    // restore_base + return_slot. return_slot is NOT bounded by stack_size --
    // it is the caller's result slot, rebased at capture, and when the prompt
    // sat at the top of the captured extent it lands just past the slice. So
    // reserve for whichever reaches further, or the write at result_slot goes
    // off the end of the allocation. ASAN caught exactly that: an 8-byte write
    // one Value past a stack still at STACK_INITIAL.
    int slice_top  = cont->stack_size;
    int result_top = cont->return_slot + 1;
    int needed_top = vm->stack_top + (slice_top > result_top ? slice_top : result_top);

    if (needed_top > STACK_MAX) {
        runtimeError(vm, "Stack overflow: resuming continuation needs %d slots, max is %d.", needed_top, STACK_MAX);
        return false;
    }

    // The extent's own prompts are about to go back on both stacks, so their
    // capacity is a precondition of the resume exactly like FRAMES_MAX and
    // STACK_MAX are. Checked up here, with the other preconditions, because
    // everything below the spill reservation is deliberately infallible.
    if (vm->prompt_count + cont->prompt_count > MAX_PROMPTS) {
        runtimeError(vm, "Prompt stack overflow: resuming continuation needs %d prompts, max is %d.",
                     vm->prompt_count + cont->prompt_count, MAX_PROMPTS);
        return false;
    }

    int restore_boundary_count = 0;
    for (int i = 0; i < cont->prompt_count; i++) {
        if (cont->prompts[i].from_with_prompt) restore_boundary_count++;
    }
    if (vm->with_prompt_depth + restore_boundary_count > MAX_WITH_PROMPT_DEPTH) {
        runtimeError(vm, "Cont.resume: maximum withPrompt nesting depth exceeded.");
        return false;
    }

    // The extent's resume boundaries go back on vm->resume_stack, which is
    // fixed-size for the same reason the other three are. Cont.resume has
    // already pushed its own entry by the time it calls this, so the count it
    // checked does not cover these.
    if (vm->resume_depth + cont->resume_count > MAX_RESUME_DEPTH) {
        runtimeError(vm, "Cont.resume: maximum resume nesting depth exceeded.");
        return false;
    }

    // growStackForCall disables the GC across the realloc and NULL-fills slots
    // above the OLD capacity; the hand-rolled GROW_ARRAY this replaced did
    // neither. Note the fill only covers newly added capacity -- slots already
    // within capacity keep whatever they held, same as the CALL opcodes.
    if (!growStackForCall(vm, needed_top, NULL)) {
        return false;
    }

    // Put the spilled locals back before anything else moves, so the rest of
    // the restore can rebase against a base that already exists. Done through
    // reserveSpillSlots rather than by hand because that is the same path a
    // frame push takes: it grows the array, NULL-fills the new region, and
    // bumps spill_top, and the fill matters -- the growth can collect, and the
    // collector scans the whole live region.
    //
    // Placed below the last failure check (growStackForCall) on purpose:
    // spill_top is VM state and this bump is not undone on the way out, so a
    // resume that still had a way to bail after it would silently strand slots
    // for the rest of the run. Nothing from here on can fail.
    //
    // The extent is a slice of the array, so the whole restore is one memcpy at
    // one base -- the per-frame rebase below is arithmetic on spill_base only,
    // not a second copy.
    int restore_spill_base = vm->spill_top;
    if (cont->spill_size > 0) {
        restore_spill_base = reserveSpillSlots(vm, cont->spill_size);
        memcpy(&vm->spill_stack[restore_spill_base], cont->spill,
               cont->spill_size * sizeof(Value));
    }

    cont->state = CONT_CONSUMED;

    int restore_base = vm->stack_top;

    // cont->stack is NULL when nothing was captured; memcpy(dst, NULL, 0) is UB.
    if (cont->stack_size > 0) {
        memcpy(&vm->stack[restore_base], cont->stack, cont->stack_size * sizeof(Value));
    }
    vm->stack_top = restore_base + cont->stack_size;

    int restore_frame_base = vm->frame_count;

    for (int i = 0; i < cont->frame_count; i++) {
        CallFrame* src = &cont->frames[i];
        CallFrame* dst = &vm->frames[vm->frame_count + i];

        dst->closure = src->closure;
        dst->ip = src->ip;
        dst->caller_chunk = src->caller_chunk;
        dst->flags = src->flags;
        // Capture preserved these; restoring only five of seven fields left the
        // frame inheriting arg_count/preempt_id from whatever last used the slot.
        dst->arg_count = src->arg_count;
        dst->preempt_id = src->preempt_id;

        int original_offset = src->stack_base - cont->stack_base_offset;
        dst->stack_base = restore_base + original_offset;

        // Same rebase as stack_base, against the spill snapshot's own origin.
        // Per-frame rather than one shared base: the captured frames sit at
        // different offsets inside the snapshot, and copying spill_base
        // verbatim (which is what happened before) hands the resumed frame an
        // offset the VM has since re-issued to somebody else.
        dst->spill_base = restore_spill_base + (src->spill_base - cont->spill_base_offset);
    }
    vm->frame_count += cont->frame_count;
    vm->cur_base = vm->frame_count == 0 ? 0 : vm->frames[vm->frame_count - 1].stack_base;
    vm->current_frame = vm->frame_count == 0 ? NULL : &vm->frames[vm->frame_count - 1];

    // Put the extent's own prompts back, rebased the same way the frames were:
    // frame_index against restore_frame_base, stack_base against restore_base.
    // Without this the resumed body stands dynamically inside a withPrompt it
    // cannot name -- `Cont.capture(INNER)` right after a resume reports "prompt
    // tag not found" for a prompt that is, by construction, live.
    //
    // A restored entry sorts above every live one: restore_frame_base ==
    // vm->frame_count before the splice, and a live with_prompt boundary is the
    // index of a frame that already exists, hence strictly smaller. So pushing
    // in snapshot order keeps both stacks ordered.
    //
    // The paired with_prompt boundary is re-pushed for exactly the entries that
    // had one. unwindFrames dropped those boundaries at capture time -- rightly,
    // the frames were gone -- and the frames are back now, so the boundary that
    // pops the prompt on RET has to come back too. Its frame_boundary is the
    // entry's own frame_index because Cont.withPrompt reads both from the same
    // vm->frame_count. Skipping this half would swap the lost prompt for a
    // leaked one.
    int prompt_frame_delta = restore_frame_base - cont->frame_base_offset;
    for (int i = 0; i < cont->prompt_count; i++) {
        PromptEntry* src = &cont->prompts[i];
        PromptEntry* dst = &vm->prompt_stack[vm->prompt_count++];

        dst->tag = src->tag;
        dst->frame_index = src->frame_index + prompt_frame_delta;
        dst->stack_base = restore_base + (src->stack_base - cont->stack_base_offset);
        dst->from_with_prompt = src->from_with_prompt;

        if (src->from_with_prompt) {
            vm->with_prompt_stack[vm->with_prompt_depth].frame_boundary = dst->frame_index;
            vm->with_prompt_depth++;
            vm->active_boundaries++;
        }
    }

    // Put the extent's own splice points back, rebased the same way. Ordering
    // works out for the same reason the prompts' does, and the entry Cont.
    // resume pushed for this very resume stays correctly at the bottom of the
    // run: its frame_boundary is restore_frame_base, and every entry here is
    // strictly above that (captureContinuation kept only boundaries above the
    // delimiting frame).
    //
    // Without this, every RET inside a multi-splice extent below the outermost
    // one falls through to the ordinary path and writes the value to the
    // returning frame's own stack_base -- which for a spliced-in frame is a
    // slot its caller never reads.
    for (int i = 0; i < cont->resume_count; i++) {
        ResumeContext* src = &cont->resumes[i];
        ResumeContext* dst = &vm->resume_stack[vm->resume_depth++];

        dst->frame_boundary = src->frame_boundary + prompt_frame_delta;
        dst->result_slot = restore_base + (src->result_slot - cont->stack_base_offset);
        // A stack index like result_slot, so it rebases the same way: the mark
        // this boundary has to put back is the one that was live at its own
        // splice point, which sits inside the slice being laid down here.
        dst->saved_stack_top = restore_base + (src->saved_stack_top - cont->stack_base_offset);
        vm->active_boundaries++;
    }

    // Restore the open-upvalue aliases (see ObjContinuation.upvalues).
    // For each upvalue the capture sealed: the cell's current value is
    // written into the restored slot FIRST -- a closure that ran while the
    // continuation was parked wrote through the cell, and those writes
    // must not be undone by the snapshot copy -- then the upvalue
    // re-opens onto the slot and rejoins vm->open_upvalues, which both
    // restores the alias and puts it back where stack relocation
    // (updateStackReferences) can rebase it. Insertion keeps the list's
    // descending-address order; the restored slots normally sit above
    // every live open upvalue (the splice lands on top of the stack), but
    // the walk assumes nothing.
    for (int i = 0; i < cont->upvalue_count; i++) {
        ObjUpvalue* uv = cont->upvalues[i].upvalue;
        Value* slot = &vm->stack[restore_base + cont->upvalues[i].slot];

        *slot = *uv->location;
        uv->location = slot;

        if (vm->open_upvalues == NULL || vm->open_upvalues->location < slot) {
            uv->next = vm->open_upvalues;
            vm->open_upvalues = uv;
        } else {
            ObjUpvalue* prev = vm->open_upvalues;
            while (prev->next != NULL && prev->next->location > slot) {
                prev = prev->next;
            }
            uv->next = prev->next;
            prev->next = uv;
        }
    }

    vm->ip = cont->saved_ip;
    vm->chunk = cont->saved_chunk;
    // Added, not assigned. The field counts only what the captured frames
    // contribute (captureContinuation is its sole writer and counts DISABLE_
    // PREEMPT frames inside the captured extent, so it is >= 0 by construction
    // and needs no clamp here). The resumer's own shields stay live: its frames
    // are still on the stack below the splice point and will decrement on their
    // way out, so their depth must survive the resume that runs between.
    vm->preempt_shield_depth += cont->preempt_shield_depth;

    // -1 means the capture was preempt-truncated: the interrupted computation
    // resumes mid instruction stream and nothing is waiting on a value. See the
    // matching note in captureContinuation.
    if (cont->return_slot >= 0) {
        int result_slot = restore_base + cont->return_slot;
        vm->stack[result_slot] = resume_value;
    }

    // The snapshot is dead the instant the state flips to CONT_CONSUMED: the
    // guard at the top makes the continuation permanently unresumable, and
    // everything it held now lives in the stack again. Release it here rather
    // than waiting for a sweep -- blackenObject marks the captured stack with
    // no state check, so a spent continuation left holding its snapshot pins
    // its whole capture-time object graph for as long as script keeps the
    // handle. Safe here: reallocate only collects when growing, so a free
    // cannot re-enter the GC while the VM sits half-restored.
    // Guarded on pointer AND count, matching freeObject: a mis-sized FREE_ARRAY
    // would corrupt the allocator's accounting if that invariant ever slipped.
    if (cont->frames != NULL && cont->frame_count > 0) {
        FREE_ARRAY(vm, CallFrame, cont->frames, cont->frame_count);
    }
    cont->frames = NULL;
    cont->frame_count = 0;

    if (cont->stack != NULL && cont->stack_size > 0) {
        FREE_ARRAY(vm, Value, cont->stack, cont->stack_size);
    }
    cont->stack = NULL;
    cont->stack_size = 0;

    if (cont->spill != NULL && cont->spill_size > 0) {
        FREE_ARRAY(vm, Value, cont->spill, cont->spill_size);
    }
    cont->spill = NULL;
    cont->spill_size = 0;

    if (cont->prompts != NULL && cont->prompt_count > 0) {
        FREE_ARRAY(vm, PromptEntry, cont->prompts, cont->prompt_count);
    }
    cont->prompts = NULL;
    cont->prompt_count = 0;

    if (cont->resumes != NULL && cont->resume_count > 0) {
        FREE_ARRAY(vm, ResumeContext, cont->resumes, cont->resume_count);
    }
    cont->resumes = NULL;
    cont->resume_count = 0;

    if (cont->upvalues != NULL && cont->upvalue_count > 0) {
        FREE_ARRAY(vm, ContUpvalue, cont->upvalues, cont->upvalue_count);
    }
    cont->upvalues = NULL;
    cont->upvalue_count = 0;

    return true;
}

typedef struct {
    int dummy;
} ContData;

static void cont_cleanup(ZymVM* vm, void* ptr) {
    ContData* data = (ContData*)ptr;
    const ZymAllocator* alloc = zym_getAllocator(vm);
    ZYM_FREE((ZymAllocator*)alloc, data, sizeof(ContData));
}

static ZymValue cont_newPrompt_0(ZymVM* vm, ZymValue context) {
    (void)zym_getNativeData(context);
    ObjPromptTag* tag = newPromptTag(vm, NULL);
    return OBJ_VAL(tag);
}

static ZymValue cont_newPrompt_1(ZymVM* vm, ZymValue context, ZymValue name) {
    (void)zym_getNativeData(context);
    ObjString* nameStr = NULL;
    if (zym_isString(name)) {
        nameStr = AS_STRING(name);
    }
    ObjPromptTag* tag = newPromptTag(vm, nameStr);
    return OBJ_VAL(tag);
}

static ZymValue cont_isValid(ZymVM* vm, ZymValue context, ZymValue continuation) {
    (void)vm;
    (void)zym_getNativeData(context);
    if (!IS_CONTINUATION(continuation)) {
        return zym_newBool(false);
    }
    ObjContinuation* cont = AS_CONTINUATION(continuation);
    return zym_newBool(cont->state == CONT_VALID);
}

static ZymValue cont_isPromptTag(ZymVM* vm, ZymValue context, ZymValue value) {
    (void)vm;
    (void)zym_getNativeData(context);
    return zym_newBool(IS_PROMPT_TAG(value));
}

static ZymValue cont_isContinuation(ZymVM* vm, ZymValue context, ZymValue value) {
    (void)vm;
    (void)zym_getNativeData(context);
    return zym_newBool(IS_CONTINUATION(value));
}

static ZymValue cont_pushPrompt_native(ZymVM* vm, ZymValue context, ZymValue tag_val) {
    (void)zym_getNativeData(context);

    if (!IS_PROMPT_TAG(tag_val)) {
        zym_runtimeError(vm, "Cont.pushPrompt: argument must be a prompt tag.");
        return ZYM_ERROR;
    }

    ObjPromptTag* tag = AS_PROMPT_TAG(tag_val);
    if (!pushPrompt(vm, tag)) {
        return ZYM_ERROR;
    }

    return zym_newNull();
}

static ZymValue cont_popPrompt_native(ZymVM* vm, ZymValue context) {
    (void)vm;
    (void)zym_getNativeData(context);

    if (vm->prompt_count == 0) {
        zym_runtimeError(vm, "Cont.popPrompt: no active prompts to pop.");
        return ZYM_ERROR;
    }

    popPrompt(vm);
    return zym_newNull();
}

static ZymValue cont_withPrompt(ZymVM* vm, ZymValue context, ZymValue tag, ZymValue fn) {
    (void)zym_getNativeData(context);

    if (!IS_PROMPT_TAG(tag)) {
        zym_runtimeError(vm, "Cont.withPrompt: first argument must be a prompt tag.");
        return ZYM_ERROR;
    }

    if (!IS_CLOSURE(fn)) {
        zym_runtimeError(vm, "Cont.withPrompt: second argument must be a function.");
        return ZYM_ERROR;
    }

    ObjPromptTag* promptTag = AS_PROMPT_TAG(tag);
    ObjClosure* closure = AS_CLOSURE(fn);
    ObjFunction* function = closure->function;

    if (function->arity != 0) {
        zym_runtimeError(vm, "Cont.withPrompt: function must take 0 arguments, got %d.", function->arity);
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
        zym_runtimeError(vm, "Cont.withPrompt: could not determine call context.");
        return ZYM_ERROR;
    }

    if (vm->frame_count >= FRAMES_MAX) {
        zym_runtimeError(vm, "Cont.withPrompt: stack overflow (max call depth reached).");
        return ZYM_ERROR;
    }

    if (vm->with_prompt_depth >= MAX_WITH_PROMPT_DEPTH) {
        zym_runtimeError(vm, "Cont.withPrompt: maximum nesting depth exceeded.");
        return ZYM_ERROR;
    }

    // Every other frame-push site sizes the window as max_regs (vm.c and the
    // CALL opcodes). Spill slots are reserved separately on vm->spill_stack
    // just below, so they are deliberately absent from this arithmetic.
    int needed_top = callee_slot + function->max_regs;
    if (needed_top > STACK_MAX) {
        zym_runtimeError(vm, "Cont.withPrompt: stack overflow.");
        return ZYM_ERROR;
    }
    if (!growStackForCall(vm, needed_top, NULL)) {
        return ZYM_ERROR;
    }

    vm->stack[callee_slot] = fn;

    if (!pushPrompt(vm, promptTag)) {
        return ZYM_ERROR;
    }
    // Marks the entry as owning the with_prompt boundary pushed just below, so
    // a continuation that snapshots this prompt knows to re-push both halves.
    vm->prompt_stack[vm->prompt_count - 1].from_with_prompt = true;

    vm->with_prompt_stack[vm->with_prompt_depth].frame_boundary = vm->frame_count;
    vm->with_prompt_depth++;
    vm->active_boundaries++;

    CallFrame* frame = &vm->frames[vm->frame_count++];
    frame->closure = closure;
    frame->ip = vm->ip;
    frame->stack_base = callee_slot;
    frame->spill_base = frameReserveSpills(vm, function->spill_count);
    frame->caller_chunk = vm->chunk;
    frame->flags = 0;
    frame->arg_count = 0;    // withPrompt requires arity 0
    frame->preempt_id = 0;

    vm->current_frame = frame;
    vm->cur_base = callee_slot;
    vm->chunk = &function->chunk;
    vm->ip = function->chunk.code;

    if (needed_top > vm->stack_top) {
        vm->stack_top = needed_top;
    }

    return ZYM_CONTROL_TRANSFER;
}

static ZymValue cont_capture(ZymVM* vm, ZymValue context, ZymValue tag_val) {
    (void)zym_getNativeData(context);

    if (!IS_PROMPT_TAG(tag_val)) {
        zym_runtimeError(vm, "Cont.capture: argument must be a prompt tag.");
        return ZYM_ERROR;
    }

    ObjPromptTag* tag = AS_PROMPT_TAG(tag_val);

    PromptEntry* prompt = findPrompt(vm, tag);
    if (prompt == NULL) {
        zym_runtimeError(vm, "Cont.capture: prompt tag not found.");
        return ZYM_ERROR;
    }
    // Taken before the unwind, while the stack still holds the entry. Nothing
    // between here and the pop touches vm->prompt_count, and prompt_stack is a
    // fixed array, so the pointer and the index stay in step.
    int prompt_index = (int)(prompt - vm->prompt_stack);

    int return_slot = 0;

    if (vm->chunk != NULL && vm->ip > vm->chunk->code) {
        uint32_t prev_instr = *(vm->ip - 1);
        int opcode = prev_instr & 0xFF;

        if (opcode == CALL || opcode == CALL_SELF || opcode == TAIL_CALL ||
            opcode == TAIL_CALL_SELF) {
            int result_reg = (prev_instr >> 8) & 0xFF;
            int frame_base = (vm->frame_count > 0) ? vm->frames[vm->frame_count - 1].stack_base : 0;
            int absolute_slot = frame_base + result_reg;

            return_slot = absolute_slot - prompt->stack_base;
        }
    }

    ObjContinuation* cont = captureContinuation(vm, tag, return_slot);
    if (cont == NULL) {
        return ZYM_ERROR;
    }

    // captureContinuation drops its own temp root before returning, so `cont` is
    // just a C local until it reaches a stack slot below. Nothing in that window
    // allocates today, but cont_shift roots it over the same span; match that
    // rather than depend on the window staying allocation-free.
    pushTempRoot(vm, (Obj*)cont);

    unwindFrames(vm, prompt->frame_index);
    // Releases every slot the unwind just abandoned. Through lowerStackTop
    // rather than by assignment: the released slots have to be cleared, or
    // the objects they name are collected and the next call raises the mark
    // back over the dangling pointers. See the note on lowerStackTop.
    lowerStackTop(vm, prompt->stack_base);

    if (cont->frame_count > 0) {
        CallFrame* captured_frame = &cont->frames[0];
        vm->ip = captured_frame->ip;
        vm->chunk = captured_frame->caller_chunk;

        if (vm->chunk == NULL && vm->frame_count > 0) {
            vm->chunk = &vm->frames[vm->frame_count - 1].closure->function->chunk;
        }
    } else if (vm->frame_count > 0) {
        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        vm->ip = frame->ip;
        vm->chunk = frame->caller_chunk ? frame->caller_chunk : &frame->closure->function->chunk;
    }

    popPromptsThrough(vm, prompt_index);

    while (vm->resume_depth > 0 &&
           vm->resume_stack[vm->resume_depth - 1].frame_boundary >= vm->frame_count) {
        ResumeContext* ctx = &vm->resume_stack[--vm->resume_depth];
        vm->active_boundaries--;
        vm->stack[ctx->result_slot] = OBJ_VAL(cont);
    }

    if (vm->chunk != NULL && vm->ip > vm->chunk->code) {
        uint32_t prev_instr = *(vm->ip - 1);
        int opcode = prev_instr & 0xFF;

        if (opcode == CALL || opcode == CALL_SELF || opcode == TAIL_CALL ||
            opcode == TAIL_CALL_SELF) {
            int result_reg = (prev_instr >> 8) & 0xFF;
            int frame_base = (vm->frame_count > 0) ? vm->frames[vm->frame_count - 1].stack_base : 0;
            int result_slot = frame_base + result_reg;

            vm->stack[result_slot] = OBJ_VAL(cont);
        } else {
            vm->stack[vm->stack_top] = OBJ_VAL(cont);
            vm->stack_top++;
        }
    } else {
        vm->stack[vm->stack_top] = OBJ_VAL(cont);
        vm->stack_top++;
    }

    popTempRoot(vm);
    return ZYM_CONTROL_TRANSFER;
}

static ZymValue cont_resume(ZymVM* vm, ZymValue context, ZymValue continuation, ZymValue value) {
    (void)zym_getNativeData(context);

    if (!IS_CONTINUATION(continuation)) {
        zym_runtimeError(vm, "Cont.resume: first argument must be a continuation.");
        return ZYM_ERROR;
    }

    ObjContinuation* cont = AS_CONTINUATION(continuation);

    if (vm->resume_depth >= MAX_RESUME_DEPTH) {
        zym_runtimeError(vm, "Cont.resume: maximum resume nesting depth exceeded.");
        return ZYM_ERROR;
    }

    int resume_result_slot = -1;
    if (vm->chunk != NULL && vm->ip > vm->chunk->code) {
        uint32_t prev_instr = *(vm->ip - 1);
        int opcode = prev_instr & 0xFF;

        if (opcode == CALL || opcode == CALL_SELF || opcode == TAIL_CALL ||
            opcode == TAIL_CALL_SELF) {
            int result_reg = (prev_instr >> 8) & 0xFF;
            int frame_base = (vm->frame_count > 0) ? vm->frames[vm->frame_count - 1].stack_base : 0;
            resume_result_slot = frame_base + result_reg;
        }
    }

    if (resume_result_slot < 0) {
        zym_runtimeError(vm, "Cont.resume must be called in a value position (as part of a call expression).");
        return ZYM_ERROR;
    }

    uint32_t* resume_return_ip = vm->ip;
    Chunk* resume_return_chunk = vm->chunk;
    int frames_before = vm->frame_count;

    vm->resume_stack[vm->resume_depth].frame_boundary = frames_before;
    vm->resume_stack[vm->resume_depth].result_slot = resume_result_slot;
    // Recorded here rather than inside resumeContinuation because this is the
    // last point at which the value is unambiguously "before the splice" --
    // and it is the same value resumeContinuation goes on to use as its
    // restore_base. growStackForCall, the only thing that runs in between,
    // moves capacity and not the mark.
    vm->resume_stack[vm->resume_depth].saved_stack_top = vm->stack_top;
    vm->resume_depth++;
    vm->active_boundaries++;

    if (!resumeContinuation(vm, cont, value)) {
        vm->resume_depth--;
        vm->active_boundaries--;
        return ZYM_ERROR;
    }

    if (frames_before < vm->frame_count) {
        vm->frames[frames_before].ip = resume_return_ip;
        vm->frames[frames_before].caller_chunk = resume_return_chunk;
    }

    return ZYM_CONTROL_TRANSFER;
}

static ZymValue cont_abort(ZymVM* vm, ZymValue context, ZymValue tag_val, ZymValue abort_value) {
    (void)zym_getNativeData(context);

    if (!IS_PROMPT_TAG(tag_val)) {
        zym_runtimeError(vm, "Cont.abort: first argument must be a prompt tag.");
        return ZYM_ERROR;
    }

    ObjPromptTag* tag = AS_PROMPT_TAG(tag_val);

    PromptEntry* prompt = findPrompt(vm, tag);
    if (prompt == NULL) {
        zym_runtimeError(vm, "Cont.abort: prompt tag not found.");
        return ZYM_ERROR;
    }
    int prompt_index = (int)(prompt - vm->prompt_stack);

    closeUpvalues(vm, &vm->stack[prompt->stack_base]);

    uint32_t* saved_ip = NULL;
    Chunk* saved_chunk = NULL;
    if (vm->frame_count > prompt->frame_index) {
        CallFrame* first_unwound_frame = &vm->frames[prompt->frame_index];
        saved_ip = first_unwound_frame->ip;
        saved_chunk = first_unwound_frame->caller_chunk;
    }

    unwindFrames(vm, prompt->frame_index);
    // Releases every slot the unwind just abandoned. Through lowerStackTop
    // rather than by assignment: the released slots have to be cleared, or
    // the objects they name are collected and the next call raises the mark
    // back over the dangling pointers. See the note on lowerStackTop.
    lowerStackTop(vm, prompt->stack_base);

    if (saved_ip != NULL) {
        vm->ip = saved_ip;
        vm->chunk = saved_chunk;

        if (vm->chunk == NULL && vm->frame_count > 0) {
            vm->chunk = &vm->frames[vm->frame_count - 1].closure->function->chunk;
        }
    } else if (vm->frame_count > 0) {
        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        vm->ip = frame->ip;
        vm->chunk = frame->caller_chunk ? frame->caller_chunk : &frame->closure->function->chunk;
    }

    popPromptsThrough(vm, prompt_index);

    while (vm->resume_depth > 0 &&
           vm->resume_stack[vm->resume_depth - 1].frame_boundary >= vm->frame_count) {
        ResumeContext* ctx = &vm->resume_stack[--vm->resume_depth];
        vm->active_boundaries--;
        vm->stack[ctx->result_slot] = abort_value;
    }

    if (vm->chunk != NULL && vm->ip > vm->chunk->code) {
        uint32_t prev_instr = *(vm->ip - 1);
        int opcode = prev_instr & 0xFF;

        if (opcode == CALL || opcode == CALL_SELF || opcode == TAIL_CALL ||
            opcode == TAIL_CALL_SELF) {
            int result_reg = (prev_instr >> 8) & 0xFF;
            int frame_base = (vm->frame_count > 0) ? vm->frames[vm->frame_count - 1].stack_base : 0;
            int result_slot = frame_base + result_reg;

            vm->stack[result_slot] = abort_value;
        } else {
            vm->stack[vm->stack_top] = abort_value;
            vm->stack_top++;
        }
    } else {
        vm->stack[vm->stack_top] = abort_value;
        vm->stack_top++;
    }

    return ZYM_CONTROL_TRANSFER;
}

static ZymValue cont_shift(ZymVM* vm, ZymValue context, ZymValue tag_val, ZymValue handler) {
    (void)zym_getNativeData(context);

    if (!IS_PROMPT_TAG(tag_val)) {
        zym_runtimeError(vm, "Cont.shift: first argument must be a prompt tag.");
        return ZYM_ERROR;
    }

    if (!IS_CLOSURE(handler)) {
        zym_runtimeError(vm, "Cont.shift: second argument must be a function.");
        return ZYM_ERROR;
    }

    ObjClosure* handler_closure = AS_CLOSURE(handler);
    ObjFunction* handler_fn = handler_closure->function;

    if (handler_fn->arity != 1) {
        zym_runtimeError(vm, "Cont.shift: handler must take 1 argument (the continuation), got %d.", handler_fn->arity);
        return ZYM_ERROR;
    }

    ObjPromptTag* tag = AS_PROMPT_TAG(tag_val);

    PromptEntry* prompt = findPrompt(vm, tag);
    if (prompt == NULL) {
        zym_runtimeError(vm, "Cont.shift: prompt tag not found.");
        return ZYM_ERROR;
    }
    int prompt_index = (int)(prompt - vm->prompt_stack);

    int return_slot = 0;
    if (vm->chunk != NULL && vm->ip > vm->chunk->code) {
        uint32_t prev_instr = *(vm->ip - 1);
        int opcode = prev_instr & 0xFF;

        if (opcode == CALL || opcode == CALL_SELF || opcode == TAIL_CALL ||
            opcode == TAIL_CALL_SELF) {
            int result_reg = (prev_instr >> 8) & 0xFF;
            int frame_base = (vm->frame_count > 0) ? vm->frames[vm->frame_count - 1].stack_base : 0;
            int absolute_slot = frame_base + result_reg;

            return_slot = absolute_slot - prompt->stack_base;
        }
    }

    ObjContinuation* cont = captureContinuation(vm, tag, return_slot);
    if (cont == NULL) {
        return ZYM_ERROR;
    }

    pushTempRoot(vm, (Obj*)cont);
    pushTempRoot(vm, (Obj*)handler_closure);

    unwindFrames(vm, prompt->frame_index);
    // Releases every slot the unwind just abandoned. Through lowerStackTop
    // rather than by assignment: the released slots have to be cleared, or
    // the objects they name are collected and the next call raises the mark
    // back over the dangling pointers. See the note on lowerStackTop.
    lowerStackTop(vm, prompt->stack_base);

    if (cont->frame_count > 0) {
        CallFrame* captured_frame = &cont->frames[0];
        vm->ip = captured_frame->ip;
        vm->chunk = captured_frame->caller_chunk;

        if (vm->chunk == NULL && vm->frame_count > 0) {
            vm->chunk = &vm->frames[vm->frame_count - 1].closure->function->chunk;
        }
    } else if (vm->frame_count > 0) {
        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        vm->ip = frame->ip;
        vm->chunk = frame->caller_chunk ? frame->caller_chunk : &frame->closure->function->chunk;
    }

    popPromptsThrough(vm, prompt_index);

    // Drop the resume boundaries the unwind above just invalidated. cont_capture
    // and cont_abort have run this loop all along; shift did not, and every
    // non-local exit past a splice point owes it -- a boundary left behind names
    // a frame index that no longer exists, and it fires on whatever unrelated
    // call next returns at that depth.
    //
    // No value is written into their result_slots, which is the one difference
    // from the other two. Capture delivers the continuation there and abort
    // delivers the abort value because for them that write IS the delivery.
    // Shift's value is the handler's return, and the handler frame pushed below
    // is based at callee_slot, so its ordinary RET writes exactly that slot --
    // the same slot this loop would have written, derived from the same call
    // instruction. Writing here too would only put a placeholder in a slot that
    // is about to be overwritten. The deeper entries address unwound frames and
    // now sit above stack_top, so nothing is owed there either.
    //
    // Left un-drained, this was already a silently misrouted return value. It
    // became memory-unsafe once the boundary started carrying a stack_top to
    // reinstate: a stale entry restores a mark from a torn-down layout, and if
    // that mark is below the live registers of the frame that is running when it
    // fires, markRoots stops scanning them and the next allocation collects
    // locals that are still in use.
    while (vm->resume_depth > 0 &&
           vm->resume_stack[vm->resume_depth - 1].frame_boundary >= vm->frame_count) {
        vm->resume_depth--;
        vm->active_boundaries--;
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
        popTempRoot(vm);  // handler_closure
        popTempRoot(vm);  // cont
        zym_runtimeError(vm, "Cont.shift: could not determine call context at prompt boundary.");
        return ZYM_ERROR;
    }

    if (vm->frame_count >= FRAMES_MAX) {
        popTempRoot(vm);  // handler_closure
        popTempRoot(vm);  // cont
        zym_runtimeError(vm, "Cont.shift: stack overflow (max call depth reached).");
        return ZYM_ERROR;
    }

    // Same spill_count omission as withPrompt, and worse here: stack_top was
    // truncated to the prompt base above, so the spill area is guaranteed to
    // land outside the region the GC scans.
    int needed_top = callee_slot + handler_fn->max_regs;
    if (needed_top > STACK_MAX) {
        popTempRoot(vm);
        popTempRoot(vm);
        zym_runtimeError(vm, "Cont.shift: stack overflow.");
        return ZYM_ERROR;
    }
    if (!growStackForCall(vm, needed_top, NULL)) {
        popTempRoot(vm);
        popTempRoot(vm);
        return ZYM_ERROR;
    }

    vm->stack[callee_slot] = handler;
    vm->stack[callee_slot + 1] = OBJ_VAL(cont);

    popTempRoot(vm);
    popTempRoot(vm);

    CallFrame* frame = &vm->frames[vm->frame_count++];
    frame->closure = handler_closure;
    frame->ip = vm->ip;
    frame->stack_base = callee_slot;
    frame->spill_base = frameReserveSpills(vm, handler_fn->spill_count);
    frame->caller_chunk = vm->chunk;
    frame->flags = 0;
    frame->arg_count = 1;    // shift requires arity 1 (the continuation)
    frame->preempt_id = 0;

    vm->current_frame = frame;
    // withPrompt sets this on its own frame push; shift did not, so LOAD_STATE
    // came back with the unwound caller's base instead of the handler's.
    vm->cur_base = callee_slot;
    vm->chunk = &handler_fn->chunk;
    vm->ip = handler_fn->chunk.code;

    if (needed_top > vm->stack_top) {
        vm->stack_top = needed_top;
    }

    return ZYM_CONTROL_TRANSFER;
}

ZymValue nativeCont_create(ZymVM* vm) {
    const ZymAllocator* alloc = zym_getAllocator(vm);
    ContData* data = ZYM_CALLOC((ZymAllocator*)alloc, 1, sizeof(ContData));
    if (!data) {
        zym_runtimeError(vm, "Out of memory");
        return ZYM_ERROR;
    }

    ZymValue context = zym_createNativeContext(vm, data, cont_cleanup);
    zym_pushRoot(vm, context);

    ZymValue newPrompt_0 = zym_createNativeClosure(vm, "newPrompt()", (void*)cont_newPrompt_0, context);
    zym_pushRoot(vm, newPrompt_0);

    ZymValue newPrompt_1 = zym_createNativeClosure(vm, "newPrompt(name)", (void*)cont_newPrompt_1, context);
    zym_pushRoot(vm, newPrompt_1);

    ZymValue isValid = zym_createNativeClosure(vm, "isValid(continuation)", (void*)cont_isValid, context);
    zym_pushRoot(vm, isValid);

    ZymValue isPromptTag = zym_createNativeClosure(vm, "isPromptTag(value)", (void*)cont_isPromptTag, context);
    zym_pushRoot(vm, isPromptTag);

    ZymValue isContinuation = zym_createNativeClosure(vm, "isContinuation(value)", (void*)cont_isContinuation, context);
    zym_pushRoot(vm, isContinuation);

    ZymValue pushPromptClosure = zym_createNativeClosure(vm, "pushPrompt(tag)", (void*)cont_pushPrompt_native, context);
    zym_pushRoot(vm, pushPromptClosure);

    ZymValue popPromptClosure = zym_createNativeClosure(vm, "popPrompt()", (void*)cont_popPrompt_native, context);
    zym_pushRoot(vm, popPromptClosure);

    ZymValue withPrompt = zym_createNativeClosure(vm, "withPrompt(tag, fn)", (void*)cont_withPrompt, context);
    zym_pushRoot(vm, withPrompt);

    ZymValue capture = zym_createNativeClosure(vm, "capture(tag)", (void*)cont_capture, context);
    zym_pushRoot(vm, capture);

    ZymValue resume = zym_createNativeClosure(vm, "resume(continuation, value)", (void*)cont_resume, context);
    zym_pushRoot(vm, resume);

    ZymValue abort_closure = zym_createNativeClosure(vm, "abort(tag, value)", (void*)cont_abort, context);
    zym_pushRoot(vm, abort_closure);

    ZymValue shift_closure = zym_createNativeClosure(vm, "shift(tag, handler)", (void*)cont_shift, context);
    zym_pushRoot(vm, shift_closure);

    ZymValue newPrompt_dispatcher = zym_createDispatcher(vm);
    zym_pushRoot(vm, newPrompt_dispatcher);
    zym_addOverload(vm, newPrompt_dispatcher, newPrompt_0);
    zym_addOverload(vm, newPrompt_dispatcher, newPrompt_1);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "newPrompt", newPrompt_dispatcher);
    zym_mapSet(vm, obj, "isValid", isValid);
    zym_mapSet(vm, obj, "isPromptTag", isPromptTag);
    zym_mapSet(vm, obj, "isContinuation", isContinuation);
    zym_mapSet(vm, obj, "pushPrompt", pushPromptClosure);
    zym_mapSet(vm, obj, "popPrompt", popPromptClosure);
    zym_mapSet(vm, obj, "withPrompt", withPrompt);
    zym_mapSet(vm, obj, "capture", capture);
    zym_mapSet(vm, obj, "resume", resume);
    zym_mapSet(vm, obj, "abort", abort_closure);
    zym_mapSet(vm, obj, "shift", shift_closure);

    for (int i = 0; i < 15; i++) {
        zym_popRoot(vm);
    }

    return obj;
}

void registerContinuationModule(VM* vm) {
    ZymValue contModule = nativeCont_create(vm);
    zym_pushRoot(vm, contModule);

    ObjString* name = copyString(vm, "Cont", 4);
    pushTempRoot(vm, (Obj*)name);
    tableSet(vm, &vm->globals, name, contModule);
    popTempRoot(vm);

    zym_popRoot(vm);
}
