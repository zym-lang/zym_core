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
    return true;
}

void popPrompt(VM* vm) {
    if (vm->prompt_count > 0) {
        vm->prompt_count--;
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

ObjContinuation* captureContinuation(VM* vm, ObjPromptTag* tag, int return_slot) {
    PromptEntry* prompt = findPrompt(vm, tag);
    if (prompt == NULL) {
        runtimeError(vm, "Cannot capture: prompt tag not found.");
        return NULL;
    }

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

    cont->frames = frames_buf;
    cont->frame_count = capture_frame_count;

    cont->stack = stack_buf;
    cont->stack_size = capture_stack_size;

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
    
    int saved_depth = vm->preempt_shield_depth;
    for (int i = capture_frame_count + prompt_frame; i < vm->frame_count; i++) {
        if (vm->frames[i].flags & (FRAME_FLAG_PREEMPT | FRAME_FLAG_DISABLE_PREEMPT)) {
            saved_depth--;
        }
    }
    cont->preempt_shield_depth = saved_depth;

    cont->return_slot = return_slot + (prompt->stack_base - capture_stack_base);

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

    int needed_top = vm->stack_top + cont->stack_size;

    if (needed_top > STACK_MAX) {
        runtimeError(vm, "Stack overflow: resuming continuation needs %d slots, max is %d.", needed_top, STACK_MAX);
        return false;
    }

    // growStackForCall disables the GC across the realloc and NULL-fills slots
    // above the OLD capacity; the hand-rolled GROW_ARRAY this replaced did
    // neither. Note the fill only covers newly added capacity -- slots already
    // within capacity keep whatever they held, same as the CALL opcodes.
    if (!growStackForCall(vm, needed_top, NULL)) {
        return false;
    }

    cont->state = CONT_CONSUMED;

    int restore_base = vm->stack_top;

    // cont->stack is NULL when nothing was captured; memcpy(dst, NULL, 0) is UB.
    if (cont->stack_size > 0) {
        memcpy(&vm->stack[restore_base], cont->stack, cont->stack_size * sizeof(Value));
    }
    vm->stack_top = restore_base + cont->stack_size;

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
    }
    vm->frame_count += cont->frame_count;
    vm->cur_base = vm->frame_count == 0 ? 0 : vm->frames[vm->frame_count - 1].stack_base;
    vm->current_frame = vm->frame_count == 0 ? NULL : &vm->frames[vm->frame_count - 1];

    vm->ip = cont->saved_ip;
    vm->chunk = cont->saved_chunk;
    vm->preempt_shield_depth = cont->preempt_shield_depth;

    int result_slot = restore_base + cont->return_slot;
    vm->stack[result_slot] = resume_value;

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

    // Every other frame-push site sizes the window as max_regs + spill_count
    // (vm.c:581 and the CALL opcodes). Omitting the spill area let SPILL_STORE
    // write above stack_top, which markRoots never scans.
    int needed_top = callee_slot + function->max_regs + function->spill_count;
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

    vm->with_prompt_stack[vm->with_prompt_depth].frame_boundary = vm->frame_count;
    vm->with_prompt_depth++;
    vm->active_boundaries++;

    CallFrame* frame = &vm->frames[vm->frame_count++];
    frame->closure = closure;
    frame->ip = vm->ip;
    frame->stack_base = callee_slot;
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
    vm->stack_top = prompt->stack_base;

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

    popPrompt(vm);

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

    closeUpvalues(vm, &vm->stack[prompt->stack_base]);

    uint32_t* saved_ip = NULL;
    Chunk* saved_chunk = NULL;
    if (vm->frame_count > prompt->frame_index) {
        CallFrame* first_unwound_frame = &vm->frames[prompt->frame_index];
        saved_ip = first_unwound_frame->ip;
        saved_chunk = first_unwound_frame->caller_chunk;
    }

    unwindFrames(vm, prompt->frame_index);
    vm->stack_top = prompt->stack_base;

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

    popPrompt(vm);

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
    vm->stack_top = prompt->stack_base;

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

    popPrompt(vm);

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
    int needed_top = callee_slot + handler_fn->max_regs + handler_fn->spill_count;
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
