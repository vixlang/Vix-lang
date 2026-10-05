#!/bin/sh
#
# Compile and run a Vix program with the LLVM backend.
#
# Usage:
#   sh scripts/run-vix.sh examples/impl2.vix
#   VIXX_ARGS="-opt=l2" sh scripts/run-vix.sh prog.vix
#
# The self/LIR backend refuses non-x86_64 hosts and vixc's embedded linker only
# implements the ELF and COFF drivers, so this emits a Mach-O object with the
# LLVM backend and links it with the platform toolchain before running it.

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

SOURCE="$1"
if [ -z "$SOURCE" ]; then
  echo "usage: sh scripts/run-vix.sh <file.vix>" >&2
  exit 2
fi

if [ -z "$VIXC" ]; then
  if [ -x "$ROOT/build/vixc-patched" ]; then
    VIXC="$ROOT/build/vixc-patched"
  else
    VIXC="$ROOT/build/vixc"
  fi
fi

if [ -z "$CXX" ]; then
  CXX=clang++
fi

RUNTIME="$ROOT/runtime/runtime.o"
if [ ! -f "$RUNTIME" ]; then
  echo "run-vix: runtime object missing at $RUNTIME" >&2
  exit 1
fi

WORK=$(mktemp -d -t run-vix)
trap 'rm -rf "$WORK"' EXIT

OBJ="$WORK/program.o"
BIN="$WORK/program"

# shellcheck disable=SC2086
"$VIXC" "$SOURCE" -obj -o "$OBJ" $VIX_ARGS
"$CXX" "$OBJ" "$RUNTIME" -o "$BIN"

"$BIN"
status=$?
echo "run-vix: $SOURCE exited $status"
exit "$status"
