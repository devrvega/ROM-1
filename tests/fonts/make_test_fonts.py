#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The tests' SoundFonts, made here (no third-party sounds in the tests): tones of known pitch, loops of
several kinds, a layered preset, a drum kit with an exclusive class.
    make_test_fonts.py OUTDIR"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
from sf2write import SF2, GEN  # noqa: E402


def sine(n, cycles, amp=12000.0):
    return [amp * math.sin(2 * math.pi * cycles * i / n) for i in range(n)]


def main(out):
    os.makedirs(out, exist_ok=True)
    sf = SF2("ROM-1 test tones")
    s1 = sf.sample("A440 44k", sine(100, 1), rate=44000, root=69, loop=(0, 100))           # 440 Hz at key 69
    s2 = sf.sample("400Hz 32k", sine(80, 1), rate=32000, root=67, loop=(0, 80), corr=35)   # 400 Hz, played 35 c up
    s3 = sf.sample("220Hz long", sine(4410, 22), rate=44100, root=57, loop=(2205, 4410))   # 220 Hz, a long loop (11 cycles)
    s4 = sf.sample("ping", [12000.0 * math.exp(-i / 2000.0) * math.sin(2 * math.pi * 440 * i / 44100) for i in range(8000)],
                   rate=44100, root=69)                                                     # no loop
    loop1 = {GEN.sampleModes: 1, GEN.releaseVolEnv: -3000}
    i1 = sf.instrument("Sine 440", [dict(sample=s1, gens=loop1)])
    i2 = sf.instrument("Sine 400", [dict(sample=s2, gens=loop1)])
    i3 = sf.instrument("Sine 220", [dict(sample=s3, gens=loop1)])
    i4 = sf.instrument("Ping", [dict(sample=s4)])
    i5 = sf.instrument("Layers", [dict(sample=s1, keys=(0, 63), gens=loop1), dict(sample=s3, keys=(64, 127), gens=loop1),
                                  dict(sample=s4, vels=(100, 127))])
    sf.preset("Sine 440", 0, 0, [dict(inst=i1)])
    sf.preset("Sine 400 32k", 0, 1, [dict(inst=i2)])
    sf.preset("Sine 220 loop", 0, 2, [dict(inst=i3)])
    sf.preset("Ping", 0, 3, [dict(inst=i4)])
    sf.preset("Layers", 0, 4, [dict(inst=i5)])
    sf.preset("Sine +12", 0, 5, [dict(inst=i1, gens={GEN.coarseTune: 12})])                # a preset's tuning adds
    # drums: hats that cut each other (exclusive class), a kick
    hat = sf.sample("hat", [9000.0 * math.sin(i * 1.7) * math.sin(i * 0.37) * math.exp(-i / 900.0) for i in range(3000)], rate=44100, root=60)
    kick = sf.sample("kick", [16000.0 * math.sin(2 * math.pi * (50 + 120 * math.exp(-i / 800.0)) * i / 44100) * math.exp(-i / 6000.0) for i in range(12000)], rate=44100, root=60)
    kit = sf.instrument("Kit", [dict(sample=kick, keys=(36, 36)), dict(sample=hat, keys=(42, 42), gens={GEN.exclusiveClass: 1}),
                                dict(sample=hat, keys=(46, 46), gens={GEN.exclusiveClass: 1, GEN.decayVolEnv: 2000, GEN.sustainVolEnv: 0})])
    sf.preset("Test Kit", 128, 0, [dict(inst=kit)])
    sf.save(os.path.join(out, "tones.sf2"))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
