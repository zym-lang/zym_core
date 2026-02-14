#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "./compiler.h"
#include "./common.h"
#include "./object.h"
#include "./vm.h"
#include "./memory.h"
#include "./utils.h"
#include "gc.h"

#define OPCODE(i) ((i) & 0xFF)

#define EMIT_MOVE_IF_NEEDED(compiler, target, source, line) \
    do { if ((compiler)->result_needed) emit_move((compiler), (target), (source), (line)); } while(0)

#define COMPILE_REQUIRED(compiler, expr, target) \
    do { \
        bool _old_needed = (compiler)->result_needed; \
        (compiler)->result_needed = true; \
        compile_expression((compiler), (expr), (target)); \
        (compiler)->result_needed = _old_needed; \
    } while(0)

/*
 * --- Instruction Encoding (8-bit opcode, 8-bit registers) ---
 *
 * Format ABC (3 register operands):
 * [ OpCode(8) | A(8) | B(8) | C(8) ] (32 bits total)
 *
 * Format ABx (1 register, 1 wide operand):
 * [ OpCode(8) | A(8) | Bx(16) ] (32 bits total)
 */
#define PACK_ABC(op, a, b, c) ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 24))
#define PACK_ABx(op, a, bx) ((uint32_t)(op) | ((uint32_t)(a) << 8) | (((uint32_t)(bx) & 0xFFFF) << 16))

#define OPCODE_MASK 0xFF
#define BX_MASK 0xFFFF
#define JUMP_PLACEHOLDER 9999
#define MAX_JUMP_OFFSET_POS 32767
#define MAX_JUMP_OFFSET_NEG -32768
#define REG_SHIFT_A 8
#define REG_SHIFT_B 16
#define REG_SHIFT_C 24

static char* mangle_name(Compiler* compiler, Token* name, int arity);
static void declare_function(Compiler* compiler, Stmt* stmt);
static void define_function(Compiler* compiler, Stmt* stmt);
static bool compile_statement(Compiler* compiler, Stmt* stmt);
static void compile_expression(Compiler* compiler, Expr* expr, int target_reg);
static int compile_sub_expression(Compiler* c, Expr* e);
static int reserve_register(Compiler* compiler);
static void free_register(Compiler* compiler);
static int emit_jump_instruction(Compiler* compiler, OpCode opcode, int reg, int line);
static void patch_jump(Compiler* compiler, int jump_address);
static ObjFunction* compile_function_body(Compiler* current_compiler, FuncDeclStmt* stmt);
static int single_hoisted_arity(Compiler* c, const Token* name);
static void collect_local_hoisted_in_stmt(Compiler* c, Stmt* s);
static ObjStructSchema* get_struct_schema(Compiler* compiler, const Token* name);
static int try_emit_branch_compare(Compiler* compiler, Expr* condition, bool jump_if_true, int line);

static inline OpCode get_binary_op_from_compound(Compiler* compiler, TokenType op_type) {
    switch (op_type) {
        case TOKEN_PLUS_EQUAL: return ADD;
        case TOKEN_MINUS_EQUAL: return SUB;
        case TOKEN_STAR_EQUAL: return MUL;
        case TOKEN_SLASH_EQUAL: return DIV;
        case TOKEN_PERCENT_EQUAL: return MOD;
        case TOKEN_BINARY_AND_EQUAL: return BAND;
        case TOKEN_BINARY_OR_EQUAL: return BOR;
        case TOKEN_BINARY_XOR_EQUAL: return BXOR;
        case TOKEN_LEFT_SHIFT_EQUAL: return BLSHIFT;
        case TOKEN_RIGHT_SHIFT_EQUAL: return BRSHIFT_I;  // arithmetic right shift
        case TOKEN_UNSIGNED_RIGHT_SHIFT_EQUAL: return BRSHIFT_U;  // logical right shift
        default: return ADD;
    }
}
static double parse_number_literal(const char* start, int length);

static void emit_instruction(Compiler* compiler, uint32_t instruction, int line) {
    writeInstruction(compiler->vm, compiler->compiling_chunk, instruction, line);
}

static void emit_move(Compiler* c, int dst, int src, int line) {
    if (dst != src) {
        emit_instruction(c, PACK_ABC(MOVE, dst, src, 0), line);
    }
}

static void emit_load_const(Compiler* c, int reg, int const_idx, int line) {
    emit_instruction(c, PACK_ABx(LOAD_CONST, reg, const_idx), line);
}

static void emit_get_global(Compiler* c, int reg, int name_const, int line) {
    emit_instruction(c, PACK_ABx(GET_GLOBAL, reg, name_const), line);
}

static void emit_set_global(Compiler* c, int reg, int name_const, int line) {
    emit_instruction(c, PACK_ABx(SET_GLOBAL, reg, name_const), line);
}

static void emit_get_upvalue(Compiler* c, int reg, int upvalue_idx, int line) {
    emit_instruction(c, PACK_ABx(GET_UPVALUE, reg, upvalue_idx), line);
}

static void emit_set_upvalue(Compiler* c, int reg, int upvalue_idx, int line) {
    emit_instruction(c, PACK_ABx(SET_UPVALUE, reg, upvalue_idx), line);
}

static void emit_closure(Compiler* c, int reg, int const_idx, int line) {
    emit_instruction(c, PACK_ABx(CLOSURE, reg, const_idx), line);
}

static void compiler_error(Compiler* compiler, int line, const char* format, ...) {
    compiler->has_error = true;
    if (compiler->current_module_name) {
        fprintf(stderr, "[%.*s] line %d: ",
                compiler->current_module_name->length,
                compiler->current_module_name->chars,
                line);
    } else {
        fprintf(stderr, "[line %d]: ", line);
    }

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void compiler_error_and_exit(int line, const char* format, ...) {
    fprintf(stderr, "Error at line %d: ", line);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}

static bool tokens_equal(const Token* a, const Token* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static double parse_number_literal(const char* start, int length) {
    double value;
    if (length >= 2 && start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
        long long hex = 0;
        for (int i = 2; i < length; i++) {
            char c = start[i];
            if (c == '_') continue;
            if (c >= '0' && c <= '9') {
                hex = hex * 16 + (c - '0');
            } else if (c >= 'a' && c <= 'f') {
                hex = hex * 16 + (10 + (c - 'a'));
            } else if (c >= 'A' && c <= 'F') {
                hex = hex * 16 + (10 + (c - 'A'));
            } else {
                break;
            }
        }
        value = (double)hex;
    } else if (length >= 2 && start[0] == '0' && (start[1] == 'b' || start[1] == 'B')) {
        long long bin = 0;
        for (int i = 2; i < length; i++) {
            char c = start[i];
            if (c == '_') continue;
            if (c == '0' || c == '1') {
                bin = bin * 2 + (c - '0');
            } else {
                break;
            }
        }
        value = (double)bin;
    } else {
        char* clean = (char*)malloc(length + 1);
        int pos = 0;
        for (int i = 0; i < length; i++) {
            if (start[i] != '_') {
                clean[pos++] = start[i];
            }
        }
        clean[pos] = '\0';
        value = strtod(clean, NULL);
        free(clean);
    }

    return value;
}

static int make_constant(Compiler* compiler, Value value) {
    int constant = addConstant(compiler->vm, compiler->compiling_chunk, value);
    if (constant > 0xFFFF) {
        printf("Too many constants in one chunk.\n");
        return 0;
    }
    return constant;
}

static int identifier_constant(Compiler* compiler, Token* name) {
    ObjString* str = copyString(compiler->vm, name->start, name->length);
    pushTempRoot(compiler->vm, (Obj*)str);
    Value str_val = OBJ_VAL(str);
    int index = make_constant(compiler, str_val);
    popTempRoot(compiler->vm);
    return index;
}

static Local* get_local_by_reg(Compiler* c, int reg) {
    for (int i = 0; i < c->local_count; i++) {
        if (c->locals[i].reg == reg) {
            return &c->locals[i];
        }
    }
    return NULL;
}

static int is_local_reg(Compiler* c, int r) {
    return get_local_by_reg(c, r) != NULL;
}

static bool is_local_reference(Compiler* c, int reg) {
    Local* local = get_local_by_reg(c, reg);
    return local != NULL && local->is_reference;
}

static bool is_local_ref_param(Compiler* c, int reg) {
    Local* local = get_local_by_reg(c, reg);
    return local != NULL && local->is_ref_param;
}

static bool is_local_slot_param(Compiler* c, int reg) {
    Local* local = get_local_by_reg(c, reg);
    return local != NULL && local->is_slot_param;
}

static bool is_local_ref_or_slot_param(Compiler* c, int reg) {
    Local* local = get_local_by_reg(c, reg);
    return local != NULL && (local->is_ref_param || local->is_slot_param);
}

static bool is_local_holding_reference(Compiler* c, int reg) {
    Local* local = get_local_by_reg(c, reg);
    return local != NULL && local->is_reference;
}



static int alloc_temp(Compiler* c) {
    if (c->next_register < c->local_count) {
        c->next_register = c->local_count;
    }

    if (c->next_register >= MAX_PHYSICAL_REGS) {
        compiler_error(c, -1, "Too many registers in use (%d). Maximum is %d.", c->next_register + 1, MAX_PHYSICAL_REGS);
        return 0;
    }

    int r = c->next_register++;
    if (r > c->max_register_seen) c->max_register_seen = r;
    return r;
}

static inline int save_temp_top(Compiler* c) {
    return c->next_register;
}

static inline void restore_temp_top(Compiler* c, int saved_top) {
    if (saved_top >= c->local_count && saved_top <= c->next_register) {
        c->next_register = saved_top;
    }
}

static inline void restore_temp_top_preserve(Compiler* c, int saved_top, int target_reg) {
    int safe_top = saved_top;

    if (target_reg >= c->local_count) {
        int min_for_preserve = target_reg + 1;
        if (saved_top < min_for_preserve) {
            safe_top = min_for_preserve;
        }
    }

    if (safe_top < c->local_count) {
        safe_top = c->local_count;
    }

    if (safe_top >= c->local_count && safe_top <= c->next_register) {
        c->next_register = safe_top;
    }
}

static int reserve_register(Compiler* c) {
    if (c->next_register >= MAX_PHYSICAL_REGS) {
        compiler_error(c, -1, "Too many local variables (%d). Maximum is %d per function.", c->next_register + 1, MAX_LOCALS);
        return 0;
    }

    int r = c->next_register++;
    if (r > c->max_register_seen) c->max_register_seen = r;

    return r;
}

static void free_register(Compiler* c) {
    if (c->next_register > 0) c->next_register--;
}

static void begin_scope(Compiler* compiler) { compiler->scope_depth++; }

static int emit_jump_instruction(Compiler* compiler, OpCode opcode, int reg, int line) {
    emit_instruction(compiler, PACK_ABx(opcode, reg, JUMP_PLACEHOLDER), line);
    return compiler->compiling_chunk->count - 1;
}

static void patch_jump(Compiler* compiler, int jump_address) {
    int offset = compiler->compiling_chunk->count - jump_address - 1;
    if (offset < MAX_JUMP_OFFSET_NEG || offset > MAX_JUMP_OFFSET_POS) {
        printf("Error: Jump offset out of 16-bit signed range.\n");
        compiler->has_error = true;
    }
    uint32_t old = compiler->compiling_chunk->code[jump_address];

    if (old == JUMP_PLACEHOLDER) {
        compiler->compiling_chunk->code[jump_address] = (uint32_t)(offset & 0xFFFF);
    } else {
        uint32_t cleared = old & ~(((uint32_t)BX_MASK) << REG_SHIFT_B);
        uint32_t patched = cleared | (((uint32_t)offset & BX_MASK) << REG_SHIFT_B);
        compiler->compiling_chunk->code[jump_address] = patched;
    }
}

static int try_emit_branch_compare(Compiler* compiler, Expr* condition, bool jump_if_true, int line) {
    if (condition->type != EXPR_BINARY) {
        return -1;
    }

    BinaryExpr* bin = &condition->as.binary;
    TokenType op = bin->operator.type;

    bool is_comparison = (op == TOKEN_LESS || op == TOKEN_LESS_EQUAL ||
                         op == TOKEN_GREATER || op == TOKEN_GREATER_EQUAL ||
                         op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL);

    if (!is_comparison) {
        return -1;
    }

    TokenType effective_op = op;
    if (!jump_if_true) {
        switch (op) {
            case TOKEN_LESS:          effective_op = TOKEN_GREATER_EQUAL; break;
            case TOKEN_LESS_EQUAL:    effective_op = TOKEN_GREATER; break;
            case TOKEN_GREATER:       effective_op = TOKEN_LESS_EQUAL; break;
            case TOKEN_GREATER_EQUAL: effective_op = TOKEN_LESS; break;
            case TOKEN_EQUAL_EQUAL:   effective_op = TOKEN_BANG_EQUAL; break;
            case TOKEN_BANG_EQUAL:    effective_op = TOKEN_EQUAL_EQUAL; break;
            default: return -1;
        }
    }

    bool right_is_const = (bin->right->type == EXPR_LITERAL &&
                          bin->right->as.literal.literal.type == TOKEN_NUMBER);

    double const_value = 0;
    bool use_immediate = false;
    bool use_literal = false;

    if (right_is_const) {
        const_value = parse_number_literal(bin->right->as.literal.literal.start,
                                          bin->right->as.literal.literal.length);

        if (const_value == floor(const_value)) {
            int64_t int_val = (int64_t)const_value;
            if (int_val >= -32768 && int_val <= 32767) {
                use_immediate = true;
            } else {
                use_literal = true;
            }
        } else {
            use_literal = true;
        }
    }

    OpCode branch_op;
    switch (effective_op) {
        case TOKEN_LESS:          branch_op = use_immediate ? BRANCH_LT_I : (use_literal ? BRANCH_LT_L : BRANCH_LT); break;
        case TOKEN_LESS_EQUAL:    branch_op = use_immediate ? BRANCH_LE_I : (use_literal ? BRANCH_LE_L : BRANCH_LE); break;
        case TOKEN_GREATER:       branch_op = use_immediate ? BRANCH_GT_I : (use_literal ? BRANCH_GT_L : BRANCH_GT); break;
        case TOKEN_GREATER_EQUAL: branch_op = use_immediate ? BRANCH_GE_I : (use_literal ? BRANCH_GE_L : BRANCH_GE); break;
        case TOKEN_EQUAL_EQUAL:   branch_op = use_immediate ? BRANCH_EQ_I : (use_literal ? BRANCH_EQ_L : BRANCH_EQ); break;
        case TOKEN_BANG_EQUAL:    branch_op = use_immediate ? BRANCH_NE_I : (use_literal ? BRANCH_NE_L : BRANCH_NE); break;
        default: return -1;
    }

    int left_reg = compile_sub_expression(compiler, bin->left);

    if (use_immediate) {
        int64_t int_val = (int64_t)const_value;
        uint32_t imm_bits = (uint32_t)(int_val & 0xFFFF);
        emit_instruction(compiler, PACK_ABx(branch_op, left_reg, imm_bits), line);
        int jump_addr = compiler->compiling_chunk->count;
        emit_instruction(compiler, JUMP_PLACEHOLDER, line);
        return jump_addr;
    } else if (use_literal) {
        emit_instruction(compiler, PACK_ABx(branch_op, left_reg, 0), line);
        write64BitLiteral(compiler->vm, compiler->compiling_chunk, const_value, line);
        int jump_addr = compiler->compiling_chunk->count;
        emit_instruction(compiler, JUMP_PLACEHOLDER, line);
        return jump_addr;
    } else {
        return -1;
    }
}

static void emit_loop(Compiler* compiler, int loop_start, int line) {
    int offset = loop_start - (compiler->compiling_chunk->count + 1);
    if (offset < MAX_JUMP_OFFSET_NEG) {
        printf("Error: Loop body too large.\n");
    }
    emit_instruction(compiler, PACK_ABx(JUMP, 0, (uint32_t)offset), line);
}

static void add_break_jump(Compiler* compiler, int jump_address) {
    if (compiler->break_capacity < compiler->break_count + 1) {
        int old_cap = compiler->break_capacity;
        compiler->break_capacity = old_cap < 8 ? 8 : old_cap * 2;
        compiler->break_jumps = (int*)reallocate(compiler->vm, compiler->break_jumps, sizeof(int) * old_cap, sizeof(int) * compiler->break_capacity);
    }
    compiler->break_jumps[compiler->break_count++] = jump_address;
}

static void end_scope(Compiler* c) {
    c->scope_depth--;
    while (c->local_count > 0 && c->locals[c->local_count - 1].depth > c->scope_depth) {
        emit_instruction(c, PACK_ABx(CLOSE_UPVALUE, c->locals[c->local_count - 1].reg, 0), 0);
        int r = c->locals[--c->local_count].reg;
    }
}

static int resolve_ref_target(Compiler* compiler, int reg) {
    for (int i = 0; i < compiler->local_count; i++) {
        if (compiler->locals[i].reg == reg && compiler->locals[i].is_reference) {
            int target = compiler->locals[i].ref_target_reg;
            if (target >= 0) {
                return resolve_ref_target(compiler, target);
            } else {
                return reg;
            }
        }
    }
    return reg;
}

static int resolve_ref_target_name(Compiler* compiler, Token* name) {
    int ar = single_hoisted_arity(compiler, name);
    if (ar >= 0) {
        char* mangled = mangle_name(compiler, name, ar);

        ObjString* str = copyString(compiler->vm, mangled, (int)strlen(mangled));
        pushTempRoot(compiler->vm, (Obj*)str);
        int k = make_constant(compiler, OBJ_VAL(str));
        popTempRoot(compiler->vm);

        FREE_ARRAY(compiler->vm, char, mangled, strlen(mangled) + 1);
        return k;
    } else {
        return identifier_constant(compiler, name);
    }
}

static int resolve_local(Compiler* compiler, Token* name) {
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (tokens_equal(name, &local->name)) {
            return local->reg;
        }
    }
    return -1;
}

static int add_upvalue(Compiler* compiler, uint8_t index, bool is_local, ObjStructSchema* struct_type) {
    for (int i = 0; i < compiler->upvalue_count; i++) {
        Upvalue* upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->is_local == is_local) {
            return i;
        }
    }

    if (compiler->upvalue_count == MAX_LOCALS) {
        printf("Too many upvalues in function.\n");
        return -1;
    }

    compiler->upvalues[compiler->upvalue_count].is_local = is_local;
    compiler->upvalues[compiler->upvalue_count].index = index;
    compiler->upvalues[compiler->upvalue_count].struct_type = struct_type;
    return compiler->upvalue_count++;
}

static int single_local_hoisted_arity(Compiler* c, const Token* name) {
    int found = -1;
    bool multiple = false;
    for (int i = 0; i < c->local_hoisted_count; i++) {
        if (c->local_hoisted[i].name.length == name->length &&
            memcmp(c->local_hoisted[i].name.start, name->start, name->length) == 0) {
            if (found == -1) found = c->local_hoisted[i].arity;
            else if (c->local_hoisted[i].arity != found) { multiple = true; break; }
            }
    }
    if (multiple) return -2;
    return found; // -1: none, >=0: unique arity
}

static bool all_digits(const char* s, int start, int end) {
    for (int i = start; i < end; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

static int resolve_mangled_local_by_base(Compiler* c, const Token* base) {
    int found_reg = -1;
    int found_count = 0;
    for (int i = c->local_count - 1; i >= 0; i--) {
        Local* local = &c->locals[i];
        int L = local->name.length;
        if (L < base->length + 2) continue; // need at least "x@0"
        if (memcmp(local->name.start, base->start, base->length) != 0) continue;
        if (local->name.start[base->length] != '@') continue;
        if (!all_digits(local->name.start, base->length + 1, L)) continue;
        found_reg = local->reg;
        found_count++;
    }
    return (found_count == 1) ? found_reg : -1;
}

static int resolve_upvalue(Compiler* compiler, Token* name) {
    if (compiler->enclosing == NULL) return -1;

    int local = resolve_local(compiler->enclosing, name);
    if (local != -1) {
        Local* parent_local = get_local_by_reg(compiler->enclosing, local);
        ObjStructSchema* struct_type = parent_local ? parent_local->struct_type : NULL;
        return add_upvalue(compiler, (uint8_t)local, true, struct_type);
    }

    int mlocal = resolve_mangled_local_by_base(compiler->enclosing, name);
    if (mlocal != -1) {
        Local* parent_local = get_local_by_reg(compiler->enclosing, mlocal);
        ObjStructSchema* struct_type = parent_local ? parent_local->struct_type : NULL;
        return add_upvalue(compiler, (uint8_t)mlocal, true, struct_type);
    }

    int up = resolve_upvalue(compiler->enclosing, name);
    if (up != -1) {
        ObjStructSchema* struct_type = compiler->enclosing->upvalues[up].struct_type;
        return add_upvalue(compiler, (uint8_t)up, false, struct_type);
    }

    return -1;
}

static void add_local_at_reg(Compiler* compiler, Token name, int reg) {
    if (compiler->local_count >= MAX_LOCALS) {
        compiler_error(compiler, -1, "Too many local variables (%d). Maximum is %d per function.", compiler->local_count + 1, MAX_LOCALS);
        return;
    }
    Local* local = &compiler->locals[compiler->local_count++];
    local->name = name;
    local->depth = compiler->scope_depth;
    local->reg = reg;
    local->is_initialized = true;
    local->is_reference = false;
    local->is_ref_param = false;
    local->is_slot_param = false;
    local->ref_target_reg = -1;
    local->struct_type = NULL;
}

static int add_local(Compiler* compiler, Token name) {
    if (compiler->local_count >= MAX_LOCALS) {
        compiler_error(compiler, -1, "Too many local variables (%d). Maximum is %d per function.", compiler->local_count + 1, MAX_LOCALS);
        return -1;
    }
    Local* local = &compiler->locals[compiler->local_count++];
    local->name = name;
    local->depth = compiler->scope_depth;
    local->reg = compiler->next_register;
    local->is_initialized = false;
    local->is_reference = false;
    local->is_ref_param = false;
    local->is_slot_param = false;
    local->ref_target_reg = -1;
    local->struct_type = NULL;
    return reserve_register(compiler);
}

static void declare_variable(Compiler* compiler, Token* name) {
    if (compiler->scope_depth == 0) return;
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (local->depth != -1 && local->depth < compiler->scope_depth) break;
        if (tokens_equal(name, &local->name)) {
            printf("Error: Already a variable with this name in this scope.\n");
        }
    }
}

static ObjStructSchema* get_struct_schema(Compiler* compiler, const Token* name) {
    for (int i = compiler->struct_schema_count - 1; i >= 0; i--) {
        if (tokens_equal(name, &compiler->struct_schemas[i].name)) {
            return compiler->struct_schemas[i].schema;
        }
    }

    if (compiler->enclosing) {
        return get_struct_schema(compiler->enclosing, name);
    }

    return NULL;
}

static ObjEnumSchema* get_enum_schema(Compiler* compiler, const Token* name) {
    for (int i = compiler->enum_schema_count - 1; i >= 0; i--) {
        if (tokens_equal(name, &compiler->enum_schemas[i].name)) {
            return compiler->enum_schemas[i].schema;
        }
    }

    if (compiler->enclosing) {
        return get_enum_schema(compiler->enclosing, name);
    }

    return NULL;
}

static void record_global_type(Compiler* compiler, ObjString* var_name, ObjStructSchema* schema) {
    Compiler* root = compiler;
    while (root->enclosing) root = root->enclosing;

    if (root->global_type_count >= root->global_type_capacity) {
        int old_cap = root->global_type_capacity;
        int new_cap = old_cap < 8 ? 8 : old_cap * 2;
        size_t elem_size = sizeof(root->global_types[0]);
        root->global_types = (void*)reallocate(
            root->vm,
            root->global_types,
            elem_size * old_cap,
            elem_size * new_cap
        );
        root->global_type_capacity = new_cap;
    }

    root->global_types[root->global_type_count].name = var_name;
    root->global_types[root->global_type_count].schema = schema;
    root->global_type_count++;
}

static ObjStructSchema* get_global_type(Compiler* compiler, const Token* name) {
    Compiler* root = compiler;
    while (root->enclosing) root = root->enclosing;

    for (int i = 0; i < root->global_type_count; i++) {
        if (root->global_types[i].name->length == name->length &&
            memcmp(root->global_types[i].name->chars, name->start, name->length) == 0) {
            return root->global_types[i].schema;
        }
    }
    return NULL;
}

static inline bool can_use_target_directly(Compiler* c, int target_reg) {
    return target_reg >= c->local_count;
}

static int compile_sub_expression_to(Compiler* c, Expr* e, int preferred_target) {
    if (e->type == EXPR_VARIABLE) {
        int reg = resolve_local(c, &e->as.variable.name);
        if (reg != -1) {
            bool is_ref = false;
            for (int i = 0; i < c->local_count; i++) {
                if (c->locals[i].reg == reg && c->locals[i].is_reference) {
                    is_ref = true;
                    break;
                }
            }

            if (!is_ref) {
                return reg;
            }
        }
    }

    bool old_needed = c->result_needed;
    c->result_needed = true;

    if (preferred_target >= 0 && can_use_target_directly(c, preferred_target)) {
        compile_expression(c, e, preferred_target);
        c->result_needed = old_needed;
        return preferred_target;
    }

    int r = alloc_temp(c);
    compile_expression(c, e, r);
    c->result_needed = old_needed;
    return r;
}

static int compile_sub_expression(Compiler* c, Expr* e) {
    return compile_sub_expression_to(c, e, -1);
}

static Compiler* root_compiler(Compiler* c) {
    while (c->enclosing) c = c->enclosing;
    return c;
}

typedef enum {
    HOISTED_GLOBAL,
    HOISTED_LOCAL,
    HOISTED_ENCLOSING
} HoistedScope;

static bool find_hoisted_function(Compiler* c, const Token* name, int arity, HoistedScope scope) {
    switch (scope) {
        case HOISTED_GLOBAL: {
            Compiler* root = root_compiler(c);
            for (int i = 0; i < root->hoisted_count; i++) {
                if (root->hoisted[i].arity == arity && tokens_equal(&root->hoisted[i].name, name)) {
                    return true;
                }
            }
            return false;
        }

        case HOISTED_LOCAL: {
            for (int i = 0; i < c->local_hoisted_count; i++) {
                if (c->local_hoisted[i].arity == arity && tokens_equal(&c->local_hoisted[i].name, name)) {
                    return true;
                }
            }
            return false;
        }

        case HOISTED_ENCLOSING: {
            Compiler* enclosing = c->enclosing;
            while (enclosing) {
                for (int i = 0; i < enclosing->local_hoisted_count; i++) {
                    if (enclosing->local_hoisted[i].arity == arity &&
                        tokens_equal(&enclosing->local_hoisted[i].name, name)) {
                        return true;
                    }
                }
                enclosing = enclosing->enclosing;
            }
            return false;
        }
    }
    return false;
}

static bool is_hoisted_global(Compiler* c, const Token* name, int arity) {
    return find_hoisted_function(c, name, arity, HOISTED_GLOBAL);
}

static bool is_hoisted_local(Compiler* c, const Token* name, int arity) {
    return find_hoisted_function(c, name, arity, HOISTED_LOCAL);
}

static bool is_hoisted_in_enclosing(Compiler* c, const Token* name, int arity) {
    return find_hoisted_function(c, name, arity, HOISTED_ENCLOSING);
}

static int single_hoisted_arity(Compiler* c, const Token* name) {
    Compiler* root = root_compiler(c);
    int found = -1;
    bool multiple_different = false;

    for (int i = 0; i < root->hoisted_count; i++) {
        if (tokens_equal(&root->hoisted[i].name, name)) {
            if (found == -1) {
                found = root->hoisted[i].arity;        // first arity seen
            } else if (root->hoisted[i].arity != found) {
                multiple_different = true;              // different arity also found
                break;
            }
        }
    }

    if (multiple_different) return -2;
    return found;
}

static void track_owned_name(Compiler* c, char* s) {
    if (!s) return;
    int old_cap = c->owned_names_cap;
    if (c->owned_names_count == old_cap) {
        int new_cap = old_cap < 8 ? 8 : old_cap * 2;
        c->owned_names = (char**)reallocate(
            c->vm,
            c->owned_names,
            sizeof(char*) * old_cap,
            sizeof(char*) * new_cap
        );
        c->owned_names_cap = new_cap;
    }
    c->owned_names[c->owned_names_count++] = s;
}

static void free_owned_names(Compiler* c) {
    for (int i = 0; i < c->owned_names_count; i++) {
        reallocate(c->vm, c->owned_names[i], strlen(c->owned_names[i]) + 1, 0);
    }
    reallocate(c->vm, c->owned_names, sizeof(char*) * c->owned_names_cap, 0);
    c->owned_names = NULL;
    c->owned_names_count = 0;
    c->owned_names_cap = 0;
}

static char* mangle_name_tracked(Compiler* c, Token* name, int arity) {
    char* mangled = mangle_name(c, name, arity);
    track_owned_name(c, mangled);
    return mangled;
}

typedef enum {
    REF_MODE_NORMAL,
    REF_MODE_SLOT
} RefCreationMode;

typedef struct {
    Compiler* c;
    char* str;
} ScopedString;

static ScopedString scoped_mangle(Compiler* c, Token* name, int arity) {
    ScopedString result;
    result.c = c;
    result.str = mangle_name(c, name, arity);
    return result;
}

static void scoped_string_free(ScopedString* s) {
    if (s->str) {
        FREE_ARRAY(s->c->vm, char, s->str, strlen(s->str) + 1);
        s->str = NULL;
    }
}

// Helper: Create reference to a variable (handles local/upvalue/global with function name mangling)
static void emit_variable_reference_typed(Compiler* compiler, Token* var_name, int target_reg, int line, RefCreationMode ref_mode) {
    int var_reg = resolve_local(compiler, var_name);

    // If not found as exact local name, try resolving as a local function by base name
    if (var_reg == -1) {
        int arity = single_local_hoisted_arity(compiler, var_name);
        if (arity >= 0) {
            // It's a unique local function - try resolving by mangled name
            var_reg = resolve_mangled_local_by_base(compiler, var_name);
        } else if (arity == -2) {
            compiler_error_and_exit(line, "Cannot create reference to overloaded function '%.*s'. Store the function in a variable first, then create a reference to that variable.", var_name->length, var_name->start);
        }
    }

    if (var_reg != -1) {
        // Local variable - create local reference
        if (ref_mode == REF_MODE_NORMAL) {
            int ultimate_target = resolve_ref_target(compiler, var_reg);
            emit_instruction(compiler, PACK_ABC(MAKE_REF, target_reg, ultimate_target, 0), line);
        } else {
            emit_instruction(compiler, PACK_ABC(SLOT_MAKE_REF, target_reg, var_reg, 0), line);
        }
    } else if ((var_reg = resolve_upvalue(compiler, var_name)) != -1) {
        // Upvalue - create upvalue reference
        emit_instruction(compiler, PACK_ABx(MAKE_UPVALUE_REF, target_reg, var_reg), line);
    } else {
        // Check if this is an overloaded global function
        int arity = single_hoisted_arity(compiler, var_name);
        if (arity == -2) {
            compiler_error_and_exit(line, "Cannot create reference to overloaded function '%.*s'. Store the function in a variable first, then create a reference to that variable.",
                    var_name->length, var_name->start);
        }
        // Global variable - create global reference (handles function name mangling)
        int name_const = resolve_ref_target_name(compiler, var_name);
        OpCode opcode = (ref_mode == REF_MODE_NORMAL) ? MAKE_GLOBAL_REF : SLOT_MAKE_GLOBAL_REF;
        emit_instruction(compiler, PACK_ABx(opcode, target_reg, name_const), line);
    }
}

static void emit_variable_reference(Compiler* compiler, Token* var_name, int target_reg, int line) {
    emit_variable_reference_typed(compiler, var_name, target_reg, line, REF_MODE_NORMAL);
}

// Helper: Create reference to array[index]
static void emit_subscript_reference_typed(Compiler* compiler, SubscriptExpr* sub_expr, int target_reg, int line, RefCreationMode ref_mode) {
    int container_reg = alloc_temp(compiler);
    int index_reg = alloc_temp(compiler);

    COMPILE_REQUIRED(compiler, sub_expr->object, container_reg);
    COMPILE_REQUIRED(compiler, sub_expr->index, index_reg);

    OpCode opcode = (ref_mode == REF_MODE_NORMAL) ? MAKE_INDEX_REF : SLOT_MAKE_INDEX_REF;
    emit_instruction(compiler, PACK_ABC(opcode, target_reg, container_reg, index_reg), line);
}

static void emit_subscript_reference(Compiler* compiler, SubscriptExpr* sub_expr, int target_reg, int line) {
    emit_subscript_reference_typed(compiler, sub_expr, target_reg, line, REF_MODE_NORMAL);
}

// Helper: Create reference to map.property
static void emit_property_reference_typed(Compiler* compiler, GetExpr* get_expr, int target_reg, int line, RefCreationMode ref_mode) {
    int container_reg = alloc_temp(compiler);
    int key_reg = alloc_temp(compiler);

    COMPILE_REQUIRED(compiler, get_expr->object, container_reg);

    // Convert property name to string constant
    int key_const = identifier_constant(compiler, &get_expr->name);
    emit_load_const(compiler, key_reg, key_const, line);

    OpCode opcode = (ref_mode == REF_MODE_NORMAL) ? MAKE_PROPERTY_REF : SLOT_MAKE_PROPERTY_REF;
    emit_instruction(compiler, PACK_ABC(opcode, target_reg, container_reg, key_reg), line);
}

static void emit_property_reference(Compiler* compiler, GetExpr* get_expr, int target_reg, int line) {
    emit_property_reference_typed(compiler, get_expr, target_reg, line, REF_MODE_NORMAL);
}

// Helper: Create reference from an expression (used for ref variable initialization)
static void emit_reference_from_expr(Compiler* compiler, Expr* initializer, int target_reg, int line) {
    if (initializer->type == EXPR_VARIABLE) {
        emit_variable_reference(compiler, &initializer->as.variable.name, target_reg, line);
    } else if (initializer->type == EXPR_SUBSCRIPT) {
        emit_subscript_reference(compiler, &initializer->as.subscript, target_reg, line);
    } else if (initializer->type == EXPR_GET) {
        emit_property_reference(compiler, &initializer->as.get, target_reg, line);
    } else {
        // Provide specific error messages for common mistakes
        const char* error_msg;
        if (initializer->type == EXPR_LITERAL) {
            error_msg = "Cannot create reference to literal value (number, string, boolean, null). References must point to variables, array elements, or map properties.";
        } else if (initializer->type == EXPR_CALL) {
            error_msg = "Cannot create reference directly to function call result. Assign the result to a variable first, then create a reference to that variable.";
        } else if (initializer->type == EXPR_BINARY || initializer->type == EXPR_UNARY) {
            error_msg = "Cannot create reference to expression result. Assign the expression to a variable first, then create a reference to that variable.";
        } else if (initializer->type == EXPR_LIST || initializer->type == EXPR_MAP) {
            error_msg = "Cannot create reference to inline list or map literal. Assign the literal to a variable first, then create a reference to that variable.";
        } else {
            error_msg = "Invalid reference target. References can only point to variables, array elements (array[index]), or map properties (map.property).";
        }
        compiler_error_and_exit(line, "%s", error_msg);
    }
}


// Helper: Compile argument for ref parameter
// Creates a reference to the argument that will be passed to the function
static void compile_ref_param_argument(Compiler* compiler, Expr* arg, int arg_slot, int line) {
    if (arg->type == EXPR_VARIABLE) {
        Token* var_name = &arg->as.variable.name;
        int var_reg = resolve_local(compiler, var_name);

        if (var_reg != -1) {
            if (is_local_ref_param(compiler, var_reg)) {
                // Already a ref parameter - pass the reference directly
                emit_move(compiler, arg_slot, var_reg, line);
            } else {
                // Local variable - create a reference to it
                emit_instruction(compiler, PACK_ABC(MAKE_REF, arg_slot, var_reg, 0), line);
            }
        } else if ((var_reg = resolve_upvalue(compiler, var_name)) != -1) {
            // Upvalue - create upvalue reference
            emit_instruction(compiler, PACK_ABx(MAKE_UPVALUE_REF, arg_slot, var_reg), line);
        } else {
            // Global variable - create global reference
            int name_const = identifier_constant(compiler, var_name);
            emit_instruction(compiler, PACK_ABx(MAKE_GLOBAL_REF, arg_slot, name_const), line);
        }
    } else if (arg->type == EXPR_SUBSCRIPT) {
        emit_subscript_reference(compiler, &arg->as.subscript, arg_slot, line);
    } else if (arg->type == EXPR_GET) {
        emit_property_reference(compiler, &arg->as.get, arg_slot, line);
    } else {
        // Complex expression - evaluate and create reference to result slot
        COMPILE_REQUIRED(compiler, arg, arg_slot);
        int temp_ref = alloc_temp(compiler);
        emit_instruction(compiler, PACK_ABC(MAKE_REF, temp_ref, arg_slot, 0), line);
        emit_move(compiler, arg_slot, temp_ref, line);
    }
}

// Helper: Compile argument for slot parameter
// Creates a NON-FLATTENING reference for slot semantics
static void compile_slot_param_argument(Compiler* compiler, Expr* arg, int arg_slot, int line) {
    if (arg->type == EXPR_VARIABLE) {
        Token* var_name = &arg->as.variable.name;
        int var_reg = resolve_local(compiler, var_name);

        if (var_reg != -1 && (is_local_ref_or_slot_param(compiler, var_reg) || is_local_holding_reference(compiler, var_reg))) {
            // Already a ref/slot parameter or holds a reference - pass directly
            emit_move(compiler, arg_slot, var_reg, line);
        } else {
            // Use typed reference creator with SLOT mode
            emit_variable_reference_typed(compiler, var_name, arg_slot, line, REF_MODE_SLOT);
        }
    } else if (arg->type == EXPR_SUBSCRIPT) {
        emit_subscript_reference_typed(compiler, &arg->as.subscript, arg_slot, line, REF_MODE_SLOT);
    } else if (arg->type == EXPR_GET) {
        emit_property_reference_typed(compiler, &arg->as.get, arg_slot, line, REF_MODE_SLOT);
    } else {
        // Complex expression - evaluate and create reference
        COMPILE_REQUIRED(compiler, arg, arg_slot);
        int temp_ref = alloc_temp(compiler);
        emit_instruction(compiler, PACK_ABC(MAKE_REF, temp_ref, arg_slot, 0), line);
        emit_move(compiler, arg_slot, temp_ref, line);
    }
}

// Helper: Compile argument for dynamic call (no known signature)
// Passes l-values as references to enable ref/slot params to work for closures
static void compile_dynamic_call_argument(Compiler* compiler, Expr* arg, int arg_slot, int line) {
    if (arg->type == EXPR_VARIABLE) {
        Token* var_name = &arg->as.variable.name;
        int var_reg = resolve_local(compiler, var_name);

        if (var_reg != -1) {
            if (is_local_ref_or_slot_param(compiler, var_reg)) {
                // Already a ref/slot parameter - pass directly
                emit_move(compiler, arg_slot, var_reg, line);
            } else {
                // Local variable - use non-flattening ref
                emit_instruction(compiler, PACK_ABC(SLOT_MAKE_REF, arg_slot, var_reg, 0), line);
            }
        } else if ((var_reg = resolve_upvalue(compiler, var_name)) != -1) {
            // Upvalue - load it first
            emit_get_upvalue(compiler, arg_slot, var_reg, line);
        } else {
            // Global variable - use non-flattening ref
            int name_const = resolve_ref_target_name(compiler, var_name);
            emit_instruction(compiler, PACK_ABx(SLOT_MAKE_GLOBAL_REF, arg_slot, name_const), line);
        }
    } else if (arg->type == EXPR_GET) {
        // Check if this is an enum value (EnumName.VARIANT) before treating it as property access
        GetExpr* get_expr = &arg->as.get;
        if (get_expr->object->type == EXPR_VARIABLE) {
            Token* enum_name = &get_expr->object->as.variable.name;
            ObjEnumSchema* enum_schema = get_enum_schema(compiler, enum_name);

            if (enum_schema) {
                // This is an enum value - just compile it as an expression
                COMPILE_REQUIRED(compiler, arg, arg_slot);
                return;
            }
        }

        // Not an enum - treat as property reference
        emit_property_reference_typed(compiler, &arg->as.get, arg_slot, line, REF_MODE_SLOT);
    } else if (arg->type == EXPR_SUBSCRIPT) {
        emit_subscript_reference_typed(compiler, &arg->as.subscript, arg_slot, line, REF_MODE_SLOT);
    } else {
        // Complex expression - evaluate normally
        COMPILE_REQUIRED(compiler, arg, arg_slot);
    }
}

// Helper: Check if TCO is compile-time safe for a given function call
static bool is_tco_compile_time_safe(Compiler* compiler, Token* name, int arg_count) {
    // Check 1: Is this self-recursion?
    if (compiler->function && compiler->function->name &&
        compiler->function->name->length == name->length &&
        memcmp(compiler->function->name->chars, name->start, name->length) == 0) {
        return compiler->upvalue_count == 0;
    }

    // Check 2: Mangled self-recursion (overloaded function calling itself)
    ScopedString mangled = scoped_mangle(compiler, name, arg_count);
    bool is_self = false;
    if (compiler->function && compiler->function->name &&
        compiler->function->name->length == (int)strlen(mangled.str) &&
        memcmp(compiler->function->name->chars, mangled.str, strlen(mangled.str)) == 0) {
        is_self = (compiler->upvalue_count == 0);
    }
    scoped_string_free(&mangled);
    if (is_self) return true;

    // Check 3: Hoisted function with zero upvalues (global)
    Compiler* root = root_compiler(compiler);
    for (int i = 0; i < root->hoisted_count; i++) {
        if (tokens_equal(&root->hoisted[i].name, name) &&
            root->hoisted[i].arity == arg_count &&
            root->hoisted[i].upvalue_count == 0) {
            return true;
        }
    }

    // Check 4: Hoisted function with zero upvalues (local in any scope)
    for (Compiler* c = compiler; c != NULL; c = c->enclosing) {
        for (int i = 0; i < c->local_hoisted_count; i++) {
            if (tokens_equal(&c->local_hoisted[i].name, name) &&
                c->local_hoisted[i].arity == arg_count &&
                c->local_hoisted[i].upvalue_count == 0) {
                return true;
            }
        }
    }

    return false;
}

// Helper: Compile tail call - loads function into R0
static void compile_tco_callee(Compiler* compiler, Token* name, int arg_count, int call_base, int line) {
    int reg = -1;

    // Check for hoisted local functions
    if (is_hoisted_local(compiler, name, arg_count)) {
        ScopedString mangled = scoped_mangle(compiler, name, arg_count);
        Token mangled_token = { .start = mangled.str, .length = (int)strlen(mangled.str), .line = name->line };
        reg = resolve_local(compiler, &mangled_token);
        scoped_string_free(&mangled);
    }

    // Check if it's a hoisted global function with this arity (before falling back to plain local)
    if (reg == -1 && is_hoisted_global(compiler, name, arg_count)) {
        // Don't resolve as local; will be handled in the global section
        reg = -1;
    }
    // Fall back to plain locals or a single "<base>@digits" block-local
    else if (reg == -1) {
        reg = resolve_local(compiler, name);
        if (reg == -1) {
            reg = resolve_mangled_local_by_base(compiler, name);
        }
    }

    if (reg != -1) {
        emit_move(compiler, call_base, reg, line);
    } else if (is_hoisted_in_enclosing(compiler, name, arg_count)) {
        ScopedString mangled = scoped_mangle(compiler, name, arg_count);
        Token mangled_token = { .start = mangled.str, .length = (int)strlen(mangled.str), .line = name->line };
        reg = resolve_upvalue(compiler, &mangled_token);
        scoped_string_free(&mangled);
        if (reg != -1) {
            emit_get_upvalue(compiler, call_base, reg, line);
        } else {
            reg = resolve_upvalue(compiler, name);
            if (reg != -1) {
                emit_get_upvalue(compiler, call_base, reg, line);
            } else {
                int name_const = identifier_constant(compiler, name);
                emit_get_global(compiler, call_base, name_const, line);
            }
        }
    } else if ((reg = resolve_upvalue(compiler, name)) != -1) {
        emit_get_upvalue(compiler, call_base, reg, line);
    } else {
        bool should_mangle = false;

        if (is_hoisted_global(compiler, name, arg_count)) {
            should_mangle = true;
        } else {
            // Check if it's a registered native function
            // Native functions are registered with mangled names (e.g., "print@1")
            ScopedString mangled = scoped_mangle(compiler, name, arg_count);
            ObjString* mangled_str = copyString(compiler->vm, mangled.str, (int)strlen(mangled.str));
            pushTempRoot(compiler->vm, (Obj*)mangled_str);
            Value func_val;
            if (tableGet(&compiler->vm->globals, mangled_str, &func_val) && IS_NATIVE_FUNCTION(func_val)) {
                should_mangle = true;
            }
            popTempRoot(compiler->vm);
            scoped_string_free(&mangled);
        }

        if (should_mangle) {
            ScopedString mangled = scoped_mangle(compiler, name, arg_count);
            ObjString* str = copyString(compiler->vm, mangled.str, (int)strlen(mangled.str));
            pushTempRoot(compiler->vm, (Obj*)str);
            int name_const = make_constant(compiler, OBJ_VAL(str));
            popTempRoot(compiler->vm);
            scoped_string_free(&mangled);
            emit_get_global(compiler, call_base, name_const, line);
        } else {
            int name_const = identifier_constant(compiler, name);
            emit_get_global(compiler, call_base, name_const, line);
        }
    }
}

// Helper: Compile tail call optimization for return statement
static bool try_compile_tail_call(Compiler* compiler, Expr* return_expr, int line) {
    // Check if this is even a call expression
    CallExpr* call_expr = &return_expr->as.call;
    Expr* callee = call_expr->callee;
    int arg_count = call_expr->arg_count;

    // Determine if this tail call can be verified safe at compile-time
    bool compile_time_safe = false;
    bool use_smart_fallback = false;

    // Try to verify safety at compile-time (for SAFE and SMART modes)
    if (compiler->tco_mode == TCO_SAFE || compiler->tco_mode == TCO_SMART) {
        // Can only verify safety for variable calls (not arr[0], obj.method, etc.)
        if (callee->type == EXPR_VARIABLE) {
            Token* name = &callee->as.variable.name;
            compile_time_safe = is_tco_compile_time_safe(compiler, name, arg_count);
        }

        // If not compile-time safe, decide what to do based on mode
        if (!compile_time_safe) {
            if (compiler->tco_mode == TCO_SAFE) {
                // SAFE mode: Can't verify at compile-time, so don't optimize
                return false;
            } else {
                // SMART mode: Can't verify at compile-time, use runtime check
                use_smart_fallback = true;
            }
        }
    }
    // TCO_AGGRESSIVE: Optimize any call expression

    // Use register 0 as call base (where function will be)
    int call_base = 0;

    // Check if this is a recursive self-call (do this BEFORE compiling the callee)
    bool is_self_call = false;
    if (callee->type == EXPR_VARIABLE && compiler->function && compiler->function->name) {
        Token* name = &callee->as.variable.name;

        // Compare the unmangled names - the function name is stored unmangled
        if (compiler->function->name->length == name->length &&
            memcmp(compiler->function->name->chars, name->start, name->length) == 0) {
            // Same base name - now check if arity matches
            is_self_call = (compiler->function->arity == arg_count);
        }
    }

    // For self-calls, we don't need to load the callee - the VM will get it from the frame
    if (!is_self_call) {
        // Compile the callee into R0
        if (callee->type == EXPR_VARIABLE) {
            Token* name = &callee->as.variable.name;
            compile_tco_callee(compiler, name, arg_count, call_base, callee->line);
        } else {
            // TCO_AGGRESSIVE: Non-variable callee (e.g., arr[0], obj.method, lambda)
            compile_expression(compiler, callee, call_base);
        }
    }

    // Compile arguments into temporary registers first to avoid overwriting current parameters
    int* temp_regs = (int*)malloc(sizeof(int) * arg_count);
    for (int i = 0; i < arg_count; i++) {
        temp_regs[i] = reserve_register(compiler);
        Expr* arg = call_expr->args[i];

        // Check if arg is a ref/slot parameter or holds a reference - if so, don't dereference it
        if (arg->type == EXPR_VARIABLE) {
            Token* var_name = &arg->as.variable.name;
            int var_reg = resolve_local(compiler, var_name);

            if (var_reg != -1 && (is_local_ref_param(compiler, var_reg) || is_local_slot_param(compiler, var_reg) || is_local_holding_reference(compiler, var_reg))) {
                // Pass ref/slot parameter or reference-holding variable directly without dereferencing
                emit_move(compiler, temp_regs[i], var_reg, arg->line);
            } else {
                compile_expression(compiler, arg, temp_regs[i]);
            }
        } else {
            compile_expression(compiler, arg, temp_regs[i]);
        }
    }

    // CRITICAL: Close frame upvalues BEFORE moving arguments
    // This ensures closures capture the OLD parameter values before we overwrite them
    emit_instruction(compiler, PACK_ABx(CLOSE_FRAME_UPVALUES, 0, 0), line);

    // Now move them to R1, R2, R3, ... for the tail call
    for (int i = 0; i < arg_count; i++) {
        emit_move(compiler, 1 + i, temp_regs[i], line);
    }
    free(temp_regs);

    // Emit appropriate tail call instruction
    if (use_smart_fallback) {
        // Smart mode fallback: couldn't verify at compile-time, use runtime check
        if (is_self_call) {
            emit_instruction(compiler, PACK_ABx(SMART_TAIL_CALL_SELF, call_base, arg_count), line);
        } else {
            emit_instruction(compiler, PACK_ABx(SMART_TAIL_CALL, call_base, arg_count), line);
        }
        // IMPORTANT: After SMART_TAIL_CALL, if it falls back to normal call,
        // execution continues to the next instruction. We need to return the result.
        // The result will be in call_base (R0), so emit a RET for that register.
        emit_instruction(compiler, PACK_ABx(RET, call_base, 0), line);
    } else {
        // Compile-time verified safe OR aggressive mode: direct tail call
        if (is_self_call) {
            emit_instruction(compiler, PACK_ABx(TAIL_CALL_SELF, call_base, arg_count), line);
        } else {
            emit_instruction(compiler, PACK_ABx(TAIL_CALL, call_base, arg_count), line);
        }
    }

    return true;
}

// Helper: Create a dispatcher for overloaded functions by collecting all matching overloads
// Used when referencing an overloaded function name without specifying arity
static void emit_dispatcher(Compiler* compiler, Token* name, int target_reg, int line, bool is_local) {
    int overload_regs[MAX_OVERLOADS];
    int overload_count = 0;

    if (is_local) {
        // Collect local overloads
        for (int i = 0; i < compiler->local_hoisted_count && overload_count < MAX_OVERLOADS; i++) {
            if (tokens_equal(&compiler->local_hoisted[i].name, name)) {
                // Found an overload - resolve its register
                ScopedString mangled = scoped_mangle(compiler, name, compiler->local_hoisted[i].arity);
                Token mtoken = { .start = mangled.str, .length = (int)strlen(mangled.str), .line = name->line };
                int mreg = resolve_local(compiler, &mtoken);
                scoped_string_free(&mangled);

                if (mreg != -1) {
                    overload_regs[overload_count++] = mreg;
                }
            }
        }
    } else {
        // Collect global overloads
        Compiler* root = root_compiler(compiler);
        for (int i = 0; i < root->hoisted_count && overload_count < MAX_OVERLOADS; i++) {
            if (tokens_equal(&root->hoisted[i].name, name)) {
                // Found an overload - load it into a temp register
                ScopedString mangled = scoped_mangle(compiler, name, root->hoisted[i].arity);
                ObjString* str = copyString(compiler->vm, mangled.str, (int)strlen(mangled.str));
                pushTempRoot(compiler->vm, (Obj*)str);
                int k = make_constant(compiler, OBJ_VAL(str));
                popTempRoot(compiler->vm);
                scoped_string_free(&mangled);

                int temp_reg = alloc_temp(compiler);
                emit_get_global(compiler, temp_reg, k, line);
                overload_regs[overload_count++] = temp_reg;
            }
        }
    }

    if (overload_count > 0) {
        // Create the dispatcher
        emit_instruction(compiler, PACK_ABx(NEW_DISPATCHER, target_reg, 0), line);

        // Add all overloads
        for (int i = 0; i < overload_count; i++) {
            emit_instruction(compiler, PACK_ABC(ADD_OVERLOAD, target_reg, overload_regs[i], 0), line);
        }
    }
}

// Helper: Resolve and load a function by name with arity into target register
// Handles mangled names, hoisted functions, locals, upvalues, and globals
// Returns true if function was resolved, false otherwise
static bool resolve_and_load_function(Compiler* compiler, Token* name, int arg_count, int target_reg, int line) {
    int reg = -1;

    // 1. Prioritize hoisted local functions (for overloading)
    if (is_hoisted_local(compiler, name, arg_count)) {
        char* mangled = mangle_name(compiler, name, arg_count);
        Token mangled_token = { .start = mangled, .length = (int)strlen(mangled), .line = name->line };
        reg = resolve_local(compiler, &mangled_token);
        FREE_ARRAY(compiler->vm, char, mangled, strlen(mangled) + 1);
    }

    // 2. Check if it's a hoisted global function with this arity (before falling back to plain local)
    if (reg == -1 && is_hoisted_global(compiler, name, arg_count)) {
        // Don't resolve as local; will be handled in step 4
        reg = -1;
    }
    // 3. Fall back to plain locals or a single "<base>@digits" block-local
    else if (reg == -1) {
        reg = resolve_local(compiler, name);
        if (reg == -1) {
            reg = resolve_mangled_local_by_base(compiler, name);
        }
    }

    if (reg != -1) {
        // Found as a local (either mangled or plain)
        emit_move(compiler, target_reg, reg, line);
        return true;
    } else if (is_hoisted_in_enclosing(compiler, name, arg_count)) {
        // 3a. Check enclosing scope for overloaded functions
        char* mangled = mangle_name(compiler, name, arg_count);
        Token mangled_token = { .start = mangled, .length = (int)strlen(mangled), .line = name->line };
        reg = resolve_upvalue(compiler, &mangled_token);
        FREE_ARRAY(compiler->vm, char, mangled, strlen(mangled) + 1);
        if (reg != -1) {
            emit_instruction(compiler, PACK_ABx(GET_UPVALUE, target_reg, reg), line);
            return true;
        } else {
            // Fall back to plain name
            reg = resolve_upvalue(compiler, name);
            if (reg != -1) {
                emit_instruction(compiler, PACK_ABx(GET_UPVALUE, target_reg, reg), line);
                return true;
            } else {
                // Treat as global
                int name_const = identifier_constant(compiler, name);
                emit_instruction(compiler, PACK_ABx(GET_GLOBAL, target_reg, name_const), line);
                return true;
            }
        }
    } else if ((reg = resolve_upvalue(compiler, name)) != -1) {
        // 3b. Check upvalues with plain name
        emit_instruction(compiler, PACK_ABx(GET_UPVALUE, target_reg, reg), line);
        return true;
    } else {
        // 4. Fall back to global scope
        bool should_mangle = false;

        // Check if it's a hoisted global function
        if (is_hoisted_global(compiler, name, arg_count)) {
            should_mangle = true;
        } else {
            // Check if it's a registered native function
            // Native functions are registered with mangled names (e.g., "native_add@2")
            char* mangled = mangle_name(compiler, name, arg_count);
            ObjString* mangled_str = copyString(compiler->vm, mangled, (int)strlen(mangled));
            pushTempRoot(compiler->vm, (Obj*)mangled_str);
            Value func_val;
            if (tableGet(&compiler->vm->globals, mangled_str, &func_val) && IS_NATIVE_FUNCTION(func_val)) {
                should_mangle = true;
            }
            popTempRoot(compiler->vm);
            FREE_ARRAY(compiler->vm, char, mangled, strlen(mangled) + 1);
        }

        if (should_mangle) {
            char* mangled = mangle_name(compiler, name, arg_count);
            ObjString* str = copyString(compiler->vm, mangled, (int)strlen(mangled));
            pushTempRoot(compiler->vm, (Obj*)str);
            int name_const = make_constant(compiler, OBJ_VAL(str));
            popTempRoot(compiler->vm);
            FREE_ARRAY(compiler->vm, char, mangled, strlen(mangled) + 1);
            emit_instruction(compiler, PACK_ABx(GET_GLOBAL, target_reg, name_const), line);
        } else {
            int name_const = identifier_constant(compiler, name);
            emit_instruction(compiler, PACK_ABx(GET_GLOBAL, target_reg, name_const), line);
        }
        return true;
    }
}

static void compile_expression(Compiler* compiler, Expr* expr, int target_reg) {
    // Defensive check: if expr is NULL, report error and emit null constant
    if (expr == NULL) {
        compiler->has_error = true;
        fprintf(stderr, "Internal compiler error: NULL expression encountered\n");
        // Emit a null constant as a safe fallback
        Value null_val = NULL_VAL;
        int const_idx = make_constant(compiler, null_val);
        emit_load_const(compiler, target_reg, const_idx, 0);
        return;
    }

    switch (expr->type) {
        case EXPR_VARIABLE: {
            Token name = expr->as.variable.name;
            int reg = resolve_local(compiler, &name);
            if (reg != -1) {
                // Check if this is a ref or slot parameter (which should auto-dereference on read)
                // Both ref and slot parameters dereference ONE level on read
                bool should_deref = false;
                for (int i = 0; i < compiler->local_count; i++) {
                    if (compiler->locals[i].reg == reg &&
                        (compiler->locals[i].is_ref_param || compiler->locals[i].is_slot_param)) {
                        should_deref = true;
                        break;
                    }
                }

                if (should_deref) {
                    // Ref or slot parameter: dereference ONE level to get the aliased value
                    emit_instruction(compiler, PACK_ABC(DEREF_GET, target_reg, reg, 0), expr->line);
                } else if (reg == target_reg) {
                    // Already in target register - no move needed!
                    // This is a common case and eliminates unnecessary MOVE dispatches
                } else {
                    // Regular variable or ref object: just move (refs are first-class values)
                    EMIT_MOVE_IF_NEEDED(compiler, target_reg, reg, expr->line);
                }
                break;
            }

            // handle block-scoped local functions like "f@0"
            reg = resolve_mangled_local_by_base(compiler, &name);
            if (reg != -1) {
                if (reg != target_reg) {
                    EMIT_MOVE_IF_NEEDED(compiler, target_reg, reg, expr->line);
                }
                break;
            }

            // if there's a uniquely hoisted *local* function with this base name,
            // read the *mangled* local symbol (e.g., "inc@0") even for plain variable access
            int lar = single_local_hoisted_arity(compiler, &name);
            if (lar >= 0) {
                ScopedString mangled = scoped_mangle(compiler, &name, lar);
                Token mtoken = { .start = mangled.str, .length = (int)strlen(mangled.str), .line = name.line };
                int mreg = resolve_local(compiler, &mtoken);
                if (mreg != -1) {
                    if (mreg != target_reg) {
                        EMIT_MOVE_IF_NEEDED(compiler, target_reg, mreg, expr->line);
                    }
                    scoped_string_free(&mangled);
                    break;
                }
                scoped_string_free(&mangled);
                // fall through if somehow not found
            } else if (lar == -2) {
                // Multiple local overloads exist - create a dispatcher
                emit_dispatcher(compiler, &name, target_reg, expr->line, true);
                break;
            }

            // upvalue and global logic
            if ((reg = resolve_upvalue(compiler, &name)) != -1) {
                emit_get_upvalue(compiler, target_reg, reg, expr->line);
            } else {
                int ar = single_hoisted_arity(compiler, &name);
                if (ar >= 0) {
                    // Single global overload with unique arity
                    ScopedString mangled = scoped_mangle(compiler, &name, ar);
                    ObjString* str = copyString(compiler->vm, mangled.str, (int)strlen(mangled.str));
                    pushTempRoot(compiler->vm, (Obj*)str);
                    int k = make_constant(compiler, OBJ_VAL(str));
                    popTempRoot(compiler->vm);
                    scoped_string_free(&mangled);
                    emit_get_global(compiler, target_reg, k, expr->line);
                } else if (ar == -2) {
                    // Multiple global overloads exist - create a dispatcher
                    emit_dispatcher(compiler, &name, target_reg, expr->line, false);
                } else {
                    // No overloads found
                    int k = identifier_constant(compiler, &name);
                    emit_get_global(compiler, target_reg, k, expr->line);
                }
            }
            break;
        }
        case EXPR_ASSIGN: {
            int saved_top = save_temp_top(compiler);

            // Check if this is a compound assignment (value is a binary expr with compound op)
            bool is_compound = false;
            OpCode binary_op = ADD;
            if (expr->as.assign.value->type == EXPR_BINARY) {
                TokenType op_type = expr->as.assign.value->as.binary.operator.type;
                // Check if it's a compound assignment token
                if (op_type >= TOKEN_PLUS_EQUAL && op_type <= TOKEN_UNSIGNED_RIGHT_SHIFT_EQUAL) {
                    is_compound = true;
                    binary_op = get_binary_op_from_compound(compiler, op_type);
                }
            }

            // Handle compound assignment for variables
            if (is_compound && expr->as.assign.target->type == EXPR_VARIABLE) {
                Token name = expr->as.assign.target->as.variable.name;
                int target_var_reg = resolve_local(compiler, &name);

                if (target_var_reg != -1) {
                    // Local variable compound assignment: target = target op value
                    // Check if this is a ref or slot parameter
                    bool is_ref_or_slot = false;
                    for (int i = 0; i < compiler->local_count; i++) {
                        if (compiler->locals[i].reg == target_var_reg &&
                            (compiler->locals[i].is_ref_param || compiler->locals[i].is_slot_param)) {
                            is_ref_or_slot = true;
                            break;
                        }
                    }

                    if (is_ref_or_slot) {
                        // Ref/slot parameter: need to deref, compute, then write through
                        int temp_reg = alloc_temp(compiler);
                        emit_instruction(compiler, PACK_ABC(DEREF_GET, temp_reg, target_var_reg, 0), expr->line);
                        int value_reg = alloc_temp(compiler);
                        COMPILE_REQUIRED(compiler, expr->as.assign.value->as.binary.right, value_reg);
                        emit_instruction(compiler, PACK_ABC(binary_op, temp_reg, temp_reg, value_reg), expr->line);
                        emit_instruction(compiler, PACK_ABC(DEREF_SET, target_var_reg, temp_reg, 0), expr->line);
                        EMIT_MOVE_IF_NEEDED(compiler, target_reg, temp_reg, expr->line);
                    } else {
                        // Normal variable: direct operation
                        int value_reg = alloc_temp(compiler);
                        COMPILE_REQUIRED(compiler, expr->as.assign.value->as.binary.right, value_reg);
                        emit_instruction(compiler, PACK_ABC(binary_op, target_var_reg, target_var_reg, value_reg), expr->line);
                        EMIT_MOVE_IF_NEEDED(compiler, target_reg, target_var_reg, expr->line);
                    }
                } else if ((target_var_reg = resolve_upvalue(compiler, &name)) != -1) {
                    // Upvalue compound assignment - need to load, modify, store
                    int temp_reg = alloc_temp(compiler);
                    emit_get_upvalue(compiler, temp_reg, target_var_reg, expr->line);
                    int value_reg = alloc_temp(compiler);
                    COMPILE_REQUIRED(compiler, expr->as.assign.value->as.binary.right, value_reg);
                    emit_instruction(compiler, PACK_ABC(binary_op, temp_reg, temp_reg, value_reg), expr->line);
                    emit_set_upvalue(compiler, temp_reg, target_var_reg, expr->line);
                    EMIT_MOVE_IF_NEEDED(compiler, target_reg, temp_reg, expr->line);
                } else {
                    // Global compound assignment - need to load, modify, store
                    int name_const = identifier_constant(compiler, &name);
                    int temp_reg = alloc_temp(compiler);
                    emit_get_global(compiler, temp_reg, name_const, expr->line);
                    int value_reg = alloc_temp(compiler);
                    COMPILE_REQUIRED(compiler, expr->as.assign.value->as.binary.right, value_reg);
                    emit_instruction(compiler, PACK_ABC(binary_op, temp_reg, temp_reg, value_reg), expr->line);
                    emit_set_global(compiler, temp_reg, name_const, expr->line);
                    EMIT_MOVE_IF_NEEDED(compiler, target_reg, temp_reg, expr->line);
                }
                restore_temp_top_preserve(compiler, saved_top, target_reg);
                break;
            }

            // list assignment
            if (expr->as.assign.target->type == EXPR_SUBSCRIPT) {
                SubscriptExpr* sub_expr = &expr->as.assign.target->as.subscript;

                // Compile the list, index, and the value to be assigned into temp registers.
                int list_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, sub_expr->object, list_reg);

                int index_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, sub_expr->index, index_reg);

                int value_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, expr->as.assign.value, value_reg);

                // Emit the instruction to perform the set.
                // Use SLOT_SET_SUBSCRIPT if has_slot_modifier is true to bypass reference dereferencing
                OpCode set_opcode = expr->as.assign.has_slot_modifier ? SLOT_SET_SUBSCRIPT : SET_SUBSCRIPT;
                emit_instruction(compiler, PACK_ABC(set_opcode, list_reg, index_reg, value_reg), expr->line);

                // The result of an assignment is the assigned value. Move it to the target register.
                EMIT_MOVE_IF_NEEDED(compiler, target_reg, value_reg, expr->line);

                // Free temporary registers.
                restore_temp_top_preserve(compiler, saved_top, target_reg);
                break;
            }

            // simple variable
            if (expr->as.assign.target->type == EXPR_VARIABLE) {
                Token name = expr->as.assign.target->as.variable.name;
                int reg = resolve_local(compiler, &name);

                if (reg != -1) {
                    // Check if this is a reference and/or slot parameter
                    bool is_ref = false;
                    bool is_slot = false;
                    for (int i = 0; i < compiler->local_count; i++) {
                        if (compiler->locals[i].reg == reg) {
                            if (compiler->locals[i].is_reference) {
                                is_ref = true;
                            }
                            if (compiler->locals[i].is_slot_param) {
                                is_slot = true;
                            }
                            break;
                        }
                    }

                    // Slot parameters: ALWAYS use SLOT_DEREF_SET to write to the aliased variable
                    // SLOT_DEREF_SET will recursively write through if the target contains a reference
                    // Regular refs with slot modifier: rebind instead of write through
                    // Regular refs without slot modifier: write through (DEREF_SET) following entire ref chain
                    if (is_slot && expr->as.assign.has_slot_modifier) {
                        // Slot parameter WITH 'slot' keyword: always rebind (never write through nested refs)
                        // This is the explicit rebinding case: slot r = value
                        int value_reg = alloc_temp(compiler);
                        COMPILE_REQUIRED(compiler, expr->as.assign.value, value_reg);
                        emit_instruction(compiler, PACK_ABC(SLOT_DEREF_SET, reg, value_reg, 0), expr->line);
                        EMIT_MOVE_IF_NEEDED(compiler, target_reg, value_reg, expr->line);
                    } else if (is_slot && !expr->as.assign.has_slot_modifier) {
                        // Slot parameter WITHOUT 'slot' keyword: write through (including nested refs)
                        // This is the implicit write-through case: r = value
                        int value_reg = alloc_temp(compiler);
                        COMPILE_REQUIRED(compiler, expr->as.assign.value, value_reg);
                        emit_instruction(compiler, PACK_ABC(DEREF_SET, reg, value_reg, 0), expr->line);
                        EMIT_MOVE_IF_NEEDED(compiler, target_reg, value_reg, expr->line);
                    } else if (is_ref && !expr->as.assign.has_slot_modifier) {
                        // Regular ref without slot modifier: write through the entire reference chain
                        int value_reg = alloc_temp(compiler);
                        COMPILE_REQUIRED(compiler, expr->as.assign.value, value_reg);
                        emit_instruction(compiler, PACK_ABC(DEREF_SET, reg, value_reg, 0), expr->line);
                        EMIT_MOVE_IF_NEEDED(compiler, target_reg, value_reg, expr->line);
                    } else {
                        // Normal variable or ref with slot modifier: compile value directly into its register
                        // This rebinds the variable to the new value (no dereferencing)
                        COMPILE_REQUIRED(compiler, expr->as.assign.value, reg);
                        EMIT_MOVE_IF_NEEDED(compiler, target_reg, reg, expr->line);
                    }
                } else if ((reg = resolve_upvalue(compiler, &name)) != -1) {
                    // Assign to an upvalue.
                    // Use SLOT_SET_UPVALUE if 'slot' modifier is present (explicit rebinding)
                    // Use SET_UPVALUE otherwise (write through, including nested refs)
                    int value_reg = alloc_temp(compiler);
                    COMPILE_REQUIRED(compiler, expr->as.assign.value, value_reg);
                    OpCode set_op = expr->as.assign.has_slot_modifier ? SLOT_SET_UPVALUE : SET_UPVALUE;
                    emit_instruction(compiler, PACK_ABx(set_op, value_reg, reg), expr->line);
                    EMIT_MOVE_IF_NEEDED(compiler, target_reg, value_reg, expr->line);
                } else {
                    // Assign to a global.
                    int value_reg = alloc_temp(compiler);
                    COMPILE_REQUIRED(compiler, expr->as.assign.value, value_reg);
                    int name_const = identifier_constant(compiler, &name);
                    // Use SLOT_SET_GLOBAL if has_slot_modifier is true to bypass reference dereferencing
                    OpCode set_opcode = expr->as.assign.has_slot_modifier ? SLOT_SET_GLOBAL : SET_GLOBAL;
                    emit_instruction(compiler, PACK_ABx(set_opcode, value_reg, name_const), expr->line);
                    EMIT_MOVE_IF_NEEDED(compiler, target_reg, value_reg, expr->line);
                }
            }
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_LITERAL: {
            int const_index = -1;
            switch (expr->as.literal.literal.type) {
                case TOKEN_TRUE:
                    const_index = make_constant(compiler, TRUE_VAL);
                    break;
                case TOKEN_FALSE:
                    const_index = make_constant(compiler, FALSE_VAL);
                    break;
                case TOKEN_NULL:
                    const_index = make_constant(compiler, NULL_VAL);
                    break;
                case TOKEN_NUMBER: {
                    double value = parse_number_literal(expr->as.literal.literal.start, expr->as.literal.literal.length);
                    const_index = make_constant(compiler, DOUBLE_VAL(value));
                    break;
                }
                case TOKEN_STRING: {
                    // Process escape sequences in string literals
                    const char* raw_str = expr->as.literal.literal.start + 1;  // Skip opening quote
                    int raw_len = expr->as.literal.literal.length - 2;         // Skip both quotes

                    int processed_len;
                    const char* error_msg = NULL;
                    int error_pos = 0;

                    char* processed = processEscapeSequences(raw_str, raw_len, &processed_len,
                                                            &error_msg, &error_pos);

                    if (!processed) {
                        // Escape sequence processing failed
                        compiler_error(compiler, expr->line, "Invalid escape sequence: %s", error_msg);
                        const_index = -1;
                        break;
                    }

                    ObjString* str = copyString(compiler->vm, processed, processed_len);
                    pushTempRoot(compiler->vm, (Obj*)str);
                    Value str_val = OBJ_VAL(str);
                    free(processed);
                    const_index = make_constant(compiler, str_val);
                    popTempRoot(compiler->vm);
                    break;
                }
                case TOKEN_IDENTIFIER: {
                    // Identifier used as a string literal (for map keys without quotes)
                    ObjString* str = copyString(compiler->vm, expr->as.literal.literal.start, expr->as.literal.literal.length);
                    pushTempRoot(compiler->vm, (Obj*)str);
                    Value str_val = OBJ_VAL(str);
                    const_index = make_constant(compiler, str_val);
                    popTempRoot(compiler->vm);
                    break;
                }
                default:
                    // Should be unreachable.
                    break;
            }

            if (const_index != -1) {
                emit_instruction(compiler, PACK_ABx(LOAD_CONST, target_reg, const_index), expr->line);
            }
            break;
        }
        case EXPR_BINARY: {
            int saved_top = save_temp_top(compiler);

            // Handle logical AND and OR with short-circuit evaluation
            if (expr->as.binary.operator.type == TOKEN_AND) {
                // Compile: left AND right
                // If left is false, skip right and result is false (left value)
                // If left is true, result is right value
                COMPILE_REQUIRED(compiler, expr->as.binary.left, target_reg);
                int skip_jump = emit_jump_instruction(compiler, JUMP_IF_FALSE, target_reg, expr->line);
                COMPILE_REQUIRED(compiler, expr->as.binary.right, target_reg);
                patch_jump(compiler, skip_jump);
                restore_temp_top_preserve(compiler, saved_top, target_reg);
                break;
            }

            if (expr->as.binary.operator.type == TOKEN_OR) {
                // Compile: left OR right
                // If left is true, skip right and result is true (left value)
                // If left is false, result is right value
                COMPILE_REQUIRED(compiler, expr->as.binary.left, target_reg);
                // We need to jump if true, but we only have JUMP_IF_FALSE
                // So: if left is false, continue to evaluate right; otherwise jump over right
                int eval_right_jump = emit_jump_instruction(compiler, JUMP_IF_FALSE, target_reg, expr->line);
                int skip_right_jump = emit_jump_instruction(compiler, JUMP, 0, expr->line);
                patch_jump(compiler, eval_right_jump);
                COMPILE_REQUIRED(compiler, expr->as.binary.right, target_reg);
                patch_jump(compiler, skip_right_jump);
                restore_temp_top_preserve(compiler, saved_top, target_reg);
                break;
            }

            // Check if right operand is a constant number literal
            bool right_is_const = (expr->as.binary.right->type == EXPR_LITERAL &&
                                   expr->as.binary.right->as.literal.literal.type == TOKEN_NUMBER);

            double const_value = 0;
            bool use_immediate = false;
            bool use_literal = false;

            if (right_is_const) {
                const_value = parse_number_literal(expr->as.binary.right->as.literal.literal.start,
                                                   expr->as.binary.right->as.literal.literal.length);

                // Prefer _L (3-register) over _I (in-place) when left operand is a variable
                // that would require a MOVE to get into target_reg
                bool prefer_literal = false;
                if (expr->as.binary.left->type == EXPR_VARIABLE) {
                    Token* name = &expr->as.binary.left->as.variable.name;
                    int src_reg = resolve_local(compiler, name);
                    // If it's a local in a different register, prefer _L to avoid MOVE
                    if (src_reg != -1 && src_reg != target_reg) {
                        prefer_literal = true;
                    }
                }

                // Decide between _I and _L
                if (prefer_literal) {
                    // Use _L for 3-register operation (no MOVE needed)
                    use_literal = true;
                } else if (const_value == floor(const_value)) {
                    // Check if it's an integer in 16-bit signed range [-32768, 32767]
                    int64_t int_val = (int64_t)const_value;
                    if (int_val >= -32768 && int_val <= 32767) {
                        use_immediate = true;
                    } else {
                        use_literal = true;
                    }
                } else {
                    // Non-integer, must use _L
                    use_literal = true;
                }
            }

            // Determine the opcode based on operator type
            OpCode op_base;
            bool is_math_or_bitwise = true;
            switch (expr->as.binary.operator.type) {
                case TOKEN_PLUS:          op_base = ADD; break;
                case TOKEN_MINUS:         op_base = SUB; break;
                case TOKEN_STAR:          op_base = MUL; break;
                case TOKEN_SLASH:         op_base = DIV; break;
                case TOKEN_PERCENT:       op_base = MOD; break;
                case TOKEN_BINARY_AND:    op_base = BAND; break;
                case TOKEN_BINARY_OR:     op_base = BOR; break;
                case TOKEN_BINARY_XOR:    op_base = BXOR; break;
                case TOKEN_LEFT_SHIFT:    op_base = BLSHIFT; break;
                case TOKEN_RIGHT_SHIFT:   op_base = BRSHIFT_I; break;
                case TOKEN_UNSIGNED_RIGHT_SHIFT: op_base = BRSHIFT_U; break;
                case TOKEN_PLUS_EQUAL:    op_base = ADD; break;
                case TOKEN_MINUS_EQUAL:   op_base = SUB; break;
                case TOKEN_STAR_EQUAL:    op_base = MUL; break;
                case TOKEN_SLASH_EQUAL:   op_base = DIV; break;
                case TOKEN_PERCENT_EQUAL: op_base = MOD; break;
                case TOKEN_BINARY_AND_EQUAL:  op_base = BAND; break;
                case TOKEN_BINARY_OR_EQUAL:   op_base = BOR; break;
                case TOKEN_BINARY_XOR_EQUAL:  op_base = BXOR; break;
                case TOKEN_LEFT_SHIFT_EQUAL:  op_base = BLSHIFT; break;
                case TOKEN_RIGHT_SHIFT_EQUAL: op_base = BRSHIFT_I; break;
                case TOKEN_UNSIGNED_RIGHT_SHIFT_EQUAL: op_base = BRSHIFT_U; break;
                // Comparison ops now have immediate variants too
                case TOKEN_LESS:          op_base = LT;  break;
                case TOKEN_GREATER:       op_base = GT;  break;
                case TOKEN_EQUAL_EQUAL:   op_base = EQ;  break;
                case TOKEN_BANG_EQUAL:    op_base = NE;  break;
                case TOKEN_LESS_EQUAL:    op_base = LE;  break;
                case TOKEN_GREATER_EQUAL: op_base = GE;  break;
                default: return; // Unreachable
            }

            // Emit optimized instruction if possible
            if (use_immediate) {
                // Emit _I variant: Ra = Ra op imm16 (in-place operation)
                // Format: ABx with A=target/source register, Bx=16-bit immediate
                // First compile left side into target register directly
                COMPILE_REQUIRED(compiler, expr->as.binary.left, target_reg);

                int64_t int_val = (int64_t)const_value;
                uint32_t imm_bits = (uint32_t)(int_val & 0xFFFF);

                // Map base opcode to _I variant
                OpCode op_imm;
                if (op_base >= ADD && op_base <= MOD) {
                    op_imm = op_base + (ADD_I - ADD);
                } else if (op_base >= BAND && op_base <= BRSHIFT_I) {
                    op_imm = op_base + (BAND_I - BAND);
                } else if (op_base >= EQ && op_base <= GE) {
                    op_imm = op_base + (EQ_I - EQ);
                } else {
                    op_imm = op_base; // Fallback
                }

                emit_instruction(compiler, PACK_ABx(op_imm, target_reg, imm_bits), expr->line);
            } else if (use_literal) {
                // Emit _L variant: Ra = Rb op lit64 (3-register with literal)
                // ABC format: A=dest, B=source, C=unused
                // Followed by 64-bit literal in next two words

                // Try to get source register directly if left is a simple variable
                int src_reg = -1;
                bool need_free_src = false;

                if (expr->as.binary.left->type == EXPR_VARIABLE) {
                    Token* name = &expr->as.binary.left->as.variable.name;
                    src_reg = resolve_local(compiler, name);
                }

                // If we couldn't get a direct register, compile into a temp
                // Use smart targeting to potentially avoid a MOVE
                if (src_reg == -1) {
                    src_reg = compile_sub_expression_to(compiler, expr->as.binary.left, target_reg);
                    need_free_src = !is_local_reg(compiler, src_reg);
                }

                // Map base opcode to _L variant
                OpCode op_lit;
                if (op_base >= ADD && op_base <= MOD) {
                    op_lit = op_base + (ADD_L - ADD);
                } else if (op_base >= BAND && op_base <= BRSHIFT_I) {
                    op_lit = op_base + (BAND_L - BAND);
                } else if (op_base >= EQ && op_base <= GE) {
                    op_lit = op_base + (EQ_L - EQ);
                } else {
                    op_lit = op_base; // Fallback
                }

                // Emit ABC instruction with source register
                emit_instruction(compiler, PACK_ABC(op_lit, target_reg, src_reg, 0), expr->line);
                write64BitLiteral(compiler->vm, compiler->compiling_chunk, const_value, expr->line);
            } else {
                // Standard 3-register operation
                // 1. Compile the left operand DIRECTLY into the target register.
                COMPILE_REQUIRED(compiler, expr->as.binary.left, target_reg);
                // 2. Compile right operand into a new temporary register as usual.
                int right_reg = compile_sub_expression(compiler, expr->as.binary.right);
                // 3. Emit instruction: target = target OP right
                emit_instruction(compiler, PACK_ABC(op_base, target_reg, target_reg, right_reg), expr->line);
            }
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_UNARY: {
            int saved_top = save_temp_top(compiler);

            if (expr->as.unary.operator.type == TOKEN_REF) {
                // Handle ref as unary expression
                // Validate that ref is followed by a valid target (not a literal or other invalid expression)
                if (expr->as.unary.right->type == EXPR_VARIABLE) {
                    emit_variable_reference(compiler, &expr->as.unary.right->as.variable.name, target_reg, expr->line);
                } else if (expr->as.unary.right->type == EXPR_SUBSCRIPT) {
                    emit_subscript_reference(compiler, &expr->as.unary.right->as.subscript, target_reg, expr->line);
                } else if (expr->as.unary.right->type == EXPR_GET) {
                    emit_property_reference(compiler, &expr->as.unary.right->as.get, target_reg, expr->line);
                } else {
                    // Provide specific error messages for common mistakes
                    const char* error_msg;
                    if (expr->as.unary.right->type == EXPR_LITERAL) {
                        error_msg = "'ref' cannot be applied to literal values (numbers, strings, booleans, null). Use 'ref' with variables, array elements, or map properties only.";
                    } else if (expr->as.unary.right->type == EXPR_CALL) {
                        error_msg = "'ref' cannot be applied directly to function call results. Assign the result to a variable first, then create a reference to that variable.";
                    } else if (expr->as.unary.right->type == EXPR_BINARY || expr->as.unary.right->type == EXPR_UNARY) {
                        error_msg = "'ref' cannot be applied to expressions. Assign the expression result to a variable first, then create a reference to that variable.";
                    } else {
                        error_msg = "'ref' can only be applied to variables, array elements, or map properties.";
                    }
                    compiler_error_and_exit(expr->line, "%s", error_msg);
                }
                restore_temp_top_preserve(compiler, saved_top, target_reg);
                break;
            } else if (expr->as.unary.operator.type == TOKEN_VAL) {
                // Handle val as unary expression - deep clone
                int right_reg = alloc_temp(compiler);
                compile_expression(compiler, expr->as.unary.right, right_reg);
                emit_instruction(compiler, PACK_ABC(CLONE_VALUE, target_reg, right_reg, 0), expr->line);
                restore_temp_top_preserve(compiler, saved_top, target_reg);
                break;
            }

            // Regular unary operators (-, !, ~)
            int right_reg = alloc_temp(compiler);
            compile_expression(compiler, expr->as.unary.right, right_reg);
            OpCode op;
            switch (expr->as.unary.operator.type) {
                case TOKEN_MINUS:
                    op = NEG;
                    break;
                case TOKEN_BANG:
                    op = NOT;
                    break;
                case TOKEN_BINARY_NOT:
                    op = BNOT;  // Simple bitwise NOT on i32
                    break;
                default: return; // Unreachable
            }
            emit_instruction(compiler, PACK_ABC(op, target_reg, right_reg, 0), expr->line);
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_GROUPING: {
            compile_expression(compiler, expr->as.grouping.expression, target_reg);
            break;
        }
        case EXPR_CALL: {
            int saved_top = save_temp_top(compiler);

            const int arg_count = expr->as.call.arg_count;
            Expr* callee = expr->as.call.callee;

            // Check if this is actually a struct instantiation: StructName(args...)
            if (callee->type == EXPR_VARIABLE) {
                Token* name = &callee->as.variable.name;

                // Look up if this name refers to a struct schema (check current and enclosing scopes)
                ObjStructSchema* schema = NULL;
                Compiler* search_compiler = compiler;
                while (search_compiler != NULL && schema == NULL) {
                    for (int i = search_compiler->struct_schema_count - 1; i >= 0; i--) {
                        if (tokens_equal(name, &search_compiler->struct_schemas[i].name)) {
                            schema = search_compiler->struct_schemas[i].schema;
                            break;
                        }
                    }
                    search_compiler = search_compiler->enclosing;
                }

                // If it's a struct schema, handle as positional struct instantiation
                if (schema) {
                    // Validate argument count matches field count
                    if (arg_count != schema->field_count) {
                        compiler_error(compiler, expr->line,
                                       "Positional initialization of struct '%.*s' requires exactly %d arguments, got %d",
                                       schema->name->length, schema->name->chars,
                                       schema->field_count, arg_count);
                        restore_temp_top_preserve(compiler, saved_top, target_reg);
                        break;
                    }

                    // Add schema to constants
                    int schema_const = make_constant(compiler, OBJ_VAL(schema));

                    // Emit NEW_STRUCT to create instance
                    emit_instruction(compiler, PACK_ABx(NEW_STRUCT, target_reg, schema_const), expr->line);

                    // Set fields in positional order
                    for (int i = 0; i < arg_count; i++) {
                        int value_reg = alloc_temp(compiler);
                        COMPILE_REQUIRED(compiler, expr->as.call.args[i], value_reg);
                        emit_instruction(compiler, PACK_ABC(SET_STRUCT_FIELD, target_reg, i, value_reg), expr->line);
                    }
                    restore_temp_top_preserve(compiler, saved_top, target_reg);
                    break;
                }
            }

            // Not a struct - proceed with regular function call
            const int call_slots_needed = 1 + arg_count;

            // Optimization: Try to use target_reg as call_base to avoid a MOVE after the call
            // The call needs contiguous registers: [callee, arg1, arg2, ...]
            // We can only do this if none of the call slots overlap with active local variables
            //
            // IMPORTANT: We must check this BEFORE compiling arguments, because argument
            // compilation may allocate temps and advance next_register, making the check invalid

            // Record the local variable boundary before compiling arguments
            int next_register_before_args = compiler->next_register;

            bool can_optimize = true;

            // Check 1: target_reg must be >= next_register_before_args (outside the local region)
            // We allow target_reg == next_register_before_args because it's at the boundary
            // We also allow target_reg == next_register_before_args - 1 if it's a temporary (not a local)
            if (target_reg < next_register_before_args) {
                if (target_reg != next_register_before_args - 1 || is_local_reg(compiler, target_reg)) {
                    can_optimize = false;
                    #ifdef DEBUG_CALL_OPT
                    printf("[CALL OPT] FAILED: target_reg=%d < next_register_before_args=%d (is_local=%d)\n",
                           target_reg, next_register_before_args, is_local_reg(compiler, target_reg));
                    #endif
                }
            }

            // Check 2: Verify no local variable uses any register in [target_reg, target_reg + call_slots_needed)
            if (can_optimize) {
                for (int i = 0; i < compiler->local_count; i++) {
                    int local_reg = compiler->locals[i].reg;
                    if (local_reg >= target_reg && local_reg < target_reg + call_slots_needed) {
                        can_optimize = false;
                        #ifdef DEBUG_CALL_OPT
                        printf("[CALL OPT] FAILED: local[%d] reg=%d conflicts with [%d,%d)\n",
                               i, local_reg, target_reg, target_reg + call_slots_needed);
                        #endif
                        break;
                    }
                }
            }

            #ifdef DEBUG_CALL_OPT
            if (can_optimize) {
                printf("[CALL OPT] SUCCESS: target_reg=%d, next_register_before_args=%d, slots=%d\n",
                       target_reg, next_register_before_args, call_slots_needed);
            }
            #endif

            int call_base;
            if (can_optimize) {
                // Safe to optimize - use target_reg directly as call_base
                call_base = target_reg;
                // Ensure we have enough registers allocated for the call
                if (call_base + call_slots_needed > compiler->next_register) {
                    compiler->next_register = call_base + call_slots_needed;
                    if (compiler->next_register > compiler->max_register_seen) {
                        compiler->max_register_seen = compiler->next_register;
                    }
                }
            } else {
                // Can't optimize - allocate fresh registers above the local region
                call_base = compiler->next_register;
                compiler->next_register += call_slots_needed;
                if (compiler->next_register > compiler->max_register_seen) {
                    compiler->max_register_seen = compiler->next_register;
                }
            }

            // Check if this is a recursive self-call BEFORE loading the callee
            bool is_self_call = false;
            if (callee->type == EXPR_VARIABLE && compiler->function && compiler->function->name) {
                Token* name = &callee->as.variable.name;

                // Compare the unmangled names - the function name is stored unmangled
                if (compiler->function->name->length == name->length &&
                    memcmp(compiler->function->name->chars, name->start, name->length) == 0) {
                    // Same base name - now check if arity matches
                    is_self_call = (compiler->function->arity == arg_count);
                }
            }

            // For self-calls, we don't need to load the callee - the VM will get it from the frame
            if (!is_self_call) {
                if (callee->type == EXPR_VARIABLE) {
                    resolve_and_load_function(compiler, &callee->as.variable.name, arg_count, call_base, callee->line);
                } else {
                    COMPILE_REQUIRED(compiler, callee, call_base);
                }
            }

            // Get param_qualifiers for this function (if available from hoisting)
            uint8_t* param_qualifiers = NULL;
            bool is_direct_hoisted_call = false;
            if (callee->type == EXPR_VARIABLE) {
                Token* name = &callee->as.variable.name;

                // Check local hoisted functions
                for (int j = 0; j < compiler->local_hoisted_count; j++) {
                    if (tokens_equal(&compiler->local_hoisted[j].name, name) &&
                        compiler->local_hoisted[j].arity == arg_count) {
                        param_qualifiers = compiler->local_hoisted[j].param_qualifiers;
                        is_direct_hoisted_call = true;
                        break;
                    }
                }

                // Check global hoisted functions if not found locally
                if (param_qualifiers == NULL) {
                    for (int j = 0; j < compiler->hoisted_count; j++) {
                        if (tokens_equal(&compiler->hoisted[j].name, name) &&
                            compiler->hoisted[j].arity == arg_count) {
                            param_qualifiers = compiler->hoisted[j].param_qualifiers;
                            is_direct_hoisted_call = true;
                            break;
                        }
                    }
                }

                // Check enclosing scopes
                if (param_qualifiers == NULL && compiler->enclosing) {
                    Compiler* enc = compiler->enclosing;
                    while (enc) {
                        for (int j = 0; j < enc->hoisted_count; j++) {
                            if (tokens_equal(&enc->hoisted[j].name, name) &&
                                enc->hoisted[j].arity == arg_count) {
                                param_qualifiers = enc->hoisted[j].param_qualifiers;
                                is_direct_hoisted_call = true;
                                break;
                            }
                        }
                        if (param_qualifiers) break;
                        enc = enc->enclosing;
                    }
                }

                // Check if it's a native function
                if (param_qualifiers == NULL) {
                    char* mangled = mangle_name(compiler, name, arg_count);
                    ObjString* mangled_str = copyString(compiler->vm, mangled, (int)strlen(mangled));
                    pushTempRoot(compiler->vm, (Obj*)mangled_str);
                    Value func_val;
                    if (tableGet(&compiler->vm->globals, mangled_str, &func_val) && IS_NATIVE_FUNCTION(func_val)) {
                        ObjNativeFunction* native = AS_NATIVE_FUNCTION(func_val);
                        param_qualifiers = native->param_qualifiers;
                        is_direct_hoisted_call = true;
                    }
                    popTempRoot(compiler->vm);
                    FREE_ARRAY(compiler->vm, char, mangled, strlen(mangled) + 1);
                }
            }

            // For dynamic calls (non-hoisted), we can't know param_qualifiers at compile time.
            // As a workaround: for variable arguments in dynamic calls, pass them as-is.
            // The VM will try to create references, but it has limitations.
            bool needs_runtime_handling = !is_direct_hoisted_call;

            // Compile arguments with ref/val handling
            for (int i = 0; i < arg_count; i++) {
                Expr* arg = expr->as.call.args[i];
                int arg_slot = call_base + 1 + i;
                ParamQualifier qualifier = param_qualifiers ? (ParamQualifier)param_qualifiers[i] : PARAM_NORMAL;

                if (needs_runtime_handling) {
                    compile_dynamic_call_argument(compiler, arg, arg_slot, arg->line);
                } else if (qualifier == PARAM_REF) {
                    compile_ref_param_argument(compiler, arg, arg_slot, arg->line);
                } else if (qualifier == PARAM_VAL) {
                    // For val parameters: evaluate and clone
                    COMPILE_REQUIRED(compiler, arg, arg_slot);
                    int temp_clone = alloc_temp(compiler);
                    emit_instruction(compiler, PACK_ABC(CLONE_VALUE, temp_clone, arg_slot, 0), arg->line);
                    emit_move(compiler, arg_slot, temp_clone, arg->line);
                } else if (qualifier == PARAM_CLONE) {
                    // For clone parameters: evaluate and deep clone
                    COMPILE_REQUIRED(compiler, arg, arg_slot);
                    int temp_clone = alloc_temp(compiler);
                    emit_instruction(compiler, PACK_ABC(DEEP_CLONE_VALUE, temp_clone, arg_slot, 0), arg->line);
                    emit_move(compiler, arg_slot, temp_clone, arg->line);
                } else if (qualifier == PARAM_SLOT) {
                    compile_slot_param_argument(compiler, arg, arg_slot, arg->line);
                } else {
                    // Normal parameter: compile as usual
                    // But check if the argument is a ref parameter variable
                    if (arg->type == EXPR_VARIABLE) {
                        Token* var_name = &arg->as.variable.name;
                        int var_reg = resolve_local(compiler, var_name);

                        if (var_reg != -1 && is_local_ref_param(compiler, var_reg)) {
                            // Ref parameter passed to normal parameter - pass the reference as-is
                            // (let the VM/callee decide what to do with it)
                            emit_move(compiler, arg_slot, var_reg, arg->line);
                        } else {
                            // Normal variable or upvalue/global
                            COMPILE_REQUIRED(compiler, arg, arg_slot);
                        }
                    } else {
                        // Complex expression
                        COMPILE_REQUIRED(compiler, arg, arg_slot);
                    }
                }
            }

            if (compiler->max_register_seen < call_base + arg_count) {
                compiler->max_register_seen = call_base + arg_count;
            }

            #ifdef DEBUG_CALL
            printf("[COMPILER CALL] Line %d: call_base=R%d, arg_count=%d, next_register=%d, is_self=%d\n",
                   expr->line, call_base, arg_count, compiler->next_register, is_self_call);
            if (callee->type == EXPR_VARIABLE) {
                printf("[COMPILER CALL]   Function: %.*s\n",
                       callee->as.variable.name.length, callee->as.variable.name.start);
            }
            uint32_t packed = PACK_ABx(is_self_call ? CALL_SELF : CALL, call_base, arg_count);
            printf("[COMPILER CALL]   Encoded instruction: 0x%08X (REG_A=%d, REG_Bx=%u)\n",
                   packed, (packed >> 8) & 0xFF, packed >> 16);
            #endif

            if (is_self_call) {
                emit_instruction(compiler, PACK_ABx(CALL_SELF, call_base, arg_count), expr->line);
            } else {
                emit_instruction(compiler, PACK_ABx(CALL, call_base, arg_count), expr->line);
            }

            // Only emit MOVE if the result isn't already in target_reg
            if (call_base != target_reg) {
                EMIT_MOVE_IF_NEEDED(compiler, target_reg, call_base, expr->line);
            }
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_LIST: {
            int saved_top = save_temp_top(compiler);

            ListExpr* list_expr = &expr->as.list;
            emit_instruction(compiler, PACK_ABx(NEW_LIST, target_reg, 0), expr->line);
            int temp_reg = alloc_temp(compiler);
            for (int i = 0; i < list_expr->count; i++) {
                Expr* elem = list_expr->elements[i];
                if (elem->type == EXPR_SPREAD) {
                    // Handle spread element
                    COMPILE_REQUIRED(compiler, elem->as.spread.expression, temp_reg);
                    emit_instruction(compiler, PACK_ABC(LIST_SPREAD, target_reg, temp_reg, 0), expr->line);
                } else {
                    COMPILE_REQUIRED(compiler, elem, temp_reg);
                    emit_instruction(compiler, PACK_ABC(LIST_APPEND, target_reg, temp_reg, 0), expr->line);
                }
            }
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_SUBSCRIPT: {
            int saved_top = save_temp_top(compiler);

            SubscriptExpr* sub_expr = &expr->as.subscript;
            int list_reg = alloc_temp(compiler);
            COMPILE_REQUIRED(compiler, sub_expr->object, list_reg);
            int index_reg = alloc_temp(compiler);
            COMPILE_REQUIRED(compiler, sub_expr->index, index_reg);
            emit_instruction(compiler, PACK_ABC(GET_SUBSCRIPT, target_reg, list_reg, index_reg), expr->line);
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_MAP: {
            int saved_top = save_temp_top(compiler);

            MapExpr* map_expr = &expr->as.map;
            emit_instruction(compiler, PACK_ABx(NEW_MAP, target_reg, 0), expr->line);
            int key_reg = alloc_temp(compiler);
            int value_reg = alloc_temp(compiler);
            for (int i = 0; i < map_expr->count; i++) {
                Expr* key = map_expr->keys[i];
                if (key->type == EXPR_SPREAD) {
                    // Handle spread element (value will be NULL in parser)
                    COMPILE_REQUIRED(compiler, key->as.spread.expression, key_reg);
                    emit_instruction(compiler, PACK_ABC(MAP_SPREAD, target_reg, key_reg, 0), expr->line);
                } else {
                    COMPILE_REQUIRED(compiler, key, key_reg);
                    COMPILE_REQUIRED(compiler, map_expr->values[i], value_reg);
                    emit_instruction(compiler, PACK_ABC(MAP_SET, target_reg, key_reg, value_reg), expr->line);
                }
            }
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_STRUCT_INST: {
            int saved_top = save_temp_top(compiler);

            StructInstExpr* struct_expr = &expr->as.struct_inst;

            // Lookup struct schema (check current and enclosing scopes)
            ObjStructSchema* schema = NULL;
            Compiler* search_compiler = compiler;
            while (search_compiler != NULL && schema == NULL) {
                for (int i = search_compiler->struct_schema_count - 1; i >= 0; i--) {
                    if (tokens_equal(&struct_expr->struct_name, &search_compiler->struct_schemas[i].name)) {
                        schema = search_compiler->struct_schemas[i].schema;
                        break;
                    }
                }
                search_compiler = search_compiler->enclosing;
            }

            if (!schema) {
                compiler_error(compiler, expr->line, "Undefined struct '%.*s'", struct_expr->struct_name.length, struct_expr->struct_name.start);
                restore_temp_top_preserve(compiler, saved_top, target_reg);
                break;
            }

            // Add schema to constants
            int schema_const = make_constant(compiler, OBJ_VAL(schema));

            // Emit NEW_STRUCT to create instance
            emit_instruction(compiler, PACK_ABx(NEW_STRUCT, target_reg, schema_const), expr->line);

            // Check if this is positional or named initialization
            if (struct_expr->field_names == NULL) {
                // Positional initialization: StructName(value1, value2, ...)
                // All fields must be provided
                if (struct_expr->field_count != schema->field_count) {
                    compiler_error(compiler, expr->line,
                                   "Positional initialization of struct '%.*s' requires exactly %d arguments, got %d",
                                   schema->name->length, schema->name->chars,
                                   schema->field_count, struct_expr->field_count);
                    restore_temp_top_preserve(compiler, saved_top, target_reg);
                    break;
                }

                // Set fields in order
                for (int i = 0; i < struct_expr->field_count; i++) {
                    int value_reg = alloc_temp(compiler);
                    compile_expression(compiler, struct_expr->field_values[i], value_reg);
                    emit_instruction(compiler, PACK_ABC(SET_STRUCT_FIELD, target_reg, i, value_reg), expr->line);
                }
            } else {
                // Named initialization: StructName{field1: value1, field2: value2, ...}
                // Track which fields have been initialized to detect duplicates
                bool* field_initialized = (bool*)calloc(schema->field_count, sizeof(bool));

                // Set field values
                for (int i = 0; i < struct_expr->field_count; i++) {
                    // Check if this is a spread element
                    if (struct_expr->field_names[i].type == TOKEN_DOT_DOT_DOT) {
                        // Handle spread element - unwrap the EXPR_SPREAD node
                        int value_reg = alloc_temp(compiler);
                        Expr* spread_value = struct_expr->field_values[i];
                        if (spread_value->type == EXPR_SPREAD) {
                            compile_expression(compiler, spread_value->as.spread.expression, value_reg);
                        } else {
                            compile_expression(compiler, spread_value, value_reg);
                        }
                        emit_instruction(compiler, PACK_ABC(STRUCT_SPREAD, target_reg, value_reg, 0), expr->line);
                        continue;
                    }

                    // Find field index
                    int field_index = -1;
                    for (int j = 0; j < schema->field_count; j++) {
                        if (struct_expr->field_names[i].length == schema->field_names[j]->length &&
                            memcmp(struct_expr->field_names[i].start, schema->field_names[j]->chars, struct_expr->field_names[i].length) == 0) {
                            field_index = j;
                            break;
                        }
                    }

                    if (field_index == -1) {
                        compiler_error(compiler, expr->line, "Unknown field '%.*s' in struct '%.*s'",
                                       struct_expr->field_names[i].length, struct_expr->field_names[i].start,
                                       schema->name->length, schema->name->chars);
                        continue;
                    }

                    // Check for duplicate field initialization
                    if (field_initialized[field_index]) {
                        compiler_error(compiler, expr->line, "Duplicate field '%.*s' in struct initialization",
                                       struct_expr->field_names[i].length, struct_expr->field_names[i].start);
                        continue;
                    }
                    field_initialized[field_index] = true;

                    // Compile field value
                    int value_reg = alloc_temp(compiler);
                    compile_expression(compiler, struct_expr->field_values[i], value_reg);

                    // Emit SET_STRUCT_FIELD
                    emit_instruction(compiler, PACK_ABC(SET_STRUCT_FIELD, target_reg, field_index, value_reg), expr->line);
                }

                free(field_initialized);
            }
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_TERNARY: {
            int saved_top = save_temp_top(compiler);

            // Compile: condition ? then_expr : else_expr
            TernaryExpr* ternary = &expr->as.ternary;

            // Compile condition
            int cond_reg = alloc_temp(compiler);
            COMPILE_REQUIRED(compiler, ternary->condition, cond_reg);

            // Emit conditional jump: if condition is false, jump to else branch
            int jump_to_else = emit_jump_instruction(compiler, JUMP_IF_FALSE, cond_reg, expr->line);

            // Compile then branch
            COMPILE_REQUIRED(compiler, ternary->then_expr, target_reg);

            // Jump over else branch
            int jump_to_end = emit_jump_instruction(compiler, JUMP, 0, expr->line);

            // Patch jump to else
            patch_jump(compiler, jump_to_else);

            // Compile else branch
            COMPILE_REQUIRED(compiler, ternary->else_expr, target_reg);

            // Patch jump to end
            patch_jump(compiler, jump_to_end);

            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_GET: {
            int saved_top = save_temp_top(compiler);

            // Handle dot notation: obj.field
            GetExpr* get_expr = &expr->as.get;

            // First, check if this is actually an enum value access (EnumName.VARIANT)
            if (get_expr->object->type == EXPR_VARIABLE) {
                Token* enum_name = &get_expr->object->as.variable.name;
                ObjEnumSchema* enum_schema = get_enum_schema(compiler, enum_name);

                if (enum_schema) {
                    // This is an enum value access, not a property access
                    Token* variant_name = &get_expr->name;

                    // Find the variant index
                    int variant_index = -1;
                    for (int i = 0; i < enum_schema->variant_count; i++) {
                        if (tokens_equal(variant_name, &(Token){
                            .start = enum_schema->variant_names[i]->chars,
                            .length = enum_schema->variant_names[i]->length
                        })) {
                            variant_index = i;
                            break;
                        }
                    }

                    if (variant_index == -1) {
                        compiler_error(compiler, expr->line, "Undefined variant '%.*s' in enum '%.*s'",
                                     variant_name->length, variant_name->start,
                                     enum_name->length, enum_name->start);
                        restore_temp_top_preserve(compiler, saved_top, target_reg);
                        break;
                    }

                    // Create the enum value and load it as a constant
                    Value enum_val = ENUM_VAL(enum_schema->type_id, variant_index);
                    int const_idx = make_constant(compiler, enum_val);
                    emit_instruction(compiler, PACK_ABx(LOAD_CONST, target_reg, const_idx), expr->line);
                    restore_temp_top_preserve(compiler, saved_top, target_reg);
                    break;
                }
            }

            // Try to resolve compile-time struct type
            ObjStructSchema* schema = NULL;
            int obj_reg = -1;

            // Check if object is a variable with known struct type
            if (get_expr->object->type == EXPR_VARIABLE) {
                Token* var_name = &get_expr->object->as.variable.name;

                // Try local first
                obj_reg = resolve_local(compiler, var_name);
                if (obj_reg != -1) {
                    Local* local = get_local_by_reg(compiler, obj_reg);
                    if (local && local->struct_type) {
                        schema = local->struct_type;
                    }
                } else {
                    // Try upvalue
                    int upvalue_idx = resolve_upvalue(compiler, var_name);
                    if (upvalue_idx != -1) {
                        schema = compiler->upvalues[upvalue_idx].struct_type;
                    } else {
                        // Try global
                        schema = get_global_type(compiler, var_name);
                    }
                }
            }

            // If we know the struct type at compile time, emit direct field access
            if (schema) {
                // Look up field index
                Value index_val;
                ObjString* field_name = copyString(compiler->vm, get_expr->name.start, get_expr->name.length);
                pushTempRoot(compiler->vm, (Obj*)field_name);
                if (tableGet(schema->field_to_index, field_name, &index_val)) {
                    int field_index = (int)AS_DOUBLE(index_val);

                    // If we already have obj_reg from local lookup, use it
                    if (obj_reg == -1) {
                        obj_reg = alloc_temp(compiler);
                        COMPILE_REQUIRED(compiler, get_expr->object, obj_reg);
                    }

                    // Emit direct struct field access
                    emit_instruction(compiler, PACK_ABC(GET_STRUCT_FIELD, target_reg, obj_reg, field_index), expr->line);
                    restore_temp_top_preserve(compiler, saved_top, target_reg);
                    break;
                }
                popTempRoot(compiler->vm);
            }

            // Fallback: dynamic property access (for maps or unknown types)
            if (obj_reg == -1) {
                obj_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, get_expr->object, obj_reg);
            }

            // Convert the identifier to a string constant
            int key_const = identifier_constant(compiler, &get_expr->name);
            int key_reg = alloc_temp(compiler);
            emit_instruction(compiler, PACK_ABx(LOAD_CONST, key_reg, key_const), expr->line);

            emit_instruction(compiler, PACK_ABC(GET_MAP_PROPERTY, target_reg, obj_reg, key_reg), expr->line);
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_SET: {
            int saved_top = save_temp_top(compiler);

            // Handle dot notation assignment: obj.field = value
            SetExpr* set_expr = &expr->as.set;

            // Try to resolve compile-time struct type
            ObjStructSchema* schema = NULL;
            int obj_reg = -1;

            // Check if object is a variable with known struct type
            if (set_expr->object->type == EXPR_VARIABLE) {
                Token* var_name = &set_expr->object->as.variable.name;

                // Try local first
                obj_reg = resolve_local(compiler, var_name);
                if (obj_reg != -1) {
                    Local* local = get_local_by_reg(compiler, obj_reg);
                    if (local && local->struct_type) {
                        schema = local->struct_type;
                    }
                } else {
                    // Try upvalue
                    int upvalue_idx = resolve_upvalue(compiler, var_name);
                    if (upvalue_idx != -1) {
                        schema = compiler->upvalues[upvalue_idx].struct_type;
                    } else {
                        // Try global
                        schema = get_global_type(compiler, var_name);
                    }
                }
            }

            // If we know the struct type at compile time, emit direct field access
            if (schema) {
                // Look up field index
                Value index_val;
                ObjString* field_name = copyString(compiler->vm, set_expr->name.start, set_expr->name.length);
                pushTempRoot(compiler->vm, (Obj*)field_name);
                if (tableGet(schema->field_to_index, field_name, &index_val)) {
                    int field_index = (int)AS_DOUBLE(index_val);

                    // If we already have obj_reg from local lookup, use it
                    if (obj_reg == -1) {
                        obj_reg = alloc_temp(compiler);
                        COMPILE_REQUIRED(compiler, set_expr->object, obj_reg);
                    }

                    int value_reg = alloc_temp(compiler);
                    COMPILE_REQUIRED(compiler, set_expr->value, value_reg);

                    // Emit direct struct field set
                    OpCode set_opcode = set_expr->has_slot_modifier ? SLOT_SET_STRUCT_FIELD : SET_STRUCT_FIELD;
                    emit_instruction(compiler, PACK_ABC(set_opcode, obj_reg, field_index, value_reg), expr->line);

                    // The result of an assignment is the assigned value
                    EMIT_MOVE_IF_NEEDED(compiler, target_reg, value_reg, expr->line);

                    restore_temp_top_preserve(compiler, saved_top, target_reg);
                    break;
                }
                popTempRoot(compiler->vm);
            }

            // Fallback: dynamic property access (for maps or unknown types)
            if (obj_reg == -1) {
                obj_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, set_expr->object, obj_reg);
            }

            // Convert the identifier to a string constant
            int key_const = identifier_constant(compiler, &set_expr->name);
            int key_reg = alloc_temp(compiler);
            emit_instruction(compiler, PACK_ABx(LOAD_CONST, key_reg, key_const), expr->line);

            int value_reg = alloc_temp(compiler);
            COMPILE_REQUIRED(compiler, set_expr->value, value_reg);

            // Use SLOT_SET_MAP_PROPERTY if has_slot_modifier is true to bypass reference dereferencing
            OpCode set_opcode = set_expr->has_slot_modifier ? SLOT_SET_MAP_PROPERTY : SET_MAP_PROPERTY;
            emit_instruction(compiler, PACK_ABC(set_opcode, obj_reg, key_reg, value_reg), expr->line);

            // The result of an assignment is the assigned value
            EMIT_MOVE_IF_NEEDED(compiler, target_reg, value_reg, expr->line);

            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_FUNCTION: {
            int saved_top = save_temp_top(compiler);

            // Compile anonymous function expression
            FunctionExpr* func_expr = &expr->as.function;

            // Create a temporary FuncDeclStmt to reuse compile_function_body
            Token anon_name = {
                .start = "<anon>",
                .length = 6,
                .line = expr->line,
                .type = TOKEN_IDENTIFIER
            };

            FuncDeclStmt temp_stmt;
            temp_stmt.name = anon_name;
            temp_stmt.params = func_expr->params;
            temp_stmt.param_count = func_expr->param_count;
            temp_stmt.param_capacity = func_expr->param_capacity;
            temp_stmt.body = func_expr->body;
            temp_stmt.return_type = func_expr->return_type;
            temp_stmt.function = NULL;  // Initialize function field to NULL

            // Compile the function body
            ObjFunction* function = compile_function_body(compiler, &temp_stmt);
            int const_index = make_constant(compiler, OBJ_VAL(function));
            popTempRoot(compiler->vm);  // Pop protection from compile_function_body

            // Emit CLOSURE instruction to create a closure in target_reg
            emit_closure(compiler, target_reg, const_index, expr->line);
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_PRE_INC: {
            int saved_top = save_temp_top(compiler);

            Expr* target_expr = expr->as.pre_inc.target;

            if (target_expr->type == EXPR_VARIABLE) {
                Token* name = &target_expr->as.variable.name;
                int reg = resolve_local(compiler, name);

                if (reg != -1) {
                    // Local variable: PRE_INC directly on register
                    emit_instruction(compiler, PACK_ABC(PRE_INC, target_reg, reg, 0), expr->line);
                } else if ((reg = resolve_upvalue(compiler, name)) != -1) {
                    // Upvalue: load, increment, store back
                    int temp = alloc_temp(compiler);
                    emit_get_upvalue(compiler, temp, reg, expr->line);
                    emit_instruction(compiler, PACK_ABC(PRE_INC, target_reg, temp, 0), expr->line);
                    emit_instruction(compiler, PACK_ABx(SET_UPVALUE, target_reg, reg), expr->line);
                } else {
                    // Global: load, increment, store back
                    int name_const = identifier_constant(compiler, name);
                    int temp = alloc_temp(compiler);
                    emit_get_global(compiler, temp, name_const, expr->line);
                    emit_instruction(compiler, PACK_ABC(PRE_INC, target_reg, temp, 0), expr->line);
                    emit_instruction(compiler, PACK_ABx(SET_GLOBAL, target_reg, name_const), expr->line);
                }
            } else if (target_expr->type == EXPR_SUBSCRIPT) {
                // Subscript: arr[i]++
                SubscriptExpr* sub = &target_expr->as.subscript;
                int obj_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, sub->object, obj_reg);
                int idx_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, sub->index, idx_reg);

                // Get current value
                int val_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABC(GET_SUBSCRIPT, val_reg, obj_reg, idx_reg), expr->line);

                // Increment in place
                emit_instruction(compiler, PACK_ABC(PRE_INC, target_reg, val_reg, 0), expr->line);

                // Set back
                emit_instruction(compiler, PACK_ABC(SET_SUBSCRIPT, obj_reg, idx_reg, target_reg), expr->line);
            } else if (target_expr->type == EXPR_GET) {
                // Property: obj.prop++
                GetExpr* get = &target_expr->as.get;
                int obj_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, get->object, obj_reg);
                int key_const = identifier_constant(compiler, &get->name);
                int key_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABx(LOAD_CONST, key_reg, key_const), expr->line);

                // Get current value
                int val_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABC(GET_MAP_PROPERTY, val_reg, obj_reg, key_reg), expr->line);

                // Increment in place
                emit_instruction(compiler, PACK_ABC(PRE_INC, target_reg, val_reg, 0), expr->line);

                // Set back
                emit_instruction(compiler, PACK_ABC(SET_MAP_PROPERTY, obj_reg, key_reg, target_reg), expr->line);
            } else {
                compiler_error_and_exit(expr->line, "Pre-increment operator can only be applied to variables, subscripts, or properties.");
            }
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_POST_INC: {
            int saved_top = save_temp_top(compiler);

            Expr* target_expr = expr->as.post_inc.target;

            if (target_expr->type == EXPR_VARIABLE) {
                Token* name = &target_expr->as.variable.name;
                int reg = resolve_local(compiler, name);

                if (reg != -1) {
                    // Local variable: POST_INC directly on register
                    emit_instruction(compiler, PACK_ABC(POST_INC, target_reg, reg, 0), expr->line);
                } else if ((reg = resolve_upvalue(compiler, name)) != -1) {
                    // Upvalue: load, increment, store back
                    int temp = alloc_temp(compiler);
                    emit_get_upvalue(compiler, temp, reg, expr->line);
                    emit_instruction(compiler, PACK_ABC(POST_INC, target_reg, temp, 0), expr->line);
                    // POST_INC returns old value in target_reg, temp now has new value
                    emit_instruction(compiler, PACK_ABx(SET_UPVALUE, temp, reg), expr->line);
                } else {
                    // Global: load, increment, store back
                    int name_const = identifier_constant(compiler, name);
                    int temp = alloc_temp(compiler);
                    emit_get_global(compiler, temp, name_const, expr->line);
                    emit_instruction(compiler, PACK_ABC(POST_INC, target_reg, temp, 0), expr->line);
                    // POST_INC returns old value, temp now has incremented value
                    emit_instruction(compiler, PACK_ABx(SET_GLOBAL, temp, name_const), expr->line);
                }
            } else if (target_expr->type == EXPR_SUBSCRIPT) {
                // Subscript: arr[i]++
                SubscriptExpr* sub = &target_expr->as.subscript;
                int obj_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, sub->object, obj_reg);
                int idx_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, sub->index, idx_reg);

                // Get current value
                int val_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABC(GET_SUBSCRIPT, val_reg, obj_reg, idx_reg), expr->line);

                // Post-increment: returns old value, increments val_reg
                emit_instruction(compiler, PACK_ABC(POST_INC, target_reg, val_reg, 0), expr->line);

                // Set incremented value back
                emit_instruction(compiler, PACK_ABC(SET_SUBSCRIPT, obj_reg, idx_reg, val_reg), expr->line);
            } else if (target_expr->type == EXPR_GET) {
                // Property: obj.prop++
                GetExpr* get = &target_expr->as.get;
                int obj_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, get->object, obj_reg);
                int key_const = identifier_constant(compiler, &get->name);
                int key_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABx(LOAD_CONST, key_reg, key_const), expr->line);

                // Get current value
                int val_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABC(GET_MAP_PROPERTY, val_reg, obj_reg, key_reg), expr->line);

                // Post-increment: returns old value, increments val_reg
                emit_instruction(compiler, PACK_ABC(POST_INC, target_reg, val_reg, 0), expr->line);

                // Set incremented value back
                emit_instruction(compiler, PACK_ABC(SET_MAP_PROPERTY, obj_reg, key_reg, val_reg), expr->line);
            } else {
                compiler_error_and_exit(expr->line, "Post-increment operator can only be applied to variables, subscripts, or properties.");
            }
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_PRE_DEC: {
            int saved_top = save_temp_top(compiler);

            Expr* target_expr = expr->as.pre_dec.target;

            if (target_expr->type == EXPR_VARIABLE) {
                Token* name = &target_expr->as.variable.name;
                int reg = resolve_local(compiler, name);

                if (reg != -1) {
                    // Local variable: PRE_DEC directly on register
                    emit_instruction(compiler, PACK_ABC(PRE_DEC, target_reg, reg, 0), expr->line);
                } else if ((reg = resolve_upvalue(compiler, name)) != -1) {
                    // Upvalue: load, decrement, store back
                    int temp = alloc_temp(compiler);
                    emit_get_upvalue(compiler, temp, reg, expr->line);
                    emit_instruction(compiler, PACK_ABC(PRE_DEC, target_reg, temp, 0), expr->line);
                    emit_instruction(compiler, PACK_ABx(SET_UPVALUE, target_reg, reg), expr->line);
                } else {
                    // Global: load, decrement, store back
                    int name_const = identifier_constant(compiler, name);
                    int temp = alloc_temp(compiler);
                    emit_get_global(compiler, temp, name_const, expr->line);
                    emit_instruction(compiler, PACK_ABC(PRE_DEC, target_reg, temp, 0), expr->line);
                    emit_instruction(compiler, PACK_ABx(SET_GLOBAL, target_reg, name_const), expr->line);
                }
            } else if (target_expr->type == EXPR_SUBSCRIPT) {
                // Subscript: arr[i]--
                SubscriptExpr* sub = &target_expr->as.subscript;
                int obj_reg = alloc_temp(compiler);
                compile_expression(compiler, sub->object, obj_reg);
                int idx_reg = alloc_temp(compiler);
                compile_expression(compiler, sub->index, idx_reg);

                // Get current value
                int val_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABC(GET_SUBSCRIPT, val_reg, obj_reg, idx_reg), expr->line);

                // Decrement in place
                emit_instruction(compiler, PACK_ABC(PRE_DEC, target_reg, val_reg, 0), expr->line);

                // Set back
                emit_instruction(compiler, PACK_ABC(SET_SUBSCRIPT, obj_reg, idx_reg, target_reg), expr->line);
            } else if (target_expr->type == EXPR_GET) {
                // Property: obj.prop--
                GetExpr* get = &target_expr->as.get;
                int obj_reg = alloc_temp(compiler);
                compile_expression(compiler, get->object, obj_reg);
                int key_const = identifier_constant(compiler, &get->name);
                int key_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABx(LOAD_CONST, key_reg, key_const), expr->line);

                // Get current value
                int val_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABC(GET_MAP_PROPERTY, val_reg, obj_reg, key_reg), expr->line);

                // Decrement in place
                emit_instruction(compiler, PACK_ABC(PRE_DEC, target_reg, val_reg, 0), expr->line);

                // Set back
                emit_instruction(compiler, PACK_ABC(SET_MAP_PROPERTY, obj_reg, key_reg, target_reg), expr->line);
            } else {
                compiler_error_and_exit(expr->line, "Pre-decrement operator can only be applied to variables, subscripts, or properties.");
            }
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_POST_DEC: {
            int saved_top = save_temp_top(compiler);

            Expr* target_expr = expr->as.post_dec.target;

            if (target_expr->type == EXPR_VARIABLE) {
                Token* name = &target_expr->as.variable.name;
                int reg = resolve_local(compiler, name);

                if (reg != -1) {
                    // Local variable: POST_DEC directly on register
                    emit_instruction(compiler, PACK_ABC(POST_DEC, target_reg, reg, 0), expr->line);
                } else if ((reg = resolve_upvalue(compiler, name)) != -1) {
                    // Upvalue: load, decrement, store back
                    int temp = alloc_temp(compiler);
                    emit_get_upvalue(compiler, temp, reg, expr->line);
                    emit_instruction(compiler, PACK_ABC(POST_DEC, target_reg, temp, 0), expr->line);
                    // POST_DEC returns old value, temp now has decremented value
                    emit_instruction(compiler, PACK_ABx(SET_UPVALUE, temp, reg), expr->line);
                } else {
                    // Global: load, decrement, store back
                    int name_const = identifier_constant(compiler, name);
                    int temp = alloc_temp(compiler);
                    emit_get_global(compiler, temp, name_const, expr->line);
                    emit_instruction(compiler, PACK_ABC(POST_DEC, target_reg, temp, 0), expr->line);
                    // POST_DEC returns old value, temp now has decremented value
                    emit_instruction(compiler, PACK_ABx(SET_GLOBAL, temp, name_const), expr->line);
                }
            } else if (target_expr->type == EXPR_SUBSCRIPT) {
                // Subscript: arr[i]--
                SubscriptExpr* sub = &target_expr->as.subscript;
                int obj_reg = alloc_temp(compiler);
                compile_expression(compiler, sub->object, obj_reg);
                int idx_reg = alloc_temp(compiler);
                compile_expression(compiler, sub->index, idx_reg);

                // Get current value
                int val_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABC(GET_SUBSCRIPT, val_reg, obj_reg, idx_reg), expr->line);

                // Post-decrement: returns old value, decrements val_reg
                emit_instruction(compiler, PACK_ABC(POST_DEC, target_reg, val_reg, 0), expr->line);

                // Set decremented value back
                emit_instruction(compiler, PACK_ABC(SET_SUBSCRIPT, obj_reg, idx_reg, val_reg), expr->line);
            } else if (target_expr->type == EXPR_GET) {
                // Property: obj.prop--
                GetExpr* get = &target_expr->as.get;
                int obj_reg = alloc_temp(compiler);
                compile_expression(compiler, get->object, obj_reg);
                int key_const = identifier_constant(compiler, &get->name);
                int key_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABx(LOAD_CONST, key_reg, key_const), expr->line);

                // Get current value
                int val_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABC(GET_MAP_PROPERTY, val_reg, obj_reg, key_reg), expr->line);

                // Post-decrement: returns old value, decrements val_reg
                emit_instruction(compiler, PACK_ABC(POST_DEC, target_reg, val_reg, 0), expr->line);

                // Set decremented value back
                emit_instruction(compiler, PACK_ABC(SET_MAP_PROPERTY, obj_reg, key_reg, val_reg), expr->line);
            } else {
                compiler_error_and_exit(expr->line, "Post-decrement operator can only be applied to variables, subscripts, or properties.");
            }
            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_TYPEOF: {
            int saved_top = save_temp_top(compiler);

            // Compile the operand into a temporary register
            int operand_reg = alloc_temp(compiler);
            compile_expression(compiler, expr->as.typeof_expr.operand, operand_reg);

            // Emit TYPEOF instruction which will evaluate the type at runtime
            emit_instruction(compiler, PACK_ABC(TYPEOF, target_reg, operand_reg, 0), expr->line);

            restore_temp_top_preserve(compiler, saved_top, target_reg);
            break;
        }
        case EXPR_SPREAD: {
            // EXPR_SPREAD should not appear in isolation - it should only appear within
            // list/map/struct literals. If we reach here, it's a syntax error.
            compiler_error(compiler, expr->line, "Spread operator can only be used within list, map, or struct literals.");
            break;
        }
        default: break;
    }
}

// --- Label and Goto Helper Functions ---

// Find a label by name in the current function
static Label* find_label(Compiler* compiler, Token* name) {
    for (int i = 0; i < compiler->label_count; i++) {
        if (tokens_equal(name, &compiler->labels[i].name)) {
            return &compiler->labels[i];
        }
    }
    return NULL;
}

// Add a pending goto to the list
static void add_pending_goto(Compiler* compiler, int jump_address, Token target_label, int scope_depth, int local_count, int bytecode_pos) {
    if (compiler->pending_goto_count >= compiler->pending_goto_capacity) {
        int old_cap = compiler->pending_goto_capacity;
        compiler->pending_goto_capacity = old_cap < 8 ? 8 : old_cap * 2;
        compiler->pending_gotos = (PendingGoto*)reallocate(
            compiler->vm,
            compiler->pending_gotos,
            sizeof(PendingGoto) * old_cap,
            sizeof(PendingGoto) * compiler->pending_goto_capacity
        );
    }
    compiler->pending_gotos[compiler->pending_goto_count++] = (PendingGoto){
        .jump_address = jump_address,
        .target_label = target_label,
        .goto_scope_depth = scope_depth,
        .goto_local_count = local_count,
        .goto_bytecode_pos = bytecode_pos,
        .is_resolved = false
    };
}

// Validate goto safety
typedef enum {
    GOTO_SAFE,
    GOTO_ERROR_INTO_SCOPE,
    GOTO_ERROR_SKIP_INIT
} GotoSafetyResult;

static GotoSafetyResult validate_goto_safety(Compiler* compiler, int goto_scope, int goto_locals, int goto_bytecode_pos, int label_scope, int label_locals, int label_bytecode_pos) {
    // Case 1: Jump into deeper scope - ILLEGAL
    if (label_scope > goto_scope) {
        return GOTO_ERROR_INTO_SCOPE;
    }

    // Case 2: Forward jump in same scope - check for skipped declarations
    if (goto_scope == label_scope && goto_bytecode_pos < label_bytecode_pos) {
        // Check for skipped local variable declarations (all declarations auto-initialize)
        for (int i = goto_locals; i < label_locals; i++) {
            if (compiler->locals[i].depth == goto_scope) {
                return GOTO_ERROR_SKIP_INIT;
            }
        }

        // For global scope (depth 0), check if we're skipping any global declarations
        if (goto_scope == 0) {
            for (int i = 0; i < compiler->global_decl_count; i++) {
                int decl_pos = compiler->global_decls[i].bytecode_pos;
                // If the declaration bytecode is between goto and label, we're skipping it
                if (decl_pos > goto_bytecode_pos && decl_pos < label_bytecode_pos) {
                    return GOTO_ERROR_SKIP_INIT;
                }
            }
        }
    }

    // Case 3: Jump to outer scope or backward jump in same scope - SAFE
    return GOTO_SAFE;
}

// Emit cleanup trampolines for jumping from inner to outer scope
static void emit_goto_cleanup(Compiler* compiler, int from_scope, int to_scope, int line) {
    // Close upvalues for locals between the two scope levels
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        // Close upvalues for locals that are in scopes deeper than target but not deeper than current
        if (local->depth > to_scope && local->depth <= from_scope) {
            emit_instruction(compiler, PACK_ABx(CLOSE_UPVALUE, local->reg, 0), line);
        }
    }
}

// Record a global variable declaration for goto validation
static void record_global_decl(Compiler* compiler, Token name, int bytecode_pos) {
    if (compiler->global_decl_count >= MAX_GLOBAL_DECLS) {
        return; // Silently ignore if too many globals (unlikely)
    }
    compiler->global_decls[compiler->global_decl_count++] = (GlobalDecl){
        .bytecode_pos = bytecode_pos,
        .name = name
    };
}

// --- Statement Compilation ---

static bool compile_statement(Compiler* compiler, Stmt* stmt) {
    // Defensive check: if stmt is NULL, report error and return
    if (stmt == NULL) {
        compiler->has_error = true;
        fprintf(stderr, "Internal compiler error: NULL statement encountered\n");
        return false;
    }

    switch (stmt->type) {
        case STMT_COMPILER_DIRECTIVE: {
            CompilerDirectiveStmt* dir = &stmt->as.compiler_directive;
            if (dir->type == DIRECTIVE_TCO) {
                Token arg = dir->argument;
                if (arg.length == 10 && memcmp(arg.start, "aggressive", 10) == 0) {
                    compiler->tco_mode = TCO_AGGRESSIVE;
                } else if (arg.length == 5 && memcmp(arg.start, "smart", 5) == 0) {
                    compiler->tco_mode = TCO_SMART;
                } else if (arg.length == 4 && memcmp(arg.start, "safe", 4) == 0) {
                    compiler->tco_mode = TCO_SAFE;
                } else if (arg.length == 3 && memcmp(arg.start, "off", 3) == 0) {
                    compiler->tco_mode = TCO_OFF;
                }
            }
            return false;
        }
        case STMT_VAR_DECLARATION: {
            VarDeclStmt* var_stmt = &stmt->as.var_declaration;
            for (int i = 0; i < var_stmt->count; i++) {
                VarDecl* var = &var_stmt->variables[i];
                if (compiler->scope_depth > 0) { // Local variable
                    declare_variable(compiler, &var->name);

                    if (var->qualifier == VAR_REF) {
                        // Reference: create a reference object that points to another variable or collection element
                        if (!var->initializer) {
                            printf("Error: ref variable must have an initializer.\n");
                            break;
                        }

                        int ref_reg = reserve_register(compiler);
                        emit_reference_from_expr(compiler, var->initializer, ref_reg, stmt->line);

                        // Add the local (it stores a reference object)
                        add_local_at_reg(compiler, var->name, ref_reg);
                        compiler->locals[compiler->local_count - 1].is_reference = true;
                        compiler->locals[compiler->local_count - 1].ref_target_reg = -1;

                    } else if (var->qualifier == VAR_VAL) {
                        // Value: deep clone the initializer
                        int value_reg = reserve_register(compiler);
                        if (var->initializer) {
                            int temp_reg = alloc_temp(compiler);
                            compile_expression(compiler, var->initializer, temp_reg);
                            // Emit CLONE_VALUE instruction
                            emit_instruction(compiler, PACK_ABC(CLONE_VALUE, value_reg, temp_reg, 0), stmt->line);
                        } else {
                            int null_const = make_constant(compiler, NULL_VAL);
                            emit_instruction(compiler, PACK_ABx(LOAD_CONST, value_reg, null_const), stmt->line);
                        }
                        add_local_at_reg(compiler, var->name, value_reg);

                    } else if (var->qualifier == VAR_CLONE) {
                        // Clone: deep clone with reference rewriting
                        int value_reg = reserve_register(compiler);
                        if (var->initializer) {
                            int temp_reg = alloc_temp(compiler);
                            compile_expression(compiler, var->initializer, temp_reg);
                            // Emit DEEP_CLONE_VALUE instruction
                            emit_instruction(compiler, PACK_ABC(DEEP_CLONE_VALUE, value_reg, temp_reg, 0), stmt->line);
                        } else {
                            int null_const = make_constant(compiler, NULL_VAL);
                            emit_instruction(compiler, PACK_ABx(LOAD_CONST, value_reg, null_const), stmt->line);
                        }
                        add_local_at_reg(compiler, var->name, value_reg);

                        // Check for struct type
                        if (var->initializer && var->initializer->type == EXPR_STRUCT_INST) {
                            ObjStructSchema* struct_schema = get_struct_schema(compiler, &var->initializer->as.struct_inst.struct_name);
                            if (struct_schema) {
                                compiler->locals[compiler->local_count - 1].struct_type = struct_schema;
                            }
                        }

                    } else {
                        // Normal: current behavior
                        int value_reg = reserve_register(compiler);
                        bool initializer_is_ref = false;
                        ObjStructSchema* struct_schema = NULL;

                        if (var->initializer) {
                            // Check if initializer is a ref expression
                            if (var->initializer->type == EXPR_UNARY &&
                                var->initializer->as.unary.operator.type == TOKEN_REF) {
                                initializer_is_ref = true;
                            }
                            // Also check if initializer is a function call (might return a reference)
                            else if (var->initializer->type == EXPR_CALL) {
                                initializer_is_ref = true;
                            }
                            // Check if initializer is a struct instantiation
                            else if (var->initializer->type == EXPR_STRUCT_INST) {
                                struct_schema = get_struct_schema(compiler, &var->initializer->as.struct_inst.struct_name);
                            }
                            compile_expression(compiler, var->initializer, value_reg);
                        } else {
                            int null_const = make_constant(compiler, NULL_VAL);
                            emit_instruction(compiler, PACK_ABx(LOAD_CONST, value_reg, null_const), stmt->line);
                        }
                        add_local_at_reg(compiler, var->name, value_reg);

                        // If initializer was a ref expression or function call, mark this local as holding a reference
                        if (initializer_is_ref) {
                            compiler->locals[compiler->local_count - 1].is_reference = true;
                        }
                        // If initializer was a struct, record the struct type
                        if (struct_schema) {
                            compiler->locals[compiler->local_count - 1].struct_type = struct_schema;
                        }
                    }
                } else { // Global variable
                    if (var->qualifier == VAR_REF) {
                        // Global references: create reference object
                        if (!var->initializer) {
                            printf("Error: ref variable must have an initializer.\n");
                            break;
                        }

                        int ref_reg = alloc_temp(compiler);
                        emit_reference_from_expr(compiler, var->initializer, ref_reg, stmt->line);

                        // Define the new global with this reference
                        int name_const = identifier_constant(compiler, &var->name);
                        int bytecode_pos = compiler->compiling_chunk->count;
                        emit_instruction(compiler, PACK_ABx(DEFINE_GLOBAL, ref_reg, name_const), stmt->line);
                        record_global_decl(compiler, var->name, bytecode_pos);

                    } else {
                        int value_reg = alloc_temp(compiler);
                        ObjStructSchema* struct_schema = NULL;

                        if (var->qualifier == VAR_VAL && var->initializer) {
                            // Clone for global val
                            int temp_reg = alloc_temp(compiler);
                            // Check if initializer is a struct instantiation
                            if (var->initializer->type == EXPR_STRUCT_INST) {
                                struct_schema = get_struct_schema(compiler, &var->initializer->as.struct_inst.struct_name);
                            }
                            compile_expression(compiler, var->initializer, temp_reg);
                            emit_instruction(compiler, PACK_ABC(CLONE_VALUE, value_reg, temp_reg, 0), stmt->line);
                        } else if (var->qualifier == VAR_CLONE && var->initializer) {
                            // Deep clone for global clone
                            int temp_reg = alloc_temp(compiler);
                            // Check if initializer is a struct instantiation
                            if (var->initializer->type == EXPR_STRUCT_INST) {
                                struct_schema = get_struct_schema(compiler, &var->initializer->as.struct_inst.struct_name);
                            }
                            compile_expression(compiler, var->initializer, temp_reg);
                            emit_instruction(compiler, PACK_ABC(DEEP_CLONE_VALUE, value_reg, temp_reg, 0), stmt->line);
                        } else if (var->initializer) {
                            // Check if initializer is a struct instantiation
                            if (var->initializer->type == EXPR_STRUCT_INST) {
                                struct_schema = get_struct_schema(compiler, &var->initializer->as.struct_inst.struct_name);
                            }
                            compile_expression(compiler, var->initializer, value_reg);
                        } else {
                            int null_const = make_constant(compiler, NULL_VAL);
                            emit_instruction(compiler, PACK_ABx(LOAD_CONST, value_reg, null_const), stmt->line);
                        }

                        int name_const = identifier_constant(compiler, &var->name);
                        int bytecode_pos = compiler->compiling_chunk->count;
                        emit_instruction(compiler, PACK_ABx(DEFINE_GLOBAL, value_reg, name_const), stmt->line);

                        // Record all global declarations (they auto-initialize to null if no explicit initializer)
                        record_global_decl(compiler, var->name, bytecode_pos);

                        // If initializer was a struct, record the global type
                        if (struct_schema) {
                            ObjString* var_name = copyString(compiler->vm, var->name.start, var->name.length);
                            pushTempRoot(compiler->vm, (Obj*)var_name);
                            record_global_type(compiler, var_name, struct_schema);
                            popTempRoot(compiler->vm);
                        }
                    }
                }
            }
            return false;
        }
        case STMT_STRUCT_DECLARATION: {
            StructDeclStmt* struct_stmt = &stmt->as.struct_declaration;

            // Create interned field names
            ObjString** field_names = ALLOCATE(compiler->vm, ObjString*, struct_stmt->field_count);
            for (int i = 0; i < struct_stmt->field_count; i++) {
                field_names[i] = copyString(compiler->vm, struct_stmt->fields[i].start, struct_stmt->fields[i].length);
                pushTempRoot(compiler->vm, (Obj*)field_names[i]);
            }

            // Create struct name
            ObjString* struct_name = copyString(compiler->vm, struct_stmt->name.start, struct_stmt->name.length);
            pushTempRoot(compiler->vm, (Obj*)struct_name);

            // Create the schema object
            ObjStructSchema* schema = newStructSchema(compiler->vm, struct_name, field_names, struct_stmt->field_count);

            popTempRoot(compiler->vm); // Pop struct_name
            for (int i = 0; i < struct_stmt->field_count; i++) {
                popTempRoot(compiler->vm); // Pop each field string
            }

            // Store schema in compiler for lookup (supports shadowing)
            if (compiler->struct_schema_count < MAX_LOCALS) {
                compiler->struct_schemas[compiler->struct_schema_count].name = struct_stmt->name;
                compiler->struct_schemas[compiler->struct_schema_count].field_names = field_names;
                compiler->struct_schemas[compiler->struct_schema_count].field_count = struct_stmt->field_count;
                compiler->struct_schemas[compiler->struct_schema_count].depth = compiler->scope_depth;
                compiler->struct_schemas[compiler->struct_schema_count].schema = schema;
                compiler->struct_schema_count++;
            }

            // Schemas are not runtime values - they're compile-time only
            // The schema is stored in constants when instantiating
            return false;
        }
        case STMT_ENUM_DECLARATION: {
            EnumDeclStmt* enum_stmt = &stmt->as.enum_declaration;

            // Create interned variant names
            ObjString** variant_names = ALLOCATE(compiler->vm, ObjString*, enum_stmt->variant_count);
            for (int i = 0; i < enum_stmt->variant_count; i++) {
                variant_names[i] = copyString(compiler->vm, enum_stmt->variants[i].start, enum_stmt->variants[i].length);
                pushTempRoot(compiler->vm, (Obj*)variant_names[i]);
            }

            // Create enum name
            ObjString* enum_name = copyString(compiler->vm, enum_stmt->name.start, enum_stmt->name.length);
            pushTempRoot(compiler->vm, (Obj*)enum_name);

            // Create the enum schema object (assigns unique type_id)
            ObjEnumSchema* schema = newEnumSchema(compiler->vm, enum_name, variant_names, enum_stmt->variant_count);

            popTempRoot(compiler->vm); // Pop enum_name
            for (int i = 0; i < enum_stmt->variant_count; i++) {
                popTempRoot(compiler->vm); // Pop each enum variant string
            }

            // Store schema in compiler for lookup (supports shadowing)
            if (compiler->enum_schema_count < MAX_LOCALS) {
                compiler->enum_schemas[compiler->enum_schema_count].name = enum_stmt->name;
                compiler->enum_schemas[compiler->enum_schema_count].variant_names = variant_names;
                compiler->enum_schemas[compiler->enum_schema_count].variant_count = enum_stmt->variant_count;
                compiler->enum_schemas[compiler->enum_schema_count].depth = compiler->scope_depth;
                compiler->enum_schemas[compiler->enum_schema_count].schema = schema;
                compiler->enum_schema_count++;
            }

            // Store schema as a global so VM can look it up by type_id for error messages
            // Use a special internal name prefix to avoid conflicts: "__enum_schema_<name>"
            if (compiler->scope_depth == 0) {
                int schema_reg = alloc_temp(compiler);
                int schema_const = make_constant(compiler, OBJ_VAL(schema));
                emit_instruction(compiler, PACK_ABx(LOAD_CONST, schema_reg, schema_const), stmt->line);

                // Create internal name: "__enum_schema_Color"
                char internal_name[256];
                int name_len = snprintf(internal_name, sizeof(internal_name), "__enum_schema_%.*s", enum_stmt->name.length, enum_stmt->name.start);
                ObjString* str = copyString(compiler->vm, internal_name, name_len);
                pushTempRoot(compiler->vm, (Obj*)str);
                int name_const = make_constant(compiler, OBJ_VAL(str));
                popTempRoot(compiler->vm);
                emit_instruction(compiler, PACK_ABx(DEFINE_GLOBAL, schema_reg, name_const), stmt->line);
            }

            return false;
        }
        case STMT_FUNC_DECLARATION: {
            FuncDeclStmt* func_stmt = &stmt->as.func_declaration;

            // First, declare a variable for the function in the current scope.
            // This allows a function to refer to itself for recursion.
            int name_ident;
            if (compiler->scope_depth > 0) {
                // It's a local function. Mangle the name to support local overloading.
                char* mangled_chars = mangle_name(compiler, &func_stmt->name, func_stmt->param_count);
                Token mangled_token = {
                    .start = mangled_chars,
                    .length = (int)strlen(mangled_chars),
                    .line = func_stmt->name.line
                };

                // Check if this variable was already declared (from block pre-declaration)
                name_ident = resolve_local(compiler, &mangled_token);
                if (name_ident == -1) {
                    // Not already declared, so declare it now
                    declare_variable(compiler, &mangled_token);
                    name_ident = add_local(compiler, mangled_token);

                    // Mark the local as initialized immediately so the function body can reference itself
                    compiler->locals[compiler->local_count - 1].is_initialized = true;

                    // we own this temporary string; remember to free it when this function finishes compiling
                    // memory leak without
                    track_owned_name(compiler, mangled_chars);
                } else {
                    // Already declared, clean up the mangled name
                    FREE_ARRAY(compiler->vm, char, mangled_chars, strlen(mangled_chars) + 1);
                }
            } else {
                // It's a global function. We just need the constant for its name.
                char* mangled = mangle_name(compiler, &func_stmt->name, func_stmt->param_count);
                ObjString* str = copyString(compiler->vm, mangled, strlen(mangled));
                pushTempRoot(compiler->vm, (Obj*)str);
                name_ident = make_constant(compiler, OBJ_VAL(str));
                popTempRoot(compiler->vm);
                FREE_ARRAY(compiler->vm, char, mangled, strlen(mangled) + 1);
            }

            // Compile the function body and create the closure.
            ObjFunction* function = compile_function_body(compiler, func_stmt);
            int const_index = make_constant(compiler, OBJ_VAL(function));
            popTempRoot(compiler->vm);  // Pop protection from compile_function_body

            // Store upvalue count in hoisted function info for TCO optimization
            if (compiler->scope_depth == 0) {
                // Global function - find it in hoisted array and store upvalue count
                for (int i = 0; i < compiler->hoisted_count; i++) {
                    if (compiler->hoisted[i].name.length == func_stmt->name.length &&
                        memcmp(compiler->hoisted[i].name.start, func_stmt->name.start, func_stmt->name.length) == 0 &&
                        compiler->hoisted[i].arity == func_stmt->param_count) {
                        compiler->hoisted[i].upvalue_count = function->upvalue_count;
                        break;
                    }
                }
            } else {
                // Local function - find it in local_hoisted array and store upvalue count
                for (int i = 0; i < compiler->local_hoisted_count; i++) {
                    if (compiler->local_hoisted[i].name.length == func_stmt->name.length &&
                        memcmp(compiler->local_hoisted[i].name.start, func_stmt->name.start, func_stmt->name.length) == 0 &&
                        compiler->local_hoisted[i].arity == func_stmt->param_count) {
                        compiler->local_hoisted[i].upvalue_count = function->upvalue_count;
                        break;
                    }
                }
            }

            int closure_reg = alloc_temp(compiler);
            emit_closure(compiler, closure_reg, const_index, stmt->line);

            // Now, store that closure in the variable we declared.
            if (compiler->scope_depth > 0) {
                // For a local function, MOVE the closure into its assigned register.
                emit_move(compiler, name_ident, closure_reg, stmt->line);
            } else {
                // For a global function, update the global variable.
                emit_set_global(compiler, closure_reg, name_ident, stmt->line);
            }
            return false;
        }
        case STMT_BLOCK: {
            bool saved_tail_block = compiler->in_tail_position;
            compiler->in_tail_position = false;
            begin_scope(compiler);

            // Collect locally hoisted functions in this block before compiling statements
            BlockStmt* block = &stmt->as.block;
            for (int i = 0; i < block->count; i++) {
                collect_local_hoisted_in_stmt(compiler, block->statements[i]);
            }

            // Pre-declare all function declarations to make them visible
            for (int i = 0; i < block->count; i++) {
                if (block->statements[i]->type == STMT_FUNC_DECLARATION) {
                    FuncDeclStmt* func_stmt = &block->statements[i]->as.func_declaration;

                    // Declare the mangled name
                    char* mangled_chars = mangle_name(compiler, &func_stmt->name, func_stmt->param_count);
                    Token mangled_token = {
                        .start = mangled_chars,
                        .length = (int)strlen(mangled_chars),
                        .line = func_stmt->name.line
                    };

                    declare_variable(compiler, &mangled_token);
                    int name_ident = add_local(compiler, mangled_token);

                    // Mark as initialized so it can be referenced
                    compiler->locals[compiler->local_count - 1].is_initialized = true;

                    // Track the mangled name for cleanup
                    track_owned_name(compiler, mangled_chars);
                }
            }

            // Pre-declare all variable declarations WITHOUT evaluating initializers.
            // This reserves register slots for all local variables so they can be captured by closures
            // that are defined earlier in the block (function hoisting).
            for (int i = 0; i < block->count; i++) {
                if (block->statements[i]->type == STMT_VAR_DECLARATION) {
                    VarDeclStmt* var_stmt = &block->statements[i]->as.var_declaration;
                    for (int j = 0; j < var_stmt->count; j++) {
                        VarDecl* var = &var_stmt->variables[j];
                        declare_variable(compiler, &var->name);
                        int value_reg = reserve_register(compiler);
                        // Initialize to null for now; actual initializer will be evaluated later
                        int null_const = make_constant(compiler, NULL_VAL);
                        emit_instruction(compiler, PACK_ABx(LOAD_CONST, value_reg, null_const), block->statements[i]->line);
                        add_local_at_reg(compiler, var->name, value_reg);
                    }
                }
            }

            // Process directives first, then compile function declarations (hoisting), then other statements
            // This ensures directives affect functions that come after them
            for (int i = 0; i < block->count; i++) {
                if (block->statements[i]->type == STMT_COMPILER_DIRECTIVE) {
                    compile_statement(compiler, block->statements[i]);
                }
            }

            // Compile function declarations second (hoisting)
            for (int i = 0; i < block->count; i++) {
                if (block->statements[i]->type == STMT_FUNC_DECLARATION) {
                    compile_statement(compiler, block->statements[i]);
                }
            }

            // Find the last compilable statement index for tail position propagation
            int last_compilable_idx = -1;
            for (int i = 0; i < block->count; i++) {
                if (block->statements[i]->type != STMT_FUNC_DECLARATION &&
                    block->statements[i]->type != STMT_COMPILER_DIRECTIVE &&
                    block->statements[i]->type != STMT_VAR_DECLARATION) {
                    last_compilable_idx = i;
                }
            }

            // Check if any statement terminates
            bool terminates = false;
            for (int i = 0; i < block->count; i++) {
                Stmt* s = block->statements[i];

                // Skip function declarations and compiler directives - already handled
                if (s->type == STMT_FUNC_DECLARATION || s->type == STMT_COMPILER_DIRECTIVE) {
                    continue;
                }

                // Handle pre-declared variable declarations specially
                if (s->type == STMT_VAR_DECLARATION) {
                    VarDeclStmt* var_stmt = &s->as.var_declaration;
                    for (int j = 0; j < var_stmt->count; j++) {
                        VarDecl* var = &var_stmt->variables[j];
                        int var_reg = resolve_local(compiler, &var->name);

                        if (var_reg != -1) {
                            // Variable was pre-declared - just compile the initializer
                            if (var->qualifier == VAR_REF) {
                                // Reference variable
                                if (var->initializer) {
                                    emit_reference_from_expr(compiler, var->initializer, var_reg, s->line);
                                    // Mark as reference
                                    for (int k = 0; k < compiler->local_count; k++) {
                                        if (compiler->locals[k].reg == var_reg) {
                                            compiler->locals[k].is_reference = true;
                                            compiler->locals[k].ref_target_reg = -1;
                                            break;
                                        }
                                    }
                                }
                            } else if (var->qualifier == VAR_VAL) {
                                // Val variable - deep clone
                                if (var->initializer) {
                                    int temp_reg = alloc_temp(compiler);
                                    compile_expression(compiler, var->initializer, temp_reg);
                                    emit_instruction(compiler, PACK_ABC(CLONE_VALUE, var_reg, temp_reg, 0), s->line);
                                }
                            } else if (var->qualifier == VAR_CLONE) {
                                // Clone variable - deep clone with ref rewriting
                                if (var->initializer) {
                                    int temp_reg = alloc_temp(compiler);
                                    compile_expression(compiler, var->initializer, temp_reg);
                                    emit_instruction(compiler, PACK_ABC(DEEP_CLONE_VALUE, var_reg, temp_reg, 0), s->line);
                                }
                            } else {
                                // Normal variable - evaluate initializer
                                if (var->initializer) {
                                    compile_expression(compiler, var->initializer, var_reg);
                                    // Handle special cases for references from function calls
                                    if (var->initializer->type == EXPR_CALL) {
                                        for (int k = 0; k < compiler->local_count; k++) {
                                            if (compiler->locals[k].reg == var_reg) {
                                                compiler->locals[k].is_reference = true;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                            // Mark as initialized
                            for (int k = 0; k < compiler->local_count; k++) {
                                if (compiler->locals[k].reg == var_reg) {
                                    compiler->locals[k].is_initialized = true;
                                    break;
                                }
                            }
                        }
                    }
                    continue;
                }

                // Propagate tail position to the last compilable statement
                if (i == last_compilable_idx) {
                    compiler->in_tail_position = saved_tail_block;
                }

                bool stmt_terminates = compile_statement(compiler, s);
                compiler->in_tail_position = false;
                if (stmt_terminates) {
                    terminates = true;
                    // Note: We still compile remaining statements for error checking
                    // but we know the block terminates
                }
            }

            compiler->in_tail_position = false;
            end_scope(compiler);
            return terminates;
        }
        case STMT_EXPRESSION: {
            Expr* expression = stmt->as.expression.expression;

            // --- Tail position TCO: bare call at end of function ---
            if (compiler->in_tail_position && compiler->tco_mode != TCO_OFF) {
                Expr* unwrapped = expression;
                while (unwrapped->type == EXPR_GROUPING) {
                    unwrapped = unwrapped->as.grouping.expression;
                }
                if (unwrapped->type == EXPR_CALL) {
                    if (try_compile_tail_call(compiler, unwrapped, stmt->line)) {
                        return true;
                    }
                }
            }

            // --- Optimize for assignment statements ---
            if (expression->type == EXPR_ASSIGN) {
                AssignExpr* assign = &expression->as.assign;
                // Check if we are assigning to a simple local variable.
                if (assign->target->type == EXPR_VARIABLE) {
                    Token name = assign->target->as.variable.name;
                    int reg = resolve_local(compiler, &name);
                    if (reg != -1) {
                        // Check if this is a reference - if so, skip optimization
                        bool is_ref = false;
                        for (int i = 0; i < compiler->local_count; i++) {
                            if (compiler->locals[i].reg == reg && compiler->locals[i].is_reference) {
                                is_ref = true;
                                break;
                            }
                        }

                        if (!is_ref) {
                            // Compile the value directly into the variable's home register.
                            // We don't need to ask for the result in a new temp register.
                            compile_expression(compiler, assign->value, reg);
                            return false; // Optimization complete, statement does not terminate.
                        }
                    }
                }
            }

            // Default logic for all other kinds of expressions (like function calls)
            // or assignments to globals/properties.
            // Mark that the result is not needed (dead store elimination)
            bool saved_result_needed = compiler->result_needed;
            compiler->result_needed = false;
            int temp_reg = alloc_temp(compiler);
            compile_expression(compiler, expression, temp_reg);
            compiler->result_needed = saved_result_needed;
            return false;
        }
        case STMT_IF: {
            bool saved_tail_if = compiler->in_tail_position;
            compiler->in_tail_position = false;

            // Try to optimize with branch-compare instruction
            int then_jump = try_emit_branch_compare(compiler, stmt->as.if_stmt.condition, false, stmt->line);

            if (then_jump == -1) {
                // Fallback: use regular comparison + JUMP_IF_FALSE
                int condition_reg = alloc_temp(compiler);
                compile_expression(compiler, stmt->as.if_stmt.condition, condition_reg);
                then_jump = emit_jump_instruction(compiler, JUMP_IF_FALSE, condition_reg, stmt->line);
            }

            // Propagate tail position to both branches
            compiler->in_tail_position = saved_tail_if;
            bool then_terminates = compile_statement(compiler, stmt->as.if_stmt.then_branch);
            compiler->in_tail_position = false;

            // Only emit else-jump if then-branch doesn't terminate
            int else_jump = -1;
            if (!then_terminates) {
                else_jump = emit_jump_instruction(compiler, JUMP, 0, stmt->line);
            }

            patch_jump(compiler, then_jump);
            compiler->in_tail_position = saved_tail_if;
            bool else_terminates = false;
            if (stmt->as.if_stmt.else_branch != NULL) {
                else_terminates = compile_statement(compiler, stmt->as.if_stmt.else_branch);
            }
            compiler->in_tail_position = false;

            if (else_jump != -1) {
                patch_jump(compiler, else_jump);
            }

            // If statement terminates if both branches exist and both terminate
            return then_terminates && stmt->as.if_stmt.else_branch != NULL && else_terminates;
        }
        case STMT_WHILE: {
            bool saved_tail_while = compiler->in_tail_position;
            compiler->in_tail_position = false;
            int loop_start = compiler->compiling_chunk->count;

            // Try to optimize with branch-compare instruction
            int exit_jump = try_emit_branch_compare(compiler, stmt->as.while_stmt.condition, false, stmt->line);

            if (exit_jump == -1) {
                // Fallback: use regular comparison + JUMP_IF_FALSE
                int condition_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, stmt->as.while_stmt.condition, condition_reg);
                exit_jump = emit_jump_instruction(compiler, JUMP_IF_FALSE, condition_reg, stmt->line);
            }

            // Mark the start of this loop's 'break' jump list.
            int break_list_start = compiler->break_count;
            compiler->loop_depth++;

            compiler->loop_continues[compiler->loop_depth - 1] = loop_start;

            compile_statement(compiler, stmt->as.while_stmt.body);
            emit_loop(compiler, loop_start, stmt->line);

            patch_jump(compiler, exit_jump);

            // Now, patch all 'break' jumps that occurred inside this loop.
            for (int i = break_list_start; i < compiler->break_count; i++) {
                patch_jump(compiler, compiler->break_jumps[i]);
            }
            // "Pop" this loop's breaks from the list.
            compiler->break_count = break_list_start;
            compiler->loop_depth--;
            compiler->in_tail_position = saved_tail_while;
            return false;
        }
        case STMT_DO_WHILE: {
            bool saved_tail_dowhile = compiler->in_tail_position;
            compiler->in_tail_position = false;

            // Do-while: body executes first, then condition is checked
            // Layout:
            //   1. Jump over jump-to-condition (first iteration executes body)
            //   2. loop_start: Jump to condition (for continue statements)
            //   3. body_start: <body code>
            //   4. condition_start: <condition check>
            //   5. If true, jump to body_start; if false, exit

            // Mark the start of this loop's 'break' jump list
            int break_list_start = compiler->break_count;
            compiler->loop_depth++;

            // On first iteration, jump over the continue-target-jump
            int skip_continue_jump = emit_jump_instruction(compiler, JUMP, 0, stmt->line);

            // loop_start: This is where continue statements will jump to
            int loop_start = compiler->compiling_chunk->count;

            // Emit a forward jump to condition (for continue)
            int jump_to_condition = emit_jump_instruction(compiler, JUMP, 0, stmt->line);

            // Set continue target
            compiler->loop_continues[compiler->loop_depth - 1] = loop_start;

            // body_start: Patch first-iteration jump to here
            int body_start = compiler->compiling_chunk->count;
            patch_jump(compiler, skip_continue_jump);

            // Compile the body
            compile_statement(compiler, stmt->as.do_while_stmt.body);

            // condition_start: Patch the continue jump to here
            int condition_start = compiler->compiling_chunk->count;
            patch_jump(compiler, jump_to_condition);

            int condition_reg = alloc_temp(compiler);
            COMPILE_REQUIRED(compiler, stmt->as.do_while_stmt.condition, condition_reg);

            // If condition is false, skip the loop-back jump
            int skip_jump = emit_jump_instruction(compiler, JUMP_IF_FALSE, condition_reg, stmt->line);

            // If condition is true, jump back to body_start (not loop_start!)
            emit_loop(compiler, body_start, stmt->line);

            // Patch the skip jump to here (exit point)
            patch_jump(compiler, skip_jump);

            // Patch all 'break' jumps that occurred inside this loop
            for (int i = break_list_start; i < compiler->break_count; i++) {
                patch_jump(compiler, compiler->break_jumps[i]);
            }
            // "Pop" this loop's breaks from the list
            compiler->break_count = break_list_start;
            compiler->loop_depth--;
            compiler->in_tail_position = saved_tail_dowhile;
            return false;
        }
        case STMT_FOR: {
            bool saved_tail_for = compiler->in_tail_position;
            compiler->in_tail_position = false;
            begin_scope(compiler);

            // 1) initializer
            if (stmt->as.for_stmt.initializer) {
                compile_statement(compiler, stmt->as.for_stmt.initializer);
            }

            // 2) Skip increment on first entry; we'll patch this to the condition.
            int jump_to_cond = emit_jump_instruction(compiler, JUMP, 0, stmt->line);

            // 3) continue target (increment label) — 'continue' should land here
            int continue_target = compiler->compiling_chunk->count;
            if (stmt->as.for_stmt.increment) {
                int tmp = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, stmt->as.for_stmt.increment, tmp);
            }

            // 4) condition label; patch the first-entry jump to here
            int cond_label = compiler->compiling_chunk->count;
            patch_jump(compiler, jump_to_cond);

            int exit_jump = -1;
            if (stmt->as.for_stmt.condition) {
                int cond_reg = alloc_temp(compiler);
                COMPILE_REQUIRED(compiler, stmt->as.for_stmt.condition, cond_reg);
                exit_jump = emit_jump_instruction(compiler, JUMP_IF_FALSE, cond_reg, stmt->line);
            }

            // 5) loop bookkeeping
            int break_list_start = compiler->break_count;
            compiler->loop_depth++;
            compiler->loop_continues[compiler->loop_depth - 1] = continue_target;

            // 6) body
            compile_statement(compiler, stmt->as.for_stmt.body);

            // 7) tail jump back to increment (and then condition)
            emit_loop(compiler, continue_target, stmt->line);

            // 8) patch exits & breaks
            if (exit_jump != -1) patch_jump(compiler, exit_jump);
            for (int i = break_list_start; i < compiler->break_count; i++) {
                patch_jump(compiler, compiler->break_jumps[i]);
            }
            compiler->break_count = break_list_start;
            compiler->loop_depth--;

            end_scope(compiler);
            compiler->in_tail_position = saved_tail_for;
            return false;
        }
        case STMT_BREAK: {
            if (compiler->loop_depth == 0) {
                printf("[Line %d] Error: 'break' statement outside of a loop.\n", stmt->line);
                return false;
            }
            // Emit a placeholder jump and add it to the patch list for the current loop.
            int jump_address = emit_jump_instruction(compiler, JUMP, 0, stmt->line);
            add_break_jump(compiler, jump_address);
            return false;
        }
        case STMT_CONTINUE: {
            if (compiler->loop_depth == 0) {
                printf("[Line %d] Error: 'continue' statement outside of a loop.\n", stmt->line);
                return false;
            }
            // Emit a jump back to the start of the current loop's next iteration.
            emit_loop(compiler, compiler->loop_continues[compiler->loop_depth - 1], stmt->line);
            return false;
        }
        case STMT_RETURN: {
            if (stmt->as.return_stmt.value) {
                // Check if we're returning an overloaded function by plain name
                // If so, create a dispatcher that holds all overloads
                if (stmt->as.return_stmt.value->type == EXPR_VARIABLE) {
                    Token* name = &stmt->as.return_stmt.value->as.variable.name;
                    int lar = single_local_hoisted_arity(compiler, name);

                    if (lar == -2) {
                        // Multiple overloads exist! Use emit_dispatcher
                        int reg = alloc_temp(compiler);
                        emit_dispatcher(compiler, name, reg, stmt->line, true);
                        emit_instruction(compiler, PACK_ABx(RET, reg, 0), stmt->line);
                        return true;
                    }
                }

                // Check for tail call optimization: return <function_call>
                // Unwrap any grouping expressions (e.g., return (foo()) should still be TCO'd)
                Expr* return_expr = stmt->as.return_stmt.value;
                while (return_expr->type == EXPR_GROUPING) {
                    return_expr = return_expr->as.grouping.expression;
                }

                if (return_expr->type == EXPR_CALL && compiler->tco_mode != TCO_OFF) {
                    // Try to compile as tail call - if successful, we're done
                    if (try_compile_tail_call(compiler, return_expr, stmt->line)) {
                        return true;
                    }
                    // Otherwise fall through to normal return
                }

            normal_return:
                // Normal return path - optimize for direct variable returns
                if (stmt->as.return_stmt.value->type == EXPR_VARIABLE) {
                    Token* name = &stmt->as.return_stmt.value->as.variable.name;
                    int var_reg = resolve_local(compiler, name);

                    if (var_reg != -1 && !is_local_ref_param(compiler, var_reg)) {
                        // Normal local variable - return it directly without MOVE
                        emit_instruction(compiler, PACK_ABx(RET, var_reg, 0), stmt->line);
                        return true;
                    }
                }

                // Complex expression or needs dereferencing - use temp register
                int reg = alloc_temp(compiler);
                compile_expression(compiler, stmt->as.return_stmt.value, reg);
                emit_instruction(compiler, PACK_ABx(RET, reg, 0), stmt->line);
            } else {
                // Implicit return of null
                emit_instruction(compiler, PACK_ABx(RET, 0, 1), stmt->line); // Bx=1 means return null
            }
            return true;
        }
        case STMT_LABEL: {
            Token* label_name = &stmt->as.label.label_name;

            // Check if label already exists
            if (find_label(compiler, label_name)) {
                compiler_error(compiler, stmt->line, "Label '%.*s' already defined", label_name->length, label_name->start);
                break;
            }

            // Check label count
            if (compiler->label_count >= MAX_LABELS) {
                compiler_error(compiler, stmt->line, "Too many labels in function (max %d)", MAX_LABELS);
                break;
            }

            // Register label at current instruction address
            int addr = compiler->compiling_chunk->count;
            compiler->labels[compiler->label_count++] = (Label){
                .name = *label_name,
                .instruction_address = addr,
                .scope_depth = compiler->scope_depth,
                .local_count = compiler->local_count,
                .is_resolved = true
            };

            // Patch any pending forward gotos to this label
            for (int i = 0; i < compiler->pending_goto_count; i++) {
                PendingGoto* pending = &compiler->pending_gotos[i];
                if (tokens_equal(&pending->target_label, label_name)) {
                    // Validate the goto
                    GotoSafetyResult safety = validate_goto_safety(
                        compiler,
                        pending->goto_scope_depth,
                        pending->goto_local_count,
                        pending->goto_bytecode_pos,
                        compiler->scope_depth,
                        compiler->local_count,
                        addr
                    );

                    if (safety == GOTO_ERROR_INTO_SCOPE) {
                        compiler_error(compiler, stmt->line,
                            "goto jumps into inner scope (not allowed)");
                    } else if (safety == GOTO_ERROR_SKIP_INIT) {
                        compiler_error(compiler, stmt->line,
                            "goto skips variable initialization (not allowed)");
                    } else {
                        // Safe - patch the jump and mark as resolved
                        patch_jump(compiler, pending->jump_address);
                        pending->is_resolved = true;
                    }
                }
            }
            return false;
        }
        case STMT_GOTO: {
            Token* target_label = &stmt->as.goto_stmt.target_label;
            int current_bytecode_pos = compiler->compiling_chunk->count;

            // Look for existing label (backward jump)
            Label* label = find_label(compiler, target_label);

            if (label) {
                // Backward jump - validate immediately
                GotoSafetyResult safety = validate_goto_safety(
                    compiler,
                    compiler->scope_depth,
                    compiler->local_count,
                    current_bytecode_pos,
                    label->scope_depth,
                    label->local_count,
                    label->instruction_address
                );

                if (safety == GOTO_ERROR_INTO_SCOPE) {
                    compiler_error(compiler, stmt->line,
                        "goto jumps into inner scope (not allowed)");
                    break;
                } else if (safety == GOTO_ERROR_SKIP_INIT) {
                    compiler_error(compiler, stmt->line,
                        "goto skips variable initialization (not allowed)");
                    break;
                }

                // Emit cleanup if jumping to outer scope
                if (compiler->scope_depth > label->scope_depth) {
                    emit_goto_cleanup(compiler, compiler->scope_depth, label->scope_depth, stmt->line);
                }

                // Emit backward jump
                emit_loop(compiler, label->instruction_address, stmt->line);
            } else {
                // Forward jump - add to pending list
                int jump_addr = emit_jump_instruction(compiler, JUMP, 0, stmt->line);
                add_pending_goto(compiler, jump_addr, *target_label,
                                compiler->scope_depth, compiler->local_count, current_bytecode_pos);
            }
            return false;
        }
        case STMT_SWITCH: {
            bool saved_tail_switch = compiler->in_tail_position;
            compiler->in_tail_position = false;

            // Compile the switch expression once
            int switch_reg = alloc_temp(compiler);
            compile_expression(compiler, stmt->as.switch_stmt.expression, switch_reg);

            // Track jump addresses for each case body
            int* case_body_jumps = ALLOCATE(compiler->vm, int, stmt->as.switch_stmt.case_count);

            // First pass: emit all case comparisons
            int default_body_start = -1;
            int case_body_count = 0;

            for (int i = 0; i < stmt->as.switch_stmt.case_count; i++) {
                CaseClause* case_clause = &stmt->as.switch_stmt.cases[i];

                if (case_clause->value == NULL) {
                    // This is the default case - note its position but don't emit comparison
                    default_body_start = i;
                    case_body_jumps[i] = -1; // Will be set later
                    continue;
                }

                // Compile case value into a temp register
                int case_value_reg = alloc_temp(compiler);
                compile_expression(compiler, case_clause->value, case_value_reg);

                // Compare: cmp_reg = (switch_reg == case_value_reg)
                int cmp_reg = alloc_temp(compiler);
                emit_instruction(compiler, PACK_ABC(EQ, cmp_reg, switch_reg, case_value_reg), stmt->line);

                // If comparison is true (equal), jump to this case's body
                // We emit a JUMP_IF_FALSE and then a JUMP - the JUMP gets us to the body
                int skip_to_body = emit_jump_instruction(compiler, JUMP_IF_FALSE, cmp_reg, stmt->line);

                // Match! Jump to case body (will be patched in second pass)
                case_body_jumps[i] = emit_jump_instruction(compiler, JUMP, 0, stmt->line);

                // No match - patch skip_to_body to continue to next case
                patch_jump(compiler, skip_to_body);
            }

            // After all case checks, jump to default (if exists) or end
            int no_match_jump = emit_jump_instruction(compiler, JUMP, 0, stmt->line);

            // Track break jumps for this switch (switch supports break)
            int break_list_start = compiler->break_count;
            compiler->loop_depth++; // Treat switch like a loop for break statements

            // Second pass: emit all case bodies
            for (int i = 0; i < stmt->as.switch_stmt.case_count; i++) {
                CaseClause* case_clause = &stmt->as.switch_stmt.cases[i];

                // Mark the start of this case's body
                int body_start = compiler->compiling_chunk->count;

                // Patch the jump from the comparison to here
                if (case_clause->value != NULL) {
                    patch_jump(compiler, case_body_jumps[i]);
                } else if (i == default_body_start) {
                    // Patch the no-match jump to come here
                    patch_jump(compiler, no_match_jump);
                }

                // Compile all statements in this case, propagating tail position to last statement
                for (int j = 0; j < case_clause->statement_count; j++) {
                    if (j == case_clause->statement_count - 1) {
                        compiler->in_tail_position = saved_tail_switch;
                    }
                    compile_statement(compiler, case_clause->statements[j]);
                    compiler->in_tail_position = false;
                }

                // Note: We don't automatically emit a jump to exit here.
                // If there's no break, execution falls through to the next case.
            }

            // If there was no default case, patch no_match_jump to here (end of switch)
            if (default_body_start == -1) {
                patch_jump(compiler, no_match_jump);
            }

            // Patch all break statements to jump to end of switch
            for (int i = break_list_start; i < compiler->break_count; i++) {
                patch_jump(compiler, compiler->break_jumps[i]);
            }
            compiler->break_count = break_list_start;
            compiler->loop_depth--;

            FREE_ARRAY(compiler->vm, int, case_body_jumps, stmt->as.switch_stmt.case_count);
            return false;
        }
        default: return false;
    }
}

static void init_compiler(Compiler* compiler, VM* vm, Compiler* enclosing) {
    compiler->enclosing = enclosing;
    compiler->vm = vm;
    compiler->current_module_name = NULL;
    compiler->has_error = false;

    // Each compiler works on one function. For the top-level script,
    // this will be a placeholder. For a user-defined function, it will
    // be the function we're building.
    compiler->function = NULL; // Will be set properly when compiling a function body

    compiler->compiling_chunk = NULL; // Will be set to the function's chunk

    compiler->next_register = 0;
    compiler->max_register_seen = 0;
    compiler->temp_free_top = 0;

    compiler->local_count = 0;
    compiler->scope_depth = 0;

    compiler->loop_depth = 0;
    compiler->break_jumps = NULL;
    compiler->break_count = 0;
    compiler->break_capacity = 0;

    memset(compiler->upvalues, 0, sizeof(compiler->upvalues));
    compiler->upvalue_count = 0;

    memset(compiler->hoisted, 0, sizeof(compiler->hoisted));
    compiler->hoisted_count = 0;

    memset(compiler->local_hoisted, 0, sizeof(compiler->local_hoisted));
    compiler->local_hoisted_count = 0;

    compiler->owned_names = NULL;
    compiler->owned_names_count = 0;
    compiler->owned_names_cap = 0;

    // Initialize struct schema tracking
    memset(compiler->struct_schemas, 0, sizeof(compiler->struct_schemas));
    compiler->struct_schema_count = 0;

    // Initialize enum schema tracking
    memset(compiler->enum_schemas, 0, sizeof(compiler->enum_schemas));
    compiler->enum_schema_count = 0;

    // Initialize global type tracking
    compiler->global_types = NULL;
    compiler->global_type_count = 0;
    compiler->global_type_capacity = 0;

    // Inherit TCO mode from enclosing compiler, or default to safe
    compiler->tco_mode = enclosing ? enclosing->tco_mode : TCO_SAFE;

    // Initially not in tail position
    compiler->in_tail_position = false;

    // By default, expression results are needed
    compiler->result_needed = true;

    // Initialize label and goto tracking
    memset(compiler->labels, 0, sizeof(compiler->labels));
    compiler->label_count = 0;
    compiler->pending_gotos = NULL;
    compiler->pending_goto_count = 0;
    compiler->pending_goto_capacity = 0;

    // Initialize global declaration tracking
    memset(compiler->global_decls, 0, sizeof(compiler->global_decls));
    compiler->global_decl_count = 0;


}

// Creates a mangled name string in the format "name@arity".
// The caller is responsible for freeing the returned string.
static char* mangle_name(Compiler* compiler, Token* name, int arity) {
    // Allocate enough space for "name" + "@" + "arity" (e.g., up to 3 digits) + \0
    char* buffer = ALLOCATE(compiler->vm, char, name->length + 5); // +1 for '@', +3 for arity digits, +1 for '\0'
    sprintf(buffer, "%.*s@%d", name->length, name->start, arity);
    return buffer;
}

// --- Pass 1: Declare a function's name (needed for hoisting) ---
static void declare_function(Compiler* compiler, Stmt* stmt) {
    FuncDeclStmt* func_stmt = &stmt->as.func_declaration;

    // Check if function with same name and arity already exists
    for (int i = 0; i < compiler->hoisted_count; i++) {
        if (compiler->hoisted[i].name.length == func_stmt->name.length &&
            memcmp(compiler->hoisted[i].name.start, func_stmt->name.start, func_stmt->name.length) == 0 &&
            compiler->hoisted[i].arity == func_stmt->param_count) {
            fprintf(stderr, "Error at line %d: Function '%.*s' with %d parameter(s) is already defined.\n",
                    stmt->line, func_stmt->name.length, func_stmt->name.start, func_stmt->param_count);
            exit(1);
        }
    }

    // For hoisting, we only need to declare a global variable for the function.
    // We'll initialize it to null. The second pass will patch it with the real closure.
    char* mangled = mangle_name(compiler, &func_stmt->name, func_stmt->param_count);
    ObjString* str = copyString(compiler->vm, mangled, strlen(mangled));
    pushTempRoot(compiler->vm, (Obj*)str);
    int name_const = make_constant(compiler, OBJ_VAL(str));
    popTempRoot(compiler->vm);
    FREE_ARRAY(compiler->vm, char, mangled, strlen(mangled) + 1);

    // Emit code to create a global variable initialized to null.
    int null_reg = alloc_temp(compiler);
    int null_const_idx = make_constant(compiler, NULL_VAL);
    emit_instruction(compiler, PACK_ABx(LOAD_CONST, null_reg, null_const_idx), stmt->line);
    emit_instruction(compiler, PACK_ABx(DEFINE_GLOBAL, null_reg, name_const), stmt->line);

    // Record this function as hoisted so we know its base name + arity later.
    if (compiler->hoisted_count < MAX_HOISTED) {
        compiler->hoisted[compiler->hoisted_count].name = func_stmt->name;
        compiler->hoisted[compiler->hoisted_count].arity = func_stmt->param_count;

        // Store param_qualifiers for compile-time ref/val handling
        if (func_stmt->param_count > 0) {
            compiler->hoisted[compiler->hoisted_count].param_qualifiers =
                ALLOCATE(compiler->vm, uint8_t, func_stmt->param_count);
            for (int i = 0; i < func_stmt->param_count; i++) {
                compiler->hoisted[compiler->hoisted_count].param_qualifiers[i] =
                    (uint8_t)func_stmt->params[i].qualifier;
            }
        } else {
            compiler->hoisted[compiler->hoisted_count].param_qualifiers = NULL;
        }

        // Initialize upvalue_count to -1 (will be set when function body is compiled)
        compiler->hoisted[compiler->hoisted_count].upvalue_count = -1;

        compiler->hoisted_count++;
    }
}

static void collect_local_hoisted_in_stmt(Compiler* c, Stmt* s) {
    if (!s) return;

    switch (s->type) {
        case STMT_FUNC_DECLARATION: {
                FuncDeclStmt* fd = &s->as.func_declaration;

                // Check if local function with same name and arity already exists
                for (int i = 0; i < c->local_hoisted_count; i++) {
                    if (c->local_hoisted[i].name.length == fd->name.length &&
                        memcmp(c->local_hoisted[i].name.start, fd->name.start, fd->name.length) == 0 &&
                        c->local_hoisted[i].arity == fd->param_count) {
                        fprintf(stderr, "Error at line %d: Function '%.*s' with %d parameter(s) is already defined in this scope.\n",
                                s->line, fd->name.length, fd->name.start, fd->param_count);
                        exit(1);
                    }
                }

                if (c->local_hoisted_count < MAX_LOCALS) {
                    c->local_hoisted[c->local_hoisted_count].name  = fd->name;
                    c->local_hoisted[c->local_hoisted_count].arity = fd->param_count;

                    // Store param_qualifiers for compile-time ref/val handling
                    if (fd->param_count > 0) {
                        c->local_hoisted[c->local_hoisted_count].param_qualifiers =
                            ALLOCATE(c->vm, uint8_t, fd->param_count);
                        for (int i = 0; i < fd->param_count; i++) {
                            c->local_hoisted[c->local_hoisted_count].param_qualifiers[i] =
                                (uint8_t)fd->params[i].qualifier;
                        }
                    } else {
                        c->local_hoisted[c->local_hoisted_count].param_qualifiers = NULL;
                    }

                    // Initialize upvalue_count to -1 (will be set when function body is compiled)
                    c->local_hoisted[c->local_hoisted_count].upvalue_count = -1;

                    c->local_hoisted_count++;
                }
                // Don't recurse into nested function bodies - those will be collected
                // when the nested function itself is compiled
                break;
        }
        case STMT_VAR_DECLARATION: {
                VarDeclStmt* var_stmt = &s->as.var_declaration;
                for (int i = 0; i < var_stmt->count; i++) {
                    if (var_stmt->variables[i].initializer && var_stmt->variables[i].initializer->type == EXPR_FUNCTION) {
                        // Function in initializer - will be collected when scanned at block level
                    }
                }
                break;
        }
        case STMT_BLOCK: {
                BlockStmt* b = &s->as.block;
                for (int i = 0; i < b->count; i++) {
                    collect_local_hoisted_in_stmt(c, b->statements[i]);
                }
                break;
        }
        case STMT_IF: {
                collect_local_hoisted_in_stmt(c, s->as.if_stmt.then_branch);
                collect_local_hoisted_in_stmt(c, s->as.if_stmt.else_branch);
                break;
        }
        case STMT_WHILE: {
                collect_local_hoisted_in_stmt(c, s->as.while_stmt.body);
                break;
        }
        case STMT_DO_WHILE: {
                collect_local_hoisted_in_stmt(c, s->as.do_while_stmt.body);
                break;
        }
        case STMT_FOR: {
                // Initializer can be a statement
                collect_local_hoisted_in_stmt(c, s->as.for_stmt.initializer);
                collect_local_hoisted_in_stmt(c, s->as.for_stmt.body);
                // increment/condition are expressions; nothing to collect there.
                break;
        }
        default: break;
    }
}

static ObjFunction* compile_function_body(Compiler* current_compiler, FuncDeclStmt* stmt) {
    Compiler fn_compiler = {0};  // Zero-initialize to prevent garbage values during GC
    init_compiler(&fn_compiler, current_compiler->vm, current_compiler);

    // Create a new function object for the body we are about to compile.
    ObjFunction* function = newFunction(fn_compiler.vm);

    // Assign function to compiler BEFORE registering with VM
    // This ensures the function is marked if GC triggers
    fn_compiler.function = function;
    fn_compiler.compiling_chunk = function->chunk;

    // Now register this compiler with VM so GC can find it
    current_compiler->vm->compiler = &fn_compiler;

    // Now safe to modify the function (it's protected via compiler chain)
    function->name = copyString(fn_compiler.vm, stmt->name.start, stmt->name.length);
    function->arity = stmt->param_count;

    // Allocate and store parameter qualifiers, and compute qualifier signature
    if (stmt->param_count > 0) {
        function->param_qualifiers = ALLOCATE(fn_compiler.vm, uint8_t, stmt->param_count);
        bool has_non_normal = false;
        for (int i = 0; i < stmt->param_count; i++) {
            function->param_qualifiers[i] = (uint8_t)stmt->params[i].qualifier;
            if (stmt->params[i].qualifier != PARAM_NORMAL) {
                has_non_normal = true;
            }
        }
        // Set qualifier signature for call fast-path optimization
        function->qualifier_sig = has_non_normal ? QUAL_SIG_HAS_QUALIFIERS : QUAL_SIG_ALL_NORMAL;
    } else {
        // No parameters - fastest path, nothing to process
        function->qualifier_sig = QUAL_SIG_ALL_NORMAL_NO_REFS;
    }

    if (stmt->name.length > 9 && memcmp(stmt->name.start, "__module_", 9) == 0) {
        // Case 1: We are compiling a Module Factory.
        // Decode encoded path: "__module_src_slash_math_dot_zym" -> "src/math.zym"
        char* decoded_path = decodeModulePath(stmt->name.start + 9, stmt->name.length - 9);
        fn_compiler.current_module_name = copyString(fn_compiler.vm, decoded_path, strlen(decoded_path));
        free(decoded_path);
    }
    else if (current_compiler->current_module_name != NULL) {
        // Case 2: We are inside a module (e.g. 'sum' inside 'array_utils'). Inherit it.
        fn_compiler.current_module_name = current_compiler->current_module_name;
    }
    else {
        // Case 3: Top level or unknown
        fn_compiler.current_module_name = copyString(fn_compiler.vm, "script", 6);
    }

    // Tag the function object so the VM can see it later
    function->module_name = fn_compiler.current_module_name;

    begin_scope(&fn_compiler);

    // Reserve register R0 for the function itself, using the function's actual name
    // so that it can reference itself for recursion.
    Local* local = &fn_compiler.locals[fn_compiler.local_count++];
    local->name = stmt->name; // Use the actual function name, not empty string
    local->name.length = stmt->name.length;
    local->depth = fn_compiler.scope_depth;
    local->is_initialized = true;
    local->is_reference = false;
    local->is_ref_param = false;
    local->is_slot_param = false;
    local->ref_target_reg = -1;
    reserve_register(&fn_compiler); // Consumes R0

    // Compile parameters, which will now start at R1.
    for (int i = 0; i < stmt->param_count; i++) {
        declare_variable(&fn_compiler, &stmt->params[i].name);
        int reg = reserve_register(&fn_compiler);
        add_local_at_reg(&fn_compiler, stmt->params[i].name, reg);

        // Mark ref parameters as references in the locals array
        if (stmt->params[i].qualifier == PARAM_REF) {
            for (int j = 0; j < fn_compiler.local_count; j++) {
                if (fn_compiler.locals[j].reg == reg) {
                    fn_compiler.locals[j].is_reference = true;
                    fn_compiler.locals[j].is_ref_param = true;  // Ref params auto-dereference on read
                    fn_compiler.locals[j].ref_target_reg = -1; // Will be set at runtime
                    break;
                }
            }
        }
        // Mark slot parameters as references that DON'T auto-dereference on read
        // Slot params work like ref params but preserve the reference object on read
        else if (stmt->params[i].qualifier == PARAM_SLOT) {
            for (int j = 0; j < fn_compiler.local_count; j++) {
                if (fn_compiler.locals[j].reg == reg) {
                    fn_compiler.locals[j].is_reference = true;     // Holds a reference object
                    fn_compiler.locals[j].is_slot_param = true;    // But doesn't auto-dereference on read
                    fn_compiler.locals[j].ref_target_reg = -1;     // Will be set at runtime
                    break;
                }
            }
        }
    }

    // --- Multi-Pass Compilation for the function body ---
    BlockStmt* body = &stmt->body->as.block;

    // Pass 0: Recursively scan for locally declared functions to populate the local hoist registry.
    for (int i = 0; i < body->count; i++) {
        collect_local_hoisted_in_stmt(&fn_compiler, body->statements[i]);
    }

    // Pass 0.5: Pre-declare all function declarations to make them visible for calls.
    // This allows overloaded functions in the same scope to call each other.
    for (int i = 0; i < body->count; i++) {
        if (body->statements[i]->type == STMT_FUNC_DECLARATION) {
            FuncDeclStmt* func_stmt = &body->statements[i]->as.func_declaration;

            // Declare the mangled name
            char* mangled_chars = mangle_name(&fn_compiler, &func_stmt->name, func_stmt->param_count);
            Token mangled_token = {
                .start = mangled_chars,
                .length = (int)strlen(mangled_chars),
                .line = func_stmt->name.line
            };

            declare_variable(&fn_compiler, &mangled_token);
            int name_ident = add_local(&fn_compiler, mangled_token);

            // Mark as initialized so it can be referenced
            fn_compiler.locals[fn_compiler.local_count - 1].is_initialized = true;

            // Track the mangled name for cleanup
            track_owned_name(&fn_compiler, mangled_chars);
        }
    }

    // Pass 1: Declare variables WITHOUT evaluating initializers.
    // This reserves register slots for all local variables so they can be captured by closures.
    for (int i = 0; i < body->count; i++) {
        if (body->statements[i]->type == STMT_VAR_DECLARATION) {
            VarDeclStmt* var_stmt = &body->statements[i]->as.var_declaration;
            for (int j = 0; j < var_stmt->count; j++) {
                VarDecl* var = &var_stmt->variables[j];
                declare_variable(&fn_compiler, &var->name);
                int value_reg = reserve_register(&fn_compiler);
                // Initialize to null for now; actual initializer will be evaluated later
                int null_const = make_constant(&fn_compiler, NULL_VAL);
                emit_instruction(&fn_compiler, PACK_ABx(LOAD_CONST, value_reg, null_const), body->statements[i]->line);
                add_local_at_reg(&fn_compiler, var->name, value_reg);
            }
        }
    }

    // Pass 2: Process directives and compile function declarations in source order.
    // This ensures directives affect functions that come after them (scope-aware hoisting).
    for (int i = 0; i < body->count; i++) {
        if (body->statements[i]->type == STMT_COMPILER_DIRECTIVE) {
            compile_statement(&fn_compiler, body->statements[i]);
        } else if (body->statements[i]->type == STMT_FUNC_DECLARATION) {
            compile_statement(&fn_compiler, body->statements[i]);
        }
    }

    // Pass 3: Compile all other statements in order.
    // Variable declarations with their initializers, directives, and other executable
    // statements are processed here in sequence.
    for (int i = 0; i < body->count; i++) {
        Stmt* s = body->statements[i];

        // Skip function declarations - already handled in Pass 2
        if (s->type == STMT_FUNC_DECLARATION) {
            continue;
        }

        // Mark the last non-function statement as being in tail position for TCO
        bool is_last_stmt = true;
        for (int j = i + 1; j < body->count; j++) {
            if (body->statements[j]->type != STMT_FUNC_DECLARATION) {
                is_last_stmt = false;
                break;
            }
        }
        if (is_last_stmt) {
            fn_compiler.in_tail_position = true;
        }

        if (s->type == STMT_VAR_DECLARATION) {
            VarDeclStmt* var_stmt = &s->as.var_declaration;
            for (int j = 0; j < var_stmt->count; j++) {
                VarDecl* var = &var_stmt->variables[j];

                // For ref variables, we need special handling
                if (var->qualifier == VAR_REF) {
                    if (!var->initializer) {
                        printf("Error: ref variable must have an initializer.\n");
                        continue;
                    }

                    int var_reg = resolve_local(&fn_compiler, &var->name);
                    if (var_reg == -1) continue; // Should not happen

                    // Check if initializer is a simple variable reference
                    if (var->initializer->type == EXPR_VARIABLE) {
                        // Optimize for simple variable reference
                        Token target_name = var->initializer->as.variable.name;
                        int target_reg = resolve_local(&fn_compiler, &target_name);

                        if (target_reg != -1) {
                            // Target is a local variable - flatten reference chains
                            int ultimate_target = resolve_ref_target(&fn_compiler, target_reg);

                            // If ultimate_target is -1, the target is a local holding a global reference
                            // In this case, don't flatten at compile time - let runtime handle it
                            if (ultimate_target == -1) {
                                ultimate_target = target_reg;
                            }

                            emit_instruction(&fn_compiler, PACK_ABC(MAKE_REF, var_reg, ultimate_target, 0), s->line);

                            // Mark as reference to local
                            for (int k = 0; k < fn_compiler.local_count; k++) {
                                if (fn_compiler.locals[k].reg == var_reg) {
                                    fn_compiler.locals[k].is_initialized = true;
                                    fn_compiler.locals[k].is_reference = true;
                                    // If target is a local holding a global ref, we also store -1 to indicate runtime flattening needed
                                    fn_compiler.locals[k].ref_target_reg = (ultimate_target == target_reg) ? -1 : ultimate_target;
                                    break;
                                }
                            }
                        } else if ((target_reg = resolve_upvalue(&fn_compiler, &target_name)) != -1) {
                            // Target is an upvalue
                            emit_instruction(&fn_compiler, PACK_ABx(MAKE_UPVALUE_REF, var_reg, target_reg), s->line);

                            // Mark as reference to upvalue
                            for (int k = 0; k < fn_compiler.local_count; k++) {
                                if (fn_compiler.locals[k].reg == var_reg) {
                                    fn_compiler.locals[k].is_reference = true;
                                    fn_compiler.locals[k].ref_target_reg = -1; // Upvalue refs don't have compile-time target
                                    break;
                                }
                            }
                        } else {
                            // Target is not a local or upvalue, must be a global
                            // Check if this is an overloaded global function
                            int arity = single_hoisted_arity(&fn_compiler, &target_name);
                            if (arity == -2) {
                                fprintf(stderr, "Error at line %d: Cannot create reference to overloaded function '%.*s'. Store the function in a variable first, then create a reference to that variable.\n",
                                        s->line, target_name.length, target_name.start);
                                exit(1);
                            }

                            int target_name_const = resolve_ref_target_name(&fn_compiler, &target_name);
                            emit_instruction(&fn_compiler, PACK_ABx(MAKE_GLOBAL_REF, var_reg, target_name_const), s->line);

                            // Mark as reference to global
                            for (int k = 0; k < fn_compiler.local_count; k++) {
                                if (fn_compiler.locals[k].reg == var_reg) {
                                    fn_compiler.locals[k].is_reference = true;
                                    fn_compiler.locals[k].ref_target_reg = -1;
                                    break;
                                }
                            }
                        }
                    } else if (var->initializer->type == EXPR_SUBSCRIPT) {
                        // Reference to array[index] - handle in Pass 2
                        int var_reg = resolve_local(&fn_compiler, &var->name);
                        if (var_reg != -1) {
                            SubscriptExpr* subscript = &var->initializer->as.subscript;
                            int obj_reg = alloc_temp(&fn_compiler);
                            int index_reg = alloc_temp(&fn_compiler);

                            compile_expression(&fn_compiler, subscript->object, obj_reg);
                            compile_expression(&fn_compiler, subscript->index, index_reg);

                            emit_instruction(&fn_compiler, PACK_ABC(MAKE_INDEX_REF, var_reg, obj_reg, index_reg), s->line);

                            // Mark as reference
                            for (int k = 0; k < fn_compiler.local_count; k++) {
                                if (fn_compiler.locals[k].reg == var_reg) {
                                    fn_compiler.locals[k].is_reference = true;
                                    fn_compiler.locals[k].ref_target_reg = -1;
                                    break;
                                }
                            }
                        }
                    } else if (var->initializer->type == EXPR_GET) {
                        // Reference to obj.property
                        int var_reg = resolve_local(&fn_compiler, &var->name);
                        if (var_reg != -1) {
                            GetExpr* get_expr = &var->initializer->as.get;
                            int obj_reg = alloc_temp(&fn_compiler);
                            int key_reg = alloc_temp(&fn_compiler);

                            compile_expression(&fn_compiler, get_expr->object, obj_reg);

                            // Make a string constant for the key
                            ObjString* key_string = copyString(fn_compiler.vm, get_expr->name.start, get_expr->name.length);
                            pushTempRoot(fn_compiler.vm, (Obj*)key_string);
                            int key_const = make_constant(&fn_compiler, OBJ_VAL(key_string));
                            popTempRoot(fn_compiler.vm);
                            emit_instruction(&fn_compiler, PACK_ABx(LOAD_CONST, key_reg, key_const), s->line);

                            emit_instruction(&fn_compiler, PACK_ABC(MAKE_PROPERTY_REF, var_reg, obj_reg, key_reg), s->line);

                            // Mark as reference
                            for (int k = 0; k < fn_compiler.local_count; k++) {
                                if (fn_compiler.locals[k].reg == var_reg) {
                                    fn_compiler.locals[k].is_reference = true;
                                    fn_compiler.locals[k].ref_target_reg = -1;
                                    break;
                                }
                            }
                        }
                    } else {
                        printf("Error: ref variable initializer must be a variable, subscript, or property.\n");
                        continue;
                    }
                } else if (var->qualifier == VAR_VAL) {
                    // Val variable - deep clone the initializer
                    if (var->initializer) {
                        int var_reg = resolve_local(&fn_compiler, &var->name);
                        if (var_reg != -1) {
                            int temp_reg = alloc_temp(&fn_compiler);
                            compile_expression(&fn_compiler, var->initializer, temp_reg);
                            emit_instruction(&fn_compiler, PACK_ABC(CLONE_VALUE, var_reg, temp_reg, 0), s->line);

                            // Mark the variable as initialized
                            for (int k = 0; k < fn_compiler.local_count; k++) {
                                if (fn_compiler.locals[k].reg == var_reg) {
                                    fn_compiler.locals[k].is_initialized = true;
                                    break;
                                }
                            }
                        }
                    }
                } else if (var->qualifier == VAR_CLONE) {
                    // Clone variable - deep clone with reference rewriting
                    if (var->initializer) {
                        int var_reg = resolve_local(&fn_compiler, &var->name);
                        if (var_reg != -1) {
                            int temp_reg = alloc_temp(&fn_compiler);
                            compile_expression(&fn_compiler, var->initializer, temp_reg);
                            emit_instruction(&fn_compiler, PACK_ABC(DEEP_CLONE_VALUE, var_reg, temp_reg, 0), s->line);

                            // Mark the variable as initialized
                            for (int k = 0; k < fn_compiler.local_count; k++) {
                                if (fn_compiler.locals[k].reg == var_reg) {
                                    fn_compiler.locals[k].is_initialized = true;
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    // Normal variable - just evaluate the initializer
                    if (var->initializer) {
                        int var_reg = resolve_local(&fn_compiler, &var->name);
                        if (var_reg != -1) {
                            compile_expression(&fn_compiler, var->initializer, var_reg);

                            // Mark the variable as initialized
                            for (int k = 0; k < fn_compiler.local_count; k++) {
                                if (fn_compiler.locals[k].reg == var_reg) {
                                    fn_compiler.locals[k].is_initialized = true;

                                    // If initializer is a function call, it might return a reference
                                    // Mark the variable as potentially holding a reference for proper assignment semantics
                                    if (var->initializer->type == EXPR_CALL) {
                                        fn_compiler.locals[k].is_reference = true;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } else {
            // Not a variable declaration, just compile the statement normally
            compile_statement(&fn_compiler, s);
        }

        if (fn_compiler.scope_depth == 1) {
            fn_compiler.next_register = fn_compiler.local_count;
            fn_compiler.temp_free_top = 0;
        }
    }

    if (fn_compiler.compiling_chunk->count == 0 || OPCODE(fn_compiler.compiling_chunk->code[fn_compiler.compiling_chunk->count - 1]) != RET)
        emit_instruction(&fn_compiler, PACK_ABx(RET, 0, 1), stmt->body->line);

    // Validate all gotos have been resolved
    for (int i = 0; i < fn_compiler.pending_goto_count; i++) {
        PendingGoto* pending = &fn_compiler.pending_gotos[i];
        if (!pending->is_resolved) {
            Token* target = &pending->target_label;
            compiler_error(&fn_compiler, target->line, "goto to undefined label '%.*s'", target->length, target->start);
        }
    }

    // Calculate max_regs: highest register used + 1
    fn_compiler.function->max_regs = fn_compiler.max_register_seen + 1;

    fn_compiler.function->upvalue_count = fn_compiler.upvalue_count;
    memcpy(fn_compiler.function->upvalues, fn_compiler.upvalues, sizeof(Upvalue) * fn_compiler.upvalue_count);

    free_owned_names(&fn_compiler);

    // Clean up pending gotos array
    if (fn_compiler.pending_gotos) {
        FREE_ARRAY(fn_compiler.vm, PendingGoto, fn_compiler.pending_gotos, fn_compiler.pending_goto_capacity);
    }

    // Protect the function with temp_roots before restoring parent compiler
    // The function is no longer reachable via compiler chain after we restore vm->compiler
    // The caller must pop this after adding the function to constants
    pushTempRoot(current_compiler->vm, (Obj*)fn_compiler.function);

    // Propagate error flag from nested compiler to parent
    if (fn_compiler.has_error) {
        current_compiler->has_error = true;
    }

    current_compiler->vm->compiler = current_compiler;
    return fn_compiler.function;
}

bool compile(VM* vm, const char* source, Chunk* chunk, const LineMap* line_map, const char* entry_file, CompilerConfig config) {
    AstResult ast = parse(vm, source, line_map, entry_file);
    if (ast.statements == NULL) return false;

    // Use init_compiler to set up the top-level compiler correctly.
    Compiler compiler = {0};  // Zero-initialize to prevent garbage values during GC
    init_compiler(&compiler, vm, NULL); // The top-level script has no enclosing compiler.

    // Register this compiler with the VM so GC can mark compiler roots
    vm->compiler = &compiler;

    // The main script is compiled into its own implicit function.
    compiler.function = newFunction(vm);
    compiler.function->name = copyString(vm, "<script>", 8);

    // Set the entry file as the module_name for the script function
    if (entry_file) {
        compiler.function->module_name = copyString(vm, entry_file, strlen(entry_file));
        compiler.current_module_name = compiler.function->module_name;
        // Also store in VM for runtime errors when no frames exist
        vm->entry_file = compiler.function->module_name;
    }
    compiler.compiling_chunk = compiler.function->chunk;
    // ---------------

    // --- PASS 1: DECLARATION ---
    // Find all function, struct, and enum declarations first to allow for hoisting.
    for (int i = 0; ast.statements[i] != NULL; i++) {
        if (ast.statements[i]->type == STMT_FUNC_DECLARATION) {
            declare_function(&compiler, ast.statements[i]);
        } else if (ast.statements[i]->type == STMT_STRUCT_DECLARATION) {
            // Register struct schemas early so they're available for type checking
            compile_statement(&compiler, ast.statements[i]);
            if (compiler.has_error) goto cleanup_on_error;
        } else if (ast.statements[i]->type == STMT_ENUM_DECLARATION) {
            // Register enum schemas early so they're available for type checking
            compile_statement(&compiler, ast.statements[i]);
            if (compiler.has_error) goto cleanup_on_error;
        }
    }

    // --- PASS 2: CODE GENERATION ---
    // Pass 2a: Compile function definitions and process directives in source order.
    // This ensures directives affect functions that come after them.
    for (int i = 0; ast.statements[i] != NULL; i++) {
        if (ast.statements[i]->type == STMT_COMPILER_DIRECTIVE) {
            // Process directive immediately to affect subsequent functions
            CompilerDirectiveStmt* dir = &ast.statements[i]->as.compiler_directive;
            if (dir->type == DIRECTIVE_TCO) {
                Token arg = dir->argument;
                if (arg.length == 10 && memcmp(arg.start, "aggressive", 10) == 0) {
                    compiler.tco_mode = TCO_AGGRESSIVE;
                } else if (arg.length == 5 && memcmp(arg.start, "smart", 5) == 0) {
                    compiler.tco_mode = TCO_SMART;
                } else if (arg.length == 4 && memcmp(arg.start, "safe", 4) == 0) {
                    compiler.tco_mode = TCO_SAFE;
                } else if (arg.length == 3 && memcmp(arg.start, "off", 3) == 0) {
                    compiler.tco_mode = TCO_OFF;
                }
            }
        } else if (ast.statements[i]->type == STMT_FUNC_DECLARATION) {
            compile_statement(&compiler, ast.statements[i]);
            if (compiler.has_error) goto cleanup_on_error;

            // Reset register allocator for next statement at top level
            if (compiler.scope_depth == 0) {
                compiler.next_register = 0;
                compiler.temp_free_top = 0;
            }
        }
    }

    // Pass 2b: Compile all other executable statements (including blocks with nested functions).
    // By the time this code runs, all top-level hoisted functions and struct schemas will be available.
    // Functions inside blocks are compiled here when their containing block is compiled,
    // respecting scope-level directives.
    int last_line = 0;
    for (int i = 0; ast.statements[i] != NULL; i++) {
        if (ast.statements[i]->type != STMT_FUNC_DECLARATION &&
            ast.statements[i]->type != STMT_COMPILER_DIRECTIVE &&
            ast.statements[i]->type != STMT_STRUCT_DECLARATION &&
            ast.statements[i]->type != STMT_ENUM_DECLARATION) {
            compile_statement(&compiler, ast.statements[i]);
            if (compiler.has_error) goto cleanup_on_error;

            if (ast.statements[i]->line > 0) { // Ensure we don't use line 0 from synthetic stmts
                last_line = ast.statements[i]->line;
            }

            // Reset register allocator for next statement at top level (scope_depth == 0)
            // This prevents register numbers from growing beyond the 7-bit limit (127)
            if (compiler.scope_depth == 0) {
                compiler.next_register = 0;
                compiler.temp_free_top = 0;  // Clear the temp free list
            }
        }
    }

    // End the main script with an implicit return unless it already ends with RET.
    if (compiler.compiling_chunk->count == 0 || ((compiler.compiling_chunk->code[compiler.compiling_chunk->count - 1] & 0xFF) != RET)) {
        emit_instruction(&compiler, PACK_ABx(RET, 0, 1), last_line);
    }

    // Validate all gotos have been resolved in the main script
    for (int i = 0; i < compiler.pending_goto_count; i++) {
        PendingGoto* pending = &compiler.pending_gotos[i];
        if (!pending->is_resolved) {
            Token* target = &pending->target_label;
            compiler_error(&compiler, target->line, "goto to undefined label '%.*s'", target->length, target->start);
        }
    }

cleanup_on_error:
    // Clean up pending gotos array
    if (compiler.pending_gotos) {
        FREE_ARRAY(vm, PendingGoto, compiler.pending_gotos, compiler.pending_goto_capacity);
    }

    // Check if any errors occurred during compilation
    bool success = !compiler.has_error;

    // Report compilation status
    if (!success) {
        fprintf(stderr, "\nCompilation failed with errors.\n");
    }

    // NOTE: compiler.function is managed by the GC (it's in vm->objects list)
    // We don't manually free it here - the GC will handle cleanup
    // Manually freeing it would cause a double-free during freeVM()

    // Free the AST
    for (int i = 0; ast.statements[i] != NULL; i++) free_stmt(vm, ast.statements[i]);
    FREE_ARRAY(vm, Stmt*, ast.statements, ast.capacity);

    // Deep copy the compiled chunk to the external chunk parameter
    // We compiled into compiler.function->chunk, but caller expects results in chunk parameter
    if (success) {
        chunk->count = compiler.function->chunk->count;
        chunk->capacity = compiler.function->chunk->capacity;
        chunk->code = compiler.function->chunk->code;
        chunk->lines = compiler.function->chunk->lines;
        chunk->constants = compiler.function->chunk->constants;

        // Mark the function's chunk as "don't free" by NULLing the pointers
        // This prevents double-free when the function is eventually freed by GC
        compiler.function->chunk->code = NULL;
        compiler.function->chunk->lines = NULL;
        compiler.function->chunk->constants.values = NULL;
        compiler.function->chunk->count = 0;
        compiler.function->chunk->capacity = 0;
        compiler.function->chunk->constants.count = 0;
        compiler.function->chunk->constants.capacity = 0;

        // Set vm->chunk to point to the external chunk so GC will mark its constants
        // This keeps the compiled functions alive until the VM is freed
        vm->chunk = chunk;
    }

    // Unregister the compiler from the VM now that compilation is complete
    vm->compiler = NULL;

    return success;
}