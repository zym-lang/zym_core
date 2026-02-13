#pragma once

#include "../vm.h"
#include "../zym.h"

// String manipulation native functions
ZymValue nativeString_charAt(ZymVM* vm, ZymValue str, ZymValue indexVal);
ZymValue nativeString_charCodeAt(ZymVM* vm, ZymValue str, ZymValue indexVal);
ZymValue nativeString_byteLength(ZymVM* vm, ZymValue str);
ZymValue nativeString_fromCodePoint(ZymVM* vm, ZymValue codePointVal);
ZymValue nativeString_startsWith(ZymVM* vm, ZymValue str, ZymValue prefixStr);
ZymValue nativeString_endsWith(ZymVM* vm, ZymValue str, ZymValue suffixStr);
ZymValue nativeString_lastIndexOf(ZymVM* vm, ZymValue str, ZymValue searchStr);
ZymValue nativeString_toUpperCase(ZymVM* vm, ZymValue str);
ZymValue nativeString_toLowerCase(ZymVM* vm, ZymValue str);
ZymValue nativeString_trim(ZymVM* vm, ZymValue str);
ZymValue nativeString_trimStart(ZymVM* vm, ZymValue str);
ZymValue nativeString_trimEnd(ZymVM* vm, ZymValue str);
ZymValue nativeString_replace(ZymVM* vm, ZymValue str, ZymValue searchStr, ZymValue replaceStr);
ZymValue nativeString_replaceAll(ZymVM* vm, ZymValue str, ZymValue searchStr, ZymValue replaceStr);
ZymValue nativeString_split(ZymVM* vm, ZymValue str, ZymValue delimiterStr);
ZymValue nativeString_repeat(ZymVM* vm, ZymValue str, ZymValue countVal);
ZymValue nativeString_padStart(ZymVM* vm, ZymValue str, ZymValue targetLenVal, ZymValue padStr);
ZymValue nativeString_padEnd(ZymVM* vm, ZymValue str, ZymValue targetLenVal, ZymValue padStr);
ZymValue nativeString_substr(ZymVM* vm, ZymValue str, ZymValue startVal, ZymValue endVal);

// Register string natives into the VM
void registerStringNatives(VM* vm);
