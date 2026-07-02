#include <emscripten.h>
#include "../src/libvixc_frontend.h"
#include "../src/compiler/WasmCodegen.h"
#include <string>
#include <vector>
#include <cstring>

extern "C" {

EMSCRIPTEN_KEEPALIVE
int compile_vix(const char *source, char **out_wasm_bytes, int *out_wasm_len, char **out_error) {
    EM_ASM({ console.log('[vixc] compile_vix entered, source length:', $0); }, source ? strlen(source) : 0);

    CompileResult cr = vixc_compile_string(source);
    EM_ASM({ console.log('[vixc] compile done, errors:', $0, 'root:', $1); }, cr.error_count, !!cr.root);

    if (cr.error_count > 0 || !cr.root) {
        const char *err = vixc_get_last_error();
        if (!err || !err[0]) {
            err = "compile failed";
        }
        EM_ASM({ console.log('[vixc] compile error:', UTF8ToString($0)); }, err);
        *out_error = strdup(err);
        return 0;
    }

    WasmCodegen cg;
    std::vector<uint8_t> wasm_bytes;
    std::string error;
    EM_ASM({ console.log('[vixc] starting WasmCodegen emit'); });
    if (!cg.emit(cr.root, wasm_bytes, error)) {
        EM_ASM({ console.log('[vixc] WasmCodegen failed:', UTF8ToString($0)); }, error.c_str());
        *out_error = strdup(error.c_str());
        vixc_free_result(&cr);
        return 0;
    }

    *out_wasm_len = wasm_bytes.size();
    *out_wasm_bytes = (char*)malloc(wasm_bytes.size());
    memcpy(*out_wasm_bytes, wasm_bytes.data(), wasm_bytes.size());

    vixc_free_result(&cr);
    EM_ASM({ console.log('[vixc] compile_vix success, wasm size:', $0); }, *out_wasm_len);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void free_wasm_result(char *bytes, char *error) {
    if (bytes) free(bytes);
    if (error) free(error);
}

}
