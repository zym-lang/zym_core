#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "../object.h"

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

// =============================================================================
// SORT HELPERS
// =============================================================================

// Thread-local VM pointer for qsort comparison (non-reentrant by design)
static VM* sort_vm = NULL;

// Compare two Values for sorting: numbers < strings < other types
// Within numbers: numeric order. Within strings: lexicographic.
static int compareValues(const void* a, const void* b) {
    Value va = *(const Value*)a;
    Value vb = *(const Value*)b;

    bool aNum = IS_DOUBLE(va);
    bool bNum = IS_DOUBLE(vb);
    bool aStr = !aNum && IS_OBJ(va) && IS_STRING(va);
    bool bStr = !bNum && IS_OBJ(vb) && IS_STRING(vb);

    // Both numbers
    if (aNum && bNum) {
        double da = AS_DOUBLE(va);
        double db = AS_DOUBLE(vb);
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }

    // Both strings
    if (aStr && bStr) {
        ObjString* sa = AS_STRING(va);
        ObjString* sb = AS_STRING(vb);
        int minLen = sa->length < sb->length ? sa->length : sb->length;
        int cmp = memcmp(sa->chars, sb->chars, minLen);
        if (cmp != 0) return cmp;
        return sa->length - sb->length;
    }

    // Numbers before strings, strings before everything else
    if (aNum && !bNum) return -1;
    if (!aNum && bNum) return 1;
    if (aStr && !bStr) return -1;
    if (!aStr && bStr) return 1;

    // Both are non-number, non-string: preserve relative order (stable-ish)
    return 0;
}

// Sort list in place (non-reentrant, no higher-order comparator)
ZymValue nativeList_sort(ZymVM* vm, ZymValue list) {
    if (!zym_isList(list)) {
        zym_runtimeError(vm, "sort() requires a list");
        return ZYM_ERROR;
    }

    ObjList* objList = AS_LIST(list);
    int count = objList->items.count;
    if (count <= 1) return zym_newNull();

    sort_vm = vm;
    qsort(objList->items.values, count, sizeof(Value), compareValues);
    sort_vm = NULL;

    return zym_newNull();
}

// =============================================================================
// JOIN
// =============================================================================

// Join list elements into a string with a separator
ZymValue nativeList_join(ZymVM* vm, ZymValue list, ZymValue sepVal) {
    if (!zym_isList(list)) {
        zym_runtimeError(vm, "join() requires a list as first argument");
        return ZYM_ERROR;
    }
    if (!zym_isString(sepVal)) {
        zym_runtimeError(vm, "join() requires a string separator");
        return ZYM_ERROR;
    }

    int len = zym_listLength(list);
    if (len == 0) return zym_newString(vm, "");

    const char* sep = zym_asCString(sepVal);
    int sepLen = zym_stringByteLength(sepVal);

    // First pass: convert all elements to strings and measure total size
    // We'll store converted string values on the root stack for GC safety
    int totalBytes = 0;
    for (int i = 0; i < len; i++) {
        ZymValue elem = zym_listGet(vm, list, i);
        ZymValue strVal = zym_valueToString(vm, elem);
        zym_pushRoot(vm, strVal);
        totalBytes += zym_stringByteLength(strVal);
    }
    // Add separator bytes
    totalBytes += sepLen * (len - 1);

    // Second pass: build the result
    char* buf = malloc(totalBytes + 1);
    if (!buf) {
        for (int i = 0; i < len; i++) zym_popRoot(vm);
        zym_runtimeError(vm, "join() out of memory");
        return ZYM_ERROR;
    }

    int offset = 0;
    for (int i = 0; i < len; i++) {
        // Roots are in order: index 0 is deepest, index len-1 is top
        ZymValue strVal = zym_peekRoot(vm, len - 1 - i);
        const char* s = zym_asCString(strVal);
        int sLen = zym_stringByteLength(strVal);
        memcpy(buf + offset, s, sLen);
        offset += sLen;
        if (i < len - 1) {
            memcpy(buf + offset, sep, sepLen);
            offset += sepLen;
        }
    }
    buf[offset] = '\0';

    // Pop all roots
    for (int i = 0; i < len; i++) zym_popRoot(vm);

    ZymValue result = zym_newStringN(vm, buf, totalBytes);
    free(buf);
    return result;
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
    zym_defineNative(vm, "sort(list)", nativeList_sort);
    zym_defineNative(vm, "join(list, separator)", nativeList_join);
}
