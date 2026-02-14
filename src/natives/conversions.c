#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "conversions.h"

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
static bool ensureBufferSpace(char** buffer, size_t* buffer_size, size_t* current_pos, size_t needed) {
    while (*current_pos + needed >= *buffer_size) {
        *buffer_size *= 2;
        *buffer = realloc(*buffer, *buffer_size);
        if (*buffer == NULL) {
            return false;
        }
    }
    return true;
}

// Helper to append string to buffer
static bool appendToBuffer(char** buffer, size_t* buffer_size, size_t* current_pos, const char* str, size_t len) {
    if (!ensureBufferSpace(buffer, buffer_size, current_pos, len)) {
        return false;
    }
    memcpy(*buffer + *current_pos, str, len);
    *current_pos += len;
    return true;
}

// Helper to append formatted value to buffer using zym_valueToString
static bool appendFormattedValue(ZymVM* vm, char** buffer, size_t* buffer_size, size_t* current_pos,
                                  char format, ZymValue val, int argIndex) {
    char temp[256];

    switch (format) {
        case 's': // String
            if (!zym_isString(val)) {
                zym_runtimeError(vm, "str() format %%s at position %d expects string, got %s", argIndex, zym_typeName(val));
                return false;
            }
            return appendToBuffer(buffer, buffer_size, current_pos, zym_asCString(val), strlen(zym_asCString(val)));

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
                return appendToBuffer(buffer, buffer_size, current_pos, temp, len);
            }

        case 'b': // Boolean
            if (!zym_isBool(val)) {
                zym_runtimeError(vm, "str() format %%b at position %d expects bool, got %s", argIndex, zym_typeName(val));
                return false;
            }
            {
                const char* str = zym_asBool(val) ? "true" : "false";
                return appendToBuffer(buffer, buffer_size, current_pos, str, strlen(str));
            }

        case 'v': // Any value
            {
                ZymValue str_val = zym_valueToString(vm, val);
                if (str_val == ZYM_ERROR) return false;
                const char* str = zym_asCString(str_val);
                return appendToBuffer(buffer, buffer_size, current_pos, str, strlen(str));
            }

        default:
            zym_runtimeError(vm, "str() unknown format specifier '%%%c'", format);
            return false;
    }
}

// Core str implementation that processes format string and arguments
static ZymValue str_impl(ZymVM* vm, const char* format_str, ZymValue* args, int arg_count) {
    size_t buffer_size = 256;
    char* buffer = malloc(buffer_size);
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
                free(buffer);
                zym_runtimeError(vm, "str() format string ends with incomplete format specifier");
                return ZYM_ERROR;
            }

            if (*ptr == '%') {
                // Escaped % - append literal %
                if (current_pos >= buffer_size - 1) {
                    buffer_size *= 2;
                    buffer = realloc(buffer, buffer_size);
                    if (buffer == NULL) {
                        zym_runtimeError(vm, "str() out of memory");
                        return ZYM_ERROR;
                    }
                }
                buffer[current_pos++] = '%';
                ptr++;
            } else {
                // Format specifier
                if (arg_index >= arg_count) {
                    free(buffer);
                    zym_runtimeError(vm, "str() format string requires more arguments than provided");
                    return ZYM_ERROR;
                }

                if (!appendFormattedValue(vm, &buffer, &buffer_size, &current_pos, *ptr, args[arg_index], arg_index + 1)) {
                    free(buffer);
                    return ZYM_ERROR;
                }

                arg_index++;
                ptr++;
            }
        } else {
            // Regular character
            if (current_pos >= buffer_size - 1) {
                buffer_size *= 2;
                buffer = realloc(buffer, buffer_size);
                if (buffer == NULL) {
                    zym_runtimeError(vm, "str() out of memory");
                    return ZYM_ERROR;
                }
            }
            buffer[current_pos++] = *ptr;
            ptr++;
        }
    }

    // Check if we have unused arguments
    if (arg_index < arg_count) {
        free(buffer);
        zym_runtimeError(vm, "str() provided %d arguments but format string only uses %d", arg_count, arg_index);
        return ZYM_ERROR;
    }

    // Null-terminate and create string
    buffer[current_pos] = '\0';
    ZymValue result = zym_newString(vm, buffer);
    free(buffer);

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

// Multi-argument versions of str (str with 2+ arguments)
ZymValue nativeConversions_str_02(ZymVM* vm, ZymValue format, ZymValue a) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a };
    return str_impl(vm, zym_asCString(format), args, 1);
}

ZymValue nativeConversions_str_03(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b };
    return str_impl(vm, zym_asCString(format), args, 2);
}

ZymValue nativeConversions_str_04(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c };
    return str_impl(vm, zym_asCString(format), args, 3);
}

ZymValue nativeConversions_str_05(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d };
    return str_impl(vm, zym_asCString(format), args, 4);
}

ZymValue nativeConversions_str_06(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e };
    return str_impl(vm, zym_asCString(format), args, 5);
}

ZymValue nativeConversions_str_07(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f };
    return str_impl(vm, zym_asCString(format), args, 6);
}

ZymValue nativeConversions_str_08(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g };
    return str_impl(vm, zym_asCString(format), args, 7);
}

ZymValue nativeConversions_str_09(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h };
    return str_impl(vm, zym_asCString(format), args, 8);
}

ZymValue nativeConversions_str_10(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i };
    return str_impl(vm, zym_asCString(format), args, 9);
}

ZymValue nativeConversions_str_11(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j };
    return str_impl(vm, zym_asCString(format), args, 10);
}

ZymValue nativeConversions_str_12(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k };
    return str_impl(vm, zym_asCString(format), args, 11);
}

ZymValue nativeConversions_str_13(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l };
    return str_impl(vm, zym_asCString(format), args, 12);
}

ZymValue nativeConversions_str_14(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m };
    return str_impl(vm, zym_asCString(format), args, 13);
}

ZymValue nativeConversions_str_15(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n };
    return str_impl(vm, zym_asCString(format), args, 14);
}

ZymValue nativeConversions_str_16(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o };
    return str_impl(vm, zym_asCString(format), args, 15);
}

ZymValue nativeConversions_str_17(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p };
    return str_impl(vm, zym_asCString(format), args, 16);
}

ZymValue nativeConversions_str_18(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q };
    return str_impl(vm, zym_asCString(format), args, 17);
}

ZymValue nativeConversions_str_19(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r };
    return str_impl(vm, zym_asCString(format), args, 18);
}

ZymValue nativeConversions_str_20(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s };
    return str_impl(vm, zym_asCString(format), args, 19);
}

ZymValue nativeConversions_str_21(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t };
    return str_impl(vm, zym_asCString(format), args, 20);
}

ZymValue nativeConversions_str_22(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t, ZymValue u) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u };
    return str_impl(vm, zym_asCString(format), args, 21);
}

ZymValue nativeConversions_str_23(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t, ZymValue u, ZymValue v) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v };
    return str_impl(vm, zym_asCString(format), args, 22);
}

ZymValue nativeConversions_str_24(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t, ZymValue u, ZymValue v, ZymValue w) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w };
    return str_impl(vm, zym_asCString(format), args, 23);
}

ZymValue nativeConversions_str_25(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t, ZymValue u, ZymValue v, ZymValue w, ZymValue x) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x };
    return str_impl(vm, zym_asCString(format), args, 24);
}

ZymValue nativeConversions_str_26(ZymVM* vm, ZymValue format, ZymValue a, ZymValue b, ZymValue c, ZymValue d, ZymValue e, ZymValue f, ZymValue g, ZymValue h, ZymValue i, ZymValue j, ZymValue k, ZymValue l, ZymValue m, ZymValue n, ZymValue o, ZymValue p, ZymValue q, ZymValue r, ZymValue s, ZymValue t, ZymValue u, ZymValue v, ZymValue w, ZymValue x, ZymValue y) {
    if (!zym_isString(format)) {
        zym_runtimeError(vm, "str() first argument must be a string");
        return ZYM_ERROR;
    }

    ZymValue args[] = { a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y };
    return str_impl(vm, zym_asCString(format), args, 25);
}

// =============================================================================
// Registration (loaded at VM startup via core_natives)
// =============================================================================

void registerConversionsNatives(VM* vm) {
    zym_defineNative(vm, "num(value)", nativeConversions_num);
    zym_defineNative(vm, "str(value)", nativeConversions_str);
    zym_defineNative(vm, "str(a, b)", nativeConversions_str_02);
    zym_defineNative(vm, "str(a, b, c)", nativeConversions_str_03);
    zym_defineNative(vm, "str(a, b, c, d)", nativeConversions_str_04);
    zym_defineNative(vm, "str(a, b, c, d, e)", nativeConversions_str_05);
    zym_defineNative(vm, "str(a, b, c, d, e, f)", nativeConversions_str_06);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g)", nativeConversions_str_07);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h)", nativeConversions_str_08);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i)", nativeConversions_str_09);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j)", nativeConversions_str_10);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k)", nativeConversions_str_11);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l)", nativeConversions_str_12);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m)", nativeConversions_str_13);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n)", nativeConversions_str_14);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o)", nativeConversions_str_15);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p)", nativeConversions_str_16);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q)", nativeConversions_str_17);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r)", nativeConversions_str_18);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s)", nativeConversions_str_19);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t)", nativeConversions_str_20);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u)", nativeConversions_str_21);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v)", nativeConversions_str_22);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w)", nativeConversions_str_23);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x)", nativeConversions_str_24);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y)", nativeConversions_str_25);
    zym_defineNative(vm, "str(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z)", nativeConversions_str_26);
}
