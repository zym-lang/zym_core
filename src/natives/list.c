#include <stdio.h>
#include <string.h>
#include "list.h"

// =============================================================================
// LIST MANIPULATION FUNCTIONS
// =============================================================================

// Push value to end of list
ZymValue nativeList_push(ZymVM* vm, ZymValue list, ZymValue value) {
    if (!zym_isList(list)) {
        zym_runtimeError(vm, "push() requires a list as first argument");
        return ZYM_ERROR;
    }

    if (!zym_listAppend(vm, list, value)) {
        zym_runtimeError(vm, "push() failed to append value");
        return ZYM_ERROR;
    }

    return zym_newNull();
}

// Pop value from end of list
ZymValue nativeList_pop(ZymVM* vm, ZymValue list) {
    if (!zym_isList(list)) {
        zym_runtimeError(vm, "pop() requires a list");
        return ZYM_ERROR;
    }

    int len = zym_listLength(list);
    if (len == 0) {
        zym_runtimeError(vm, "pop() called on empty list");
        return ZYM_ERROR;
    }

    ZymValue value = zym_listGet(vm, list, len - 1);
    if (!zym_listRemove(vm, list, len - 1)) {
        zym_runtimeError(vm, "pop() failed to remove value");
        return ZYM_ERROR;
    }

    return value;
}

// Remove and return first element
ZymValue nativeList_shift(ZymVM* vm, ZymValue list) {
    if (!zym_isList(list)) {
        zym_runtimeError(vm, "shift() requires a list");
        return ZYM_ERROR;
    }

    int len = zym_listLength(list);
    if (len == 0) {
        zym_runtimeError(vm, "shift() called on empty list");
        return ZYM_ERROR;
    }

    ZymValue value = zym_listGet(vm, list, 0);
    if (!zym_listRemove(vm, list, 0)) {
        zym_runtimeError(vm, "shift() failed to remove value");
        return ZYM_ERROR;
    }

    return value;
}

// Add value to start of list
ZymValue nativeList_unshift(ZymVM* vm, ZymValue list, ZymValue value) {
    if (!zym_isList(list)) {
        zym_runtimeError(vm, "unshift() requires a list as first argument");
        return ZYM_ERROR;
    }

    if (!zym_listInsert(vm, list, 0, value)) {
        zym_runtimeError(vm, "unshift() failed to insert value");
        return ZYM_ERROR;
    }

    return zym_newNull();
}

// Insert value at specific index
ZymValue nativeList_insert(ZymVM* vm, ZymValue list, ZymValue indexVal, ZymValue value) {
    if (!zym_isList(list)) {
        zym_runtimeError(vm, "insert() requires a list as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isNumber(indexVal)) {
        zym_runtimeError(vm, "insert() requires a number as second argument");
        return ZYM_ERROR;
    }

    int index = (int)zym_asNumber(indexVal);
    int len = zym_listLength(list);

    // Handle negative indices (count from end)
    if (index < 0) {
        index = len + index + 1;
    }

    // Clamp to valid range [0, len]
    if (index < 0) index = 0;
    if (index > len) index = len;

    if (!zym_listInsert(vm, list, index, value)) {
        zym_runtimeError(vm, "insert() failed to insert value");
        return ZYM_ERROR;
    }

    return zym_newNull();
}

// Remove value at specific index
ZymValue nativeList_remove(ZymVM* vm, ZymValue list, ZymValue indexVal) {
    if (!zym_isList(list)) {
        zym_runtimeError(vm, "remove() requires a list as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isNumber(indexVal)) {
        zym_runtimeError(vm, "remove() requires a number as second argument");
        return ZYM_ERROR;
    }

    int index = (int)zym_asNumber(indexVal);
    int len = zym_listLength(list);

    // Handle negative indices (count from end)
    if (index < 0) {
        index = len + index;
    }

    if (index < 0 || index >= len) {
        zym_runtimeError(vm, "remove() index %d out of bounds (0-%d)", index, len - 1);
        return ZYM_ERROR;
    }

    ZymValue value = zym_listGet(vm, list, index);
    if (!zym_listRemove(vm, list, index)) {
        zym_runtimeError(vm, "remove() failed to remove value");
        return ZYM_ERROR;
    }

    return value;
}

// Reverse list in place
ZymValue nativeList_reverse(ZymVM* vm, ZymValue list) {
    if (!zym_isList(list)) {
        zym_runtimeError(vm, "reverse() requires a list");
        return ZYM_ERROR;
    }

    int len = zym_listLength(list);
    for (int i = 0; i < len / 2; i++) {
        ZymValue temp = zym_listGet(vm, list, i);
        ZymValue opposite = zym_listGet(vm, list, len - 1 - i);

        if (!zym_listSet(vm, list, i, opposite)) {
            zym_runtimeError(vm, "reverse() failed to set value at index %d", i);
            return ZYM_ERROR;
        }
        if (!zym_listSet(vm, list, len - 1 - i, temp)) {
            zym_runtimeError(vm, "reverse() failed to set value at index %d", len - 1 - i);
            return ZYM_ERROR;
        }
    }

    return zym_newNull();
}

// =============================================================================
// Registration
// =============================================================================

void registerListNatives(VM* vm) {
    zym_defineNative(vm, "push(list, value)", nativeList_push);
    zym_defineNative(vm, "pop(list)", nativeList_pop);
    zym_defineNative(vm, "shift(list)", nativeList_shift);
    zym_defineNative(vm, "unshift(list, value)", nativeList_unshift);
    zym_defineNative(vm, "insert(list, index, value)", nativeList_insert);
    zym_defineNative(vm, "remove(list, index)", nativeList_remove);
    zym_defineNative(vm, "reverse(list)", nativeList_reverse);
}
