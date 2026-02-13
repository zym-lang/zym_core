#include "core_natives.h"
#include "gc_native.h"
#include "conversions.h"
#include "error.h"
#include "list.h"
#include "map.h"
#include "shared.h"
#include "string_natives.h"
#include "math.h"

// ============================================================================
// Core Natives Registration
// ============================================================================

void setupCoreNatives(VM* vm) {
    registerGCModule(vm);
    registerConversionsNatives(vm);
    registerErrorNatives(vm);
    registerSharedNatives(vm);
    registerListNatives(vm);
    registerMapNatives(vm);
    registerStringNatives(vm);
    registerMathNatives(vm);
}
