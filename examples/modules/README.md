# Vix Module System Examples

This directory demonstrates the multi-file module system in Vix.

## Overview

The Vix module system allows you to organize code across multiple files:
- **Module declaration**: `mod module_name` or `pub mod module_name`
- **Import statement**: `import module::function` or `import module::Type as Alias`
- **Qualified calls**: `module::function()` for functions from imported modules

## File Resolution

When you declare `mod math`, the compiler looks for:
1. `math.vix` in the same directory
2. `math/mod.vix` as a subdirectory module

## Examples

### 1. Basic Module (`basic_example.vix`)
Simple two-file program with a math module.

```bash
# Compile with self backend (recommended)
vixc examples/modules/basic_example.vix --backend=self -o output
./output  # Returns 17
```

### 2. Multiple Modules (`multi_module.vix`)
Demonstrates importing multiple modules.

### 3. Module with Structs (`structs_example.vix`)
Shows how to define and use structs across modules.

## Compilation

**Using Self Backend (Recommended)**:
```bash
vixc your_file.vix --backend=self -o output
```

**View Module Graph**:
```bash
vixc your_file.vix --module-graph
```

**Debug MIR Output**:
```bash
vixc your_file.vix --debug=mir
```

## Known Limitations

- LLVM backend currently has issues with multi-file compilation
- Use `--backend=self` or `--backend=self-lir` for multi-file programs
- Import statements are parsed but full error checking is pending

## Syntax Reference

### Module Declaration
```vix
// In main.vix
mod math              // Private module (not re-exported)
pub mod utilities     // Public module (can be re-exported)

fn main(): i32 {
    return 0
}
```

### Module File (math.vix)
```vix
// All top-level functions are public by default
pub fn add(a: i32, b: i32): i32 {
    return a + b
}

pub fn multiply(x: i32, y: i32): i32 {
    return x * y
}
```

### Import and Usage
```vix
mod math

import math::add
import math::multiply as mul

fn main(): i32 {
    let sum = math::add(3, 4)      // Qualified call
    let product = math::mul(2, 5)  // Using alias
    return sum + product           // Returns 17
}
```

### Nested Modules (utils/mod.vix)
```vix
mod utils

import utils::string_helpers

fn main(): i32 {
    return utils::string_helpers::length("hello")
}
```
