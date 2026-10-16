# ROM-1 licensing

ROM-1 is free software under the GNU General Public License, version 3 only
(`GPL-3.0-only`, full text in `LICENSE`). It is built on SEQ-4, CRT-4, LCD-4, IND-1 and AMB-1, which are built
on Felucca's platform layer (through X0X and FoMni), and keeps their licence. If you distribute ROM-1, or
firmware derived from it, you must give your recipients its complete corresponding source under the same
licence (`tools/make_pages.py` puts a tarball of the source on the site it makes, next to the firmware).

## Where the code comes from

| What | Origin | Licence |
| --- | --- | --- |
| Platform: `firmware/hal/`, `firmware/loader/`, `firmware/src/{libc,lcd,gfx,usb,storage,ota,midi_uart}.c`, `tools/` (build, package, install, rescue), `web/fm1*.js` | [Felucca](https://github.com/hugelton/Felucca), Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, as changed for [X0X](https://github.com/charlesvestal/fm1-x0x), FoMni and [AMB-1](https://github.com/charlesvestal/fm1-amb), then IND-1, LCD-4, CRT-4, SEQ-4 and ROM-1 | GPL-3.0-only |
| `firmware/src/app/{panel,plat_fm1,main_fm1}.c`, the frame of `firmware/src/app/ui.c` (input, knobs, help cards, lights), `host/rom1_host.c`, `web/emu/`, `web/rom1_installer.html`, `tools/{make_pages,manual_html,fm1_status,readme_gif,gen_manual_figures}.py` | adapted from SEQ-4, CRT-4, LCD-4, IND-1 and AMB-1 (themselves from X0X, from Felucca's `panel.c`, `main.c`, `audio.c`) | GPL-3.0-only |
| The idea of a browser emulator for FM-1 firmware (`web/emu/`) | [Groove OS](https://www.groove-os.com/) by Peter Gombos, whose site runs its firmware in the browser; no code from it | credit only |
| Everything else: the engine (`firmware/src/synth/`), the bank format, the converter (`tools/conv/`, `web/rom1conv.js`), the package patcher (`tools/rom1pkg.py`, `web/rom1pkg.js`), the upload (`firmware/src/app/upload.c`, `web/rom1dev.js`, `tools/rom1_load.py`), the "Your sounds" page, the UI and its dot-matrix face (`tools/gen_dotfont.py`), the starter sounds (`tools/starter/`), `tests/` | ROM-1 | GPL-3.0-only |

The engine follows the published .sf2 file format's description of how its presets are to be played
(generators, envelopes, default modulators); no code of any other player is in it.

## Sounds

- **The starter sounds** (`tools/starter/make_starter.py`, built into the firmware) are ROM-1's own: made by
  that script from synthesis, no recordings and nothing from anyone's .sf2 file. They are part of ROM-1, under
  its licence.
- **The tests' fonts** (`tests/fonts/make_test_fonts.py`) are made by that script, likewise.
- **Your .sf2 files** are not part of ROM-1. The "Your sounds" page and `tools/rom1_load.py` convert them on
  your computer and put them on your FM-1; nothing of them is in this tree or on the site. Their samples belong
  to whoever made them, and their licences say what you may do with them.

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Barlow Semi Condensed (The Barlow Project Authors), the start and crash screens' face | SIL OFL 1.1 | `assets/fonts/BarlowSemiCondensed-*.ttf`, `assets/fonts/Barlow-OFL.txt` |
| Terminus (Dimitar Toshkov Zhekov), an alternative font set | SIL OFL 1.1 | `assets/fonts/ter-u*.bdf`, `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments. "M-VAVE" and "FM-1" are trademarks
of their respective owners. ROM-1 is independent firmware; it is not affiliated with, endorsed by or supported
by any of them. Their names are used only to credit where code comes from and to say what ROM-1 runs on.

## Radio

ROM-1 never enables the Bluetooth / Wi-Fi radio of the hardware.
