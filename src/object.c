#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "./object.h"
#include "./memory.h"
#include "./vm.h"
#include "./table.h"
#include "./gc.h"
#include "./utf8.h"

#define ALLOCATE_OBJ(vm, type, objectType) (type*)allocateObject(vm, sizeof(type), objectType)

Obj* allocateObject(VM* vm, size_t size, ObjType type) {
    Obj* object = (Obj*)reallocate(vm, NULL, 0, size);
    object->type = type;
    object->is_marked = false;

    object->next = vm->objects;
    vm->objects = object;

    return object;
}

// Allocate the string as ONE block (header + content tail) and intern it.
// `src` is a borrowed, non-GC buffer: allocateObject may collect, but plain
// heap memory is never moved or freed by the GC, so copying after the
// allocation is safe. The free path in gc.c must mirror this size.
static ObjString* allocateString(VM* vm, const char* src, int byte_length, uint32_t hash) {
    ObjString* string = (ObjString*)allocateObject(vm,
        sizeof(ObjString) + (size_t)byte_length + 1, OBJ_STRING);
    string->byte_length = byte_length;
    string->hash = hash;
    memcpy(string->chars, src, (size_t)byte_length);
    string->chars[byte_length] = '\0';
    string->length = utf8_strlen(string->chars, byte_length);

    pushTempRoot(vm, (Obj*)string);
    tableSet(vm, &vm->strings, string, NULL_VAL);
    popTempRoot(vm);
    return string;
}

static uint32_t hashString(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

ObjString* takeString(VM* vm, char* chars, int length) {
    uint32_t hash = hashString(chars, length);
    ObjString* interned = tableFindString(&vm->strings, chars, length, hash);
    if (interned != NULL) {
        reallocate(vm, chars, length + 1, 0);
        return interned;
    }

    // Content is copied into the string's tail; the donated buffer (always
    // a vm-accounted allocation of length+1, per this function's contract)
    // is released either way.
    ObjString* result = allocateString(vm, chars, length, hash);
    reallocate(vm, chars, length + 1, 0);
    return result;
}

ObjString* copyString(VM* vm, const char* chars, int length) {
    uint32_t hash = hashString(chars, length);
    ObjString* interned = tableFindString(&vm->strings, chars, length, hash);
    if (interned != NULL) {
        return interned;
    }

    // No intermediate heap copy anymore — the single allocation in
    // allocateString IS the copy.
    return allocateString(vm, chars, length, hash);
}

ObjString* concatStrings(VM* vm, ObjString* a, ObjString* b) {
    // Buffer-free concat: stream the hash across both halves, probe the
    // intern table with a two-segment compare, and only on a miss write
    // the content directly into the new string's tail. One allocation,
    // two memcpys, no temporary. GC safety: `a` and `b` are reachable
    // from the caller's stack slots and strings never move, so the
    // pointers stay valid across the allocation.
    uint32_t hash = 2166136261u;
    for (int i = 0; i < a->byte_length; i++) {
        hash ^= (uint8_t)a->chars[i];
        hash *= 16777619;
    }
    for (int i = 0; i < b->byte_length; i++) {
        hash ^= (uint8_t)b->chars[i];
        hash *= 16777619;
    }

    ObjString* interned = tableFindStringPair(&vm->strings,
        a->chars, a->byte_length, b->chars, b->byte_length, hash);
    if (interned != NULL) return interned;

    int byte_length = a->byte_length + b->byte_length;
    ObjString* string = (ObjString*)allocateObject(vm,
        sizeof(ObjString) + (size_t)byte_length + 1, OBJ_STRING);
    string->byte_length = byte_length;
    string->hash = hash;
    memcpy(string->chars, a->chars, (size_t)a->byte_length);
    memcpy(string->chars + a->byte_length, b->chars, (size_t)b->byte_length);
    string->chars[byte_length] = '\0';
    string->length = utf8_strlen(string->chars, byte_length);

    pushTempRoot(vm, (Obj*)string);
    tableSet(vm, &vm->strings, string, NULL_VAL);
    popTempRoot(vm);
    return string;
}

ObjFunction* newFunction(VM* vm) {
    ObjFunction* function = (ObjFunction*)allocateObject(vm, sizeof(ObjFunction), OBJ_FUNCTION);

    function->arity = 0;
    function->fixed_arity = 0;
    function->is_variadic = false;
    function->upvalue_count = 0;
    function->upvalue_capacity = 0;
    function->upvalues = NULL;
    function->max_regs = 1;
    function->spill_count = 0;
    function->name = NULL;
    function->module_name = NULL;
    initChunk(&function->chunk);

    return function;
}

ObjNativeFunction* newNativeFunction(VM* vm, ObjString* name, int arity, void* func_ptr, NativeDispatcher dispatcher) {
    ObjNativeFunction* native = (ObjNativeFunction*)allocateObject(vm, sizeof(ObjNativeFunction), OBJ_NATIVE_FUNCTION);

    native->name = name;
    native->arity = arity;
    native->func_ptr = func_ptr;
    native->dispatcher = dispatcher;
    native->variadic_dispatcher = NULL;
    native->is_variadic = false;
    return native;
}

ObjNativeContext* newNativeContext(VM* vm, void* native_data, NativeFinalizerFunc finalizer) {
    ObjNativeContext* context = (ObjNativeContext*)allocateObject(vm, sizeof(ObjNativeContext), OBJ_NATIVE_CONTEXT);
    context->native_data = native_data;
    context->finalizer = finalizer;
    return context;
}

ObjNativeClosure* newNativeClosure(VM* vm, ObjString* name, int arity, void* func_ptr, NativeDispatcher dispatcher, Value context) {
    ObjNativeClosure* closure = (ObjNativeClosure*)allocateObject(vm, sizeof(ObjNativeClosure), OBJ_NATIVE_CLOSURE);

    closure->name = name;
    closure->arity = arity;
    closure->func_ptr = func_ptr;
    closure->dispatcher = dispatcher;
    closure->variadic_dispatcher = NULL;
    closure->context = context;
    closure->is_variadic = false;
    return closure;
}


ObjClosure* newClosure(VM* vm, ObjFunction* function) {
    // GC SAFETY: `function` is reached from the caller's C stack only, and
    // the allocation below can trigger GC before the closure exists. The
    // compiler emits OP(CLOSURE) reading the function out of
    // `chunk->constants`, which is marked via `markChunk`, so this is
    // usually safe — but we root it here defensively to make the ownership
    // model independent of every call site's discipline.
    pushTempRoot(vm, (Obj*)function);

    // Single allocation: header + upvalue pointer array in one block (the
    // array is a flexible member on the closure's tail). The free path in
    // gc.c must mirror this size for the bytes_allocated accounting.
    int count = function->upvalue_count;
    ObjClosure* closure = (ObjClosure*)allocateObject(vm,
        sizeof(ObjClosure) + sizeof(ObjUpvalue*) * (size_t)count, OBJ_CLOSURE);
    // CRITICAL: `upvalue_count` must be set before any allocation that
    // could trigger GC; otherwise the closure is reachable (via
    // vm->objects) with garbage in the fields `blackenObject(OBJ_CLOSURE)`
    // dereferences. No allocation happens between these stores today, but
    // be explicit so a future edit can't accidentally introduce one.
    closure->function = function;
    closure->upvalue_count = count;
    for (int i = 0; i < count; i++) {
        closure->upvalues[i] = NULL;
    }

    popTempRoot(vm);
    return closure;
}

ObjList* newList(VM* vm) {
    ObjList* list = ALLOCATE_OBJ(vm, ObjList, OBJ_LIST);
    initValueArray(&list->items);
    return list;
}

ObjMap* newMap(VM* vm) {
    ObjMap* map = ALLOCATE_OBJ(vm, ObjMap, OBJ_MAP);
    initTable(&map->table);
    return map;
}

ObjDispatcher* newDispatcher(VM* vm) {
    ObjDispatcher* dispatcher = ALLOCATE_OBJ(vm, ObjDispatcher, OBJ_DISPATCHER);
    dispatcher->count = 0;
    dispatcher->variadic_fallback = NULL;
    dispatcher->variadic_min_arity = 0;
    for (int i = 0; i < MAX_OVERLOADS; i++) {
        dispatcher->overloads[i] = NULL;
    }
    return dispatcher;
}


ObjStructSchema* newStructSchema(VM* vm, ObjString* name, ObjString** field_names, int field_count) {
    ObjStructSchema* schema = ALLOCATE_OBJ(vm, ObjStructSchema, OBJ_STRUCT_SCHEMA);

    schema->name = name;
    schema->field_count = field_count;
    schema->field_names = field_names;

    return schema;
}

ObjStructInstance* newStructInstance(VM* vm, ObjStructSchema* schema) {
    // Single allocation: instance header + fields array in one contiguous block.
    // Eliminates a second heap allocation and improves cache locality.
    size_t size = sizeof(ObjStructInstance) + sizeof(Value) * schema->field_count;
    ObjStructInstance* instance = (ObjStructInstance*)allocateObject(vm, size, OBJ_STRUCT_INSTANCE);

    instance->schema = schema;
    instance->field_count = schema->field_count;
    // Fields are stored immediately after the struct header
    instance->fields = (Value*)(instance + 1);

    for (int i = 0; i < schema->field_count; i++) {
        instance->fields[i] = NULL_VAL;
    }

    return instance;
}

ObjEnumSchema* newEnumSchema(VM* vm, ObjString* name, ObjString** variant_names, int variant_count) {
    ObjEnumSchema* schema = ALLOCATE_OBJ(vm, ObjEnumSchema, OBJ_ENUM_SCHEMA);
    schema->name = name;
    schema->variant_count = variant_count;
    schema->variant_names = variant_names;
    schema->type_id = vm->next_enum_type_id++;
    return schema;
}

ObjPromptTag* newPromptTag(VM* vm, ObjString* name) {
    ObjPromptTag* tag = ALLOCATE_OBJ(vm, ObjPromptTag, OBJ_PROMPT_TAG);
    tag->id = vm->next_prompt_tag_id++;
    tag->name = name;
    return tag;
}

ObjContinuation* newContinuation(VM* vm) {
    ObjContinuation* cont = ALLOCATE_OBJ(vm, ObjContinuation, OBJ_CONTINUATION);
    cont->frames = NULL;
    cont->frame_count = 0;
    cont->stack = NULL;
    cont->stack_size = 0;
    cont->stack_base_offset = 0;
    cont->spill = NULL;
    cont->spill_size = 0;
    cont->spill_base_offset = 0;
    cont->prompts = NULL;
    cont->prompt_count = 0;
    cont->resumes = NULL;
    cont->resume_count = 0;
    cont->frame_base_offset = 0;
    cont->saved_ip = NULL;
    cont->saved_chunk = NULL;
    cont->saved_owner = NULL;
    cont->prompt_tag = NULL;
    cont->state = CONT_VALID;
    cont->return_slot = 0;
    cont->preempt_shield_depth = 0;
    return cont;
}

void printObject(Value value) {
    switch (OBJ_TYPE(value)) {
        case OBJ_STRING:
            printf("%s", AS_CSTRING(value));
            break;
        case OBJ_LIST: {
                ObjList* list = AS_LIST(value);
                printf("[");
                for (int i = 0; i < list->items.count; i++) {
                    printValue(NULL, list->items.values[i]);
                    if (i < list->items.count - 1) {
                        printf(", ");
                    }
                }
                printf("]");
                break;
        }
        case OBJ_DISPATCHER:
            printf("<overloaded function>");
            break;
        case OBJ_STRUCT_SCHEMA: {
            ObjStructSchema* schema = AS_STRUCT_SCHEMA(value);
            printf("<struct %s>", schema->name->chars);
            break;
        }
        case OBJ_STRUCT_INSTANCE: {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(value);
            printf("%s {", instance->schema->name->chars);
            for (int i = 0; i < instance->schema->field_count; i++) {
                if (i > 0) printf(", ");
                printf("%s: ", instance->schema->field_names[i]->chars);
                printValue(NULL, instance->fields[i]);
            }
            printf("}");
            break;
        }
        case OBJ_ENUM_SCHEMA: {
            ObjEnumSchema* schema = AS_ENUM_SCHEMA(value);
            printf("<enum %s>", schema->name->chars);
            break;
        }
        case OBJ_PROMPT_TAG: {
            ObjPromptTag* tag = AS_PROMPT_TAG(value);
            if (tag->name != NULL) {
                printf("<prompt-tag '%s' #%" PRIu32 ">", tag->name->chars, tag->id);
            } else {
                printf("<prompt-tag #%" PRIu32 ">", tag->id);
            }
            break;
        }
        case OBJ_CONTINUATION: {
            ObjContinuation* cont = AS_CONTINUATION(value);
            const char* state_str = cont->state == CONT_VALID ? "valid" :
                                    cont->state == CONT_CONSUMED ? "consumed" : "invalid";
            printf("<continuation %s, %d frames>", state_str, cont->frame_count);
            break;
        }
        default:
            printf("<unknown object>");
            break;
    }
}