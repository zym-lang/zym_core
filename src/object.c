#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

static ObjString* allocateString(VM* vm, char* chars, int byte_length, uint32_t hash) {
    ObjString* string = ALLOCATE_OBJ(vm, ObjString, OBJ_STRING);
    string->byte_length = byte_length;
    string->chars = chars;
    string->hash = hash;
    string->length = utf8_strlen(chars, byte_length);

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

    return allocateString(vm, chars, length, hash);
}

ObjString* copyString(VM* vm, const char* chars, int length) {
    uint32_t hash = hashString(chars, length);
    ObjString* interned = tableFindString(&vm->strings, chars, length, hash);
    if (interned != NULL) {
        return interned;
    }

    char* heapChars = (char*)reallocate(vm, NULL, 0, length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';

    return allocateString(vm, heapChars, length, hash);
}

ObjFunction* newFunction(VM* vm) {
    ObjFunction* function = (ObjFunction*)allocateObject(vm, sizeof(ObjFunction), OBJ_FUNCTION);

    function->arity = 0;
    function->upvalue_count = 0;
    function->max_regs = 1;
    function->name = NULL;
    function->module_name = NULL;
    function->param_qualifiers = NULL;
    function->qualifier_sig = QUAL_SIG_ALL_NORMAL;  // Default: check for refs to deref
    function->chunk = NULL;

    pushTempRoot(vm, (Obj*)function);

    function->chunk = ALLOCATE(vm, Chunk, 1);
    initChunk(function->chunk);

    popTempRoot(vm);
    return function;
}

ObjNativeFunction* newNativeFunction(VM* vm, ObjString* name, int arity, void* func_ptr, NativeDispatcher dispatcher) {
    ObjNativeFunction* native = (ObjNativeFunction*)allocateObject(vm, sizeof(ObjNativeFunction), OBJ_NATIVE_FUNCTION);

    native->name = name;
    native->arity = arity;
    native->func_ptr = func_ptr;
    native->dispatcher = dispatcher;
    native->param_qualifiers = NULL;
    native->qualifier_sig = QUAL_SIG_ALL_NORMAL;  // Default: check for refs to deref

    if (arity > 0) {
        pushTempRoot(vm, (Obj*)native);
        native->param_qualifiers = ALLOCATE(vm, uint8_t, arity);
        for (int i = 0; i < arity; i++) {
            native->param_qualifiers[i] = 0;
        }
        popTempRoot(vm);
    }

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
    closure->context = context;
    closure->param_qualifiers = NULL;
    closure->qualifier_sig = QUAL_SIG_ALL_NORMAL;  // Default: check for refs to deref

    if (arity > 0) {
        pushTempRoot(vm, (Obj*)closure);
        closure->param_qualifiers = ALLOCATE(vm, uint8_t, arity);
        for (int i = 0; i < arity; i++) {
            closure->param_qualifiers[i] = 0;
        }
        popTempRoot(vm);
    }

    return closure;
}

ObjNativeReference* newNativeReference(VM* vm, Value context, size_t value_offset, NativeRefGetHook get_hook, NativeRefSetHook set_hook) {
    ObjNativeReference* ref = ALLOCATE_OBJ(vm, ObjNativeReference, OBJ_NATIVE_REFERENCE);
    ref->context = context;
    ref->value_offset = value_offset;
    ref->get_hook = get_hook;
    ref->set_hook = set_hook;
    return ref;
}

ObjClosure* newClosure(VM* vm, ObjFunction* function) {
    ObjUpvalue** upvalues = NULL;
    if (function->upvalue_count > 0) {
        upvalues = ALLOCATE(vm, ObjUpvalue*, function->upvalue_count);
        for (int i = 0; i < function->upvalue_count; i++) {
            upvalues[i] = NULL;
        }
    }

    ObjClosure* closure = (ObjClosure*)allocateObject(vm, sizeof(ObjClosure), OBJ_CLOSURE);
    closure->function = function;
    closure->upvalues = upvalues;
    closure->upvalue_count = function->upvalue_count;

    return closure;
}

ObjList* newList(VM* vm) {
    ObjList* list = ALLOCATE_OBJ(vm, ObjList, OBJ_LIST);
    initValueArray(&list->items);
    return list;
}

ObjMap* newMap(VM* vm) {
    ObjMap* map = ALLOCATE_OBJ(vm, ObjMap, OBJ_MAP);
    map->table = NULL;

    pushTempRoot(vm, (Obj*)map);

    map->table = ALLOCATE(vm, Table, 1);
    initTable(map->table);

    popTempRoot(vm);
    return map;
}

ObjDispatcher* newDispatcher(VM* vm) {
    ObjDispatcher* dispatcher = ALLOCATE_OBJ(vm, ObjDispatcher, OBJ_DISPATCHER);
    dispatcher->count = 0;
    for (int i = 0; i < MAX_OVERLOADS; i++) {
        dispatcher->overloads[i] = NULL;
    }
    return dispatcher;
}

ObjReference* newReference(VM* vm, Value* location) {
    ObjReference* ref = ALLOCATE_OBJ(vm, ObjReference, OBJ_REFERENCE);
    ref->ref_type = REF_LOCAL;
    ref->as.local.location = location;
    return ref;
}

ObjReference* newStackSlotReference(VM* vm, int slot_index) {
    ObjReference* ref = ALLOCATE_OBJ(vm, ObjReference, OBJ_REFERENCE);
    ref->ref_type = REF_LOCAL;
    ref->as.local.location = &vm->stack[slot_index];
    return ref;
}

ObjReference* newGlobalReference(VM* vm, ObjString* global_name) {
    ObjReference* ref = ALLOCATE_OBJ(vm, ObjReference, OBJ_REFERENCE);
    ref->ref_type = REF_GLOBAL;
    ref->as.global.global_name = global_name;
    return ref;
}

ObjReference* newIndexReference(VM* vm, Value container, Value index) {
    ObjReference* ref = ALLOCATE_OBJ(vm, ObjReference, OBJ_REFERENCE);
    ref->ref_type = REF_INDEX;
    ref->as.index.container = container;
    ref->as.index.index = index;
    return ref;
}

ObjReference* newPropertyReference(VM* vm, Value container, Value key) {
    ObjReference* ref = ALLOCATE_OBJ(vm, ObjReference, OBJ_REFERENCE);
    ref->ref_type = REF_PROPERTY;
    ref->as.property.container = container;
    ref->as.property.key = key;
    return ref;
}

ObjReference* newUpvalueReference(VM* vm, ObjUpvalue* upvalue) {
    ObjReference* ref = ALLOCATE_OBJ(vm, ObjReference, OBJ_REFERENCE);
    ref->ref_type = REF_UPVALUE;
    ref->as.upvalue.upvalue = upvalue;
    return ref;
}

ObjStructSchema* newStructSchema(VM* vm, ObjString* name, ObjString** field_names, int field_count) {
    ObjStructSchema* schema = ALLOCATE_OBJ(vm, ObjStructSchema, OBJ_STRUCT_SCHEMA);

    schema->name = name;
    schema->field_count = field_count;
    schema->field_names = field_names;
    schema->field_to_index = NULL;

    pushTempRoot(vm, (Obj*)schema);

    schema->field_to_index = ALLOCATE(vm, Table, 1);
    initTable(schema->field_to_index);

    for (int i = 0; i < field_count; i++) {
        tableSet(vm, schema->field_to_index, field_names[i], DOUBLE_VAL((double)i));
    }

    popTempRoot(vm);
    return schema;
}

ObjStructInstance* newStructInstance(VM* vm, ObjStructSchema* schema) {
    ObjStructInstance* instance = ALLOCATE_OBJ(vm, ObjStructInstance, OBJ_STRUCT_INSTANCE);

    instance->schema = schema;
    instance->field_count = schema->field_count;
    instance->fields = NULL;

    pushTempRoot(vm, (Obj*)instance);

    instance->fields = ALLOCATE(vm, Value, schema->field_count);

    for (int i = 0; i < schema->field_count; i++) {
        instance->fields[i] = NULL_VAL;
    }

    popTempRoot(vm);
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
    cont->saved_ip = NULL;
    cont->saved_chunk = NULL;
    cont->prompt_tag = NULL;
    cont->state = CONT_VALID;
    cont->return_slot = 0;
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
                printf("<prompt-tag '%s' #%u>", tag->name->chars, tag->id);
            } else {
                printf("<prompt-tag #%u>", tag->id);
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
        case OBJ_REFERENCE: {
            ObjReference* ref = AS_REFERENCE(value);
            printf("<ref -> ");
            switch (ref->ref_type) {
                case REF_LOCAL:
                    printValue(NULL, *ref->as.local.location);
                    break;
                case REF_GLOBAL:
                    printf("global '%.*s'", ref->as.global.global_name->length, ref->as.global.global_name->chars);
                    break;
                case REF_INDEX:
                    printf("array[");
                    printValue(NULL, ref->as.index.index);
                    printf("]");
                    break;
                case REF_PROPERTY:
                    printf("map.");
                    printValue(NULL, ref->as.property.key);
                    break;
                case REF_UPVALUE:
                    printf("upvalue ");
                    printValue(NULL, *ref->as.upvalue.upvalue->location);
                    break;
            }
            printf(">");
            break;
        }
        default:
            printf("<unknown object>");
            break;
    }
}