#!/usr/bin/env python3
import subprocess
import sys
import argparse
from pathlib import Path


NAMED_TESTS = {
    "test1.vix":  {"exit": 42},
    "test2.vix":  {"exit": 21},
    "test3.vix":  {"exit": 7},
    "test4.vix":  {"exit": 9},
    "test5.vix":  {"exit": 7},
    "test6.vix":  {"exit": 15},
    "test7.vix":  {"exit": 10},
    "test8.vix":  {"exit": 52},
    "test9.vix":  {"exit": 7},
    "test10.vix": {"exit": 0, "stdout": "7\n"},
    "test11.vix": {"exit": 7},
    "test12.vix": {"exit": 13},
    "test13.vix": {"exit": 9},
    "test14.vix": {"exit": 9},
    "test15.vix": {"exit": 17, "args": ["run"]},
    "test16.vix": {"exit": 13, "args": ["ok"]},
    "test17.vix": {"exit": 7, "stdout": "7\n"},
    "test18.vix": {"exit": 0, "stdout": "Hello syscall\n"},
    "test19.vix": {"exit": 0, "stdout": "Compat io\n"},
    "test20.vix": {"exit": 0},
    "test21.vix": {"exit": 0},
    "test22.vix": {"exit": 74},
    "test123.vix": {"exit": 42},
    "test124.vix": {"exit": 10},
    "test125.vix": {"exit": 17},
    "test126.vix": {"exit": 26},
    "test127.vix": {"exit": 36},
    "test128.vix": {"exit": 28},
    "test401.vix": {"exit": 15},
    "test402.vix": {"exit": 36},
    "test403.vic": {"exit": 0},
    "test404.vic": {"exit": 7},
    "test405.vic": {"exit": 10},
    "test406.vic": {"exit": 15},
    "test407.vic": {"exit": 36},
    "test408.vic": {"exit": 37},
    "test409.vic": {"exit": 42},
    "test410.vic": {"exit": 22},
    "test411.vic": {"exit": 18},
    "test412.vic": {"exit": 21},
    "test413.vic": {"exit": 28},
    "test414.vic": {"exit": 36},
    "test415.vic": {"exit": 36},
    "test416.vic": {"exit": 9},
    "test417.vic": {"exit": 8},
    "test419.vic": {"compile_fail": ["MIR line", "@main", "undefined local SSA operand %missing"], "only": ["self-lir"]},
    "test420.vic": {"compile_fail": ["MIR line", "@main", "unknown branch target %missing"], "only": ["self-lir"]},
    "test421.vic": {"exit": 14},
    "test422.vic": {"exit": 42},
    "test423.vic": {"exit": 3},
    "test424.vic": {"compile_fail": ["MIR line", "@main", "invalid load type mystery"], "only": ["self-lir"]},
}

TESTS = dict(NAMED_TESTS)
for i in range(100):
    a = (i * 13 + 5) % 97
    b = (i * 17 + 9) % 89
    c = (i * 19 + 3) % 83
    left = (i + 7 + a) * 3
    right = (i % 23 + 4 + b) * 2
    TESTS[f"test{i + 23}.vix"] = {"exit": (left - right + c) % 251}


def large_macro_expected(idx: int) -> int:
    mul = 2 + idx % 5
    add = 3 + idx % 11
    bias = 1 + idx % 7
    total = 0
    for j in range(92):
        x = j + 1 + idx % 9
        if j % 4 == 0:
            total += mul * x + add
        elif j % 4 == 1:
            total += mul * (x + x) + add
        elif j % 4 == 2:
            total += (mul * x + add) + (mul * x + add)
        else:
            total += mul * (x + bias) + add
    return total % 251


for i in range(129, 401):
    TESTS[f"test{i}.vix"] = {"exit": large_macro_expected(i)}


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


def run_test(root: Path, files_dir: Path, bin_dir: Path, name: str, spec: dict, backend: str, compiler: Path) -> bool:
    source = files_dir / name
    output = bin_dir / name.removesuffix(".vix")

    cmd = [str(compiler), str(source), "-o", str(output)]
    if backend == "self":
        cmd.append("--backend=self")
    elif backend == "self-lir":
        cmd.append("--backend=self-lir")

    compile_result = run_cmd(cmd, root)

    expected_failure = spec.get("compile_fail")
    if expected_failure is not None:
        if compile_result.returncode == 0:
            return fail(name, "expected compilation to fail", compile_result)
        diagnostics = compile_result.stdout + compile_result.stderr
        for text in expected_failure:
            if text not in diagnostics:
                return fail(name, f"expected compiler diagnostic containing {text!r}", compile_result)
        print(f"ok   {name} compile-fail")
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

    expected_stdout = spec.get("stdout", "")
    if run_result.stdout != expected_stdout:
        return fail(
            name,
            f"expected stdout {expected_stdout!r}, got {run_result.stdout!r}",
            run_result,
        )

    print(f"ok   {name} exit={expected_exit}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Vix compiler tests")
    parser.add_argument("--llvm", action="store_true", help="use LLVM backend")
    parser.add_argument("--self", action="store_true", help="use self backend")
    parser.add_argument("--self-lir", action="store_true", help="use self-lir backend")
    parser.add_argument("--compiler", type=Path, help="compiler executable (default: build/vixc)")
    args = parser.parse_args()

    selected = sum((args.llvm, args.self, args.self_lir))
    if selected > 1:
        print("error: choose only one backend")
        return 1

    if args.self_lir:
        backend = "self-lir"
    elif args.self:
        backend = "self"
    else:
        backend = "llvm"
    backend_name = backend

    tests_dir = Path(__file__).resolve().parent
    root = repo_root()
    compiler = args.compiler or (root / "build" / "vixc")
    if not compiler.is_absolute():
        compiler = root / compiler
    if not compiler.exists():
        print(f"error: compiler not found: {compiler}")
        return 1
    files_dir = tests_dir / "files"
    bin_dir = tests_dir / "bin"

    if not files_dir.exists():
        print("error: files/ directory not found")
        return 1

    bin_dir.mkdir(parents=True, exist_ok=True)

    found = {path.name for pattern in ("test*.vix", "test*.vic") for path in files_dir.glob(pattern)}
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
    for name in sorted(TESTS):
        spec = TESTS[name]
        if backend not in spec.get("only", ["llvm", "self", "self-lir"]):
            continue
        if not run_test(root, files_dir, bin_dir, name, spec, backend, compiler):
            return 1
        passed += 1

    print(f"passed {passed} {backend_name} tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
