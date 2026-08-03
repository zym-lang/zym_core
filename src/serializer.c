#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

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

// =============================================================================
// String pool
// =============================================================================
//
// The serialized form stores every string exactly once, in a pool at the
// head of the file; all string-bearing records (string constants,
// function names, module names, schema names/fields/variants, the entry
// file) reference it by i32 index (-1 = none). In-memory strings are
// interned, so an identifier referenced from many function chunks used
// to serialize as one inline copy per chunk; the pool restores the 1:1
// relationship and spares the loader re-interning every copy.

typedef struct {
    ObjString** items;    // encounter-ordered pool entries
    int count;
    int capacity;
    ObjString** slots;    // open-addressed set over interned pointers
    int* slot_index;      // pool index parallel to `slots`
    int slot_capacity;    // power of two, 0 until first insert
} StringPool;

static void poolInit(StringPool* pool) {
    memset(pool, 0, sizeof(*pool));
}

static void poolFree(VM* vm, StringPool* pool) {
    if (pool->items)      FREE_ARRAY(vm, ObjString*, pool->items, pool->capacity);
    if (pool->slots)      FREE_ARRAY(vm, ObjString*, pool->slots, pool->slot_capacity);
    if (pool->slot_index) FREE_ARRAY(vm, int, pool->slot_index, pool->slot_capacity);
}

static void poolRehash(VM* vm, StringPool* pool, int new_capacity) {
    ObjString** slots = ALLOCATE(vm, ObjString*, new_capacity);
    int* slot_index   = ALLOCATE(vm, int, new_capacity);
    for (int i = 0; i < new_capacity; i++) slots[i] = NULL;
    int mask = new_capacity - 1;
    for (int i = 0; i < pool->count; i++) {
        ObjString* s = pool->items[i];
        int probe = (int)(s->hash & (uint32_t)mask);
        while (slots[probe] != NULL) probe = (probe + 1) & mask;
        slots[probe] = s;
        slot_index[probe] = i;
    }
    if (pool->slots)      FREE_ARRAY(vm, ObjString*, pool->slots, pool->slot_capacity);
    if (pool->slot_index) FREE_ARRAY(vm, int, pool->slot_index, pool->slot_capacity);
    pool->slots = slots;
    pool->slot_index = slot_index;
    pool->slot_capacity = new_capacity;
}

// Strings are interned (copyString/takeString), so pointer identity is
// value identity and the set probes on the pointer alone.
static int poolIntern(VM* vm, StringPool* pool, ObjString* s) {
    if ((pool->count + 1) * 2 > pool->slot_capacity) {
        poolRehash(vm, pool, pool->slot_capacity < 64 ? 64 : pool->slot_capacity * 2);
    }
    int mask = pool->slot_capacity - 1;
    int probe = (int)(s->hash & (uint32_t)mask);
    while (pool->slots[probe] != NULL) {
        if (pool->slots[probe] == s) return pool->slot_index[probe];
        probe = (probe + 1) & mask;
    }
    if (pool->count == pool->capacity) {
        int old = pool->capacity;
        pool->capacity = old < 64 ? 64 : old * 2;
        pool->items = (ObjString**)reallocate(vm, pool->items,
                                              sizeof(ObjString*) * (size_t)old,
                                              sizeof(ObjString*) * (size_t)pool->capacity);
    }
    pool->items[pool->count] = s;
    pool->slots[probe] = s;
    pool->slot_index[probe] = pool->count;
    return pool->count++;
}

static void writePoolRef(VM* vm, OutputBuffer* out, StringPool* pool, ObjString* s) {
    int idx = s ? poolIntern(vm, pool, s) : -1;
    writeBytes(vm, out, &idx, sizeof(int));
}

static void serializeChunkBody(VM* vm, Chunk* chunk, CompilerConfig config,
                               OutputBuffer* out, StringPool* pool) {
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
            writePoolRef(vm, out, pool, AS_STRING(value));
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
            writeBytes(vm, out, &fn->fixed_arity, sizeof(int));
            uint8_t variadic_flag = fn->is_variadic ? 1 : 0;
            writeBytes(vm, out, &variadic_flag, sizeof(uint8_t));
            writeBytes(vm, out, &fn->max_regs, sizeof(int));
            writeBytes(vm, out, &fn->spill_count, sizeof(int));
            writeBytes(vm, out, &fn->upvalue_count, sizeof(int));
            if (fn->upvalue_count > 0) {
                // Write zeroed copies: the in-memory Upvalue carries
                // struct padding and a compile-time ObjStructSchema*
                // whose raw pointer value must not leak into the
                // bytecode (it made output nondeterministic). Only
                // index/is_local are meaningful on the wire.
                for (int u = 0; u < fn->upvalue_count; u++) {
                    Upvalue wire;
                    memset(&wire, 0, sizeof(Upvalue));
                    wire.index = fn->upvalues[u].index;
                    wire.is_local = fn->upvalues[u].is_local;
                    writeBytes(vm, out, &wire, sizeof(Upvalue));
                }
            }

            writePoolRef(vm, out, pool, fn->name);
            writePoolRef(vm, out, pool, fn->module_name);

            OutputBuffer nested;
            initOutputBuffer(&nested);
            serializeChunkBody(vm, &fn->chunk, config, &nested, pool);
            int32_t nestedSize = (int32_t)nested.count;
            writeBytes(vm, out, &nestedSize, sizeof(int32_t));
            writeBytes(vm, out, nested.buffer, (size_t)nestedSize);
            freeOutputBuffer(vm, &nested);
        } else if (IS_OBJ(value) && IS_STRUCT_SCHEMA(value)) {
            uint8_t tag = 0x07;
            writeBytes(vm, out, &tag, sizeof(uint8_t));

            ObjStructSchema* schema = AS_STRUCT_SCHEMA(value);
            writePoolRef(vm, out, pool, schema->name);
            writeBytes(vm, out, &schema->field_count, sizeof(int));
            for (int f = 0; f < schema->field_count; f++) {
                writePoolRef(vm, out, pool, schema->field_names[f]);
            }
        } else if (IS_OBJ(value) && IS_ENUM_SCHEMA(value)) {
            uint8_t tag = 0x08;
            writeBytes(vm, out, &tag, sizeof(uint8_t));

            ObjEnumSchema* schema = AS_ENUM_SCHEMA(value);
            writePoolRef(vm, out, pool, schema->name);
            writeBytes(vm, out, &schema->type_id, sizeof(int));
            writeBytes(vm, out, &schema->variant_count, sizeof(int));
            for (int v = 0; v < schema->variant_count; v++) {
                writePoolRef(vm, out, pool, schema->variant_names[v]);
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

void serializeChunk(VM* vm, Chunk* chunk, CompilerConfig config, OutputBuffer* out) {
    // The pool's bookkeeping arrays hold bare ObjString* that are only
    // reachable through the (untracked) chunk being serialized, and the
    // buffers grow through `reallocate`; pause collection for the
    // duration, mirroring growStack's save/restore of BOTH gc fields
    // (restoring only gc_enabled would leave the debt counter at the
    // INT32_MAX that reallocate parks it at when it wraps while paused).
    bool gc_was_enabled = vm->gc_enabled;
    int32_t gc_saved_debt = vm->gc_debt;
    vm->gc_enabled = false;
    vm->gc_debt = INT32_MAX;

    StringPool pool;
    poolInit(&pool);

    // Reserve the entry-file slot first so it lands at a stable index.
    int entryIdx = vm->entry_file ? poolIntern(vm, &pool, vm->entry_file) : -1;

    OutputBuffer body;
    initOutputBuffer(&body);
    serializeChunkBody(vm, chunk, config, &body, &pool);

    const char magic[] = "ZYM\0";
    // Pre-release bytecode format, pinned at 1 until the first stable
    // release. The string pool, spill_count, and zeroed upvalue records
    // are all part of this baseline — no compatibility shims for
    // earlier drafts.
    const uint8_t version = 1;
    writeBytes(vm, out, magic, 4);
    writeBytes(vm, out, &version, sizeof(uint8_t));

    writeBytes(vm, out, &pool.count, sizeof(int));
    for (int i = 0; i < pool.count; i++) {
        ObjString* s = pool.items[i];
        writeBytes(vm, out, &s->byte_length, sizeof(int));
        writeBytes(vm, out, s->chars, (size_t)s->byte_length);
    }
    writeBytes(vm, out, &entryIdx, sizeof(int));

    appendToOutputBuffer(vm, out, body.buffer, (size_t)body.count);

    freeOutputBuffer(vm, &body);
    poolFree(vm, &pool);
    vm->gc_enabled = gc_was_enabled;
    vm->gc_debt = gc_saved_debt;
}

static bool deserializeChunkBody(VM* vm, Chunk* chunk, const uint8_t* buffer, size_t size,
                                 ObjString** pool, int pool_count) {
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

    // Resolve an i32 pool reference: yields NULL for -1, fails the
    // chunk on any other out-of-range index.
    #define READ_POOL_REF(out_str) \
        do { \
            int _idx = 0; \
            READ_BYTES(&_idx, sizeof(int)); \
            if (_idx == -1) { (out_str) = NULL; } \
            else if (_idx < 0 || _idx >= pool_count) { \
                fprintf(stderr, "Invalid string pool index %d (pool size %d)\n", _idx, pool_count); \
                return false; \
            } else { (out_str) = pool[_idx]; } \
        } while (0)

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
                ObjString* s = NULL;
                READ_POOL_REF(s);
                if (s == NULL) return false;
                addConstant(vm, chunk, OBJ_VAL(s));
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
                READ_BYTES_OR_FAIL(&fn->fixed_arity, sizeof(int));
                uint8_t variadic_flag = 0;
                READ_BYTES_OR_FAIL(&variadic_flag, sizeof(uint8_t));
                fn->is_variadic = (variadic_flag != 0);
                READ_BYTES_OR_FAIL(&fn->max_regs, sizeof(int));
                READ_BYTES_OR_FAIL(&fn->spill_count, sizeof(int));
                READ_BYTES_OR_FAIL(&fn->upvalue_count, sizeof(int));
                if (fn->upvalue_count > 0) {
                    fn->upvalues = ALLOCATE(vm, Upvalue, fn->upvalue_count);
                    fn->upvalue_capacity = fn->upvalue_count;
                    READ_BYTES_OR_FAIL(fn->upvalues, sizeof(Upvalue) * fn->upvalue_count);
                    // struct_type is a compile-time pointer; whatever
                    // bytes the file carries are meaningless here.
                    for (int u = 0; u < fn->upvalue_count; u++) {
                        fn->upvalues[u].struct_type = NULL;
                    }
                }

                int nameIdx = -1;
                READ_BYTES_OR_FAIL(&nameIdx, sizeof(int));
                if (nameIdx == -1) {
                    fn->name = NULL;
                } else if (nameIdx < 0 || nameIdx >= pool_count) {
                    fprintf(stderr, "Invalid string pool index %d (pool size %d)\n", nameIdx, pool_count);
                    goto fn_deserialize_fail;
                } else {
                    fn->name = pool[nameIdx];
                }

                int modNameIdx = -1;
                READ_BYTES_OR_FAIL(&modNameIdx, sizeof(int));
                if (modNameIdx == -1) {
                    fn->module_name = NULL;
                } else if (modNameIdx < 0 || modNameIdx >= pool_count) {
                    fprintf(stderr, "Invalid string pool index %d (pool size %d)\n", modNameIdx, pool_count);
                    goto fn_deserialize_fail;
                } else {
                    fn->module_name = pool[modNameIdx];
                }

                int32_t nestedSize = 0;
                READ_BYTES_OR_FAIL(&nestedSize, sizeof(int32_t));
                if (nestedSize < 0) {
                    fprintf(stderr, "Function deserialization: invalid nestedSize=%" PRId32 "\n", nestedSize);
                    goto fn_deserialize_fail;
                }
                if (nestedSize > 0) {
                    const uint8_t* nestedStart = p;
                    if ((size_t)(p - buffer) + (size_t)nestedSize > size) {
                        fprintf(stderr, "Function deserialization: nested chunk out of bounds, offset=%zu, nestedSize=%" PRId32 ", total_size=%zu\n", (size_t)(p - buffer), nestedSize, size);
                        goto fn_deserialize_fail;
                    }

                    if (!deserializeChunkBody(vm, &fn->chunk, nestedStart, (size_t)nestedSize, pool, pool_count)) {
                        fprintf(stderr, "Function deserialization: recursive deserializeChunkBody failed for nested function\n");
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
                ObjString* name = NULL;
                READ_POOL_REF(name);
                if (name == NULL) return false;

                int field_count = 0;
                READ_BYTES(&field_count, sizeof(int));
                if (field_count < 0) return false;

                ObjString** field_names = ALLOCATE(vm, ObjString*, field_count);
                for (int f = 0; f < field_count; f++) {
                    READ_POOL_REF(field_names[f]);
                    if (field_names[f] == NULL) return false;
                }

                ObjStructSchema* schema = newStructSchema(vm, name, field_names, field_count);
                addConstant(vm, chunk, OBJ_VAL(schema));
                break;
            }
            case 0x08: {
                ObjString* name = NULL;
                READ_POOL_REF(name);
                if (name == NULL) return false;

                int type_id = 0;
                READ_BYTES(&type_id, sizeof(int));

                int variant_count = 0;
                READ_BYTES(&variant_count, sizeof(int));
                if (variant_count < 0) return false;

                ObjString** variant_names = ALLOCATE(vm, ObjString*, variant_count);
                for (int v = 0; v < variant_count; v++) {
                    READ_POOL_REF(variant_names[v]);
                    if (variant_names[v] == NULL) return false;
                }

                ObjEnumSchema* schema = newEnumSchema(vm, name, variant_names, variant_count);
                schema->type_id = type_id;
                addConstant(vm, chunk, OBJ_VAL(schema));
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
    } else if (instruction_count > 0) {
        // Stripped bytecode carries no line table; zero it so readers
        // (e.g. the disassembler) don't see uninitialized memory.
        memset(chunk->lines, 0, sizeof(int) * (size_t)instruction_count);
    }

    return true;

    #undef READ_POOL_REF
    #undef READ_BYTES
}

bool deserializeChunk(VM* vm, Chunk* chunk, const uint8_t* buffer, size_t size) {
    const uint8_t* p = buffer;

    if (size < 4 + sizeof(uint8_t) + sizeof(int)) {
        fprintf(stderr, "Bytecode too small for header (size=%zu)\n", size);
        return false;
    }

    if (strncmp((const char*)p, "ZYM\0", 4) != 0) {
        fprintf(stderr, "Invalid magic header\n");
        return false;
    }
    p += 4;

    uint8_t version = *p++;
    if (version != 1) {
        fprintf(stderr, "Unsupported bytecode version %u\n", version);
        return false;
    }

    int pool_count = 0;
    memcpy(&pool_count, p, sizeof(int));
    p += sizeof(int);
    if (pool_count < 0) return false;
    // Every pool entry carries at least its 4-byte length; reject
    // counts the remaining bytes cannot possibly hold before sizing
    // the pool array from them. Division form so the check cannot
    // overflow on 32-bit size_t targets.
    if ((size_t)pool_count > (size - (size_t)(p - buffer)) / sizeof(int)) {
        fprintf(stderr, "Invalid string pool count %d for %zu remaining bytes\n",
                pool_count, size - (size_t)(p - buffer));
        return false;
    }

    // The pool array and the half-built chunk are invisible to the GC
    // while deserialization is in flight; pause collection for the
    // duration (same dual-field save/restore as the serializer).
    bool gc_was_enabled = vm->gc_enabled;
    int32_t gc_saved_debt = vm->gc_debt;
    vm->gc_enabled = false;
    vm->gc_debt = INT32_MAX;

    ObjString** pool = pool_count > 0 ? ALLOCATE(vm, ObjString*, pool_count) : NULL;
    bool ok = false;
    int entryIdx = -1;

    for (int i = 0; i < pool_count; i++) {
        int len = 0;
        if ((size_t)(p - buffer) + sizeof(int) > size) goto done;
        memcpy(&len, p, sizeof(int));
        p += sizeof(int);
        if (len < 0 || (size_t)(p - buffer) + (size_t)len > size) goto done;
        pool[i] = copyString(vm, (const char*)p, len);
        p += len;
    }

    if ((size_t)(p - buffer) + sizeof(int) > size) goto done;
    memcpy(&entryIdx, p, sizeof(int));
    p += sizeof(int);
    if (entryIdx < -1 || entryIdx >= pool_count) goto done;
    vm->entry_file = (entryIdx >= 0) ? pool[entryIdx] : NULL;

    ok = deserializeChunkBody(vm, chunk, p, size - (size_t)(p - buffer), pool, pool_count);

done:
    if (pool) FREE_ARRAY(vm, ObjString*, pool, pool_count);
    vm->gc_enabled = gc_was_enabled;
    vm->gc_debt = gc_saved_debt;
    return ok;
}