# Make it yours

ROM-1 is free software (GPL-3.0). You're welcome to fork it, change it, rename it and release your own
version; no need to ask. You can also send changes back here. Both are described below.

## What the licence asks

If you give someone a firmware built from this code (a release, a `.fwsc` in a chat, a page with an
installer), give them its complete source under the same licence, and keep the copyright notices and
`LICENSE` that are in the files. That's all. The release workflow and the site generator already do it:
every release's tag is its source, and the site carries a tarball of it.

## Build it, try it, test it

Everything runs on GitHub: a fork builds and tests every push (**Actions → Build and test**), and the firmware
is attached to the run as an artifact to download. On your own machine:

| | |
|---|---|
| `tests/run_tests.sh` | the tests (the converter and the engine, the platform, the flash, the package, the app in a simulator, the browser's side if Emscripten and node are there) |
| `./build.sh` | the firmware with the starter sounds, `build/rom1.fwsc` (needs the JieLi toolchain and three SDK files; see [BUILDING.md](BUILDING.md)) |
| `build/gen/rom1conv -o DIR FONT.sf2 ...` | the converter alone: `bank_a.bin`, `bank_b.bin`, `report.json`; `-l` lists a font's presets |
| `build/host/rom1_host SCRIPT OUT` | the whole app on your computer from a script, with the screen as PNGs and the sound as a WAV (`tests/scenarios/*.r1`) |
| `web/emu/build.sh` | the browser emulator and the converter in WebAssembly (needs Emscripten) |
| `python3 tools/rom1_load.py FONT.sf2 ...` | your sounds on an FM-1: converted, installed, the rest sent over USB |
| `python3 tools/fm1_status.py load` | the engine's load on the FM-1 itself, the voices, the screen's |

On a Mac the build runs the JieLi tools in Docker; on Linux x86-64 (and on GitHub) it runs them directly.
`tools/get_toolchain.sh` and `tools/get_sdk_files.sh` fetch what it needs.

## Where things are

| | |
|---|---|
| `firmware/src/synth/r1bank.h` | the bank format: region A (in the firmware) and region B (the data area); zones, parameters, samples, 4-bit ADPCM |
| `firmware/src/synth/rom1.h`, `rom1.c` | the engine: the bank attached, MIDI, the parts, the voices (fetch, interpolation, filter, envelopes, LFOs), the effects, the voice cap |
| `tools/conv/conv.c` | the converter: .sf2 preset tables flattened into zones, the plan that fits (the quality levels), resampling, ADPCM, the bank |
| `tools/conv/rom1conv.c`, `web/conv/conv_web.c`, `web/rom1conv.js` | the converter from the command line, and in the browser |
| `tools/rom1pkg.py`, `web/rom1pkg.js` | a package with another bank in it (region A replaced, the checksums and ciphers redone) |
| `firmware/src/app/upload.c`, `tools/rom1_load.py`, `web/rom1dev.js` | region B written over USB (SysEx), both ends |
| `firmware/src/app/ui.c` | the pages, knobs, buttons, lights, the screen and its dot-matrix face |
| `firmware/src/app/settings.c` | the settings in flash |
| `firmware/src/app/main_fm1.c` | the FM-1: boot, the engine in the audio interrupt, the status and upload SysEx |
| `tools/starter/make_starter.py` | the starter sounds |
| `firmware/hal/`, `firmware/loader/`, `firmware/src/{usb,storage,ota}.c` | the FM-1 platform (Felucca's) |
| `host/`, `tests/` | the simulator and the tests |
| `web/`, `tools/make_pages.py` | the emulator, the installer, the "Your sounds" page, the site |

## Your own firmware, from this one

To release it under your own name, so that it and ROM-1 can live side by side (and on the
[FM-1 firmware hub](https://thefirmwarehub.com/)):

1. **The name.** Search for `ROM-1` and change what you want: the start screen (`firmware/src/app/main_fm1.c`),
   the USB names (`STR1` and `STR2` in `firmware/src/usb.c`), the docs and the site.
2. **The identity**, what the FM-1 reports and the installers check: `PRODUCT` and the release prefix in
   `tools/build.py`, and the `FELUCCA_ID` default in `firmware/src/main.c`. Pick a prefix nobody uses (ROM-1
   is `FM-1_0…`); the hub's [catalogue](https://thefirmwarehub.com/catalog.json) lists the ones in use.
3. **The saved data's tags**, `SET_MAGIC` in `firmware/src/app/app.h`, and the bank's `R1_MAGIC` and
   `R1_MAGIC_B` in `firmware/src/synth/r1bank.h`, so that your firmware and ROM-1 never read each other's
   settings or sounds. The upload SysEx's ID (`7D 52 31`) is worth changing too.
4. **Releases:** push a tag `vX.Y.Z`; the Release workflow builds and publishes it. For the site, set
   **Settings → Pages → Source** to **GitHub Actions**; the Pages workflow then publishes it after each
   release.

## Sending changes back

Pull requests are welcome, small or large; there's no form to fill in. It helps if:

- `tests/run_tests.sh` passes (the pull request's checks run it for you);
- new behaviour has a test or a line in a scenario script, and the manual (`docs/MANUAL.md`) says how to use it;
- a change to the converter keeps the browser's and the command line's banks the same (`tests/host/conv_wasm_test.mjs`);
- you say whether you tried it on an FM-1, and what `tools/fm1_status.py load` showed for anything that
  plays.

Some habits of the code: C99, `float` only in the firmware (the FM-1 has no double-precision hardware, and the
build fails if a `double` gets in), no libm there (`fastmath.h` instead), and nothing slow in the engine: it
renders in an interrupt every 2.9 ms, and its worst case is measured (the bench builds). The converter runs on
a computer and may use `double`, but must give the same bytes everywhere (no unstable sorts, no fused
multiply-adds). The comments say what a thing is for, in plain words.
