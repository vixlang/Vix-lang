#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
from pathlib import Path

CASES = [
    {"name": "gen_opt", "source": "gen_opt.vix", "exit": 0},
    {"name": "gen_adt", "source": "gen_adt.vix", "exit": 0},
    {"name": "gen_struct", "source": "gen_struct.vix", "exit": 0},
    {"name": "gen_nested", "source": "gen_nested.vix", "exit": 0},
    {"name": "gen_multi", "source": "gen_multi.vix", "exit": 0},
    {"name": "adt_match", "source": "adt_match.vix", "exit": 0},
    {"name": "adt_nested", "source": "adt_nested.vix", "exit": 0},
    {"name": "adt_string", "source": "adt_string.vix", "exit": 0},
    {"name": "adt_loop", "source": "adt_loop.vix", "exit": 0},
    {"name": "adt_lambda", "source": "adt_lambda.vix", "exit": 0},
    {"name": "lam_basic", "source": "lam_basic.vix", "exit": 0},
    {"name": "lam_capture", "source": "lam_capture.vix", "exit": 0},
    {"name": "lam_nested", "source": "lam_nested.vix", "exit": 0},
    {"name": "lam_return", "source": "lam_return.vix", "exit": 0},
    {"name": "lam_generic", "source": "lam_generic.vix", "exit": 0},
    {"name": "mac_basic", "source": "mac_basic.vix", "exit": 0},
    {"name": "mac_expr", "source": "mac_expr.vix", "exit": 0},
    {"name": "mac_nested", "source": "mac_nested.vix", "exit": 0},
    {"name": "mac_hyg", "source": "mac_hyg.vix", "exit": 0},
    {"name": "mac_repeat", "source": "mac_repeat.vix", "exit": 0},
    {"name": "pipe_basic", "source": "pipe_basic.vix", "exit": 0},
    {"name": "pipe_gen", "source": "pipe_gen.vix", "exit": 0},
    {"name": "pipe_opt", "source": "pipe_opt.vix", "exit": 0},
    {"name": "pipe_lam", "source": "pipe_lam.vix", "exit": 0},
    {"name": "pipe_nested", "source": "pipe_nested.vix", "exit": 0},
    {"name": "own_copy", "source": "own_copy.vix", "ownership": True},
    {"name": "own_borrow", "source": "own_borrow.vix", "ownership": True},
    {"name": "own_branch", "source": "own_branch.vix", "ownership": True},
    {"name": "own_loop", "source": "own_loop.vix", "ownership": True},
    {"name": "life_return", "source": "life_return.vix", "ownership": True},
    {"name": "syn_nested", "source": "syn_nested.vix", "check": True},
    {"name": "syn_precedence", "source": "syn_precedence.vix", "check": True},
    {"name": "syn_array", "source": "syn_array.vix", "check": True},
    {"name": "syn_nullish", "source": "syn_nullish.vix", "check": True},
    {"name": "syn_if_match", "source": "syn_if_match.vix", "check": True},
    {"name": "diag_generic", "source": "diag_generic.vix", "fail": "error[E3002]"},
    {"name": "diag_match", "source": "diag_match.vix", "fail": "error"},
    {"name": "diag_lambda", "source": "diag_lambda.vix", "fail": "explicitly captured"},
    {"name": "diag_macro", "source": "diag_macro.vix", "fail": "error"},
    {"name": "diag_life", "source": "diag_life.vix", "fail": "error"},
    {"name": "llvm_adt", "source": "llvm_adt.vix", "exit": 0},
    {"name": "llvm_gen", "source": "llvm_gen.vix", "exit": 0},
    {"name": "llvm_load", "source": "llvm_load.vix", "exit": 0},
    {"name": "llvm_lambda", "source": "llvm_lambda.vix", "exit": 0},
    {"name": "llvm_pipe", "source": "llvm_pipe.vix", "exit": 0},
    {"name": "self_adt", "source": "self_adt.vix", "exit": 0, "only": "self"},
    {"name": "self_gen", "source": "self_gen.vix", "exit": 0, "only": "self"},
    {"name": "ast_nested", "source": "ast_nested.vix", "ast": "Program"},
    {"name": "ast_lambda", "source": "ast_lambda.vix", "ast": "LambdaExpression"},
    {"name": "check_generic", "source": "gen_opt.vix", "check": True},
    {"name": "check_macro", "source": "mac_nested.vix", "check": True},
    {"name": "mod_basic", "source": "mod_basic.vix", "exit": 0},
]

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, default=Path("build/vixc"))
    parser.add_argument("--backend", choices=("llvm", "self", "self-lir"), default="llvm")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = args.compiler if args.compiler.is_absolute() else root / args.compiler
    source_dir = root / "tests" / "complex"
    failures = 0
    for case in CASES:
        if case.get("only") and case["only"] != args.backend:
            continue
        source = source_dir / case["source"]
        cmd = [str(compiler), str(source)]
        if case.get("check"):
            cmd.append("--check")
        elif case.get("ownership"):
            cmd.append("--ownership-check")
        elif "ast" in case:
            cmd.append("--ast-json")
        else:
            output = root / "tests" / "bin" / (case["name"] + "-complex")
            cmd += ["-o", str(output)]
            if args.backend != "llvm":
                cmd.append("--backend=" + args.backend)
        try:
            result = subprocess.run(cmd, cwd=root, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=args.timeout)
        except subprocess.TimeoutExpired as exc:
            print(f"FAIL {case['name']} timeout={args.timeout}s stdout={exc.stdout!r} stderr={exc.stderr!r}")
            failures += 1
            continue
        text = result.stdout + result.stderr
        if "fail" in case:
            ok = result.returncode != 0 and case["fail"] in text
        elif "ast" in case:
            try:
                ast = json.loads(result.stdout)
                ok = result.returncode == 0 and case["ast"] in result.stdout and ast.get("type") == "Program"
            except json.JSONDecodeError:
                ok = False
        elif case.get("check") or case.get("ownership"):
            ok = result.returncode == 0
        else:
            ok = result.returncode == 0
            if ok:
                try:
                    run = subprocess.run([str(output)], cwd=root, text=True,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         timeout=args.timeout)
                    ok = run.returncode == case["exit"]
                    if not ok:
                        text += f"\nrun exit={run.returncode} stdout={run.stdout} stderr={run.stderr}"
                except subprocess.TimeoutExpired:
                    ok = False
                    text += f"\nrun timeout={args.timeout}s"
        print(("ok   " if ok else "FAIL ") + case["name"] + f" rc={result.returncode}")
        if not ok:
            print(text, end="" if text.endswith("\n") else "\n")
            failures += 1
    print(f"complex cases={len(CASES)} failures={failures}")
    return 1 if failures else 0

if __name__ == "__main__":
    raise SystemExit(main())
