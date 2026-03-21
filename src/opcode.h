#pragma once

typedef enum {
    // Memory and Constants
    MOVE,
    LOAD_CONST,

    // Arithmetic (Ra = Rb op Rc)
    ADD,
    SUB,
    MUL,
    DIV,
    MOD,

    // Arithmetic with immediate (Ra = Rb op imm15)
    ADD_I,
    SUB_I,
    MUL_I,
    DIV_I,
    MOD_I,

    // Arithmetic 3-register with 64-bit literal (Ra = Rb op lit64)
    // ABC format + 2 words for literal
    ADD_L,  // Ra = Rb + lit64
    SUB_L,  // Ra = Rb - lit64
    MUL_L,  // Ra = Rb * lit64
    DIV_L,  // Ra = Rb / lit64
    MOD_L,  // Ra = Rb % lit64

    // Bitwise operations (Ra = Rb op Rc)
    BAND,       // Bitwise AND
    BOR,        // Bitwise OR
    BXOR,       // Bitwise XOR
    BLSHIFT,    // Bitwise left shift
    BRSHIFT_U,  // Bitwise right shift unsigned (logical)
    BRSHIFT_I,  // Bitwise right shift signed (arithmetic)

    // Bitwise with immediate (Ra = Rb op imm15)
    BAND_I,
    BOR_I,
    BXOR_I,
    BLSHIFT_I,
    BRSHIFT_U_I,
    BRSHIFT_I_I,

    // Bitwise 3-register with 64-bit literal (Ra = Rb op lit64)
    // ABC format + 2 words for literal
    BAND_L,      // Ra = Rb & lit64
    BOR_L,       // Ra = Rb | lit64
    BXOR_L,      // Ra = Rb ^ lit64
    BLSHIFT_L,   // Ra = Rb << lit64
    BRSHIFT_U_L, // Ra = Rb >>> lit64 (unsigned)
    BRSHIFT_I_L, // Ra = Rb >> lit64 (signed)

    // Unary (Ra = op Rb)
    NEG,
    NOT,
    BNOT,       // Bitwise NOT (i32)

    // Comparison (Ra = Rb op Rc)
    EQ,
    GT,
    LT,
    NE,
    LE,
    GE,

    // Comparison with 16-bit immediate (Ra = Rb op imm16)
    EQ_I,
    GT_I,
    LT_I,
    NE_I,
    LE_I,
    GE_I,

    // Comparison 3-register with 64-bit literal (Ra = Rb op lit64)
    // ABC format + 2 words for literal
    EQ_L,  // Ra = (Rb == lit64)
    GT_L,  // Ra = (Rb > lit64)
    LT_L,  // Ra = (Rb < lit64)
    NE_L,  // Ra = (Rb != lit64)
    LE_L,  // Ra = (Rb <= lit64)
    GE_L,  // Ra = (Rb >= lit64)

    // Control Flow
    JUMP_IF_FALSE,
    JUMP_IF_TRUE,
    JUMP,
    CALL,
    CALL_SELF,              // Call current function (recursive call optimization)
    TAIL_CALL,
    TAIL_CALL_SELF,         // Tail call to current function (recursive TCO)
    SMART_TAIL_CALL,        // Tail call with runtime upvalue check
    SMART_TAIL_CALL_SELF,   // Smart tail call to current function
    RET,

    // Branch-Compare Opcodes (compare and jump if true)
    // Register-register: if (Ra op Rb) jump offset
    BRANCH_EQ,
    BRANCH_NE,
    BRANCH_LT,
    BRANCH_LE,
    BRANCH_GT,
    BRANCH_GE,

    // Register-immediate: if (Ra op imm16) jump offset
    BRANCH_EQ_I,
    BRANCH_NE_I,
    BRANCH_LT_I,
    BRANCH_LE_I,
    BRANCH_GT_I,
    BRANCH_GE_I,

    // Register-literal: if (Ra op lit64) jump offset
    BRANCH_EQ_L,
    BRANCH_NE_L,
    BRANCH_LT_L,
    BRANCH_LE_L,
    BRANCH_GT_L,
    BRANCH_GE_L,

    // Global Variable Opcodes
    DEFINE_GLOBAL,
    GET_GLOBAL,
    GET_GLOBAL_CACHED,  // Optimized GET_GLOBAL using direct slot indexing
    SET_GLOBAL,
    SET_GLOBAL_CACHED,  // Optimized SET_GLOBAL using direct slot indexing

    // Closure Opcodes
    CLOSURE,
    GET_UPVALUE,
    SET_UPVALUE,
    CLOSE_UPVALUE,
    CLOSE_FRAME_UPVALUES,  // Close all upvalues for current frame (used before TAIL_CALL)

    // List Opcodes
    NEW_LIST,
    LIST_APPEND,
    LIST_SPREAD,         // Spread list/array into another list (Ra = target list, Rb = source to spread)
    GET_SUBSCRIPT,
    GET_SUBSCRIPT_I,     // Ra = container[Rb][imm8] - immediate index, optimized for lists
    SET_SUBSCRIPT,
    SET_SUBSCRIPT_I,     // container[Ra][imm8] = Rc - immediate index variant

    // Map Opcodes
    NEW_MAP,
    MAP_SET,
    MAP_SPREAD,          // Spread map into another map (Ra = target map, Rb = source to spread)
    GET_MAP_PROPERTY,
    SET_MAP_PROPERTY,

    // Dispatcher Opcodes (for overloaded function returns)
    NEW_DISPATCHER,
    ADD_OVERLOAD,


    // Struct Opcodes
    NEW_STRUCT,            // Ra = new struct instance, Bx = schema constant index
    STRUCT_SPREAD,         // Spread struct fields into another struct (Ra = target struct, Rb = source to spread)
    GET_STRUCT_FIELD,      // Ra = struct[Rb].field[C] where C is field index
    SET_STRUCT_FIELD,      // struct[Ra].field[B] = Rc

    // Increment/Decrement Opcodes
    PRE_INC,       // Ra = ++stack[Rb] (increment then return new value)
    POST_INC,      // Ra = stack[Rb]++ (return old value then increment)
    PRE_DEC,       // Ra = --stack[Rb] (decrement then return new value)
    POST_DEC,      // Ra = stack[Rb]-- (return old value then decrement)

} OpCode;