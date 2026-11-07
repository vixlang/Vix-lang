#!/bin/sh
#
# Format Vix sources without remembering the compiler flags.
#
#   sh scripts/vix-fmt.sh file.vix                 rewrite in place
#   sh scripts/vix-fmt.sh src/                     rewrite every .vix under src/
#   sh scripts/vix-fmt.sh --check src/             report files that need formatting
#   sh scripts/vix-fmt.sh --max-width=80 file.vix
#
# Flags are forwarded to "vixc --fmt". See docs/FORMATTING.md.

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

if [ -z "$VIXC" ]; then
  if [ -x "$ROOT/build/vixc-patched" ]; then
    VIXC="$ROOT/build/vixc-patched"
  elif [ -x "$ROOT/build/vixc" ]; then
    VIXC="$ROOT/build/vixc"
  else
    echo "vix-fmt: no compiler found in $ROOT/build" >&2
    exit 1
  fi
fi

check=0
flags=""
targets=""
for arg in "$@"; do
  case "$arg" in
    --check) check=1 ;;
    -*) flags="$flags $arg" ;;
    *) targets="$targets $arg" ;;
  esac
done

if [ -z "$targets" ]; then
  echo "usage: sh scripts/vix-fmt.sh [--check] [--max-width=N] [--indent=N] [--use-tabs] <file.vix|dir>..." >&2
  exit 2
fi

# Expand directories to the .vix files inside them.
files=""
for target in $targets; do
  if [ -d "$target" ]; then
    for found in $(find "$target" -name '*.vix' | sort); do
      files="$files $found"
    done
  else
    files="$files $target"
  fi
done

status=0
for file in $files; do
  case "$file" in
    *.vix|*.vic) ;;
    *) echo "vix-fmt: not a Vix source: $file" >&2; status=1; continue ;;
  esac
  # shellcheck disable=SC2086
  if [ "$check" = "1" ]; then
    if ! "$VIXC" "$file" --fmt-check $flags 2>/dev/null; then
      echo "would reformat: $file"
      status=1
    fi
  else
    "$VIXC" "$file" --fmt $flags || status=1
  fi
done
exit "$status"
