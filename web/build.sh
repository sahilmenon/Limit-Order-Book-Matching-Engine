#!/usr/bin/env bash
# Compile the order-book engine to WebAssembly for the live browser demo.
#
# Requires the Emscripten SDK on PATH (emcc). Install once with:
#   git clone https://github.com/emscripten-core/emsdk && cd emsdk
#   ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh
#
# Output: web/public/lob.js + web/public/lob.wasm (an ES module).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/web/public"
mkdir -p "$OUT"

emcc \
  "$ROOT/web/wasm/lob_wasm.cpp" \
  "$ROOT/src/fast_order_book.cpp" \
  "$ROOT/src/order_book.cpp" \
  -I "$ROOT/include" \
  -std=c++20 -O3 \
  --bind \
  -s MODULARIZE=1 \
  -s EXPORT_ES6=1 \
  -s EXPORT_NAME=createLobModule \
  -s ENVIRONMENT=web \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=33554432 \
  -o "$OUT/lob.js"

echo "Built $OUT/lob.js + lob.wasm"
