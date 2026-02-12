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
var distance = Math.sqrt(Math.pow(p.x, 2) + Math.pow(p.y, 2));
print("Distance: " + str(distance));

enum Color { Red, Green, Blue }
var c = Color.Red;
```

```javascript
// memory semantics, the distinction is observable
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
// continuations, TCO, and ref, working together
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

- **Embeddable** — one static library, dropped into any C or C++ project, no global state, no dependencies beyond the C standard library, by design.
- **Small** — the entire language fits in a compact codebase, minimal footprint for embedded and resource-constrained environments.
- **Rich built-in types** — strings, lists, maps, structs, enums, all first-class, all doing what you'd expect.
- **First-class functions** — closures, higher-order functions, anonymous functions, natural and lightweight.
- **Real memory semantics** — `ref`, `slot`, and `val` modifiers control how values move between caller and callee, the distinction is observable.
- **Delimited continuations** — fibers, coroutines, generators, async/await, algebraic effects, all from a small set of primitives.
- **Script-directed tail-call optimization** — `@tco` with `aggressive`, `safe`, `smart`, and `off` modes, stack behavior is predictable.
- **Preemptive scheduling** — instruction-count-based time slicing at the VM level, build fair schedulers without cooperative yields, correctness is yours.
- **Thread-safe VM** — each instance owns its heap, globals, and execution state, nothing shared.
- **Bytecode serialization** — compile once, distribute bytecode, run anywhere, this is efficient.
- **Native C API** — register functions, bind closures to C data, expose references with get/set hooks, consistently named `zym_*` prefixed API.

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

Scripts call `add(10, 20)` and get `30`.

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

MIT — see [LICENSE](LICENSE). All remaining behavior shall conform thereto.
