#!/usr/bin/env python3
"""Keeps the skill documentation honest.

skill/ is meant to be a stable, cache friendly prefix for AI sessions. Stale
paths, dead links and code samples that no longer compile all destroy that: the
reader follows a link that is gone, or copies an example that cannot build.

Checks:
  1. every relative markdown link under skill/ resolves
  2. every repository path shown in inline code exists
  3. every fenced vix block that is a complete program (it defines main)
     compiles with --check

Blocks that only illustrate a statement or a declaration are counted as
fragments and are not compiled; a fragment cannot be a complete program by
definition.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKILL = ROOT / "skill"

TICK = chr(96)
FENCE = re.compile(TICK * 3 + r".*?" + TICK * 3, re.S)
VIX_BLOCK = re.compile(TICK * 3 + r"vix\n(.*?)" + TICK * 3, re.S)
LINK = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
CODE = re.compile(TICK + r"([^" + TICK + r"]+)" + TICK)
PREFIXES = ("src/", "tests/", "scripts/", "docs/", "vix-analyzer/", "editors/", "examples/", "runtime/", "lib/")
SUFFIXES = (".vix", ".md", ".sh", ".py", ".json", ".c", ".h", ".vsix", ".o", ".vc", ".tyir")

COMPILER = ROOT / "build" / "vixc-patched"


def check_links(files):
    failures = []
    for path in files:
        rel = path.relative_to(ROOT)
        text = path.read_text()
        prose = FENCE.sub("", text)
        for target in LINK.findall(prose):
            if target.startswith(("http://", "https://", "#")):
                continue
            if not (path.parent / target).resolve().exists():
                failures.append(str(rel) + ": dead link -> " + target)
        for token in CODE.findall(text):
            token = token.strip()
            if not token.startswith(PREFIXES) or not token.endswith(SUFFIXES):
                continue
            if any(ch in token for ch in " <>*$"):
                continue
            if not (ROOT / token).exists():
                failures.append(str(rel) + ": missing path -> " + token)
    return failures


def check_blocks(files, tmp):
    compiled = 0
    fragments = 0
    failures = []
    if not COMPILER.exists():
        return compiled, fragments, ["compiler missing at " + str(COMPILER)]
    for path in files:
        rel = path.relative_to(ROOT)
        for index, block in enumerate(VIX_BLOCK.findall(path.read_text())):
            if "fn main(" not in block:
                fragments += 1
                continue
            source = Path(tmp) / (path.stem + "_%d.vix" % index)
            source.write_text(block)
            result = subprocess.run(
                [str(COMPILER), str(source), "--check"],
                capture_output=True,
                text=True,
            )
            if result.returncode == 0:
                compiled += 1
                continue
            errors = [line for line in (result.stdout + result.stderr).splitlines() if line.startswith("error[")]
            detail = errors[0] if errors else "exit " + str(result.returncode)
            failures.append(str(rel) + " block " + str(index) + ": " + detail)
    return compiled, fragments, failures


def main() -> int:
    files = sorted(SKILL.rglob("*.md"))
    if not files:
        print("FAIL no skill documentation found")
        return 1

    failures = check_links(files)
    with tempfile.TemporaryDirectory() as tmp:
        compiled, fragments, block_failures = check_blocks(files, tmp)
    failures.extend(block_failures)

    for failure in failures:
        print("FAIL " + failure)
    if failures:
        return 1
    print(
        "skill: " + str(len(files)) + " documents, links resolve, "
        + str(compiled) + " complete examples compile (" + str(fragments) + " fragments skipped)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
