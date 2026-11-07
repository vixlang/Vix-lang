# Vix 0.4.1 Release Notes

## Overview

Vix 0.4.1 standardizes the generic syntax by removing the `<>` angle bracket syntax and keeping only the `:[T]` bracket syntax for all generic constructs. This change resolves syntax conflicts between generic types and comparison operators, providing a cleaner and unambiguous grammar.

## Breaking Changes

### Removed `<>` Generic Syntax

The `<>` angle bracket syntax for generics has been completely removed. All generic constructs now use `:[T]` bracket syntax exclusively.

**Before (removed):**
```vix
// Type definitions
type Result<T, E> = Ok(T) | Err(E)
struct Box<T> { value: T }

// Function definitions
fn id<T>(x: T): T { return x }
fn add<T>(a: T, b: T): T { return a + b }

// Receiver methods
fn (Box<T>) with_value(value: T): Box:[T] { return Box:[T]{ value: value } }

// Function calls
let x = id<i32>(42)
let y = add<i32>(4, 6)

// Struct literals
let b = Box<i32>{ value: 10 }

// Type references
let arr: Box<i32> = b
```

**After (required):**
```vix
// Type definitions
type Result:[T, E] = Ok(T) | Err(E)
struct Box:[T] { value: T }

// Function definitions
fn id:[T](x: T): T { return x }
fn add:[T](a: T, b: T): T { return a + b }

// Receiver methods
fn (Box:[T]) with_value(value: T): Box:[T] { return Box:[T]{ value: value } }

// Function calls
let x = id:[i32](42)
let y = add:[i32](4, 6)

// Struct literals
let b = Box:[i32]{ value: 10 }

// Type references
let arr: Box:[i32] = b
```

## Migration Guide

To migrate existing code:

1. Replace all `TypeName<T>` with `TypeName:[T]`
2. Replace all `fn name<T>` with `fn name:[T]`
3. Replace old `impl` blocks with direct receiver declarations such as `fn (&Name) method(...)`
4. Update type references from `Name<T>` to `Name:[T]`
5. Ensure nested generics like `Box<Box<i32>>` become `Box:[Box:[i32]]`

## What Changed

- **Parser**: Removed 16 grammar rules for `<>` angle bracket syntax
- **Examples**: Updated 5 example files to use `:[T]` syntax
- **Standard Library**: Updated 4 std files to use `:[T]` syntax
- **Tests**: Updated 37 regression test files and test_types.py to use `:[T]` syntax
- **Documentation**: Updated release notes

## Compatibility

- The bootstrap compiler (`vixc0/`) already uses `:[T]` syntax
- All existing code using `<>` syntax must be updated to compile
- The `:[T]` syntax is now the only supported generic syntax

## Why This Change?

1. **Eliminates Ambiguity**: The `<>` syntax conflicted with comparison operators (`<`, `>`, `<=`, `>=`), making parsing ambiguous in some contexts
2. **Consistency**: All generic constructs now use the same syntax pattern
3. **Simpler Grammar**: Reduces parser complexity and shift/reduce conflicts
4. **Array Distinction**: Clearly distinguishes between array types `[T]` and generic types `Name:[T]`

## Known Issues

- None reported for this change

## Contributors

- Daweidie
