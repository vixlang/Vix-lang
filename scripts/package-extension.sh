#!/bin/sh
#
# Build the analyzer, package the VS Code extension, and prove the package is
# actually loadable.
#
# Why this script exists
# ----------------------
# The 0.8.1 VSIX was produced with "vsce package --no-dependencies". That flag
# drops node_modules, so the shipped extension could not resolve
# "vscode-languageclient/node". The top-level require threw while the module was
# loading, activate() never ran, no webview provider was registered, and the Vix
# activity bar panel stayed blank.
#
# Packaging with dependencies is necessary but not sufficient: the failure was
# invisible because nothing inspected the produced archive. This script extracts
# the VSIX and runs the real load path against it, so the same mistake fails the
# build instead of shipping.

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

echo "== grammar =="
python3 editors/vscode/tests/grammar_test.py

echo "== analyzer =="
sh scripts/build-analyzer.sh

echo "== bundle server =="
cp build/vix-analyzer editors/vscode/server/vix-analyzer

echo "== package =="
cd editors/vscode
npm install --no-audit --no-fund
npx vsce package
VSIX=$(ls -t vix-analyzer-*.vsix | head -1)
echo "packaged $VSIX"

echo "== verify package =="
VERIFY_DIR=$(mktemp -d)
trap 'rm -rf "$VERIFY_DIR"' EXIT
unzip -q "$VSIX" -d "$VERIFY_DIR"

EXPECTED=$(shasum -a 256 server/vix-analyzer | awk '{print $1}')
PACKAGED=$(shasum -a 256 "$VERIFY_DIR/extension/server/vix-analyzer" | awk '{print $1}')
if [ "$EXPECTED" != "$PACKAGED" ]; then
  echo "packaging error: packaged vix-analyzer differs from editors/vscode/server/vix-analyzer" >&2
  exit 1
fi

node "$ROOT/editors/vscode/tests/vsix_load_test.js" "$VERIFY_DIR/extension"

echo "== done: $VSIX =="
