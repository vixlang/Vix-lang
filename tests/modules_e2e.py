#!/usr/bin/env python3
"""End-to-end multi-file compilation on the LLVM backend.

Compiles a multi-module program with `-obj`, links it with the system C++
driver (vixc's embedded linker only implements the ELF and COFF drivers, so on
macOS the object file has to be linked externally) and checks the exit code.

This is the baseline that the module system work is verified against: it is the
only thing that says "codegen still works" after the module syntax changes.

Usage:  python3 tests/modules_e2e.py [path-to-vixc]
"""

import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME = os.path.join(ROOT, "runtime", "runtime.o")

# (source relative to the repo root, expected exit code)
CASES = [
    ("tests/module/main.vix", 17),                    # two modules, qualified calls
    ("tests/modules_generics/main.vix", 49),          # cross-module generics
    ("examples/modules/basic_example.vix", 17),       # two modules
    ("examples/modules/multi_module.vix", 0),         # three modules, struct across modules
    ("examples/modules/nested_module.vix", 5),        # directory module + submodule
    ("tests/module_global_merge.vix", 0),             # globals merged across modules
    ("tests/complex/mod_basic.vix", 0),
]


def find_compiler(argv):
    if len(argv) > 1:
        return argv[1]
    for name in ("vixc-patched", "vixc"):
        candidate = os.path.join(ROOT, "build", name)
        if os.path.exists(candidate):
            return candidate
    return None


def main():
    compiler = find_compiler(sys.argv)
    if compiler is None:
        print("modules e2e: no compiler found in build/")
        return 1
    if not os.path.exists(RUNTIME):
        print("modules e2e: missing " + RUNTIME)
        return 1
    linker = shutil.which("clang++") or shutil.which("clang")
    if linker is None:
        print("modules e2e: skipped (no clang++ to link Mach-O objects)")
        return 0

    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for source, expected in CASES:
            path = os.path.join(ROOT, source)
            if not os.path.exists(path):
                print("modules e2e: MISSING " + source)
                failed += 1
                continue
            obj = os.path.join(tmp, "unit.o")
            exe = os.path.join(tmp, "unit")
            for stale in (obj, exe):
                if os.path.exists(stale):
                    os.remove(stale)

            built = subprocess.run(
                [compiler, path, "-obj", "-o", obj],
                capture_output=True, text=True)
            if built.returncode != 0 or not os.path.exists(obj):
                print("modules e2e: FAIL compile " + source)
                print((built.stdout + built.stderr).strip()[:400])
                failed += 1
                continue

            linked = subprocess.run(
                [linker, obj, RUNTIME, "-o", exe],
                capture_output=True, text=True)
            if linked.returncode != 0:
                print("modules e2e: FAIL link " + source)
                print((linked.stdout + linked.stderr).strip()[:400])
                failed += 1
                continue

            ran = subprocess.run([exe], capture_output=True, text=True)
            if ran.returncode != expected:
                print("modules e2e: FAIL run %s (exit %d, expected %d)"
                      % (source, ran.returncode, expected))
                failed += 1
                continue
            print("modules e2e: ok %-42s exit=%d" % (source, ran.returncode))

    if failed:
        print("modules e2e: %d case(s) failed" % failed)
        return 1
    print("modules e2e: %d multi-module programs compile, link and run"
          % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
