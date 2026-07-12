#!/bin/sh
set -eu

root=$(CDPATH= cd "$(dirname "$0")" && pwd)
compiler=${VIXC:-"$root/build/vixc"}
seed_compiler="$root/seed/vixc"

if [ ! -x "$compiler" ]; then
    echo "refresh-seed: missing executable compiler: $compiler" >&2
    echo "refresh-seed: build it first with make after ./seed.sh has produced seed/vixc" >&2
    exit 1
fi

if [ ! -d "$root/seed" ]; then
    echo "refresh-seed: missing seed directory" >&2
    exit 1
fi

tmp_ir=$(mktemp "$root/seed/vixc.ll.tmp.XXXXXX")
normalized_ir=$(mktemp "$root/seed/vixc.ll.normalized.XXXXXX")
bootstrap_log=$(mktemp "${TMPDIR:-/tmp}/vixc-bootstrap.XXXXXX")
trap 'rm -f "$tmp_ir" "$normalized_ir" "$bootstrap_log"' EXIT HUP INT TERM

ulimit -s 65536
if ! "$compiler" "$root/src/main.vix" -ll -o "$tmp_ir" >"$bootstrap_log" 2>&1; then
    if [ -z "${VIXC+x}" ] && [ "$compiler" != "$seed_compiler" ] && [ -x "$seed_compiler" ]; then
        echo "refresh-seed: $compiler failed; retrying with stable $seed_compiler"
        compiler="$seed_compiler"
        if ! "$compiler" "$root/src/main.vix" -ll -o "$tmp_ir" >"$bootstrap_log" 2>&1; then
            cat "$bootstrap_log" >&2
            exit 1
        fi
    else
        cat "$bootstrap_log" >&2
        exit 1
    fi
fi

if [ ! -s "$tmp_ir" ]; then
    echo "refresh-seed: compiler produced an empty seed IR" >&2
    exit 1
fi

sed "1s|^; ModuleID = .*|; ModuleID = 'vixc.ll'|" "$tmp_ir" >"$normalized_ir"
if [ ! -s "$normalized_ir" ]; then
    echo "refresh-seed: failed to normalize seed IR" >&2
    exit 1
fi

rm -f "$tmp_ir"
chmod 644 "$normalized_ir"
mv "$normalized_ir" "$root/seed/vixc.ll"
rm -f "$bootstrap_log"
trap - EXIT HUP INT TERM
echo "refresh-seed: updated seed/vixc.ll using $compiler"
git add .
git commit -m "$1"
git push
