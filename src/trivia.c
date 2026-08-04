#include "./trivia.h"

#include <stddef.h>

#include "./memory.h"
#include "./vm.h"

void trivia_init(TriviaBuffer* tb) {
    tb->pieces = NULL;
    tb->count = 0;
    tb->capacity = 0;
}

void trivia_free(VM* vm, TriviaBuffer* tb) {
    if (tb == NULL) return;
    if (tb->pieces != NULL) {
        FREE_ARRAY(vm, TriviaPiece, tb->pieces, tb->capacity);
    }
    tb->pieces = NULL;
    tb->count = 0;
    tb->capacity = 0;
}

void trivia_append(VM* vm, TriviaBuffer* tb, const TriviaPiece* piece) {
    if (tb->count + 1 > tb->capacity) {
        int old_cap = tb->capacity;
        int new_cap = GROW_CAPACITY(old_cap);
        tb->pieces = GROW_ARRAY(vm, TriviaPiece, tb->pieces, old_cap, new_cap);
        tb->capacity = new_cap;
    }
    tb->pieces[tb->count++] = *piece;
}

int trivia_lower_bound(const TriviaBuffer* tb, int byteOffset) {
    int lo = 0, hi = tb->count;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (tb->pieces[mid].startByte < byteOffset) lo = mid + 1;
        else                                        hi = mid;
    }
    return lo;
}

const TriviaPiece* trivia_find_at(const TriviaBuffer* tb, int byteOffset) {
    if (tb->count == 0) return NULL;
    // First piece with startByte > byteOffset; the candidate is the one
    // immediately before it.
    int lo = 0, hi = tb->count;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (tb->pieces[mid].startByte <= byteOffset) lo = mid + 1;
        else                                         hi = mid;
    }
    if (lo == 0) return NULL;
    const TriviaPiece* p = &tb->pieces[lo - 1];
    if (byteOffset >= p->startByte && byteOffset < p->startByte + p->length) {
        return p;
    }
    return NULL;
}
