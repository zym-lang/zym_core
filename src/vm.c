#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "./vm.h"
#include "./common.h"
#include "./debug.h"
#include "./compiler.h"
#include "./object.h"
#include "./memory.h"
#include "./ast.h"
#include "./gc.h"
#include "./native.h"
#include "zym/zym.h"
#include "./modules/continuation.h"
#include "./modules/core_modules.h"

static const char* ERR_MAP_KEYS_TYPE = "Map keys must be strings or numbers.";
static const char* ERR_MAP_KEY_TYPE = "Map key must be a string or number.";
static const char* ERR_LIST_INDEX_TYPE = "List index must be a number.";
static const char* ERR_PROPERTY_KEY_TYPE = "Property key must be a string or number.";
static const char* ERR_INDEX_TYPE = "Index must be a string or number.";
static const char* ERR_OPERANDS_NUMBERS = "Operands must be numbers.";
static const char* ERR_ONLY_CALL_FUNCTIONS = "Can only call functions and classes.";
static const char* ERR_UNDEFINED_IDENTIFIER = "Undefined identifier '%.*s'.";
static const char* ERR_ONLY_MAPS = "Can only use dot notation on maps.";
static const char* ERR_ONLY_SUBSCRIPT_LISTS_MAPS = "Can only subscript lists or maps.";
static const char* ERR_INDEX_CONTAINER_NOT_LIST = "Index reference container is not a list.";
static const char* ERR_INDEX_CONTAINER_NOT_MAP = "Index reference container must be a list or map.";
static const char* ERR_PROPERTY_CONTAINER_NOT_MAP = "Property reference container is not a map.";
static const char* ERR_INDEX_CONTAINER_NOT_OBJECT = "Index reference container is not an object.";
static const char* ERR_NESTED_COLLECTION_REFS = "Nested collection references not yet fully supported.";

#define OPCODE(i) ((i) & 0xFF)
#define REG_A(i)  (((i) >> 8) & 0xFF)
#define REG_B(i)  (((i) >> 16) & 0xFF)
#define REG_C(i)  (((i) >> 24) & 0xFF)
#define REG_Bx(i) ((i) >> 16)

void initVM(VM* vm) {
    vm->chunk = NULL;
    vm->ip = NULL;
    vm->frame_count = 0;
    vm->cur_base = 0;
    vm->active_boundaries = 0;
    vm->current_frame = NULL;

    vm->objects = NULL;
    vm->bytes_allocated = 0;
    vm->next_gc = 1024 * 1024;
    vm->gray_count = 0;
    vm->gray_capacity = 0;
    vm->gray_stack = NULL;
    vm->gc_enabled = false;
    vm->compiler = NULL;
    vm->temp_roots = NULL;
    vm->temp_root_count = 0;
    vm->temp_root_capacity = 0;

    vm->stack_capacity = STACK_INITIAL;
    vm->stack = (Value*)reallocate(vm, NULL, 0, sizeof(Value) * vm->stack_capacity);
    vm->stack_top = 0;
    for (int i = 0; i < vm->stack_capacity; i++) vm->stack[i] = NULL_VAL;

    initTable(&vm->globals);
    initValueArray(&vm->globalSlots);
    initTable(&vm->strings);
    vm->open_upvalues = NULL;
    vm->api_stack_top = 0;
    vm->next_enum_type_id = 1;
    vm->entry_file = NULL;

    initChunk(&vm->api_trampoline);
    uint32_t halt = (uint32_t)RET | (0u << 8) | (1u << 16);
    writeInstruction(vm, &vm->api_trampoline, halt, 0);

    vm->prompt_count = 0;
    vm->next_prompt_tag_id = 1;

    vm->yield_budget = DEFAULT_TIMESLICE;
    vm->default_timeslice = DEFAULT_TIMESLICE;
    vm->preempt_requested = false;
    vm->preemption_enabled = false;
    vm->preemption_disable_depth = 0;
    vm->on_preempt_callback = NULL_VAL;

    vm->resume_depth = 0;
    vm->with_prompt_depth = 0;

    vm->error_callback = NULL;
    vm->error_user_data = NULL;

    vm->gc_enabled = true;

    setupCoreModules(vm);
}

void freeVM(VM* vm) {
    vm->gc_enabled = false;

    freeTable(vm, &vm->globals);
    freeValueArray(vm, &vm->globalSlots);
    freeTable(vm, &vm->strings);
    freeChunk(vm, &vm->api_trampoline);

    Obj* object = vm->objects;
    while (object != NULL) {
        Obj* next = object->next;

        if (object->type < 0 || object->type > OBJ_CONTINUATION) {
            fprintf(stderr, "ERROR: Corrupted object detected at %p with invalid type %d during VM cleanup\n",
                    (void*)object, object->type);
            fprintf(stderr, "Stopping cleanup to prevent cascading corruption. This indicates a memory management bug.\n");
            break;
        }

        freeObject(vm, object);
        object = next;
    }

    ZYM_FREE(&vm->allocator, vm->gray_stack, sizeof(Obj*) * vm->gray_capacity);
    ZYM_FREE(&vm->allocator, vm->temp_roots, sizeof(Obj*) * vm->temp_root_capacity);

    reallocate(vm, vm->stack, sizeof(Value) * vm->stack_capacity, 0);
    vm->stack = NULL;
    vm->stack_capacity = 0;
    vm->stack_top = 0;
}

bool globalGet(VM* vm, ObjString* name, Value* out_value) {
    Value slot_or_value;
    if (!tableGet(&vm->globals, name, &slot_or_value)) {
        return false;
    }
    if (IS_DOUBLE(slot_or_value)) {
        int slot_index = (int)AS_DOUBLE(slot_or_value);
        *out_value = vm->globalSlots.values[slot_index];
    } else {
        *out_value = slot_or_value;
    }
    return true;
}

bool globalSet(VM* vm, ObjString* name, Value value) {
    Value slot_or_value;
    if (!tableGet(&vm->globals, name, &slot_or_value)) {
        return false;
    }
    if (IS_DOUBLE(slot_or_value)) {
        int slot_index = (int)AS_DOUBLE(slot_or_value);
        vm->globalSlots.values[slot_index] = value;
    } else {
        return false;
    }
    return true;
}

static int line_at_ip(Chunk* chunk, uint32_t* ip) {
    if (!chunk || !chunk->code || chunk->count <= 0 || !ip) return -1;
    ptrdiff_t idx = (ip - chunk->code) - 1;
    if (idx < 0) idx = 0;
    if (idx >= (ptrdiff_t)chunk->count) idx = (ptrdiff_t)chunk->count - 1;
    return chunk->lines[(int)idx];
}

void runtimeError(VM* vm, const char* format, ...) {
    // Format the error message
    char msg_buf[1024];
    va_list args;
    va_start(args, format);
    int msg_len = vsnprintf(msg_buf, sizeof(msg_buf), format, args);
    va_end(args);
    if (msg_len < 0) msg_len = 0;

    // Determine file and line for the error location
    const char* err_file = NULL;
    int err_line = -1;

    if (vm->frame_count > 0) {
        CallFrame* cur = &vm->frames[vm->frame_count - 1];
        Chunk* cur_chunk = cur->closure->function ? &cur->closure->function->chunk : vm->chunk;
        err_line = line_at_ip(cur_chunk, vm->ip);
        if (cur->closure->function && cur->closure->function->module_name) {
            err_file = cur->closure->function->module_name->chars;
        } else if (vm->entry_file) {
            err_file = vm->entry_file->chars;
        }
    } else {
        err_line = line_at_ip(vm->chunk, vm->ip);
        if (vm->entry_file) {
            err_file = vm->entry_file->chars;
        }
    }

    if (vm->error_callback) {
        // Build the full message with location and stack trace into a buffer
        char full_buf[4096];
        int pos = 0;

        // Error message
        pos += snprintf(full_buf + pos, sizeof(full_buf) - pos, "%s\n", msg_buf);

        // Location
        if (err_file) {
            pos += snprintf(full_buf + pos, sizeof(full_buf) - pos, "[%s] line %d\n", err_file, err_line);
        } else {
            pos += snprintf(full_buf + pos, sizeof(full_buf) - pos, "[line %d]\n", err_line);
        }

        // Stack trace
        for (int i = (int)vm->frame_count - 1; i >= 0 && pos < (int)sizeof(full_buf) - 1; --i) {
            CallFrame* f = &vm->frames[i];
            Chunk* caller_chunk = f->caller_chunk ? f->caller_chunk : vm->chunk;
            int call_line = line_at_ip(caller_chunk, f->ip);

            const char* call_file = NULL;
            const char* caller_name = "<script>";
            int caller_len = 8;

            if (i > 0) {
                ObjFunction* caller_fn = vm->frames[i - 1].closure->function;
                if (caller_fn && caller_fn->name) {
                    caller_name = caller_fn->name->chars;
                    caller_len  = caller_fn->name->length;
                    if (caller_len > 9 && memcmp(caller_name, "__module_", 9) == 0) {
                        caller_name = "<script>";
                        caller_len = 8;
                    }
                }
                if (caller_fn && caller_fn->module_name) {
                    call_file = caller_fn->module_name->chars;
                }
            }

            pos += snprintf(full_buf + pos, sizeof(full_buf) - pos, "    at ");
            if (call_file) {
                pos += snprintf(full_buf + pos, sizeof(full_buf) - pos, "[%s] line %d", call_file, call_line);
            } else if (vm->entry_file) {
                pos += snprintf(full_buf + pos, sizeof(full_buf) - pos, "[%s] line %d", vm->entry_file->chars, call_line);
            } else {
                pos += snprintf(full_buf + pos, sizeof(full_buf) - pos, "[line %d]", call_line);
            }
            pos += snprintf(full_buf + pos, sizeof(full_buf) - pos, " (called from %.*s)\n", caller_len, caller_name);
        }

        vm->error_callback(vm, ZYM_STATUS_RUNTIME_ERROR, err_file, err_line, full_buf, vm->error_user_data);
    } else {
        // Default: write to stderr (original behavior)
        fprintf(stderr, "%s\n", msg_buf);

        if (err_file) {
            fprintf(stderr, "[%s] line %d\n", err_file, err_line);
        } else {
            fprintf(stderr, "[line %d]\n", err_line);
        }

        for (int i = (int)vm->frame_count - 1; i >= 0; --i) {
            CallFrame* f = &vm->frames[i];
            Chunk* caller_chunk = f->caller_chunk ? f->caller_chunk : vm->chunk;
            int call_line = line_at_ip(caller_chunk, f->ip);

            const char* call_file = NULL;
            const char* caller_name = "<script>";
            int caller_len = 8;

            if (i > 0) {
                ObjFunction* caller_fn = vm->frames[i - 1].closure->function;
                if (caller_fn && caller_fn->name) {
                    caller_name = caller_fn->name->chars;
                    caller_len  = caller_fn->name->length;
                    if (caller_len > 9 && memcmp(caller_name, "__module_", 9) == 0) {
                        caller_name = "<script>";
                        caller_len = 8;
                    }
                }
                if (caller_fn && caller_fn->module_name) {
                    call_file = caller_fn->module_name->chars;
                }
            }

            fprintf(stderr, "    at ");
            if (call_file) {
                fprintf(stderr, "[%s] line %d", call_file, call_line);
            } else if (vm->entry_file) {
                fprintf(stderr, "[%s] line %d", vm->entry_file->chars, call_line);
            } else {
                fprintf(stderr, "[line %d]", call_line);
            }
            fprintf(stderr, " (called from %.*s)\n", caller_len, caller_name);
        }
    }
}

static inline int32_t sign_extend_16(uint32_t x) {
    return (int32_t)((int32_t)(x << 16) >> 16);
}

static inline int32_t sign_extend_8(uint32_t x) {
    return (int32_t)((int32_t)(x << 24) >> 24);
}

static const char* getEnumNameByTypeId(VM* vm, int type_id, int* out_len) {
    for (int i = 0; i < vm->globals.capacity; i++) {
        Entry* entry = &vm->globals.entries[i];
        if (entry->key != NULL && IS_OBJ(entry->value) && IS_ENUM_SCHEMA(entry->value)) {
            ObjEnumSchema* schema = AS_ENUM_SCHEMA(entry->value);
            if (schema->type_id == type_id) {
                *out_len = schema->name->length;
                return schema->name->chars;
            }
        }
    }
    *out_len = 0;
    return NULL;
}

static inline bool value_equals(Value x, Value y) {
    if (x == y) return true;
    if (IS_DOUBLE(x) && IS_DOUBLE(y)) {
        return AS_DOUBLE(x) == AS_DOUBLE(y);
    }
    if (IS_ENUM(x) && IS_ENUM(y)) {
        return false;
    }
    if (IS_ENUM(x) || IS_ENUM(y)) {
        return false;
    }
    return false;
}

static ObjUpvalue* captureUpvalue(VM* vm, Value* local) {
    ObjUpvalue* prevUpvalue = NULL;
    ObjUpvalue* upvalue = vm->open_upvalues;
    while (upvalue != NULL && upvalue->location > local) {
        prevUpvalue = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    ObjUpvalue* createdUpvalue = (ObjUpvalue*)allocateObject(vm, sizeof(ObjUpvalue), OBJ_UPVALUE);

    createdUpvalue->location = local;
    createdUpvalue->closed = NULL_VAL;
    createdUpvalue->next = upvalue;

    if (prevUpvalue == NULL) {
        vm->open_upvalues = createdUpvalue;
    } else {
        prevUpvalue->next = createdUpvalue;
    }

    return createdUpvalue;
}

void updateStackReferences(VM* vm, Value* old_stack, Value* new_stack) {
    if (old_stack == new_stack) return;

    ptrdiff_t offset = new_stack - old_stack;

    for (ObjUpvalue* upvalue = vm->open_upvalues; upvalue != NULL; upvalue = upvalue->next) {
        if (upvalue->location >= old_stack &&
            upvalue->location < old_stack + vm->stack_capacity) {
            upvalue->location += offset;
        }
    }
}

void closeUpvalues(VM* vm, Value* last) {
    #define MAX_CLOSING_UPVALUES 256
    struct {
        ObjUpvalue* upvalue;
        Value* old_location;
    } closing[MAX_CLOSING_UPVALUES];
    int closing_count = 0;

    while (vm->open_upvalues != NULL && vm->open_upvalues->location >= last) {
        ObjUpvalue* upvalue = vm->open_upvalues;
        Value* old_location = upvalue->location;

        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;

        if (closing_count < MAX_CLOSING_UPVALUES) {
            closing[closing_count].upvalue = upvalue;
            closing[closing_count].old_location = old_location;
            closing_count++;
        }

        vm->open_upvalues = upvalue->next;
    }
    #undef MAX_CLOSING_UPVALUES
}

void unwindFrames(VM* vm, int new_frame_count) {
    while (vm->frame_count > new_frame_count) {
        CallFrame* frame = &vm->frames[--vm->frame_count];
        if (frame->flags & (FRAME_FLAG_PREEMPT | FRAME_FLAG_DISABLE_PREEMPT)) {
            vm->preemption_disable_depth--;
        }
    }
    vm->cur_base = vm->frame_count == 0 ? 0 : vm->frames[vm->frame_count - 1].stack_base;
    vm->current_frame = vm->frame_count == 0 ? NULL : &vm->frames[vm->frame_count - 1];

    while (vm->with_prompt_depth > 0 &&
           vm->with_prompt_stack[vm->with_prompt_depth - 1].frame_boundary >= vm->frame_count) {
        vm->with_prompt_depth--;
        vm->active_boundaries--;
    }
}

static bool validateUpvalue(VM* vm, ObjUpvalue* upvalue, const char* context) {
    if (upvalue == NULL || upvalue->location == NULL) {
        runtimeError(vm, "Invalid upvalue reference in %s.", context);
        return false;
    }
    return true;
}

static bool validateListIndex(VM* vm, ObjList* list, int idx, const char* context) {
    if (idx < 0 || idx >= list->items.count) {
        runtimeError(vm, "List index %d out of bounds in %s.", idx, context);
        return false;
    }
    return true;
}

static ObjString* keyToString(VM* vm, Value key_val) {
    if (IS_STRING(key_val)) {
        return AS_STRING(key_val);
    } else if (IS_DOUBLE(key_val)) {
        char buffer[64];
        double num = AS_DOUBLE(key_val);
        if (num == (long long)num && num >= -1e15 && num <= 1e15) {
            snprintf(buffer, sizeof(buffer), "%.0f", num);
        } else {
            snprintf(buffer, sizeof(buffer), "%g", num);
        }
        return copyString(vm, buffer, strlen(buffer));
    }
    return NULL;
}

static Value resolveOverload(VM* vm, ObjDispatcher* dispatcher, uint16_t arg_count) {
    for (int i = 0; i < dispatcher->count; i++) {
        Obj* overload = dispatcher->overloads[i];
        int arity = -1;

        if (overload->type == OBJ_CLOSURE) {
            arity = ((ObjClosure*)overload)->function->arity;
        } else if (overload->type == OBJ_NATIVE_CLOSURE) {
            arity = ((ObjNativeClosure*)overload)->arity;
        }

        if (arity == arg_count) {
            return OBJ_VAL(overload);
        }
    }
    return NULL_VAL;
}

static bool growStackForCall(VM* vm, int needed_top, Value** old_stack_out) {
    if (needed_top <= vm->stack_capacity) {
        return true;
    }

    if (needed_top > STACK_MAX) {
        runtimeError(vm, "Stack overflow: function needs %d slots, max is %d.", needed_top, STACK_MAX);
        return false;
    }

    int new_capacity = vm->stack_capacity;
    while (new_capacity < needed_top) {
        new_capacity *= 2;
        if (new_capacity > STACK_MAX) {
            new_capacity = STACK_MAX;
            break;
        }
    }

    Value* old_stack = vm->stack;

    bool gc_was_enabled = vm->gc_enabled;
    vm->gc_enabled = false;

    Value* new_stack = (Value*)reallocate(vm, vm->stack,
        sizeof(Value) * vm->stack_capacity,
        sizeof(Value) * new_capacity);

    for (int i = vm->stack_capacity; i < new_capacity; i++) {
        new_stack[i] = NULL_VAL;
    }

    vm->stack = new_stack;
    vm->stack_capacity = new_capacity;

    updateStackReferences(vm, old_stack, new_stack);

    vm->gc_enabled = gc_was_enabled;

    if (old_stack_out) *old_stack_out = old_stack;
    return true;
}

static bool pushPreemptFrame(VM* vm) {
    if (IS_NULL(vm->on_preempt_callback) || !IS_CLOSURE(vm->on_preempt_callback)) {
        return false;
    }

    ObjClosure* closure = AS_CLOSURE(vm->on_preempt_callback);
    ObjFunction* function = closure->function;

    if (vm->frame_count == FRAMES_MAX) {
        return false;
    }

    if (function->arity != 0) {
        return false;
    }

    int callee_slot = vm->stack_top;
    int needed_top = callee_slot + function->max_regs;

    if (!growStackForCall(vm, needed_top, NULL)) {
        return false;
    }

    vm->stack[callee_slot] = vm->on_preempt_callback;
    vm->stack_top = needed_top;

    CallFrame* frame = &vm->frames[vm->frame_count++];
    frame->closure      = closure;
    frame->ip           = vm->ip;
    frame->stack_base   = callee_slot;
    frame->caller_chunk = vm->chunk;
    frame->flags        = FRAME_FLAG_PREEMPT;

    vm->current_frame = frame;
    vm->cur_base = callee_slot;
    vm->chunk = &function->chunk;
    vm->ip = function->chunk.code;

    vm->preemption_disable_depth++;

    return true;
}

// --- The Core Execution Loop ---
static InterpretResult run(VM* vm) {
#define JUMP_ENTRY(op) [op] = &&CASE_##op
    static void* dispatch_table[] = {
        JUMP_ENTRY(MOVE),
        JUMP_ENTRY(LOAD_CONST),
        JUMP_ENTRY(ADD),
        JUMP_ENTRY(SUB),
        JUMP_ENTRY(MUL),
        JUMP_ENTRY(DIV),
        JUMP_ENTRY(MOD),
        JUMP_ENTRY(ADD_I),
        JUMP_ENTRY(SUB_I),
        JUMP_ENTRY(MUL_I),
        JUMP_ENTRY(DIV_I),
        JUMP_ENTRY(MOD_I),
        JUMP_ENTRY(ADD_L),
        JUMP_ENTRY(SUB_L),
        JUMP_ENTRY(MUL_L),
        JUMP_ENTRY(DIV_L),
        JUMP_ENTRY(MOD_L),
        JUMP_ENTRY(BAND),
        JUMP_ENTRY(BOR),
        JUMP_ENTRY(BXOR),
        JUMP_ENTRY(BLSHIFT),
        JUMP_ENTRY(BRSHIFT_U),
        JUMP_ENTRY(BRSHIFT_I),
        JUMP_ENTRY(BAND_I),
        JUMP_ENTRY(BOR_I),
        JUMP_ENTRY(BXOR_I),
        JUMP_ENTRY(BLSHIFT_I),
        JUMP_ENTRY(BRSHIFT_U_I),
        JUMP_ENTRY(BRSHIFT_I_I),
        JUMP_ENTRY(BAND_L),
        JUMP_ENTRY(BOR_L),
        JUMP_ENTRY(BXOR_L),
        JUMP_ENTRY(BLSHIFT_L),
        JUMP_ENTRY(BRSHIFT_U_L),
        JUMP_ENTRY(BRSHIFT_I_L),
        JUMP_ENTRY(NEG),
        JUMP_ENTRY(NOT),
        JUMP_ENTRY(BNOT),
        JUMP_ENTRY(EQ),
        JUMP_ENTRY(GT),
        JUMP_ENTRY(LT),
        JUMP_ENTRY(NE),
        JUMP_ENTRY(LE),
        JUMP_ENTRY(GE),
        JUMP_ENTRY(EQ_I),
        JUMP_ENTRY(GT_I),
        JUMP_ENTRY(LT_I),
        JUMP_ENTRY(NE_I),
        JUMP_ENTRY(LE_I),
        JUMP_ENTRY(GE_I),
        JUMP_ENTRY(EQ_L),
        JUMP_ENTRY(GT_L),
        JUMP_ENTRY(LT_L),
        JUMP_ENTRY(NE_L),
        JUMP_ENTRY(LE_L),
        JUMP_ENTRY(GE_L),
        JUMP_ENTRY(JUMP_IF_FALSE),
        JUMP_ENTRY(JUMP_IF_TRUE),
        JUMP_ENTRY(JUMP),
        JUMP_ENTRY(BRANCH_EQ),
        JUMP_ENTRY(BRANCH_NE),
        JUMP_ENTRY(BRANCH_LT),
        JUMP_ENTRY(BRANCH_LE),
        JUMP_ENTRY(BRANCH_GT),
        JUMP_ENTRY(BRANCH_GE),
        JUMP_ENTRY(BRANCH_EQ_I),
        JUMP_ENTRY(BRANCH_NE_I),
        JUMP_ENTRY(BRANCH_LT_I),
        JUMP_ENTRY(BRANCH_LE_I),
        JUMP_ENTRY(BRANCH_GT_I),
        JUMP_ENTRY(BRANCH_GE_I),
        JUMP_ENTRY(BRANCH_EQ_L),
        JUMP_ENTRY(BRANCH_NE_L),
        JUMP_ENTRY(BRANCH_LT_L),
        JUMP_ENTRY(BRANCH_LE_L),
        JUMP_ENTRY(BRANCH_GT_L),
        JUMP_ENTRY(BRANCH_GE_L),
        JUMP_ENTRY(CALL),
        JUMP_ENTRY(CALL_SELF),
        JUMP_ENTRY(TAIL_CALL),
        JUMP_ENTRY(TAIL_CALL_SELF),
        JUMP_ENTRY(RET),
        JUMP_ENTRY(DEFINE_GLOBAL),
        JUMP_ENTRY(GET_GLOBAL),
        JUMP_ENTRY(GET_GLOBAL_CACHED),
        JUMP_ENTRY(SET_GLOBAL),
        JUMP_ENTRY(SET_GLOBAL_CACHED),
        JUMP_ENTRY(CLOSURE),
        JUMP_ENTRY(GET_UPVALUE),
        JUMP_ENTRY(SET_UPVALUE),
        JUMP_ENTRY(CLOSE_UPVALUE),
        JUMP_ENTRY(CLOSE_FRAME_UPVALUES),
        JUMP_ENTRY(NEW_LIST),
        JUMP_ENTRY(LIST_APPEND),
        JUMP_ENTRY(LIST_SPREAD),
        JUMP_ENTRY(GET_SUBSCRIPT),
        JUMP_ENTRY(GET_SUBSCRIPT_I),
        JUMP_ENTRY(SET_SUBSCRIPT),
        JUMP_ENTRY(SET_SUBSCRIPT_I),
        JUMP_ENTRY(NEW_MAP),
        JUMP_ENTRY(MAP_SET),
        JUMP_ENTRY(MAP_SPREAD),
        //JUMP_ENTRY(GET_MAP_PROPERTY),
        //JUMP_ENTRY(SET_MAP_PROPERTY),
        JUMP_ENTRY(GET_MAP_PROPERTY_L),
        JUMP_ENTRY(SET_MAP_PROPERTY_L),
        JUMP_ENTRY(GET_STRUCT_FIELD_IC),
        JUMP_ENTRY(SET_STRUCT_FIELD_IC),
        JUMP_ENTRY(NEW_DISPATCHER),
        JUMP_ENTRY(ADD_OVERLOAD),
        JUMP_ENTRY(NEW_STRUCT),
        JUMP_ENTRY(STRUCT_SPREAD),
        JUMP_ENTRY(GET_STRUCT_FIELD),
        JUMP_ENTRY(SET_STRUCT_FIELD),
        JUMP_ENTRY(PRE_INC),
        JUMP_ENTRY(POST_INC),
        JUMP_ENTRY(PRE_DEC),
        JUMP_ENTRY(POST_DEC),
    };
#undef JUMP_ENTRY

    // Cached hot VM state in locals for register allocation.
    // These avoid repeated pointer-chase through vm-> on every opcode.
    register uint32_t* ip = vm->ip;
    register Value* stack = vm->stack;
    register int base = vm->cur_base;

    // Sync locals back to VM struct before calls that read vm->ip/cur_base
#define STORE_IP()    (vm->ip = ip)
#define STORE_STATE() do { vm->ip = ip; vm->cur_base = base; } while(0)
    // Reload locals from VM struct after frame changes or stack reallocation
#define LOAD_STATE()  do { ip = vm->ip; stack = vm->stack; base = vm->cur_base; } while(0)

#define OP(c) CASE_##c:
#define DISPATCH() do { \
    if (vm->preemption_enabled && vm->preemption_disable_depth == 0 && (--vm->yield_budget <= 0 || vm->preempt_requested)) { \
        vm->yield_budget = vm->default_timeslice; \
        vm->preempt_requested = false; \
        STORE_STATE(); \
        if (IS_CLOSURE(vm->on_preempt_callback)) { \
            if (pushPreemptFrame(vm)) { \
                LOAD_STATE(); \
                goto *dispatch_table[OPCODE(*ip++)]; \
            } \
        } \
        return INTERPRET_YIELD; \
    } \
    goto *dispatch_table[OPCODE(*ip++)]; \
} while(0)
#define CUR_BASE() (base)
#define BINARY_OP(op) \
    do { \
        uint32_t instr = ip[-1]; \
        int a = CUR_BASE() + REG_A(instr); \
        int b = CUR_BASE() + REG_B(instr); \
        int c = CUR_BASE() + REG_C(instr); \
        Value vb = stack[b]; \
        Value vc = stack[c]; \
        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) { \
            stack[a] = DOUBLE_VAL(AS_DOUBLE(vb) op AS_DOUBLE(vc)); \
        } else { \
            STORE_IP(); \
            runtimeError(vm, ERR_OPERANDS_NUMBERS); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
    } while(false)
#define BINARY_COMPARE(op) \
    do { \
        uint32_t instr = ip[-1]; \
        int a = CUR_BASE() + REG_A(instr); \
        int b = CUR_BASE() + REG_B(instr); \
        int c = CUR_BASE() + REG_C(instr); \
        Value vb = stack[b]; \
        Value vc = stack[c]; \
        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) { \
            stack[a] = BOOL_VAL(AS_DOUBLE(vb) op AS_DOUBLE(vc)); \
        } else { \
            STORE_IP(); \
            runtimeError(vm, "Operands must be numbers for comparison."); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
    } while(false)

    #define CHECK_IP_BOUNDS() \
    do { \
        if (!vm->chunk || !vm->chunk->code) { \
            fprintf(stderr, "IP bounds check failed: no current chunk\n"); \
            abort(); \
        } \
        uint32_t* code_base = vm->chunk->code; \
        uint32_t* code_end  = vm->chunk->code + vm->chunk->count; \
        if (ip < code_base || ip > code_end) { \
            fprintf(stderr, "IP out of range: ip=%p, base=%p, end=%p\n", \
            (void*)ip, (void*)code_base, (void*)code_end); \
            abort(); \
        } \
    } while(0)

    // Start execution.
    CHECK_IP_BOUNDS();
    DISPATCH();
    OP(MOVE) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        stack[a] = stack[b];
        DISPATCH();
    }
    OP(LOAD_CONST) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        stack[a] = currentChunk(vm)->constants.values[bx];
        DISPATCH();
    }
    OP(ADD) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value val_b = stack[b];
        Value val_c = stack[c];


        if (IS_DOUBLE(val_b) && IS_DOUBLE(val_c)) {
            stack[a] = DOUBLE_VAL(AS_DOUBLE(val_b) + AS_DOUBLE(val_c));
        } else if (IS_STRING(val_b) && IS_STRING(val_c)) {
            ObjString* str_b = AS_STRING(val_b);
            ObjString* str_c = AS_STRING(val_c);

            int length = str_b->length + str_c->length;
            char* chars = (char*)reallocate(vm, NULL, 0, length + 1);
            memcpy(chars, str_b->chars, str_b->length);
            memcpy(chars + str_b->length, str_c->chars, str_c->length);
            chars[length] = '\0';

            // takeString takes ownership of the 'chars' buffer
            ObjString* result = takeString(vm, chars, length);
            stack = vm->stack; // GC may have reallocated stack

            // Protect the string before the write (which can trigger GC via tableSet)
            pushTempRoot(vm, (Obj*)result);
            stack[a] = OBJ_VAL(result);
            popTempRoot(vm);

        } else {
            STORE_IP(); runtimeError(vm, "Operands for '+' must be two numbers or two strings.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(SUB) {
        BINARY_OP(-);
        DISPATCH();
    }
    OP(MUL) {
        BINARY_OP(*);
        DISPATCH();
    }
    OP(DIV) {
        BINARY_OP(/);
        DISPATCH();
    }
    OP(MOD) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = stack[b];
        Value vc = stack[c];


        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            double rhs = AS_DOUBLE(vc);
            if (rhs == 0.0) {
                STORE_IP(); runtimeError(vm, "Division by zero in '%%'.");
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            stack[a] = DOUBLE_VAL(fmod(AS_DOUBLE(vb), rhs));
        } else {
            STORE_IP(); runtimeError(vm, "Operands for '%%' must be numbers.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(EQ) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = stack[b];
        Value vc = stack[c];


        // Special handling for enum type checking
        if (IS_ENUM(vb) && IS_ENUM(vc)) {
            int type_b = ENUM_TYPE_ID(vb);
            int type_c = ENUM_TYPE_ID(vc);
            if (type_b != type_c) {
                int len_b, len_c;
                const char* name_b = getEnumNameByTypeId(vm, type_b, &len_b);
                const char* name_c = getEnumNameByTypeId(vm, type_c, &len_c);
                if (name_b && name_c) {
                    STORE_IP(); runtimeError(vm, "Cannot compare enum '%.*s' with enum '%.*s'",
                                len_b, name_b, len_c, name_c);
                } else {
                    STORE_IP(); runtimeError(vm, "Cannot compare enum values of different types (type IDs: %d vs %d)", type_b, type_c);
                }
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
        }

        stack[a] = BOOL_VAL(value_equals(vb, vc));
        DISPATCH();
    }
    OP(GT) {
        BINARY_COMPARE(>);
        DISPATCH();
    }
    OP(LT) {
        BINARY_COMPARE(<);
        DISPATCH();
    }
    OP(NE) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = stack[b];
        Value vc = stack[c];


        stack[a] = BOOL_VAL(!value_equals(vb, vc));
        DISPATCH();
    }
    OP(LE) { // Ra = (Rb <= Rc)
        BINARY_COMPARE(<=);
        DISPATCH();
    }
    OP(GE) { // Ra = (Rb >= Rc)
        BINARY_COMPARE(>=);
        DISPATCH();
    }

    // ===== Comparison with 16-bit Immediate =====
    OP(EQ_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];

        Value imm_val = DOUBLE_VAL((double)imm);
        bool result = (va == imm_val);
        if (!result) {
            if (imm == 0) {
                result = (va == 0) || (va == NULL_VAL) || (va == FALSE_VAL);
            } else if (IS_BOOL(va)) {
                result = (AS_BOOL(va) == (imm != 0));
            }
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(GT_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        bool result = false;
        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            result = (AS_DOUBLE(va) > (double)imm);
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(LT_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        bool result = false;
        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            result = (AS_DOUBLE(va) < (double)imm);
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(NE_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];

        Value imm_val = DOUBLE_VAL((double)imm);
        bool result = (va != imm_val);
        if (result) {
            if (imm == 0) {
                result = (va != 0) && (va != NULL_VAL) && (va != FALSE_VAL);
            } else if (IS_BOOL(va)) {
                result = (AS_BOOL(va) != (imm != 0));
            }
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(LE_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        bool result = false;
        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            result = (AS_DOUBLE(va) <= (double)imm);
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(GE_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        bool result = false;
        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            result = (AS_DOUBLE(va) >= (double)imm);
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }

    // ===== Comparison with 64-bit Literal =====
    OP(EQ_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        Value literal_val = ((uint64_t)high << 32) | (uint64_t)low;

        Value vb = stack[b];

        bool result = (vb == literal_val);
        if (!result) {
            if (literal_val == 0) {
                result = (vb == 0) || (vb == NULL_VAL) || (vb == FALSE_VAL);
            } else if (IS_DOUBLE(vb) && IS_DOUBLE(literal_val)) {
                result = (AS_DOUBLE(vb) == AS_DOUBLE(literal_val));
            } else if (IS_BOOL(vb)) {
                double literal; memcpy(&literal, &literal_val, sizeof(double));
                result = (AS_BOOL(vb) == (literal != 0.0));
            }
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(GT_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        bool result = false;
        if (__builtin_expect(IS_DOUBLE(vb), 1)) {
            result = (AS_DOUBLE(vb) > literal);
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(LT_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        bool result = false;
        if (__builtin_expect(IS_DOUBLE(vb), 1)) {
            result = (AS_DOUBLE(vb) < literal);
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(NE_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        Value literal_val = ((uint64_t)high << 32) | (uint64_t)low;

        Value vb = stack[b];

        bool result = (vb != literal_val);
        if (result) {
            if (literal_val == 0) {
                result = (vb != 0) && (vb != NULL_VAL) && (vb != FALSE_VAL);
            } else if (IS_DOUBLE(vb) && IS_DOUBLE(literal_val)) {
                result = (AS_DOUBLE(vb) != AS_DOUBLE(literal_val));
            } else if (IS_BOOL(vb)) {
                double literal; memcpy(&literal, &literal_val, sizeof(double));
                result = (AS_BOOL(vb) != (literal != 0.0));
            }
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(LE_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        bool result = false;
        if (__builtin_expect(IS_DOUBLE(vb), 1)) {
            result = (AS_DOUBLE(vb) <= literal);
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(GE_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        bool result = false;
        if (__builtin_expect(IS_DOUBLE(vb), 1)) {
            result = (AS_DOUBLE(vb) >= literal);
        }

        stack[a] = BOOL_VAL(result);
        DISPATCH();
    }

    OP(NOT) { // Ra = !Rb    (false/null/0 => true, everything else => false)
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(ip[-1]);
        int b = CUR_BASE() + REG_B(ip[-1]);
        Value v = stack[b];


        bool is_falsey = (v == 0) || (v == NULL_VAL) || (v == FALSE_VAL);
        stack[a] = BOOL_VAL(is_falsey);
        DISPATCH();
    }
    OP(BAND) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = stack[b];
        Value vc = stack[c];


        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            int32_t result = lhs & rhs;
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operands for '&' must be numbers.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BOR) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = stack[b];
        Value vc = stack[c];


        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            int32_t result = lhs | rhs;
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operands for '|' must be numbers.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BXOR) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = stack[b];
        Value vc = stack[c];


        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            int32_t result = lhs ^ rhs;
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operands for '^' must be numbers.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BLSHIFT) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = stack[b];
        Value vc = stack[c];


        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            // Mask shift amount to 0-31 for i32
            int32_t result = lhs << (rhs & 0x1F);
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operands for '<<' must be numbers.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_U) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = stack[b];
        Value vc = stack[c];


        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            // JavaScript behavior: convert to uint32, logical shift with 0-31 mask
            uint32_t lhs = (uint32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            // Mask shift amount to 0-31 for i32
            uint32_t result = lhs >> (rhs & 0x1F);
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operands for '>>>' must be numbers.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = stack[b];
        Value vc = stack[c];


        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);  // Signed for arithmetic shift
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            // Mask shift amount to 0-31 for 32-bit signed
            int32_t result = lhs >> (rhs & 0x1F);
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operands for '>>' must be numbers.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Arithmetic with 16-bit Immediate =====
    OP(ADD_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        // Sign-extend 16-bit immediate
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        if (IS_DOUBLE(va)) {
            stack[a] = DOUBLE_VAL(AS_DOUBLE(va) + (double)imm);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '+' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(SUB_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        if (IS_DOUBLE(va)) {
            stack[a] = DOUBLE_VAL(AS_DOUBLE(va) - (double)imm);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '-' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(MUL_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        if (IS_DOUBLE(va)) {
            stack[a] = DOUBLE_VAL(AS_DOUBLE(va) * (double)imm);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '*' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(DIV_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        if (IS_DOUBLE(va)) {
            stack[a] = DOUBLE_VAL(AS_DOUBLE(va) / (double)imm);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '/' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(MOD_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        if (IS_DOUBLE(va)) {
            double rhs = (double)imm;
            if (rhs == 0.0) {
                STORE_IP(); runtimeError(vm, "Division by zero in '%%'.");
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            stack[a] = DOUBLE_VAL(fmod(AS_DOUBLE(va), rhs));
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '%%' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Arithmetic with 64-bit Literal =====
    OP(ADD_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        // Read 64-bit literal from next two instructions
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        if (IS_DOUBLE(vb)) {
            stack[a] = DOUBLE_VAL(AS_DOUBLE(vb) + literal);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '+' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(SUB_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        if (IS_DOUBLE(vb)) {
            stack[a] = DOUBLE_VAL(AS_DOUBLE(vb) - literal);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '-' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(MUL_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        if (IS_DOUBLE(vb)) {
            stack[a] = DOUBLE_VAL(AS_DOUBLE(vb) * literal);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '*' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(DIV_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        if (IS_DOUBLE(vb)) {
            stack[a] = DOUBLE_VAL(AS_DOUBLE(vb) / literal);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '/' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(MOD_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        if (IS_DOUBLE(vb)) {
            if (literal == 0.0) {
                STORE_IP(); runtimeError(vm, "Division by zero in '%%'.");
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            stack[a] = DOUBLE_VAL(fmod(AS_DOUBLE(vb), literal));
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '%%' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Bitwise with 16-bit Immediate =====
    OP(BAND_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        if (IS_DOUBLE(va)) {
            int32_t lhs = (int32_t)AS_DOUBLE(va);
            int32_t result = lhs & (int32_t)imm;
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '&' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BOR_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        if (IS_DOUBLE(va)) {
            int32_t lhs = (int32_t)AS_DOUBLE(va);
            int32_t result = lhs | (int32_t)imm;
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '|' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BXOR_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        if (IS_DOUBLE(va)) {
            int32_t lhs = (int32_t)AS_DOUBLE(va);
            int32_t result = lhs ^ (int32_t)imm;
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '^' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BLSHIFT_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        if (IS_DOUBLE(va)) {
            int32_t lhs = (int32_t)AS_DOUBLE(va);
            int32_t result = lhs << ((int32_t)imm & 0x1F);
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '<<' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_U_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        if (IS_DOUBLE(va)) {
            uint32_t lhs = (uint32_t)AS_DOUBLE(va);
            uint32_t result = lhs >> ((int32_t)imm & 0x1F);
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '>>>' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_I_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = stack[a];


        if (IS_DOUBLE(va)) {
            int32_t lhs = (int32_t)AS_DOUBLE(va);
            int32_t result = lhs >> ((int32_t)imm & 0x1F);
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '>>' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Bitwise with 64-bit Literal =====
    OP(BAND_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        if (IS_DOUBLE(vb)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            int32_t result = lhs & rhs;
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '&' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BOR_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        if (IS_DOUBLE(vb)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            int32_t result = lhs | rhs;
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '|' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BXOR_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        if (IS_DOUBLE(vb)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            int32_t result = lhs ^ rhs;
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '^' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BLSHIFT_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        if (IS_DOUBLE(vb)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            int32_t result = lhs << (rhs & 0x1F);
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '<<' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_U_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        if (IS_DOUBLE(vb)) {
            uint32_t lhs = (uint32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            uint32_t result = lhs >> (rhs & 0x1F);
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '>>>' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_I_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = stack[b];

        if (IS_DOUBLE(vb)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            int32_t result = lhs >> (rhs & 0x1F);
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '>>' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    OP(NEG) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value val_b = stack[b];


        if (IS_DOUBLE(val_b)) {
            stack[a] = DOUBLE_VAL(-AS_DOUBLE(val_b));
        } else {
            STORE_IP(); runtimeError(vm, "Operand must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BNOT) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value vb = stack[b];


        if (IS_DOUBLE(vb)) {
            int32_t val = (int32_t)AS_DOUBLE(vb);
            int32_t result = ~val;
            stack[a] = DOUBLE_VAL((double)result);
        } else {
            STORE_IP(); runtimeError(vm, "Operand for '~' must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(DEFINE_GLOBAL) {
        uint32_t instr = ip[-1];
        int src_reg = CUR_BASE() + REG_A(instr);
        uint16_t name_const_idx = REG_Bx(instr);
        ObjString* name = AS_STRING(currentChunk(vm)->constants.values[name_const_idx]);

        // Check if this global already has a slot
        Value existing_slot_or_value;
        if (!tableGet(&vm->globals, name, &existing_slot_or_value)) {
            // New global: allocate a slot
            int slot_index = vm->globalSlots.count;
            if (slot_index > UINT16_MAX) {
                STORE_IP(); runtimeError(vm, "Too many global variables (max %d).", UINT16_MAX + 1);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            // Store the slot index in the globals table
            writeValueArray(vm, &vm->globalSlots, stack[src_reg]);
            tableSet(vm, &vm->globals, name, DOUBLE_VAL((double)slot_index));
        } else if (IS_DOUBLE(existing_slot_or_value)) {
            // Redefining existing slot-based global: update the value in the slot
            int slot_index = (int)AS_DOUBLE(existing_slot_or_value);
            vm->globalSlots.values[slot_index] = stack[src_reg];
        } else {
            // Trying to redefine a direct-storage global (e.g., native function)
            STORE_IP(); runtimeError(vm, "Cannot redefine native function '%.*s'.", name->length, name->chars);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(GET_GLOBAL) {
        uint32_t instr = ip[-1];
        int dest_reg = CUR_BASE() + REG_A(instr);
        uint16_t name_const_idx = REG_Bx(instr);
        ObjString* name = AS_STRING(currentChunk(vm)->constants.values[name_const_idx]);
        Value slot_index_val;
        if (!tableGet(&vm->globals, name, &slot_index_val)) {
            STORE_IP(); runtimeError(vm, "Undefined identifier '%.*s'.", name->length, name->chars);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        // Check if this is a slot index (number) or direct value (e.g., native function)
        if (IS_DOUBLE(slot_index_val)) {
            // Slot-based global: get slot index and cache it
            uint16_t slot_index = (uint16_t)AS_DOUBLE(slot_index_val);

            // Self-modify: rewrite this instruction to GET_GLOBAL_CACHED with the slot index
            uint32_t new_instr = (uint32_t)GET_GLOBAL_CACHED | (REG_A(instr) << 8) | (slot_index << 16);
            ip[-1] = new_instr;

            // Execute the cached version
            stack[dest_reg] = vm->globalSlots.values[slot_index];
        } else {
            // Direct value (e.g., native function) - use as-is, no caching
            stack[dest_reg] = slot_index_val;
        }
        DISPATCH();
    }
    OP(SET_GLOBAL) {
        uint32_t instr = ip[-1];
        int src_reg = CUR_BASE() + REG_A(instr);
        uint16_t name_const_idx = REG_Bx(instr);
        ObjString* name = AS_STRING(currentChunk(vm)->constants.values[name_const_idx]);

        // Check if global exists
        Value slot_index_val;
        if (!tableGet(&vm->globals, name, &slot_index_val)) {
            STORE_IP(); runtimeError(vm, "Undefined identifier '%.*s'.", name->length, name->chars);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        // Check if this is a slot index (number) or direct value (e.g., native function)
        if (IS_DOUBLE(slot_index_val)) {
            // Slot-based global: get slot index and cache it
            uint16_t slot_index = (uint16_t)AS_DOUBLE(slot_index_val);
            vm->globalSlots.values[slot_index] = stack[src_reg];

            // Self-modify: rewrite this instruction to SET_GLOBAL_CACHED with the slot index
            uint32_t new_instr = (uint32_t)SET_GLOBAL_CACHED | (REG_A(instr) << 8) | (slot_index << 16);
            ip[-1] = new_instr;
        } else {
            // Direct value (e.g., native function) - can't set, this is an error
            STORE_IP(); runtimeError(vm, "Cannot assign to native function '%.*s'.", name->length, name->chars);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        DISPATCH();
    }
    OP(GET_GLOBAL_CACHED) {
        // Fast path: direct array lookup using cached slot index
        uint32_t instr = ip[-1];
        int dest_reg = CUR_BASE() + REG_A(instr);
        uint16_t slot_index = REG_Bx(instr);
        stack[dest_reg] = vm->globalSlots.values[slot_index];
        DISPATCH();
    }
    OP(SET_GLOBAL_CACHED) {
        // Fast path: direct array write using cached slot index
        uint32_t instr = ip[-1];
        int src_reg = CUR_BASE() + REG_A(instr);
        uint16_t slot_index = REG_Bx(instr);
        vm->globalSlots.values[slot_index] = stack[src_reg];
        DISPATCH();
    }
    OP(JUMP_IF_FALSE) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t raw = REG_Bx(instr);
        int32_t off = sign_extend_16(raw);

        Value condition = stack[a];

        // falsey = null (0x7FF8000000000001), false (0x7FF8000000000002), or 0.0 (0x0000000000000000)
        if (condition == 0 || condition == NULL_VAL || condition == FALSE_VAL) {
            ip += off;
        }
        DISPATCH();
    }
    OP(JUMP_IF_TRUE) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t raw = REG_Bx(instr);
        int32_t off = sign_extend_16(raw);

        Value condition = stack[a];

        if (condition != 0 && condition != NULL_VAL && condition != FALSE_VAL) {
            ip += off;
        }
        DISPATCH();
    }
    OP(JUMP) {
        uint32_t instr = ip[-1];
        uint16_t raw = REG_Bx(instr);
        int32_t off = sign_extend_16(raw);
        ip += off;
        DISPATCH();
    }

    // ===== Branch-Compare Opcodes (Register-Register) =====
    OP(BRANCH_EQ) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = stack[a];
        Value vb = stack[b];

        if (va == vb || (IS_DOUBLE(va) && IS_DOUBLE(vb) && AS_DOUBLE(va) == AS_DOUBLE(vb))) {
            ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_NE) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = stack[a];
        Value vb = stack[b];

        if (va != vb && !(IS_DOUBLE(va) && IS_DOUBLE(vb) && AS_DOUBLE(va) == AS_DOUBLE(vb))) {
            ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_LT) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = stack[a];
        Value vb = stack[b];


        if (IS_DOUBLE(va) && IS_DOUBLE(vb)) {
            if (AS_DOUBLE(va) < AS_DOUBLE(vb)) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operands must be numbers for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_LE) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = stack[a];
        Value vb = stack[b];


        if (IS_DOUBLE(va) && IS_DOUBLE(vb)) {
            if (AS_DOUBLE(va) <= AS_DOUBLE(vb)) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operands must be numbers for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GT) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = stack[a];
        Value vb = stack[b];


        if (IS_DOUBLE(va) && IS_DOUBLE(vb)) {
            if (AS_DOUBLE(va) > AS_DOUBLE(vb)) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operands must be numbers for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GE) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = stack[a];
        Value vb = stack[b];


        if (IS_DOUBLE(va) && IS_DOUBLE(vb)) {
            if (AS_DOUBLE(va) >= AS_DOUBLE(vb)) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operands must be numbers for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Branch-Compare Opcodes (Register-Immediate) =====
    OP(BRANCH_EQ_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *ip++;  // Offset in next instruction
        off = sign_extend_16(off);
        Value va = stack[a];

        Value imm_val = DOUBLE_VAL((double)imm);
        bool matches = (va == imm_val);
        if (!matches) {
            if (imm == 0) {
                matches = (va == 0) || (va == NULL_VAL) || (va == FALSE_VAL);
            } else if (IS_BOOL(va)) {
                matches = (AS_BOOL(va) == (imm != 0));
            }
        }

        if (matches) {
            ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_NE_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *ip++;
        off = sign_extend_16(off);
        Value va = stack[a];

        Value imm_val = DOUBLE_VAL((double)imm);
        bool matches = (va != imm_val);
        if (matches) {
            if (imm == 0) {
                matches = (va != 0) && (va != NULL_VAL) && (va != FALSE_VAL);
            } else if (IS_BOOL(va)) {
                matches = (AS_BOOL(va) != (imm != 0));
            }
        }

        if (matches) {
            ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_LT_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *ip++;
        off = sign_extend_16(off);
        Value va = stack[a];


        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            if (AS_DOUBLE(va) < (double)imm) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operand must be a number for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_LE_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *ip++;
        off = sign_extend_16(off);
        Value va = stack[a];


        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            if (AS_DOUBLE(va) <= (double)imm) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operand must be a number for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GT_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *ip++;
        off = sign_extend_16(off);
        Value va = stack[a];


        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            if (AS_DOUBLE(va) > (double)imm) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operand must be a number for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GE_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *ip++;
        off = sign_extend_16(off);
        Value va = stack[a];


        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            if (AS_DOUBLE(va) >= (double)imm) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operand must be a number for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Branch-Compare Opcodes (Register-Literal) =====
    OP(BRANCH_EQ_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        Value literal_val = ((uint64_t)high << 32) | (uint64_t)low;
        int32_t off = *ip++;
        off = sign_extend_16(off);
        Value va = stack[a];

        bool matches = (va == literal_val);
        if (!matches) {
            if (literal_val == 0) {
                matches = (va == 0) || (va == NULL_VAL) || (va == FALSE_VAL);
            } else if (IS_DOUBLE(va) && IS_DOUBLE(literal_val)) {
                matches = (AS_DOUBLE(va) == AS_DOUBLE(literal_val));
            } else if (IS_BOOL(va)) {
                double literal; memcpy(&literal, &literal_val, sizeof(double));
                matches = (AS_BOOL(va) == (literal != 0.0));
            }
        }

        if (matches) {
            ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_NE_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        Value literal_val = ((uint64_t)high << 32) | (uint64_t)low;
        int32_t off = *ip++;
        off = sign_extend_16(off);
        Value va = stack[a];

        bool matches = (va != literal_val);
        if (matches) {
            if (literal_val == 0) {
                matches = (va != 0) && (va != NULL_VAL) && (va != FALSE_VAL);
            } else if (IS_DOUBLE(va) && IS_DOUBLE(literal_val)) {
                matches = (AS_DOUBLE(va) != AS_DOUBLE(literal_val));
            } else if (IS_BOOL(va)) {
                double literal; memcpy(&literal, &literal_val, sizeof(double));
                matches = (AS_BOOL(va) != (literal != 0.0));
            }
        }

        if (matches) {
            ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_LT_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));
        int32_t off = *ip++;
        off = sign_extend_16(off);
        Value va = stack[a];


        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            if (AS_DOUBLE(va) < literal) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operand must be a number for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_LE_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));
        int32_t off = *ip++;
        off = sign_extend_16(off);
        Value va = stack[a];


        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            if (AS_DOUBLE(va) <= literal) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operand must be a number for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GT_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));
        int32_t off = *ip++;
        off = sign_extend_16(off);
        Value va = stack[a];


        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            if (AS_DOUBLE(va) > literal) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operand must be a number for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GE_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *ip++;
        uint32_t high = *ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));
        int32_t off = *ip++;
        off = sign_extend_16(off);
        Value va = stack[a];


        if (__builtin_expect(IS_DOUBLE(va), 1)) {
            if (AS_DOUBLE(va) >= literal) {
                ip += off;
            }
        } else {
            STORE_IP(); runtimeError(vm, "Operand must be a number for comparison.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    OP(CALL) {
        uint32_t instr = ip[-1];
        int callee_slot = CUR_BASE() + REG_A(instr);
        uint16_t arg_count = REG_Bx(instr);
        Value callee = stack[callee_slot];

        // Dereference if callee is a reference (refs are first-class)
        stack[callee_slot] = callee;

        // Resolve dispatcher overload if needed
        if (IS_DISPATCHER(callee)) {
            Value matched_closure = resolveOverload(vm, AS_DISPATCHER(callee), arg_count);
            if (IS_NULL(matched_closure)) {
                STORE_IP(); runtimeError(vm, "No overload found for %u arguments.", arg_count);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            callee = matched_closure;
            stack[callee_slot] = callee;
        }

        // Handle closures first (most common call target)
        if (IS_CLOSURE(callee)) {
            ObjClosure*  closure  = AS_CLOSURE(callee);
            ObjFunction* function = closure->function;

            #ifdef DEBUG_CALL
            printf("[VM CALL] CUR_BASE=%d, REG_A=%d, callee_slot=%d, arg_count=%u\n",
                   CUR_BASE(), REG_A(instr), callee_slot, arg_count);
            printf("[VM CALL] Function: %.*s, arity=%d\n",
                   function->name ? function->name->length : 6,
                   function->name ? function->name->chars : "<anon>",
                   function->arity);
            for (int i = 1; i <= arg_count; i++) {
                printf("[VM CALL]   Arg %d at stack[%d]: ", i, callee_slot + i);
                printValue(stack[callee_slot + i]);
                printf("\n");
            }
            #endif

            if (__builtin_expect(arg_count != function->arity, 0)) {
                STORE_IP(); runtimeError(vm, "Expected %d arguments but got %u.", function->arity, arg_count);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }

            if (__builtin_expect(vm->frame_count >= FRAMES_MAX, 0)) {
                STORE_IP(); runtimeError(vm, "Stack overflow: maximum call depth (%d) reached.", FRAMES_MAX);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }

            // Calculate required stack size and grow if needed
            int needed_top = callee_slot + function->max_regs;
            if (__builtin_expect(needed_top > vm->stack_capacity, 0)) {
                if (!growStackForCall(vm, needed_top, NULL)) {
                    STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
                }
                stack = vm->stack;
            }

            // Update stack_top to track highest used slot
            if (needed_top > vm->stack_top) {
                vm->stack_top = needed_top;
            }

            // Push frame
            CallFrame* frame = &vm->frames[vm->frame_count++];
            frame->closure      = closure;
            frame->ip           = ip;
            frame->stack_base   = callee_slot;
            frame->caller_chunk = vm->chunk;
            frame->flags        = 0;

            vm->current_frame = frame;
            base = callee_slot;
            // Enter callee
            vm->chunk = &function->chunk;
            ip = function->chunk.code;
            DISPATCH();
        }

        // Handle native functions
        if (IS_NATIVE_FUNCTION(callee)) {
            ObjNativeFunction* native = AS_NATIVE_FUNCTION(callee);

            if (arg_count != native->arity) {
                STORE_IP(); runtimeError(vm, "Expected %d arguments but got %u.", native->arity, arg_count);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }

            // Prepare arguments array (points to first arg on stack)
            Value* args = &stack[callee_slot + 1];

            // Protect arguments from GC during native call
            int saved_temp_root_count = vm->temp_root_count;
            for (int i = 0; i < arg_count; i++) {
                if (IS_OBJ(args[i])) {
                    pushTempRoot(vm, AS_OBJ(args[i]));
                }
            }

            // Call native function via dispatcher
            STORE_STATE();
            Value result = native->dispatcher(vm, args, native->func_ptr);
            stack = vm->stack; // native may trigger GC that reallocates stack

            // Restore temp root count
            vm->temp_root_count = saved_temp_root_count;

            // Check for error
            if (result == ZYM_ERROR) {
                // Native function reported error via zym_runtimeError
                return INTERPRET_RUNTIME_ERROR;
            }

            // Check for control transfer (capture/abort)
            // The native has already modified VM state; just continue execution
            if (result == ZYM_CONTROL_TRANSFER) {
                LOAD_STATE(); DISPATCH();
            }

            // Place result in callee slot
            stack[callee_slot] = result;

            DISPATCH();
        }

        // Handle native closures
        if (IS_NATIVE_CLOSURE(callee)) {
            ObjNativeClosure* native_closure = AS_NATIVE_CLOSURE(callee);

            if (arg_count != native_closure->arity) {
                STORE_IP(); runtimeError(vm, "Expected %d arguments but got %u.", native_closure->arity, arg_count);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }

            // Prepare arguments array: [context, arg1, arg2, ...]
            // We need to build a temporary array with context as first element
            Value closure_args[MAX_NATIVE_ARITY + 1];
            closure_args[0] = native_closure->context;  // Context is first argument
            for (int i = 0; i < arg_count; i++) {
                closure_args[i + 1] = stack[callee_slot + 1 + i];
            }

            // Protect arguments and context from GC during native call
            int saved_temp_root_count = vm->temp_root_count;
            if (IS_OBJ(closure_args[0])) {
                pushTempRoot(vm, AS_OBJ(closure_args[0]));
            }
            for (int i = 0; i < arg_count; i++) {
                if (IS_OBJ(closure_args[i + 1])) {
                    pushTempRoot(vm, AS_OBJ(closure_args[i + 1]));
                }
            }

            // Call native closure via dispatcher (context-aware dispatcher)
            STORE_STATE();
            Value result = native_closure->dispatcher(vm, closure_args, native_closure->func_ptr);
            stack = vm->stack; // native may trigger GC that reallocates stack

            // Restore temp root count
            vm->temp_root_count = saved_temp_root_count;

            // Check for error
            if (result == ZYM_ERROR) {
                // Native closure reported error via zym_runtimeError
                return INTERPRET_RUNTIME_ERROR;
            }

            // Check for control transfer (capture/abort)
            // The native has already modified VM state; just continue execution
            if (result == ZYM_CONTROL_TRANSFER) {
                LOAD_STATE(); DISPATCH();
            }

            // Place result in callee slot
            stack[callee_slot] = result;

            DISPATCH();
        }

        STORE_IP(); runtimeError(vm, ERR_ONLY_CALL_FUNCTIONS);
        STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
    }
    OP(CALL_SELF) {
        uint32_t instr = ip[-1];
        CallFrame* current_frame = vm->current_frame;
        int callee_slot = current_frame->stack_base + REG_A(instr);
        ObjClosure* closure = current_frame->closure;
        ObjFunction* function = closure->function;

        stack[callee_slot] = OBJ_VAL(closure);

        #ifdef DEBUG_CALL
        uint16_t arg_count_dbg = REG_Bx(instr);
        printf("[VM CALL_SELF] CUR_BASE=%d, REG_A=%d, callee_slot=%d, arg_count=%u\n",
               CUR_BASE(), REG_A(instr), callee_slot, arg_count_dbg);
        printf("[VM CALL_SELF] Function: %.*s, arity=%d\n",
               function->name ? function->name->length : 6,
               function->name ? function->name->chars : "<anon>",
               function->arity);
        #endif

        if (__builtin_expect(vm->frame_count >= FRAMES_MAX, 0)) {
            STORE_IP(); runtimeError(vm, "Stack overflow: maximum call depth (%d) reached.", FRAMES_MAX);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        int needed_top = callee_slot + function->max_regs;
        if (__builtin_expect(needed_top > vm->stack_capacity, 0)) {
            if (!growStackForCall(vm, needed_top, NULL)) {
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            stack = vm->stack;
        }

        if (needed_top > vm->stack_top) {
            vm->stack_top = needed_top;
        }

        CallFrame* frame = &vm->frames[vm->frame_count++];
        frame->closure      = closure;
        frame->ip           = ip;
        frame->stack_base   = callee_slot;
        frame->caller_chunk = vm->chunk;
        frame->flags        = 0;

        vm->current_frame = frame;
        base = callee_slot;
        ip = function->chunk.code;
        DISPATCH();
    }
    OP(TAIL_CALL) {
        uint32_t instr = ip[-1];
        int callee_slot = CUR_BASE() + REG_A(instr);
        uint16_t arg_count = REG_Bx(instr);
        Value callee = stack[callee_slot];

        // Dereference if callee is a reference
        stack[callee_slot] = callee;

        // Resolve dispatcher overload if needed
        if (IS_DISPATCHER(callee)) {
            Value matched_closure = resolveOverload(vm, AS_DISPATCHER(callee), arg_count);
            if (IS_NULL(matched_closure)) {
                STORE_IP(); runtimeError(vm, "No overload found for %u arguments.", arg_count);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            callee = matched_closure;
            stack[callee_slot] = callee;
        }

        // Handle closures first (most common tail call target)
        if (IS_CLOSURE(callee)) {
            ObjClosure*  closure  = AS_CLOSURE(callee);
            ObjFunction* function = closure->function;

            if (arg_count != function->arity) {
                STORE_IP(); runtimeError(vm, "Expected %d arguments but got %u.", function->arity, arg_count);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }

            // TAIL CALL OPTIMIZATION: Reuse current frame instead of pushing new one
            CallFrame* current_frame = vm->current_frame;
            int frame_base = current_frame->stack_base;
            int needed_top = frame_base + function->max_regs;

            // Grow stack if needed
            if (!growStackForCall(vm, needed_top, NULL)) {
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            stack = vm->stack;

            if (needed_top > vm->stack_top) {
                vm->stack_top = needed_top;
            }

            // Upvalues have already been closed by CLOSE_FRAME_UPVALUES instruction
            // Move args to the frame base
            for (int i = 0; i < arg_count; i++) {
                stack[frame_base + 1 + i] = stack[callee_slot + 1 + i];
            }

            // Put the new callee in R0 of this frame
            stack[frame_base] = callee;

            // Update the frame to point at the new closure
            current_frame->closure = closure;

            // Jump into the new function
            vm->chunk = &function->chunk;
            ip    = function->chunk.code;

            DISPATCH();
        }

        // Handle native functions in tail position: call directly and return result
        if (IS_NATIVE_FUNCTION(callee)) {
            ObjNativeFunction* native = AS_NATIVE_FUNCTION(callee);

            if (arg_count != native->arity) {
                STORE_IP(); runtimeError(vm, "Expected %d arguments but got %u.", native->arity, arg_count);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }

            Value* args = &stack[callee_slot + 1];

            int saved_temp_root_count = vm->temp_root_count;
            for (int i = 0; i < arg_count; i++) {
                if (IS_OBJ(args[i])) {
                    pushTempRoot(vm, AS_OBJ(args[i]));
                }
            }

            STORE_STATE();
            Value result = native->dispatcher(vm, args, native->func_ptr);
            stack = vm->stack; // native may trigger GC that reallocates stack

            vm->temp_root_count = saved_temp_root_count;

            if (result == ZYM_ERROR) {
                return INTERPRET_RUNTIME_ERROR;
            }

            if (result == ZYM_CONTROL_TRANSFER) {
                LOAD_STATE(); DISPATCH();
            }

            // Tail position: return the native's result from the current frame
            CallFrame* frame = vm->current_frame;
            if (__builtin_expect(vm->open_upvalues != NULL &&
                                vm->open_upvalues->location >= &stack[frame->stack_base], 0)) {
                closeUpvalues(vm, &stack[frame->stack_base]);
            }
            vm->frame_count--;
            base = vm->frame_count == 0 ? 0 : vm->frames[vm->frame_count - 1].stack_base;
            vm->current_frame = vm->frame_count == 0 ? NULL : &vm->frames[vm->frame_count - 1];

            if (__builtin_expect(vm->active_boundaries > 0, 0)) {
                if (vm->with_prompt_depth > 0) {
                    WithPromptContext* wpc = &vm->with_prompt_stack[vm->with_prompt_depth - 1];
                    if (vm->frame_count == wpc->frame_boundary) {
                        popPrompt(vm);
                        vm->with_prompt_depth--;
                        vm->active_boundaries--;
                    }
                }

                if (vm->resume_depth > 0) {
                    ResumeContext* ctx = &vm->resume_stack[vm->resume_depth - 1];
                    if (vm->frame_count == ctx->frame_boundary) {
                        stack[ctx->result_slot] = result;
                        vm->resume_depth--;
                        vm->active_boundaries--;
                        ip    = frame->ip;
                        vm->chunk = frame->caller_chunk;
                        DISPATCH();
                    }
                }
            }

            ip    = frame->ip;
            vm->chunk = frame->caller_chunk;
            stack[frame->stack_base] = result;

            DISPATCH();
        }

        STORE_IP(); runtimeError(vm, ERR_ONLY_CALL_FUNCTIONS);
        STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
    }
    OP(TAIL_CALL_SELF) {
        uint32_t instr = ip[-1];
        CallFrame* current_frame = vm->current_frame;
        int callee_slot = current_frame->stack_base + REG_A(instr);
        uint16_t arg_count = REG_Bx(instr);
        ObjClosure* closure = current_frame->closure;
        ObjFunction* function = closure->function;

        stack[callee_slot] = OBJ_VAL(closure);

        int frame_base = current_frame->stack_base;
        int needed_top = frame_base + function->max_regs;

        if (__builtin_expect(needed_top > STACK_MAX, 0)) {
            STORE_IP(); runtimeError(vm, "Stack overflow.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        // Move arguments from callee_slot to frame_base
        for (int i = 0; i <= arg_count; i++) {
            stack[frame_base + i] = stack[callee_slot + i];
        }

        // Update stack_top if needed
        if (needed_top > vm->stack_top) {
            vm->stack_top = needed_top;
        }

        // Jump into the function (restart from beginning)
        ip = function->chunk.code;

        DISPATCH();
    }
    OP(RET) {
        uint32_t instr = ip[-1];
        if (vm->frame_count == 0) {
            STORE_STATE(); return INTERPRET_OK;
        }

        int  ret_reg = REG_A(instr);
        bool implicit_null = (REG_Bx(instr) == 1);
        CallFrame* frame = vm->current_frame;

        // Get the return value BEFORE closing upvalues
        Value return_value = implicit_null
                           ? NULL_VAL
                           : stack[frame->stack_base + ret_reg];

        // Before we pop the frame, close any upvalues pointing to its stack slots.
        // Fast path: skip the function call if no open upvalues reach into this frame.
        if (__builtin_expect(vm->open_upvalues != NULL &&
                            vm->open_upvalues->location >= &stack[frame->stack_base], 0)) {
            closeUpvalues(vm, &stack[frame->stack_base]);
        }

        // Now pop the callee frame
        vm->frame_count--;
        base = vm->frame_count == 0 ? 0 : vm->frames[vm->frame_count - 1].stack_base;
        vm->current_frame = vm->frame_count == 0 ? NULL : &vm->frames[vm->frame_count - 1];

        if (frame->flags & (FRAME_FLAG_PREEMPT | FRAME_FLAG_DISABLE_PREEMPT)) {
            vm->preemption_disable_depth--;
        }

        // Single check for any active boundary (withPrompt or resume)
        if (__builtin_expect(vm->active_boundaries > 0, 0)) {
            // Check if we're returning from a withPrompt boundary frame
            if (vm->with_prompt_depth > 0) {
                WithPromptContext* wpc = &vm->with_prompt_stack[vm->with_prompt_depth - 1];
                if (vm->frame_count == wpc->frame_boundary) {
                    popPrompt(vm);
                    vm->with_prompt_depth--;
                    vm->active_boundaries--;
                }
            }

            // Check if we're returning from a resumed continuation's boundary frame
            if (vm->resume_depth > 0) {
                ResumeContext* ctx = &vm->resume_stack[vm->resume_depth - 1];
                if (vm->frame_count == ctx->frame_boundary) {
                    stack[ctx->result_slot] = return_value;
                    vm->resume_depth--;
                    vm->active_boundaries--;
                    ip    = frame->ip;
                    vm->chunk = frame->caller_chunk;
                    DISPATCH();
                }
            }
        }

        // Normal return: restore caller context
        ip    = frame->ip;
        vm->chunk = frame->caller_chunk;
        stack[frame->stack_base] = return_value;

        DISPATCH();
    }
    OP(CLOSURE) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);

        // 1. Get the function template from the constant pool.
        ObjFunction* function = AS_FUNCTION(currentChunk(vm)->constants.values[bx]);

        // 2. Create the closure object.
        ObjClosure* closure = newClosure(vm, function);
        stack = vm->stack; // GC may have reallocated stack
        // Protect closure from GC during stack write and captureUpvalue calls
        pushTempRoot(vm, (Obj*)closure);

        stack[a] = OBJ_VAL(closure);

        // 3. Capture the upvalues based on the "recipe" stored in the ObjFunction.
        // Get the current stack base (handles both main script with frame_count=0 and functions)
        int cur_base = CUR_BASE();
        for (int i = 0; i < closure->upvalue_count; i++) {
            uint8_t is_local = function->upvalues[i].is_local;
            uint8_t index = function->upvalues[i].index;
            if (is_local) {
                // Capture a local variable from the current (enclosing) function's stack frame.
                closure->upvalues[i] = captureUpvalue(vm, &stack[cur_base + index]);
                stack = vm->stack; // GC may have reallocated stack
            } else {
                // Capture an upvalue from the enclosing function itself.
                // This requires an actual call frame (not the main script).
                if (vm->current_frame != NULL) {
                    closure->upvalues[i] = vm->current_frame->closure->upvalues[index];
                } else {
                    // Main script cannot have parent upvalues (should never happen)
                    closure->upvalues[i] = NULL;
                }
            }
        }

        popTempRoot(vm);
        DISPATCH();
    }
    OP(GET_UPVALUE) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);

        CallFrame* frame = vm->current_frame;
        // The value is read from the location the upvalue points to.
        Value value = *frame->closure->upvalues[bx]->location;

        // Don't auto-dereference - let references be first-class values
        // Dereferencing happens at use sites (arithmetic, print, etc.)
        stack[a] = value;
        DISPATCH();
    }
    OP(SET_UPVALUE) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);

        CallFrame* frame = vm->current_frame;
        if (!validateUpvalue(vm, frame->closure->upvalues[bx], "SET_UPVALUE")) {
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        *frame->closure->upvalues[bx]->location = stack[a];
        DISPATCH();
    }
    OP(CLOSE_UPVALUE) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        closeUpvalues(vm, &stack[a]);
        DISPATCH();
    }
    OP(CLOSE_FRAME_UPVALUES) {
        // Close all upvalues for the current frame
        // Used before TAIL_CALL to ensure upvalues are closed before we overwrite the stack
        CallFrame* frame = vm->current_frame;
        closeUpvalues(vm, &stack[frame->stack_base]);
        DISPATCH();
    }
    OP(NEW_LIST) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int count = REG_Bx(instr);
        ObjList* list = newList(vm);
        stack = vm->stack; // GC may have reallocated stack
        // Protect the list from GC during writeValueArray and stack write
        pushTempRoot(vm, (Obj*)list);
        // If count > 0, copy elements from subsequent stack slots.
        // (This makes our opcode future-proof for optimizations).
        for (int i = 0; i < count; i++) {
            writeValueArray(vm, &list->items, stack[a + 1 + i]);
        }
        stack[a] = OBJ_VAL(list);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(LIST_APPEND) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // list
        int b = CUR_BASE() + REG_B(instr); // value
        Value list_val = stack[a];
        if (!IS_LIST(list_val)) {
            STORE_IP(); runtimeError(vm, "Can only append to a list.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        ObjList* list = AS_LIST(list_val);
        Value value_to_append = stack[b];
        writeValueArray(vm, &list->items, value_to_append);
        DISPATCH();
    }
    OP(LIST_SPREAD) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // target list
        int b = CUR_BASE() + REG_B(instr); // source list to spread
        Value target_val = stack[a];
        Value source_val = stack[b];
        if (!IS_LIST(target_val)) {
            STORE_IP(); runtimeError(vm, "Spread target must be a list.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        if (!IS_LIST(source_val)) {
            STORE_IP(); runtimeError(vm, "Spread source must be a list.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        ObjList* target = AS_LIST(target_val);
        ObjList* source = AS_LIST(source_val);
        // Append all elements from source to target
        for (int i = 0; i < source->items.count; i++) {
            writeValueArray(vm, &target->items, source->items.values[i]);
        }
        DISPATCH();
    }
    OP(NEW_MAP) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        ObjMap* map = newMap(vm);
        stack = vm->stack; // GC may have reallocated stack
        // Protect the map from GC during stack write
        pushTempRoot(vm, (Obj*)map);
        stack[a] = OBJ_VAL(map);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(MAP_SET) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Map register
        int b = CUR_BASE() + REG_B(instr); // Key register
        int c = CUR_BASE() + REG_C(instr); // Value register
        Value map_val = stack[a];
        Value key_val = stack[b];
        Value value_val = stack[c];

        if (!IS_MAP(map_val)) {
            STORE_IP(); runtimeError(vm, "MAP_SET expects a map object.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjMap* map = AS_MAP(map_val);
        ObjString* key_str = keyToString(vm, key_val);
        if (!key_str) {
            STORE_IP(); runtimeError(vm, ERR_MAP_KEYS_TYPE);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        // Set the key-value pair (or skip if value is null)
        if (!IS_NULL(value_val)) {
            tableSet(vm, &map->table, key_str, value_val);
        }
        DISPATCH();
    }
    OP(MAP_SPREAD) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // target map
        int b = CUR_BASE() + REG_B(instr); // source map to spread
        Value target_val = stack[a];
        Value source_val = stack[b];
        if (!IS_MAP(target_val)) {
            STORE_IP(); runtimeError(vm, "Spread target must be a map.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        if (!IS_MAP(source_val)) {
            STORE_IP(); runtimeError(vm, "Spread source must be a map.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        ObjMap* target = AS_MAP(target_val);
        ObjMap* source = AS_MAP(source_val);
        // Copy all key-value pairs from source to target
        for (int i = 0; i < source->table.capacity; i++) {
            Entry* entry = &source->table.entries[i];
            if (entry->key != NULL) {
                tableSet(vm, &target->table, entry->key, entry->value);
            }
        }
        DISPATCH();
    }
    OP(GET_SUBSCRIPT) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register
        int b = CUR_BASE() + REG_B(instr); // Object register (list or map)
        int c = CUR_BASE() + REG_C(instr); // Index/Key register
        Value obj_val = stack[b];
        Value key_val = stack[c];

        // Dereference container if it's a reference

        // Handle maps
        if (IS_MAP(obj_val)) {
            ObjMap* map = AS_MAP(obj_val);
            ObjString* key_str = keyToString(vm, key_val);
            if (!key_str) {
                STORE_IP(); runtimeError(vm, ERR_MAP_KEYS_TYPE);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }

            Value result;
            if (tableGet(&map->table, key_str, &result)) {
                stack[a] = result;
            } else {
                stack[a] = NULL_VAL;
            }
            DISPATCH();
        }

        // Handle lists
        if (!IS_LIST(obj_val)) {
            STORE_IP(); runtimeError(vm, ERR_ONLY_SUBSCRIPT_LISTS_MAPS);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        ObjList* list = AS_LIST(obj_val);
        if (!IS_DOUBLE(key_val)) {
            STORE_IP(); runtimeError(vm, ERR_LIST_INDEX_TYPE);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        double index_double = AS_DOUBLE(key_val);
        int index = (int)index_double;
        if (index != index_double) {
            STORE_IP(); runtimeError(vm, "List index must be an integer.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        if (index < 0 || index >= list->items.count) {
            STORE_IP(); runtimeError(vm, "List index out of bounds.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        Value result = list->items.values[index];
        // Don't auto-dereference - refs are first-class values
        stack[a] = result;
        DISPATCH();
    }
    OP(GET_SUBSCRIPT_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int index = REG_C(instr);
        Value obj_val = stack[b];


        if (IS_LIST(obj_val)) {
            ObjList* list = AS_LIST(obj_val);
            if (index >= list->items.count) {
                STORE_IP(); runtimeError(vm, "List index out of bounds.");
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            stack[a] = list->items.values[index];
            DISPATCH();
        }

        if (IS_MAP(obj_val)) {
            ObjMap* map = AS_MAP(obj_val);
            char buf[12];
            snprintf(buf, sizeof(buf), "%d", index);
            ObjString* key_str = copyString(vm, buf, (int)strlen(buf));
            stack = vm->stack; // GC may have reallocated stack
            Value result;
            if (tableGet(&map->table, key_str, &result)) {
                stack[a] = result;
            } else {
                stack[a] = NULL_VAL;
            }
            DISPATCH();
        }

        STORE_IP(); runtimeError(vm, ERR_ONLY_SUBSCRIPT_LISTS_MAPS);
        STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
    }
    OP(SET_SUBSCRIPT) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Object register (list or map)
        int b = CUR_BASE() + REG_B(instr); // Index/Key register
        int c = CUR_BASE() + REG_C(instr); // Value register
        Value obj_val = stack[a];
        Value key_val = stack[b];
        Value value_val = stack[c];

        // Dereference container if it's a reference

        // Handle maps
        if (IS_MAP(obj_val)) {
            ObjMap* map = AS_MAP(obj_val);
            ObjString* key_str = keyToString(vm, key_val);
            if (!key_str) {
                STORE_IP(); runtimeError(vm, ERR_MAP_KEYS_TYPE);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }

            // Delete key if value is null
            if (IS_NULL(value_val)) {
                tableDelete(&map->table, key_str);
            } else {
                tableSet(vm, &map->table, key_str, value_val);
            }
            DISPATCH();
        }

        // Handle lists
        if (!IS_LIST(obj_val)) {
            STORE_IP(); runtimeError(vm, ERR_ONLY_SUBSCRIPT_LISTS_MAPS);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        ObjList* list = AS_LIST(obj_val);
        if (!IS_DOUBLE(key_val)) {
            STORE_IP(); runtimeError(vm, ERR_LIST_INDEX_TYPE);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        double index_double = AS_DOUBLE(key_val);
        int index = (int)index_double;
        if (index != index_double) {
            STORE_IP(); runtimeError(vm, "List index must be an integer.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        if (index < 0 || index >= list->items.count) {
            STORE_IP(); runtimeError(vm, "List index out of bounds.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        list->items.values[index] = value_val;
        DISPATCH();
    }
    OP(SET_SUBSCRIPT_I) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int index = REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value obj_val = stack[a];
        Value value_val = stack[c];


        if (IS_LIST(obj_val)) {
            ObjList* list = AS_LIST(obj_val);
            if (index >= list->items.count) {
                STORE_IP(); runtimeError(vm, "List index out of bounds.");
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            list->items.values[index] = value_val;
            DISPATCH();
        }

        if (IS_MAP(obj_val)) {
            ObjMap* map = AS_MAP(obj_val);
            char buf[12];
            snprintf(buf, sizeof(buf), "%d", index);
            ObjString* key_str = copyString(vm, buf, (int)strlen(buf));
            stack = vm->stack; // GC may have reallocated stack

            if (IS_NULL(value_val)) {
                tableDelete(&map->table, key_str);
            } else {
                tableSet(vm, &map->table, key_str, value_val);
            }
            DISPATCH();
        }

        STORE_IP(); runtimeError(vm, ERR_ONLY_SUBSCRIPT_LISTS_MAPS);
        STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
    }
    /*OP(GET_MAP_PROPERTY) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register
        int b = CUR_BASE() + REG_B(instr); // Container register (map or struct)
        int c = CUR_BASE() + REG_C(instr); // Key register (must be a string)
        Value container_val = stack[b];
        Value key_val = stack[c];

        // Dereference container if it's a reference

        if (!IS_STRING(key_val)) {
            STORE_IP(); runtimeError(vm, "Property key must be a string.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjString* key_str = AS_STRING(key_val);

        // Handle struct instances
        if (IS_STRUCT_INSTANCE(container_val)) {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(container_val);

            // Fast field lookup using pointer comparison on interned strings
            int field_index = find_field_index(instance->schema, key_str);
            if (field_index >= 0) {
                stack[a] = instance->fields[field_index];
            } else {
                STORE_IP(); runtimeError(vm, "Struct '%s' has no field '%s'.",
                             instance->schema->name->chars, key_str->chars);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            DISPATCH();
        }

        // Handle maps
        if (!IS_MAP(container_val)) {
            STORE_IP(); runtimeError(vm, ERR_ONLY_MAPS);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjMap* map = AS_MAP(container_val);
        Value result;
        if (tableGet(&map->table, key_str, &result)) {
            stack[a] = result;
        } else {
            stack[a] = NULL_VAL;
        }
        DISPATCH();
    }*/
    /*OP(SET_MAP_PROPERTY) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Container register (map or struct)
        int b = CUR_BASE() + REG_B(instr); // Key register (must be a string)
        int c = CUR_BASE() + REG_C(instr); // Value register
        Value container_val = stack[a];
        Value key_val = stack[b];
        Value value_val = stack[c];

        // Dereference container if it's a reference

        if (!IS_STRING(key_val)) {
            STORE_IP(); runtimeError(vm, "Property key must be a string.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjString* key_str = AS_STRING(key_val);

        // Handle struct instances
        if (IS_STRUCT_INSTANCE(container_val)) {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(container_val);

            // Fast field lookup using pointer comparison on interned strings
            int field_index = find_field_index(instance->schema, key_str);
            if (field_index >= 0) {
                instance->fields[field_index] = value_val;
            } else {
                STORE_IP(); runtimeError(vm, "Struct '%s' has no field '%s'.",
                             instance->schema->name->chars, key_str->chars);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
            DISPATCH();
        }

        // Handle maps
        if (!IS_MAP(container_val)) {
            STORE_IP(); runtimeError(vm, ERR_ONLY_MAPS);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjMap* map = AS_MAP(container_val);

        // Normal set/delete
        if (IS_NULL(value_val)) {
            tableDelete(&map->table, key_str);
        } else {
            tableSet(vm, &map->table, key_str, value_val);
        }
        DISPATCH();
    }*/
    OP(GET_MAP_PROPERTY_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);

        // Read key string from constant pool via trailing index word
        uint32_t const_idx = *ip++;
        ObjString* key_str = AS_STRING(currentChunk(vm)->constants.values[const_idx]);

        Value container_val = stack[b];

        // Handle struct instances
        if (IS_STRUCT_INSTANCE(container_val)) {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(container_val);
            int field_index = find_field_index(instance->schema, key_str);
            if (field_index >= 0) {
                stack[a] = instance->fields[field_index];

                // Self-patch: bake field_index into C, switch to IC opcode
                ip[-2] = (uint32_t)(GET_STRUCT_FIELD_IC) | ((uint32_t)REG_A(instr) << 8) | ((uint32_t)REG_B(instr) << 16) | ((uint32_t)field_index << 24);
                DISPATCH();
            }
            STORE_IP(); runtimeError(vm, "Struct '%s' has no field '%s'.",
                         instance->schema->name->chars, key_str->chars);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        // Handle maps
        if (!IS_MAP(container_val)) {
            STORE_IP(); runtimeError(vm, ERR_ONLY_MAPS);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjMap* map = AS_MAP(container_val);
        Value result;
        if (tableGet(&map->table, key_str, &result)) {
            stack[a] = result;
        } else {
            stack[a] = NULL_VAL;
        }
        DISPATCH();
    }
    OP(SET_MAP_PROPERTY_L) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int c = CUR_BASE() + REG_C(instr);

        // Read key string from constant pool via trailing index word
        uint32_t const_idx = *ip++;
        ObjString* key_str = AS_STRING(currentChunk(vm)->constants.values[const_idx]);

        Value container_val = stack[a];
        Value value_val = stack[c];

        // Handle struct instances
        if (IS_STRUCT_INSTANCE(container_val)) {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(container_val);
            int field_index = find_field_index(instance->schema, key_str);
            if (field_index >= 0) {
                instance->fields[field_index] = value_val;

                // Self-patch: bake field_index into B, switch to IC opcode
                ip[-2] = (uint32_t)(SET_STRUCT_FIELD_IC) | ((uint32_t)REG_A(instr) << 8) | ((uint32_t)field_index << 16) | ((uint32_t)REG_C(instr) << 24);
                DISPATCH();
            }
            STORE_IP(); runtimeError(vm, "Struct '%s' has no field '%s'.",
                         instance->schema->name->chars, key_str->chars);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        // Handle maps
        if (!IS_MAP(container_val)) {
            STORE_IP(); runtimeError(vm, ERR_ONLY_MAPS);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjMap* map = AS_MAP(container_val);
        if (IS_NULL(value_val)) {
            tableDelete(&map->table, key_str);
        } else {
            tableSet(vm, &map->table, key_str, value_val);
        }
        DISPATCH();
    }
    OP(GET_STRUCT_FIELD_IC) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int cached_field = REG_C(instr);

        // Read key from constant pool via trailing index word
        uint32_t const_idx = *ip++;
        ObjString* key_str = AS_STRING(currentChunk(vm)->constants.values[const_idx]);

        Value container_val = stack[b];

        if (IS_STRUCT_INSTANCE(container_val)) {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(container_val);

            // IC hit: verify cached field index maps to the expected field name
            if (cached_field < instance->field_count &&
                instance->schema->field_names[cached_field] == key_str) {
                stack[a] = instance->fields[cached_field];
                DISPATCH();
            }

            // IC miss: full lookup, re-cache
            int field_index = find_field_index(instance->schema, key_str);
            if (field_index >= 0) {
                ip[-2] = (uint32_t)(GET_STRUCT_FIELD_IC) | ((uint32_t)REG_A(instr) << 8) | ((uint32_t)REG_B(instr) << 16) | ((uint32_t)field_index << 24);
                stack[a] = instance->fields[field_index];
                DISPATCH();
            }
            STORE_IP(); runtimeError(vm, "Struct '%s' has no field '%s'.",
                         instance->schema->name->chars, key_str->chars);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        // Not a struct: revert to _L and handle as map
        ip[-2] = (uint32_t)(GET_MAP_PROPERTY_L) | ((uint32_t)REG_A(instr) << 8) | ((uint32_t)REG_B(instr) << 16);

        // Handle maps inline (avoid re-dispatch overhead)
        if (!IS_MAP(container_val)) {
            STORE_IP(); runtimeError(vm, ERR_ONLY_MAPS);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjMap* map = AS_MAP(container_val);
        Value result;
        if (tableGet(&map->table, key_str, &result)) {
            stack[a] = result;
        } else {
            stack[a] = NULL_VAL;
        }
        DISPATCH();
    }
    OP(SET_STRUCT_FIELD_IC) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int cached_field = REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);

        // Read key from constant pool via trailing index word
        uint32_t const_idx = *ip++;
        ObjString* key_str = AS_STRING(currentChunk(vm)->constants.values[const_idx]);

        Value container_val = stack[a];
        Value value_val = stack[c];

        if (IS_STRUCT_INSTANCE(container_val)) {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(container_val);

            // IC hit: verify cached field index maps to the expected field name
            if (cached_field < instance->field_count &&
                instance->schema->field_names[cached_field] == key_str) {
                instance->fields[cached_field] = value_val;
                DISPATCH();
            }

            // IC miss: full lookup, re-cache
            int field_index = find_field_index(instance->schema, key_str);
            if (field_index >= 0) {
                ip[-2] = (uint32_t)(SET_STRUCT_FIELD_IC) | ((uint32_t)REG_A(instr) << 8) | ((uint32_t)field_index << 16) | ((uint32_t)REG_C(instr) << 24);
                instance->fields[field_index] = value_val;
                DISPATCH();
            }
            STORE_IP(); runtimeError(vm, "Struct '%s' has no field '%s'.",
                         instance->schema->name->chars, key_str->chars);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        // Not a struct: revert to _L and handle as map
        ip[-2] = (uint32_t)(SET_MAP_PROPERTY_L) | ((uint32_t)REG_A(instr) << 8) | ((uint32_t)REG_C(instr) << 24);

        // Handle maps inline
        if (!IS_MAP(container_val)) {
            STORE_IP(); runtimeError(vm, ERR_ONLY_MAPS);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjMap* map = AS_MAP(container_val);
        if (IS_NULL(value_val)) {
            tableDelete(&map->table, key_str);
        } else {
            tableSet(vm, &map->table, key_str, value_val);
        }
        DISPATCH();
    }
    OP(NEW_DISPATCHER) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        ObjDispatcher* dispatcher = newDispatcher(vm);
        pushTempRoot(vm, (Obj*)dispatcher);
        stack[a] = OBJ_VAL(dispatcher);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(ADD_OVERLOAD) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Dispatcher register
        int b = CUR_BASE() + REG_B(instr); // Closure register

        Value disp_val = stack[a];
        Value closure_val = stack[b];

        if (!IS_DISPATCHER(disp_val)) {
            STORE_IP(); runtimeError(vm, "ADD_OVERLOAD requires a dispatcher.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        if (!IS_CLOSURE(closure_val)) {
            STORE_IP(); runtimeError(vm, "ADD_OVERLOAD requires a closure.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjDispatcher* dispatcher = AS_DISPATCHER(disp_val);
        ObjClosure* closure = AS_CLOSURE(closure_val);

        if (dispatcher->count >= MAX_OVERLOADS) {
            STORE_IP(); runtimeError(vm, "Too many overloads (max %d).", MAX_OVERLOADS);
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        dispatcher->overloads[dispatcher->count++] = (Obj*)closure;
        DISPATCH();
    }
    OP(NEW_STRUCT) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register
        int bx = REG_Bx(instr);             // Schema constant index

        Value schema_val = vm->chunk->constants.values[bx];
        if (!IS_STRUCT_SCHEMA(schema_val)) {
            STORE_IP(); runtimeError(vm, "NEW_STRUCT requires a struct schema constant.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjStructSchema* schema = AS_STRUCT_SCHEMA(schema_val);
        ObjStructInstance* instance = newStructInstance(vm, schema);
        stack = vm->stack; // GC may have reallocated stack
        // Protect instance before writing to stack (which can trigger GC)
        pushTempRoot(vm, (Obj*)instance);
        stack[a] = OBJ_VAL(instance);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(STRUCT_SPREAD) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // target struct
        int b = CUR_BASE() + REG_B(instr); // source struct to spread
        Value target_val = stack[a];
        Value source_val = stack[b];
        if (!IS_STRUCT_INSTANCE(target_val)) {
            STORE_IP(); runtimeError(vm, "Spread target must be a struct instance.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        if (!IS_STRUCT_INSTANCE(source_val)) {
            STORE_IP(); runtimeError(vm, "Spread source must be a struct instance.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }
        ObjStructInstance* target = AS_STRUCT_INSTANCE(target_val);
        ObjStructInstance* source = AS_STRUCT_INSTANCE(source_val);

        // Check if schemas are compatible - must be the same struct type
        if (target->schema != source->schema) {
            // Fallback for serialized schemas that lose pointer identity
            bool compatible = (target->schema->name == source->schema->name &&
                               target->schema->field_count == source->schema->field_count);
            if (compatible) {
                for (int i = 0; i < target->schema->field_count; i++) {
                    if (target->schema->field_names[i] != source->schema->field_names[i]) {
                        compatible = false;
                        break;
                    }
                }
            }

            if (!compatible) {
                STORE_IP(); runtimeError(vm, "Cannot spread struct '%s' into struct '%s' - incompatible types.",
                            source->schema->name->chars, target->schema->name->chars);
                STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
            }
        }

        // Copy all fields from source to target
        for (int i = 0; i < source->schema->field_count; i++) {
            target->fields[i] = source->fields[i];
        }
        DISPATCH();
    }
    OP(GET_STRUCT_FIELD) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register
        int b = CUR_BASE() + REG_B(instr); // Struct instance register
        int c = REG_C(instr);               // Field index

        Value struct_val = stack[b];

        // Dereference container if it's a reference

        if (!IS_STRUCT_INSTANCE(struct_val)) {
            STORE_IP(); runtimeError(vm, "GET_STRUCT_FIELD requires a struct instance.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjStructInstance* instance = AS_STRUCT_INSTANCE(struct_val);
        if (c < 0 || c >= instance->schema->field_count) {
            STORE_IP(); runtimeError(vm, "Struct field index out of bounds.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        stack[a] = instance->fields[c];
        DISPATCH();
    }
    OP(SET_STRUCT_FIELD) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Struct instance register
        int b = REG_B(instr);               // Field index
        int c = CUR_BASE() + REG_C(instr); // Value register

        Value struct_val = stack[a];
        Value new_value = stack[c];

        // Dereference container if it's a reference

        if (!IS_STRUCT_INSTANCE(struct_val)) {
            STORE_IP(); runtimeError(vm, "SET_STRUCT_FIELD requires a struct instance.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        ObjStructInstance* instance = AS_STRUCT_INSTANCE(struct_val);
        if (b < 0 || b >= instance->schema->field_count) {
            STORE_IP(); runtimeError(vm, "Struct field index out of bounds.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        instance->fields[b] = new_value;
        DISPATCH();
    }
    OP(PRE_INC) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value val_b = stack[b];

        if (!IS_DOUBLE(val_b)) {
            STORE_IP(); runtimeError(vm, "Pre-increment operand must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        Value result = DOUBLE_VAL(AS_DOUBLE(val_b) + 1.0);
        stack[b] = result;
        stack[a] = result;

        DISPATCH();
    }
    OP(POST_INC) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value val_b = stack[b];

        if (!IS_DOUBLE(val_b)) {
            STORE_IP(); runtimeError(vm, "Post-increment operand must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        double old_value = AS_DOUBLE(val_b);
        stack[b] = DOUBLE_VAL(old_value + 1.0);
        stack[a] = DOUBLE_VAL(old_value);

        DISPATCH();
    }
    OP(PRE_DEC) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value val_b = stack[b];

        if (!IS_DOUBLE(val_b)) {
            STORE_IP(); runtimeError(vm, "Pre-decrement operand must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        Value result = DOUBLE_VAL(AS_DOUBLE(val_b) - 1.0);
        stack[b] = result;
        stack[a] = result;

        DISPATCH();
    }
    OP(POST_DEC) {
        uint32_t instr = ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value val_b = stack[b];

        if (!IS_DOUBLE(val_b)) {
            STORE_IP(); runtimeError(vm, "Post-decrement operand must be a number.");
            STORE_STATE(); return INTERPRET_RUNTIME_ERROR;
        }

        double old_value = AS_DOUBLE(val_b);
        stack[b] = DOUBLE_VAL(old_value - 1.0);
        stack[a] = DOUBLE_VAL(old_value);

        DISPATCH();
    }
#undef OP
#undef DISPATCH
#undef CUR_BASE
#undef BINARY_OP
#undef BINARY_COMPARE
#undef STORE_IP
#undef STORE_STATE
#undef LOAD_STATE
}

InterpretResult runVM(VM* vm) {
    return run(vm);
}

InterpretResult runChunk(VM* vm, Chunk* chunk) {
    vm->chunk = chunk;
    vm->ip = vm->chunk->code;

    // For top-level chunk execution (no call frame), conservatively set stack_top
    // to protect registers used by the chunk. Since we don't track max_regs for chunks,
    // we use a reasonable upper bound (128 registers should be more than enough for
    // most top-level scripts).
    if (vm->frame_count == 0) {
        vm->stack_top = 128;  // Conservative estimate for top-level chunk registers
    }

#ifdef DEBUG_PRINT_CODE
    printf("== Executing Chunk ==\n");
    disassembleChunk(chunk, "Bytecode");
#endif

    return run(vm);
}

bool zym_call_prepare(VM* vm, const char* functionName, int arity) {
    // Mangle the name provided by the C host to match the compiler's internal name.
    char mangled[256];
    sprintf(mangled, "%s@%d", functionName, arity);

    Value func_val;
    ObjString* name_obj = copyString(vm, mangled, strlen(mangled));

    if (!globalGet(vm, name_obj, &func_val) || !IS_CLOSURE(func_val)) {
        fprintf(stderr, "Error: Function '%s' with arity %d not found.\n", functionName, arity);
        return false;
    }

    // Reset the API stack and place the function at the base.
    vm->api_stack_top = 0;
    vm->stack[vm->api_stack_top] = func_val;
    return true;
}

InterpretResult zym_call_execute(VM* vm, int argCount) {
    // base: function at stack[api_stack_top - argCount]
    int frame_base = vm->api_stack_top - argCount;

    Value callee = vm->stack[frame_base];
    if (!IS_CLOSURE(callee)) {
        runtimeError(vm, "Can only call functions.");
        vm->api_stack_top = frame_base;
        return INTERPRET_RUNTIME_ERROR;
    }

    ObjClosure* closure = AS_CLOSURE(callee);
    ObjFunction* function = closure->function;

    if (argCount != function->arity) {
        runtimeError(vm, "Expected %d arguments but got %d.", function->arity, argCount);
        vm->api_stack_top = frame_base;
        return INTERPRET_RUNTIME_ERROR;
    }

    if (vm->frame_count == FRAMES_MAX) {
        runtimeError(vm, "Stack overflow.");
        vm->api_stack_top = frame_base;
        return INTERPRET_RUNTIME_ERROR;
    }

    // Calculate required stack size for this call
    int needed_top = frame_base + function->max_regs;

    // Grow stack if needed (same logic as OP(CALL))
    if (needed_top > vm->stack_capacity) {
        if (needed_top > STACK_MAX) {
            runtimeError(vm, "Stack overflow: function needs %d slots, max is %d.", needed_top, STACK_MAX);
            vm->api_stack_top = frame_base;
            return INTERPRET_RUNTIME_ERROR;
        }

        int new_capacity = vm->stack_capacity;
        while (new_capacity < needed_top) {
            new_capacity *= 2;
            if (new_capacity > STACK_MAX) new_capacity = STACK_MAX;
        }

        Value* old_stack = vm->stack;
        Value* new_stack = (Value*)reallocate(vm, vm->stack, sizeof(Value) * vm->stack_capacity, sizeof(Value) * new_capacity);

        // Initialize new slots
        for (int i = vm->stack_capacity; i < new_capacity; i++) {
            new_stack[i] = NULL_VAL;
        }

        vm->stack = new_stack;
        vm->stack_capacity = new_capacity;

        // Update all references that point to the old stack
        updateStackReferences(vm, old_stack, new_stack);
    }

    // Update stack_top to track highest used slot
    if (needed_top > vm->stack_top) {
        vm->stack_top = needed_top;
    }

    // Push frame just like OP(CALL)
    CallFrame* frame = &vm->frames[vm->frame_count++];
    frame->closure      = closure;
    frame->stack_base   = frame_base;
    frame->flags        = 0;

    // On return, resume at the API trampoline, not bytecode.
    frame->ip           = vm->api_trampoline.code;
    frame->caller_chunk = &vm->api_trampoline;

    vm->current_frame = frame;
    vm->cur_base = frame_base;
    uint32_t* saved_ip = vm->ip;
    Chunk* saved_chunk = vm->chunk;
    int saved_cur_base = vm->cur_base;
    CallFrame* saved_current_frame = vm->current_frame;

    // Enter the callee
    vm->chunk = &function->chunk;
    vm->ip    = function->chunk.code;

    InterpretResult result = run(vm);

    vm->ip    = saved_ip;
    vm->chunk = saved_chunk;
    vm->cur_base = saved_cur_base;
    vm->current_frame = saved_current_frame;

    // Result is placed in stack[frame_base] by OP(RET); expose that at API top.
    vm->api_stack_top = frame_base;
    return result;
}

Value zym_call_getResult(VM* vm) {
    // The result of the last C API call is at the top of the API stack.
    return vm->stack[vm->api_stack_top];
}