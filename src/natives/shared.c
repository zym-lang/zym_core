#include <stdio.h>
#include <string.h>
#include "shared.h"
#include "../utf8.h"

// =============================================================================
// SHARED FUNCTIONS (work with both lists and strings)
// =============================================================================

// Helper function to compare two ZymValues for equality
static bool valuesEqual(ZymVM* vm, ZymValue a, ZymValue b) {
    // Check if same type
    if (zym_isNull(a) && zym_isNull(b)) return true;
    if (zym_isBool(a) && zym_isBool(b)) return zym_asBool(a) == zym_asBool(b);
    if (zym_isNumber(a) && zym_isNumber(b)) return zym_asNumber(a) == zym_asNumber(b);
    if (zym_isString(a) && zym_isString(b)) return strcmp(zym_asCString(a), zym_asCString(b)) == 0;
    return a == b;
}

// Get length of list or string
ZymValue nativeShared_length(ZymVM* vm, ZymValue value) {
    if (zym_isList(value)) {
        return zym_newNumber((double)zym_listLength(value));
    } else if (zym_isString(value)) {
        int length;
        zym_toString(value, NULL, &length);
        return zym_newNumber((double)length);
    } else {
        zym_runtimeError(vm, "length() requires a list or string");
        return ZYM_ERROR;
    }
}

// Concatenate two lists or two strings
ZymValue nativeShared_concat(ZymVM* vm, ZymValue val1, ZymValue val2) {
    // Both must be lists
    if (zym_isList(val1) && zym_isList(val2)) {
        ZymValue newList = zym_newList(vm);
        zym_pushRoot(vm, newList);

        // Copy first list
        int len1 = zym_listLength(val1);
        for (int i = 0; i < len1; i++) {
            ZymValue item = zym_listGet(vm, val1, i);
            if (!zym_listAppend(vm, newList, item)) {
                zym_popRoot(vm);
                zym_runtimeError(vm, "concat() failed to append value from first list");
                return ZYM_ERROR;
            }
        }

        // Copy second list
        int len2 = zym_listLength(val2);
        for (int i = 0; i < len2; i++) {
            ZymValue item = zym_listGet(vm, val2, i);
            if (!zym_listAppend(vm, newList, item)) {
                zym_popRoot(vm);
                zym_runtimeError(vm, "concat() failed to append value from second list");
                return ZYM_ERROR;
            }
        }

        zym_popRoot(vm);
        return newList;
    }
    // Both must be strings
    else if (zym_isString(val1) && zym_isString(val2)) {
        const char* cstr1 = zym_asCString(val1);
        const char* cstr2 = zym_asCString(val2);

        char buffer[4096];
        int len1 = strlen(cstr1);
        int len2 = strlen(cstr2);

        if (len1 + len2 >= 4095) {
            zym_runtimeError(vm, "concat() result string too long");
            return ZYM_ERROR;
        }

        strcpy(buffer, cstr1);
        strcat(buffer, cstr2);

        return zym_newString(vm, buffer);
    } else {
        zym_runtimeError(vm, "concat() requires both arguments to be lists or both to be strings");
        return ZYM_ERROR;
    }
}

// Find index of value in list or substring in string (-1 if not found)
ZymValue nativeShared_indexOf(ZymVM* vm, ZymValue haystack, ZymValue needle) {
    if (zym_isList(haystack)) {
        int len = zym_listLength(haystack);
        for (int i = 0; i < len; i++) {
            ZymValue item = zym_listGet(vm, haystack, i);
            if (valuesEqual(vm, item, needle)) {
                return zym_newNumber((double)i);
            }
        }
        return zym_newNumber(-1.0);
    } else if (zym_isString(haystack)) {
        if (!zym_isString(needle)) {
            zym_runtimeError(vm, "indexOf() requires second argument to be a string when first is a string");
            return ZYM_ERROR;
        }

        const char* cstr = zym_asCString(haystack);
        const char* search = zym_asCString(needle);

        const char* found = strstr(cstr, search);
        if (found == NULL) {
            return zym_newNumber(-1.0);
        }

        return zym_newNumber((double)(found - cstr));
    } else {
        zym_runtimeError(vm, "indexOf() requires a list or string as first argument");
        return ZYM_ERROR;
    }
}

// Check if list contains value or string contains substring
ZymValue nativeShared_contains(ZymVM* vm, ZymValue haystack, ZymValue needle) {
    if (zym_isList(haystack)) {
        int len = zym_listLength(haystack);
        for (int i = 0; i < len; i++) {
            ZymValue item = zym_listGet(vm, haystack, i);
            if (valuesEqual(vm, item, needle)) {
                return zym_newBool(true);
            }
        }
        return zym_newBool(false);
    } else if (zym_isString(haystack)) {
        if (!zym_isString(needle)) {
            zym_runtimeError(vm, "contains() requires second argument to be a string when first is a string");
            return ZYM_ERROR;
        }

        const char* cstr = zym_asCString(haystack);
        const char* search = zym_asCString(needle);

        return zym_newBool(strstr(cstr, search) != NULL);
    } else {
        zym_runtimeError(vm, "contains() requires a list or string as first argument");
        return ZYM_ERROR;
    }
}

// Create a slice of list or substring [start, end)
ZymValue nativeShared_slice(ZymVM* vm, ZymValue value, ZymValue startVal, ZymValue endVal) {
    if (!zym_isNumber(startVal)) {
        zym_runtimeError(vm, "slice() requires a number as second argument (start)");
        return ZYM_ERROR;
    }

    if (!zym_isNumber(endVal)) {
        zym_runtimeError(vm, "slice() requires a number as third argument (end)");
        return ZYM_ERROR;
    }

    int start = (int)zym_asNumber(startVal);
    int end = (int)zym_asNumber(endVal);

    if (zym_isList(value)) {
        int len = zym_listLength(value);

        // Handle negative indices
        if (start < 0) start = len + start;
        if (end < 0) end = len + end;

        // Clamp to valid range
        if (start < 0) start = 0;
        if (end > len) end = len;
        if (start > end) start = end;

        ZymValue newList = zym_newList(vm);
        zym_pushRoot(vm, newList);

        for (int i = start; i < end; i++) {
            ZymValue item = zym_listGet(vm, value, i);
            if (!zym_listAppend(vm, newList, item)) {
                zym_popRoot(vm);
                zym_runtimeError(vm, "slice() failed to append value");
                return ZYM_ERROR;
            }
        }

        zym_popRoot(vm);
        return newList;
    } else if (zym_isString(value)) {
        const char* cstr;
        int char_len;
        int byte_len;
        zym_toString(value, &cstr, &char_len);
        zym_toStringBytes(value, NULL, &byte_len);

        // Handle negative indices (character-based)
        if (start < 0) start = char_len + start;
        if (end < 0) end = char_len + end;

        // Clamp to valid range
        if (start < 0) start = 0;
        if (end > char_len) end = char_len;
        if (start > end) start = end;

        // Convert character indices to byte offsets
        int start_byte, end_byte;
        if (!utf8_substring(cstr, byte_len, start, end, &start_byte, &end_byte)) {
            zym_runtimeError(vm, "slice() character indices out of range");
            return ZYM_ERROR;
        }

        int subLen = end_byte - start_byte;
        if (subLen >= 4095) {
            zym_runtimeError(vm, "slice() result string too long");
            return ZYM_ERROR;
        }

        char buffer[4096];
        memcpy(buffer, cstr + start_byte, subLen);
        buffer[subLen] = '\0';

        return zym_newString(vm, buffer);
    } else {
        zym_runtimeError(vm, "slice() requires a list or string as first argument");
        return ZYM_ERROR;
    }
}

// =============================================================================
// Registration
// =============================================================================

void registerSharedNatives(VM* vm) {
    zym_defineNative(vm, "length(value)", nativeShared_length);
    zym_defineNative(vm, "concat(a, b)", nativeShared_concat);
    zym_defineNative(vm, "indexOf(haystack, needle)", nativeShared_indexOf);
    zym_defineNative(vm, "contains(haystack, needle)", nativeShared_contains);
    zym_defineNative(vm, "slice(value, start, end)", nativeShared_slice);
}
