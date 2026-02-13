#include <stdio.h>
#include <string.h>
#include "map.h"

// =============================================================================
// MAP MANIPULATION FUNCTIONS
// =============================================================================

// Get map size
ZymValue nativeMap_size(ZymVM* vm, ZymValue map) {
    if (!zym_isMap(map)) {
        zym_runtimeError(vm, "size() requires a map");
        return ZYM_ERROR;
    }

    return zym_newNumber((double)zym_mapSize(map));
}

// Check if map is empty
ZymValue nativeMap_isEmpty(ZymVM* vm, ZymValue map) {
    if (!zym_isMap(map)) {
        zym_runtimeError(vm, "isEmpty() requires a map");
        return ZYM_ERROR;
    }

    return zym_newBool(zym_mapSize(map) == 0);
}

// Helper structure for collecting keys
typedef struct {
    ZymVM* vm;
    ZymValue list;
    bool failed;
} KeyCollector;

// Callback for collecting keys
static bool collectKeys(ZymVM* vm, const char* key, ZymValue val, void* userdata) {
    KeyCollector* collector = (KeyCollector*)userdata;
    ZymValue keyStr = zym_newString(vm, key);
    if (!zym_listAppend(vm, collector->list, keyStr)) {
        collector->failed = true;
        return false; // Stop iteration
    }
    return true; // Continue iteration
}

// Get list of all keys in map
ZymValue nativeMap_keys(ZymVM* vm, ZymValue map) {
    if (!zym_isMap(map)) {
        zym_runtimeError(vm, "keys() requires a map");
        return ZYM_ERROR;
    }

    ZymValue keyList = zym_newList(vm);
    zym_pushRoot(vm, keyList);

    KeyCollector collector = { vm, keyList, false };
    zym_mapForEach(vm, map, collectKeys, &collector);

    zym_popRoot(vm);

    if (collector.failed) {
        zym_runtimeError(vm, "keys() failed to build key list");
        return ZYM_ERROR;
    }

    return keyList;
}

// Callback for collecting values
static bool collectValues(ZymVM* vm, const char* key, ZymValue val, void* userdata) {
    KeyCollector* collector = (KeyCollector*)userdata;
    if (!zym_listAppend(vm, collector->list, val)) {
        collector->failed = true;
        return false;
    }
    return true;
}

// Get list of all values in map
ZymValue nativeMap_values(ZymVM* vm, ZymValue map) {
    if (!zym_isMap(map)) {
        zym_runtimeError(vm, "values() requires a map");
        return ZYM_ERROR;
    }

    ZymValue valueList = zym_newList(vm);
    zym_pushRoot(vm, valueList);

    KeyCollector collector = { vm, valueList, false };
    zym_mapForEach(vm, map, collectValues, &collector);

    zym_popRoot(vm);

    if (collector.failed) {
        zym_runtimeError(vm, "values() failed to build value list");
        return ZYM_ERROR;
    }

    return valueList;
}

// Callback for collecting entries (key-value pairs)
static bool collectEntries(ZymVM* vm, const char* key, ZymValue val, void* userdata) {
    KeyCollector* collector = (KeyCollector*)userdata;

    // Create a list with [key, value]
    ZymValue entry = zym_newList(vm);
    zym_pushRoot(vm, entry);  // Protect from GC

    ZymValue keyStr = zym_newString(vm, key);

    if (!zym_listAppend(vm, entry, keyStr) || !zym_listAppend(vm, entry, val)) {
        zym_popRoot(vm);
        collector->failed = true;
        return false;
    }

    if (!zym_listAppend(vm, collector->list, entry)) {
        zym_popRoot(vm);
        collector->failed = true;
        return false;
    }

    zym_popRoot(vm);
    return true;
}

// Get list of all entries as [key, value] pairs
ZymValue nativeMap_entries(ZymVM* vm, ZymValue map) {
    if (!zym_isMap(map)) {
        zym_runtimeError(vm, "entries() requires a map");
        return ZYM_ERROR;
    }

    ZymValue entryList = zym_newList(vm);
    zym_pushRoot(vm, entryList);

    KeyCollector collector = { vm, entryList, false };
    zym_mapForEach(vm, map, collectEntries, &collector);

    zym_popRoot(vm);

    if (collector.failed) {
        zym_runtimeError(vm, "entries() failed to build entry list");
        return ZYM_ERROR;
    }

    return entryList;
}

// Clear all entries from map
ZymValue nativeMap_clear(ZymVM* vm, ZymValue map) {
    if (!zym_isMap(map)) {
        zym_runtimeError(vm, "clear() requires a map");
        return ZYM_ERROR;
    }

    // Get all keys first
    ZymValue keyList = zym_newList(vm);
    zym_pushRoot(vm, keyList);

    KeyCollector collector = { vm, keyList, false };
    zym_mapForEach(vm, map, collectKeys, &collector);

    if (collector.failed) {
        zym_popRoot(vm);
        zym_runtimeError(vm, "clear() failed to collect keys");
        return ZYM_ERROR;
    }

    // Delete each key
    int len = zym_listLength(keyList);
    for (int i = 0; i < len; i++) {
        ZymValue keyVal = zym_listGet(vm, keyList, i);
        const char* key = zym_asCString(keyVal);
        zym_mapDelete(vm, map, key);
    }

    zym_popRoot(vm);
    return zym_newNull();
}

// Helper structure for merging
typedef struct {
    ZymVM* vm;
    ZymValue targetMap;
    bool failed;
} MergeHelper;

// Callback to copy entries
static bool mergeCallback(ZymVM* vm, const char* key, ZymValue val, void* userdata) {
    MergeHelper* helper = (MergeHelper*)userdata;
    if (!zym_mapSet(vm, helper->targetMap, key, val)) {
        helper->failed = true;
        return false;
    }
    return true;
}

// Merge source map into target map
ZymValue nativeMap_merge(ZymVM* vm, ZymValue targetMap, ZymValue sourceMap) {
    if (!zym_isMap(targetMap)) {
        zym_runtimeError(vm, "merge() requires a map as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isMap(sourceMap)) {
        zym_runtimeError(vm, "merge() requires a map as second argument");
        return ZYM_ERROR;
    }

    MergeHelper helper = { vm, targetMap, false };
    zym_mapForEach(vm, sourceMap, mergeCallback, &helper);

    if (helper.failed) {
        zym_runtimeError(vm, "merge() failed to merge maps");
        return ZYM_ERROR;
    }

    return zym_newNull();
}

// =============================================================================
// Registration
// =============================================================================

void registerMapNatives(VM* vm) {
    zym_defineNative(vm, "size(map)", nativeMap_size);
    zym_defineNative(vm, "isEmpty(map)", nativeMap_isEmpty);
    zym_defineNative(vm, "keys(map)", nativeMap_keys);
    zym_defineNative(vm, "values(map)", nativeMap_values);
    zym_defineNative(vm, "entries(map)", nativeMap_entries);
    zym_defineNative(vm, "clear(map)", nativeMap_clear);
    zym_defineNative(vm, "merge(target, source)", nativeMap_merge);
}
