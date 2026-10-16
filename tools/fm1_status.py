#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Ask a connected FM-1 running ROM-1 how it's doing, over USB-MIDI (ROM-1's SysEx, F0 7D 52 31 .. F7,
described in firmware/src/app/main_fm1.c and BUILDING.md). Needs mido with python-rtmidi. Apart from
`bench`, nothing it sends changes anything on the FM-1.

  tools/fm1_status.py load [SECONDS]   the engine's load: %, its slowest render, renders late, the voices and
                                       the voices allowed; the screen's frames a second and their time; MIDI
                                       in; over SECONDS (default 5); play something meanwhile
  tools/fm1_status.py bank             the bank: its id, region A's length, region B's (there and right?)
  tools/fm1_status.py bench [SECONDS]  (a bench build: ./build.sh --bench) every part a four-note chord, held,
                                       then a chord a part and a beat; the load of each (then all notes off)

The engine renders 128 frames at a time (2902 us at 44.1 kHz); the FM-1 keeps up while the slowest render
stays under that. When a render takes over 85 % of it, the voices allowed come down (and go up again slowly)."""
import os
import sys
import time

import mido

HDR = [0x7D, 0x52, 0x31]
NAMES = ["cpu", "worst_us", "late", "renders", "bench", "voices", "notes", "frames", "ui_us", "ui_max", "midi_in",
         "bank_id", "len_a", "len_b", "b_ok", "flash_ok", "write_rc", "boots", "vcap"]
BUDGET_US = 2902


def r35(b, i):
    return sum(b[i + k] << (7 * k) for k in range(5))


class Dev:
    def __init__(self):
        names = mido.get_output_names()
        name = next((n for n in names if "ROM-1" in n), None) or next((n for n in names if "FM-1" in n), None)
        if not name:
            sys.exit(f"fm1_status: no FM-1 MIDI port (ports: {names})")
        self.i = mido.open_input(name)
        self.o = mido.open_output(name)
        time.sleep(0.3)
        self.ask(1, timeout=1.0, must=False)     # (the first message after opening can go astray)

    def drain(self, secs=0.1):
        end = time.time() + secs
        while self.i.poll() is not None and time.time() < end:
            pass

    def ask(self, cmd, args=(), reply=True, timeout=2.0, must=True):
        self.drain()
        self.o.send(mido.Message("sysex", data=HDR + [cmd] + list(args)))
        if not reply:
            time.sleep(0.15)                     # the FM-1 takes one SysEx at a time
            return None
        end = time.time() + timeout
        while time.time() < end:
            m = self.i.poll()
            if m is not None and m.type == "sysex" and list(m.data[:4]) == HDR + [cmd]:
                return list(m.data[4:])
            if m is None:
                time.sleep(0.002)
        if not must:
            return None
        sys.exit("fm1_status: no answer (is it ROM-1?)")

    def status(self):
        d = self.ask(1)
        return {n: r35(d, 5 * k) for k, n in enumerate(NAMES) if 5 * k + 5 <= len(d)}


def load(dev, secs, label=""):
    dev.ask(3, reply=False)                      # the slowest and the late counted again
    s0 = dev.status()
    time.sleep(secs)
    s1 = dev.status()
    fr = max(s1["frames"] - s0["frames"], 1)
    print(f"{label}engine {s1['cpu']} %, slowest render {s1['worst_us']} us of {BUDGET_US}, late {s1['late']}, "
          f"{(s1['renders'] - s0['renders']) / secs:.0f} renders a second; voices {s1['voices']} "
          f"(allowed {s1.get('vcap', '?')}), {s1['notes'] - s0['notes']} notes")
    print(f"{' ' * len(label)}screen {fr / secs:.1f} frames a second, {(s1['ui_us'] - s0['ui_us']) / fr / 1000:.2f} ms a "
          f"frame, slowest {s1['ui_max'] / 1000:.2f} ms; MIDI in {s1['midi_in'] - s0['midi_in']}")


def bank(dev):
    s = dev.status()
    if not s["len_a"]:
        print("no bank in the firmware (region A missing or damaged)")
        return
    b = "none needed" if not s["len_b"] else (f"{s['len_b']} B, there and right" if s["b_ok"] else
                                               f"{s['len_b']} B expected, NOT THERE (send it: tools/rom1_load.py --region-b)")
    print(f"bank {s['bank_id']:#010x}: region A {s['len_a']} B in the firmware; region B {b}")
    print(f"flash {'ok' if s['flash_ok'] else 'NOT FOUND'}, the last write {s['write_rc'] - 100}, boots {s['boots']}")


def bench(dev, secs):
    if not dev.status().get("bench"):
        sys.exit("fm1_status: bench needs a bench build (./build.sh --bench)")
    try:
        dev.ask(2, [0], reply=False)
        load(dev, secs, "16 chords held: ")
        dev.ask(6, reply=False)
        time.sleep(0.5)
        dev.ask(2, [1], reply=False)
        load(dev, secs, "a chord a part, a beat: ")
    finally:
        dev.ask(6, reply=False)


def main():
    a = sys.argv[1:] or ["load"]
    if a[0] not in ("load", "bank", "bench"):
        sys.exit(__doc__)
    dev = Dev()
    if a[0] == "load":
        load(dev, float(a[1]) if len(a) > 1 else 5.0)
    elif a[0] == "bank":
        bank(dev)
    else:
        bench(dev, float(a[1]) if len(a) > 1 else 3.0)
    sys.stdout.flush()
    os._exit(0)                                  # (closing a port while a lot of MIDI comes in can hang)


main()
