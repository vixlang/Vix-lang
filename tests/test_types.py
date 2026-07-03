import pytest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from helpers import compile_and_run, compile_source

@pytest.mark.feature
class TestTypeAnnotations:
    @pytest.mark.parametrize("type_name,default_val", [
        ("i32", "0"),
        ("i64", "0"),
        ("f32", "0"),
        ("f64", "0"),
        ("string", '""'),
    ])
    def test_variable_type_annotation(self, compiler, tmp_path, type_name, default_val):
        src = f'fn main(): i32 {{ let x: {type_name} = {default_val} print(0) return 0 }}'
        compile_res, run_res = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode == 0, f"Failed to compile with type {type_name}"

    def test_string_type_annotation(self, compiler, tmp_path):
        src = 'fn main(): i32 { let s: string = "hello" print(s) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "hello"

    def test_i32_type_annotation(self, compiler, tmp_path):
        src = 'fn main(): i32 { let x: i32 = 42 print(x) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"

    def test_i64_type_annotation(self, compiler, tmp_path):
        src = 'fn main(): i32 { let x: i64 = 100 print(x) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "100"

    def test_f64_type_annotation(self, compiler, tmp_path):
        src = 'fn main(): i32 { let x: f64 = 3.14 print(x) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "3.14" in run.stdout.strip()

    def test_bool_type(self, compiler, tmp_path):
        src = 'fn main(): i32 { let t = true let f = false print(t) print(f) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "true" in run.stdout
        assert "false" in run.stdout


# ============================================================
# 2. Type Inference
# ============================================================
@pytest.mark.feature
class TestTypeInference:
    def test_infer_i32_from_int_literal(self, compiler, tmp_path):
        src = 'fn main(): i32 { let x = 42 print(x) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"

    def test_infer_string_from_literal(self, compiler, tmp_path):
        src = 'fn main(): i32 { let s = "hello" print(s) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "hello"

    def test_infer_f64_from_float_literal(self, compiler, tmp_path):
        src = 'fn main(): i32 { let x = 3.14 print(x) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "3.14" in run.stdout.strip()

    def test_infer_from_expression(self, compiler, tmp_path):
        src = 'fn main(): i32 { let x = 10 + 20 print(x) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "30"

    def test_infer_from_function_call(self, compiler, tmp_path):
        src = '''fn get_val(): i32 { return 99 }
fn main(): i32 { let x = get_val() print(x) return 0 }'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "99"


# ============================================================
# 3. Function Type Signatures
# ============================================================
@pytest.mark.feature
class TestFunctionTypes:
    def test_void_return(self, compiler, tmp_path):
        src = '''fn greet() { print("hi") }
fn main(): i32 { greet() return 0 }'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "hi"

    def test_i32_return(self, compiler, tmp_path):
        src = '''fn add(a: i32, b: i32): i32 { return a + b }
fn main(): i32 { print(add(3, 4)) return 0 }'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "7"

    def test_string_param_and_return(self, compiler, tmp_path):
        src = '''fn greet(name: string) { print("hello ", name) }
fn main(): i32 { greet("world") return 0 }'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "hello" in run.stdout

    def test_pointer_param(self, compiler, tmp_path):
        src = '''fn deref(p: ptr): i32 { return @p }
fn main(): i32 { let x = 42 print(deref(ref x)) return 0 }'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"

    def test_f64_params(self, compiler, tmp_path):
        src = '''fn add(a: f64, b: f64): f64 { return a + b }
fn main(): i32 { print(add(1.5, 2.5)) return 0 }'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "4" in run.stdout.strip()

    def test_function_no_return_type(self, compiler, tmp_path):
        src = '''fn do_nothing(a: i32) { print(a) }
fn main(): i32 { do_nothing(42) print("ok") return 0 }'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "ok" in run.stdout

    def test_multiple_params(self, compiler, tmp_path):
        src = '''fn sum5(a: i32, b: i32, c: i32, d: i32, e: i32): i32 { return a + b + c + d + e }
fn main(): i32 { print(sum5(1, 2, 3, 4, 5)) return 0 }'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "15"


# ============================================================
# 4. Numeric Type Promotion
# ============================================================
@pytest.mark.feature
class TestTypePromotion:
    def test_i32_to_i64_promotion(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let a: i32 = 10
    let b: i64 = 20
    print(a + b)
    return 0
}'''
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode == 0

    def test_i32_literal_in_i64_context(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x: i64 = 42
    print(x)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"


# ============================================================
# 5. Pointer Types
# ============================================================
@pytest.mark.feature
class TestPointerTypes:
    def test_address_of_and_deref(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = 42
    let p = ref x
    print(@p)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"

    def test_pointer_mutation(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut x = 10
    let mut p = ref x
    @p = 20
    print(x)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "20"

    def test_swap_via_pointers(self, compiler, tmp_path):
        src = '''fn swap(mut a: ref i32, mut b: ref i32) {
    let temp = @a
    @a = @b
    @b = temp
}
fn main(): i32 {
    let a = 10
    let b = 20
    swap(ref a, ref b)
    print(a)
    print(b)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "20\n10", f"Expected '20\\n10', got '{run.stdout.strip()}'"

    def test_ptr_type_annotation(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = 99
    let p: ptr = ref x
    print(@p)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "99"


# ============================================================
# 6. Array Types
# ============================================================
@pytest.mark.feature
class TestArrayTypes:
    def test_fixed_array_type(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let arr: [i32 * 3] = [10, 20, 30]
    print(arr[0])
    print(arr[1])
    print(arr[2])
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "10" in run.stdout
        assert "20" in run.stdout
        assert "30" in run.stdout

    def test_dynamic_array_type(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let arr = [1, 2, 3, 4, 5]
    print(arr.length)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "5"

    def test_array_mutation(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut arr = [1, 2, 3]
    arr[1] = 99
    print(arr[1])
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "99"

    def test_string_array(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let arr = ["hello", "world"]
    print(arr[0])
    print(arr[1])
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "hello" in run.stdout
        assert "world" in run.stdout

    def test_array_param_copy_mutation_warning(self, compiler, tmp_path):
        src = '''fn add_one(points: [i32]): i32 {
    let len = points.length
    points.push(4)
    return len
}
fn main(): i32 {
    let points = [1, 2, 3]
    return add_one(points)
}'''
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode == 0
        assert "array parameter 'points' is modified after reading its length" in compile_res.stderr
        assert "array parameter 'points' is modified but changes are lost" in compile_res.stderr

    def test_unused_array_param_warning(self, compiler, tmp_path):
        src = '''fn process(points: [i32]): i32 {
    return 0
}
fn main(): i32 {
    let points = [1, 2, 3]
    return process(points)
}'''
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode == 0
        assert "array parameter 'points' is never used" in compile_res.stderr


# ============================================================
# 7. Struct Types
# ============================================================
@pytest.mark.feature
class TestStructTypes:
    def test_struct_definition_and_access(self, compiler, tmp_path):
        src = '''type Point = struct { x: i32, y: i32 }
fn main(): i32 {
    let p = Point { x: 10, y: 20 }
    print(p.x)
    print(p.y)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "10" in run.stdout
        assert "20" in run.stdout

    def test_struct_with_string_fields(self, compiler, tmp_path):
        src = '''type Person = struct { name: string, age: i32 }
fn main(): i32 {
    let p = Person { name: "Alice", age: 30 }
    print(p.name)
    print(p.age)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "Alice" in run.stdout
        assert "30" in run.stdout

    def test_struct_mutation(self, compiler, tmp_path):
        src = '''type Counter = struct { value: i32 }
fn main(): i32 {
    let mut c = Counter { value: 0 }
    c = Counter { value: 42 }
    print(c.value)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"

    def test_nested_struct(self, compiler, tmp_path):
        src = '''type Address = struct { city: string }
type Person = struct { name: string, addr: Address }
fn main(): i32 {
    let a = Address { city: "Beijing" }
    let p = Person { name: "Bob", addr: a }
    print(p.name)
    print(p.addr.city)
    return 0
}'''
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode == 0


# ============================================================
# 8. ADT Types (Algebraic Data Types)
# ============================================================
@pytest.mark.feature
class TestADTTypes:
    def test_option_type(self, compiler, tmp_path):
        src = '''fn find(x: i32): ?i32 {
    if (x > 0) { return Some(x) }
    return None
}
fn main(): i32 {
    let r = find(5)
    print(0)
    return 0
}'''
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode == 0

    def test_result_type(self, compiler, tmp_path):
        src = '''fn safe_div(a: i32, b: i32): Result:[i32, string] {
    if (b == 0) { return Err("division by zero") }
    return Ok(a / b)
}
fn main(): i32 {
    let r = safe_div(10, 2)
    print(0)
    return 0
}'''
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode == 0

    def test_custom_adt(self, compiler, tmp_path):
        src = '''type Color = Red | Green | Blue
fn main(): i32 {
    let c = Red
    print(0)
    return 0
}'''
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode == 0


# ============================================================
# 9. String Type
# ============================================================
@pytest.mark.feature
class TestStringType:
    def test_string_literal(self, compiler, tmp_path):
        src = 'fn main(): i32 { print("hello world") return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "hello world"

    def test_string_variable(self, compiler, tmp_path):
        src = 'fn main(): i32 { let s = "test" print(s) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "test"

    def test_string_annotation(self, compiler, tmp_path):
        src = 'fn main(): i32 { let s: string = "annotated" print(s) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "annotated"

    def test_string_length(self, compiler, tmp_path):
        src = 'fn main(): i32 { let s = "hello" print(s.length) return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "5"

    def test_string_index(self, compiler, tmp_path):
        src = 'fn main(): i32 { let s = "abc" print(s[0]) return 0 }'
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode == 0

    def test_string_in_print(self, compiler, tmp_path):
        src = 'fn main(): i32 { let name = "Vix" print("Hello, ", name, "!") return 0 }'
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "Hello" in run.stdout
        assert "Vix" in run.stdout


# ============================================================
# 10. Extern Function Types
# ============================================================
@pytest.mark.feature
class TestExternTypes:
    def test_extern_function_call(self, compiler, tmp_path):
        src = '''extern "C" { fn printf(format: ptr, ...): i32 }
fn main(): i32 {
    printf("extern works\\n")
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "extern works" in run.stdout

    def test_extern_with_varargs(self, compiler, tmp_path):
        src = '''extern "C" { fn printf(format: ptr, ...): i32 }
fn main(): i32 {
    printf("value: %d\\n", 42)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "42" in run.stdout


# ============================================================
# 11. Type Error Detection (should fail compilation)
# ============================================================
@pytest.mark.feature
class TestTypeErrors:
    def test_type_mismatch_let(self, compiler, tmp_path):
        src = 'fn main(): i32 { let x: i32 = "hello" return 0 }'
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode != 0

    def test_immutable_assignment(self, compiler, tmp_path):
        src = 'fn main(): i32 { let x = 10 x = 20 return 0 }'
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode != 0

    def test_undefined_variable(self, compiler, tmp_path):
        src = 'fn main(): i32 { print(x) return 0 }'
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode != 0

    def test_undefined_function(self, compiler, tmp_path):
        src = 'fn main(): i32 { foo() return 0 }'
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode != 0

    def test_no_main_function(self, compiler, tmp_path):
        src = 'fn foo() { print("hi") }'
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode != 0


# ============================================================
# 12. Compound Assignment Types
# ============================================================
@pytest.mark.feature
class TestCompoundAssignment:
    def test_plus_assign(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut x = 10
    x += 5
    print(x)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "15"

    def test_minus_assign(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut x = 10
    x -= 3
    print(x)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "7"

    def test_multiply_assign(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut x = 5
    x *= 3
    print(x)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "15"

    def test_divide_assign(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut x = 20
    x /= 4
    print(x)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "5"

    def test_modulo_assign(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut x = 17
    x %= 5
    print(x)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "2"


# ============================================================
# 13. Void Type
# ============================================================
@pytest.mark.feature
class TestVoidType:
    def test_void_function_as_statement(self, compiler, tmp_path):
        src = '''fn log(msg: string) { print(msg) }
fn main(): i32 {
    log("test")
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "test"

    def test_void_function_no_return(self, compiler, tmp_path):
        src = '''fn do_stuff() {
    let x = 1
    let y = 2
    print(x + y)
}
fn main(): i32 { do_stuff() return 0 }'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "3"


# ============================================================
# 14. Implicit Return Type (void when no return type specified)
# ============================================================
@pytest.mark.feature
class TestImplicitReturnType:
    def test_function_without_return_type(self, compiler, tmp_path):
        src = '''fn greet(name: string) {
    print("Hello, ", name)
}
fn main(): i32 {
    greet("World")
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "Hello" in run.stdout

    def test_function_body_ends_with_void(self, compiler, tmp_path):
        src = '''fn do_loop() {
    for (i in 0 .. 3) {
        print(i)
    }
}
fn main(): i32 {
    do_loop()
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "0" in run.stdout


# ============================================================
# 15. Type Compatibility in Expressions
# ============================================================
@pytest.mark.feature
class TestTypeCompatibility:
    def test_i32_arithmetic(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let a: i32 = 10
    let b: i32 = 3
    print(a + b)
    print(a - b)
    print(a * b)
    print(a / b)
    print(a % b)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "13" in run.stdout
        assert "7" in run.stdout
        assert "30" in run.stdout
        assert "3" in run.stdout
        assert "1" in run.stdout

    def test_comparison_returns_bool(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let a = 5
    let b = 10
    if (a < b) { print("less") }
    if (b > a) { print("greater") }
    if (a == a) { print("equal") }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "less" in run.stdout
        assert "greater" in run.stdout
        assert "equal" in run.stdout

    def test_string_comparison(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let a = "hello"
    let b = "hello"
    if (a == b) { print("match") }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "match"

    def test_logical_operators(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let a = true
    let b = false
    if (a and a) { print("and_ok") }
    if (a or b) { print("or_ok") }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "and_ok" in run.stdout
        assert "or_ok" in run.stdout


# ============================================================
# 16. Logical NOT Operator (!)
# ============================================================
@pytest.mark.feature
class TestLogicalNot:
    def test_not_true(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = true
    if (!x) { print("fail") } else { print("ok") }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "ok"

    def test_not_false(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = false
    if (!x) { print("ok") } else { print("fail") }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "ok"

    def test_not_comparison(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = 5
    if (!(x == 3)) { print("ok") }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "ok"

    def test_double_not(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = true
    if (!!x) { print("ok") }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "ok"


# ============================================================
# 17. Generic Struct via type keyword
# ============================================================
@pytest.mark.feature
class TestGenericStructType:
    def test_generic_struct_definition(self, compiler, tmp_path):
        src = '''type Box: [T] = struct {
    value: T
}
fn main(): i32 {
    let b: Box: [i32] = Box: [i32] { value: 42 }
    print(b.value)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"

    def test_generic_struct_with_string(self, compiler, tmp_path):
        src = '''type Wrapper: [T] = struct {
    data: T
}
fn main(): i32 {
    let w: Wrapper: [string] = Wrapper: [string] { data: "hello" }
    print(w.data)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "hello"


# ============================================================
# 18. ADT Constructors in Arrays
# ============================================================
@pytest.mark.feature
class TestADTInArrays:
    def test_option_in_variable(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let a = Some(1)
    let b = Some(2)
    let c = None
    match a {
        Some(v) -> { print(v) }
        None -> { print(0) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "1"

    def test_option_match_none(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = None
    match x {
        Some(v) -> { print(v) }
        None -> { print(0) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "0"


# ============================================================
# 19. Pointer Dereference Error Detection
# ============================================================
@pytest.mark.feature
class TestPointerErrors:
    def test_deref_non_pointer_fails(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x: i32 = 42
    let y = @x
    return 0
}'''
        compile_res, _ = compile_and_run(compiler, src, tmp_path)
        assert compile_res.returncode != 0

    def test_pointer_deref_works(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = 42
    let p = ref x
    print(@p)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"


# ============================================================
# 20. Edge Cases and Boundary Tests
# ============================================================
@pytest.mark.feature
class TestEdgeCases:
    def test_nested_if_expressions(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = 10
    if (x > 5) {
        if (x > 8) { print("big") }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "big"

    def test_function_as_expression(self, compiler, tmp_path):
        src = '''fn add(a: i32, b: i32): i32 { return a + b }
fn main(): i32 {
    let x = add(3, 4)
    print(x)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "7"

    def test_empty_function_body(self, compiler, tmp_path):
        src = '''fn noop() { let _x = 0 }
fn main(): i32 { noop() print("ok") return 0 }'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "ok"

    def test_chained_member_access(self, compiler, tmp_path):
        src = '''type Inner = struct { val: i32 }
type Outer = struct { inner: Inner }
fn main(): i32 {
    let o = Outer { inner: Inner { val: 99 } }
    print(o.inner.val)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "99"

    def test_let_with_type_and_no_init(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut x: i32 = 0
    x = 42
    print(x)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"

    def test_for_loop_array_iteration(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let arr = [10, 20, 30]
    for (i in arr) {
        print(i)
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert "10" in run.stdout
        assert "20" in run.stdout
        assert "30" in run.stdout


# ============================================================
# 21. ADT Match Payload Type (Err should return E, not T)
# ============================================================
@pytest.mark.feature
class TestADTMatchPayload:
    def test_result_err_payload_type(self, compiler, tmp_path):
        src = '''type Result:[T, E] = Ok(T) | Err(E)
fn main(): i32 {
    let bad = Err("division by zero") : Result:[i32, string]
    match bad {
        Ok(v) -> { print(v) }
        Err(e) -> { print(e) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "division by zero"

    def test_result_ok_payload_type(self, compiler, tmp_path):
        src = '''type Result:[T, E] = Ok(T) | Err(E)
fn main(): i32 {
    let good = Ok(42) : Result:[i32, string]
    match good {
        Ok(v) -> { print(v) }
        Err(e) -> { print(e) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"

    def test_result_both_arms(self, compiler, tmp_path):
        src = '''type Result:[T, E] = Ok(T) | Err(E)
fn main(): i32 {
    let r1 = Ok(5) : Result:[i32, string]
    let r2 = Err("cannot divide by zero") : Result:[i32, string]
    match r1 {
        Ok(v) -> { print(v) }
        Err(e) -> { print(e) }
    }
    match r2 {
        Ok(v) -> { print(v) }
        Err(e) -> { print(e) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        lines = run.stdout.strip().split("\n")
        assert lines[0] == "5"
        assert lines[1] == "cannot divide by zero"

    def test_option_from_function(self, compiler, tmp_path):
        src = '''fn first_or_none(list: [string]): ?string {
    if (list.length > 0) {
        return Some(list[0])
    }
    return None
}
fn main(): i32 {
    let names = ["i32", "f64"]
    let empty = []
    let a = first_or_none(names)
    let b = first_or_none(empty)
    match a {
        Some(v) -> { print(v) }
        None -> { print("fail") }
    }
    match b {
        Some(v) -> { print(v) }
        None -> { print("empty is empty") }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        lines = run.stdout.strip().split("\n")
        assert lines[0] == "i32"
        assert lines[1] == "empty is empty"

    def test_adt_tag_annotation_syntax(self, compiler, tmp_path):
        src = '''type Result:[T, E] = Ok(T) | Err(E)
fn main(): i32 {
    let a = Ok(42) : Result:[i32, string]
    let b = Err("oops") : Result:[i32, string]
    match a {
        Ok(v) -> { print(v) }
        Err(e) -> { print(e) }
    }
    match b {
        Ok(v) -> { print(v) }
        Err(e) -> { print(e) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        lines = run.stdout.strip().split("\n")
        assert lines[0] == "42"
        assert lines[1] == "oops"


# ============================================================
# 22. Unit Type ()
# ============================================================
@pytest.mark.feature
class TestUnitType:
    def test_unit_type_in_generics(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = ()
    print("ok")
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "ok"


# ============================================================
# 23. Logical NOT in match context
# ============================================================
@pytest.mark.feature
class TestNotInContext:
    def test_not_in_while(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut x = 3
    while (!(x == 0)) {
        print(x)
        x -= 1
    }
    print(0)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "3\n2\n1\n0", f"Expected '3\\n2\\n1\\n0', got '{run.stdout.strip()}'"

    def test_not_with_and_or(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let a = true
    let b = false
    if (!a and b) { print("fail") }
    if (!(!a or b)) { print("ok") }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "ok"


# ============================================================
# 24. ADT Constructor Stability (no type annotation needed)
# ============================================================
@pytest.mark.feature
class TestADTConstructorStability:
    def test_ok_without_annotation(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = Ok(100)
    match x {
        Ok(v) -> { print(v) }
        Err(e) -> { print(-1) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "100"

    def test_err_without_annotation(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = Err("fail")
    match x {
        Ok(v) -> { print(v) }
        Err(e) -> { print(e) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "fail"

    def test_some_without_annotation(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = Some(42)
    match x {
        Some(v) -> { print(v) }
        None -> { print(0) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"

    def test_none_without_annotation(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = None
    match x {
        Some(v) -> { print(v) }
        None -> { print(0) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "0"

    def test_ok_with_annotation(self, compiler, tmp_path):
        src = '''type Result:[T, E] = Ok(T) | Err(E)
fn main(): i32 {
    let x = Ok(42) : Result:[i32, string]
    match x {
        Ok(v) -> { print(v) }
        Err(e) -> { print(e) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "42"

    def test_err_with_annotation(self, compiler, tmp_path):
        src = '''type Result:[T, E] = Ok(T) | Err(E)
fn main(): i32 {
    let x = Err("oops") : Result:[i32, string]
    match x {
        Ok(v) -> { print(v) }
        Err(e) -> { print(e) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "oops"

    def test_adt_in_function_return(self, compiler, tmp_path):
        src = '''fn safe_div(a: i32, b: i32) {
    if (b == 0) {
        let r = Err("division by zero")
        match r {
            Ok(v) -> { print(v) }
            Err(e) -> { print(e) }
        }
    } else {
        let r = Ok(a / b)
        match r {
            Ok(v) -> { print(v) }
            Err(e) -> { print(e) }
        }
    }
}
fn main(): i32 {
    safe_div(10, 2)
    safe_div(10, 0)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        lines = [l.strip() for l in run.stdout.strip().splitlines() if l.strip()]
        assert lines == ["5", "division by zero"]

    def test_multiple_adt_matches(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let a = Some(1)
    let b = Some(2)
    let c = None
    match a { Some(v) -> { print(v) } None -> { print(0) } }
    match b { Some(v) -> { print(v) } None -> { print(0) } }
    match c { Some(v) -> { print(v) } None -> { print(0) } }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        lines = [l.strip() for l in run.stdout.strip().splitlines() if l.strip()]
        assert lines == ["1", "2", "0"]


# ============================================================
# 25. Compiler Robustness Tests
# ============================================================
@pytest.mark.feature
class TestCompilerRobustness:
    def test_nested_match(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let x = 1
    let y = 2
    match x {
        0 -> { print(0) }
        1 -> {
            match y {
                0 -> { print(10) }
                1 -> { print(11) }
                2 -> { print(12) }
                _ -> { print(19) }
            }
        }
        _ -> { print(99) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "12"

    def test_deeply_nested_if(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    if (1) {
        if (1) {
            if (1) {
                if (1) {
                    if (1) {
                        print("deep")
                    }
                }
            }
        }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "deep"

    def test_many_local_variables(self, compiler, tmp_path):
        lines = ['fn main(): i32 {']
        for i in range(50):
            lines.append(f'    let v{i} = {i}')
        total = " + ".join(f"v{i}" for i in range(50))
        lines.append(f'    print({total})')
        lines.append('    return 0\n}')
        src = '\n'.join(lines)
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        expected = sum(range(50))
        assert run.stdout.strip() == str(expected)

    def test_recursive_function(self, compiler, tmp_path):
        src = '''fn fib(n: i32): i32 {
    if (n <= 1) { return n }
    return fib(n - 1) + fib(n - 2)
}
fn main(): i32 {
    print(fib(10))
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "55"

    def test_struct_with_match(self, compiler, tmp_path):
        src = '''struct Point { x: i32, y: i32 }
fn classify(p: Point) {
    if (p.x > p.y) { print("x-greater") }
    elif (p.x < p.y) { print("y-greater") }
    else { print("equal") }
}
fn main(): i32 {
    classify(Point { x: 5, y: 3 })
    classify(Point { x: 2, y: 7 })
    classify(Point { x: 4, y: 4 })
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        lines = [l.strip() for l in run.stdout.strip().splitlines() if l.strip()]
        assert lines == ["x-greater", "y-greater", "equal"]

    def test_for_loop_with_break(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut sum = 0
    for (i in 0 .. 100) {
        if (i == 10) { break }
        sum += i
    }
    print(sum)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "45"

    def test_while_with_continue(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut i = 0
    let mut sum = 0
    while (i < 10) {
        i += 1
        if (i % 2 == 0) { continue }
        sum += i
    }
    print(sum)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "25"

    def test_string_operations(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let a = "hello"
    let b = "world"
    if (a != b) { print("different") }
    if (a == "hello") { print("match") }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        lines = [l.strip() for l in run.stdout.strip().splitlines() if l.strip()]
        assert lines == ["different", "match"]

    def test_pointer_operations(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut x = 10
    let mut p = ref x
    @p = 20
    print(x)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "20"

    def test_compound_assignment(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    let mut x = 0
    x += 10
    x -= 3
    x *= 2
    print(x)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        assert run.stdout.strip() == "14"

    def test_power_operator(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    print(2 ** 10)
    print(3 ** 3)
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        lines = [l.strip() for l in run.stdout.strip().splitlines() if l.strip()]
        assert lines == ["1024", "27"]

    def test_fizzbuzz(self, compiler, tmp_path):
        src = '''fn main(): i32 {
    for (i in 1 .. 16) {
        if (i % 15 == 0) { print("FizzBuzz") }
        elif (i % 3 == 0) { print("Fizz") }
        elif (i % 5 == 0) { print("Buzz") }
        else { print(i) }
    }
    return 0
}'''
        _, run = compile_and_run(compiler, src, tmp_path)
        assert run is not None
        lines = [l.strip() for l in run.stdout.strip().splitlines() if l.strip()]
        expected = ["1","2","Fizz","4","Buzz","Fizz","7","8","Fizz","Buzz","11","Fizz","13","14","FizzBuzz"]
        assert lines == expected
