#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/../.."
# A normal Emscripten SDK on PATH takes precedence.  The Codex-cloud image also
# provides a Debian Emscripten package in this location; using it here keeps the
# documented build reproducible without requiring an interactive shell setup.
if ! command -v emcc >/dev/null; then
  wasm_root="${TAMAMPE_WASM_ROOT:-/workspace/.tamampe-deps/wasm-debian}"
  test -x "$wasm_root/usr/share/emscripten/emcc" || { echo 'Emscripten was not found; install it or set TAMAMPE_WASM_ROOT.' >&2; exit 1; }
  # Resolve Node before adding the package's bin directory: its bundled Node is
  # not complete in the managed image, while the runtime Node is.
  system_node="${EM_NODE_JS:-$(command -v node)}"
  export PATH="$wasm_root/usr/share/emscripten:$wasm_root/usr/bin:$PATH"
  export LD_LIBRARY_PATH="$wasm_root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export EM_LLVM_ROOT="$wasm_root/usr/bin"
  export EM_BINARYEN_ROOT="$wasm_root/usr"
  export EM_NODE_JS="$system_node"
fi
test -f build/gen/melodee_tables.h || { echo 'Run ./build.sh first.' >&2; exit 1; }
mkdir -p build/fm1-virtual
emcc web/fm1-virtual/tamampe_virtual.c -Ibuild/gen -Ifirmware/src -O2 -ffp-contract=off -std=gnu99 -w \
  --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=33554432 -sFILESYSTEM=0 \
  -o build/fm1-virtual/tamampe-virtual.wasm
cc web/fm1-virtual/tamampe_virtual.c -Ibuild/gen -Ifirmware/src -O2 -ffp-contract=off -std=gnu99 -w \
  -DTAMA_VIRTUAL_REFERENCE -lm -o build/fm1-virtual/reference
build/fm1-virtual/reference > build/fm1-virtual/reference.f32
cp web/fm1-virtual/{index.html,main.js,worklet.js} build/fm1-virtual/
echo "virtual FM-1: build/fm1-virtual/tamampe-virtual.wasm"
