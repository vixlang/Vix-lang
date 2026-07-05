#!/usr/bin/env python3
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def expected(case: int) -> int:
    seed = (case * 37 + 11) % 251
    mul = 2 + case % 7
    add = 3 + case % 19
    rounds = 4 + case % 9
    acc = seed
    for i in range(rounds):
        probe = (i + acc % 11 + case % 5) % 251
        step = (probe * mul + add) % 251
        if step > 120:
            acc = (acc + step - (case % 13)) % 251
        else:
            acc = (acc + step + (case % 17)) % 251
    return acc % 251


def source(case: int) -> str:
    seed = (case * 37 + 11) % 251
    mul = 2 + case % 7
    add = 3 + case % 19
    rounds = 4 + case % 9
    dec = case % 13
    inc = case % 17
    skew = case % 5
    return f"""fn step_{case}(x: i32): i32
{{
    return (x * {mul} + {add}) % 251
}}

fn main(): i32
{{
    let mut acc = {seed}
    for (i in 0 .. {rounds})
    {{
        let probe = (i + acc % 11 + {skew}) % 251
        let value = step_{case}(probe)
        if (value > 120)
        {{
            acc = (acc + value - {dec}) % 251
        }}
        else
        {{
            acc = (acc + value + {inc}) % 251
        }}
    }}
    if (acc < 0) {{ return acc + 251 }}
    return acc
}}
"""


def run_cmd(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="deterministic Vix self-backend fuzz tests")
    parser.add_argument("--cases", type=int, default=600)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    compiler = root / "build" / "vixc"
    with tempfile.TemporaryDirectory(prefix="vix-fuzz-", dir=root / "tests") as tmp_name:
        tmp = Path(tmp_name)
        for case in range(args.cases):
            src = tmp / f"fuzz_{case}.vix"
            out = tmp / f"fuzz_{case}"
            src.write_text(source(case))
            compile_result = run_cmd([str(compiler), str(src), "--backend=self", "-o", str(out)], root)
            if compile_result.returncode != 0:
                print(f"FAIL fuzz {case}: compile exited {compile_result.returncode}")
                print(compile_result.stderr, end="")
                if args.keep:
                    print(f"kept {tmp}")
                    return 1
                return 1
            run_result = run_cmd([str(out)], root)
            want = expected(case)
            if run_result.returncode != want:
                print(f"FAIL fuzz {case}: expected exit {want}, got {run_result.returncode}")
                print(run_result.stdout, end="")
                print(run_result.stderr, end="")
                if args.keep:
                    print(f"kept {tmp}")
                    return 1
                return 1
        if args.keep:
            print(f"kept {tmp}")
    print(f"passed {args.cases} self fuzz tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
