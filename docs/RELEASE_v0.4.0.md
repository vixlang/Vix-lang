# Vix 0.4.0 Release Notes

## Overview

Vix 0.4.0 introduces significant language improvements including receiver methods for method syntax, fixed ADT payload handling, new generic syntax, and compiler flags for static linking and library paths.

## New Features

### 1. Receiver Methods (Method Syntax)

Vix defines methods directly with a receiver declaration:

```vix
type Point = struct {
    x: i32,
    y: i32
}

fn (Point) new(x: i32, y: i32): Point {
    return Point{ x: x, y: y }
}
fn (&Point) distance(): f64 {
    return 5.0
}

fn main(): i32 {
    let p = Point{ x: 3, y: 4 }
    let d = p.distance()  // Instance method call
    return 0
}
```

Impl blocks also support primitive types:

```vix
import "String.vix"

fn main(): i32 {
    let s = "Hello, World!"
    let len = s.length()  // String method call
    print(len)
    return 0
}
```

### 2. ADT Payload Fix

Fixed a critical bug where ADT constructor payloads (e.g., `Num(i32)`) were not properly typed when accessed outside of match arms. The type checker now correctly resolves payload types for custom ADTs.

```vix
type Expr = Num(i32) | Nil

fn eval(e: Expr): i32 {
    match e {
        Num(n) -> { return n }  // n is correctly typed as i32
        Nil -> { return 0 }
    }
}
```

### 3. Generic Syntax

Vix supports generic types using `<>` angle bracket syntax:

```vix
// Type definitions
type Option<T> = Some(T) | None
type Box<T> = struct { value: T }

// Function definitions
fn id<T>(value: T): T { return value }
fn add<T>(a: T, b: T): T { return a + b }

// Function calls
let x = id<i32>(42)
let y = add<i32>(4, 6)

// Struct literals
let b = Box<i32>{ value: 10 }

// Type references
let arr: Box<i32> = b
```

### 4. String Methods via Impl

The standard library's String functions are now available as methods:

```vix
import "String.vix"

fn main(): i32 {
    let s = "Hello, World!"
    print(s.length())        // 13
    print(s.contains("World"))  // 1
    print(s.to_upper())      // "HELLO, WORLD!"
    return 0
}
```

### 5. Static Linking and Library Paths

Added `-static` and `-L` compiler flags:

```bash
# Static linking
vixc program.vix -o program -static

# Add library search paths
vixc program.vix -o program -L /usr/local/lib -L /opt/lib
```

## Bug Fixes

- Fixed ADT payload type resolution in type checker
- Fixed unifier to handle `Struct` vs `App` type compatibility
- Fixed `check_member` to properly set `inferred_type` for numeric field access
- Fixed match payload binding for custom ADTs with `Struct` type (not just `App` type)

## Examples

- `examples/adt_payload.vix` - ADT with payload example
- `examples/impl_basic.vix` - Impl blocks example
- `examples/string_impl.vix` - String methods example
- `examples/generics.vix` - Generic functions and types
- `examples/generics2.vix` - Generic functions

## Breaking Changes

- Generic syntax now uses `<>` angle brackets (e.g., `id<i32>(42)`, `Box<i32>{ value: 10 }`)

## Known Issues

- Static method calls (e.g., `Type.method()`) may cause compilation hangs

## Contributors

- Daweidie
