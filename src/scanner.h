#pragma once

#include <stdbool.h>

#include "./token.h"
#include "./linemap.h"

typedef struct {
    const char* start;
    const char* current;
    int line;
    const LineMap* line_map;
} Scanner;

void initScanner(Scanner* scanner, const char* source, const LineMap* line_map);
Token scanToken(Scanner* scanner);
bool isAlpha(char c);