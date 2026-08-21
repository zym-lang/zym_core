#pragma once

#include "../vm.h"
#include "../value.h"
#include "zym/zym.h"

ZymValue nativeFiber_create(ZymVM* vm);
void registerFiberModule(VM* vm);
