#include <stdlib.h>
#include <stdio.h>

#include "./linemap.h"
#include "./memory.h"

void initLineMap(LineMap* map) {
    map->lines = NULL;
    map->count = 0;
    map->capacity = 0;
}

void freeLineMap(VM* vm, LineMap* map) {
    reallocate(vm, map->lines, sizeof(int) * map->capacity, 0);
    initLineMap(map);
}

void addLineMapping(VM* vm, LineMap* map, int original_line) {
    if (map->capacity < map->count + 1) {
        int old_capacity = map->capacity;
        map->capacity = old_capacity < 8 ? 8 : old_capacity * 2;
        map->lines = (int*)reallocate(vm, map->lines, sizeof(int) * old_capacity, sizeof(int) * map->capacity);
        if (map->lines == NULL) {
            fprintf(stderr, "Failed to allocate memory for line map.\n");
            exit(1);
        }
    }

    map->lines[map->count] = original_line;
    map->count++;
}