#pragma once

#include "../vm.h"
#include "../zym.h"

// Map manipulation native functions
ZymValue nativeMap_size(ZymVM* vm, ZymValue map);
ZymValue nativeMap_isEmpty(ZymVM* vm, ZymValue map);
ZymValue nativeMap_keys(ZymVM* vm, ZymValue map);
ZymValue nativeMap_values(ZymVM* vm, ZymValue map);
ZymValue nativeMap_entries(ZymVM* vm, ZymValue map);
ZymValue nativeMap_clear(ZymVM* vm, ZymValue map);
ZymValue nativeMap_merge(ZymVM* vm, ZymValue targetMap, ZymValue sourceMap);

// Register map natives into the VM
void registerMapNatives(VM* vm);
