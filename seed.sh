#!/bin/sh
set -eu

cd "$(dirname "$0")"

if [ ! -f seed/vixc.ll ]; then
    echo "restore-seed: missing seed/vixc.ll" >&2
    exit 1
fi

mkdir -p build runtime

gcc -c src/helper.c -o build/helper.o $(llvm-config --cflags) -Wno-deprecated-declarations
gcc -c src/runtime.c -o runtime/runtime.o
gcc -c lib/api.c -o build/api.o
clang++ -c lib/llvm/Llc.cpp -o build/Llc.o $(llvm-config --cxxflags) -Wno-deprecated-declarations
clang++ -c lib/llvm/Linker.cpp -o build/Linker.o $(llvm-config --cxxflags) -Wno-deprecated-declarations
clang++ -c lib/llvm/Passes.cpp -o build/Passes.o $(llvm-config --cxxflags) -Wno-deprecated-declarations

clang++ -fuse-ld=lld -o vixc seed/vixc.ll build/helper.o runtime/runtime.o build/api.o build/Llc.o build/Linker.o build/Passes.o $(llvm-config --ldflags --libs all) -llldELF -llldCommon -lz -lzstd -lz -lzstd
echo "build vixc ok!"
ulimit -s 65536 && ./vixc src/main.vix --check
mv vixc seed/
