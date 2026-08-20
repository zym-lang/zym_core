#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

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
    }

    array->values[array->count] = value;
    array->count++;

    if (IS_OBJ(value)) popTempRoot(vm);
}

void freeValueArray(VM* vm, ValueArray* array) {
    FREE_ARRAY(vm, Value, array->values, array->capacity);
    initValueArray(array);
}

static void printDouble(FILE* f, double num) {
    if (floor(num) == num && fabs(num) < 1e15) {
        fprintf(f, "%.0f", num);
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
        fprintf(f, "%s", buffer);

    }
}

// Helper for printValue with cycle detection
static void printValueHelper(VM* vm, FILE* f, Value value, Obj** visited, int depth) {
    if (IS_BOOL(value)) {
        fprintf(f, AS_BOOL(value) ? "true" : "false");
    } else if (IS_NULL(value)) {
        fprintf(f, "null");
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
                fprintf(f, "%.*s.%.*s",
                       schema->name->length, schema->name->chars,
                       variant_name->length, variant_name->chars);
            } else {
                fprintf(f, "<enum#%d.%d>", type_id, variant_idx);
            }
        } else {
            fprintf(f, "<enum#%d.%d>", type_id, variant_idx);
        }
    } else if (IS_DOUBLE(value)) {
        printDouble(f, AS_DOUBLE(value));
    } else if (IS_OBJ(value)) {
        Obj* obj = AS_OBJ(value);

        // Check depth limit to prevent stack overflow
        if (depth >= 100) {
            fprintf(f, "...");
            return;
        }

        for (int i = 0; i < depth; i++) {
            if (visited[i] == obj) {
                fprintf(f, "...");
                return;
            }
        }

        visited[depth] = obj;

        switch (obj->type) {
            case OBJ_LIST: {
                    ObjList* list = AS_LIST(value);
                    fprintf(f, "[");
                    for (int i = 0; i < list->items.count; i++) {
                        printValueHelper(vm, f, list->items.values[i], visited, depth + 1);
                        if (i < list->items.count - 1) {
                            fprintf(f, ", ");
                        }
                    }
                    fprintf(f, "]");
                    break;
            }
            case OBJ_CLOSURE: {
                    ObjFunction* fn = AS_CLOSURE(value)->function;
                    if (fn->name) {
                        fprintf(f, "<closure %.*s/%d>", fn->name->length, fn->name->chars, fn->arity);
                    } else {
                        fprintf(f, "<closure /%d>", fn->arity);
                    }
                    break;
            }
            case OBJ_FUNCTION: {
                    ObjFunction* fn = AS_FUNCTION(value);
                    if (fn->name) {
                        fprintf(f, "<fn %.*s/%d>", fn->name->length, fn->name->chars, fn->arity);
                    } else {
                        fprintf(f, "<fn /%d>", fn->arity);
                    }
                    break;
            }
            case OBJ_NATIVE_FUNCTION: {
                    ObjNativeFunction* native = AS_NATIVE_FUNCTION(value);
                    if (native->name) {
                        fprintf(f, "<native fn %.*s/%d>", native->name->length, native->name->chars, native->arity);
                    } else {
                        fprintf(f, "<native fn /%d>", native->arity);
                    }
                    break;
            }
            case OBJ_NATIVE_CONTEXT:
                    fprintf(f, "<native context>");
                    break;
            case OBJ_NATIVE_CLOSURE: {
                    ObjNativeClosure* closure = AS_NATIVE_CLOSURE(value);
                    if (closure->name) {
                        fprintf(f, "<native closure %.*s/%d>", closure->name->length, closure->name->chars, closure->arity);
                    } else {
                        fprintf(f, "<native closure /%d>", closure->arity);
                    }
                    break;
            }
            case OBJ_STRING:
                fprintf(f, "%.*s", ((ObjString*)obj)->length, ((ObjString*)obj)->chars);
                break;
            case OBJ_UPVALUE:
                fprintf(f, "<upvalue>");
                break;
            case OBJ_INT64:
                fprintf(f, "%lld", ((ObjInt64*)obj)->value);
                break;
            case OBJ_DISPATCHER:
                fprintf(f, "<overloaded function>");
                break;
            case OBJ_MAP: {
                    ObjMap* map = AS_MAP(value);
                    fprintf(f, "{");
                    int printed = 0;
                    for (int i = 0; i < map->table.capacity; i++) {
                        Entry* entry = &map->table.entries[i];
                        if (entry->key != NULL) {
                            if (printed > 0) {
                                fprintf(f, ", ");
                            }
                            fprintf(f, "\"%.*s\": ", entry->key->length, entry->key->chars);
                            printValueHelper(vm, f, entry->value, visited, depth + 1);
                            printed++;
                        }
                    }
                    fprintf(f, "}");
                    break;
            }
            case OBJ_STRUCT_SCHEMA: {
                    ObjStructSchema* schema = AS_STRUCT_SCHEMA(value);
                    if (schema->name != NULL) {
                        fprintf(f, "<struct %.*s>", schema->name->length, schema->name->chars);
                    } else {
                        fprintf(f, "<struct schema@%p>", (void*)schema);
                    }
                    break;
            }
            case OBJ_STRUCT_INSTANCE: {
                    ObjStructInstance* instance = AS_STRUCT_INSTANCE(value);
                    // Use cached field_count for safety during GC
                    if (instance->schema != NULL && instance->schema->name != NULL) {
                        fprintf(f, "<struct %.*s>", instance->schema->name->length, instance->schema->name->chars);
                    } else {
                        fprintf(f, "<struct instance@%p>", (void*)instance);
                    }
                    break;
            }
            case OBJ_ENUM_SCHEMA: {
                    ObjEnumSchema* schema = AS_ENUM_SCHEMA(value);
                    if (schema->name != NULL) {
                        fprintf(f, "<enum %.*s { ", schema->name->length, schema->name->chars);
                        for (int i = 0; i < schema->variant_count; i++) {
                            if (i > 0) fprintf(f, ", ");
                            if (schema->variant_names && schema->variant_names[i] != NULL) {
                                fprintf(f, "%.*s", schema->variant_names[i]->length, schema->variant_names[i]->chars);
                            }
                        }
                        fprintf(f, " }>");
                    } else {
                        fprintf(f, "<enum schema@%p>", (void*)schema);
                    }
                    break;
            }
        }
    }
}

void fprintValue(VM* vm, FILE* file, Value value) {
    Obj* visited[100];
    printValueHelper(vm, file, value, visited, 0);
}

void printValue(VM* vm, Value value) {
    fprintValue(vm, stdout, value);
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
                for (int i = 0; i < original->table.capacity; i++) {
                    Entry* entry = &original->table.entries[i];
                    if (entry->key != NULL) {
                        Value cloned_value = cloneValue(vm, entry->value);
                        tableSet(vm, &cloned->table, entry->key, cloned_value);
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
            case OBJ_UPVALUE:
            case OBJ_INT64:
            case OBJ_DISPATCHER:
                return value;
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

            for (int i = 0; i < original->table.capacity; i++) {
                Entry* entry = &original->table.entries[i];
                if (entry->key != NULL) {
                    Value cloned_value = deepCloneHelper(vm, entry->value, visited, depth + 1);
                    tableSet(vm, &cloned->table, entry->key, cloned_value);
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