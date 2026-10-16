#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the ROM-1 host simulator (the whole app on this machine) into build/host/rom1_host, and the starter
# sounds' bank it plays by default (build/gen/starter/bank_a.bin). The same sources as the firmware: the app in
# one unit, the engine (synth/rom1.c) in another, as on the device; -ffp-contract=off like the device.
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
mkdir -p build/gen build/host build/gen/starter
[ -f build/gen/felucca_font.h ] || python3 tools/gen_font.py build/gen/felucca_font.h >/dev/null
python3 tools/gen_dotfont.py build/gen/dotfont.h
$CC -O2 -std=c99 -o build/gen/rom1conv tools/conv/rom1conv.c tools/conv/conv.c -lm
if [ ! -f build/gen/starter/bank_a.bin ] || [ tools/starter/make_starter.py -nt build/gen/starter/bank_a.bin ] ||
   [ tools/conv/conv.c -nt build/gen/starter/bank_a.bin ]; then
    python3 tools/starter/make_starter.py "build/gen/ROM-1 Starter.sf2"
    build/gen/rom1conv -a 483260 -b 0 -n "ROM-1 Starter" -o build/gen/starter "build/gen/ROM-1 Starter.sf2" >/dev/null
fi
$CC -O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -DEN_HOST -Ifirmware/src -Ifirmware/src/synth -Ibuild/gen -o build/host/rom1_host host/rom1_host.c \
    firmware/src/synth/rom1.c -lm
echo "host: build/host/rom1_host"
