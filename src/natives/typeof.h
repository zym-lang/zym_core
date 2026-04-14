#pragma once

#include "../vm.h"
#include "zym/zym.h"

// Returns the type of a value as a string
ZymValue nativeTypeof(ZymVM* vm, ZymValue value);

// Register typeof native into the VM
void registerTypeofNative(VM* vm);
