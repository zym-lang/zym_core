#pragma once

#include "../vm.h"
#include "../zym.h"

// Conversion functions
ZymValue nativeConversions_num(ZymVM* vm, ZymValue value);
ZymValue nativeConversions_str(ZymVM* vm, ZymValue value);
ZymValue nativeConversions_str_02(ZymVM* vm, ZymValue format, ZymValue a);
ZymValue nativeConversions_str_03(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b);
ZymValue nativeConversions_str_04(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c);
ZymValue nativeConversions_str_05(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d);
ZymValue nativeConversions_str_06(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e);
ZymValue nativeConversions_str_07(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f);
ZymValue nativeConversions_str_08(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g);
ZymValue nativeConversions_str_09(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h);
ZymValue nativeConversions_str_10(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i);
ZymValue nativeConversions_str_11(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j);
ZymValue nativeConversions_str_12(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k);
ZymValue nativeConversions_str_13(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l);
ZymValue nativeConversions_str_14(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m);
ZymValue nativeConversions_str_15(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n);
ZymValue nativeConversions_str_16(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o);
ZymValue nativeConversions_str_17(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p);
ZymValue nativeConversions_str_18(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q);
ZymValue nativeConversions_str_19(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r);
ZymValue nativeConversions_str_20(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s);
ZymValue nativeConversions_str_21(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t);
ZymValue nativeConversions_str_22(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t, ZymValue u);
ZymValue nativeConversions_str_23(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t, ZymValue u, ZymValue v);
ZymValue nativeConversions_str_24(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t, ZymValue u, ZymValue v, ZymValue w);
ZymValue nativeConversions_str_25(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t, ZymValue u, ZymValue v, ZymValue w, ZymValue x);
ZymValue nativeConversions_str_26(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t, ZymValue u, ZymValue v, ZymValue w, ZymValue x, ZymValue y);

void registerConversionsNatives(VM* vm);
