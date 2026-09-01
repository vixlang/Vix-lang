#!/usr/bin/env python3
import argparse
import re
import subprocess
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="verify that LLVM allocas are entry-block static allocations")
    parser.add_argument("--compiler", type=Path, default=Path("build/vixc"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = args.compiler if args.compiler.is_absolute() else root / args.compiler
    source = root / "tests" / "ownership" / "mir_debug.vix"

    with tempfile.TemporaryDirectory(prefix="vix-entry-alloca-") as tmp:
        ir = Path(tmp) / "alloca.ll"
        result = subprocess.run(
            [str(compiler), str(source), "-ll", "-o", str(ir)],
            cwd=root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            print(f"entry-alloca: compiler exited {result.returncode}")
            print(result.stdout, end="")
            print(result.stderr, end="")
            return 1

        block = ""
        violations: list[str] = []
        for number, line in enumerate(ir.read_text().splitlines(), 1):
            label = re.match(r"^([A-Za-z$._][A-Za-z0-9$._-]*):", line)
            if label:
                block = label.group(1)
            if " alloca " in line and block != "entry":
                violations.append(f"{number}:{block}: {line.strip()}")
        if violations:
            print("entry-alloca: dynamic loop/control-flow allocas found")
            print("\n".join(violations))
            return 1

        verify = subprocess.run(
            ["llvm-as", str(ir), "-o", str(Path(tmp) / "alloca.bc")],
            cwd=root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if verify.returncode != 0:
            print(f"entry-alloca: llvm-as exited {verify.returncode}")
            print(verify.stdout, end="")
            print(verify.stderr, end="")
            return 1

    print("entry-alloca: all allocas are in entry blocks; LLVM verifier passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
