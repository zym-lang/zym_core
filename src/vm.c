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
#include "./zym.h"
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

    vm->resume_depth = 0;
    vm->with_prompt_depth = 0;

    vm->gc_enabled = true;

    // Register core modules (Cont, Preemption, GC) as part of VM init
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

    free(vm->gray_stack);
    free(vm->temp_roots);

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
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    if (vm->frame_count > 0) {
        CallFrame* cur = &vm->frames[vm->frame_count - 1];
        Chunk* cur_chunk = cur->closure->function ? cur->closure->function->chunk : vm->chunk;
        int cur_line = line_at_ip(cur_chunk, vm->ip);

        if (cur->closure->function && cur->closure->function->module_name) {
            fprintf(stderr, "[%s] line %d\n", cur->closure->function->module_name->chars, cur_line);
        } else if (vm->entry_file) {
            fprintf(stderr, "[%s] line %d\n", vm->entry_file->chars, cur_line);
        } else {
            fprintf(stderr, "[line %d]\n", cur_line);
        }
    } else {
        int line = line_at_ip(vm->chunk, vm->ip);
        if (vm->entry_file) {
            fprintf(stderr, "[%s] line %d\n", vm->entry_file->chars, line);
        } else {
            fprintf(stderr, "[line %d]\n", line);
        }
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

    for (Obj* obj = vm->objects; obj != NULL; obj = obj->next) {
        if (obj->type == OBJ_REFERENCE) {
            ObjReference* ref = (ObjReference*)obj;
            if (ref->ref_type == REF_LOCAL) {
                if (ref->as.local.location >= old_stack &&
                    ref->as.local.location < old_stack + vm->stack_capacity) {
                    ref->as.local.location += offset;
                }
            }
        }
    }

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
    for (int i = 0; i < closing_count; i++) {
        ObjUpvalue* upvalue = closing[i].upvalue;
        if (IS_REFERENCE(upvalue->closed)) {
            ObjReference* ref = AS_REFERENCE(upvalue->closed);
            if (ref->ref_type == REF_LOCAL && ref->as.local.location >= last) {
                bool found = false;
                for (int j = 0; j < closing_count; j++) {
                    if (closing[j].old_location == ref->as.local.location) {
                        ref->ref_type = REF_UPVALUE;
                        ref->as.upvalue.upvalue = closing[j].upvalue;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    ObjUpvalue* new_upvalue = captureUpvalue(vm, ref->as.local.location);

                    new_upvalue->closed = *new_upvalue->location;
                    new_upvalue->location = &new_upvalue->closed;

                    if (vm->open_upvalues == new_upvalue) {
                        vm->open_upvalues = new_upvalue->next;
                    } else {
                        ObjUpvalue* prev = vm->open_upvalues;
                        while (prev != NULL && prev->next != new_upvalue) {
                            prev = prev->next;
                        }
                        if (prev != NULL) {
                            prev->next = new_upvalue->next;
                        }
                    }

                    ref->ref_type = REF_UPVALUE;
                    ref->as.upvalue.upvalue = new_upvalue;
                }
            }
        }
    }
    #undef MAX_CLOSING_UPVALUES
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

static bool derefContainer(VM* vm, Value* container, const char* context) {
    if (IS_REFERENCE(*container)) {
        if (!dereferenceValue(vm, *container, container)) {
            runtimeError(vm, "Dead reference: cannot %s on dead reference.", context);
            return false;
        }
    }
    return true;
}

static bool derefOperand(VM* vm, Value* val, const char* operation) {
    if (IS_REFERENCE(*val) || (IS_OBJ(*val) && IS_NATIVE_REFERENCE(*val))) {
        if (!dereferenceValue(vm, *val, val)) {
            runtimeError(vm, "Dead reference in %s.", operation);
            return false;
        }
    }
    return true;
}

static ObjString* keyToString(VM* vm, Value key_val) {
    if (IS_STRING(key_val)) {
        return AS_STRING(key_val);
    } else if (IS_DOUBLE(key_val)) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%g", AS_DOUBLE(key_val));
        return copyString(vm, buffer, strlen(buffer));
    }
    return NULL;
}

static bool referencesGlobal(VM* vm, ObjReference* ref, ObjString* target_global, int depth) {
    if (depth >= 64) {
        return false;
    }

    switch (ref->ref_type) {
        case REF_GLOBAL:
            if (ref->as.global.global_name == target_global) {
                return true;
            }
            Value global_value;
            if (globalGet(vm, ref->as.global.global_name, &global_value) && IS_REFERENCE(global_value)) {
                return referencesGlobal(vm, AS_REFERENCE(global_value), target_global, depth + 1);
            }
            return false;

        case REF_LOCAL:
            if (IS_REFERENCE(*ref->as.local.location)) {
                return referencesGlobal(vm, AS_REFERENCE(*ref->as.local.location), target_global, depth + 1);
            }
            return false;

        case REF_INDEX: {
            Value element_value;
            if (dereferenceValue(vm, OBJ_VAL(ref), &element_value) && IS_REFERENCE(element_value)) {
                return referencesGlobal(vm, AS_REFERENCE(element_value), target_global, depth + 1);
            }
            return false;
        }

        case REF_PROPERTY: {
            Value property_value;
            if (dereferenceValue(vm, OBJ_VAL(ref), &property_value) && IS_REFERENCE(property_value)) {
                return referencesGlobal(vm, AS_REFERENCE(property_value), target_global, depth + 1);
            }
            return false;
        }

        case REF_UPVALUE:
            if (ref->as.upvalue.upvalue && ref->as.upvalue.upvalue->location) {
                if (IS_REFERENCE(*ref->as.upvalue.upvalue->location)) {
                    return referencesGlobal(vm, AS_REFERENCE(*ref->as.upvalue.upvalue->location), target_global, depth + 1);
                }
            }
            return false;

        default:
            return false;
    }
}

static bool referencesLocal(VM* vm, ObjReference* ref, Value* target_location, int depth) {
    if (depth >= 64) {
        return false;
    }

    switch (ref->ref_type) {
        case REF_LOCAL:
            if (ref->as.local.location == target_location) {
                return true;
            }
            if (IS_REFERENCE(*ref->as.local.location)) {
                return referencesLocal(vm, AS_REFERENCE(*ref->as.local.location), target_location, depth + 1);
            }
            return false;

        case REF_GLOBAL: {
            Value global_value;
            if (globalGet(vm, ref->as.global.global_name, &global_value) && IS_REFERENCE(global_value)) {
                return referencesLocal(vm, AS_REFERENCE(global_value), target_location, depth + 1);
            }
            return false;
        }

        case REF_INDEX: {
            Value element_value;
            if (dereferenceValue(vm, OBJ_VAL(ref), &element_value) && IS_REFERENCE(element_value)) {
                return referencesLocal(vm, AS_REFERENCE(element_value), target_location, depth + 1);
            }
            return false;
        }

        case REF_PROPERTY: {
            Value property_value;
            if (dereferenceValue(vm, OBJ_VAL(ref), &property_value) && IS_REFERENCE(property_value)) {
                return referencesLocal(vm, AS_REFERENCE(property_value), target_location, depth + 1);
            }
            return false;
        }

        case REF_UPVALUE:
            if (ref->as.upvalue.upvalue && ref->as.upvalue.upvalue->location) {
                if (IS_REFERENCE(*ref->as.upvalue.upvalue->location)) {
                    return referencesLocal(vm, AS_REFERENCE(*ref->as.upvalue.upvalue->location), target_location, depth + 1);
                }
            }
            return false;

        default:
            return false;
    }
}

static Value flattenReferenceHelper(VM* vm, ObjReference* target_ref, ObjReference** visited_refs, int depth) {
    if (depth >= 64) {
        runtimeError(vm, "Reference chain too deep (max 64 levels) - possible circular reference.");
        return NULL_VAL;
    }

    for (int i = 0; i < depth; i++) {
        if (visited_refs[i] == target_ref) {
            runtimeError(vm, "Circular reference detected: cannot create reference that forms a cycle.");
            return NULL_VAL;
        }
    }

    visited_refs[depth] = target_ref;

    Value target_value;
    if (dereferenceValue(vm, OBJ_VAL(target_ref), &target_value)) {
        if (IS_REFERENCE(target_value)) {
            return flattenReferenceHelper(vm, AS_REFERENCE(target_value), visited_refs, depth + 1);
        }
    }
    ObjReference* ref = NULL;
    switch (target_ref->ref_type) {
        case REF_LOCAL:
            ref = newReference(vm, target_ref->as.local.location);
            break;
        case REF_GLOBAL:
            ref = newGlobalReference(vm, target_ref->as.global.global_name);
            break;
        case REF_INDEX:
            ref = newIndexReference(vm, target_ref->as.index.container, target_ref->as.index.index);
            break;
        case REF_PROPERTY:
            ref = newPropertyReference(vm, target_ref->as.property.container, target_ref->as.property.key);
            break;
        case REF_UPVALUE:
            ref = newUpvalueReference(vm, target_ref->as.upvalue.upvalue);
            break;
    }
    return ref ? OBJ_VAL(ref) : NULL_VAL;
}

static Value flattenReference(VM* vm, ObjReference* target_ref) {
    ObjReference* visited_refs[64];
    return flattenReferenceHelper(vm, target_ref, visited_refs, 0);
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

static bool writeThruListElement(VM* vm, ObjList* list, int idx, Value new_value) {
    Value current = list->items.values[idx];
    if (IS_REFERENCE(current)) {
        ObjReference* inner_ref = AS_REFERENCE(current);
        switch (inner_ref->ref_type) {
            case REF_LOCAL:
                *inner_ref->as.local.location = new_value;
                return true;
            case REF_GLOBAL:
                if (!globalSet(vm, inner_ref->as.global.global_name, new_value)) {
                    runtimeError(vm, "Failed to write through global reference in list element.");
                    return false;
                }
                return true;
            case REF_UPVALUE:
                if (!validateUpvalue(vm, inner_ref->as.upvalue.upvalue, "writeThruListElement")) {
                    return false;
                }
                *inner_ref->as.upvalue.upvalue->location = new_value;
                return true;
            case REF_INDEX:
            case REF_PROPERTY:
                runtimeError(vm, ERR_NESTED_COLLECTION_REFS);
                return false;
        }
    }
    list->items.values[idx] = new_value;
    return true;
}

static bool writeThruMapField(VM* vm, ObjMap* map, ObjString* key_str, Value new_value) {
    Value current;
    if (tableGet(map->table, key_str, &current) && IS_REFERENCE(current)) {
        ObjReference* inner_ref = AS_REFERENCE(current);
        switch (inner_ref->ref_type) {
            case REF_LOCAL:
                *inner_ref->as.local.location = new_value;
                return true;
            case REF_GLOBAL:
                if (!globalSet(vm, inner_ref->as.global.global_name, new_value)) {
                    runtimeError(vm, "Failed to write through global reference in map field.");
                    return false;
                }
                return true;
            case REF_UPVALUE:
                if (!validateUpvalue(vm, inner_ref->as.upvalue.upvalue, "writeThruMapField")) {
                    return false;
                }
                *inner_ref->as.upvalue.upvalue->location = new_value;
                return true;
            case REF_INDEX:
            case REF_PROPERTY:
                runtimeError(vm, ERR_NESTED_COLLECTION_REFS);
                return false;
        }
    }
    tableSet(vm, map->table, key_str, new_value);
    return true;
}

static bool writeThruReference(VM* vm, ObjReference* ref, Value new_value, bool recursive) {
    switch (ref->ref_type) {
        case REF_LOCAL: {
            if (recursive) {
                Value current = *ref->as.local.location;
                if (IS_REFERENCE(current)) {
                    return writeReferenceValue(vm, current, new_value);
                }
            }
            *ref->as.local.location = new_value;
            return true;
        }

        case REF_GLOBAL: {
            if (recursive) {
                Value current;
                if (globalGet(vm, ref->as.global.global_name, &current) && IS_REFERENCE(current)) {
                    return writeReferenceValue(vm, current, new_value);
                }
            }
            if (!globalSet(vm, ref->as.global.global_name, new_value)) {
                runtimeError(vm, "Failed to write global variable '%.*s'.",
                    ref->as.global.global_name->length, ref->as.global.global_name->chars);
                return false;
            }
            return true;
        }

        case REF_UPVALUE: {
            if (recursive) {
                Value current = *ref->as.upvalue.upvalue->location;
                if (IS_REFERENCE(current)) {
                    return writeReferenceValue(vm, current, new_value);
                }
            }
            if (!validateUpvalue(vm, ref->as.upvalue.upvalue, "writeThruReference")) {
                return false;
            }
            *ref->as.upvalue.upvalue->location = new_value;
            return true;
        }

        case REF_INDEX: {
            Value container = ref->as.index.container;
            Value index = ref->as.index.index;

            if (!IS_OBJ(container)) {
                runtimeError(vm, "Index reference container is not an object.");
                return false;
            }

            if (IS_LIST(container)) {
                if (!IS_DOUBLE(index)) {
                    runtimeError(vm, ERR_LIST_INDEX_TYPE);
                    return false;
                }
                ObjList* list = AS_LIST(container);
                int idx = (int)AS_DOUBLE(index);
                if (!validateListIndex(vm, list, idx, "writeThruReference")) {
                    return false;
                }
                if (recursive) {
                    return writeThruListElement(vm, list, idx, new_value);
                }
                list->items.values[idx] = new_value;
                return true;
            } else if (IS_MAP(container)) {
                ObjMap* map = AS_MAP(container);
                ObjString* key_str = keyToString(vm, index);
                if (!key_str) {
                    runtimeError(vm, ERR_MAP_KEYS_TYPE);
                    return false;
                }
                if (recursive) {
                    return writeThruMapField(vm, map, key_str, new_value);
                }
                tableSet(vm, map->table, key_str, new_value);
                return true;
            } else {
                runtimeError(vm, "Index reference container must be a list or map.");
                return false;
            }
        }

        case REF_PROPERTY: {
            Value container = ref->as.property.container;
            Value key = ref->as.property.key;

            if (!IS_OBJ(container) || (!IS_MAP(container) && !IS_STRUCT_INSTANCE(container))) {
                runtimeError(vm, "Property reference container is not a map or struct.");
                return false;
            }

            ObjString* key_str = keyToString(vm, key);
            if (!key_str) {
                runtimeError(vm, ERR_MAP_KEY_TYPE);
                return false;
            }

            if (IS_MAP(container)) {
                ObjMap* map = AS_MAP(container);
                if (recursive) {
                    return writeThruMapField(vm, map, key_str, new_value);
                }
                tableSet(vm, map->table, key_str, new_value);
                return true;
            } else if (IS_STRUCT_INSTANCE(container)) {
                ObjStructInstance* instance = AS_STRUCT_INSTANCE(container);
                Value index_val;
                if (!tableGet(instance->schema->field_to_index, key_str, &index_val)) {
                    runtimeError(vm, "Struct field '%.*s' does not exist.", key_str->length, key_str->chars);
                    return false;
                }
                int field_index = (int)AS_DOUBLE(index_val);

                // For recursive writes, check if the field itself contains a reference
                if (recursive && IS_REFERENCE(instance->fields[field_index])) {
                    return writeReferenceValue(vm, instance->fields[field_index], new_value);
                }

                instance->fields[field_index] = new_value;
                return true;
            }
            return false;
        }
    }

    return false;
}

static bool processParamQualifiers(VM* vm, ObjFunction* function, int callee_slot, int arg_count, bool is_tco) {
    // Fast path using qualifier signature
    switch (function->qualifier_sig) {
        case QUAL_SIG_ALL_NORMAL_NO_REFS:
            // Fastest path: no arguments to process
            return true;
            
        case QUAL_SIG_ALL_NORMAL:
            // Fast path: all PARAM_NORMAL, just check for references to dereference
            for (int i = 0; i < arg_count; i++) {
                int arg_slot = callee_slot + 1 + i;
                Value arg_value = vm->stack[arg_slot];
                if (IS_REFERENCE(arg_value)) {
                    Value deref_value;
                    if (!dereferenceValue(vm, arg_value, &deref_value)) {
                        deref_value = NULL_VAL;
                    }
                    vm->stack[arg_slot] = deref_value;
                }
            }
            return true;
            
        case QUAL_SIG_HAS_QUALIFIERS:
            // Slow path: has non-NORMAL qualifiers, full processing required
            break;
            
        default:
            // Fallback for safety - treat as ALL_NORMAL if param_qualifiers is NULL
            if (function->param_qualifiers == NULL) {
                for (int i = 0; i < arg_count; i++) {
                    int arg_slot = callee_slot + 1 + i;
                    Value arg_value = vm->stack[arg_slot];
                    if (IS_REFERENCE(arg_value)) {
                        Value deref_value;
                        if (!dereferenceValue(vm, arg_value, &deref_value)) {
                            deref_value = NULL_VAL;
                        }
                        vm->stack[arg_slot] = deref_value;
                    }
                }
                return true;
            }
            break;
    }

    // Full qualifier processing (QUAL_SIG_HAS_QUALIFIERS) - param_qualifiers is guaranteed non-NULL here
    for (int i = 0; i < arg_count; i++) {
        int arg_slot = callee_slot + 1 + i;
        ParamQualifier qualifier = (ParamQualifier)function->param_qualifiers[i];
        Value arg_value = vm->stack[arg_slot];

        if (IS_REFERENCE(arg_value) || (IS_OBJ(arg_value) && IS_NATIVE_REFERENCE(arg_value))) {
            // Argument is a reference or native reference (from dynamic call with variable)
            if (qualifier == PARAM_REF) {
                // For ref parameters, flatten if the arg is a regular (non-flattening) reference
                // pointing to another reference. Native references don't need flattening.
                if (IS_REFERENCE(arg_value)) {
                    ObjReference* arg_ref = AS_REFERENCE(arg_value);
                    if (arg_ref->ref_type == REF_GLOBAL || arg_ref->ref_type == REF_LOCAL ||
                        arg_ref->ref_type == REF_PROPERTY || arg_ref->ref_type == REF_INDEX) {
                        Value target_value;
                        if (dereferenceValue(vm, arg_value, &target_value)) {
                            if (IS_REFERENCE(target_value)) {
                                // The reference points to another reference - use that one
                                vm->stack[arg_slot] = target_value;
                            }
                        }
                    }
                }
                // Keep the reference (possibly flattened) or native reference as-is
                continue;
            } else if (qualifier == PARAM_VAL) {
                // Dereference and clone
                Value deref_value;
                if (!dereferenceValue(vm, arg_value, &deref_value)) {
                    deref_value = NULL_VAL;
                }
                Value cloned = cloneValue(vm, deref_value);
                vm->stack[arg_slot] = cloned;
            } else if (qualifier == PARAM_CLONE) {
                // PARAM_CLONE: The compiler emits DEEP_CLONE_VALUE before the call,
                // but if the argument is a reference (from dynamic dispatch), we need
                // to handle it here. Dereference and deep clone.
                Value deref_value;
                if (!dereferenceValue(vm, arg_value, &deref_value)) {
                    deref_value = NULL_VAL;
                }
                Value cloned = deepCloneValue(vm, deref_value);
                vm->stack[arg_slot] = cloned;
            } else if (qualifier == PARAM_SLOT) {
                // PARAM_SLOT: keep the value as-is (whether ref or not)
                continue;
            } else if (qualifier == PARAM_TYPEOF) {
                // PARAM_TYPEOF: get the type name as a string WITHOUT dereferencing
                // This allows detecting if the argument is a reference
                const char* type_name = NULL;
                if (IS_DOUBLE(arg_value)) {
                    type_name = "number";
                } else if (IS_BOOL(arg_value)) {
                    type_name = "boolean";
                } else if (IS_NULL(arg_value)) {
                    type_name = "null";
                } else if (IS_ENUM(arg_value)) {
                    type_name = "enum";
                } else if (IS_OBJ(arg_value)) {
                    Obj* obj = AS_OBJ(arg_value);
                    switch (obj->type) {
                        case OBJ_STRING: type_name = "string"; break;
                        case OBJ_FUNCTION:
                        case OBJ_CLOSURE:
                        case OBJ_DISPATCHER: type_name = "function"; break;
                        case OBJ_NATIVE_FUNCTION: type_name = "native_function"; break;
                        case OBJ_NATIVE_CLOSURE: type_name = "native_closure"; break;
                        case OBJ_LIST: type_name = "list"; break;
                        case OBJ_MAP: type_name = "map"; break;
                        case OBJ_REFERENCE: type_name = "reference"; break;
                        case OBJ_NATIVE_REFERENCE: type_name = "native_reference"; break;
                        case OBJ_NATIVE_CONTEXT: type_name = "native_context"; break;
                        case OBJ_STRUCT_SCHEMA: type_name = "struct_schema"; break;
                        case OBJ_STRUCT_INSTANCE: type_name = "struct"; break;
                        case OBJ_ENUM_SCHEMA: type_name = "enum_schema"; break;
                        case OBJ_UPVALUE: type_name = "upvalue"; break;
                        case OBJ_INT64: type_name = "number"; break;
                        default: type_name = "unknown"; break;
                    }
                } else {
                    type_name = "unknown";
                }

                ObjString* type_string = copyString(vm, type_name, strlen(type_name));
                vm->stack[arg_slot] = OBJ_VAL(type_string);
            } else {
                // PARAM_NORMAL: dereference to get the value
                Value deref_value;
                if (!dereferenceValue(vm, arg_value, &deref_value)) {
                    deref_value = NULL_VAL;
                }
                vm->stack[arg_slot] = deref_value;
            }
        } else {
            // Argument is a direct value (not a reference)
            if (qualifier == PARAM_VAL) {
                // Val: deep clone, preserving refs (refs are first-class values)
                Value cloned = cloneValue(vm, arg_value);
                vm->stack[arg_slot] = cloned;
            } else if (qualifier == PARAM_CLONE) {
                // PARAM_CLONE: The compiler already emitted DEEP_CLONE_VALUE,
                // so the value is already cloned. Just pass it through.
                continue;
            } else if (qualifier == PARAM_SLOT) {
                // PARAM_SLOT: keep the value as-is
                continue;
            } else if (qualifier == PARAM_TYPEOF) {
                // PARAM_TYPEOF: get the type name as a string
                const char* type_name = NULL;
                if (IS_DOUBLE(arg_value)) {
                    type_name = "number";
                } else if (IS_BOOL(arg_value)) {
                    type_name = "boolean";
                } else if (IS_NULL(arg_value)) {
                    type_name = "null";
                } else if (IS_ENUM(arg_value)) {
                    type_name = "enum";
                } else if (IS_OBJ(arg_value)) {
                    Obj* obj = AS_OBJ(arg_value);
                    switch (obj->type) {
                        case OBJ_STRING: type_name = "string"; break;
                        case OBJ_FUNCTION:
                        case OBJ_CLOSURE:
                        case OBJ_DISPATCHER: type_name = "function"; break;
                        case OBJ_NATIVE_FUNCTION: type_name = "native_function"; break;
                        case OBJ_NATIVE_CLOSURE: type_name = "native_closure"; break;
                        case OBJ_LIST: type_name = "list"; break;
                        case OBJ_MAP: type_name = "map"; break;
                        case OBJ_REFERENCE: type_name = "reference"; break;
                        case OBJ_NATIVE_REFERENCE: type_name = "native_reference"; break;
                        case OBJ_NATIVE_CONTEXT: type_name = "native_context"; break;
                        case OBJ_STRUCT_SCHEMA: type_name = "struct_schema"; break;
                        case OBJ_STRUCT_INSTANCE: type_name = "struct"; break;
                        case OBJ_ENUM_SCHEMA: type_name = "enum_schema"; break;
                        case OBJ_UPVALUE: type_name = "upvalue"; break;
                        case OBJ_INT64: type_name = "number"; break;
                        default: type_name = "unknown"; break;
                    }
                } else {
                    type_name = "unknown";
                }

                ObjString* type_string = copyString(vm, type_name, strlen(type_name));
                vm->stack[arg_slot] = OBJ_VAL(type_string);
            } else if (qualifier == PARAM_NORMAL) {
                // Normal: pass by reference (shared pointer)
                // Arrays/maps are shared, no cloning needed
                // The value is already in the argument slot
                continue;
            } else if (qualifier == PARAM_REF) {
                // PARAM_REF with non-reference arg
                if (is_tco) {
                    // For TCO, defer creation until after argument move
                    continue;
                }
                // Create a temporary reference
                // Save the original value first, then create ref pointing to temp slot
                Value original_value = vm->stack[arg_slot];
                int temp_slot = vm->stack_top++;
                vm->stack[temp_slot] = original_value;
                // Use safe version that recomputes pointer after allocation
                ObjReference* temp_ref = newStackSlotReference(vm, temp_slot);
                // Protect the reference before writing to stack (which can trigger GC)
                pushTempRoot(vm, (Obj*)temp_ref);
                vm->stack[arg_slot] = OBJ_VAL(temp_ref);
                popTempRoot(vm);
            }
        }
    }
    return true;
}

// Helper: Handle PARAM_REF with non-reference args after TCO argument move
static bool processParamRefAfterMove(VM* vm, ObjFunction* function, int frame_base, int arg_count) {
    if (function->param_qualifiers == NULL) {
        return true;
    }

    for (int i = 0; i < arg_count; i++) {
        ParamQualifier qualifier = (ParamQualifier)function->param_qualifiers[i];
        int final_slot = frame_base + 1 + i;
        Value arg_value = vm->stack[final_slot];

        if (qualifier == PARAM_REF && !IS_REFERENCE(arg_value)) {
            // Create a temp slot for the value, then create reference to that slot
            int temp_slot = vm->stack_top++;
            vm->stack[temp_slot] = arg_value;
            // Use safe version that recomputes pointer after allocation
            ObjReference* temp_ref = newStackSlotReference(vm, temp_slot);
            // Protect the reference before writing to stack (which can trigger GC)
            pushTempRoot(vm, (Obj*)temp_ref);
            vm->stack[final_slot] = OBJ_VAL(temp_ref);
            popTempRoot(vm);
        }
    }
    return true;
}

// Helper: Recursively protect REF_LOCAL references in returned values by converting them to REF_UPVALUE
// This prevents dangling pointers when returning containers (arrays/maps) that contain local references
void protectLocalRefsInValue(VM* vm, Value value, Value* frame_start) {
    if (IS_REFERENCE(value)) {
        ObjReference* ref = AS_REFERENCE(value);
        if (ref->ref_type == REF_LOCAL) {
            // Check if this reference points into the frame being popped
            if (ref->as.local.location >= frame_start) {
                // Capture the referenced local as an upvalue
                ObjUpvalue* upvalue = captureUpvalue(vm, ref->as.local.location);
                // Convert REF_LOCAL to REF_UPVALUE
                ref->ref_type = REF_UPVALUE;
                ref->as.upvalue.upvalue = upvalue;
            }
        }
    } else if (IS_OBJ(value)) {
        Obj* obj = AS_OBJ(value);
        switch (obj->type) {
            case OBJ_LIST: {
                ObjList* list = (ObjList*)obj;
                for (int i = 0; i < list->items.count; i++) {
                    protectLocalRefsInValue(vm, list->items.values[i], frame_start);
                }
                break;
            }
            case OBJ_MAP: {
                ObjMap* map = (ObjMap*)obj;
                Table* table = map->table;
                for (int i = 0; i < table->capacity; i++) {
                    Entry* entry = &table->entries[i];
                    if (entry->key != NULL) {
                        protectLocalRefsInValue(vm, entry->value, frame_start);
                    }
                }
                break;
            }
            case OBJ_STRUCT_INSTANCE: {
                ObjStructInstance* instance = (ObjStructInstance*)obj;
                for (int i = 0; i < instance->field_count; i++) {
                    protectLocalRefsInValue(vm, instance->fields[i], frame_start);
                }
                break;
            }
            default:
                // Other object types don't contain nested values that need protection
                break;
        }
    }
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
        JUMP_ENTRY(SMART_TAIL_CALL),
        JUMP_ENTRY(SMART_TAIL_CALL_SELF),
        JUMP_ENTRY(RET),
        JUMP_ENTRY(DEFINE_GLOBAL),
        JUMP_ENTRY(GET_GLOBAL),
        JUMP_ENTRY(GET_GLOBAL_CACHED),
        JUMP_ENTRY(SET_GLOBAL),
        JUMP_ENTRY(SET_GLOBAL_CACHED),
        JUMP_ENTRY(SLOT_SET_GLOBAL),
        JUMP_ENTRY(CLOSURE),
        JUMP_ENTRY(GET_UPVALUE),
        JUMP_ENTRY(SET_UPVALUE),
        JUMP_ENTRY(SLOT_SET_UPVALUE),
        JUMP_ENTRY(CLOSE_UPVALUE),
        JUMP_ENTRY(CLOSE_FRAME_UPVALUES),
        JUMP_ENTRY(NEW_LIST),
        JUMP_ENTRY(LIST_APPEND),
        JUMP_ENTRY(LIST_SPREAD),
        JUMP_ENTRY(GET_SUBSCRIPT),
        JUMP_ENTRY(SET_SUBSCRIPT),
        JUMP_ENTRY(SLOT_SET_SUBSCRIPT),
        JUMP_ENTRY(NEW_MAP),
        JUMP_ENTRY(MAP_SET),
        JUMP_ENTRY(MAP_SPREAD),
        JUMP_ENTRY(GET_MAP_PROPERTY),
        JUMP_ENTRY(SET_MAP_PROPERTY),
        JUMP_ENTRY(SLOT_SET_MAP_PROPERTY),
        JUMP_ENTRY(NEW_DISPATCHER),
        JUMP_ENTRY(ADD_OVERLOAD),
        JUMP_ENTRY(CLONE_VALUE),
        JUMP_ENTRY(DEEP_CLONE_VALUE),
        JUMP_ENTRY(MAKE_REF),
        JUMP_ENTRY(SLOT_MAKE_REF),
        JUMP_ENTRY(MAKE_GLOBAL_REF),
        JUMP_ENTRY(SLOT_MAKE_GLOBAL_REF),
        JUMP_ENTRY(MAKE_UPVALUE_REF),
        JUMP_ENTRY(MAKE_INDEX_REF),
        JUMP_ENTRY(SLOT_MAKE_INDEX_REF),
        JUMP_ENTRY(MAKE_PROPERTY_REF),
        JUMP_ENTRY(SLOT_MAKE_PROPERTY_REF),
        JUMP_ENTRY(DEREF_GET),
        JUMP_ENTRY(DEREF_SET),
        JUMP_ENTRY(SLOT_DEREF_SET),
        JUMP_ENTRY(NEW_STRUCT),
        JUMP_ENTRY(STRUCT_SPREAD),
        JUMP_ENTRY(GET_STRUCT_FIELD),
        JUMP_ENTRY(SET_STRUCT_FIELD),
        JUMP_ENTRY(SLOT_SET_STRUCT_FIELD),
        JUMP_ENTRY(PRE_INC),
        JUMP_ENTRY(POST_INC),
        JUMP_ENTRY(PRE_DEC),
        JUMP_ENTRY(POST_DEC),
        JUMP_ENTRY(TYPEOF),
        JUMP_ENTRY(PUSH_PROMPT),
        JUMP_ENTRY(POP_PROMPT),
        JUMP_ENTRY(CAPTURE),
        JUMP_ENTRY(RESUME),
        JUMP_ENTRY(ABORT),
    };
#undef JUMP_ENTRY

#define OP(c) CASE_##c:
#define DISPATCH() do { \
    if (vm->preemption_enabled && --vm->yield_budget <= 0) { \
        vm->yield_budget = vm->default_timeslice; \
        if (vm->preempt_requested) { \
            vm->preempt_requested = false; \
            return INTERPRET_YIELD; \
        } \
    } \
    goto *dispatch_table[OPCODE(*vm->ip++)]; \
} while(0)
#define CUR_BASE() (vm->frame_count == 0 ? 0 : vm->frames[vm->frame_count - 1].stack_base)
#define BINARY_OP(op) \
    do { \
        uint32_t instr = vm->ip[-1]; \
        int a = CUR_BASE() + REG_A(instr); \
        int b = CUR_BASE() + REG_B(instr); \
        int c = CUR_BASE() + REG_C(instr); \
        Value vb = vm->stack[b]; \
        Value vc = vm->stack[c]; \
        if (!derefOperand(vm, &vb, "arithmetic operation")) return INTERPRET_RUNTIME_ERROR; \
        if (!derefOperand(vm, &vc, "arithmetic operation")) return INTERPRET_RUNTIME_ERROR; \
        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) { \
            vm->stack[a] = DOUBLE_VAL(AS_DOUBLE(vb) op AS_DOUBLE(vc)); \
        } else { \
            runtimeError(vm, ERR_OPERANDS_NUMBERS); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
    } while(false)
#define BINARY_COMPARE(op) \
    do { \
        uint32_t instr = vm->ip[-1]; \
        int a = CUR_BASE() + REG_A(instr); \
        int b = CUR_BASE() + REG_B(instr); \
        int c = CUR_BASE() + REG_C(instr); \
        Value vb = vm->stack[b]; \
        Value vc = vm->stack[c]; \
        if (!derefOperand(vm, &vb, "comparison")) return INTERPRET_RUNTIME_ERROR; \
        if (!derefOperand(vm, &vc, "comparison")) return INTERPRET_RUNTIME_ERROR; \
        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) { \
            vm->stack[a] = BOOL_VAL(AS_DOUBLE(vb) op AS_DOUBLE(vc)); \
        } else { \
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
        uint32_t* base = vm->chunk->code; \
        uint32_t* end  = vm->chunk->code + vm->chunk->count; \
        if (vm->ip < base || vm->ip > end) { \
            fprintf(stderr, "IP out of range: ip=%p, base=%p, end=%p\n", \
            (void*)vm->ip, (void*)base, (void*)end); \
            abort(); \
        } \
    } while(0)

    // Start execution.
    CHECK_IP_BOUNDS();
    DISPATCH();
    OP(MOVE) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        vm->stack[a] = vm->stack[b];
        DISPATCH();
    }
    OP(LOAD_CONST) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        vm->stack[a] = currentChunk(vm)->constants.values[bx];
        DISPATCH();
    }
    OP(ADD) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value val_b = vm->stack[b];
        Value val_c = vm->stack[c];

        if (!derefOperand(vm, &val_b, "addition operation")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &val_c, "addition operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(val_b) && IS_DOUBLE(val_c)) {
            vm->stack[a] = DOUBLE_VAL(AS_DOUBLE(val_b) + AS_DOUBLE(val_c));
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

            // Protect the string before the write (which can trigger GC via tableSet)
            pushTempRoot(vm, (Obj*)result);
            vm->stack[a] = OBJ_VAL(result);
            popTempRoot(vm);

        } else {
            runtimeError(vm, "Operands for '+' must be two numbers or two strings.");
            return INTERPRET_RUNTIME_ERROR;
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
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = vm->stack[b];
        Value vc = vm->stack[c];

        if (!derefOperand(vm, &vb, "modulo operation")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vc, "modulo operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            double rhs = AS_DOUBLE(vc);
            if (rhs == 0.0) {
                runtimeError(vm, "Division by zero in '%%'.");
                return INTERPRET_RUNTIME_ERROR;
            }
            vm->stack[a] = DOUBLE_VAL(fmod(AS_DOUBLE(vb), rhs));
        } else {
            runtimeError(vm, "Operands for '%%' must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(EQ) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = vm->stack[b];
        Value vc = vm->stack[c];

        if (!derefOperand(vm, &vb, "equality check")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vc, "equality check")) return INTERPRET_RUNTIME_ERROR;

        // Special handling for enum type checking
        if (IS_ENUM(vb) && IS_ENUM(vc)) {
            int type_b = ENUM_TYPE_ID(vb);
            int type_c = ENUM_TYPE_ID(vc);
            if (type_b != type_c) {
                int len_b, len_c;
                const char* name_b = getEnumNameByTypeId(vm, type_b, &len_b);
                const char* name_c = getEnumNameByTypeId(vm, type_c, &len_c);
                if (name_b && name_c) {
                    runtimeError(vm, "Cannot compare enum '%.*s' with enum '%.*s'",
                                len_b, name_b, len_c, name_c);
                } else {
                    runtimeError(vm, "Cannot compare enum values of different types (type IDs: %d vs %d)", type_b, type_c);
                }
                return INTERPRET_RUNTIME_ERROR;
            }
        }

        vm->stack[a] = BOOL_VAL(value_equals(vb, vc));
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
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = vm->stack[b];
        Value vc = vm->stack[c];

        if (!derefOperand(vm, &vb, "inequality check")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vc, "inequality check")) return INTERPRET_RUNTIME_ERROR;

        vm->stack[a] = BOOL_VAL(!value_equals(vb, vc));
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
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = false;
        if (IS_DOUBLE(va)) {
            result = (AS_DOUBLE(va) == (double)imm);
        } else if (IS_BOOL(va)) {
            result = (AS_BOOL(va) == (imm != 0));
        } else if (IS_NULL(va)) {
            result = (imm == 0);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(GT_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = false;
        if (IS_DOUBLE(va)) {
            result = (AS_DOUBLE(va) > (double)imm);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(LT_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = false;
        if (IS_DOUBLE(va)) {
            result = (AS_DOUBLE(va) < (double)imm);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(NE_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = true;
        if (IS_DOUBLE(va)) {
            result = (AS_DOUBLE(va) != (double)imm);
        } else if (IS_BOOL(va)) {
            result = (AS_BOOL(va) != (imm != 0));
        } else if (IS_NULL(va)) {
            result = (imm != 0);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(LE_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = false;
        if (IS_DOUBLE(va)) {
            result = (AS_DOUBLE(va) <= (double)imm);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(GE_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = false;
        if (IS_DOUBLE(va)) {
            result = (AS_DOUBLE(va) >= (double)imm);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }

    // ===== Comparison with 64-bit Literal =====
    OP(EQ_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = false;
        if (IS_DOUBLE(vb)) {
            result = (AS_DOUBLE(vb) == literal);
        } else if (IS_BOOL(vb)) {
            result = (AS_BOOL(vb) == (literal != 0.0));
        } else if (IS_NULL(vb)) {
            result = (literal == 0.0);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(GT_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = false;
        if (IS_DOUBLE(vb)) {
            result = (AS_DOUBLE(vb) > literal);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(LT_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = false;
        if (IS_DOUBLE(vb)) {
            result = (AS_DOUBLE(vb) < literal);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(NE_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = true;
        if (IS_DOUBLE(vb)) {
            result = (AS_DOUBLE(vb) != literal);
        } else if (IS_BOOL(vb)) {
            result = (AS_BOOL(vb) != (literal != 0.0));
        } else if (IS_NULL(vb)) {
            result = (literal != 0.0);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(LE_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = false;
        if (IS_DOUBLE(vb)) {
            result = (AS_DOUBLE(vb) <= literal);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }
    OP(GE_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "comparison operation")) return INTERPRET_RUNTIME_ERROR;

        bool result = false;
        if (IS_DOUBLE(vb)) {
            result = (AS_DOUBLE(vb) >= literal);
        }

        vm->stack[a] = BOOL_VAL(result);
        DISPATCH();
    }

    OP(NOT) { // Ra = !Rb    (false/null/0 => true, everything else => false)
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(vm->ip[-1]);
        int b = CUR_BASE() + REG_B(vm->ip[-1]);
        Value v = vm->stack[b];

        if (!derefOperand(vm, &v, "NOT operation")) return INTERPRET_RUNTIME_ERROR;

        bool is_falsey = IS_NULL(v) || (IS_BOOL(v) && !AS_BOOL(v)) || (IS_DOUBLE(v) && AS_DOUBLE(v) == 0.0);
        vm->stack[a] = BOOL_VAL(is_falsey);
        DISPATCH();
    }
    OP(BAND) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = vm->stack[b];
        Value vc = vm->stack[c];

        if (!derefOperand(vm, &vb, "bitwise AND operation")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vc, "bitwise AND operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            int32_t result = lhs & rhs;
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operands for '&' must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BOR) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = vm->stack[b];
        Value vc = vm->stack[c];

        if (!derefOperand(vm, &vb, "bitwise OR operation")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vc, "bitwise OR operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            int32_t result = lhs | rhs;
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operands for '|' must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BXOR) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = vm->stack[b];
        Value vc = vm->stack[c];

        if (!derefOperand(vm, &vb, "bitwise XOR operation")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vc, "bitwise XOR operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            int32_t result = lhs ^ rhs;
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operands for '^' must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BLSHIFT) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = vm->stack[b];
        Value vc = vm->stack[c];

        if (!derefOperand(vm, &vb, "bitwise left shift operation")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vc, "bitwise left shift operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            // Mask shift amount to 0-31 for i32
            int32_t result = lhs << (rhs & 0x1F);
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operands for '<<' must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_U) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = vm->stack[b];
        Value vc = vm->stack[c];

        if (!derefOperand(vm, &vb, "bitwise right shift operation")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vc, "bitwise right shift operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            // JavaScript behavior: convert to uint32, logical shift with 0-31 mask
            uint32_t lhs = (uint32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            // Mask shift amount to 0-31 for i32
            uint32_t result = lhs >> (rhs & 0x1F);
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operands for '>>>' must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int c = CUR_BASE() + REG_C(instr);
        Value vb = vm->stack[b];
        Value vc = vm->stack[c];

        if (!derefOperand(vm, &vb, "bitwise right shift operation")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vc, "bitwise right shift operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb) && IS_DOUBLE(vc)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);  // Signed for arithmetic shift
            int32_t rhs = (int32_t)AS_DOUBLE(vc);
            // Mask shift amount to 0-31 for 32-bit signed
            int32_t result = lhs >> (rhs & 0x1F);
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operands for '>>' must be numbers.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Arithmetic with 16-bit Immediate =====
    OP(ADD_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        // Sign-extend 16-bit immediate
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "addition operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            vm->stack[a] = DOUBLE_VAL(AS_DOUBLE(va) + (double)imm);
        } else {
            runtimeError(vm, "Operand for '+' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(SUB_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "subtraction operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            vm->stack[a] = DOUBLE_VAL(AS_DOUBLE(va) - (double)imm);
        } else {
            runtimeError(vm, "Operand for '-' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(MUL_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "multiplication operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            vm->stack[a] = DOUBLE_VAL(AS_DOUBLE(va) * (double)imm);
        } else {
            runtimeError(vm, "Operand for '*' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(DIV_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "division operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            vm->stack[a] = DOUBLE_VAL(AS_DOUBLE(va) / (double)imm);
        } else {
            runtimeError(vm, "Operand for '/' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(MOD_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "modulo operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            double rhs = (double)imm;
            if (rhs == 0.0) {
                runtimeError(vm, "Division by zero in '%%'.");
                return INTERPRET_RUNTIME_ERROR;
            }
            vm->stack[a] = DOUBLE_VAL(fmod(AS_DOUBLE(va), rhs));
        } else {
            runtimeError(vm, "Operand for '%%' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Arithmetic with 64-bit Literal =====
    OP(ADD_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        // Read 64-bit literal from next two instructions
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "addition operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            vm->stack[a] = DOUBLE_VAL(AS_DOUBLE(vb) + literal);
        } else {
            runtimeError(vm, "Operand for '+' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(SUB_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "subtraction operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            vm->stack[a] = DOUBLE_VAL(AS_DOUBLE(vb) - literal);
        } else {
            runtimeError(vm, "Operand for '-' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(MUL_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "multiplication operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            vm->stack[a] = DOUBLE_VAL(AS_DOUBLE(vb) * literal);
        } else {
            runtimeError(vm, "Operand for '*' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(DIV_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "division operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            vm->stack[a] = DOUBLE_VAL(AS_DOUBLE(vb) / literal);
        } else {
            runtimeError(vm, "Operand for '/' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(MOD_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "modulo operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            if (literal == 0.0) {
                runtimeError(vm, "Division by zero in '%%'.");
                return INTERPRET_RUNTIME_ERROR;
            }
            vm->stack[a] = DOUBLE_VAL(fmod(AS_DOUBLE(vb), literal));
        } else {
            runtimeError(vm, "Operand for '%%' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Bitwise with 16-bit Immediate =====
    OP(BAND_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "bitwise AND operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            int32_t lhs = (int32_t)AS_DOUBLE(va);
            int32_t result = lhs & (int32_t)imm;
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '&' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BOR_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "bitwise OR operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            int32_t lhs = (int32_t)AS_DOUBLE(va);
            int32_t result = lhs | (int32_t)imm;
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '|' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BXOR_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "bitwise XOR operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            int32_t lhs = (int32_t)AS_DOUBLE(va);
            int32_t result = lhs ^ (int32_t)imm;
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '^' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BLSHIFT_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "bitwise left shift operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            int32_t lhs = (int32_t)AS_DOUBLE(va);
            int32_t result = lhs << ((int32_t)imm & 0x1F);
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '<<' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_U_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "bitwise unsigned right shift operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            uint32_t lhs = (uint32_t)AS_DOUBLE(va);
            uint32_t result = lhs >> ((int32_t)imm & 0x1F);
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '>>>' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_I_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "bitwise signed right shift operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            int32_t lhs = (int32_t)AS_DOUBLE(va);
            int32_t result = lhs >> ((int32_t)imm & 0x1F);
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '>>' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Bitwise with 64-bit Literal =====
    OP(BAND_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "bitwise AND operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            int32_t result = lhs & rhs;
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '&' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BOR_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "bitwise OR operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            int32_t result = lhs | rhs;
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '|' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BXOR_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "bitwise XOR operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            int32_t result = lhs ^ rhs;
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '^' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BLSHIFT_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "bitwise left shift operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            int32_t result = lhs << (rhs & 0x1F);
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '<<' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_U_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "bitwise unsigned right shift operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            uint32_t lhs = (uint32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            uint32_t result = lhs >> (rhs & 0x1F);
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '>>>' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRSHIFT_I_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));

        Value vb = vm->stack[b];
        if (!derefOperand(vm, &vb, "bitwise signed right shift operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            int32_t lhs = (int32_t)AS_DOUBLE(vb);
            int32_t rhs = (int32_t)literal;
            int32_t result = lhs >> (rhs & 0x1F);
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '>>' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    OP(NEG) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value val_b = vm->stack[b];

        if (!derefOperand(vm, &val_b, "negation operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(val_b)) {
            vm->stack[a] = DOUBLE_VAL(-AS_DOUBLE(val_b));
        } else {
            runtimeError(vm, "Operand must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BNOT) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value vb = vm->stack[b];

        if (!derefOperand(vm, &vb, "bitwise NOT operation")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(vb)) {
            int32_t val = (int32_t)AS_DOUBLE(vb);
            int32_t result = ~val;
            vm->stack[a] = DOUBLE_VAL((double)result);
        } else {
            runtimeError(vm, "Operand for '~' must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(DEFINE_GLOBAL) {
        uint32_t instr = vm->ip[-1];
        int src_reg = CUR_BASE() + REG_A(instr);
        uint16_t name_const_idx = REG_Bx(instr);
        ObjString* name = AS_STRING(currentChunk(vm)->constants.values[name_const_idx]);

        // Check if this global already has a slot
        Value existing_slot_or_value;
        if (!tableGet(&vm->globals, name, &existing_slot_or_value)) {
            // New global: allocate a slot
            int slot_index = vm->globalSlots.count;
            if (slot_index > UINT16_MAX) {
                runtimeError(vm, "Too many global variables (max %d).", UINT16_MAX + 1);
                return INTERPRET_RUNTIME_ERROR;
            }
            // Store the slot index in the globals table
            writeValueArray(vm, &vm->globalSlots, vm->stack[src_reg]);
            tableSet(vm, &vm->globals, name, DOUBLE_VAL((double)slot_index));
        } else if (IS_DOUBLE(existing_slot_or_value)) {
            // Redefining existing slot-based global: update the value in the slot
            int slot_index = (int)AS_DOUBLE(existing_slot_or_value);
            vm->globalSlots.values[slot_index] = vm->stack[src_reg];
        } else {
            // Trying to redefine a direct-storage global (e.g., native function)
            runtimeError(vm, "Cannot redefine native function '%.*s'.", name->length, name->chars);
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(GET_GLOBAL) {
        uint32_t instr = vm->ip[-1];
        int dest_reg = CUR_BASE() + REG_A(instr);
        uint16_t name_const_idx = REG_Bx(instr);
        ObjString* name = AS_STRING(currentChunk(vm)->constants.values[name_const_idx]);
        Value slot_index_val;
        if (!tableGet(&vm->globals, name, &slot_index_val)) {
            runtimeError(vm, "Undefined identifier '%.*s'.", name->length, name->chars);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Check if this is a slot index (number) or direct value (e.g., native function)
        if (IS_DOUBLE(slot_index_val)) {
            // Slot-based global: get slot index and cache it
            uint16_t slot_index = (uint16_t)AS_DOUBLE(slot_index_val);

            // Self-modify: rewrite this instruction to GET_GLOBAL_CACHED with the slot index
            uint32_t new_instr = (uint32_t)GET_GLOBAL_CACHED | (REG_A(instr) << 8) | (slot_index << 16);
            vm->ip[-1] = new_instr;

            // Execute the cached version
            vm->stack[dest_reg] = vm->globalSlots.values[slot_index];
        } else {
            // Direct value (e.g., native function) - use as-is, no caching
            vm->stack[dest_reg] = slot_index_val;
        }
        DISPATCH();
    }
    OP(SET_GLOBAL) {
        uint32_t instr = vm->ip[-1];
        int src_reg = CUR_BASE() + REG_A(instr);
        uint16_t name_const_idx = REG_Bx(instr);
        ObjString* name = AS_STRING(currentChunk(vm)->constants.values[name_const_idx]);

        // Check for circular reference: if assigning a reference to a global,
        // make sure it doesn't point back to the same global (directly or indirectly)
        Value new_value = vm->stack[src_reg];
        if (IS_REFERENCE(new_value)) {
            ObjReference* new_ref = AS_REFERENCE(new_value);

            // Error: cannot store REF_LOCAL in a global variable (it will become invalid)
            if (new_ref->ref_type == REF_LOCAL) {
                runtimeError(vm, "Cannot store a reference to a local variable in a global variable.");
                return INTERPRET_RUNTIME_ERROR;
            }

            if (referencesGlobal(vm, new_ref, name, 0)) {
                runtimeError(vm, "Circular reference: cannot assign variable '%.*s' to a reference that points back to itself.",
                             name->length, name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
        }

        // Check if this global is actually a reference
        Value existing_slot_or_value;
        if (!tableGet(&vm->globals, name, &existing_slot_or_value)) {
            // Global doesn't exist - will be caught below
        } else {
            // Get the actual value (might be slot-based or direct)
            Value existing;
            if (IS_DOUBLE(existing_slot_or_value)) {
                // Slot-based: fetch from slot
                int slot_index = (int)AS_DOUBLE(existing_slot_or_value);
                existing = vm->globalSlots.values[slot_index];
            } else {
                // Direct value
                existing = existing_slot_or_value;
            }

            if (IS_REFERENCE(existing)) {
                ObjReference* ref = AS_REFERENCE(existing);
                Value new_value = vm->stack[src_reg];

                switch (ref->ref_type) {
                case REF_LOCAL: {
                    *ref->as.local.location = new_value;
                    DISPATCH();
                }

                case REF_GLOBAL: {
                    // REF_GLOBAL is special: it's an alias to another variable.
                    // If we're assigning a new reference, REPLACE the alias (rebind).
                    // If we're assigning a non-reference value, write through to the target.
                    if (IS_REFERENCE(new_value)) {
                        // Rebind: replace the REF_GLOBAL with the new reference
                        if (!globalSet(vm, name, new_value)) {
                            runtimeError(vm, "Failed to rebind global reference '%.*s'.", name->length, name->chars);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    } else {
                        // Write through: update the target variable
                        if (!globalSet(vm, ref->as.global.global_name, new_value)) {
                            runtimeError(vm, "Failed to write through global reference '%.*s'.",
                                ref->as.global.global_name->length, ref->as.global.global_name->chars);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                    DISPATCH();
                }

                case REF_UPVALUE: {
                    if (!validateUpvalue(vm, ref->as.upvalue.upvalue, "SET_GLOBAL")) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    // Check if the upvalue's location contains a nested reference - if so, write through it recursively
                    Value current = *ref->as.upvalue.upvalue->location;
                    if (IS_REFERENCE(current)) {
                        if (!writeReferenceValue(vm, current, new_value)) {
                            runtimeError(vm, "Failed to write through nested reference in upvalue.");
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    } else {
                        if (!validateUpvalue(vm, ref->as.upvalue.upvalue, "SET_GLOBAL")) return INTERPRET_RUNTIME_ERROR;
                        *ref->as.upvalue.upvalue->location = new_value;
                    }
                    DISPATCH();
                }

                case REF_INDEX: {
                    Value container = ref->as.index.container;
                    Value index = ref->as.index.index;

                    if (!IS_OBJ(container)) {
                        runtimeError(vm, ERR_INDEX_CONTAINER_NOT_OBJECT);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    if (IS_LIST(container)) {
                        if (!IS_DOUBLE(index)) {
                            runtimeError(vm, ERR_LIST_INDEX_TYPE);
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        ObjList* list = AS_LIST(container);
                        int idx = (int)AS_DOUBLE(index);
                        if (!validateListIndex(vm, list, idx, "SET_GLOBAL")) {
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        if (!writeThruListElement(vm, list, idx, new_value)) {
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    } else if (IS_MAP(container)) {
                        ObjMap* map = AS_MAP(container);
                        ObjString* key_str = keyToString(vm, index);
                        if (!key_str) {
                            runtimeError(vm, ERR_MAP_KEYS_TYPE);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        if (!writeThruMapField(vm, map, key_str, new_value)) {
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    } else {
                        runtimeError(vm, ERR_INDEX_CONTAINER_NOT_MAP);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    DISPATCH();
                }

                case REF_PROPERTY: {
                    Value container = ref->as.property.container;
                    Value key = ref->as.property.key;

                    if (!IS_OBJ(container) || (!IS_MAP(container) && !IS_STRUCT_INSTANCE(container))) {
                        runtimeError(vm, "Property reference container is not a map or struct.");
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    // Key must be a string
                    if (!IS_OBJ(key) || !IS_STRING(key)) {
                        runtimeError(vm, "Property key must be a string.");
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    ObjString* key_str = AS_STRING(key);

                    if (IS_MAP(container)) {
                        ObjMap* map = AS_MAP(container);
                        tableSet(vm, map->table, key_str, new_value);
                    } else { // IS_STRUCT_INSTANCE - already validated above
                        ObjStructInstance* instance = AS_STRUCT_INSTANCE(container);
                        Value index_val;
                        if (!tableGet(instance->schema->field_to_index, key_str, &index_val)) {
                            runtimeError(vm, "Struct field '%.*s' does not exist.", key_str->length, key_str->chars);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        int field_index = (int)AS_DOUBLE(index_val);
                        instance->fields[field_index] = new_value;
                    }

                    DISPATCH();
                }
                }
            }
        }

        // Normal SET_GLOBAL (not a reference)
        // Check if global exists
        Value slot_index_val;
        if (!tableGet(&vm->globals, name, &slot_index_val)) {
            runtimeError(vm, "Undefined identifier '%.*s'.", name->length, name->chars);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Check if this is a slot index (number) or direct value (e.g., native function)
        if (IS_DOUBLE(slot_index_val)) {
            // Slot-based global: get slot index and cache it
            uint16_t slot_index = (uint16_t)AS_DOUBLE(slot_index_val);
            vm->globalSlots.values[slot_index] = vm->stack[src_reg];

            // Self-modify: rewrite this instruction to SET_GLOBAL_CACHED with the slot index
            uint32_t new_instr = (uint32_t)SET_GLOBAL_CACHED | (REG_A(instr) << 8) | (slot_index << 16);
            vm->ip[-1] = new_instr;
        } else {
            // Direct value (e.g., native function) - can't set, this is an error
            runtimeError(vm, "Cannot assign to native function '%.*s'.", name->length, name->chars);
            return INTERPRET_RUNTIME_ERROR;
        }

        DISPATCH();
    }
    OP(GET_GLOBAL_CACHED) {
        // Fast path: direct array lookup using cached slot index
        uint32_t instr = vm->ip[-1];
        int dest_reg = CUR_BASE() + REG_A(instr);
        uint16_t slot_index = REG_Bx(instr);
        vm->stack[dest_reg] = vm->globalSlots.values[slot_index];
        DISPATCH();
    }
    OP(SET_GLOBAL_CACHED) {
        // Fast path: direct array write using cached slot index
        uint32_t instr = vm->ip[-1];
        int src_reg = CUR_BASE() + REG_A(instr);
        uint16_t slot_index = REG_Bx(instr);
        vm->globalSlots.values[slot_index] = vm->stack[src_reg];
        DISPATCH();
    }
    OP(SLOT_SET_GLOBAL) {
        // SLOT_SET_GLOBAL: directly replace the global variable value, bypassing reference dereferencing
        // This is used for the `slot` keyword which rebinds variables instead of writing through references
        uint32_t instr = vm->ip[-1];
        int src_reg = CUR_BASE() + REG_A(instr);
        uint16_t name_const_idx = REG_Bx(instr);
        ObjString* name = AS_STRING(currentChunk(vm)->constants.values[name_const_idx]);

        // Check if global exists
        Value slot_index_val;
        if (!tableGet(&vm->globals, name, &slot_index_val)) {
            runtimeError(vm, "Undefined identifier '%.*s'.", name->length, name->chars);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Check if this is a slot index (number) or direct value (e.g., native function)
        if (IS_DOUBLE(slot_index_val)) {
            // Slot-based global: directly replace the value in the slot (bypassing reference logic)
            uint16_t slot_index = (uint16_t)AS_DOUBLE(slot_index_val);
            vm->globalSlots.values[slot_index] = vm->stack[src_reg];
        } else {
            // Direct value (e.g., native function) - can't rebind, this is an error
            runtimeError(vm, "Cannot rebind native function '%.*s'.", name->length, name->chars);
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(JUMP_IF_FALSE) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t raw = REG_Bx(instr);
        int32_t off = sign_extend_16(raw);

        Value condition = vm->stack[a];

        if (!derefOperand(vm, &condition, "conditional")) return INTERPRET_RUNTIME_ERROR;

        // falsey = null, false, or 0
        if (IS_NULL(condition) || (IS_BOOL(condition) && !AS_BOOL(condition)) || (IS_DOUBLE(condition) && AS_DOUBLE(condition) == 0.0)) {
            vm->ip += off;
        }
        DISPATCH();
    }
    OP(JUMP) {
        uint32_t instr = vm->ip[-1];
        uint16_t raw = REG_Bx(instr);
        int32_t off = sign_extend_16(raw);
        vm->ip += off;
        DISPATCH();
    }

    // ===== Branch-Compare Opcodes (Register-Register) =====
    OP(BRANCH_EQ) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = vm->stack[a];
        Value vb = vm->stack[b];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vb, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (value_equals(va, vb)) {
            vm->ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_NE) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = vm->stack[a];
        Value vb = vm->stack[b];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vb, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (!value_equals(va, vb)) {
            vm->ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_LT) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = vm->stack[a];
        Value vb = vm->stack[b];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vb, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va) && IS_DOUBLE(vb)) {
            if (AS_DOUBLE(va) < AS_DOUBLE(vb)) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operands must be numbers for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_LE) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = vm->stack[a];
        Value vb = vm->stack[b];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vb, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va) && IS_DOUBLE(vb)) {
            if (AS_DOUBLE(va) <= AS_DOUBLE(vb)) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operands must be numbers for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GT) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = vm->stack[a];
        Value vb = vm->stack[b];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vb, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va) && IS_DOUBLE(vb)) {
            if (AS_DOUBLE(va) > AS_DOUBLE(vb)) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operands must be numbers for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GE) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        int32_t off = sign_extend_8(REG_C(instr));
        Value va = vm->stack[a];
        Value vb = vm->stack[b];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;
        if (!derefOperand(vm, &vb, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va) && IS_DOUBLE(vb)) {
            if (AS_DOUBLE(va) >= AS_DOUBLE(vb)) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operands must be numbers for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Branch-Compare Opcodes (Register-Immediate) =====
    OP(BRANCH_EQ_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *vm->ip++;  // Offset in next instruction
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        bool matches = false;
        if (IS_DOUBLE(va)) {
            matches = (AS_DOUBLE(va) == (double)imm);
        } else if (IS_BOOL(va)) {
            matches = (AS_BOOL(va) == (imm != 0));
        } else if (IS_NULL(va)) {
            matches = (imm == 0);
        }

        if (matches) {
            vm->ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_NE_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *vm->ip++;
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        bool matches = true;
        if (IS_DOUBLE(va)) {
            matches = (AS_DOUBLE(va) != (double)imm);
        } else if (IS_BOOL(va)) {
            matches = (AS_BOOL(va) != (imm != 0));
        } else if (IS_NULL(va)) {
            matches = (imm != 0);
        }

        if (matches) {
            vm->ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_LT_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *vm->ip++;
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            if (AS_DOUBLE(va) < (double)imm) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operand must be a number for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_LE_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *vm->ip++;
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            if (AS_DOUBLE(va) <= (double)imm) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operand must be a number for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GT_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *vm->ip++;
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            if (AS_DOUBLE(va) > (double)imm) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operand must be a number for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GE_I) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);
        int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
        int32_t off = *vm->ip++;
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            if (AS_DOUBLE(va) >= (double)imm) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operand must be a number for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    // ===== Branch-Compare Opcodes (Register-Literal) =====
    OP(BRANCH_EQ_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));
        int32_t off = *vm->ip++;
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        bool matches = false;
        if (IS_DOUBLE(va)) {
            matches = (AS_DOUBLE(va) == literal);
        } else if (IS_BOOL(va)) {
            matches = (AS_BOOL(va) == (literal != 0.0));
        } else if (IS_NULL(va)) {
            matches = (literal == 0.0);
        }

        if (matches) {
            vm->ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_NE_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));
        int32_t off = *vm->ip++;
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        bool matches = true;
        if (IS_DOUBLE(va)) {
            matches = (AS_DOUBLE(va) != literal);
        } else if (IS_BOOL(va)) {
            matches = (AS_BOOL(va) != (literal != 0.0));
        } else if (IS_NULL(va)) {
            matches = (literal != 0.0);
        }

        if (matches) {
            vm->ip += off;
        }
        DISPATCH();
    }
    OP(BRANCH_LT_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));
        int32_t off = *vm->ip++;
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            if (AS_DOUBLE(va) < literal) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operand must be a number for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_LE_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));
        int32_t off = *vm->ip++;
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            if (AS_DOUBLE(va) <= literal) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operand must be a number for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GT_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));
        int32_t off = *vm->ip++;
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            if (AS_DOUBLE(va) > literal) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operand must be a number for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP(BRANCH_GE_L) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint32_t low = *vm->ip++;
        uint32_t high = *vm->ip++;
        uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
        double literal;
        memcpy(&literal, &bits, sizeof(double));
        int32_t off = *vm->ip++;
        off = sign_extend_16(off);
        Value va = vm->stack[a];

        if (!derefOperand(vm, &va, "comparison")) return INTERPRET_RUNTIME_ERROR;

        if (IS_DOUBLE(va)) {
            if (AS_DOUBLE(va) >= literal) {
                vm->ip += off;
            }
        } else {
            runtimeError(vm, "Operand must be a number for comparison.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }

    OP(CALL) {
        uint32_t instr = vm->ip[-1];
        int callee_slot = CUR_BASE() + REG_A(instr);
        uint16_t arg_count = REG_Bx(instr);
        Value callee = vm->stack[callee_slot];

        // Dereference if callee is a reference (refs are first-class)
        if (!derefOperand(vm, &callee, "function call")) return INTERPRET_RUNTIME_ERROR;
        vm->stack[callee_slot] = callee;

        // Resolve dispatcher overload if needed
        if (IS_DISPATCHER(callee)) {
            Value matched_closure = resolveOverload(vm, AS_DISPATCHER(callee), arg_count);
            if (IS_NULL(matched_closure)) {
                runtimeError(vm, "No overload found for %u arguments.", arg_count);
                return INTERPRET_RUNTIME_ERROR;
            }
            callee = matched_closure;
            vm->stack[callee_slot] = callee;
        }

        // Handle native functions
        if (IS_NATIVE_FUNCTION(callee)) {
            ObjNativeFunction* native = AS_NATIVE_FUNCTION(callee);

            if (arg_count != native->arity) {
                runtimeError(vm, "Expected %d arguments but got %u.", native->arity, arg_count);
                return INTERPRET_RUNTIME_ERROR;
            }

            // Process parameter qualifiers (ref, val, slot, clone)
            if (native->param_qualifiers == NULL) {
                // No qualifiers means all parameters are PARAM_NORMAL
                // Dereference any reference arguments
                for (int i = 0; i < arg_count; i++) {
                    int arg_slot = callee_slot + 1 + i;
                    Value arg_value = vm->stack[arg_slot];
                    if (IS_REFERENCE(arg_value)) {
                        Value deref_value;
                        if (!dereferenceValue(vm, arg_value, &deref_value)) {
                            deref_value = NULL_VAL;
                        }
                        vm->stack[arg_slot] = deref_value;
                    }
                }
            } else {
                for (int i = 0; i < arg_count; i++) {
                    int arg_slot = callee_slot + 1 + i;
                    ParamQualifier qualifier = (ParamQualifier)native->param_qualifiers[i];
                    Value arg_value = vm->stack[arg_slot];

                    // Apply qualifier transformations
                    if (IS_REFERENCE(arg_value) || (IS_OBJ(arg_value) && IS_NATIVE_REFERENCE(arg_value))) {
                        if (qualifier == PARAM_REF || qualifier == PARAM_SLOT) {
                            // Keep reference
                            continue;
                        } else if (qualifier == PARAM_VAL) {
                            // Dereference and clone
                            Value deref_value;
                            if (!dereferenceValue(vm, arg_value, &deref_value)) {
                                deref_value = NULL_VAL;
                            }
                            vm->stack[arg_slot] = cloneValue(vm, deref_value);
                        } else if (qualifier == PARAM_CLONE) {
                            // Dereference and deep clone
                            Value deref_value;
                            if (!dereferenceValue(vm, arg_value, &deref_value)) {
                                deref_value = NULL_VAL;
                            }
                            vm->stack[arg_slot] = deepCloneValue(vm, deref_value);
                        } else {
                            // PARAM_NORMAL: dereference
                            Value deref_value;
                            if (!dereferenceValue(vm, arg_value, &deref_value)) {
                                deref_value = NULL_VAL;
                            }
                            vm->stack[arg_slot] = deref_value;
                        }
                    } else {
                        if (qualifier == PARAM_VAL) {
                            vm->stack[arg_slot] = cloneValue(vm, arg_value);
                        } else if (qualifier == PARAM_CLONE) {
                            vm->stack[arg_slot] = deepCloneValue(vm, arg_value);
                        }
                        // PARAM_NORMAL and PARAM_SLOT: pass through as-is
                    }
                }
            }

            // Prepare arguments array (points to first arg on stack)
            Value* args = &vm->stack[callee_slot + 1];

            // Protect arguments from GC during native call
            int saved_temp_root_count = vm->temp_root_count;
            for (int i = 0; i < arg_count; i++) {
                if (IS_OBJ(args[i])) {
                    pushTempRoot(vm, AS_OBJ(args[i]));
                }
            }

            // Call native function via dispatcher
            Value result = native->dispatcher(vm, args, native->func_ptr);

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
                DISPATCH();
            }

            // Place result in callee slot
            vm->stack[callee_slot] = result;

            DISPATCH();
        }

        // Handle native closures
        if (IS_NATIVE_CLOSURE(callee)) {
            ObjNativeClosure* native_closure = AS_NATIVE_CLOSURE(callee);

            if (arg_count != native_closure->arity) {
                runtimeError(vm, "Expected %d arguments but got %u.", native_closure->arity, arg_count);
                return INTERPRET_RUNTIME_ERROR;
            }

            // Process parameter qualifiers (ref, val, slot, clone) - same as native functions
            if (native_closure->param_qualifiers == NULL) {
                // No qualifiers means all parameters are PARAM_NORMAL
                // Dereference any reference arguments
                for (int i = 0; i < arg_count; i++) {
                    int arg_slot = callee_slot + 1 + i;
                    Value arg_value = vm->stack[arg_slot];
                    if (IS_REFERENCE(arg_value)) {
                        Value deref_value;
                        if (!dereferenceValue(vm, arg_value, &deref_value)) {
                            deref_value = NULL_VAL;
                        }
                        vm->stack[arg_slot] = deref_value;
                    }
                }
            } else {
                for (int i = 0; i < arg_count; i++) {
                    int arg_slot = callee_slot + 1 + i;
                    ParamQualifier qualifier = (ParamQualifier)native_closure->param_qualifiers[i];
                    Value arg_value = vm->stack[arg_slot];

                    // Apply qualifier transformations
                    if (IS_REFERENCE(arg_value) || (IS_OBJ(arg_value) && IS_NATIVE_REFERENCE(arg_value))) {
                        if (qualifier == PARAM_REF || qualifier == PARAM_SLOT) {
                            // Keep reference
                            continue;
                        } else if (qualifier == PARAM_VAL) {
                            // Dereference and clone
                            Value deref_value;
                            if (!dereferenceValue(vm, arg_value, &deref_value)) {
                                deref_value = NULL_VAL;
                            }
                            vm->stack[arg_slot] = cloneValue(vm, deref_value);
                        } else if (qualifier == PARAM_CLONE) {
                            // Dereference and deep clone
                            Value deref_value;
                            if (!dereferenceValue(vm, arg_value, &deref_value)) {
                                deref_value = NULL_VAL;
                            }
                            vm->stack[arg_slot] = deepCloneValue(vm, deref_value);
                        } else {
                            // PARAM_NORMAL: dereference
                            Value deref_value;
                            if (!dereferenceValue(vm, arg_value, &deref_value)) {
                                deref_value = NULL_VAL;
                            }
                            vm->stack[arg_slot] = deref_value;
                        }
                    } else {
                        if (qualifier == PARAM_VAL) {
                            vm->stack[arg_slot] = cloneValue(vm, arg_value);
                        } else if (qualifier == PARAM_CLONE) {
                            vm->stack[arg_slot] = deepCloneValue(vm, arg_value);
                        }
                        // PARAM_NORMAL and PARAM_SLOT: pass through as-is
                    }
                }
            }

            // Prepare arguments array: [context, arg1, arg2, ...]
            // We need to build a temporary array with context as first element
            Value closure_args[MAX_NATIVE_ARITY + 1];
            closure_args[0] = native_closure->context;  // Context is first argument
            for (int i = 0; i < arg_count; i++) {
                closure_args[i + 1] = vm->stack[callee_slot + 1 + i];
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
            Value result = native_closure->dispatcher(vm, closure_args, native_closure->func_ptr);

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
                DISPATCH();
            }

            // Place result in callee slot
            vm->stack[callee_slot] = result;

            DISPATCH();
        }

        if (!IS_CLOSURE(callee)) {
            runtimeError(vm, ERR_ONLY_CALL_FUNCTIONS);
            return INTERPRET_RUNTIME_ERROR;
        }

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
            printValue(vm->stack[callee_slot + i]);
            printf("\n");
        }
        #endif

        if (arg_count != function->arity) {
            runtimeError(vm, "Expected %d arguments but got %u.", function->arity, arg_count);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Handle ref/val parameters at runtime
        if (!processParamQualifiers(vm, function, callee_slot, arg_count, false)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        if (vm->frame_count == FRAMES_MAX) {
            runtimeError(vm, "Stack overflow: maximum call depth (%d) reached.", FRAMES_MAX);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Calculate required stack size and grow if needed
        int needed_top = callee_slot + function->max_regs;
        if (!growStackForCall(vm, needed_top, NULL)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // Update stack_top to track highest used slot
        if (needed_top > vm->stack_top) {
            vm->stack_top = needed_top;
        }

        // Push frame
        CallFrame* frame = &vm->frames[vm->frame_count++];
        frame->closure      = closure;
        frame->ip           = vm->ip;
        frame->stack_base   = callee_slot;
        frame->caller_chunk = vm->chunk;

        // Enter callee
        vm->chunk = function->chunk;
        vm->ip = function->chunk->code;
        DISPATCH();
    }
    OP(CALL_SELF) {
        // Optimized recursive call - no global lookup needed
        // The current function's closure is in the current frame
        uint32_t instr = vm->ip[-1];
        int callee_slot = CUR_BASE() + REG_A(instr);
        uint16_t arg_count = REG_Bx(instr);

        // Get the current frame's closure (the function we're already in)
        CallFrame* current_frame = &vm->frames[vm->frame_count - 1];
        ObjClosure* closure = current_frame->closure;
        ObjFunction* function = closure->function;

        // Place the closure in the callee slot
        vm->stack[callee_slot] = OBJ_VAL(closure);

        #ifdef DEBUG_CALL
        printf("[VM CALL_SELF] CUR_BASE=%d, REG_A=%d, callee_slot=%d, arg_count=%u\n",
               CUR_BASE(), REG_A(instr), callee_slot, arg_count);
        printf("[VM CALL_SELF] Function: %.*s, arity=%d\n",
               function->name ? function->name->length : 6,
               function->name ? function->name->chars : "<anon>",
               function->arity);
        #endif

        if (arg_count != function->arity) {
            runtimeError(vm, "Expected %d arguments but got %u.", function->arity, arg_count);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Handle ref/val parameters at runtime
        if (!processParamQualifiers(vm, function, callee_slot, arg_count, false)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        if (vm->frame_count == FRAMES_MAX) {
            runtimeError(vm, "Stack overflow: maximum call depth (%d) reached.", FRAMES_MAX);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Calculate required stack size and grow if needed
        int needed_top = callee_slot + function->max_regs;
        if (!growStackForCall(vm, needed_top, NULL)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // Update stack_top to track highest used slot
        if (needed_top > vm->stack_top) {
            vm->stack_top = needed_top;
        }

        // Push frame
        CallFrame* frame = &vm->frames[vm->frame_count++];
        frame->closure      = closure;
        frame->ip           = vm->ip;
        frame->stack_base   = callee_slot;
        frame->caller_chunk = vm->chunk;

        // Enter callee (same chunk, restart from beginning)
        vm->ip = function->chunk->code;
        DISPATCH();
    }
    OP(TAIL_CALL) {
        uint32_t instr = vm->ip[-1];
        int callee_slot = CUR_BASE() + REG_A(instr);
        uint16_t arg_count = REG_Bx(instr);
        Value callee = vm->stack[callee_slot];

        // Dereference if callee is a reference
        if (!derefOperand(vm, &callee, "tail call")) return INTERPRET_RUNTIME_ERROR;
        vm->stack[callee_slot] = callee;

        // Resolve dispatcher overload if needed
        if (IS_DISPATCHER(callee)) {
            Value matched_closure = resolveOverload(vm, AS_DISPATCHER(callee), arg_count);
            if (IS_NULL(matched_closure)) {
                runtimeError(vm, "No overload found for %u arguments.", arg_count);
                return INTERPRET_RUNTIME_ERROR;
            }
            callee = matched_closure;
            vm->stack[callee_slot] = callee;
        }

        if (!IS_CLOSURE(callee)) {
            runtimeError(vm, ERR_ONLY_CALL_FUNCTIONS);
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjClosure*  closure  = AS_CLOSURE(callee);
        ObjFunction* function = closure->function;

        if (arg_count != function->arity) {
            runtimeError(vm, "Expected %d arguments but got %u.", function->arity, arg_count);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Handle ref/val parameters at runtime (TCO path: defer PARAM_REF handling)
        if (!processParamQualifiers(vm, function, callee_slot, arg_count, true)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // TAIL CALL OPTIMIZATION: Reuse current frame instead of pushing new one
        CallFrame* current_frame = &vm->frames[vm->frame_count - 1];
        int frame_base = current_frame->stack_base;
        int needed_top = frame_base + function->max_regs;

        // Grow stack if needed
        if (!growStackForCall(vm, needed_top, NULL)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        if (needed_top > vm->stack_top) {
            vm->stack_top = needed_top;
        }

        // Upvalues have already been closed by CLOSE_FRAME_UPVALUES instruction
        // Move args to the frame base
        for (int i = 0; i < arg_count; i++) {
            vm->stack[frame_base + 1 + i] = vm->stack[callee_slot + 1 + i];
        }

        // Handle PARAM_REF with non-reference args AFTER the move
        if (!processParamRefAfterMove(vm, function, frame_base, arg_count)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // Put the new callee in R0 of this frame
        vm->stack[frame_base] = callee;

        // Update the frame to point at the new closure
        current_frame->closure = closure;

        // Jump into the new function
        vm->chunk = function->chunk;
        vm->ip    = function->chunk->code;

        DISPATCH();
    }
    OP(TAIL_CALL_SELF) {
        // Optimized recursive tail call - no global lookup needed
        uint32_t instr = vm->ip[-1];
        int callee_slot = CUR_BASE() + REG_A(instr);
        uint16_t arg_count = REG_Bx(instr);

        // Get the current frame's closure (the function we're already in)
        CallFrame* current_frame = &vm->frames[vm->frame_count - 1];
        ObjClosure* closure = current_frame->closure;
        ObjFunction* function = closure->function;

        // Place the closure in the callee slot
        vm->stack[callee_slot] = OBJ_VAL(closure);

        if (arg_count != function->arity) {
            runtimeError(vm, "Expected %d arguments but got %u.", function->arity, arg_count);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Handle ref/val parameters (TCO path: defer PARAM_REF handling)
        if (!processParamQualifiers(vm, function, callee_slot, arg_count, true)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // Reuse current frame (tail call optimization)
        int frame_base = current_frame->stack_base;
        int needed_top = frame_base + function->max_regs;

        if (needed_top > STACK_MAX) {
            runtimeError(vm, "Stack overflow.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // Move arguments from callee_slot to frame_base
        for (int i = 0; i <= arg_count; i++) {
            vm->stack[frame_base + i] = vm->stack[callee_slot + i];
        }

        // Handle PARAM_REF after moving arguments
        if (!processParamRefAfterMove(vm, function, frame_base, arg_count)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // Update stack_top if needed
        if (needed_top > vm->stack_top) {
            vm->stack_top = needed_top;
        }

        // Jump into the function (restart from beginning)
        vm->ip = function->chunk->code;

        DISPATCH();
    }
    OP(SMART_TAIL_CALL) {
        // SMART_TAIL_CALL: Runtime check for upvalues, then TCO or normal call
        // If callee has upvalues, fall back to normal CALL
        // If callee has no upvalues, perform TAIL_CALL optimization
        uint32_t instr = vm->ip[-1];
        int callee_slot = CUR_BASE() + REG_A(instr);
        uint16_t arg_count = REG_Bx(instr);
        Value callee = vm->stack[callee_slot];

        // Dereference if callee is a reference
        if (!derefOperand(vm, &callee, "smart tail call")) return INTERPRET_RUNTIME_ERROR;
        vm->stack[callee_slot] = callee;

        // Resolve dispatcher overload if needed
        if (IS_DISPATCHER(callee)) {
            Value matched_closure = resolveOverload(vm, AS_DISPATCHER(callee), arg_count);
            if (IS_NULL(matched_closure)) {
                runtimeError(vm, "No overload found for %u arguments.", arg_count);
                return INTERPRET_RUNTIME_ERROR;
            }
            callee = matched_closure;
            vm->stack[callee_slot] = callee;
        }

        if (!IS_CLOSURE(callee)) {
            runtimeError(vm, ERR_ONLY_CALL_FUNCTIONS);
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjClosure* closure = AS_CLOSURE(callee);
        ObjFunction* function = closure->function;

        if (arg_count != function->arity) {
            runtimeError(vm, "Expected %d arguments but got %u.", function->arity, arg_count);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Handle ref/val parameters (use TCO mode - defer PARAM_REF handling)
        if (!processParamQualifiers(vm, function, callee_slot, arg_count, true)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // SMART MODE: Runtime check for upvalues
        if (closure->upvalue_count > 0) {
            // Callee has upvalues - fall back to normal CALL to avoid breaking closures
            if (vm->frame_count == FRAMES_MAX) {
                runtimeError(vm, "Stack overflow: maximum call depth (%d) reached.", FRAMES_MAX);
                return INTERPRET_RUNTIME_ERROR;
            }

            // Calculate required stack size and grow if needed
            int needed_top = callee_slot + function->max_regs;
            if (!growStackForCall(vm, needed_top, NULL)) {
                return INTERPRET_RUNTIME_ERROR;
            }

            if (needed_top > vm->stack_top) {
                vm->stack_top = needed_top;
            }

            // Push frame (normal call)
            CallFrame* frame = &vm->frames[vm->frame_count++];
            frame->closure      = closure;
            frame->ip           = vm->ip;
            frame->stack_base   = callee_slot;
            frame->caller_chunk = vm->chunk;

            // Enter callee
            vm->chunk = closure->function->chunk;
            vm->ip = function->chunk->code;
            DISPATCH();
        }

        // NO UPVALUES: Perform tail call optimization
        CallFrame* current_frame = &vm->frames[vm->frame_count - 1];
        int frame_base = current_frame->stack_base;
        int needed_top = frame_base + function->max_regs;

        // Grow stack if needed
        if (!growStackForCall(vm, needed_top, NULL)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        if (needed_top > vm->stack_top) {
            vm->stack_top = needed_top;
        }

        // Move args to the frame base
        for (int i = 0; i < arg_count; i++) {
            vm->stack[frame_base + 1 + i] = vm->stack[callee_slot + 1 + i];
        }

        // Handle PARAM_REF with non-reference args AFTER the move
        if (!processParamRefAfterMove(vm, function, frame_base, arg_count)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // Put the new callee in R0 of this frame
        vm->stack[frame_base] = callee;

        // Update the frame to point at the new closure
        current_frame->closure = closure;

        // Jump into the new function
        vm->chunk = function->chunk;
        vm->ip = function->chunk->code;

        DISPATCH();
    }
    OP(SMART_TAIL_CALL_SELF) {
        // Optimized recursive smart tail call - no global lookup needed
        // For recursive self-calls, we know we're calling the same function,
        // so we can skip upvalue checks and directly perform tail call
        uint32_t instr = vm->ip[-1];
        int callee_slot = CUR_BASE() + REG_A(instr);
        uint16_t arg_count = REG_Bx(instr);

        // Get the current frame's closure (the function we're already in)
        CallFrame* current_frame = &vm->frames[vm->frame_count - 1];
        ObjClosure* closure = current_frame->closure;
        ObjFunction* function = closure->function;

        // Place the closure in the callee slot
        vm->stack[callee_slot] = OBJ_VAL(closure);

        if (arg_count != function->arity) {
            runtimeError(vm, "Expected %d arguments but got %u.", function->arity, arg_count);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Handle ref/val parameters (use TCO mode - defer PARAM_REF handling)
        if (!processParamQualifiers(vm, function, callee_slot, arg_count, true)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // Check if function has upvalues
        if (closure->upvalue_count > 0) {
            // HAS UPVALUES: Fall back to normal call to preserve upvalue semantics
            // Close any upvalues in the current frame before making the call
            closeUpvalues(vm, &vm->stack[current_frame->stack_base]);

            if (vm->frame_count == FRAMES_MAX) {
                runtimeError(vm, "Stack overflow.");
                return INTERPRET_RUNTIME_ERROR;
            }

            // Calculate required stack size and grow if needed
            int needed_top = callee_slot + function->max_regs;
            if (!growStackForCall(vm, needed_top, NULL)) {
                return INTERPRET_RUNTIME_ERROR;
            }

            if (needed_top > vm->stack_top) {
                vm->stack_top = needed_top;
            }

            // Push frame (normal call)
            CallFrame* frame = &vm->frames[vm->frame_count++];
            frame->closure      = closure;
            frame->ip           = vm->ip;
            frame->stack_base   = callee_slot;
            frame->caller_chunk = vm->chunk;

            // Enter callee
            vm->ip = function->chunk->code;
            DISPATCH();
        }

        // NO UPVALUES: Perform tail call optimization
        int frame_base = current_frame->stack_base;
        int needed_top = frame_base + function->max_regs;

        if (needed_top > STACK_MAX) {
            runtimeError(vm, "Stack overflow.");
            return INTERPRET_RUNTIME_ERROR;
        }

        if (needed_top > vm->stack_top) {
            vm->stack_top = needed_top;
        }

        // Move arguments from callee_slot to frame_base
        for (int i = 0; i <= arg_count; i++) {
            vm->stack[frame_base + i] = vm->stack[callee_slot + i];
        }

        // Handle PARAM_REF after moving arguments
        if (!processParamRefAfterMove(vm, function, frame_base, arg_count)) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // Jump into the function (restart from beginning)
        vm->ip = function->chunk->code;

        DISPATCH();
    }
    OP(RET) {
        uint32_t instr = vm->ip[-1];
        if (vm->frame_count == 0) {
            return INTERPRET_OK;
        }

        int  ret_reg = REG_A(instr);
        bool implicit_null = (REG_Bx(instr) == 1);
        CallFrame* frame = &vm->frames[vm->frame_count - 1];

        // Get the return value BEFORE closing upvalues
        Value return_value = implicit_null
                           ? NULL_VAL
                           : vm->stack[frame->stack_base + ret_reg];

        // Protect any REF_LOCAL references in the return value (including nested in containers)
        // by converting them to REF_UPVALUE before the frame is popped
        protectLocalRefsInValue(vm, return_value, &vm->stack[frame->stack_base]);

        // Before we pop the frame, close any upvalues pointing to its stack slots.
        closeUpvalues(vm, &vm->stack[frame->stack_base]);

        // Now pop the callee frame
        vm->frame_count--;

        // Check if we're returning from a withPrompt boundary frame
        // If so, auto-pop the prompt that withPrompt installed
        if (vm->with_prompt_depth > 0) {
            WithPromptContext* wpc = &vm->with_prompt_stack[vm->with_prompt_depth - 1];
            if (vm->frame_count == wpc->frame_boundary) {
                popPrompt(vm);
                vm->with_prompt_depth--;
            }
        }

        // Check if we're returning from a resumed continuation's boundary frame
        // If so, redirect the return value to where resume() expects it
        if (vm->resume_depth > 0) {
            ResumeContext* ctx = &vm->resume_stack[vm->resume_depth - 1];
            if (vm->frame_count == ctx->frame_boundary) {
                // Resumed continuation has completed!
                // Redirect return value to where resume() expects it
                vm->stack[ctx->result_slot] = return_value;
                
                // Pop the resume context
                vm->resume_depth--;
                
                // Restore caller context and continue
                vm->ip    = frame->ip;
                vm->chunk = frame->caller_chunk;
                
                DISPATCH();
            }
        }

        // Normal return: restore caller context
        vm->ip    = frame->ip;
        vm->chunk = frame->caller_chunk;
        vm->stack[frame->stack_base] = return_value;

        DISPATCH();
    }
    OP(CLOSURE) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);

        // 1. Get the function template from the constant pool.
        ObjFunction* function = AS_FUNCTION(currentChunk(vm)->constants.values[bx]);

        // 2. Create the closure object.
        ObjClosure* closure = newClosure(vm, function);
        // Protect closure from GC during stack write and captureUpvalue calls
        pushTempRoot(vm, (Obj*)closure);

        vm->stack[a] = OBJ_VAL(closure);

        // 3. Capture the upvalues based on the "recipe" stored in the ObjFunction.
        // Get the current stack base (handles both main script with frame_count=0 and functions)
        int cur_base = CUR_BASE();
        for (int i = 0; i < closure->upvalue_count; i++) {
            uint8_t is_local = function->upvalues[i].is_local;
            uint8_t index = function->upvalues[i].index;
            if (is_local) {
                // Capture a local variable from the current (enclosing) function's stack frame.
                closure->upvalues[i] = captureUpvalue(vm, &vm->stack[cur_base + index]);
            } else {
                // Capture an upvalue from the enclosing function itself.
                // This requires an actual call frame (not the main script).
                if (vm->frame_count > 0) {
                    CallFrame* frame = &vm->frames[vm->frame_count - 1];
                    closure->upvalues[i] = frame->closure->upvalues[index];
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
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);

        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        // The value is read from the location the upvalue points to.
        Value value = *frame->closure->upvalues[bx]->location;

        // Don't auto-dereference - let references be first-class values
        // Dereferencing happens at use sites (arithmetic, print, etc.)
        vm->stack[a] = value;
        DISPATCH();
    }
    OP(SET_UPVALUE) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);

        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        Value* upvalue_location = frame->closure->upvalues[bx]->location;

        // If the upvalue holds a reference, write through the reference
        if (IS_REFERENCE(*upvalue_location)) {
            ObjReference* ref = AS_REFERENCE(*upvalue_location);
            Value new_value = vm->stack[a];

            switch (ref->ref_type) {
                case REF_LOCAL: {
                    // Check if the location contains a reference - if so, write through it recursively
                    Value current = *ref->as.local.location;
                    if (IS_REFERENCE(current)) {
                        if (!writeReferenceValue(vm, current, new_value)) {
                            runtimeError(vm, "Failed to write through nested reference in local.");
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    } else {
                        *ref->as.local.location = new_value;
                    }
                    break;
                }
                case REF_GLOBAL: {
                    // Check if the global contains a reference - if so, write through it recursively
                    Value current;
                    if (globalGet(vm, ref->as.global.global_name, &current) && IS_REFERENCE(current)) {
                        if (!writeReferenceValue(vm, current, new_value)) {
                            runtimeError(vm, "Failed to write through nested reference in global.");
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    } else {
                        if (!globalSet(vm, ref->as.global.global_name, new_value)) {
                            runtimeError(vm, "Failed to set global '%.*s' in SET_UPVALUE.",
                                ref->as.global.global_name->length, ref->as.global.global_name->chars);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                    break;
                }
                case REF_UPVALUE:
                    // Use the recursive helper to handle nested references
                    if (!writeReferenceValue(vm, OBJ_VAL(ref), new_value)) {
                        runtimeError(vm, "Failed to write through upvalue reference.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    break;
                case REF_INDEX: {
                    if (!IS_LIST(ref->as.index.container)) {
                        runtimeError(vm, ERR_INDEX_CONTAINER_NOT_LIST);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjList* list = AS_LIST(ref->as.index.container);
                    int idx = (int)AS_DOUBLE(ref->as.index.index);
                    if (!validateListIndex(vm, list, idx, "SET_UPVALUE")) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    list->items.values[idx] = new_value;
                    break;
                }
                case REF_PROPERTY: {
                    Value container = ref->as.property.container;
                    if (!IS_OBJ(container) || (!IS_MAP(container) && !IS_STRUCT_INSTANCE(container))) {
                        runtimeError(vm, "Property reference container is not a map or struct.");
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    ObjString* key_str = keyToString(vm, ref->as.property.key);
                    if (!key_str) {
                        runtimeError(vm, ERR_MAP_KEY_TYPE);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    if (IS_MAP(container)) {
                        ObjMap* map = AS_MAP(container);
                        tableSet(vm, map->table, key_str, new_value);
                    } else { // IS_STRUCT_INSTANCE - already validated above
                        ObjStructInstance* instance = AS_STRUCT_INSTANCE(container);
                        Value index_val;
                        if (!tableGet(instance->schema->field_to_index, key_str, &index_val)) {
                            runtimeError(vm, "Struct field '%.*s' does not exist.", key_str->length, key_str->chars);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        int field_index = (int)AS_DOUBLE(index_val);
                        instance->fields[field_index] = new_value;
                    }
                    break;
                }
            }
        } else {
            // Normal upvalue: write directly
            // Check for circular reference: if assigning a reference to a local,
            // make sure it doesn't point back to the same local (directly or indirectly)
            Value new_value = vm->stack[a];
            if (IS_REFERENCE(new_value)) {
                ObjReference* new_ref = AS_REFERENCE(new_value);
                if (referencesLocal(vm, new_ref, upvalue_location, 0)) {
                    runtimeError(vm, "Circular reference: cannot assign local variable to a reference that points back to itself.");
                    return INTERPRET_RUNTIME_ERROR;
                }
            }

            // Special post-assignment check for references only:
            // If the new value is a reference, verify we didn't create a circular reference
            // by attempting to fully dereference it after assignment
            if (IS_REFERENCE(new_value)) {
                Value old_value = *upvalue_location;  // Save in case we need to rollback
                if (!validateUpvalue(vm, frame->closure->upvalues[bx], "SET_UPVALUE")) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                *upvalue_location = new_value;

                Value test_deref;
                if (!dereferenceValue(vm, new_value, &test_deref)) {
                    // Dereferencing failed - likely hit a cycle!
                    // Rollback the assignment
                    *upvalue_location = old_value;
                    runtimeError(vm, "Circular reference: cannot assign local variable to a reference that points back to itself.");
                    return INTERPRET_RUNTIME_ERROR;
                }
            } else {
                // Not a reference, just write normally
                if (!validateUpvalue(vm, frame->closure->upvalues[bx], "SET_UPVALUE")) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                *upvalue_location = new_value;
            }
        }
        DISPATCH();
    }
    OP(SLOT_SET_UPVALUE) {
        // SLOT_SET_UPVALUE: rebind the upvalue
        // If the upvalue holds a "binding reference" (REF_GLOBAL, REF_PROPERTY, REF_INDEX),
        // we write through to that binding. This ensures slot parameters work correctly.
        // For other reference types and non-references, we just replace the upvalue's value.
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        uint16_t bx = REG_Bx(instr);

        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        Value* upvalue_location = frame->closure->upvalues[bx]->location;
        Value new_value = vm->stack[a];

        // Special handling for binding references (slot parameters)
        if (IS_REFERENCE(*upvalue_location)) {
            ObjReference* ref = AS_REFERENCE(*upvalue_location);
            switch (ref->ref_type) {
                case REF_GLOBAL: {
                    // Global slot parameter - update the global
                    if (!globalSet(vm, ref->as.global.global_name, new_value)) {
                        runtimeError(vm, "Failed to set global '%.*s' in SLOT_SET_UPVALUE.",
                            ref->as.global.global_name->length, ref->as.global.global_name->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    DISPATCH();
                }
                case REF_PROPERTY: {
                    // Property slot parameter - update the map property
                    Value container = ref->as.property.container;
                    Value key = ref->as.property.key;
                    if (IS_MAP(container)) {
                        ObjMap* map = AS_MAP(container);
                        ObjString* key_str = keyToString(vm, key);
                        if (!key_str) {
                            runtimeError(vm, ERR_PROPERTY_KEY_TYPE);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        tableSet(vm, map->table, key_str, new_value);
                    }
                    DISPATCH();
                }
                case REF_INDEX: {
                    // Index slot parameter - update the container element
                    Value container = ref->as.index.container;
                    Value index = ref->as.index.index;
                    if (IS_LIST(container)) {
                        ObjList* list = AS_LIST(container);
                        int idx = (int)AS_DOUBLE(index);
                        if (idx >= 0 && idx < list->items.count) {
                            list->items.values[idx] = new_value;
                        }
                    } else if (IS_MAP(container)) {
                        ObjMap* map = AS_MAP(container);
                        ObjString* key_str = keyToString(vm, index);
                        if (!key_str) {
                            runtimeError(vm, ERR_INDEX_TYPE);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        tableSet(vm, map->table, key_str, new_value);
                    }
                    DISPATCH();
                }
                default:
                    // For other reference types (REF_LOCAL, REF_UPVALUE), fall through to replace
                    break;
            }
        }

        // For non-binding references and non-references, just replace the upvalue's value
        if (!validateUpvalue(vm, frame->closure->upvalues[bx], "SLOT_SET_UPVALUE")) {
            return INTERPRET_RUNTIME_ERROR;
        }
        *upvalue_location = new_value;
        DISPATCH();
    }
    OP(CLOSE_UPVALUE) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        closeUpvalues(vm, &vm->stack[a]);
        DISPATCH();
    }
    OP(CLOSE_FRAME_UPVALUES) {
        // Close all upvalues for the current frame
        // Used before TAIL_CALL to ensure upvalues are closed before we overwrite the stack
        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        closeUpvalues(vm, &vm->stack[frame->stack_base]);
        DISPATCH();
    }
    OP(NEW_LIST) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int count = REG_Bx(instr);
        fflush(stdout);
        ObjList* list = newList(vm);
        // Protect the list from GC during writeValueArray and stack write
        pushTempRoot(vm, (Obj*)list);
        // If count > 0, copy elements from subsequent stack slots.
        // (This makes our opcode future-proof for optimizations).
        for (int i = 0; i < count; i++) {
            writeValueArray(vm, &list->items, vm->stack[a + 1 + i]);
        }
        vm->stack[a] = OBJ_VAL(list);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(LIST_APPEND) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // list
        int b = CUR_BASE() + REG_B(instr); // value
        Value list_val = vm->stack[a];
        if (!IS_LIST(list_val)) {
            runtimeError(vm, "Can only append to a list.");
            return INTERPRET_RUNTIME_ERROR;
        }
        ObjList* list = AS_LIST(list_val);
        Value value_to_append = vm->stack[b];
        writeValueArray(vm, &list->items, value_to_append);
        DISPATCH();
    }
    OP(LIST_SPREAD) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // target list
        int b = CUR_BASE() + REG_B(instr); // source list to spread
        Value target_val = vm->stack[a];
        Value source_val = vm->stack[b];
        if (!IS_LIST(target_val)) {
            runtimeError(vm, "Spread target must be a list.");
            return INTERPRET_RUNTIME_ERROR;
        }
        if (!IS_LIST(source_val)) {
            runtimeError(vm, "Spread source must be a list.");
            return INTERPRET_RUNTIME_ERROR;
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
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        ObjMap* map = newMap(vm);
        // Protect the map from GC during stack write
        pushTempRoot(vm, (Obj*)map);
        vm->stack[a] = OBJ_VAL(map);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(MAP_SET) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Map register
        int b = CUR_BASE() + REG_B(instr); // Key register
        int c = CUR_BASE() + REG_C(instr); // Value register
        Value map_val = vm->stack[a];
        Value key_val = vm->stack[b];
        Value value_val = vm->stack[c];

        if (!IS_MAP(map_val)) {
            runtimeError(vm, "MAP_SET expects a map object.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjMap* map = AS_MAP(map_val);
        ObjString* key_str = keyToString(vm, key_val);
        if (!key_str) {
            runtimeError(vm, ERR_MAP_KEYS_TYPE);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Set the key-value pair (or skip if value is null)
        if (!IS_NULL(value_val)) {
            tableSet(vm, map->table, key_str, value_val);
        }
        DISPATCH();
    }
    OP(MAP_SPREAD) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // target map
        int b = CUR_BASE() + REG_B(instr); // source map to spread
        Value target_val = vm->stack[a];
        Value source_val = vm->stack[b];
        if (!IS_MAP(target_val)) {
            runtimeError(vm, "Spread target must be a map.");
            return INTERPRET_RUNTIME_ERROR;
        }
        if (!IS_MAP(source_val)) {
            runtimeError(vm, "Spread source must be a map.");
            return INTERPRET_RUNTIME_ERROR;
        }
        ObjMap* target = AS_MAP(target_val);
        ObjMap* source = AS_MAP(source_val);
        // Copy all key-value pairs from source to target
        for (int i = 0; i < source->table->capacity; i++) {
            Entry* entry = &source->table->entries[i];
            if (entry->key != NULL) {
                tableSet(vm, target->table, entry->key, entry->value);
            }
        }
        DISPATCH();
    }
    OP(GET_SUBSCRIPT) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register
        int b = CUR_BASE() + REG_B(instr); // Object register (list or map)
        int c = CUR_BASE() + REG_C(instr); // Index/Key register
        Value obj_val = vm->stack[b];
        Value key_val = vm->stack[c];

        // Dereference container if it's a reference
        if (!derefContainer(vm, &obj_val, "access subscript")) return INTERPRET_RUNTIME_ERROR;

        // Handle maps
        if (IS_MAP(obj_val)) {
            ObjMap* map = AS_MAP(obj_val);
            ObjString* key_str = keyToString(vm, key_val);
            if (!key_str) {
                runtimeError(vm, ERR_MAP_KEYS_TYPE);
                return INTERPRET_RUNTIME_ERROR;
            }

            Value result;
            if (tableGet(map->table, key_str, &result)) {
                vm->stack[a] = result;
            } else {
                vm->stack[a] = NULL_VAL;
            }
            DISPATCH();
        }

        // Handle lists
        if (!IS_LIST(obj_val)) {
            runtimeError(vm, ERR_ONLY_SUBSCRIPT_LISTS_MAPS);
            return INTERPRET_RUNTIME_ERROR;
        }
        ObjList* list = AS_LIST(obj_val);
        if (!IS_DOUBLE(key_val)) {
            runtimeError(vm, ERR_LIST_INDEX_TYPE);
            return INTERPRET_RUNTIME_ERROR;
        }
        double index_double = AS_DOUBLE(key_val);
        int index = (int)index_double;
        if (index != index_double) {
            runtimeError(vm, "List index must be an integer.");
            return INTERPRET_RUNTIME_ERROR;
        }
        if (index < 0 || index >= list->items.count) {
            runtimeError(vm, "List index out of bounds.");
            return INTERPRET_RUNTIME_ERROR;
        }
        Value result = list->items.values[index];
        // Don't auto-dereference - refs are first-class values
        vm->stack[a] = result;
        DISPATCH();
    }
    OP(SET_SUBSCRIPT) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Object register (list or map)
        int b = CUR_BASE() + REG_B(instr); // Index/Key register
        int c = CUR_BASE() + REG_C(instr); // Value register
        Value obj_val = vm->stack[a];
        Value key_val = vm->stack[b];
        Value value_val = vm->stack[c];

        // Dereference container if it's a reference
        if (!derefContainer(vm, &obj_val, "set subscript")) return INTERPRET_RUNTIME_ERROR;

        // Handle maps
        if (IS_MAP(obj_val)) {
            ObjMap* map = AS_MAP(obj_val);
            ObjString* key_str = keyToString(vm, key_val);
            if (!key_str) {
                runtimeError(vm, ERR_MAP_KEYS_TYPE);
                return INTERPRET_RUNTIME_ERROR;
            }

            // Check if we're setting through a reference
            Value existing;
            if (tableGet(map->table, key_str, &existing)) {
                if (IS_REFERENCE(existing)) {
                    if (!writeThruReference(vm, AS_REFERENCE(existing), value_val, false)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    DISPATCH();
                } else if (IS_OBJ(existing) && IS_NATIVE_REFERENCE(existing)) {
                    if (!writeReferenceValue(vm, existing, value_val)) {
                        runtimeError(vm, "Failed to write through native reference in map subscript.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    DISPATCH();
                }
            }

            // Delete key if value is null
            if (IS_NULL(value_val)) {
                tableDelete(map->table, key_str);
            } else {
                tableSet(vm, map->table, key_str, value_val);
            }
            DISPATCH();
        }

        // Handle lists
        if (!IS_LIST(obj_val)) {
            runtimeError(vm, ERR_ONLY_SUBSCRIPT_LISTS_MAPS);
            return INTERPRET_RUNTIME_ERROR;
        }
        ObjList* list = AS_LIST(obj_val);
        if (!IS_DOUBLE(key_val)) {
            runtimeError(vm, ERR_LIST_INDEX_TYPE);
            return INTERPRET_RUNTIME_ERROR;
        }
        double index_double = AS_DOUBLE(key_val);
        int index = (int)index_double;
        if (index != index_double) {
            runtimeError(vm, "List index must be an integer.");
            return INTERPRET_RUNTIME_ERROR;
        }
        if (index < 0 || index >= list->items.count) {
            runtimeError(vm, "List index out of bounds.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // Check if we're setting through a reference
        Value existing = list->items.values[index];
        if (IS_REFERENCE(existing)) {
            if (!writeThruReference(vm, AS_REFERENCE(existing), value_val, false)) {
                return INTERPRET_RUNTIME_ERROR;
            }
        } else if (IS_OBJ(existing) && IS_NATIVE_REFERENCE(existing)) {
            if (!writeReferenceValue(vm, existing, value_val)) {
                runtimeError(vm, "Failed to write through native reference in list.");
                return INTERPRET_RUNTIME_ERROR;
            }
        } else {
            list->items.values[index] = value_val;
        }
        DISPATCH();
    }
    OP(SLOT_SET_SUBSCRIPT) {
        // SLOT_SET_SUBSCRIPT: directly replace the element value, bypassing reference dereferencing
        // This is used for the `slot` keyword which rebinds elements instead of writing through references
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Object register (list or map)
        int b = CUR_BASE() + REG_B(instr); // Index/Key register
        int c = CUR_BASE() + REG_C(instr); // Value register
        Value obj_val = vm->stack[a];
        Value key_val = vm->stack[b];
        Value value_val = vm->stack[c];

        // Dereference container if it's a reference
        if (!derefContainer(vm, &obj_val, "slot set subscript")) return INTERPRET_RUNTIME_ERROR;

        // Handle maps
        if (IS_MAP(obj_val)) {
            ObjMap* map = AS_MAP(obj_val);
            ObjString* key_str = keyToString(vm, key_val);
            if (!key_str) {
                runtimeError(vm, ERR_MAP_KEYS_TYPE);
                return INTERPRET_RUNTIME_ERROR;
            }

            // Direct assignment without checking for references
            if (IS_NULL(value_val)) {
                tableDelete(map->table, key_str);
            } else {
                tableSet(vm, map->table, key_str, value_val);
            }
            DISPATCH();
        }

        // Handle lists
        if (!IS_LIST(obj_val)) {
            runtimeError(vm, ERR_ONLY_SUBSCRIPT_LISTS_MAPS);
            return INTERPRET_RUNTIME_ERROR;
        }
        ObjList* list = AS_LIST(obj_val);
        if (!IS_DOUBLE(key_val)) {
            runtimeError(vm, ERR_LIST_INDEX_TYPE);
            return INTERPRET_RUNTIME_ERROR;
        }
        double index_double = AS_DOUBLE(key_val);
        int index = (int)index_double;
        if (index != index_double) {
            runtimeError(vm, "List index must be an integer.");
            return INTERPRET_RUNTIME_ERROR;
        }
        if (index < 0 || index >= list->items.count) {
            runtimeError(vm, "List index out of bounds.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // Direct assignment to list element without checking for references
        list->items.values[index] = value_val;
        DISPATCH();
    }
    OP(GET_MAP_PROPERTY) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register
        int b = CUR_BASE() + REG_B(instr); // Container register (map or struct)
        int c = CUR_BASE() + REG_C(instr); // Key register (must be a string)
        Value container_val = vm->stack[b];
        Value key_val = vm->stack[c];

        // Dereference container if it's a reference
        if (!derefContainer(vm, &container_val, "access property")) return INTERPRET_RUNTIME_ERROR;

        if (!IS_STRING(key_val)) {
            runtimeError(vm, "Property key must be a string.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjString* key_str = AS_STRING(key_val);

        // Handle struct instances
        if (IS_STRUCT_INSTANCE(container_val)) {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(container_val);

            // Look up field by name
            Value index_val;
            if (tableGet(instance->schema->field_to_index, key_str, &index_val)) {
                int field_index = (int)AS_DOUBLE(index_val);
                vm->stack[a] = instance->fields[field_index];
            } else {
                runtimeError(vm, "Struct '%s' has no field '%s'.",
                             instance->schema->name->chars, key_str->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            DISPATCH();
        }

        // Handle maps
        if (!IS_MAP(container_val)) {
            runtimeError(vm, ERR_ONLY_MAPS);
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjMap* map = AS_MAP(container_val);
        Value result;
        if (tableGet(map->table, key_str, &result)) {
            vm->stack[a] = result;
        } else {
            vm->stack[a] = NULL_VAL;
        }
        DISPATCH();
    }
    OP(SET_MAP_PROPERTY) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Container register (map or struct)
        int b = CUR_BASE() + REG_B(instr); // Key register (must be a string)
        int c = CUR_BASE() + REG_C(instr); // Value register
        Value container_val = vm->stack[a];
        Value key_val = vm->stack[b];
        Value value_val = vm->stack[c];

        // Dereference container if it's a reference
        if (!derefContainer(vm, &container_val, "set property")) return INTERPRET_RUNTIME_ERROR;

        if (!IS_STRING(key_val)) {
            runtimeError(vm, "Property key must be a string.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjString* key_str = AS_STRING(key_val);

        // Handle struct instances
        if (IS_STRUCT_INSTANCE(container_val)) {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(container_val);

            // Look up field by name
            Value index_val;
            if (tableGet(instance->schema->field_to_index, key_str, &index_val)) {
                int field_index = (int)AS_DOUBLE(index_val);

                // Check if field value is a reference and write through it
                Value current = instance->fields[field_index];
                if (IS_REFERENCE(current) || (IS_OBJ(current) && IS_NATIVE_REFERENCE(current))) {
                    if (!writeReferenceValue(vm, current, value_val)) {
                        runtimeError(vm, "Failed to write through reference in struct field.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else {
                    instance->fields[field_index] = value_val;
                }
            } else {
                runtimeError(vm, "Struct '%s' has no field '%s'.",
                             instance->schema->name->chars, key_str->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            DISPATCH();
        }

        // Handle maps
        if (!IS_MAP(container_val)) {
            runtimeError(vm, ERR_ONLY_MAPS);
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjMap* map = AS_MAP(container_val);

        // Check if the existing value is a reference - if so, write through it
        Value existing;
        if (tableGet(map->table, key_str, &existing)) {
            if (IS_REFERENCE(existing)) {
                if (!writeThruReference(vm, AS_REFERENCE(existing), value_val, true)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                DISPATCH();
            } else if (IS_OBJ(existing) && IS_NATIVE_REFERENCE(existing)) {
                // Write through native reference
                if (!writeReferenceValue(vm, existing, value_val)) {
                    runtimeError(vm, "Failed to write through native reference in map.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                DISPATCH();
            }
        }

        // Not a reference - normal set/delete
        if (IS_NULL(value_val)) {
            tableDelete(map->table, key_str);
        } else {
            tableSet(vm, map->table, key_str, value_val);
        }
        DISPATCH();
    }
    OP(SLOT_SET_MAP_PROPERTY) {
        // SLOT_SET_MAP_PROPERTY: directly replace the property value, bypassing reference dereferencing
        // This is used for the `slot` keyword which rebinds properties instead of writing through references
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Map register
        int b = CUR_BASE() + REG_B(instr); // Key register (must be a string)
        int c = CUR_BASE() + REG_C(instr); // Value register
        Value map_val = vm->stack[a];
        Value key_val = vm->stack[b];
        Value value_val = vm->stack[c];

        // Dereference container if it's a reference
        if (!derefContainer(vm, &map_val, "slot set property")) {
            return INTERPRET_RUNTIME_ERROR;
        }

        if (!IS_MAP(map_val)) {
            runtimeError(vm, ERR_ONLY_MAPS);
            return INTERPRET_RUNTIME_ERROR;
        }

        if (!IS_STRING(key_val)) {
            runtimeError(vm, "Map property key must be a string.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjMap* map = AS_MAP(map_val);
        ObjString* key_str = AS_STRING(key_val);

        // Direct assignment without checking for references
        if (IS_NULL(value_val)) {
            tableDelete(map->table, key_str);
        } else {
            tableSet(vm, map->table, key_str, value_val);
        }
        DISPATCH();
    }
    OP(NEW_DISPATCHER) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        ObjDispatcher* dispatcher = newDispatcher(vm);
        pushTempRoot(vm, (Obj*)dispatcher);
        vm->stack[a] = OBJ_VAL(dispatcher);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(ADD_OVERLOAD) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Dispatcher register
        int b = CUR_BASE() + REG_B(instr); // Closure register

        Value disp_val = vm->stack[a];
        Value closure_val = vm->stack[b];

        if (!IS_DISPATCHER(disp_val)) {
            runtimeError(vm, "ADD_OVERLOAD requires a dispatcher.");
            return INTERPRET_RUNTIME_ERROR;
        }

        if (!IS_CLOSURE(closure_val)) {
            runtimeError(vm, "ADD_OVERLOAD requires a closure.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjDispatcher* dispatcher = AS_DISPATCHER(disp_val);
        ObjClosure* closure = AS_CLOSURE(closure_val);

        if (dispatcher->count >= MAX_OVERLOADS) {
            runtimeError(vm, "Too many overloads (max %d).", MAX_OVERLOADS);
            return INTERPRET_RUNTIME_ERROR;
        }

        dispatcher->overloads[dispatcher->count++] = (Obj*)closure;
        DISPATCH();
    }
    OP(CLONE_VALUE) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register
        int b = CUR_BASE() + REG_B(instr); // Source register

        Value source = vm->stack[b];
        Value cloned = cloneValue(vm, source);
        // Protect cloned object before stack write (which can trigger GC via tableSet)
        if (IS_OBJ(cloned)) pushTempRoot(vm, AS_OBJ(cloned));
        vm->stack[a] = cloned;
        if (IS_OBJ(cloned)) popTempRoot(vm);
        DISPATCH();
    }
    OP(DEEP_CLONE_VALUE) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register
        int b = CUR_BASE() + REG_B(instr); // Source register

        Value source = vm->stack[b];
        Value cloned = deepCloneValue(vm, source);
        // Protect cloned object before stack write (which can trigger GC via tableSet)
        if (IS_OBJ(cloned)) pushTempRoot(vm, AS_OBJ(cloned));
        vm->stack[a] = cloned;
        if (IS_OBJ(cloned)) popTempRoot(vm);
        DISPATCH();
    }
    OP(MAKE_REF) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register for reference
        int b = CUR_BASE() + REG_B(instr); // Target register to reference

        // Flatten: if target is itself a reference, follow the chain
        Value target_value = vm->stack[b];

        if (IS_REFERENCE(target_value)) {
            Value flattened = flattenReference(vm, AS_REFERENCE(target_value));
            if (IS_NULL(flattened)) return INTERPRET_RUNTIME_ERROR;  // Circular reference detected
            // Protect the flattened reference before stack write (which can trigger GC)
            if (IS_OBJ(flattened)) pushTempRoot(vm, AS_OBJ(flattened));
            vm->stack[a] = flattened;
            if (IS_OBJ(flattened)) popTempRoot(vm);
        } else {
            // Target is not a reference, create a reference to the target's stack slot
            ObjReference* ref = newStackSlotReference(vm, b);
            pushTempRoot(vm, (Obj*)ref);
            vm->stack[a] = OBJ_VAL(ref);
            popTempRoot(vm);
        }
        DISPATCH();
    }
    OP(SLOT_MAKE_REF) {
        // SLOT_MAKE_REF: Like MAKE_REF but does NOT flatten references
        // Creates a reference to the stack slot itself, regardless of whether it holds a ref
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register for reference
        int b = CUR_BASE() + REG_B(instr); // Target register to reference

        // Always create a reference to the target's stack slot, even if it holds a reference
        ObjReference* ref = newStackSlotReference(vm, b);
        pushTempRoot(vm, (Obj*)ref);
        vm->stack[a] = OBJ_VAL(ref);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(MAKE_GLOBAL_REF) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register for reference
        int bx = REG_Bx(instr);            // Constant index for global name

        Value name_val = vm->chunk->constants.values[bx];
        if (!IS_STRING(name_val)) {
            runtimeError(vm, "MAKE_GLOBAL_REF requires a string constant.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjString* target_name = AS_STRING(name_val);

        // Flatten: if target is itself a reference, follow the chain
        Value target_value;
        if (globalGet(vm, target_name, &target_value) && IS_REFERENCE(target_value)) {
            ObjReference* target_ref = AS_REFERENCE(target_value);
            if (target_ref->ref_type == REF_GLOBAL) {
                // Target is a global reference, use its target instead
                target_name = target_ref->as.global.global_name;
            } else {
                // Other reference types: use flattening helper
                Value flattened = flattenReference(vm, target_ref);
                if (IS_NULL(flattened)) return INTERPRET_RUNTIME_ERROR;  // Circular reference detected
                // Protect the flattened reference before stack write (which can trigger GC)
                if (IS_OBJ(flattened)) pushTempRoot(vm, AS_OBJ(flattened));
                vm->stack[a] = flattened;
                if (IS_OBJ(flattened)) popTempRoot(vm);
                DISPATCH();
            }
        }

        ObjReference* ref = newGlobalReference(vm, target_name);
        pushTempRoot(vm, (Obj*)ref);
        vm->stack[a] = OBJ_VAL(ref);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(SLOT_MAKE_GLOBAL_REF) {
        // SLOT_MAKE_GLOBAL_REF: Like MAKE_GLOBAL_REF but does NOT flatten references
        // Creates a reference to the global variable itself, regardless of whether it holds a ref
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register for reference
        int bx = REG_Bx(instr);            // Constant index for global name

        Value name_val = vm->chunk->constants.values[bx];
        if (!IS_STRING(name_val)) {
            runtimeError(vm, "SLOT_MAKE_GLOBAL_REF requires a string constant.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjString* target_name = AS_STRING(name_val);

        // Always create a reference to the global, even if it holds a reference
        ObjReference* ref = newGlobalReference(vm, target_name);
        pushTempRoot(vm, (Obj*)ref);
        vm->stack[a] = OBJ_VAL(ref);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(MAKE_UPVALUE_REF) {
        // MAKE_UPVALUE_REF: Creates a reference to an upvalue (REF_UPVALUE type)
        // This allows refs to follow the upvalue lifecycle correctly
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register for reference
        uint16_t bx = REG_Bx(instr);       // Upvalue index

        CallFrame* frame = &vm->frames[vm->frame_count - 1];
        ObjUpvalue* upvalue = frame->closure->upvalues[bx];

        if (upvalue == NULL) {
            runtimeError(vm, "Attempted to create reference to NULL upvalue.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // Create a reference to the upvalue itself
        ObjReference* ref = newUpvalueReference(vm, upvalue);
        pushTempRoot(vm, (Obj*)ref);
        vm->stack[a] = OBJ_VAL(ref);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(MAKE_INDEX_REF) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register for reference
        int b = CUR_BASE() + REG_B(instr); // Container register (array)
        int c = CUR_BASE() + REG_C(instr); // Index register

        Value container = vm->stack[b];
        Value index = vm->stack[c];

        // Dereference container if it's a reference (for cases like: ref myRefToArray[0])
        if (!derefContainer(vm, &container, "create index reference")) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // Validate that the index/key exists before creating reference
        if (!IS_OBJ(container)) {
            runtimeError(vm, "Cannot create reference to non-object container.");
            return INTERPRET_RUNTIME_ERROR;
        }

        if (IS_LIST(container)) {
            if (!IS_DOUBLE(index)) {
                runtimeError(vm, ERR_LIST_INDEX_TYPE);
                return INTERPRET_RUNTIME_ERROR;
            }

            ObjList* list = AS_LIST(container);
            int idx = (int)AS_DOUBLE(index);
            if (!validateListIndex(vm, list, idx, "MAKE_INDEX_REF")) {
                return INTERPRET_RUNTIME_ERROR;
            }
        } else if (IS_MAP(container)) {
            ObjMap* map = AS_MAP(container);

            // Convert key to string
            ObjString* key_str = NULL;
            if (IS_STRING(index)) {
                key_str = AS_STRING(index);
            } else if (IS_DOUBLE(index)) {
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "%g", AS_DOUBLE(index));
                key_str = copyString(vm, buffer, strlen(buffer));
            } else {
                runtimeError(vm, ERR_MAP_KEYS_TYPE);
                return INTERPRET_RUNTIME_ERROR;
            }

            Value dummy;
            if (!tableGet(map->table, key_str, &dummy)) {
                runtimeError(vm, "Cannot create reference: map key does not exist.");
                return INTERPRET_RUNTIME_ERROR;
            }
        } else {
            runtimeError(vm, "Cannot create reference to non-list/non-map container.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // Check if the value at the index/key is itself a reference - if so, flatten it
        Value element_value;
        bool has_value = false;

        if (IS_LIST(container)) {
            ObjList* list = AS_LIST(container);
            int idx = (int)AS_DOUBLE(index);
            element_value = list->items.values[idx];
            has_value = true;
        } else if (IS_MAP(container)) {
            ObjMap* map = AS_MAP(container);
            ObjString* key_str = keyToString(vm, index);
            if (key_str) {
                has_value = tableGet(map->table, key_str, &element_value);
            }
        }

        // Flatten: if the element contains a reference, create a reference to what it points to
        if (has_value && IS_REFERENCE(element_value)) {
            Value flattened = flattenReference(vm, AS_REFERENCE(element_value));
            if (IS_NULL(flattened)) return INTERPRET_RUNTIME_ERROR;  // Circular reference detected
            // Protect the flattened reference before writeValue (which can trigger GC)
            if (IS_OBJ(flattened)) pushTempRoot(vm, AS_OBJ(flattened));
            vm->stack[a] = flattened;
            if (IS_OBJ(flattened)) popTempRoot(vm);
            DISPATCH();
        }

        // Create an index reference (no flattening needed)
        ObjReference* ref = newIndexReference(vm, container, index);
        pushTempRoot(vm, (Obj*)ref);
        vm->stack[a] = OBJ_VAL(ref);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(MAKE_PROPERTY_REF) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register for reference
        int b = CUR_BASE() + REG_B(instr); // Container register (map)
        int c = CUR_BASE() + REG_C(instr); // Key register

        Value container = vm->stack[b];
        Value key = vm->stack[c];

        // Dereference container if it's a reference (for cases like: ref myRefToMap.field)
        if (!derefContainer(vm, &container, "create property reference")) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // Validate that container is map or struct
        if (!IS_OBJ(container) || (!IS_MAP(container) && !IS_STRUCT_INSTANCE(container))) {
            runtimeError(vm, "Cannot create property reference: container is not a map or struct.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // Convert key to string
        ObjString* key_str = NULL;
        if (IS_STRING(key)) {
            key_str = AS_STRING(key);
        } else if (IS_DOUBLE(key)) {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%g", AS_DOUBLE(key));
            key_str = copyString(vm, buffer, strlen(buffer));
        } else {
            runtimeError(vm, ERR_MAP_KEYS_TYPE);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Validate that the property/field exists and get the value
        Value element_value;
        if (IS_MAP(container)) {
            ObjMap* map = AS_MAP(container);
            if (!tableGet(map->table, key_str, &element_value)) {
                runtimeError(vm, "Cannot create reference: map property does not exist.");
                return INTERPRET_RUNTIME_ERROR;
            }
        } else if (IS_STRUCT_INSTANCE(container)) {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(container);
            Value index_val;
            if (!tableGet(instance->schema->field_to_index, key_str, &index_val)) {
                runtimeError(vm, "Cannot create reference: struct field '%.*s' does not exist.", key_str->length, key_str->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            int field_index = (int)AS_DOUBLE(index_val);
            element_value = instance->fields[field_index];
        }

        // Flatten: if the property contains a reference, create a reference to what it points to
        if (IS_REFERENCE(element_value)) {
            Value flattened = flattenReference(vm, AS_REFERENCE(element_value));
            if (IS_NULL(flattened)) return INTERPRET_RUNTIME_ERROR;  // Circular reference detected
            // Protect the flattened reference before stack write (which can trigger GC)
            if (IS_OBJ(flattened)) pushTempRoot(vm, AS_OBJ(flattened));
            vm->stack[a] = flattened;
            if (IS_OBJ(flattened)) popTempRoot(vm);
            DISPATCH();
        }

        // Create a property reference (no flattening needed)
        ObjReference* ref = newPropertyReference(vm, container, key);
        pushTempRoot(vm, (Obj*)ref);
        vm->stack[a] = OBJ_VAL(ref);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(SLOT_MAKE_PROPERTY_REF) {
        // SLOT_MAKE_PROPERTY_REF: Create a reference to a map property WITHOUT flattening
        // This is used for slot parameters to maintain the binding, not the value
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register for reference
        int b = CUR_BASE() + REG_B(instr); // Container register (map)
        int c = CUR_BASE() + REG_C(instr); // Key register

        Value container = vm->stack[b];
        Value key = vm->stack[c];

        // Dereference container if it's a reference
        if (!derefContainer(vm, &container, "create slot property reference")) {
            return INTERPRET_RUNTIME_ERROR;
        }

        if (!IS_OBJ(container) || (!IS_MAP(container) && !IS_STRUCT_INSTANCE(container))) {
            runtimeError(vm, "Cannot create property reference: container is not a map or struct.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // Convert key to string
        ObjString* key_str = NULL;
        if (IS_STRING(key)) {
            key_str = AS_STRING(key);
        } else if (IS_DOUBLE(key)) {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%g", AS_DOUBLE(key));
            key_str = copyString(vm, buffer, strlen(buffer));
        } else {
            runtimeError(vm, ERR_MAP_KEYS_TYPE);
            return INTERPRET_RUNTIME_ERROR;
        }

        // Validate that the property/field exists
        Value element_value;
        if (IS_MAP(container)) {
            ObjMap* map = AS_MAP(container);
            if (!tableGet(map->table, key_str, &element_value)) {
                runtimeError(vm, "Cannot create reference: map property does not exist.");
                return INTERPRET_RUNTIME_ERROR;
            }
        } else if (IS_STRUCT_INSTANCE(container)) {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(container);
            Value index_val;
            if (!tableGet(instance->schema->field_to_index, key_str, &index_val)) {
                runtimeError(vm, "Cannot create reference: struct field '%.*s' does not exist.", key_str->length, key_str->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            int field_index = (int)AS_DOUBLE(index_val);
            element_value = instance->fields[field_index];
        }

        // NO flattening - create a reference to the property binding itself
        ObjReference* ref = newPropertyReference(vm, container, key);
        pushTempRoot(vm, (Obj*)ref);
        vm->stack[a] = OBJ_VAL(ref);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(SLOT_MAKE_INDEX_REF) {
        // SLOT_MAKE_INDEX_REF: Create a reference to array/map element WITHOUT flattening
        // This is used for slot parameters to maintain the binding, not the value
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register for reference
        int b = CUR_BASE() + REG_B(instr); // Container register
        int c = CUR_BASE() + REG_C(instr); // Index register

        Value container = vm->stack[b];
        Value index = vm->stack[c];

        // Dereference container if it's a reference
        if (!derefContainer(vm, &container, "create slot index reference")) {
            return INTERPRET_RUNTIME_ERROR;
        }

        // Validate container and index
        if (!IS_OBJ(container)) {
            runtimeError(vm, "Cannot create index reference: container is not an object.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // NO flattening - create a reference to the element binding itself
        ObjReference* ref = newIndexReference(vm, container, index);
        pushTempRoot(vm, (Obj*)ref);
        vm->stack[a] = OBJ_VAL(ref);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(DEREF_GET) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register
        int b = CUR_BASE() + REG_B(instr); // Reference register

        Value ref_val = vm->stack[b];
        if (!IS_REFERENCE(ref_val)) {
            runtimeError(vm, "DEREF_GET requires a reference.");
            return INTERPRET_RUNTIME_ERROR;
        }

        Value value;
        if (!dereferenceValue(vm, ref_val, &value)) {
            runtimeError(vm, "Failed to dereference value.");
            return INTERPRET_RUNTIME_ERROR;
        }
        vm->stack[a] = value;
        DISPATCH();
    }
    OP(DEREF_SET) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Reference register (or value register if not a ref)
        int b = CUR_BASE() + REG_B(instr); // Value register

        Value ref_val = vm->stack[a];
        // If the value is not a reference, just do a normal assignment
        // This handles cases where a variable might or might not hold a reference
        if (!IS_REFERENCE(ref_val)) {
            vm->stack[a] = vm->stack[b];
            DISPATCH();
        }

        ObjReference* ref = AS_REFERENCE(ref_val);
        Value new_value = vm->stack[b];

        switch (ref->ref_type) {
            case REF_LOCAL: {
                // Check if the location contains a reference - if so, write through it
                Value current = *ref->as.local.location;
                if (IS_REFERENCE(current)) {
                    if (!writeReferenceValue(vm, current, new_value)) {
                        runtimeError(vm, "Failed to write through nested reference in local.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else {
                    *ref->as.local.location = new_value;
                }
                break;
            }
            case REF_GLOBAL: {
                // Check if the global contains a reference - if so, write through it
                Value current;
                if (globalGet(vm, ref->as.global.global_name, &current) && IS_REFERENCE(current)) {
                    if (!writeReferenceValue(vm, current, new_value)) {
                        runtimeError(vm, "Failed to write through nested reference in global.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else {
                    if (!globalSet(vm, ref->as.global.global_name, new_value)) {
                        runtimeError(vm, "Failed to set global '%.*s' in DEREF_SET.",
                            ref->as.global.global_name->length, ref->as.global.global_name->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }
                break;
            }
            case REF_INDEX: {
                Value container = ref->as.index.container;
                Value index = ref->as.index.index;

                if (!IS_OBJ(container)) {
                    runtimeError(vm, ERR_INDEX_CONTAINER_NOT_OBJECT);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Handle both lists and maps
                if (IS_LIST(container)) {
                    if (!IS_DOUBLE(index)) {
                        runtimeError(vm, ERR_LIST_INDEX_TYPE);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    ObjList* list = AS_LIST(container);
                    int idx = (int)AS_DOUBLE(index);
                    if (!validateListIndex(vm, list, idx, "DEREF_SET")) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (!writeThruListElement(vm, list, idx, new_value)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (IS_MAP(container)) {
                    ObjMap* map = AS_MAP(container);
                    ObjString* key_str = keyToString(vm, index);
                    if (!key_str) {
                        runtimeError(vm, ERR_MAP_KEYS_TYPE);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (!writeThruMapField(vm, map, key_str, new_value)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else {
                    runtimeError(vm, "Index reference container must be a list or map.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case REF_PROPERTY: {
                Value container = ref->as.property.container;
                Value key = ref->as.property.key;

                if (!IS_OBJ(container) || (!IS_MAP(container) && !IS_STRUCT_INSTANCE(container))) {
                    runtimeError(vm, "Property reference container is not a map or struct.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjString* key_str = keyToString(vm, key);
                if (!key_str) {
                    runtimeError(vm, ERR_MAP_KEY_TYPE);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (IS_MAP(container)) {
                    ObjMap* map = AS_MAP(container);
                    tableSet(vm, map->table, key_str, new_value);
                } else { // IS_STRUCT_INSTANCE - already validated above
                    ObjStructInstance* instance = AS_STRUCT_INSTANCE(container);
                    Value index_val;
                    if (!tableGet(instance->schema->field_to_index, key_str, &index_val)) {
                        runtimeError(vm, "Struct field '%.*s' does not exist.", key_str->length, key_str->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int field_index = (int)AS_DOUBLE(index_val);
                    instance->fields[field_index] = new_value;
                }
                break;
            }
            case REF_UPVALUE: {
                // Write through the upvalue (follows upvalue lifecycle)
                if (!validateUpvalue(vm, ref->as.upvalue.upvalue, "DEREF_SET")) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                // Check if the upvalue's location contains a reference - if so, write through it
                Value current = *ref->as.upvalue.upvalue->location;
                if (IS_REFERENCE(current)) {
                    if (!writeReferenceValue(vm, current, new_value)) {
                        runtimeError(vm, "Failed to write through nested reference in upvalue.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else {
                    if (!validateUpvalue(vm, ref->as.upvalue.upvalue, "DEREF_SET upvalue write")) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    *ref->as.upvalue.upvalue->location = new_value;
                }
                break;
            }
        }
        DISPATCH();
    }
    OP(SLOT_DEREF_SET) {
        // SLOT_DEREF_SET: Like DEREF_SET but only dereferences ONE level
        // If the target holds a reference, replace that reference's value directly
        // without following any further reference chains
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Reference register
        int b = CUR_BASE() + REG_B(instr); // Value register

        Value ref_val = vm->stack[a];
        Value new_value = vm->stack[b];

        // If not a reference, just assign directly
        if (!IS_REFERENCE(ref_val)) {
            vm->stack[a] = new_value;
            DISPATCH();
        }

        // Dereference one level and replace directly (don't follow further refs)
        ObjReference* ref = AS_REFERENCE(ref_val);

        switch (ref->ref_type) {
            case REF_LOCAL: {
                // Replace the value at the referenced location directly
                // SLOT_DEREF_SET is only used for explicit rebinding (slot r = value)
                // so we never write through nested references
                *ref->as.local.location = new_value;
                break;
            }
            case REF_GLOBAL: {
                // Replace the global variable directly
                // SLOT_DEREF_SET is only used for explicit rebinding (slot r = value)
                // so we never write through nested references
                if (!globalSet(vm, ref->as.global.global_name, new_value)) {
                    runtimeError(vm, "Failed to set global '%.*s' in SLOT_DEREF_SET.",
                        ref->as.global.global_name->length, ref->as.global.global_name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case REF_INDEX: {
                // Replace array/map element directly
                Value container = ref->as.index.container;
                Value index = ref->as.index.index;

                if (!IS_OBJ(container)) {
                    runtimeError(vm, ERR_INDEX_CONTAINER_NOT_OBJECT);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (IS_LIST(container)) {
                    if (!IS_DOUBLE(index)) {
                        runtimeError(vm, ERR_LIST_INDEX_TYPE);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    ObjList* list = AS_LIST(container);
                    int idx = (int)AS_DOUBLE(index);
                    if (!validateListIndex(vm, list, idx, "SLOT_DEREF_SET")) {
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    // Replace directly, even if current value is a reference
                    list->items.values[idx] = new_value;
                } else if (IS_MAP(container)) {
                    ObjMap* map = AS_MAP(container);
                    ObjString* key_str = keyToString(vm, index);
                    if (!key_str) {
                        runtimeError(vm, ERR_MAP_KEY_TYPE);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    // Replace directly, even if current value is a reference
                    tableSet(vm, map->table, key_str, new_value);
                } else {
                    runtimeError(vm, "Index reference container must be a list or map.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case REF_PROPERTY: {
                // Replace map/struct property/field directly
                Value container = ref->as.property.container;
                Value key = ref->as.property.key;

                if (!IS_OBJ(container) || (!IS_MAP(container) && !IS_STRUCT_INSTANCE(container))) {
                    runtimeError(vm, "Property reference container is not a map or struct.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjString* key_str = keyToString(vm, key);
                if (!key_str) {
                    runtimeError(vm, ERR_MAP_KEY_TYPE);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Replace directly, even if current value is a reference
                if (IS_MAP(container)) {
                    ObjMap* map = AS_MAP(container);
                    tableSet(vm, map->table, key_str, new_value);
                } else { // IS_STRUCT_INSTANCE - already validated above
                    ObjStructInstance* instance = AS_STRUCT_INSTANCE(container);
                    Value index_val;
                    if (!tableGet(instance->schema->field_to_index, key_str, &index_val)) {
                        runtimeError(vm, "Struct field '%.*s' does not exist.", key_str->length, key_str->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int field_index = (int)AS_DOUBLE(index_val);
                    instance->fields[field_index] = new_value;
                }
                break;
            }
            case REF_UPVALUE: {
                // Replace the upvalue's value directly
                if (!validateUpvalue(vm, ref->as.upvalue.upvalue, "SLOT_DEREF_SET")) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                *ref->as.upvalue.upvalue->location = new_value;
                break;
            }
        }
        DISPATCH();
    }
    OP(NEW_STRUCT) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register
        int bx = REG_Bx(instr);             // Schema constant index

        Value schema_val = vm->chunk->constants.values[bx];
        if (!IS_STRUCT_SCHEMA(schema_val)) {
            runtimeError(vm, "NEW_STRUCT requires a struct schema constant.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjStructSchema* schema = AS_STRUCT_SCHEMA(schema_val);
        ObjStructInstance* instance = newStructInstance(vm, schema);
        // Protect instance before writing to stack (which can trigger GC)
        pushTempRoot(vm, (Obj*)instance);
        vm->stack[a] = OBJ_VAL(instance);
        popTempRoot(vm);
        DISPATCH();
    }
    OP(STRUCT_SPREAD) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // target struct
        int b = CUR_BASE() + REG_B(instr); // source struct to spread
        Value target_val = vm->stack[a];
        Value source_val = vm->stack[b];
        if (!IS_STRUCT_INSTANCE(target_val)) {
            runtimeError(vm, "Spread target must be a struct instance.");
            return INTERPRET_RUNTIME_ERROR;
        }
        if (!IS_STRUCT_INSTANCE(source_val)) {
            runtimeError(vm, "Spread source must be a struct instance.");
            return INTERPRET_RUNTIME_ERROR;
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
                runtimeError(vm, "Cannot spread struct '%s' into struct '%s' - incompatible types.",
                            source->schema->name->chars, target->schema->name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
        }

        // Copy all fields from source to target
        for (int i = 0; i < source->schema->field_count; i++) {
            target->fields[i] = source->fields[i];
        }
        DISPATCH();
    }
    OP(GET_STRUCT_FIELD) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Destination register
        int b = CUR_BASE() + REG_B(instr); // Struct instance register
        int c = REG_C(instr);               // Field index

        Value struct_val = vm->stack[b];

        // Dereference container if it's a reference
        if (!derefContainer(vm, &struct_val, "get struct field")) return INTERPRET_RUNTIME_ERROR;

        if (!IS_STRUCT_INSTANCE(struct_val)) {
            runtimeError(vm, "GET_STRUCT_FIELD requires a struct instance.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjStructInstance* instance = AS_STRUCT_INSTANCE(struct_val);
        if (c < 0 || c >= instance->schema->field_count) {
            runtimeError(vm, "Struct field index out of bounds.");
            return INTERPRET_RUNTIME_ERROR;
        }

        vm->stack[a] = instance->fields[c];
        DISPATCH();
    }
    OP(SET_STRUCT_FIELD) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Struct instance register
        int b = REG_B(instr);               // Field index
        int c = CUR_BASE() + REG_C(instr); // Value register

        Value struct_val = vm->stack[a];
        Value new_value = vm->stack[c];

        // Dereference container if it's a reference
        if (!derefContainer(vm, &struct_val, "set struct field")) return INTERPRET_RUNTIME_ERROR;

        if (!IS_STRUCT_INSTANCE(struct_val)) {
            runtimeError(vm, "SET_STRUCT_FIELD requires a struct instance.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjStructInstance* instance = AS_STRUCT_INSTANCE(struct_val);
        if (b < 0 || b >= instance->schema->field_count) {
            runtimeError(vm, "Struct field index out of bounds.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // Check if field value is a reference and dereference if needed
        Value current = instance->fields[b];
        if (IS_REFERENCE(current)) {
            if (!writeReferenceValue(vm, current, new_value)) {
                runtimeError(vm, "Failed to write through reference in struct field.");
                return INTERPRET_RUNTIME_ERROR;
            }
        } else {
            instance->fields[b] = new_value;
        }
        DISPATCH();
    }
    OP(SLOT_SET_STRUCT_FIELD) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr); // Struct instance register
        int b = REG_B(instr);               // Field index
        int c = CUR_BASE() + REG_C(instr); // Value register

        Value struct_val = vm->stack[a];
        Value new_value = vm->stack[c];

        // Dereference container if it's a reference
        if (!derefContainer(vm, &struct_val, "slot set struct field")) return INTERPRET_RUNTIME_ERROR;

        if (!IS_STRUCT_INSTANCE(struct_val)) {
            runtimeError(vm, "SLOT_SET_STRUCT_FIELD requires a struct instance.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjStructInstance* instance = AS_STRUCT_INSTANCE(struct_val);
        if (b < 0 || b >= instance->schema->field_count) {
            runtimeError(vm, "Struct field index out of bounds.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // Direct assignment, bypassing references (slot semantics)
        instance->fields[b] = new_value;
        DISPATCH();
    }
    OP(PRE_INC) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value original_b = vm->stack[b];
        Value val_b = original_b;

        // Auto-dereference if it's a reference
        if (!derefOperand(vm, &val_b, "pre-increment operation")) {
            return INTERPRET_RUNTIME_ERROR;
        }

        if (!IS_DOUBLE(val_b)) {
            runtimeError(vm, "Pre-increment operand must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }

        double new_value = AS_DOUBLE(val_b) + 1.0;
        Value result = DOUBLE_VAL(new_value);

        // Write back to original location
        // If the original was a reference, write through it; otherwise write directly
        if (IS_REFERENCE(original_b)) {
            if (!writeReferenceValue(vm, original_b, result)) {
                runtimeError(vm, "Failed to write through reference in PRE_INC.");
                return INTERPRET_RUNTIME_ERROR;
            }
        } else {
            vm->stack[b] = result;
        }

        // Store result in destination register (new value)
        vm->stack[a] = result;

        DISPATCH();
    }
    OP(POST_INC) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value original_b = vm->stack[b];
        Value val_b = original_b;

        // Auto-dereference if it's a reference
        if (!derefOperand(vm, &val_b, "post-increment operation")) {
            return INTERPRET_RUNTIME_ERROR;
        }

        if (!IS_DOUBLE(val_b)) {
            runtimeError(vm, "Post-increment operand must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }

        double old_value = AS_DOUBLE(val_b);
        double new_value = old_value + 1.0;

        // Write new value back to original location
        // If the original was a reference, write through it; otherwise write directly
        if (IS_REFERENCE(original_b)) {
            ObjReference* ref = AS_REFERENCE(original_b);
            if (!writeReferenceValue(vm, original_b, DOUBLE_VAL(new_value))) {
                runtimeError(vm, "Failed to write through reference in POST_INC.");
                return INTERPRET_RUNTIME_ERROR;
            }
        } else {
            vm->stack[b] = DOUBLE_VAL(new_value);
        }

        // Store OLD value in destination register (key difference!)
        vm->stack[a] = DOUBLE_VAL(old_value);

        DISPATCH();
    }
    OP(PRE_DEC) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value original_b = vm->stack[b];
        Value val_b = original_b;

        // Auto-dereference if it's a reference
        if (!derefOperand(vm, &val_b, "pre-decrement operation")) {
            return INTERPRET_RUNTIME_ERROR;
        }

        if (!IS_DOUBLE(val_b)) {
            runtimeError(vm, "Pre-decrement operand must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }

        double new_value = AS_DOUBLE(val_b) - 1.0;
        Value result = DOUBLE_VAL(new_value);

        // Write back to original location
        // If the original was a reference, write through it; otherwise write directly
        if (IS_REFERENCE(original_b)) {
            if (!writeReferenceValue(vm, original_b, result)) {
                runtimeError(vm, "Failed to write through reference in PRE_DEC.");
                return INTERPRET_RUNTIME_ERROR;
            }
        } else {
            vm->stack[b] = result;
        }

        // Store result in destination register (new value)
        vm->stack[a] = result;

        DISPATCH();
    }
    OP(POST_DEC) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value original_b = vm->stack[b];
        Value val_b = original_b;

        // Auto-dereference if it's a reference
        if (!derefOperand(vm, &val_b, "post-decrement operation")) {
            return INTERPRET_RUNTIME_ERROR;
        }

        if (!IS_DOUBLE(val_b)) {
            runtimeError(vm, "Post-decrement operand must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }

        double old_value = AS_DOUBLE(val_b);
        double new_value = old_value - 1.0;

        // Write new value back to original location
        // If the original was a reference, write through it; otherwise write directly
        if (IS_REFERENCE(original_b)) {
            if (!writeReferenceValue(vm, original_b, DOUBLE_VAL(new_value))) {
                runtimeError(vm, "Failed to write through reference in POST_DEC.");
                return INTERPRET_RUNTIME_ERROR;
            }
        } else {
            vm->stack[b] = DOUBLE_VAL(new_value);
        }

        // Store OLD value in destination register (key difference!)
        vm->stack[a] = DOUBLE_VAL(old_value);

        DISPATCH();
    }

    OP(TYPEOF) {
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value val_b = vm->stack[b];

        // Do NOT dereference - we want to check if it's a reference itself
        // This allows typeof to distinguish between a reference and the value it points to
        const char* type_name = NULL;
        if (IS_DOUBLE(val_b)) {
            type_name = "number";
        } else if (IS_BOOL(val_b)) {
            type_name = "boolean";
        } else if (IS_NULL(val_b)) {
            type_name = "null";
        } else if (IS_ENUM(val_b)) {
            type_name = "enum";
        } else if (IS_OBJ(val_b)) {
            Obj* obj = AS_OBJ(val_b);
            switch (obj->type) {
                case OBJ_STRING:
                    type_name = "string";
                    break;
                case OBJ_FUNCTION:
                case OBJ_CLOSURE:
                case OBJ_DISPATCHER:
                    type_name = "function";
                    break;
                case OBJ_NATIVE_FUNCTION:
                    type_name = "native_function";
                    break;
                case OBJ_NATIVE_CLOSURE:
                    type_name = "native_closure";
                    break;
                case OBJ_LIST:
                    type_name = "list";
                    break;
                case OBJ_MAP:
                    type_name = "map";
                    break;
                case OBJ_REFERENCE:
                    type_name = "reference";
                    break;
                case OBJ_NATIVE_REFERENCE:
                    type_name = "native_reference";
                    break;
                case OBJ_NATIVE_CONTEXT:
                    type_name = "native_context";
                    break;
                case OBJ_STRUCT_SCHEMA:
                    type_name = "struct_schema";
                    break;
                case OBJ_STRUCT_INSTANCE:
                    type_name = "struct";
                    break;
                case OBJ_ENUM_SCHEMA:
                    type_name = "enum_schema";
                    break;
                case OBJ_UPVALUE:
                    type_name = "upvalue";
                    break;
                case OBJ_INT64:
                    type_name = "number";
                    break;
                case OBJ_PROMPT_TAG:
                    type_name = "prompt_tag";
                    break;
                case OBJ_CONTINUATION:
                    type_name = "continuation";
                    break;
                default:
                    type_name = "unknown";
                    break;
            }
        } else {
            type_name = "unknown";
        }

        // Create a string object with the type name
        ObjString* type_string = copyString(vm, type_name, strlen(type_name));
        vm->stack[a] = OBJ_VAL(type_string);

        DISPATCH();
    }

    // ========================================================================
    // Delimited Continuations Opcodes
    // ========================================================================

    OP(PUSH_PROMPT) {
        // PUSH_PROMPT Ra - Push prompt boundary with tag in Ra
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        Value tag_val = vm->stack[a];

        if (!IS_PROMPT_TAG(tag_val)) {
            runtimeError(vm, "PUSH_PROMPT: expected a prompt tag.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjPromptTag* tag = AS_PROMPT_TAG(tag_val);
        if (!pushPrompt(vm, tag)) {
            return INTERPRET_RUNTIME_ERROR;  // Error already reported
        }

        DISPATCH();
    }

    OP(POP_PROMPT) {
        // POP_PROMPT - Remove topmost prompt from prompt stack
        popPrompt(vm);
        DISPATCH();
    }

    OP(CAPTURE) {
        // CAPTURE Ra, Rb - Capture continuation to prompt tag in Ra, store in Rb
        // Control transfers to the prompt's withPrompt location
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value tag_val = vm->stack[a];

        if (!IS_PROMPT_TAG(tag_val)) {
            runtimeError(vm, "CAPTURE: expected a prompt tag.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjPromptTag* tag = AS_PROMPT_TAG(tag_val);
        
        // Find the prompt to determine where the continuation should resume
        PromptEntry* prompt = findPrompt(vm, tag);
        if (prompt == NULL) {
            runtimeError(vm, "CAPTURE: prompt tag not found.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // Calculate the return slot relative to the prompt's stack base
        // This is where the resume value will be placed
        int return_slot = b - prompt->stack_base;

        // Capture the continuation
        ObjContinuation* cont = captureContinuation(vm, tag, return_slot);
        if (cont == NULL) {
            return INTERPRET_RUNTIME_ERROR;  // Error already reported
        }

        // Remove the captured frames and stack from the VM
        // Unwind to the prompt's frame
        vm->frame_count = prompt->frame_index;
        vm->stack_top = prompt->stack_base;

        // Restore IP/chunk to the prompt's context
        if (vm->frame_count > 0) {
            CallFrame* frame = &vm->frames[vm->frame_count - 1];
            vm->ip = frame->ip;
            vm->chunk = frame->caller_chunk ? frame->caller_chunk : frame->closure->function->chunk;
        }

        // Pop the prompt (it's been used)
        popPrompt(vm);

        // Place the continuation object as the result
        // The withPrompt will receive this as its return value
        // Put it in the slot that withPrompt expects (the result destination)
        // For now, we push it on top of the current stack position
        vm->stack[vm->stack_top] = OBJ_VAL(cont);
        vm->stack_top++;

        DISPATCH();
    }

    OP(RESUME) {
        // RESUME Ra, Rb, Rc - Resume continuation in Ra with value in Rb, result in Rc
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        // int c = CUR_BASE() + REG_C(instr);  // Result slot (handled by continuation)
        Value cont_val = vm->stack[a];
        Value resume_val = vm->stack[b];

        if (!IS_CONTINUATION(cont_val)) {
            runtimeError(vm, "RESUME: expected a continuation.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjContinuation* cont = AS_CONTINUATION(cont_val);

        if (!resumeContinuation(vm, cont, resume_val)) {
            return INTERPRET_RUNTIME_ERROR;  // Error already reported
        }

        // After resume, execution continues from the continuation's capture point
        // The IP and chunk have been restored by resumeContinuation
        DISPATCH();
    }

    OP(ABORT) {
        // ABORT Ra, Rb - Abort to prompt tag in Ra with value in Rb
        // Control transfers to the prompt's withPrompt, skipping capture
        uint32_t instr = vm->ip[-1];
        int a = CUR_BASE() + REG_A(instr);
        int b = CUR_BASE() + REG_B(instr);
        Value tag_val = vm->stack[a];
        Value abort_val = vm->stack[b];

        if (!IS_PROMPT_TAG(tag_val)) {
            runtimeError(vm, "ABORT: expected a prompt tag.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjPromptTag* tag = AS_PROMPT_TAG(tag_val);

        // Find the prompt
        PromptEntry* prompt = findPrompt(vm, tag);
        if (prompt == NULL) {
            runtimeError(vm, "ABORT: prompt tag not found.");
            return INTERPRET_RUNTIME_ERROR;
        }

        // Close upvalues in the region being aborted
        closeUpvalues(vm, &vm->stack[prompt->stack_base]);

        // Unwind to the prompt's frame
        vm->frame_count = prompt->frame_index;
        vm->stack_top = prompt->stack_base;

        // Restore IP/chunk to the prompt's context
        if (vm->frame_count > 0) {
            CallFrame* frame = &vm->frames[vm->frame_count - 1];
            vm->ip = frame->ip;
            vm->chunk = frame->caller_chunk ? frame->caller_chunk : frame->closure->function->chunk;
        }

        // Pop the prompt (it's been used)
        popPrompt(vm);

        // Place the abort value as the result
        vm->stack[vm->stack_top] = abort_val;
        vm->stack_top++;

        DISPATCH();
    }
#undef OP
#undef DISPATCH
#undef CUR_BASE
#undef BINARY_OP
#undef BINARY_COMPARE
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

    // On return, resume at the API trampoline, not bytecode.
    frame->ip           = vm->api_trampoline.code;
    frame->caller_chunk = &vm->api_trampoline;

    // Enter the callee
    vm->chunk = function->chunk;
    vm->ip    = function->chunk->code;

    InterpretResult result = run(vm);

    // Result is placed in stack[frame_base] by OP(RET); expose that at API top.
    vm->api_stack_top = frame_base;
    return result;
}

Value zym_call_getResult(VM* vm) {
    // The result of the last C API call is at the top of the API stack.
    return vm->stack[vm->api_stack_top];
}