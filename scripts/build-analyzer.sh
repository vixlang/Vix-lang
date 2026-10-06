#!/bin/sh
#
# Build the vix-analyzer language server into build/vix-analyzer.
#
# Why this script exists
# ----------------------
# vixc's embedded linker only implements the ELF and COFF drivers, so on macOS
# "vixc prog.vix -o prog" cannot link. The LLVM backend itself is fine: it emits
# a real Mach-O object. This script therefore emits an object with the LLVM
# backend and links it with the platform toolchain.
#
# The compiler also needs the Option-aggregate fix that is in src/codegen.vix
# (Some(struct) used to emit "add <struct>, 1"). A patched vixc is built first
# and used to compile the analyzer.
#
# Toolchain layout on this machine
# --------------------------------
#   LLVM 21 headers + llvm-config : homebrew llvm@21
#   lld headers + lld/LLVM libs   : conda (miniconda3)
# The support objects, the patched vixc and the analyzer must all agree on LLVM
# 21, so the support objects are rebuilt here rather than reused.
#
# Override any of these with environment variables when the layout differs.

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CLANG=${CLANG:-/usr/bin/clang}
CLANGXX=${CLANGXX:-/usr/bin/clang++}
LLVM_CONFIG=${LLVM_CONFIG:-llvm-config}
LLD_INCLUDE=${LLD_INCLUDE:-/Users/Admin/miniconda3/include}
LLD_LIBDIR=${LLD_LIBDIR:-/Users/Admin/miniconda3/lib}
LLVM_LINK_LIB=${LLVM_LINK_LIB:-LLVM-21}
GC_LIBDIR=${GC_LIBDIR:-$(pkg-config --libs-only-L bdw-gc 2>/dev/null | sed 's/^-L//')}

if [ ! -d "$LLD_INCLUDE/lld" ]; then
  echo "build-analyzer: no lld headers under $LLD_INCLUDE (set LLD_INCLUDE)" >&2
  exit 1
fi
if [ -z "$GC_LIBDIR" ]; then
  echo "build-analyzer: cannot locate the bdw-gc library directory (set GC_LIBDIR)" >&2
  exit 1
fi

LLVM_CXXFLAGS=$($LLVM_CONFIG --cxxflags)
LLVM_CFLAGS=$($LLVM_CONFIG --cflags)
LLVM_LIBDIR=$($LLVM_CONFIG --libdir)

echo "== support objects (LLVM 21) =="
$CLANG -c lib/api.c -o build/api.o
$CLANG -c src/helper.c -o build/helper.o $LLVM_CFLAGS -Wno-deprecated-declarations
$CLANG -c src/runtime.c -o runtime/runtime.o
$CLANG -c src/compiler_gc.c -o build/compiler_gc.o
$CLANGXX -c lib/llvm/Llc.cpp -o build/Llc.o $LLVM_CXXFLAGS -Wno-deprecated-declarations
$CLANGXX -c lib/llvm/Passes.cpp -o build/Passes.o $LLVM_CXXFLAGS -Wno-deprecated-declarations
$CLANGXX -c lib/llvm/Linker.cpp -o build/Linker.o $LLVM_CXXFLAGS -I"$LLD_INCLUDE" -Wno-deprecated-declarations

COMPILER_SUPPORT="build/helper.o runtime/runtime.o build/api.o build/Llc.o build/Linker.o build/Passes.o build/compiler_gc.o"

echo "== bootstrap compiler through module graph =="
MAIN_BACKUP=$(mktemp)
cp src/main.vix "$MAIN_BACKUP"
restore_main() { cp "$MAIN_BACKUP" src/main.vix; rm -f "$MAIN_BACKUP"; }
trap restore_main EXIT
python3 - <<'PY'
p = "src/main.vix"
s = open(p, encoding="utf-8").read()
if not s.startswith('use "sys.vix" as sys'):
    raise SystemExit("src/main.vix must use sys module")
open(p, "w", encoding="utf-8").write('include "sys.vix"' + s[len('use "sys.vix" as sys'):])
PY
build/vixc src/main.vix -obj -o build/vixc-stage1.o
$CLANGXX -o build/vixc-stage1 build/vixc-stage1.o $COMPILER_SUPPORT \
  -L"$LLD_LIBDIR" -llldELF -llldCommon -l"$LLVM_LINK_LIB" \
  -L"$GC_LIBDIR" -lgc -lz -lzstd -Wl,-rpath,"$LLD_LIBDIR"
cp "$MAIN_BACKUP" src/main.vix
build/vixc-stage1 src/main.vix -obj -o build/vixc-patched.o
$CLANGXX -o build/vixc-patched build/vixc-patched.o $COMPILER_SUPPORT \
  -L"$LLD_LIBDIR" -llldELF -llldCommon -l"$LLVM_LINK_LIB" \
  -L"$GC_LIBDIR" -lgc -lz -lzstd -Wl,-rpath,"$LLD_LIBDIR"
build/vixc-patched --version

echo "== analyzer =="
build/vixc-patched vix-analyzer/main.vix -obj -o build/vix-analyzer.o
$CLANG -c vix-analyzer/support.c -o build/vix-analyzer-support.o
$CLANG build/vix-analyzer.o build/vix-analyzer-support.o runtime/runtime.o -o build/vix-analyzer

echo "== done =="
file build/vix-analyzer
