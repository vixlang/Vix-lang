#!/usr/bin/env python3
"""End to end receiver method tests through the LLVM backend.

The self/LIR backend refuses non-x86_64 hosts, and vixc's embedded linker only
implements ELF and COFF, so on macOS the LLVM backend emits a Mach-O object that
this script links with the platform toolchain before running it.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "runtime" / "runtime.o"

ACCEPT = [
    ("methods_owned.vix", 7),
    ("methods_shared.vix", 14),
    ("methods_mut.vix", 5),
    ("methods_ambiguous.vix", 12),
    ("methods_owned_receiver.vix", 3),
]

REJECT = [
    ("methods_negative_mut.vix", "use a mutable reference"),
    ("methods_negative_unknown.vix", "unknown method"),
]


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
    with tempfile.TemporaryDirectory(prefix="vix-methods-") as tmp_name:
        tmp = Path(tmp_name)
        for name, expected in ACCEPT:
            source = ROOT / "tests" / name
            obj = tmp / (name + ".o")
            binary = tmp / name.replace(".vix", "")
            build = run([str(vixc), str(source), "-obj", "-o", str(obj)], cwd=ROOT)
            if build.returncode != 0:
                failures.append(name + ": compile failed\n" + build.stdout)
                continue
            link = run(["clang++", str(obj), str(RUNTIME), "-o", str(binary)], cwd=ROOT)
            if link.returncode != 0:
                failures.append(name + ": link failed\n" + link.stdout)
                continue
            result = run([str(binary)], cwd=ROOT)
            if result.returncode != expected:
                failures.append(name + ": expected exit " + str(expected) + " got " + str(result.returncode))
            else:
                print("ok   " + name + " -> " + str(expected))

        for name, needle in REJECT:
            source = ROOT / "tests" / name
            result = run([str(vixc), str(source), "--ownership-check"], cwd=ROOT)
            if result.returncode == 0:
                failures.append(name + ": expected a diagnostic, compilation succeeded")
            elif needle not in result.stdout:
                failures.append(name + ": diagnostic did not mention " + repr(needle) + "\n" + result.stdout)
            else:
                print("ok   " + name + " -> rejected")

    for failure in failures:
        print("FAIL " + failure)
    if failures:
        return 1
    print("methods: " + str(len(ACCEPT)) + " accepted, " + str(len(REJECT)) + " rejected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
