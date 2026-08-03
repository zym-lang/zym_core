#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "conversions.h"
#include "../memory.h"

// =============================================================================
// CONVERSION FUNCTIONS
// =============================================================================

// Convert string to number
ZymValue nativeConversions_num(ZymVM* vm, ZymValue value) {
    if (!zym_isString(value)) {
        zym_runtimeError(vm, "num() requires a string argument");
        return ZYM_ERROR;
    }

    const char* str = zym_asCString(value);

    // Trim leading whitespace
    while (isspace((unsigned char)*str)) {
        str++;
    }

    // Check for empty string after trimming
    if (*str == '\0') {
        zym_runtimeError(vm, "num() cannot convert empty string to number");
        return ZYM_ERROR;
    }

    // Try to parse as number
    char* endptr;
    double result = strtod(str, &endptr);

    // Skip trailing whitespace
    while (isspace((unsigned char)*endptr)) {
        endptr++;
    }

    // Check if entire string was consumed (valid number)
    if (*endptr != '\0') {
        zym_runtimeError(vm, "num() invalid number format");
        return ZYM_ERROR;
    }

    return zym_newNumber(result);
}

// Helper to ensure buffer has space
static bool ensureBufferSpace(ZymAllocator* alloc, char** buffer, size_t* buffer_size, size_t* current_pos, size_t needed) {
    while (*current_pos + needed >= *buffer_size) {
        size_t old = *buffer_size;
        *buffer_size *= 2;
        *buffer = ZYM_REALLOC(alloc, *buffer, old, *buffer_size);
        if (*buffer == NULL) {
            return false;
        }
    }
    return true;
}

// Helper to append string to buffer
static bool appendToBuffer(ZymAllocator* alloc, char** buffer, size_t* buffer_size, size_t* current_pos, const char* str, size_t len) {
    if (!ensureBufferSpace(alloc, buffer, buffer_size, current_pos, len)) {
        return false;
    }
    memcpy(*buffer + *current_pos, str, len);
    *current_pos += len;
    return true;
}

// Helper to append formatted value to buffer using zym_valueToString
static bool appendFormattedValue(ZymVM* vm, ZymAllocator* alloc, char** buffer, size_t* buffer_size, size_t* current_pos,
                                  char format, ZymValue val, int argIndex) {
    char temp[256];

    switch (format) {
        case 's': // String
            if (!zym_isString(val)) {
                zym_runtimeError(vm, "str() format %%s at position %d expects string, got %s", argIndex, zym_typeName(val));
                return false;
            }
            return appendToBuffer(alloc, buffer, buffer_size, current_pos, zym_asCString(val), strlen(zym_asCString(val)));

        case 'n': // Number
            if (!zym_isNumber(val)) {
                zym_runtimeError(vm, "str() format %%n at position %d expects number, got %s", argIndex, zym_typeName(val));
                return false;
            }
            {
                double num = zym_asNumber(val);
                int len;
                if (num == (long long)num && num >= -1e15 && num <= 1e15) {
                    len = snprintf(temp, sizeof(temp), "%.0f", num);
                } else {
                    len = snprintf(temp, sizeof(temp), "%g", num);
                }
                return appendToBuffer(alloc, buffer, buffer_size, current_pos, temp, len);
            }

        case 'b': // Boolean
            if (!zym_isBool(val)) {
                zym_runtimeError(vm, "str() format %%b at position %d expects bool, got %s", argIndex, zym_typeName(val));
                return false;
            }
            {
                const char* str = zym_asBool(val) ? "true" : "false";
                return appendToBuffer(alloc, buffer, buffer_size, current_pos, str, strlen(str));
            }

        case 'v': // Any value
            {
                ZymValue str_val = zym_valueToString(vm, val);
                if (str_val == ZYM_ERROR) return false;
                const char* str = zym_asCString(str_val);
                return appendToBuffer(alloc, buffer, buffer_size, current_pos, str, strlen(str));
            }

        default:
            zym_runtimeError(vm, "str() unknown format specifier '%%%c'", format);
            return false;
    }
}

// Core str implementation that processes format string and arguments
static ZymValue str_impl(ZymVM* vm, const char* format_str, ZymValue* args, int arg_count) {
    ZymAllocator* alloc = (ZymAllocator*)zym_getAllocator(vm);
    size_t buffer_size = 256;
    char* buffer = ZYM_ALLOC(alloc, buffer_size);
    if (buffer == NULL) {
        zym_runtimeError(vm, "str() out of memory");
        return ZYM_ERROR;
    }

    size_t current_pos = 0;
    const char* ptr = format_str;
    int arg_index = 0;

    while (*ptr) {
        if (*ptr == '%') {
            ptr++;
            if (*ptr == '\0') {
                ZYM_FREE(alloc, buffer, buffer_size);
                zym_runtimeError(vm, "str() format string ends with incomplete format specifier");
                return ZYM_ERROR;
            }

            if (*ptr == '%') {
                // Escaped % - append literal %
                if (!ensureBufferSpace(alloc, &buffer, &buffer_size, &current_pos, 1)) {
                    zym_runtimeError(vm, "str() out of memory");
                    return ZYM_ERROR;
                }
                buffer[current_pos++] = '%';
                ptr++;
            } else {
                // Format specifier
                if (arg_index >= arg_count) {
                    ZYM_FREE(alloc, buffer, buffer_size);
                    zym_runtimeError(vm, "str() format string requires more arguments than provided");
                    return ZYM_ERROR;
                }

                if (!appendFormattedValue(vm, alloc, &buffer, &buffer_size, &current_pos, *ptr, args[arg_index], arg_index + 1)) {
                    ZYM_FREE(alloc, buffer, buffer_size);
                    return ZYM_ERROR;
                }

                arg_index++;
                ptr++;
            }
        } else {
            // Regular character
            if (!ensureBufferSpace(alloc, &buffer, &buffer_size, &current_pos, 1)) {
                zym_runtimeError(vm, "str() out of memory");
                return ZYM_ERROR;
            }
            buffer[current_pos++] = *ptr;
            ptr++;
        }
    }

    // Check if we have unused arguments
    if (arg_index < arg_count) {
        ZYM_FREE(alloc, buffer, buffer_size);
        zym_runtimeError(vm, "str() provided %d arguments but format string only uses %d", arg_count, arg_index);
        return ZYM_ERROR;
    }

    // Null-terminate and create string
    buffer[current_pos] = '\0';
    ZymValue result = zym_newString(vm, buffer);
    ZYM_FREE(alloc, buffer, buffer_size);

    return result;
}

// Convert value(s) to string with format support
ZymValue nativeConversions_str(ZymVM* vm, ZymValue value) {
    // If it's a string with format specifiers, process it
    if (zym_isString(value)) {
        const char* format_str = zym_asCString(value);

        // Check if string contains any format specifiers
        bool has_format = false;
        for (const char* p = format_str; *p; p++) {
            if (*p == '%' && *(p + 1) != '%') {
                has_format = true;
                break;
            }
        }

        // If no format specifiers, just return the string as-is
        if (!has_format) {
            return value;
        }

        // Has format specifiers but no args - treat as format string with no arguments
        return str_impl(vm, format_str, NULL, 0);
    }

    // Single non-string value - convert to string using zym_valueToString
    ZymValue str_val = zym_valueToString(vm, value);
    if (str_val == ZYM_ERROR) {
        zym_runtimeError(vm, "str() failed to convert value to string");
        return ZYM_ERROR;
    }
    return str_val;
}

// Variadic str: format string + variable arguments
ZymValue nativeConversions_str_variadic(ZymVM* vm, ZymValue format, ZymValue* vargs, int vargc) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    return str_impl(vm, zym_asCString(format), vargs, vargc);
}

// =============================================================================
// Registration (loaded at VM startup via core_natives)
// =============================================================================

void registerConversionsNatives(VM* vm) {
    zym_defineNative(vm, "num(value)", nativeConversions_num);
    zym_defineNative(vm, "str(value)", nativeConversions_str);
    zym_defineNativeVariadic(vm, "str(format, ...)", nativeConversions_str_variadic);
}
