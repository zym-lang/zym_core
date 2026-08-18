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

static int simpleInstruction(FILE* f, const char* name, int offset) {
    fprintf(f, "%-16s\n", name);
    return offset + 1;
}

static int reg_instruction_abc(FILE* f, const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr), b = REG_B(instr), c = REG_C(instr);
    fprintf(f, "%-16s R%-2u, R%-2u, R%-2u\n", name, a, b, c);
    return offset + 1;
}

static int reg_instruction_ab(FILE* f, const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr), b = REG_B(instr);
    fprintf(f, "%-16s R%-2u, R%-2u\n", name, a, b);
    return offset + 1;
}

static int reg_instruction_a(FILE* f, const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    fprintf(f, "%-16s R%-2u\n", name, a);
    return offset + 1;
}

static int reg_instruction_abx(FILE* f, const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint16_t bx = REG_Bx(instr);
    fprintf(f, "%-16s R%-2u, %u\n", name, a, bx);
    return offset + 1;
}

static int immediate_instruction(FILE* f, const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint16_t bx = REG_Bx(instr);
    int16_t imm = (int16_t)sign_extend_16(bx);
    fprintf(f, "%-16s R%-2u, #%d\n", name, a, imm);
    return offset + 1;
}

static int literal_instruction(FILE* f, const char* name, Chunk* chunk, int offset) {
    uint32_t instr = chunk->code[offset];
    uint8_t a = REG_A(instr);

    if (offset + 2 >= chunk->count) {
        fprintf(f, "%-16s R%-2u, <incomplete literal>\n", name, a);
        return offset + 1;
    }

    uint32_t low = chunk->code[offset + 1];
    uint32_t high = chunk->code[offset + 2];
    uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
    double literal;
    memcpy(&literal, &bits, sizeof(double));

    fprintf(f, "%-16s R%-2u, #%.15g\n", name, a, literal);
    return offset + 3;
}

static int constantInstruction(FILE* f, const char* name, Chunk* chunk, uint32_t instr, int offset) {
    uint8_t  a  = REG_A(instr);
    uint16_t ix = (uint16_t)REG_Bx(instr);
    fprintf(f, "%-16s R%-2u, %4u ", name, a, ix);
    if (ix < chunk->constants.count) {
        Value constant = chunk->constants.values[ix];
        fprintf(f, "'");

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
                fprintf(f, "%.*s.%.*s",
                        schema->name->length, schema->name->chars,
                        variant_name->length, variant_name->chars);
            } else {
                fprintValue(NULL, f, constant);
            }
        } else {
            fprintValue(NULL, f, constant);
        }

        fprintf(f, "'");
    } else {
        fprintf(f, "<const OOB>");
    }
    fprintf(f, "\n");
    return offset + 1;
}

static int callInstruction(FILE* f, const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint16_t argc = REG_Bx(instr);
    fprintf(f, "%-16s R%-2u, %4u args\n", name, a, argc);
    return offset + 1;
}

static int upvalueInstruction(FILE* f, const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint16_t upvalue_index = REG_Bx(instr);
    fprintf(f, "%-16s R%-2u, upvalue[%u]\n", name, a, upvalue_index);
    return offset + 1;
}

static int jump_conditional_instruction(FILE* f, const char* name, uint32_t instr, int offset) {
    uint8_t  a   = REG_A(instr);
    int32_t  off = sign_extend_16(REG_Bx(instr));
    int      tgt = offset + 1 + off;
    fprintf(f, "%-16s R%-2u, off %+5d -> %04d\n", name, a, (int)off, tgt);
    return offset + 1;
}

static int jump_instruction(FILE* f, uint32_t instr, int offset) {
    int32_t off = sign_extend_16(REG_Bx(instr));
    int     tgt = offset + 1 + off;
    fprintf(f, "%-16s off %+5d -> %04d\n", "JUMP", (int)off, tgt);
    return offset + 1;
}

static int reg3_instruction(FILE* f, const char* name, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint8_t b = REG_B(instr);
    int8_t c = (int8_t)REG_C(instr);
    int tgt = offset + 1 + c;
    fprintf(f, "%-16s R%d, R%d, off %+d -> %04d\n", name, a, b, (int)c, tgt);
    return offset + 1;
}

static int branch_imm_instruction(FILE* f, const char* name, Chunk* chunk, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint16_t bx = REG_Bx(instr);
    int16_t imm = (int16_t)((int32_t)(bx << 16) >> 16);
    uint32_t off_word = chunk->code[offset + 1];
    int32_t off = sign_extend_16(off_word);
    int tgt = offset + 2 + off;
    fprintf(f, "%-16s R%d, #%d, off %+d -> %04d\n", name, a, (int)imm, (int)off, tgt);
    return offset + 2;
}

static int branch_lit_instruction(FILE* f, const char* name, Chunk* chunk, uint32_t instr, int offset) {
    uint8_t a = REG_A(instr);
    uint32_t low = chunk->code[offset + 1];
    uint32_t high = chunk->code[offset + 2];
    uint64_t bits = ((uint64_t)high << 32) | (uint64_t)low;
    double literal;
    memcpy(&literal, &bits, sizeof(double));
    uint32_t off_word = chunk->code[offset + 3];
    int32_t off = sign_extend_16(off_word);
    int tgt = offset + 4 + off;
    fprintf(f, "%-16s R%d, #%.17g, off %+d -> %04d\n", name, a, literal, (int)off, tgt);
    return offset + 4;
}

static int reg_bx_instruction(FILE* f, const char* name, Chunk* chunk, int offset) {
    uint32_t instr = chunk->code[offset];
    uint8_t a = (instr >> 8) & 0xFF;
    uint16_t bx = (instr >> 16) & 0xFFFF;
    fprintf(f, "%-16s R%d, %d\n", name, a, bx);
    return offset + 1;
}

static int reg2_instruction(FILE* f, const char* name, Chunk* chunk, int offset) {
    uint32_t instr = chunk->code[offset];
    uint8_t a = (instr >> 8) & 0xFF;
    uint8_t b = (instr >> 16) & 0xFF;
    fprintf(f, "%-16s R%d, R%d\n", name, a, b);
    return offset + 1;
}

void disassembleChunkToFile(Chunk* chunk, const char* name, FILE* file) {
    fprintf(file, "== %s ==\n", name);

    for (int offset = 0; offset < chunk->count; ) {
        offset = disassembleInstructionToFile(chunk, offset, file);
    }

    for (int i = 0; i < chunk->constants.count; i++) {
        Value v = chunk->constants.values[i];
        if (IS_OBJ(v) && IS_FUNCTION(v)) {
            ObjFunction* fn = AS_FUNCTION(v);
            const char* fname = fn->name ? fn->name->chars : "<anon>";
            fprintf(file, "\n-- Function constant %d: %s/%d --\n", i, fname, fn->arity);
            if (fn->chunk.count > 0) {
                disassembleChunkToFile(&fn->chunk, fname, file);
            }
        }
    }
}

void disassembleChunk(Chunk* chunk, const char* name) {
    disassembleChunkToFile(chunk, name, stdout);
}

int disassembleInstructionToFile(Chunk* chunk, int offset, FILE* f) {
    fprintf(f, "%04d ", offset);

    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        fprintf(f, "   | ");
    } else {
        fprintf(f, "%4d ", chunk->lines[offset]);
    }

    uint32_t instruction = chunk->code[offset];
    uint16_t opcode      = OPCODE(instruction);

    switch (opcode) {
        case MOVE:          return reg_instruction_ab(f, "MOVE", instruction, offset);
        case LOAD_CONST:    return constantInstruction(f, "LOAD_CONST", chunk, instruction, offset);
        case ADD:           return reg_instruction_abc(f, "ADD", instruction, offset);
        case SUB:           return reg_instruction_abc(f, "SUB", instruction, offset);
        case MUL:           return reg_instruction_abc(f, "MUL", instruction, offset);
        case DIV:           return reg_instruction_abc(f, "DIV", instruction, offset);
        case MOD:           return reg_instruction_abc(f, "MOD", instruction, offset);
        case ADD_I:         return immediate_instruction(f, "ADD_I", instruction, offset);
        case SUB_I:         return immediate_instruction(f, "SUB_I", instruction, offset);
        case MUL_I:         return immediate_instruction(f, "MUL_I", instruction, offset);
        case DIV_I:         return immediate_instruction(f, "DIV_I", instruction, offset);
        case MOD_I:         return immediate_instruction(f, "MOD_I", instruction, offset);
        case ADD_L:         return literal_instruction(f, "ADD_L", chunk, offset);
        case SUB_L:         return literal_instruction(f, "SUB_L", chunk, offset);
        case MUL_L:         return literal_instruction(f, "MUL_L", chunk, offset);
        case DIV_L:         return literal_instruction(f, "DIV_L", chunk, offset);
        case MOD_L:         return literal_instruction(f, "MOD_L", chunk, offset);
        case BAND:          return reg_instruction_abc(f, "BAND", instruction, offset);
        case BOR:           return reg_instruction_abc(f, "BOR", instruction, offset);
        case BXOR:          return reg_instruction_abc(f, "BXOR", instruction, offset);
        case BLSHIFT:       return reg_instruction_abc(f, "BLSHIFT", instruction, offset);
        case BRSHIFT_U:     return reg_instruction_abc(f, "BRSHIFT_U", instruction, offset);
        case BRSHIFT_I:     return reg_instruction_abc(f, "BRSHIFT_I", instruction, offset);
        case BAND_I:        return immediate_instruction(f, "BAND_I", instruction, offset);
        case BOR_I:         return immediate_instruction(f, "BOR_I", instruction, offset);
        case BXOR_I:        return immediate_instruction(f, "BXOR_I", instruction, offset);
        case BLSHIFT_I:     return immediate_instruction(f, "BLSHIFT_I", instruction, offset);
        case BRSHIFT_U_I:   return immediate_instruction(f, "BRSHIFT_U_I", instruction, offset);
        case BRSHIFT_I_I:   return immediate_instruction(f, "BRSHIFT_I_I", instruction, offset);
        case BAND_L:        return literal_instruction(f, "BAND_L", chunk, offset);
        case BOR_L:         return literal_instruction(f, "BOR_L", chunk, offset);
        case BXOR_L:        return literal_instruction(f, "BXOR_L", chunk, offset);
        case BLSHIFT_L:     return literal_instruction(f, "BLSHIFT_L", chunk, offset);
        case BRSHIFT_U_L:   return literal_instruction(f, "BRSHIFT_U_L", chunk, offset);
        case BRSHIFT_I_L:   return literal_instruction(f, "BRSHIFT_I_L", chunk, offset);
        case EQ:            return reg_instruction_abc(f, "EQ",  instruction, offset);
        case GT:            return reg_instruction_abc(f, "GT",  instruction, offset);
        case LT:            return reg_instruction_abc(f, "LT",  instruction, offset);
        case NE:            return reg_instruction_abc(f, "NE",  instruction, offset);
        case LE:            return reg_instruction_abc(f, "LE",  instruction, offset);
        case GE:            return reg_instruction_abc(f, "GE",  instruction, offset);
        case EQ_I:          return immediate_instruction(f, "EQ_I", instruction, offset);
        case GT_I:          return immediate_instruction(f, "GT_I", instruction, offset);
        case LT_I:          return immediate_instruction(f, "LT_I", instruction, offset);
        case NE_I:          return immediate_instruction(f, "NE_I", instruction, offset);
        case LE_I:          return immediate_instruction(f, "LE_I", instruction, offset);
        case GE_I:          return immediate_instruction(f, "GE_I", instruction, offset);
        case EQ_L:          return literal_instruction(f, "EQ_L", chunk, offset);
        case GT_L:          return literal_instruction(f, "GT_L", chunk, offset);
        case LT_L:          return literal_instruction(f, "LT_L", chunk, offset);
        case NE_L:          return literal_instruction(f, "NE_L", chunk, offset);
        case LE_L:          return literal_instruction(f, "LE_L", chunk, offset);
        case GE_L:          return literal_instruction(f, "GE_L", chunk, offset);
        case NEG:           return reg_instruction_ab(f, "NEG", instruction, offset);
        case NOT:           return reg_instruction_ab(f, "NOT", instruction, offset);
        case BNOT:          return reg_instruction_ab(f, "BNOT", instruction, offset);
        case JUMP_IF_FALSE: return jump_conditional_instruction(f, "JUMP_IF_FALSE", instruction, offset);
        case JUMP_IF_TRUE:  return jump_conditional_instruction(f, "JUMP_IF_TRUE", instruction, offset);
        case JUMP:          return jump_instruction(f, instruction, offset);
        case BRANCH_EQ:     return reg3_instruction(f, "BRANCH_EQ", instruction, offset);
        case BRANCH_NE:     return reg3_instruction(f, "BRANCH_NE", instruction, offset);
        case BRANCH_LT:     return reg3_instruction(f, "BRANCH_LT", instruction, offset);
        case BRANCH_LE:     return reg3_instruction(f, "BRANCH_LE", instruction, offset);
        case BRANCH_GT:     return reg3_instruction(f, "BRANCH_GT", instruction, offset);
        case BRANCH_GE:     return reg3_instruction(f, "BRANCH_GE", instruction, offset);
        case BRANCH_EQ_I:   return branch_imm_instruction(f, "BRANCH_EQ_I", chunk, instruction, offset);
        case BRANCH_NE_I:   return branch_imm_instruction(f, "BRANCH_NE_I", chunk, instruction, offset);
        case BRANCH_LT_I:   return branch_imm_instruction(f, "BRANCH_LT_I", chunk, instruction, offset);
        case BRANCH_LE_I:   return branch_imm_instruction(f, "BRANCH_LE_I", chunk, instruction, offset);
        case BRANCH_GT_I:   return branch_imm_instruction(f, "BRANCH_GT_I", chunk, instruction, offset);
        case BRANCH_GE_I:   return branch_imm_instruction(f, "BRANCH_GE_I", chunk, instruction, offset);
        case BRANCH_EQ_L:   return branch_lit_instruction(f, "BRANCH_EQ_L", chunk, instruction, offset);
        case BRANCH_NE_L:   return branch_lit_instruction(f, "BRANCH_NE_L", chunk, instruction, offset);
        case BRANCH_LT_L:   return branch_lit_instruction(f, "BRANCH_LT_L", chunk, instruction, offset);
        case BRANCH_LE_L:   return branch_lit_instruction(f, "BRANCH_LE_L", chunk, instruction, offset);
        case BRANCH_GT_L:   return branch_lit_instruction(f, "BRANCH_GT_L", chunk, instruction, offset);
        case BRANCH_GE_L:   return branch_lit_instruction(f, "BRANCH_GE_L", chunk, instruction, offset);
        case DEFINE_GLOBAL: return constantInstruction(f, "DEFINE_GLOBAL", chunk, instruction, offset);
        case GET_GLOBAL:    return constantInstruction(f, "GET_GLOBAL", chunk, instruction, offset);
        case GET_GLOBAL_CACHED: return reg_instruction_abx(f, "GET_GLOBAL_CACHED", instruction, offset);
        case SET_GLOBAL:    return constantInstruction(f, "SET_GLOBAL", chunk, instruction, offset);
        case SET_GLOBAL_CACHED: return reg_instruction_abx(f, "SET_GLOBAL_CACHED", instruction, offset);
        case CALL:          return callInstruction(f, "CALL", instruction, offset);
        case CALL_SELF:     return callInstruction(f, "CALL_SELF", instruction, offset);
        case CALL_VAR:      return reg_instruction_a(f, "CALL_VAR", instruction, offset);
        case CALL_ARG_PREP: return reg_instruction_abx(f, "CALL_ARG_PREP", instruction, offset);
        case CALL_ARG_SPREAD: return reg_instruction_a(f, "CALL_ARG_SPREAD", instruction, offset);
        case CALL_ARG_PUSH: return reg_instruction_a(f, "CALL_ARG_PUSH", instruction, offset);
        case TAIL_CALL:     return callInstruction(f, "TAIL_CALL", instruction, offset);
        case TAIL_CALL_SELF: return callInstruction(f, "TAIL_CALL_SELF", instruction, offset);
        case CLOSURE:       return constantInstruction(f, "CLOSURE", chunk, instruction, offset);
        case GET_UPVALUE:   return upvalueInstruction(f, "GET_UPVALUE", instruction, offset);
        case SET_UPVALUE:   return upvalueInstruction(f, "SET_UPVALUE", instruction, offset);
        case CLOSE_UPVALUE: return reg_instruction_a(f, "CLOSE_UPVALUE", instruction, offset);
        case CLOSE_FRAME_UPVALUES: return simpleInstruction(f, "CLOSE_FRAME_UPVALUES", offset);
        case NEW_LIST:      return reg_bx_instruction(f, "NEW_LIST", chunk, offset);
        case LIST_APPEND:   return reg2_instruction(f, "LIST_APPEND", chunk, offset);
        case LIST_SPREAD:   return reg2_instruction(f, "LIST_SPREAD", chunk, offset);
        case GET_SUBSCRIPT: return reg_instruction_abc(f, "GET_SUBSCRIPT", instruction, offset);
        case GET_SUBSCRIPT_I: {
            uint8_t a = REG_A(instruction);
            uint8_t b = REG_B(instruction);
            uint8_t c = REG_C(instruction);
            fprintf(f, "%-16s R%d, R%d, #%d\n", "GET_SUBSCRIPT_I", a, b, c);
            return offset + 1;
        }
        case SET_SUBSCRIPT: return reg_instruction_abc(f, "SET_SUBSCRIPT", instruction, offset);
        case SET_SUBSCRIPT_I: {
            uint8_t a = REG_A(instruction);
            uint8_t b = REG_B(instruction);
            uint8_t c = REG_C(instruction);
            fprintf(f, "%-16s R%d, #%d, R%d\n", "SET_SUBSCRIPT_I", a, b, c);
            return offset + 1;
        }
        case NEW_MAP:       return reg_instruction_a(f, "NEW_MAP", instruction, offset);
        case MAP_SET:       return reg_instruction_abc(f, "MAP_SET", instruction, offset);
        case MAP_SPREAD:    return reg2_instruction(f, "MAP_SPREAD", chunk, offset);
        //case GET_MAP_PROPERTY: return reg_instruction_abc(f, "GET_MAP_PROPERTY", instruction, offset);
        //case SET_MAP_PROPERTY: return reg_instruction_abc(f, "SET_MAP_PROPERTY", instruction, offset);
        case GET_MAP_PROPERTY_L: {
            uint8_t a = REG_A(instruction);
            uint8_t b = REG_B(instruction);
            uint32_t ci = chunk->code[offset + 1];
            ObjString* key = AS_STRING(chunk->constants.values[ci]);
            fprintf(f, "%-16s R%d, R%d, @%lu(\"%.*s\")\n", "GET_MAP_PROP_L", a, b, ci, key->length, key->chars);
            return offset + 2;
        }
        case SET_MAP_PROPERTY_L: {
            uint8_t a = REG_A(instruction);
            uint8_t c = REG_C(instruction);
            uint32_t ci = chunk->code[offset + 1];
            ObjString* key = AS_STRING(chunk->constants.values[ci]);
            fprintf(f, "%-16s R%d, @%lu(\"%.*s\"), R%d\n", "SET_MAP_PROP_L", a, ci, key->length, key->chars, c);
            return offset + 2;
        }
        case GET_STRUCT_FIELD_IC: {
            uint8_t a = REG_A(instruction);
            uint8_t b = REG_B(instruction);
            uint8_t c = REG_C(instruction);
            fprintf(f, "%-16s R%d, R%d, field[%d]\n", "GET_FIELD_IC", a, b, c);
            return offset + 2;
        }
        case SET_STRUCT_FIELD_IC: {
            uint8_t a = REG_A(instruction);
            uint8_t b = REG_B(instruction);
            uint8_t c = REG_C(instruction);
            fprintf(f, "%-16s R%d, field[%d], R%d\n", "SET_FIELD_IC", a, b, c);
            return offset + 2;
        }
        case NEW_DISPATCHER: return reg_instruction_a(f, "NEW_DISPATCHER", instruction, offset);
        case ADD_OVERLOAD:   return reg2_instruction(f, "ADD_OVERLOAD", chunk, offset);
        case SET_VARIADIC_FALLBACK: return reg_instruction_abc(f, "SET_VAR_FALLBACK", instruction, offset);
        case PACK_REST:     return reg_instruction_abc(f, "PACK_REST", instruction, offset);
        case NEW_STRUCT: return constantInstruction(f, "NEW_STRUCT", chunk, instruction, offset);
        case STRUCT_SPREAD: return reg2_instruction(f, "STRUCT_SPREAD", chunk, offset);
        case GET_STRUCT_FIELD: return reg_instruction_abc(f, "GET_STRUCT_FIELD", instruction, offset);
        case SET_STRUCT_FIELD: return reg_instruction_abc(f, "SET_STRUCT_FIELD", instruction, offset);
        case PRE_INC: return reg_instruction_ab(f, "PRE_INC", instruction, offset);
        case POST_INC: return reg_instruction_ab(f, "POST_INC", instruction, offset);
        case PRE_DEC: return reg_instruction_ab(f, "PRE_DEC", instruction, offset);
        case POST_DEC: return reg_instruction_ab(f, "POST_DEC", instruction, offset);
        case SPILL_LOAD:  return reg_instruction_abx(f, "SPILL_LOAD",  instruction, offset);
        case SPILL_STORE: return reg_instruction_abx(f, "SPILL_STORE", instruction, offset);
        case CONCAT_N: {
            uint8_t a = REG_A(instruction), b = REG_B(instruction), c = REG_C(instruction);
            fprintf(f, "%-16s R%-3d, R%d..R%d (%d)\n", "CONCAT_N", a, b, b + c - 1, c);
            return offset + 1;
        }
        case RET: {
            uint32_t instr = instruction;
            uint8_t  a  = REG_A(instr);
            uint16_t bx = REG_Bx(instr);
            if (bx == 1) {
                fprintf(f, "%-16s (implicit null)\n", "RET");
            } else {
                fprintf(f, "%-16s R%-2u\n", "RET", a);
            }
            return offset + 1;
        }
        default:
            fprintf(f, "Unknown opcode %u\n", opcode);
            return offset + 1;
    }
}

int disassembleInstruction(Chunk* chunk, int offset) {
    return disassembleInstructionToFile(chunk, offset, stdout);
}
