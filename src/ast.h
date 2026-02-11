#pragma once

#include <stdbool.h>

#include "./token.h"

typedef struct VM VM;

typedef struct ObjFunction ObjFunction;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct TypeSpecifier TypeSpecifier;
typedef struct Param Param;

typedef enum {
    TYPE_SIMPLE,
    TYPE_LIST
} TypeKind;

typedef struct {
    TypeSpecifier* element_type;
    Expr* size;
} ListTypeSpec;

struct TypeSpecifier {
    TypeKind kind;
    union {
        Token simple;
        ListTypeSpec list;
    } as;
};

typedef enum {
    EXPR_ASSIGN,
    EXPR_BINARY,
    EXPR_CALL,
    EXPR_GET,
    EXPR_SET,
    EXPR_UNARY,
    EXPR_LITERAL,
    EXPR_GROUPING,
    EXPR_VARIABLE,
    EXPR_LIST,
    EXPR_SUBSCRIPT,
    EXPR_MAP,
    EXPR_FUNCTION,
    EXPR_STRUCT_INST,
    EXPR_TERNARY,
    EXPR_PRE_INC,
    EXPR_POST_INC,
    EXPR_PRE_DEC,
    EXPR_POST_DEC,
    EXPR_TYPEOF,
    EXPR_SPREAD
} ExprType;

typedef struct { Expr* target; Expr* value; bool has_slot_modifier; } AssignExpr;
typedef struct { Expr* left; Token operator; Expr* right; } BinaryExpr;
typedef struct { Expr* callee; Token paren; Expr** args; int arg_count; int arg_capacity; } CallExpr;
typedef struct { Expr* object; Token name; } GetExpr;
typedef struct { Expr* object; Token name; Expr* value; bool has_slot_modifier; } SetExpr;
typedef struct { Token operator; Expr* right; } UnaryExpr;
typedef struct { Token literal; } LiteralExpr;
typedef struct { Expr* expression; } GroupingExpr;
typedef struct { Token name; } VariableExpr;
typedef struct { Expr** elements; int count; int capacity; } ListExpr;
typedef struct { Expr* object; Token bracket; Expr* index; } SubscriptExpr;
typedef struct { Expr** keys; Expr** values; int count; int capacity; } MapExpr;
typedef struct { Param* params; int param_count; int param_capacity; Stmt* body; TypeSpecifier* return_type; } FunctionExpr;
typedef struct { Token struct_name; Token* field_names; Expr** field_values; int field_count; int field_capacity; } StructInstExpr;
typedef struct { Expr* condition; Expr* then_expr; Expr* else_expr; } TernaryExpr;
typedef struct { Expr* target; } PreIncExpr;
typedef struct { Expr* target; } PostIncExpr;
typedef struct { Expr* target; } PreDecExpr;
typedef struct { Expr* target; } PostDecExpr;
typedef struct { Expr* operand; } TypeofExpr;
typedef struct { Expr* expression; } SpreadExpr;

struct Expr {
    ExprType type;
    int line;
    union {
        AssignExpr   assign;
        BinaryExpr   binary;
        CallExpr     call;
        GetExpr      get;
        SetExpr      set;
        UnaryExpr    unary;
        LiteralExpr  literal;
        GroupingExpr grouping;
        VariableExpr variable;
        ListExpr     list;
        SubscriptExpr subscript;
        MapExpr      map;
        FunctionExpr function;
        StructInstExpr struct_inst;
        TernaryExpr  ternary;
        PreIncExpr   pre_inc;
        PostIncExpr  post_inc;
        PreDecExpr   pre_dec;
        PostDecExpr  post_dec;
        TypeofExpr   typeof_expr;
        SpreadExpr   spread;
    } as;
};

typedef enum {
    STMT_EXPRESSION,
    STMT_VAR_DECLARATION,
    STMT_BLOCK,
    STMT_IF,
    STMT_WHILE,
    STMT_DO_WHILE,
    STMT_FOR,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_FUNC_DECLARATION,
    STMT_RETURN,
    STMT_COMPILER_DIRECTIVE,
    STMT_STRUCT_DECLARATION,
    STMT_ENUM_DECLARATION,
    STMT_LABEL,
    STMT_GOTO,
    STMT_SWITCH
} StmtType;

typedef struct {
    Stmt** statements;
    int count;
    int capacity;
} BlockStmt;

typedef struct {
    Expr* condition;
    Stmt* then_branch;
    Stmt* else_branch;
} IfStmt;

typedef struct {
    Expr* condition;
    Stmt* body;
} WhileStmt;

typedef struct {
    Expr* condition;
    Stmt* body;
} DoWhileStmt;

typedef struct {
    Stmt* initializer;
    Expr* condition;
    Expr* increment;
    Stmt* body;
} ForStmt;

typedef enum {
    PARAM_NORMAL,
    PARAM_REF,
    PARAM_VAL,
    PARAM_SLOT,
    PARAM_CLONE,
    PARAM_TYPEOF
} ParamQualifier;

struct Param {
    Token name;
    TypeSpecifier* type;
    ParamQualifier qualifier;
};

typedef struct {
    Token name;
    Param* params;
    int param_count;
    int param_capacity;
    Stmt* body;
    TypeSpecifier* return_type;
    ObjFunction* function;
} FuncDeclStmt;

typedef struct {
    Token keyword;
    Expr* value;
} ReturnStmt;


typedef struct { Expr* expression; } ExpressionStmt;

typedef enum {
    DIRECTIVE_TCO
} DirectiveType;

typedef struct {
    DirectiveType type;
    Token argument;
} CompilerDirectiveStmt;

typedef enum {
    VAR_NORMAL,
    VAR_REF,
    VAR_VAL,
    VAR_CLONE
} VarQualifier;

typedef struct {
    Token name;
    TypeSpecifier* type;
    Expr* initializer;
    VarQualifier qualifier;
} VarDecl;

typedef struct {
    VarDecl* variables;
    int count;
    int capacity;
} VarDeclStmt;

typedef struct {
    Token name;
    Token* fields;
    int field_count;
    int field_capacity;
} StructDeclStmt;

typedef struct {
    Token name;
    Token* variants;
    int variant_count;
    int variant_capacity;
} EnumDeclStmt;

typedef struct {
    Token label_name;
} LabelStmt;

typedef struct {
    Token target_label;
} GotoStmt;

typedef struct {
    Expr* value;  // NULL for default case
    Stmt** statements;
    int statement_count;
    int statement_capacity;
} CaseClause;

typedef struct {
    Expr* expression;
    CaseClause* cases;
    int case_count;
    int case_capacity;
    int default_index;  // -1 if no default
} SwitchStmt;

struct Stmt {
    StmtType type;
    int line;
    Token keyword;
    union {
        ExpressionStmt       expression;
        VarDeclStmt          var_declaration;
        BlockStmt            block;
        IfStmt               if_stmt;
        WhileStmt            while_stmt;
        DoWhileStmt          do_while_stmt;
        ForStmt              for_stmt;
        FuncDeclStmt         func_declaration;
        ReturnStmt           return_stmt;
        CompilerDirectiveStmt compiler_directive;
        StructDeclStmt       struct_declaration;
        EnumDeclStmt         enum_declaration;
        LabelStmt            label;
        GotoStmt             goto_stmt;
        SwitchStmt           switch_stmt;
    } as;
};

TypeSpecifier* new_simple_type_spec(VM* vm, Token token);
TypeSpecifier* new_list_type_spec(VM* vm, TypeSpecifier* element_type, Expr* size);
void free_type_spec(VM* vm, TypeSpecifier* spec);

Expr* new_assign_expr(VM* vm, Expr* target, Expr* value, bool has_slot_modifier);
Expr* new_binary_expr(VM* vm, Expr* left, Token operator, Expr* right);
Expr* new_call_expr(VM* vm, Expr* callee, Token paren, Expr** args, int arg_count, int arg_capacity);
Expr* new_get_expr(VM* vm, Expr* object, Token name);
Expr* new_set_expr(VM* vm, Expr* object, Token name, Expr* value, bool has_slot_modifier);
Expr* new_unary_expr(VM* vm, Token operator, Expr* right);
Expr* new_literal_expr(VM* vm, Token literal);
Expr* new_grouping_expr(VM* vm, Expr* expression);
Expr* new_variable_expr(VM* vm, Token name);
Expr* new_list_expr(VM* vm, Expr** elements, int count, int capacity, Token bracket);
Expr* new_subscript_expr(VM* vm, Expr* object, Token bracket, Expr* index);
Expr* new_map_expr(VM* vm, Expr** keys, Expr** values, int count, int capacity, Token brace);
Expr* new_function_expr(VM* vm, Param* params, int param_count, int param_capacity, Stmt* body, TypeSpecifier* return_type, Token token);
Expr* new_struct_inst_expr(VM* vm, Token struct_name, Token* field_names, Expr** field_values, int field_count, int field_capacity, Token brace);
Expr* new_ternary_expr(VM* vm, Expr* condition, Expr* then_expr, Expr* else_expr);
Expr* new_pre_inc_expr(VM* vm, Expr* target, Token token);
Expr* new_post_inc_expr(VM* vm, Expr* target, Token token);
Expr* new_pre_dec_expr(VM* vm, Expr* target, Token token);
Expr* new_post_dec_expr(VM* vm, Expr* target, Token token);
Expr* new_typeof_expr(VM* vm, Expr* operand, Token token);
Expr* new_spread_expr(VM* vm, Expr* expression, Token token);
Expr* clone_expr(VM* vm, Expr* expr);
void free_expr(VM* vm, Expr* expr);

Stmt* new_expression_stmt(VM* vm, Expr* expression);
Stmt* new_var_decl_stmt(VM* vm, VarDecl* variables, int count, int capacity, Token keyword);
Stmt* new_block_stmt(VM* vm, Stmt** statements, int count, int capacity, Token brace);
Stmt* new_if_stmt(VM* vm, Expr* condition, Stmt* then_branch, Stmt* else_branch, Token keyword);
Stmt* new_while_stmt(VM* vm, Expr* condition, Stmt* body, Token keyword);
Stmt* new_do_while_stmt(VM* vm, Stmt* body, Expr* condition, Token keyword);
Stmt* new_for_stmt(VM* vm, Stmt* initializer, Expr* condition, Expr* increment, Stmt* body, Token keyword);
Stmt* new_break_stmt(VM* vm, Token keyword);
Stmt* new_continue_stmt(VM* vm, Token keyword);
Stmt* new_print_stmt(VM* vm, Expr* expression, Token keyword);
Stmt* new_func_decl_stmt(VM* vm, Token name, Param* params, int param_count, int param_capacity, Stmt* body, TypeSpecifier* return_type);
Stmt* new_return_stmt(VM* vm, Token keyword, Expr* value);
Stmt* new_struct_decl_stmt(VM* vm, Token name, Token* fields, int field_count, int field_capacity, Token keyword);
Stmt* new_enum_decl_stmt(VM* vm, Token name, Token* variants, int variant_count, int variant_capacity, Token keyword);
Stmt* new_label_stmt(VM* vm, Token label_name);
Stmt* new_goto_stmt(VM* vm, Token keyword, Token target_label);
Stmt* new_switch_stmt(VM* vm, Expr* expression, CaseClause* cases, int case_count, int case_capacity, int default_index, Token keyword);
void free_stmt(VM* vm, Stmt* stmt);