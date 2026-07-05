#!/usr/bin/env python3
import subprocess
import sys
import tempfile
from pathlib import Path


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


def run_cmd(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def run_dedicated_macro_tests(root: Path, macro_files: list[Path]) -> int:
    compiler = root / "build" / "vixc"
    with tempfile.TemporaryDirectory(prefix="vix-macro-smoke-", dir=root / "tests") as tmp_name:
        tmp = Path(tmp_name)
        for path in macro_files:
            idx = int(path.stem.removeprefix("macro_test"))
            out = tmp / path.stem
            compile_result = run_cmd([str(compiler), str(path), "--backend=self", "-o", str(out)], root)
            if compile_result.returncode != 0:
                print(f"FAIL {path.name}: compile exited {compile_result.returncode}")
                print(compile_result.stderr, end="")
                return 1
            run_result = run_cmd([str(out)], root)
            want = large_macro_expected(idx)
            if run_result.returncode != want:
                print(f"FAIL {path.name}: expected exit {want}, got {run_result.returncode}")
                print(run_result.stdout, end="")
                print(run_result.stderr, end="")
                return 1
    print(f"passed {len(macro_files)} dedicated large macro smoke tests")
    return 0


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    files = sorted((root / "tests" / "files").glob("test*.vix"))
    if len(files) != 400:
        print(f"error: expected 400 smoke files, found {len(files)}")
        return 1

    short = [path for path in files if int(path.stem.removeprefix("test")) >= 129 and len(path.read_text().splitlines()) < 300]
    if short:
        print("error: large macro smoke tests below 300 lines:")
        for path in short[:10]:
            print(f"  {path}")
        return 1

    macro_files = sorted((root / "tests" / "files").glob("macro_test*.vix"))
    if len(macro_files) != 400:
        print(f"error: expected 400 dedicated large macro smoke files, found {len(macro_files)}")
        return 1
    short_macro = [path for path in macro_files if len(path.read_text().splitlines()) < 300]
    if short_macro:
        print("error: dedicated macro smoke tests below 300 lines:")
        for path in short_macro[:10]:
            print(f"  {path}")
        return 1

    numeric_result = subprocess.run([sys.executable, str(root / "tests" / "run.py"), "--self"], cwd=root).returncode
    if numeric_result != 0:
        return numeric_result
    return run_dedicated_macro_tests(root, macro_files)


if __name__ == "__main__":
    raise SystemExit(main())
