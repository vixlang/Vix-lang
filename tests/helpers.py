import os
import platform
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMPILER = ROOT / "build" / "vixc"
if platform.system() == "Windows" and not str(COMPILER).endswith(".exe"):
    COMPILER = Path(str(COMPILER) + ".exe")
TEST_DIR = ROOT / "tests" / "regression"
EXAMPLES_DIR = ROOT / "examples"
STD_DIR = ROOT / "src" / "std"


def compile_vix(compiler: Path, source: Path, output: Path, extra_args=None):
    cmd = [str(compiler), str(source), "-o", str(output)]
    if extra_args:
        cmd.extend(extra_args)
    return subprocess.run(cmd, capture_output=True, text=True, timeout=30)


def run_binary(binary: Path, timeout=5):
    if platform.system() == "Windows" and not str(binary).endswith(".exe"):
        binary = Path(str(binary) + ".exe")
    return subprocess.run([str(binary)], capture_output=True, text=True, timeout=timeout)


def compile_and_run(compiler: Path, source_code: str, tmp_dir: Path, extra_args=None):
    src = tmp_dir / "test.vix"
    src.write_text(source_code)
    bin_path = tmp_dir / "test_bin"
    compile_res = compile_vix(compiler, src, bin_path, extra_args)
    if compile_res.returncode != 0:
        return compile_res, None
    run_res = run_binary(bin_path)
    return compile_res, run_res


def compile_source(compiler: Path, source_code: str, tmp_dir: Path, extra_args=None):
    src = tmp_dir / "test.vix"
    src.write_text(source_code)
    bin_path = tmp_dir / "test_bin"
    return compile_vix(compiler, src, bin_path, extra_args), bin_path
