#!/usr/bin/env python3
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def compile_case(compiler: Path, source: str, extra: list[str]) -> str:
    result = subprocess.run(
        [str(compiler), str(ROOT / source), "--check", "--diag-test", "--color=never", *extra],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    output = result.stdout + result.stderr
    if result.returncode == 0 and "diag_warning" not in source:
        raise AssertionError(f"{source}: expected compilation failure\n{output}")
    return output


def require(output: str, source: str, expected: list[str]) -> None:
    for text in expected:
        if text not in output:
            raise AssertionError(f"{source}: missing {text!r}\n{output}")


def main() -> int:
    compiler = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "vixc"
    if not compiler.is_absolute():
        compiler = ROOT / compiler

    cases = [
        (
            "tests/diag_parse.vix",
            [],
            ["error[E1001]", "note: the parser cannot continue", "help: insert the expected token"],
        ),
        (
            "tests/diag_test.vix",
            [],
            ["error[E3002]", "note: Vix does not implicitly convert", "help: make the value type match"],
        ),
        (
            "tests/diag_warning.vix",
            [],
            ["warning[W4001]", "note: unused bindings often indicate", "help: remove the binding or use it"],
        ),
        (
            "tests/ownership/use_after_move.vix",
            ["--ownership-check"],
            ["error[E5001]", "note: moving a non-copy value", "help: use the value before it is moved"],
        ),
        (
            "tests/mac_hyg_err.vix",
            [],
            ["error[E1106]", "note: captured names and deliberately unhygienic", "help: use `$capture(name)`"],
        ),
        (
            "tests/mac_rec_limit.vix",
            [],
            ["error[E1104]", "tests/mac_rec_limit.vix:8:12", "note: expanded from macro `$loop`", "help: remove the recursive expansion"],
        ),
    ]

    for source, extra, expected in cases:
        output = compile_case(compiler, source, extra)
        require(output, source, expected)
        print(f"ok   {source}")

    json_output = compile_case(compiler, "tests/diag_test.vix", ["--diag-format=json"])
    diagnostic = json.loads(next(line for line in json_output.splitlines() if '"code":"E3002"' in line))
    if not diagnostic["notes"] or not diagnostic["help"]:
        raise AssertionError(f"JSON diagnostic omitted note/help arrays\n{json_output}")
    print("ok   diagnostic JSON note/help")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
