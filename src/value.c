#include <stdio.h>
#include <stdlib.h>

#include "./value.h"

#include <math.h>

#include "./object.h"
#include "./memory.h"
#include "./vm.h"
#include "./table.h"
#include "gc.h"

void initValueArray(ValueArray* array) {
    array->values = NULL;
    array->capacity = 0;
    array->count = 0;
}

void writeValueArray(VM* vm, ValueArray* array, Value value) {
    // Protect value from GC during GROW_ARRAY (which can trigger GC)
    if (IS_OBJ(value)) pushTempRoot(vm, AS_OBJ(value));

    if (array->capacity < array->count + 1) {
        int oldCapacity = array->capacity;
        array->capacity = GROW_CAPACITY(oldCapacity);
        array->values = GROW_ARRAY(vm, Value, array->values, oldCapacity, array->capacity);
        if (array->values == NULL) {
            if (IS_OBJ(value)) popTempRoot(vm);
            printf("Out of memory!\n");
            exit(1);
        }
    }

    array->values[array->count] = value;
    array->count++;

    if (IS_OBJ(value)) popTempRoot(vm);
}

void freeValueArray(VM* vm, ValueArray* array) {
    FREE_ARRAY(vm, Value, array->values, array->capacity);
    initValueArray(array);
}

static void printDouble(double num) {
    if (floor(num) == num && fabs(num) < 1e15) {
        printf("%.0f", num);
    } else {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%.17f", num);
        char* decimal_point = strchr(buffer, '.');
        if (decimal_point != NULL) {
            char* end = buffer + strlen(buffer) - 1;
            while (end > decimal_point && *end == '0') {
                *end = '\0';
                end--;
            }
            if (end == decimal_point) {
                *end = '\0';

            }
        }
        printf("%s", buffer);

    }
}

// Helper for printValue with cycle detection
static void printValueHelper(VM* vm, Value value, Obj** visited, int depth) {
    if (IS_BOOL(value)) {
        printf(AS_BOOL(value) ? "true" : "false");
    } else if (IS_NULL(value)) {
        printf("null");
    } else if (IS_ENUM(value)) {
        int type_id = ENUM_TYPE_ID(value);
        int variant_idx = ENUM_VARIANT(value);

        if (vm != NULL) {
            ObjEnumSchema* schema = NULL;
            for (int i = 0; i < vm->globals.capacity; i++) {
                Entry* entry = &vm->globals.entries[i];
                if (entry->key != NULL && IS_OBJ(entry->value) && IS_ENUM_SCHEMA(entry->value)) {
                    ObjEnumSchema* candidate = AS_ENUM_SCHEMA(entry->value);
                    if (candidate->type_id == type_id) {
                        schema = candidate;
                        break;
                    }
                }
            }

            if (schema != NULL && variant_idx >= 0 && variant_idx < schema->variant_count) {
                ObjString* variant_name = schema->variant_names[variant_idx];
                printf("%.*s.%.*s",
                       schema->name->length, schema->name->chars,
                       variant_name->length, variant_name->chars);
            } else {
                printf("<enum#%d.%d>", type_id, variant_idx);
            }
        } else {
            printf("<enum#%d.%d>", type_id, variant_idx);
        }
    } else if (IS_DOUBLE(value)) {
        printDouble(AS_DOUBLE(value));
    } else if (IS_OBJ(value)) {
        Obj* obj = AS_OBJ(value);

        // Check depth limit to prevent stack overflow
        if (depth >= 100) {
            printf("...");
            return;
        }

        for (int i = 0; i < depth; i++) {
            if (visited[i] == obj) {
                printf("...");
                return;
            }
        }

        visited[depth] = obj;

        switch (obj->type) {
            case OBJ_LIST: {
                    ObjList* list = AS_LIST(value);
                    printf("[");
                    for (int i = 0; i < list->items.count; i++) {
                        printValueHelper(vm, list->items.values[i], visited, depth + 1);
                        if (i < list->items.count - 1) {
                            printf(", ");
                        }
                    }
                    printf("]");
                    break;
            }
            case OBJ_CLOSURE: {
                    ObjFunction* fn = AS_CLOSURE(value)->function;
                    if (fn->name) {
                        printf("<closure %.*s/%d>", fn->name->length, fn->name->chars, fn->arity);
                    } else {
                        printf("<closure /%d>", fn->arity);
                    }
                    break;
            }
            case OBJ_FUNCTION: {
                    ObjFunction* fn = AS_FUNCTION(value);
                    if (fn->name) {
                        printf("<fn %.*s/%d>", fn->name->length, fn->name->chars, fn->arity);
                    } else {
                        printf("<fn /%d>", fn->arity);
                    }
                    break;
            }
            case OBJ_NATIVE_FUNCTION: {
                    ObjNativeFunction* native = AS_NATIVE_FUNCTION(value);
                    if (native->name) {
                        printf("<native fn %.*s/%d>", native->name->length, native->name->chars, native->arity);
                    } else {
                        printf("<native fn /%d>", native->arity);
                    }
                    break;
            }
            case OBJ_NATIVE_CONTEXT:
                    printf("<native context>");
                    break;
            case OBJ_NATIVE_CLOSURE: {
                    ObjNativeClosure* closure = AS_NATIVE_CLOSURE(value);
                    if (closure->name) {
                        printf("<native closure %.*s/%d>", closure->name->length, closure->name->chars, closure->arity);
                    } else {
                        printf("<native closure /%d>", closure->arity);
                    }
                    break;
            }
            case OBJ_NATIVE_REFERENCE: {
                    Value deref_value;
                    if (dereferenceValue(vm, value, &deref_value)) {
                        printValueHelper(vm, deref_value, visited, depth + 1);
                    } else {
                        printf("<dead native ref>");
                    }
                    break;
            }
            case OBJ_STRING:
                printf("%.*s", ((ObjString*)obj)->length, ((ObjString*)obj)->chars);
                break;
            case OBJ_UPVALUE:
                printf("<upvalue>");
                break;
            case OBJ_INT64:
                printf("%lld", ((ObjInt64*)obj)->value);
                break;
            case OBJ_MAP: {
                    ObjMap* map = AS_MAP(value);
                    printf("{");
                    int printed = 0;
                    for (int i = 0; i < map->table->capacity; i++) {
                        Entry* entry = &map->table->entries[i];
                        if (entry->key != NULL) {
                            if (printed > 0) {
                                printf(", ");
                            }
                            printf("\"%.*s\": ", entry->key->length, entry->key->chars);
                            printValueHelper(vm, entry->value, visited, depth + 1);
                            printed++;
                        }
                    }
                    printf("}");
                    break;
            }
            case OBJ_REFERENCE: {
                    ObjReference* ref = (ObjReference*)obj;

                    if (vm == NULL) {
                        switch (ref->ref_type) {
                            case REF_LOCAL:
                                printf("<ref to local>");
                                break;
                            case REF_GLOBAL:
                                printf("<ref to global '%.*s'>", ref->as.global.global_name->length, ref->as.global.global_name->chars);
                                break;
                            case REF_INDEX:
                                printf("<ref to array element>");
                                break;
                            case REF_PROPERTY:
                                printf("<ref to map property>");
                                break;
                        }
                        break;
                    }

                    Value referenced_value;
                    if (dereferenceValue(vm, value, &referenced_value)) {
                        printValueHelper(vm, referenced_value, visited, depth + 1);
                    } else {
                        printf("<undefined ref>");
                    }
                    break;
            }
            case OBJ_STRUCT_SCHEMA: {
                    ObjStructSchema* schema = AS_STRUCT_SCHEMA(value);
                    if (schema->name != NULL) {
                        printf("<struct %.*s>", schema->name->length, schema->name->chars);
                    } else {
                        printf("<struct schema@%p>", (void*)schema);
                    }
                    break;
            }
            case OBJ_STRUCT_INSTANCE: {
                    ObjStructInstance* instance = AS_STRUCT_INSTANCE(value);
                    // Use cached field_count for safety during GC
                    if (instance->schema != NULL && instance->schema->name != NULL) {
                        printf("<struct %.*s>", instance->schema->name->length, instance->schema->name->chars);
                    } else {
                        printf("<struct instance@%p>", (void*)instance);
                    }
                    break;
            }
            case OBJ_ENUM_SCHEMA: {
                    ObjEnumSchema* schema = AS_ENUM_SCHEMA(value);
                    if (schema->name != NULL) {
                        printf("<enum %.*s { ", schema->name->length, schema->name->chars);
                        for (int i = 0; i < schema->variant_count; i++) {
                            if (i > 0) printf(", ");
                            if (schema->variant_names && schema->variant_names[i] != NULL) {
                                printf("%.*s", schema->variant_names[i]->length, schema->variant_names[i]->chars);
                            }
                        }
                        printf(" }>");
                    } else {
                        printf("<enum schema@%p>", (void*)schema);
                    }
                    break;
            }
        }
    }
}

void printValue(VM* vm, Value value) {
    Obj* visited[100];
    printValueHelper(vm, value, visited, 0);
}

Value cloneValue(VM* vm, Value value) {
    if (IS_DOUBLE(value) || IS_BOOL(value) || IS_NULL(value) || IS_ENUM(value)) {
        return value;
    }

    if (IS_OBJ(value)) {
        Obj* obj = AS_OBJ(value);
        switch (obj->type) {
            case OBJ_STRING: {
                ObjString* str = (ObjString*)obj;
                return OBJ_VAL(copyString(vm, str->chars, str->length));
            }
            case OBJ_LIST: {
                ObjList* original = (ObjList*)obj;
                ObjList* cloned = newList(vm);
                pushTempRoot(vm, (Obj*)cloned);
                for (int i = 0; i < original->items.count; i++) {
                    Value cloned_element = cloneValue(vm, original->items.values[i]);
                    writeValueArray(vm, &cloned->items, cloned_element);
                }
                popTempRoot(vm);
                return OBJ_VAL(cloned);
            }
            case OBJ_MAP: {
                ObjMap* original = (ObjMap*)obj;
                ObjMap* cloned = newMap(vm);
                pushTempRoot(vm, (Obj*)cloned);
                for (int i = 0; i < original->table->capacity; i++) {
                    Entry* entry = &original->table->entries[i];
                    if (entry->key != NULL) {
                        Value cloned_value = cloneValue(vm, entry->value);
                        tableSet(vm, cloned->table, entry->key, cloned_value);
                    }
                }
                popTempRoot(vm);
                return OBJ_VAL(cloned);
            }
            case OBJ_CLOSURE:
            case OBJ_FUNCTION:
            case OBJ_NATIVE_FUNCTION:
                return value;
            case OBJ_NATIVE_CONTEXT:
            case OBJ_NATIVE_CLOSURE:
                return value;
            case OBJ_NATIVE_REFERENCE: {
                ObjNativeReference* native_ref = (ObjNativeReference*)obj;
                return OBJ_VAL(newNativeReference(vm, native_ref->context, native_ref->value_offset,
                                                  native_ref->get_hook, native_ref->set_hook));
            }
            case OBJ_UPVALUE:
            case OBJ_INT64:
            case OBJ_DISPATCHER:
                return value;
            case OBJ_REFERENCE: {
                ObjReference* ref = (ObjReference*)obj;

                switch (ref->ref_type) {
                    case REF_LOCAL:
                        return OBJ_VAL(newReference(vm, ref->as.local.location));
                    case REF_GLOBAL:
                        return OBJ_VAL(newGlobalReference(vm, ref->as.global.global_name));
                    case REF_INDEX:
                        return OBJ_VAL(newIndexReference(vm, ref->as.index.container, ref->as.index.index));
                    case REF_PROPERTY:
                        return OBJ_VAL(newPropertyReference(vm, ref->as.property.container, ref->as.property.key));
                    case REF_UPVALUE:
                        return OBJ_VAL(newUpvalueReference(vm, ref->as.upvalue.upvalue));
                    default:
                        return value;
                }
            }
            case OBJ_STRUCT_SCHEMA:
                return value;
            case OBJ_STRUCT_INSTANCE: {
                ObjStructInstance* original = (ObjStructInstance*)obj;
                ObjStructInstance* cloned = newStructInstance(vm, original->schema);
                pushTempRoot(vm, (Obj*)cloned);
                for (int i = 0; i < original->schema->field_count; i++) {
                    cloned->fields[i] = cloneValue(vm, original->fields[i]);
                }
                popTempRoot(vm);
                return OBJ_VAL(cloned);
            }
            default:
                return value;
        }
    }

    return value;
}
typedef struct {
    Obj** keys;
    Value* values;
    int count;
    int capacity;
} CloneMap;

static void initCloneMap(CloneMap* map) {
    map->count = 0;
    map->capacity = 0;
    map->keys = NULL;
    map->values = NULL;
}

static void freeCloneMap(VM* vm, CloneMap* map) {
    FREE_ARRAY(vm, Obj*, map->keys, map->capacity);
    FREE_ARRAY(vm, Value, map->values, map->capacity);
    initCloneMap(map);
}

static Value* cloneMapGet(CloneMap* map, Obj* key) {
    for (int i = 0; i < map->count; i++) {
        if (map->keys[i] == key) {
            return &map->values[i];
        }
    }
    return NULL;
}

static void cloneMapPut(VM* vm, CloneMap* map, Obj* key, Value value) {
    pushTempRoot(vm, key);
    if (IS_OBJ(value)) pushTempRoot(vm, AS_OBJ(value));

    for (int i = 0; i < map->count; i++) {
        if (map->keys[i] == key) {
            map->values[i] = value;
            if (IS_OBJ(value)) popTempRoot(vm);
            popTempRoot(vm);
            return;
        }
    }

    if (map->count >= map->capacity) {
        int old_cap = map->capacity;
        map->capacity = GROW_CAPACITY(old_cap);
        map->keys = GROW_ARRAY(vm, Obj*, map->keys, old_cap, map->capacity);
        map->values = GROW_ARRAY(vm, Value, map->values, old_cap, map->capacity);
    }
    map->keys[map->count] = key;
    map->values[map->count] = value;
    map->count++;

    if (IS_OBJ(value)) popTempRoot(vm);
    popTempRoot(vm);
}

static Value deepCloneHelper(VM* vm, Value value, CloneMap* visited, int depth);

Value deepCloneValue(VM* vm, Value value) {
    CloneMap visited;
    initCloneMap(&visited);
    Value result = deepCloneHelper(vm, value, &visited, 0);
    freeCloneMap(vm, &visited);
    return result;
}

static Value deepCloneHelper(VM* vm, Value value, CloneMap* visited, int depth) {
    if (depth >= 100) {
        return value;
    }

    if (IS_DOUBLE(value) || IS_BOOL(value) || IS_NULL(value) || IS_ENUM(value)) {
        return value;
    }

    if (!IS_OBJ(value)) {
        return value;
    }

    Obj* obj = AS_OBJ(value);

    Value* existing = cloneMapGet(visited, obj);
    if (existing) {
        return *existing;
    }

    if (IS_REFERENCE(value)) {
        ObjReference* ref = AS_REFERENCE(value);

        if (ref->ref_type == REF_INDEX) {
            Value container = ref->as.index.container;
            if (IS_OBJ(container)) {
                Value* cloned_container = cloneMapGet(visited, AS_OBJ(container));
                if (cloned_container) {
                    Value cloned_index = deepCloneHelper(vm, ref->as.index.index, visited, depth + 1);
                    Value new_ref = OBJ_VAL(newIndexReference(vm, *cloned_container, cloned_index));
                    return new_ref;
                }
            }
        } else if (ref->ref_type == REF_PROPERTY) {
            Value container = ref->as.property.container;
            if (IS_OBJ(container)) {
                Value* cloned_container = cloneMapGet(visited, AS_OBJ(container));
                if (cloned_container) {
                    Value cloned_key = deepCloneHelper(vm, ref->as.property.key, visited, depth + 1);
                    Value new_ref = OBJ_VAL(newPropertyReference(vm, *cloned_container, cloned_key));
                    return new_ref;
                }
            }
        }

        Value deref_value;
        if (dereferenceValue(vm, value, &deref_value)) {
            return deepCloneHelper(vm, deref_value, visited, depth + 1);
        }

        return NULL_VAL;
    }

    if (IS_NATIVE_REFERENCE(value)) {
        Value deref_value;
        if (dereferenceValue(vm, value, &deref_value)) {
            return deepCloneHelper(vm, deref_value, visited, depth + 1);
        }

        return NULL_VAL;
    }

    switch (obj->type) {
        case OBJ_STRING: {
            ObjString* str = (ObjString*)obj;
            return OBJ_VAL(copyString(vm, str->chars, str->length));
        }

        case OBJ_LIST: {
            ObjList* original = (ObjList*)obj;
            ObjList* cloned = newList(vm);

            pushTempRoot(vm, (Obj*)cloned);
            cloneMapPut(vm, visited, obj, OBJ_VAL(cloned));

            for (int i = 0; i < original->items.count; i++) {
                Value cloned_elem = deepCloneHelper(vm, original->items.values[i], visited, depth + 1);
                writeValueArray(vm, &cloned->items, cloned_elem);
            }

            popTempRoot(vm);
            return OBJ_VAL(cloned);
        }

        case OBJ_MAP: {
            ObjMap* original = (ObjMap*)obj;
            ObjMap* cloned = newMap(vm);

            pushTempRoot(vm, (Obj*)cloned);
            cloneMapPut(vm, visited, obj, OBJ_VAL(cloned));

            for (int i = 0; i < original->table->capacity; i++) {
                Entry* entry = &original->table->entries[i];
                if (entry->key != NULL) {
                    Value cloned_value = deepCloneHelper(vm, entry->value, visited, depth + 1);
                    tableSet(vm, cloned->table, entry->key, cloned_value);
                }
            }

            popTempRoot(vm);
            return OBJ_VAL(cloned);
        }

        case OBJ_STRUCT_INSTANCE: {
            ObjStructInstance* original = (ObjStructInstance*)obj;
            ObjStructInstance* cloned = newStructInstance(vm, original->schema);

            pushTempRoot(vm, (Obj*)cloned);
            cloneMapPut(vm, visited, obj, OBJ_VAL(cloned));

            for (int i = 0; i < original->schema->field_count; i++) {
                cloned->fields[i] = deepCloneHelper(vm, original->fields[i], visited, depth + 1);
            }

            popTempRoot(vm);
            return OBJ_VAL(cloned);
        }

        case OBJ_CLOSURE:
        case OBJ_FUNCTION:
        case OBJ_NATIVE_FUNCTION:
        case OBJ_NATIVE_CONTEXT:
        case OBJ_NATIVE_CLOSURE:
        case OBJ_UPVALUE:
        case OBJ_INT64:
        case OBJ_DISPATCHER:
        case OBJ_STRUCT_SCHEMA:
        case OBJ_ENUM_SCHEMA:
            return value;

        default:
            return value;
    }
}

static bool dereferenceValueHelper(VM* vm, Value refValue, Value* out, ObjReference** visited_refs, int depth) {
    if (!IS_OBJ(refValue) || !IS_REFERENCE(refValue)) {
        return false;
    }

    if (depth >= 64) {
        return false;
    }

    ObjReference* ref = AS_REFERENCE(refValue);

    for (int i = 0; i < depth; i++) {
        if (visited_refs[i] == ref) {
            return false;
        }
    }

    visited_refs[depth] = ref;

    switch (ref->ref_type) {
        case REF_LOCAL:
            *out = *ref->as.local.location;
            if (IS_REFERENCE(*out)) {
                return dereferenceValueHelper(vm, *out, out, visited_refs, depth + 1);
            }
            if (IS_OBJ(*out) && IS_NATIVE_REFERENCE(*out)) {
                return dereferenceValue(vm, *out, out);
            }
            return true;

        case REF_GLOBAL: {
            Value value;
            if (!globalGet(vm, ref->as.global.global_name, &value)) {
                return false;
            }
            *out = value;
            if (IS_REFERENCE(*out)) {
                return dereferenceValueHelper(vm, *out, out, visited_refs, depth + 1);
            }
            if (IS_OBJ(*out) && IS_NATIVE_REFERENCE(*out)) {
                return dereferenceValue(vm, *out, out);
            }
            return true;
        }

        case REF_INDEX: {
            Value container = ref->as.index.container;
            Value index = ref->as.index.index;

            if (!IS_OBJ(container)) {
                return false;
            }

            if (IS_LIST(container)) {
                if (!IS_DOUBLE(index)) {
                    return false;
                }

                ObjList* list = AS_LIST(container);
                int idx = (int)AS_DOUBLE(index);

                if (idx < 0 || idx >= list->items.count) {
                    return false;
                }

                *out = list->items.values[idx];
                if (IS_REFERENCE(*out)) {
                    return dereferenceValueHelper(vm, *out, out, visited_refs, depth + 1);
                }
                if (IS_OBJ(*out) && IS_NATIVE_REFERENCE(*out)) {
                    return dereferenceValue(vm, *out, out);
                }
                return true;
            } else if (IS_MAP(container)) {
                ObjMap* map = AS_MAP(container);

                ObjString* key_str = NULL;
                if (IS_STRING(index)) {
                    key_str = AS_STRING(index);
                } else if (IS_DOUBLE(index)) {
                    char buffer[64];
                    snprintf(buffer, sizeof(buffer), "%g", AS_DOUBLE(index));
                    key_str = copyString(vm, buffer, strlen(buffer));
                } else {
                    return false;
                }

                Value value;
                if (!tableGet(map->table, key_str, &value)) {
                    return false;
                }
                *out = value;
                if (IS_REFERENCE(*out)) {
                    return dereferenceValueHelper(vm, *out, out, visited_refs, depth + 1);
                }
                if (IS_OBJ(*out) && IS_NATIVE_REFERENCE(*out)) {
                    return dereferenceValue(vm, *out, out);
                }
                return true;
            } else {
                return false;
            }
        }

        case REF_PROPERTY: {
            Value container = ref->as.property.container;
            Value key = ref->as.property.key;

            if (!IS_OBJ(container) || (!IS_MAP(container) && !IS_STRUCT_INSTANCE(container))) {
                return false;
            }

            ObjString* key_str;
            if (IS_OBJ(key) && IS_STRING(key)) {
                key_str = AS_STRING(key);
            } else if (IS_DOUBLE(key)) {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%.0f", AS_DOUBLE(key));
                key_str = copyString(vm, buffer, (int)strlen(buffer));
            } else {
                return false;
            }

            Value value;
            if (IS_MAP(container)) {
                ObjMap* map = AS_MAP(container);
                if (!tableGet(map->table, key_str, &value)) {
                    return false;
                }
            } else if (IS_STRUCT_INSTANCE(container)) {
                ObjStructInstance* instance = AS_STRUCT_INSTANCE(container);
                Value index_val;
                if (!tableGet(instance->schema->field_to_index, key_str, &index_val)) {
                    return false;
                }
                int field_index = (int)AS_DOUBLE(index_val);
                value = instance->fields[field_index];
            } else {
                return false;
            }

            *out = value;
            if (IS_REFERENCE(*out)) {
                return dereferenceValueHelper(vm, *out, out, visited_refs, depth + 1);
            }
            if (IS_OBJ(*out) && IS_NATIVE_REFERENCE(*out)) {
                return dereferenceValue(vm, *out, out);
            }
            return true;
        }

        case REF_UPVALUE:
            if (ref->as.upvalue.upvalue == NULL || ref->as.upvalue.upvalue->location == NULL) {
                return false;
            }
            *out = *ref->as.upvalue.upvalue->location;
            if (IS_REFERENCE(*out)) {
                return dereferenceValueHelper(vm, *out, out, visited_refs, depth + 1);
            }
            if (IS_OBJ(*out) && IS_NATIVE_REFERENCE(*out)) {
                return dereferenceValue(vm, *out, out);
            }
            return true;

        default:
            return false;
    }
}

bool dereferenceValue(VM* vm, Value refValue, Value* out) {
    if (IS_OBJ(refValue) && IS_NATIVE_REFERENCE(refValue)) {
        ObjNativeReference* native_ref = AS_NATIVE_REFERENCE(refValue);

        if (!IS_NATIVE_CONTEXT(native_ref->context)) {
            return false;
        }
        ObjNativeContext* ctx = AS_NATIVE_CONTEXT(native_ref->context);
        void* native_data = ctx->native_data;

        Value* value_ptr = (Value*)((char*)native_data + native_ref->value_offset);
        Value current_value = *value_ptr;

        if (native_ref->get_hook) {
            *out = native_ref->get_hook(vm, native_ref->context, current_value);
        } else {
            *out = current_value;
        }

        if (IS_REFERENCE(*out)) {
            ObjReference* visited_refs[64];
            return dereferenceValueHelper(vm, *out, out, visited_refs, 0);
        }

        return true;
    }

    ObjReference* visited_refs[64];
    return dereferenceValueHelper(vm, refValue, out, visited_refs, 0);
}

bool writeReferenceValue(VM* vm, Value refValue, Value new_value) {
    if (IS_OBJ(refValue) && IS_NATIVE_REFERENCE(refValue)) {
        ObjNativeReference* native_ref = AS_NATIVE_REFERENCE(refValue);

        if (!IS_NATIVE_CONTEXT(native_ref->context)) {
            return false;
        }
        ObjNativeContext* ctx = AS_NATIVE_CONTEXT(native_ref->context);
        void* native_data = ctx->native_data;

        if (native_ref->set_hook) {
            native_ref->set_hook(vm, native_ref->context, new_value);
        } else {
            Value* value_ptr = (Value*)((char*)native_data + native_ref->value_offset);
            *value_ptr = new_value;
        }

        return true;
    }

    if (!IS_REFERENCE(refValue)) {
        return false;
    }

    ObjReference* ref = AS_REFERENCE(refValue);

    switch (ref->ref_type) {
        case REF_LOCAL:
            *ref->as.local.location = new_value;
            return true;

        case REF_GLOBAL:
            return globalSet(vm, ref->as.global.global_name, new_value);

        case REF_UPVALUE:
            if (ref->as.upvalue.upvalue == NULL || ref->as.upvalue.upvalue->location == NULL) {
                return false;
            }
            if (IS_REFERENCE(*ref->as.upvalue.upvalue->location)) {
                return writeReferenceValue(vm, *ref->as.upvalue.upvalue->location, new_value);
            }
            *ref->as.upvalue.upvalue->location = new_value;
            return true;

        case REF_INDEX: {
            if (!IS_LIST(ref->as.index.container)) {
                return false;
            }
            ObjList* list = AS_LIST(ref->as.index.container);
            int idx = (int)AS_DOUBLE(ref->as.index.index);
            if (idx < 0 || idx >= list->items.count) {
                return false;
            }
            list->items.values[idx] = new_value;
            return true;
        }

        case REF_PROPERTY: {
            if (!IS_MAP(ref->as.property.container)) {
                return false;
            }
            ObjMap* map = AS_MAP(ref->as.property.container);
            ObjString* key_str;
            if (IS_STRING(ref->as.property.key)) {
                key_str = AS_STRING(ref->as.property.key);
            } else if (IS_DOUBLE(ref->as.property.key)) {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%.0f", AS_DOUBLE(ref->as.property.key));
                key_str = copyString(vm, buffer, (int)strlen(buffer));
            } else {
                return false;
            }
            tableSet(vm, map->table, key_str, new_value);
            return true;
        }

        default:
            return false;
    }
}