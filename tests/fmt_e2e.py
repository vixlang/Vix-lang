#!/usr/bin/env python3
"""End to end checks for vixc --fmt.

The formatter is a hard requirement away from being a nuisance: it must be
idempotent, it must not change what a program means, and it must actually
respect the width and indentation it advertises.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MESSY = """type    Point = struct {
        x: i32,
      y: i32
}


fn (&Point)   length( ):   i32 {
    return self.x+self.y
}
fn main( ): i32 {
        let p=Point{ x: 3, y: 4 }
   print( p.length( ) )
  return 0
}
"""

EXPECTED = """type Point = struct {
    x: i32,
    y: i32
}

fn (&Point) length(): i32 {
    return self.x + self.y
}

fn main(): i32 {
    let p = Point{ x: 3, y: 4 }
    print(p.length())
    return 0
}
"""

LONG = """fn compute(first_argument: i32, second_argument: i32, third_argument: i32): i32 {
    return first_argument + second_argument + third_argument
}
"""


def compiler() -> Path:
    for name in ("vixc-patched", "vixc"):
        candidate = ROOT / "build" / name
        if candidate.exists():
            return candidate
    raise SystemExit("no compiler found in build/")


def run(cmd, **kwargs):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kwargs)


def main() -> int:
    vixc = compiler()
    failures = []
    with tempfile.TemporaryDirectory(prefix="vix-fmt-") as tmp_name:
        tmp = Path(tmp_name)

        source = tmp / "messy.vix"
        source.write_text(MESSY)
        result = run([str(vixc), str(source), "--fmt"])
        if result.returncode != 0:
            failures.append("--fmt failed: " + result.stdout)
        elif source.read_text() != EXPECTED:
            failures.append("--fmt output differs from the expected layout:\n" + source.read_text())

        # Idempotence across several widths.
        for width in ("100", "60", "40", "30"):
            work = tmp / ("w" + width + ".vix")
            work.write_text(LONG)
            run([str(vixc), str(work), "--fmt", "--max-width=" + width])
            first = work.read_text()
            run([str(vixc), str(work), "--fmt", "--max-width=" + width])
            if work.read_text() != first:
                failures.append("not idempotent at max-width " + width)

        # Width is respected for the structures the formatter can break.
        work = tmp / "width.vix"
        work.write_text(LONG)
        run([str(vixc), str(work), "--fmt", "--max-width=40"])
        for line in work.read_text().splitlines():
            if len(line) > 40 and "first_argument" not in line:
                failures.append("line over the limit at max-width 40: " + repr(line))

        # --fmt-check reports and exits non zero only when the file is not formatted.
        check = tmp / "check.vix"
        check.write_text(MESSY)
        if run([str(vixc), str(check), "--fmt-check"]).returncode == 0:
            failures.append("--fmt-check accepted an unformatted file")
        run([str(vixc), str(check), "--fmt"])
        if run([str(vixc), str(check), "--fmt-check"]).returncode != 0:
            failures.append("--fmt-check rejected a formatted file")

        # Indentation options.
        indent = tmp / "indent.vix"
        indent.write_text(MESSY)
        run([str(vixc), str(indent), "--fmt", "--indent=2"])
        if "\n  x: i32," not in indent.read_text():
            failures.append("--indent=2 was ignored")
        tabs = tmp / "tabs.vix"
        tabs.write_text(MESSY)
        run([str(vixc), str(tabs), "--fmt", "--use-tabs"])
        if "\n\tx: i32," not in tabs.read_text():
            failures.append("--use-tabs was ignored")

        # vix-fmt.toml next to the source.
        config_dir = tmp / "cfg"
        config_dir.mkdir()
        (config_dir / "vix-fmt.toml").write_text("max_width = 46\nindent_width = 2\n")
        configured = config_dir / "long.vix"
        configured.write_text(LONG)
        run([str(vixc), str(configured), "--fmt"])
        formatted = configured.read_text()
        if "fn compute(\n" not in formatted:
            failures.append("vix-fmt.toml max_width was ignored")
        if "\n  first_argument: i32," not in formatted:
            failures.append("vix-fmt.toml indent_width was ignored")

        # Formatting must not change semantics: the formatted program has to
        # compile exactly like the original.
        semantic = tmp / "semantic.vix"
        semantic.write_text(MESSY)
        before = run([str(vixc), str(semantic), "--check"]).returncode
        run([str(vixc), str(semantic), "--fmt"])
        after = run([str(vixc), str(semantic), "--check"]).returncode
        if before != after:
            failures.append("formatting changed the check result: " + str(before) + " -> " + str(after))

        # A file with syntax errors must be refused, not rewritten.
        broken = tmp / "broken.vix"
        broken.write_text("fn broken(: i32\n{\n    return 1\n}\n")
        original = broken.read_text()
        if run([str(vixc), str(broken), "--fmt"]).returncode == 0:
            failures.append("--fmt accepted a file with syntax errors")
        if broken.read_text() != original:
            failures.append("--fmt rewrote a file with syntax errors")

    for failure in failures:
        print("FAIL " + failure)
    if failures:
        return 1
    print("fmt: layout, idempotence, width, indent, config, check mode and semantics all OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
