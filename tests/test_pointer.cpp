#include "binaryen-c.h"
#include "WasmCodegen.h"
#include "libvixc_frontend.h"
#include <cstdio>
#include <vector>
#include <string>

int main() {
    const char *source =
        "fn main(): i32 {\n"
        "    let x = 10\n"
        "    let mut ptr = ref x\n"
        "    @ptr = 20\n"
        "    let arr = [1, 2, 3, 4, 5]\n"
        "    let p = ref arr[0]\n"
        "    let second = @(p + 1)\n"
        "    print(second)\n"
        "    return 0\n"
        "}\n";

    CompileResult cr = vixc_compile_string(source);
    if (cr.error_count != 0 || !cr.root) {
        fprintf(stderr, "FAIL: frontend compilation failed\n");
        return 1;
    }
    fprintf(stderr, "frontend OK, root=%p\n", cr.root);

    WasmCodegen cg;
    std::vector<uint8_t> wasm_bytes;
    std::string error;
    bool ok = cg.emit(cr.root, wasm_bytes, error);
    if (!ok) {
        fprintf(stderr, "FAIL: emit failed: %s\n", error.c_str());
        vixc_free_result(&cr);
        return 1;
    }
    fprintf(stderr, "PASS: emit OK, %zu bytes\n", wasm_bytes.size());

    // Dump wasm to file for inspection
    FILE *fp = fopen("pointer_test.wasm", "wb");
    if (fp) {
        fwrite(wasm_bytes.data(), 1, wasm_bytes.size(), fp);
        fclose(fp);
        fprintf(stderr, "Dumped to pointer_test.wasm\n");
    }

    vixc_free_result(&cr);
    fprintf(stderr, "ALL PASSED\n");
    return 0;
}
