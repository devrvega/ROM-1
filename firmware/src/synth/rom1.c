/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1's engine (rom1.h): the bank's presets played as a SoundFont 2 synthesizer would, on 32 voices.
 *
 *   a voice      a zone's sample (16-bit or ADPCM, decoded a block at a time) at a pitch, read between its
 *                samples by a cubic, looped as the zone says; a low-pass (a state-variable filter); its gain
 *                from the volume envelope, the zone's and the controllers' attenuation; panned; sent to the
 *                reverb and the chorus
 *   control      every 32 samples: the envelopes (delay, attack, hold, decay, sustain, release; SoundFont's
 *                timecents, the volume's decay and release in decibels), the two LFOs (triangles), the
 *                SoundFont's default modulators (velocity and CC 7 / 11 to attenuation, CC 1 and pressure to
 *                vibrato, CC 10 pan, CC 91 / 93 the sends, the pitch wheel), and ROM-1's knobs (CC 74 / 71
 *                the cutoff and resonance, CC 73 / 72 attack and release) - the gain ramped across the 32
 *   effects      a chorus (two moving taps), a reverb (eight combs and four all-passes a side, Freeverb's
 *                layout, its tunings) in four kinds
 * Compiled as its own unit at -O2 (tools/build.py). */
#include "rom1.h"
#include "fastmath.h"

#ifdef EN_HOST
#define R1_POOL
#else
#define R1_POOL __attribute__((section(".pool")))
#endif

#define SUB 32                          /* control every 32 samples */
#define INV_FS (1.0f / R1_FS)
#define SUB_DT ((float)SUB / R1_FS)

r1_part_t r1_part[R1_NPART];
r1_fx_t r1_fx;
volatile float r1_meter[R1_NPART];
volatile uint8_t r1_voices;
volatile uint32_t r1_notes;
volatile uint16_t r1_activity;

static const uint8_t *bank_a, *bank_b;
static const r1_bank_t *bh;
static const r1_font_t *bfont;
static const r1_preset_t *bpre;
static const r1_zone_t *bzone;
static const r1_param_t *bpar;
static const r1_sample_t *bsmp;
static uint8_t b_ok;

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float tc2s(float tc) { return fm_exp2f(tc * (1.0f / 1200.0f)); }   /* timecents to seconds */

/* ---------------------------------------------------------------------------------- the bank --- */
int r1_bank_attach(const uint8_t *a, const uint8_t *b)
{
    const r1_bank_t *h = (const r1_bank_t *)(const void *)a;
    bank_a = 0;
    bh = 0;
    b_ok = 0;
    if (!a || h->magic != R1_MAGIC || h->version != R1_VERSION || h->len < sizeof *h || h->len > 0x100000u ||
        r1_crc32(0, a + 16, h->len - 16u) != h->crc)
        return -1;
    bank_a = a;
    bh = h;
    bfont = (const r1_font_t *)(const void *)(a + h->off_font);
    bpre = (const r1_preset_t *)(const void *)(a + h->off_preset);
    bzone = (const r1_zone_t *)(const void *)(a + h->off_zone);
    bpar = (const r1_param_t *)(const void *)(a + h->off_param);
    bsmp = (const r1_sample_t *)(const void *)(a + h->off_sample);
    bank_b = b;
    if (b && h->lenb) {
        const r1_bankb_t *hb = (const r1_bankb_t *)(const void *)b;
        b_ok = hb->magic == R1_MAGIC_B && hb->id == h->id && hb->len == h->lenb && hb->crc == h->crcb &&
               hb->len <= R1_B_ROOM && r1_crc32(0, b + 16, hb->len - 16u) == hb->crc;
    }
    return 0;
}
int r1_bank_ok_b(void) { return b_ok; }
const r1_bank_t *r1_bank(void) { return bh; }
int r1_fonts(void) { return bh ? bh->nfont : 0; }
const r1_font_t *r1_font_at(int f) { return bh && f >= 0 && f < (int)bh->nfont ? &bfont[f] : 0; }
const r1_preset_t *r1_preset_at(int i) { return bh && i >= 0 && i < (int)bh->npreset ? &bpre[i] : 0; }

static int find_exact(int font, int bank, int prog)
{
    int lo = 0, hi = bh ? (int)bh->npreset - 1 : -1;
    uint32_t want = (uint32_t)font << 16 | (uint32_t)bank << 8 | (uint32_t)prog;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const r1_preset_t *p = &bpre[mid];
        uint32_t k = (uint32_t)p->font << 16 | (uint32_t)p->bank << 8 | p->prog;
        if (k == want)
            return mid;
        if (k < want)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}
int r1_find(int font, int bank, int prog)
{
    int i, f, drums = bank == 128;
    if (!bh || !bh->npreset)
        return -1;
    font = clampi(font, 0, bh->nfont - 1);
    if ((i = find_exact(font, bank, prog)) >= 0)
        return i;
    if ((i = find_exact(font, drums ? 128 : 0, prog)) >= 0)
        return i;
    for (f = 0; f < (int)bh->nfont; f++)              /* another font's, the same kind */
        if ((i = find_exact(f, drums ? 128 : 0, prog)) >= 0 && (drums || f == font))
            return i;
    for (i = bfont[font].first; i < bfont[font].first + bfont[font].count; i++)   /* the font's first of the kind */
        if ((bpre[i].bank == 128) == drums)
            return i;
    for (i = 0; i < (int)bh->npreset; i++)
        if ((bpre[i].bank == 128) == drums)
            return i;
    return bfont[font].first;
}

/* -------------------------------------------------------------------------------- the voices --- */
typedef struct {
    uint8_t st;                         /* 0 free, 1 held, 2 released, 3 cut short */
    uint8_t part, key, vel, sus, fmt, loop, mode;
    uint8_t vstage, mstage, filt, done;
    uint32_t age;
    const r1_zone_t *z;
    const r1_param_t *p;
    const r1_sample_t *s;
    const uint8_t *data;
    uint32_t len, ls, le, fetch, bidx;
    int32_t h1, h2;
    int16_t blk[16];
    float w0, w1, w2, w3, frac;
    float pitch0;                       /* log2 of the step before modulation */
    float atten0;                       /* cB: the zone's and the velocity's */
    float vt, vamp, vatt, vrel;         /* the volume envelope */
    float mt, mval, mrel;               /* the modulation envelope */
    float vib_ph, vib_t, lfo_ph, lfo_t;
    float ic1, ic2, a1, a2, a3;         /* the filter */
    float gl, gr, gs;                   /* the gains, left, right, the sends' */
    float srev, scho;
    float level;                        /* (the meter's) */
} voice_t;

static voice_t vc[R1_NVOICE];
static uint32_t age_n;
#define VCAP0 28                                      /* the voices allowed at first: well within the time */
static uint8_t vcap = VCAP0, vcap_up, vcap_quiet;   /* the voices allowed now (r1_load) */

/* n samples from the voice's sample into out, in runs: up to the loop's end (then back to its start), or to
 * the sample's end (then silence); ADPCM a block at a time */
static void vfill(voice_t *v, float *out, uint32_t n)
{
    while (n) {
        int looping = v->loop && (v->mode == 1 || v->st == 1);
        uint32_t j = v->fetch, lim, k, run;
        if (looping && j >= v->le) {
            j = v->ls;
            v->fetch = j;
        }
        if (j >= v->len) {                            /* past the end: silence (the voice is done soon) */
            for (k = 0; k < n; k++)
                out[k] = 0.0f;
            v->fetch += n;
            if (v->fetch > v->len + 4u)
                v->done = 1;
            return;
        }
        lim = looping ? v->le : v->len;
        run = lim - j < n ? lim - j : n;
        if (v->fmt == R1_PCM16) {
            const int16_t *d = (const int16_t *)(const void *)v->data + j;
            for (k = 0; k < run; k++)
                out[k] = (float)d[k];
        } else {
            uint32_t done = 0;
            while (done < run) {
                uint32_t jj = j + done, b = jj >> 4, at = jj & 15u, m = 16u - at;
                if (b != v->bidx) {
                    if (b != v->bidx + 1u) {          /* back to the loop: the history stored for it */
                        v->h1 = v->s->h1;
                        v->h2 = v->s->h2;
                    }
                    r1_adpcm_block(v->data + b * R1_BLKB, v->blk, &v->h1, &v->h2);
                    v->bidx = b;
                }
                if (m > run - done)
                    m = run - done;
                for (k = 0; k < m; k++)
                    out[done + k] = (float)v->blk[at + k];
                done += m;
            }
        }
        out += run;
        n -= run;
        v->fetch = j + run;
    }
}

/* attenuation from a 0..127 controller, SoundFont's concave curve: 960 cB at 0 */
static float concave_cb(int v)
{
    static float t[128];
    static int ready;
    if (!ready) {
        int i;
        t[0] = 960.0f;
        for (i = 1; i < 128; i++) {
            float c = 400.0f * fm_log2f(127.0f / (float)i) * 0.30103f;
            t[i] = c > 960.0f ? 960.0f : c;
        }
        ready = 1;
    }
    return t[v & 127];
}

static void voice_release(voice_t *v, int fast)
{
    if (!v->st)
        return;
    if (v->vstage == 1 && v->vamp < 1.0f) {           /* still rising: from where it is, in decibels */
        v->vatt = v->vamp > 1e-5f ? -200.0f * fm_log2f(v->vamp) * 0.30103f : 960.0f;
        v->vamp = 1.0f;
    }
    v->vstage = 5;
    v->mstage = 5;
    v->st = (uint8_t)(fast ? 3 : 2);
    if (fast)
        v->vrel = 960.0f / 0.006f;                    /* cut: 6 ms */
    else {
        int r = v->part < R1_NPART ? r1_part[v->part].release - 64 : 0;
        v->vrel = 960.0f / tc2s((float)v->p->vr + (float)r * 60.0f);
    }
    v->mrel = 1.0f / tc2s((float)v->p->mr);
}

static voice_t *voice_alloc(void)
{
    voice_t *best = 0, *free = 0;
    int i, live = 0;
    for (i = 0; i < R1_NVOICE; i++)
        if (vc[i].st)
            live++;
        else if (!free)
            free = &vc[i];
    if (free && live < vcap)
        return free;
    for (i = 0; i < R1_NVOICE; i++) {                 /* the quietest let go, else the oldest */
        voice_t *v = &vc[i];
        if (!v->st)
            continue;
        if (v->st >= 2 && (!best || best->st < 2 || v->level < best->level))
            best = v;
        else if (!best || (best->st < 2 && v->age < best->age))
            best = v;
    }
    return best;
}

static void voice_start(int part, int key, int vel, const r1_zone_t *z)
{
    const r1_param_t *p = &bpar[z->par];
    const r1_sample_t *s = &bsmp[z->smp];
    const r1_part_t *pt = &r1_part[part];
    voice_t *v;
    int kk, vv, i;
    float semis;
    if ((s->off & R1_REGION_B) && !b_ok)
        return;                                       /* its data is not on the FM-1 (yet) */
    if (p->excl)                                      /* its exclusive class: the others of it go */
        for (i = 0; i < R1_NVOICE; i++)
            if (vc[i].st && vc[i].part == part && vc[i].p->excl == p->excl)
                voice_release(&vc[i], 1);
    v = voice_alloc();
    if (!v)
        return;
    kk = p->fkey != 255 ? p->fkey : key;
    vv = p->fvel != 255 ? p->fvel : vel;
    v->st = 1;
    v->part = (uint8_t)part;
    v->key = (uint8_t)key;
    v->vel = (uint8_t)vv;
    v->sus = 0;
    v->age = ++age_n;
    v->z = z;
    v->p = p;
    v->s = s;
    v->data = (s->off & R1_REGION_B) ? bank_b + (s->off & ~R1_REGION_B) : bank_a + s->off;
    v->fmt = s->fmt;
    v->len = s->len;
    v->ls = s->ls;
    v->le = s->le;
    v->loop = (uint8_t)((z->mode == 1 || z->mode == 3) && s->le > s->ls);
    v->mode = z->mode;
    v->fetch = 0;
    v->bidx = 0xFFFFFFFFu;
    v->h1 = v->h2 = 0;
    v->done = 0;
    v->frac = 0.0f;
    v->w0 = 0.0f;
    {
        float w[3];
        vfill(v, w, 3);
        v->w1 = w[0];
        v->w2 = w[1];
        v->w3 = w[2];
    }
    semis = (float)(kk - z->root) * (float)p->scale * 0.01f + (float)z->tune * 0.01f + (float)pt->coarse +
            (float)pt->fine * 0.01f;
    v->pitch0 = fm_log2f(s->rate * INV_FS) + semis * (1.0f / 12.0f);
    v->atten0 = (float)z->atten + concave_cb(vv);
    v->vstage = 0;
    v->vt = tc2s((float)p->vd);
    v->vamp = 0.0f;
    v->vatt = 0.0f;
    v->mstage = 0;
    v->mt = tc2s((float)p->md);
    v->mval = 0.0f;
    v->vib_ph = v->lfo_ph = 0.0f;
    v->vib_t = tc2s((float)p->vibd);
    v->lfo_t = tc2s((float)p->lfod);
    v->ic1 = v->ic2 = 0.0f;
    v->gl = v->gr = v->gs = 0.0f;
    v->level = 0.0f;
    r1_notes++;
}

static void note_on(int part, int key, int vel)
{
    const r1_part_t *pt = &r1_part[part];
    const r1_preset_t *pr;
    uint32_t i;
    if (!bh || pt->preset == 0xFFFF || pt->preset >= bh->npreset)
        return;
    if (!pt->drums)                                   /* the same key again: the old one lets go */
        for (i = 0; i < R1_NVOICE; i++)
            if (vc[i].st == 1 && vc[i].part == part && vc[i].key == key)
                voice_release(&vc[i], 0);
    pr = &bpre[pt->preset];
    for (i = pr->zone; i < pr->zone + pr->nzone && i < bh->nzone; i++) {
        const r1_zone_t *z = &bzone[i];
        if (key >= z->klo && key <= z->khi && vel >= z->vlo && vel <= z->vhi)
            voice_start(part, key, vel, z);
    }
    r1_activity |= (uint16_t)(1u << (part & 15));
}
static void note_off(int part, int key)
{
    int i;
    for (i = 0; i < R1_NVOICE; i++) {
        voice_t *v = &vc[i];
        if (v->st == 1 && v->part == part && v->key == key) {
            if (r1_part[part].sustain >= 64)
                v->sus = 1;
            else
                voice_release(v, 0);
        }
    }
}
static void part_all_off(int part, int fast)
{
    int i;
    for (i = 0; i < R1_NVOICE; i++)
        if (vc[i].st && (part < 0 || vc[i].part == part))
            voice_release(&vc[i], fast);
}

/* ------------------------------------------------------------------------------- the parts --- */
static void part_defaults(int i)
{
    r1_part_t *p = &r1_part[i];
    p->font = 0;
    p->bank = 0;
    p->prog = 0;
    p->vol = 100;
    p->expr = 127;
    p->pan = 64;
    p->rev = 40;
    p->cho = 0;
    p->cutoff = p->reso = p->attack = p->release = 64;
    p->mod = p->sustain = p->pressure = 0;
    p->pbrange = 2;
    p->bend = 0;
    p->coarse = p->fine = 0;
    p->drums = (uint8_t)(i == R1_DRUMS);
    p->rx = 1;
    p->mute = p->solo = 0;
    p->font_pend = 0;
    p->rpn_lsb = p->rpn_msb = 127;
    p->preset = (uint16_t)(bh ? r1_find(0, p->drums ? 128 : 0, 0) : 0xFFFF);
    if (bh && p->preset != 0xFFFF)
        p->font = bpre[p->preset].font;
}
static void controllers_reset(int i)
{
    r1_part_t *p = &r1_part[i];
    p->expr = 127;
    p->mod = p->sustain = p->pressure = 0;
    p->bend = 0;
    p->rpn_lsb = p->rpn_msb = 127;
}
static void program(int part, int prog)
{
    r1_part_t *p = &r1_part[part];
    int i;
    if (!bh)
        return;
    i = r1_find(p->font_pend, p->drums ? 128 : p->bank, prog);
    if (i < 0)
        return;
    p->preset = (uint16_t)i;
    p->font = bpre[i].font;
    p->prog = (uint8_t)prog;
}
static void cc(int part, int c, int v)
{
    r1_part_t *p = &r1_part[part];
    switch (c) {
    case 0: p->font_pend = (uint8_t)v; break;         /* bank MSB: the font (in at the program change) */
    case 32: p->bank = (uint8_t)v; break;             /* bank LSB: the font's own bank */
    case 1: p->mod = (uint8_t)v; break;
    case 6:                                           /* data entry: RPN 0 bend range, 1 fine, 2 coarse */
        if (p->rpn_msb == 0 && p->rpn_lsb == 0)
            p->pbrange = (uint8_t)clampi(v, 0, 24);
        else if (p->rpn_msb == 0 && p->rpn_lsb == 1)
            p->fine = (int8_t)clampi((v - 64) * 100 / 64, -100, 99);
        else if (p->rpn_msb == 0 && p->rpn_lsb == 2)
            p->coarse = (int8_t)clampi(v - 64, -24, 24);
        break;
    case 7: p->vol = (uint8_t)v; break;
    case 10: p->pan = (uint8_t)v; break;
    case 11: p->expr = (uint8_t)v; break;
    case 64:
        p->sustain = (uint8_t)v;
        if (v < 64) {
            int i;
            for (i = 0; i < R1_NVOICE; i++)
                if (vc[i].st == 1 && vc[i].part == part && vc[i].sus)
                    voice_release(&vc[i], 0);
        }
        break;
    case 71: p->reso = (uint8_t)v; break;
    case 72: p->release = (uint8_t)v; break;
    case 73: p->attack = (uint8_t)v; break;
    case 74: p->cutoff = (uint8_t)v; break;
    case 91: p->rev = (uint8_t)v; break;
    case 93: p->cho = (uint8_t)v; break;
    case 98: case 99: p->rpn_lsb = p->rpn_msb = 127; break;   /* (NRPNs: not ours) */
    case 100: p->rpn_lsb = (uint8_t)v; break;
    case 101: p->rpn_msb = (uint8_t)v; break;
    case 120: part_all_off(part, 1); break;
    case 121: controllers_reset(part); break;
    case 123: case 124: case 125: case 126: case 127: part_all_off(part, 0); break;
    default: break;
    }
}

static void do_cmds(void);
void r1_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    int part = (int)(st & 15u);
    do_cmds();                                        /* what the UI asked before it (a reset, all notes off) */
    if (st >= 0xF0u || !r1_part[part].rx)
        return;
    d1 &= 127u;
    d2 &= 127u;
    switch (st & 0xF0u) {
    case 0x90:
        if (d2) {
            note_on(part, (int)d1, (int)d2);
            break;
        }
        /* fall through: velocity 0 is a note off */
    case 0x80: note_off(part, (int)d1); break;
    case 0xB0: cc(part, (int)d1, (int)d2); break;
    case 0xC0: program(part, (int)d1); break;
    case 0xD0: r1_part[part].pressure = (uint8_t)d1; break;
    case 0xE0: r1_part[part].bend = (int16_t)((int)(d1 | d2 << 7) - 8192); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- the UI's queue --- */
enum { Q_NOTE, Q_CC, Q_PROG, Q_PANIC, Q_RESET };
typedef struct {
    uint8_t c, part, a, b;
    uint16_t x;
} cmd_t;
static cmd_t q[64];
static volatile uint32_t q_w, q_r;
static void put(int c, int part, int a, int b, int x)
{
    if (q_w - q_r >= 64u)
        return;
    q[q_w & 63u].c = (uint8_t)c;
    q[q_w & 63u].part = (uint8_t)part;
    q[q_w & 63u].a = (uint8_t)a;
    q[q_w & 63u].b = (uint8_t)b;
    q[q_w & 63u].x = (uint16_t)x;
    __asm__ volatile("" ::: "memory");
    q_w++;
}
void r1_note(int part, int key, int vel) { put(Q_NOTE, part & 15, key & 127, vel & 127, 0); }
void r1_cc(int part, int c, int val) { put(Q_CC, part & 15, c & 127, val & 127, 0); }
void r1_program(int part, int preset) { put(Q_PROG, part & 15, 0, 0, preset); }
void r1_panic(void) { put(Q_PANIC, 0, 0, 0, 0); }
void r1_reset(void) { put(Q_RESET, 0, 0, 0, 0); }

int r1_sysex(const uint8_t *p, uint32_t n)
{
    if ((n >= 4u && p[0] == 0x7Eu && p[2] == 0x09u && (p[3] == 0x01u || p[3] == 0x03u)) ||    /* GM on (1, 2) */
        (n >= 7u && p[0] == 0x41u && p[2] == 0x42u && p[3] == 0x12u && p[4] == 0x40u && p[5] == 0x00u &&
         p[6] == 0x7Fu)) {                                                                    /* GS reset */
        r1_reset();
        return 1;
    }
    return 0;
}

static void do_cmds(void)
{
    while (q_r != q_w) {
        cmd_t c = q[q_r & 63u];
        q_r++;
        switch (c.c) {
        case Q_NOTE:
            if (c.b)
                note_on(c.part, c.a, c.b);
            else
                note_off(c.part, c.a);
            break;
        case Q_CC: cc(c.part, c.a, c.b); break;
        case Q_PROG:
            if (bh && c.x < bh->npreset) {
                r1_part_t *p = &r1_part[c.part];
                p->preset = c.x;
                p->font = p->font_pend = bpre[c.x].font;
                p->bank = (uint8_t)(bpre[c.x].bank & 127u);
                p->prog = bpre[c.x].prog;
            }
            break;
        case Q_PANIC: part_all_off(-1, 1); break;
        case Q_RESET: {
            int i;
            part_all_off(-1, 1);
            for (i = 0; i < R1_NPART; i++)
                part_defaults(i);
            break;
        }
        default: break;
        }
    }
}

/* ------------------------------------------------------------------------------- control --- */
static float lfo_tri(float ph) { return ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f; }

/* a voice's 32 samples' worth of control; 0 when it has finished */
static int voice_control(voice_t *v, float *step, float *gl, float *gr, float *gs)
{
    const r1_param_t *p = v->p;
    const r1_part_t *pt = &r1_part[v->part];
    float amp, att, cents, vib = 0.0f, lfo = 0.0f, menv;
    int key = v->key;
    /* the volume envelope */
    switch (v->vstage) {
    case 0:
        if ((v->vt -= SUB_DT) <= 0.0f) {
            v->vstage = 1;
            v->vt = tc2s((float)p->va + (float)(pt->attack - 64) * 60.0f);
        }
        break;
    case 1:
        v->vamp += SUB_DT / (v->vt > 1e-4f ? v->vt : 1e-4f);
        if (v->vamp >= 1.0f) {
            v->vamp = 1.0f;
            v->vstage = 2;
            v->vt = tc2s((float)p->vh + (float)p->k2vh * (float)(60 - key));
        }
        break;
    case 2:
        if ((v->vt -= SUB_DT) <= 0.0f) {
            v->vstage = 3;
            v->vt = tc2s((float)p->vdc + (float)p->k2vd * (float)(60 - key));
        }
        break;
    case 3:
        v->vatt += 960.0f * SUB_DT / (v->vt > 1e-4f ? v->vt : 1e-4f);
        if (v->vatt >= (float)p->vs) {
            v->vatt = (float)p->vs;
            v->vstage = 4;
        }
        break;
    case 4: break;
    default:
        v->vatt += v->vrel * SUB_DT;
        if (v->vatt >= 960.0f)
            return 0;
        break;
    }
    if (v->vstage >= 4 && v->vatt >= 960.0f)          /* (sustained in silence) */
        return 0;
    if (v->done)
        return 0;
    /* the modulation envelope */
    switch (v->mstage) {
    case 0:
        if ((v->mt -= SUB_DT) <= 0.0f) {
            v->mstage = 1;
            v->mt = tc2s((float)p->ma);
        }
        break;
    case 1:
        v->mval += SUB_DT / (v->mt > 1e-4f ? v->mt : 1e-4f);
        if (v->mval >= 1.0f) {
            v->mval = 1.0f;
            v->mstage = 2;
            v->mt = tc2s((float)p->mh);
        }
        break;
    case 2:
        if ((v->mt -= SUB_DT) <= 0.0f) {
            v->mstage = 3;
            v->mt = tc2s((float)p->mdc);
        }
        break;
    case 3: {
        float sus = 1.0f - (float)p->ms * 0.001f;
        v->mval -= SUB_DT / (v->mt > 1e-4f ? v->mt : 1e-4f);
        if (v->mval <= sus) {
            v->mval = sus;
            v->mstage = 4;
        }
        break;
    }
    case 4: break;
    default:
        v->mval -= v->mrel * SUB_DT;
        if (v->mval < 0.0f)
            v->mval = 0.0f;
        break;
    }
    menv = v->mval;
    /* the LFOs */
    if ((v->vib_t -= SUB_DT) <= 0.0f) {
        vib = lfo_tri(v->vib_ph);
        v->vib_ph += 8.176f * fm_exp2f((float)p->vibf * (1.0f / 1200.0f)) * SUB_DT;
        v->vib_ph -= (float)(int)v->vib_ph;
    }
    if ((v->lfo_t -= SUB_DT) <= 0.0f) {
        lfo = lfo_tri(v->lfo_ph);
        v->lfo_ph += 8.176f * fm_exp2f((float)p->lfof * (1.0f / 1200.0f)) * SUB_DT;
        v->lfo_ph -= (float)(int)v->lfo_ph;
    }
    /* pitch: the zone's, its modulators, the wheel, the mod wheel and pressure's vibrato */
    cents = (float)p->vib2p * vib + (float)p->lfo2p * lfo + (float)p->m2pitch * menv +
            (float)pt->bend * (float)pt->pbrange * (100.0f / 8192.0f) +
            ((float)pt->mod + (float)pt->pressure) * (50.0f / 127.0f) * vib;
    *step = fm_exp2f(v->pitch0 + cents * (1.0f / 1200.0f));
    if (*step > 16.0f)
        *step = 16.0f;
    /* the filter */
    {
        float fc = (float)p->fc + (float)p->m2fc * menv + (float)p->lfo2fc * lfo + (float)(pt->cutoff - 64) * 60.0f;
        float qc = (float)p->q + (float)(pt->reso - 64) * 4.0f;
        if (qc < 0.0f)
            qc = 0.0f;
        v->filt = fc < 13490.0f || qc > 0.5f;
        if (v->filt) {
            float hz = 8.176f * fm_exp2f(fc * (1.0f / 1200.0f)), g, k;
            if (hz < 30.0f)
                hz = 30.0f;
            if (hz > 18000.0f)
                hz = 18000.0f;
            g = fm_tanf(3.14159265f * hz * INV_FS);
            k = 1.41421356f * fm_exp2f(-qc * (1.0f / 602.06f));   /* 1 / Q, Q = 0.707 x the peak's gain */
            v->a1 = 1.0f / (1.0f + g * (g + k));
            v->a2 = g * v->a1;
            v->a3 = g * v->a2;
            qc *= 0.5f;                               /* (half the peak taken off the level) */
        } else
            qc = 0.0f;
        /* the gain: the envelope, the zone's attenuation and velocity's, CC 7 and 11, the LFO, the filter's peak */
        att = v->atten0 + concave_cb(pt->vol) + concave_cb(pt->expr) + (float)p->lfo2v * lfo + v->vatt + qc;
        amp = v->vamp * fm_exp2f(-att * (1.0f / 602.06f)) * (1.0f / 32768.0f);
    }
    if (pt->mute)
        amp = 0.0f;
    /* pan: the zone's and CC 10's, equal power */
    {
        float pan = (float)v->z->pan + (float)(pt->pan - 64) * (500.0f / 64.0f), a;
        if (pan < -500.0f)
            pan = -500.0f;
        if (pan > 500.0f)
            pan = 500.0f;
        a = (pan + 500.0f) * (0.25f / 1000.0f);       /* 0 .. 1/4 turn */
        *gl = amp * fm_sin_turns(0.25f - a);
        *gr = amp * fm_sin_turns(a);
        *gs = amp;
    }
    v->srev = ((float)p->rev + 200.0f * (float)pt->rev * (1.0f / 127.0f)) * 0.001f;
    v->scho = ((float)p->cho + 200.0f * (float)pt->cho * (1.0f / 127.0f)) * 0.001f;
    v->level = amp * 32768.0f;
    return 1;
}

/* ------------------------------------------------------------------------------- effects --- */
#define NCOMB 8
#define NAP 4
static const uint16_t COMB_L[NCOMB] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const uint16_t AP_L[NAP] = {556, 441, 341, 225};
#define SPREAD 23
static float comb_buf[2][NCOMB][1617 + SPREAD] R1_POOL;
static float ap_buf[2][NAP][556 + SPREAD] R1_POOL;
static uint16_t comb_i[2][NCOMB], ap_i[2][NAP];
static float comb_lp[2][NCOMB];
#define CHO_N 2048
static float cho_buf[CHO_N] R1_POOL;
static uint32_t cho_w;
static float cho_ph;

static float mix_l[256], mix_r[256], bus_rev[256], bus_cho[256];
static float src[4 + 8 * SUB + 8], tmp[SUB];         /* a voice's source samples, its output, for a block of SUB */

static void effects(uint32_t n)
{
    static const float SIZE[4] = {0.70f, 0.82f, 0.90f, 0.86f}, DAMP[4] = {0.55f, 0.40f, 0.30f, 0.12f};
    uint32_t i;
    int t = r1_fx.rev_type & 7;
    if (t < 4) {
        float fb = SIZE[t] + (float)(r1_fx.rev_time - 64) * (0.12f / 64.0f), damp = DAMP[t];
        float wet = (float)r1_fx.rev_level * (1.0f / 127.0f) * 3.0f;
        int c, k;
        if (fb > 0.98f)
            fb = 0.98f;
        if (fb < 0.5f)
            fb = 0.5f;
        for (c = 0; c < 2; c++) {
            float *mix = c ? mix_r : mix_l;
            for (i = 0; i < n; i++) {
                float in = bus_rev[i] * 0.015f, out = 0.0f;
                for (k = 0; k < NCOMB; k++) {
                    uint32_t L = COMB_L[k] + (uint32_t)(c * SPREAD);
                    float *b = comb_buf[c][k], y = b[comb_i[c][k]];
                    comb_lp[c][k] = y * (1.0f - damp) + comb_lp[c][k] * damp;
                    b[comb_i[c][k]] = in + comb_lp[c][k] * fb;
                    if (++comb_i[c][k] >= L)
                        comb_i[c][k] = 0;
                    out += y;
                }
                for (k = 0; k < NAP; k++) {
                    uint32_t L = AP_L[k] + (uint32_t)(c * SPREAD);
                    float *b = ap_buf[c][k], bo = b[ap_i[c][k]];
                    b[ap_i[c][k]] = out + bo * 0.5f;
                    out = bo - out;
                    if (++ap_i[c][k] >= L)
                        ap_i[c][k] = 0;
                }
                mix[i] += fm_flush(out) * wet;
            }
        }
    }
    if (r1_fx.cho_level) {
        float rate = 0.1f + (float)r1_fx.cho_rate * (3.0f / 127.0f), depth = 20.0f + (float)r1_fx.cho_depth * 1.6f;
        float wet = (float)r1_fx.cho_level * (1.0f / 127.0f), base = 560.0f;
        for (i = 0; i < n; i++) {
            float d0 = base + depth * fm_sin_turns(cho_ph), d1 = base + depth * fm_sin_turns(cho_ph + 0.5f);
            float r0, r1;
            uint32_t j0, j1;
            float f0, f1;
            cho_buf[cho_w & (CHO_N - 1)] = bus_cho[i];
            r0 = (float)cho_w - d0;
            r1 = (float)cho_w - d1;
            j0 = (uint32_t)(int32_t)r0;
            j1 = (uint32_t)(int32_t)r1;
            f0 = r0 - (float)(int32_t)r0;
            f1 = r1 - (float)(int32_t)r1;
            mix_l[i] += wet * (cho_buf[j0 & (CHO_N - 1)] * (1.0f - f0) + cho_buf[(j0 + 1) & (CHO_N - 1)] * f0);
            mix_r[i] += wet * (cho_buf[j1 & (CHO_N - 1)] * (1.0f - f1) + cho_buf[(j1 + 1) & (CHO_N - 1)] * f1);
            cho_w++;
            cho_ph += rate * INV_FS;
            if (cho_ph >= 1.0f)
                cho_ph -= 1.0f;
        }
    }
}

/* ---------------------------------------------------------------------------------- render --- */
void r1_render(int32_t *out, uint32_t n, uint32_t gain_q12)
{
    uint32_t i, at;
    int k, live = 0;
    float meter[R1_NPART] = {0}, master = (float)gain_q12 * (1.0f / 4096.0f) * 0.6f;
    do_cmds();
    if (n > 256u)
        n = 256u;
    for (i = 0; i < n; i++)
        mix_l[i] = mix_r[i] = bus_rev[i] = bus_cho[i] = 0.0f;
    for (k = 0; k < R1_NVOICE; k++) {
        voice_t *v = &vc[k];
        if (!v->st)
            continue;
        for (at = 0; at < n && v->st; at += SUB) {
            float step, gl, gr, gs, dl, dr, ds, cl, cr, cs, t, frac;
            uint32_t m = n - at < SUB ? n - at : SUB, j, adv;
            float *ml = mix_l + at, *mr = mix_r + at;
            if (!voice_control(v, &step, &gl, &gr, &gs)) {
                v->st = 0;
                break;
            }
            if (step > 8.0f)
                step = 8.0f;
            /* the source: the window (4 samples), then what these m outputs step over */
            frac = v->frac;
            adv = (uint32_t)(frac + step * (float)m);
            src[0] = v->w0;
            src[1] = v->w1;
            src[2] = v->w2;
            src[3] = v->w3;
            if (adv)
                vfill(v, src + 4, adv);
            /* read between them: a cubic */
            t = frac;
            for (j = 0; j < m; j++) {
                int32_t i = (int32_t)t;
                float f = t - (float)i, a0 = src[i], a1 = src[i + 1], a2 = src[i + 2], a3 = src[i + 3];
                float c1 = 0.5f * (a2 - a0), c2 = a0 - 2.5f * a1 + 2.0f * a2 - 0.5f * a3, c3 = 0.5f * (a3 - a0) + 1.5f * (a1 - a2);
                tmp[j] = ((c3 * f + c2) * f + c1) * f + a1;
                t += step;
            }
            v->w0 = src[adv];
            v->w1 = src[adv + 1];
            v->w2 = src[adv + 2];
            v->w3 = src[adv + 3];
            v->frac = t - (float)adv;
            if (v->frac < 0.0f)
                v->frac = 0.0f;
            if (v->filt) {                            /* the state-variable low-pass */
                float i1 = v->ic1, i2 = v->ic2, a1 = v->a1, a2 = v->a2, a3 = v->a3;
                for (j = 0; j < m; j++) {
                    float v3 = tmp[j] - i2, v1 = a1 * i1 + a2 * v3, v2 = i2 + a2 * i1 + a3 * v3;
                    i1 = 2.0f * v1 - i1;
                    i2 = 2.0f * v2 - i2;
                    tmp[j] = v2;
                }
                v->ic1 = fm_flush(i1);
                v->ic2 = fm_flush(i2);
            }
            /* into the mix, the gains ramped across the m */
            cl = v->gl;
            cr = v->gr;
            cs = v->gs;
            dl = (gl - cl) / (float)m;
            dr = (gr - cr) / (float)m;
            ds = (gs - cs) / (float)m;
            for (j = 0; j < m; j++) {
                float y = tmp[j];
                cl += dl;
                cr += dr;
                ml[j] += y * cl;
                mr[j] += y * cr;
            }
            if (v->srev > 0.0f || v->scho > 0.0f) {
                float sr = v->srev, sc = v->scho, *br = bus_rev + at, *bc = bus_cho + at;
                cs = v->gs;
                for (j = 0; j < m; j++) {
                    float y;
                    cs += ds;
                    y = tmp[j] * cs;
                    br[j] += y * sr;
                    bc[j] += y * sc;
                }
            }
            v->gl = gl;
            v->gr = gr;
            v->gs = gs;
        }
        if (v->st) {
            live++;
            if (v->level > meter[v->part])
                meter[v->part] = v->level;
        }
    }
    effects(n);
    for (k = 0; k < R1_NPART; k++) {                  /* the meters: up at once, down slowly */
        float m = meter[k] > 1.0f ? 1.0f : meter[k], o = r1_meter[k] * 0.92f;
        r1_meter[k] = m > o ? m : o;
    }
    r1_voices = (uint8_t)live;
    for (i = 0; i < n; i++) {                         /* the master, softly limited */
        float l = mix_l[i] * master, r = mix_r[i] * master;
        l = l > 1.5f ? 1.0f : l < -1.5f ? -1.0f : l - (4.0f / 27.0f) * l * l * l;
        r = r > 1.5f ? 1.0f : r < -1.5f ? -1.0f : r - (4.0f / 27.0f) * r * r * r;
        out[2 * i] = (int32_t)(l * 8300000.0f);
        out[2 * i + 1] = (int32_t)(r * 8300000.0f);
    }
}

/* the cap on the voices follows the time the renders take: down at once below the voices sounding when a
 * render takes over 85 % of its time (over 92 %: the quietest let go is cut as well); up a voice at a time
 * only while the voices fill it and the renders take under 70 %; back to VCAP0 after a quiet moment */
void r1_load(uint32_t pct)
{
    int i, live = r1_voices;
    voice_t *q = 0;
    if (pct > 85u) {
        int c = (live < vcap ? live : vcap) - 1;
        vcap = (uint8_t)(c < R1_NVOICE / 2 ? R1_NVOICE / 2 : c);
        vcap_up = 0;
        if (pct > 92u)
            for (i = 0; i < R1_NVOICE; i++)
                if (vc[i].st == 2 && (!q || vc[i].level < q->level))
                    q = &vc[i];
        if (q)
            voice_release(q, 1);
    } else if (live + 1 >= vcap && pct < 70u && vcap < R1_NVOICE) {
        if (++vcap_up >= 8u) {
            vcap++;
            vcap_up = 0;
        }
    }
    if (live)
        vcap_quiet = 0;
    else if (vcap != VCAP0 && ++vcap_quiet >= 250u) {         /* (0.7 s) */
        vcap = VCAP0;
        vcap_quiet = 0;
    }
}

uint32_t r1_voice_cap(void) { return vcap; }

void r1_init(void)
{
    int i;
    for (i = 0; i < R1_NVOICE; i++)
        vc[i].st = 0;
    vcap = VCAP0;
    for (i = 0; i < R1_NPART; i++)
        part_defaults(i);
    r1_fx.rev_type = 1;
    r1_fx.rev_time = 64;
    r1_fx.rev_level = 64;
    r1_fx.cho_level = 48;
    r1_fx.cho_rate = 20;
    r1_fx.cho_depth = 40;
}
