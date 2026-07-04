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

clang -o vixc seed/vixc.ll build/helper.o runtime/runtime.o $(llvm-config --ldflags --libs)
echo "build vixc ok!"
./vixc src/main.vix --check
mv vixc seed/

