#pragma once

// Phase 1.2 frontend hygiene: SourceMap — per-expanded-line origin table.
//
// The preprocessor produces a post-expansion byte buffer that the scanner
// walks. Every byte of that buffer originates either from a specific
// byte range in the user-visible source (the common case) or from a
// preprocessor-synthesized fragment (macro body expansion). A SourceMap
// stores one segment per expanded line describing where the bytes of
// that line came from, so the scanner can populate each token's
// origin{FileId,StartByte,Length} fields.
//
// Granularity is per-expanded-line (Phase 1.6: LineMap was folded into
// SourceMap so this is now the sole origin-tracking primitive). A
// token's origin span covers the origin line it was scanned from; finer
// (sub-line) origin tracking for macro-synthesized bytes is a Phase 2
// concern.
//
// Related: source_file.h — the ZymFileId registry tokens key off of.

#include <stddef.h>

#include "./source_file.h"

typedef struct VM VM;

typedef struct SourceMapSegment {
    // Expanded-buffer byte range this segment covers (half-open:
    // [expandedStartByte, expandedStartByte + expandedLength)).
    int expandedStartByte;
    int expandedLength;

    // Where these expanded bytes came from in the user-visible source.
    ZymFileId originFileId;
    int originStartByte;
    int originLength;

    // 1-based origin line, kept for diagnostics that want a line number
    // without walking the file bytes.
    int originLine;
} SourceMapSegment;

typedef struct SourceMap {
    SourceMapSegment* segments;
    int count;
    int capacity;
} SourceMap;

void initSourceMap(SourceMap* map);
void freeSourceMap(VM* vm, SourceMap* map);

// Appends a segment to the map. Segments are expected to be appended in
// expanded-byte order, which the preprocessor produces naturally.
void appendSourceMapSegment(VM* vm, SourceMap* map,
                            int expandedStartByte, int expandedLength,
                            ZymFileId originFileId,
                            int originStartByte, int originLength,
                            int originLine);

// Returns the segment whose expanded byte range contains expandedByte,
// or NULL if the map is empty or the byte is past the last segment.
// O(log N) via binary search on the sorted segment list.
const SourceMapSegment* sourcemap_lookup(const SourceMap* map, int expandedByte);
