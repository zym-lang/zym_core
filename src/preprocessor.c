#include "./preprocessor.h"
#include "./utils.h"
#include "./memory.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ===========================================================
// Internal helpers / data structures
// ===========================================================

typedef struct {
    char* name;
    char* body;
    char** params;
    int param_count;
    bool is_function;
} Macro;

typedef struct {
    char* key;
    Macro* val;
    bool used;
    bool tombstone;
} HTEntry;

typedef struct {
    int count;
    int capacity;
    HTEntry* entries;
} HashTable;

typedef struct {
    VM* vm;

    const char* source;
    const char* cur;
    int line;

    HashTable macros;
    ConditionalStack cond;

    // recursion guard
    char** active;
    int active_count;
    int active_cap;

    bool had_error;
} Preprocessor;

// ---------------- Memory wrappers ----------------

static void* xmalloc(VM* vm, size_t n) {
    return reallocate(vm, NULL, 0, n);
}

static char* xstrdup(VM* vm, const char* s) {
    size_t n = strlen(s);
    char* r = (char*)reallocate(vm, NULL, 0, n + 1);
    memcpy(r, s, n + 1);
    return r;
}
static void xfree_any(VM* vm, void* p) {
    (void)reallocate(vm, p, 0, 0);
}

// ---------------- Hashing ----------------
static uint32_t fnv1a(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t hash_cstr(const char* s) { return fnv1a(s, strlen(s)); }

// ---------------- HashTable ----------------
static void ht_init(VM* vm, HashTable* ht) {
    ht->count = 0;
    ht->capacity = 32;
    ht->entries = ALLOCATE(vm, HTEntry, ht->capacity);
    memset(ht->entries, 0, sizeof(HTEntry) * (size_t)ht->capacity);
}

static void macro_free(VM* vm, Macro* m) {
    if (!m) return;
    xfree_any(vm, m->name);
    xfree_any(vm, m->body);
    if (m->params) {
        for (int i = 0; i < m->param_count; i++) xfree_any(vm, m->params[i]);
        xfree_any(vm, m->params);
    }
    FREE(vm, Macro, m);
}

static void ht_free(VM* vm, HashTable* ht) {
    if (!ht->entries) return;
    for (int i = 0; i < ht->capacity; i++) {
        if (ht->entries[i].used && !ht->entries[i].tombstone) {
            macro_free(vm, ht->entries[i].val);
            xfree_any(vm, ht->entries[i].key);
        }
    }
    FREE_ARRAY(vm, HTEntry, ht->entries, ht->capacity);
    ht->entries = NULL;
    ht->count = 0;
    ht->capacity = 0;
}

static HTEntry* ht_find_slot(HashTable* ht, const char* key) {
    uint32_t h = hash_cstr(key);
    uint32_t mask = (uint32_t)ht->capacity - 1;
    uint32_t idx = h & mask;

    HTEntry* first_tomb = NULL;
    while (1) {
        HTEntry* e = &ht->entries[idx];
        if (!e->used) {
            return first_tomb ? first_tomb : e;
        }
        if (!e->tombstone && e->key && strcmp(e->key, key) == 0) return e;
        if (e->tombstone && !first_tomb) first_tomb = e;
        idx = (idx + 1) & mask;
    }
}

static void ht_grow(VM* vm, HashTable* ht);

static Macro* ht_get(HashTable* ht, const char* key) {
    if (ht->count == 0) return NULL;
    HTEntry* e = ht_find_slot(ht, key);
    if (e->used && !e->tombstone && e->key) return e->val;
    return NULL;
}

static void ht_set(VM* vm, HashTable* ht, const char* key, Macro* val) {
    if ((ht->count + 1) * 100 / ht->capacity > 70) ht_grow(vm, ht);
    HTEntry* e = ht_find_slot(ht, key);
    if (!e->used || e->tombstone || !e->key) {
        e->used = true;
        e->tombstone = false;
        e->key = xstrdup(vm, key);
        e->val = val;
        ht->count++;
    } else {
        macro_free(vm, e->val);
        e->val = val;
    }
}

static void ht_del(VM* vm, HashTable* ht, const char* key) {
    if (ht->count == 0) return;
    HTEntry* e = ht_find_slot(ht, key);
    if (e->used && !e->tombstone && e->key) {
        xfree_any(vm, e->key);
        macro_free(vm, e->val);
        e->key = NULL;
        e->val = NULL;
        e->tombstone = true;
        ht->count--;
    }
}

static void ht_grow(VM* vm, HashTable* ht) {
    int oldcap = ht->capacity;
    HTEntry* old = ht->entries;

    ht->capacity = oldcap * 2;
    ht->entries = ALLOCATE(vm, HTEntry, ht->capacity);
    memset(ht->entries, 0, sizeof(HTEntry) * (size_t)ht->capacity);
    ht->count = 0;

    for (int i = 0; i < oldcap; i++) {
        HTEntry* e = &old[i];
        if (e->used && !e->tombstone && e->key) {
            HTEntry* dst = ht_find_slot(ht, e->key);
            dst->used = true;
            dst->tombstone = false;
            dst->key = e->key;
            dst->val = e->val;
            ht->count++;
        }
    }
    FREE_ARRAY(vm, HTEntry, old, oldcap);
}

// ---------------- Active macro stack ----------------
static void push_active(Preprocessor* pp, const char* name) {
    if (pp->active_count + 1 > pp->active_cap) {
        int old_cap = pp->active_cap;
        pp->active_cap = GROW_CAPACITY(old_cap);
        pp->active = GROW_ARRAY(pp->vm, char*, pp->active, old_cap, pp->active_cap);
    }
    pp->active[pp->active_count++] = (char*)name;
}

static void pop_active(Preprocessor* pp) {
    if (pp->active_count > 0) pp->active_count--;
}

static bool is_active_on_stack(Preprocessor* pp, const char* name) {
    for (int i = 0; i < pp->active_count; i++) {
        if (strcmp(pp->active[i], name) == 0) return true;
    }
    return false;
}

// ---------------- Small helpers ----------------
static const char* skip_spaces(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool is_ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static char* strndup_s(VM* vm, const char* s, size_t n) {
    char* r = (char*)xmalloc(vm, n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

// ===========================================================
// Logical line reader
// ===========================================================
static char* read_logical_line(Preprocessor* pp) {
    if (!*pp->cur) return NULL;

    const char* start = pp->cur;
    const char* p = pp->cur;
    OutputBuffer ob;
    initOutputBuffer(&ob);

    while (*p) {
        char c = *p++;
        if (c == '\n') {
            pp->line++;
            const char* q = p - 2;
            bool continued = (q >= start && *q == '\\');
            if (continued) {
                appendToOutputBuffer(pp->vm, &ob, start, (size_t)(q - start));
                start = p;
                continue;
            } else {
                appendToOutputBuffer(pp->vm, &ob, start, (size_t)((p - 1) - start));
                pp->cur = p;

                if (ob.count > 0 && ob.buffer[ob.count - 1] == '\r') ob.count--;

                appendToOutputBuffer(pp->vm, &ob, "\0", 1);
                return ob.buffer;
            }
        }
    }
    if (!*p) {
        if (p != start) appendToOutputBuffer(pp->vm, &ob, start, (size_t)(p - start));
        pp->cur = p;
    } else {
        pp->cur = p;
    }
    if (ob.count > 0 && ob.buffer[ob.count - 1] == '\r') ob.count--;

    appendToOutputBuffer(pp->vm, &ob, "\0", 1);
    return ob.buffer;
}

// ===========================================================
// Conditional helpers
// ===========================================================
static bool active_all(const ConditionalStack* st) {
    for (int i = 0; i < st->count; i++) if (!st->states[i].condition_met) return false;
    return true;
}

static bool active_parents_exclusive(const ConditionalStack* st) {
    int n = st->count - 1;
    if (n < 0) return true;
    for (int i = 0; i < n; i++) if (!st->states[i].condition_met) return false;
    return true;
}

// ===========================================================
// Expression parser for #if
// ===========================================================
typedef struct { const char* s; } Expr;
static void expr_skip_ws(Expr* e) { while (*e->s == ' ' || *e->s == '\t') e->s++; }

static bool parse_primary(Expr* e, Preprocessor* pp, int* out);

static bool parse_unary(Expr* e, Preprocessor* pp, int* out) {
    expr_skip_ws(e);
    if (*e->s == '!') { e->s++; int v=0; if (!parse_unary(e, pp, &v)) return false; *out = !v; return true; }
    return parse_primary(e, pp, out);
}

static bool parse_eq(Expr* e, Preprocessor* pp, int* out) {
    int lhs = 0;
    if (!parse_unary(e, pp, &lhs)) return false;
    for (;;) {
        const char* s = e->s;
        expr_skip_ws(e);
        if (e->s[0] == '=' && e->s[1] == '=') {
            e->s += 2;
            int rhs = 0; if (!parse_unary(e, pp, &rhs)) return false;
            lhs = (lhs == rhs) ? 1 : 0;
        } else if (e->s[0] == '!' && e->s[1] == '=') {
            e->s += 2;
            int rhs = 0; if (!parse_unary(e, pp, &rhs)) return false;
            lhs = (lhs != rhs) ? 1 : 0;
        } else {
            e->s = s;
            break;
        }
    }
    *out = lhs;
    return true;
}

static bool parse_and(Expr* e, Preprocessor* pp, int* out) {
    int lhs = 0; if (!parse_eq(e, pp, &lhs)) return false;
    for (;;) {
        const char* s = e->s; expr_skip_ws(e);
        if (e->s[0] == '&' && e->s[1] == '&') {
            e->s += 2;
            int rhs = 0; if (!parse_eq(e, pp, &rhs)) return false;
            lhs = (lhs != 0 && rhs != 0) ? 1 : 0; // booleanize for logical and
        } else {
            e->s = s; break;
        }
    }
    *out = lhs;
    return true;
}

static bool parse_or(Expr* e, Preprocessor* pp, int* out) {
    int lhs=0; if (!parse_and(e, pp, &lhs)) return false;
    for(;;){ const char* s=e->s; expr_skip_ws(e);
        if (e->s[0]=='|' && e->s[1]=='|'){ e->s+=2; int rhs=0; if(!parse_and(e,pp,&rhs))return false; lhs = (lhs || rhs)?1:0; }
        else { e->s=s; break; }
    }
    *out=lhs; return true;
}

static bool parse_paren(Expr* e, Preprocessor* pp, int* out) {
    expr_skip_ws(e); if (*e->s!='(') return false; e->s++; if(!parse_or(e,pp,out))return false;
    expr_skip_ws(e); if (*e->s!=')') return false; e->s++; return true;
}

static bool parse_number(Expr* e, int* out) {
    expr_skip_ws(e);
    if (!isdigit((unsigned char)*e->s)) return false;
    long v = 0;
    while (isdigit((unsigned char)*e->s)) {
        v = v * 10 + (*e->s - '0');
        e->s++;
    }
    *out = (int)v;  // keep full value; booleanization happens at logical ops
    return true;
}

static bool parse_defined(Expr* e, Preprocessor* pp, int* out) {
    expr_skip_ws(e);
    const char* save=e->s; const char kw[]="defined"; size_t kwlen=sizeof(kw)-1;
    if (strncmp(e->s, kw, kwlen)!=0) return false;
    e->s += kwlen; expr_skip_ws(e);

    char ident[256];
    if (*e->s=='('){ e->s++; expr_skip_ws(e);
        if (!isalpha((unsigned char)*e->s) && *e->s!='_'){ e->s=save; return false; }
        size_t n=0; while(isalnum((unsigned char)*e->s)||*e->s=='_'){ if(n+1<sizeof(ident)) ident[n++]=*e->s; e->s++; }
        ident[n]='\0'; expr_skip_ws(e); if(*e->s!=')'){ e->s=save; return false; } e->s++;
    } else {
        expr_skip_ws(e);
        if (!isalpha((unsigned char)*e->s) && *e->s!='_'){ e->s=save; return false; }
        size_t n=0; while(isalnum((unsigned char)*e->s)||*e->s=='_'){ if(n+1<sizeof(ident)) ident[n++]=*e->s; e->s++; }
        ident[n]='\0';
    }
    *out = ht_get(&pp->macros, ident) ? 1 : 0;
    return true;
}

static bool parse_primary(Expr* e, Preprocessor* pp, int* out) {
    expr_skip_ws(e);
    return parse_paren(e, pp, out) || parse_defined(e, pp, out) || parse_number(e, out);
}

static bool eval_expr(const char* s, Preprocessor* pp, int* out) {
    Expr e = { s };
    bool ok = parse_or(&e, pp, out);
    if (!ok) return false;
    expr_skip_ws(&e);
    //*out = *out ? 1 : 0;
    return true;
}

// ===========================================================
// Function-like macro parsing / substitution helpers
// ===========================================================
static bool parse_param_list(VM* vm, const char** p_in, char*** out_params, int* out_count) {
    const char* p = *p_in;
    if (*p != '(') return false;
    p++;
    char** params = NULL;
    int count = 0, cap = 0;

    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ')') { p++; break; }
        if (!is_ident_start(*p)) {
            for (int i=0;i<count;i++) xfree_any(vm, params[i]);
            xfree_any(vm, params);
            return false;
        }
        const char* n0 = p; p++;
        while (is_ident_char(*p)) p++;
        size_t nlen = (size_t)(p - n0);
        char* name = strndup_s(vm, n0, nlen);
        if (count + 1 > cap) {
            int old = cap;
            cap = cap ? cap * 2 : 4;
            params = GROW_ARRAY(vm, char*, params, old, cap);
        }
        params[count++] = name;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') { p++; continue; }
        if (*p == ')') { p++; break; }
        for (int i=0;i<count;i++) xfree_any(vm, params[i]);
        xfree_any(vm, params);
        return false;
    }

    *out_params = params;
    *out_count = count;
    *p_in = p;
    return true;
}

static void trim_spaces_inplace(char* s) {
    size_t len = strlen(s);
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    size_t j = len;
    while (j > i && (s[j-1] == ' ' || s[j-1] == '\t')) j--;
    if (i > 0) memmove(s, s + i, j - i);
    s[j - i] = '\0';
}

typedef struct {
    char** items;
    int count;
} ArgList;

static void free_arglist(VM* vm, ArgList* al) {
    for (int i=0;i<al->count;i++) xfree_any(vm, al->items[i]);
    xfree_any(vm, al->items);
    al->items = NULL;
    al->count = 0;
}

static bool parse_call_args(VM* vm, const char** p_in, ArgList* out) {
    const char* p = *p_in;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '(') return false;
    p++;

    char** items = NULL;
    int count = 0, cap = 0;

    OutputBuffer cur;
    initOutputBuffer(&cur);
    int depth = 1;
    bool in_str = false, in_char = false, in_line_cmt = false, in_blk_cmt = false;

    for (;;) {
        char c = *p;
        if (!c) {
            freeOutputBuffer(vm, &cur);
            for (int i=0;i<count;i++) xfree_any(vm, items[i]);
            xfree_any(vm, items);
            return false;
        }

        if (in_line_cmt) {
            if (c == '\n') in_line_cmt = false;
            appendToOutputBuffer(vm, &cur, &c, 1);
            p++;
            continue;
        }
        if (in_blk_cmt) {
            appendToOutputBuffer(vm, &cur, &c, 1);
            if (c == '*' && p[1] == '/') { appendToOutputBuffer(vm, &cur, "/", 1); p += 2; in_blk_cmt = false; continue; }
            p++;
            continue;
        }
        if (in_str) {
            appendToOutputBuffer(vm, &cur, &c, 1);
            if (c == '\\' && p[1]) { appendToOutputBuffer(vm, &cur, p+1, 1); p += 2; continue; }
            if (c == '"') { in_str = false; }
            p++;
            continue;
        }
        if (in_char) {
            appendToOutputBuffer(vm, &cur, &c, 1);
            if (c == '\\' && p[1]) { appendToOutputBuffer(vm, &cur, p+1, 1); p += 2; continue; }
            if (c == '\'') { in_char = false; }
            p++;
            continue;
        }

        // normal
        if (c == '"') { in_str = true; appendToOutputBuffer(vm, &cur, &c, 1); p++; continue; }
        if (c == '\'') { in_char = true; appendToOutputBuffer(vm, &cur, &c, 1); p++; continue; }
        if (c == '/' && p[1] == '/') { in_line_cmt = true; appendToOutputBuffer(vm, &cur, "//", 2); p += 2; continue; }
        if (c == '/' && p[1] == '*') { in_blk_cmt = true; appendToOutputBuffer(vm, &cur, "/*", 2); p += 2; continue; }

        if (c == '(') { depth++; appendToOutputBuffer(vm, &cur, &c, 1); p++; continue; }
        if (c == ')') {
            depth--;
            if (depth == 0) {
                appendToOutputBuffer(vm, &cur, "\0", 1);
                char* arg = cur.buffer;
                trim_spaces_inplace(arg);
                if (count + 1 > cap) {
                    int old = cap;
                    cap = cap ? cap * 2 : 4;
                    items = GROW_ARRAY(vm, char*, items, old, cap);
                }
                items[count++] = arg;
                p++;
                break;
            } else {
                appendToOutputBuffer(vm, &cur, &c, 1);
                p++;
                continue;
            }
        }
        if (c == ',' && depth == 1) {
            appendToOutputBuffer(vm, &cur, "\0", 1);
            char* arg = cur.buffer;
            trim_spaces_inplace(arg);
            if (count + 1 > cap) {
                int old = cap;
                cap = cap ? cap * 2 : 4;
                items = GROW_ARRAY(vm, char*, items, old, cap);
            }
            items[count++] = arg;
            initOutputBuffer(&cur);
            p++;
            continue;
        }

        appendToOutputBuffer(vm, &cur, &c, 1);
        p++;
    }

    out->items = items;
    out->count = count;
    *p_in = p;
    return true;
}

static void substitute_body_with_args(VM* vm, Macro* m, char** args, int argc, OutputBuffer* out) {
    const char* b = m->body;
    while (*b) {
        char c = *b;

        if (c == '"') {
            const char* start = b++;
            while (*b && *b != '"') { if (*b == '\\' && b[1]) b += 2; else b++; }
            if (*b == '"') b++;
            appendToOutputBuffer(vm, out, start, (size_t)(b - start));
            continue;
        }
        if (c == '\'') {
            const char* start = b++;
            while (*b && *b != '\'') { if (*b == '\\' && b[1]) b += 2; else b++; }
            if (*b == '\'') b++;
            appendToOutputBuffer(vm, out, start, (size_t)(b - start));
            continue;
        }
        if (c == '/' && b[1] == '/') {
            const char* start = b;
            b += 2;
            while (*b && *b != '\n') b++;
            appendToOutputBuffer(vm, out, start, (size_t)(b - start));
            continue;
        }
        if (c == '/' && b[1] == '*') {
            const char* start = b;
            b += 2;
            while (*b && !(b[0] == '*' && b[1] == '/')) b++;
            if (*b) b += 2;
            appendToOutputBuffer(vm, out, start, (size_t)(b - start));
            continue;
        }

        if (is_ident_start(c)) {
            const char* s = b++;
            while (is_ident_char(*b)) b++;
            size_t len = (size_t)(b - s);
            int idx = -1;
            for (int i = 0; i < m->param_count; i++) {
                if (strlen(m->params[i]) == len && strncmp(m->params[i], s, len) == 0) {
                    idx = i; break;
                }
            }
            if (idx >= 0 && idx < argc) {
                appendToOutputBuffer(vm, out, args[idx], strlen(args[idx]));
            } else {
                appendToOutputBuffer(vm, out, s, len);
            }
            continue;
        }

        appendToOutputBuffer(vm, out, b, 1);
        b++;
    }
}

static char* strip_comments_preserve_newlines(VM* vm, const char* src) {
    OutputBuffer ob;
    initOutputBuffer(&ob);

    const char* p = src;
    while (*p) {
        char c = *p;

        if (c == '"') {
            const char* start = p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p += 2;
                else p++;
            }
            if (*p == '"') p++;
            appendToOutputBuffer(vm, &ob, start, (size_t)(p - start));
            continue;
        }

        if (c == '\'') {
            const char* start = p++;
            while (*p && *p != '\'') {
                if (*p == '\\' && p[1]) p += 2;
                else p++;
            }
            if (*p == '\'') p++;
            appendToOutputBuffer(vm, &ob, start, (size_t)(p - start));
            continue;
        }

        if (c == '/' && p[1] == '/') {
            appendToOutputBuffer(vm, &ob, " ", 1);
            p += 2;
            while (*p && *p != '\n') p++;
            if (*p == '\n') {
                appendToOutputBuffer(vm, &ob, "\n", 1);
                p++;
            }
            continue;
        }

        if (c == '/' && p[1] == '*') {
            appendToOutputBuffer(vm, &ob, " ", 1);
            p += 2;
            while (*p) {
                if (p[0] == '*' && p[1] == '/') { p += 2; break; }
                if (*p == '\n') {
                    appendToOutputBuffer(vm, &ob, "\n", 1);
                }
                p++;
            }
            continue;
        }

        appendToOutputBuffer(vm, &ob, p, 1);
        p++;
    }

    appendToOutputBuffer(vm, &ob, "\0", 1);
    return ob.buffer;
}

// ===========================================================
// Macro expansion with object-like + function-like
// ===========================================================
static void expand_and_append(Preprocessor* pp, const char* text, OutputBuffer* out);

static bool try_expand_function_like(Preprocessor* pp, Macro* m, const char** p_in, OutputBuffer* out) {
    const char* p = *p_in;
    const char* la = p;
    while (*la == ' ' || *la == '\t') la++;
    if (*la != '(') return false;

    ArgList call = (ArgList){0};
    const char* after = la;
    if (!parse_call_args(pp->vm, &after, &call)) return false;

    int argc = m->param_count;
    char** argv = ALLOCATE(pp->vm, char*, argc);
    for (int i=0; i<argc; i++) {
        if (i < call.count) argv[i] = xstrdup(pp->vm, call.items[i]);
        else argv[i] = xstrdup(pp->vm, "");
    }
    free_arglist(pp->vm, &call);

    for (int i=0; i<argc; i++) {
        OutputBuffer tmp; initOutputBuffer(&tmp);
        expand_and_append(pp, argv[i], &tmp);
        appendToOutputBuffer(pp->vm, &tmp, "\0", 1);
        xfree_any(pp->vm, argv[i]);
        argv[i] = tmp.buffer;
    }

    OutputBuffer substituted; initOutputBuffer(&substituted);
    substitute_body_with_args(pp->vm, m, argv, argc, &substituted);
    appendToOutputBuffer(pp->vm, &substituted, "\0", 1);

    push_active(pp, m->name);
    expand_and_append(pp, substituted.buffer, out);
    pop_active(pp);

    for (int i=0; i<argc; i++) xfree_any(pp->vm, argv[i]);
    xfree_any(pp->vm, argv);

    *p_in = after;
    return true;
}

static void expand_expr_and_append(Preprocessor* pp, const char* text, OutputBuffer* out) {
    const char* p = text;
    while (*p) {
        char c = *p;

        if (c == '"') {
            const char* start = p++;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p += 2; else p++; }
            if (*p == '"') p++;
            appendToOutputBuffer(pp->vm, out, start, (size_t)(p - start));
            continue;
        }
        if (c == '\'') {
            const char* start = p++;
            while (*p && *p != '\'') { if (*p == '\\' && p[1]) p += 2; else p++; }
            if (*p == '\'') p++;
            appendToOutputBuffer(pp->vm, out, start, (size_t)(p - start));
            continue;
        }
        if (c == '/' && p[1] == '/') {
            const char* start = p; p += 2;
            while (*p && *p != '\n') p++;
            appendToOutputBuffer(pp->vm, out, start, (size_t)(p - start));
            continue;
        }
        if (c == '/' && p[1] == '*') {
            const char* start = p; p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            appendToOutputBuffer(pp->vm, out, start, (size_t)(p - start));
            continue;
        }

        if (is_ident_start(c)) {
            const char* id_start = p++; while (is_ident_char(*p)) p++;
            size_t id_len = (size_t)(p - id_start);

            if (id_len == 7 && strncmp(id_start, "defined", 7) == 0) {
                appendToOutputBuffer(pp->vm, out, id_start, id_len);

                const char* q = p;
                while (*q == ' ' || *q == '\t') q++;
                appendToOutputBuffer(pp->vm, out, p, (size_t)(q - p));
                p = q;

                if (*p == '(') {
                    appendToOutputBuffer(pp->vm, out, "(", 1);
                    p++;
                    while (*p == ' ' || *p == '\t') { appendToOutputBuffer(pp->vm, out, p, 1); p++; }
                    if (is_ident_start(*p)) {
                        const char* a0 = p++; while (is_ident_char(*p)) p++;
                        appendToOutputBuffer(pp->vm, out, a0, (size_t)(p - a0));
                        const char* r = p;
                        while (*r == ' ' || *r == '\t') r++;
                        appendToOutputBuffer(pp->vm, out, p, (size_t)(r - p));
                        p = r;
                    }
                    if (*p == ')') { appendToOutputBuffer(pp->vm, out, ")", 1); p++; }
                } else {
                    while (*p == ' ' || *p == '\t') { appendToOutputBuffer(pp->vm, out, p, 1); p++; }
                    if (is_ident_start(*p)) {
                        const char* a0 = p++; while (is_ident_char(*p)) p++;
                        appendToOutputBuffer(pp->vm, out, a0, (size_t)(p - a0));
                    }
                }
                continue;
            }

            char idbuf[256]; Macro* m = NULL;
            if (id_len < sizeof(idbuf)) {
                memcpy(idbuf, id_start, id_len); idbuf[id_len] = '\0';
                m = ht_get(&pp->macros, idbuf);
            }
            if (m && !is_active_on_stack(pp, m->name)) {
                if (m->is_function) {
                    const char* p_after = p;
                    if (try_expand_function_like(pp, m, &p_after, out)) { p = p_after; continue; }
                } else {
                    push_active(pp, m->name);
                    expand_and_append(pp, m->body, out);
                    pop_active(pp);
                    continue;
                }
            }
            appendToOutputBuffer(pp->vm, out, id_start, id_len);
            continue;
        }

        appendToOutputBuffer(pp->vm, out, p, 1);
        p++;
    }
}

static void expand_and_append(Preprocessor* pp, const char* text, OutputBuffer* out) {
    const char* p = text;
    while (*p) {
        char c = *p;

        if (c == '"') {
            const char* start = p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p += 2; else p++;
            }
            if (*p == '"') p++;
            appendToOutputBuffer(pp->vm, out, start, (size_t)(p - start));
            continue;
        }
        if (c == '\'') {
            const char* start = p++;
            while (*p && *p != '\'') {
                if (*p == '\\' && p[1]) p += 2; else p++;
            }
            if (*p == '\'') p++;
            appendToOutputBuffer(pp->vm, out, start, (size_t)(p - start));
            continue;
        }
        if (c == '/' && p[1] == '/') {
            const char* start = p;
            p += 2;
            while (*p && *p != '\n') p++;
            appendToOutputBuffer(pp->vm, out, start, (size_t)(p - start));
            continue;
        }
        if (c == '/' && p[1] == '*') {
            const char* start = p;
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            appendToOutputBuffer(pp->vm, out, start, (size_t)(p - start));
            continue;
        }

        if (is_ident_start(c)) {
            const char* id_start = p++;
            while (is_ident_char(*p)) p++;
            size_t id_len = (size_t)(p - id_start);
            char idbuf[256];
            if (id_len < sizeof(idbuf)) {
                memcpy(idbuf, id_start, id_len);
                idbuf[id_len] = '\0';
                Macro* m = ht_get(&pp->macros, idbuf);
                if (m && !is_active_on_stack(pp, m->name)) {
                    if (m->is_function) {
                        const char* save_p = p;
                        const char* p_after = save_p;
                        if (try_expand_function_like(pp, m, &p_after, out)) {
                            p = p_after;
                            continue;
                        }
                    } else {
                        push_active(pp, m->name);
                        expand_and_append(pp, m->body, out);
                        pop_active(pp);
                        continue;
                    }
                }
            }
            appendToOutputBuffer(pp->vm, out, id_start, id_len);
            continue;
        }

        appendToOutputBuffer(pp->vm, out, p, 1);
        p++;
    }
}

// ===========================================================
// Directive handling
// ===========================================================

static Macro* make_object_macro(VM* vm, const char* name, const char* body0) {
    size_t blen = strlen(body0);
    while (blen > 0 && (body0[blen - 1] == ' ' || body0[blen - 1] == '\t')) blen--;

    Macro* m = ALLOCATE(vm, Macro, 1);
    m->name = xstrdup(vm, name);
    m->body = (char*)xmalloc(vm, blen + 1);
    memcpy(m->body, body0, blen);
    m->body[blen] = '\0';
    m->params = NULL;
    m->param_count = 0;
    m->is_function = false;
    return m;
}

static Macro* make_function_macro(VM* vm, const char* name, char** params, int param_count, const char* body_text) {
    Macro* m = ALLOCATE(vm, Macro, 1);
    m->name = xstrdup(vm, name);
    m->body = xstrdup(vm, body_text);
    m->params = params;
    m->param_count = param_count;
    m->is_function = true;
    return m;
}

static void do_define_single(Preprocessor* pp, const char* rest) {
    const char* p = skip_spaces(rest);
    if (!is_ident_start(*p)) return;

    const char* n0 = p; p++;
    while (is_ident_char(*p)) p++;
    size_t nlen = (size_t)(p - n0);
    if (nlen >= 256) return;
    char name[256]; memcpy(name, n0, nlen); name[nlen] = '\0';

    p = skip_spaces(p);
    if (*p == '(') {
        char** params = NULL; int param_count = 0;
        if (!parse_param_list(pp->vm, &p, &params, &param_count)) {
            for (int i=0;i<param_count;i++) xfree_any(pp->vm, params[i]);
            xfree_any(pp->vm, params);
            return;
        }
        p = skip_spaces(p);
        const char* body0 = p;
        Macro* m = make_function_macro(pp->vm, name, params, param_count, body0);
        ht_set(pp->vm, &pp->macros, name, m);
    } else {
        const char* body0 = skip_spaces(p);
        Macro* m = make_object_macro(pp->vm, name, body0);
        ht_set(pp->vm, &pp->macros, name, m);
    }
}

static void do_define_block(Preprocessor* pp, const char* rest, bool active_now) {
    const char* p = skip_spaces(rest);
    const char* n0 = p; p++;
    while (is_ident_char(*p)) p++;
    size_t nlen = (size_t)(p - n0);
    char name[256] = {0};
    if (nlen > 0 && nlen < sizeof(name)) {
        memcpy(name, n0, nlen); name[nlen] = '\0';
    }

    p = skip_spaces(p);
    char** params = NULL; int param_count = 0;
    bool has_params = false;
    if (*p == '(') {
        has_params = parse_param_list(pp->vm, &p, &params, &param_count);
        if (!has_params) {
            params = NULL; param_count = 0;
        }
    }

    OutputBuffer body; initOutputBuffer(&body);
    bool found_end = false;

    for (;;) {
        char* line = read_logical_line(pp);
        if (!line) break;

        const char* t = line;
        while (*t == ' ' || *t == '\t') t++;
        bool is_end = false;
        if (*t == '#') {
            const char* q = t + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '#') {
                q++; while (*q == ' ' || *q == '\t') q++;
                char dir[32]; size_t dn = 0;
                while (isalpha((unsigned char)*q) && dn + 1 < sizeof(dir)) dir[dn++] = *q++;
                dir[dn] = '\0';
                if (strcmp(dir, "enddefine") == 0) {
                    is_end = true;
                }
            }
        }

        if (is_end) {
            found_end = true;
            xfree_any(pp->vm, line);
            break;
        }

        if (active_now && name[0]) {
            appendToOutputBuffer(pp->vm, &body, line, strlen(line));
            appendToOutputBuffer(pp->vm, &body, "\n", 1);
        }
        xfree_any(pp->vm, line);
    }

    if (active_now && name[0] && found_end) {
        appendToOutputBuffer(pp->vm, &body, "\0", 1);
        Macro* m;
        if (has_params) {
            m = make_function_macro(pp->vm, name, params, param_count, body.buffer);
        } else {
            m = make_object_macro(pp->vm, name, body.buffer);
        }
        ht_set(pp->vm, &pp->macros, name, m);
    } else {
        for (int i=0;i<param_count;i++) xfree_any(pp->vm, params ? params[i] : NULL);
        xfree_any(pp->vm, params);
    }

    freeOutputBuffer(pp->vm, &body);
}

static void do_undef(Preprocessor* pp, const char* rest) {
    const char* p = skip_spaces(rest);
    if (!is_ident_start(*p)) return;
    const char* n0 = p; p++;
    while (is_ident_char(*p)) p++;
    size_t nlen = (size_t)(p - n0);
    if (nlen >= 256) return;
    char name[256]; memcpy(name, n0, nlen); name[nlen] = '\0';
    ht_del(pp->vm, &pp->macros, name);
}

static void push_if(Preprocessor* pp, bool cond_value) {
    bool parents_active = active_all(&pp->cond);
    IfState st;
    st.condition_met = cond_value && parents_active;
    st.branch_taken = st.condition_met;
    pushConditionalStack(pp->vm, &pp->cond, st);
}

static void do_if(Preprocessor* pp, const char* rest) {
    OutputBuffer tmp; initOutputBuffer(&tmp);
    expand_expr_and_append(pp, rest, &tmp);
    appendToOutputBuffer(pp->vm, &tmp, "\0", 1);

    int v = 0;
    if (!eval_expr(tmp.buffer, pp, &v)) v = 0;
    freeOutputBuffer(pp->vm, &tmp);

    push_if(pp, v != 0);
}

static void do_ifdef(Preprocessor* pp, const char* rest, bool negated) {
    const char* p = skip_spaces(rest);
    if (!is_ident_start(*p)) { push_if(pp, false); return; }
    const char* n0 = p; p++; while (is_ident_char(*p)) p++;
    size_t nlen = (size_t)(p - n0);
    char name[256]; if (nlen >= sizeof(name)) { push_if(pp, false); return; }
    memcpy(name, n0, nlen); name[nlen] = '\0';
    bool defined = ht_get(&pp->macros, name) != NULL;
    push_if(pp, negated ? !defined : defined);
}

static void do_elif(Preprocessor* pp, const char* rest) {
    IfState* top = peekConditionalStack(&pp->cond);
    if (!top) return;
    bool parents_active = active_parents_exclusive(&pp->cond);
    if (top->branch_taken || !parents_active) { top->condition_met = false; return; }

    OutputBuffer tmp; initOutputBuffer(&tmp);
    expand_expr_and_append(pp, rest, &tmp);
    appendToOutputBuffer(pp->vm, &tmp, "\0", 1);

    int v = 0;
    if (!eval_expr(tmp.buffer, pp, &v)) v = 0;
    freeOutputBuffer(pp->vm, &tmp);

    top->condition_met = (v != 0);
    if (top->condition_met) top->branch_taken = true;
}

static void do_else(Preprocessor* pp) {
    IfState* top = peekConditionalStack(&pp->cond);
    if (!top) return;
    bool parents_active = active_parents_exclusive(&pp->cond);
    if (!parents_active) { top->condition_met = false; return; }
    if (top->branch_taken) top->condition_met = false;
    else { top->condition_met = true; top->branch_taken = true; }
}

static void do_endif(Preprocessor* pp) {
    if (pp->cond.count > 0) popConditionalStack(&pp->cond);
}

static void handle_directive(Preprocessor* pp, const char* after_hash, OutputBuffer* out) {
    const char* p = skip_spaces(after_hash);

    bool double_hash = false;
    if (*p == '#') {
        double_hash = true;
        p++; p = skip_spaces(p);
    }

    char dir[32]; size_t dn = 0;
    while (isalpha((unsigned char)*p) && dn + 1 < sizeof(dir)) dir[dn++] = *p++;
    dir[dn] = '\0';

    const char* rest = p;
    bool active_now = active_all(&pp->cond);

    if (strcmp(dir, "include") == 0) {
        return;
    }

    if (strcmp(dir, "error") == 0) {
        if (active_now) {
            pp->had_error = true;
        }
        return;
    }

    if (double_hash && strcmp(dir, "define") == 0) {
        do_define_block(pp, rest, active_now);
        return;
    }

    if (strcmp(dir, "define") == 0) {
        if (active_now) do_define_single(pp, rest);
        return;
    }

    if (strcmp(dir, "undef") == 0) { if (active_now) do_undef(pp, rest); return; }
    if (strcmp(dir, "ifdef") == 0) { do_ifdef(pp, rest, false); return; }
    if (strcmp(dir, "ifndef") == 0) { do_ifdef(pp, rest, true);  return; }
    if (strcmp(dir, "if") == 0)     { do_if(pp, rest);           return; }
    if (strcmp(dir, "elif") == 0)   { do_elif(pp, rest);         return; }
    if (strcmp(dir, "else") == 0)   { do_else(pp);               return; }
    if (strcmp(dir, "endif") == 0)  { do_endif(pp);              return; }

    if (active_now) {
        appendToOutputBuffer(pp->vm, out, "#", 1);
        appendToOutputBuffer(pp->vm, out, after_hash, strlen(after_hash));
        appendToOutputBuffer(pp->vm, out, "\n", 1);
    }
}

// ===========================================================
// Public API
// ===========================================================
char* preprocess(VM* vm, const char* source, LineMap* line_map) {
    if (!source) return NULL;

    Preprocessor pp;
    memset(&pp, 0, sizeof(pp));
    pp.vm = vm;

    char* stripped = strip_comments_preserve_newlines(pp.vm, source);

    pp.source = stripped;
    pp.cur = stripped;
    pp.line = 1;

    ht_init(pp.vm, &pp.macros);
    initConditionalStack(&pp.cond);

    OutputBuffer out;
    initOutputBuffer(&out);

    for (;;) {
        int current_original_line = pp.line;

        char* line = read_logical_line(&pp);
        if (!line) break;

        const char* t = line;
        while (*t == ' ' || *t == '\t') t++;
        bool is_directive = (*t == '#');

        bool active_now = active_all(&pp.cond);
        if (is_directive) {
            handle_directive(&pp, t + 1, &out);
            if (pp.had_error) { xfree_any(pp.vm, line); break; }
        } else if (active_now) {
            int buffer_pos_before = out.count;
            expand_and_append(&pp, line, &out);

            for (int i = buffer_pos_before; i < out.count; i++) {
                if (out.buffer[i] == '\n') {
                    addLineMapping(pp.vm, line_map, current_original_line);
                }
            }

            if (out.count > buffer_pos_before && out.buffer[out.count - 1] != '\n') {
                appendToOutputBuffer(pp.vm, &out, "\n", 1);
                addLineMapping(pp.vm, line_map, current_original_line);
            }
        }

        xfree_any(pp.vm, line);
    }

    if (pp.had_error) {
        while (pp.cond.count > 0) popConditionalStack(&pp.cond);
        freeConditionalStack(pp.vm, &pp.cond);
        ht_free(pp.vm, &pp.macros);
        if (pp.active) FREE_ARRAY(vm, char*, pp.active, pp.active_cap);
        freeOutputBuffer(pp.vm, &out);
        xfree_any(pp.vm, stripped);
        return NULL;
    }

    while (pp.cond.count > 0) popConditionalStack(&pp.cond);
    freeConditionalStack(pp.vm, &pp.cond);

    appendToOutputBuffer(pp.vm, &out, "\0", 1);
    char* result = out.buffer;

    ht_free(pp.vm, &pp.macros);
    if (pp.active) FREE_ARRAY(vm, char*, pp.active, pp.active_cap);
    xfree_any(pp.vm, stripped);

    return result;
}