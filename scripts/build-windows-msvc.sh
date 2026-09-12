#!/bin/sh
# Build the Vix bootstrap compiler on Windows with the MSVC toolchain.
#
# Usage (from Git Bash):
#   scripts/build-windows-msvc.sh              # build build/vixc.exe (seed)
#   scripts/build-windows-msvc.sh --bootstrap  # also self-host two stages
#   scripts/build-windows-msvc.sh --smoke      # also compile+run a hello world
#
# Requirements:
#   - LLVM release tarball for x86_64-pc-windows-msvc (clang, llvm-config,
#     lld, static LLVM/lld libraries)
#   - Visual Studio with the MSVC toolset (cl.exe) installed
#   - Windows SDK installed
# Adjust the three *_ROOT variables below to your machine.
set -eu

LLVM_ROOT='E:\Desktop\clang+llvm-21.1.0-x86_64-pc-windows-msvc.tar\clang+llvm-21.1.0-x86_64-pc-windows-msvc'
MSVC_ROOT='E:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\MSVC\14.51.36231'
SDK_ROOT='D:\Windows Kits\10'
SDK_VERSION=10.0.26100.0

DO_BOOTSTRAP=0
DO_SMOKE=0
for arg in "$@"; do
  case "$arg" in
    --bootstrap) DO_BOOTSTRAP=1 ;;
    --smoke) DO_SMOKE=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

LLVM_POSIX=$(cygpath -u "$LLVM_ROOT")
MSVC_POSIX=$(cygpath -u "$MSVC_ROOT")
CLANG="$LLVM_POSIX/bin/clang.exe"
CLANGXX="$LLVM_POSIX/bin/clang++.exe"

# clang locates the MSVC install by finding cl.exe on PATH; LIB lets the
# in-process lld-link resolve the MSVC CRT, Windows SDK and LLVM import
# libraries when the compiler links user programs.
export PATH="$MSVC_POSIX/bin/Hostx64/x64:$PATH"
export LIB="$LLVM_ROOT\\lib;$MSVC_ROOT\\lib\\x64;$SDK_ROOT\\Lib\\$SDK_VERSION\\ucrt\\x64;$SDK_ROOT\\Lib\\$SDK_VERSION\\um\\x64"

LLVM_DEFINES='-D_CRT_SECURE_NO_DEPRECATE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -DUNICODE -D_UNICODE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS'

cd "$(dirname "$0")/.."
mkdir -p build runtime

# Full paths to every static LLVM library; drop the manifest library since
# its libxml2 dependency is not shipped in the release tarball.
ALL_LIBS=$("$LLVM_POSIX/bin/llvm-config.exe" --libs all | tr ' ' '\n' | grep -v WindowsManifest)
SYS_LIBS=$("$LLVM_POSIX/bin/llvm-config.exe" --system-libs | tr ' ' '\n' | grep -v libxml2s | sed 's/^/-Wl,/')
LLD_LIBS="$LLVM_ROOT\\lib\\lldCommon.lib $LLVM_ROOT\\lib\\lldELF.lib $LLVM_ROOT\\lib\\lldCOFF.lib"

link_vixc() { # link_vixc <output.exe> <compiler .obj>
  "$CLANGXX" -o "$1" \
    "$2" build/helper.obj runtime/runtime.o build/api.obj \
    build/Llc.obj build/Linker.obj build/Passes.obj \
    $ALL_LIBS $LLD_LIBS $SYS_LIBS -Wl,/STACK:16777216
}

echo '[1/6] C bridge objects'
"$CLANG" -c src/helper.c   -o build/helper.obj -I"$LLVM_ROOT\\include"
"$CLANG" -c src/runtime.c  -o runtime/runtime.o
"$CLANG" -c lib/api.c      -o build/api.obj -I"$LLVM_ROOT\\include"

echo '[2/6] LLVM C++ bridge objects'
for f in Llc Linker Passes; do
  "$CLANGXX" -c "lib/llvm/$f.cpp" -o "build/$f.obj" \
    -I"$LLVM_ROOT\\include" -std=c++17 -fno-exceptions -fno-rtti \
    $LLVM_DEFINES
done

echo '[3/6] seed compiler object (from seed/vixc.ll)'
"$CLANG" --target=x86_64-pc-windows-msvc -c seed/vixc.ll -o build/vixc-seed.obj

echo '[4/6] linking build/vixc.exe (seed)'
link_vixc build/vixc.exe build/vixc-seed.obj
./build/vixc.exe --version
./build/vixc.exe src/main.vix --check && echo 'seed ok'

if [ "$DO_BOOTSTRAP" = 1 ]; then
  echo '[5/6] self-hosting stage 1: seed compiles src/main.vix'
  ./build/vixc.exe src/main.vix -obj -o build/vixc-bootstrap.obj
  link_vixc build/vixc-bootstrap.exe build/vixc-bootstrap.obj
  ./build/vixc-bootstrap.exe src/main.vix --check && echo 'bootstrap ok'

  echo '[6/6] self-hosting stage 2: bootstrapped compiler compiles itself'
  ./build/vixc-bootstrap.exe src/main.vix -obj -o build/vixc.obj
  link_vixc build/vixc-full.exe build/vixc.obj
  ./build/vixc-full.exe src/main.vix --check && echo 'self-hosted ok'
fi

if [ "$DO_SMOKE" = 1 ]; then
  echo '[smoke] compiling and running a hello world'
  cat > build/smoke.vix <<'EOF'
extern "C"
{
    fn puts(s: string): i32
}

fn main(): i32
{
    puts("Hello from Vix on Windows!")
    return 0
}
EOF
  ./build/vixc.exe build/smoke.vix -o build/smoke.exe
  ./build/smoke.exe
fi
