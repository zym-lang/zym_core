#include <stdio.h>
#include <string.h>
#include "./debug.h"
#include "./value.h"
#include "./object.h"

#define OPCODE(i) ((i) & 0xFF)
#define REG_A(i)  (((i) >> 8) & 0xFF)
#define REG_B(i)  (((i) >> 16) & 0xFF)
#define REG_C(i)  (((i) >> 24) & 0xFF)
#define REG_Bx(i) ((i) >> 16)

static inline int32_t sign_extend_16(uint32_t x) {
    return (int32_t)((int32_t)(x << 16) >> 16);
}

static int simpleInstruction(const char* name, int offset) {
    printf("%-16s\n", name);
    return offset + 1;
}

static int reg_instruction_abc(const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr), b = REG_B(instr), c = REG_C(instr);
    printf("%-16s R%-2u, R%-2u, R%-2u\n", name, a, b, c);
    return offset + 1;
}

static int reg_instruction_ab(const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr), b = REG_B(instr);
    printf("%-16s R%-2u, R%-2u\n", name, a, b);
    return offset + 1;
}

static int reg_instruction_a(const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    printf("%-16s R%-2u\n", name, a);
    return offset + 1;
}

static int reg_instruction_abx(const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint16_t bx = REG_Bx(instr);
    printf("%-16s R%-2u, %u\n", name, a, bx);
    return offset + 1;
}

static int immediate_instruction(const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint16_t bx = REG_Bx(instr);
    int16_t imm = (int16_t)sign_extend_16(bx);
    printf("%-16s R%-2u, #%d\n", name, a, imm);
    return offset + 1;
}

static int literal_instruction(const char* name, Chunk* chunk, int offset) {
    uint32_t instr = chunk->code[offset];
    uint8_t a = REG_A(instr);

    if (offset + 2 >= chunk->count) {
        printf("%-16s R%-2u, <incomplete literal>\n", name, a);
        return offset + 1;
    }

    uint32_t low = chunk->code[offset + 1];
    uint32_t high = chunk->code[offset + 2];
    uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
    double literal;
    memcpy(&literal, &bits, sizeof(double));

    printf("%-16s R%-2u, #%.15g\n", name, a, literal);
    return offset + 3;
}

static int constantInstruction(const char* name, Chunk* chunk, uint32_t instr, int offset) {
    uint8_t  a  = REG_A(instr);
    uint16_t ix = (uint16_t)REG_Bx(instr);
    printf("%-16s R%-2u, %4u ", name, a, ix);
    if (ix < chunk->constants.count) {
        Value constant = chunk->constants.values[ix];
        printf("'");

        if (IS_ENUM(constant)) {
            int type_id = ENUM_TYPE_ID(constant);
            int variant_idx = ENUM_VARIANT(constant);

            ObjEnumSchema* schema = NULL;
            for (int i = 0; i < chunk->constants.count; i++) {
                Value v = chunk->constants.values[i];
                if (IS_OBJ(v) && IS_ENUM_SCHEMA(v)) {
                    ObjEnumSchema* candidate = AS_ENUM_SCHEMA(v);
                    if (candidate->type_id == type_id) {
                        schema = candidate;
                        break;
                    }
                }
            }

            if (schema != NULL && variant_idx >= 0 && variant_idx < schema->variant_count) {
                ObjString* variant_name = schema->variant_names[variant_idx];
                printf("%.*s.%.*s",
                       schema->name->length, schema->name->chars,
                       variant_name->length, variant_name->chars);
            } else {
                printValue(NULL, constant);
            }
        } else {
            printValue(NULL, constant);
        }

        printf("'");
    } else {
        printf("<const OOB>");
    }
    printf("\n");
    return offset + 1;
}

static int callInstruction(const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint16_t argc = REG_Bx(instr);
    printf("%-16s R%-2u, %4u args\n", name, a, argc);
    return offset + 1;
}

static int upvalueInstruction(const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint16_t upvalue_index = REG_Bx(instr);
    printf("%-16s R%-2u, upvalue[%u]\n", name, a, upvalue_index);
    return offset + 1;
}

static int jump_if_false_instruction(uint32_t instr, int offset) {
    uint8_t  a   = REG_A(instr);
    int32_t  off = sign_extend_16(REG_Bx(instr));
    int      tgt = offset + 1 + off;
    printf("%-16s R%-2u, off %+5d -> %04d\n", "JUMP_IF_FALSE", a, (int)off, tgt);
    return offset + 1;
}

static int jump_instruction(uint32_t instr, int offset) {
    int32_t off = sign_extend_16(REG_Bx(instr));
    int     tgt = offset + 1 + off;
    printf("%-16s off %+5d -> %04d\n", "JUMP", (int)off, tgt);
    return offset + 1;
}

static int reg3_instruction(const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint8_t b = REG_B(instr);
    int8_t c = (int8_t)REG_C(instr);
    int tgt = offset + 1 + c;
    printf("%-16s R%d, R%d, off %+d -> %04d\n", name, a, b, (int)c, tgt);
    return offset + 1;
}

static int branch_imm_instruction(const char* name, Chunk* chunk, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint16_t bx = REG_Bx(instr);
    int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
    uint32_t off_word = chunk->code[offset + 1];
    int32_t off = sign_extend_16(off_word);
    int tgt = offset + 2 + off;
    printf("%-16s R%d, #%d, off %+d -> %04d\n", name, a, (int)imm, (int)off, tgt);
    return offset + 2;
}

static int branch_lit_instruction(const char* name, Chunk* chunk, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint32_t low = chunk->code[offset + 1];
    uint32_t high = chunk->code[offset + 2];
    uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
    double literal;
    memcpy(&literal, &bits, sizeof(double));
    uint32_t off_word = chunk->code[offset + 3];
    int32_t off = sign_extend_16(off_word);
    int tgt = offset + 4 + off;
    printf("%-16s R%d, #%.17g, off %+d -> %04d\n", name, a, literal, (int)off, tgt);
    return offset + 4;
}

static int reg_bx_instruction(const char* name, Chunk* chunk, int offset) {
    uint32_t instr = chunk->code[offset];
    uint8_t a = (instr >> 8) & 0xFF;
    uint16_t bx = (instr >> 16) & 0xFFFF;
    printf("%-16s R%d, %d\n", name, a, bx);
    return offset + 1;
}

static int reg2_instruction(const char* name, Chunk* chunk, int offset) {
    uint32_t instr = chunk->code[offset];
    uint8_t a = (instr >> 8) & 0xFF;
    uint8_t b = (instr >> 16) & 0xFF;
    printf("%-16s R%d, R%d\n", name, a, b);
    return offset + 1;
}

void disassembleChunkToFile(Chunk* chunk, const char* name, FILE* file) {
    fprintf(file, "== %s ==\n", name);

    for (int offset = 0; offset < chunk->count; ) {
        offset = disassembleInstruction(chunk, offset);
    }

    for (int i = 0; i < chunk->constants.count; i++) {
        Value v = chunk->constants.values[i];
        if (IS_OBJ(v) && IS_FUNCTION(v)) {
            ObjFunction* fn = AS_FUNCTION(v);
            const char* fname = fn->name ? fn->name->chars : "<anon>";
            fprintf(file, "\n-- Function constant %d: %s/%d --\n", i, fname, fn->arity);
            if (fn->chunk && fn->chunk->count > 0) {
                disassembleChunkToFile(fn->chunk, fname, file);
            }
        }
    }
}

void disassembleChunk(Chunk* chunk, const char* name) {
    printf("== %s ==\n", name);

    for (int offset = 0; offset < chunk->count; ) {
        offset = disassembleInstruction(chunk, offset);
    }

    for (int i = 0; i < chunk->constants.count; i++) {
        Value v = chunk->constants.values[i];
        if (IS_OBJ(v) && IS_FUNCTION(v)) {
            ObjFunction* fn = AS_FUNCTION(v);
            const char* fname = fn->name ? fn->name->chars : "<anon>";
            printf("\n-- Function constant %d: %s/%d --\n", i, fname, fn->arity);
            if (fn->chunk && fn->chunk->count > 0) {
                disassembleChunk(fn->chunk, fname);
            }
        }
    }
}

int disassembleInstruction(Chunk* chunk, int offset) {
    printf("%04d ", offset);

    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("   | ");
    } else {
        printf("%4d ", chunk->lines[offset]);
    }

    uint32_t instruction = chunk->code[offset];
    uint16_t opcode      = OPCODE(instruction);

    switch (opcode) {
        case MOVE:          return reg_instruction_ab("MOVE", instruction, offset);
        case LOAD_CONST:    return constantInstruction("LOAD_CONST", chunk, instruction, offset);
        case ADD:           return reg_instruction_abc("ADD", instruction, offset);
        case SUB:           return reg_instruction_abc("SUB", instruction, offset);
        case MUL:           return reg_instruction_abc("MUL", instruction, offset);
        case DIV:           return reg_instruction_abc("DIV", instruction, offset);
        case MOD:           return reg_instruction_abc("MOD", instruction, offset);
        case ADD_I:         return immediate_instruction("ADD_I", instruction, offset);
        case SUB_I:         return immediate_instruction("SUB_I", instruction, offset);
        case MUL_I:         return immediate_instruction("MUL_I", instruction, offset);
        case DIV_I:         return immediate_instruction("DIV_I", instruction, offset);
        case MOD_I:         return immediate_instruction("MOD_I", instruction, offset);
        case ADD_L:         return literal_instruction("ADD_L", chunk, offset);
        case SUB_L:         return literal_instruction("SUB_L", chunk, offset);
        case MUL_L:         return literal_instruction("MUL_L", chunk, offset);
        case DIV_L:         return literal_instruction("DIV_L", chunk, offset);
        case MOD_L:         return literal_instruction("MOD_L", chunk, offset);
        case BAND:          return reg_instruction_abc("BAND", instruction, offset);
        case BOR:           return reg_instruction_abc("BOR", instruction, offset);
        case BXOR:          return reg_instruction_abc("BXOR", instruction, offset);
        case BLSHIFT:       return reg_instruction_abc("BLSHIFT", instruction, offset);
        case BRSHIFT_U:     return reg_instruction_abc("BRSHIFT_U", instruction, offset);
        case BRSHIFT_I:     return reg_instruction_abc("BRSHIFT_I", instruction, offset);
        case BAND_I:        return immediate_instruction("BAND_I", instruction, offset);
        case BOR_I:         return immediate_instruction("BOR_I", instruction, offset);
        case BXOR_I:        return immediate_instruction("BXOR_I", instruction, offset);
        case BLSHIFT_I:     return immediate_instruction("BLSHIFT_I", instruction, offset);
        case BRSHIFT_U_I:   return immediate_instruction("BRSHIFT_U_I", instruction, offset);
        case BRSHIFT_I_I:   return immediate_instruction("BRSHIFT_I_I", instruction, offset);
        case BAND_L:        return literal_instruction("BAND_L", chunk, offset);
        case BOR_L:         return literal_instruction("BOR_L", chunk, offset);
        case BXOR_L:        return literal_instruction("BXOR_L", chunk, offset);
        case BLSHIFT_L:     return literal_instruction("BLSHIFT_L", chunk, offset);
        case BRSHIFT_U_L:   return literal_instruction("BRSHIFT_U_L", chunk, offset);
        case BRSHIFT_I_L:   return literal_instruction("BRSHIFT_I_L", chunk, offset);
        case EQ:            return reg_instruction_abc("EQ",  instruction, offset);
        case GT:            return reg_instruction_abc("GT",  instruction, offset);
        case LT:            return reg_instruction_abc("LT",  instruction, offset);
        case NE:            return reg_instruction_abc("NE",  instruction, offset);
        case LE:            return reg_instruction_abc("LE",  instruction, offset);
        case GE:            return reg_instruction_abc("GE",  instruction, offset);
        case EQ_I:          return immediate_instruction("EQ_I", instruction, offset);
        case GT_I:          return immediate_instruction("GT_I", instruction, offset);
        case LT_I:          return immediate_instruction("LT_I", instruction, offset);
        case NE_I:          return immediate_instruction("NE_I", instruction, offset);
        case LE_I:          return immediate_instruction("LE_I", instruction, offset);
        case GE_I:          return immediate_instruction("GE_I", instruction, offset);
        case EQ_L:          return literal_instruction("EQ_L", chunk, offset);
        case GT_L:          return literal_instruction("GT_L", chunk, offset);
        case LT_L:          return literal_instruction("LT_L", chunk, offset);
        case NE_L:          return literal_instruction("NE_L", chunk, offset);
        case LE_L:          return literal_instruction("LE_L", chunk, offset);
        case GE_L:          return literal_instruction("GE_L", chunk, offset);
        case NEG:           return reg_instruction_ab("NEG", instruction, offset);
        case NOT:           return reg_instruction_ab("NOT", instruction, offset);
        case BNOT:          return reg_instruction_ab("BNOT", instruction, offset);
        case JUMP_IF_FALSE: return jump_if_false_instruction(instruction, offset);
        case JUMP:          return jump_instruction(instruction, offset);
        case BRANCH_EQ:     return reg3_instruction("BRANCH_EQ", instruction, offset);
        case BRANCH_NE:     return reg3_instruction("BRANCH_NE", instruction, offset);
        case BRANCH_LT:     return reg3_instruction("BRANCH_LT", instruction, offset);
        case BRANCH_LE:     return reg3_instruction("BRANCH_LE", instruction, offset);
        case BRANCH_GT:     return reg3_instruction("BRANCH_GT", instruction, offset);
        case BRANCH_GE:     return reg3_instruction("BRANCH_GE", instruction, offset);
        case BRANCH_EQ_I:   return branch_imm_instruction("BRANCH_EQ_I", chunk, instruction, offset);
        case BRANCH_NE_I:   return branch_imm_instruction("BRANCH_NE_I", chunk, instruction, offset);
        case BRANCH_LT_I:   return branch_imm_instruction("BRANCH_LT_I", chunk, instruction, offset);
        case BRANCH_LE_I:   return branch_imm_instruction("BRANCH_LE_I", chunk, instruction, offset);
        case BRANCH_GT_I:   return branch_imm_instruction("BRANCH_GT_I", chunk, instruction, offset);
        case BRANCH_GE_I:   return branch_imm_instruction("BRANCH_GE_I", chunk, instruction, offset);
        case BRANCH_EQ_L:   return branch_lit_instruction("BRANCH_EQ_L", chunk, instruction, offset);
        case BRANCH_NE_L:   return branch_lit_instruction("BRANCH_NE_L", chunk, instruction, offset);
        case BRANCH_LT_L:   return branch_lit_instruction("BRANCH_LT_L", chunk, instruction, offset);
        case BRANCH_LE_L:   return branch_lit_instruction("BRANCH_LE_L", chunk, instruction, offset);
        case BRANCH_GT_L:   return branch_lit_instruction("BRANCH_GT_L", chunk, instruction, offset);
        case BRANCH_GE_L:   return branch_lit_instruction("BRANCH_GE_L", chunk, instruction, offset);
        case DEFINE_GLOBAL: return constantInstruction("DEFINE_GLOBAL", chunk, instruction, offset);
        case GET_GLOBAL:    return constantInstruction("GET_GLOBAL", chunk, instruction, offset);
        case GET_GLOBAL_CACHED: return reg_instruction_abx("GET_GLOBAL_CACHED", instruction, offset);
        case SET_GLOBAL:    return constantInstruction("SET_GLOBAL", chunk, instruction, offset);
        case SET_GLOBAL_CACHED: return reg_instruction_abx("SET_GLOBAL_CACHED", instruction, offset);
        case SLOT_SET_GLOBAL: return constantInstruction("SLOT_SET_GLOBAL", chunk, instruction, offset);
        case CALL:          return callInstruction("CALL", instruction, offset);
        case CALL_SELF:     return callInstruction("CALL_SELF", instruction, offset);
        case TAIL_CALL:     return callInstruction("TAIL_CALL", instruction, offset);
        case TAIL_CALL_SELF: return callInstruction("TAIL_CALL_SELF", instruction, offset);
        case SMART_TAIL_CALL: return callInstruction("SMART_TAIL_CALL", instruction, offset);
        case SMART_TAIL_CALL_SELF: return callInstruction("SMART_TAIL_CALL_SELF", instruction, offset);
        case CLOSURE:       return constantInstruction("CLOSURE", chunk, instruction, offset);
        case GET_UPVALUE:   return upvalueInstruction("GET_UPVALUE", instruction, offset);
        case SET_UPVALUE:   return upvalueInstruction("SET_UPVALUE", instruction, offset);
        case SLOT_SET_UPVALUE: return upvalueInstruction("SLOT_SET_UPVALUE", instruction, offset);
        case CLOSE_UPVALUE: return reg_instruction_a("CLOSE_UPVALUE", instruction, offset);
        case CLOSE_FRAME_UPVALUES: return simpleInstruction("CLOSE_FRAME_UPVALUES", offset);
        case NEW_LIST:      return reg_bx_instruction("NEW_LIST", chunk, offset);
        case LIST_APPEND:   return reg2_instruction("LIST_APPEND", chunk, offset);
        case LIST_SPREAD:   return reg2_instruction("LIST_SPREAD", chunk, offset);
        case GET_SUBSCRIPT: return reg_instruction_abc("GET_SUBSCRIPT", instruction, offset);
        case SET_SUBSCRIPT: return reg_instruction_abc("SET_SUBSCRIPT", instruction, offset);
        case SLOT_SET_SUBSCRIPT: return reg_instruction_abc("SLOT_SET_SUBSCRIPT", instruction, offset);
        case NEW_MAP:       return reg_instruction_a("NEW_MAP", instruction, offset);
        case MAP_SET:       return reg_instruction_abc("MAP_SET", instruction, offset);
        case MAP_SPREAD:    return reg2_instruction("MAP_SPREAD", chunk, offset);
        case GET_MAP_PROPERTY: return reg_instruction_abc("GET_MAP_PROPERTY", instruction, offset);
        case SET_MAP_PROPERTY: return reg_instruction_abc("SET_MAP_PROPERTY", instruction, offset);
        case SLOT_SET_MAP_PROPERTY: return reg_instruction_abc("SLOT_SET_MAP_PROPERTY", instruction, offset);
        case NEW_DISPATCHER: return reg_instruction_a("NEW_DISPATCHER", instruction, offset);
        case ADD_OVERLOAD:   return reg2_instruction("ADD_OVERLOAD", chunk, offset);
        case CLONE_VALUE:    return reg_instruction_ab("CLONE_VALUE", instruction, offset);
        case DEEP_CLONE_VALUE: return reg_instruction_ab("DEEP_CLONE_VALUE", instruction, offset);
        case NEW_STRUCT: return constantInstruction("NEW_STRUCT", chunk, instruction, offset);
        case STRUCT_SPREAD: return reg2_instruction("STRUCT_SPREAD", chunk, offset);
        case GET_STRUCT_FIELD: return reg_instruction_abc("GET_STRUCT_FIELD", instruction, offset);
        case SET_STRUCT_FIELD: return reg_instruction_abc("SET_STRUCT_FIELD", instruction, offset);
        case SLOT_SET_STRUCT_FIELD: return reg_instruction_abc("SLOT_SET_STRUCT_FIELD", instruction, offset);
        case PRE_INC: return reg_instruction_ab("PRE_INC", instruction, offset);
        case POST_INC: return reg_instruction_ab("POST_INC", instruction, offset);
        case PRE_DEC: return reg_instruction_ab("PRE_DEC", instruction, offset);
        case POST_DEC: return reg_instruction_ab("POST_DEC", instruction, offset);
        case MAKE_REF:       return reg_instruction_ab("MAKE_REF", instruction, offset);
        case SLOT_MAKE_REF:  return reg_instruction_ab("SLOT_MAKE_REF", instruction, offset);
        case MAKE_GLOBAL_REF: return constantInstruction("MAKE_GLOBAL_REF", chunk, instruction, offset);
        case SLOT_MAKE_GLOBAL_REF: return constantInstruction("SLOT_MAKE_GLOBAL_REF", chunk, instruction, offset);
        case MAKE_UPVALUE_REF: return upvalueInstruction("MAKE_UPVALUE_REF", instruction, offset);
        case MAKE_INDEX_REF: return reg_instruction_abc("MAKE_INDEX_REF", instruction, offset);
        case SLOT_MAKE_INDEX_REF: return reg_instruction_abc("SLOT_MAKE_INDEX_REF", instruction, offset);
        case MAKE_PROPERTY_REF: return reg_instruction_abc("MAKE_PROPERTY_REF", instruction, offset);
        case SLOT_MAKE_PROPERTY_REF: return reg_instruction_abc("SLOT_MAKE_PROPERTY_REF", instruction, offset);
        case DEREF_GET:      return reg_instruction_ab("DEREF_GET", instruction, offset);
        case DEREF_SET:      return reg_instruction_ab("DEREF_SET", instruction, offset);
        case SLOT_DEREF_SET: return reg_instruction_ab("SLOT_DEREF_SET", instruction, offset);
        case TYPEOF:         return reg_instruction_ab("TYPEOF", instruction, offset);
        case PUSH_PROMPT:    return reg_instruction_a("PUSH_PROMPT", instruction, offset);
        case POP_PROMPT:     return simpleInstruction("POP_PROMPT", offset);
        case CAPTURE:        return reg_instruction_ab("CAPTURE", instruction, offset);
        case RESUME:         return reg_instruction_abc("RESUME", instruction, offset);
        case ABORT:          return reg_instruction_ab("ABORT", instruction, offset);
        case RET: {
            uint32_t instr = instruction;
            uint8_t  a  = REG_A(instr);
            uint16_t bx = REG_Bx(instr);
            if (bx == 1) {
                printf("%-16s (implicit null)\n", "RET");
            } else {
                printf("%-16s R%-2u\n", "RET", a);
            }
            return offset + 1;
        }
        default:
            printf("Unknown opcode %u\n", opcode);
            return offset + 1;
    }
}