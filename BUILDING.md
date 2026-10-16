# Building ROM-1

The build makes three files in `build/`:

| File | What |
| --- | --- |
| `rom1.bin` | the firmware app, with region A of a bank after its code (the starter sounds, or `--bank`'s) |
| `loader/ota.bin` | the update loader |
| `rom1.fwsc` | the installable package (app + loader) |

## Prerequisites (macOS)

- Python 3 with Pillow: `pip3 install Pillow`
- A C compiler for the computer itself (`cc`: Xcode's command line tools on a Mac; gcc on Linux), for the
  converter, the tests and the simulator.
- Docker Desktop. The JieLi toolchain is Linux x86-64 only; the build runs each tool in a
  `linux/amd64` `debian:bookworm-slim` container (Rosetta on Apple silicon). Keep the source
  tree in a folder Docker can share, e.g. under `/Users`.
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server):

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

- Three files of the JieLi AC79 SDK (Apache-2.0): `cpu/wl82/tools/uboot.boot`, `cfg_tool.bin` and
  `cfg/eq_cfg_hw.bin`. They are not part of this tree; this fetches just them, from JieLi's repository or a
  GitHub mirror of it, and keeps them only if their SHA-256 is the one the build expects:

  ```
  tools/get_sdk_files.sh            # into ~/fw-AC79_AIoT_SDK
  ```

  (or the whole SDK: `git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13
  https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK`)

- Emscripten 3.1.74 and Node.js (optional: the converter in WebAssembly, the emulator, the web tests).

On Linux x86-64 the toolchain runs natively and Docker is not needed.

## Build

```
./build.sh
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations
(`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`).

The build first makes what it puts in: the fonts' headers (`build/gen/felucca_font.h`, `build/gen/dotfont.h`,
the screen's dot-matrix face), the converter (`build/gen/rom1conv`), the starter sounds
(`build/gen/ROM-1 Starter.sf2`, from `tools/starter/make_starter.py`) and their bank, converted for the room
the code leaves. Region A goes in at the first 4 KiB boundary after the code, to the end of the app's slot;
the build says how much room that is.

- `./build.sh --release 0.1.0` makes a release build; the package is `build/rom1-0.1.0.fwsc`.
- `./build.sh --bank DIR/bank_a.bin` builds another bank's region A in (`rom1conv`'s); its region B then goes
  over USB (`tools/rom1_load.py --region-b DIR/bank_b.bin`). `tools/rom1pkg.py patch` does the same to a
  package already built, byte for byte.
- `./build.sh --bench` makes a development build (`build/rom1-bench.fwsc`) that also plays test notes over
  SysEx, to measure the FM-1 itself.

Every build answers ROM-1's SysEx, `F0 7D 52 31 cmd ... F7` (values as five 7-bit groups):

| SysEx | What it does |
|---|---|
| `01` | The status: the engine's load (%), its slowest render (µs, of the 2902 µs a 128-frame render has), renders late, renders; the bench flag; voices, notes; the screen's frames, their time (µs, summed) and the slowest; MIDI in; region A's bank id and length, region B's expected length, region B there and right; flash found, the last write's result (+100), the boots counted; the voices allowed now |
| `03` | The slowest render and the late ones counted again |
| `04 s` | The screen left undrawn for s seconds (to see what it costs) |
| `20 len id` | Region B: writing begins (no sound until `22`). Answered `20 0` |
| `21 off data` | Region B: bytes at `off`, 7-bit packed (each 7 bytes after a byte of their high bits), at most a 256-byte page and within it; a sector is erased as its first byte comes. The last page sent again (its answer lost) is taken as written. Answered `21 off` (or `7FFFFFF`) |
| `22` | Region B written: checked, and played. Answered `22 1` when it is there and right |
| General MIDI on, GS reset | Every part back to how it starts |

A bench build also takes `02 k` (0: every part a four-note chord, held; 1: a chord a part and a beat), `06`
(all notes off) and `0A` (each part of a screen frame timed, in ticks of 24 MHz, and the wait for the LCD's
transfers).

On macOS with podman instead of Docker, put a `docker` script that runs `exec podman "$@"` first
on your PATH. `EN_JOBS` (default 4) limits parallel compiles: a podman machine drops
connections when many containers start at once.

Build option: `EN_CDC=1` adds Felucca's USB serial function (off by default: one plain
MIDI interface).

The build fails if a soft-double routine is linked (a `double` crept in) and checks the image, RAM and pool
sizes. The engine (`firmware/src/synth/rom1.c`) is compiled on its own at -O2; the rest is one unit at -Os.

## Tests

```
tests/run_tests.sh
```

Runs, on the build machine:

- the maths library against libm; the panel's encoder decoding and the TRS jack's MIDI parser; flash storage
  (the settings' objects, clear of region B);
- the converter and the engine (`tests/host/rom1_test.c`): fonts made by `tests/fonts/make_test_fonts.py`
  converted at three sizes (16-bit, 4-bit, 4-bit at lower rates), each tone's pitch measured to within 0.1 %;
  the starter sounds and two fonts together, every preset sounding, every voice free after; picked presets; a
  bank too big refused;
- the package: the build's own bank patched in again gives the package byte for byte, another bank goes in
  with the code untouched, and Felucca's update-path tests read both (`tests/pkg_test.py`, `tests/ota_test.c`,
  `tests/ldr_test.c` with `AC79_SDK`, `tests/install_test.py`);
- the browser's side, with node and Emscripten: `web/rom1pkg.js` against `rom1pkg.py`, `web/rom1dev.js`
  against a simulated FM-1, the converter in WebAssembly against the command line's (the same bytes), the
  emulator;
- the whole app in the simulator (`tests/scenarios/*.r1`: the keys and the drum part, the pages and buttons,
  MIDI in, the presets and fonts, region B's upload, the settings, many voices), with its screenshots in
  `build/scenarios/`, then the same scenarios and the converter again under the address and
  undefined-behaviour sanitizers.

`host/build_host.sh` builds the simulator alone; `build/host/rom1_host SCRIPT OUTDIR` runs one script (the
command list is at the top of `host/rom1_host.c`). It renders as the FM-1 does (128-frame blocks), keeps its
flash across a simulated restart, and plays the starter sounds unless `bank A.bin [B.bin]` gives another.

- `tools/manual_shots.sh`: the manual's screenshots into `docs/img`; `tools/readme_gif.py` the README's animation.

## Install

From the command line (needs `pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py build/rom1.fwsc                     # the starter sounds
python3 tools/rom1_load.py --package build/rom1.fwsc FONT.sf2    # yours: converted, installed, region B sent
python3 tools/fm1_install.py --info                              # identity of the connected FM-1
```

Installing firmware is at your own risk. Hold OCT− and OCT+ for 5 seconds for Felucca's update
mode. If the FM-1 no longer starts but reaches the chip's update mode (4C4A:8057 on USB),
`bash tools/fm1_rescue.sh` puts stock firmware back from a Mac; otherwise recovery needs
[FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).

## On GitHub: builds, releases, the site

Three workflows in `.github/workflows/` (they work the same in a fork):

- **Build and test** (every push and pull request): the toolchain and the SDK files fetched (and cached), the
  firmware built, `tests/run_tests.sh` run (with the browser's side), the package kept as an artifact.
- **Release** (a tag `vX.Y.Z`): the tests, then `./build.sh --release X.Y.Z` (identity `FM-1_0XXYYZZ`, the
  version on the FM-1's start screen and BANK page), and a GitHub release with `rom1-X.Y.Z.fwsc`, its notes
  taken from `CHANGELOG.md`.
- **Pages** (after a release, or by hand): the site for the latest release on GitHub Pages: the landing page,
  the browser emulator, the web installer and the "Your sounds" page with the release's package, the manual,
  and the complete source as a tarball (as the GPL asks). In a fork, set **Settings → Pages → Source** to
  **GitHub Actions** first.

To release: add the version's section to `CHANGELOG.md`, then `git tag vX.Y.Z && git push origin vX.Y.Z`.

The same by hand:

1. `./build.sh --release X.Y.Z` (the identity, FM-1_0XXYYZZ, is what the installers check; SEQ-4's is
   FM-1_1..., CRT-4's FM-1_2..., Polly's FM-1_3..., LCD-4's FM-1_4..., IND-1's FM-1_5..., CTL-1's FM-1_6...,
   AMB-1's FM-1_7..., FiMba's FM-1_8..., X0X's FM-1_9...).
2. `web/emu/build.sh` (needs Emscripten) for the browser emulator and the converter in WebAssembly.
3. `[ROM1_REPO=https://...] python3 tools/make_pages.py build/rom1-X.Y.Z.fwsc X.Y.Z OUT_DIR`: the site in a
   static folder, for any web server. The installers need it served over HTTPS (Web MIDI). The manual's
   source is `docs/MANUAL.md` (`tools/manual_html.py` turns it into the page).
4. `tools/readme_gif.py` remakes the README's animation, `tools/manual_shots.sh` the manual's screenshots.

## On the FM-1: how it's doing

`python3 tools/fm1_status.py load` asks a connected FM-1 running ROM-1 for its load, the voices, the screen's
frames and the MIDI coming in; `bank` what bank it has (and whether region B is there); `bench` plays test
chords and measures them (it needs a bench build). Apart from `bench`, it only asks; it never changes anything
on the FM-1.
