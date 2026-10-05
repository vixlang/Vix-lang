#!/usr/bin/env python3
"""Grammar regression: receiver method declarations must be highlighted.

The method form "fn (&mut Point) move(...)" is not matched by the plain
function rule, which requires an identifier right after "fn".  Before the
dedicated method rule existed, "fn" fell through to the variable rule and was
rendered as an ordinary identifier, so it looked unhighlighted.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GRAMMAR = ROOT / "editors" / "vscode" / "syntaxes" / "vix.tmLanguage.json"


def find_rule(repo: dict, scope: str) -> dict:
    for pattern in repo["patterns"]:
        if pattern.get("name") == scope:
            return pattern
    raise AssertionError(scope + " rule not found")


def main() -> int:
    grammar = json.loads(GRAMMAR.read_text())
    repo = grammar["repository"]
    failures: list[str] = []

    method = find_rule(repo["declarations"], "meta.function.method.vix")
    plain = find_rule(repo["declarations"], "meta.function.vix")
    method_re = re.compile(method["match"])
    plain_re = re.compile(plain["match"])

    method_cases = {
        "fn (Point) copy(): Point": {"1": "fn", "3": None, "4": None, "5": "Point", "7": "copy"},
        "fn (&Point) length(): i32": {"1": "fn", "3": "&", "4": None, "5": "Point", "7": "length"},
        "fn (&mut Point) move(dx: i32, dy: i32)": {"1": "fn", "3": "&", "4": "mut", "5": "Point", "7": "move"},
        "fn (Point[T]) size(): i32": {"1": "fn", "3": None, "4": None, "5": "Point[T]", "7": "size"},
        "fn (&mut Point[T]) clear()": {"1": "fn", "3": "&", "4": "mut", "5": "Point[T]", "7": "clear"},
    }
    for line, want in method_cases.items():
        match = method_re.search(line)
        if not match:
            failures.append("method rule did not match: " + line)
            continue
        for group, expected in want.items():
            got = match.group(int(group))
            if got != expected:
                failures.append(
                    line + ": capture " + group + " expected " + repr(expected) + " got " + repr(got)
                )

    if not plain_re.search("fn make_point(x: i32): Point"):
        failures.append("plain function rule regressed for fn make_point")

    keyword_patterns = [p.get("match", "") for p in repo["keywords"]["patterns"]]
    if not any(re.search(r"\\bfn\\b", pattern) for pattern in keyword_patterns):
        failures.append("no fallback fn keyword rule in #keywords")

    type_patterns = [
        p.get("match", "")
        for p in repo["declarations"]["patterns"]
        if p.get("name") == "meta.type.vix"
    ]
    if any("impl" in pattern for pattern in type_patterns):
        failures.append("stale impl keyword still present in meta.type.vix")

    for failure in failures:
        print("FAIL " + failure)
    if failures:
        return 1
    print("grammar: method receiver highlighting OK (" + str(len(method_cases)) + " cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
