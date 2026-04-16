#pragma once

#include "../vm.h"
#include "zym/zym.h"

// Conversion functions
ZymValue nativeConversions_num(ZymVM* vm, ZymValue value);
ZymValue nativeConversions_str(ZymVM* vm, ZymValue value);
ZymValue nativeConversions_str_variadic(ZymVM* vm, ZymValue format, ZymValue* vargs, int vargc);

void registerConversionsNatives(VM* vm);
