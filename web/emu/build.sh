#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# ROM-1 in the browser: the host simulator compiled to WebAssembly (Emscripten), its page, the starter sounds'
# region A, and the converter (for .sf2 files loaded in the page).
#   [EN_VERSION=0.1] web/emu/build.sh  ->  build/emu/{index.html, worklet.js, rom1.wasm, starter.bin, rom1conv.js,
#                                                   rom1conv.wasm}
# Same sources and flags as host/build_host.sh (-ffp-contract=off, like the device).
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/gen build/emu
sh host/build_host.sh >/dev/null                    # (the fonts' headers, the converter, the starter's bank)
emcc -O2 -ffp-contract=off -std=gnu99 -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
    -DEN_HOST -DEN_WEB "-DEN_VERSION=\"$(printf %s "${EN_VERSION:-DEV}" | tr a-z A-Z)\"" \
    -Ifirmware/src -Ifirmware/src/synth -Ibuild/gen \
    --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=33554432 -sALLOW_MEMORY_GROWTH=1 -sFILESYSTEM=0 \
    -o build/emu/rom1.wasm web/emu/rom1_web.c firmware/src/synth/rom1.c
sh web/conv/build.sh >/dev/null
cp web/emu/index.html web/emu/worklet.js web/rom1conv.js build/web/rom1conv.wasm build/emu/
cp build/gen/starter/bank_a.bin build/emu/starter.bin
echo "emu: build/emu ($(wc -c < build/emu/rom1.wasm) B wasm, starter $(wc -c < build/emu/starter.bin) B)"
