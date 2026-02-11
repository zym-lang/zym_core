#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "./vm.h"
#include "./chunk.h"
#include "./linemap.h"
#ifndef ZYM_RUNTIME_ONLY
#include "./preprocessor.h"
#include "./compiler.h"
#endif
#include "./serializer.h"
#include "./value.h"
#include "./object.h"
#include "./native.h"
#include "./table.h"
#include "./gc.h"
#include "./memory.h"

#include "./zym.h"

// =============================================================================
// VM LIFECYCLE
// =============================================================================

ZymVM* zym_newVM()
{
    ZymVM* vm = (ZymVM*)malloc(sizeof(ZymVM));
    if (vm == NULL) return NULL;
    initVM(vm);
    return vm;
}

void zym_freeVM(ZymVM* vm)
{
    if (vm == NULL) return;
    freeVM(vm);
    free(vm);
}

// =============================================================================
// COMPILATION AND EXECUTION
// =============================================================================

ZymChunk* zym_newChunk(ZymVM* vm)
{
    ZymChunk* chunk = (ZymChunk*)malloc(sizeof(ZymChunk));
    if (chunk == NULL) return NULL;
    initChunk(chunk);
    return chunk;
}

void zym_freeChunk(ZymVM* vm, ZymChunk* chunk)
{
    if (chunk == NULL) return;
    freeChunk(vm, chunk);
    free(chunk);
}

ZymLineMap* zym_newLineMap(ZymVM* vm)
{
#ifndef ZYM_RUNTIME_ONLY
    ZymLineMap* map = (ZymLineMap*)malloc(sizeof(ZymLineMap));
    if (map == NULL) return NULL;
    initLineMap(map);
    return map;
#else
    (void)vm;
    return (ZymLineMap*)1;
#endif
}

void zym_freeLineMap(ZymVM* vm, ZymLineMap* map)
{
#ifndef ZYM_RUNTIME_ONLY
    if (map == NULL) return;
    freeLineMap(vm, map);
    free(map);
#else
    (void)vm;
    (void)map;
#endif
}

#ifndef ZYM_RUNTIME_ONLY
ZymStatus zym_preprocess(ZymVM* vm, const char* source, ZymLineMap* map, const char** processedSource)
{
    if (source == NULL || map == NULL || processedSource == NULL) return ZYM_STATUS_COMPILE_ERROR;

    char* processed = preprocess(vm, source, map);
    if (processed == NULL) {
        *processedSource = NULL;
        return ZYM_STATUS_COMPILE_ERROR;
    }

    *processedSource = processed;
    return ZYM_STATUS_OK;
}

ZymStatus zym_compile(ZymVM* vm, const char* source, ZymChunk* chunk, ZymLineMap* map, const char* entry_file, ZymCompilerConfig config)
{
    if (source == NULL || chunk == NULL) return ZYM_STATUS_COMPILE_ERROR;
    bool success = compile(vm, source, chunk, map, entry_file, config);
    return success ? ZYM_STATUS_OK : ZYM_STATUS_COMPILE_ERROR;
}
#endif

ZymStatus zym_runChunk(ZymVM* vm, ZymChunk* chunk)
{
    if (vm == NULL || chunk == NULL) return ZYM_STATUS_RUNTIME_ERROR;

    InterpretResult result = runChunk(vm, chunk);
    switch (result) {
        case INTERPRET_OK: return ZYM_STATUS_OK;
        case INTERPRET_RUNTIME_ERROR: return ZYM_STATUS_RUNTIME_ERROR;
        case INTERPRET_COMPILE_ERROR: return ZYM_STATUS_COMPILE_ERROR;
        default: return ZYM_STATUS_RUNTIME_ERROR;
    }
}

#ifndef ZYM_RUNTIME_ONLY
ZymStatus zym_serializeChunk(ZymVM* vm, ZymCompilerConfig config, ZymChunk* chunk, char** out_buffer, size_t* out_size)
{
    if (vm == NULL || chunk == NULL || out_buffer == NULL || out_size == NULL) return ZYM_STATUS_COMPILE_ERROR;

    OutputBuffer temp_buffer;
    initOutputBuffer(&temp_buffer);
    serializeChunk(vm, chunk, config, &temp_buffer);
/*
    printf(" *** BYTECODE : %zu bytes ***\n", temp_buffer.count);
    printf(" *** BYTECODE START : %zu bytes ***\n", temp_buffer.count);
    for (size_t i = 0; i < temp_buffer.count; i++) {
        printf("%02x ", temp_buffer.buffer[i]);
    }
    printf("\n");
    printf(" *** BYTECODE END ***\n\n");
*/
    char* host_buffer = (char*)malloc(temp_buffer.count);
    if (host_buffer == NULL) {
        freeOutputBuffer(vm, &temp_buffer);
        *out_buffer = NULL;
        *out_size = 0;
        return ZYM_STATUS_COMPILE_ERROR;
    }

    memcpy(host_buffer, temp_buffer.buffer, temp_buffer.count);

    *out_buffer = host_buffer;
    *out_size = temp_buffer.count;

    freeOutputBuffer(vm, &temp_buffer);

    return ZYM_STATUS_OK;
}
#endif

ZymStatus zym_deserializeChunk(ZymVM* vm, ZymChunk* chunk, const char* buffer, size_t size)
{
    if (vm == NULL || chunk == NULL || buffer == NULL) return ZYM_STATUS_COMPILE_ERROR;

    vm->chunk = chunk;

    bool success = deserializeChunk(vm, chunk, (const uint8_t*)buffer, size);
    return success ? ZYM_STATUS_OK : ZYM_STATUS_COMPILE_ERROR;
}

// =============================================================================
// NATIVE FUNCTION REGISTRATION
// =============================================================================

ZymStatus zym_defineNative(ZymVM* vm, const char* signature, void* func_ptr) {
    if (!vm || !signature || !func_ptr) {
        return ZYM_STATUS_COMPILE_ERROR;
    }

    bool success = registerNativeFunction(vm, signature, func_ptr);
    return success ? ZYM_STATUS_OK : ZYM_STATUS_COMPILE_ERROR;
}

// =============================================================================
// NATIVE CLOSURES
// =============================================================================

ZymValue zym_createNativeContext(ZymVM* vm, void* native_data, void (*finalizer)(ZymVM*, void*)) {
    if (!vm) return NULL_VAL;

    ObjNativeContext* context = newNativeContext(vm, native_data, finalizer);
    return OBJ_VAL(context);
}

void* zym_getNativeData(ZymValue context) {
    if (!IS_NATIVE_CONTEXT(context)) {
        return NULL;
    }

    ObjNativeContext* ctx = AS_NATIVE_CONTEXT(context);
    return ctx->native_data;
}

ZymValue zym_createNativeClosure(ZymVM* vm, const char* signature, void* func_ptr, ZymValue context) {
    if (!vm || !signature || !func_ptr) {
        return NULL_VAL;
    }

    if (!IS_NATIVE_CONTEXT(context)) {
        fprintf(stderr, "zym_createNativeClosure: context must be a native context\n");
        return NULL_VAL;
    }

    char func_name[256];
    int arity;
    uint8_t* qualifiers = NULL;

    if (!parseNativeSignature(signature, func_name, &arity, &qualifiers)) {
        return NULL_VAL;
    }

    if (arity > MAX_NATIVE_ARITY) {
        fprintf(stderr, "Native closure '%s' has too many parameters (max %d)\n", func_name, MAX_NATIVE_ARITY);
        if (qualifiers) free(qualifiers);
        return NULL_VAL;
    }

    NativeDispatcher dispatcher = getNativeClosureDispatcher(arity);
    if (!dispatcher) {
        fprintf(stderr, "No closure dispatcher available for arity %d\n", arity);
        if (qualifiers) free(qualifiers);
        return NULL_VAL;
    }

    pushTempRoot(vm, AS_OBJ(context));
    ObjString* name_obj = copyString(vm, func_name, (int)strlen(func_name));
    pushTempRoot(vm, (Obj*)name_obj);
    ObjNativeClosure* closure = newNativeClosure(vm, name_obj, arity, func_ptr, dispatcher, context);
    popTempRoot(vm);
    popTempRoot(vm);

    if (arity > 0 && qualifiers) {
        memcpy(closure->param_qualifiers, qualifiers, arity * sizeof(uint8_t));
        free(qualifiers);
    }

    return OBJ_VAL(closure);
}

ZymValue zym_getClosureContext(ZymValue closure) {
    if (!IS_OBJ(closure)) {
        return NULL_VAL;
    }

    Obj* obj = AS_OBJ(closure);
    if (obj->type != OBJ_NATIVE_CLOSURE) {
        return NULL_VAL;
    }

    ObjNativeClosure* native_closure = (ObjNativeClosure*)obj;
    return native_closure->context;
}

// =============================================================================
// FUNCTION OVERLOADING (DISPATCHER)
// =============================================================================

ZymValue zym_createDispatcher(ZymVM* vm) {
    if (!vm) {
        return NULL_VAL;
    }

    ObjDispatcher* dispatcher = newDispatcher(vm);
    return OBJ_VAL(dispatcher);
}

bool zym_addOverload(ZymVM* vm, ZymValue dispatcher, ZymValue closure) {
    if (!vm || !IS_DISPATCHER(dispatcher)) {
        return false;
    }

    if (!IS_OBJ(closure)) {
        return false;
    }

    Obj* obj = AS_OBJ(closure);
    if (obj->type != OBJ_CLOSURE && obj->type != OBJ_NATIVE_CLOSURE) {
        return false;
    }

    ObjDispatcher* disp = AS_DISPATCHER(dispatcher);

    if (disp->count >= MAX_OVERLOADS) {
        return false;
    }

    disp->overloads[disp->count++] = obj;
    return true;
}

// =============================================================================
// NATIVE REFERENCES
// =============================================================================

ZymValue zym_createNativeReference(ZymVM* vm, ZymValue context, size_t value_offset,
                                   ZymValue (*get_hook)(ZymVM*, ZymValue, ZymValue),
                                   void (*set_hook)(ZymVM*, ZymValue, ZymValue)) {
    if (!IS_OBJ(context) || !IS_NATIVE_CONTEXT(context)) {
        fprintf(stderr, "ERROR: zym_createNativeReference requires a native context\n");
        return NULL_VAL;
    }

    pushTempRoot(vm, AS_OBJ(context));

    ObjNativeReference* ref = newNativeReference(vm, context, value_offset,
                                                 (NativeRefGetHook)get_hook,
                                                 (NativeRefSetHook)set_hook);

    popTempRoot(vm);

    return OBJ_VAL(ref);
}

// =============================================================================
// VALUE TYPE CHECKING
// =============================================================================

bool zym_isNull(ZymValue value) { return IS_NULL(value); }
bool zym_isBool(ZymValue value) { return IS_BOOL(value); }
bool zym_isNumber(ZymValue value) { return IS_DOUBLE(value); }
bool zym_isString(ZymValue value) { return IS_STRING(value); }
bool zym_isList(ZymValue value) { return IS_LIST(value); }
bool zym_isMap(ZymValue value) { return IS_MAP(value); }
bool zym_isStruct(ZymValue value) { return IS_STRUCT_INSTANCE(value); }
bool zym_isEnum(ZymValue value) { return IS_ENUM(value); }
bool zym_isFunction(ZymValue value) {
    return IS_FUNCTION(value) || IS_CLOSURE(value) || IS_NATIVE_FUNCTION(value) || IS_NATIVE_CLOSURE(value);
}
bool zym_isReference(ZymValue value) { return IS_REFERENCE(value); }
bool zym_isNativeReference(ZymValue value) { return IS_OBJ(value) && IS_NATIVE_REFERENCE(value); }
bool zym_isClosure(ZymValue value) { return IS_CLOSURE(value); }
bool zym_isPromptTag(ZymValue value) { return IS_OBJ(value) && IS_PROMPT_TAG(value); }
bool zym_isContinuation(ZymValue value) { return IS_OBJ(value) && IS_CONTINUATION(value); }

// =============================================================================
// VALUE EXTRACTION (SAFE)
// =============================================================================

bool zym_toBool(ZymValue value, bool* out) {
    if (!IS_BOOL(value)) return false;
    if (out) *out = AS_BOOL(value);
    return true;
}

bool zym_toNumber(ZymValue value, double* out) {
    if (!IS_DOUBLE(value)) return false;
    if (out) *out = AS_DOUBLE(value);
    return true;
}

bool zym_toString(ZymValue value, const char** out, int* length) {
    if (!IS_STRING(value)) return false;
    ObjString* str = AS_STRING(value);
    if (out) *out = str->chars;
    if (length) *length = str->length;  // UTF-8 character count
    return true;
}

bool zym_toStringBytes(ZymValue value, const char** out, int* byte_length) {
    if (!IS_STRING(value)) return false;
    ObjString* str = AS_STRING(value);
    if (out) *out = str->chars;
    if (byte_length) *byte_length = str->byte_length;  // Byte length
    return true;
}

// =============================================================================
// VALUE EXTRACTION (UNSAFE)
// =============================================================================

double zym_asNumber(ZymValue value) { return AS_DOUBLE(value); }
bool zym_asBool(ZymValue value) { return AS_BOOL(value); }
const char* zym_asCString(ZymValue value) { return AS_CSTRING(value); }

// =============================================================================
// VALUE INSPECTION
// =============================================================================

const char* zym_typeName(ZymValue value) {
    if (IS_NULL(value)) return "null";
    if (IS_BOOL(value)) return "bool";
    if (IS_DOUBLE(value)) return "number";
    if (IS_ENUM(value)) return "enum";
    if (IS_OBJ(value)) {
        Obj* obj = AS_OBJ(value);
        switch (obj->type) {
            case OBJ_STRING: return "string";
            case OBJ_LIST: return "list";
            case OBJ_MAP: return "map";
            case OBJ_FUNCTION: return "function";
            case OBJ_CLOSURE: return "closure";
            case OBJ_NATIVE_FUNCTION: return "native_function";
            case OBJ_NATIVE_CLOSURE: return "native_closure";
            case OBJ_NATIVE_CONTEXT: return "native_context";
            case OBJ_NATIVE_REFERENCE: return "native_reference";
            case OBJ_REFERENCE: return "reference";
            case OBJ_PROMPT_TAG: return "prompt_tag";
            case OBJ_CONTINUATION: return "continuation";
            case OBJ_STRUCT_SCHEMA: return "struct_schema";
            case OBJ_STRUCT_INSTANCE: return "struct";
            case OBJ_ENUM_SCHEMA: return "enum_schema";
            case OBJ_DISPATCHER: return "dispatcher";
            default: return "unknown";
        }
    }
    return "unknown";
}

int zym_stringLength(ZymValue value) {
    if (!IS_STRING(value)) return 0;
    return AS_STRING(value)->length;
}

int zym_stringByteLength(ZymValue value) {
    if (!IS_STRING(value)) return 0;
    return AS_STRING(value)->byte_length;
}

// =============================================================================
// VALUE DISPLAY
// =============================================================================

// Internal helper: write value to a dynamically growing buffer
static bool valueToStringHelper(VM* vm, Value value, char** buffer, size_t* buf_size, size_t* pos, Obj** visited, int depth) {
    char temp[256];

    if (depth >= 100) {
        size_t need = 3;
        while (*pos + need >= *buf_size) { *buf_size *= 2; *buffer = realloc(*buffer, *buf_size); if (!*buffer) return false; }
        memcpy(*buffer + *pos, "...", 3); *pos += 3;
        return true;
    }

#define APPEND(s, n) do { \
    size_t _n = (n); \
    while (*pos + _n >= *buf_size) { *buf_size *= 2; *buffer = realloc(*buffer, *buf_size); if (!*buffer) return false; } \
    memcpy(*buffer + *pos, (s), _n); *pos += _n; \
} while(0)

    if (IS_NULL(value)) {
        APPEND("null", 4);
    } else if (IS_BOOL(value)) {
        const char* s = AS_BOOL(value) ? "true" : "false";
        APPEND(s, strlen(s));
    } else if (IS_ENUM(value)) {
        int type_id = ENUM_TYPE_ID(value);
        int variant_idx = ENUM_VARIANT(value);
        if (vm != NULL) {
            ObjEnumSchema* schema = NULL;
            for (int i = 0; i < vm->globals.capacity; i++) {
                Entry* entry = &vm->globals.entries[i];
                if (entry->key != NULL && IS_OBJ(entry->value) && IS_ENUM_SCHEMA(entry->value)) {
                    ObjEnumSchema* candidate = AS_ENUM_SCHEMA(entry->value);
                    if (candidate->type_id == type_id) { schema = candidate; break; }
                }
            }
            if (schema != NULL && variant_idx >= 0 && variant_idx < schema->variant_count) {
                ObjString* vname = schema->variant_names[variant_idx];
                int len = snprintf(temp, sizeof(temp), "%.*s.%.*s",
                    schema->name->length, schema->name->chars,
                    vname->length, vname->chars);
                APPEND(temp, len);
            } else {
                int len = snprintf(temp, sizeof(temp), "<enum#%d.%d>", type_id, variant_idx);
                APPEND(temp, len);
            }
        } else {
            int len = snprintf(temp, sizeof(temp), "<enum#%d.%d>", type_id, variant_idx);
            APPEND(temp, len);
        }
    } else if (IS_DOUBLE(value)) {
        double num = AS_DOUBLE(value);
        int len;
        if (num == (long long)num && num >= -1e15 && num <= 1e15) {
            len = snprintf(temp, sizeof(temp), "%.0f", num);
        } else {
            len = snprintf(temp, sizeof(temp), "%g", num);
        }
        APPEND(temp, len);
    } else if (IS_OBJ(value)) {
        Obj* obj = AS_OBJ(value);

        for (int i = 0; i < depth; i++) {
            if (visited[i] == obj) { APPEND("...", 3); return true; }
        }
        visited[depth] = obj;

        switch (obj->type) {
            case OBJ_STRING: {
                ObjString* str = AS_STRING(value);
                APPEND(str->chars, str->byte_length);
                break;
            }
            case OBJ_LIST: {
                ObjList* list = AS_LIST(value);
                APPEND("[", 1);
                for (int i = 0; i < list->items.count; i++) {
                    if (i > 0) APPEND(", ", 2);
                    if (!valueToStringHelper(vm, list->items.values[i], buffer, buf_size, pos, visited, depth + 1)) return false;
                }
                APPEND("]", 1);
                break;
            }
            case OBJ_MAP: {
                ObjMap* map = AS_MAP(value);
                APPEND("{", 1);
                int printed = 0;
                for (int i = 0; i < map->table->capacity; i++) {
                    Entry* entry = &map->table->entries[i];
                    if (entry->key != NULL) {
                        if (printed > 0) APPEND(", ", 2);
                        APPEND("\"", 1);
                        APPEND(entry->key->chars, entry->key->byte_length);
                        APPEND("\": ", 3);
                        if (!valueToStringHelper(vm, entry->value, buffer, buf_size, pos, visited, depth + 1)) return false;
                        printed++;
                    }
                }
                APPEND("}", 1);
                break;
            }
            case OBJ_FUNCTION: {
                ObjFunction* fn = AS_FUNCTION(value);
                int len;
                if (fn->name) len = snprintf(temp, sizeof(temp), "<fn %.*s/%d>", fn->name->length, fn->name->chars, fn->arity);
                else len = snprintf(temp, sizeof(temp), "<fn /%d>", fn->arity);
                APPEND(temp, len);
                break;
            }
            case OBJ_CLOSURE: {
                ObjFunction* fn = AS_CLOSURE(value)->function;
                int len;
                if (fn->name) len = snprintf(temp, sizeof(temp), "<closure %.*s/%d>", fn->name->length, fn->name->chars, fn->arity);
                else len = snprintf(temp, sizeof(temp), "<closure /%d>", fn->arity);
                APPEND(temp, len);
                break;
            }
            case OBJ_NATIVE_FUNCTION: {
                ObjNativeFunction* native = AS_NATIVE_FUNCTION(value);
                int len;
                if (native->name) len = snprintf(temp, sizeof(temp), "<native fn %.*s/%d>", native->name->length, native->name->chars, native->arity);
                else len = snprintf(temp, sizeof(temp), "<native fn /%d>", native->arity);
                APPEND(temp, len);
                break;
            }
            case OBJ_NATIVE_CONTEXT: {
                APPEND("<native context>", 16);
                break;
            }
            case OBJ_NATIVE_CLOSURE: {
                ObjNativeClosure* closure = AS_NATIVE_CLOSURE(value);
                int len;
                if (closure->name) len = snprintf(temp, sizeof(temp), "<native closure %.*s/%d>", closure->name->length, closure->name->chars, closure->arity);
                else len = snprintf(temp, sizeof(temp), "<native closure /%d>", closure->arity);
                APPEND(temp, len);
                break;
            }
            case OBJ_REFERENCE:
            case OBJ_NATIVE_REFERENCE: {
                Value deref_value;
                if (dereferenceValue(vm, value, &deref_value)) {
                    if (!valueToStringHelper(vm, deref_value, buffer, buf_size, pos, visited, depth + 1)) return false;
                } else {
                    const char* msg = obj->type == OBJ_REFERENCE ? "<undefined ref>" : "<dead native ref>";
                    APPEND(msg, strlen(msg));
                }
                break;
            }
            case OBJ_STRUCT_INSTANCE: {
                ObjStructInstance* inst = AS_STRUCT_INSTANCE(value);
                ObjStructSchema* schema = inst->schema;
                APPEND(schema->name->chars, schema->name->byte_length);
                APPEND(" { ", 3);
                for (int i = 0; i < schema->field_count; i++) {
                    if (i > 0) APPEND(", ", 2);
                    APPEND(schema->field_names[i]->chars, schema->field_names[i]->byte_length);
                    APPEND(": ", 2);
                    if (!valueToStringHelper(vm, inst->fields[i], buffer, buf_size, pos, visited, depth + 1)) return false;
                }
                APPEND(" }", 2);
                break;
            }
            case OBJ_PROMPT_TAG: {
                ObjPromptTag* tag = AS_PROMPT_TAG(value);
                int len;
                if (tag->name != NULL) len = snprintf(temp, sizeof(temp), "<prompt_tag: %.*s>", tag->name->length, tag->name->chars);
                else len = snprintf(temp, sizeof(temp), "<prompt_tag #%u>", tag->id);
                APPEND(temp, len);
                break;
            }
            case OBJ_CONTINUATION: {
                ObjContinuation* cont = AS_CONTINUATION(value);
                const char* state_str = "valid";
                if (cont->state == CONT_CONSUMED) state_str = "consumed";
                else if (cont->state == CONT_INVALID) state_str = "invalid";
                int len = snprintf(temp, sizeof(temp), "<continuation: %s>", state_str);
                APPEND(temp, len);
                break;
            }
            case OBJ_DISPATCHER: {
                APPEND("<dispatcher>", 12);
                break;
            }
            default: {
                int len = snprintf(temp, sizeof(temp), "<object>");
                APPEND(temp, len);
                break;
            }
        }
    } else {
        APPEND("<unknown>", 9);
    }

#undef APPEND
    return true;
}

ZymValue zym_valueToString(ZymVM* vm, ZymValue value) {
    if (vm == NULL) return ZYM_ERROR;

    size_t buf_size = 256;
    char* buffer = malloc(buf_size);
    if (buffer == NULL) return ZYM_ERROR;

    size_t pos = 0;
    Obj* visited[100] = {0};

    if (!valueToStringHelper(vm, value, &buffer, &buf_size, &pos, visited, 0)) {
        free(buffer);
        return ZYM_ERROR;
    }

    buffer[pos] = '\0';
    ZymValue result = zym_newString(vm, buffer);
    free(buffer);
    return result;
}

void zym_printValue(ZymVM* vm, ZymValue value) {
    printValue(vm, value);
}

// =============================================================================
// VALUE CREATION
// =============================================================================

ZymValue zym_newNull(void) { return NULL_VAL; }
ZymValue zym_newBool(bool value) { return BOOL_VAL(value); }
ZymValue zym_newNumber(double value) { return DOUBLE_VAL(value); }

ZymValue zym_newString(ZymVM* vm, const char* str) {
    if (!vm || !str) return NULL_VAL;
    ObjString* obj = copyString(vm, str, (int)strlen(str));
    return OBJ_VAL(obj);
}

ZymValue zym_newStringN(ZymVM* vm, const char* str, int len) {
    if (!vm || !str || len < 0) return NULL_VAL;
    ObjString* obj = copyString(vm, str, len);
    return OBJ_VAL(obj);
}

ZymValue zym_newList(ZymVM* vm) {
    if (!vm) return NULL_VAL;
    ObjList* list = newList(vm);
    return OBJ_VAL(list);
}

ZymValue zym_newMap(ZymVM* vm) {
    if (!vm) return NULL_VAL;
    ObjMap* map = newMap(vm);
    return OBJ_VAL(map);
}

ZymValue zym_newStruct(ZymVM* vm, const char* structName) {
    if (!vm || !structName) return NULL_VAL;

    // Look up the struct schema in globals
    ObjString* name = copyString(vm, structName, (int)strlen(structName));
    Value schemaVal;
    if (!globalGet(vm, name, &schemaVal) || !IS_STRUCT_SCHEMA(schemaVal)) {
        return NULL_VAL;  // Schema not found
    }

    ObjStructSchema* schema = AS_STRUCT_SCHEMA(schemaVal);
    ObjStructInstance* instance = newStructInstance(vm, schema);
    return OBJ_VAL(instance);
}

ZymValue zym_newEnum(ZymVM* vm, const char* enumName, const char* variantName) {
    if (!vm || !enumName || !variantName) return NULL_VAL;

    // Look up the enum schema in globals
    ObjString* name = copyString(vm, enumName, (int)strlen(enumName));
    Value schemaVal;
    if (!globalGet(vm, name, &schemaVal) || !IS_ENUM_SCHEMA(schemaVal)) {
        return NULL_VAL;  // Schema not found
    }

    ObjEnumSchema* schema = AS_ENUM_SCHEMA(schemaVal);

    // Find variant index
    int variantIndex = -1;
    for (int i = 0; i < schema->variant_count; i++) {
        if (strcmp(schema->variant_names[i]->chars, variantName) == 0) {
            variantIndex = i;
            break;
        }
    }

    if (variantIndex == -1) {
        return NULL_VAL;  // Variant not found
    }

    return ENUM_VAL(schema->type_id, variantIndex);
}

// =============================================================================
// LIST OPERATIONS
// =============================================================================

int zym_listLength(ZymValue list) {
    if (!IS_LIST(list)) return -1;
    ObjList* lst = AS_LIST(list);
    return lst->items.count;
}

ZymValue zym_listGet(ZymVM* vm, ZymValue list, int index) {
    if (!IS_LIST(list)) return ZYM_ERROR;
    ObjList* lst = AS_LIST(list);
    if (index < 0 || index >= lst->items.count) return ZYM_ERROR;
    return lst->items.values[index];
}

bool zym_listSet(ZymVM* vm, ZymValue list, int index, ZymValue val) {
    if (!IS_LIST(list)) return false;
    ObjList* lst = AS_LIST(list);
    if (index < 0 || index >= lst->items.count) return false;
    lst->items.values[index] = val;
    return true;
}

bool zym_listAppend(ZymVM* vm, ZymValue list, ZymValue val) {
    if (!IS_LIST(list)) return false;
    ObjList* lst = AS_LIST(list);
    writeValueArray(vm, &lst->items, val);
    return true;
}

bool zym_listInsert(ZymVM* vm, ZymValue list, int index, ZymValue val) {
    if (!IS_LIST(list)) return false;
    ObjList* lst = AS_LIST(list);
    if (index < 0 || index > lst->items.count) return false;

    if (lst->items.count >= lst->items.capacity) {
        int oldCapacity = lst->items.capacity;
        int newCapacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        lst->items.values = (Value*)reallocate(vm, lst->items.values,
                                               sizeof(Value) * oldCapacity,
                                               sizeof(Value) * newCapacity);
        lst->items.capacity = newCapacity;
    }

    for (int i = lst->items.count; i > index; i--) {
        lst->items.values[i] = lst->items.values[i - 1];
    }

    lst->items.values[index] = val;
    lst->items.count++;
    return true;
}

bool zym_listRemove(ZymVM* vm, ZymValue list, int index) {
    if (!IS_LIST(list)) return false;
    ObjList* lst = AS_LIST(list);
    if (index < 0 || index >= lst->items.count) return false;

    for (int i = index; i < lst->items.count - 1; i++) {
        lst->items.values[i] = lst->items.values[i + 1];
    }

    lst->items.count--;
    return true;
}

// =============================================================================
// MAP OPERATIONS
// =============================================================================

int zym_mapSize(ZymValue map) {
    if (!IS_MAP(map)) return -1;
    ObjMap* m = AS_MAP(map);
    return m->table->count;
}

ZymValue zym_mapGet(ZymVM* vm, ZymValue map, const char* key) {
    if (!IS_MAP(map) || !key) return ZYM_ERROR;
    ObjMap* m = AS_MAP(map);
    ObjString* keyStr = copyString(vm, key, (int)strlen(key));

    Value result;
    if (!tableGet(m->table, keyStr, &result)) {
        return ZYM_ERROR;
    }
    return result;
}

bool zym_mapSet(ZymVM* vm, ZymValue map, const char* key, ZymValue val) {
    if (!IS_MAP(map) || !key) return false;
    ObjMap* m = AS_MAP(map);
    ObjString* keyStr = copyString(vm, key, (int)strlen(key));
    tableSet(vm, m->table, keyStr, val);
    return true;
}

bool zym_mapHas(ZymValue map, const char* key) {
    if (!IS_MAP(map) || !key) return false;
    ObjMap* m = AS_MAP(map);

    uint32_t hash = 0;
    int len = (int)strlen(key);
    for (int i = 0; i < len; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }

    ObjString* keyStr = tableFindString(m->table, key, len, hash);
    if (!keyStr) return false;

    Value dummy;
    return tableGet(m->table, keyStr, &dummy);
}

bool zym_mapDelete(ZymVM* vm, ZymValue map, const char* key) {
    if (!IS_MAP(map) || !key) return false;
    ObjMap* m = AS_MAP(map);

    uint32_t hash = 0;
    int len = (int)strlen(key);
    for (int i = 0; i < len; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }

    ObjString* keyStr = tableFindString(m->table, key, len, hash);
    if (!keyStr) return false;

    return tableDelete(m->table, keyStr);
}

void zym_mapForEach(ZymVM* vm, ZymValue map, ZymMapIterFunc func, void* userdata) {
    if (!IS_MAP(map) || !func) return;
    ObjMap* m = AS_MAP(map);

    for (int i = 0; i < m->table->capacity; i++) {
        Entry* entry = &m->table->entries[i];
        if (entry->key != NULL) {
            bool shouldContinue = func(vm, entry->key->chars, entry->value, userdata);
            if (!shouldContinue) break;
        }
    }
}

// =============================================================================
// STRUCT OPERATIONS
// =============================================================================

ZymValue zym_structGet(ZymVM* vm, ZymValue structVal, const char* fieldName) {
    if (!IS_STRUCT_INSTANCE(structVal) || !fieldName) return ZYM_ERROR;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);
    ObjString* fieldStr = copyString(vm, fieldName, (int)strlen(fieldName));

    Value indexVal;
    if (!tableGet(inst->schema->field_to_index, fieldStr, &indexVal)) {
        return ZYM_ERROR;
    }

    int index = (int)AS_DOUBLE(indexVal);
    if (index < 0 || index >= inst->field_count) return ZYM_ERROR;

    return inst->fields[index];
}

bool zym_structSet(ZymVM* vm, ZymValue structVal, const char* fieldName, ZymValue val) {
    if (!IS_STRUCT_INSTANCE(structVal) || !fieldName) return false;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);
    ObjString* fieldStr = copyString(vm, fieldName, (int)strlen(fieldName));

    Value indexVal;
    if (!tableGet(inst->schema->field_to_index, fieldStr, &indexVal)) {
        return false;
    }

    int index = (int)AS_DOUBLE(indexVal);
    if (index < 0 || index >= inst->field_count) return false;

    inst->fields[index] = val;
    return true;
}

bool zym_structHasField(ZymValue structVal, const char* fieldName) {
    if (!IS_STRUCT_INSTANCE(structVal) || !fieldName) return false;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);

    for (int i = 0; i < inst->schema->field_count; i++) {
        if (strcmp(inst->schema->field_names[i]->chars, fieldName) == 0) {
            return true;
        }
    }
    return false;
}

const char* zym_structGetName(ZymValue structVal) {
    if (!IS_STRUCT_INSTANCE(structVal)) return NULL;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);
    return inst->schema->name->chars;
}

int zym_structFieldCount(ZymValue structVal) {
    if (!IS_STRUCT_INSTANCE(structVal)) return -1;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);
    return inst->field_count;
}

const char* zym_structFieldNameAt(ZymValue structVal, int index) {
    if (!IS_STRUCT_INSTANCE(structVal)) return NULL;
    ObjStructInstance* inst = AS_STRUCT_INSTANCE(structVal);
    if (index < 0 || index >= inst->field_count) return NULL;
    return inst->schema->field_names[index]->chars;
}

// =============================================================================
// ENUM OPERATIONS
// =============================================================================

const char* zym_enumGetName(ZymVM* vm, ZymValue enumVal) {
    if (!IS_ENUM(enumVal)) return NULL;
    int type_id = ENUM_TYPE_ID(enumVal);

    for (int i = 0; i < vm->globals.capacity; i++) {
        Entry* entry = &vm->globals.entries[i];
        if (entry->key != NULL && IS_ENUM_SCHEMA(entry->value)) {
            ObjEnumSchema* schema = AS_ENUM_SCHEMA(entry->value);
            if (schema->type_id == type_id) {
                return schema->name->chars;
            }
        }
    }
    return NULL;
}

const char* zym_enumGetVariant(ZymVM* vm, ZymValue enumVal) {
    if (!IS_ENUM(enumVal)) return NULL;
    int type_id = ENUM_TYPE_ID(enumVal);
    int variant = ENUM_VARIANT(enumVal);

    for (int i = 0; i < vm->globals.capacity; i++) {
        Entry* entry = &vm->globals.entries[i];
        if (entry->key != NULL && IS_ENUM_SCHEMA(entry->value)) {
            ObjEnumSchema* schema = AS_ENUM_SCHEMA(entry->value);
            if (schema->type_id == type_id) {
                if (variant >= 0 && variant < schema->variant_count) {
                    return schema->variant_names[variant]->chars;
                }
                return NULL;
            }
        }
    }
    return NULL;
}

bool zym_enumEquals(ZymValue a, ZymValue b) {
    if (!IS_ENUM(a) || !IS_ENUM(b)) return false;
    return ENUM_TYPE_ID(a) == ENUM_TYPE_ID(b) && ENUM_VARIANT(a) == ENUM_VARIANT(b);
}

int zym_enumVariantIndex(ZymVM* vm, ZymValue enumVal) {
    if (!IS_ENUM(enumVal)) return -1;
    return ENUM_VARIANT(enumVal);
}

// =============================================================================
// REFERENCE OPERATIONS
// =============================================================================

ZymValue zym_deref(ZymVM* vm, ZymValue val) {
    Value result;
    if (dereferenceValue(vm, val, &result)) {
        return result;
    }
    return val;
}

bool zym_refSet(ZymVM* vm, ZymValue refVal, ZymValue newVal) {
    return writeReferenceValue(vm, refVal, newVal);
}

// =============================================================================
// CALLING SCRIPT FUNCTIONS FROM C
// =============================================================================

bool zym_hasFunction(ZymVM* vm, const char* funcName, int arity) {
    if (!vm || !funcName) return false;

    char mangled[256];
    snprintf(mangled, sizeof(mangled), "%s@%d", funcName, arity);

    ObjString* nameObj = copyString(vm, mangled, (int)strlen(mangled));
    Value funcVal;

    return globalGet(vm, nameObj, &funcVal) &&
           (IS_CLOSURE(funcVal) || IS_NATIVE_FUNCTION(funcVal));
}

ZymStatus zym_callv(ZymVM* vm, const char* funcName, int argc, ZymValue* argv) {
    if (!vm || !funcName) return ZYM_STATUS_RUNTIME_ERROR;

    bool success = zym_call_prepare(vm, funcName, argc);
    if (!success) return ZYM_STATUS_RUNTIME_ERROR;

    for (int i = 0; i < argc; i++) {
        vm->stack[vm->api_stack_top + 1 + i] = argv[i];
    }
    vm->api_stack_top += argc;

    InterpretResult result = zym_call_execute(vm, argc);
    switch (result) {
        case INTERPRET_OK: return ZYM_STATUS_OK;
        case INTERPRET_RUNTIME_ERROR:
        default: return ZYM_STATUS_RUNTIME_ERROR;
    }
}

ZymStatus zym_call(ZymVM* vm, const char* funcName, int argc, ...) {
    if (!vm || !funcName) return ZYM_STATUS_RUNTIME_ERROR;

    ZymValue* args = NULL;
    if (argc > 0) {
        args = (ZymValue*)malloc(sizeof(ZymValue) * argc);
        if (!args) return ZYM_STATUS_RUNTIME_ERROR;

        va_list ap;
        va_start(ap, argc);
        for (int i = 0; i < argc; i++) {
            args[i] = va_arg(ap, ZymValue);
        }
        va_end(ap);
    }

    ZymStatus result = zym_callv(vm, funcName, argc, args);

    if (args) free(args);
    return result;
}

ZymValue zym_getCallResult(ZymVM* vm) {
    if (!vm) return NULL_VAL;
    return zym_call_getResult(vm);
}

// =============================================================================
// GC PROTECTION (TEMPORARY ROOTS)
// =============================================================================

void zym_pushRoot(ZymVM* vm, ZymValue val) {
    if (!vm || !IS_OBJ(val)) return;
    pushTempRoot(vm, AS_OBJ(val));
}

void zym_popRoot(ZymVM* vm) {
    if (!vm) return;
    popTempRoot(vm);
}

ZymValue zym_peekRoot(ZymVM* vm, int depth) {
    if (!vm || depth < 0 || depth >= vm->temp_root_count) return NULL_VAL;
    int index = vm->temp_root_count - 1 - depth;
    return OBJ_VAL(vm->temp_roots[index]);
}

// =============================================================================
// ERROR HANDLING
// =============================================================================

void zym_runtimeError(ZymVM* vm, const char* format, ...) {
    if (!vm || !format) return;

    va_list args;
    va_start(args, format);
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    runtimeError((VM*)vm, "%s", buffer);
}

