#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "./object.h"
#include "./memory.h"
#include "./vm.h"
#include "./table.h"
#include "./gc.h"
#include "./utf8.h"

#define ALLOCATE_OBJ(vm, type, objectType) (type*)allocateObject(vm, sizeof(type), objectType)

Obj* allocateObject(VM* vm, size_t size, ObjType type) {
    Obj* object = (Obj*)reallocate(vm, NULL, 0, size);
    object->type = type;
    object->is_marked = false;

    object->next = vm->objects;
    vm->objects = object;

#ifdef ZYM_HEAP_CENSUS
    zymCensusObject((int)type, size);
    object->census_birth = zym_census_epoch;
#endif

    return object;
}

// Allocate the string as ONE block (header + content tail) and intern it.
// `src` is a borrowed, non-GC buffer: allocateObject may collect, but plain
// heap memory is never moved or freed by the GC, so copying after the
// allocation is safe. The free path in gc.c must mirror this size.
static ObjString* allocateString(VM* vm, const char* src, int byte_length, uint32_t hash) {
    ObjString* string = (ObjString*)allocateObject(vm,
        sizeof(ObjString) + (size_t)byte_length + 1, OBJ_STRING);
    string->byte_length = byte_length;
    string->hash = hash;
    memcpy(string->chars, src, (size_t)byte_length);
    string->chars[byte_length] = '\0';
    string->length = utf8_strlen(string->chars, byte_length);

    pushTempRoot(vm, (Obj*)string);
    tableSet(vm, &vm->strings, string, NULL_VAL);
    popTempRoot(vm);
    return string;
}

// ---- String hashing (see object.h) ----------------------------------------
// Bulk path: wyhash-style 64x64->128 multiply-fold per 8-byte word, seeded
// with the length. The tail (0..7 bytes) is folded through the same mixer.
// A streaming caller feeds words in the same order the flat caller reads
// them, so the two agree bit for bit; the tail is assembled identically.
#ifdef __SIZEOF_INT128__
static inline uint64_t hash_mix64(uint64_t a, uint64_t b) {
    unsigned __int128 r = (unsigned __int128)a * b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}
#else
// No __int128 on 32-bit targets (RV32/Xtensa MCUs). Schoolbook 64x64->128
// from 32x32->64 halves; must stay bit-identical to the wide path, since
// interned hashes travel across platforms inside serialized chunks.
static inline uint64_t hash_mix64(uint64_t a, uint64_t b) {
    uint32_t a_lo = (uint32_t)a, a_hi = (uint32_t)(a >> 32);
    uint32_t b_lo = (uint32_t)b, b_hi = (uint32_t)(b >> 32);
    uint64_t ll = (uint64_t)a_lo * b_lo;
    uint64_t lh = (uint64_t)a_lo * b_hi;
    uint64_t hl = (uint64_t)a_hi * b_lo;
    uint64_t hh = (uint64_t)a_hi * b_hi;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    uint64_t lo = (mid << 32) | (uint32_t)ll;
    uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    return lo ^ hi;
}
#endif
#define HASH_P0 0xa0761d6478bd642full
#define HASH_P1 0xe7037ed1a0b428dbull
#define HASH_SEED0 0x9e3779b97f4a7c15ull

static inline uint32_t hash_fnv_flat(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

// Tail word for the last `rem` (0..7) bytes, identical whether the bytes
// arrive flat or staged: little-endian packing of the bytes in order.
static inline uint64_t hash_tail_word(const uint8_t* p, int rem) {
    uint64_t t = 0;
    for (int i = 0; i < rem; i++) t |= (uint64_t)p[i] << (8 * i);
    return t;
}

static inline uint32_t hash_bulk_finish(uint64_t seed, uint64_t tail_word, int rem) {
    // Mix the tail (length of tail folded in so "abc"+"" and "ab"+"c"
    // agree only through identical byte sequences, never through
    // coincidental word equality).
    seed = hash_mix64(tail_word ^ HASH_P0 ^ (uint64_t)rem, seed ^ HASH_P1);
    return (uint32_t)hash_mix64(seed, HASH_P1);
}

uint32_t zymHashString(const char* key, int length) {
    if (length < ZYM_HASH_BULK_MIN) return hash_fnv_flat(key, length);
    uint64_t seed = HASH_SEED0 ^ (uint64_t)length;
    const uint8_t* p = (const uint8_t*)key;
    int rem = length;
    while (rem >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        seed = hash_mix64(w ^ HASH_P0, seed ^ HASH_P1);
        p += 8; rem -= 8;
    }
    return hash_bulk_finish(seed, hash_tail_word(p, rem), rem);
}

void zymHashInit(ZymHashStream* h, int total_length) {
    h->expected = total_length;
    h->total = 0;
    h->staged = 0;
    h->stage = 0;
    h->fnv = 2166136261u;
    h->seed = HASH_SEED0 ^ (uint64_t)total_length;
}

void zymHashFeed(ZymHashStream* h, const char* bytes, int length) {
    const uint8_t* p = (const uint8_t*)bytes;
    if (h->expected < ZYM_HASH_BULK_MIN) {
        for (int i = 0; i < length; i++) { h->fnv ^= p[i]; h->fnv *= 16777619; }
        h->total += length;
        return;
    }
    int i = 0;
    // Fill a partially staged word first.
    while (h->staged > 0 && h->staged < 8 && i < length) {
        h->stage |= (uint64_t)p[i] << (8 * h->staged);
        h->staged++; i++;
    }
    if (h->staged == 8) {
        h->seed = hash_mix64(h->stage ^ HASH_P0, h->seed ^ HASH_P1);
        h->staged = 0; h->stage = 0;
    }
    // Whole words straight from the input.
    while (length - i >= 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h->seed = hash_mix64(w ^ HASH_P0, h->seed ^ HASH_P1);
        i += 8;
    }
    // Stage the remainder.
    while (i < length) {
        h->stage |= (uint64_t)p[i] << (8 * h->staged);
        h->staged++; i++;
    }
    h->total += length;
}

uint32_t zymHashFinish(ZymHashStream* h) {
    if (h->expected < ZYM_HASH_BULK_MIN) return h->fnv;
    // Anything still staged is exactly the flat path's tail (< 8 bytes,
    // because a full stage word was mixed the moment it filled).
    return hash_bulk_finish(h->seed, h->stage, h->staged);
}

static uint32_t hashString(const char* key, int length) {
    return zymHashString(key, length);
}

ObjString* takeString(VM* vm, char* chars, int length) {
    uint32_t hash = hashString(chars, length);
    ObjString* interned = tableFindString(&vm->strings, chars, length, hash);
    if (interned != NULL) {
        reallocate(vm, chars, length + 1, 0);
        return interned;
    }

    // Content is copied into the string's tail; the donated buffer (always
    // a vm-accounted allocation of length+1, per this function's contract)
    // is released either way.
    ObjString* result = allocateString(vm, chars, length, hash);
    reallocate(vm, chars, length + 1, 0);
    return result;
}

ObjString* copyString(VM* vm, const char* chars, int length) {
    uint32_t hash = hashString(chars, length);
    ObjString* interned = tableFindString(&vm->strings, chars, length, hash);
    if (interned != NULL) {
        return interned;
    }

    // No intermediate heap copy anymore — the single allocation in
    // allocateString IS the copy.
    return allocateString(vm, chars, length, hash);
}

ObjString* concatStringsN(VM* vm, Value* parts, int count) {
    // Bounded scratch: CONCAT_N's count is an 8-bit operand, so 255 is the
    // hard ceiling and a stack array is fine (no allocation before we know
    // the intern probe missed).
    ObjString* strs[256];
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        ObjString* s = AS_STRING(parts[i]);
        strs[i] = s;
        total += (size_t)s->byte_length;
    }
    if (total > (size_t)INT32_MAX - 1) {
        runtimeError(vm, "Concatenation result too large.");
        return NULL;
    }
    int byte_length = (int)total;

    // Stream the hash across the pieces: bit-identical to zymHashString
    // over the joined bytes, so the intern probe below finds a string
    // built any other way.
    ZymHashStream hs;
    zymHashInit(&hs, byte_length);
    for (int i = 0; i < count; i++) {
        zymHashFeed(&hs, strs[i]->chars, strs[i]->byte_length);
    }
    uint32_t hash = zymHashFinish(&hs);

    ObjString* interned = tableFindStringParts(&vm->strings, strs, count, byte_length, hash);
    if (interned != NULL) return interned;

    // Parts are reachable from the caller's registers (they are the operand
    // window), and strings never move, so the pointers survive the allocation.
    ObjString* string = (ObjString*)allocateObject(vm,
        sizeof(ObjString) + (size_t)byte_length + 1, OBJ_STRING);
    string->byte_length = byte_length;
    string->hash = hash;
    char* p = string->chars;
    for (int i = 0; i < count; i++) {
        memcpy(p, strs[i]->chars, (size_t)strs[i]->byte_length);
        p += strs[i]->byte_length;
    }
    *p = '\0';
    string->length = utf8_strlen(string->chars, byte_length);

    pushTempRoot(vm, (Obj*)string);
    tableSet(vm, &vm->strings, string, NULL_VAL);
    popTempRoot(vm);
    return string;
}

ObjString* concatStrings(VM* vm, ObjString* a, ObjString* b) {
    // Buffer-free concat: stream the hash across both halves, probe the
    // intern table with a two-segment compare, and only on a miss write
    // the content directly into the new string's tail. One allocation,
    // two memcpys, no temporary. GC safety: `a` and `b` are reachable
    // from the caller's stack slots and strings never move, so the
    // pointers stay valid across the allocation.
    ZymHashStream hs;
    zymHashInit(&hs, a->byte_length + b->byte_length);
    zymHashFeed(&hs, a->chars, a->byte_length);
    zymHashFeed(&hs, b->chars, b->byte_length);
    uint32_t hash = zymHashFinish(&hs);

    ObjString* interned = tableFindStringPair(&vm->strings,
        a->chars, a->byte_length, b->chars, b->byte_length, hash);
    if (interned != NULL) return interned;

    int byte_length = a->byte_length + b->byte_length;
    ObjString* string = (ObjString*)allocateObject(vm,
        sizeof(ObjString) + (size_t)byte_length + 1, OBJ_STRING);
    string->byte_length = byte_length;
    string->hash = hash;
    memcpy(string->chars, a->chars, (size_t)a->byte_length);
    memcpy(string->chars + a->byte_length, b->chars, (size_t)b->byte_length);
    string->chars[byte_length] = '\0';
    string->length = utf8_strlen(string->chars, byte_length);

    pushTempRoot(vm, (Obj*)string);
    tableSet(vm, &vm->strings, string, NULL_VAL);
    popTempRoot(vm);
    return string;
}

ObjFunction* newFunction(VM* vm) {
    ObjFunction* function = (ObjFunction*)allocateObject(vm, sizeof(ObjFunction), OBJ_FUNCTION);

    function->arity = 0;
    function->fixed_arity = 0;
    function->is_variadic = false;
    function->upvalue_count = 0;
    function->upvalue_capacity = 0;
    function->upvalues = NULL;
    function->max_regs = 1;
    function->spill_count = 0;
    function->name = NULL;
    function->module_name = NULL;
    initChunk(&function->chunk);

    return function;
}

ObjNativeFunction* newNativeFunction(VM* vm, ObjString* name, int arity, void* func_ptr, NativeDispatcher dispatcher) {
    ObjNativeFunction* native = (ObjNativeFunction*)allocateObject(vm, sizeof(ObjNativeFunction), OBJ_NATIVE_FUNCTION);

    native->name = name;
    native->arity = arity;
    native->func_ptr = func_ptr;
    native->dispatcher = dispatcher;
    native->variadic_dispatcher = NULL;
    native->is_variadic = false;
    return native;
}

ObjNativeContext* newNativeContext(VM* vm, void* native_data, NativeFinalizerFunc finalizer) {
    ObjNativeContext* context = (ObjNativeContext*)allocateObject(vm, sizeof(ObjNativeContext), OBJ_NATIVE_CONTEXT);
    context->native_data = native_data;
    context->finalizer = finalizer;
    return context;
}

ObjNativeClosure* newNativeClosure(VM* vm, ObjString* name, int arity, void* func_ptr, NativeDispatcher dispatcher, Value context) {
    ObjNativeClosure* closure = (ObjNativeClosure*)allocateObject(vm, sizeof(ObjNativeClosure), OBJ_NATIVE_CLOSURE);

    closure->name = name;
    closure->arity = arity;
    closure->func_ptr = func_ptr;
    closure->dispatcher = dispatcher;
    closure->variadic_dispatcher = NULL;
    closure->context = context;
    closure->is_variadic = false;
    return closure;
}


ObjClosure* newClosure(VM* vm, ObjFunction* function) {
    // GC SAFETY: `function` is reached from the caller's C stack only, and
    // the allocation below can trigger GC before the closure exists. The
    // compiler emits OP(CLOSURE) reading the function out of
    // `chunk->constants`, which is marked via `markChunk`, so this is
    // usually safe — but we root it here defensively to make the ownership
    // model independent of every call site's discipline.
    pushTempRoot(vm, (Obj*)function);

    // Single allocation: header + upvalue pointer array in one block (the
    // array is a flexible member on the closure's tail). The free path in
    // gc.c must mirror this size for the bytes_allocated accounting.
    int count = function->upvalue_count;
    ObjClosure* closure = (ObjClosure*)allocateObject(vm,
        sizeof(ObjClosure) + sizeof(ObjUpvalue*) * (size_t)count, OBJ_CLOSURE);
    // CRITICAL: `upvalue_count` must be set before any allocation that
    // could trigger GC; otherwise the closure is reachable (via
    // vm->objects) with garbage in the fields `blackenObject(OBJ_CLOSURE)`
    // dereferences. No allocation happens between these stores today, but
    // be explicit so a future edit can't accidentally introduce one.
    closure->function = function;
    closure->upvalue_count = count;
    for (int i = 0; i < count; i++) {
        closure->upvalues[i] = NULL;
    }

    popTempRoot(vm);
    return closure;
}

ObjList* newList(VM* vm) {
    ObjList* list = ALLOCATE_OBJ(vm, ObjList, OBJ_LIST);
    initValueArray(&list->items);
    return list;
}

ObjMap* newMap(VM* vm) {
    ObjMap* map = ALLOCATE_OBJ(vm, ObjMap, OBJ_MAP);
    initTable(&map->table);
    return map;
}

ObjDispatcher* newDispatcher(VM* vm) {
    ObjDispatcher* dispatcher = ALLOCATE_OBJ(vm, ObjDispatcher, OBJ_DISPATCHER);
    dispatcher->count = 0;
    dispatcher->variadic_fallback = NULL;
    dispatcher->variadic_min_arity = 0;
    for (int i = 0; i < MAX_OVERLOADS; i++) {
        dispatcher->overloads[i] = NULL;
    }
    return dispatcher;
}


ObjStructSchema* newStructSchema(VM* vm, ObjString* name, ObjString** field_names, int field_count) {
    ObjStructSchema* schema = ALLOCATE_OBJ(vm, ObjStructSchema, OBJ_STRUCT_SCHEMA);

    schema->name = name;
    schema->field_count = field_count;
    schema->field_names = field_names;

    return schema;
}

ObjStructInstance* newStructInstance(VM* vm, ObjStructSchema* schema) {
    // Single allocation: instance header + fields array in one contiguous block.
    // Eliminates a second heap allocation and improves cache locality.
    size_t size = sizeof(ObjStructInstance) + sizeof(Value) * schema->field_count;
    ObjStructInstance* instance = (ObjStructInstance*)allocateObject(vm, size, OBJ_STRUCT_INSTANCE);

    instance->schema = schema;
    instance->field_count = schema->field_count;
    // Fields are stored immediately after the struct header
    instance->fields = (Value*)(instance + 1);

    for (int i = 0; i < schema->field_count; i++) {
        instance->fields[i] = NULL_VAL;
    }

    return instance;
}

ObjEnumSchema* newEnumSchema(VM* vm, ObjString* name, ObjString** variant_names, int variant_count) {
    ObjEnumSchema* schema = ALLOCATE_OBJ(vm, ObjEnumSchema, OBJ_ENUM_SCHEMA);
    schema->name = name;
    schema->variant_count = variant_count;
    schema->variant_names = variant_names;
    schema->type_id = vm->next_enum_type_id++;
    return schema;
}

void printObject(Value value) {
    switch (OBJ_TYPE(value)) {
        case OBJ_STRING:
            printf("%s", AS_CSTRING(value));
            break;
        case OBJ_LIST: {
                ObjList* list = AS_LIST(value);
                printf("[");
                for (int i = 0; i < list->items.count; i++) {
                    printValue(NULL, list->items.values[i]);
                    if (i < list->items.count - 1) {
                        printf(", ");
                    }
                }
                printf("]");
                break;
        }
        case OBJ_DISPATCHER:
            printf("<overloaded function>");
            break;
        case OBJ_STRUCT_SCHEMA: {
            ObjStructSchema* schema = AS_STRUCT_SCHEMA(value);
            printf("<struct %s>", schema->name->chars);
            break;
        }
        case OBJ_STRUCT_INSTANCE: {
            ObjStructInstance* instance = AS_STRUCT_INSTANCE(value);
            printf("%s {", instance->schema->name->chars);
            for (int i = 0; i < instance->schema->field_count; i++) {
                if (i > 0) printf(", ");
                printf("%s: ", instance->schema->field_names[i]->chars);
                printValue(NULL, instance->fields[i]);
            }
            printf("}");
            break;
        }
        case OBJ_ENUM_SCHEMA: {
            ObjEnumSchema* schema = AS_ENUM_SCHEMA(value);
            printf("<enum %s>", schema->name->chars);
            break;
        }
        default:
            printf("<unknown object>");
            break;
    }
}