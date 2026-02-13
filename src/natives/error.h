#pragma once

#include "../vm.h"
#include "../zym.h"

// Error handling native functions
ZymValue nativeError_error(ZymVM* vm, ZymValue message);
ZymValue nativeError_assert(ZymVM* vm, ZymValue condition, ZymValue message);

// Register error handling natives into the VM
void registerErrorNatives(VM* vm);
