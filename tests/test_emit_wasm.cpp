#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

#define VIXC_FRONTEND_ONLY
#include "binaryen-c.h"
#include "WasmCodegen.h"

extern "C" {
#include "libvixc_frontend.h"
}

int main(int argc, char **argv) {
    const char *source = "fn main(): i32 {\n"
        "    let arr = [1, 2, 3, 4, 5]\n"
        "    let p = ref arr[0]\n"
        "    let second = @(p + 1)\n"
        "    print(second)\n"
        "    return 0\n"
        "}\n";

    if (argc > 1) source = argv[1];

    CompileResult cr = vixc_compile_string(source);
    if (cr.error_count != 0 || !cr.root) {
        fprintf(stderr, "Frontend error\n");
        return 1;
    }

    WasmCodegen cg;
    std::vector<uint8_t> wasm_bytes;
    std::string error;
    if (!cg.emit(cr.root, wasm_bytes, error)) {
        fprintf(stderr, "Codegen error: %s\n", error.c_str());
        return 1;
    }

    vixc_free_result(&cr);

    // Write WASM to stdout or file
    if (argc > 2) {
        FILE *fp = fopen(argv[2], "wb");
        if (fp) {
            fwrite(wasm_bytes.data(), 1, wasm_bytes.size(), fp);
            fclose(fp);
        }
    } else {
        fwrite(wasm_bytes.data(), 1, wasm_bytes.size(), stdout);
    }

    return 0;
}
