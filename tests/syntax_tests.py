#!/usr/bin/env python3
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SYNTAX = ROOT / "tests" / "syntax"
LEGAL = {
    "expr_if": ["IfExpression", "IfStatement"],
    "expr_else": ["IfExpression", "BlockExpression", "IfStatement"],
    "expr_while": ["WhileStatement", "AssignStatement"],
    "expr_for": ["ForRangeStatement", "ExpressionStatement"],
    "expr_fn": ["ReturnStatement", "BinaryExpression"],
    "expr_nested": ["IfExpression", "WhileStatement", "ForRangeStatement"],
    "expr_assignment": ["AssignStatement", '"operator":"=="'],
}
INVALID = {
    "expr_missing": ["error[E1001]", "expected expression after '=>'", "got ''", "add one return expression"],
    "expr_bad_else": ["error[E1001]", "expected 'else'", "got 'fn'", "add 'else => expression'"],
    "expr_fn_type": ["error[E3002]", "expected i32, got string"],
    "expr_multi_stmt": ["error[E1002]", "got 'let'", "use a function body '{ ... }'"],
    "expr_arrow_conflict": ["error[E1001]", "got '->'", "replace '->' with '=>'"],
    "expr_if_type": ["error[E3002]", "expected i32, got string"],
    "expr_while_type": ["error[E3002]", "expected bool, got string"],
    "expr_for_type": ["error[E3002]", "expected [i32], got i32"],
    "expr_loop_decl": ["error[E1002]", "got declaration or statement 'let'", "use '{ ... }'"],
    "expr_anon_fn": ["error[E1001]", "expected function name", "got '('", "name the function"],
}


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def require(result: subprocess.CompletedProcess[str], expected_rc: int, context: str) -> str:
    output = result.stdout + result.stderr
    if result.returncode != expected_rc:
        raise AssertionError(f"{context}: expected rc={expected_rc}, got {result.returncode}\n{output}")
    return output


def main() -> int:
    compiler = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "vixc"
    if not compiler.is_absolute():
        compiler = ROOT / compiler
    output_dir = ROOT / "build" / "syntax-tests"
    output_dir.mkdir(parents=True, exist_ok=True)

    for name, ast_fragments in LEGAL.items():
        source = SYNTAX / f"{name}.vix"
        ast_output = require(run([str(compiler), str(source), "--ast-json"]), 0, f"{name} AST")
        parsed = json.loads(ast_output)
        if parsed.get("type") != "Program":
            raise AssertionError(f"{name}: AST root is not Program\n{ast_output}")
        for fragment in ast_fragments:
            if fragment not in ast_output:
                raise AssertionError(f"{name}: AST omitted {fragment!r}\n{ast_output}")

        require(run([str(compiler), "--check", str(source)]), 0, f"{name} --check")
        require(run([str(compiler), "--ownership-check", str(source)]), 0, f"{name} ownership/lifetime")
        for backend in ("llvm", "self", "self-lir"):
            artifact = output_dir / f"{name}-{backend}"
            command = [str(compiler), str(source), "-o", str(artifact)]
            if backend != "llvm":
                command.append(f"--backend={backend}")
            require(run(command), 0, f"{name} {backend} compile")
            require(run([str(artifact)]), 0, f"{name} {backend} run")
        print(f"ok   {name}: AST/check/ownership/llvm/self/self-lir")

    lex_output = require(run([str(compiler), str(SYNTAX / "expr_assignment.vix"), "--lex"]), 0, "operator lexing")
    for token in ("text='='", "text='=='", "text='=>'" ):
        if token not in lex_output:
            raise AssertionError(f"operator lexing omitted {token!r}\n{lex_output}")
    print("ok   =, ==, => token separation")

    for name, fragments in INVALID.items():
        source = SYNTAX / f"{name}.vix"
        result = run([str(compiler), str(source), "--check", "--color=never"])
        output = result.stdout + result.stderr
        if result.returncode == 0:
            raise AssertionError(f"{name}: expected failure\n{output}")
        location = f"tests/syntax/{name}.vix:"
        for fragment in [location, *fragments]:
            if fragment not in output:
                raise AssertionError(f"{name}: diagnostic omitted {fragment!r}\n{output}")
        print(f"ok   {name}: rejected with located diagnostic")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
