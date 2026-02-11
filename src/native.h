#pragma once

#include "./vm.h"
#include "./value.h"
#include "./object.h"

#define MAX_NATIVE_ARITY 26

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
typedef Value (*ZymNative11)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k);
typedef Value (*ZymNative12)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l);
typedef Value (*ZymNative13)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m);
typedef Value (*ZymNative14)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n);
typedef Value (*ZymNative15)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o);
typedef Value (*ZymNative16)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p);
typedef Value (*ZymNative17)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q);
typedef Value (*ZymNative18)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r);
typedef Value (*ZymNative19)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s);
typedef Value (*ZymNative20)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t);
typedef Value (*ZymNative21)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u);
typedef Value (*ZymNative22)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u, Value v);
typedef Value (*ZymNative23)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u, Value v, Value w);
typedef Value (*ZymNative24)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u, Value v, Value w, Value x);
typedef Value (*ZymNative25)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u, Value v, Value w, Value x, Value y);
typedef Value (*ZymNative26)(VM* vm, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u, Value v, Value w, Value x, Value y, Value z);

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
typedef Value (*ZymNativeClosure11)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k);
typedef Value (*ZymNativeClosure12)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l);
typedef Value (*ZymNativeClosure13)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m);
typedef Value (*ZymNativeClosure14)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n);
typedef Value (*ZymNativeClosure15)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o);
typedef Value (*ZymNativeClosure16)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p);
typedef Value (*ZymNativeClosure17)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q);
typedef Value (*ZymNativeClosure18)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r);
typedef Value (*ZymNativeClosure19)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s);
typedef Value (*ZymNativeClosure20)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t);
typedef Value (*ZymNativeClosure21)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u);
typedef Value (*ZymNativeClosure22)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u, Value v);
typedef Value (*ZymNativeClosure23)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u, Value v, Value w);
typedef Value (*ZymNativeClosure24)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u, Value v, Value w, Value x);
typedef Value (*ZymNativeClosure25)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u, Value v, Value w, Value x, Value y);
typedef Value (*ZymNativeClosure26)(VM* vm, Value context, Value a, Value b, Value c, Value d, Value e, Value f, Value g, Value h, Value i, Value j, Value k, Value l, Value m, Value n, Value o, Value p, Value q, Value r, Value s, Value t, Value u, Value v, Value w, Value x, Value y, Value z);

bool parseNativeSignature(const char* signature, char* out_name, int* out_arity, uint8_t** out_qualifiers);
NativeDispatcher getNativeDispatcher(int arity);
NativeDispatcher getNativeClosureDispatcher(int arity);
bool registerNativeFunction(VM* vm, const char* signature, void* func_ptr);
