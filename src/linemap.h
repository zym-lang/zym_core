#pragma once

typedef struct VM VM;

typedef struct LineMap {
    int* lines;
    int count;
    int capacity;
} LineMap;

void initLineMap(LineMap* map);
void freeLineMap(VM* vm, LineMap* map);
void addLineMapping(VM* vm, LineMap* map, int original_line);