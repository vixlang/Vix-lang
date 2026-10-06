#!/usr/bin/env python3
"""The .vixi cache: written after a check, trusted only when still valid.

Three modules, root -> mid -> dep, so that mid records dep's interface hash and
the interesting property can be observed:

  * editing dep's BODY does not move dep's interface hash, so mid is not
    invalidated -- this is why the interface is hashed separately from the
    source;
  * editing dep's SIGNATURE does move it, so mid is invalidated.

Get either of those wrong and the cache is either useless or a source of silent
miscompiles.

Usage:  python3 tests/vixi_cache_e2e.py [path-to-vixc]
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEP_BODY_7 = "pub fn value(): i32\n{\n    return 7\n}\n"
DEP_BODY_9 = "pub fn value(): i32\n{\n    return 9\n}\n"
DEP_SIG = "pub fn extra(): i32\n{\n    return 1\n}\n\n" + DEP_BODY_7

MID = 'use "dep.vix" as dep\n\npub fn call(): i32\n{\n    return dep::value()\n}\n'
ROOT_SRC = 'use "mid.vix" as mid\n\nfn main(): i32\n{\n    return mid::call()\n}\n'


def find_compiler(argv):
    if len(argv) > 1:
        return argv[1]
    for name in ("vixc-patched", "vixc"):
        candidate = os.path.join(ROOT, "build", name)
        if os.path.exists(candidate):
            return candidate
    return None


def write(path, text):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def main():
    compiler = find_compiler(sys.argv)
    if compiler is None:
        print("vixi cache: no compiler found in build/")
        return 1

    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        dep = os.path.join(tmp, "dep.vix")
        mid = os.path.join(tmp, "mid.vix")
        root = os.path.join(tmp, "main.vix")
        dep_vixi = os.path.join(tmp, "dep.vixi")
        mid_vixi = os.path.join(tmp, "mid.vixi")

        write(dep, DEP_BODY_7)
        write(mid, MID)
        write(root, ROOT_SRC)

        first = subprocess.run([compiler, root, "--check"], capture_output=True, text=True)
        if first.returncode != 0:
            print("vixi cache: FAIL first compile: " + first.stderr.strip()[:300])
            return 1
        if not os.path.exists(dep_vixi) or not os.path.exists(mid_vixi):
            print("vixi cache: FAIL interfaces were not written")
            return 1

        dep_before = load(dep_vixi)
        mid_before = load(mid_vixi)
        if not mid_before["dep_hashes"]:
            print("vixi cache: FAIL mid did not record its dependency")
            failed += 1
        elif mid_before["dep_hashes"][0] != dep_before["interface_hash"]:
            print("vixi cache: FAIL mid recorded the wrong dependency hash")
            failed += 1

        # 1) body-only change
        write(dep, DEP_BODY_9)
        second = subprocess.run([compiler, root, "--check"], capture_output=True, text=True)
        if second.returncode != 0:
            print("vixi cache: FAIL second compile: " + second.stderr.strip()[:300])
            return 1
        dep_after = load(dep_vixi)
        if dep_after["interface_hash"] != dep_before["interface_hash"]:
            print("vixi cache: FAIL a body-only change moved the interface hash")
            failed += 1
        if dep_after["source_hash"] == dep_before["source_hash"]:
            print("vixi cache: FAIL a body-only change did not move the source hash")
            failed += 1
        if load(mid_vixi)["dep_hashes"] != mid_before["dep_hashes"]:
            print("vixi cache: FAIL a body-only change invalidated the dependent")
            failed += 1

        # 2) signature change
        write(dep, DEP_SIG)
        third = subprocess.run([compiler, root, "--check"], capture_output=True, text=True)
        if third.returncode != 0:
            print("vixi cache: FAIL third compile: " + third.stderr.strip()[:300])
            return 1
        dep_final = load(dep_vixi)
        if dep_final["interface_hash"] == dep_before["interface_hash"]:
            print("vixi cache: FAIL a signature change kept the interface hash")
            failed += 1
        if load(mid_vixi)["dep_hashes"] == mid_before["dep_hashes"]:
            print("vixi cache: FAIL a signature change did not invalidate the dependent")
            failed += 1

    if failed:
        print("vixi cache: %d check(s) failed" % failed)
        return 1
    print("vixi cache: interfaces are written after a check; a body edit is "
          "inert, a signature edit is not")
    return 0


if __name__ == "__main__":
    sys.exit(main())
