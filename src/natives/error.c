#include <stdio.h>
#include <string.h>
#include "error.h"

// =============================================================================
// ERROR HANDLING FUNCTIONS
// =============================================================================

// Trigger a runtime error with a custom message
ZymValue nativeError_error(ZymVM* vm, ZymValue message) {
    if (!zym_isString(message)) {
        zym_runtimeError(vm, "error() requires a string argument");
        return ZYM_ERROR;
    }

    const char* msg = zym_asCString(message);
    zym_runtimeError(vm, msg);
    return ZYM_ERROR;
}

// Assert a condition is true, otherwise trigger runtime error
ZymValue nativeError_assert(ZymVM* vm, ZymValue condition, ZymValue message) {
    // Check if condition is truthy
    bool conditionMet = false;

    if (zym_isBool(condition)) {
        conditionMet = zym_asBool(condition);
    } else if (zym_isNull(condition)) {
        conditionMet = false;
    } else if (zym_isNumber(condition)) {
        // 0 is falsy, everything else is truthy
        conditionMet = zym_asNumber(condition) != 0.0;
    } else {
        // All other types (strings, lists, maps, etc.) are truthy
        conditionMet = true;
    }

    // If condition is not met, trigger error
    if (!conditionMet) {
        if (zym_isString(message)) {
            const char* msg = zym_asCString(message);
            zym_runtimeError(vm, msg);
        } else {
            zym_runtimeError(vm, "Assertion failed");
        }
        return ZYM_ERROR;
    }

    return zym_newNull();
}

// =============================================================================
// Registration
// =============================================================================

void registerErrorNatives(VM* vm) {
    zym_defineNative(vm, "error(message)", nativeError_error);
    zym_defineNative(vm, "assert(condition, message)", nativeError_assert);
}
