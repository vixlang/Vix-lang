import subprocess
import sys
import pytest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from helpers import compile_vix, COMPILER, TEST_DIR


@pytest.mark.cli
class TestVersionFlag:
    def test_version_short(self, compiler):
        res = subprocess.run([str(compiler), "-v"], capture_output=True, text=True, timeout=5)
        assert res.returncode == 0
        assert "Vix Compiler" in res.stdout
        assert "0.4.2" in res.stdout

    def test_version_long(self, compiler):
        res = subprocess.run([str(compiler), "--version"], capture_output=True, text=True, timeout=5)
        assert res.returncode == 0
        assert "Vix Compiler" in res.stdout


@pytest.mark.cli
class TestHelpFlag:
    def test_help_shows_usage(self, compiler):
        res = subprocess.run([str(compiler)], capture_output=True, text=True, timeout=5)
        assert res.returncode == 1
        assert "USAGE" in res.stderr
        assert "OPTIONS" in res.stderr

    def test_help_shows_options(self, compiler):
        res = subprocess.run([str(compiler)], capture_output=True, text=True, timeout=5)
        stderr = res.stderr
        assert "-o" in stderr
        assert "-S" in stderr
        assert "-ll" in stderr
        assert "-llvm" in stderr
        assert "-ast" in stderr


@pytest.mark.cli
class TestOutputFlag:
    def test_output_flag(self, compiler, tmp_path):
        src = tmp_path / "test.vix"
        src.write_text('fn main(): i32 { print("hello") return 0 }')
        out = tmp_path / "myoutput"
        res = compile_vix(compiler, src, out)
        assert res.returncode == 0
        assert out.exists()

    def test_output_missing_arg(self, compiler):
        res = subprocess.run([str(compiler), "-o"], capture_output=True, text=True, timeout=5)
        assert res.returncode == 1


@pytest.mark.cli
class TestLLVMOutput:
    def test_llvm_ir_output(self, compiler, tmp_path):
        src = tmp_path / "test.vix"
        src.write_text('fn main(): i32 { print(42) return 0 }')
        ll_file = tmp_path / "test.ll"
        res = subprocess.run(
            [str(compiler), str(src), "-ll", str(ll_file)],
            capture_output=True, text=True, timeout=30
        )
        assert res.returncode == 0
        assert ll_file.exists()
        content = ll_file.read_text()
        assert "define" in content

    def test_llvm_stdout(self, compiler, tmp_path):
        src = tmp_path / "test.vix"
        src.write_text('fn main(): i32 { return 0 }')
        res = subprocess.run(
            [str(compiler), str(src), "-llvm"],
            capture_output=True, text=True, timeout=30
        )
        assert res.returncode == 0
        assert "define" in res.stdout


@pytest.mark.cli
class TestASTOutput:
    def test_ast_dump(self, compiler, tmp_path):
        src = tmp_path / "test.vix"
        src.write_text('fn main(): i32 { let x = 42 print(x) return 0 }')
        res = subprocess.run(
            [str(compiler), str(src), "-ast"],
            capture_output=True, text=True, timeout=30
        )
        assert res.returncode == 0
        assert len(res.stdout) > 0


@pytest.mark.cli
class TestObjectOutput:
    def test_object_file(self, compiler, tmp_path):
        src = tmp_path / "test.vix"
        src.write_text('fn main(): i32 { return 0 }')
        obj = tmp_path / "test.o"
        res = subprocess.run(
            [str(compiler), str(src), "-obj", str(obj)],
            capture_output=True, text=True, timeout=30
        )
        assert res.returncode == 0
        assert obj.exists()

    def test_assembly_output(self, compiler, tmp_path):
        src = tmp_path / "test.vix"
        src.write_text('fn main(): i32 { return 0 }')
        asm = tmp_path / "test.s"
        res = subprocess.run(
            [str(compiler), str(src), "-S", str(asm)],
            capture_output=True, text=True, timeout=30
        )
        assert res.returncode == 0
        assert asm.exists()


@pytest.mark.cli
class TestOptimizationLevels:
    @pytest.mark.parametrize("level", [0, 1, 2, 3])
    def test_optimization_levels(self, compiler, tmp_path, level):
        src = tmp_path / "test.vix"
        src.write_text('fn main(): i32 { print(42) return 0 }')
        out = tmp_path / f"test_opt{level}"
        res = compile_vix(compiler, src, out, [f"-opt=l{level}"])
        assert res.returncode == 0
        assert out.exists()


@pytest.mark.cli
class TestErrorCases:
    def test_missing_input_file(self, compiler):
        res = subprocess.run(
            [str(compiler), "nonexistent.vix"],
            capture_output=True, text=True, timeout=5
        )
        assert res.returncode != 0

    def test_no_input_file(self, compiler):
        res = subprocess.run(
            [str(compiler), "-o", "out"],
            capture_output=True, text=True, timeout=5
        )
        assert res.returncode != 0
