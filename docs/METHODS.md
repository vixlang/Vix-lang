# Methods

Vix methods are declared at top level with a receiver. There is no `impl` block syntax.

## Declaration

A method has an implicit first parameter named `self`:

```vix
fn (Point) copy(): Point {
    return self
}

fn (&Point) length(): i32 {
    return self.x + self.y
}

fn (&mut Point) move_by(dx: i32, dy: i32) {
    self.x = self.x + dx
    self.y = self.y + dy
}
```

The declaration does not write `self` in the parameter list. The parser adds it to
the method scope and to the lowered signature.

## Receiver kinds

| Syntax | Kind | Meaning |
| --- | --- | --- |
| `(Point)` | `ReceiverOwned` | the call takes the receiver by value |
| `(&Point)` | `ReceiverShared` | the call borrows the receiver for reading |
| `(&mut Point)` | `ReceiverMut` | the call mutably borrows the receiver |

The kind is the ADT `ReceiverKind` in [src/receiver.vix](../src/receiver.vix). It is
never stored or compared as a bare number.

## Where method logic lives

Method handling is deliberately concentrated. [scripts/check-method-invariants.sh](../scripts/check-method-invariants.sh)
enforces this and fails the build when the old patterns return.

| File | Responsibility |
| --- | --- |
| [src/receiver.vix](../src/receiver.vix) | `ReceiverKind` and its predicates |
| [src/intrinsic.vix](../src/intrinsic.vix) | runtime provided methods (`push`) |
| [src/method_decl.vix](../src/method_decl.vix) | `MethodSignature`, `MethodTable`, `MethodResolution` and the accessors that own the receiver offset |
| [src/method.vix](../src/method.vix) | `method_table_build`, `method_resolve`, `method_qualify`, type normalisation |
| [src/analysis/typed_analysis.vix](../src/analysis/typed_analysis.vix) | ownership rules for receivers |

Rules the guard script checks:

- the array `push` is never matched by name outside the intrinsic registry
- receiver kinds are never compared against bare numbers
- `"Type::method"` is only built by `method_qualify`
- callers never compute a receiver parameter offset themselves

## Resolution

The pipeline attaches one `MethodTable` per compilation unit
(`method_program_attach`), carried in the expression store so every phase sees the
same declaration index. `method_resolve(table, receiver_type, name)` is the single
entry point and returns a `MethodResolution` holding the receiver kind, the receiver
parameter type, the declared parameter and return types, the storage name and, for
intrinsics, which lowering to use.

Resolution matches on the normalised receiver type and the short method name, so two
types may declare methods with the same name:

```vix
fn (&Point) size(): i32 { return self.x }
fn (&Vector) size(): i32 { return self.n }
```

`p.size()` and `v.size()` resolve to different declarations.

`Type::method` remains the storage and symbol name, because it is what
`mangle_flat_name` consumes and changing it would change the ABI. It is produced by
`method_qualify` only, and it is never used as a lookup key.

## Ownership

Enforced today:

- a `&mut` receiver method requires an assignable mutable receiver
  (`error[E5006]` when the receiver is immutable)
- a shared or mutable receiver is borrowed, not consumed
- an owned receiver follows the same rules as an ordinary by-value parameter

Not enforced today: use-after-move for by-value struct arguments. A plain
`fn take(p: P)` has the same behaviour, so owned receiver methods are consistent with
the rest of the language rather than stricter.

## Backends

Every backend consumes the same `MethodResolution`:

- [src/mir/mir.vix](../src/mir/mir.vix) for the LIR/MIR text backends
- [src/codegen.vix](../src/codegen.vix) for the LLVM backend, where intrinsics dispatch
  on `IntrinsicKind` instead of a method name

The method table is lowered alongside the rest of the program by
`analysis_lower_program_for_backend`, so the backends see the same type strings the
AST does.

## Analyzer

The analyzer shares `method_qualify` with the compiler, so declaration names cannot
drift. Method declarations render with their receiver (`fn (&Point) length(): i32`)
and the implicit `self` parameter is not repeated in the argument list.

`symbol_index_find_method_on_receiver` resolves a call by receiver type and method
name. When the analyzer cannot determine the receiver's static type it falls back to
`symbol_index_find_method`, a name-only scan that reports the first match; the analyzer
does not run type inference, so that fallback is documented behaviour rather than a
complete solution.

## Tests

```sh
sh scripts/check-method-invariants.sh          # design invariants
python3 tests/methods_e2e.py                   # compile, link and run through LLVM
python3 editors/vscode/tests/grammar_test.py   # editor grammar for method receivers
```

The end-to-end script emits a Mach-O object with the LLVM backend and links it with the
platform toolchain, because the self/LIR backend refuses non-x86_64 hosts and vixc's
embedded linker only implements ELF and COFF.

## Migration

Replace:

```vix
impl Point {
    fn distance(self: &Point): i32 { ... }
}
```

with:

```vix
fn (&Point) distance(): i32 { ... }
```

For mutable methods use `fn (&mut Point) method(...)`; do not write `mut self: &Point`.
For associated functions that do not take a receiver, declare an ordinary function.
