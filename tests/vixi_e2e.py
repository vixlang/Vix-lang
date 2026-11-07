#!/usr/bin/env python3
"""Interface file (.vixi) emission and its invalidation rules.

The interface file is a cache, so the properties that matter are:
  * it round-trips (the JSON carries the signatures back),
  * changing a signature does,
  * a pure reorder is treated as a change too (conservative, never stale),
  * changing the source does.

Usage:  python3 tests/vixi_e2e.py [path-to-vixc]
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MODULE_A = """pub type Point = struct {
    x: i32,
    y: i32
}

pub type Colour = Red | Green

type Secret = Hidden | Gone

pub fn add(a: i32, b: i32): i32
{
    return a + b
}

fn hidden(value: i32): i32 { return value }

pub fn id:[T](value: T): T
{
    return value
}
"""

# Same declarations, different order.
MODULE_A_REORDERED = """pub fn id:[T](value: T): T
{
    return value
}

pub type Colour = Red | Green

pub fn add(a: i32, b: i32): i32
{
    return a + b
}

pub type Point = struct {
    x: i32,
    y: i32
}
"""

# Same shape, different signature.
MODULE_A_CHANGED = MODULE_A.replace(
    "pub fn add(a: i32, b: i32): i32", "pub fn add(a: i32, b: i32, c: i32): i32")


def find_compiler(argv):
    if len(argv) > 1:
        return argv[1]
    for name in ("vixc-patched", "vixc"):
        candidate = os.path.join(ROOT, "build", name)
        if os.path.exists(candidate):
            return candidate
    return None


def emit(compiler, directory, name, source):
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(source)
    result = subprocess.run([compiler, path, "--emit-vixi"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("emit failed for %s: %s" % (name, result.stderr))
    interface = path[: -len(".vix")] + ".vixi"
    with open(interface, encoding="utf-8") as handle:
        return json.load(handle)


def main():
    compiler = find_compiler(sys.argv)
    if compiler is None:
        print("vixi e2e: no compiler found in build/")
        return 1

    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        first = emit(compiler, tmp, "a1.vix", MODULE_A)
        reordered = emit(compiler, tmp, "a2.vix", MODULE_A_REORDERED)
        changed = emit(compiler, tmp, "a3.vix", MODULE_A_CHANGED)

        names = [f["name"] for f in first["functions"]]
        if names != ["add", "id"]:
            print("vixi e2e: FAIL function list is %r" % (names,))
            failed += 1

        generic = [f for f in first["functions"] if f["name"] == "id"]
        if not generic or generic[0]["type_params"] != ["T"]:
            print("vixi e2e: FAIL generics not preserved")
            failed += 1

        if any(f["name"] == "hidden" for f in first["functions"]):
            print("vixi e2e: FAIL private function leaked into interface")
            failed += 1
        if any(t["name"] == "Secret" for t in first["types"]):
            print("vixi e2e: FAIL private type leaked into interface")
            failed += 1
        if any(f["name"] == "hidden" for f in first["functions"]):
            print("vixi e2e: FAIL private function leaked")
            failed += 1
        if any(t["name"] == "Secret" for t in first["types"]):
            print("vixi e2e: FAIL private type leaked")
            failed += 1
        if any(f["is_pub"] != 1 for f in first["functions"]):
            print("vixi e2e: FAIL exported function is not marked pub")
            failed += 1
        kinds = sorted(t["kind"] for t in first["types"])
        if kinds != ["adt", "struct"]:
            print("vixi e2e: FAIL type kinds are %r" % (kinds,))
            failed += 1

        point = [t for t in first["types"] if t["name"] == "Point"]
        if not point or point[0]["field_names"] != ["x", "y"]:
            print("vixi e2e: FAIL struct fields not recorded")
            failed += 1

        # Order sensitivity is intentional: the interface hash never misses a
        # change, at the cost of also invalidating on a pure reorder.
        if first["interface_hash"] == reordered["interface_hash"]:
            print("vixi e2e: FAIL reordering kept the interface hash")
            failed += 1
        if first["source_hash"] == reordered["source_hash"]:
            print("vixi e2e: FAIL reordering did not change the source hash")
            failed += 1
        if first["interface_hash"] == changed["interface_hash"]:
            print("vixi e2e: FAIL a changed signature kept the interface hash")
            failed += 1

        # Interface format 2: type names are module qualified.
        if first["vixi"] != 2 or "vixc" not in first["compiler"]:
            print("vixi e2e: FAIL format/compiler header is %r/%r"
                  % (first["vixi"], first["compiler"]))
            failed += 1

    if failed:
        print("vixi e2e: %d check(s) failed" % failed)
        return 1
    print("vixi e2e: interfaces round-trip; signatures and sources invalidate "
          "them, and a reorder does too (conservative)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
