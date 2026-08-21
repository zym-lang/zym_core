#include "core_modules.h"
#include "fiber.h"
#include "../natives/core_natives.h"

// ============================================================================
// Core Modules Registration
// ============================================================================

void setupCoreModules(VM* vm) {
    registerFiberModule(vm);
    setupCoreNatives(vm);
}
