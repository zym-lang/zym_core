#include "core_modules.h"
#include "continuation.h"
#include "preemption.h"
#include "../natives/core_natives.h"

// ============================================================================
// Core Modules Registration
// ============================================================================

void setupCoreModules(VM* vm) {
    registerContinuationModule(vm);
    registerPreemptionModule(vm);
    setupCoreNatives(vm);
}
