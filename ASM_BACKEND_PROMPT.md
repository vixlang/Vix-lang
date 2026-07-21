# Task: Fix Vix-lang Self (ASM) Backend to Enable Self-Bootstrapping

## Project Overview

Vix-lang is a self-hosted compiler written in its own language (`.vix`). It has two code generation backends:

1. **LLVM backend** (default, `--backend=llvm`): Production-ready, self-hosting works
2. **Self backend** (`--backend=self`): Generates NASM x86_64 assembly, currently incomplete

The self backend pipeline is:
```
.vix source → parser → AST → MIR text (SSA IR) → mir2asm → NASM x86_64 assembly → nasm → .o → built-in LLD → executable
```

The goal is to make the self backend stable enough to compile the compiler itself (`make self-stage`).

## Current State

### What Works
- Basic integer arithmetic: `add`, `sub`, `mul`, `sdiv`, `srem`, `neg`
- Float arithmetic: `fadd`, `fsub`, `fmul`, `fdiv`
- Bitwise: `and`, `or`, `xor`, `shl`, `ashr`
- Comparisons: `icmp` (all predicates), `fcmp` (oeq/one/olt/ole/ogt/oge)
- Casts: `zext`, `trunc` (i64→i32 only), `sitofp` (i32→f64 only), `fpext` (f32→f64)
- Control flow: `br label`, `br_cond`, `ret`
- Memory: `alloca`, `load`, `store`, `gep`
- Calls: direct calls with System V AMD64 ABI, variadic support, struct return via sret
- Structs: `insertvalue`, `extractvalue`, const structs
- String literals, constants

### What's Missing (Critical for Self-Bootstrapping)

These MIR instructions from `src/mir/spec.vic` are NOT implemented and will hit the `mir2asm_emit_unsupported` fallback:

| Instruction | Why Needed | x86_64 Implementation |
|-------------|-----------|----------------------|
| `fneg T %a` | Float negation | `xorps xmm0, [rel .sign_bit]` or `movss/xorps` |
| `frem T %a, %b` | Float modulo | `fprem` or call `fmod` |
| `udiv T %a, %b` | Unsigned division | `xor edx,edx` + `div` (not `idiv`) |
| `urem T %a, %b` | Unsigned remainder | `xor edx,edx` + `div` → remainder in `edx` |
| `sext T %a to T` | Sign extension | `movsx`/`movsxd` |
| `lshr T %a, %b` | Logical shift right | `shr` (not `sar`) |
| `fptrunc T %a to T` | f64→f32 | `cvtsd2ss` |
| `fptoui T %a to T` | Float→unsigned int | `cvtss2sd` + `cvttsd2si` + fix sign |
| `fptosi T %a to T` | Float→signed int | `cvttsd2si`/`cvtss2si` |
| `uitofp T %a to T` | Unsigned→float | `mov eax` + `cvtsi2sd` (32-bit) or `mov eax` + fix high 32 bits |
| `ptrtoint T %a to T` | Pointer→integer | `mov rax, [ptr]` (trivial on x86_64) |
| `inttoptr T %a to T` | Integer→pointer | `mov rax, [int]` (trivial on x86_64) |
| `bitcast T %a to T` | Type punning | No-op (data is in memory/register) |

### Incomplete Handlers

1. **`sitofp`** (codegen.vix:682-688): Only handles i32→f64 (`cvtsi2sd xmm0, eax`). Missing:
   - i64→f64: needs `cvtsi2sd xmm0, rax`
   - i32→f32: needs `cvtsi2ss`
   - i64→f32: needs `cvtsi2ss xmm0, rax`

2. **`trunc`**: Only handles i64→i32. Should also handle i64→i8, i32→i8 etc.

## Architecture Notes

### Key Files
- `src/mir/mir2asm/x86_64/codegen.vix` — Main instruction emission (1245 lines)
  - `mir2asm_emit_instruction()` at line 970: main dispatch
  - `mir2asm_emit_cast()` at line 653: cast handler
  - `mir2asm_emit_binop()` at line ~490: integer binary ops
  - `mir2asm_emit_fbinop()` at line ~530: float binary ops
  - `mir2asm_emit_unary()` at line ~510: unary ops
- `src/mir/mir2asm/x86_64/mod.vix` — Two-pass planner+emitter orchestration
- `src/mir/mir2asm/x86_64/abi.vix` — System V AMD64 ABI
- `src/mir/mir2asm/x86_64/instructions.vix` — Helper functions for instruction emission
- `src/mir/mir2asm/types.vix` — Type helpers
- `src/mir/mir2asm/registers.vix` — Register definitions

### Two-Pass Design
The backend uses a two-pass approach in `mir2asm_emit_direct_machine_function` (mod.vix):
1. Pass 1 (`discard_output=1`): computes stack layout, slot offsets
2. Pass 2: emits actual assembly using the planned layout

**Critical**: Both passes must handle the same instructions identically. If the planner misses an instruction that the emitter handles (or vice versa), stack offsets will be wrong → silent corruption.

### Code Conventions
- All code is Vix (`.vix` files)
- String comparison: use `mir2asm_starts_with(line, "prefix")`
- String slicing: `mir2asm_slice(str, start, end)`, `mir2asm_after(str, pos)`
- String trim: `mir2asm_trim(str)`
- Output: `mir2asm_line(b, "assembly text")`
- Slot allocation: `mir2asm_alloc_slot_typed(b, name, is_alloca, type)`
- Operand loading: `mir2asm_operand_typed(b, val, ty)`, `mir2asm_emit_load_int_reg(b, reg, val, ty)`
- Name lookup: `mir2asm_find_name(b, name)` returns index or -1
- Type checks: `mir2asm_type_is_float(ty)`, `mir2asm_type_is_struct_value(b, ty)`

### Testing
```bash
# Build the LLVM backend compiler first
make all

# Try self-bootstrapping (this is what needs to work)
make self-stage

# The self-stage target does:
# 1. build/vixc --backend=self src/main.vix -obj -o build/vixc-self.o
# 2. clang++ -fuse-ld=lld build/vixc-self.o [support objs] -o build/vixc-self
```

## Task: Phase 1 — Minimum Required for Self-Bootstrapping

### Step 1: Add Missing Instruction Handlers

Add these to `mir2asm_emit_instruction()` in `src/mir/mir2asm/x86_64/codegen.vix`:

1. **`fneg`**: Add handler similar to `neg` but for floats. Load into xmm0, XOR with sign bit mask, store back.

2. **`frem`**: Add handler. Either use x87 `fprem` or call `fmod`/`fmodf` extern.

3. **`udiv`**: Add handler similar to `sdiv` but use unsigned `div` instruction (zero-extend via `xor edx,edx` before `div`).

4. **`urem`**: Add handler similar to `srem` but unsigned. After `div`, remainder is in `edx`.

5. **`lshr`**: Add to the binop dispatch, mapping to `shr` instruction.

6. **`sext`**: Add cast handler. Use `movsx` for i8→i32, `movsxd` for i32→i64.

7. **`fptrunc`**: Add cast handler. Use `cvtsd2ss` for f64→f32.

8. **`fptoui`**: Add cast handler. Use `cvttsd2si` then fix unsigned with `mov eax` + `cvtsi2sd` if needed.

9. **`fptosi`**: Add cast handler. Use `cvttsd2si`/`cvtss2si`.

10. **`uitofp`**: Add cast handler. For i32→f64: `mov eax, src` + `cvtsi2sd xmm0, eax`.

11. **`ptrtoint`**: Add cast handler. On x86_64, pointer is 64-bit. If target is i64, it's a no-op mov. If i32, truncate.

12. **`inttoptr`**: Add cast handler. On x86_64, integer is 64-bit. If source is i32, zero-extend. If i64, no-op mov.

13. **`bitcast`**: Add cast handler. On x86_64, bitcast is a no-op (same size, same register).

### Step 2: Fix Incomplete `sitofp` Handler

Update `mir2asm_emit_cast()` at line 682 to handle all size combinations:
- i32→f64: `cvtsi2sd xmm0, eax` (existing)
- i64→f64: `cvtsi2sd xmm0, rax`
- i32→f32: `cvtsi2ss xmm0, eax`
- i64→f32: `cvtsi2ss xmm0, rax`

### Step 3: Update Cast Dispatch

Update the cast dispatch at line 1205 to include all new cast types:
```vix
if (mir2asm_starts_with(rhs, "zext ") == 1 or mir2asm_starts_with(rhs, "trunc ") == 1 or mir2asm_starts_with(rhs, "sitofp ") == 1 or mir2asm_starts_with(rhs, "fpext ") == 1 or mir2asm_starts_with(rhs, "sext ") == 1 or mir2asm_starts_with(rhs, "fptrunc ") == 1 or mir2asm_starts_with(rhs, "fptoui ") == 1 or mir2asm_starts_with(rhs, "fptosi ") == 1 or mir2asm_starts_with(rhs, "uitofp ") == 1 or mir2asm_starts_with(rhs, "ptrtoint ") == 1 or mir2asm_starts_with(rhs, "inttoptr ") == 1 or mir2asm_starts_with(rhs, "bitcast ") == 1)
```

### Step 4: Add `fneg` and `frem` Dispatch

Add to the instruction dispatch after the existing `fdiv` handler:
```vix
if (mir2asm_starts_with(rhs, "fneg ") == 1) { mir2asm_emit_fneg(b, dest, rhs); return }
if (mir2asm_starts_with(rhs, "frem ") == 1) { mir2asm_emit_frem(b, dest, rhs); return }
```

### Step 5: Add `udiv`, `urem`, `lshr` Dispatch

Add to the instruction dispatch after existing handlers:
```vix
if (mir2asm_starts_with(rhs, "udiv ") == 1) { mir2asm_emit_binop(b, dest, rhs, "udiv", "div"); return }
if (mir2asm_starts_with(rhs, "urem ") == 1) { mir2asm_emit_binop(b, dest, rhs, "urem", "div"); return }
if (mir2asm_starts_with(rhs, "lshr ") == 1) { mir2asm_emit_binop(b, dest, rhs, "lshr", "shr"); return }
```

**Important**: `udiv`/`urem` need special handling in `mir2asm_emit_binop` because they use `div` which requires dividend in `edx:eax` (zero-extended), not `idiv` (sign-extended). You may need to add a special case in `mir2asm_emit_binop` for these.

### Step 6: Test

Run `make self-stage` and iterate. The compiler source (`src/main.vix` and all files it imports) will exercise these instructions. Fix any additional issues that surface.

## Task: Phase 2 — Feature Parity (Future)

After Phase 1, implement:
- `switch` statement support
- `phi` node support
- `call_indirect` (function pointers)
- `select` (conditional select)
- More `trunc`/`zext` size combinations
- `unreachable` instruction
- Global variables

## Verification

After each change:
1. `make clean && make all` — ensure LLVM backend still works
2. `make self-stage` — the key test; this compiles the compiler with itself using `--backend=self`
3. If `vixc-self` is produced, it should be able to compile simple test programs
4. Run `./build/vixc-self src/main.vix --backend=self -obj -o /tmp/test.o` to verify the self-compiled compiler works

## Important Notes

- **DO NOT modify the LLVM backend** (`src/codegen.vix`, `src/backend/`)
- **DO NOT modify the MIR generator** (`src/mir/gen/`)
- Only modify files under `src/mir/mir2asm/` and `src/mir/lir2asm/`
- The two-pass design means you must ensure the planner (pass 1) handles new instructions the same way as the emitter (pass 2)
- Test incrementally — add one instruction, test, then add the next
