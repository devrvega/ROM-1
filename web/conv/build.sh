#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# ROM-1's converter for the browser (the upload page, the emulator): tools/conv/conv.c compiled to WebAssembly.
#   web/conv/build.sh  ->  build/web/rom1conv.wasm
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/web
emcc -O2 -std=c99 -Wall -Wextra -Wno-unused-function -ffp-contract=off \
    --no-entry -sSTANDALONE_WASM -sFILESYSTEM=0 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 \
    -sMAXIMUM_MEMORY=2147483648 -sSTACK_SIZE=1048576 \
    -o build/web/rom1conv.wasm web/conv/conv_web.c tools/conv/conv.c
echo "conv: build/web/rom1conv.wasm ($(wc -c < build/web/rom1conv.wasm) B)"
