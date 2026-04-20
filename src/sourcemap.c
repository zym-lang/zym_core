#include "./sourcemap.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "./memory.h"

void initSourceMap(SourceMap* map) {
    map->segments = NULL;
    map->count = 0;
    map->capacity = 0;
}

void freeSourceMap(VM* vm, SourceMap* map) {
    if (map->segments != NULL) {
        reallocate(vm, map->segments,
                   sizeof(SourceMapSegment) * map->capacity, 0);
    }
    initSourceMap(map);
}

void appendSourceMapSegment(VM* vm, SourceMap* map,
                            int expandedStartByte, int expandedLength,
                            ZymFileId originFileId,
                            int originStartByte, int originLength,
                            int originLine) {
    if (map->capacity < map->count + 1) {
        int old_capacity = map->capacity;
        map->capacity = old_capacity < 8 ? 8 : old_capacity * 2;
        map->segments = (SourceMapSegment*)reallocate(
            vm, map->segments,
            sizeof(SourceMapSegment) * old_capacity,
            sizeof(SourceMapSegment) * map->capacity);
        if (map->segments == NULL) {
            fprintf(stderr, "Failed to allocate memory for source map.\n");
            exit(1);
        }
    }

    // Note: segments are appended one-per-expanded-line and never coalesced.
    // Keeping per-line granularity lets callers (scanner `mappedLine`,
    // module_loader combined-source assembly) look up origin by either
    // byte offset (binary search) or expanded-line index (direct array
    // indexing) without tracking cumulative line counts on the segments.
    SourceMapSegment* seg = &map->segments[map->count++];
    seg->expandedStartByte = expandedStartByte;
    seg->expandedLength = expandedLength;
    seg->originFileId = originFileId;
    seg->originStartByte = originStartByte;
    seg->originLength = originLength;
    seg->originLine = originLine;
}

const SourceMapSegment* sourcemap_lookup(const SourceMap* map, int expandedByte) {
    if (map == NULL || map->count == 0) return NULL;
    int lo = 0;
    int hi = map->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const SourceMapSegment* seg = &map->segments[mid];
        if (expandedByte < seg->expandedStartByte) {
            hi = mid - 1;
        } else if (expandedByte >= seg->expandedStartByte + seg->expandedLength) {
            lo = mid + 1;
        } else {
            return seg;
        }
    }
    return NULL;
}
