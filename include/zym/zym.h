#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

// =============================================================================
// CORE TYPES
// =============================================================================

typedef struct VM ZymVM;
typedef struct Chunk ZymChunk;
typedef struct LineMap ZymLineMap;
typedef uint64_t ZymValue;

typedef struct CompilerConfig {
    bool include_line_info;
} ZymCompilerConfig;

typedef enum {
    ZYM_STATUS_OK,
    ZYM_STATUS_COMPILE_ERROR,
    ZYM_STATUS_RUNTIME_ERROR
} ZymStatus;

// Error sentinel for native functions (distinct from NULL_VAL using tag 5)
#define ZYM_ERROR ((ZymValue)0x7ff8000000000005ULL)

// Control transfer sentinel for native functions (tag 6)
// Used by continuation operations (capture, abort) to indicate VM state has been
// modified and normal return value handling should be skipped
#define ZYM_CONTROL_TRANSFER ((ZymValue)0x7ff8000000000006ULL)

// =============================================================================
// VM LIFECYCLE
// =============================================================================

ZymVM* zym_newVM();
void zym_freeVM(ZymVM* vm);

// =============================================================================
// ERROR CALLBACK
// =============================================================================

// Error callback signature.
// Parameters:
//   vm:        The VM that produced the error
//   type:      ZYM_STATUS_COMPILE_ERROR or ZYM_STATUS_RUNTIME_ERROR
//   file:      Source file name (may be NULL)
//   line:      Line number (-1 if unknown)
//   message:   The fully formatted error message (includes stack trace for runtime errors)
//   user_data: Opaque pointer passed through from zym_setErrorCallback
typedef void (*ZymErrorCallback)(ZymVM* vm, ZymStatus type, const char* file,
                                 int line, const char* message, void* user_data);

// Set an error callback on the VM. When set, all error messages (compile errors,
// parse errors, runtime errors) are routed to the callback instead of stderr.
// Pass NULL to restore default behavior (fprintf to stderr).
void zym_setErrorCallback(ZymVM* vm, ZymErrorCallback callback, void* user_data);

// =============================================================================
// COMPILATION AND EXECUTION
// =============================================================================

ZymChunk* zym_newChunk(ZymVM* vm);
void zym_freeChunk(ZymVM* vm, ZymChunk* chunk);

ZymLineMap* zym_newLineMap(ZymVM* vm);
void zym_freeLineMap(ZymVM* vm, ZymLineMap* map);

ZymStatus zym_preprocess(ZymVM* vm, const char* source, ZymLineMap* map, const char** processedSource);
ZymStatus zym_compile(ZymVM* vm, const char* source, ZymChunk* chunk, ZymLineMap* map, const char* entry_file, ZymCompilerConfig config);
ZymStatus zym_runChunk(ZymVM* vm, ZymChunk* chunk);

ZymStatus zym_serializeChunk(ZymVM* vm, ZymCompilerConfig config, ZymChunk* chunk, char** out_buffer, size_t* out_size);
ZymStatus zym_deserializeChunk(ZymVM* vm, ZymChunk* chunk, const char* buffer, size_t size);

// =============================================================================
// NATIVE FUNCTION REGISTRATION
// =============================================================================

// Native function signature: ZymValue myFunc(ZymVM* vm, ZymValue arg1, ZymValue arg2, ...)
// Parameters are passed directly (not as an array)
// Signature format: "funcName(param1, ref param2, slot param3, val param4, clone param5)"
// Returns ZYM_STATUS_OK on success, ZYM_STATUS_COMPILE_ERROR on parse error
ZymStatus zym_defineNative(ZymVM* vm, const char* signature, void* func_ptr);

// =============================================================================
// NATIVE CLOSURES
// =============================================================================

// Create a native context with private data and optional finalizer
// The finalizer is called by GC when the context is collected
// Returns a Value that can be passed to zym_createNativeClosure
ZymValue zym_createNativeContext(ZymVM* vm, void* native_data, void (*finalizer)(ZymVM*, void*));

// Get native data from context
// Returns NULL if value is not a native context
void* zym_getNativeData(ZymValue context);

// Create a native closure bound to context
// Signature format: "funcName(param1, ref param2, ...)" - same as zym_defineNative
// Native closure function signature: ZymValue myFunc(ZymVM* vm, ZymValue context, ZymValue arg1, ...)
// Context is passed as the first argument after vm
ZymValue zym_createNativeClosure(ZymVM* vm, const char* signature, void* func_ptr, ZymValue context);

// Get the context from a native closure
// Returns the context value that was bound when the closure was created
// Returns ZYM_NULL if the value is not a native closure
ZymValue zym_getClosureContext(ZymValue closure);

// Create a native reference pointing to a Value field in native context data
// Parameters:
//   vm: VM instance
//   context: Native context containing the data
//   value_offset: offsetof(YourStruct, your_value_field)
//   get_hook: Optional getter (called after read, can transform/log)
//   set_hook: Optional setter (called before write, can validate/clamp)
// Returns: Native reference object
ZymValue zym_createNativeReference(ZymVM* vm, ZymValue context, size_t value_offset,
                                   ZymValue (*get_hook)(ZymVM*, ZymValue, ZymValue),
                                   void (*set_hook)(ZymVM*, ZymValue, ZymValue));

// =============================================================================
// FUNCTION OVERLOADING (DISPATCHER)
// =============================================================================

// Create a dispatcher for overloaded functions (max 8 overloads)
// A dispatcher can hold multiple closures with different arities
// When called, it automatically dispatches to the matching arity
ZymValue zym_createDispatcher(ZymVM* vm);

// Add a closure to a dispatcher
// dispatcher: The dispatcher value created with zym_createDispatcher
// closure: A closure created with zym_createNativeClosure
// Returns: true on success, false if dispatcher is full (>8 overloads)
bool zym_addOverload(ZymVM* vm, ZymValue dispatcher, ZymValue closure);

// =============================================================================
// VALUE TYPE CHECKING
// =============================================================================

bool zym_isNull(ZymValue value);
bool zym_isBool(ZymValue value);
bool zym_isNumber(ZymValue value);
bool zym_isString(ZymValue value);
bool zym_isList(ZymValue value);
bool zym_isMap(ZymValue value);
bool zym_isStruct(ZymValue value);
bool zym_isEnum(ZymValue value);
bool zym_isFunction(ZymValue value);
bool zym_isReference(ZymValue value);
bool zym_isNativeReference(ZymValue value);
bool zym_isClosure(ZymValue value);
bool zym_isPromptTag(ZymValue value);
bool zym_isContinuation(ZymValue value);

// =============================================================================
// VALUE EXTRACTION (SAFE)
// =============================================================================

// Safe extraction - returns false if type mismatch, true on success
bool zym_toBool(ZymValue value, bool* out);
bool zym_toNumber(ZymValue value, double* out);
bool zym_toString(ZymValue value, const char** out, int* length);      // Returns character count
bool zym_toStringBytes(ZymValue value, const char** out, int* byte_length); // Returns byte length

// =============================================================================
// VALUE EXTRACTION (UNSAFE - ASSUMES CORRECT TYPE)
// =============================================================================

// Direct extraction - undefined behavior if type is wrong
// Use only after type checking with zym_isX()
double zym_asNumber(ZymValue value);
bool zym_asBool(ZymValue value);
const char* zym_asCString(ZymValue value);  // Null-terminated, VM-owned

// =============================================================================
// VALUE INSPECTION
// =============================================================================

// Get the type name of a value as a string (e.g. "string", "number", "list")
const char* zym_typeName(ZymValue value);

// Get string length in UTF-8 characters (assumes value is a string)
int zym_stringLength(ZymValue value);

// Get string length in bytes (assumes value is a string)
int zym_stringByteLength(ZymValue value);

// =============================================================================
// VALUE DISPLAY
// =============================================================================

// Convert any value to its string representation (like the VM's print output)
// Returns a VM-managed ZymValue string. Returns ZYM_ERROR on failure.
ZymValue zym_valueToString(ZymVM* vm, ZymValue value);

// Print any value to stdout (same format as the VM's print statement)
void zym_printValue(ZymVM* vm, ZymValue value);

// =============================================================================
// VALUE CREATION
// =============================================================================

ZymValue zym_newNull(void);
ZymValue zym_newBool(bool value);
ZymValue zym_newNumber(double value);
ZymValue zym_newString(ZymVM* vm, const char* str);            // Copies and interns
ZymValue zym_newStringN(ZymVM* vm, const char* str, int len);  // With explicit length

ZymValue zym_newList(ZymVM* vm);
ZymValue zym_newMap(ZymVM* vm);

// Create struct by schema name (must be defined in script)
ZymValue zym_newStruct(ZymVM* vm, const char* structName);

// Create enum by type and variant name
ZymValue zym_newEnum(ZymVM* vm, const char* enumName, const char* variantName);

// =============================================================================
// LIST OPERATIONS
// =============================================================================

int zym_listLength(ZymValue list);
ZymValue zym_listGet(ZymVM* vm, ZymValue list, int index);           // Returns ZYM_ERROR on failure
bool zym_listSet(ZymVM* vm, ZymValue list, int index, ZymValue val); // Returns false on failure
bool zym_listAppend(ZymVM* vm, ZymValue list, ZymValue val);
bool zym_listInsert(ZymVM* vm, ZymValue list, int index, ZymValue val);
bool zym_listRemove(ZymVM* vm, ZymValue list, int index);

// =============================================================================
// MAP OPERATIONS
// =============================================================================

int zym_mapSize(ZymValue map);
ZymValue zym_mapGet(ZymVM* vm, ZymValue map, const char* key);      // Returns ZYM_ERROR if not found
bool zym_mapSet(ZymVM* vm, ZymValue map, const char* key, ZymValue val);
bool zym_mapHas(ZymValue map, const char* key);
bool zym_mapDelete(ZymVM* vm, ZymValue map, const char* key);

// Map iteration
typedef bool (*ZymMapIterFunc)(ZymVM* vm, const char* key, ZymValue val, void* userdata);
void zym_mapForEach(ZymVM* vm, ZymValue map, ZymMapIterFunc func, void* userdata);

// =============================================================================
// STRUCT OPERATIONS
// =============================================================================

ZymValue zym_structGet(ZymVM* vm, ZymValue structVal, const char* fieldName);
bool zym_structSet(ZymVM* vm, ZymValue structVal, const char* fieldName, ZymValue val);
bool zym_structHasField(ZymValue structVal, const char* fieldName);

const char* zym_structGetName(ZymValue structVal);
int zym_structFieldCount(ZymValue structVal);
const char* zym_structFieldNameAt(ZymValue structVal, int index);

// =============================================================================
// ENUM OPERATIONS
// =============================================================================

const char* zym_enumGetName(ZymVM* vm, ZymValue enumVal);        // Returns enum type name
const char* zym_enumGetVariant(ZymVM* vm, ZymValue enumVal);     // Returns variant name
bool zym_enumEquals(ZymValue a, ZymValue b);                     // Safe comparison
int zym_enumVariantIndex(ZymVM* vm, ZymValue enumVal);           // 0-based variant index

// =============================================================================
// REFERENCE OPERATIONS
// =============================================================================

ZymValue zym_deref(ZymVM* vm, ZymValue val);
bool zym_refSet(ZymVM* vm, ZymValue refVal, ZymValue newVal);

// =============================================================================
// CALLING SCRIPT FUNCTIONS FROM C
// =============================================================================

// Check if a function exists
bool zym_hasFunction(ZymVM* vm, const char* funcName, int arity);

// Call a script function with varargs
// Example: zym_call(vm, "add", 2, zym_newNumber(5), zym_newNumber(3))
ZymStatus zym_call(ZymVM* vm, const char* funcName, int argc, ...);

// Call a script function with argument array
ZymStatus zym_callv(ZymVM* vm, const char* funcName, int argc, ZymValue* argv);

// Get the result of the last call
ZymValue zym_getCallResult(ZymVM* vm);

// =============================================================================
// GC PROTECTION (TEMPORARY ROOTS)
// =============================================================================

// Protect a value from garbage collection
// Must be balanced with zym_popRoot()
void zym_pushRoot(ZymVM* vm, ZymValue val);
void zym_popRoot(ZymVM* vm);
ZymValue zym_peekRoot(ZymVM* vm, int depth);  // 0 = top of root stack

// Note: Native function arguments are automatically protected during the call

// =============================================================================
// ERROR HANDLING
// =============================================================================

// Report a runtime error from native code
// This will print the error and set the VM to error state
void zym_runtimeError(ZymVM* vm, const char* format, ...);

