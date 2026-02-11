#include "core_natives.h"
#include "gc_native.h"

// ============================================================================
// Core Natives Registration
// ============================================================================

void setupCoreNatives(VM* vm) {
    registerGCModule(vm);
}
