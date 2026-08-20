#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// CUSTOM ALLOCATOR (defined in allocator.h, shared with internal headers)
// =============================================================================

#include "../../src/allocator.h"
#include "../../src/config.h"

// =============================================================================
// CORE TYPES
// =============================================================================

#ifndef ZYM_VM_FWD_DECLARED
#define ZYM_VM_FWD_DECLARED
typedef struct VM ZymVM;
#endif
typedef struct Chunk ZymChunk;
typedef uint64_t ZymValue;

#include "zym/sourcemap.h"
#include "zym/diagnostics.h"
#include "zym/frontend.h"

// Error sentinel for native functions (distinct from NULL_VAL using tag 5)
#define ZYM_ERROR ((ZymValue)0x7ff8000000000005ULL)

// =============================================================================
// VM LIFECYCLE
// =============================================================================

// Create a new VM with an optional custom allocator.
// If allocator is NULL, the default (malloc/free) allocator is used.
// The allocator is copied into the VM and used for all internal allocations.
ZymVM* zym_newVM(ZymAllocator* allocator);
void zym_freeVM(ZymVM* vm);

// Get the allocator associated with a VM
const ZymAllocator* zym_getAllocator(ZymVM* vm);

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

// Preprocess `source`, returning the expanded buffer through
// `processedSource`. If `source_map` is non-NULL and `origin_file_id`
// is valid, each expanded line is recorded in the map so downstream
// consumers (scanner, diagnostics, LSP) can translate expanded positions
// back to user-visible coordinates.
ZymStatus zym_preprocess(ZymVM* vm, const char* source,
                         ZymSourceMap* source_map, ZymFileId origin_file_id,
                         const char** processedSource);
void zym_freeProcessedSource(ZymVM* vm, const char* processedSource);

// Compile `source` into `chunk`. `source_map`, when non-NULL, is the
// per-expanded-line origin table produced by `zym_preprocess`. Pass
// NULL only when compiling raw unpreprocessed text.
//
// `out_tree` is the Phase 2 retained-parse-tree out-parameter.
// Pass NULL unless you want the AST handed back to you:
//   - Retention ON (ZYM_HAS_PARSE_TREE_RETENTION=1) + non-NULL out_tree +
//     compile succeeded: *out_tree receives a ZymParseTree* that the
//     caller owns and must release via zym_freeParseTree.
//   - Any other case: *out_tree is set to NULL and today's behavior
//     (AST freed at end of compile) applies.
// The parameter is accepted unconditionally so host code compiles
// against either build profile; pass NULL on MCU builds.
ZymStatus zym_compile(ZymVM* vm, const char* source, ZymChunk* chunk,
                      const ZymSourceMap* source_map,
                      const char* entry_file, ZymCompilerConfig config,
                      ZymParseTree** out_tree);

#if ZYM_HAS_PARSE_TREE_RETENTION
// Phase 3 — parse-only entry point (ZYM_COMPILE_PARSE_ONLY).
//
// Run scan + preprocess + parse only. No bytecode is produced, no Chunk
// is touched. On success, `*out_tree` receives a non-NULL ZymParseTree*
// that the caller owns (release via `zym_freeParseTree`); on parse
// failure, returns ZYM_STATUS_COMPILE_ERROR, leaves `*out_tree == NULL`,
// and pushes diagnostics to the VM's sink (drain with
// `zymGetDiagnostics()`).
//
// `source_map` must be the map produced by `zym_preprocess` (or NULL
// when compiling raw, unpreprocessed text). Semantically equivalent to
// `zym_compile(...)` up to and including parsing, then stopping before
// any codegen — so the retained tree, trivia buffer, and all spans are
// identical to what an EXECUTE-mode compile would hand back.
//
// Only declared when ZYM_HAS_PARSE_TREE_RETENTION=1; MCU builds cannot
// call it (compile error).
ZymStatus zym_parseOnly(ZymVM* vm, const char* source,
                        const ZymSourceMap* source_map,
                        const char* entry_file,
                        ZymParseTree** out_tree);
#endif

#if ZYM_HAS_SYMBOL_TABLE
// Phase 4 — check entry point (ZYM_COMPILE_CHECK).
//
// Run scan + preprocess + parse, then the parallel resolver. On success
// `*out_tree` receives the retained ZymParseTree* and `*out_table`
// receives the ZymSymbolTable* — both caller-owned. Release them with
// `zym_freeParseTree` and `zym_freeSymbolTable` respectively.
//
// The resolver is a *parallel* pass: it never influences code
// generation and is never invoked from `zym_compile`. It exists purely
// for tooling consumers (LSP, docs, outline).
//
// 4.1a behavior: the resolver records top-level declarations only
// (var / func / struct / enum). Lexical scopes, references, and
// closures arrive in 4.1b / 4.1c.
//
// Only declared when ZYM_HAS_SYMBOL_TABLE=1 (which implies
// ZYM_HAS_PARSE_TREE_RETENTION=1); MCU builds cannot call it (compile
// error).
ZymStatus zym_check(ZymVM* vm, const char* source,
                    const ZymSourceMap* source_map,
                    const char* entry_file,
                    ZymParseTree** out_tree,
                    ZymSymbolTable** out_table);
#endif

// =============================================================================
// COOPERATIVE CANCELLATION (Phase 1.5)
// =============================================================================
//
// The frontend (parser + compiler) polls vm->compile_cancelled at every
// statement / declaration boundary. An external thread (e.g. an LSP
// request that has been superseded) may call zym_requestCancel(vm) at
// any time to ask an in-flight compile to abort cooperatively. The
// aborted compile returns ZYM_STATUS_COMPILE_ERROR and pushes a single
// "Compilation cancelled." diagnostic; the host can distinguish cancel
// from a genuine compile error by calling zym_wasCancelled(vm).
//
// The flag is NOT cleared automatically at the start of a new compile
// (the API is explicit to avoid hiding a stale cancel). Call
// zym_clearCancel(vm) before the next compile. Writes from one thread
// are observed by the compile thread through the underlying
// `volatile sig_atomic_t`; this is sufficient for the one-way
// 0 -> 1 signal the parser/compiler need.
//
// NOTE: This API only governs the compile pipeline. It does not
// interrupt bytecode execution; runtime interruption is a separate
// concern handled by the preemption machinery.
void zym_requestCancel(ZymVM* vm);
void zym_clearCancel(ZymVM* vm);
bool zym_wasCancelled(const ZymVM* vm);

ZymStatus zym_runChunk(ZymVM* vm, ZymChunk* chunk);
ZymStatus zym_resume(ZymVM* vm);

// Run, transparently continuing past suspensions the host has no decision to
// make about -- today only ZYM_CAUSE_PREEMPT_BLOCKED, where a preempt callback
// could not be pushed because the call stack was exhausted. A watchdog, a host
// stop, and the memory ceiling all return ZYM_STATUS_SUSPENDED to the caller,
// because auto-resuming those would defeat them.
//
// Prefer these over hand-rolling a resume loop: a loop written as
// `while (s == ZYM_STATUS_SUSPENDED) s = zym_resume(vm);` silently disarms
// every watchdog on the VM.
ZymStatus zym_runToCompletion(ZymVM* vm, ZymChunk* chunk);
ZymStatus zym_callToCompletion(ZymVM* vm, const char* funcName,
                               int argc, ZymValue* argv);

#if ZYM_HAS_HOST_GUARD
// -----------------------------------------------------------------------------
// HOST GUARD (0.4.0: host-only; script has no preemption surface)
// -----------------------------------------------------------------------------
// A single countdown drives a small table of independent entries, so a host
// watchdog and a host UI pump coexist without fighting over one global timer.
// The dispatch loop cost is one decrement and one predicted branch per
// instruction. All table work happens on expiry.
//
// Authority is the point of the design. Every entry is host-owned and
// unmaskable: script cannot register, cancel, retune, observe, or shield
// against any of them. A host entry with a NULL callback aborts execution on
// expiry, which is the watchdog shape -- there is no script callback to
// intercept, mishandle, or loop inside.
//
// The whole section -- watchdog table, hard stop, memory ceiling -- exists
// only when the build carries the guard (ZYM_HAS_HOST_GUARD=1). A guard-off
// build removes the per-instruction check and with it every way to interrupt
// a run in flight; these declarations disappear so the mistake is caught at
// compile time.

// ZymPreemptId is declared in config.h alongside ZymVmInfo, which uses it.

#define ZYM_PREEMPT_ONESHOT   (1u << 1) // retire after firing instead of rearming

// Register a host-owned entry firing every `slice` instructions. Pass
// zym_newNull() as `callback` for an abort-on-expiry watchdog. Returns 0
// when the table is full.
ZymPreemptId zym_preemptRegister(ZymVM* vm, int slice,
                                 ZymValue callback, uint32_t flags);
bool zym_preemptUnregister(ZymVM* vm, ZymPreemptId id);
bool zym_preemptSetSlice(ZymVM* vm, ZymPreemptId id, int slice);
int  zym_preemptRemaining(ZymVM* vm, ZymPreemptId id);   // -1 if unknown
bool zym_preemptTrigger(ZymVM* vm, ZymPreemptId id);     // fire at next dispatch
int  zym_preemptCapacity(void);                          // build-time table size

// Occupancy: live entries in the table.
int  zym_preemptCount(const ZymVM* vm);

// Writes up to `max` live ids into `out` and returns the total number live,
// which may exceed `max`. Pass NULL to count only.
int  zym_preemptIds(const ZymVM* vm, ZymPreemptId* out, int max);

// -----------------------------------------------------------------------------
// HARD STOP
// -----------------------------------------------------------------------------
// Unmaskable and sticky. Checked before anything else, so an in-flight
// preempt callback or an empty entry table cannot
// suppress it. Never cleared by the VM: call zym_clearStop() before reusing
// the VM. Declared for cross-context writes, so it is safe to call from an
// ISR, a signal handler, or another thread.
//
// Execution suspends with ZYM_STATUS_SUSPENDED and cause ZYM_CAUSE_HOST_STOP.
// Deliberately NOT a runtime error: no diagnostic is pushed and no
// script-visible handler runs, so a sandboxed script cannot observe or
// intercept its own termination.
//
// A native that re-enters the VM must PROPAGATE ZYM_STATUS_SUSPENDED rather
// than treating it as an ordinary failure; swallowing it defeats the stop.
void zym_requestStop(ZymVM* vm);
void zym_clearStop(ZymVM* vm);
bool zym_stopRequested(const ZymVM* vm);
bool zym_isAborting(const ZymVM* vm);

// -----------------------------------------------------------------------------
// MEMORY CEILING
// -----------------------------------------------------------------------------
// A per-VM byte budget. 0 (the default) means unlimited.
//
// Crossing the ceiling does NOT fail the allocation. The request is satisfied
// -- the host allocator still has memory -- and the VM is then suspended at the
// next instruction boundary with ZYM_STATUS_SUSPENDED and cause
// ZYM_CAUSE_MEMORY_LIMIT, exactly like a watchdog.
// Failing the allocation instead would strand every caller in the VM that
// assumes allocation succeeds, and would leave the host nothing to recover
// from. The overshoot is therefore bounded by one allocation rather than zero.
//
// The condition is sticky, like the hard stop: resuming without clearing it
// suspends again immediately. The host's options are to raise the limit (which
// clears it automatically once usage is back under budget), free what it can
// and call zym_clearOom(), or discard the VM.
//
// A collection is attempted before the ceiling is declared crossed, so a
// program that merely produces garbage is never charged for it.
//
// This bounds script-driven allocation only. It does NOT make a genuine
// allocator failure recoverable -- that still terminates the process, as do
// the collector's own internal allocations.
void   zym_setMemoryLimit(ZymVM* vm, size_t bytes);
size_t zym_getMemoryLimit(const ZymVM* vm);
bool   zym_oomPending(const ZymVM* vm);
void   zym_clearOom(ZymVM* vm);

#endif // ZYM_HAS_HOST_GUARD

// Bytes currently allocated by this VM. Pure telemetry; available in every
// build, guard or not.
size_t zym_memoryUsed(const ZymVM* vm);

// -----------------------------------------------------------------------------
// VM STATE AND CAUSE
// -----------------------------------------------------------------------------
// A ZymStatus tells you what one call returned. These tell you what the VM
// *is*, and why -- readable at any time, including from inside a native while
// the VM is running, or from another context.
//
// The two are separate axes on purpose. `state` answers whether execution can
// continue; `cause` answers what put it there. New reasons to stop become new
// causes, so neither the state enum nor any existing branch on it has to grow.
//
// `cause` latches the reason for the last transition out of RUNNING and stays
// readable until the next run begins, so it can be inspected after the fact.
// The detail fields in ZymVmInfo are only meaningful for their own cause:
// `preempt_id` for the preemption causes, `bytes_wanted` for ZYM_CAUSE_MEMORY_LIMIT.
//
// `resumable` is the field to branch on when driving a resume loop: it folds
// together "is anything suspended" with "has every sticky condition been
// cleared", which is otherwise three flags the host has to check itself.
//
// See config.h for ZymVmState, ZymVmCause, and ZymVmInfo.
ZymVmState zym_vmState(const ZymVM* vm);
ZymVmCause zym_vmCause(const ZymVM* vm);
void       zym_vmInfo(const ZymVM* vm, ZymVmInfo* out);

ZymStatus zym_serializeChunk(ZymVM* vm, ZymCompilerConfig config, ZymChunk* chunk, char** out_buffer, size_t* out_size);
ZymStatus zym_deserializeChunk(ZymVM* vm, ZymChunk* chunk, const char* buffer, size_t size);

// =============================================================================
// NATIVE FUNCTION REGISTRATION
// =============================================================================

// Native function signature: ZymValue myFunc(ZymVM* vm, ZymValue arg1, ZymValue arg2, ...)
// Parameters are passed directly (not as an array)
// Signature format: "funcName(param1, param2, param3, param4, param5)"
// Returns ZYM_STATUS_OK on success, ZYM_STATUS_COMPILE_ERROR on parse error
ZymStatus zym_defineNative(ZymVM* vm, const char* signature, void* func_ptr);

// Register a variadic native function
// Fixed params are passed directly, followed by a variadic args array + count:
//   "print(...)"          -> ZymValue myFunc(ZymVM* vm, ZymValue* vargs, int vargc)
//   "format(template, ...)" -> ZymValue myFunc(ZymVM* vm, ZymValue template, ZymValue* vargs, int vargc)
//   "log(level, tag, ...)"  -> ZymValue myFunc(ZymVM* vm, ZymValue level, ZymValue tag, ZymValue* vargs, int vargc)
// vargs points to the remaining arguments after the fixed params, vargc is their count
// Signature format: "funcName(...)" for pure variadic (min 0 args)
//                   "funcName(a, b, ...)" for fixed + rest (min 2 args)
// Returns ZYM_STATUS_OK on success, ZYM_STATUS_COMPILE_ERROR on parse error
ZymStatus zym_defineNativeVariadic(ZymVM* vm, const char* signature, void* func_ptr);

// Define a global variable accessible from Zym code
// This sets the value directly in the VM's global table
ZymStatus zym_defineGlobal(ZymVM* vm, const char* name, ZymValue value);

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

// Create a variadic native closure bound to context
// Fixed params are passed directly after context, followed by variadic args array + count:
//   "func(...)"       -> ZymValue myFunc(ZymVM* vm, ZymValue context, ZymValue* vargs, int vargc)
//   "func(a, ...)"    -> ZymValue myFunc(ZymVM* vm, ZymValue context, ZymValue a, ZymValue* vargs, int vargc)
// Signature format: "funcName(...)" or "funcName(a, b, ...)"
ZymValue zym_createNativeClosureVariadic(ZymVM* vm, const char* signature, void* func_ptr, ZymValue context);

// Get the context from a native closure
// Returns the context value that was bound when the closure was created
// Returns ZYM_NULL if the value is not a native closure
ZymValue zym_getClosureContext(ZymValue closure);


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

// Set the variadic fallback on a dispatcher
// Called when no exact-arity overload matches
// closure: A native closure (variadic or fixed) to use as fallback
// min_arity: minimum number of args required (number of fixed params before ...)
// Returns: true on success, false if dispatcher is invalid
bool zym_setVariadicFallback(ZymVM* vm, ZymValue dispatcher, ZymValue closure, int min_arity);

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
bool zym_isClosure(ZymValue value);

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
// CALLING SCRIPT FUNCTIONS FROM C
// =============================================================================

// Check if a function exists at the exact fixed-arity slot `name@arity`.
// This is the strict slot-presence question: does `funcName@arity` literally
// resolve to a callable? Variadic mangling (`name@vF`) is NOT consulted.
bool zym_hasFunction(ZymVM* vm, const char* funcName, int arity);

// Check if any callable with the given base name is reachable in the VM's
// globals — at any fixed arity (`name@0..MAX_NATIVE_ARITY`) or any variadic
// prefix (`name@v0..vMAX_NATIVE_ARITY`). Mirrors how the compiler's own
// dispatcher discovers a base name; useful for "does this name resolve at
// all?" introspection from embedders.
bool zym_hasAnyFunction(ZymVM* vm, const char* funcName);

// Check if calling `funcName` with exactly `argc` args can dispatch without
// raising a runtime error. Returns true iff either:
//   (a) `funcName@argc` is bound (fixed-arity exact match), or
//   (b) some `funcName@vF` is bound where `argc >= F` (variadic acceptance).
// This is the question users actually want answered when guarding a call.
bool zym_canCallWith(ZymVM* vm, const char* funcName, int argc);

// ---- Calling into the VM ---------------------------------------------------
//
// READ THIS BEFORE CALLING IN FROM A NATIVE. These entry points are re-entrant
// -- a native may call back into the same VM -- but re-entrancy is not free,
// and three properties are the caller's responsibility, not the VM's:
//
// 1. NATIVES ARE ATOMIC WITH RESPECT TO SUSPENSION. While your C frame is
//    live, the VM cannot suspend out past it: doing so would mean slicing
//    the C stack, and there is no protocol for a native to be unwound and
//    rebuilt. Script that runs underneath your frame is still fully
//    guardable in place -- a preempt callback fires, runs, and execution
//    continues -- but anything that has to hand control back to the host
//    cannot cross you. If your API re-enters the VM, say so in its
//    documentation. Callers who arm a watchdog across it need to know it is
//    not a plain call.
//
// 2. A FAILED VM STAYS FAILED. Calling in while `zym_vmState()` is
//    ZYM_STATE_FAILED is permitted -- a teardown or a diagnostic is a fair
//    reason -- and a call that succeeds does NOT clear the failure. The state
//    and cause you observed before the call are still there afterwards. Check
//    the state before deciding the VM is healthy; a successful call is not
//    evidence of one.
//
// 3. A SUSPENSION SURVIVES YOUR CALL. Calling into a VM parked on a preemption
//    or a stop is the supported way to run an event pump, and the parked run
//    is restored when your call returns. What you must not do is start a
//    nested run or resume from inside a preempt callback.
//
// Call a script function with varargs
// Example: zym_call(vm, "add", 2, zym_newNumber(5), zym_newNumber(3))
ZymStatus zym_call(ZymVM* vm, const char* funcName, int argc, ...);

// Call a script function with argument array
ZymStatus zym_callv(ZymVM* vm, const char* funcName, int argc, ZymValue* argv);

// Call a closure directly. Note this one reports through its return status
// only -- unlike zym_callv it does not write vm_state/vm_cause at all, so a
// failure here leaves whatever the VM was last reporting in place.
ZymStatus zym_callClosurev(ZymVM* vm, ZymValue closure, int argc, ZymValue* argv);

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

#ifdef __cplusplus
}
#endif

