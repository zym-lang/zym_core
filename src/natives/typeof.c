#include "typeof.h"
#include "../object.h"

// =============================================================================
// TYPEOF NATIVE
// =============================================================================

ZymValue nativeTypeof(ZymVM* vm, ZymValue value) {
    if (IS_NULL(value))   return zym_newString(vm, "null");
    if (IS_BOOL(value))   return zym_newString(vm, "bool");
    if (IS_DOUBLE(value)) return zym_newString(vm, "number");
    if (IS_ENUM(value))   return zym_newString(vm, "enum");

    if (IS_OBJ(value)) {
        switch (AS_OBJ(value)->type) {
            case OBJ_STRING:          return zym_newString(vm, "string");
            case OBJ_INT64:           return zym_newString(vm, "number");
            case OBJ_LIST:            return zym_newString(vm, "list");
            case OBJ_MAP:             return zym_newString(vm, "map");
            case OBJ_FUNCTION:        return zym_newString(vm, "function");
            case OBJ_CLOSURE:         return zym_newString(vm, "function");
            case OBJ_NATIVE_FUNCTION: return zym_newString(vm, "function");
            case OBJ_NATIVE_CLOSURE:  return zym_newString(vm, "function");
            case OBJ_NATIVE_CONTEXT:  return zym_newString(vm, "function");
            case OBJ_DISPATCHER:      return zym_newString(vm, "function");
            case OBJ_UPVALUE:         return zym_newString(vm, "upvalue");
            case OBJ_STRUCT_SCHEMA:   return zym_newString(vm, "struct_schema");
            case OBJ_STRUCT_INSTANCE: return zym_newString(vm, "struct");
            case OBJ_ENUM_SCHEMA:     return zym_newString(vm, "enum_schema");
            case OBJ_PROMPT_TAG:      return zym_newString(vm, "prompt_tag");
            case OBJ_CONTINUATION:    return zym_newString(vm, "continuation");
            default:                  return zym_newString(vm, "unknown");
        }
    }

    return zym_newString(vm, "unknown");
}

// =============================================================================
// Registration
// =============================================================================

void registerTypeofNative(VM* vm) {
    zym_defineNative(vm, "typeof(value)", nativeTypeof);
}
