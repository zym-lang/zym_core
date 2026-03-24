#pragma once

#include "../vm.h"
#include "zym/zym.h"

ZymValue nativeGC_create(ZymVM* vm);
void registerGCModule(VM* vm);
