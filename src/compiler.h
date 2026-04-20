#pragma once

#include "./parser.h"
#include "./parse_tree.h"
#include "./chunk.h"
#include "./sourcemap.h"
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
    TCO_AGGRESSIVE  // Optimize any return <call-expr> in tail position
} TcoMode;


typedef struct {
    Token name;
    int depth;
    int reg;
    bool is_initialized;
    bool is_captured;       // true if this local is captured by an inner closure as an upvalue
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
    int upvalue_count;
    bool is_variadic;
} HoistedFn;

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

typedef struct {
    int bytecode_pos;
    Token name;
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

    Upvalue* upvalues;
    int upvalue_count;
    int upvalue_capacity;

    HoistedFn hoisted[MAX_HOISTED];
    int hoisted_count;

    HoistedFn local_hoisted[MAX_LOCALS];
    int local_hoisted_count;

    char** owned_names;
    int    owned_names_count;
    int    owned_names_cap;

    StructSchema struct_schemas[MAX_LOCALS];
    int struct_schema_count;

    EnumSchema enum_schemas[MAX_LOCALS];
    int enum_schema_count;

    GlobalType* global_types;
    int global_type_count;
    int global_type_capacity;

    TcoMode tco_mode;
    bool in_tail_position;
    bool result_needed;

    Label labels[MAX_LABELS];
    int label_count;

    PendingGoto* pending_gotos;
    int pending_goto_count;
    int pending_goto_capacity;

    GlobalDecl global_decls[MAX_GLOBAL_DECLS];
    int global_decl_count;

    ObjString* current_module_name;
} Compiler;

// Compiles `source` into `chunk`. `source_map`, when non-NULL, is the
// per-expanded-line origin table produced by `preprocess()`; the scanner
// uses it to resolve each token's mapped line number and origin{FileId,
// StartByte,Length} fields. Pass `NULL` only when scanning a raw
// unpreprocessed buffer (tests, debugging).
//
// `out_tree` is the Phase 2 retained-parse-tree out-parameter:
//   - Pass NULL to keep today's behavior: the AST is walked for
//     codegen and freed at the end of compile().
//   - When ZYM_HAS_PARSE_TREE_RETENTION is 1 and `out_tree` is
//     non-NULL, the AST is handed off into a heap-allocated
//     ZymParseTree that the caller must release via parse_tree_free()
//     (public: zym_freeParseTree). On failure *out_tree is set to NULL.
//   - When ZYM_HAS_PARSE_TREE_RETENTION is 0, `out_tree` is ignored
//     (the parameter still exists for ABI uniformity but the AST is
//     always freed at the end of compile()).
bool compile(VM* vm, const char* source, Chunk* chunk,
             const SourceMap* source_map,
             const char* entry_file, CompilerConfig config,
             ZymParseTree** out_tree);

#if ZYM_HAS_PARSE_TREE_RETENTION
// Phase 3 — Parse-only compile mode.
//
// Runs the same sfr_reset + sfr_register + trivia allocation + parse()
// steps as compile(), then stops. On success `*out_tree` receives the
// retained ZymParseTree (caller owns it, must release via
// parse_tree_free / zym_freeParseTree); no Chunk is touched, no
// codegen runs. Returns false on parse failure (diagnostics pushed to
// the VM's sink; *out_tree left NULL).
//
// Internal-only; the public entry point is zym_parseOnly() in zym.h.
bool parseOnly(VM* vm, const char* source,
               const SourceMap* source_map,
               const char* entry_file,
               ZymParseTree** out_tree);
#endif

#if ZYM_HAS_SYMBOL_TABLE
// Phase 4 — check compile mode.
//
// Runs the full parseOnly() prologue (sfr_reset + sfr_register +
// trivia alloc + parse) and, on a clean parse, invokes the parallel
// resolver over the retained tree to produce a ZymSymbolTable. Both
// artifacts are handed to the caller on success:
//   *out_tree  — owns the AST + trivia (caller: parse_tree_free)
//   *out_table — owns the symbol array    (caller: symbol_table_free)
// On parse failure / cancellation both out-params are set to NULL and
// any partial artifacts are freed.
//
// Internal-only; the public entry point is zym_check() in zym.h.
struct ZymSymbolTable;
bool checkCompile(VM* vm, const char* source,
                  const SourceMap* source_map,
                  const char* entry_file,
                  ZymParseTree** out_tree,
                  struct ZymSymbolTable** out_table);
#endif