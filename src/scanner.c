#include "./scanner.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void initScanner(Scanner* scanner, const char* source, const LineMap* line_map,
                 const SourceMap* source_map, ZymFileId file_id) {
    scanner->base = source;
    scanner->file_id = file_id;
    scanner->start = source;
    scanner->current = source;
    scanner->start_byte = 0;
    scanner->start_line = 1;
    scanner->start_column = 1;
    scanner->current_line = 1;
    scanner->current_column = 1;
    scanner->line = 1;
    scanner->line_map = line_map;
    scanner->source_map = source_map;
}

bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

static bool isAtEnd(Scanner* scanner) {
    return *scanner->current == '\0';
}

// advance() moves the cursor by one byte and keeps line/column state in
// sync. `\n` bumps the line and resets the column to 1. Columns are
// counted in UTF-8 bytes (§1.4 of core_changes.md) — conversion to UTF-16
// or UTF-32 code units is a tooling-layer concern, done on demand.
static char advance(Scanner* scanner) {
    char c = scanner->current[0];
    scanner->current++;
    if (c == '\n') {
        scanner->current_line++;
        scanner->current_column = 1;
    } else {
        scanner->current_column++;
    }
    scanner->line = scanner->current_line;
    return c;
}

static char peek(Scanner* scanner) {
    return *scanner->current;
}

static char peekNext(Scanner* scanner) {
    if (isAtEnd(scanner)) return '\0';
    return scanner->current[1];
}

static bool match(Scanner* scanner, char expected) {
    if (isAtEnd(scanner)) return false;
    if (*scanner->current != expected) return false;
    advance(scanner);
    return true;
}

// Resolves a scanner's current_line through the LineMap (if present) to
// the corresponding original-source line number. This preserves the
// pre-1.1 behavior where tokens emitted from preprocessed source report
// the user's line, not the expanded-buffer line.
static int mappedLine(const Scanner* scanner, int scanner_line) {
    if (scanner->line_map == NULL || scanner->line_map->count == 0) {
        return scanner_line;
    }
    int map_index = scanner_line - 1;
    if (map_index < 0) map_index = 0;
    if (map_index >= scanner->line_map->count) {
        map_index = scanner->line_map->count - 1;
    }
    return scanner->line_map->lines[map_index];
}

static Token makeToken(Scanner* scanner, TokenType type) {
    Token token;
    token.type = type;
    token.start = scanner->start;
    token.length = (int)(scanner->current - scanner->start);

    // Legacy `line` field: mapped through the LineMap so existing
    // diagnostics continue to print the user-source line.
    token.line = mappedLine(scanner, scanner->start_line);

    // Phase 1.1 canonical span: fileId + byte offset into the preprocessed
    // buffer the scanner walks.
    token.fileId = scanner->file_id;
    token.startByte = scanner->start_byte;
    token.startLine = scanner->start_line;
    token.startColumn = scanner->start_column;
    token.endLine = scanner->current_line;
    token.endColumn = scanner->current_column;

    // Phase 1.2: resolve origin via SourceMap if present; otherwise
    // fall back to reporting the scanned file as the origin.
    if (scanner->source_map != NULL) {
        const SourceMapSegment* seg = sourcemap_lookup(scanner->source_map, scanner->start_byte);
        if (seg != NULL) {
            token.originFileId = seg->originFileId;
            token.originStartByte = seg->originStartByte;
            token.originLength = seg->originLength;
        } else {
            token.originFileId = scanner->file_id;
            token.originStartByte = scanner->start_byte;
            token.originLength = token.length;
        }
    } else {
        token.originFileId = scanner->file_id;
        token.originStartByte = scanner->start_byte;
        token.originLength = token.length;
    }
    return token;
}

static Token errorToken(Scanner* scanner, const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);

    token.line = mappedLine(scanner, scanner->current_line);

    // Error tokens don't correspond to a contiguous byte range in the
    // source; we report the current scan position as a zero-length span.
    token.fileId = scanner->file_id;
    token.startByte = (int)(scanner->current - scanner->base);
    token.startLine = scanner->current_line;
    token.startColumn = scanner->current_column;
    token.endLine = scanner->current_line;
    token.endColumn = scanner->current_column;

    if (scanner->source_map != NULL) {
        const SourceMapSegment* seg = sourcemap_lookup(scanner->source_map, token.startByte);
        if (seg != NULL) {
            token.originFileId = seg->originFileId;
            token.originStartByte = seg->originStartByte;
            token.originLength = 0;
        } else {
            token.originFileId = scanner->file_id;
            token.originStartByte = token.startByte;
            token.originLength = 0;
        }
    } else {
        token.originFileId = scanner->file_id;
        token.originStartByte = token.startByte;
        token.originLength = 0;
    }
    return token;
}

static void skipWhitespace(Scanner* scanner) {
    for (;;) {
        char c = peek(scanner);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
            case '\n':
                advance(scanner);
                break;
            case '/':
                if (peekNext(scanner) == '/') {
                    while (peek(scanner) != '\n' && !isAtEnd(scanner)) advance(scanner);
                } else if (peekNext(scanner) == '*') {
                    advance(scanner);
                    advance(scanner);
                    while (!(peek(scanner) == '*' && peekNext(scanner) == '/')) {
                        if (isAtEnd(scanner)) return;
                        advance(scanner);
                    }
                    advance(scanner);
                    advance(scanner);
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static TokenType checkKeyword(Scanner* scanner, int start, int length, const char* rest, TokenType type) {
    if (scanner->current - scanner->start == start + length && memcmp(scanner->start + start, rest, length) == 0) {
        return type;
    }
    return TOKEN_IDENTIFIER;
}

static TokenType identifierType(Scanner* scanner) {
#define CASE(ch, BODY)          \
    case ch: {                  \
        size_t _old = pos;      \
        pos = _old + 1;         \
        if (len > pos) {        \
            switch (s[pos]) {   \
                BODY            \
                default: break; \
            }                   \
        }                       \
        pos = _old;             \
    } break;
#define KW(ch, rest, token) case ch: return checkKeyword(scanner, pos + 1, sizeof(rest) - 1, rest, token)

    const char *s = scanner->start;
    size_t len = (size_t)(scanner->current - scanner->start);
    size_t pos = 0;

    switch (s[0]) {
        KW('a', "nd", TOKEN_AND);
        CASE('b', {
            KW('r', "eak", TOKEN_BREAK);
        })
        CASE('c', {
            CASE('a', {
                KW('s', "e", TOKEN_CASE);
            })
            KW('o', "ntinue", TOKEN_CONTINUE);
        })
        CASE('d', {
            KW('e', "fault", TOKEN_DEFAULT);
            KW('o', "", TOKEN_DO);
        })
        CASE('e', {
            KW('l', "se", TOKEN_ELSE);
            KW('n', "um", TOKEN_ENUM);
        })
        CASE('f', {
            KW('a', "lse", TOKEN_FALSE);
            KW('o', "r", TOKEN_FOR);
            KW('u', "nc", TOKEN_FUNC);
        })
        KW('g', "oto", TOKEN_GOTO);
        KW('i', "f", TOKEN_IF);
        KW('n', "ull", TOKEN_NULL);
        KW('o', "r", TOKEN_OR);
        CASE('r', {
            CASE('e', {
                KW('t', "urn", TOKEN_RETURN);
            })
        })
        CASE('s', {
            KW('t', "ruct", TOKEN_STRUCT);
            KW('w', "itch", TOKEN_SWITCH);
        })
        CASE('t', {
            KW('r', "ue", TOKEN_TRUE);
        })
        CASE('v', {
            CASE('a', {
                KW('r', "", TOKEN_VAR);
            })
        })
        KW('w', "hile", TOKEN_WHILE);
        default: break;
    }

    return TOKEN_IDENTIFIER;

#undef CASE
#undef KW
}


static Token identifier(Scanner* scanner) {
    while (isAlpha(peek(scanner)) || isDigit(peek(scanner))) advance(scanner);
    return makeToken(scanner, identifierType(scanner));
}

static Token number(Scanner* scanner) {
    if (scanner->start[0] == '0' && (peek(scanner) == 'x' || peek(scanner) == 'X')) {
        advance(scanner);
        while (isDigit(peek(scanner)) || (peek(scanner) >= 'a' && peek(scanner) <= 'f') || (peek(scanner) >= 'A' && peek(scanner) <= 'F') || peek(scanner) == '_') {
            advance(scanner);
        }
    } else if (scanner->start[0] == '0' && (peek(scanner) == 'b' || peek(scanner) == 'B')) {
        advance(scanner);
        while (peek(scanner) == '0' || peek(scanner) == '1' || peek(scanner) == '_') {
            advance(scanner);
        }
    } else {
        while (isDigit(peek(scanner)) || peek(scanner) == '_') advance(scanner);
        if (peek(scanner) == '.' && isDigit(peekNext(scanner))) {
            advance(scanner);
            while (isDigit(peek(scanner)) || peek(scanner) == '_') advance(scanner);
        }
    }

    return makeToken(scanner, TOKEN_NUMBER);
}

static Token string(Scanner* scanner) {
    while (peek(scanner) != '"' && !isAtEnd(scanner)) {
        if (peek(scanner) == '\\') {
            advance(scanner);
            if (isAtEnd(scanner)) break;
        }
        advance(scanner);
    }

    if (isAtEnd(scanner)) return errorToken(scanner, "Unterminated string.");

    advance(scanner);
    return makeToken(scanner, TOKEN_STRING);
}


Token scanToken(Scanner* scanner) {
    skipWhitespace(scanner);
    scanner->start = scanner->current;
    scanner->start_byte = (int)(scanner->current - scanner->base);
    scanner->start_line = scanner->current_line;
    scanner->start_column = scanner->current_column;

    if (isAtEnd(scanner)) return makeToken(scanner, TOKEN_EOF);

    char c = advance(scanner);
    if (isAlpha(c)) return identifier(scanner);
    if (isDigit(c)) return number(scanner);

    switch (c) {
        case '(': return makeToken(scanner, TOKEN_LEFT_PAREN);
        case ')': return makeToken(scanner, TOKEN_RIGHT_PAREN);
        case '{': return makeToken(scanner, TOKEN_LEFT_BRACE);
        case '}': return makeToken(scanner, TOKEN_RIGHT_BRACE);
        case '[': return makeToken(scanner, TOKEN_LEFT_BRACKET);
        case ']': return makeToken(scanner, TOKEN_RIGHT_BRACKET);
        case ';': return makeToken(scanner, TOKEN_SEMICOLON);
        case ':': return makeToken(scanner, TOKEN_COLON);
        case ',': return makeToken(scanner, TOKEN_COMMA);
        case '.':
            if (match(scanner, '.') && match(scanner, '.')) {
                return makeToken(scanner, TOKEN_DOT_DOT_DOT);
            }
            return makeToken(scanner, TOKEN_DOT);
        case '@': return makeToken(scanner, TOKEN_AT);
        case '-':
            if (match(scanner, '=')) return makeToken(scanner, TOKEN_MINUS_EQUAL);
            if (match(scanner, '-')) return makeToken(scanner, TOKEN_MINUS_MINUS);
            return makeToken(scanner, TOKEN_MINUS);
        case '+':
            if (match(scanner, '=')) return makeToken(scanner, TOKEN_PLUS_EQUAL);
            if (match(scanner, '+')) return makeToken(scanner, TOKEN_PLUS_PLUS);
            return makeToken(scanner, TOKEN_PLUS);
        case '/':
            if (match(scanner, '=')) return makeToken(scanner, TOKEN_SLASH_EQUAL);
            return makeToken(scanner, TOKEN_SLASH);
        case '*':
            if (match(scanner, '=')) return makeToken(scanner, TOKEN_STAR_EQUAL);
            return makeToken(scanner, TOKEN_STAR);
        case '%':
            if (match(scanner, '=')) return makeToken(scanner, TOKEN_PERCENT_EQUAL);
            return makeToken(scanner, TOKEN_PERCENT);
        case '?': return makeToken(scanner, TOKEN_QUESTION);
        case '!': return makeToken(scanner, match(scanner, '=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
        case '=':
            if (match(scanner, '=')) return makeToken(scanner, TOKEN_EQUAL_EQUAL);
            if (match(scanner, '>')) return makeToken(scanner, TOKEN_FAT_ARROW);
            return makeToken(scanner, TOKEN_EQUAL);
        case '&':
            if (match(scanner, '&')) return makeToken(scanner, TOKEN_AND);
            if (match(scanner, '=')) return makeToken(scanner, TOKEN_BINARY_AND_EQUAL);
            return makeToken(scanner, TOKEN_BINARY_AND);
        case '|':
            if (match(scanner, '|')) return makeToken(scanner, TOKEN_OR);
            if (match(scanner, '=')) return makeToken(scanner, TOKEN_BINARY_OR_EQUAL);
            return makeToken(scanner, TOKEN_BINARY_OR);
        case '^':
            if (match(scanner, '=')) return makeToken(scanner, TOKEN_BINARY_XOR_EQUAL);
            return makeToken(scanner, TOKEN_BINARY_XOR);
        case '~': return makeToken(scanner, TOKEN_BINARY_NOT);
        case '<':
            if (match(scanner, '<')) {
                if (match(scanner, '=')) return makeToken(scanner, TOKEN_LEFT_SHIFT_EQUAL);
                return makeToken(scanner, TOKEN_LEFT_SHIFT);
            }
            return makeToken(scanner, match(scanner, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
        case '>':
            if (match(scanner, '>')) {
                if (match(scanner, '>')) {
                    if (match(scanner, '=')) return makeToken(scanner, TOKEN_UNSIGNED_RIGHT_SHIFT_EQUAL);
                    return makeToken(scanner, TOKEN_UNSIGNED_RIGHT_SHIFT);
                }
                if (match(scanner, '=')) return makeToken(scanner, TOKEN_RIGHT_SHIFT_EQUAL);
                return makeToken(scanner, TOKEN_RIGHT_SHIFT);
            }
            return makeToken(scanner, match(scanner, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);

        case '"': return string(scanner);
    }

    if (c >= 32 && c < 127) {
        snprintf(scanner->error_buf, sizeof(scanner->error_buf), "Unexpected character '%c'.", c);
    } else {
        snprintf(scanner->error_buf, sizeof(scanner->error_buf), "Unexpected character (code %d).", (unsigned char)c);
    }
    return errorToken(scanner, scanner->error_buf);
}
