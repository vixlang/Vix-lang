#!/usr/bin/env python3

import shutil
import subprocess
import sys
from pathlib import Path


def run(command: list[str], root: Path, timeout: int = 30) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=root, text=True, capture_output=True, timeout=timeout)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    compiler = root / "build" / "vixc"
    output = root / "tests" / "boot16.bin"
    source = root / "tests" / "files" / "boot16_minimal.vix"
    output.unlink(missing_ok=True)

    result = run([str(compiler), "--target=boot16", "--emit=bin", str(source), "-o", str(output)], root)
    if result.returncode != 0:
        print(result.stdout + result.stderr)
        return 1
    data = output.read_bytes()
    if len(data) != 512:
        print(f"boot16: expected 512 bytes, got {len(data)}")
        return 1
    if data[510:512] != b"\x55\xaa":
        print(f"boot16: invalid signature {data[510:512].hex()}")
        return 1

    failures = {
        "boot16_call_fail.vix": "boot16 does not support function calls",
        "boot16_float_fail.vix": "boot16 does not support floating point",
    }
    for name, diagnostic in failures.items():
        failed = run([str(compiler), "--target=boot16", "--emit=bin", str(root / "tests" / "files" / name), "-o", str(output)], root)
        if failed.returncode == 0 or diagnostic not in failed.stdout + failed.stderr:
            print(f"boot16: missing expected diagnostic for {name}: {diagnostic}")
            print(failed.stdout + failed.stderr)
            return 1

    file_result = run(["file", str(output)], root)
    xxd_result = run(["xxd", "-s", "510", "-l", "2", str(output)], root)
    print(f"ok   boot16 size=512 signature=55aa")
    print(file_result.stdout.strip())
    print(xxd_result.stdout.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
