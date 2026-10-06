# Multi-File Module System Tests

## Test Structure

### Basic Test (tests/module/)
- `main.vix`: Entry point that imports math module
- `math.vix`: Simple arithmetic functions

### Usage

```bash
# View module dependency graph
build/vixc tests/module/main.vix --module-graph

# Compile
build/vixc tests/module/main.vix -obj -o output.o
clang++ output.o runtime/runtime.o -o output
./output  # Should return exit code 17
```

## Expected Output

The test computes: `(3 + 4) + (2 * 5) = 7 + 10 = 17`

## End-to-end coverage

`tests/modules_e2e.py` compiles, links and runs every multi-module program in
the repository on the LLVM backend and checks the exit code:

```bash
python3 tests/modules_e2e.py
```

## Notes

- The LLVM backend handles multi-file compilation; the earlier report of a
  segfault here is obsolete (all multi-module programs build and run).
- `vixc -o` cannot link on macOS: the embedded linker only implements the ELF
  and COFF drivers. Use `-obj` and link with `clang++`, or use
  `sh scripts/run-vix.sh <file>`.
- The `self` backends are x86_64 only and refuse to run on arm64.
