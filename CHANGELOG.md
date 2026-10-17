# Changelog

Each release's notes come from its section here (`tools/release_notes.py`). The newest is first.

## 0.1.0 — 2026-10-17

The first release.

- **A sixteen-part sound module:** a part for each MIDI channel (channel 10 the drums), over USB and the TRS
  jack; each part a preset, with volume, pan, reverb and chorus sends, cutoff, resonance, attack and release.
- **.sf2 files, converted to fit:** about 740 KB for sounds, in the firmware and in the FM-1's data area. The
  converter keeps samples 16-bit where there is room and steps the biggest down first, to 4-bit ADPCM, then to
  lower rates, as much as needed; loops and pitch stay exact. Several fonts at once: bank select picks the
  font, program change its preset, channel 10 its kit.
- **Your sounds from the browser:** the "Your sounds" page reads .sf2 files on your computer (a piece at a time,
  so big files work), lets you pick presets with a meter of how full the FM-1 is and at what quality, converts
  them (the converter in WebAssembly), lets you try them in the emulator, then installs ROM-1 with them and
  writes the rest over USB, checked. `tools/rom1_load.py` does the same from the command line.
- **The engine:** up to 32 voices, cubic interpolation, a resonant low-pass filter, the .sf2 envelopes, LFOs and
  default modulators, exclusive classes; a stereo reverb (room, hall, large hall, plate) and a chorus.
- **On the panel:** a backlit module LCD (the parts' meters, the preset's name in big dots, the page's knobs);
  the keys play the part on screen; five pages on the knobs; audition, hold, mute and solo, part and GM reset,
  all notes off; a card for each button when held. The settings save themselves.
- **Starter sounds of its own:** eighteen General MIDI instruments and a drum kit, made by synthesis.
- **Measured on an FM-1:** about 63 µs a voice in each 2.9 ms render; sixteen parts each starting a note every
  30 ms take 70 % of the FM-1's time and at most 2.5 ms of a render, none late. A voice cap (28 to start, up
  to 32) follows the render time. The data area's part of a bank goes over USB at about 34 KB a second.
- **In the browser:** the same code, with the starter sounds, yours from the page, or .sf2 files loaded there.
