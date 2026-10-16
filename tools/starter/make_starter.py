#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""ROM-1's starter sounds: a small General MIDI-numbered set, every sample made here by synthesis (no
recordings, nothing from anyone's SoundFont), written as a SoundFont (tools/sf2write.py) that the converter
turns into the bank the firmware starts with.

    make_starter.py OUT.sf2

Pianos, plucks and bells are rendered whole from partials (or a plucked string); organs, leads and brass
are single cycles; strings, pads and the choir are long loops whose every voice repeats exactly in the loop
(so they join without a click); the drums are synthesized hits. Deterministic: the same file every time."""
import math
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from sf2write import SF2, GEN  # noqa: E402

TAU = 2 * math.pi
R = 32000                                   # most samples' rate


def norm(x, peak=0.89):
    m = max(1e-9, max(abs(v) for v in x))
    return [v * peak * 32767.0 / m for v in x]


def key_hz(k):
    return 440.0 * 2 ** ((k - 69) / 12.0)


def cents(f, k):
    """the correction (cents) that makes a sample of pitch f play as key k"""
    return int(round(-1200.0 * math.log2(f / key_hz(k))))


def partials(f, secs, parts, rate=R, inharm=0.0):
    """sum of sines: parts = [(harmonic, amplitude, decay seconds)]; stretched by inharm (a piano's)"""
    n = int(secs * rate)
    out = [0.0] * n
    for h, a, d in parts:
        fh = f * h * math.sqrt(1.0 + inharm * h * h)
        if fh >= rate * 0.45:
            continue
        w, k = TAU * fh / rate, math.exp(-1.0 / (d * rate))
        g = a
        for i in range(n):
            out[i] += g * math.sin(w * i)
            g *= k
    return out


def tail_loop(x, f, rate=R, cycles=8):
    """a loop at the tail: a whole number of the fundamental's cycles, ending at the sample's end"""
    n = len(x)
    L = int(round(cycles * rate / f))
    return (n - L, n)


def cycle(n, harm):
    """one cycle of n samples: harm = [(harmonic, amplitude)] (only those under Nyquist)"""
    return [sum(a * math.sin(TAU * h * i / n) for h, a in harm if h < n / 2) for i in range(n)]


def saw_h(n, maxh=None, tilt=1.0):
    return [(h, 1.0 / h ** tilt) for h in range(1, (maxh or n // 2))]


def ensemble(f0, secs, detune, harm_fn, rate=R):
    """voices at exact multiples of 1/secs Hz (so all repeat in the loop), bandlimited"""
    n = int(secs * rate)
    res = 1.0 / secs
    out = [0.0] * n
    for d in detune:
        f = round(f0 * 2 ** (d / 1200.0) / res) * res
        hs = harm_fn(f)
        ph = random.Random(int(f * 7)).random()
        for h, a in hs:
            fh = f * h
            if fh >= rate * 0.45:
                break
            w = TAU * fh / rate
            p = TAU * ph * h
            for i in range(n):
                out[i] += a * math.sin(w * i + p)
    return out


def ks(f, secs, rate=R, damp=0.996, bright=0.5, seed=1):
    """a plucked string (Karplus-Strong), its period rounded: the correction says how far"""
    rnd = random.Random(seed)
    N = int(round(rate / f))
    buf = [rnd.uniform(-1, 1) for _ in range(N)]
    for _ in range(2):                                 # (a softer pluck)
        buf = [(buf[i] + buf[i - 1]) * 0.5 for i in range(N)]
    out, i = [], 0
    for _ in range(int(secs * rate)):
        a, b = buf[i % N], buf[(i + 1) % N]
        v = damp * (bright * a + (1 - bright) * (a + b) * 0.5)
        buf[i % N] = v
        out.append(a)
        i += 1
    return out, rate / N


def noise(n, seed):
    rnd = random.Random(seed)
    return [rnd.uniform(-1, 1) for _ in range(n)]


def hp1(x, a=0.9):
    y, px, py = [], 0.0, 0.0
    for v in x:
        py = a * (py + v - px)
        px = v
        y.append(py)
    return y


def lp1(x, a=0.5):
    y, s = [], 0.0
    for v in x:
        s += a * (v - s)
        y.append(s)
    return y


def env(x, decay, rate=R, attack=0.0):
    k = math.exp(-1.0 / (decay * rate))
    g, out = 1.0, []
    na = int(attack * rate)
    for i, v in enumerate(x):
        out.append(v * g * (i / na if i < na else 1.0))
        g *= k
    return out


def make(out):
    sf = SF2("ROM-1 Starter")
    L1 = GEN.sampleModes

    def single(name, harm, root=60, n=128):
        f = key_hz(root)
        return sf.sample(name, norm(cycle(n, harm), 0.7), rate=int(round(f * n)), root=root, loop=(0, n))

    def inst(name, zones, **kw):
        return sf.instrument(name, zones, **kw)

    def preset(name, prog, i, bank=0, **kw):
        sf.preset(name, bank, prog, [dict(inst=i, **kw)])

    # ---- keys
    pz = []
    for k, lo, hi in ((48, 0, 54), (60, 55, 66), (72, 67, 127)):
        f = key_hz(k)
        parts = [(h, 1.0 / h ** 1.3, 2.4 / h ** 0.7) for h in range(1, 14)]
        x = norm(env(partials(f, 1.6, parts, inharm=0.0004), 3.0))
        s = sf.sample("piano %d" % k, x, rate=R, root=k, loop=tail_loop(x, f))
        pz.append(dict(sample=s, keys=(lo, hi), gens={L1: 1, GEN.decayVolEnv: 2400, GEN.sustainVolEnv: 1000,
                                                       GEN.releaseVolEnv: -1500, GEN.keynumToVolEnvDecay: 30}))
    preset("Grand Piano", 0, inst("Piano", pz))
    ez = []
    for k, lo, hi in ((48, 0, 60), (72, 61, 127)):
        f = key_hz(k)
        n = int(1.2 * R)
        x = []
        for i in range(n):
            t = i / R
            idx = 2.2 * math.exp(-t * 3.0)
            x.append(math.sin(TAU * f * t + idx * math.sin(TAU * f * t)) * math.exp(-t * 1.2))
        x = norm(x)
        s = sf.sample("epiano %d" % k, x, rate=R, root=k, loop=tail_loop(x, f))
        ez.append(dict(sample=s, keys=(lo, hi), gens={L1: 1, GEN.decayVolEnv: 2000, GEN.sustainVolEnv: 1000,
                                                       GEN.releaseVolEnv: -1800}))
    preset("Electric Piano", 4, inst("E.Piano", ez), gens={GEN.chorusEffectsSend: 300})
    f = key_hz(72)
    x = norm(env(partials(f, 1.4, [(1, 1.0, 2.0), (4.0, 0.35, 0.4), (10.0, 0.1, 0.1)]), 4.0))
    s = sf.sample("vibes", x, rate=R, root=72, loop=tail_loop(x, f))
    preset("Vibraphone", 11, inst("Vibes", [dict(sample=s, gens={L1: 1, GEN.decayVolEnv: 2800, GEN.sustainVolEnv: 1000,
                                                                  GEN.releaseVolEnv: -800, GEN.modLfoToVolume: 40,
                                                                  GEN.freqModLFO: -500})]))
    s = sf.sample("bell", norm(env(partials(key_hz(72), 2.0, [(1, 1, 1.8), (2.76, 0.6, 0.9), (5.4, 0.4, 0.5),
                                                               (8.93, 0.25, 0.3), (13.3, 0.12, 0.2)]), 3.0)), rate=R, root=72)
    preset("Tubular Bells", 14, inst("Bell", [dict(sample=s, gens={GEN.releaseVolEnv: 0})]))
    # ---- organs
    o1 = single("drawbar", [(1, 1.0), (2, 0.8), (3, 0.6), (4, 0.5), (6, 0.3), (8, 0.25)])
    preset("Drawbar Organ", 16, inst("Drawbar", [dict(sample=o1, gens={L1: 1, GEN.releaseVolEnv: -3000,
                                                                        GEN.modLfoToVolume: 20, GEN.freqModLFO: 600})]))
    o2 = single("pipe", [(1, 1.0), (2, 0.6), (3, 0.35), (4, 0.3), (5, 0.15), (6, 0.2), (8, 0.15), (10, 0.08), (12, 0.06)])
    preset("Church Organ", 19, inst("Pipe", [dict(sample=o2, gens={L1: 1, GEN.attackVolEnv: -2400, GEN.releaseVolEnv: -1000,
                                                                    GEN.reverbEffectsSend: 300})]))
    # ---- plucked
    gz = []
    for k, lo, hi in ((52, 0, 58), (64, 59, 127)):
        x, fr = ks(key_hz(k), 1.6, damp=0.997, bright=0.35, seed=k)
        gz.append(dict(sample=sf.sample("nylon %d" % k, norm(x), rate=R, root=k, corr=cents(fr, k)), keys=(lo, hi),
                       gens={GEN.releaseVolEnv: -1800}))
    preset("Nylon Guitar", 24, inst("Nylon", gz))
    x, fr = ks(key_hz(40), 1.8, damp=0.998, bright=0.2, seed=40)
    sb = sf.sample("bass", norm(lp1(x, 0.35)), rate=R, root=40, corr=cents(fr, 40))
    preset("Finger Bass", 33, inst("Bass", [dict(sample=sb, gens={GEN.releaseVolEnv: -2000})]))
    saw = single("saw", saw_h(128, 40), root=48)
    preset("Synth Bass", 38, inst("Synth Bass", [dict(sample=saw, gens={L1: 1, GEN.initialFilterFc: 5000, GEN.initialFilterQ: 120,
                                                                          GEN.modEnvToFilterFc: 3600, GEN.decayModEnv: -1200,
                                                                          GEN.sustainModEnv: 1000, GEN.releaseVolEnv: -3000,
                                                                          GEN.coarseTune: -12})]))
    # ---- ensembles: long loops, every voice periodic in them
    random.seed(5)
    st = norm(ensemble(key_hz(60), 0.5, (-14, -6, 0, 5, 12), lambda f: saw_h(64, 30, 1.15)))
    ss = sf.sample("strings", st, rate=R, root=60, loop=(0, len(st)))
    strings = inst("Strings", [dict(sample=ss, gens={L1: 1, GEN.attackVolEnv: -1800, GEN.releaseVolEnv: -800,
                                                      GEN.initialFilterFc: 9000, GEN.reverbEffectsSend: 250,
                                                      GEN.vibLfoToPitch: 6, GEN.freqVibLFO: 150})])
    preset("Strings", 48, strings)
    preset("Warm Pad", 89, inst("Pad", [dict(sample=ss, gens={L1: 1, GEN.attackVolEnv: 0, GEN.releaseVolEnv: 600,
                                                               GEN.initialFilterFc: 6000, GEN.chorusEffectsSend: 500,
                                                               GEN.reverbEffectsSend: 400})]))
    # a choir: pulses through three formants ("ah"), voices periodic in the loop
    n, rate = int(0.4 * R), R
    ch = [0.0] * n
    for d in (-8, 0, 7):
        f = round(key_hz(60) * 2 ** (d / 1200.0) / 2.5) * 2.5
        for h in range(1, int(rate * 0.45 / f)):
            fh = f * h
            a = sum(g / (1 + ((fh - fo) / bw) ** 2) for fo, bw, g in ((700, 110, 1.0), (1220, 120, 0.5), (2600, 160, 0.25)))
            w = TAU * fh / rate
            p = random.random() * TAU
            for i in range(n):
                ch[i] += a * math.sin(w * i + p)
    chs = sf.sample("choir", norm(ch), rate=R, root=60, loop=(0, n))
    preset("Choir Aahs", 52, inst("Choir", [dict(sample=chs, gens={L1: 1, GEN.attackVolEnv: -1600, GEN.releaseVolEnv: -600,
                                                                    GEN.vibLfoToPitch: 10, GEN.freqVibLFO: 200,
                                                                    GEN.delayVibLFO: -1200, GEN.reverbEffectsSend: 350})]))
    # ---- brass, reeds, pipes, leads
    br = single("brass", saw_h(128, 40, 0.9), root=60)
    preset("Trumpet", 56, inst("Trumpet", [dict(sample=br, gens={L1: 1, GEN.initialFilterFc: 6500, GEN.modEnvToFilterFc: 2400,
                                                                  GEN.attackModEnv: -3600, GEN.decayModEnv: -1500,
                                                                  GEN.sustainModEnv: 400, GEN.attackVolEnv: -4000,
                                                                  GEN.releaseVolEnv: -2400, GEN.vibLfoToPitch: 12,
                                                                  GEN.delayVibLFO: -1000, GEN.freqVibLFO: 300})]))
    preset("Brass Section", 61, inst("Brass", [dict(sample=br, gens={L1: 1, GEN.initialFilterFc: 5500, GEN.modEnvToFilterFc: 3000,
                                                                      GEN.attackModEnv: -2400, GEN.decayModEnv: -1000,
                                                                      GEN.sustainModEnv: 500, GEN.attackVolEnv: -2400,
                                                                      GEN.releaseVolEnv: -2000, GEN.chorusEffectsSend: 400}),
                                                dict(sample=br, gens={L1: 1, GEN.fineTune: 9, GEN.initialFilterFc: 5500,
                                                                      GEN.attackVolEnv: -2400, GEN.releaseVolEnv: -2000,
                                                                      GEN.initialAttenuation: 30})]))
    cl = single("clarinet", [(h, 1.0 / h) for h in range(1, 24, 2)], root=60)
    preset("Clarinet", 71, inst("Clarinet", [dict(sample=cl, gens={L1: 1, GEN.initialFilterFc: 7000, GEN.attackVolEnv: -3600,
                                                                    GEN.releaseVolEnv: -2400, GEN.vibLfoToPitch: 8,
                                                                    GEN.delayVibLFO: -800, GEN.freqVibLFO: 250})]))
    n = int(0.3 * R)
    f = round(key_hz(72) / (1 / 0.3)) * (1 / 0.3)
    fl = [math.sin(TAU * f * i / R) + 0.25 * math.sin(TAU * 2 * f * i / R) for i in range(n)]
    br_ = hp1(lp1(noise(n, 9), 0.3), 0.95)
    br_ = br_[:]                                        # (breath: made periodic by a cross-fade)
    m = n // 8
    for i in range(m):
        a = i / m
        br_[i] = br_[i] * a + br_[n - m + i] * (1 - a)
    flute = [v + 0.08 * b for v, b in zip(fl, br_[:n - m] + br_[n - m:])]
    fs = sf.sample("flute", norm(flute), rate=R, root=72, loop=(0, n), corr=cents(f, 72))
    preset("Flute", 73, inst("Flute", [dict(sample=fs, gens={L1: 1, GEN.attackVolEnv: -3000, GEN.releaseVolEnv: -2400,
                                                              GEN.vibLfoToPitch: 10, GEN.delayVibLFO: -600, GEN.freqVibLFO: 280,
                                                              GEN.reverbEffectsSend: 200})]))
    sq = single("square", [(h, 1.0 / h) for h in range(1, 40, 2)], root=60)
    preset("Square Lead", 80, inst("Square", [dict(sample=sq, gens={L1: 1, GEN.releaseVolEnv: -2400, GEN.vibLfoToPitch: 15,
                                                                     GEN.delayVibLFO: -500, GEN.freqVibLFO: 350})]))
    preset("Saw Lead", 81, inst("Saw", [dict(sample=saw, gens={L1: 1, GEN.releaseVolEnv: -2400, GEN.initialFilterFc: 9000,
                                                                GEN.vibLfoToPitch: 15, GEN.delayVibLFO: -500,
                                                                GEN.freqVibLFO: 350, GEN.chorusEffectsSend: 300})]))
    # ---- drums: bank 128, program 0 (General MIDI's keys)
    D = 44100

    def hit(name, x, root=60):
        return sf.sample(name, norm(x, 0.9), rate=D, root=root)

    def sweep(f0, f1, k, secs, dec):
        out, ph = [], 0.0
        for i in range(int(secs * D)):
            t = i / D
            ph += TAU * (f1 + (f0 - f1) * math.exp(-t * k)) / D
            out.append(math.sin(ph) * math.exp(-t / dec))
        return out
    kick = hit("kick", [v + (0.6 * math.exp(-i / 40.0) if i < 200 else 0) for i, v in enumerate(sweep(160, 48, 30, 0.5, 0.18))])
    nz = noise(int(0.3 * D), 2)
    snare = hit("snare", [0.6 * (math.sin(TAU * 185 * i / D) + 0.6 * math.sin(TAU * 330 * i / D)) * math.exp(-i / (0.05 * D)) +
                          a * math.exp(-i / (0.07 * D)) for i, a in enumerate(hp1(nz, 0.85))])
    stick = hit("stick", [math.sin(TAU * 1700 * i / D) * math.exp(-i / 180.0) + n_ * math.exp(-i / 90.0)
                          for i, n_ in enumerate(noise(int(0.08 * D), 3))])
    cl_n = hp1(noise(int(0.4 * D), 4), 0.8)
    clap = hit("clap", [a * (sum(math.exp(-max(0, i - o) / 250.0) * (i >= o) for o in (0, 500, 1000)) * 0.6 +
                             math.exp(-max(0, i - 1500) / (0.09 * D)) * (i >= 1500)) for i, a in enumerate(cl_n)])
    metal = [0.0] * int(1.4 * D)
    for fq in (205.3, 304.4, 369.6, 522.7, 540.0, 800.0):  # six square waves, inharmonic (the classic cymbal's)
        for i in range(len(metal)):
            metal[i] += 1.0 if math.sin(TAU * fq * 1.5 * i / D) > 0 else -1.0
    metal = hp1(hp1(metal, 0.7), 0.7)
    hat_c = hit("hat closed", [v * math.exp(-i / (0.025 * D)) for i, v in enumerate(metal[:int(0.15 * D)])])
    hat_o = hit("hat open", [v * math.exp(-i / (0.25 * D)) for i, v in enumerate(metal[:int(0.7 * D)])])
    crash_n = hp1(noise(len(metal), 5), 0.6)
    crash = hit("crash", [(0.6 * v + 0.8 * c) * math.exp(-i / (0.45 * D)) for i, (v, c) in enumerate(zip(metal, crash_n))])
    ride = hit("ride", [(0.8 * v + 0.15 * c + 0.5 * math.sin(TAU * 3100 * i / D)) * math.exp(-i / (0.6 * D))
                        for i, (v, c) in enumerate(zip(metal, crash_n))])
    tom = hit("tom", sweep(220, 140, 18, 0.6, 0.2), root=60)
    tamb = hit("tambourine", [v * (math.exp(-i / 900.0) + 0.5 * math.exp(-max(0, i - 2600) / 1500.0) * (i >= 2600))
                              for i, v in enumerate(hp1(noise(int(0.3 * D), 6), 0.5))])
    cow = [(1 if math.sin(TAU * 545 * i / D) > 0 else -1) * 0.5 + (1 if math.sin(TAU * 815 * i / D) > 0 else -1) * 0.5
           for i in range(int(0.3 * D))]
    bell = hit("cowbell", [v * math.exp(-i / (0.08 * D)) for i, v in enumerate(lp1(hp1(cow, 0.9), 0.4))])
    E = GEN.exclusiveClass
    kz = [dict(sample=kick, keys=(35, 36)), dict(sample=stick, keys=(37, 37)), dict(sample=snare, keys=(38, 38)),
          dict(sample=clap, keys=(39, 39)), dict(sample=snare, keys=(40, 40), gens={GEN.coarseTune: 2}),
          dict(sample=hat_c, keys=(42, 42), gens={E: 1}), dict(sample=hat_c, keys=(44, 44), gens={E: 1, GEN.coarseTune: -2}),
          dict(sample=hat_o, keys=(46, 46), gens={E: 1}), dict(sample=crash, keys=(49, 49)), dict(sample=ride, keys=(51, 51)),
          dict(sample=tamb, keys=(54, 54)), dict(sample=bell, keys=(56, 56)), dict(sample=crash, keys=(57, 57), gens={GEN.coarseTune: 2}),
          dict(sample=ride, keys=(59, 59), gens={GEN.coarseTune: -2})]
    for k in (41, 43, 45, 47, 48, 50):                 # the toms, tuned across the keys
        kz.append(dict(sample=tom, keys=(k, k), gens={GEN.overridingRootKey: 60, GEN.coarseTune: (k - 47) * 2 // 3}))
    for z in kz:
        z.setdefault("gens", {})[GEN.releaseVolEnv] = 0
        z["gens"][GEN.reverbEffectsSend] = 150
    sf.preset("Standard Kit", 128, 0, [dict(inst=inst("Standard Kit", kz))])
    sf.save(out)


if __name__ == "__main__":
    make(sys.argv[1] if len(sys.argv) > 1 else "starter.sf2")
