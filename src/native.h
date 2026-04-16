#pragma once

#include "./vm.h"
#include "./value.h"
#include "./object.h"

#define MAX_NATIVE_ARITY 10

typedef Value (*ZymNative0)(VM* vm);
typedef Value (*ZymNative1)(VM* vm, Value a);
typedef Value (*ZymNative2)(VM* vm, Value a, Value b);
typedef Value (*ZymNative3)(VM* vm, Value a, Value b, Value c);
typedef Value (*ZymNative4)(VM* vm, Value a, Value b, Value c, Value d);
typedef Value (*ZymNative5)(VM* vm, Value a, Value b, Value c, Value d, Value e);
typedef Value (*ZymNative6)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f);
typedef Value (*ZymNative7)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g);
typedef Value (*ZymNative8)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h);
typedef Value (*ZymNative9)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i);
typedef Value (*ZymNative10)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j);

typedef Value (*ZymNativeClosure0)(VM* vm, Value context);
typedef Value (*ZymNativeClosure1)(VM* vm, Value context, Value a);
typedef Value (*ZymNativeClosure2)(VM* vm, Value context, Value a, Value b);
typedef Value (*ZymNativeClosure3)(VM* vm, Value context, Value a, Value b, Value c);
typedef Value (*ZymNativeClosure4)(VM* vm, Value context, Value a, Value b, Value c, Value d);
typedef Value (*ZymNativeClosure5)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e);
typedef Value (*ZymNativeClosure6)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f);
typedef Value (*ZymNativeClosure7)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g);
typedef Value (*ZymNativeClosure8)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h);
typedef Value (*ZymNativeClosure9)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i);
typedef Value (*ZymNativeClosure10)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j);

// Variadic native function typedefs: fn(vm, [fixed args...], vargs, vargc)
typedef Value (*ZymNativeVariadic0)(VM* vm, Value* vargs, int vargc);
typedef Value (*ZymNativeVariadic1)(VM* vm, Value a, Value* vargs, int vargc);
typedef Value (*ZymNativeVariadic2)(VM* vm, Value a, Value b, Value* vargs, int vargc);
typedef Value (*ZymNativeVariadic3)(VM* vm, Value a, Value b, Value c, Value* vargs, int vargc);
typedef Value (*ZymNativeVariadic4)(VM* vm, Value a, Value b, Value c, Value d, Value* vargs, int vargc);
typedef Value (*ZymNativeVariadic5)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value* vargs, int vargc);
typedef Value (*ZymNativeVariadic6)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value* vargs, int vargc);
typedef Value (*ZymNativeVariadic7)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value* vargs, int vargc);
typedef Value (*ZymNativeVariadic8)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value* vargs, int vargc);
typedef Value (*ZymNativeVariadic9)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value* vargs, int vargc);
typedef Value (*ZymNativeVariadic10)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value* vargs, int vargc);

// Variadic native closure typedefs: fn(vm, context, [fixed args...], vargs, vargc)
typedef Value (*ZymNativeClosureVariadic0)(VM* vm, Value context, Value* vargs, int vargc);
typedef Value (*ZymNativeClosureVariadic1)(VM* vm, Value context, Value a, Value* vargs, int vargc);
typedef Value (*ZymNativeClosureVariadic2)(VM* vm, Value context, Value a, Value b, Value* vargs, int vargc);
typedef Value (*ZymNativeClosureVariadic3)(VM* vm, Value context, Value a, Value b, Value c, Value* vargs, int vargc);
typedef Value (*ZymNativeClosureVariadic4)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value* vargs, int vargc);
typedef Value (*ZymNativeClosureVariadic5)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value* vargs, int vargc);
typedef Value (*ZymNativeClosureVariadic6)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value* vargs, int vargc);
typedef Value (*ZymNativeClosureVariadic7)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value* vargs, int vargc);
typedef Value (*ZymNativeClosureVariadic8)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value* vargs, int vargc);
typedef Value (*ZymNativeClosureVariadic9)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value* vargs, int vargc);
typedef Value (*ZymNativeClosureVariadic10)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value* vargs, int vargc);

bool parseNativeSignature(const char* signature, char* out_name, int* out_arity);
bool parseNativeSignatureEx(const char* signature, char* out_name, int* out_arity, bool* out_is_variadic);
NativeDispatcher getNativeDispatcher(int arity);
NativeDispatcher getNativeClosureDispatcher(int arity);
NativeVariadicDispatcher getNativeVariadicDispatcher(int fixed_arity);
NativeVariadicDispatcher getNativeVariadicClosureDispatcher(int fixed_arity);
bool registerNativeFunction(VM* vm, const char* signature, void* func_ptr);
bool registerNativeVariadicFunction(VM* vm, const char* signature, void* func_ptr);
