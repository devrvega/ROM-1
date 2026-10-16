#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The manual's and the site's screenshots, from the simulator (tests/manual.r1: the starter sounds, MIDI coming
# in), into docs/img; then the panel figure (tools/gen_manual_figures.py) with the PART page's screen in it.
#   tools/manual_shots.sh        (needs Pillow)
set -e
cd "$(dirname "$0")/.."
sh host/build_host.sh >/dev/null
mkdir -p build/manual docs/img
rm -f docs/img/screen-*.png
build/host/rom1_host tests/manual.r1 build/manual >/dev/null
cp build/manual/screen-*.png docs/img/
"${PYTHON:-python3}" -c 'import sys; from PIL import Image; [Image.open(f).save(f, optimize=True) for f in sys.argv[1:]]' docs/img/screen-*.png
"${PYTHON:-python3}" tools/gen_manual_figures.py docs/img/screen-part.png
ls docs/img
