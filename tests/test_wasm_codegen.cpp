#include "binaryen-c.h"
#include "WasmCodegen.h"
#include "libvixc_frontend.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

static int tests_passed = 0;
static int tests_failed = 0;

static std::string read_fixture(const char *path) {
    FILE *fp = fopen(path, "rb");
    assert(fp != nullptr);
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::string buf;
    buf.resize((size_t)len);
    size_t n = fread(buf.data(), 1, (size_t)len, fp);
    fclose(fp);
    assert(n == (size_t)len);
    return buf;
}

static void expect_emit_success(const char *fixture_path) {
    std::string source = read_fixture(fixture_path);
    CompileResult cr = vixc_compile_string(source.c_str());
    assert(cr.error_count == 0);
    assert(cr.root != nullptr);

    WasmCodegen cg;
    std::vector<uint8_t> wasm_bytes;
    std::string error;
    bool ok = cg.emit(cr.root, wasm_bytes, error);
    assert(ok);
    assert(!wasm_bytes.empty());
    assert(wasm_bytes[0] == 0x00);
    assert(wasm_bytes[1] == 0x61);
    assert(wasm_bytes[2] == 0x73);
    assert(wasm_bytes[3] == 0x6d);

    vixc_free_result(&cr);
}

void test_binaryen_basic() {
    fprintf(stderr, "test_binaryen_basic: creating module...\n"); fflush(stderr);
    BinaryenModuleRef mod = BinaryenModuleCreate();
    fprintf(stderr, "test_binaryen_basic: writing...\n"); fflush(stderr);
    BinaryenModuleAllocateAndWriteResult r = BinaryenModuleAllocateAndWrite(mod, nullptr);
    fprintf(stderr, "test_binaryen_basic: binary=%p bytes=%zu\n", r.binary, r.binaryBytes); fflush(stderr);
    if (!r.binary) {
        fprintf(stderr, "FAIL: null binary from BinaryenModuleAllocateAndWrite\n"); fflush(stderr);
        return;
    }
    assert(r.binaryBytes > 4);
    uint8_t *b = (uint8_t*)r.binary;
    fprintf(stderr, "  magic: %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3]); fflush(stderr);
    assert(b[0] == 0x00);
    assert(b[1] == 0x61);
    assert(b[2] == 0x73);
    assert(b[3] == 0x6d);
    free(r.binary);
    BinaryenModuleDispose(mod);
    fprintf(stderr, "PASS: test_binaryen_basic (%zu bytes)\n", r.binaryBytes); fflush(stderr);
    tests_passed++;
}

void test_compile_to_wasm() {
    const char *source = "fn add(a: i32, b: i32): i32 {\n"
                         "    return a + b\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return add(1, 2)\n"
                         "}\n";

    fprintf(stderr, "test_compile_to_wasm: compiling source...\n"); fflush(stderr);
    CompileResult cr = vixc_compile_string(source);
    fprintf(stderr, "  error_count=%d root=%p\n", cr.error_count, cr.root); fflush(stderr);
    if (cr.error_count != 0 || !cr.root) {
        fprintf(stderr, "FAIL: frontend compilation failed\n"); fflush(stderr);
        tests_failed++;
        return;
    }

    WasmCodegen cg;
    std::vector<uint8_t> wasm_bytes;
    std::string error;
    fprintf(stderr, "test_compile_to_wasm: emitting WASM...\n"); fflush(stderr);
    bool ok = cg.emit(cr.root, wasm_bytes, error);
    fprintf(stderr, "  ok=%d bytes=%zu error=[%s]\n", ok, wasm_bytes.size(), error.c_str()); fflush(stderr);

    if (!ok) {
        fprintf(stderr, "FAIL: emit() returned false\n"); fflush(stderr);
        tests_failed++;
        vixc_free_result(&cr);
        return;
    }
    assert(!wasm_bytes.empty());
    assert(wasm_bytes[0] == 0x00);
    assert(wasm_bytes[1] == 0x61);
    assert(wasm_bytes[2] == 0x73);
    assert(wasm_bytes[3] == 0x6d);

    vixc_free_result(&cr);
    fprintf(stderr, "PASS: test_compile_to_wasm (%zu bytes)\n", wasm_bytes.size()); fflush(stderr);
    tests_passed++;
}

void test_string_literal_embedded_in_wasm() {
    const char *source = "fn main(): i32 {\n"
                         "    print(\"Hello, Vix Playground!\")\n"
                         "    return 0\n"
                         "}\n";
    const char *needle = "Hello, Vix Playground!";

    fprintf(stderr, "test_string_literal_embedded_in_wasm: compiling source...\n"); fflush(stderr);
    CompileResult cr = vixc_compile_string(source);
    fprintf(stderr, "  error_count=%d root=%p\n", cr.error_count, cr.root); fflush(stderr);
    if (cr.error_count != 0 || !cr.root) {
        fprintf(stderr, "FAIL: frontend compilation failed\n"); fflush(stderr);
        tests_failed++;
        return;
    }

    WasmCodegen cg;
    std::vector<uint8_t> wasm_bytes;
    std::string error;
    fprintf(stderr, "test_string_literal_embedded_in_wasm: emitting WASM...\n"); fflush(stderr);
    bool ok = cg.emit(cr.root, wasm_bytes, error);
    fprintf(stderr, "  ok=%d bytes=%zu error=[%s]\n", ok, wasm_bytes.size(), error.c_str()); fflush(stderr);

    if (!ok) {
        fprintf(stderr, "FAIL: emit() returned false\n"); fflush(stderr);
        tests_failed++;
        vixc_free_result(&cr);
        return;
    }

    bool found = false;
    size_t needle_len = strlen(needle) + 1;
    for (size_t i = 0; i + needle_len <= wasm_bytes.size(); i++) {
        if (memcmp(wasm_bytes.data() + i, needle, needle_len) == 0) {
            found = true;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "FAIL: string literal not embedded in wasm data segments\n"); fflush(stderr);
        tests_failed++;
        vixc_free_result(&cr);
        return;
    }

    vixc_free_result(&cr);
    fprintf(stderr, "PASS: test_string_literal_embedded_in_wasm (%zu bytes)\n", wasm_bytes.size()); fflush(stderr);
    tests_passed++;
}

void test_parse_error_reports_message() {
    const char *source = "fn main(): i32 {\n"
                         "    if 1 { return 1 }\n"
                         "    return 0\n"
                         "}\n";

    fprintf(stderr, "test_parse_error_reports_message: compiling source...\n"); fflush(stderr);
    CompileResult cr = vixc_compile_string(source);
    const char *err = vixc_get_last_error();
    fprintf(stderr, "  error_count=%d root=%p err=[%s]\n", cr.error_count, cr.root, err ? err : ""); fflush(stderr);

    if (cr.error_count == 0 || cr.root != nullptr) {
        fprintf(stderr, "FAIL: invalid source unexpectedly compiled\n"); fflush(stderr);
        tests_failed++;
        vixc_free_result(&cr);
        return;
    }

    if (!err || err[0] == '\0') {
        fprintf(stderr, "FAIL: parse error message is empty\n"); fflush(stderr);
        tests_failed++;
        return;
    }

    fprintf(stderr, "PASS: test_parse_error_reports_message\n"); fflush(stderr);
    tests_passed++;
}

void test_fixture_hello() {
    expect_emit_success("tests/fixtures/wasm_core/hello.vix");
    fprintf(stderr, "PASS: test_fixture_hello\n"); fflush(stderr);
    tests_passed++;
}

void test_compile_function_params_and_locals() {
    const char *source =
        "fn add(a: i32, b: i32): i32 {\n"
        "    let c = a + b\n"
        "    return c\n"
        "}\n"
        "fn main(): i32 {\n"
        "    print(add(4, 5))\n"
        "    return 0\n"
        "}\n";

    CompileResult cr = vixc_compile_string(source);
    assert(cr.error_count == 0);
    assert(cr.root != nullptr);

    WasmCodegen cg;
    std::vector<uint8_t> wasm_bytes;
    std::string error;
    bool ok = cg.emit(cr.root, wasm_bytes, error);
    assert(ok);
    assert(!wasm_bytes.empty());
    vixc_free_result(&cr);
    fprintf(stderr, "PASS: test_compile_function_params_and_locals\n"); fflush(stderr);
    tests_passed++;
}

int main() {
    fprintf(stderr, "=== WASM Codegen Test ===\n");
    test_binaryen_basic();
    test_compile_to_wasm();
    test_string_literal_embedded_in_wasm();
    test_parse_error_reports_message();
    test_fixture_hello();
    test_compile_function_params_and_locals();
    fprintf(stderr, "\n%d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
