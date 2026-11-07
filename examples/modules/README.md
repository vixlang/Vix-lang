# Vix Module System Examples

This directory demonstrates the two ways one file joins another.

## Overview

- `include "path.vix"` splices a file in (flat namespace)
- `use "path.vix" as name` loads a module; call it as `name::item`

The path identifies the module and the alias names it. Neither is derived
from the other, and `as` is required: five files in this tree are
called `mod.vix`, so a namespace guessed from a file name cannot work.

There is no `import` and no `mod` any more; both report a
migration message rather than failing as unknown syntax.

## File Resolution

`use "math.vix" as math` resolves relative to the file containing it, and
the path is normalised lexically. A directory module is reached with
`use "utils" as utils`, which finds `utils.vix` or `utils/mod.vix`.

## Examples

| File | Shows |
|------|-------|
| `basic_example.vix` | Two modules, qualified calls. Exit code 17 |
| `multi_module.vix` | Three modules, a struct defined in one and used from another |
| `nested_module.vix` | Directory module with a submodule |
| `qualified_calls.vix` | Cross-module calls through `module::function` |

## Compiling and running

`vixc -o` cannot link on macOS: the embedded linker only implements the
ELF and COFF drivers. Emit an object and link it yourself, or use
`sh scripts/run-vix.sh`:

```bash
vixc examples/modules/basic_example.vix -obj -o output.o
clang++ output.o runtime/runtime.o -o output
./output                    # exit code 17
```

`python3 tests/modules_e2e.py` builds, links and runs every multi-module
program in the repository and checks the exit code.

## Syntax Reference

```vix
use "math.vix" as math

fn main(): i32
{
    return math::add(3, 4)
}
```

A module file:

```vix
pub fn add(a: i32, b: i32): i32
{
    return a + b
}
```

A directory module, `utils/mod.vix`:

```vix
use "string_helpers.vix" as string_helpers

pub fn greet(name: string): i32
{
    print(name)
    return 0
}
```

## Known Limitations

- `pub` is **not** enforced: every declaration is visible after merging.
- Structs and ADTs are merged **without** a module namespace prefix, so two
  modules declaring the same struct or ADT name collide (for ADTs the second
  is silently dropped).
- The `self` backends are x86_64 only and refuse to run on arm64.
