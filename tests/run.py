#!/usr/bin/env python3

import subprocess
import sys
from pathlib import Path

import platform

ROOT = Path(__file__).resolve().parent
if platform.system() == "Windows":
    VENV_PYTHON = ROOT / ".venv" / "Scripts" / "python.exe"
else:
    VENV_PYTHON = ROOT / ".venv" / "bin" / "python3"
COMPILER = ROOT / "build" / "vixc"
if platform.system() == "Windows" and not str(COMPILER).endswith(".exe"):
    COMPILER = Path(str(COMPILER) + ".exe")

GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
BOLD = "\033[1m"
RESET = "\033[0m"


def header(title):
    print(f"\n{BOLD}{'=' * 60}")
    print(f"  {title}")
    print(f"{'=' * 60}{RESET}\n")


def run_step(name, cmd, cwd=None):
    print(f"{YELLOW}[RUN]{RESET} {name}")
    res = subprocess.run(cmd, cwd=cwd or str(ROOT), text=True)
    if res.returncode == 0:
        print(f"{GREEN}[PASS]{RESET} {name}\n")
    else:
        print(f"{RED}[FAIL]{RESET} {name} (exit code {res.returncode})\n")
    return res.returncode


def main():
    results = {}
    python = str(VENV_PYTHON) if VENV_PYTHON.exists() else sys.executable

    header("pytest")
    rc = run_step(
        "pytest",
        [python, "-m", "pytest", "-v", "--tb=short"],
    )
    results["pytest"] = rc

    header("Summary")
    all_pass = True
    for name, rc in results.items():
        status = f"{GREEN}PASS{RESET}" if rc == 0 else f"{RED}FAIL{RESET}"
        print(f"  {name:<20} {status}")
        if rc != 0:
            all_pass = False

    print()
    if all_pass:
        print(f"{GREEN}{BOLD}All test suites passed.{RESET}")
        return 0
    else:
        print(f"{RED}{BOLD}Some test suites failed.{RESET}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
