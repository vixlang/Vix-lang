#include "binaryen-c.h"
#include <cstdio>

int main() {
    BinaryenModuleRef mod = BinaryenModuleCreate();
    if (!mod) {
        printf("FAIL: null module\n");
        return 1;
    }
    BinaryenModuleAllocateAndWriteResult r = BinaryenModuleAllocateAndWrite(mod, nullptr);
    if (!r.binary) {
        printf("FAIL: null binary\n");
        return 1;
    }
    printf("PASS: binaryen works, %zu bytes\n", r.binaryBytes);
    free(r.binary);
    BinaryenModuleDispose(mod);
    return 0;
}
