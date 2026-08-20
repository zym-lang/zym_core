#include "core_modules.h"
#include "preemption.h"
#include "../natives/core_natives.h"

// ============================================================================
// Core Modules Registration
// ============================================================================

void setupCoreModules(VM* vm) {
    setupCoreNatives(vm);
}
