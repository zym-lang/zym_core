#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "./native.h"
#include "./compiler.h"
#include "./memory.h"
#include "gc.h"

static Value native_dispatch_0(VM* vm, Value* args, void* func_ptr) {
    ZymNative0 func = (ZymNative0)func_ptr;
    return func(vm);
}

static Value native_dispatch_1(VM* vm, Value* args, void* func_ptr) {
    ZymNative1 func = (ZymNative1)func_ptr;
    return func(vm, args[0]);
}

static Value native_dispatch_2(VM* vm, Value* args, void* func_ptr) {
    ZymNative2 func = (ZymNative2)func_ptr;
    return func(vm, args[0], args[1]);
}

static Value native_dispatch_3(VM* vm, Value* args, void* func_ptr) {
    ZymNative3 func = (ZymNative3)func_ptr;
    return func(vm, args[0], args[1], args[2]);
}

static Value native_dispatch_4(VM* vm, Value* args, void* func_ptr) {
    ZymNative4 func = (ZymNative4)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3]);
}

static Value native_dispatch_5(VM* vm, Value* args, void* func_ptr) {
    ZymNative5 func = (ZymNative5)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4]);
}

static Value native_dispatch_6(VM* vm, Value* args, void* func_ptr) {
    ZymNative6 func = (ZymNative6)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5]);
}

static Value native_dispatch_7(VM* vm, Value* args, void* func_ptr) {
    ZymNative7 func = (ZymNative7)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
}

static Value native_dispatch_8(VM* vm, Value* args, void* func_ptr) {
    ZymNative8 func = (ZymNative8)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
}

static Value native_dispatch_9(VM* vm, Value* args, void* func_ptr) {
    ZymNative9 func = (ZymNative9)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
}

static Value native_dispatch_10(VM* vm, Value* args, void* func_ptr) {
    ZymNative10 func = (ZymNative10)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]);
}

static Value native_dispatch_11(VM* vm, Value* args, void* func_ptr) {
    ZymNative11 func = (ZymNative11)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10]);
}

static Value native_dispatch_12(VM* vm, Value* args, void* func_ptr) {
    ZymNative12 func = (ZymNative12)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11]);
}

static Value native_dispatch_13(VM* vm, Value* args, void* func_ptr) {
    ZymNative13 func = (ZymNative13)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12]);
}

static Value native_dispatch_14(VM* vm, Value* args, void* func_ptr) {
    ZymNative14 func = (ZymNative14)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13]);
}

static Value native_dispatch_15(VM* vm, Value* args, void* func_ptr) {
    ZymNative15 func = (ZymNative15)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14]);
}

static Value native_dispatch_16(VM* vm, Value* args, void* func_ptr) {
    ZymNative16 func = (ZymNative16)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15]);
}

static Value native_dispatch_17(VM* vm, Value* args, void* func_ptr) {
    ZymNative17 func = (ZymNative17)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16]);
}

static Value native_dispatch_18(VM* vm, Value* args, void* func_ptr) {
    ZymNative18 func = (ZymNative18)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17]);
}

static Value native_dispatch_19(VM* vm, Value* args, void* func_ptr) {
    ZymNative19 func = (ZymNative19)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18]);
}

static Value native_dispatch_20(VM* vm, Value* args, void* func_ptr) {
    ZymNative20 func = (ZymNative20)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19]);
}

static Value native_dispatch_21(VM* vm, Value* args, void* func_ptr) {
    ZymNative21 func = (ZymNative21)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20]);
}

static Value native_dispatch_22(VM* vm, Value* args, void* func_ptr) {
    ZymNative22 func = (ZymNative22)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21]);
}

static Value native_dispatch_23(VM* vm, Value* args, void* func_ptr) {
    ZymNative23 func = (ZymNative23)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21], args[22]);
}

static Value native_dispatch_24(VM* vm, Value* args, void* func_ptr) {
    ZymNative24 func = (ZymNative24)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21], args[22], args[23]);
}

static Value native_dispatch_25(VM* vm, Value* args, void* func_ptr) {
    ZymNative25 func = (ZymNative25)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21], args[22], args[23], args[24]);
}

static Value native_dispatch_26(VM* vm, Value* args, void* func_ptr) {
    ZymNative26 func = (ZymNative26)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21], args[22], args[23], args[24], args[25]);
}

static NativeDispatcher dispatchers[MAX_NATIVE_ARITY + 1] = {
    native_dispatch_0,
    native_dispatch_1,
    native_dispatch_2,
    native_dispatch_3,
    native_dispatch_4,
    native_dispatch_5,
    native_dispatch_6,
    native_dispatch_7,
    native_dispatch_8,
    native_dispatch_9,
    native_dispatch_10,
    native_dispatch_11,
    native_dispatch_12,
    native_dispatch_13,
    native_dispatch_14,
    native_dispatch_15,
    native_dispatch_16,
    native_dispatch_17,
    native_dispatch_18,
    native_dispatch_19,
    native_dispatch_20,
    native_dispatch_21,
    native_dispatch_22,
    native_dispatch_23,
    native_dispatch_24,
    native_dispatch_25,
    native_dispatch_26,
};

NativeDispatcher getNativeDispatcher(int arity) {
    if (arity < 0 || arity > MAX_NATIVE_ARITY) {
        return NULL;
    }
    return dispatchers[arity];
}

static Value native_closure_dispatch_0(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure0 func = (ZymNativeClosure0)func_ptr;
    return func(vm, args[0]);  // args[0] = context
}

static Value native_closure_dispatch_1(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure1 func = (ZymNativeClosure1)func_ptr;
    return func(vm, args[0], args[1]);
}

static Value native_closure_dispatch_2(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure2 func = (ZymNativeClosure2)func_ptr;
    return func(vm, args[0], args[1], args[2]);
}

static Value native_closure_dispatch_3(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure3 func = (ZymNativeClosure3)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3]);
}

static Value native_closure_dispatch_4(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure4 func = (ZymNativeClosure4)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4]);
}

static Value native_closure_dispatch_5(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure5 func = (ZymNativeClosure5)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5]);
}

static Value native_closure_dispatch_6(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure6 func = (ZymNativeClosure6)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
}

static Value native_closure_dispatch_7(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure7 func = (ZymNativeClosure7)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
}

static Value native_closure_dispatch_8(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure8 func = (ZymNativeClosure8)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
}

static Value native_closure_dispatch_9(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure9 func = (ZymNativeClosure9)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]);
}

static Value native_closure_dispatch_10(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure10 func = (ZymNativeClosure10)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10]);
}

static Value native_closure_dispatch_11(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure11 func = (ZymNativeClosure11)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11]);
}

static Value native_closure_dispatch_12(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure12 func = (ZymNativeClosure12)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12]);
}

static Value native_closure_dispatch_13(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure13 func = (ZymNativeClosure13)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13]);
}

static Value native_closure_dispatch_14(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure14 func = (ZymNativeClosure14)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14]);
}

static Value native_closure_dispatch_15(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure15 func = (ZymNativeClosure15)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15]);
}

static Value native_closure_dispatch_16(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure16 func = (ZymNativeClosure16)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16]);
}

static Value native_closure_dispatch_17(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure17 func = (ZymNativeClosure17)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17]);
}

static Value native_closure_dispatch_18(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure18 func = (ZymNativeClosure18)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18]);
}

static Value native_closure_dispatch_19(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure19 func = (ZymNativeClosure19)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19]);
}

static Value native_closure_dispatch_20(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure20 func = (ZymNativeClosure20)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20]);
}

static Value native_closure_dispatch_21(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure21 func = (ZymNativeClosure21)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21]);
}

static Value native_closure_dispatch_22(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure22 func = (ZymNativeClosure22)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21], args[22]);
}

static Value native_closure_dispatch_23(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure23 func = (ZymNativeClosure23)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21], args[22], args[23]);
}

static Value native_closure_dispatch_24(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure24 func = (ZymNativeClosure24)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21], args[22], args[23], args[24]);
}

static Value native_closure_dispatch_25(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure25 func = (ZymNativeClosure25)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21], args[22], args[23], args[24], args[25]);
}

static Value native_closure_dispatch_26(VM* vm, Value* args, void* func_ptr) {
    ZymNativeClosure26 func = (ZymNativeClosure26)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21], args[22], args[23], args[24], args[25], args[26]);
}

static NativeDispatcher closure_dispatchers[MAX_NATIVE_ARITY + 1] = {
    native_closure_dispatch_0,
    native_closure_dispatch_1,
    native_closure_dispatch_2,
    native_closure_dispatch_3,
    native_closure_dispatch_4,
    native_closure_dispatch_5,
    native_closure_dispatch_6,
    native_closure_dispatch_7,
    native_closure_dispatch_8,
    native_closure_dispatch_9,
    native_closure_dispatch_10,
    native_closure_dispatch_11,
    native_closure_dispatch_12,
    native_closure_dispatch_13,
    native_closure_dispatch_14,
    native_closure_dispatch_15,
    native_closure_dispatch_16,
    native_closure_dispatch_17,
    native_closure_dispatch_18,
    native_closure_dispatch_19,
    native_closure_dispatch_20,
    native_closure_dispatch_21,
    native_closure_dispatch_22,
    native_closure_dispatch_23,
    native_closure_dispatch_24,
    native_closure_dispatch_25,
    native_closure_dispatch_26,
};

NativeDispatcher getNativeClosureDispatcher(int arity) {
    if (arity < 0 || arity > MAX_NATIVE_ARITY) {
        return NULL;
    }
    return closure_dispatchers[arity];
}

static const char* skipWhitespace(const char* str) {
    while (*str && isspace(*str)) str++;
    return str;
}

static const char* parseIdentifier(const char* str, char* out, int max_len) {
    str = skipWhitespace(str);
    int i = 0;
    while (*str && (isalnum(*str) || *str == '_') && i < max_len - 1) {
        out[i++] = *str++;
    }
    out[i] = '\0';
    return str;
}

static const char* parseQualifier(const char* str, uint8_t* out_qualifier) {
    str = skipWhitespace(str);

    char qualifier[16];
    const char* start = str;
    int i = 0;
    while (*str && isalpha(*str) && i < 15) {
        qualifier[i++] = *str++;
    }
    qualifier[i] = '\0';

    if (strcmp(qualifier, "ref") == 0) {
        *out_qualifier = PARAM_REF;
        return str;
    } else if (strcmp(qualifier, "val") == 0) {
        *out_qualifier = PARAM_VAL;
        return str;
    } else if (strcmp(qualifier, "slot") == 0) {
        *out_qualifier = PARAM_SLOT;
        return str;
    } else if (strcmp(qualifier, "clone") == 0) {
        *out_qualifier = PARAM_CLONE;
        return str;
    } else if (strcmp(qualifier, "typeof") == 0) {
        *out_qualifier = PARAM_TYPEOF;
        return str;
    }

    *out_qualifier = PARAM_NORMAL;
    return start;
}

bool parseNativeSignature(const char* signature, char* out_name, int* out_arity, uint8_t** out_qualifiers) {
    if (!signature || !out_name || !out_arity || !out_qualifiers) {
        return false;
    }

    const char* ptr = parseIdentifier(signature, out_name, 256);
    if (out_name[0] == '\0') {
        fprintf(stderr, "Native function signature parse error: missing function name\n");
        return false;
    }

    ptr = skipWhitespace(ptr);
    if (*ptr != '(') {
        fprintf(stderr, "Native function signature parse error: expected '(' after function name\n");
        return false;
    }
    ptr++;

    int arity = 0;
    uint8_t temp_qualifiers[MAX_NATIVE_ARITY];
    memset(temp_qualifiers, 0, sizeof(temp_qualifiers));

    ptr = skipWhitespace(ptr);
    if (*ptr != ')') {
        while (true) {
            if (arity >= MAX_NATIVE_ARITY) {
                fprintf(stderr, "Native function signature parse error: too many parameters (max %d)\n", MAX_NATIVE_ARITY);
                return false;
            }

            uint8_t qualifier = PARAM_NORMAL;
            ptr = parseQualifier(ptr, &qualifier);
            temp_qualifiers[arity] = qualifier;

            char param_name[256];
            ptr = skipWhitespace(ptr);
            ptr = parseIdentifier(ptr, param_name, 256);
            if (param_name[0] == '\0') {
                fprintf(stderr, "Native function signature parse error: expected parameter name\n");
                return false;
            }

            arity++;

            ptr = skipWhitespace(ptr);
            if (*ptr == ')') {
                break;
            } else if (*ptr == ',') {
                ptr++;
            } else {
                fprintf(stderr, "Native function signature parse error: expected ',' or ')' after parameter\n");
                return false;
            }
        }
    }

    if (*ptr != ')') {
        fprintf(stderr, "Native function signature parse error: expected ')'\n");
        return false;
    }

    *out_arity = arity;

    if (arity > 0) {
        *out_qualifiers = (uint8_t*)malloc(arity * sizeof(uint8_t));
        if (!*out_qualifiers) {
            fprintf(stderr, "Native function signature parse error: memory allocation failed\n");
            return false;
        }
        memcpy(*out_qualifiers, temp_qualifiers, arity * sizeof(uint8_t));
    } else {
        *out_qualifiers = NULL;
    }

    return true;
}

bool registerNativeFunction(VM* vm, const char* signature, void* func_ptr) {
    char func_name[256];
    int arity;
    uint8_t* qualifiers = NULL;

    if (!parseNativeSignature(signature, func_name, &arity, &qualifiers)) {
        return false;
    }

    if (arity > MAX_NATIVE_ARITY) {
        fprintf(stderr, "Native function '%s' has too many parameters (max %d)\n", func_name, MAX_NATIVE_ARITY);
        if (qualifiers) free(qualifiers);
        return false;
    }

    NativeDispatcher dispatcher = getNativeDispatcher(arity);
    if (!dispatcher) {
        fprintf(stderr, "No dispatcher available for arity %d\n", arity);
        if (qualifiers) free(qualifiers);
        return false;
    }

    // Create mangled name (funcName@arity)
    char mangled_name[256];
    snprintf(mangled_name, sizeof(mangled_name), "%s@%d", func_name, arity);

    ObjString* name_obj = copyString(vm, mangled_name, (int)strlen(mangled_name));
    pushTempRoot(vm, (Obj*)name_obj);

    ObjNativeFunction* native = newNativeFunction(vm, name_obj, arity, func_ptr, dispatcher);
    pushTempRoot(vm, (Obj*)native);

    if (arity > 0 && qualifiers) {
        memcpy(native->param_qualifiers, qualifiers, arity * sizeof(uint8_t));
        free(qualifiers);
    }

    // Registered before compilation, bypasses slot optimization
    tableSet(vm, &vm->globals, name_obj, OBJ_VAL(native));
    popTempRoot(vm);
    popTempRoot(vm);

    return true;
}
