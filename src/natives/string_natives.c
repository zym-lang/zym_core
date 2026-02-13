#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "string_natives.h"
#include "../utf8.h"

// =============================================================================
// STRING MANIPULATION FUNCTIONS
// Note: length, concat, indexOf, contains, slice are in shared.c
// =============================================================================

// Get character at index (UTF-8 aware)
ZymValue nativeString_charAt(ZymVM* vm, ZymValue str, ZymValue indexVal) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "charAt() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isNumber(indexVal)) {
        zym_runtimeError(vm, "charAt() requires a number as second argument");
        return ZYM_ERROR;
    }

    const char* cstr;
    int char_len;
    int byte_len;
    zym_toString(str, &cstr, &char_len);
    zym_toStringBytes(str, NULL, &byte_len);

    int index = (int)zym_asNumber(indexVal);

    // Handle negative indices
    if (index < 0) {
        index = char_len + index;
    }

    if (index < 0 || index >= char_len) {
        zym_runtimeError(vm, "charAt() index %d out of bounds (0-%d)", index, char_len - 1);
        return ZYM_ERROR;
    }

    // Find byte offset for this character index
    int byte_offset = utf8_offset(cstr, byte_len, index);
    if (byte_offset == -1) {
        zym_runtimeError(vm, "charAt() failed to find character at index %d", index);
        return ZYM_ERROR;
    }

    // Decode the UTF-8 character
    uint32_t codepoint;
    int char_bytes = utf8_decode(cstr + byte_offset, byte_len - byte_offset, &codepoint);
    if (char_bytes == 0) {
        zym_runtimeError(vm, "charAt() encountered invalid UTF-8 at index %d", index);
        return ZYM_ERROR;
    }

    // Create a new string with this character
    char result[5] = {0};  // Max 4 bytes + null terminator
    memcpy(result, cstr + byte_offset, char_bytes);
    result[char_bytes] = '\0';

    return zym_newString(vm, result);
}

// Get Unicode code point at index (UTF-8 aware)
ZymValue nativeString_charCodeAt(ZymVM* vm, ZymValue str, ZymValue indexVal) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "charCodeAt() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isNumber(indexVal)) {
        zym_runtimeError(vm, "charCodeAt() requires a number as second argument");
        return ZYM_ERROR;
    }

    const char* cstr;
    int char_len;
    int byte_len;
    zym_toString(str, &cstr, &char_len);
    zym_toStringBytes(str, NULL, &byte_len);

    int index = (int)zym_asNumber(indexVal);

    // Handle negative indices
    if (index < 0) {
        index = char_len + index;
    }

    if (index < 0 || index >= char_len) {
        zym_runtimeError(vm, "charCodeAt() index %d out of bounds (0-%d)", index, char_len - 1);
        return ZYM_ERROR;
    }

    // Find byte offset for this character index
    int byte_offset = utf8_offset(cstr, byte_len, index);
    if (byte_offset == -1) {
        zym_runtimeError(vm, "charCodeAt() failed to find character at index %d", index);
        return ZYM_ERROR;
    }

    // Decode the UTF-8 character to get code point
    uint32_t codepoint;
    int char_bytes = utf8_decode(cstr + byte_offset, byte_len - byte_offset, &codepoint);
    if (char_bytes == 0) {
        zym_runtimeError(vm, "charCodeAt() encountered invalid UTF-8 at index %d", index);
        return ZYM_ERROR;
    }

    return zym_newNumber((double)codepoint);
}

// Get byte length of UTF-8 string
ZymValue nativeString_byteLength(ZymVM* vm, ZymValue str) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "byteLength() requires a string argument");
        return ZYM_ERROR;
    }

    int byte_len;
    zym_toStringBytes(str, NULL, &byte_len);
    return zym_newNumber((double)byte_len);
}

// Create string from Unicode code point
ZymValue nativeString_fromCodePoint(ZymVM* vm, ZymValue codePointVal) {
    if (!zym_isNumber(codePointVal)) {
        zym_runtimeError(vm, "fromCodePoint() requires a number argument");
        return ZYM_ERROR;
    }

    uint32_t codepoint = (uint32_t)zym_asNumber(codePointVal);

    // Validate code point range
    if (codepoint > UTF8_MAX_CODEPOINT || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        zym_runtimeError(vm, "fromCodePoint() invalid code point: %u", codepoint);
        return ZYM_ERROR;
    }

    // Encode to UTF-8
    char buffer[5] = {0};
    int bytes = utf8_encode(codepoint, buffer);
    if (bytes == 0) {
        zym_runtimeError(vm, "fromCodePoint() failed to encode code point: %u", codepoint);
        return ZYM_ERROR;
    }

    buffer[bytes] = '\0';
    return zym_newString(vm, buffer);
}

// Check if string starts with prefix
ZymValue nativeString_startsWith(ZymVM* vm, ZymValue str, ZymValue prefixStr) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "startsWith() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isString(prefixStr)) {
        zym_runtimeError(vm, "startsWith() requires a string as second argument");
        return ZYM_ERROR;
    }

    const char* cstr = zym_asCString(str);
    const char* prefix = zym_asCString(prefixStr);

    int prefixLen = strlen(prefix);
    return zym_newBool(strncmp(cstr, prefix, prefixLen) == 0);
}

// Check if string ends with suffix
ZymValue nativeString_endsWith(ZymVM* vm, ZymValue str, ZymValue suffixStr) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "endsWith() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isString(suffixStr)) {
        zym_runtimeError(vm, "endsWith() requires a string as second argument");
        return ZYM_ERROR;
    }

    const char* cstr = zym_asCString(str);
    const char* suffix = zym_asCString(suffixStr);

    int strLen = strlen(cstr);
    int suffixLen = strlen(suffix);

    if (suffixLen > strLen) {
        return zym_newBool(false);
    }

    return zym_newBool(strcmp(cstr + strLen - suffixLen, suffix) == 0);
}

// Find last index of substring (returns -1 if not found)
ZymValue nativeString_lastIndexOf(ZymVM* vm, ZymValue str, ZymValue searchStr) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "lastIndexOf() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isString(searchStr)) {
        zym_runtimeError(vm, "lastIndexOf() requires a string as second argument");
        return ZYM_ERROR;
    }

    const char* cstr = zym_asCString(str);
    const char* search = zym_asCString(searchStr);
    int searchLen = strlen(search);

    const char* lastFound = NULL;
    const char* current = cstr;

    while ((current = strstr(current, search)) != NULL) {
        lastFound = current;
        current += searchLen;
    }

    if (lastFound == NULL) {
        return zym_newNumber(-1.0);
    }

    return zym_newNumber((double)(lastFound - cstr));
}

// Convert string to uppercase
ZymValue nativeString_toUpperCase(ZymVM* vm, ZymValue str) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "toUpperCase() requires a string");
        return ZYM_ERROR;
    }

    const char* cstr;
    int byte_len;
    zym_toStringBytes(str, &cstr, &byte_len);

    if (byte_len >= 4095) {
        zym_runtimeError(vm, "toUpperCase() string too long");
        return ZYM_ERROR;
    }

    int result_len;
    char* result = utf8_toupper(cstr, byte_len, &result_len);
    if (result == NULL) {
        zym_runtimeError(vm, "toUpperCase() out of memory");
        return ZYM_ERROR;
    }

    ZymValue newStr = zym_newString(vm, result);
    free(result);
    return newStr;
}

// Convert string to lowercase
ZymValue nativeString_toLowerCase(ZymVM* vm, ZymValue str) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "toLowerCase() requires a string");
        return ZYM_ERROR;
    }

    const char* cstr;
    int byte_len;
    zym_toStringBytes(str, &cstr, &byte_len);

    if (byte_len >= 4095) {
        zym_runtimeError(vm, "toLowerCase() string too long");
        return ZYM_ERROR;
    }

    int result_len;
    char* result = utf8_tolower(cstr, byte_len, &result_len);
    if (result == NULL) {
        zym_runtimeError(vm, "toLowerCase() out of memory");
        return ZYM_ERROR;
    }

    ZymValue newStr = zym_newString(vm, result);
    free(result);
    return newStr;
}

// Trim whitespace from both ends
ZymValue nativeString_trim(ZymVM* vm, ZymValue str) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "trim() requires a string");
        return ZYM_ERROR;
    }

    const char* cstr = zym_asCString(str);
    int len = strlen(cstr);

    // Find first non-whitespace
    int start = 0;
    while (start < len && isspace((unsigned char)cstr[start])) {
        start++;
    }

    // Find last non-whitespace
    int end = len - 1;
    while (end >= start && isspace((unsigned char)cstr[end])) {
        end--;
    }

    int newLen = end - start + 1;
    if (newLen <= 0) {
        return zym_newString(vm, "");
    }

    char buffer[4096];
    if (newLen >= 4095) {
        zym_runtimeError(vm, "trim() result string too long");
        return ZYM_ERROR;
    }

    strncpy(buffer, cstr + start, newLen);
    buffer[newLen] = '\0';

    return zym_newString(vm, buffer);
}

// Trim whitespace from start
ZymValue nativeString_trimStart(ZymVM* vm, ZymValue str) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "trimStart() requires a string");
        return ZYM_ERROR;
    }

    const char* cstr = zym_asCString(str);
    int len = strlen(cstr);

    // Find first non-whitespace
    int start = 0;
    while (start < len && isspace((unsigned char)cstr[start])) {
        start++;
    }

    return zym_newString(vm, cstr + start);
}

// Trim whitespace from end
ZymValue nativeString_trimEnd(ZymVM* vm, ZymValue str) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "trimEnd() requires a string");
        return ZYM_ERROR;
    }

    const char* cstr = zym_asCString(str);
    int len = strlen(cstr);

    // Find last non-whitespace
    int end = len - 1;
    while (end >= 0 && isspace((unsigned char)cstr[end])) {
        end--;
    }

    int newLen = end + 1;
    if (newLen <= 0) {
        return zym_newString(vm, "");
    }

    char buffer[4096];
    if (newLen >= 4095) {
        zym_runtimeError(vm, "trimEnd() result string too long");
        return ZYM_ERROR;
    }

    strncpy(buffer, cstr, newLen);
    buffer[newLen] = '\0';

    return zym_newString(vm, buffer);
}

// Replace first occurrence of search string with replacement
ZymValue nativeString_replace(ZymVM* vm, ZymValue str, ZymValue searchStr, ZymValue replaceStr) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "replace() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isString(searchStr)) {
        zym_runtimeError(vm, "replace() requires a string as second argument");
        return ZYM_ERROR;
    }

    if (!zym_isString(replaceStr)) {
        zym_runtimeError(vm, "replace() requires a string as third argument");
        return ZYM_ERROR;
    }

    const char* cstr = zym_asCString(str);
    const char* search = zym_asCString(searchStr);
    const char* replace = zym_asCString(replaceStr);

    const char* found = strstr(cstr, search);
    if (found == NULL) {
        // Not found, return original string
        return str;
    }

    int searchLen = strlen(search);
    int replaceLen = strlen(replace);
    int beforeLen = found - cstr;
    int afterLen = strlen(found + searchLen);

    if (beforeLen + replaceLen + afterLen >= 4095) {
        zym_runtimeError(vm, "replace() result string too long");
        return ZYM_ERROR;
    }

    char buffer[4096];
    strncpy(buffer, cstr, beforeLen);
    buffer[beforeLen] = '\0';
    strcat(buffer, replace);
    strcat(buffer, found + searchLen);

    return zym_newString(vm, buffer);
}

// Replace all occurrences of search string with replacement
ZymValue nativeString_replaceAll(ZymVM* vm, ZymValue str, ZymValue searchStr, ZymValue replaceStr) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "replaceAll() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isString(searchStr)) {
        zym_runtimeError(vm, "replaceAll() requires a string as second argument");
        return ZYM_ERROR;
    }

    if (!zym_isString(replaceStr)) {
        zym_runtimeError(vm, "replaceAll() requires a string as third argument");
        return ZYM_ERROR;
    }

    const char* cstr = zym_asCString(str);
    const char* search = zym_asCString(searchStr);
    const char* replace = zym_asCString(replaceStr);
    int searchLen = strlen(search);

    if (searchLen == 0) {
        // Empty search string, return original
        return str;
    }

    char buffer[4096];
    buffer[0] = '\0';
    int bufferPos = 0;

    const char* current = cstr;
    const char* found;

    while ((found = strstr(current, search)) != NULL) {
        // Copy before match
        int beforeLen = found - current;
        if (bufferPos + beforeLen >= 4095) {
            zym_runtimeError(vm, "replaceAll() result string too long");
            return ZYM_ERROR;
        }
        strncpy(buffer + bufferPos, current, beforeLen);
        bufferPos += beforeLen;

        // Copy replacement
        int replaceLen = strlen(replace);
        if (bufferPos + replaceLen >= 4095) {
            zym_runtimeError(vm, "replaceAll() result string too long");
            return ZYM_ERROR;
        }
        strcpy(buffer + bufferPos, replace);
        bufferPos += replaceLen;

        current = found + searchLen;
    }

    // Copy remaining
    int remainLen = strlen(current);
    if (bufferPos + remainLen >= 4095) {
        zym_runtimeError(vm, "replaceAll() result string too long");
        return ZYM_ERROR;
    }
    strcpy(buffer + bufferPos, current);

    return zym_newString(vm, buffer);
}

// Split string by delimiter into a list
ZymValue nativeString_split(ZymVM* vm, ZymValue str, ZymValue delimiterStr) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "split() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isString(delimiterStr)) {
        zym_runtimeError(vm, "split() requires a string as second argument");
        return ZYM_ERROR;
    }

    const char* cstr = zym_asCString(str);
    const char* delimiter = zym_asCString(delimiterStr);
    int delimLen = strlen(delimiter);

    ZymValue list = zym_newList(vm);
    zym_pushRoot(vm, list);

    if (delimLen == 0) {
        // Empty delimiter, split into individual characters
        int len = strlen(cstr);
        for (int i = 0; i < len; i++) {
            char charStr[2] = {cstr[i], '\0'};
            ZymValue charVal = zym_newString(vm, charStr);
            if (!zym_listAppend(vm, list, charVal)) {
                zym_popRoot(vm);
                zym_runtimeError(vm, "split() failed to append to list");
                return ZYM_ERROR;
            }
        }
        zym_popRoot(vm);
        return list;
    }

    const char* current = cstr;
    const char* found;

    while ((found = strstr(current, delimiter)) != NULL) {
        // Extract part before delimiter
        int partLen = found - current;
        char buffer[4096];
        if (partLen >= 4095) {
            zym_popRoot(vm);
            zym_runtimeError(vm, "split() substring too long");
            return ZYM_ERROR;
        }
        strncpy(buffer, current, partLen);
        buffer[partLen] = '\0';

        ZymValue part = zym_newString(vm, buffer);
        if (!zym_listAppend(vm, list, part)) {
            zym_popRoot(vm);
            zym_runtimeError(vm, "split() failed to append to list");
            return ZYM_ERROR;
        }

        current = found + delimLen;
    }

    // Add remaining part
    ZymValue lastPart = zym_newString(vm, current);
    if (!zym_listAppend(vm, list, lastPart)) {
        zym_popRoot(vm);
        zym_runtimeError(vm, "split() failed to append to list");
        return ZYM_ERROR;
    }

    zym_popRoot(vm);
    return list;
}

// Repeat string n times
ZymValue nativeString_repeat(ZymVM* vm, ZymValue str, ZymValue countVal) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "repeat() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isNumber(countVal)) {
        zym_runtimeError(vm, "repeat() requires a number as second argument");
        return ZYM_ERROR;
    }

    int count = (int)zym_asNumber(countVal);
    if (count < 0) {
        zym_runtimeError(vm, "repeat() count must be non-negative");
        return ZYM_ERROR;
    }

    if (count == 0) {
        return zym_newString(vm, "");
    }

    const char* cstr = zym_asCString(str);
    int len = strlen(cstr);

    if (len * count >= 4095) {
        zym_runtimeError(vm, "repeat() result string too long");
        return ZYM_ERROR;
    }

    char buffer[4096];
    buffer[0] = '\0';

    for (int i = 0; i < count; i++) {
        strcat(buffer, cstr);
    }

    return zym_newString(vm, buffer);
}

// Pad string to target length with pad string on the left
ZymValue nativeString_padStart(ZymVM* vm, ZymValue str, ZymValue targetLenVal, ZymValue padStr) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "padStart() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isNumber(targetLenVal)) {
        zym_runtimeError(vm, "padStart() requires a number as second argument");
        return ZYM_ERROR;
    }

    if (!zym_isString(padStr)) {
        zym_runtimeError(vm, "padStart() requires a string as third argument");
        return ZYM_ERROR;
    }

    const char* cstr = zym_asCString(str);
    const char* pad = zym_asCString(padStr);
    int strLen = strlen(cstr);
    int targetLen = (int)zym_asNumber(targetLenVal);
    int padLen = strlen(pad);

    if (padLen == 0 || targetLen <= strLen) {
        return str;
    }

    if (targetLen >= 4095) {
        zym_runtimeError(vm, "padStart() result string too long");
        return ZYM_ERROR;
    }

    char buffer[4096];
    int fillLen = targetLen - strLen;
    int pos = 0;

    // Add padding
    while (pos < fillLen) {
        int copyLen = (fillLen - pos < padLen) ? (fillLen - pos) : padLen;
        strncpy(buffer + pos, pad, copyLen);
        pos += copyLen;
    }

    // Add original string
    strcpy(buffer + pos, cstr);

    return zym_newString(vm, buffer);
}

// Pad string to target length with pad string on the right
ZymValue nativeString_padEnd(ZymVM* vm, ZymValue str, ZymValue targetLenVal, ZymValue padStr) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "padEnd() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isNumber(targetLenVal)) {
        zym_runtimeError(vm, "padEnd() requires a number as second argument");
        return ZYM_ERROR;
    }

    if (!zym_isString(padStr)) {
        zym_runtimeError(vm, "padEnd() requires a string as third argument");
        return ZYM_ERROR;
    }

    const char* cstr = zym_asCString(str);
    const char* pad = zym_asCString(padStr);
    int strLen = strlen(cstr);
    int targetLen = (int)zym_asNumber(targetLenVal);
    int padLen = strlen(pad);

    if (padLen == 0 || targetLen <= strLen) {
        return str;
    }

    if (targetLen >= 4095) {
        zym_runtimeError(vm, "padEnd() result string too long");
        return ZYM_ERROR;
    }

    char buffer[4096];
    strcpy(buffer, cstr);
    int fillLen = targetLen - strLen;
    int pos = strLen;

    // Add padding
    while (pos < targetLen) {
        int copyLen = (targetLen - pos < padLen) ? (targetLen - pos) : padLen;
        strncpy(buffer + pos, pad, copyLen);
        pos += copyLen;
    }
    buffer[targetLen] = '\0';

    return zym_newString(vm, buffer);
}

// Extract substring from start to end (or to end of string if end is -1)
ZymValue nativeString_substr(ZymVM* vm, ZymValue str, ZymValue startVal, ZymValue endVal) {
    if (!zym_isString(str)) {
        zym_runtimeError(vm, "substr() requires a string as first argument");
        return ZYM_ERROR;
    }

    if (!zym_isNumber(startVal)) {
        zym_runtimeError(vm, "substr() requires a number as second argument (start)");
        return ZYM_ERROR;
    }

    if (!zym_isNumber(endVal)) {
        zym_runtimeError(vm, "substr() requires a number as third argument (end)");
        return ZYM_ERROR;
    }

    const char* cstr;
    int char_len;
    int byte_len;
    zym_toString(str, &cstr, &char_len);
    zym_toStringBytes(str, NULL, &byte_len);

    int start = (int)zym_asNumber(startVal);
    int end = (int)zym_asNumber(endVal);

    // Special case: -1 means "to end of string"
    if (end == -1) {
        end = char_len;
    }

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
        zym_runtimeError(vm, "substr() character indices out of range");
        return ZYM_ERROR;
    }

    int subLen = end_byte - start_byte;
    if (subLen >= 4095) {
        zym_runtimeError(vm, "substr() result string too long");
        return ZYM_ERROR;
    }

    char buffer[4096];
    memcpy(buffer, cstr + start_byte, subLen);
    buffer[subLen] = '\0';

    return zym_newString(vm, buffer);
}

// =============================================================================
// Registration
// =============================================================================

void registerStringNatives(VM* vm) {
    zym_defineNative(vm, "charAt(str, index)", nativeString_charAt);
    zym_defineNative(vm, "charCodeAt(str, index)", nativeString_charCodeAt);
    zym_defineNative(vm, "byteLength(str)", nativeString_byteLength);
    zym_defineNative(vm, "fromCodePoint(codepoint)", nativeString_fromCodePoint);
    zym_defineNative(vm, "startsWith(str, prefix)", nativeString_startsWith);
    zym_defineNative(vm, "endsWith(str, suffix)", nativeString_endsWith);
    zym_defineNative(vm, "lastIndexOf(str, search)", nativeString_lastIndexOf);
    zym_defineNative(vm, "toUpperCase(str)", nativeString_toUpperCase);
    zym_defineNative(vm, "toLowerCase(str)", nativeString_toLowerCase);
    zym_defineNative(vm, "trim(str)", nativeString_trim);
    zym_defineNative(vm, "trimStart(str)", nativeString_trimStart);
    zym_defineNative(vm, "trimEnd(str)", nativeString_trimEnd);
    zym_defineNative(vm, "replace(str, search, replace)", nativeString_replace);
    zym_defineNative(vm, "replaceAll(str, search, replace)", nativeString_replaceAll);
    zym_defineNative(vm, "split(str, delimiter)", nativeString_split);
    zym_defineNative(vm, "repeat(str, count)", nativeString_repeat);
    zym_defineNative(vm, "padStart(str, targetLen, pad)", nativeString_padStart);
    zym_defineNative(vm, "padEnd(str, targetLen, pad)", nativeString_padEnd);
    zym_defineNative(vm, "substr(str, start, end)", nativeString_substr);
}
