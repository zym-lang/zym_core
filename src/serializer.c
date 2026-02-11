#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "chunk.h"
#include "config.h"
#include "gc.h"
#include "memory.h"
#include "utils.h"
#include "object.h"

#define TYPE_TAG_NUMBER   0x01
#define TYPE_TAG_STRING   0x02
#define TYPE_TAG_NULL     0x03
#define TYPE_TAG_FALSE    0x04
#define TYPE_TAG_TRUE     0x05
#define TYPE_TAG_FUNCTION 0x06

static void writeBytes(VM* vm, OutputBuffer* out, const void* data, size_t size) {
    appendToOutputBuffer(vm, out, (const char*)data, size);
}

void serializeChunk(VM* vm, Chunk* chunk, CompilerConfig config, OutputBuffer* out) {
    const char magic[] = "ZYM\0";
    const uint8_t version = 1;
    writeBytes(vm, out, magic, 4);
    writeBytes(vm, out, &version, sizeof(uint8_t));

    int entryFileLen = (vm->entry_file ? vm->entry_file->length : -1);
    writeBytes(vm, out, &entryFileLen, sizeof(int));
    if (entryFileLen > 0) {
        writeBytes(vm, out, vm->entry_file->chars, (size_t)entryFileLen);
    }

    writeBytes(vm, out, &chunk->constants.count, sizeof(int));
    for (int i = 0; i < chunk->constants.count; i++) {
        Value value = chunk->constants.values[i];

        if (IS_DOUBLE(value)) {
            uint8_t tag = TYPE_TAG_NUMBER;
            writeBytes(vm, out, &tag, sizeof(uint8_t));
            double number = AS_DOUBLE(value);
            writeBytes(vm, out, &number, sizeof(double));
        } else if (IS_STRING(value)) {
            uint8_t tag = TYPE_TAG_STRING;
            writeBytes(vm, out, &tag, sizeof(uint8_t));
            ObjString* s = AS_STRING(value);
            writeBytes(vm, out, &s->length, sizeof(int));
            writeBytes(vm, out, s->chars, (size_t)s->length);
        } else if (IS_NULL(value)) {
            uint8_t tag = TYPE_TAG_NULL;
            writeBytes(vm, out, &tag, sizeof(uint8_t));
        } else if (IS_BOOL(value)) {
            uint8_t tag = AS_BOOL(value) ? TYPE_TAG_TRUE : TYPE_TAG_FALSE;
            writeBytes(vm, out, &tag, sizeof(uint8_t));
        } else if (IS_OBJ(value) && IS_FUNCTION(value)) {
            uint8_t tag = TYPE_TAG_FUNCTION;
            writeBytes(vm, out, &tag, sizeof(uint8_t));

            ObjFunction* fn = AS_FUNCTION(value);
            writeBytes(vm, out, &fn->arity, sizeof(int));
            writeBytes(vm, out, &fn->max_regs, sizeof(int));
            writeBytes(vm, out, &fn->upvalue_count, sizeof(int));
            if (fn->upvalue_count > 0) {
                writeBytes(vm, out, fn->upvalues, sizeof(Upvalue) * fn->upvalue_count);
            }

            int nameLen = (fn->name ? fn->name->length : -1);
            writeBytes(vm, out, &nameLen, sizeof(int));
            if (nameLen > 0) {
                writeBytes(vm, out, fn->name->chars, (size_t)nameLen);
            }

            int modNameLen = (fn->module_name ? fn->module_name->length : -1);
            writeBytes(vm, out, &modNameLen, sizeof(int));
            if (modNameLen > 0) {
                writeBytes(vm, out, fn->module_name->chars, (size_t)modNameLen);
            }

            if (fn->arity > 0 && fn->param_qualifiers != NULL) {
                writeBytes(vm, out, fn->param_qualifiers, sizeof(uint8_t) * fn->arity);
            }
            
            // Write qualifier_sig for call fast-path optimization
            writeBytes(vm, out, &fn->qualifier_sig, sizeof(uint8_t));

            OutputBuffer nested;
            initOutputBuffer(&nested);
            serializeChunk(vm, fn->chunk, config, &nested);
            int32_t nestedSize = (int32_t)nested.count;
            writeBytes(vm, out, &nestedSize, sizeof(int32_t));
            writeBytes(vm, out, nested.buffer, (size_t)nestedSize);
            freeOutputBuffer(vm, &nested);
        } else if (IS_OBJ(value) && IS_STRUCT_SCHEMA(value)) {
            uint8_t tag = 0x07;
            writeBytes(vm, out, &tag, sizeof(uint8_t));

            ObjStructSchema* schema = AS_STRUCT_SCHEMA(value);
            int nameLen = schema->name->length;
            writeBytes(vm, out, &nameLen, sizeof(int));
            writeBytes(vm, out, schema->name->chars, (size_t)nameLen);

            writeBytes(vm, out, &schema->field_count, sizeof(int));
            for (int i = 0; i < schema->field_count; i++) {
                int fieldLen = schema->field_names[i]->length;
                writeBytes(vm, out, &fieldLen, sizeof(int));
                writeBytes(vm, out, schema->field_names[i]->chars, (size_t)fieldLen);
            }
        } else if (IS_OBJ(value) && IS_ENUM_SCHEMA(value)) {
            uint8_t tag = 0x08;
            writeBytes(vm, out, &tag, sizeof(uint8_t));

            ObjEnumSchema* schema = AS_ENUM_SCHEMA(value);
            int nameLen = schema->name->length;
            writeBytes(vm, out, &nameLen, sizeof(int));
            writeBytes(vm, out, schema->name->chars, (size_t)nameLen);

            writeBytes(vm, out, &schema->type_id, sizeof(int));
            writeBytes(vm, out, &schema->variant_count, sizeof(int));
            for (int i = 0; i < schema->variant_count; i++) {
                int variantLen = schema->variant_names[i]->length;
                writeBytes(vm, out, &variantLen, sizeof(int));
                writeBytes(vm, out, schema->variant_names[i]->chars, (size_t)variantLen);
            }
        } else if (IS_ENUM(value)) {
            uint8_t tag = 0x09;
            writeBytes(vm, out, &tag, sizeof(uint8_t));
            int type_id = ENUM_TYPE_ID(value);
            int variant = ENUM_VARIANT(value);
            writeBytes(vm, out, &type_id, sizeof(int));
            writeBytes(vm, out, &variant, sizeof(int));
        } else if (IS_OBJ(value) && (IS_NATIVE_CONTEXT(value) || IS_NATIVE_CLOSURE(value))) {
            fprintf(stderr, "ERROR: Cannot serialize native closures or native contexts\n");
        }
    }

    writeBytes(vm, out, &chunk->count, sizeof(int));
    writeBytes(vm, out, chunk->code, sizeof(uint32_t) * chunk->count);

    if (config.include_line_info) {
        writeBytes(vm, out, &chunk->count, sizeof(int));
        writeBytes(vm, out, chunk->lines, sizeof(int) * chunk->count);
    } else {
        int zero = 0;
        writeBytes(vm, out, &zero, sizeof(int));
    }
}

bool deserializeChunk(VM* vm, Chunk* chunk, const uint8_t* buffer, size_t size) {
    const uint8_t* p = buffer;

    #define READ_BYTES(dest, count) \
        do { \
            if ((size_t)(p - buffer) + (size_t)(count) > size) { \
                fprintf(stderr, "READ_BYTES failed: offset=%zu, count=%zu, size=%zu\n", (size_t)(p - buffer), (size_t)(count), size); \
                return false; \
            } \
            memcpy((dest), p, (count)); \
            p += (count); \
        } while (0)

    char magic[4];
    READ_BYTES(magic, 4);
    if (strncmp(magic, "ZYM\0", 4) != 0) {
        fprintf(stderr, "Invalid magic header\n");
        return false;
    }

    uint8_t version = 0;
    READ_BYTES(&version, sizeof(uint8_t));
    if (version != 1) return false;

    int entryFileLen = 0;
    READ_BYTES(&entryFileLen, sizeof(int));
    if (entryFileLen > 0) {
        char* entryFileChars = (char*)malloc(entryFileLen + 1);
        READ_BYTES(entryFileChars, entryFileLen);
        entryFileChars[entryFileLen] = '\0';
        vm->entry_file = copyString(vm, entryFileChars, entryFileLen);
        free(entryFileChars);
    } else {
        vm->entry_file = NULL;
    }

    int constant_count = 0;
    READ_BYTES(&constant_count, sizeof(int));
    for (int i = 0; i < constant_count; i++) {
        uint8_t tag = 0;
        READ_BYTES(&tag, sizeof(uint8_t));

        switch (tag) {
            case TYPE_TAG_NUMBER: {
                double num = 0.0;
                READ_BYTES(&num, sizeof(double));
                addConstant(vm, chunk, DOUBLE_VAL(num));
                break;
            }
            case TYPE_TAG_STRING: {
                int length = 0;
                READ_BYTES(&length, sizeof(int));
                if (length < 0) return false;
                char* chars = (char*)reallocate(vm, NULL, 0, (size_t)length + 1);
                READ_BYTES(chars, (size_t)length);
                chars[length] = '\0';
                ObjString* s = copyString(vm, chars, length);
                pushTempRoot(vm, (Obj*)s);
                reallocate(vm, chars, (size_t)length + 1, 0);
                addConstant(vm, chunk, OBJ_VAL(s));
                popTempRoot(vm);
                break;
            }
            case TYPE_TAG_NULL: {
                addConstant(vm, chunk, NULL_VAL);
                break;
            }
            case TYPE_TAG_FALSE: {
                addConstant(vm, chunk, FALSE_VAL);
                break;
            }
            case TYPE_TAG_TRUE: {
                addConstant(vm, chunk, TRUE_VAL);
                break;
            }
            case TYPE_TAG_FUNCTION: {
                ObjFunction* fn = newFunction(vm);
                pushTempRoot(vm, (Obj*)fn);
                #define READ_BYTES_OR_FAIL(dest, count) \
                    do { \
                        if ((size_t)(p - buffer) + (size_t)(count) > size) { \
                            fprintf(stderr, "Function deserialization: READ_BYTES_OR_FAIL failed at offset=%zu, count=%zu, size=%zu\n", (size_t)(p - buffer), (size_t)(count), size); \
                            goto fn_deserialize_fail; \
                        } \
                        memcpy((dest), p, (count)); \
                        p += (count); \
                    } while (0)

                READ_BYTES_OR_FAIL(&fn->arity, sizeof(int));
                READ_BYTES_OR_FAIL(&fn->max_regs, sizeof(int));
                READ_BYTES_OR_FAIL(&fn->upvalue_count, sizeof(int));
                if (fn->upvalue_count > 0) {
                    READ_BYTES_OR_FAIL(fn->upvalues, sizeof(Upvalue) * fn->upvalue_count);
                }

                int nameLen = -1;
                READ_BYTES_OR_FAIL(&nameLen, sizeof(int));
                if (nameLen > 0) {
                    char* nameBuf = (char*)reallocate(vm, NULL, 0, (size_t)nameLen + 1);
                    READ_BYTES_OR_FAIL(nameBuf, (size_t)nameLen);
                    nameBuf[nameLen] = '\0';
                    fn->name = takeString(vm, nameBuf, nameLen);
                } else {
                    fn->name = NULL;
                }

                int modNameLen = -1;
                READ_BYTES_OR_FAIL(&modNameLen, sizeof(int));
                if (modNameLen > 0) {
                    char* modNameBuf = (char*)reallocate(vm, NULL, 0, (size_t)modNameLen + 1);
                    READ_BYTES_OR_FAIL(modNameBuf, (size_t)modNameLen);
                    modNameBuf[modNameLen] = '\0';
                    fn->module_name = takeString(vm, modNameBuf, modNameLen);
                } else {
                    fn->module_name = NULL;
                }

                if (fn->arity > 0) {
                    fn->param_qualifiers = ALLOCATE(vm, uint8_t, fn->arity);
                    READ_BYTES_OR_FAIL(fn->param_qualifiers, sizeof(uint8_t) * fn->arity);
                } else {
                    fn->param_qualifiers = NULL;
                }
                
                // Read qualifier_sig for call fast-path optimization
                READ_BYTES_OR_FAIL(&fn->qualifier_sig, sizeof(uint8_t));

                int32_t nestedSize = 0;
                READ_BYTES_OR_FAIL(&nestedSize, sizeof(int32_t));
                if (nestedSize < 0) {
                    fprintf(stderr, "Function deserialization: invalid nestedSize=%d\n", nestedSize);
                    goto fn_deserialize_fail;
                }
                if (nestedSize > 0) {
                    const uint8_t* nestedStart = p;
                    if ((size_t)(p - buffer) + (size_t)nestedSize > size) {
                        fprintf(stderr, "Function deserialization: nested chunk out of bounds, offset=%zu, nestedSize=%d, total_size=%zu\n", (size_t)(p - buffer), nestedSize, size);
                        goto fn_deserialize_fail;
                    }

                    if (!deserializeChunk(vm, fn->chunk, nestedStart, (size_t)nestedSize)) {
                        fprintf(stderr, "Function deserialization: recursive deserializeChunk failed for nested function\n");
                        goto fn_deserialize_fail;
                    }
                    p += nestedSize;
                }

                addConstant(vm, chunk, OBJ_VAL(fn));
                popTempRoot(vm);
                #undef READ_BYTES_OR_FAIL
                break;

            fn_deserialize_fail:
                #undef READ_BYTES_OR_FAIL
                popTempRoot(vm);
                return false;
            }
            case 0x07: {
                int nameLen = 0;
                READ_BYTES(&nameLen, sizeof(int));
                if (nameLen < 0) return false;

                char* nameChars = (char*)reallocate(vm, NULL, 0, (size_t)nameLen + 1);
                READ_BYTES(nameChars, (size_t)nameLen);
                nameChars[nameLen] = '\0';
                ObjString* name = takeString(vm, nameChars, nameLen);
                pushTempRoot(vm, (Obj*)name);

                int field_count = 0;
                READ_BYTES(&field_count, sizeof(int));
                if (field_count < 0) {
                    popTempRoot(vm);
                    return false;
                }

                ObjString** field_names = ALLOCATE(vm, ObjString*, field_count);
                for (int i = 0; i < field_count; i++) {
                    int fieldLen = 0;
                    READ_BYTES(&fieldLen, sizeof(int));
                    if (fieldLen < 0) {
                        // Cleanup already pushed strings
                        for (int j = 0; j < i; j++) popTempRoot(vm);
                        popTempRoot(vm); // name
                        return false;
                    }

                    char* fieldChars = (char*)reallocate(vm, NULL, 0, (size_t)fieldLen + 1);
                    READ_BYTES(fieldChars, (size_t)fieldLen);
                    fieldChars[fieldLen] = '\0';
                    field_names[i] = takeString(vm, fieldChars, fieldLen);
                    pushTempRoot(vm, (Obj*)field_names[i]);
                }

                ObjStructSchema* schema = newStructSchema(vm, name, field_names, field_count);
                pushTempRoot(vm, (Obj*)schema);
                addConstant(vm, chunk, OBJ_VAL(schema));
                popTempRoot(vm); // schema

                // Pop field names and name
                for (int i = 0; i < field_count; i++) popTempRoot(vm);
                popTempRoot(vm); // name
                break;
            }
            case 0x08: {
                int nameLen = 0;
                READ_BYTES(&nameLen, sizeof(int));
                if (nameLen < 0) return false;

                char* nameChars = (char*)reallocate(vm, NULL, 0, (size_t)nameLen + 1);
                READ_BYTES(nameChars, (size_t)nameLen);
                nameChars[nameLen] = '\0';
                ObjString* name = takeString(vm, nameChars, nameLen);
                pushTempRoot(vm, (Obj*)name);

                int type_id = 0;
                READ_BYTES(&type_id, sizeof(int));

                int variant_count = 0;
                READ_BYTES(&variant_count, sizeof(int));
                if (variant_count < 0) {
                    popTempRoot(vm);
                    return false;
                }

                ObjString** variant_names = ALLOCATE(vm, ObjString*, variant_count);
                for (int i = 0; i < variant_count; i++) {
                    int variantLen = 0;
                    READ_BYTES(&variantLen, sizeof(int));
                    if (variantLen < 0) {
                        for (int j = 0; j < i; j++) popTempRoot(vm);
                        popTempRoot(vm);
                        return false;
                    }

                    char* variantChars = (char*)reallocate(vm, NULL, 0, (size_t)variantLen + 1);
                    READ_BYTES(variantChars, (size_t)variantLen);
                    variantChars[variantLen] = '\0';
                    variant_names[i] = takeString(vm, variantChars, variantLen);
                    pushTempRoot(vm, (Obj*)variant_names[i]);
                }

                ObjEnumSchema* schema = newEnumSchema(vm, name, variant_names, variant_count);
                schema->type_id = type_id;
                pushTempRoot(vm, (Obj*)schema);
                addConstant(vm, chunk, OBJ_VAL(schema));
                popTempRoot(vm); // schema

                for (int i = 0; i < variant_count; i++) popTempRoot(vm);
                popTempRoot(vm); // name
                break;
            }
            case 0x09: {
                int type_id = 0;
                READ_BYTES(&type_id, sizeof(int));
                int variant = 0;
                READ_BYTES(&variant, sizeof(int));

                Value enum_val = ENUM_VAL(type_id, variant);
                addConstant(vm, chunk, enum_val);
                break;
            }
            default:
                return false;
        }
    }

    int instruction_count = 0;
    READ_BYTES(&instruction_count, sizeof(int));
    if (instruction_count < 0) return false;
    if (instruction_count > 0) {
        chunk->capacity = instruction_count;
        chunk->code = GROW_ARRAY(vm, uint32_t, chunk->code, 0, chunk->capacity);
        chunk->lines = GROW_ARRAY(vm, int, chunk->lines, 0, chunk->capacity);

        READ_BYTES(chunk->code, sizeof(uint32_t) * (size_t)instruction_count);
        chunk->count = instruction_count;
    } else {
        chunk->count = 0;
    }

    int line_count = 0;
    READ_BYTES(&line_count, sizeof(int));
    if (line_count < 0) return false;
    if (line_count > 0) {
        if (line_count != instruction_count) return false;
        READ_BYTES(chunk->lines, sizeof(int) * (size_t)line_count);
    }

    return true;

    #undef READ_BYTES
}