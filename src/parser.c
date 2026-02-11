#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "./parser.h"
#include "./memory.h"
#include "./vm.h"
#include "./utils.h"

typedef struct {
    VM* vm;
    Scanner scanner;
    Token current;
    Token previous;
    bool had_error;
    bool panic_mode;
    const char* current_module_name;
    int module_name_length;
} Parser;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,  // =
    PREC_TERNARY,     // ? :
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_BINARY_OR,   // |
    PREC_BINARY_XOR,  // ^
    PREC_BINARY_AND,  // &
    PREC_EQUALITY,    // == !=
    PREC_COMPARISON,  // < > <= >=
    PREC_SHIFT,       // << >>
    PREC_TERM,        // + -
    PREC_FACTOR,      // * / %
    PREC_UNARY,       // ! - ~
    PREC_CALL,        // . () []
    PREC_PRIMARY
} Precedence;

typedef Expr* (*PrefixParseFn)(Parser* parser, bool can_assign);
typedef Expr* (*InfixParseFn)(Parser* parser, Expr* left);

typedef struct {
    PrefixParseFn prefix;
    InfixParseFn  infix;
    Precedence    precedence;
} ParseRule;

static Stmt* parse_statement(Parser* parser);
static Stmt* parse_declaration(Parser* parser);
static Expr* parse_expression(Parser* parser);
static ParseRule* get_rule(TokenType type);
static void error_at_current(Parser* parser, const char* message);
static void advance(Parser* parser);
static void consume(Parser* parser, TokenType type, const char* message);
static bool match(Parser* parser, TokenType type);
static bool check(Parser* parser, TokenType type);
static bool is_statement_start(Parser* parser);
static void consume_end_of_statement(Parser* parser, const char* message);
static Expr* parse_precedence(Parser* parser, Precedence precedence);
static bool is_compound_assign_op(Parser* parser);
static void synchronize(Parser* parser);
static Expr* grouping(Parser* parser, bool can_assign);
static Expr* unary(Parser* parser, bool can_assign);
static Expr* binary(Parser* parser, Expr* left);
static Expr* ternary(Parser* parser, Expr* left);
static Expr* literal(Parser* parser, bool can_assign);
static Expr* variable(Parser* parser, bool can_assign);
static Expr* call(Parser* parser, Expr* callee);
static Expr* dot(Parser* parser, Expr* left);
static Expr* list_literal(Parser* parser, bool can_assign);
static Expr* subscript(Parser* parser, Expr* left);
static Expr* map_literal(Parser* parser, bool can_assign);
static Expr* function_expression(Parser* parser, bool can_assign);
static Expr* slot_assignment(Parser* parser, bool can_assign);
static Expr* pre_increment(Parser* parser, bool can_assign);
static Expr* pre_decrement(Parser* parser, bool can_assign);
static Expr* post_increment(Parser* parser, Expr* left);
static Expr* post_decrement(Parser* parser, Expr* left);
static Expr* typeof_expression(Parser* parser, bool can_assign);
static Stmt* parse_block(Parser* parser);
static Stmt* parse_if_statement(Parser* parser);
static Stmt* parse_while_statement(Parser* parser);
static Stmt* parse_do_while_statement(Parser* parser);
static Stmt* parse_for_statement(Parser* parser);
static Stmt* parse_jump_statement(Parser* parser);
static Stmt* parse_goto_statement(Parser* parser);
static Stmt* parse_switch_statement(Parser* parser);
static Stmt* parse_return_statement(Parser* parser);
static Stmt* function(Parser* parser, const char* kind);
static TypeSpecifier* parse_type_specifier(Parser* parser);

static const ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping,     call,      PREC_CALL},
    [TOKEN_LEFT_BRACKET]  = {list_literal, subscript, PREC_CALL},
    [TOKEN_DOT]           = {NULL,         dot,       PREC_CALL},
    [TOKEN_MINUS]         = {unary,        binary,    PREC_TERM},
    [TOKEN_PLUS]          = {NULL,         binary,    PREC_TERM},
    [TOKEN_QUESTION]      = {NULL,         ternary,   PREC_TERNARY},
    [TOKEN_SLASH]         = {NULL,         binary,    PREC_FACTOR},
    [TOKEN_STAR]          = {NULL,         binary,    PREC_FACTOR},
    [TOKEN_PERCENT]       = {NULL,         binary,    PREC_FACTOR},
    [TOKEN_BANG]          = {unary,        NULL,      PREC_NONE},
    [TOKEN_BANG_EQUAL]    = {NULL,         binary,    PREC_EQUALITY},
    [TOKEN_EQUAL_EQUAL]   = {NULL,         binary,    PREC_EQUALITY},
    [TOKEN_GREATER]       = {NULL,         binary,    PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL,         binary,    PREC_COMPARISON},
    [TOKEN_LESS]          = {NULL,         binary,    PREC_COMPARISON},
    [TOKEN_LESS_EQUAL]    = {NULL,         binary,    PREC_COMPARISON},
    [TOKEN_IDENTIFIER]    = {variable,     NULL,      PREC_NONE},
    [TOKEN_STRING]        = {literal,      NULL,      PREC_NONE},
    [TOKEN_NUMBER]        = {literal,      NULL,      PREC_NONE},
    [TOKEN_AND]           = {NULL,         binary,   PREC_AND},
    [TOKEN_OR]            = {NULL,         binary,   PREC_OR},
    [TOKEN_FALSE]         = {literal,      NULL,      PREC_NONE},
    [TOKEN_TRUE]          = {literal,      NULL,      PREC_NONE},
    [TOKEN_NULL]          = {literal,      NULL,      PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {map_literal,  NULL,      PREC_NONE},
    [TOKEN_FUNC]          = {function_expression, NULL, PREC_NONE},
    [TOKEN_REF]           = {unary,        NULL,      PREC_NONE},
    [TOKEN_SLOT]          = {slot_assignment, NULL,   PREC_NONE},
    [TOKEN_VAL]           = {unary,        NULL,      PREC_NONE},
    [TOKEN_CLONE]         = {unary,        NULL,      PREC_NONE},
    [TOKEN_PLUS_PLUS]     = {pre_increment, post_increment, PREC_CALL},
    [TOKEN_MINUS_MINUS]   = {pre_decrement, post_decrement, PREC_CALL},
    [TOKEN_TYPEOF]        = {typeof_expression, NULL, PREC_NONE},
    [TOKEN_BINARY_AND]             = {NULL,         binary,    PREC_BINARY_AND},
    [TOKEN_BINARY_OR]              = {NULL,         binary,    PREC_BINARY_OR},
    [TOKEN_BINARY_XOR]             = {NULL,         binary,    PREC_BINARY_XOR},
    [TOKEN_LEFT_SHIFT]             = {NULL,         binary,    PREC_SHIFT},
    [TOKEN_RIGHT_SHIFT]            = {NULL,         binary,    PREC_SHIFT},
    [TOKEN_UNSIGNED_RIGHT_SHIFT]   = {NULL,         binary,    PREC_SHIFT},
    [TOKEN_BINARY_NOT]             = {unary,        NULL,      PREC_NONE},
    [TOKEN_EOF]           = {NULL,         NULL,      PREC_NONE},
};
static void advance(Parser* parser) {
    parser->previous = parser->current;
    for (;;) {
        parser->current = scanToken(&parser->scanner);
        if (parser->current.type != TOKEN_ERROR) break;
        error_at_current(parser, parser->current.start);
    }
}

static void error_at_current(Parser* parser, const char* message) {
    if (parser->panic_mode) return;
    parser->panic_mode = true;

    if (parser->current_module_name) {
        char* decoded = decodeModulePath(parser->current_module_name, parser->module_name_length);
        fprintf(stderr, "[%s] line %d", decoded, parser->current.line);
        free(decoded);
    } else {
        fprintf(stderr, "[line %d]", parser->current.line);
    }

    if (parser->current.type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (parser->current.type != TOKEN_ERROR) {
        if (parser->current.start != NULL && parser->current.length > 0) {
            const int MAX_TOKEN_DISPLAY = 40;
            if (parser->current.length <= MAX_TOKEN_DISPLAY) {
                fprintf(stderr, " at '%.*s'", parser->current.length, parser->current.start);
            } else {
                fprintf(stderr, " at '%.*s...'", MAX_TOKEN_DISPLAY, parser->current.start);
            }
        }
    }

    fprintf(stderr, ": %s\n", message);
    parser->had_error = true;
}

static void consume(Parser* parser, TokenType type, const char* message) {
    if (parser->current.type == type) {
        advance(parser);
        return;
    }
    error_at_current(parser, message);
}

static bool match(Parser* parser, TokenType type) {
    if (parser->current.type != type) return false;
    advance(parser);
    return true;
}

static bool check(Parser* parser, TokenType type) {
    return parser->current.type == type;
}

static bool is_compound_assign_op(Parser* parser) {
    return match(parser, TOKEN_PLUS_EQUAL) || match(parser, TOKEN_MINUS_EQUAL) ||
           match(parser, TOKEN_STAR_EQUAL) || match(parser, TOKEN_SLASH_EQUAL) ||
           match(parser, TOKEN_PERCENT_EQUAL) || match(parser, TOKEN_BINARY_AND_EQUAL) ||
           match(parser, TOKEN_BINARY_OR_EQUAL) || match(parser, TOKEN_BINARY_XOR_EQUAL) ||
           match(parser, TOKEN_LEFT_SHIFT_EQUAL) || match(parser, TOKEN_RIGHT_SHIFT_EQUAL) ||
           match(parser, TOKEN_UNSIGNED_RIGHT_SHIFT_EQUAL);
}

static bool is_statement_start(Parser* parser) {
    switch (parser->current.type) {
        case TOKEN_RETURN:
        case TOKEN_IF:
        case TOKEN_WHILE:
        case TOKEN_FOR:
        case TOKEN_SWITCH:
        case TOKEN_LEFT_BRACE:
        case TOKEN_BREAK:
        case TOKEN_CONTINUE:
        case TOKEN_FUNC:
        case TOKEN_VAR:
        case TOKEN_AT: // Compiler directive
            return true;
        default:
            return false;
    }
}

static void consume_end_of_statement(Parser* parser, const char* message) {
    if (match(parser, TOKEN_SEMICOLON)) return;
    if (parser->current.line > parser->previous.line) return;
    if (check(parser, TOKEN_EOF)) return;
    if (check(parser, TOKEN_RIGHT_BRACE)) return;
    if (is_statement_start(parser)) return;
    error_at_current(parser, message);
}
static void synchronize(Parser* parser) {
    parser->panic_mode = false;

    while (parser->current.type != TOKEN_EOF) {
        if (parser->previous.type == TOKEN_SEMICOLON) return;

        if (parser->current.line > parser->previous.line) {
            switch (parser->current.type) {
                case TOKEN_FUNC:
                case TOKEN_VAR:
                case TOKEN_FOR:
                case TOKEN_IF:
                case TOKEN_WHILE:
                case TOKEN_SWITCH:
                case TOKEN_RETURN:
                case TOKEN_STRUCT:
                case TOKEN_ENUM:
                case TOKEN_AT:
                    return;
                default:
                    ;
            }
        }

        advance(parser);
    }
}
static ParseRule* get_rule(TokenType type) { return &rules[type]; }

static Expr* parse_precedence(Parser* parser, Precedence precedence) {
    advance(parser);
    PrefixParseFn prefix_rule = get_rule(parser->previous.type)->prefix;
    if (prefix_rule == NULL) {
        error_at_current(parser, "Expect expression.");
        return NULL;
    }
    bool can_assign = precedence <= PREC_ASSIGNMENT;
    Expr* left_expr = prefix_rule(parser, can_assign);

    while (precedence <= get_rule(parser->current.type)->precedence) {
        // Don't treat '(' as call operator if it's on a new line
        if (parser->current.type == TOKEN_LEFT_PAREN &&
            parser->current.line > parser->previous.line) {
            break;
        }

        advance(parser);
        InfixParseFn infix_rule = get_rule(parser->previous.type)->infix;
        left_expr = infix_rule(parser, left_expr);
    }

    if (can_assign && match(parser, TOKEN_EQUAL)) {
        error_at_current(parser, "Invalid assignment target.");
    }

    return left_expr;
}

static Expr* grouping(Parser* parser, bool can_assign) {
    Token paren = parser->previous;
    if (parser->current.type == TOKEN_RIGHT_PAREN) {
        advance(parser);
        if (parser->current.type == TOKEN_FAT_ARROW) {
            advance(parser);
            Stmt* body;
            if (parser->current.type == TOKEN_LEFT_BRACE) {
                advance(parser);
                body = parse_block(parser);
            } else {
                Expr* expr = parse_expression(parser);
                Stmt* return_stmt = new_return_stmt(parser->vm, paren, expr);
                Stmt** statements = ALLOCATE(parser->vm, Stmt*, 1);
                statements[0] = return_stmt;
                body = new_block_stmt(parser->vm, statements, 1, 1, paren);
            }

            return new_function_expr(parser->vm, NULL, 0, 0, body, NULL, paren);
        }
        error_at_current(parser, "Expect expression.");
        return NULL;
    }

    if (parser->current.type == TOKEN_IDENTIFIER) {
        Token saved_current = parser->current;
        Token saved_previous = parser->previous;
        Scanner saved_scanner = parser->scanner;

        bool looks_like_arrow_function = false;
        int lookahead_depth = 0;
        while (parser->current.type == TOKEN_IDENTIFIER || parser->current.type == TOKEN_COMMA) {
            if (parser->current.type == TOKEN_IDENTIFIER) {
                advance(parser);
                lookahead_depth++;
                if (parser->current.type == TOKEN_COLON) {
                    advance(parser);
                    while (parser->current.type == TOKEN_IDENTIFIER ||
                           parser->current.type == TOKEN_LEFT_BRACKET ||
                           parser->current.type == TOKEN_RIGHT_BRACKET) {
                        advance(parser);
                    }
                }
            } else if (parser->current.type == TOKEN_COMMA) {
                advance(parser);
            }
        }

        if (parser->current.type == TOKEN_RIGHT_PAREN) {
            advance(parser);
            if (parser->current.type == TOKEN_FAT_ARROW) {
                looks_like_arrow_function = true;
            }
        }

        parser->current = saved_current;
        parser->previous = saved_previous;
        parser->scanner = saved_scanner;
        if (!looks_like_arrow_function) {
            goto parse_as_expression;
        }

        Param* params = NULL;
        int param_count = 0;
        int param_capacity = 0;

        do {
            if (parser->current.type != TOKEN_IDENTIFIER) {
                goto parse_as_expression;
            }

            if (param_count + 1 > param_capacity) {
                int old_capacity = param_capacity;
                param_capacity = GROW_CAPACITY(old_capacity);
                params = GROW_ARRAY(parser->vm, Param, params, old_capacity, param_capacity);
            }

            advance(parser);
            params[param_count].name = parser->previous;
            params[param_count].type = NULL;
            params[param_count].qualifier = PARAM_NORMAL;

            if (match(parser, TOKEN_COLON)) {
                params[param_count].type = parse_type_specifier(parser);
            }

            param_count++;
        } while (match(parser, TOKEN_COMMA));

        if (parser->current.type == TOKEN_RIGHT_PAREN) {
            advance(parser);

            if (parser->current.type == TOKEN_FAT_ARROW) {
                advance(parser);

                Stmt* body;
                if (parser->current.type == TOKEN_LEFT_BRACE) {
                    advance(parser);
                    body = parse_block(parser);
                } else {
                    Expr* expr = parse_expression(parser);
                    Stmt* return_stmt = new_return_stmt(parser->vm, paren, expr);
                    Stmt** statements = ALLOCATE(parser->vm, Stmt*, 1);
                    statements[0] = return_stmt;
                    body = new_block_stmt(parser->vm, statements, 1, 1, paren);
                }

                return new_function_expr(parser->vm, params, param_count, param_capacity, body, NULL, paren);
            }
        }

        error_at_current(parser, "Invalid syntax after parameter-like sequence.");
        if (params) FREE_ARRAY(parser->vm, Param, params, param_capacity);
        return NULL;
    }

parse_as_expression:
    Expr* expr = parse_expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");

    if (parser->current.type == TOKEN_FAT_ARROW) {
        error_at_current(parser, "Arrow function requires parameter list.");
        return expr;
    }

    return new_grouping_expr(parser->vm, expr);
}

static Expr* unary(Parser* parser, bool can_assign) {
    Token operator = parser->previous;
    Expr* right = parse_precedence(parser, PREC_UNARY);
    return new_unary_expr(parser->vm, operator, right);
}

static Expr* literal(Parser* parser, bool can_assign) {
    return new_literal_expr(parser->vm, parser->previous);
}

static Expr* variable(Parser* parser, bool can_assign) {
    Token name = parser->previous;

    if (check(parser, TOKEN_LEFT_BRACE)) {
        Parser saved = *parser;
        advance(parser);

        bool looks_like_struct = false;
        if (parser->current.type == TOKEN_RIGHT_BRACE) {
            looks_like_struct = true;
        } else if (parser->current.type == TOKEN_DOT_DOT_DOT) {
            looks_like_struct = true;
        } else if (parser->current.type == TOKEN_DOT) {
            advance(parser);
            if (parser->current.type == TOKEN_IDENTIFIER) {
                advance(parser);
                if (parser->current.type == TOKEN_EQUAL) {
                    looks_like_struct = true;
                }
            }
        }

        *parser = saved;

        if (!looks_like_struct) {
            return new_variable_expr(parser->vm, name);
        }
        Token brace = parser->current;
        advance(parser);

        int field_count = 0;
        int field_capacity = 0;
        Token* field_names = NULL;
        Expr** field_values = NULL;

        if (parser->current.type != TOKEN_RIGHT_BRACE) {
            do {
                if (field_count + 1 > field_capacity) {
                    int old_capacity = field_capacity;
                    field_capacity = GROW_CAPACITY(old_capacity);
                    field_names = GROW_ARRAY(parser->vm, Token, field_names, old_capacity, field_capacity);
                    field_values = GROW_ARRAY(parser->vm, Expr*, field_values, old_capacity, field_capacity);
                }

                if (match(parser, TOKEN_DOT_DOT_DOT)) {
                    Token spread_token = parser->previous;
                    Expr* spread_expr = parse_expression(parser);
                    field_names[field_count].type = TOKEN_DOT_DOT_DOT;
                    field_names[field_count].start = spread_token.start;
                    field_names[field_count].length = spread_token.length;
                    field_names[field_count].line = spread_token.line;
                    field_values[field_count] = new_spread_expr(parser->vm, spread_expr, spread_token);
                    field_count++;
                } else {
                    consume(parser, TOKEN_DOT, "Expect '.' before field name in struct initialization.");

                    if (parser->current.type != TOKEN_IDENTIFIER) {
                        error_at_current(parser, "Expect field name in struct initialization.");
                        break;
                    }
                    advance(parser);
                    field_names[field_count] = parser->previous;

                    consume(parser, TOKEN_EQUAL, "Expect '=' after field name in struct initialization.");
                    field_values[field_count] = parse_expression(parser);
                    field_count++;
                }
                if (!match(parser, TOKEN_COMMA)) break;
                if (parser->current.type == TOKEN_RIGHT_BRACE) break;
            } while (true);
        }
        consume(parser, TOKEN_RIGHT_BRACE, "Expect '}' after struct initialization.");
        return new_struct_inst_expr(parser->vm, name, field_names, field_values, field_count, field_capacity, brace);
    }

    if (can_assign && match(parser, TOKEN_EQUAL)) {
        Expr* value = parse_expression(parser);
        Expr* target = new_variable_expr(parser->vm, name);
        return new_assign_expr(parser->vm, target, value, false);
    }
    if (can_assign && is_compound_assign_op(parser)) {
        Token operator = parser->previous;
        Expr* value = parse_precedence(parser, PREC_TERNARY);
        Expr* target = new_variable_expr(parser->vm, name);
        Expr* get_expr = new_variable_expr(parser->vm, name);
        Expr* binary_expr = new_binary_expr(parser->vm, get_expr, operator, value);
        return new_assign_expr(parser->vm, target, binary_expr, false);
    }
    return new_variable_expr(parser->vm, name);
}

static Expr* binary(Parser* parser, Expr* left) {
    Token operator = parser->previous;
    ParseRule* rule = get_rule(operator.type);
    Expr* right = parse_precedence(parser, (Precedence)(rule->precedence + 1));
    return new_binary_expr(parser->vm, left, operator, right);
}

static Expr* ternary(Parser* parser, Expr* left) {
    Expr* then_expr = parse_precedence(parser, PREC_TERNARY);
    consume(parser, TOKEN_COLON, "Expect ':' after then branch of ternary expression.");
    Expr* else_expr = parse_precedence(parser, PREC_TERNARY);
    return new_ternary_expr(parser->vm, left, then_expr, else_expr);
}

static Expr* parse_expression(Parser* parser) {
    return parse_precedence(parser, PREC_ASSIGNMENT);
}

static Expr* call(Parser* parser, Expr* callee) {
    int arg_count = 0;
    int arg_cap   = 0;
    Expr** args   = NULL;

    if (parser->current.type != TOKEN_RIGHT_PAREN) {
        do {
            if (arg_count >= 255) {
                error_at_current(parser, "Can't have more than 255 arguments.");
            }
            if (parser->current.type == TOKEN_REF || parser->current.type == TOKEN_VAL) {
                error_at_current(parser, "Cannot explicitly use 'ref' or 'val' in function call arguments.");
                advance(parser);
            }
            if (arg_count + 1 > arg_cap) {
                int old_cap = arg_cap;
                arg_cap = GROW_CAPACITY(old_cap);
                args = GROW_ARRAY(parser->vm, Expr*, args, old_cap, arg_cap);
            }
            args[arg_count++] = parse_expression(parser);
        } while (match(parser, TOKEN_COMMA));
    }

    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    return new_call_expr(parser->vm, callee, parser->previous, args, arg_count, arg_cap);
}

static Expr* dot(Parser* parser, Expr* left) {
    if (parser->current.type != TOKEN_IDENTIFIER) {
        bool is_usable_keyword = false;
        switch (parser->current.type) {
            case TOKEN_FUNC:
            case TOKEN_VAR:
            case TOKEN_IF:
            case TOKEN_ELSE:
            case TOKEN_WHILE:
            case TOKEN_FOR:
            case TOKEN_SWITCH:
            case TOKEN_RETURN:
            case TOKEN_BREAK:
            case TOKEN_CONTINUE:
            case TOKEN_STRUCT:
            case TOKEN_ENUM:
            case TOKEN_REF:
            case TOKEN_VAL:
            case TOKEN_CLONE:
            case TOKEN_SLOT:
            case TOKEN_TYPEOF:
            case TOKEN_NULL:
            case TOKEN_TRUE:
            case TOKEN_FALSE:
            case TOKEN_AND:
            case TOKEN_OR:
            case TOKEN_DO:
            case TOKEN_GOTO:
                is_usable_keyword = true;
                advance(parser);
                break;
            default:
                consume(parser, TOKEN_IDENTIFIER, "Expect property name after '.'.");
                if (parser->panic_mode) {
                    return left;
                }
                break;
        }
    } else {
        advance(parser);
    }

    Token name = parser->previous;

    if (match(parser, TOKEN_EQUAL)) {
        Expr* value = parse_expression(parser);
        return new_set_expr(parser->vm, left, name, value, false);
    }
    if (is_compound_assign_op(parser)) {
        Token operator = parser->previous;
        Expr* value = parse_precedence(parser, PREC_TERNARY);
        Expr* get_expr = new_get_expr(parser->vm, clone_expr(parser->vm, left), name);
        Expr* binary_expr = new_binary_expr(parser->vm, get_expr, operator, value);
        return new_set_expr(parser->vm, left, name, binary_expr, false);
    }
    return new_get_expr(parser->vm, left, name);
}

static Expr* slot_assignment(Parser* parser, bool can_assign) {
    advance(parser);
    Token name = parser->previous;

    Expr* target = new_variable_expr(parser->vm, name);
    Expr* last_object = NULL;
    Token last_property_name;
    bool ends_with_property = false;
    while (true) {
        if (match(parser, TOKEN_DOT)) {
            consume(parser, TOKEN_IDENTIFIER, "Expect property name after '.'.");
            Token property = parser->previous;
            last_object = target;
            last_property_name = property;
            ends_with_property = true;
            target = new_get_expr(parser->vm, target, property);
        } else if (match(parser, TOKEN_LEFT_BRACKET)) {
            Expr* index = parse_expression(parser);
            consume(parser, TOKEN_RIGHT_BRACKET, "Expect ']' after subscript index.");
            Token bracket = parser->previous;
            ends_with_property = false;
            target = new_subscript_expr(parser->vm, target, bracket, index);
        } else {
            break;
        }
    }

    if (!match(parser, TOKEN_EQUAL)) {
        error_at_current(parser, "Expect '=' after 'slot' target.");
        return target;
    }

    Expr* value = parse_expression(parser);

    if (ends_with_property) {
        return new_set_expr(parser->vm, last_object, last_property_name, value, true);
    } else {
        return new_assign_expr(parser->vm, target, value, true);
    }
}

static Expr* pre_increment(Parser* parser, bool can_assign) {
    Token operator = parser->previous;
    Expr* target = parse_precedence(parser, PREC_UNARY);
    return new_pre_inc_expr(parser->vm, target, operator);
}

static Expr* pre_decrement(Parser* parser, bool can_assign) {
    Token operator = parser->previous;
    Expr* target = parse_precedence(parser, PREC_UNARY);
    return new_pre_dec_expr(parser->vm, target, operator);
}

static Expr* post_increment(Parser* parser, Expr* left) {
    Token operator = parser->previous;
    return new_post_inc_expr(parser->vm, left, operator);
}

static Expr* post_decrement(Parser* parser, Expr* left) {
    Token operator = parser->previous;
    return new_post_dec_expr(parser->vm, left, operator);
}

static Expr* typeof_expression(Parser* parser, bool can_assign) {
    Token operator = parser->previous;
    Expr* operand = parse_precedence(parser, PREC_UNARY);
    return new_typeof_expr(parser->vm, operand, operator);
}

static Expr* list_literal(Parser* parser, bool can_assign) {
    Token bracket = parser->previous;
    int count = 0;
    int capacity = 0;
    Expr** elements = NULL;

    if (parser->current.type != TOKEN_RIGHT_BRACKET) {
        do {
            if (count + 1 > capacity) {
                int old_capacity = capacity;
                capacity = GROW_CAPACITY(old_capacity);
                elements = GROW_ARRAY(parser->vm, Expr*, elements, old_capacity, capacity);
            }
            if (match(parser, TOKEN_DOT_DOT_DOT)) {
                Token spread_token = parser->previous;
                Expr* spread_expr = parse_expression(parser);
                elements[count++] = new_spread_expr(parser->vm, spread_expr, spread_token);
            } else {
                elements[count++] = parse_expression(parser);
            }
            if (!match(parser, TOKEN_COMMA)) break;
            if (parser->current.type == TOKEN_RIGHT_BRACKET) break;
        } while (true);
    }
    consume(parser, TOKEN_RIGHT_BRACKET, "Expect ']' after list elements.");
    return new_list_expr(parser->vm, elements, count, capacity, bracket);
}

static Expr* subscript(Parser* parser, Expr* left) {
    Token bracket = parser->previous;
    Expr* index = parse_expression(parser);
    consume(parser, TOKEN_RIGHT_BRACKET, "Expect ']' after subscript index.");

    if (match(parser, TOKEN_EQUAL)) {
        Expr* value = parse_expression(parser);
        Expr* target = new_subscript_expr(parser->vm, left, bracket, index);
        return new_assign_expr(parser->vm, target, value, false);
    }
    if (is_compound_assign_op(parser)) {
        Token operator = parser->previous;
        Expr* value = parse_precedence(parser, PREC_TERNARY);
        Expr* get_expr = new_subscript_expr(parser->vm, clone_expr(parser->vm, left), bracket, clone_expr(parser->vm, index));
        Expr* binary_expr = new_binary_expr(parser->vm, get_expr, operator, value);
        Expr* target = new_subscript_expr(parser->vm, left, bracket, index);
        return new_assign_expr(parser->vm, target, binary_expr, false);
    }
    return new_subscript_expr(parser->vm, left, bracket, index);
}

static Expr* map_literal(Parser* parser, bool can_assign) {
    Token brace = parser->previous;
    int count = 0;
    int capacity = 0;
    Expr** keys = NULL;
    Expr** values = NULL;

    if (parser->current.type != TOKEN_RIGHT_BRACE) {
        do {
            if (count + 1 > capacity) {
                int old_capacity = capacity;
                capacity = GROW_CAPACITY(old_capacity);
                keys = GROW_ARRAY(parser->vm, Expr*, keys, old_capacity, capacity);
                values = GROW_ARRAY(parser->vm, Expr*, values, old_capacity, capacity);
            }

            if (match(parser, TOKEN_DOT_DOT_DOT)) {
                Token spread_token = parser->previous;
                Expr* spread_expr = parse_expression(parser);
                keys[count] = new_spread_expr(parser->vm, spread_expr, spread_token);
                values[count] = NULL;
                count++;
            } else {
                Expr* key;
                if (match(parser, TOKEN_LEFT_PAREN)) {
                    key = parse_expression(parser);
                    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after expression key.");
                } else if (parser->current.type == TOKEN_NUMBER) {
                    advance(parser);
                    key = new_literal_expr(parser->vm, parser->previous);
                } else if (parser->current.type == TOKEN_STRING) {
                    advance(parser);
                    key = new_literal_expr(parser->vm, parser->previous);
                } else if (parser->current.type == TOKEN_IDENTIFIER) {
                    advance(parser);
                    key = new_literal_expr(parser->vm, parser->previous);
                } else {
                    error_at_current(parser, "Expect key (string, number, or expression in parentheses).");
                    return NULL;
                }

                consume(parser, TOKEN_COLON, "Expect ':' after map key.");
                Expr* value = parse_expression(parser);

                keys[count] = key;
                values[count] = value;
                count++;
            }
            if (!match(parser, TOKEN_COMMA)) break;
            if (parser->current.type == TOKEN_RIGHT_BRACE) break;
        } while (true);
    }
    consume(parser, TOKEN_RIGHT_BRACE, "Expect '}' after map elements.");
    return new_map_expr(parser->vm, keys, values, count, capacity, brace);
}

static Expr* function_expression(Parser* parser, bool can_assign) {
    Token func_token = parser->previous;
    consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'func' in function expression.");

    Param* params = NULL;
    int param_count = 0;
    int param_capacity = 0;
    if (parser->current.type != TOKEN_RIGHT_PAREN) {
        do {
            if (param_count >= 255) {
                error_at_current(parser, "Can't have more than 255 parameters.");
            }

            if (param_count + 1 > param_capacity) {
                int old_capacity = param_capacity;
                param_capacity = GROW_CAPACITY(old_capacity);
                params = GROW_ARRAY(parser->vm, Param, params, old_capacity, param_capacity);
            }
            ParamQualifier qualifier = PARAM_NORMAL;
            if (match(parser, TOKEN_REF)) {
                qualifier = PARAM_REF;
            } else if (match(parser, TOKEN_VAL)) {
                qualifier = PARAM_VAL;
            } else if (match(parser, TOKEN_CLONE)) {
                qualifier = PARAM_CLONE;
            } else if (match(parser, TOKEN_SLOT)) {
                qualifier = PARAM_SLOT;
            } else if (match(parser, TOKEN_TYPEOF)) {
                qualifier = PARAM_TYPEOF;
            }

            consume(parser, TOKEN_IDENTIFIER, "Expect parameter name.");

            params[param_count].name = parser->previous;
            params[param_count].qualifier = qualifier;

            if (match(parser, TOKEN_COLON)) {
                params[param_count].type = parse_type_specifier(parser);
            } else {
                params[param_count].type = NULL;
            }
            param_count++;
        } while (match(parser, TOKEN_COMMA));
    }
    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

    TypeSpecifier* return_type = NULL;
    if (match(parser, TOKEN_ARROW)) {
        return_type = parse_type_specifier(parser);
    }

    consume(parser, TOKEN_LEFT_BRACE, "Expect '{' before function body.");
    Stmt* body = parse_block(parser);
    return new_function_expr(parser->vm, params, param_count, param_capacity, body, return_type, func_token);
}
static TypeSpecifier* parse_type_specifier(Parser* parser) {
    if (match(parser, TOKEN_LEFT_BRACKET)) {
        TypeSpecifier* element_type = NULL;
        Expr* size = NULL;

        if (match(parser, TOKEN_RIGHT_BRACKET)) {
            return new_list_type_spec(parser->vm, NULL, NULL);
        }

        element_type = parse_type_specifier(parser);

        if (match(parser, TOKEN_SEMICOLON)) {
            size = parse_expression(parser);
        }

        consume(parser, TOKEN_RIGHT_BRACKET, "Expect ']' after list type specifier.");
        TypeSpecifier* list_type = new_list_type_spec(parser->vm, element_type, size);

        while (match(parser, TOKEN_LEFT_BRACKET)) {
            consume(parser, TOKEN_RIGHT_BRACKET, "Expect ']' for nested list type.");
            list_type = new_list_type_spec(parser->vm, list_type, NULL);
        }
        return list_type;
    }

    consume(parser, TOKEN_IDENTIFIER, "Expect type name.");
    return new_simple_type_spec(parser->vm, parser->previous);
}
static Stmt* parse_var_declaration(Parser* parser) {
    Token keyword = parser->previous;
    int count = 0;
    int capacity = 0;
    VarDecl* variables = NULL;

    do {
        if (count + 1 > capacity) {
            int old_capacity = capacity;
            capacity = GROW_CAPACITY(old_capacity);
            variables = GROW_ARRAY(parser->vm, VarDecl, variables, old_capacity, capacity);
        }

        consume(parser, TOKEN_IDENTIFIER, "Expect variable name.");
        Token name = parser->previous;

        TypeSpecifier* type = NULL;
        if (match(parser, TOKEN_COLON)) {
            type = parse_type_specifier(parser);
        }

        Expr* initializer = NULL;
        VarQualifier qualifier = VAR_NORMAL;
        if (match(parser, TOKEN_EQUAL)) {
            if (match(parser, TOKEN_REF)) {
                qualifier = VAR_REF;
            } else if (match(parser, TOKEN_VAL)) {
                qualifier = VAR_VAL;
            } else if (match(parser, TOKEN_CLONE)) {
                qualifier = VAR_CLONE;
            }
            initializer = parse_expression(parser);
        }

        variables[count].name = name;
        variables[count].type = type;
        variables[count].initializer = initializer;
        variables[count].qualifier = qualifier;
        count++;
    } while (match(parser, TOKEN_COMMA));

    consume_end_of_statement(parser, "Expect ';' after variable declaration.");
    return new_var_decl_stmt(parser->vm, variables, count, capacity, keyword);
}

static Stmt* function(Parser* parser, const char* kind) {
    consume(parser, TOKEN_IDENTIFIER, "Expect function name.");
    Token name = parser->previous;

    const char* saved_module_name = parser->current_module_name;
    int saved_module_name_length = parser->module_name_length;

    if (name.length > 9 && memcmp(name.start, "__module_", 9) == 0) {
        parser->current_module_name = name.start + 9;
        parser->module_name_length = name.length - 9;
    }

    consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after function name.");

    Param* params = NULL;
    int param_count = 0;
    int param_capacity = 0;
    if (parser->current.type != TOKEN_RIGHT_PAREN) {
        do {
            if (param_count >= 255) {
                error_at_current(parser, "Can't have more than 255 parameters.");
            }

            if (param_count + 1 > param_capacity) {
                int old_capacity = param_capacity;
                param_capacity = GROW_CAPACITY(old_capacity);
                params = GROW_ARRAY(parser->vm, Param, params, old_capacity, param_capacity);
            }

            ParamQualifier qualifier = PARAM_NORMAL;
            if (match(parser, TOKEN_REF)) {
                qualifier = PARAM_REF;
            } else if (match(parser, TOKEN_VAL)) {
                qualifier = PARAM_VAL;
            } else if (match(parser, TOKEN_CLONE)) {
                qualifier = PARAM_CLONE;
            } else if (match(parser, TOKEN_SLOT)) {
                qualifier = PARAM_SLOT;
            } else if (match(parser, TOKEN_TYPEOF)) {
                qualifier = PARAM_TYPEOF;
            }

            consume(parser, TOKEN_IDENTIFIER, "Expect parameter name.");

            params[param_count].name = parser->previous;
            params[param_count].qualifier = qualifier;

            if (match(parser, TOKEN_COLON)) {
                params[param_count].type = parse_type_specifier(parser);
            } else {
                params[param_count].type = NULL;
            }
            param_count++;
        } while (match(parser, TOKEN_COMMA));
    }
    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

    TypeSpecifier* return_type = NULL;
    if (match(parser, TOKEN_ARROW)) {
        return_type = parse_type_specifier(parser);
    }

    consume(parser, TOKEN_LEFT_BRACE, "Expect '{' before function body.");
    Stmt* body = parse_block(parser);

    parser->current_module_name = saved_module_name;
    parser->module_name_length = saved_module_name_length;

    return new_func_decl_stmt(parser->vm, name, params, param_count, param_capacity, body, return_type);
}

static Stmt* parse_compiler_directive(Parser* parser) {
    Token at_token = parser->previous;

    if (parser->current.type != TOKEN_IDENTIFIER) {
        printf("[Line %d] Error: Expected directive name after '@'.\n", at_token.line);
        return NULL;
    }

    advance(parser);
    Token directive_name = parser->previous;

    if (directive_name.length == 3 && memcmp(directive_name.start, "tco", 3) == 0) {
        if (parser->current.type != TOKEN_IDENTIFIER) {
            printf("[Line %d] Error: Expected TCO mode (aggressive/safe/off) after '@tco'.\n", directive_name.line);
            return NULL;
        }

        advance(parser);
        Token mode_token = parser->previous;

        bool valid = false;
        if (mode_token.length == 10 && memcmp(mode_token.start, "aggressive", 10) == 0) valid = true;
        else if (mode_token.length == 5 && memcmp(mode_token.start, "smart", 5) == 0) valid = true;
        else if (mode_token.length == 4 && memcmp(mode_token.start, "safe", 4) == 0) valid = true;
        else if (mode_token.length == 3 && memcmp(mode_token.start, "off", 3) == 0) valid = true;

        if (!valid) {
            printf("[Line %d] Error: Invalid TCO mode. Expected 'aggressive', 'smart', 'safe', or 'off'.\n", mode_token.line);
            return NULL;
        }

        Stmt* stmt = ALLOCATE(parser->vm, Stmt, 1);
        stmt->type = STMT_COMPILER_DIRECTIVE;
        stmt->line = at_token.line;
        stmt->keyword = at_token;
        stmt->as.compiler_directive.type = DIRECTIVE_TCO;
        stmt->as.compiler_directive.argument = mode_token;

        return stmt;
    }


    printf("[Line %d] Error: Unknown compiler directive '@%.*s'.\n", directive_name.line, directive_name.length, directive_name.start);
    return NULL;
}

static Stmt* parse_statement(Parser* parser) {
    if (match(parser, TOKEN_AT)) return parse_compiler_directive(parser);
    if (match(parser, TOKEN_RETURN))  return parse_return_statement(parser);
    if (match(parser, TOKEN_IF)) return parse_if_statement(parser);
    if (match(parser, TOKEN_WHILE))return parse_while_statement(parser);
    if (match(parser, TOKEN_DO)) return parse_do_while_statement(parser);
    if (match(parser, TOKEN_FOR)) return parse_for_statement(parser);
    if (match(parser, TOKEN_SWITCH)) return parse_switch_statement(parser);
    if (match(parser, TOKEN_LEFT_BRACE)) return parse_block(parser);
    if (match(parser, TOKEN_BREAK) || match(parser, TOKEN_CONTINUE)) return parse_jump_statement(parser);
    if (match(parser, TOKEN_GOTO)) return parse_goto_statement(parser);

    if (check(parser, TOKEN_IDENTIFIER)) {
        Scanner saved_scanner = parser->scanner;
        Token next_token = scanToken(&parser->scanner);
        parser->scanner = saved_scanner;

        if (next_token.type == TOKEN_COLON) {
            Token label_name = parser->current;
            advance(parser);
            advance(parser);
            return new_label_stmt(parser->vm, label_name);
        }
    }

    Expr* expr = parse_expression(parser);
    consume_end_of_statement(parser, "Expect ';' after expression.");
    return new_expression_stmt(parser->vm, expr);
}

static Stmt* parse_struct_declaration(Parser* parser) {
    Token keyword = parser->previous;
    consume(parser, TOKEN_IDENTIFIER, "Expect struct name.");
    Token name = parser->previous;

    consume(parser, TOKEN_LEFT_BRACE, "Expect '{' after struct name.");

    int field_count = 0;
    int field_capacity = 0;
    Token* fields = NULL;

    while (parser->current.type != TOKEN_RIGHT_BRACE && parser->current.type != TOKEN_EOF) {
        if (parser->current.type != TOKEN_IDENTIFIER) {
            error_at_current(parser, "Expect field name in struct declaration.");
            break;
        }

        if (field_count + 1 > field_capacity) {
            int old_capacity = field_capacity;
            field_capacity = GROW_CAPACITY(old_capacity);
            fields = GROW_ARRAY(parser->vm, Token, fields, old_capacity, field_capacity);
        }

        advance(parser);
        Token field_name = parser->previous;
        fields[field_count++] = field_name;

        if (match(parser, TOKEN_SEMICOLON)) {
            continue;
        }

        if (parser->current.line > parser->previous.line) {
            continue;
        }

        if (parser->current.type == TOKEN_RIGHT_BRACE) {
            break;
        }

        if (parser->current.type == TOKEN_EOF) {
            break;
        }

        error_at_current(parser, "Expect ';' or newline after field name.");
        break;
    }

    consume(parser, TOKEN_RIGHT_BRACE, "Expect '}' after struct fields.");
    return new_struct_decl_stmt(parser->vm, name, fields, field_count, field_capacity, keyword);
}

static Stmt* parse_enum_declaration(Parser* parser) {
    Token keyword = parser->previous;
    consume(parser, TOKEN_IDENTIFIER, "Expect enum name.");
    Token name = parser->previous;

    consume(parser, TOKEN_LEFT_BRACE, "Expect '{' after enum name.");

    int variant_count = 0;
    int variant_capacity = 0;
    Token* variants = NULL;

    if (parser->current.type != TOKEN_RIGHT_BRACE) {
        do {
            if (parser->current.type != TOKEN_IDENTIFIER) {
                error_at_current(parser, "Expect variant name in enum declaration.");
                break;
            }

            if (variant_count + 1 > variant_capacity) {
                int old_capacity = variant_capacity;
                variant_capacity = GROW_CAPACITY(old_capacity);
                variants = GROW_ARRAY(parser->vm, Token, variants, old_capacity, variant_capacity);
            }

            advance(parser);
            variants[variant_count++] = parser->previous;
        } while (match(parser, TOKEN_COMMA));
    }

    consume(parser, TOKEN_RIGHT_BRACE, "Expect '}' after enum variants.");
    return new_enum_decl_stmt(parser->vm, name, variants, variant_count, variant_capacity, keyword);
}

static Stmt* parse_declaration(Parser* parser) {
    Stmt* stmt = NULL;

    if (match(parser, TOKEN_FUNC)) {
        stmt = function(parser, "function");
    } else if (match(parser, TOKEN_VAR)) {
        stmt = parse_var_declaration(parser);
    } else if (match(parser, TOKEN_STRUCT)) {
        stmt = parse_struct_declaration(parser);
    } else if (match(parser, TOKEN_ENUM)) {
        stmt = parse_enum_declaration(parser);
    } else {
        stmt = parse_statement(parser);
    }

    if (stmt == NULL && parser->panic_mode) {
        synchronize(parser);
        return new_expression_stmt(parser->vm, new_literal_expr(parser->vm, (Token){
            .type = TOKEN_NULL,
            .start = "null",
            .length = 4,
            .line = parser->previous.line
        }));
    }

    return stmt;
}

static Stmt* parse_block(Parser* parser) {
    Token brace = parser->previous;
    int count = 0;
    int capacity = 0;
    Stmt** statements = NULL;

    while (parser->current.type != TOKEN_RIGHT_BRACE && parser->current.type != TOKEN_EOF) {
        if (count + 1 > capacity) {
            int old_capacity = capacity;
            capacity = GROW_CAPACITY(old_capacity);
            statements = GROW_ARRAY(parser->vm, Stmt*, statements, old_capacity, capacity);
        }
        Stmt* stmt = parse_declaration(parser);
        statements[count++] = stmt;
    }
    consume(parser, TOKEN_RIGHT_BRACE, "Expect '}' after block.");
    return new_block_stmt(parser->vm, statements, count, capacity, brace);
}

static Stmt* parse_if_statement(Parser* parser) {
    Token keyword = parser->previous;
    consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    Expr* condition = parse_expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after if condition.");
    Stmt* then_branch = parse_statement(parser);
    Stmt* else_branch = NULL;
    if (match(parser, TOKEN_ELSE)) {
        else_branch = parse_statement(parser);
    }
    return new_if_stmt(parser->vm, condition, then_branch, else_branch, keyword);
}

static Stmt* parse_while_statement(Parser* parser) {
    Token keyword = parser->previous;
    consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
    Expr* condition = parse_expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after while condition.");
    Stmt* body = parse_statement(parser);
    return new_while_stmt(parser->vm, condition, body, keyword);
}

static Stmt* parse_do_while_statement(Parser* parser) {
    Token keyword = parser->previous;
    Stmt* body = parse_statement(parser);
    consume(parser, TOKEN_WHILE, "Expect 'while' after do-while body.");
    consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
    Expr* condition = parse_expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after do-while condition.");
    match(parser, TOKEN_SEMICOLON);
    return new_do_while_stmt(parser->vm, body, condition, keyword);
}

static Stmt* parse_for_statement(Parser* parser) {
    Token keyword = parser->previous;
    consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
    Stmt* initializer;
    if (match(parser, TOKEN_SEMICOLON)) {
        initializer = NULL;
    } else if (match(parser, TOKEN_VAR)) {
        initializer = parse_var_declaration(parser);
    } else {
        Expr* expr = parse_expression(parser);
        consume(parser, TOKEN_SEMICOLON, "Expect ';' after loop initializer.");
        initializer = new_expression_stmt(parser->vm, expr);
    }
    Expr* condition = NULL;
    if (!match(parser, TOKEN_SEMICOLON)) {
        condition = parse_expression(parser);
        consume(parser, TOKEN_SEMICOLON, "Expect ';' after loop condition.");
    }
    Expr* increment = NULL;
    if (!match(parser, TOKEN_RIGHT_PAREN)) {
        increment = parse_expression(parser);
        consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");
    }
    Stmt* body = parse_statement(parser);
    return new_for_stmt(parser->vm, initializer, condition, increment, body, keyword);
}

static Stmt* parse_jump_statement(Parser* parser) {
    Token keyword = parser->previous;
    consume_end_of_statement(parser, "Expect ';' after jump statement.");
    return (keyword.type == TOKEN_BREAK) ? new_break_stmt(parser->vm, keyword) : new_continue_stmt(parser->vm, keyword);
}

static Stmt* parse_goto_statement(Parser* parser) {
    Token keyword = parser->previous;
    if (!check(parser, TOKEN_IDENTIFIER)) {
        error_at_current(parser, "Expect label name after 'goto'.");
        return new_expression_stmt(parser->vm, NULL);
    }
    Token target = parser->current;
    advance(parser);
    consume_end_of_statement(parser, "Expect ';' after goto statement.");
    return new_goto_stmt(parser->vm, keyword, target);
}

static Stmt* parse_switch_statement(Parser* parser) {
    Token keyword = parser->previous;

    consume(parser, TOKEN_LEFT_PAREN, "Expect '(' after 'switch'.");
    Expr* expression = parse_expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after switch expression.");
    consume(parser, TOKEN_LEFT_BRACE, "Expect '{' to start switch body.");
    int case_capacity = 8;
    int case_count = 0;
    CaseClause* cases = ALLOCATE(parser->vm, CaseClause, case_capacity);
    int default_index = -1;

    while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
        if (match(parser, TOKEN_CASE)) {
            if (case_count >= case_capacity) {
                int old_capacity = case_capacity;
                case_capacity = case_capacity * 2;
                cases = GROW_ARRAY(parser->vm, CaseClause, cases, old_capacity, case_capacity);
            }

            Expr* case_value = parse_expression(parser);
            consume(parser, TOKEN_COLON, "Expect ':' after case value.");
            int stmt_capacity = 4;
            int stmt_count = 0;
            Stmt** statements = ALLOCATE(parser->vm, Stmt*, stmt_capacity);

            while (!check(parser, TOKEN_CASE) && !check(parser, TOKEN_DEFAULT) &&
                   !check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
                if (stmt_count >= stmt_capacity) {
                    int old_stmt_capacity = stmt_capacity;
                    stmt_capacity = stmt_capacity * 2;
                    statements = GROW_ARRAY(parser->vm, Stmt*, statements, old_stmt_capacity, stmt_capacity);
                }
                statements[stmt_count++] = parse_statement(parser);
            }

            cases[case_count].value = case_value;
            cases[case_count].statements = statements;
            cases[case_count].statement_count = stmt_count;
            cases[case_count].statement_capacity = stmt_capacity;
            case_count++;
        }
        else if (match(parser, TOKEN_DEFAULT)) {
            consume(parser, TOKEN_COLON, "Expect ':' after 'default'.");

            if (default_index != -1) {
                error_at_current(parser, "Multiple 'default' cases in switch.");
            }

            if (case_count >= case_capacity) {
                int old_capacity = case_capacity;
                case_capacity = case_capacity * 2;
                cases = GROW_ARRAY(parser->vm, CaseClause, cases, old_capacity, case_capacity);
            }

            default_index = case_count;
            int stmt_capacity = 4;
            int stmt_count = 0;
            Stmt** statements = ALLOCATE(parser->vm, Stmt*, stmt_capacity);

            while (!check(parser, TOKEN_CASE) && !check(parser, TOKEN_DEFAULT) &&
                   !check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
                if (stmt_count >= stmt_capacity) {
                    int old_stmt_capacity = stmt_capacity;
                    stmt_capacity = stmt_capacity * 2;
                    statements = GROW_ARRAY(parser->vm, Stmt*, statements, old_stmt_capacity, stmt_capacity);
                }
                statements[stmt_count++] = parse_statement(parser);
            }

            cases[case_count].value = NULL;
            cases[case_count].statements = statements;
            cases[case_count].statement_count = stmt_count;
            cases[case_count].statement_capacity = stmt_capacity;
            case_count++;
        }
        else {
            error_at_current(parser, "Expect 'case' or 'default' in switch body.");
            break;
        }
    }

    consume(parser, TOKEN_RIGHT_BRACE, "Expect '}' after switch body.");

    return new_switch_stmt(parser->vm, expression, cases, case_count,
                          case_capacity, default_index, keyword);
}

static Stmt* parse_return_statement(Parser* parser) {
    Token keyword = parser->previous;
    Expr* value = NULL;

    if (parser->current.type != TOKEN_SEMICOLON &&
        parser->current.type != TOKEN_EOF &&
        parser->current.type != TOKEN_RIGHT_BRACE &&
        parser->current.line == parser->previous.line) {
        value = parse_expression(parser);
    }

    consume_end_of_statement(parser, "Expect ';' after return value.");
    return new_return_stmt(parser->vm, keyword, value);
}
AstResult parse(VM* vm, const char* source, const LineMap* line_map, const char* entry_file) {
    Parser parser;
    parser.vm = vm;
    initScanner(&parser.scanner, source, line_map);
    parser.had_error = false;
    parser.panic_mode = false;

    if (entry_file) {
        parser.current_module_name = entry_file;
        parser.module_name_length = strlen(entry_file);
    } else {
        parser.current_module_name = NULL;
        parser.module_name_length = 0;
    }

    advance(&parser);

    int count = 0;
    int capacity = 8;
    Stmt** statements = ALLOCATE(vm, Stmt*, capacity);

    while (!match(&parser, TOKEN_EOF)) {
        if (count + 1 > capacity) {
            int old_capacity = capacity;
            capacity = GROW_CAPACITY(old_capacity);
            statements = GROW_ARRAY(vm, Stmt*, statements, old_capacity, capacity);
        }
        Stmt* stmt = parse_declaration(&parser);
        statements[count++] = stmt;
    }

    if (parser.had_error) {
        fprintf(stderr, "\nCompilation aborted due to parse errors.\n");
        for (int i = 0; i < count; i++) free_stmt(vm, statements[i]);
        FREE_ARRAY(vm, Stmt*, statements, capacity);
        return (AstResult){ .statements = NULL, .capacity = 0 };
    }

    if (count + 1 > capacity) {
        int old_capacity = capacity;
        capacity = GROW_CAPACITY(old_capacity);
        statements = GROW_ARRAY(vm, Stmt*, statements, old_capacity, capacity);
    }
    statements[count] = NULL;

    return (AstResult){ .statements = statements, .capacity = capacity };
}