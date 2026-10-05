#!/bin/sh
#
# Guards the receiver method design.
#
# Method resolution is supposed to live in one place. This script fails when the
# old patterns creep back in:
#
#   * the array push compared by name instead of through the intrinsic registry
#   * receiver kinds compared against bare numbers instead of the ReceiverKind ADT
#   * method storage names built by string concatenation outside method.vix
#   * the removed per-phase method helpers reappearing
#
# Run it after any change that touches methods.

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
FAIL=0

absent() {
  description=$1
  pattern=$2
  hits=$(grep -rnE "$pattern" src vix-analyzer --include=*.vix 2>/dev/null || true)
  if [ -n "$hits" ]; then
    echo "FAIL $description"
    echo "$hits" | sed 's/^/       /'
    FAIL=1
  else
    echo "ok   $description"
  fi
}

present() {
  description=$1
  pattern=$2
  file=$3
  if grep -qE "$pattern" "$file" 2>/dev/null; then
    echo "ok   $description"
  else
    echo "FAIL $description ($file no longer matches /$pattern/)"
    FAIL=1
  fi
}

echo "== method invariants =="
absent "array push is not matched by name"            'expr\.text == "push"'
absent "no bare receiver kind comparisons"            'receiver_kind[[:space:]]*(==|!=)[[:space:]]*[0-9]'
absent "no per-phase method name builders"            'analysis_method_name|codegen_method_name|semantic_find_method|semantic_is_intrinsic_method'
absent "no method name concatenation outside method.vix" '"::" \+ (expr\.text|method|name)\)'
absent "no receiver offset arithmetic in callers"     'get_function_param_type\([a-z_]+, i \+ 1\)'

present "method_resolve is the resolution entry point" 'pub fn method_resolve'                 src/method.vix
present "method_qualify is the only name builder"     'pub fn method_qualify'                 src/method.vix
present "intrinsic registry defines push"             'intrinsic_array_push'                  src/intrinsic.vix
present "solver resolves through the table"           'method_resolve'                        src/infer/solver.vix
present "ownership resolves through the table"        'method_resolve'                        src/analysis/typed_analysis.vix
present "MIR resolves through the table"              'method_resolve'                        src/mir/mir.vix
present "LLVM resolves through the table"             'method_resolve'                        src/codegen.vix

if [ "$FAIL" -ne 0 ]; then
  echo "== method invariants FAILED =="
  exit 1
fi
echo "== method invariants OK =="
