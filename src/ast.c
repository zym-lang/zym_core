#include <stdlib.h>
#include <stdio.h>

#include "./ast.h"
#include "./memory.h"
#include "./vm.h"

/* Type specifiers */

TypeSpecifier* new_simple_type_spec(VM* vm, Token token) {
    TypeSpecifier* spec = ALLOCATE(vm, TypeSpecifier, 1);
    spec->kind = TYPE_SIMPLE;
    spec->as.simple = token;
    return spec;
}

TypeSpecifier* new_list_type_spec(VM* vm, TypeSpecifier* element_type, Expr* size) {
    TypeSpecifier* spec = ALLOCATE(vm, TypeSpecifier, 1);
    spec->kind = TYPE_LIST;
    spec->as.list.element_type = element_type;
    spec->as.list.size = size;
    return spec;
}

void free_type_spec(VM* vm, TypeSpecifier* spec) {
    if (spec == NULL) return;
    switch (spec->kind) {
        case TYPE_SIMPLE:
            break;
        case TYPE_LIST:
            free_type_spec(vm, spec->as.list.element_type);
            free_expr(vm, spec->as.list.size);
            break;
    }
    FREE(vm, TypeSpecifier, spec);
}


/* Expressions */

static Expr* new_expr(VM* vm, ExprType type, int line) {
    Expr* expr = ALLOCATE(vm, Expr, 1);
    expr->type = type;
    expr->line = line;
    return expr;
}

Expr* new_assign_expr(VM* vm, Expr* target, Expr* value, bool has_slot_modifier) {
    Expr* expr = new_expr(vm, EXPR_ASSIGN, target->line);
    expr->as.assign.target = target;
    expr->as.assign.value = value;
    expr->as.assign.has_slot_modifier = has_slot_modifier;
    return expr;
}

Expr* new_binary_expr(VM* vm, Expr* left, Token operator, Expr* right) {
    Expr* expr = new_expr(vm, EXPR_BINARY, operator.line);
    expr->as.binary.left = left;
    expr->as.binary.operator = operator;
    expr->as.binary.right = right;
    return expr;
}

Expr* new_unary_expr(VM* vm, Token operator, Expr* right) {
    Expr* expr = new_expr(vm, EXPR_UNARY, operator.line);
    expr->as.unary.operator = operator;
    expr->as.unary.right = right;
    return expr;
}

Expr* new_literal_expr(VM* vm, Token literal) {
    Expr* expr = new_expr(vm, EXPR_LITERAL, literal.line);
    expr->as.literal.literal = literal;
    return expr;
}

Expr* new_grouping_expr(VM* vm, Expr* expression) {
    Expr* expr = new_expr(vm, EXPR_GROUPING, expression->line);
    expr->as.grouping.expression = expression;
    return expr;
}

Expr* new_variable_expr(VM* vm, Token name) {
    Expr* expr = new_expr(vm, EXPR_VARIABLE, name.line);
    expr->as.variable.name = name;
    return expr;
}

Expr* new_call_expr(VM* vm, Expr* callee, Token paren, Expr** args, int arg_count, int arg_capacity) {
    Expr* expr = new_expr(vm, EXPR_CALL, paren.line);
    expr->as.call.callee = callee;
    expr->as.call.paren = paren;
    expr->as.call.args = args;
    expr->as.call.arg_count = arg_count;
    expr->as.call.arg_capacity = arg_capacity;
    return expr;
}

Expr* new_get_expr(VM* vm, Expr* object, Token name) {
    Expr* expr = new_expr(vm, EXPR_GET, name.line);
    expr->as.get.object = object;
    expr->as.get.name = name;
    return expr;
}

Expr* new_set_expr(VM* vm, Expr* object, Token name, Expr* value, bool has_slot_modifier) {
    Expr* expr = new_expr(vm, EXPR_SET, name.line);
    expr->as.set.object = object;
    expr->as.set.name = name;
    expr->as.set.value = value;
    expr->as.set.has_slot_modifier = has_slot_modifier;
    return expr;
}

Expr* new_list_expr(VM* vm, Expr** elements, int count, int capacity, Token bracket) {
    Expr* expr = new_expr(vm, EXPR_LIST, bracket.line);
    expr->as.list.elements = elements;
    expr->as.list.count = count;
    expr->as.list.capacity = capacity;
    return expr;
}

Expr* new_subscript_expr(VM* vm, Expr* object, Token bracket, Expr* index) {
    Expr* expr = new_expr(vm, EXPR_SUBSCRIPT, bracket.line);
    expr->as.subscript.object = object;
    expr->as.subscript.bracket = bracket;
    expr->as.subscript.index = index;
    return expr;
}

Expr* new_map_expr(VM* vm, Expr** keys, Expr** values, int count, int capacity, Token brace) {
    Expr* expr = new_expr(vm, EXPR_MAP, brace.line);
    expr->as.map.keys = keys;
    expr->as.map.values = values;
    expr->as.map.count = count;
    expr->as.map.capacity = capacity;
    return expr;
}

Expr* new_function_expr(VM* vm, Param* params, int param_count, int param_capacity, Stmt* body, TypeSpecifier* return_type, Token token) {
    Expr* expr = new_expr(vm, EXPR_FUNCTION, token.line);
    expr->as.function.params = params;
    expr->as.function.param_count = param_count;
    expr->as.function.param_capacity = param_capacity;
    expr->as.function.body = body;
    expr->as.function.return_type = return_type;
    return expr;
}

Expr* new_struct_inst_expr(VM* vm, Token struct_name, Token* field_names, Expr** field_values, int field_count, int field_capacity, Token brace) {
    Expr* expr = new_expr(vm, EXPR_STRUCT_INST, brace.line);
    expr->as.struct_inst.struct_name = struct_name;
    expr->as.struct_inst.field_names = field_names;
    expr->as.struct_inst.field_values = field_values;
    expr->as.struct_inst.field_count = field_count;
    expr->as.struct_inst.field_capacity = field_capacity;
    return expr;
}

Expr* new_ternary_expr(VM* vm, Expr* condition, Expr* then_expr, Expr* else_expr) {
    Expr* expr = new_expr(vm, EXPR_TERNARY, condition->line);
    expr->as.ternary.condition = condition;
    expr->as.ternary.then_expr = then_expr;
    expr->as.ternary.else_expr = else_expr;
    return expr;
}

Expr* new_pre_inc_expr(VM* vm, Expr* target, Token token) {
    Expr* expr = new_expr(vm, EXPR_PRE_INC, token.line);
    expr->as.pre_inc.target = target;
    return expr;
}

Expr* new_post_inc_expr(VM* vm, Expr* target, Token token) {
    Expr* expr = new_expr(vm, EXPR_POST_INC, token.line);
    expr->as.post_inc.target = target;
    return expr;
}

Expr* new_pre_dec_expr(VM* vm, Expr* target, Token token) {
    Expr* expr = new_expr(vm, EXPR_PRE_DEC, token.line);
    expr->as.pre_dec.target = target;
    return expr;
}

Expr* new_post_dec_expr(VM* vm, Expr* target, Token token) {
    Expr* expr = new_expr(vm, EXPR_POST_DEC, token.line);
    expr->as.post_dec.target = target;
    return expr;
}

Expr* new_typeof_expr(VM* vm, Expr* operand, Token token) {
    Expr* expr = new_expr(vm, EXPR_TYPEOF, token.line);
    expr->as.typeof_expr.operand = operand;
    return expr;
}

Expr* new_spread_expr(VM* vm, Expr* expression, Token token) {
    Expr* expr = new_expr(vm, EXPR_SPREAD, token.line);
    expr->as.spread.expression = expression;
    return expr;
}

Expr* clone_expr(VM* vm, Expr* expr) {
    if (expr == NULL) return NULL;

    switch (expr->type) {
        case EXPR_VARIABLE:
            return new_variable_expr(vm, expr->as.variable.name);

        case EXPR_LITERAL:
            return new_literal_expr(vm, expr->as.literal.literal);

        case EXPR_SUBSCRIPT:
            return new_subscript_expr(vm,
                clone_expr(vm, expr->as.subscript.object),
                expr->as.subscript.bracket,
                clone_expr(vm, expr->as.subscript.index));

        case EXPR_GET:
            return new_get_expr(vm,
                clone_expr(vm, expr->as.get.object),
                expr->as.get.name);

        case EXPR_BINARY:
            return new_binary_expr(vm,
                clone_expr(vm, expr->as.binary.left),
                expr->as.binary.operator,
                clone_expr(vm, expr->as.binary.right));

        case EXPR_UNARY:
            return new_unary_expr(vm,
                expr->as.unary.operator,
                clone_expr(vm, expr->as.unary.right));

        case EXPR_TYPEOF:
            return new_typeof_expr(vm,
                clone_expr(vm, expr->as.typeof_expr.operand),
                (Token){0});

        case EXPR_SPREAD:
            return new_spread_expr(vm,
                clone_expr(vm, expr->as.spread.expression),
                (Token){0});

        case EXPR_GROUPING:
            return new_grouping_expr(vm, clone_expr(vm, expr->as.grouping.expression));

        default:
            // Only common lvalue expressions need cloning for compound assignments
            fprintf(stderr, "Warning: clone_expr doesn't support expression type %d\n", expr->type);
            return NULL;
    }
}

void free_expr(VM* vm, Expr* expr) {
    if (expr == NULL) return;
    switch (expr->type) {
        case EXPR_ASSIGN:
            free_expr(vm, expr->as.assign.target);
            free_expr(vm, expr->as.assign.value);
            break;
        case EXPR_BINARY:
            free_expr(vm, expr->as.binary.left);
            free_expr(vm, expr->as.binary.right);
            break;
        case EXPR_CALL:
            free_expr(vm, expr->as.call.callee);
            for (int i = 0; i < expr->as.call.arg_count; i++) {
                free_expr(vm, expr->as.call.args[i]);
            }
            FREE_ARRAY(vm, Expr*, expr->as.call.args, expr->as.call.arg_capacity);
            break;
        case EXPR_GET:
            free_expr(vm, expr->as.get.object);
            break;
        case EXPR_SET:
            free_expr(vm, expr->as.set.object);
            free_expr(vm, expr->as.set.value);
            break;
        case EXPR_UNARY:
            free_expr(vm, expr->as.unary.right);
            break;
        case EXPR_GROUPING:
            free_expr(vm, expr->as.grouping.expression);
            break;
        case EXPR_LIST:
            for (int i = 0; i < expr->as.list.count; i++) {
                free_expr(vm, expr->as.list.elements[i]);
            }
            FREE_ARRAY(vm, Expr*, expr->as.list.elements, expr->as.list.capacity);
            break;
        case EXPR_SUBSCRIPT:
            free_expr(vm, expr->as.subscript.object);
            free_expr(vm, expr->as.subscript.index);
            break;
        case EXPR_MAP:
            for (int i = 0; i < expr->as.map.count; i++) {
                free_expr(vm, expr->as.map.keys[i]);
                free_expr(vm, expr->as.map.values[i]);
            }
            FREE_ARRAY(vm, Expr*, expr->as.map.keys, expr->as.map.capacity);
            FREE_ARRAY(vm, Expr*, expr->as.map.values, expr->as.map.capacity);
            break;
        case EXPR_FUNCTION:
            for (int i = 0; i < expr->as.function.param_count; i++) {
                free_type_spec(vm, expr->as.function.params[i].type);
            }
            FREE_ARRAY(vm, Param, expr->as.function.params, expr->as.function.param_capacity);
            free_stmt(vm, expr->as.function.body);
            free_type_spec(vm, expr->as.function.return_type);
            break;
        case EXPR_STRUCT_INST:
            for (int i = 0; i < expr->as.struct_inst.field_count; i++) {
                free_expr(vm, expr->as.struct_inst.field_values[i]);
            }
            FREE_ARRAY(vm, Token, expr->as.struct_inst.field_names, expr->as.struct_inst.field_capacity);
            FREE_ARRAY(vm, Expr*, expr->as.struct_inst.field_values, expr->as.struct_inst.field_capacity);
            break;
        case EXPR_TERNARY:
            free_expr(vm, expr->as.ternary.condition);
            free_expr(vm, expr->as.ternary.then_expr);
            free_expr(vm, expr->as.ternary.else_expr);
            break;
        case EXPR_PRE_INC:
            free_expr(vm, expr->as.pre_inc.target);
            break;
        case EXPR_POST_INC:
            free_expr(vm, expr->as.post_inc.target);
            break;
        case EXPR_PRE_DEC:
            free_expr(vm, expr->as.pre_dec.target);
            break;
        case EXPR_POST_DEC:
            free_expr(vm, expr->as.post_dec.target);
            break;
        case EXPR_TYPEOF:
            free_expr(vm, expr->as.typeof_expr.operand);
            break;
        case EXPR_SPREAD:
            free_expr(vm, expr->as.spread.expression);
            break;
        case EXPR_LITERAL:
        case EXPR_VARIABLE:
            break;
    }
    FREE(vm, Expr, expr);
}

/* Statements */

static Stmt* new_stmt(VM* vm, StmtType type, int line) {
    Stmt* stmt = ALLOCATE(vm, Stmt, 1);
    stmt->type = type;
    stmt->line = line;
    return stmt;
}

Stmt* new_expression_stmt(VM* vm, Expr* expression) {
    Stmt* stmt = new_stmt(vm, STMT_EXPRESSION, expression->line);
    stmt->as.expression.expression = expression;
    return stmt;
}

Stmt* new_var_decl_stmt(VM* vm, VarDecl* variables, int count, int capacity, Token keyword) {
    Stmt* stmt = new_stmt(vm, STMT_VAR_DECLARATION, keyword.line);
    stmt->keyword = keyword;
    stmt->as.var_declaration.variables = variables;
    stmt->as.var_declaration.count = count;
    stmt->as.var_declaration.capacity = capacity;
    return stmt;
}

Stmt* new_block_stmt(VM* vm, Stmt** statements, int count, int capacity, Token brace) {
    Stmt* stmt = new_stmt(vm, STMT_BLOCK, brace.line);
    stmt->as.block.statements = statements;
    stmt->as.block.count = count;
    stmt->as.block.capacity = capacity;
    return stmt;
}

Stmt* new_if_stmt(VM* vm, Expr* condition, Stmt* then_branch, Stmt* else_branch, Token keyword) {
    Stmt* stmt = new_stmt(vm, STMT_IF, keyword.line);
    stmt->keyword = keyword;
    stmt->as.if_stmt.condition = condition;
    stmt->as.if_stmt.then_branch = then_branch;
    stmt->as.if_stmt.else_branch = else_branch;
    return stmt;
}

Stmt* new_while_stmt(VM* vm, Expr* condition, Stmt* body, Token keyword) {
    Stmt* stmt = new_stmt(vm, STMT_WHILE, keyword.line);
    stmt->keyword = keyword;
    stmt->as.while_stmt.condition = condition;
    stmt->as.while_stmt.body = body;
    return stmt;
}

Stmt* new_do_while_stmt(VM* vm, Stmt* body, Expr* condition, Token keyword) {
    Stmt* stmt = new_stmt(vm, STMT_DO_WHILE, keyword.line);
    stmt->keyword = keyword;
    stmt->as.do_while_stmt.body = body;
    stmt->as.do_while_stmt.condition = condition;
    return stmt;
}

Stmt* new_for_stmt(VM* vm, Stmt* initializer, Expr* condition, Expr* increment, Stmt* body, Token keyword) {
    Stmt* stmt = new_stmt(vm, STMT_FOR, keyword.line);
    stmt->keyword = keyword;
    stmt->as.for_stmt.initializer = initializer;
    stmt->as.for_stmt.condition = condition;
    stmt->as.for_stmt.increment = increment;
    stmt->as.for_stmt.body = body;
    return stmt;
}

Stmt* new_break_stmt(VM* vm, Token keyword) {
    Stmt* stmt = new_stmt(vm, STMT_BREAK, keyword.line);
    stmt->keyword = keyword;
    return stmt;
}

Stmt* new_continue_stmt(VM* vm, Token keyword) {
    Stmt* stmt = new_stmt(vm, STMT_CONTINUE, keyword.line);
    stmt->keyword = keyword;
    return stmt;
}

Stmt* new_func_decl_stmt(VM* vm, Token name, Param* params, int param_count, int param_capacity, Stmt* body, TypeSpecifier* return_type) {
    Stmt* stmt = new_stmt(vm, STMT_FUNC_DECLARATION, name.line);
    stmt->as.func_declaration.name = name;
    stmt->as.func_declaration.params = params;
    stmt->as.func_declaration.param_count = param_count;
    stmt->as.func_declaration.param_capacity = param_capacity;
    stmt->as.func_declaration.body = body;
    stmt->as.func_declaration.return_type = return_type;
    return stmt;
}

Stmt* new_return_stmt(VM* vm, Token keyword, Expr* value) {
    Stmt* stmt = new_stmt(vm, STMT_RETURN, keyword.line);
    stmt->keyword = keyword;
    stmt->as.return_stmt.value = value;
    return stmt;
}

Stmt* new_struct_decl_stmt(VM* vm, Token name, Token* fields, int field_count, int field_capacity, Token keyword) {
    Stmt* stmt = new_stmt(vm, STMT_STRUCT_DECLARATION, keyword.line);
    stmt->keyword = keyword;
    stmt->as.struct_declaration.name = name;
    stmt->as.struct_declaration.fields = fields;
    stmt->as.struct_declaration.field_count = field_count;
    stmt->as.struct_declaration.field_capacity = field_capacity;
    return stmt;
}

Stmt* new_enum_decl_stmt(VM* vm, Token name, Token* variants, int variant_count, int variant_capacity, Token keyword) {
    Stmt* stmt = new_stmt(vm, STMT_ENUM_DECLARATION, keyword.line);
    stmt->keyword = keyword;
    stmt->as.enum_declaration.name = name;
    stmt->as.enum_declaration.variants = variants;
    stmt->as.enum_declaration.variant_count = variant_count;
    stmt->as.enum_declaration.variant_capacity = variant_capacity;
    return stmt;
}

Stmt* new_label_stmt(VM* vm, Token label_name) {
    Stmt* stmt = new_stmt(vm, STMT_LABEL, label_name.line);
    stmt->as.label.label_name = label_name;
    return stmt;
}

Stmt* new_goto_stmt(VM* vm, Token keyword, Token target_label) {
    Stmt* stmt = new_stmt(vm, STMT_GOTO, keyword.line);
    stmt->keyword = keyword;
    stmt->as.goto_stmt.target_label = target_label;
    return stmt;
}

Stmt* new_switch_stmt(VM* vm, Expr* expression, CaseClause* cases, int case_count, int case_capacity, int default_index, Token keyword) {
    Stmt* stmt = new_stmt(vm, STMT_SWITCH, keyword.line);
    stmt->keyword = keyword;
    stmt->as.switch_stmt.expression = expression;
    stmt->as.switch_stmt.cases = cases;
    stmt->as.switch_stmt.case_count = case_count;
    stmt->as.switch_stmt.case_capacity = case_capacity;
    stmt->as.switch_stmt.default_index = default_index;
    return stmt;
}

void free_stmt(VM* vm, Stmt* stmt) {
    if (stmt == NULL) return;
    switch (stmt->type) {
        case STMT_EXPRESSION:
            free_expr(vm, stmt->as.expression.expression);
            break;
        case STMT_VAR_DECLARATION:
            for (int i = 0; i < stmt->as.var_declaration.count; i++) {
                free_type_spec(vm, stmt->as.var_declaration.variables[i].type);
                free_expr(vm, stmt->as.var_declaration.variables[i].initializer);
            }
            FREE_ARRAY(vm, VarDecl, stmt->as.var_declaration.variables, stmt->as.var_declaration.capacity);
            break;
        case STMT_BLOCK: {
            for (int i = 0; i < stmt->as.block.count; i++) {
                free_stmt(vm, stmt->as.block.statements[i]);
            }
            FREE_ARRAY(vm, Stmt*, stmt->as.block.statements, stmt->as.block.capacity);
            break;
        }
        case STMT_IF: {
            free_expr(vm, stmt->as.if_stmt.condition);
            free_stmt(vm, stmt->as.if_stmt.then_branch);
            free_stmt(vm, stmt->as.if_stmt.else_branch);
            break;
        }
        case STMT_WHILE: {
            free_expr(vm, stmt->as.while_stmt.condition);
            free_stmt(vm, stmt->as.while_stmt.body);
            break;
        }
        case STMT_DO_WHILE: {
            free_stmt(vm, stmt->as.do_while_stmt.body);
            free_expr(vm, stmt->as.do_while_stmt.condition);
            break;
        }
        case STMT_FOR: {
            free_stmt(vm, stmt->as.for_stmt.initializer);
            free_expr(vm, stmt->as.for_stmt.condition);
            free_expr(vm, stmt->as.for_stmt.increment);
            free_stmt(vm, stmt->as.for_stmt.body);
            break;
        }
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;
        case STMT_FUNC_DECLARATION: {
            for (int i = 0; i < stmt->as.func_declaration.param_count; i++) {
                free_type_spec(vm, stmt->as.func_declaration.params[i].type);
            }
            FREE_ARRAY(vm, Param, stmt->as.func_declaration.params, stmt->as.func_declaration.param_capacity);
            free_type_spec(vm, stmt->as.func_declaration.return_type);
            free_stmt(vm, stmt->as.func_declaration.body);
            break;
        }
        case STMT_RETURN: {
            free_expr(vm, stmt->as.return_stmt.value);
            break;
        }
        case STMT_COMPILER_DIRECTIVE:
            break;
        case STMT_STRUCT_DECLARATION:
            FREE_ARRAY(vm, Token, stmt->as.struct_declaration.fields, stmt->as.struct_declaration.field_capacity);
            break;
        case STMT_ENUM_DECLARATION:
            FREE_ARRAY(vm, Token, stmt->as.enum_declaration.variants, stmt->as.enum_declaration.variant_capacity);
            break;
        case STMT_LABEL:
        case STMT_GOTO:
            break;
        case STMT_SWITCH: {
            free_expr(vm, stmt->as.switch_stmt.expression);
            for (int i = 0; i < stmt->as.switch_stmt.case_count; i++) {
                if (stmt->as.switch_stmt.cases[i].value != NULL) {
                    free_expr(vm, stmt->as.switch_stmt.cases[i].value);
                }
                for (int j = 0; j < stmt->as.switch_stmt.cases[i].statement_count; j++) {
                    free_stmt(vm, stmt->as.switch_stmt.cases[i].statements[j]);
                }
                FREE_ARRAY(vm, Stmt*, stmt->as.switch_stmt.cases[i].statements, stmt->as.switch_stmt.cases[i].statement_capacity);
            }
            FREE_ARRAY(vm, CaseClause, stmt->as.switch_stmt.cases, stmt->as.switch_stmt.case_capacity);
            break;
        }
    }
    FREE(vm, Stmt, stmt);
}