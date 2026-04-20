<p align="center">
  <h1 align="center">Zym</h1>
  <p align="center"><strong>Control without the ceremony.</strong></p>
  <p align="center"><em>Fast. Simple. Powerful.</em></p>
  <p align="center">
    A compact embeddable scripting core for systems that need control where it matters.
  </p>
</p>

---

If you've written JavaScript, Python, or Lua, Zym reads like you'd expect.

```javascript
func fibonacci(n) {
    if (n < 2) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

print(fibonacci(30));
```

```javascript
// closures, naturally
func makeGreeter(greeting) {
    return func(name) {
        print(greeting + ", " + name + "!");
    };
}

var hello = makeGreeter("Hello");
hello("world");  // Hello, world!
```

```javascript
// structs, enums, first-class
struct Point { x, y }

var p = Point(3, 4);
var distance = sqrt(pow(p.x, 2) + pow(p.y, 2));
print("Distance: " + str(distance));

enum Color { Red, Green, Blue }
var c = Color.Red;
```

```javascript
// cooperative fibers, all from continuations
var tag = Cont.newPrompt("fiber");

func yield() {
    return Cont.capture(tag);
}

func worker(name) {
    print(name + ": step 1");
    yield();
    print(name + ": step 2");
    yield();
    print(name + ": done");
}
```

```javascript
// TCO — never overflow the stack
@tco aggressive
func sum(n, acc) {
    if (n == 0) return acc;
    return sum(n - 1, acc + n);
}

print(sum(1000000, 0));  // no stack overflow
```

## Why Zym?

- **Embeddable** — one static library, dropped into any C or C++ project, no global state, no dependencies beyond the C standard library, by design.
- **Small** — the entire language fits in a compact codebase, minimal footprint for embedded and resource-constrained environments.
- **Rich built-in types** — strings, lists, maps, structs, enums, all first-class, all doing what you'd expect.
- **First-class functions** — closures, higher-order functions, anonymous functions, natural and lightweight.
- **Delimited continuations** — fibers, coroutines, generators, async/await, algebraic effects, all from a small set of primitives.
- **Script-directed tail-call optimization** — `@tco` with `aggressive`, `safe`, and `off` modes, stack behavior is predictable.
- **Preemptive scheduling** — instruction-count-based time slicing at the VM level, build fair schedulers without cooperative yields, correctness is yours.
- **Thread-safe VM** — each instance owns its heap, globals, and execution state, nothing shared.
- **Bytecode serialization** — compile once, distribute bytecode, run anywhere, this is efficient.
- **Native C API** — register functions, bind closures to C data, consistently named `zym_*` prefixed API.

## Building

### As a standalone library

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Produces `libzym_core.a` or `zym_core.lib`, one artifact, link it.

### As a Git submodule

```bash
git submodule add https://github.com/zym-lang/zym_core.git zym_core
```

```cmake
add_subdirectory(zym_core)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE zym_core)
```

Include with `#include "zym/zym.h"`, configuration is minimal.

### Build options

| Option | Default | Description |
|--------|---------|-------------|
| `ZYM_RUNTIME_ONLY` | `OFF` | Omit the compiler for a smaller runtime-only build |
| `ZYM_ENABLE_LSP_SURFACE` | `ON` | Umbrella flag: parse-tree retention + symbol table + native metadata + diagnostic codes. Turn off for minimum-footprint embeds. |
| `ZYM_ENABLE_PARSE_TREE_RETENTION` | follows umbrella | Retain the AST past codegen so embedders can query it. Required by the symbol table. |
| `ZYM_ENABLE_SYMBOL_TABLE` | follows umbrella | Run the resolver and expose the symbol-table query API. Implies `ZYM_ENABLE_PARSE_TREE_RETENTION`. |
| `ZYM_ENABLE_NATIVE_METADATA` | follows umbrella | Store the extended `ZymNativeInfo` fields (`summary`, `docs`, `params`, `since`, `deprecated`). Off strips these strings from rodata. |
| `ZYM_ENABLE_DIAGNOSTIC_CODES` | follows umbrella | Ship the stable diagnostic-code table and the `code`/`hint` fields on `ZymDiagnostic`. Off keeps severity/message/range only. |

Flags are build-time, not runtime. Turning a flag off removes tooling surface, not language behavior — a script that runs with `ZYM_ENABLE_LSP_SURFACE=ON` runs identically with it off.

Example for a minimum-footprint MCU build:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DZYM_ENABLE_LSP_SURFACE=OFF
cmake --build build
```

## Embedding

Compilation and execution are explicit, separate stages — the host keeps full control over native registration, caching, and error surfacing:

```c
#include "zym/zym.h"

int main() {
    ZymVM* vm = zym_newVM(NULL);

    ZymChunk* chunk = zym_newChunk(vm);
    ZymCompilerConfig config = { .include_line_info = true };

    zym_compile(vm, "print(\"Hello from Zym!\")", chunk,
                /* source_map = */ NULL, "main", config);
    zym_runChunk(vm, chunk);

    zym_freeChunk(vm, chunk);
    zym_freeVM(vm);
    return 0;
}
```

For a full preprocess → compile → run pipeline with `ZymSourceMap` origin tracking, `zym_registerSourceFile` for diagnostic file ids, and multi-file module loading, see the [Embedding Guide](https://zym-lang.org/docs-embedding.html).

### Structured diagnostics

Compile-time errors (scanner, parser, compiler) are recorded on the VM as `ZymDiagnostic` records — no `stderr` side effects in the core. The host drains them after any non-OK status:

```c
if (zym_compile(vm, src, chunk, map, "main.zym", cfg) != ZYM_STATUS_OK) {
    int n = 0;
    const ZymDiagnostic* diags = zymGetDiagnostics(vm, &n);
    for (int i = 0; i < n; ++i) {
        fprintf(stderr, "%s:%d:%d: %s\n",
                "main.zym", diags[i].line, diags[i].column, diags[i].message);
    }
    zymClearDiagnostics(vm);
}
```

Diagnostics carry byte-granular spans (`fileId`, `startByte`, `length`) when the originating token is known, making them directly consumable by tooling.

### Cooperative cancellation

An in-flight compile can be aborted from another thread via `zym_requestCancel(vm)` — useful for LSP hosts, REPLs, or compile-time budgets. The compile returns `ZYM_STATUS_COMPILE_ERROR` + a "Compilation cancelled." diagnostic; call `zym_wasCancelled(vm)` to distinguish cancellation from a real error, and `zym_clearCancel(vm)` before the next compile.

### Extending with native functions

```c
ZymValue myAdd(ZymVM* vm, ZymValue a, ZymValue b) {
    return zym_newNumber(zym_asNumber(a) + zym_asNumber(b));
}

zym_defineNative(vm, "add(a, b)", myAdd);
```

Scripts call `add(10, 20)` and get `30`.

## Documentation

Full language guide, API references, and embedding tutorials are available at **[zym-lang.org](https://zym-lang.org)**.

Try Zym instantly in the browser: **[Playground](https://zym-lang.org/playground.html)**

The docs cover:

- **[Language Guide](https://zym-lang.org/docs-language.html)** — variables, types, operators, control flow, functions, and a tour of all language features
- **[Language Semantics](https://zym-lang.org/docs-control-flow.html)** — deep-dive pages for control flow, functions, structs & enums, TCO, and spread
- **[Core Modules](https://zym-lang.org/docs-strings.html)** — built-in APIs for strings, math, lists, maps, conversions, error handling, and GC
- **[Continuations & Preemption](https://zym-lang.org/docs-continuations.html)** — delimited continuations, fibers, generators, and preemptive scheduling
- **[Macros & Preprocessor](https://zym-lang.org/docs-macros.html)** — compile-time macros, conditional compilation, and block macros
- **[Embedding Guide](https://zym-lang.org/docs-embedding.html)** — VM lifecycle, native functions, two-way FFI, and the `zym_*` C API

## Project structure

```
zym_core/
├── include/zym/       Public API headers
│   ├── zym.h              Core API (VM lifecycle, compile pipeline, values, natives, GC)
│   ├── sourcemap.h        ZymFileId + ZymSourceMap (preprocessor origin tracking)
│   ├── diagnostics.h      ZymDiagnostic + zymGetDiagnostics / zymClearDiagnostics
│   ├── config.h           Generated at build time — ZYM_HAS_* flag predicates
│   ├── debug.h            Disassembly utilities
│   ├── module_loader.h    Multi-file module loading (compiler builds)
│   └── utf8.h             UTF-8 string utilities
└── src/               Implementation (internal)
```

## License

MIT — see [LICENSE](LICENSE). All remaining behavior shall conform thereto.
