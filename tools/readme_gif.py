#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""docs/img/rom1.gif: the README's animation, from the simulator. The starter sounds playing over MIDI (a
piano, a bass, strings, a pad, the drums), the meters moving; then the parts in turn, a preset changed, the
SOUND page; a frame every 100 ms.

  tools/readme_gif.py        (builds the simulator first; needs Pillow)"""
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

SRC = Path(__file__).resolve().parents[1]
STEP_MS = 100
# the starter's presets: 0 piano, 7 finger bass, 9 strings, 17 warm pad; part 10 the kit
SETUP = ["midi C1 21 00", "midi C2 30 00", "midi C3 59 00", "wait 50"]
CHORDS = [(48, [60, 64, 67]), (45, [60, 64, 69]), (41, [60, 65, 69]), (43, [62, 67, 71])]   # C, Am, F, G


def bar(root, notes):
    """a bar (eight frames of 100 ms, as eighths at 150 BPM): its chord held, the bass, a beat, the piano"""
    out = []
    for e in range(8):
        cmds = []
        if e == 0:
            cmds += [f"midi 92 {n:02X} 50" for n in notes] + [f"midi 93 {n + 12:02X} 3C" for n in notes]
        cmds.append(f"midi 99 {36 if e % 4 == 0 else 38 if e % 4 == 2 else 42:02X} {100 if e % 2 == 0 else 70:02X}")
        if e % 2 == 0:
            cmds.append(f"midi 91 {root - 12 + (7 if e == 4 else 0):02X} 64")
        cmds.append(f"midi 90 {notes[e % 3] + 12:02X} 5A")
        cmds.append("wait 40")
        cmds.append(f"midi 90 {notes[e % 3] + 12:02X} 00")
        if e % 2 == 1:
            cmds.append(f"midi 91 {root - 12 + (7 if e == 5 else 0):02X} 00")
        if e == 7:
            cmds += [f"midi 92 {n:02X} 00" for n in notes] + [f"midi 93 {n + 12:02X} 00" for n in notes]
        out.append((cmds, 1))
    return out


# what happens, and for how long: (commands before, frames); the waits inside a frame come off its 100 ms
PLAN = []
for i, (r, n) in enumerate(CHORDS * 2):
    extra = {2: ["turn ALGO 1"], 3: ["turn ALGO 1"], 4: ["turn ALGO 7"], 5: ["turn ALGO -9"],
             6: ["turn SELECT 1"], 7: ["turn SELECT -1"]}.get(i, [])
    b = bar(r, n)
    b[0] = (extra + b[0][0], 1)
    PLAN += b


def main():
    subprocess.run(["sh", str(SRC / "host" / "build_host.sh")], check=True, capture_output=True)
    with tempfile.TemporaryDirectory() as tmp:
        lines, n = SETUP + ["wait 300"], 0
        for cmds, frames in PLAN:
            lines += cmds
            waited = sum(int(c.split()[1]) for c in cmds if c.startswith("wait "))
            waited += 80 * sum(abs(int(c.split()[2])) for c in cmds if c.startswith("turn "))
            for _ in range(frames):
                lines += [f"wait {max(1, STEP_MS - waited)}", f"shot f{n:03d}.png"]
                waited = 0
                n += 1
        script = Path(tmp) / "gif.r1"
        script.write_text("\n".join(lines) + "\n")
        subprocess.run([str(SRC / "build" / "host" / "rom1_host"), str(script), tmp], check=True, capture_output=True)
        frames = [Image.open(Path(tmp) / f"f{i:03d}.png").convert("RGB") for i in range(n)]
    out = [f.quantize(colors=64, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE) for f in frames]
    dest = SRC / "docs" / "img" / "rom1.gif"
    out[0].save(dest, save_all=True, append_images=out[1:], duration=STEP_MS, loop=0, optimize=True)
    print(f"{dest}: {n} frames, {dest.stat().st_size // 1024} KB")


if __name__ == "__main__":
    sys.exit(main())
