#!/bin/sh
#
# Compiler shim that makes vixc usable on macOS.
#
# vixc's embedded linker only implements the ELF and COFF drivers, so on macOS
# "vixc prog.vix -o prog" cannot link. The LLVM backend itself is fine: it emits
# a real Mach-O object. This shim catches the plain "build an executable"
# invocation, emits an object with the LLVM backend and links it with the
# platform toolchain.
#
# Point the editor at this file so the Build & Run panel and the vix.run command
# work:
#   "vix.compilerPath": "/Users/Admin/Desktop/Vix-lang/scripts/vixc-macos.sh"
#
# Every other mode (-obj, -S, --check, ...) is forwarded to the real compiler
# unchanged, so diagnostics and the analyzer keep working.

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

if [ -z "$VIXC" ]; then
  if [ -x "$ROOT/build/vixc-patched" ]; then
    VIXC="$ROOT/build/vixc-patched"
  elif [ -x "$ROOT/build/vixc" ]; then
    VIXC="$ROOT/build/vixc"
  else
    echo "vixc-macos: no compiler found in $ROOT/build" >&2
    exit 1
  fi
fi

if [ -z "$CXX" ]; then
  CXX=clang++
fi

RUNTIME="$ROOT/runtime/runtime.o"

mode=link
output=""
input=""
want_output=0
extra=""
for arg in "$@"; do
  if [ "$want_output" = "1" ]; then
    output="$arg"
    want_output=0
    continue
  fi
  case "$arg" in
    -o)
      want_output=1
      ;;
    -obj|-S|--check|--lex|--ast|--parser|--ast-json|--semantic|--typeinfer|--module-graph|--parse-stats|--help|-h|--version|-v)
      mode=forward
      ;;
    *.vix)
      if [ -z "$input" ]; then
        input="$arg"
      fi
      ;;
    -*)
      extra="$extra $arg"
      ;;
    *)
      ;;
  esac
done

if [ "$mode" = "forward" ] || [ -z "$input" ] || [ -z "$output" ]; then
  exec "$VIXC" "$@"
fi

if [ ! -f "$RUNTIME" ]; then
  echo "vixc-macos: runtime object missing at $RUNTIME" >&2
  exit 1
fi

tmp_obj=$(mktemp -t vixc-macos).o
trap 'rm -f "$tmp_obj"' EXIT

# The compiler prints the object path on stdout; keep it out of the editor's
# compiler output channel. Diagnostics go to stderr and are unaffected.
# shellcheck disable=SC2086
"$VIXC" "$input" -obj -o "$tmp_obj" $extra >/dev/null
"$CXX" "$tmp_obj" "$RUNTIME" -o "$output"
