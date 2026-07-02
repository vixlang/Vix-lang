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
    if (cr.error_count != 0 || !cr.root) return 1;

    WasmCodegen cg;
    std::vector<uint8_t> wasm_bytes;
    std::string error;
    if (!cg.emit(cr.root, wasm_bytes, error)) return 1;

    FILE *fp = fopen("pointer_test.wasm", "wb");
    if (fp) {
        fwrite(wasm_bytes.data(), 1, wasm_bytes.size(), fp);
        fclose(fp);
    }

    vixc_free_result(&cr);
    return 0;
}
