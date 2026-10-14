#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v emcc >/dev/null || { echo 'Activate Emscripten first (see web/emulator/README.md).' >&2; exit 1; }
test -f build/gen/melodee_tables.h || { echo 'Generate firmware headers first (see README).' >&2; exit 1; }
mkdir -p build/emulator
emcc web/emulator/fm6_bridge.c -Ibuild/gen -Ifirmware/src -O2 -ffp-contract=off \
  -Wno-unused-function -sSTANDALONE_WASM=1 --no-entry \
  -sINITIAL_MEMORY=16777216 -sSTACK_SIZE=1048576 -sALLOW_MEMORY_GROWTH=0 \
  -sFILESYSTEM=0 -sEXPORTED_FUNCTIONS='["_emulator_init","_emulator_note_on","_emulator_note_off","_emulator_render","_emulator_rate","_emulator_block"]' \
  -o build/emulator/fm6.wasm
cc web/emulator/fm6_bridge.c -Ibuild/gen -Ifirmware/src -O2 -ffp-contract=off \
  -DEMULATOR_REFERENCE -lm -o build/emulator/reference
build/emulator/reference > build/emulator/reference.f32
node web/emulator/verify.mjs
