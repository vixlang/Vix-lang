# Vix Module System Examples

This directory demonstrates the multi-file module system.

## Overview

- **Module declaration**: `mod name` / `mod "path.vix"`
- **Qualified calls**: `module::function()`
- **Textual inclusion**: `import "path.vix"` splices another file in (flat namespace)

There is no `import module::symbol` form: it used to be parsed and then silently
dropped, and it is now rejected. Call cross-module symbols by their qualified
name instead.

## File Resolution

`mod math` looks for, in the directory of the importing file:

1. `math.vix`
2. `math/mod.vix`

If both exist the compiler reports an ambiguity.

## Examples

| File | Shows |
|------|-------|
| `basic_example.vix` | Two modules, qualified calls. Exit code 17 |
| `multi_module.vix` | Three modules, structs defined in one and used from another |
| `nested_module.vix` | Directory module (`utils/mod.vix`) with a submodule |
| `qualified_calls.vix` | Cross-module calls, all through `module::function` |

## Compiling and running

`vixc -o` cannot link on macOS: the embedded linker only implements the ELF and
COFF drivers. Emit an object file and link it yourself, or use
`sh scripts/run-vix.sh`:

```bash
vixc examples/modules/basic_example.vix -obj -o output.o
clang++ output.o runtime/runtime.o -o output
./output                    # exit code 17
```

`python3 tests/modules_e2e.py` builds, links and runs every multi-module example
in this directory and checks the exit code.

## Inspecting

```bash
vixc your_file.vix --module-graph     # module tree
vixc your_file.vix --debug=llvm       # generated LLVM IR
```

## Syntax Reference

### Entry file

```vix
mod math

fn main(): i32
{
    return math::add(3, 4)
}
```

### Module file (math.vix)

```vix
pub fn add(a: i32, b: i32): i32
{
    return a + b
}

pub fn multiply(x: i32, y: i32): i32
{
    return x * y
}
```

### Directory module (utils/mod.vix)

```vix
pub mod string_helpers

pub fn greet(name: string): i32
{
    print(name)
    return 0
}
```

`utils::string_helpers::length(...)` then reaches inside `utils/string_helpers.vix`.

## Known Limitations

- `pub` is **not** enforced: every declaration is visible after merging.
- Structs and ADTs are merged **without** a module namespace prefix, so two
  modules that declare the same struct/ADT name collide (or, for ADTs, the
  second is silently dropped).
- The `self` backends are x86_64 only and refuse to run on arm64.
