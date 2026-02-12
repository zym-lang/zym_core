<p align="center">
  <h1 align="center">Zym</h1>
  <p align="center"><strong>Control without the ceremony.</strong></p>
  <p align="center"><em>Fast. Simple. Powerful.</em></p>
  <p align="center">
    A control-oriented embeddable scripting language with explicit memory semantics,<br>
    first-class continuations, and script-directed tail-call optimization —<br>
    built as a compact C runtime for performance-sensitive and embedded environments.
  </p>
</p>

---

```javascript
func fibonacci(n) {
    if (n < 2) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

print(fibonacci(30));
```

```javascript
// First-class functions and closures
func makeGreeter(greeting) {
    return func(name) {
        print(greeting + ", " + name + "!");
    };
}

var hello = makeGreeter("Hello");
hello("world");  // Hello, world!
```

```javascript
// Structs, enums, and pattern-like control flow
struct Point { x, y }

var p = Point(3, 4);
var distance = Math.sqrt(Math.pow(p.x, 2) + Math.pow(p.y, 2));
print("Distance: " + str(distance));

enum Color { Red, Green, Blue }
var c = Color.Red;
```

```javascript
// Real memory semantics — ref, slot, and val parameter modifiers
func makeMachine() {
    var total = 0
    var totalRef = ref total

    func bump(ref by) { total = total + by }

    func mix(slot ext, ref mirror, val snap) {
        ext = ext + 1       // writes back to caller's variable
        mirror = ext         // writes through to caller's ref
        bump(ext)            // total += ext via ref
        snap[0] = 999       // local copy only — caller unchanged
        totalRef = totalRef + 1
    }

    return { mix: mix, total: func() { return total; } }
}

var m = makeMachine()
var x = 10
var y = 0
var arr = [5, 6, 7]

m.mix(x, y, arr)
assert(x == 11,           "slot wrote back")
assert(y == 11,            "ref wrote through")
assert(arr[0] == 5,        "val kept caller safe")
assert(m.total() == 12,    "ref chain updated total")
```

```javascript
// Cooperative fibers via delimited continuations
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
// Continuations + forced TCO + ref — working together
var TAG = Cont.newPrompt("cap-val-nested");

@tco aggressive
func test_nested(ref counter, stepsLeft) {
    if (stepsLeft == 0) return counter;

    if (counter == 3) {
        var x = Cont.capture(TAG);
        if (x != null) counter = counter + x;
    }

    counter = counter + 1;
    return test_nested(counter, stepsLeft - 1);
}

var base = 0;

Cont.pushPrompt(TAG);
var k = test_nested(base, 10);

Cont.pushPrompt(TAG);
Cont.resume(k, 5);

assert(base == 15, "nested capture value delivered");
```

## Why Zym?

- **Embeddable** — Drop a single static library into any C or C++ project. Clean API, no global state, no dependencies beyond the C standard library.
- **Small** — The entire language fits in a compact codebase. Minimal footprint for embedded and resource-constrained environments.
- **Familiar syntax** — If you know JavaScript, Python, or Lua, you already know most of Zym.
- **Rich built-in types** — Strings (UTF-8), lists, maps, structs, and enums are all first-class.
- **First-class functions** — Closures, higher-order functions, and anonymous functions are natural and lightweight.
- **Real memory semantics** — `ref`, `slot`, and `val` parameter modifiers give precise control over how values flow between caller and callee. References are first-class and composable.
- **Delimited continuations** — Build fibers, coroutines, generators, async/await, and algebraic effects from a small set of primitives. No special syntax needed — it's all library code.
- **Script-directed tail-call optimization** — `@tco` allows explicit control over tail-call behavior (`aggressive`, `safe`, `smart`, `off`), enabling predictable stack usage.
- **Preemptive scheduling** — VM-level preemption — Instruction-count-based time slicing for building fair schedulers without cooperative yields.
- **Thread-safe VM** — Each VM instance is independent and can run on its own thread.
- **Bytecode serialization** — Compile once, distribute bytecode, run anywhere.
- **Native C API** — Register C functions, create closures bound to C data, expose references with get/set hooks — all through a clean `zym_*` prefixed API.

## Building

### As a standalone library

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Produces `libzym_core.a` (Unix) or `zym_core.lib` (Windows).

### As a Git submodule

```bash
git submodule add https://github.com/zym-lang/zym_core.git zym_core
```

```cmake
add_subdirectory(zym_core)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE zym_core)
```

Include headers with `#include "zym/zym.h"` — paths are set up automatically.

### Build options

| Option | Default | Description |
|--------|---------|-------------|
| `ZYM_RUNTIME_ONLY` | `OFF` | Omit the compiler for a smaller runtime-only build |

## Embedding

```c
#include "zym/zym.h"

int main() {
    ZymVM* vm = zym_newVM();

    ZymChunk* chunk = zym_newChunk(vm);
    ZymLineMap* map = zym_newLineMap(vm);
    ZymCompilerConfig config = { .include_line_info = true };

    zym_compile(vm, "print(\"Hello from Zym!\")", chunk, map, "main", config);
    zym_runChunk(vm, chunk);

    zym_freeChunk(vm, chunk);
    zym_freeLineMap(vm, map);
    zym_freeVM(vm);
    return 0;
}
```

### Extending with native functions

```c
ZymValue myAdd(ZymVM* vm, ZymValue a, ZymValue b) {
    return zym_newNumber(zym_asNumber(a) + zym_asNumber(b));
}

zym_defineNative(vm, "add(a, b)", myAdd);
```

Now Zym scripts can call `add(10, 20)` and get `30`.

## Project structure

```
zym_core/
├── include/zym/     Public API headers
│   ├── zym.h            Core API (VM, values, natives, GC)
│   ├── debug.h          Disassembly utilities
│   ├── module_loader.h  Module loading (compiler builds)
│   └── utf8.h           UTF-8 string utilities
└── src/             Implementation (internal)
```

## License

MIT — see [LICENSE](LICENSE) for details.
