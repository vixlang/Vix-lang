#!/usr/bin/env python3
import argparse
import subprocess
import tempfile
from pathlib import Path


FUNCS = 64


def apply_func(case: int, idx: int, x: int) -> int:
    add = (case * 3 + idx * 5) % 97
    mul = 2 + (case + idx) % 5
    value = (x + add) % 251
    if value % 2 == 0:
        return (value * mul + idx) % 251
    return (value + mul + case % 11) % 251


def expected(case: int) -> int:
    acc = (case * 29 + 7) % 251
    for idx in range(FUNCS):
        acc = apply_func(case, idx, acc)
    return acc % 251


def source(case: int) -> str:
    lines: list[str] = []
    for idx in range(FUNCS):
        add = (case * 3 + idx * 5) % 97
        mul = 2 + (case + idx) % 5
        extra = case % 11
        lines.extend(
            [
                f"fn stress_{case}_{idx}(x: i32): i32",
                "{",
                f"    let value = (x + {add}) % 251",
                "    if (value % 2 == 0)",
                "    {",
                f"        return (value * {mul} + {idx}) % 251",
                "    }",
                f"    return (value + {mul} + {extra}) % 251",
                "}",
                "",
            ]
        )
    lines.extend(
        [
            "fn main(): i32",
            "{",
            f"    let mut acc = {(case * 29 + 7) % 251}",
        ]
    )
    for idx in range(FUNCS):
        lines.append(f"    acc = stress_{case}_{idx}(acc)")
    lines.extend(["    return acc", "}"])
    return "\n".join(lines) + "\n"


def run_cmd(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="deterministic Vix self-backend stress tests")
    parser.add_argument("--cases", type=int, default=100)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    compiler = root / "build" / "vixc"
    with tempfile.TemporaryDirectory(prefix="vix-stress-", dir=root / "tests") as tmp_name:
        tmp = Path(tmp_name)
        for case in range(args.cases):
            src = tmp / f"stress_{case}.vix"
            out = tmp / f"stress_{case}"
            src.write_text(source(case))
            compile_result = run_cmd([str(compiler), str(src), "--backend=self", "-o", str(out)], root)
            if compile_result.returncode != 0:
                print(f"FAIL stress {case}: compile exited {compile_result.returncode}")
                print(compile_result.stderr, end="")
                return 1
            run_result = run_cmd([str(out)], root)
            want = expected(case)
            if run_result.returncode != want:
                print(f"FAIL stress {case}: expected exit {want}, got {run_result.returncode}")
                print(run_result.stdout, end="")
                print(run_result.stderr, end="")
                return 1
    print(f"passed {args.cases} self stress tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
