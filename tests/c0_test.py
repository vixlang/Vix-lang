#!/usr/bin/env python3
import platform
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CASES = ROOT / "tests" / "vixc0"
VIXC0 = ROOT / "vixc0" / "vixc0"
if platform.system() == "Windows" and not str(VIXC0).endswith(".exe"):
    VIXC0 = Path(str(VIXC0) + ".exe")
OUT = ROOT / "vixc0" / "test.ll"


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=20)


def main():
    if not VIXC0.exists():
        print(f"missing compiler: {VIXC0}", file=sys.stderr)
        return 1

    failures = []
    cases = sorted(CASES.glob("*.vix"))
    if not cases:
        print(f"no vixc0 tests found in {CASES}", file=sys.stderr)
        return 1

    for src in cases:
        expected_path = src.with_suffix(".expected")
        if not expected_path.exists():
            failures.append(f"{src.name}: missing {expected_path.name}")
            continue

        compile_res = run([str(VIXC0), str(src)])
        if compile_res.returncode != 0:
            failures.append(f"{src.name}: compile failed\n{compile_res.stderr}{compile_res.stdout}")
            continue

        OUT.write_text(compile_res.stdout)

        verify_res = run(["opt", "-passes=verify", "-disable-output", str(OUT)])
        if verify_res.returncode != 0:
            failures.append(f"{src.name}: LLVM verify failed\n{verify_res.stderr}")
            continue

        run_res = run(["lli", str(OUT)])
        expected = expected_path.read_text()
        if run_res.stdout != expected:
            failures.append(
                f"{src.name}: output mismatch\n"
                f"expected:\n{expected!r}\n"
                f"actual:\n{run_res.stdout!r}\n"
                f"stderr:\n{run_res.stderr}"
            )
            continue

        print(f"PASS {src.name}")

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
