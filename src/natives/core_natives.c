#include "core_natives.h"
#include "gc_native.h"
#include "conversions.h"
#include "error.h"

// ============================================================================
// Core Natives Registration
// ============================================================================

void setupCoreNatives(VM* vm) {
    registerGCModule(vm);
    registerConversionsNatives(vm);
    registerErrorNatives(vm);
}
