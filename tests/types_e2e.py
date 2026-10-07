#!/usr/bin/env python3
"""Type namespace regression tests.

Phase 1 of the type-system rework gives every struct/ADT declaration a symbol
carrying the module it came from.  What is checked here:

  * the symbol table records qualified names per module,
  * a single-file program registers its types too,
  * two real declarations of the same type are reported instead of one of them
    silently replacing the other (ADTs used to be deduplicated in silence).

Usage:  python3 tests/types_e2e.py [path-to-vixc]
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_compiler(argv):
    if len(argv) > 1:
        return argv[1]
    for name in ("vixc-patched", "vixc"):
        candidate = os.path.join(ROOT, "build", name)
        if os.path.exists(candidate):
            return candidate
    return None


def write(path, text):
    with open(path, "w") as handle:
        handle.write(text)


def run(compiler, path, mode):
    return subprocess.run([compiler, path, mode], capture_output=True, text=True)


def main():
    compiler = find_compiler(sys.argv)
    if compiler is None:
        print("types e2e: no compiler found in build/")
        return 2

    work = tempfile.mkdtemp(prefix="vix-types-")
    failures = []

    def case(name, ok, detail=""):
        if ok:
            print("types e2e: ok  " + name)
        else:
            print("types e2e: FAIL " + name + (": " + detail if detail else ""))
            failures.append(name)

    try:
        write(os.path.join(work, "a.vix"), "#[no_main]\npub type Foo = struct { x: i32 }\n"
              "pub fn make_a(): Foo { return Foo{ x: 11 } }\n")
        write(os.path.join(work, "b.vix"), "#[no_main]\npub type Foo = struct { y: i64 }\n"
              "pub fn make_b(): Foo { return Foo{ y: 22 } }\n")
        write(os.path.join(work, "same_name.vix"),
              'use "a.vix" as a\nuse "b.vix" as b\n'
              'fn main(): i32 { let p: a::Foo = a::make_a() let q: b::Foo = b::make_b() return p.x }\n')
        res = run(compiler, os.path.join(work, "same_name.vix"), "--check")
        case("same-named structs in different modules coexist",
             res.returncode == 0, res.stdout + res.stderr)

        write(os.path.join(work, "x.vix"), "#[no_main]\npub type Shape = Circle(i32)\n")
        write(os.path.join(work, "y.vix"), "#[no_main]\npub type Shape = Square(i32)\n")
        write(os.path.join(work, "dup_adt.vix"),
              'use "x.vix" as x\nuse "y.vix" as y\nfn main(): i32 { return 0 }\n')
        res = run(compiler, os.path.join(work, "dup_adt.vix"), "--check")
        case("same-named ADTs in different modules coexist",
             res.returncode == 0, res.stdout + res.stderr)

        symbols = run(compiler, os.path.join(work, "dup_adt.vix"), "--type-symbols")
        text = symbols.stdout
        case("symbols carry the module path",
             "x::Shape" in text and "y::Shape" in text, text)
        case("same-named types get different ids",
             any(line.startswith("0\tx::Shape") for line in text.splitlines())
             and any(line.startswith("1\ty::Shape") for line in text.splitlines()), text)

        write(os.path.join(work, "single.vix"),
              "type Only = struct { v: i32 }\nfn main(): i32 { return 0 }\n")
        symbols = run(compiler, os.path.join(work, "single.vix"), "--type-symbols")
        case("single-file program registers its types",
             "\tOnly\tstruct\t" in symbols.stdout, symbols.stdout)

        write(os.path.join(work, "bare_ambiguous.vix"),
              'use "a.vix" as a\nuse "b.vix" as b\n'
              'fn main(): i32 { let p: Foo = a::make_a() return p.x }\n')
        res = run(compiler, os.path.join(work, "bare_ambiguous.vix"), "--check")
        case("an ambiguous bare type name is reported",
             res.returncode != 0 and "unknown type 'Foo'" in (res.stdout + res.stderr),
             res.stdout + res.stderr)

        write(os.path.join(work, "typo.vix"),
              "fn main(): i32 { let x: NoSuchType = 0 return 0 }\n")
        res = run(compiler, os.path.join(work, "typo.vix"), "--check")
        case("an unknown type name is reported",
             res.returncode != 0 and "unknown type 'NoSuchType'" in (res.stdout + res.stderr),
             res.stdout + res.stderr)

        write(os.path.join(work, "same_file.vix"),
              "type Twice = struct { a: i32 }\ntype Twice = struct { b: i32 }\n"
              "fn main(): i32 { return 0 }\n")
        res = run(compiler, os.path.join(work, "same_file.vix"), "--check")
        case("duplicate type in one module is reported",
             res.returncode != 0 and "duplicate" in (res.stdout + res.stderr),
             res.stdout + res.stderr)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print("types e2e: " + str(len(failures)) + " failure(s)")
        return 1
    print("types e2e: type symbols are per-module and duplicates are reported")
    return 0


if __name__ == "__main__":
    sys.exit(main())
