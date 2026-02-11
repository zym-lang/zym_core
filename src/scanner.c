#include "./scanner.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void initScanner(Scanner* scanner, const char* source, const LineMap* line_map) {
    scanner->start = source;
    scanner->current = source;
    scanner->line = 1;
    scanner->line_map = line_map;
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

static char advance(Scanner* scanner) {
    scanner->current++;
    return scanner->current[-1];
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
    scanner->current++;
    return true;
}

static Token makeToken(Scanner* scanner, TokenType type) {
    Token token;
    token.type = type;
    token.start = scanner->start;
    token.length = (int)(scanner->current - scanner->start);

    if (scanner->line_map != NULL && scanner->line_map->count > 0) {
        int map_index = scanner->line - 1;
        if (map_index < scanner->line_map->count) {
            token.line = scanner->line_map->lines[map_index];
        } else {
            token.line = scanner->line_map->lines[scanner->line_map->count - 1];
        }
    } else {
        token.line = scanner->line;
    }
    return token;
}

static Token errorToken(Scanner* scanner, const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);

    if (scanner->line_map != NULL && scanner->line_map->count > 0) {
        int map_index = scanner->line - 1;
        if (map_index < scanner->line_map->count) {
            token.line = scanner->line_map->lines[map_index];
        } else {
            token.line = scanner->line_map->lines[scanner->line_map->count - 1];
        }
    } else {
        token.line = scanner->line;
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
                advance(scanner);
                break;
            case '\n':
                scanner->line++;
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
                        if (peek(scanner) == '\n') scanner->line++;
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
            KW('l', "one", TOKEN_CLONE);
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
                KW('f', "", TOKEN_REF);
                KW('t', "urn", TOKEN_RETURN);
            })
        })
        CASE('s', {
            KW('l', "ot", TOKEN_SLOT);
            KW('t', "ruct", TOKEN_STRUCT);
            KW('w', "itch", TOKEN_SWITCH);
        })
        CASE('t', {
            KW('r', "ue", TOKEN_TRUE);
            KW('y', "peof", TOKEN_TYPEOF);
        })
        CASE('v', {
            CASE('a', {
                KW('l', "", TOKEN_VAL);
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
        if (peek(scanner) == '\n') scanner->line++;
        if (peek(scanner) == '\\') {
            advance(scanner);
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
            if (match(scanner, '>')) return makeToken(scanner, TOKEN_ARROW);
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

    static char error_buf[64];
    if (c >= 32 && c < 127) {
        snprintf(error_buf, sizeof(error_buf), "Unexpected character '%c'.", c);
    } else {
        snprintf(error_buf, sizeof(error_buf), "Unexpected character (code %d).", (unsigned char)c);
    }
    return errorToken(scanner, error_buf);
}