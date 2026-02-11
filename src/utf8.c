#include "utf8.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int utf8_charlen(unsigned char first_byte) {
    if ((first_byte & 0x80) == 0x00) {
        return 1;
    } else if ((first_byte & 0xE0) == 0xC0) {
        return 2;
    } else if ((first_byte & 0xF0) == 0xE0) {
        return 3;
    } else if ((first_byte & 0xF8) == 0xF0) {
        return 4;
    }
    return 0;
}

int utf8_decode(const char* str, size_t len, uint32_t* out_codepoint) {
    if (len == 0 || str == NULL) {
        return 0;
    }

    unsigned char first = (unsigned char)str[0];
    int char_len = utf8_charlen(first);

    if (char_len == 0 || char_len > (int)len) {
        *out_codepoint = UTF8_REPLACEMENT_CHAR;
        return 0;
    }

    uint32_t codepoint = 0;

    switch (char_len) {
        case 1:
            codepoint = first;
            break;

        case 2: {
            unsigned char second = (unsigned char)str[1];
            if ((second & 0xC0) != 0x80) {
                *out_codepoint = UTF8_REPLACEMENT_CHAR;
                return 0;
            }
            codepoint = ((first & 0x1F) << 6) | (second & 0x3F);

            if (codepoint < 0x80) {
                *out_codepoint = UTF8_REPLACEMENT_CHAR;
                return 0;
            }
            break;
        }

        case 3: {
            unsigned char second = (unsigned char)str[1];
            unsigned char third = (unsigned char)str[2];
            if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80) {
                *out_codepoint = UTF8_REPLACEMENT_CHAR;
                return 0;
            }
            codepoint = ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);

            if (codepoint < 0x800 || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
                *out_codepoint = UTF8_REPLACEMENT_CHAR;
                return 0;
            }
            break;
        }

        case 4: {
            unsigned char second = (unsigned char)str[1];
            unsigned char third = (unsigned char)str[2];
            unsigned char fourth = (unsigned char)str[3];
            if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80 || (fourth & 0xC0) != 0x80) {
                *out_codepoint = UTF8_REPLACEMENT_CHAR;
                return 0;
            }
            codepoint = ((first & 0x07) << 18) | ((second & 0x3F) << 12) |
                        ((third & 0x3F) << 6) | (fourth & 0x3F);

            if (codepoint < 0x10000 || codepoint > UTF8_MAX_CODEPOINT) {
                *out_codepoint = UTF8_REPLACEMENT_CHAR;
                return 0;
            }
            break;
        }
    }

    *out_codepoint = codepoint;
    return char_len;
}

int utf8_encode(uint32_t codepoint, char* out_buffer) {
    if (codepoint > UTF8_MAX_CODEPOINT || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return 0;
    }

    if (codepoint <= 0x7F) {
        out_buffer[0] = (char)codepoint;
        return 1;
    } else if (codepoint <= 0x7FF) {
        out_buffer[0] = (char)(0xC0 | (codepoint >> 6));
        out_buffer[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    } else if (codepoint <= 0xFFFF) {
        out_buffer[0] = (char)(0xE0 | (codepoint >> 12));
        out_buffer[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out_buffer[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    } else {
        out_buffer[0] = (char)(0xF0 | (codepoint >> 18));
        out_buffer[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out_buffer[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out_buffer[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
}

int utf8_strlen(const char* str, int byte_len) {
    if (str == NULL || byte_len == 0) {
        return 0;
    }

    int char_count = 0;
    int pos = 0;

    while (pos < byte_len) {
        int char_len = utf8_charlen((unsigned char)str[pos]);
        if (char_len == 0) {
            pos++;
        } else {
            pos += char_len;
        }
        char_count++;
    }

    return char_count;
}

bool utf8_validate(const char* str, size_t byte_len) {
    size_t pos = 0;

    while (pos < byte_len) {
        uint32_t codepoint;
        int char_len = utf8_decode(str + pos, byte_len - pos, &codepoint);

        if (char_len == 0 || codepoint == UTF8_REPLACEMENT_CHAR) {
            return false;
        }

        pos += char_len;
    }

    return true;
}

int utf8_offset(const char* str, int byte_len, int char_index) {
    if (str == NULL || byte_len == 0 || char_index < 0) {
        return -1;
    }

    int current_char = 0;
    int pos = 0;

    while (pos < byte_len && current_char < char_index) {
        int char_len = utf8_charlen((unsigned char)str[pos]);
        if (char_len == 0) {
            pos++;
        } else {
            pos += char_len;
        }
        current_char++;
    }

    if (current_char == char_index && pos <= byte_len) {
        return pos;
    }

    return -1;
}

int utf8_next(const char* str, int byte_len, int current_offset) {
    if (str == NULL || current_offset >= byte_len) {
        return -1;
    }

    int char_len = utf8_charlen((unsigned char)str[current_offset]);
    if (char_len == 0) {
        char_len = 1;
    }

    int next_offset = current_offset + char_len;
    if (next_offset >= byte_len) {
        return -1;
    }

    return next_offset;
}

int utf8_prev(const char* str, int current_offset) {
    if (str == NULL || current_offset <= 0) {
        return -1;
    }

    int pos = current_offset - 1;

    while (pos > 0 && ((unsigned char)str[pos] & 0xC0) == 0x80) {
        pos--;
    }

    return pos;
}

bool utf8_substring(const char* str, int byte_len,
                    int start_char, int end_char,
                    int* out_start_byte, int* out_end_byte) {
    if (str == NULL || start_char < 0 || end_char < start_char) {
        return false;
    }

    int start_byte = utf8_offset(str, byte_len, start_char);
    if (start_byte == -1) {
        return false;
    }

    int end_byte = utf8_offset(str, byte_len, end_char);
    if (end_byte == -1) {
        end_byte = byte_len;
    }

    *out_start_byte = start_byte;
    *out_end_byte = end_byte;
    return true;
}

char* utf8_toupper(const char* str, int byte_len, int* out_len) {
    if (str == NULL || byte_len == 0) {
        *out_len = 0;
        return NULL;
    }

    char* result = (char*)malloc(byte_len + 1);
    if (result == NULL) {
        *out_len = 0;
        return NULL;
    }

    for (int i = 0; i < byte_len; i++) {
        if ((unsigned char)str[i] < 0x80) {
            result[i] = toupper((unsigned char)str[i]);
        } else {
            result[i] = str[i];
        }
    }

    result[byte_len] = '\0';
    *out_len = byte_len;
    return result;
}

char* utf8_tolower(const char* str, int byte_len, int* out_len) {
    if (str == NULL || byte_len == 0) {
        *out_len = 0;
        return NULL;
    }

    char* result = (char*)malloc(byte_len + 1);
    if (result == NULL) {
        *out_len = 0;
        return NULL;
    }

    for (int i = 0; i < byte_len; i++) {
        if ((unsigned char)str[i] < 0x80) {
            result[i] = tolower((unsigned char)str[i]);
        } else {
            result[i] = str[i];
        }
    }

    result[byte_len] = '\0';
    *out_len = byte_len;
    return result;
}
