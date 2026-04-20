#include "./diagnostics.h"

#include "./vm.h"
#include "./memory.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void diagsink_init(DiagnosticSink* sink) {
    sink->items = NULL;
    sink->count = 0;
    sink->capacity = 0;
    sink->has_error = false;
}

static void free_entry_strings(VM* vm, ZymDiagnostic* d) {
    if (d->message) {
        size_t n = strlen(d->message) + 1;
        reallocate(vm, (void*)d->message, n, 0);
        d->message = NULL;
    }
#if ZYM_HAS_DIAGNOSTIC_CODES
    // code / hint are currently produced as string literals or NULL; no owned
    // storage to free. When pushDiagnostic grows to accept caller-supplied
    // heap strings for those fields they must be duplicated here as well.
    d->code = NULL;
    d->hint = NULL;
#endif
}

void diagsink_clear(VM* vm, DiagnosticSink* sink) {
    for (size_t i = 0; i < sink->count; i++) {
        free_entry_strings(vm, &sink->items[i]);
    }
    sink->count = 0;
    sink->has_error = false;
}

void diagsink_free(VM* vm, DiagnosticSink* sink) {
    diagsink_clear(vm, sink);
    if (sink->items != NULL) {
        FREE_ARRAY(vm, ZymDiagnostic, sink->items, sink->capacity);
    }
    sink->items = NULL;
    sink->capacity = 0;
}

static char* dup_message(VM* vm, const char* src) {
    size_t len = strlen(src);
    char* dst = (char*)reallocate(vm, NULL, 0, len + 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

void pushDiagnosticV(VM* vm,
                     ZymDiagSeverity severity,
                     ZymFileId fileId,
                     int startByte,
                     int length,
                     int line,
                     int column,
                     const char* fmt,
                     va_list args) {
    DiagnosticSink* sink = &vm->diagnostics;

    // Format message into a stack buffer, then duplicate into allocator storage.
    char stack_buf[1024];
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, copy);
    va_end(copy);
    const char* message;
    if (needed < 0) {
        message = dup_message(vm, "(diagnostic formatting failed)");
    } else if ((size_t)needed < sizeof(stack_buf)) {
        message = dup_message(vm, stack_buf);
    } else {
        size_t cap = (size_t)needed + 1;
        char* heap = (char*)reallocate(vm, NULL, 0, cap);
        vsnprintf(heap, cap, fmt, args);
        message = heap;
    }

    if (sink->count + 1 > sink->capacity) {
        size_t old_capacity = sink->capacity;
        size_t new_capacity = old_capacity < 8 ? 8 : old_capacity * 2;
        sink->items = (ZymDiagnostic*)reallocate(vm, sink->items,
                                                  sizeof(ZymDiagnostic) * old_capacity,
                                                  sizeof(ZymDiagnostic) * new_capacity);
        sink->capacity = new_capacity;
    }

    ZymDiagnostic* d = &sink->items[sink->count++];
    d->severity = severity;
    d->fileId = fileId;
    d->startByte = startByte;
    d->length = length;
    d->line = line;
    d->column = column;
    d->message = message;
#if ZYM_HAS_DIAGNOSTIC_CODES
    d->code = NULL;
    d->hint = NULL;
#endif

    if (severity == ZYM_DIAG_ERROR) {
        sink->has_error = true;
    }
}

void pushDiagnostic(VM* vm,
                    ZymDiagSeverity severity,
                    ZymFileId fileId,
                    int startByte,
                    int length,
                    int line,
                    int column,
                    const char* fmt,
                    ...) {
    va_list args;
    va_start(args, fmt);
    pushDiagnosticV(vm, severity, fileId, startByte, length, line, column, fmt, args);
    va_end(args);
}
