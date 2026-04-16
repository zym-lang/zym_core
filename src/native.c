#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "./native.h"
#include "./compiler.h"
#include "./memory.h"
#include "gc.h"

// Fixed-arity native function trampolines (0-10)
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

static NativeDispatcher dispatchers[MAX_NATIVE_ARITY + 1] = {
    native_dispatch_0,  native_dispatch_1,  native_dispatch_2,
    native_dispatch_3,  native_dispatch_4,  native_dispatch_5,
    native_dispatch_6,  native_dispatch_7,  native_dispatch_8,
    native_dispatch_9,  native_dispatch_10,
};

NativeDispatcher getNativeDispatcher(int arity) {
    if (arity < 0 || arity > MAX_NATIVE_ARITY) {
        return NULL;
    }
    return dispatchers[arity];
}

// Fixed-arity native closure trampolines (0-10)
// args[0] = context, args[1..] = call args
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

static NativeDispatcher closure_dispatchers[MAX_NATIVE_ARITY + 1] = {
    native_closure_dispatch_0,  native_closure_dispatch_1,  native_closure_dispatch_2,
    native_closure_dispatch_3,  native_closure_dispatch_4,  native_closure_dispatch_5,
    native_closure_dispatch_6,  native_closure_dispatch_7,  native_closure_dispatch_8,
    native_closure_dispatch_9,  native_closure_dispatch_10,
};

NativeDispatcher getNativeClosureDispatcher(int arity) {
    if (arity < 0 || arity > MAX_NATIVE_ARITY) {
        return NULL;
    }
    return closure_dispatchers[arity];
}

// Variadic function trampolines: split args into positional + vargs array
// Signature: (VM* vm, Value* args, void* func_ptr, int argc)
// args points to the raw stack args, argc is total arg count

static Value native_variadic_dispatch_0(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeVariadic0 func = (ZymNativeVariadic0)func_ptr;
    return func(vm, args, argc);
}

static Value native_variadic_dispatch_1(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeVariadic1 func = (ZymNativeVariadic1)func_ptr;
    return func(vm, args[0], args + 1, argc - 1);
}

static Value native_variadic_dispatch_2(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeVariadic2 func = (ZymNativeVariadic2)func_ptr;
    return func(vm, args[0], args[1], args + 2, argc - 2);
}

static Value native_variadic_dispatch_3(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeVariadic3 func = (ZymNativeVariadic3)func_ptr;
    return func(vm, args[0], args[1], args[2], args + 3, argc - 3);
}

static Value native_variadic_dispatch_4(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeVariadic4 func = (ZymNativeVariadic4)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args + 4, argc - 4);
}

static Value native_variadic_dispatch_5(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeVariadic5 func = (ZymNativeVariadic5)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args + 5, argc - 5);
}

static Value native_variadic_dispatch_6(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeVariadic6 func = (ZymNativeVariadic6)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args + 6, argc - 6);
}

static Value native_variadic_dispatch_7(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeVariadic7 func = (ZymNativeVariadic7)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args + 7, argc - 7);
}

static Value native_variadic_dispatch_8(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeVariadic8 func = (ZymNativeVariadic8)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args + 8, argc - 8);
}

static Value native_variadic_dispatch_9(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeVariadic9 func = (ZymNativeVariadic9)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args + 9, argc - 9);
}

static Value native_variadic_dispatch_10(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeVariadic10 func = (ZymNativeVariadic10)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args + 10, argc - 10);
}

static NativeVariadicDispatcher variadic_dispatchers[MAX_NATIVE_ARITY + 1] = {
    native_variadic_dispatch_0,  native_variadic_dispatch_1,  native_variadic_dispatch_2,
    native_variadic_dispatch_3,  native_variadic_dispatch_4,  native_variadic_dispatch_5,
    native_variadic_dispatch_6,  native_variadic_dispatch_7,  native_variadic_dispatch_8,
    native_variadic_dispatch_9,  native_variadic_dispatch_10,
};

NativeVariadicDispatcher getNativeVariadicDispatcher(int fixed_arity) {
    if (fixed_arity < 0 || fixed_arity > MAX_NATIVE_ARITY) {
        return NULL;
    }
    return variadic_dispatchers[fixed_arity];
}

// Variadic closure trampolines: args[0] = context, args[1..] = call args
// Split into: context, positional fixed args, vargs pointer, vargc

static Value native_variadic_closure_dispatch_0(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeClosureVariadic0 func = (ZymNativeClosureVariadic0)func_ptr;
    return func(vm, args[0], args + 1, argc);
}

static Value native_variadic_closure_dispatch_1(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeClosureVariadic1 func = (ZymNativeClosureVariadic1)func_ptr;
    return func(vm, args[0], args[1], args + 2, argc - 1);
}

static Value native_variadic_closure_dispatch_2(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeClosureVariadic2 func = (ZymNativeClosureVariadic2)func_ptr;
    return func(vm, args[0], args[1], args[2], args + 3, argc - 2);
}

static Value native_variadic_closure_dispatch_3(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeClosureVariadic3 func = (ZymNativeClosureVariadic3)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args + 4, argc - 3);
}

static Value native_variadic_closure_dispatch_4(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeClosureVariadic4 func = (ZymNativeClosureVariadic4)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args + 5, argc - 4);
}

static Value native_variadic_closure_dispatch_5(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeClosureVariadic5 func = (ZymNativeClosureVariadic5)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args + 6, argc - 5);
}

static Value native_variadic_closure_dispatch_6(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeClosureVariadic6 func = (ZymNativeClosureVariadic6)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args + 7, argc - 6);
}

static Value native_variadic_closure_dispatch_7(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeClosureVariadic7 func = (ZymNativeClosureVariadic7)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args + 8, argc - 7);
}

static Value native_variadic_closure_dispatch_8(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeClosureVariadic8 func = (ZymNativeClosureVariadic8)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args + 9, argc - 8);
}

static Value native_variadic_closure_dispatch_9(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeClosureVariadic9 func = (ZymNativeClosureVariadic9)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args + 10, argc - 9);
}

static Value native_variadic_closure_dispatch_10(VM* vm, Value* args, void* func_ptr, int argc) {
    ZymNativeClosureVariadic10 func = (ZymNativeClosureVariadic10)func_ptr;
    return func(vm, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args + 11, argc - 10);
}

static NativeVariadicDispatcher variadic_closure_dispatchers[MAX_NATIVE_ARITY + 1] = {
    native_variadic_closure_dispatch_0,  native_variadic_closure_dispatch_1,  native_variadic_closure_dispatch_2,
    native_variadic_closure_dispatch_3,  native_variadic_closure_dispatch_4,  native_variadic_closure_dispatch_5,
    native_variadic_closure_dispatch_6,  native_variadic_closure_dispatch_7,  native_variadic_closure_dispatch_8,
    native_variadic_closure_dispatch_9,  native_variadic_closure_dispatch_10,
};

NativeVariadicDispatcher getNativeVariadicClosureDispatcher(int fixed_arity) {
    if (fixed_arity < 0 || fixed_arity > MAX_NATIVE_ARITY) {
        return NULL;
    }
    return variadic_closure_dispatchers[fixed_arity];
}

static const char* skipWhitespace(const char* str) {
    while (*str && isspace((unsigned char)*str)) str++;
    return str;
}

static const char* parseIdentifier(const char* str, char* out, int max_len) {
    str = skipWhitespace(str);
    int i = 0;
    while (*str && (isalnum((unsigned char)*str) || *str == '_') && i < max_len - 1) {
        out[i++] = *str++;
    }
    out[i] = '\0';
    return str;
}

bool parseNativeSignatureEx(const char* signature, char* out_name, int* out_arity, bool* out_is_variadic) {
    if (!signature || !out_name || !out_arity) {
        return false;
    }

    if (out_is_variadic) *out_is_variadic = false;

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

    ptr = skipWhitespace(ptr);
    if (*ptr != ')') {
        while (true) {
            if (arity >= MAX_NATIVE_ARITY) {
                fprintf(stderr, "Native function signature parse error: too many parameters (max %d)\n", MAX_NATIVE_ARITY);
                return false;
            }

            ptr = skipWhitespace(ptr);

            // Check for variadic "..."
            if (ptr[0] == '.' && ptr[1] == '.' && ptr[2] == '.') {
                ptr += 3;
                if (out_is_variadic) *out_is_variadic = true;
                // Skip optional rest param name (e.g., "...args")
                char rest_name[256];
                ptr = parseIdentifier(ptr, rest_name, 256);
                ptr = skipWhitespace(ptr);
                if (*ptr != ')') {
                    fprintf(stderr, "Native function signature parse error: '...' must be the last parameter\n");
                    return false;
                }
                break;
            }

            char param_name[256];
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

    return true;
}

bool parseNativeSignature(const char* signature, char* out_name, int* out_arity) {
    return parseNativeSignatureEx(signature, out_name, out_arity, NULL);
}

bool registerNativeFunction(VM* vm, const char* signature, void* func_ptr) {
    char func_name[256];
    int arity;

    if (!parseNativeSignature(signature, func_name, &arity)) {
        return false;
    }

    if (arity > MAX_NATIVE_ARITY) {
        fprintf(stderr, "Native function '%s' has too many parameters (max %d)\n", func_name, MAX_NATIVE_ARITY);
        return false;
    }

    NativeDispatcher dispatcher = getNativeDispatcher(arity);
    if (!dispatcher) {
        fprintf(stderr, "No dispatcher available for arity %d\n", arity);
        return false;
    }

    char mangled_name[256 + 16];
    snprintf(mangled_name, sizeof(mangled_name), "%s@%d", func_name, arity);

    ObjString* name_obj = copyString(vm, mangled_name, (int)strlen(mangled_name));
    pushTempRoot(vm, (Obj*)name_obj);

    ObjNativeFunction* native = newNativeFunction(vm, name_obj, arity, func_ptr, dispatcher);
    pushTempRoot(vm, (Obj*)native);

    tableSet(vm, &vm->globals, name_obj, OBJ_VAL(native));
    popTempRoot(vm);
    popTempRoot(vm);

    return true;
}

bool registerNativeVariadicFunction(VM* vm, const char* signature, void* func_ptr) {
    char func_name[256];
    int fixed_arity;
    bool is_variadic;

    if (!parseNativeSignatureEx(signature, func_name, &fixed_arity, &is_variadic)) {
        return false;
    }

    if (!is_variadic) {
        fprintf(stderr, "Native variadic function '%s' signature must contain '...'\n", func_name);
        return false;
    }

    // Variadic mangling: name@v<fixed_count>
    char mangled_name[256 + 16];
    snprintf(mangled_name, sizeof(mangled_name), "%s@v%d", func_name, fixed_arity);

    ObjString* name_obj = copyString(vm, mangled_name, (int)strlen(mangled_name));
    pushTempRoot(vm, (Obj*)name_obj);

    NativeVariadicDispatcher vdispatcher = getNativeVariadicDispatcher(fixed_arity);
    if (!vdispatcher) {
        fprintf(stderr, "No variadic dispatcher available for fixed arity %d\n", fixed_arity);
        popTempRoot(vm);
        return false;
    }

    ObjNativeFunction* native = newNativeFunction(vm, name_obj, fixed_arity, func_ptr, NULL);
    native->variadic_dispatcher = vdispatcher;
    native->is_variadic = true;
    pushTempRoot(vm, (Obj*)native);

    tableSet(vm, &vm->globals, name_obj, OBJ_VAL(native));
    popTempRoot(vm);
    popTempRoot(vm);

    return true;
}
