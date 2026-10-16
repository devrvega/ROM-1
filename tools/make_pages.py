#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Make the ROM-1 site (a static folder: GitHub Pages or any web server serves it as it is):

  index.html                  what ROM-1 is, and the ways in: try, install, your sounds, manual, download, source
  img/                        screenshots (docs/img)
  emu/                        ROM-1 in the browser (build/emu, from web/emu/build.sh), and room.json (the room
                              the release's firmware has for sounds, for .sf2 files loaded there)
  install/index.html          the web installer (web/rom1_installer.html, with fm1pkg.js, fm1ota.js and the
                              package's metadata inlined; Chrome or Edge, Web MIDI): the starter sounds
  sounds/index.html           your sounds on the FM-1 (web/rom1_sounds.html, with fm1pkg.js, fm1ota.js,
                              rom1pkg.js, rom1dev.js and rom1conv.js inlined), and rom1conv.wasm beside it
  manual/index.html, img/     the manual (tools/manual_html.py, from docs/MANUAL.md)
  firmware/rom1-VERSION.fwsc  the package the installers write (the sounds page puts your sounds in it first);
                              also the download
  source/                     the complete source (GPL-3.0: whoever gets the firmware gets the source),
                              as a tarball of this tree; and ROM1_REPO, if set, the repository's address

  [ROM1_REPO=https://...] tools/make_pages.py build/rom1-0.1.0.fwsc 0.1.0 OUT_DIR

The package must be a release build (./build.sh --release X.Y.Z): its identity, FM-1_0XXYYZZ, is what
the installers check the download against and what the FM-1 reports after the install."""
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "web"))
sys.path.insert(0, str(SRC / "tools"))
from make_site import product_of, strip_module  # noqa: E402  (Felucca's: the package format)
import gen_dotfont  # noqa: E402  (ROM-1's dot-matrix face)
import rom1pkg  # noqa: E402

REPO = os.environ.get("ROM1_REPO", "")
ROOM_B = 0x45000                                   # region B: the data area (firmware/src/synth/r1bank.h)

LANDING = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ROM-1</title>
<meta name="description" content="Firmware that turns the M-VAVE FM-1 into a sixteen-part sound module playing .sf2 files: your sounds converted in the browser to fit, a filter, envelopes, reverb and chorus, MIDI over USB and the TRS jack.">
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'%3E%3Crect width='16' height='16' rx='3' fill='%23f0a63a'/%3E%3Crect x='3' y='4' width='2' height='8' fill='%23321a06'/%3E%3Crect x='7' y='7' width='2' height='5' fill='%23321a06'/%3E%3Crect x='11' y='5' width='2' height='7' fill='%23321a06'/%3E%3C/svg%3E">
<style>
/* ROM-1's colours: a warm dark body, the module LCD's amber backlight and brown ink */
:root { --paper: #17130f; --sheet: #241d16; --ink: #f2e6d0; --muted: #a89c88; --rule: #3a3026;
        --lcd: #f0a63a; --lcd-off: #e09630; --lcd-ink: #321a06; color-scheme: dark; }
body { background: var(--paper); color: var(--ink); margin: 0; padding: 40px 16px 64px; font: 17px/1.6 "Helvetica Neue", Helvetica, Arial, sans-serif; }
main { max-width: 46rem; margin: 0 auto; }
.lcd { background: var(--lcd); border-radius: 10px; padding: 18px 20px 14px; box-shadow: inset 0 0 0 3px #c9852a, 0 0 40px rgba(240, 166, 58, .18); max-width: 30rem; }
.dots { display: grid; grid-auto-flow: column; gap: 3px; width: fit-content; max-width: 100%; }
.dots .ch { display: grid; grid-template-columns: repeat(5, 1fr); gap: 2px; margin-right: 6px; }
.dots i { width: clamp(5px, 1.6vw, 9px); aspect-ratio: 1; border-radius: 1px; background: var(--lcd-off); }
.dots i.on { background: var(--lcd-ink); }
.meters { display: grid; grid-template-columns: repeat(16, 1fr); gap: 3px; align-items: end; height: 26px; margin: 0 0 12px; }
.meters span { background: repeating-linear-gradient(to top, var(--lcd-ink) 0 3px, transparent 3px 5px); }
.lcd p { margin: 10px 0 0; color: var(--lcd-ink); font: 600 .95rem/1.3 "Helvetica Neue", Helvetica, Arial, sans-serif; letter-spacing: .04em; }
.sub { color: var(--muted); margin: 1.1rem 0 1.4rem; font-size: 1.1rem; }
.lede { font-size: 1.12rem; }
.shots { display: flex; flex-wrap: wrap; gap: 12px; margin: 1.6rem 0; }
.shots img { width: calc((100% - 24px) / 3); min-width: 150px; height: auto; image-rendering: pixelated; border-radius: 6px; }
h2 { font-size: 1.15rem; margin: 2.2rem 0 .6rem; padding-top: .6rem; border-top: 1px solid var(--rule); }
.ways { display: grid; grid-template-columns: repeat(auto-fit, minmax(13rem, 1fr)); gap: 12px; margin: 1.6rem 0; }
.ways a { display: block; padding: .8rem 1rem; background: var(--sheet); border: 1px solid var(--rule); border-radius: 8px; text-decoration: none; color: var(--ink); }
.ways a:first-child { background: var(--lcd); color: var(--lcd-ink); border-color: var(--lcd); }
.ways a strong { display: block; } .ways a span { display: block; font-size: .9rem; opacity: .8; }
.ways a:hover, .ways a:focus-visible { border-color: var(--lcd); outline: none; }
.v { font-style: normal; font-weight: 400; opacity: .8; }
table.spec { border-collapse: collapse; width: 100%; font-size: .95rem; }
table.spec td { padding: .35rem .5rem; border-bottom: 1px solid var(--rule); vertical-align: top; }
table.spec td:first-child { font-weight: 700; white-space: nowrap; }
.small { color: var(--muted); font-size: .9rem; }
a { color: var(--ink); text-decoration-color: var(--lcd); text-decoration-thickness: 2px; text-underline-offset: 3px; }
code { font-size: .88em; }
</style>
</head>
<body>
<main>
<div class="lcd" role="img" aria-label="ROM-1, on a module's amber LCD">
  <div class="meters" aria-hidden="true">__METERS__</div>
  <div class="dots" aria-hidden="true">__DOTS__</div>
  <p>16 PARTS &middot; PLAYS .SF2 FILES</p>
</div>
<p class="sub">A sound module for the M-VAVE FM-1, playing your .sf2 files</p>
<p class="lede">ROM-1 turns the FM-1 into a sixteen-part sound module, as the rack modules of the nineties were: a
part for each MIDI channel, each playing a preset of the sounds you put on it, with a filter, envelopes, reverb
and chorus. The keys play the part on the screen; MIDI over USB or the TRS jack plays all sixteen at once.
Your .sf2 files are converted in the browser to fit the FM-1, with the best quality that fits.</p>
<div class="shots">
  <img src="img/screen-part.png" width="240" height="240" alt="ROM-1's screen: the sixteen parts' levels, the preset's name in big dots, the part's volume, pan, reverb and chorus">
  <img src="img/screen-sound.png" width="240" height="240" alt="ROM-1's SOUND page: cutoff, resonance, attack and release">
  <img src="img/screen-drums.png" width="240" height="240" alt="ROM-1 on part 10, the drum part, playing a kit">
</div>
<nav class="ways" aria-label="Get ROM-1">
  <a href="emu/"><strong>Try it in the browser</strong><span>The same code the FM-1 runs: the starter sounds, or your .sf2 files. No FM-1 needed.</span></a>
  <a href="sounds/"><strong>Your sounds</strong><span>Choose .sf2 files and presets; they are converted here and put on the FM-1 with ROM-1, over USB.</span></a>
  <a href="install/"><strong>Install <em class="v">v__VERSION__</em></strong><span>ROM-1 with its starter sounds, from Chrome or Edge, the FM-1 connected by USB.</span></a>
  <a href="manual/"><strong>Manual</strong><span>Getting started, then every knob, page and MIDI message.</span></a>
  <a href="firmware/__PKG__"><strong>Download <em class="v">__PKG__</em></strong><span>For the command-line tools: <code>python3 tools/rom1_load.py FONT.sf2</code></span></a>
  <a href="source/"><strong>Source</strong><span>GPL-3.0. Built on Felucca by Hügelton Instruments, by way of X0X, AMB-1, IND-1, LCD-4, CRT-4 and SEQ-4.</span></a>
</nav>

<h2>What it does</h2>
<table class="spec">
<tr><td>Parts</td><td>Sixteen, one a MIDI channel (channel 10 the drums); a preset, volume, pan, reverb and chorus sends, filter, attack and release each.</td></tr>
<tr><td>Sounds</td><td>About 740 KB: part in the firmware, part in the FM-1's data area. 16-bit where there is room, else 4-bit ADPCM, then lower rates, as much as needed and no more. Game-console and chiptune fonts and many General MIDI sets fit whole; from big fonts, pick presets.</td></tr>
<tr><td>Fonts</td><td>Several at once: bank select (CC 0) picks the font, program change its preset, channel 10 its drum kit.</td></tr>
<tr><td>Engine</td><td>32 voices (fewer when the FM-1 runs short of time), cubic interpolation, a resonant low-pass filter, the .sf2 envelopes and LFOs, the standard modulators, 44.1 kHz.</td></tr>
<tr><td>Effects</td><td>Reverb (room, hall, large hall, plate) and chorus, with each part's sends.</td></tr>
<tr><td>MIDI</td><td>USB and the TRS jack: notes, velocity, pitch bend and its range, modulation, sustain, volume, expression, pan, the sends, cutoff and resonance, attack and release, all notes off; General MIDI's and GS's resets.</td></tr>
<tr><td>Screen</td><td>A backlit module LCD: the parts' levels, the preset's name in big dots, the page's four knobs.</td></tr>
</table>
<p class="small">To go back to the official firmware, use the installer's "Back to the stock firmware", or
M-VAVE's updater, M-UPGRADE, from <a href="https://www.m-vave.com/download">m-vave.com/download</a>.
The starter sounds are ROM-1's own. The .sf2 files you put on it are yours to choose: their samples belong to
whoever made them. M-VAVE and FM-1 are trademarks of their owners; ROM-1 is independent and not affiliated with
them.</p>
</main>
</body>
</html>
"""

SOURCE = """<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>ROM-1 source</title>
<style>body { background: #17130f; color: #f2e6d0; font: 17px/1.6 "Helvetica Neue", Helvetica, Arial, sans-serif; margin: 48px 16px; }
main { max-width: 42rem; margin: 0 auto; } a { color: #f2e6d0; text-decoration-color: #f0a63a; text-decoration-thickness: 2px; }</style></head>
<body><main><h1>ROM-1 source</h1>
<p>ROM-1 is free software under the GNU General Public License, version 3 only. The complete source of v__VERSION__:
<a href="__TAR__">__TAR__</a>.__REPOLINE__</p>
<p><a href="../">Back to ROM-1</a></p></main></body></html>
"""


def dots(text):
    """the text in ROM-1's 5 x 7 face, as the screen draws it: lit dots and unlit ones"""
    out = []
    for ch in text:
        rows = gen_dotfont.G[ch].split()
        cells = "".join(f'<i class="on"></i>' if rows[r][c] == "1" else "<i></i>" for r in range(7) for c in range(5))
        out.append(f'<span class="ch">{cells}</span>')
    return "".join(out)


def source_tarball(dest, version):
    """the tree as it is (no build output, no caches): what the GPL asks to give with the firmware"""
    name = f"rom-1-{re.sub(r'[^A-Za-z0-9.-]', '-', version)}-src"
    skip = {"build", "__pycache__", ".git", ".DS_Store", ".serena", "node_modules"}

    def keep(ti):
        return None if set(Path(ti.name).parts) & skip else ti
    with tarfile.open(dest / f"{name}.tar.gz", "w:gz") as t:
        t.add(SRC, arcname=name, filter=keep)
    return f"{name}.tar.gz"


def inline(page_path, libs, meta):
    page = page_path.read_text(encoding="utf-8")
    for mark in ("/*LIB*/", "/*META*/"):
        if page.count(mark) != 1:
            raise SystemExit(f"{page_path.name} must contain {mark} once")
    lib = "\n".join(strip_module((SRC / "web" / f).read_text(encoding="utf-8")) for f in libs)
    return page.replace("/*LIB*/", lib).replace("/*META*/", json.dumps(meta))


def main(pkg, version, out):
    pkg, out = Path(pkg), Path(out)
    raw = pkg.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"FM-1_0\d{6}", product):
        raise SystemExit(f"{pkg}: identity {product!r}: make a release build (./build.sh --release X.Y.Z)")
    if b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit(f"{pkg}: no update loader in it")
    info = rom1pkg.info(pkg)
    name = f"rom1-{re.sub(r'[^A-Za-z0-9.-]', '-', version)}.fwsc"
    emu = SRC / "build" / "emu"
    if not (emu / "rom1.wasm").exists() or not (emu / "rom1conv.wasm").exists():
        raise SystemExit("no build/emu/rom1.wasm: run web/emu/build.sh (needs Emscripten)")
    if out.exists():
        shutil.rmtree(out)
    for d in ("install", "sounds", "manual", "firmware", "source"):
        (out / d).mkdir(parents=True)
    shutil.copy(pkg, out / "firmware" / name)
    meta = {"version": version, "product": product, "pkg": "../firmware/" + name}
    # the installer, and the sounds page (with the converter beside it)
    (out / "install" / "index.html").write_text(
        inline(SRC / "web" / "rom1_installer.html", ["fm1pkg.js", "fm1ota.js"], meta), encoding="utf-8")
    (out / "sounds" / "index.html").write_text(
        inline(SRC / "web" / "rom1_sounds.html", ["fm1pkg.js", "fm1ota.js", "rom1pkg.js", "rom1dev.js", "rom1conv.js"], meta),
        encoding="utf-8")
    shutil.copy(emu / "rom1conv.wasm", out / "sounds" / "rom1conv.wasm")
    # the manual (a complete page; its images beside it)
    subprocess.run([sys.executable, str(SRC / "tools" / "manual_html.py"), str(SRC / "docs" / "MANUAL.md"),
                    str(out / "manual" / "index.html")], check=True, capture_output=True)
    shutil.copytree(SRC / "docs" / "img", out / "manual" / "img")
    # the landing page and its screenshots
    shutil.copytree(SRC / "docs" / "img", out / "img")
    meters = "".join(f'<span style="height:{h}px"></span>' for h in (23, 18, 0, 20, 13, 0, 0, 15, 0, 25, 8, 0, 0, 0, 5, 0))
    (out / "index.html").write_text(LANDING.replace("__VERSION__", html.escape(version)).replace("__PKG__", name)
                                    .replace("__DOTS__", dots("ROM-1")).replace("__METERS__", meters), encoding="utf-8")
    # the source, for the GPL
    tar = source_tarball(out / "source", version)
    repo = f' The repository: <a href="{html.escape(REPO)}">{html.escape(REPO)}</a>.' if REPO else ""
    (out / "source" / "index.html").write_text(SOURCE.replace("__VERSION__", html.escape(version)).replace("__TAR__", tar)
                                               .replace("__REPOLINE__", repo), encoding="utf-8")
    # the emulator, told the room the release's firmware has (for .sf2 files loaded there)
    shutil.copytree(emu, out / "emu")
    (out / "emu" / "room.json").write_text(json.dumps({"a": info["room"], "b": ROOM_B}))
    (out / ".nojekyll").write_text("")
    print(f"site: {out}: index.html, install/ and sounds/ ({product}, {info['room']} B of room in the firmware), "
          f"manual/, emu/, firmware/{name} ({len(raw)} B)")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:4])
