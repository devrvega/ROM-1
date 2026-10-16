#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Put .sf2 sounds on an FM-1 running ROM-1, from the command line (the upload page does the same in the
browser): convert them to a bank that fits (tools/conv), put region A in a ROM-1 package and install it
(tools/rom1pkg.py, tools/fm1_install.py), then send region B over USB (ROM-1's SysEx 20-22).
Needs mido with python-rtmidi, and a C compiler for the converter (built once, into build/gen).

  rom1_load.py [--package ROM1.fwsc] [--port NAME] FONT.sf2[@BANK:PROG,...] ...
  rom1_load.py --region-b BANK_B.bin           region B only (the firmware already has its region A)

--package is a ROM-1 package to put the sounds in (default build/rom1.fwsc; a release's rom1-X.Y.Z.fwsc
works too). The fonts' presets are all taken unless @BANK:PROG,... names some (bank 128: drum kits)."""
import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import rom1pkg  # noqa: E402

HDR = [0x7D, 0x52, 0x31]


def put35(v):
    return [(v >> (7 * k)) & 0x7F for k in range(5)]


def get35(b, i=0):
    return sum(b[i + k] << (7 * k) for k in range(5))


def pack7(data):
    out = []
    for i in range(0, len(data), 7):
        grp = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << j for j, b in enumerate(grp)))
        out.extend(b & 0x7F for b in grp)
    return out


class Dev:
    def __init__(self, port=None):
        import mido
        self.mido = mido
        names = mido.get_output_names()
        name = next((n for n in names if (port or "FM-1").lower() in n.lower() or (not port and "ROM-1" in n)), None)
        if not name:
            sys.exit(f"rom1_load: no FM-1 MIDI port (ports: {names})")
        self.i = mido.open_input(name)
        self.o = mido.open_output(name)
        time.sleep(0.3)
        self.ask(1, timeout=1.0)                       # (the first message after opening can go astray)

    def ask(self, cmd, args=(), timeout=3.0):
        end = time.time() + 0.05
        while self.i.poll() is not None and time.time() < end:
            pass
        self.o.send(self.mido.Message("sysex", data=HDR + [cmd] + list(args)))
        end = time.time() + timeout
        while time.time() < end:
            m = self.i.poll()
            if m is not None and m.type == "sysex" and list(m.data[:4]) == HDR + [cmd]:
                return list(m.data[4:])
            if m is None:
                time.sleep(0.0005)
        return None

    def status(self):
        d = self.ask(1)
        if not d:
            return None
        keys = ["cpu", "worst_us", "late", "renders", "bench", "voices", "notes", "frames", "ui_us", "ui_max",
                "midi_in", "bank_id", "len_a", "len_b", "b_ok", "flash_ok", "write_rc", "boots", "vcap"]
        return {k: get35(d, 5 * i) for i, k in enumerate(keys) if 5 * i + 5 <= len(d)}

    def upload_b(self, data, bank_id, show=True):
        r = self.ask(0x20, put35(len(data)) + put35(bank_id))
        if not r or get35(r) != 0:
            sys.exit("rom1_load: the FM-1 did not begin region B (is it running ROM-1?)")
        t0 = time.time()
        for off in range(0, len(data), 256):
            chunk = data[off:off + 256]
            for _ in range(3):
                r = self.ask(0x21, put35(off) + pack7(chunk), timeout=2.0)
                if r and get35(r) == off:
                    break
            else:
                sys.exit(f"rom1_load: region B stopped at byte {off}")
            if show and (off // 256) % 64 == 0:
                print(f"\rregion B: {100 * off // len(data):3d} %", end="", flush=True)
        r = self.ask(0x22, timeout=10.0)
        ok = bool(r and get35(r) == 1)
        if show:
            print(f"\rregion B: {len(data)} B in {time.time() - t0:.1f} s, {'checked: ok' if ok else 'CHECK FAILED'}")
        return ok


def convert(fonts, room_a, room_b, name, out):
    conv = SRC / "build" / "gen" / "rom1conv"
    if not conv.exists():
        conv.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([os.environ.get("CC", "cc"), "-O2", "-std=c99", "-o", str(conv), str(SRC / "tools/conv/rom1conv.c"),
                        str(SRC / "tools/conv/conv.c"), "-lm"], check=True)
    r = subprocess.run([str(conv), "-a", str(room_a), "-b", str(room_b), "-n", name, "-o", str(out), *fonts],
                       capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode:
        sys.exit("rom1_load: " + r.stderr.strip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fonts", nargs="*")
    ap.add_argument("--package", type=Path, default=SRC / "build" / "rom1.fwsc")
    ap.add_argument("--port")
    ap.add_argument("--name", default="My sounds")
    ap.add_argument("--region-b", type=Path)
    a = ap.parse_args()
    if a.region_b:
        dev = Dev(a.port)
        st = dev.status()
        if not st or not st.get("len_a"):
            sys.exit("rom1_load: the FM-1 does not report a ROM-1 bank")
        sys.exit(0 if dev.upload_b(a.region_b.read_bytes(), st["bank_id"]) else 1)
    if not a.fonts:
        sys.exit(__doc__)
    info = rom1pkg.info(a.package)
    with tempfile.TemporaryDirectory() as tmp:
        convert(a.fonts, info["room"], 0x45000, a.name, Path(tmp))
        bank_a = (Path(tmp) / "bank_a.bin").read_bytes()
        bank_b = (Path(tmp) / "bank_b.bin").read_bytes()
        pkg = Path(tmp) / "rom1-sounds.fwsc"
        pkg.write_bytes(rom1pkg.patch(a.package.read_bytes(), bank_a))
        print("installing the firmware with region A ...")
        r = subprocess.run([sys.executable, str(SRC / "tools/fm1_install.py"), str(pkg), "--yes"] +
                           (["--port", a.port] if a.port else []))
        if r.returncode:
            sys.exit("rom1_load: the install did not finish")
        if not bank_b:
            print("done: everything fits in the firmware")
            return
        time.sleep(2.0)
        dev = Dev(a.port)
        bank_id = int.from_bytes(bank_a[16:20], "little")
        sys.exit(0 if dev.upload_b(bank_b, bank_id) else 1)


if __name__ == "__main__":
    main()
