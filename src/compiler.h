#pragma once

#include "./parser.h"
#include "./chunk.h"
#include "./linemap.h"
#include "./config.h"

typedef struct VM VM;
typedef struct ObjString ObjString;
typedef struct ObjStructSchema ObjStructSchema;
typedef struct ObjEnumSchema ObjEnumSchema;

#define MAX_LOCALS 256            // Maximum locals per function (full 8-bit register space)
#define MAX_LOOP_DEPTH 16
#define MAX_HOISTED 1024
#define MAX_LABELS 256
#define MAX_PHYSICAL_REGS 256     // 8-bit register addressing supports 256 registers

typedef enum {
    TCO_OFF,        // No tail call optimization
    TCO_SAFE,       // Only optimize pure self-recursion (no captured upvalues)
    TCO_SMART,      // Runtime check: TCO if callee has no upvalues
    TCO_AGGRESSIVE  // Optimize any return <call-expr> in tail position
} TcoMode;


typedef struct {
    Token name;
    int depth;
    int reg;
    bool is_initialized;
    bool is_reference;      // true if this local holds a reference (either param or object)
    bool is_ref_param;      // true if this is a ref parameter (auto-dereference on read)
    bool is_slot_param;     // true if this is a slot parameter (direct variable binding)
    int ref_target_reg;     // if is_reference is true, which register does it reference?
    ObjStructSchema* struct_type; // if this local holds a struct instance, this is its schema (NULL otherwise)
} Local;

typedef struct {
    uint8_t index;
    bool is_local;
    ObjStructSchema* struct_type;  // Track struct type for upvalues (NULL if not a struct)
} Upvalue;

typedef struct {
    Token name;
    int arity;
    uint8_t* param_qualifiers;  // Array of ParamQualifier values, dynamically allocated
    int upvalue_count;          // Number of upvalues this function captures (for TCO optimization)
} HoistedFn;

// Global variable type tracking (for struct type inference at compile time)
typedef struct {
    ObjString* name;
    ObjStructSchema* schema;
} GlobalType;

typedef struct {
    Token name;                 // Struct name
    ObjString** field_names;    // Interned field names
    int field_count;            // Number of fields
    int depth;                  // Scope depth (for shadowing support)
    ObjStructSchema* schema;    // The actual schema object
} StructSchema;

typedef struct {
    Token name;                 // Enum name
    ObjEnumSchema* schema;      // Runtime schema object (with type_id)
    ObjString** variant_names;  // Interned variant names
    int variant_count;          // Number of variants
    int depth;                  // Scope depth (for shadowing support)
} EnumSchema;

typedef struct {
    Token name;                 // Label identifier
    int instruction_address;    // Bytecode offset where label is defined (-1 if forward reference)
    int scope_depth;            // Scope depth at label definition
    int local_count;            // Number of locals alive at label
    bool is_resolved;           // true if label has been defined
} Label;

typedef struct {
    int jump_address;           // Address of JUMP instruction to patch
    Token target_label;         // Label name this goto jumps to
    int goto_scope_depth;       // Scope depth where goto was emitted
    int goto_local_count;       // Number of locals at goto site
    int goto_bytecode_pos;      // Bytecode position where goto was emitted
    bool is_resolved;           // true if label has been found and jump patched
} PendingGoto;

// Track global variable declarations with initializers for goto validation
typedef struct {
    int bytecode_pos;           // Bytecode position where DEFINE_GLOBAL was emitted
    Token name;                 // Variable name
} GlobalDecl;

#define MAX_GLOBAL_DECLS 256



typedef struct Compiler {
    VM* vm;
    Chunk* compiling_chunk;

    bool has_error;  // Track if any compilation errors occurred

    int next_register;
    int max_register_seen;
    int temp_free[MAX_PHYSICAL_REGS];
    int temp_free_top;

    Local locals[MAX_LOCALS];
    int local_count;
    int scope_depth;

    int* break_jumps;
    int break_count;
    int break_capacity;

    int loop_exits[MAX_LOOP_DEPTH];
    int loop_continues[MAX_LOOP_DEPTH];
    int loop_depth;

    struct Compiler* enclosing;
    ObjFunction* function;

    Upvalue upvalues[MAX_LOCALS];
    int upvalue_count;

    HoistedFn hoisted[MAX_HOISTED];
    int hoisted_count;

    HoistedFn local_hoisted[MAX_LOCALS];
    int local_hoisted_count;

    // for freeing temporary mangled names created during compilation (locals)
    char** owned_names;
    int    owned_names_count;
    int    owned_names_cap;

    // Struct schemas (supports shadowing)
    StructSchema struct_schemas[MAX_LOCALS];
    int struct_schema_count;

    // Enum schemas (supports shadowing)
    EnumSchema enum_schemas[MAX_LOCALS];
    int enum_schema_count;

    // Global variable type tracking (for struct type inference)
    GlobalType* global_types;
    int global_type_count;
    int global_type_capacity;

    TcoMode tco_mode;  // Current tail call optimization level
    bool in_tail_position;  // True if currently compiling in tail position
    bool result_needed;  // True if expression result is needed (false in statement context)

    // Label and goto tracking
    Label labels[MAX_LABELS];
    int label_count;

    PendingGoto* pending_gotos;
    int pending_goto_count;
    int pending_goto_capacity;

    // Global variable declaration tracking (for goto validation)
    GlobalDecl global_decls[MAX_GLOBAL_DECLS];
    int global_decl_count;

    ObjString* current_module_name;
} Compiler;

bool compile(VM* vm, const char* source, Chunk* chunk, const LineMap* line_map, const char* entry_file, CompilerConfig config);