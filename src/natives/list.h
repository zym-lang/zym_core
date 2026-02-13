#pragma once

#include "../vm.h"
#include "../zym.h"

// List manipulation native functions
ZymValue nativeList_push(ZymVM* vm, ZymValue list, ZymValue value);
ZymValue nativeList_pop(ZymVM* vm, ZymValue list);
ZymValue nativeList_shift(ZymVM* vm, ZymValue list);
ZymValue nativeList_unshift(ZymVM* vm, ZymValue list, ZymValue value);
ZymValue nativeList_insert(ZymVM* vm, ZymValue list, ZymValue indexVal, ZymValue value);
ZymValue nativeList_remove(ZymVM* vm, ZymValue list, ZymValue indexVal);
ZymValue nativeList_reverse(ZymVM* vm, ZymValue list);
ZymValue nativeList_sort(ZymVM* vm, ZymValue list);
ZymValue nativeList_join(ZymVM* vm, ZymValue list, ZymValue separator);

// Register list natives into the VM
void registerListNatives(VM* vm);
