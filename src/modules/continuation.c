#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "continuation.h"
#include "../memory.h"
#include "../gc.h"
#include "../zym.h"
#include "../table.h"
#include "../opcode.h"

// ============================================================================
// Prompt Stack Operations
// ============================================================================

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

// ============================================================================
// Continuation Capture
// ============================================================================

ObjContinuation* captureContinuation(VM* vm, ObjPromptTag* tag, int return_slot) {
    PromptEntry* prompt = findPrompt(vm, tag);
    if (prompt == NULL) {
        runtimeError(vm, "Cannot capture: prompt tag not found.");
        return NULL;
    }

    int prompt_frame = prompt->frame_index;
    int capture_frame_count = vm->frame_count - prompt_frame;

    // In a register-based VM, a function's frame can have a stack_base
    // below the prompt's stack_base if the prompt was set before the call
    int capture_stack_base = prompt->stack_base;
    for (int i = prompt_frame; i < vm->frame_count; i++) {
        if (vm->frames[i].stack_base < capture_stack_base) {
            capture_stack_base = vm->frames[i].stack_base;
        }
    }

    int capture_stack_size = vm->stack_top - capture_stack_base;

    closeUpvalues(vm, &vm->stack[capture_stack_base]);

    for (int i = capture_stack_base; i < vm->stack_top; i++) {
        protectLocalRefsInValue(vm, vm->stack[i], &vm->stack[capture_stack_base]);
    }

    ObjContinuation* cont = newContinuation(vm);
    pushTempRoot(vm, (Obj*)cont);

    cont->frame_count = capture_frame_count;
    if (capture_frame_count > 0) {
        cont->frames = ALLOCATE(vm, CallFrame, capture_frame_count);
        memcpy(cont->frames, &vm->frames[prompt_frame],
               capture_frame_count * sizeof(CallFrame));
    }

    cont->stack_size = capture_stack_size;
    if (capture_stack_size > 0) {
        cont->stack = ALLOCATE(vm, Value, capture_stack_size);
        memcpy(cont->stack, &vm->stack[capture_stack_base],
               capture_stack_size * sizeof(Value));
    }

    cont->saved_ip = vm->ip;
    cont->saved_chunk = vm->chunk;
    cont->stack_base_offset = capture_stack_base;

    cont->prompt_tag = tag;
    cont->state = CONT_VALID;

    cont->return_slot = return_slot + (prompt->stack_base - capture_stack_base);

    popTempRoot(vm);
    return cont;
}

// ============================================================================
// Continuation Resume
// ============================================================================

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

    if (needed_top > vm->stack_capacity) {
        int new_capacity = vm->stack_capacity;
        while (new_capacity < needed_top) {
            new_capacity *= 2;
            if (new_capacity > STACK_MAX) {
                new_capacity = STACK_MAX;
                break;
            }
        }

        Value* old_stack = vm->stack;
        vm->stack = GROW_ARRAY(vm, Value, vm->stack, vm->stack_capacity, new_capacity);
        vm->stack_capacity = new_capacity;

        if (old_stack != vm->stack) {
            updateStackReferences(vm, old_stack, vm->stack);
        }
    }

    // Mark consumed only after all precondition checks have passed.
    cont->state = CONT_CONSUMED;

    int restore_base = vm->stack_top;

    memcpy(&vm->stack[restore_base], cont->stack, cont->stack_size * sizeof(Value));
    vm->stack_top = restore_base + cont->stack_size;

    for (int i = 0; i < cont->frame_count; i++) {
        CallFrame* src = &cont->frames[i];
        CallFrame* dst = &vm->frames[vm->frame_count + i];

        dst->closure = src->closure;
        dst->ip = src->ip;
        dst->caller_chunk = src->caller_chunk;

        int original_offset = src->stack_base - cont->stack_base_offset;
        dst->stack_base = restore_base + original_offset;
    }
    vm->frame_count += cont->frame_count;

    vm->ip = cont->saved_ip;
    vm->chunk = cont->saved_chunk;

    int result_slot = restore_base + cont->return_slot;
    vm->stack[result_slot] = resume_value;

    return true;
}

// ============================================================================
// CONT MODULE - NATIVE CLOSURE IMPLEMENTATION
// ============================================================================

typedef struct {
    int dummy;
} ContData;

static void cont_cleanup(ZymVM* vm, void* ptr) {
    (void)vm;
    ContData* data = (ContData*)ptr;
    free(data);
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
    (void)tag;
    (void)fn;
    zym_runtimeError(vm, "Cont.withPrompt not yet implemented - use pushPrompt/popPrompt for manual control.");
    return ZYM_ERROR;
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

    // Decode where the resume value should be placed when resuming.
    // The CALL instruction at (vm->ip - 1) tells us the result register.
    int return_slot = 0;

    if (vm->chunk != NULL && vm->ip > vm->chunk->code) {
        uint32_t prev_instr = *(vm->ip - 1);
        int opcode = prev_instr & 0xFF;

        if (opcode == CALL || opcode == CALL_SELF || opcode == TAIL_CALL ||
            opcode == TAIL_CALL_SELF || opcode == SMART_TAIL_CALL || opcode == SMART_TAIL_CALL_SELF) {
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

    vm->frame_count = prompt->frame_index;
    vm->stack_top = prompt->stack_base;

    if (cont->frame_count > 0) {
        CallFrame* captured_frame = &cont->frames[0];
        vm->ip = captured_frame->ip;
        vm->chunk = captured_frame->caller_chunk;

        if (vm->chunk == NULL && vm->frame_count > 0) {
            vm->chunk = vm->frames[vm->frame_count - 1].closure->function->chunk;
        }
    } else if (vm->frame_count > 0) {
        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        vm->ip = frame->ip;
        vm->chunk = frame->caller_chunk ? frame->caller_chunk : frame->closure->function->chunk;
    }

    popPrompt(vm);

    if (vm->resume_depth > 0) {
        ResumeContext* ctx = &vm->resume_stack[vm->resume_depth - 1];

        if (vm->frame_count == ctx->frame_boundary) {
            vm->stack[ctx->result_slot] = OBJ_VAL(cont);
            vm->resume_depth--;
        }
    }

    if (vm->chunk != NULL && vm->ip > vm->chunk->code) {
        uint32_t prev_instr = *(vm->ip - 1);
        int opcode = prev_instr & 0xFF;

        if (opcode == CALL || opcode == CALL_SELF || opcode == TAIL_CALL ||
            opcode == TAIL_CALL_SELF || opcode == SMART_TAIL_CALL || opcode == SMART_TAIL_CALL_SELF) {
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
            opcode == TAIL_CALL_SELF || opcode == SMART_TAIL_CALL || opcode == SMART_TAIL_CALL_SELF) {
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

    if (!resumeContinuation(vm, cont, value)) {
        vm->resume_depth--;
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

    vm->frame_count = prompt->frame_index;
    vm->stack_top = prompt->stack_base;

    if (saved_ip != NULL) {
        vm->ip = saved_ip;
        vm->chunk = saved_chunk;

        if (vm->chunk == NULL && vm->frame_count > 0) {
            vm->chunk = vm->frames[vm->frame_count - 1].closure->function->chunk;
        }
    } else if (vm->frame_count > 0) {
        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        vm->ip = frame->ip;
        vm->chunk = frame->caller_chunk ? frame->caller_chunk : frame->closure->function->chunk;
    }

    popPrompt(vm);

    if (vm->resume_depth > 0) {
        ResumeContext* ctx = &vm->resume_stack[vm->resume_depth - 1];

        if (vm->frame_count == ctx->frame_boundary) {
            vm->stack[ctx->result_slot] = abort_value;
            vm->resume_depth--;
        }
    }

    if (vm->chunk != NULL && vm->ip > vm->chunk->code) {
        uint32_t prev_instr = *(vm->ip - 1);
        int opcode = prev_instr & 0xFF;

        if (opcode == CALL || opcode == CALL_SELF || opcode == TAIL_CALL ||
            opcode == TAIL_CALL_SELF || opcode == SMART_TAIL_CALL || opcode == SMART_TAIL_CALL_SELF) {
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

// ============================================================================
// Module Factory
// ============================================================================

ZymValue nativeCont_create(ZymVM* vm) {
    ContData* data = calloc(1, sizeof(ContData));
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

    for (int i = 0; i < 14; i++) {
        zym_popRoot(vm);
    }

    return obj;
}

// ============================================================================
// Module Registration
// ============================================================================

void registerContinuationModule(VM* vm) {
    ZymValue contModule = nativeCont_create(vm);
    zym_pushRoot(vm, contModule);

    ObjString* name = copyString(vm, "Cont", 4);
    pushTempRoot(vm, (Obj*)name);
    tableSet(vm, &vm->globals, name, contModule);
    popTempRoot(vm);

    zym_popRoot(vm);
}
