#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A small SoundFont 2 writer: ROM-1's starter sounds (tools/starter/make_starter.py) and the tests' fonts
are made with it. Samples are 16-bit mono; instruments are zones over samples; presets are zones over
instruments; generators by their SoundFont numbers (GEN below).

    sf = SF2("Name")
    s = sf.sample("sine", data, rate=44100, root=69, loop=(0, 100))
    i = sf.instrument("Sine", [dict(sample=s, keys=(0, 127), gens={GEN.sampleModes: 1})])
    sf.preset("Sine", bank=0, prog=0, zones=[dict(inst=i)])
    sf.save("out.sf2")
"""
import struct


class GEN:
    startAddrsOffset, endAddrsOffset, startloopAddrsOffset, endloopAddrsOffset = 0, 1, 2, 3
    modLfoToPitch, vibLfoToPitch, modEnvToPitch, initialFilterFc, initialFilterQ = 5, 6, 7, 8, 9
    modLfoToFilterFc, modEnvToFilterFc, modLfoToVolume = 10, 11, 13
    chorusEffectsSend, reverbEffectsSend, pan = 15, 16, 17
    delayModLFO, freqModLFO, delayVibLFO, freqVibLFO = 21, 22, 23, 24
    delayModEnv, attackModEnv, holdModEnv, decayModEnv, sustainModEnv, releaseModEnv = 25, 26, 27, 28, 29, 30
    delayVolEnv, attackVolEnv, holdVolEnv, decayVolEnv, sustainVolEnv, releaseVolEnv = 33, 34, 35, 36, 37, 38
    keynumToVolEnvHold, keynumToVolEnvDecay = 39, 40
    instrument, keyRange, velRange, keynum, velocity, initialAttenuation = 41, 43, 44, 46, 47, 48
    coarseTune, fineTune, sampleID, sampleModes, scaleTuning, exclusiveClass, overridingRootKey = 51, 52, 53, 54, 56, 57, 58


def _name(s, n=20):
    b = s.encode("latin1", "replace")[:n]
    return b + b"\0" * (n - len(b))


def _chunk(cid, data):
    return cid + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")


def _gen(op, v):
    if op in (GEN.keyRange, GEN.velRange):
        return struct.pack("<HBB", op, v[0], v[1])
    if op in (GEN.instrument, GEN.sampleID):
        return struct.pack("<HH", op, v)
    return struct.pack("<Hh", op, int(v))


class SF2:
    def __init__(self, name):
        self.name = name
        self.samples = []          # (name, start, end, ls, le, rate, root, corr)
        self.data = []
        self.insts = []            # (name, [zones]); a zone: (gens dict incl. keyRange, sampleID)
        self.presets = []          # (name, bank, prog, [zones])

    def sample(self, name, data, rate=44100, root=60, loop=None, corr=0):
        start = len(self.data)
        self.data.extend(max(-32768, min(32767, int(round(x)))) for x in data)
        end = len(self.data)
        self.data.extend([0] * 46)                     # the 46 zeros the spec wants after each sample
        ls, le = (start + loop[0], start + loop[1]) if loop else (start, start)
        self.samples.append((name, start, end, ls, le, int(rate), root, corr))
        return len(self.samples) - 1

    def instrument(self, name, zones, glob=None):
        zs = []
        if glob:
            zs.append(dict(glob))
        for z in zones:
            g = dict(z.get("gens", {}))
            if "keys" in z:
                g[GEN.keyRange] = z["keys"]
            if "vels" in z:
                g[GEN.velRange] = z["vels"]
            g[GEN.sampleID] = z["sample"]
            zs.append(g)
        self.insts.append((name, zs))
        return len(self.insts) - 1

    def preset(self, name, bank=0, prog=0, zones=None, glob=None):
        zs = []
        if glob:
            zs.append(dict(glob))
        for z in zones or []:
            g = dict(z.get("gens", {}))
            if "keys" in z:
                g[GEN.keyRange] = z["keys"]
            g[GEN.instrument] = z["inst"]
            zs.append(g)
        self.presets.append((name, bank, prog, zs))

    @staticmethod
    def _ordered(g):
        """generators in the order the spec asks: key range, velocity range first; instrument/sample last"""
        last = [op for op in (GEN.instrument, GEN.sampleID) if op in g]
        first = [op for op in (GEN.keyRange, GEN.velRange) if op in g]
        mid = sorted(op for op in g if op not in last and op not in first)
        return [(op, g[op]) for op in first + mid + last]

    def save(self, path):
        info = _chunk(b"ifil", struct.pack("<HH", 2, 1)) + _chunk(b"isng", b"EMU8000\0") + \
            _chunk(b"INAM", self.name.encode("latin1") + b"\0")
        smpl = struct.pack("<%dh" % len(self.data), *self.data)
        sdta = _chunk(b"smpl", smpl)
        # presets
        phdr, pbag, pgen, pmod = b"", b"", b"", b""
        bag_i = gen_i = 0
        for name, bank, prog, zs in self.presets:
            phdr += _name(name) + struct.pack("<HHHIII", prog, bank, bag_i, 0, 0, 0)
            for g in zs:
                pbag += struct.pack("<HH", gen_i, 0)
                for op, v in self._ordered(g):
                    pgen += _gen(op, v)
                    gen_i += 1
                bag_i += 1
        phdr += _name("EOP") + struct.pack("<HHHIII", 0, 0, bag_i, 0, 0, 0)
        pbag += struct.pack("<HH", gen_i, 0)
        pgen += struct.pack("<HH", 0, 0)
        pmod = b"\0" * 10
        inst, ibag, igen = b"", b"", b""
        bag_i = gen_i = 0
        for name, zs in self.insts:
            inst += _name(name) + struct.pack("<H", bag_i)
            for g in zs:
                ibag += struct.pack("<HH", gen_i, 0)
                for op, v in self._ordered(g):
                    igen += _gen(op, v)
                    gen_i += 1
                bag_i += 1
        inst += _name("EOI") + struct.pack("<H", bag_i)
        ibag += struct.pack("<HH", gen_i, 0)
        igen += struct.pack("<HH", 0, 0)
        imod = b"\0" * 10
        shdr = b""
        for name, start, end, ls, le, rate, root, corr in self.samples:
            shdr += _name(name) + struct.pack("<IIIIIBbHH", start, end, ls, le, rate, root, corr, 0, 1)
        shdr += _name("EOS") + struct.pack("<IIIIIBbHH", 0, 0, 0, 0, 0, 0, 0, 0, 0)
        pdta = b"".join(_chunk(c, d) for c, d in ((b"phdr", phdr), (b"pbag", pbag), (b"pmod", pmod), (b"pgen", pgen),
                                                   (b"inst", inst), (b"ibag", ibag), (b"imod", imod), (b"igen", igen),
                                                   (b"shdr", shdr)))
        body = b"sfbk" + _chunk(b"LIST", b"INFO" + info) + _chunk(b"LIST", b"sdta" + sdta) + \
            _chunk(b"LIST", b"pdta" + pdta)
        with open(path, "wb") as f:
            f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
