#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "./memory.h"
#include "./vm.h"
#include "./gc.h"

// =============================================================================
// DEFAULT ALLOCATOR
// =============================================================================

static void* default_alloc(void* ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void* default_calloc(void* ctx, size_t count, size_t size) {
    (void)ctx;
    return calloc(count, size);
}

static void* default_realloc(void* ctx, void* ptr, size_t old_size, size_t new_size) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void default_free(void* ctx, void* ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

ZymAllocator zym_defaultAllocator(void) {
    return (ZymAllocator){
        .alloc   = default_alloc,
        .calloc  = default_calloc,
        .realloc = default_realloc,
        .free    = default_free,
        .ctx     = NULL
    };
}

// =============================================================================
// REALLOCATE (GC-aware, uses VM's allocator)
// =============================================================================

void zymOutOfMemory(VM* vm)
{
    if (vm != NULL && vm->oom_jmp_armed) {
        // Record the reason before leaving; the landing pad only knows that it
        // was jumped to, not why.
        vm->vm_cause = ZYM_CAUSE_OUT_OF_MEMORY;
        vm->oom_jmp_armed = false;   // one shot: a failure while unwinding is fatal
        longjmp(vm->oom_jmp, 1);
    }
    fprintf(stderr, "Fatal: Out of memory\n");
    exit(1);
}

void* reallocate(VM* vm, void* pointer, size_t oldSize, size_t newSize) {
#ifdef ZYM_HEAP_CENSUS
    zymCensusRaw(oldSize, newSize);
#endif
    vm->bytes_allocated += newSize - oldSize;

    if (newSize > oldSize) {
        bool just_collected = false;
        int32_t delta = (int32_t)((newSize - oldSize) > (size_t)INT32_MAX ? (size_t)INT32_MAX : (newSize - oldSize));
        vm->gc_debt -= delta;
        #ifdef DEBUG_STRESS_GC
            if (vm->gc_enabled) { collectGarbage(vm); just_collected = true; }
        #else
            if (__builtin_expect(vm->gc_debt <= 0, 0)) {
                if (vm->gc_enabled) {
                    collectGarbage(vm);
                    just_collected = true;
                } else {
                    // GC disabled but debt wrapped — reset to prevent repeated triggers
                    vm->gc_debt = INT32_MAX;
                }
            }
        #endif

        // Memory ceiling. Deliberately checked AFTER any collection above, so
        // a program that merely churns garbage is never charged for it.
        if (__builtin_expect(vm->memory_limit > 0 && !vm->oom_pending &&
                             vm->bytes_allocated > vm->memory_limit, 0)) {
            // One more collection before giving up -- the debt heuristic may
            // simply not have come due yet. gc_enabled is false while a
            // collection is in progress, so this cannot re-enter.
            if (!just_collected && vm->gc_enabled) {
                collectGarbage(vm);
            }

            if (vm->bytes_allocated > vm->memory_limit) {
                // Do NOT fail the allocation: the host allocator still has
                // memory, and returning NULL here would strand every caller
                // that assumes success. Satisfy this request, then suspend at
                // the next instruction boundary. The overshoot is bounded by
                // one allocation, plus whatever the current opcode still does
                // before DISPATCH runs again.
                vm->oom_pending = true;
                vm->cause_bytes_wanted = newSize - oldSize;   // what crossed it
                vm->preempt_counter = 0;   // observed by DISPATCH -> handlePreemption

                // A compile has no instruction boundary to suspend at, so the
                // dispatch-loop route above never runs and the ceiling would be
                // ignored for the whole compilation. The frontend already polls
                // a cancellation flag at every statement and declaration
                // boundary; reuse it so the compile stops there instead.
                if (vm->compiler != NULL) {
                    vm->compile_cancelled = 1;
                }
            }
        }
    }

    if (newSize == 0) {
        ZYM_FREE(&vm->allocator, pointer, oldSize);
        return NULL;
    }

    void* result = ZYM_REALLOC(&vm->allocator, pointer, oldSize, newSize);
    if (result == NULL) {
        if (vm->gc_enabled) {
            collectGarbage(vm);
            result = ZYM_REALLOC(&vm->allocator, pointer, oldSize, newSize);
        }
        if (result == NULL) {
            zymOutOfMemory(vm);   // does not return when a boundary is armed
        }
    }
    return result;
}

// ============================================================================
// Heap census (ZYM_HEAP_CENSUS builds only) — see memory.h.
// ============================================================================
#ifdef ZYM_HEAP_CENSUS
#include <stdint.h>

#define CENSUS_TYPES 32
// Size buckets: exact 8-byte steps up to 1024 (128 buckets), then log2
// overflow buckets for 1KB..>1MB (11 more).
#define CENSUS_SMALL_BUCKETS 128
#define CENSUS_BUCKETS (CENSUS_SMALL_BUCKETS + 11)

uint32_t zym_census_epoch = 0;

static uint64_t c_obj_count[CENSUS_TYPES];
static uint64_t c_obj_bytes[CENSUS_TYPES];
static uint64_t c_obj_hist[CENSUS_TYPES][CENSUS_BUCKETS];
static uint64_t c_death_age[CENSUS_TYPES][5];   // 0, 1, 2, 3-7, 8+
static uint64_t c_death_teardown[CENSUS_TYPES];
static int      c_teardown = 0;

static uint64_t c_raw_alloc_count, c_raw_alloc_bytes;
static uint64_t c_raw_grow_count,  c_raw_grow_bytes;
static uint64_t c_raw_shrink_count, c_raw_free_count;
static uint64_t c_raw_hist[CENSUS_BUCKETS];

#define CENSUS_LIVE_LOG 4096
static uint64_t c_live_bytes[CENSUS_LIVE_LOG];
static uint64_t c_live_objs[CENSUS_LIVE_LOG];

static int censusBucket(size_t size) {
    if (size < 1024) return (int)(size / 8);
    int b = CENSUS_SMALL_BUCKETS;
    size_t s = 1024;
    while (b < CENSUS_BUCKETS - 1 && size >= (s << 1)) { s <<= 1; b++; }
    return b;
}

void zymCensusObject(int type, size_t size) {
    if (type < 0 || type >= CENSUS_TYPES) type = CENSUS_TYPES - 1;
    c_obj_count[type]++;
    c_obj_bytes[type] += size;
    c_obj_hist[type][censusBucket(size)]++;
}

void zymCensusDeath(int type, uint32_t birth) {
    if (type < 0 || type >= CENSUS_TYPES) type = CENSUS_TYPES - 1;
    if (c_teardown) { c_death_teardown[type]++; return; }
    uint32_t age = zym_census_epoch - birth;
    int slot = age == 0 ? 0 : age == 1 ? 1 : age == 2 ? 2 : age < 8 ? 3 : 4;
    c_death_age[type][slot]++;
}

void zymCensusRaw(size_t old_size, size_t new_size) {
    if (old_size == 0 && new_size > 0) {
        c_raw_alloc_count++; c_raw_alloc_bytes += new_size;
        c_raw_hist[censusBucket(new_size)]++;
    } else if (new_size == 0 && old_size > 0) {
        c_raw_free_count++;
    } else if (new_size > old_size) {
        c_raw_grow_count++; c_raw_grow_bytes += new_size - old_size;
    } else if (new_size < old_size) {
        c_raw_shrink_count++;
    }
}

void zymCensusGcEnd(size_t live_bytes, uint64_t live_objects) {
    if (zym_census_epoch < CENSUS_LIVE_LOG) {
        c_live_bytes[zym_census_epoch] = live_bytes;
        c_live_objs[zym_census_epoch]  = live_objects;
    }
    zym_census_epoch++;
}

void zymCensusTeardown(void) { c_teardown = 1; }

void zymCensusDump(void) {
    fprintf(stderr, "CENSUS begin epochs=%u\n", zym_census_epoch);
    for (int t = 0; t < CENSUS_TYPES; t++) {
        if (c_obj_count[t] == 0) continue;
        fprintf(stderr, "CENSUS obj type=%d count=%llu bytes=%llu deaths=%llu,%llu,%llu,%llu,%llu teardown=%llu\n",
                t,
                (unsigned long long)c_obj_count[t],
                (unsigned long long)c_obj_bytes[t],
                (unsigned long long)c_death_age[t][0],
                (unsigned long long)c_death_age[t][1],
                (unsigned long long)c_death_age[t][2],
                (unsigned long long)c_death_age[t][3],
                (unsigned long long)c_death_age[t][4],
                (unsigned long long)c_death_teardown[t]);
        for (int b = 0; b < CENSUS_BUCKETS; b++) {
            if (c_obj_hist[t][b] == 0) continue;
            fprintf(stderr, "CENSUS hist type=%d bucket=%d count=%llu\n",
                    t, b, (unsigned long long)c_obj_hist[t][b]);
        }
    }
    fprintf(stderr, "CENSUS raw alloc_count=%llu alloc_bytes=%llu grow_count=%llu grow_bytes=%llu shrink=%llu free=%llu\n",
            (unsigned long long)c_raw_alloc_count, (unsigned long long)c_raw_alloc_bytes,
            (unsigned long long)c_raw_grow_count,  (unsigned long long)c_raw_grow_bytes,
            (unsigned long long)c_raw_shrink_count, (unsigned long long)c_raw_free_count);
    for (int b = 0; b < CENSUS_BUCKETS; b++) {
        if (c_raw_hist[b] == 0) continue;
        fprintf(stderr, "CENSUS rawhist bucket=%d count=%llu\n", b, (unsigned long long)c_raw_hist[b]);
    }
    uint32_t n = zym_census_epoch < CENSUS_LIVE_LOG ? zym_census_epoch : CENSUS_LIVE_LOG;
    uint64_t peak_b = 0, peak_o = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (c_live_bytes[i] > peak_b) peak_b = c_live_bytes[i];
        if (c_live_objs[i] > peak_o) peak_o = c_live_objs[i];
    }
    fprintf(stderr, "CENSUS live peak_bytes=%llu peak_objs=%llu last_bytes=%llu last_objs=%llu\n",
            (unsigned long long)peak_b, (unsigned long long)peak_o,
            (unsigned long long)(n ? c_live_bytes[n-1] : 0),
            (unsigned long long)(n ? c_live_objs[n-1] : 0));
    fprintf(stderr, "CENSUS end\n");
}
#endif
