/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1's converter (conv.h): SoundFont 2 preset tables flattened into zones, the samples they use fitted to
 * the room at the best quality that fits, resampled and encoded, and written as a bank (r1bank.h).
 *
 * Flattening follows the SoundFont 2.01 rules: an instrument zone's generators override its instrument's
 * global zone; a preset zone's (over the preset's global zone) are added to them, except the sample's
 * place, its loop mode, keys and velocities fixed, its root key and exclusive class, which only an
 * instrument sets; key and velocity ranges intersect. Each result is clamped to the spec's range.
 *
 * Fitting: every sample starts as 16-bit at its own rate (44.1 kHz if it is over 48 kHz). While they do
 * not fit, the levels come down evenly (the best-kept first, the biggest of those first): 4-bit ADPCM at
 * the sample's rate, then at 32, 22.05, 16, 11.025 and 8 kHz at most. A loop keeps a whole number of samples (its pitch would drift otherwise): the rate is
 * nudged to make it so, and the loop is resampled around itself so it joins without a click. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "conv.h"
#include "../../firmware/src/synth/r1bank.h"

#define NGEN 61
#define PI_ 3.14159265358979323846
#define NLVL 7
static const float LVL_CAP[NLVL] = {48000.0f, 48000.0f, 32000.0f, 22050.0f, 16000.0f, 11025.0f, 8000.0f};

typedef struct {
    char name[21];
    uint32_t start, end, ls, le, rate;
    uint8_t pitch;
    int8_t corr;
    uint16_t type;
} sf_shdr;

typedef struct {
    int16_t g[NGEN];
    uint8_t klo, khi, vlo, vhi;
    int sh;                            /* its sample header */
    int var;                           /* its bank sample (after planning), -1 none */
} cv_zone;

typedef struct {
    char name[21];
    uint16_t bank, prog;
    int z0, nz;
    uint8_t sel;
} cv_preset;

typedef struct {
    char name[24];
    sf_shdr *sh;
    int nsh;
    cv_preset *pr;
    int npr;
    cv_zone *z;
    int nz;
    uint32_t smpl_bytes;
} cv_font;

typedef struct {                       /* a bank sample: a span of a font's sample data, its loop */
    int font, sh;
    uint32_t start, end, ls, le;       /* in the smpl chunk, in samples; le exclusive */
    uint8_t loop, mode;
    int level;
    uint32_t est;                      /* its bytes at its level */
    /* encoded */
    uint8_t *data;
    uint32_t dlen, frames, rls, rle;
    float rate;
    uint8_t fmt;
    int16_t h1, h2;
    int region;                        /* 0 A, 1 B */
    uint32_t off;
    int done;
} cv_var;

typedef struct {                       /* a span the plan needs: one sample header's data, whole */
    int font, sh;
    uint32_t start, end;
    int given;
} cv_span;

static cv_font *F;
static int NF;
static cv_var *V;
static int NV;
static cv_span *SP;
static int NSP;
static uint32_t cap_a_, cap_b_, over_, planned_;
static uint8_t plan_ok;                /* the last plan fits: cv_build may go on */
static double snr_sum, snr_min;                      /* the ADPCM's signal to noise (dB), its mean and worst */
static int snr_n;
static uint8_t *outa, *outb;
static uint32_t outa_n, outb_n;
static char err[160], *rep;

const char *cv_error(void) { return err; }
static int fail(const char *m)
{
    snprintf(err, sizeof err, "%s", m);
    return -1;
}

static void vars_free(void)
{
    int i;
    for (i = 0; i < NV; i++)
        free(V[i].data);
    free(V);
    V = 0;
    NV = 0;
    free(SP);
    SP = 0;
    NSP = 0;
}
void cv_reset(void)
{
    int i;
    for (i = 0; i < NF; i++) {
        free(F[i].sh);
        free(F[i].pr);
        free(F[i].z);
    }
    free(F);
    F = 0;
    NF = 0;
    vars_free();
    free(outa);
    free(outb);
    outa = outb = 0;
    outa_n = outb_n = 0;
    free(rep);
    rep = 0;
    err[0] = 0;
    plan_ok = 0;
}

/* ------------------------------------------------------------------------- reading a font --- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int cv_riff(const uint8_t *f, uint32_t n, uint32_t *pdta, uint32_t *pdta_n, uint32_t *smpl, uint32_t *smpl_n)
{
    uint32_t o = 12;
    *pdta = *pdta_n = *smpl = *smpl_n = 0;
    if (n < 12 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "sfbk", 4))
        return -1;
    while (o + 8 <= n) {
        uint32_t sz = rd32(f + o + 4);
        if (sz > n - o - 8)
            sz = n - o - 8;
        if (!memcmp(f + o, "LIST", 4) && sz >= 4) {
            if (!memcmp(f + o + 8, "pdta", 4)) {
                *pdta = o + 12;
                *pdta_n = sz - 4;
            } else if (!memcmp(f + o + 8, "sdta", 4)) {
                uint32_t q = o + 12, e = o + 8 + sz;
                while (q + 8 <= e) {
                    uint32_t s2 = rd32(f + q + 4);
                    if (s2 > e - q - 8)
                        s2 = e - q - 8;
                    if (!memcmp(f + q, "smpl", 4)) {
                        *smpl = q + 8;
                        *smpl_n = s2;
                    }
                    q += 8 + s2 + (s2 & 1);
                }
            }
        }
        o += 8 + sz + (sz & 1);
    }
    return *pdta ? 0 : -1;
}

static void name_of(char *d, const uint8_t *s, int n)
{
    int i;
    for (i = 0; i < n && s[i]; i++)
        d[i] = (char)(s[i] >= 32 && s[i] < 127 ? s[i] : '?');
    while (i > 0 && d[i - 1] == ' ')
        i--;
    d[i] = 0;
}

static void gen_defaults(int16_t *g)
{
    int i;
    memset(g, 0, NGEN * sizeof *g);
    g[8] = 13500;
    for (i = 21; i <= 38; i++)                        /* the LFOs' delays, the envelopes' times */
        g[i] = -12000;
    g[22] = g[24] = 0;                                /* LFO rates: 8.176 Hz */
    g[29] = g[37] = 0;                                /* sustains */
    g[31] = g[32] = 0;
    g[43] = (int16_t)(127 << 8);
    g[44] = (int16_t)(127 << 8);
    g[46] = g[47] = g[58] = -1;
    g[56] = 100;
}
static int is_additive(int op)
{
    switch (op) {
    case 0: case 1: case 2: case 3: case 4: case 12: case 41: case 43: case 44: case 45: case 46: case 47:
    case 50: case 53: case 54: case 57: case 58:
        return 0;
    default:
        return op < NGEN;
    }
}
static int16_t clampg(int op, int32_t v)
{
    int32_t lo = -32768, hi = 32767;
    switch (op) {
    case 5: case 6: case 7: case 10: case 11: lo = -12000; hi = 12000; break;
    case 8: lo = 1500; hi = 13500; break;
    case 9: lo = 0; hi = 960; break;
    case 13: lo = -960; hi = 960; break;
    case 15: case 16: lo = 0; hi = 1000; break;
    case 17: lo = -500; hi = 500; break;
    case 21: case 23: case 25: case 27: case 33: case 35: lo = -12000; hi = 5000; break;
    case 22: case 24: lo = -16000; hi = 4500; break;
    case 26: case 28: case 30: case 34: case 36: case 38: lo = -12000; hi = 8000; break;
    case 29: lo = 0; hi = 1000; break;
    case 37: lo = 0; hi = 1440; break;
    case 31: case 32: case 39: case 40: lo = -1200; hi = 1200; break;
    case 48: lo = 0; hi = 1440; break;
    case 51: lo = -120; hi = 120; break;
    case 52: lo = -99; hi = 99; break;
    case 56: lo = 0; hi = 1200; break;
    case 57: lo = 0; hi = 127; break;
    case 58: lo = -1; hi = 127; break;
    default: break;
    }
    return (int16_t)(v < lo ? lo : v > hi ? hi : v);
}

typedef struct {
    int16_t v[NGEN];
    uint8_t set[NGEN];
} gset;
static void gset_apply(gset *s, const uint8_t *gen, uint32_t g0, uint32_t g1, uint32_t ngen)
{
    uint32_t i;
    for (i = g0; i < g1 && i < ngen; i++) {
        uint16_t op = rd16(gen + 4 * i), amt = rd16(gen + 4 * i + 2);
        if (op >= NGEN)
            continue;
        s->v[op] = (int16_t)amt;
        s->set[op] = 1;
    }
}

int cv_font_add(const uint8_t *pdta, uint32_t n, uint32_t smpl_bytes, const char *name)
{
    const uint8_t *ph = 0, *pb = 0, *pg = 0, *in = 0, *ib = 0, *ig = 0, *sh = 0;
    uint32_t nph = 0, npb = 0, npg = 0, nin = 0, nib = 0, nig = 0, nsh = 0, o = 0;
    cv_font *f;
    int p, zcap = 0;
    while (o + 8 <= n) {
        uint32_t sz = rd32(pdta + o + 4);
        const uint8_t *d = pdta + o + 8;
        if (sz > n - o - 8)
            return fail("a broken preset table");
        if (!memcmp(pdta + o, "phdr", 4)) { ph = d; nph = sz / 38; }
        else if (!memcmp(pdta + o, "pbag", 4)) { pb = d; npb = sz / 4; }
        else if (!memcmp(pdta + o, "pgen", 4)) { pg = d; npg = sz / 4; }
        else if (!memcmp(pdta + o, "inst", 4)) { in = d; nin = sz / 22; }
        else if (!memcmp(pdta + o, "ibag", 4)) { ib = d; nib = sz / 4; }
        else if (!memcmp(pdta + o, "igen", 4)) { ig = d; nig = sz / 4; }
        else if (!memcmp(pdta + o, "shdr", 4)) { sh = d; nsh = sz / 46; }
        o += 8 + sz + (sz & 1);
    }
    if (!ph || !pb || !pg || !in || !ib || !ig || !sh || nph < 2 || nin < 2 || nsh < 2)
        return fail("not an .sf2 file (its preset tables are missing)");
    plan_ok = 0;
    F = realloc(F, (size_t)(NF + 1) * sizeof *F);
    f = &F[NF];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name ? name : "");
    f->smpl_bytes = smpl_bytes;
    f->nsh = (int)nsh - 1;
    f->sh = calloc((size_t)f->nsh, sizeof *f->sh);
    for (p = 0; p < f->nsh; p++) {
        const uint8_t *s = sh + 46 * (uint32_t)p;
        name_of(f->sh[p].name, s, 20);
        f->sh[p].start = rd32(s + 20);
        f->sh[p].end = rd32(s + 24);
        f->sh[p].ls = rd32(s + 28);
        f->sh[p].le = rd32(s + 32);
        f->sh[p].rate = rd32(s + 36);
        f->sh[p].pitch = s[40];
        f->sh[p].corr = (int8_t)s[41];
        f->sh[p].type = rd16(s + 44);
    }
    f->npr = (int)nph - 1;
    f->pr = calloc((size_t)f->npr, sizeof *f->pr);
    for (p = 0; p < f->npr; p++) {
        const uint8_t *q = ph + 38 * (uint32_t)p;
        uint32_t b0 = rd16(q + 24), b1 = rd16(q + 38 + 24), bz;
        gset pglob;
        cv_preset *pr = &f->pr[p];
        name_of(pr->name, q, 20);
        pr->prog = rd16(q + 20);
        pr->bank = rd16(q + 22);
        pr->sel = 1;
        pr->z0 = f->nz;
        memset(&pglob, 0, sizeof pglob);
        for (bz = b0; bz < b1 && bz + 1 < npb; bz++) {
            uint32_t g0 = rd16(pb + 4 * bz), g1 = rd16(pb + 4 * (bz + 1)), gi;
            gset pz = pglob;
            int inst = -1;
            for (gi = g0; gi < g1 && gi < npg; gi++)
                if (rd16(pg + 4 * gi) == 41)
                    inst = rd16(pg + 4 * gi + 2);
            if (inst < 0) {                           /* the global zone: defaults for the others */
                if (bz == b0)
                    gset_apply(&pglob, pg, g0, g1, npg);
                continue;
            }
            gset_apply(&pz, pg, g0, g1, npg);
            if ((uint32_t)inst + 1 >= nin)
                continue;
            {
                uint32_t i0 = rd16(in + 22 * (uint32_t)inst + 20), i1 = rd16(in + 22 * (uint32_t)(inst + 1) + 20), iz;
                gset iglob;
                memset(&iglob, 0, sizeof iglob);
                for (iz = i0; iz < i1 && iz + 1 < nib; iz++) {
                    uint32_t h0 = rd16(ib + 4 * iz), h1 = rd16(ib + 4 * (iz + 1)), hi;
                    gset z;
                    int smp = -1, k;
                    cv_zone *cz;
                    uint8_t klo, khi, vlo, vhi, plo, phi;
                    for (hi = h0; hi < h1 && hi < nig; hi++)
                        if (rd16(ig + 4 * hi) == 53)
                            smp = rd16(ig + 4 * hi + 2);
                    if (smp < 0) {
                        if (iz == i0)
                            gset_apply(&iglob, ig, h0, h1, nig);
                        continue;
                    }
                    if (smp >= f->nsh || (f->sh[smp].type & 0x8000u))   /* (ROM samples: not ours to play) */
                        continue;
                    z = iglob;
                    gset_apply(&z, ig, h0, h1, nig);
                    if (f->nz >= zcap) {
                        zcap = zcap ? 2 * zcap : 256;
                        f->z = realloc(f->z, (size_t)zcap * sizeof *f->z);
                    }
                    cz = &f->z[f->nz];
                    gen_defaults(cz->g);
                    for (k = 0; k < NGEN; k++)
                        if (z.set[k])
                            cz->g[k] = z.v[k];
                    klo = (uint8_t)(cz->g[43] & 0xFF);
                    khi = (uint8_t)((uint16_t)cz->g[43] >> 8);
                    vlo = (uint8_t)(cz->g[44] & 0xFF);
                    vhi = (uint8_t)((uint16_t)cz->g[44] >> 8);
                    if (pz.set[43]) {
                        plo = (uint8_t)(pz.v[43] & 0xFF);
                        phi = (uint8_t)((uint16_t)pz.v[43] >> 8);
                        klo = klo > plo ? klo : plo;
                        khi = khi < phi ? khi : phi;
                    }
                    if (pz.set[44]) {
                        plo = (uint8_t)(pz.v[44] & 0xFF);
                        phi = (uint8_t)((uint16_t)pz.v[44] >> 8);
                        vlo = vlo > plo ? vlo : plo;
                        vhi = vhi < phi ? vhi : phi;
                    }
                    if (klo > khi || vlo > vhi || klo > 127)
                        continue;
                    for (k = 0; k < NGEN; k++)
                        if (pz.set[k] && is_additive(k))
                            cz->g[k] = (int16_t)(cz->g[k] + pz.v[k] > 32767 ? 32767 : cz->g[k] + pz.v[k] < -32768 ? -32768 : cz->g[k] + pz.v[k]);
                    for (k = 0; k < NGEN; k++)
                        if (k != 43 && k != 44)
                            cz->g[k] = clampg(k, cz->g[k]);
                    cz->klo = klo;
                    cz->khi = khi > 127 ? 127 : khi;
                    cz->vlo = vlo;
                    cz->vhi = vhi > 127 ? 127 : vhi;
                    cz->sh = smp;
                    cz->var = -1;
                    f->nz++;
                }
            }
        }
        pr->nz = f->nz - pr->z0;
    }
    return NF++;
}

int cv_fonts(void) { return NF; }
const char *cv_font_name(int f) { return f >= 0 && f < NF ? F[f].name : ""; }
int cv_presets(int f) { return f >= 0 && f < NF ? F[f].npr : 0; }
const char *cv_preset_name(int f, int p) { return cv_presets(f) > p && p >= 0 ? F[f].pr[p].name : ""; }
int cv_preset_bank(int f, int p) { return cv_presets(f) > p && p >= 0 ? F[f].pr[p].bank : 0; }
int cv_preset_prog(int f, int p) { return cv_presets(f) > p && p >= 0 ? F[f].pr[p].prog : 0; }
void cv_select(int f, int p, int on)
{
    if (cv_presets(f) > p && p >= 0)
        F[f].pr[p].sel = (uint8_t)(on != 0);
    plan_ok = 0;
}
int cv_selected(int f, int p) { return cv_presets(f) > p && p >= 0 ? F[f].pr[p].sel : 0; }

/* ------------------------------------------------------------------------------- the plan --- */
static void zone_span(const cv_font *f, const cv_zone *z, uint32_t *s, uint32_t *e, uint32_t *ls, uint32_t *le, int *loop)
{
    const sf_shdr *h = &f->sh[z->sh];
    int64_t a = (int64_t)h->start + z->g[0] + 32768 * (int64_t)z->g[4];
    int64_t b = (int64_t)h->end + z->g[1] + 32768 * (int64_t)z->g[12];
    int64_t c = (int64_t)h->ls + z->g[2] + 32768 * (int64_t)z->g[45];
    int64_t d = (int64_t)h->le + z->g[3] + 32768 * (int64_t)z->g[50];
    int64_t lim = f->smpl_bytes / 2u;
    if (a < 0)
        a = 0;
    if (b > lim)
        b = lim;
    if (b < a)
        b = a;
    if (c < a)
        c = a;
    if (d > b)
        d = b;
    *s = (uint32_t)a;
    *e = (uint32_t)b;
    *ls = (uint32_t)c;
    *le = (uint32_t)d;
    *loop = (z->g[54] & 3) == 1 || (z->g[54] & 3) == 3 ? (d > c + 1) : 0;
}
static uint32_t var_frames(const cv_var *v) { return v->loop && v->mode == 1 ? v->le - v->start : v->end - v->start; }
static float var_rate(const cv_var *v, int lvl)
{
    float r0 = (float)F[v->font].sh[v->sh].rate, r = r0;
    if (r < 400.0f)
        r = r0 = 22050.0f;                           /* (a nonsense rate: something playable) */
    if (lvl == 0 && r > 48000.0f)
        r = 44100.0f;
    if (lvl > 0 && r > LVL_CAP[lvl])
        r = LVL_CAP[lvl];
    if (lvl > 0 && r > 44100.0f)
        r = 44100.0f;
    return r;
}
static uint32_t var_bytes(const cv_var *v, int lvl)
{
    float r0 = (float)F[v->font].sh[v->sh].rate;
    double n = (double)var_frames(v) * var_rate(v, lvl) / (r0 < 400.0f ? 22050.0 : r0) + 1.0;
    uint32_t b = lvl == 0 ? (uint32_t)(n * 2.0) : (uint32_t)((n + 15.0) / 16.0) * R1_BLKB + R1_BLKB;
    return (b + 3u) & ~3u;
}
uint32_t cv_preset_bytes(int f, int p)
{
    uint32_t sum = 0;
    int k;
    if (cv_presets(f) <= p || p < 0)
        return 0;
    for (k = 0; k < F[f].pr[p].nz; k++) {
        cv_zone *z = &F[f].z[F[f].pr[p].z0 + k];
        uint32_t s, e, ls, le;
        int lp;
        zone_span(&F[f], z, &s, &e, &ls, &le, &lp);
        sum += 2u * (lp && (z->g[54] & 3) == 1 ? le - s : e - s);
    }
    return sum;
}

static int nparams;                                   /* (the plan's count of distinct parameter sets) */
static void zone_param(const cv_zone *z, r1_param_t *p)
{
    const int16_t *g = z->g;
    memset(p, 0, sizeof *p);
    p->scale = g[56];
    p->fc = g[8];
    p->q = g[9];
    p->vd = g[33]; p->va = g[34]; p->vh = g[35]; p->vdc = g[36]; p->vs = g[37]; p->vr = g[38];
    p->md = g[25]; p->ma = g[26]; p->mh = g[27]; p->mdc = g[28]; p->ms = g[29]; p->mr = g[30];
    p->m2pitch = g[7];
    p->m2fc = g[11];
    p->k2vh = g[39];
    p->k2vd = g[40];
    p->vibd = g[23]; p->vibf = g[24]; p->vib2p = g[6];
    p->lfod = g[21]; p->lfof = g[22]; p->lfo2p = g[5]; p->lfo2fc = g[10]; p->lfo2v = g[13];
    p->rev = g[16];
    p->cho = g[15];
    p->excl = (uint8_t)g[57];
    p->fkey = (uint8_t)(g[46] < 0 ? 255 : g[46]);
    p->fvel = (uint8_t)(g[47] < 0 ? 255 : g[47]);
}

static int cmp_var_size(const void *a, const void *b)      /* biggest first (ties: in order, as qsort varies) */
{
    const cv_var *x = &V[*(const int *)a], *y = &V[*(const int *)b];
    return x->est < y->est ? 1 : x->est > y->est ? -1 : *(const int *)a - *(const int *)b;
}
static int cmp_var_font(const void *a, const void *b)      /* the fonts in order, each biggest first */
{
    const cv_var *x = &V[*(const int *)a], *y = &V[*(const int *)b];
    return x->font != y->font ? x->font - y->font : cmp_var_size(a, b);
}
static uint32_t meta_bytes(int *nfont_, int *npre_, int *nzone_)
{
    int f, p, nf = 0, np = 0, nz = 0;
    for (f = 0; f < NF; f++) {
        int any = 0;
        for (p = 0; p < F[f].npr; p++)
            if (F[f].pr[p].sel && F[f].pr[p].nz) {
                any = 1;
                np++;
                nz += F[f].pr[p].nz;
            }
        nf += any;
    }
    if (nfont_)
        *nfont_ = nf;
    if (npre_)
        *npre_ = np;
    if (nzone_)
        *nzone_ = nz;
    return (uint32_t)(sizeof(r1_bank_t) + 32u * (uint32_t)nf + 32u * (uint32_t)np + 16u * (uint32_t)nz +
                      64u * (uint32_t)nparams + 32u * (uint32_t)NV + 64u);
}

/* first fit, biggest first: 0 if everything fits in A then B */
static int place_by(int (*cmp)(const void *, const void *), uint32_t room_a, uint32_t room_b, int *worst)
{
    int *ord = malloc((size_t)(NV ? NV : 1) * sizeof *ord), i;
    uint32_t ua = 0, ub = 0;
    *worst = -1;
    for (i = 0; i < NV; i++)
        ord[i] = i;
    qsort(ord, (size_t)NV, sizeof *ord, cmp);
    for (i = 0; i < NV; i++) {
        cv_var *v = &V[ord[i]];
        if (ua + v->est <= room_a) {
            v->region = 0;
            ua += v->est;
        } else if (ub + v->est <= room_b) {
            v->region = 1;
            ub += v->est;
        } else {
            if (*worst < 0)
                *worst = ord[i];
            v->region = -1;
        }
    }
    free(ord);
    return *worst < 0 ? 0 : -1;
}
/* the samples into the regions: the fonts in order first, so that region A holds whole fonts (they play before
 * region B is in, and if it never comes); biggest first when that does not fit (it packs closer) */
static int place(uint32_t room_a, uint32_t room_b, int *worst)
{
    return place_by(cmp_var_font, room_a, room_b, worst) ? place_by(cmp_var_size, room_a, room_b, worst) : 0;
}

int cv_plan(uint32_t cap_a, uint32_t cap_b)
{
    int f, k, i;
    r1_param_t *ps = 0;
    vars_free();
    snr_sum = snr_min = 0.0;
    snr_n = 0;
    cap_a_ = cap_a;
    cap_b_ = cap_b;
    over_ = 0;
    plan_ok = 0;
    nparams = 0;
    for (f = 0; f < NF; f++)                          /* the bank samples the chosen presets use */
        for (k = 0; k < F[f].nz; k++)
            F[f].z[k].var = -1;
    for (f = 0; f < NF; f++) {
        int p;
        for (p = 0; p < F[f].npr; p++) {
            if (!F[f].pr[p].sel)
                continue;
            for (k = F[f].pr[p].z0; k < F[f].pr[p].z0 + F[f].pr[p].nz; k++) {
                cv_zone *z = &F[f].z[k];
                uint32_t s, e, ls, le;
                int lp, j, mode = z->g[54] & 3;
                r1_param_t par;
                zone_span(&F[f], z, &s, &e, &ls, &le, &lp);
                if (e <= s)
                    continue;
                for (j = 0; j < NV; j++) {
                    cv_var *v = &V[j];
                    if (v->font == f && v->sh == z->sh && v->start == s && v->end == e && v->loop == lp &&
                        (!lp || (v->ls == ls && v->le == le && v->mode == (mode == 3 ? 3 : 1))))
                        break;
                }
                if (j == NV) {
                    cv_var *v;
                    V = realloc(V, (size_t)(NV + 1) * sizeof *V);
                    v = &V[NV++];
                    memset(v, 0, sizeof *v);
                    v->font = f;
                    v->sh = z->sh;
                    v->start = s;
                    v->end = e;
                    v->ls = ls;
                    v->le = le;
                    v->loop = (uint8_t)lp;
                    v->mode = (uint8_t)(lp ? (mode == 3 ? 3 : 1) : 0);
                }
                z->var = j;
                zone_param(z, &par);                  /* count the distinct parameter sets */
                for (i = 0; i < nparams; i++)
                    if (!memcmp(&ps[i], &par, sizeof par))
                        break;
                if (i == nparams) {
                    ps = realloc(ps, (size_t)(nparams + 1) * sizeof *ps);
                    ps[nparams++] = par;
                }
            }
        }
    }
    free(ps);
    if (!NV)
        return fail("nothing chosen");
    for (i = 0; i < NV; i++) {
        V[i].level = 0;
        V[i].est = var_bytes(&V[i], 0);
    }
    {
        uint32_t meta = meta_bytes(0, 0, 0), room_a, room_b = cap_b > 16u ? cap_b - 16u : 0u;
        int worst;
        if (meta + 1024u > cap_a)
            return fail("too many presets: their descriptions alone fill the firmware's room");
        room_a = cap_a - meta;
        for (;;) {
            uint64_t sum = 0;
            int big = -1;
            for (i = 0; i < NV; i++)
                sum += V[i].est;
            if (sum <= (uint64_t)room_a + room_b && !place(room_a, room_b, &worst))
                break;
            for (i = 0; i < NV; i++)                  /* the best kept so far, the biggest of them: a level down */
                if (V[i].level < NLVL - 1 && (big < 0 || V[i].level < V[big].level ||
                                              (V[i].level == V[big].level && V[i].est > V[big].est)))
                    big = i;
            if (big < 0) {
                over_ = (uint32_t)(sum > (uint64_t)room_a + room_b ? sum - room_a - room_b : 1u);
                snprintf(err, sizeof err, "too big: %u KB over at the lowest quality; choose fewer presets", (unsigned)(over_ + 1023u) / 1024u);
                return -2;
            }
            V[big].level++;
            V[big].est = var_bytes(&V[big], V[big].level);
        }
        planned_ = meta;
        for (i = 0; i < NV; i++)
            planned_ += V[i].est;
    }
    for (i = 0; i < NV; i++) {                        /* the spans: each sample header's data, whole */
        int j;
        for (j = 0; j < NSP; j++)
            if (SP[j].font == V[i].font && SP[j].sh == V[i].sh)
                break;
        if (j == NSP) {
            SP = realloc(SP, (size_t)(NSP + 1) * sizeof *SP);
            SP[NSP].font = V[i].font;
            SP[NSP].sh = V[i].sh;
            SP[NSP].start = V[i].start;
            SP[NSP].end = V[i].end;
            SP[NSP].given = 0;
            NSP++;
        } else {
            if (V[i].start < SP[j].start)
                SP[j].start = V[i].start;
            if (V[i].end > SP[j].end)
                SP[j].end = V[i].end;
        }
    }
    plan_ok = 1;
    return 0;
}
uint32_t cv_over(void) { return over_; }
int cv_plan_level(int lvl, uint32_t *bytes)           /* the plan's samples at a quality level, their bytes */
{
    int i, n = 0;
    *bytes = 0;
    for (i = 0; i < NV; i++)
        if (V[i].level == lvl) {
            n++;
            *bytes += V[i].est;
        }
    return n;
}
uint32_t cv_planned(void) { return planned_; }
int cv_needs(void) { return NSP; }
void cv_need(int i, uint32_t *font, uint32_t *off, uint32_t *bytes)
{
    *font = *off = *bytes = 0;
    if (i < 0 || i >= NSP)
        return;
    *font = (uint32_t)SP[i].font;
    *off = 2u * SP[i].start;
    *bytes = 2u * (SP[i].end - SP[i].start);
}

/* ----------------------------------------------------------------------------- encoding --- */
static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0;
    int k;
    for (k = 1; k < 40; k++) {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
    }
    return s;
}
#define WIN_N 4096
static float win[WIN_N + 1];                          /* a Kaiser window (beta 7) over |t| / half, 0 .. 1 */
static void win_init(void)
{
    static int done;
    int i;
    double i0b;
    if (done)
        return;
    i0b = bessel_i0(7.0);
    for (i = 0; i <= WIN_N; i++) {
        double u = (double)i / WIN_N;
        win[i] = (float)(bessel_i0(7.0 * sqrt(1.0 - u * u)) / i0b);
    }
    done = 1;
}

/* the span's samples, resampled by rr (<= 1) so that the loop keeps a whole number of samples; the loop
 * is resampled as the endless repeat it plays as, so it joins without a click */
static float *resample(const int16_t *x, uint32_t n, const cv_var *v, double rr, uint32_t *out_n, uint32_t *ols, uint32_t *ole)
{
    uint32_t ls = v->ls - v->start, le = v->le - v->start, m, j;
    double ls2 = 0.0;
    float *y;
    int loop = v->loop;
    if (loop) {
        uint32_t L = le - ls, L2 = (uint32_t)floor(L * rr + 0.5);
        if (L2 < 1)
            L2 = 1;
        rr = (double)L2 / (double)L;
        *ols = (uint32_t)floor(ls * rr + 0.5);
        *ole = *ols + L2;
        ls2 = *ols;
    } else
        *ols = *ole = 0;
    m = v->mode == 1 ? *ole : (uint32_t)floor((double)n * rr + 0.5);
    if (loop && m < *ole)
        m = *ole;
    if (m < 1)
        m = 1;
    y = malloc(m * sizeof *y);
    if (rr >= 0.9999) {                               /* the same rate: as it is */
        for (j = 0; j < m; j++) {
            uint32_t i = j;
            if (loop && v->mode == 1 && i >= le)
                i = ls + (i - ls) % (le - ls);
            y[j] = i < n ? (float)x[i] : 0.0f;
        }
    } else {
        double fc = 0.5 * rr * 0.92;
        int half = (int)ceil(14.0 / rr);
        win_init();
        for (j = 0; j < m; j++) {
            double c = loop ? (double)ls + ((double)j - ls2) / rr : (double)j / rr, acc = 0.0, wsum = 0.0;
            int k0 = (int)floor(c) - half, k;
            int in_loop = loop && j >= *ols && (v->mode == 1 || j < *ole);
            for (k = k0; k <= k0 + 2 * half + 1; k++) {
                double t = (double)k - c, at = fabs(t), s, w;
                int64_t i = k;
                if (at >= half)
                    continue;
                w = win[(int)(at / half * WIN_N + 0.5)];
                s = at < 1e-9 ? 2.0 * fc : sin(2.0 * PI_ * fc * t) / (PI_ * t);
                if (in_loop) {                        /* within the loop: around itself, both ways */
                    int64_t L = (int64_t)(le - ls);
                    if (i >= (int64_t)le)
                        i = ls + (i - ls) % L;
                    else if (i < (int64_t)ls)
                        i = le - 1 - ((int64_t)ls - 1 - i) % L;
                }
                acc += (i >= 0 && i < (int64_t)n ? (double)x[i] : 0.0) * s * w;
                wsum += s * w;
            }
            y[j] = (float)(wsum != 0.0 ? acc / wsum : 0.0);
        }
    }
    *out_n = m;
    return y;
}

static int q16(float x) { return x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (int)lrintf(x); }
static void adpcm_encode(const float *y, uint32_t m, uint8_t *o, int16_t *dec)
{
    uint32_t nb = (m + 15u) / 16u, b;
    int32_t p1 = 0, p2 = 0;
    for (b = 0; b < nb; b++) {
        int bf = 0, bs = 0, f, sh, i;
        double be = 1e300;
        uint8_t bn[16] = {0};
        int16_t bd[16];
        for (f = 0; f < 4; f++)
            for (sh = 0; sh <= 12; sh++) {
                int32_t q1 = p1, q2 = p2;
                double e = 0.0;
                uint8_t nn[16];
                int16_t dd[16];
                for (i = 0; i < 16; i++) {
                    uint32_t idx = b * 16u + (uint32_t)i;
                    int32_t tgt = idx < m ? q16(y[idx]) : 0, pred = (q1 * R1_P1[f] + q2 * R1_P2[f] + 32) >> 6, d, s;
                    double diff;
                    d = (int32_t)lrint((double)(tgt - pred) / (double)(1 << sh));
                    d = d < -8 ? -8 : d > 7 ? 7 : d;
                    s = r1_cl16(d * (1 << sh) + pred);
                    diff = (double)(s - tgt);
                    e += diff * diff;
                    nn[i] = (uint8_t)(d & 15);
                    dd[i] = (int16_t)s;
                    q2 = q1;
                    q1 = s;
                    if (e >= be)
                        break;
                }
                if (e < be) {
                    be = e;
                    bf = f;
                    bs = sh;
                    memcpy(bn, nn, 16);
                    memcpy(bd, dd, sizeof bd);
                }
            }
        o[9 * b] = (uint8_t)(bs << 4 | bf);
        for (i = 0; i < 16; i += 2)
            o[9 * b + 1 + (uint32_t)i / 2u] = (uint8_t)(bn[i] << 4 | bn[i + 1]);
        {                                             /* the decoder's view: exactly what will play */
            int32_t h1 = p1, h2 = p2;
            r1_adpcm_block(o + 9 * b, dec + 16 * b, &h1, &h2);
            p1 = h1;
            p2 = h2;
        }
    }
}

static void encode_var(cv_var *v, const int16_t *x, uint32_t n)
{
    float r0 = (float)F[v->font].sh[v->sh].rate, r = var_rate(v, v->level);
    double rr = r0 < 400.0f ? 1.0 : (double)r / (double)r0;
    uint32_t m, ls2, le2, j;
    float *y;
    if (rr > 1.0)
        rr = 1.0;
    y = resample(x, n, v, rr, &m, &ls2, &le2);
    if (v->loop)                                      /* (the rate the loop's whole number made) */
        v->rate = (r0 < 400.0f ? 22050.0f : r0) * (float)((double)(le2 - ls2) / (double)(v->le - v->ls));
    else
        v->rate = (r0 < 400.0f ? 22050.0f : r0) * (float)rr;
    v->frames = m;
    v->rls = ls2;
    v->rle = le2;
    if (v->level == 0) {
        v->fmt = R1_PCM16;
        v->dlen = (2u * m + 3u) & ~3u;
        v->data = calloc(v->dlen, 1);
        for (j = 0; j < m; j++) {
            int s = q16(y[j]);
            v->data[2 * j] = (uint8_t)s;
            v->data[2 * j + 1] = (uint8_t)(s >> 8);
        }
    } else {
        uint32_t nb = (m + 15u) / 16u;
        int16_t *dec = calloc(nb * 16u, sizeof *dec);
        v->fmt = R1_ADPCM;
        v->dlen = (nb * R1_BLKB + 3u) & ~3u;
        v->data = calloc(v->dlen, 1);
        adpcm_encode(y, m, v->data, dec);
        {                                             /* how close: signal against the coding's error */
            double sig = 0.0, noise = 0.0, db;
            for (j = 0; j < m; j++) {
                double e = (double)dec[j] - (double)q16(y[j]);
                sig += (double)y[j] * y[j];
                noise += e * e;
            }
            if (sig > 1e3 * m) {                      /* (not near-silence) */
                db = 10.0 * log10(sig / (noise > 1.0 ? noise : 1.0));
                snr_sum += db;
                if (!snr_n || db < snr_min)
                    snr_min = db;
                snr_n++;
            }
        }
        if (v->loop) {
            uint32_t b0 = (ls2 / 16u) * 16u;
            v->h1 = (int16_t)(b0 >= 1 ? dec[b0 - 1] : 0);
            v->h2 = (int16_t)(b0 >= 2 ? dec[b0 - 2] : 0);
        }
        free(dec);
    }
    free(y);
    v->done = 1;
}

int cv_give(int i, const uint8_t *pcm16, uint32_t bytes)
{
    cv_span *s;
    int k;
    if (i < 0 || i >= NSP)
        return fail("no such span");
    s = &SP[i];
    if (bytes < 2u * (s->end - s->start))
        return fail("a span came short");
    for (k = 0; k < NV; k++) {
        cv_var *v = &V[k];
        if (v->font != s->font || v->sh != s->sh || v->done)
            continue;
        {
            uint32_t n = v->end - v->start, j, at = 2u * (v->start - s->start);
            int16_t *x = malloc((n ? n : 1) * sizeof *x);
            for (j = 0; j < n; j++)
                x[j] = (int16_t)(pcm16[at + 2 * j] | pcm16[at + 2 * j + 1] << 8);
            encode_var(v, x, n);
            free(x);
        }
    }
    s->given = 1;
    return 0;
}

/* ------------------------------------------------------------------------------ the bank --- */
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v & 0xFFFF); wr16(p + 2, v >> 16); }

typedef struct {
    int font, pre;
} pref;
static int cmp_pref(const void *a, const void *b)
{
    const pref *x = a, *y = b;
    const cv_preset *p = &F[x->font].pr[x->pre], *q = &F[y->font].pr[y->pre];
    if (x->font != y->font)
        return x->font - y->font;
    if (p->bank != q->bank)
        return (int)p->bank - (int)q->bank;
    if (p->prog != q->prog)
        return (int)p->prog - (int)q->prog;
    return x->pre - y->pre;
}

uint32_t cv_bank_id(const char *name)
{
    uint32_t id = 0x1234567u, k;
    for (k = 0; name && name[k]; k++)
        id = id * 31u + (uint8_t)name[k];
    return id ^ planned_;
}

int cv_build(uint32_t id, const char *name)
{
    int nf, np, nz, i, f, worst;
    uint32_t meta, room_a, room_b, ua, ub, la, lb, o;
    pref *pl;
    int *fmap;
    r1_param_t *ps = 0;
    r1_bank_t h;
    if (!plan_ok)
        return fail("no plan that fits (cv_plan first)");
    for (i = 0; i < NSP; i++)
        if (!SP[i].given)
            return fail("a span was not given");
    for (i = 0; i < NV; i++)                          /* the real sizes, now they are encoded */
        V[i].est = V[i].dlen;
    meta = meta_bytes(&nf, &np, &nz);
    room_a = cap_a_ > meta ? cap_a_ - meta : 0;
    room_b = cap_b_ > 16u ? cap_b_ - 16u : 0;
    if (place(room_a, room_b, &worst))
        return fail("it no longer fits once encoded (a few bytes over): choose a preset less");
    free(outa);
    free(outb);
    outa = calloc(cap_a_ ? cap_a_ : 1, 1);
    outb = calloc(cap_b_ ? cap_b_ : 1, 1);
    /* presets: the chosen ones with zones, by font, bank, program; fonts renumbered without the empty */
    pl = malloc((size_t)(np ? np : 1) * sizeof *pl);
    fmap = malloc((size_t)(NF ? NF : 1) * sizeof *fmap);
    np = 0;
    nf = 0;
    for (f = 0; f < NF; f++) {
        int p, any = 0;
        for (p = 0; p < F[f].npr; p++)
            if (F[f].pr[p].sel && F[f].pr[p].nz) {
                int k, has = 0;
                for (k = F[f].pr[p].z0; k < F[f].pr[p].z0 + F[f].pr[p].nz; k++)
                    has |= F[f].z[k].var >= 0;
                if (!has)
                    continue;
                pl[np].font = f;
                pl[np].pre = p;
                np++;
                any = 1;
            }
        fmap[f] = any ? nf++ : -1;
    }
    qsort(pl, (size_t)np, sizeof *pl, cmp_pref);
    memset(&h, 0, sizeof h);
    h.off_font = sizeof(r1_bank_t);
    h.off_preset = h.off_font + 32u * (uint32_t)nf;
    h.off_zone = h.off_preset + 32u * (uint32_t)np;
    nz = 0;
    for (i = 0; i < np; i++) {
        const cv_preset *p = &F[pl[i].font].pr[pl[i].pre];
        int k;
        for (k = p->z0; k < p->z0 + p->nz; k++)
            nz += F[pl[i].font].z[k].var >= 0;
    }
    h.off_param = h.off_zone + 16u * (uint32_t)nz;
    /* zones (and their parameter sets, kept once) */
    o = h.off_zone;
    {
        int zi = 0, npar = 0;
        uint8_t *pp = outa + h.off_preset;
        int cur_font = -1, first = 0;
        uint8_t *fp = outa + h.off_font;
        for (i = 0; i < np; i++) {
            const cv_font *ff = &F[pl[i].font];
            const cv_preset *p = &ff->pr[pl[i].pre];
            int k, cnt = 0;
            if (fmap[pl[i].font] != cur_font) {
                if (cur_font >= 0)
                    wr16(fp + 32 * cur_font + 26, (uint32_t)(i - first));
                cur_font = fmap[pl[i].font];
                first = i;
                memcpy(fp + 32 * cur_font, ff->name, strlen(ff->name) < 23 ? strlen(ff->name) : 23);
                wr16(fp + 32 * cur_font + 24, (uint32_t)i);
            }
            memcpy(pp + 32 * i, p->name, strlen(p->name) < 20 ? strlen(p->name) : 20);
            wr16(pp + 32 * i + 20, p->bank > 128 ? 128 : p->bank);
            pp[32 * i + 22] = (uint8_t)(p->prog & 127);
            pp[32 * i + 23] = (uint8_t)fmap[pl[i].font];
            wr32(pp + 32 * i + 24, (uint32_t)zi);
            for (k = p->z0; k < p->z0 + p->nz; k++) {
                const cv_zone *z = &ff->z[k];
                const sf_shdr *sh = &ff->sh[z->sh];
                r1_param_t par;
                uint8_t *zp = outa + o;
                int pi, root = z->g[58] >= 0 ? z->g[58] : sh->pitch > 127 ? 60 : sh->pitch;
                if (z->var < 0)
                    continue;
                zone_param(z, &par);
                for (pi = 0; pi < npar; pi++)
                    if (!memcmp(&ps[pi], &par, sizeof par))
                        break;
                if (pi == npar) {
                    ps = realloc(ps, (size_t)(npar + 1) * sizeof *ps);
                    ps[npar++] = par;
                }
                zp[0] = z->klo;
                zp[1] = z->khi;
                zp[2] = z->vlo;
                zp[3] = z->vhi;
                wr16(zp + 4, (uint32_t)z->var);
                zp[6] = (uint8_t)root;
                zp[7] = (uint8_t)(V[z->var].loop ? V[z->var].mode : 0);
                wr16(zp + 8, (uint32_t)(int16_t)(z->g[51] * 100 + z->g[52] + sh->corr) & 0xFFFF);
                wr16(zp + 10, (uint32_t)(uint16_t)z->g[17]);
                wr16(zp + 12, (uint32_t)(uint16_t)z->g[48]);
                wr16(zp + 14, (uint32_t)pi);
                o += 16;
                zi++;
                cnt++;
            }
            wr16(pp + 32 * i + 28, (uint32_t)cnt);
        }
        if (cur_font >= 0)
            wr16(fp + 32 * cur_font + 26, (uint32_t)(np - first));
        h.nparam = (uint32_t)npar;
        memcpy(outa + h.off_param, ps, (size_t)npar * sizeof *ps);   /* (the host is little-endian, as the FM-1) */
        nparams = npar;
    }
    free(ps);
    h.off_sample = h.off_param + 64u * h.nparam;
    o = (h.off_sample + 32u * (uint32_t)NV + 3u) & ~3u;
    if (o > cap_a_)
        return fail("the descriptions do not fit");
    /* sample data: A after the descriptions, B after its header */
    ua = o;
    ub = 16;
    if (place(cap_a_ - o, room_b, &worst))
        return fail("it no longer fits once encoded (a few bytes over): choose a preset less");
    for (i = 0; i < NV; i++) {
        cv_var *v = &V[i];
        uint8_t *sp = outa + h.off_sample + 32u * (uint32_t)i;
        if (v->region == 0) {
            v->off = ua;
            memcpy(outa + ua, v->data, v->dlen);
            ua += v->dlen;
        } else {
            v->off = ub | R1_REGION_B;
            memcpy(outb + ub, v->data, v->dlen);
            ub += v->dlen;
        }
        wr32(sp, v->off);
        wr32(sp + 4, v->frames);
        wr32(sp + 8, v->loop ? v->rls : 0);
        wr32(sp + 12, v->loop ? v->rle : 0);
        memcpy(sp + 16, &v->rate, 4);
        sp[20] = v->fmt;
        wr16(sp + 22, (uint32_t)(uint16_t)v->h1);
        wr16(sp + 24, (uint32_t)(uint16_t)v->h2);
        if (fmap[v->font] >= 0) {                     /* the font's bytes */
            uint8_t *fb = outa + h.off_font + 32u * (uint32_t)fmap[v->font] + 28;
            wr32(fb, rd32(fb) + v->dlen);
        }
    }
    la = ua;
    lb = ub > 16u ? ub : 0u;
    h.magic = R1_MAGIC;
    h.version = R1_VERSION;
    h.nfont = (uint16_t)nf;
    h.len = la;
    h.id = id;
    h.npreset = (uint32_t)np;
    h.nzone = (uint32_t)nz;
    h.nsample = (uint32_t)NV;
    snprintf(h.name, sizeof h.name, "%s", name ? name : "");
    if (lb) {
        r1_bankb_t hb;
        hb.magic = R1_MAGIC_B;
        hb.id = id;
        hb.len = lb;
        hb.crc = r1_crc32(0, outb + 16, lb - 16u);
        memcpy(outb, &hb, sizeof hb);
        h.lenb = lb;
        h.crcb = hb.crc;
    }
    memcpy(outa, &h, sizeof h);
    h.crc = r1_crc32(0, outa + 16, la - 16u);
    memcpy(outa, &h, sizeof h);
    outa_n = la;
    outb_n = lb;
    /* the report */
    {
        size_t cap = 512 + (size_t)nf * 96u;
        int lv[NLVL] = {0};
        char *r;
        free(rep);
        rep = r = malloc(cap);
        for (i = 0; i < NV; i++)
            lv[V[i].level]++;
        r += sprintf(r, "{\"a\":%u,\"b\":%u,\"cap_a\":%u,\"cap_b\":%u,\"presets\":%d,\"zones\":%d,\"params\":%u,\"samples\":%d,\"levels\":[",
                     (unsigned)la, (unsigned)lb, (unsigned)cap_a_, (unsigned)cap_b_, np, nz, (unsigned)h.nparam, NV);
        for (i = 0; i < NLVL; i++)
            r += sprintf(r, "%s%d", i ? "," : "", lv[i]);
        r += sprintf(r, "],\"snr_mean\":%.1f,\"snr_min\":%.1f,\"fonts\":[", snr_n ? snr_sum / snr_n : 0.0, snr_n ? snr_min : 0.0);
        for (f = 0, i = 0; f < NF; f++) {
            const uint8_t *fb;
            if (fmap[f] < 0)
                continue;
            fb = outa + h.off_font + 32u * (uint32_t)fmap[f];
            r += sprintf(r, "%s{\"presets\":%u,\"bytes\":%u}", i++ ? "," : "", (unsigned)rd16(fb + 26), (unsigned)rd32(fb + 28));
        }
        sprintf(r, "]}");
    }
    free(pl);
    free(fmap);
    return 0;
}

const uint8_t *cv_out_a(uint32_t *len)
{
    *len = outa_n;
    return outa;
}
const uint8_t *cv_out_b(uint32_t *len)
{
    *len = outb_n;
    return outb;
}
const char *cv_report(void) { return rep ? rep : "{}"; }
