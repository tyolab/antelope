#!/usr/bin/env bash
# Fetch the tree-sitter runtime + grammar wasm files into spoor/grammars/.
# These are binary vendored assets (gitignored); run once after `npm install`.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
grammars="$here/grammars"
mkdir -p "$grammars"
cp "$here/node_modules/web-tree-sitter/tree-sitter.wasm" "$grammars/"
tmp="$(mktemp -d)"
( cd "$tmp" && npm pack tree-sitter-wasms@0.1.11 >/dev/null && tar -xzf tree-sitter-wasms-*.tgz )
for g in javascript typescript python; do
  cp "$tmp/package/out/tree-sitter-$g.wasm" "$grammars/"
done
rm -rf "$tmp"
echo "grammars ready:"; ls "$grammars"/*.wasm
