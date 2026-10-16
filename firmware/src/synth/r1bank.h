/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1's bank: the sounds, as the converter (tools/conv) writes them from .sf2 files and the engine
 * (synth/rom1.c) plays them. Shared by both, and by the browser's converter.
 *
 * A bank is in two parts, each where the FM-1 can read it while playing:
 *   region A   inside the firmware image (the app's last part, from a 4 KiB boundary): this header, the
 *              fonts, presets, zones and samples' descriptions, and as much sample data as fits
 *   region B   the data area (flash 0x97000 .. 0xDBFFF, read through the plain XIP window): a 16-byte
 *              header and the rest of the sample data; written over USB after the firmware is installed
 * A preset's layers are flattened by the converter: a zone is everything one sample needs to sound for a
 * key and velocity range (SoundFont's preset and instrument generators combined), so the engine never
 * walks generator lists. Little-endian; every struct is 4-byte aligned. */
#pragma once
#include <stdint.h>

#define R1_MAGIC 0x4B423152u           /* "R1BK": region A */
#define R1_MAGIC_B 0x42423152u         /* "R1BB": region B */
#define R1_VERSION 1u
#define R1_REGION_B 0x80000000u        /* a sample's data offset: in region B */
#define R1_B_BASE 0x97000u             /* region B: flash offset, and its room */
#define R1_B_ROOM 0x45000u          /* (to 0xDBFFF: 0xDC000 holds the panel's calibration) */
#define R1_BLK 16u                     /* ADPCM: samples a block, bytes a block */
#define R1_BLKB 9u

enum { R1_PCM16, R1_ADPCM };

typedef struct {                       /* region A's header: 96 bytes */
    uint32_t magic;
    uint16_t version, nfont;
    uint32_t len;                      /* region A's bytes, this header on */
    uint32_t crc;                      /* CRC-32 of region A from byte 16 to len */
    uint32_t id;                       /* this bank (region B's header carries it too) */
    uint32_t lenb, crcb;               /* region B: its bytes (header on), its CRC-32 from byte 16; 0: none */
    uint32_t npreset, nzone, nsample, nparam;
    uint32_t off_font, off_preset, off_zone, off_param, off_sample;   /* from this header's start */
    char name[32];                     /* what the converter called it */
} r1_bank_t;

typedef struct {                       /* region B's header: 16 bytes */
    uint32_t magic, id, len, crc;      /* crc: from byte 16 to len */
} r1_bankb_t;

typedef struct {                       /* a font (one .sf2): 32 bytes */
    char name[24];
    uint16_t first, count;             /* its presets */
    uint32_t bytes;                    /* its sample data */
} r1_font_t;

typedef struct {                       /* a preset: 32 bytes; sorted by font, bank, program */
    char name[20];
    uint16_t bank;                     /* 0..127, 128 the drum kits */
    uint8_t prog, font;
    uint32_t zone;                     /* its first zone */
    uint16_t nzone, rsv;
} r1_preset_t;

typedef struct {                       /* a zone: one sample for a range of keys and velocities, 16 bytes */
    uint8_t klo, khi, vlo, vhi;
    uint16_t smp;                      /* its sample */
    uint8_t root;                      /* the key the sample plays at its own pitch */
    uint8_t mode;                      /* 0 once, 1 loops, 3 loops while held then plays on to its end */
    int16_t tune;                      /* cents: the coarse and fine tunings and the sample's correction */
    int16_t pan;                       /* -500 .. 500 (0.1 %) */
    int16_t atten;                     /* centibels down */
    uint16_t par;                      /* its parameters (shared: most zones of an instrument have the same) */
} r1_zone_t;

typedef struct {                       /* a zone's parameters: 64 bytes */
    int16_t scale;                     /* cents a key (100: a semitone) */
    int16_t fc, q;                     /* the low-pass: cutoff in absolute cents (13500 and over: open); cB */
    int16_t vd, va, vh, vdc, vs, vr;   /* the volume envelope: delay, attack, hold, decay, release in
                                        * timecents; sustain as centibels down */
    int16_t md, ma, mh, mdc, ms, mr;   /* the modulation envelope: timecents; sustain in 0.1 % */
    int16_t m2pitch, m2fc;             /* ... to pitch, to the cutoff: cents */
    int16_t k2vh, k2vd;                /* a key's change to the volume envelope's hold, decay: timecents */
    int16_t vibd, vibf, vib2p;         /* the vibrato LFO: delay (timecents), rate (absolute cents), to pitch */
    int16_t lfod, lfof, lfo2p, lfo2fc, lfo2v;   /* the modulation LFO: ..., to pitch, cutoff (cents), volume (cB) */
    int16_t rev, cho;                  /* sends, 0.1 % */
    uint8_t excl;                      /* exclusive class: a new note of it on the part cuts the others */
    uint8_t fkey, fvel;                /* a fixed key, a fixed velocity (255: the note's) */
    uint8_t rsv[3];
} r1_param_t;

typedef struct {                       /* a sample: 32 bytes */
    uint32_t off;                      /* its data: bytes from its region's start, R1_REGION_B for region B */
    uint32_t len;                      /* frames */
    uint32_t ls, le;                   /* its loop: start, end (frames, le exclusive; le > ls when it has one) */
    float rate;                        /* Hz */
    uint8_t fmt;                       /* R1_PCM16, R1_ADPCM */
    uint8_t rsv;
    int16_t h1, h2;                    /* ADPCM: the decoder's last two samples before the loop start's block */
    uint32_t rsv2;
} r1_sample_t;

#define R1_SIZES_OK (sizeof(r1_bank_t) == 96 && sizeof(r1_bankb_t) == 16 && sizeof(r1_font_t) == 32 && \
                     sizeof(r1_preset_t) == 32 && sizeof(r1_zone_t) == 16 && sizeof(r1_param_t) == 64 && \
                     sizeof(r1_sample_t) == 32)

/* the ADPCM: blocks of 16 samples in 9 bytes, a header (shift << 4 | predictor) then the nibbles, high
 * first. A sample = the nibble (-8 .. 7) shifted up, plus the predictor's guess from the last two. */
static const int8_t R1_P1[4] = {0, 60, 122, 115}, R1_P2[4] = {0, 0, -60, -52};   /* x 1/64 */
static inline int16_t r1_cl16(int32_t x) { return (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x); }
static inline void r1_adpcm_block(const uint8_t *b, int16_t *out, int32_t *h1, int32_t *h2)
{
    int sh = b[0] >> 4, f = b[0] & 3, i;
    int32_t p1 = *h1, p2 = *h2, c1 = R1_P1[f], c2 = R1_P2[f];
    if (sh > 12)
        sh = 12;
    for (i = 0; i < 16; i++) {
        int nib = (b[1 + (i >> 1)] >> ((i & 1) ? 0 : 4)) & 15;
        int32_t d = nib >= 8 ? nib - 16 : nib, s;
        s = r1_cl16(d * (1 << sh) + ((p1 * c1 + p2 * c2 + 32) >> 6));
        out[i] = (int16_t)s;
        p2 = p1;
        p1 = s;
    }
    *h1 = p1;
    *h2 = p2;
}

/* CRC-32 (the zlib one), a nibble at a time */
static inline uint32_t r1_crc32(uint32_t c, const uint8_t *b, uint32_t n)
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    c = ~c;
    while (n--) {
        c ^= *b++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return ~c;
}
