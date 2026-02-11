#ifndef zym_utf8_h
#define zym_utf8_h

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define UTF8_MAX_CODEPOINT 0x10FFFF
#define UTF8_REPLACEMENT_CHAR 0xFFFD

int utf8_charlen(unsigned char first_byte);
int utf8_decode(const char* str, size_t len, uint32_t* out_codepoint);
int utf8_encode(uint32_t codepoint, char* out_buffer);
int utf8_strlen(const char* str, int byte_len);
bool utf8_validate(const char* str, size_t byte_len);

int utf8_offset(const char* str, int byte_len, int char_index);
int utf8_next(const char* str, int byte_len, int current_offset);
int utf8_prev(const char* str, int current_offset);

bool utf8_substring(const char* str, int byte_len,
                    int start_char, int end_char,
                    int* out_start_byte, int* out_end_byte);

char* utf8_toupper(const char* str, int byte_len, int* out_len);
char* utf8_tolower(const char* str, int byte_len, int* out_len);

#endif
