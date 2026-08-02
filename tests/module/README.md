# Multi-File Module System Tests

## Test Structure

### Basic Test (tests/module/)
- `main.vix`: Entry point that imports math module
- `math.vix`: Simple arithmetic functions

### Usage

```bash
# View module dependency graph
build/vixc tests/module/main.vix --module-graph

# Compile with self backend (recommended for multi-file)
build/vixc tests/module/main.vix --backend=self -o output

# Run
./output  # Should return exit code 17
```

## Expected Output

The test computes: `(3 + 4) + (2 * 5) = 7 + 10 = 17`

## Known Issues

- LLVM backend crashes with multi-file compilation (segfault)
- Use `--backend=self` or `--backend=self-lir` as workaround
- Self backend fully supports multi-file compilation
