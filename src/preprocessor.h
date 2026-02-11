#pragma once

#include "./linemap.h"

typedef struct VM VM;

char* preprocess(VM* vm, const char* source, LineMap* line_map);