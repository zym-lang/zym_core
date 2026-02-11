#include <string.h>

#include "utils.h"
#include "./memory.h"

void initOutputBuffer(OutputBuffer* ob) {
    ob->buffer = NULL;
    ob->count = 0;
    ob->capacity = 0;
}

void appendToOutputBuffer(VM* vm, OutputBuffer* ob, const char* text, size_t length) {
    if (!length) return;
    if (ob->count + (int)length > ob->capacity) {
        int old_capacity = ob->capacity;
        int new_capacity = old_capacity;
        while (new_capacity < ob->count + (int)length) {
            new_capacity = GROW_CAPACITY(new_capacity);
        }
        ob->capacity = new_capacity;
        ob->buffer = GROW_ARRAY(vm, char, ob->buffer, old_capacity, ob->capacity);
    }
    memcpy(ob->buffer + ob->count, text, length);
    ob->count += (int)length;
}

void freeOutputBuffer(VM* vm, OutputBuffer* ob) {
    FREE_ARRAY(vm, char, ob->buffer, ob->capacity);
    initOutputBuffer(ob);
}

void initConditionalStack(ConditionalStack* stack) {
    stack->states = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

void pushConditionalStack(VM* vm, ConditionalStack* stack, IfState state) {
    if (stack->count + 1 > stack->capacity) {
        int old_capacity = stack->capacity;
        stack->capacity = GROW_CAPACITY(old_capacity);
        stack->states = GROW_ARRAY(vm, IfState, stack->states, old_capacity, stack->capacity);
    }
    stack->states[stack->count++] = state;
}

IfState* peekConditionalStack(ConditionalStack* stack) {
    if (stack->count == 0) return NULL;
    return &stack->states[stack->count - 1];
}

void popConditionalStack(ConditionalStack* stack) {
    if (stack->count > 0) stack->count--;
}

void freeConditionalStack(VM* vm, ConditionalStack* stack) {
    FREE_ARRAY(vm, IfState, stack->states, stack->capacity);
    stack->states = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

static int parseHexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parseOctalDigit(char c) {
    if (c >= '0' && c <= '7') return c - '0';
    return -1;
}

char* processEscapeSequences(const char* input, int input_len, int* out_len,
                             const char** error_msg, int* error_pos) {
    char* output = (char*)malloc(input_len + 1);
    if (!output) {
        *error_msg = "Out of memory";
        *error_pos = 0;
        return NULL;
    }

    int out_pos = 0;

    for (int i = 0; i < input_len; i++) {
        if (input[i] == '\\' && i + 1 < input_len) {
            i++;
            char escape_char = input[i];

            switch (escape_char) {
                case 'n':  output[out_pos++] = '\n'; break;
                case 't':  output[out_pos++] = '\t'; break;
                case 'r':  output[out_pos++] = '\r'; break;
                case '\\': output[out_pos++] = '\\'; break;
                case '"':  output[out_pos++] = '"';  break;
                case '\'': output[out_pos++] = '\''; break;
                case 'b':  output[out_pos++] = '\b'; break;
                case 'f':  output[out_pos++] = '\f'; break;
                case 'v':  output[out_pos++] = '\v'; break;
                case 'a':  output[out_pos++] = '\a'; break;

                case 'x': {
                    if (i + 2 >= input_len) {
                        free(output);
                        *error_msg = "Incomplete hex escape sequence";
                        *error_pos = i - 1;
                        return NULL;
                    }

                    int digit1 = parseHexDigit(input[i + 1]);
                    int digit2 = parseHexDigit(input[i + 2]);

                    if (digit1 == -1 || digit2 == -1) {
                        free(output);
                        *error_msg = "Invalid hex escape sequence";
                        *error_pos = i - 1;
                        return NULL;
                    }

                    output[out_pos++] = (char)((digit1 << 4) | digit2);
                    i += 2;
                    break;
                }

                case 'u': {
                    if (i + 4 >= input_len) {
                        free(output);
                        *error_msg = "Incomplete unicode escape sequence";
                        *error_pos = i - 1;
                        return NULL;
                    }

                    int value = 0;
                    for (int j = 0; j < 4; j++) {
                        int digit = parseHexDigit(input[i + 1 + j]);
                        if (digit == -1) {
                            free(output);
                            *error_msg = "Invalid unicode escape sequence";
                            *error_pos = i - 1;
                            return NULL;
                        }
                        value = (value << 4) | digit;
                    }

                    // UTF-8 encoding (BMP only)
                    if (value <= 0x7F) {
                        output[out_pos++] = (char)value;
                    } else if (value <= 0x7FF) {
                        output[out_pos++] = (char)(0xC0 | (value >> 6));
                        output[out_pos++] = (char)(0x80 | (value & 0x3F));
                    } else {
                        output[out_pos++] = (char)(0xE0 | (value >> 12));
                        output[out_pos++] = (char)(0x80 | ((value >> 6) & 0x3F));
                        output[out_pos++] = (char)(0x80 | (value & 0x3F));
                    }

                    i += 4;
                    break;
                }

                case '0': {
                    if (i + 1 < input_len && parseOctalDigit(input[i + 1]) != -1) {
                        int value = 0;
                        for (int j = 0; j < 3 && i + 1 < input_len; j++) {
                            int digit = parseOctalDigit(input[i + 1]);
                            if (digit == -1) break;

                            int new_value = (value << 3) | digit;
                            if (new_value > 255) break;

                            value = new_value;
                            i++;
                        }
                        output[out_pos++] = (char)value;
                    } else {
                        output[out_pos++] = '\0';
                    }
                    break;
                }
                case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    int value = parseOctalDigit(escape_char);

                    for (int j = 0; j < 2 && i + 1 < input_len; j++) {
                        int digit = parseOctalDigit(input[i + 1]);
                        if (digit == -1) break;

                        int new_value = (value << 3) | digit;
                        if (new_value > 255) break;

                        value = new_value;
                        i++;
                    }

                    output[out_pos++] = (char)value;
                    break;
                }

                default: {
                    // Treat unknown escapes as literals
                    output[out_pos++] = '\\';
                    output[out_pos++] = escape_char;
                    break;
                }
            }
        } else {
            output[out_pos++] = input[i];
        }
    }

    output[out_pos] = '\0';
    *out_len = out_pos;
    return output;
}

char* decodeModulePath(const char* encoded, int length) {
    char* result = (char*)malloc(length + 1);
    size_t j = 0;

    for (int i = 0; i < length; ) {
        if (i + 7 <= length && memcmp(encoded + i, "_slash_", 7) == 0) {
            result[j++] = '/';
            i += 7;
        } else if (i + 5 <= length && memcmp(encoded + i, "_dot_", 5) == 0) {
            result[j++] = '.';
            i += 5;
        } else if (i + 6 <= length && memcmp(encoded + i, "_dash_", 6) == 0) {
            result[j++] = '-';
            i += 6;
        } else if (i + 7 <= length && memcmp(encoded + i, "_space_", 7) == 0) {
            result[j++] = ' ';
            i += 7;
        } else {
            result[j++] = encoded[i++];
        }
    }
    result[j] = '\0';

    return result;
}