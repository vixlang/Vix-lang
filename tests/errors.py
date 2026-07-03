import sys
import pytest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from helpers import compile_vix, compile_and_run


@pytest.mark.error
class TestTypeErrors:
    def test_type_mismatch_function_return(self, compiler, tmp_path):
        src = 'fn foo(): i32 { return "hello" } fn main(): i32 { return 0 }'
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0
        assert "expected type" in res.stderr.lower() or "type mismatch" in res.stderr.lower()

    def test_type_mismatch_binary_op(self, compiler, tmp_path):
        src = 'fn main(): i32 { let x = "hello" + 5 return 0 }'
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0
        assert "type" in res.stderr.lower()

    def test_type_mismatch_return_void(self, compiler, tmp_path):
        src = 'fn main() { return 0 }'
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0
        assert "expected type" in res.stderr.lower() or "type mismatch" in res.stderr.lower()


@pytest.mark.error
class TestUndefinedIdentifiers:
    def test_undefined_variable(self, compiler, tmp_path):
        src = 'fn main(): i32 { print(undefined_var) return 0 }'
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0
        assert "undefined" in res.stderr.lower() or "not found" in res.stderr.lower()

    def test_undefined_function(self, compiler, tmp_path):
        src = 'fn main(): i32 { print(undefined_fn()) return 0 }'
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0


@pytest.mark.error
class TestSelfRecursiveStruct:
    def test_self_recursive_struct(self, compiler, tmp_path):
        src = '''struct Node {
            value: i32,
            next: Node
        }
        fn main(): i32 { return 0 }'''
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0
        assert "recursive" in res.stderr.lower() or "self-recursive" in res.stderr.lower()


@pytest.mark.error
class TestCapturingLocals:
    def test_capturing_local_variable(self, compiler, tmp_path):
        src = '''fn main(): i32 {
            let x = 5
            fn inner(): i32 { return x }
            print(inner())
            return 0
        }'''
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0
        assert "capturing" in res.stderr.lower()


@pytest.mark.error
class TestMatchExhaustiveness:
    def test_non_exhaustive_match_adt(self, compiler, tmp_path):
        src = '''type Status = Active | Inactive
        fn main(): i32 {
            let s = Active
            match s {
                Active -> { print("active") }
            }
            return 0
        }'''
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0
        assert "exhaustive" in res.stderr.lower() or "non-exhaustive" in res.stderr.lower()


@pytest.mark.error
class TestRedefinition:
    def test_redefinition_variable(self, compiler, tmp_path):
        src = '''fn main(): i32 {
            let x = 10
            let x = 20
            return 0
        }'''
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0
        assert "redefinition" in res.stderr.lower() or "redefin" in res.stderr.lower()


@pytest.mark.error
class TestDiagnosticsQuality:
    def test_arrow_return_type_is_error(self, compiler, tmp_path):
        src = 'fn foo() -> i32 { return 1 } fn main(): i32 { return 0 }'
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0
        assert "invalid function return syntax" in res.stderr.lower()

    def test_syntax_error_reports_location(self, compiler, tmp_path):
        src = '''fn main(): i32 {
            let x = @@@
            return 0
        }'''
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0

    def test_type_error_has_location_info(self, compiler, tmp_path):
        src = 'fn foo(): i32 { return "hello" } fn main(): i32 { return 0 }'
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0
        assert "-->" in res.stderr


@pytest.mark.error
class TestEdgeCaseErrors:
    def test_empty_source(self, compiler, tmp_path):
        src = ''
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0

    def test_only_comments(self, compiler, tmp_path):
        src = '// just a comment\n/* block */'
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0

    def test_missing_closing_brace(self, compiler, tmp_path):
        src = 'fn main(): i32 { print("hello") return 0'
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0

    def test_missing_return_type(self, compiler, tmp_path):
        src = 'fn foo(): i32 { let x = 1 }'
        res, _ = compile_and_run(compiler, src, tmp_path)
        assert res.returncode != 0
