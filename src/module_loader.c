#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "module_loader.h"
#include "vm.h"
#include "memory.h"
#include "linemap.h"

static char* normalize_path(ZymAllocator* alloc, const char* path) {
    if (!path) return NULL;

    char* path_copy = zym_strdup(alloc, path);
    char* components[256];
    int count = 0;

    char* token = strtok(path_copy, "/\\");
    while (token != NULL && count < 256) {
        if (strcmp(token, "..") == 0) {
            if (count > 0 && strcmp(components[count - 1], "..") != 0) {
                count--;
            } else {
                components[count++] = token;
            }
        } else if (strcmp(token, ".") != 0 && strlen(token) > 0) {
            components[count++] = token;
        }
        token = strtok(NULL, "/\\");
    }

    size_t total_len = 0;
    for (int i = 0; i < count; i++) {
        total_len += strlen(components[i]) + 1;
    }

    char* result = (char*)ZYM_ALLOC(alloc, total_len + 1);
    result[0] = '\0';

    for (int i = 0; i < count; i++) {
        if (i > 0) {
            strcat(result, "/");
        }
        strcat(result, components[i]);
    }

    ZYM_FREE_STR(alloc, path_copy);
    return result;
}

static char* resolve_module_path(ZymAllocator* alloc, const char* base_path, const char* module_path) {
    const char* last_slash = strrchr(base_path, '/');
    const char* last_backslash = strrchr(base_path, '\\');
    const char* separator = last_slash > last_backslash ? last_slash : last_backslash;

    char* combined;
    if (!separator) {
        combined = zym_strdup(alloc, module_path);
    } else {
        size_t base_dir_len = separator - base_path + 1;
        size_t result_len = base_dir_len + strlen(module_path) + 1;
        combined = (char*)ZYM_ALLOC(alloc, result_len);
        memcpy(combined, base_path, base_dir_len);
        strcpy(combined + base_dir_len, module_path);
    }

    char* normalized = normalize_path(alloc, combined);
    ZYM_FREE_STR(alloc, combined);

    return normalized;
}

static unsigned int hash_path(const char* path) {
    unsigned int hash = 5381;
    int c;
    while ((c = *path++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

/* "src/math.zym" -> "src_slash_math_dot_zym" */
static char* encode_path_to_identifier(ZymAllocator* alloc, const char* path) {
    size_t len = strlen(path);
    size_t result_len = 0;
    for (size_t i = 0; i < len; i++) {
        char c = path[i];
        if (c == '/' || c == '\\') result_len += 7;
        else if (c == '.') result_len += 5;
        else if (c == '-') result_len += 6;
        else if (c == ' ') result_len += 7;
        else if (c == ':') result_len += 7;
        else result_len += 1;
    }

    char* result = (char*)ZYM_ALLOC(alloc, result_len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = path[i];
        if (c == '/' || c == '\\') {
            memcpy(result + j, "_slash_", 7);
            j += 7;
        } else if (c == '.') {
            memcpy(result + j, "_dot_", 5);
            j += 5;
        } else if (c == '-') {
            memcpy(result + j, "_dash_", 6);
            j += 6;
        } else if (c == ' ') {
            memcpy(result + j, "_space_", 7);
            j += 7;
        } else if (c == ':') {
            memcpy(result + j, "_colon_", 7);
            j += 7;
        } else {
            result[j++] = c;
        }
    }
    result[j] = '\0';

    return result;
}

/* "src_slash_math_dot_zym" -> "src/math.zym" */
#if 0
static char* decode_identifier_to_path(const char* encoded, int length) {
    char* result = (char*)malloc(length + 1);
    size_t j = 0;

    for (int i = 0; i < length; ) {
        if (i + 7 <= length && memcmp(encoded + i, "_slash_", 7) == 0) {
            result[j++] = '/';
            i += 7;
        } else if (i + 5 <= length && memcmp(encoded + i, "_dot_", 5) == 0) {
            result[j++] = '.';
            i += 5;
        } else if (i + 6 <= length && memcmp(encoded + i, "_dash_", 6) == 0) {
            result[j++] = '-';
            i += 6;
        } else if (i + 7 <= length && memcmp(encoded + i, "_space_", 7) == 0) {
            result[j++] = ' ';
            i += 7;
        } else if (i + 7 <= length && memcmp(encoded + i, "_colon_", 7) == 0) {
            result[j++] = ':';
            i += 7;
        } else {
            result[j++] = encoded[i++];
        }
    }
    result[j] = '\0';

    return result;
}
#endif

typedef struct {
    ZymAllocator* alloc;
    char* buffer;
    size_t length;
    size_t capacity;
} StringBuilder;

static void sb_init(StringBuilder* sb, ZymAllocator* alloc) {
    sb->alloc = alloc;
    sb->capacity = 1024;
    sb->length = 0;
    sb->buffer = (char*)ZYM_ALLOC(alloc, sb->capacity);
    sb->buffer[0] = '\0';
}

static void sb_append(StringBuilder* sb, const char* str) {
    size_t len = strlen(str);
    while (sb->length + len + 1 > sb->capacity) {
        size_t old = sb->capacity;
        sb->capacity *= 2;
        sb->buffer = (char*)ZYM_REALLOC(sb->alloc, sb->buffer, old, sb->capacity);
    }
    memcpy(sb->buffer + sb->length, str, len);
    sb->length += len;
    sb->buffer[sb->length] = '\0';
}

static void sb_append_char(StringBuilder* sb, char c) {
    if (sb->length + 2 > sb->capacity) {
        size_t old = sb->capacity;
        sb->capacity *= 2;
        sb->buffer = (char*)ZYM_REALLOC(sb->alloc, sb->buffer, old, sb->capacity);
    }
    sb->buffer[sb->length++] = c;
    sb->buffer[sb->length] = '\0';
}

static void sb_free(StringBuilder* sb) {
    if (sb->buffer) ZYM_FREE(sb->alloc, sb->buffer, sb->capacity);
    sb->buffer = NULL;
    sb->length = 0;
    sb->capacity = 0;
}

typedef struct {
    ZymAllocator* alloc;
    char** items;
    int count;
    int capacity;
} StringSet;

typedef struct {
    char* symbol;
    char* module_path;
    char* source_file;
} SymbolMapping;

typedef struct {
    ZymAllocator* alloc;
    SymbolMapping* mappings;
    int count;
    int capacity;
} SymbolMap;

static void set_init(StringSet* set, ZymAllocator* alloc) {
    set->alloc = alloc;
    set->capacity = 16;
    set->count = 0;
    set->items = (char**)ZYM_ALLOC(alloc, sizeof(char*) * set->capacity);
}

static bool set_contains(StringSet* set, const char* str) {
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->items[i], str) == 0) {
            return true;
        }
    }
    return false;
}

static void set_add(StringSet* set, const char* str) {
    if (set_contains(set, str)) return;

    if (set->count >= set->capacity) {
        int old = set->capacity;
        set->capacity *= 2;
        set->items = (char**)ZYM_REALLOC(set->alloc, set->items, sizeof(char*) * old, sizeof(char*) * set->capacity);
    }
    set->items[set->count++] = zym_strdup(set->alloc, str);
}

static void set_free(StringSet* set) {
    for (int i = 0; i < set->count; i++) {
        ZYM_FREE_STR(set->alloc, set->items[i]);
    }
    ZYM_FREE(set->alloc, set->items, sizeof(char*) * set->capacity);
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

static void symbolmap_init(SymbolMap* map, ZymAllocator* alloc) {
    map->alloc = alloc;
    map->capacity = 16;
    map->count = 0;
    map->mappings = (SymbolMapping*)ZYM_ALLOC(alloc, sizeof(SymbolMapping) * map->capacity);
}

static bool symbolmap_add(SymbolMap* map, const char* symbol, const char* module_path, const char* source_file, char** error_msg) {
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->mappings[i].symbol, symbol) == 0 &&
            strcmp(map->mappings[i].source_file, source_file) == 0) {
            size_t error_size = 512 + strlen(symbol) + strlen(map->mappings[i].module_path) + strlen(module_path) + strlen(source_file);
            *error_msg = (char*)ZYM_ALLOC(map->alloc, error_size);
            snprintf(*error_msg, error_size,
                "Duplicate import symbol '%s' in [%s]:\n  First imported from: [%s]\n  Duplicate import from: [%s]",
                symbol, source_file, map->mappings[i].module_path, module_path);
            return false;
        }
    }

    if (map->count >= map->capacity) {
        int old = map->capacity;
        map->capacity *= 2;
        map->mappings = (SymbolMapping*)ZYM_REALLOC(map->alloc, map->mappings, sizeof(SymbolMapping) * old, sizeof(SymbolMapping) * map->capacity);
    }
    map->mappings[map->count].symbol = zym_strdup(map->alloc, symbol);
    map->mappings[map->count].module_path = zym_strdup(map->alloc, module_path);
    map->mappings[map->count].source_file = zym_strdup(map->alloc, source_file);
    map->count++;
    return true;
}

static const char* symbolmap_get(SymbolMap* map, const char* symbol, const char* source_file) {
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->mappings[i].symbol, symbol) == 0 &&
            strcmp(map->mappings[i].source_file, source_file) == 0) {
            return map->mappings[i].module_path;
        }
    }
    return NULL;
}

static void symbolmap_free(SymbolMap* map) {
    for (int i = 0; i < map->count; i++) {
        ZYM_FREE_STR(map->alloc, map->mappings[i].symbol);
        ZYM_FREE_STR(map->alloc, map->mappings[i].module_path);
        ZYM_FREE_STR(map->alloc, map->mappings[i].source_file);
    }
    ZYM_FREE(map->alloc, map->mappings, sizeof(SymbolMapping) * map->capacity);
    map->mappings = NULL;
    map->count = 0;
    map->capacity = 0;
}

typedef struct {
    char* module_path;
    char* base_path;
} ModuleQueueItem;

typedef struct {
    ZymAllocator* alloc;
    ModuleQueueItem* items;
    int front;
    int rear;
    int capacity;
} ModuleQueue;

static void queue_init(ModuleQueue* q, ZymAllocator* alloc) {
    q->alloc = alloc;
    q->capacity = 16;
    q->front = 0;
    q->rear = 0;
    q->items = (ModuleQueueItem*)ZYM_ALLOC(alloc, sizeof(ModuleQueueItem) * q->capacity);
}

static bool queue_empty(ModuleQueue* q) {
    return q->front == q->rear;
}

static void queue_push(ModuleQueue* q, const char* module_path, const char* base_path) {
    if ((q->rear + 1) % q->capacity == q->front) {
        int old_cap = q->capacity;
        q->capacity *= 2;
        ModuleQueueItem* new_items = (ModuleQueueItem*)ZYM_ALLOC(q->alloc, sizeof(ModuleQueueItem) * q->capacity);

        int i = 0;
        while (q->front != q->rear) {
            new_items[i++] = q->items[q->front];
            q->front = (q->front + 1) % old_cap;
        }

        ZYM_FREE(q->alloc, q->items, sizeof(ModuleQueueItem) * old_cap);
        q->items = new_items;
        q->front = 0;
        q->rear = i;
    }

    q->items[q->rear].module_path = zym_strdup(q->alloc, module_path);
    q->items[q->rear].base_path = zym_strdup(q->alloc, base_path);
    q->rear = (q->rear + 1) % q->capacity;
}

static ModuleQueueItem queue_pop(ModuleQueue* q) {
    ModuleQueueItem item = {NULL, NULL};
    if (queue_empty(q)) return item;
    item = q->items[q->front];
    q->front = (q->front + 1) % q->capacity;
    return item;
}

static void queue_free(ModuleQueue* q) {
    while (!queue_empty(q)) {
        ModuleQueueItem item = queue_pop(q);
        ZYM_FREE_STR(q->alloc, item.module_path);
        ZYM_FREE_STR(q->alloc, item.base_path);
    }
    ZYM_FREE(q->alloc, q->items, sizeof(ModuleQueueItem) * q->capacity);
    q->items = NULL;
}

typedef struct {
    char* key;
    char* value;
} MapEntry;

typedef struct {
    ZymAllocator* alloc;
    MapEntry* entries;
    int count;
    int capacity;
} StringMap;

typedef struct {
    char* key;
    LineMap* value;
} LineMapEntry;

typedef struct {
    ZymAllocator* alloc;
    LineMapEntry* entries;
    int count;
    int capacity;
} LineMapMap;

static void map_init(StringMap* map, ZymAllocator* alloc) {
    map->alloc = alloc;
    map->capacity = 16;
    map->count = 0;
    map->entries = (MapEntry*)ZYM_ALLOC(alloc, sizeof(MapEntry) * map->capacity);
}

static char* map_get(StringMap* map, const char* key) {
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].key, key) == 0) {
            return map->entries[i].value;
        }
    }
    return NULL;
}

static void map_set(StringMap* map, const char* key, const char* value) {
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].key, key) == 0) {
            ZYM_FREE_STR(map->alloc, map->entries[i].value);
            map->entries[i].value = zym_strdup(map->alloc, value);
            return;
        }
    }

    if (map->count >= map->capacity) {
        int old = map->capacity;
        map->capacity *= 2;
        map->entries = (MapEntry*)ZYM_REALLOC(map->alloc, map->entries, sizeof(MapEntry) * old, sizeof(MapEntry) * map->capacity);
    }

    map->entries[map->count].key = zym_strdup(map->alloc, key);
    map->entries[map->count].value = zym_strdup(map->alloc, value);
    map->count++;
}

static void map_free(StringMap* map) {
    for (int i = 0; i < map->count; i++) {
        ZYM_FREE_STR(map->alloc, map->entries[i].key);
        ZYM_FREE_STR(map->alloc, map->entries[i].value);
    }
    ZYM_FREE(map->alloc, map->entries, sizeof(MapEntry) * map->capacity);
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

static void linemap_map_init(LineMapMap* map, ZymAllocator* alloc) {
    map->alloc = alloc;
    map->capacity = 16;
    map->count = 0;
    map->entries = (LineMapEntry*)ZYM_ALLOC(alloc, sizeof(LineMapEntry) * map->capacity);
}

static LineMap* linemap_map_get(LineMapMap* map, const char* key) {
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].key, key) == 0) {
            return map->entries[i].value;
        }
    }
    return NULL;
}

static void linemap_map_set(LineMapMap* map, const char* key, LineMap* value) {
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].key, key) == 0) {
            map->entries[i].value = value;
            return;
        }
    }

    if (map->count >= map->capacity) {
        int old = map->capacity;
        map->capacity *= 2;
        map->entries = (LineMapEntry*)ZYM_REALLOC(map->alloc, map->entries, sizeof(LineMapEntry) * old, sizeof(LineMapEntry) * map->capacity);
    }

    map->entries[map->count].key = zym_strdup(map->alloc, key);
    map->entries[map->count].value = value;
    map->count++;
}

static void linemap_map_free(VM* vm, LineMapMap* map) {
    for (int i = 0; i < map->count; i++) {
        ZYM_FREE_STR(map->alloc, map->entries[i].key);
        if (map->entries[i].value) {
            freeLineMap(vm, map->entries[i].value);
            ZYM_FREE(map->alloc, map->entries[i].value, sizeof(LineMap));
        }
    }
    ZYM_FREE(map->alloc, map->entries, sizeof(LineMapEntry) * map->capacity);
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

/* Scan for:
 * - import("path")
 * - import symbol from "path"
 */
static bool scan_for_imports(ZymAllocator* alloc, const char* source, const char* base_path, ModuleQueue* queue, SymbolMap* symbol_map, char** error_msg) {
    const char* p = source;

    while (*p) {
        if (strncmp(p, "import", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            p += 6;

            while (*p && isspace((unsigned char)*p)) p++;

            if (*p == '(') {
                p++;

                while (*p && isspace((unsigned char)*p)) p++;

                if (*p == '"') {
                    p++;
                    const char* start = p;

                    while (*p && *p != '"') {
                        if (*p == '\\' && *(p + 1)) {
                            p += 2;
                        } else {
                            p++;
                        }
                    }

                    if (*p == '"') {
                        size_t len = p - start;
                        char* path = (char*)ZYM_ALLOC(alloc, len + 1);
                        memcpy(path, start, len);
                        path[len] = '\0';

                        queue_push(queue, path, base_path);
                        ZYM_FREE(alloc, path, len + 1);

                        p++;
                    }
                }
            }
            else if (isalpha((unsigned char)*p) || *p == '_') {
                const char* symbol_start = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
                    p++;
                }
                size_t symbol_len = p - symbol_start;

                while (*p && isspace((unsigned char)*p)) p++;

                if (strncmp(p, "from", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
                    p += 4;

                    while (*p && isspace((unsigned char)*p)) p++;

                    if (*p == '"') {
                        p++;
                        const char* path_start = p;

                        while (*p && *p != '"') {
                            if (*p == '\\' && *(p + 1)) {
                                p += 2;
                            } else {
                                p++;
                            }
                        }

                        if (*p == '"') {
                            size_t path_len = p - path_start;
                            char* path = (char*)ZYM_ALLOC(alloc, path_len + 1);
                            memcpy(path, path_start, path_len);
                            path[path_len] = '\0';

                            char* symbol = (char*)ZYM_ALLOC(alloc, symbol_len + 1);
                            memcpy(symbol, symbol_start, symbol_len);
                            symbol[symbol_len] = '\0';

                            char* resolved_path = resolve_module_path(alloc, base_path, path);

                            if (!symbolmap_add(symbol_map, symbol, resolved_path, base_path, error_msg)) {
                                ZYM_FREE(alloc, path, path_len + 1);
                                ZYM_FREE(alloc, symbol, symbol_len + 1);
                                ZYM_FREE_STR(alloc, resolved_path);
                                return false;
                            }

                            queue_push(queue, path, base_path);

                            ZYM_FREE(alloc, path, path_len + 1);
                            ZYM_FREE(alloc, symbol, symbol_len + 1);
                            ZYM_FREE_STR(alloc, resolved_path);

                            p++;
                        }
                    }
                }
            }
        } else {
            p++;
        }
    }
    return true;
}

typedef struct {
    ZymAllocator* alloc;
    char** modules;
    int count;
    int capacity;
} ImportStack;

static void stack_init(ImportStack* stack, ZymAllocator* alloc) {
    stack->alloc = alloc;
    stack->capacity = 16;
    stack->count = 0;
    stack->modules = (char**)ZYM_ALLOC(alloc, sizeof(char*) * stack->capacity);
}

static void stack_push(ImportStack* stack, const char* module) {
    if (stack->count >= stack->capacity) {
        int old = stack->capacity;
        stack->capacity *= 2;
        stack->modules = (char**)ZYM_REALLOC(stack->alloc, stack->modules, sizeof(char*) * old, sizeof(char*) * stack->capacity);
    }
    stack->modules[stack->count++] = zym_strdup(stack->alloc, module);
}

static void stack_pop(ImportStack* stack) {
    if (stack->count > 0) {
        stack->count--;
        ZYM_FREE_STR(stack->alloc, stack->modules[stack->count]);
    }
}

static bool stack_contains(ImportStack* stack, const char* module) {
    for (int i = 0; i < stack->count; i++) {
        if (strcmp(stack->modules[i], module) == 0) {
            return true;
        }
    }
    return false;
}

#if 0
static char* stack_to_string(ImportStack* stack, const char* new_module) {
    size_t total_len = 0;
    for (int i = 0; i < stack->count; i++) {
        total_len += strlen(stack->modules[i]) + 4; // " -> "
    }
    total_len += strlen(new_module) + 4; // " -> " + new_module
    total_len += strlen(stack->modules[0]) + 1; // " -> " + first module again (to complete cycle)

    char* result = (char*)ZYM_ALLOC(stack->alloc, total_len);
    result[0] = '\0';

    for (int i = 0; i < stack->count; i++) {
        if (i > 0) strcat(result, " -> ");
        strcat(result, stack->modules[i]);
    }
    strcat(result, " -> ");
    strcat(result, new_module);

    return result;
}
#endif

static void stack_free(ImportStack* stack) {
    for (int i = 0; i < stack->count; i++) {
        ZYM_FREE_STR(stack->alloc, stack->modules[i]);
    }
    ZYM_FREE(stack->alloc, stack->modules, sizeof(char*) * stack->capacity);
    stack->modules = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

static bool load_module_recursive(
    VM* vm,
    const char* module_path,
    ModuleReadCallback read_callback,
    void* user_data,
    StringSet* loaded_modules,
    ImportStack* import_stack,
    StringMap* module_sources,
    LineMapMap* module_linemaps,
    SymbolMap* symbol_map,
    char** error_msg)
{
    if (stack_contains(import_stack, module_path)) {
        int cycle_start = -1;
        for (int i = 0; i < import_stack->count; i++) {
            if (strcmp(import_stack->modules[i], module_path) == 0) {
                cycle_start = i;
                break;
            }
        }

        size_t error_size = 512;
        for (int i = cycle_start; i < import_stack->count; i++) {
            error_size += strlen(import_stack->modules[i]) + 50;
        }
        error_size += strlen(module_path) + 200;

        *error_msg = (char*)ZYM_ALLOC(&vm->allocator, error_size);
        char* ptr = *error_msg;

        ptr += sprintf(ptr, "Circular import detected:\n\n");

        for (int i = cycle_start; i < import_stack->count; i++) {
            for (int j = 0; j < (i - cycle_start); j++) {
                ptr += sprintf(ptr, "  ");
            }
            if (i == cycle_start) {
                ptr += sprintf(ptr, "  [%s]\n", import_stack->modules[i]);
            } else {
                ptr += sprintf(ptr, "  `--> [%s]\n", import_stack->modules[i]);
            }
        }

        for (int j = 0; j < (import_stack->count - cycle_start); j++) {
            ptr += sprintf(ptr, "  ");
        }
        ptr += sprintf(ptr, "  `--> [%s] <-- ERROR: Already importing (creates cycle)\n", module_path);

        return false;
    }

    if (set_contains(loaded_modules, module_path)) {
        return true;
    }

    stack_push(import_stack, module_path);

    ModuleReadResult read_result = read_callback(module_path, user_data);
    if (!read_result.source) {
        size_t error_size = 256 + strlen(module_path);
        *error_msg = (char*)ZYM_ALLOC(&vm->allocator, error_size);
        snprintf(*error_msg, error_size, "Failed to read/preprocess module: [%s]", module_path);
        stack_pop(import_stack);
        return false;
    }

    map_set(module_sources, module_path, read_result.source);
    linemap_map_set(module_linemaps, module_path, read_result.line_map);
    set_add(loaded_modules, module_path);

    ModuleQueue deps;
    queue_init(&deps, &vm->allocator);
    if (!scan_for_imports(&vm->allocator, read_result.source, module_path, &deps, symbol_map, error_msg)) {
        queue_free(&deps);
        stack_pop(import_stack);
        return false;
    }

    while (!queue_empty(&deps)) {
        ModuleQueueItem item = queue_pop(&deps);
        char* resolved_path = resolve_module_path(&vm->allocator, item.base_path, item.module_path);

        if (!load_module_recursive(vm, resolved_path, read_callback, user_data,
                                   loaded_modules, import_stack, module_sources,
                                   module_linemaps, symbol_map, error_msg)) {
            ZYM_FREE_STR(&vm->allocator, resolved_path);
            ZYM_FREE_STR(&vm->allocator, item.module_path);
            ZYM_FREE_STR(&vm->allocator, item.base_path);
            queue_free(&deps);
            stack_pop(import_stack);
            return false;
        }

        ZYM_FREE_STR(&vm->allocator, resolved_path);
        ZYM_FREE_STR(&vm->allocator, item.module_path);
        ZYM_FREE_STR(&vm->allocator, item.base_path);
    }

    queue_free(&deps);

    stack_pop(import_stack);
    return true;
}

#if 0
static int count_newlines(const char* str) {
    int count = 0;
    while (*str) {
        if (*str == '\n') count++;
        str++;
    }
    return count;
}
#endif

static void add_mapped_lines(VM* vm, const char* text, LineMap* source_linemap, LineMap* combined_linemap, int* source_line_idx) {
    const char* p = text;
    while (*p) {
        if (*p == '\n') {
            int original_line = 0;
            if (source_linemap && *source_line_idx < source_linemap->count) {
                original_line = source_linemap->lines[*source_line_idx];
            }
            (*source_line_idx)++;

            addLineMapping(vm, combined_linemap, original_line);
        }
        p++;
    }
}

static bool is_fresh_module(const char* source) {
    const char* p = source;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return strncmp(p, "\"use fresh\"", 11) == 0;
}

static char* transform_imports(ZymAllocator* alloc, const char* source, const char* base_path, StringSet* loaded_modules,
                               SymbolMap* symbol_map, bool debug_names, StringSet* fresh_modules) {
    StringBuilder sb;
    sb_init(&sb, alloc);

    const char* p = source;
    const char* last = source;

    while (*p) {
        if (strncmp(p, "import", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            const char* import_start = p;

            while (last < import_start) {
                sb_append_char(&sb, *last++);
            }

            p += 6;

            while (*p && isspace((unsigned char)*p)) p++;

            if (isalpha((unsigned char)*p) || *p == '_') {
                const char* symbol_start = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
                    p++;
                }

                while (*p && isspace((unsigned char)*p)) p++;

                if (strncmp(p, "from", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
                    p += 4;

                    while (*p && isspace((unsigned char)*p)) p++;

                    if (*p == '"') {
                        p++;
                        while (*p && *p != '"') {
                            if (*p == '\\' && *(p + 1)) {
                                p += 2;
                            } else {
                                p++;
                            }
                        }
                        if (*p == '"') {
                            p++;
                        }

                        while (*p && isspace((unsigned char)*p)) p++;
                        if (*p == ';') p++;

                        last = p;
                        continue;
                    }
                }
            }

            if (*p == '(') {
                p++;

                while (*p && isspace((unsigned char)*p)) p++;

                if (*p == '"') {
                    p++;
                    const char* start = p;

                    while (*p && *p != '"') {
                        if (*p == '\\' && *(p + 1)) {
                            p += 2;
                        } else {
                            p++;
                        }
                    }

                    if (*p == '"') {
                        size_t len = p - start;
                        char* path = (char*)ZYM_ALLOC(alloc, len + 1);
                        memcpy(path, start, len);
                        path[len] = '\0';

                        p++;

                        while (*p && isspace((unsigned char)*p)) p++;

                        if (*p == ')') {
                            p++;

                            char* resolved = resolve_module_path(alloc, base_path, path);

                            if (debug_names) {
                                sb_append(&sb, "__module_");
                                char* encoded = encode_path_to_identifier(alloc, resolved);
                                sb_append(&sb, encoded);
                                ZYM_FREE_STR(alloc, encoded);
                            } else {
                                char hash_str[16];
                                snprintf(hash_str, sizeof(hash_str), "_%x", hash_path(resolved));
                                sb_append(&sb, hash_str);
                            }
                            if (!fresh_modules || set_contains(fresh_modules, resolved)) {
                                sb_append(&sb, "()");
                            }

                            ZYM_FREE_STR(alloc, resolved);
                            last = p;
                        }

                        ZYM_FREE(alloc, path, len + 1);
                    }
                }
            }
        }
        else if (isalpha((unsigned char)*p) || *p == '_') {
            const char* symbol_start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
                p++;
            }
            size_t symbol_len = p - symbol_start;

            while (*p && isspace((unsigned char)*p)) p++;

            if (*p == '(' && *(p + 1) == ')') {
                char* symbol = (char*)ZYM_ALLOC(alloc, symbol_len + 1);
                memcpy(symbol, symbol_start, symbol_len);
                symbol[symbol_len] = '\0';

                const char* module_path = symbolmap_get(symbol_map, symbol, base_path);
                if (module_path) {
                    while (last < symbol_start) {
                        sb_append_char(&sb, *last++);
                    }

                    if (debug_names) {
                        sb_append(&sb, "__module_");
                        char* encoded = encode_path_to_identifier(alloc, module_path);
                        sb_append(&sb, encoded);
                        ZYM_FREE_STR(alloc, encoded);
                    } else {
                        char hash_str[16];
                        snprintf(hash_str, sizeof(hash_str), "_%x", hash_path(module_path));
                        sb_append(&sb, hash_str);
                    }
                    if (!fresh_modules || set_contains(fresh_modules, module_path)) {
                        sb_append(&sb, "()");
                    }

                    p += 2;
                    last = p;
                }

                ZYM_FREE(alloc, symbol, symbol_len + 1);
            }
        } else {
            p++;
        }
    }

    while (*last) {
        sb_append_char(&sb, *last++);
    }

    char* result = sb.buffer;
    sb.buffer = NULL;
    sb_free(&sb);

    return result;
}

ModuleLoadResult* loadModules(
    VM* vm,
    const char* entry_source,
    LineMap* entry_line_map,
    const char* entry_path,
    ModuleReadCallback read_callback,
    void* user_data,
    bool debug_names,
    bool write_debug_output,
    const char* debug_output_path)
{
    ZymAllocator* alloc = &vm->allocator;
    ModuleLoadResult* result = (ModuleLoadResult*)ZYM_ALLOC(alloc, sizeof(ModuleLoadResult));
    result->combined_source = NULL;
    result->line_map = NULL;
    result->module_paths = NULL;
    result->module_count = 0;
    result->has_error = false;
    result->error_message = NULL;

    StringSet loaded_modules;
    set_init(&loaded_modules, alloc);

    StringMap module_sources;
    map_init(&module_sources, alloc);

    LineMapMap module_linemaps;
    linemap_map_init(&module_linemaps, alloc);

    ImportStack import_stack;
    stack_init(&import_stack, alloc);

    SymbolMap symbol_map;
    symbolmap_init(&symbol_map, alloc);

    StringSet fresh_modules;
    set_init(&fresh_modules, alloc);

    set_add(&loaded_modules, entry_path);
    map_set(&module_sources, entry_path, entry_source);

    ModuleQueue entry_deps;
    queue_init(&entry_deps, alloc);
    if (!scan_for_imports(alloc, entry_source, entry_path, &entry_deps, &symbol_map, &result->error_message)) {
        result->has_error = true;
        queue_free(&entry_deps);
        goto cleanup;
    }

    while (!queue_empty(&entry_deps)) {
        ModuleQueueItem item = queue_pop(&entry_deps);
        char* resolved_path = resolve_module_path(alloc, item.base_path, item.module_path);

        if (!load_module_recursive(vm, resolved_path, read_callback, user_data,
                                   &loaded_modules, &import_stack, &module_sources,
                                   &module_linemaps, &symbol_map, &result->error_message)) {
            result->has_error = true;
            ZYM_FREE_STR(alloc, resolved_path);
            ZYM_FREE_STR(alloc, item.module_path);
            ZYM_FREE_STR(alloc, item.base_path);
            queue_free(&entry_deps);
            goto cleanup;
        }

        ZYM_FREE_STR(alloc, resolved_path);
        ZYM_FREE_STR(alloc, item.module_path);
        ZYM_FREE_STR(alloc, item.base_path);
    }

    queue_free(&entry_deps);

    StringBuilder combined;
    sb_init(&combined, alloc);

    LineMap* combined_linemap = (LineMap*)ZYM_ALLOC(alloc, sizeof(LineMap));
    initLineMap(combined_linemap);

    for (int i = 0; i < loaded_modules.count; i++) {
        const char* module_path = loaded_modules.items[i];

        if (strcmp(module_path, entry_path) == 0) {
            continue;
        }

        char* source = map_get(&module_sources, module_path);
        LineMap* source_linemap = linemap_map_get(&module_linemaps, module_path);
        if (!source) continue;

        bool is_fresh = is_fresh_module(source);
        if (is_fresh) {
            set_add(&fresh_modules, module_path);
        }

        if (debug_names) {
            char* encoded = encode_path_to_identifier(alloc, module_path);
            if (is_fresh) {
                sb_append(&combined, "func __module_");
                sb_append(&combined, encoded);
            } else {
                sb_append(&combined, "func __module_");
                sb_append(&combined, encoded);
                sb_append(&combined, "_init");
            }
            ZYM_FREE_STR(alloc, encoded);
        } else {
            char hash_str[32];
            if (is_fresh) {
                snprintf(hash_str, sizeof(hash_str), "func _%x", hash_path(module_path));
            } else {
                snprintf(hash_str, sizeof(hash_str), "func _%x_init", hash_path(module_path));
            }
            sb_append(&combined, hash_str);
        }

        sb_append(&combined, "() {\n");
        addLineMapping(vm, combined_linemap, 0);

        char* transformed = transform_imports(alloc, source, module_path, &loaded_modules, &symbol_map, debug_names, &fresh_modules);

        int source_line_idx = 0;
        add_mapped_lines(vm, transformed, source_linemap, combined_linemap, &source_line_idx);

        sb_append(&combined, transformed);
        ZYM_FREE_STR(alloc, transformed);

        sb_append(&combined, "\n}\n");
        addLineMapping(vm, combined_linemap, 0);
        addLineMapping(vm, combined_linemap, 0);

        if (!is_fresh) {
            if (debug_names) {
                char* encoded = encode_path_to_identifier(alloc, module_path);
                sb_append(&combined, "var __module_");
                sb_append(&combined, encoded);
                sb_append(&combined, " = __module_");
                sb_append(&combined, encoded);
                sb_append(&combined, "_init()\n");
                ZYM_FREE_STR(alloc, encoded);
            } else {
                char hash_str[64];
                snprintf(hash_str, sizeof(hash_str), "var _%x = _%x_init()\n",
                         hash_path(module_path), hash_path(module_path));
                sb_append(&combined, hash_str);
            }
            addLineMapping(vm, combined_linemap, 0);
        }

        sb_append(&combined, "\n");
        addLineMapping(vm, combined_linemap, 0);
    }

    char* transformed_entry = transform_imports(alloc, entry_source, entry_path, &loaded_modules, &symbol_map, debug_names, &fresh_modules);

    int entry_line_idx = 0;
    add_mapped_lines(vm, transformed_entry, entry_line_map, combined_linemap, &entry_line_idx);

    sb_append(&combined, transformed_entry);
    ZYM_FREE_STR(alloc, transformed_entry);

    result->combined_source = combined.buffer;
    result->line_map = combined_linemap;
    combined.buffer = NULL;
    sb_free(&combined);

    if (write_debug_output && debug_output_path != NULL && result->combined_source != NULL) {
        FILE* debug_file = fopen(debug_output_path, "w");
        if (debug_file != NULL) {
            fprintf(debug_file, "// ===== Module Loader Debug Output =====\n");
            fprintf(debug_file, "// Entry: %s\n", entry_path);
            fprintf(debug_file, "// Loaded %d module(s):\n", loaded_modules.count);
            for (int i = 0; i < loaded_modules.count; i++) {
                fprintf(debug_file, "//   - %s\n", loaded_modules.items[i]);
            }
            fprintf(debug_file, "// =======================================\n\n");
            fprintf(debug_file, "%s", result->combined_source);
            fclose(debug_file);
        }
    }

    result->module_count = loaded_modules.count;
    result->module_paths = (char**)ZYM_ALLOC(alloc, sizeof(char*) * result->module_count);
    for (int i = 0; i < loaded_modules.count; i++) {
        result->module_paths[i] = zym_strdup(alloc, loaded_modules.items[i]);
    }

cleanup:
    set_free(&loaded_modules);
    map_free(&module_sources);
    linemap_map_free(vm, &module_linemaps);
    stack_free(&import_stack);
    symbolmap_free(&symbol_map);
    set_free(&fresh_modules);

    return result;
}

void freeModuleLoadResult(VM* vm, ModuleLoadResult* result) {
    if (!result) return;
    ZymAllocator* alloc = &vm->allocator;

    ZYM_FREE_STR(alloc, result->combined_source);

    if (result->line_map) {
        freeLineMap(vm, result->line_map);
        ZYM_FREE(alloc, result->line_map, sizeof(LineMap));
    }

    for (int i = 0; i < result->module_count; i++) {
        ZYM_FREE_STR(alloc, result->module_paths[i]);
    }
    ZYM_FREE(alloc, result->module_paths, sizeof(char*) * result->module_count);

    ZYM_FREE_STR(alloc, result->error_message);
    ZYM_FREE(alloc, result, sizeof(ModuleLoadResult));
}
