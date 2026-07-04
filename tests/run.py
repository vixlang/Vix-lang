#!/usr/bin/env python3
import subprocess
import sys
import tempfile
from pathlib import Path


TESTS = {
    "test_add.vix": {"exit": 42},
    "test_fib.vix": {"exit": 21},
    "test_ack.vix": {"exit": 7},
    "test_array.vix": {"exit": 9},
    "test_array_ref.vix": {"exit": 7},
    "test_array_store.vix": {"exit": 15},
    "test_array_sum.vix": {"exit": 10},
    "test_array_empty_push.vix": {"exit": 52},
    "test_if_expr.vix": {"exit": 7},
    "test_print_stmt.vix": {"exit": 0, "stdout": "7\n"},
    "test_block_expr.vix": {"exit": 7},
    "test_struct_fields.vix": {"exit": 13},
    "test_string_escape.vix": {"exit": 9},
    "test_string_length.vix": {"exit": 9},
    "test_string_match.vix": {"exit": 17, "args": ["run"]},
    "test_argv_ptr.vix": {"exit": 13, "args": ["ok"]},
    "test_extern_printf.vix": {"exit": 7, "stdout": "7\n"},
    "test_syscall_io.vix": {"exit": 0, "stdout": "Hello syscall\n"},
    "test_std_io_compat.vix": {"exit": 0, "stdout": "Compat io\n"},
    "test_unsupported_float.vix": {"compile_fails": True},
}

for i in range(100):
    a = (i * 13 + 5) % 97
    b = (i * 17 + 9) % 89
    c = (i * 19 + 3) % 83
    left = (i + 7 + a) * 3
    right = (i % 23 + 4 + b) * 2
    TESTS[f"test_regress_{i:03}.vix"] = {"exit": (left - right + c) % 251}


def repo_root() -> Path:
    path = Path(__file__).resolve()
    for parent in path.parents:
        if (parent / "build" / "vixc").exists() and (parent / "src").is_dir():
            return parent
    raise RuntimeError("cannot find repo root with build/vixc")


def run_cmd(cmd, cwd: Path):
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def fail(name: str, message: str, result=None):
    print(f"FAIL {name}: {message}")
    if result is not None:
        if result.stdout:
            print("--- stdout ---")
            print(result.stdout, end="")
        if result.stderr:
            print("--- stderr ---")
            print(result.stderr, end="")
    return False


def run_test(root: Path, tests_dir: Path, tmp_dir: Path, name: str, spec: dict) -> bool:
    compiler = root / "build" / "vixc"
    source = tests_dir / name
    output = tmp_dir / name.removesuffix(".vix")

    compile_result = run_cmd(
        [str(compiler), str(source), "--backend=self", "-o", str(output)],
        root,
    )

    if spec.get("compile_fails"):
        if compile_result.returncode == 0:
            return fail(name, "expected compilation to fail")
        print(f"ok   {name} compile failed as expected")
        return True

    if compile_result.returncode != 0:
        return fail(name, f"compile exited {compile_result.returncode}", compile_result)

    run_result = run_cmd([str(output), *spec.get("args", [])], root)
    expected_exit = spec["exit"]
    if run_result.returncode != expected_exit:
        return fail(
            name,
            f"expected exit {expected_exit}, got {run_result.returncode}",
            run_result,
        )

    expected_stdout = spec.get("stdout")
    if expected_stdout is not None and run_result.stdout != expected_stdout:
        return fail(
            name,
            f"expected stdout {expected_stdout!r}, got {run_result.stdout!r}",
            run_result,
        )

    print(f"ok   {name} exit={expected_exit}")
    return True


def main() -> int:
    tests_dir = Path(__file__).resolve().parent
    root = repo_root()

    found = {path.name for path in tests_dir.glob("test_*.vix")}
    expected = set(TESTS)
    missing = sorted(expected - found)
    unlisted = sorted(found - expected)
    if missing or unlisted:
        if missing:
            print("missing test files:")
            for name in missing:
                print(f"  {name}")
        if unlisted:
            print("unlisted test files:")
            for name in unlisted:
                print(f"  {name}")
        return 1

    passed = 0
    with tempfile.TemporaryDirectory(prefix="vix-mir2asm-tests-") as tmp:
        tmp_dir = Path(tmp)
        for name in sorted(TESTS):
            if not run_test(root, tests_dir, tmp_dir, name, TESTS[name]):
                return 1
            passed += 1

    print(f"passed {passed} mir2asm tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
