#pragma once

#include "../vm.h"
#include "../zym.h"

// Shared functions (work with both lists and strings)
ZymValue nativeShared_length(ZymVM* vm, ZymValue value);
ZymValue nativeShared_concat(ZymVM* vm, ZymValue val1, ZymValue val2);
ZymValue nativeShared_indexOf(ZymVM* vm, ZymValue haystack, ZymValue needle);
ZymValue nativeShared_contains(ZymVM* vm, ZymValue haystack, ZymValue needle);
ZymValue nativeShared_slice(ZymVM* vm, ZymValue value, ZymValue startVal, ZymValue endVal);

// Register shared natives into the VM
void registerSharedNatives(VM* vm);
