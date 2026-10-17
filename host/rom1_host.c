/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1 on the host: the whole app (UI, engine, settings, the bank's upload) against a simulated FM-1 driven
 * by a script. The engine renders as on the device (128-frame blocks); the screen goes to PNGs, the lights
 * to text, the sound to a WAV; the flash is a simulated NOR chip (erased to FF, programming only clears
 * bits), kept across a simulated restart. Region A comes from a file (the starter's by default).
 * (From SEQ-4's, CRT-4's, LCD-4's, IND-1's, AMB-1's and X0X's simulators.)
 *
 *   rom1_host SCRIPT [OUTDIR]
 *
 * Script, one command per line (# comments):
 *   bank A.bin [B.bin]           region A from a file (and region B written to the flash as the upload does);
 *                                then a restart
 *   upload B.bin [again]         region B written as the upload page does it (begin, pages, end; again: some
 *                                pages sent twice, as when an answer goes astray)
 *   wait MS                      run the device for MS milliseconds
 *   press BTN | release BTN | tap BTN | hold BTN MS   (BTN: FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+)
 *   key K down|up | tapkey K     (K: 0..26 = F3..G5, or w0..w15 white keys, b0..b10 black keys)
 *   turn ENC N [MS]              (ENC: SELECT ALGO PRESET K1 K2 K3 K4; N detents, MS apart, default 80)
 *   spin ENC N                   N detents at once, exactly N (no acceleration)
 *   midi B0 B1 B2                a message coming in over USB (hex bytes)
 *   sysex HEX...                 a SysEx coming in (the bytes between F0 and F7)
 *   wav FILE | wavstop           start / stop recording the output
 *   peakreset                    the output's peak counted again
 *   shot FILE.png                save the screen
 *   leds                         print the lit buttons and keys, and whether the others glow
 *   expect WHAT VALUE            check state: exit 1 on mismatch; VALUE "<N" / ">N" is a bound
 *     WHAT: sel page oct kvel voices notes preset font bank prog vol pan rev cho cutoff mute drums dirty
 *           presets fonts b_ok bank_id peak (x1000) store_writes erases
 *           lights lit_btn lit_keys (how many lit) glow
 *   until WHAT VALUE [MS]        run until the expectation holds (at most MS, default 20000), then check
 *   reboot                       re-run boot from the simulated flash (persistence test)
 *   echo TEXT
 */
#define EN_HOST 1
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#define __attribute__(x)

/* ------------------------------------------------------------ screen --- */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sc = (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8));
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = sc;
}
static uint32_t blits;
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    blits++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = px[j * w + i];
}
#include "../firmware/src/gfx.c"

/* ---------------------------------------------------------- platform --- */
#include "../firmware/src/app/plat.h"
#include "../firmware/src/synth/rom1.h"

static uint32_t now_ms, held_btn, held_keys, master = 2800;
static int32_t enc_acc[NE];
static uint32_t lit_btn, lit_keys, lit_glow;

uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_buttons(void) { return held_btn; }
uint32_t plat_keys(void) { return held_keys; }
int32_t plat_enc(int role)
{
    int32_t v = enc_acc[role];
    enc_acc[role] = 0;
    return v;
}
uint32_t plat_master(void) { return master; }
void plat_leds(uint32_t b, uint32_t k)
{
    lit_btn = b;
    lit_keys = k;
}
void plat_glow(int on) { lit_glow = on != 0; }
void plat_play_red(int red) { (void)red; }
void r1_lock(int on) { (void)on; }

#define MQ 1024
static uint32_t min_q[MQ], mi_w, mi_r, midi_out_count;
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = min_q[mi_r++ % MQ];
    return 1;
}
#ifdef EN_WEB
void web_midi_out(uint32_t pkt);
#endif
int plat_midi_out(uint32_t pkt)
{
    midi_out_count++;
#ifdef EN_WEB
    web_midi_out(pkt);
#endif
    return 1;
}
int plat_usb_up(void) { return 1; }

/* flash: a simulated NOR chip (the device's st_* hooks over it) */
static uint8_t nor[0x100000];
static uint32_t nor_progs, nor_erases;
static int nor_ready;
static void nor_init(void)
{
    if (!nor_ready)
        memset(nor, 0xFF, sizeof nor);
    nor_ready = 1;
}
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    if (off + n > sizeof nor)
        return -8;
    memcpy(dst, nor + off, n);
    return 0;
}
static void audio_silence(void) {}
static int st_erase(uint32_t off)
{
    if (off & 0xFFFu || off + 4096u > sizeof nor)
        return -8;
    memset(nor + off, 0xFF, 4096);
    nor_erases++;
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (off + n > sizeof nor || (off & 0xFFu) + n > 256u)
        return -8;
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    nor_progs++;
    return 0;
}
#include "../firmware/src/storage.c"
static uint32_t store_writes;
int plat_store_load(uint32_t obj, void *dst, uint32_t max) { return st_load(obj, dst, max); }
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    store_writes++;
    return st_save(obj, src, len);
}
/* the bank: region A from a file (in memory, as the app image holds it), region B in the simulated flash */
static uint8_t *bank_a_mem;
static uint32_t bank_a_len;
const uint8_t *plat_bank_a(void) { return bank_a_mem; }
const uint8_t *plat_bank_b(void) { return nor + R1_B_BASE; }
int plat_bank_b_erase(uint32_t off) { return off < R1_B_ROOM && !(off & 0xFFFu) ? st_erase(R1_B_BASE + off) : -8; }
int plat_bank_b_prog(uint32_t off, const void *src, uint32_t n) { return off + n <= R1_B_ROOM ? st_prog(R1_B_BASE + off, src, n) : -8; }
static uint32_t cpu_pct;
uint32_t plat_cpu_pct(void) { return cpu_pct; }
uint32_t plat_xruns(void) { return 0; }

/* -------------------------------------------------------------- app --- */
#include "../firmware/src/app/app.h"
#include "../firmware/src/app/settings.c"
#include "../firmware/src/app/ui.c"
#include "../firmware/src/app/upload.c"

static void png_write(const char *path)
{
    /* RGB PNG with stored (uncompressed) deflate blocks: no zlib needed */
    static uint8_t raw[240 * (1 + 240 * 3)];
    FILE *f = fopen(path, "wb");
    uint32_t crc_t[256], i, j, k, n = 0, a = 1, b = 0, pos, len = sizeof raw;
    uint8_t hdr[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_t[i] = c;
    }
    for (j = 0; j < 240; j++) {
        raw[n++] = 0;
        for (i = 0; i < 240; i++) {
            uint16_t p = fb[j * 240 + i];
            p = (uint16_t)((p >> 8) | (p << 8));
            raw[n++] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
            raw[n++] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            raw[n++] = (uint8_t)((p & 31) * 255 / 31);
        }
    }
    for (i = 0; i < len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
#define PUT32(v) do { uint8_t q_[4] = {(uint8_t)((v) >> 24), (uint8_t)((v) >> 16), (uint8_t)((v) >> 8), (uint8_t)(v)}; fwrite(q_, 1, 4, f); } while (0)
    fwrite(hdr, 1, 8, f);
    {
        uint8_t ih[17] = {'I', 'H', 'D', 'R', 0, 0, 0, 240, 0, 0, 0, 240, 8, 2, 0, 0, 0};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(13u);
        fwrite(ih, 1, 17, f);
        for (i = 0; i < 17; i++)
            c = crc_t[(c ^ ih[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    {
        uint32_t nblk = (len + 65534) / 65535, zlen = 2 + len + nblk * 5 + 4, c = 0xFFFFFFFFu;
        uint8_t *z = malloc(zlen + 4), *q = z;
        memcpy(q, "IDAT", 4);
        q += 4;
        *q++ = 0x78;
        *q++ = 0x01;
        for (pos = 0; pos < len;) {
            uint32_t m = len - pos > 65535 ? 65535 : len - pos;
            *q++ = pos + m >= len ? 1 : 0;
            *q++ = (uint8_t)m;
            *q++ = (uint8_t)(m >> 8);
            *q++ = (uint8_t)~m;
            *q++ = (uint8_t)(~m >> 8);
            memcpy(q, raw + pos, m);
            q += m;
            pos += m;
        }
        *q++ = (uint8_t)(b >> 8);
        *q++ = (uint8_t)b;
        *q++ = (uint8_t)(a >> 8);
        *q++ = (uint8_t)a;
        PUT32(zlen);
        fwrite(z, 1, zlen + 4, f);
        for (i = 0; i < zlen + 4; i++)
            c = crc_t[(c ^ z[i]) & 255] ^ (c >> 8);
        PUT32(~c);
        free(z);
    }
    {
        uint8_t ie[4] = {'I', 'E', 'N', 'D'};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(0u);
        fwrite(ie, 1, 4, f);
        for (i = 0; i < 4; i++)
            c = crc_t[(c ^ ie[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    fclose(f);
}

static FILE *wav;
static float peak_out;
static uint32_t wav_frames;
static void wav_open(const char *path)
{
    static const uint8_t h[44] = {0};
    wav = fopen(path, "wb");
    if (wav)
        fwrite(h, 1, 44, wav);
    wav_frames = 0;
}
static void wav_close(void)
{
    uint32_t bytes = wav_frames * 4, v;
    if (!wav)
        return;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav);
    v = 36 + bytes;
    fwrite(&v, 4, 1, wav);
    fwrite("WAVEfmt ", 1, 8, wav);
    v = 16;
    fwrite(&v, 4, 1, wav);
    {
        uint16_t fmt[2] = {1, 2};
        uint32_t r[2] = {44100, 44100 * 4};
        uint16_t al[2] = {4, 16};
        fwrite(fmt, 2, 2, wav);
        fwrite(r, 4, 2, wav);
        fwrite(al, 2, 2, wav);
    }
    fwrite("data", 1, 4, wav);
    fwrite(&bytes, 4, 1, wav);
    fclose(wav);
    wav = 0;
}

/* --------------------------------------------------------- the device --- */
static double render_ns_total, render_budget_ns_total, audio_due;
static uint32_t ui_due;
static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}
static uint8_t *slurp(const char *p, uint32_t *n)
{
    FILE *f = fopen(p, "rb");
    long s;
    uint8_t *b;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    s = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = calloc((size_t)s + 16, 1);
    if (fread(b, 1, (size_t)s, f) != (size_t)s)
        s = 0;
    fclose(f);
    *n = (uint32_t)s;
    return b;
}
static void boot(void)
{
    nor_init();
    r1_bank_attach(plat_bank_a(), plat_bank_b());
    settings_load();
    lcd_fill(0, 0, 240, 240, 0);
    ui_init();
}

#ifdef EN_WEB
void web_audio(const int32_t *blk, uint32_t n);
#endif
static void run_ms(uint32_t ms)
{
    static int32_t blk[256];
    uint32_t i, k, pkt;
    for (i = 0; i < ms; i++) {
        now_ms++;
        while (audio_due <= (double)now_ms) {      /* 128-frame blocks, as the device renders them */
            double t0 = now_ns(), dt;
            while (plat_midi_in(&pkt))
                r1_midi(pkt >> 8 & 0xFFu, pkt >> 16 & 0x7Fu, pkt >> 24 & 0x7Fu);
            if (uploading)
                memset(blk, 0, sizeof blk);
            else
                r1_render(blk, 128, master);
#ifdef EN_WEB
            web_audio(blk, 128);
#endif
            dt = now_ns() - t0;
            render_ns_total += dt;
            render_budget_ns_total += 128.0 * 1e9 / 44100.0;
            cpu_pct = (uint32_t)(100.0 * dt / (128.0 * 1e9 / 44100.0));
            for (k = 0; k < 128; k++) {
                float l = (float)blk[2 * k] / 8388608.0f, r = (float)blk[2 * k + 1] / 8388608.0f;
                int16_t s[2];
                if (fabsf(l) > peak_out)
                    peak_out = fabsf(l);
                if (fabsf(r) > peak_out)
                    peak_out = fabsf(r);
                s[0] = (int16_t)(l * 32767.0f);
                s[1] = (int16_t)(r * 32767.0f);
                if (wav) {
                    fwrite(s, 2, 2, wav);
                    wav_frames++;
                }
            }
            audio_due += 128.0 * 1000.0 / 44100.0;
        }
        settings_autosave(!r1_voices && !uploading);
        if (now_ms >= ui_due) {
            ui_due = now_ms + 16;
            ui_frame();
        } else {
            ui_input_only();
        }
    }
}

/* region B written as the page writes it: begin, a page at a time, end (again: every 37th page sent twice,
 * as when its answer goes astray) */
static int upload_file(const char *path, int again)
{
    uint32_t n, off;
    uint8_t *b = slurp(path, &n);
    const r1_bank_t *h = r1_bank();
    if (!b || !h)
        return -1;
    if (up_begin(n, h->id))
        return -2;
    for (off = 0; off < n; off += 256u) {
        uint32_t m = n - off < 256u ? n - off : 256u;
        if (up_data(off, b + off, m))
            return -3;
        if (again && !(off / 256u % 37u) && up_data(off, b + off, m))
            return -5;
        if (!(off & 0x3FFFu))
            run_ms(1);
    }
    free(b);
    return up_end() ? 0 : -4;
}

/* ---------------------------------------------------------- script --- */
static const char *const BTN_N[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                      "PLAY", "REC", "OCT-", "OCT+"};
static const char *const ENC_N[NE] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};
static int find_name(const char *const *t, int n, const char *s)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(t[i], s))
            return i;
    return -1;
}
static const uint8_t WHITE_K[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const uint8_t BLACK_K[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
static int key_of(const char *s)
{
    if (s[0] == 'w')
        return WHITE_K[atoi(s + 1) & 15];
    if (s[0] == 'b')
        return BLACK_K[atoi(s + 1) % 11];
    return atoi(s);
}

static int expect_quiet;
static int expect(const char *what, const char *val)
{
    int got;
    const r1_part_t *p = &r1_part[set.sel & 15];
    const r1_preset_t *pr = r1_preset_at(p->preset);
#define IS(x) (!strcmp(what, x))
    if (IS("sel")) got = set.sel;
    else if (IS("page")) got = set.page;
    else if (IS("oct")) got = set.oct;
    else if (IS("kvel")) got = set.kvel;
    else if (IS("voices")) got = r1_voices;
    else if (IS("notes")) got = (int)r1_notes;
    else if (IS("preset")) got = p->preset == 0xFFFF ? -1 : p->preset;
    else if (IS("font")) got = pr ? pr->font : -1;
    else if (IS("bank")) got = pr ? pr->bank : -1;
    else if (IS("prog")) got = pr ? pr->prog : -1;
    else if (IS("vol")) got = p->vol;
    else if (IS("pan")) got = p->pan;
    else if (IS("rev")) got = p->rev;
    else if (IS("cho")) got = p->cho;
    else if (IS("cutoff")) got = p->cutoff;
    else if (IS("mute")) got = p->mute;
    else if (IS("drums")) got = p->drums;
    else if (IS("dirty")) got = settings_is_dirty();
    else if (IS("presets")) got = r1_bank() ? (int)r1_bank()->npreset : 0;
    else if (IS("fonts")) got = r1_fonts();
    else if (IS("b_ok")) got = r1_bank_ok_b();
    else if (IS("peak")) got = (int)(peak_out * 1000.0f);
    else if (IS("store_writes")) got = (int)store_writes;
    else if (IS("erases")) got = (int)nor_erases;
    else if (IS("midi_out")) got = (int)midi_out_count;
    else if (IS("lights")) got = set.lights;
    else if (IS("lit_btn")) got = __builtin_popcount(lit_btn);
    else if (IS("lit_keys")) got = __builtin_popcount(lit_keys);
    else if (IS("glow")) got = (int)lit_glow;
    else {
        printf("FAIL unknown expect %s\n", what);
        return 1;
    }
#undef IS
    if (val[0] == '<' || val[0] == '>') {
        int lim = (int)strtol(val + 1, 0, 0);
        if (val[0] == '<' ? got >= lim : got <= lim) {
            if (!expect_quiet)
                printf("FAIL expect %s %s, got %d\n", what, val, got);
            return 1;
        }
        if (!expect_quiet)
            printf("  ok %s == %d (%s)\n", what, got, val);
        return 0;
    }
    if (got != (int)strtol(val, 0, 0)) {
        if (!expect_quiet)
            printf("FAIL expect %s == %s, got %d\n", what, val, got);
        return 1;
    }
    if (!expect_quiet)
        printf("  ok %s == %d\n", what, got);
    return 0;
}
static int quiet_expect(const char *what, const char *val)
{
    int r;
    expect_quiet = 1;
    r = expect(what, val);
    expect_quiet = 0;
    return r;
}

static int script_line(const char *line, const char *dir, int lineno, int *fails)
{
    char cmd[32] = {0}, a[256] = {0}, b[256] = {0}, c[64] = {0}, out[600];
    if (line[0] == '#' || sscanf(line, "%31s %255s %255s %63s", cmd, a, b, c) < 1)
        return 0;
    if (!strcmp(cmd, "wait"))
        run_ms((uint32_t)atoi(a));
    else if (!strcmp(cmd, "bank")) {
        free(bank_a_mem);
        bank_a_mem = slurp(a, &bank_a_len);
        if (!bank_a_mem) {
            printf("line %d: cannot read %s\n", lineno, a);
            return 2;
        }
        boot();
        if (b[0] && upload_file(b, 0)) {
            printf("line %d: region B did not go in\n", lineno);
            return 2;
        }
    } else if (!strcmp(cmd, "upload")) {
        int r = upload_file(a, !strcmp(b, "again"));
        if (r) {
            printf("FAIL upload %s: %d\n", a, r);
            (*fails)++;
        }
    } else if (!strcmp(cmd, "press") || !strcmp(cmd, "release") || !strcmp(cmd, "tap") || !strcmp(cmd, "hold")) {
        int i = find_name(BTN_N, NB, a);
        if (i < 0) {
            printf("line %d: no button %s\n", lineno, a);
            return 2;
        }
        if (cmd[0] != 'r')
            held_btn |= 1u << i;
        if (cmd[0] == 't')
            run_ms(40);
        if (cmd[0] == 'h')
            run_ms((uint32_t)atoi(b));
        if (cmd[0] != 'p')
            held_btn &= ~(1u << i);
        run_ms(cmd[0] == 't' ? 40 : 2);
    } else if (!strcmp(cmd, "key") || !strcmp(cmd, "tapkey")) {
        int k = key_of(a);
        if (cmd[0] == 't') {
            held_keys |= 1u << k;
            run_ms(60);
            held_keys &= ~(1u << k);
            run_ms(30);
        } else {
            if (!strcmp(b, "down"))
                held_keys |= 1u << k;
            else
                held_keys &= ~(1u << k);
            run_ms(2);
        }
    } else if (!strcmp(cmd, "turn")) {
        int e = find_name(ENC_N, NE, a), n = atoi(b), s = n > 0 ? 1 : -1;
        uint32_t gap = c[0] ? (uint32_t)atoi(c) : 80u;
        if (e < 0) {
            printf("line %d: no encoder %s\n", lineno, a);
            return 2;
        }
        for (; n; n -= s) {
            enc_acc[e] += s;
            run_ms(gap);
        }
    } else if (!strcmp(cmd, "spin")) {
        int e = find_name(ENC_N, NE, a);
        if (e < 0) {
            printf("line %d: no encoder %s\n", lineno, a);
            return 2;
        }
        enc_acc[e] += atoi(b);
        accel_off = 1;
        run_ms(1);
        accel_off = 0;
    } else if (!strcmp(cmd, "midi")) {
        uint32_t s = (uint32_t)strtoul(a, 0, 16), d1 = (uint32_t)strtoul(b, 0, 16), d2 = (uint32_t)strtoul(c, 0, 16);
        min_q[mi_w++ % MQ] = (s >> 4) | s << 8 | d1 << 16 | d2 << 24;
    } else if (!strcmp(cmd, "sysex")) {
        uint8_t sx[128];
        uint32_t n = 0;
        const char *q = line + 5;
        char *e;
        for (;;) {
            unsigned long v = strtoul(q, &e, 16);
            if (e == q || n >= sizeof sx)
                break;
            sx[n++] = (uint8_t)v;
            q = e;
        }
        r1_sysex(sx, n);
        run_ms(5);
    } else if (!strcmp(cmd, "wav")) {
        snprintf(out, sizeof out, "%s/%s", dir, a);
        wav_open(out);
    } else if (!strcmp(cmd, "wavstop"))
        wav_close();
    else if (!strcmp(cmd, "peakreset"))
        peak_out = 0.0f;
    else if (!strcmp(cmd, "shot")) {
        snprintf(out, sizeof out, "%s/%s", dir, a);
        png_write(out);
    } else if (!strcmp(cmd, "leds")) {
        int i;
        printf("  leds: buttons");
        for (i = 0; i < NB; i++)
            if (lit_btn >> i & 1u)
                printf(" %s", BTN_N[i]);
        printf(", keys");
        for (i = 0; i < NKEYS; i++)
            if (lit_keys >> i & 1u)
                printf(" %d", i);
        printf(lit_glow ? ", the others glow\n" : "\n");
    } else if (!strcmp(cmd, "expect"))
        *fails += expect(a, b);
    else if (!strcmp(cmd, "until")) {
        uint32_t lim = c[0] ? (uint32_t)atoi(c) : 20000u, t;
        for (t = 0; t < lim && quiet_expect(a, b); t += 10)
            run_ms(10);
        *fails += expect(a, b);
    } else if (!strcmp(cmd, "reboot")) {
        memset(&set, 0, sizeof set);
        boot();
    } else if (!strcmp(cmd, "echo"))
        printf("%s", line + 5);
    else {
        printf("line %d: unknown command %s\n", lineno, cmd);
        return 2;
    }
    return 0;
}

#ifndef EN_WEB
int main(int argc, char **argv)
{
    FILE *sc;
    char line[512];
    const char *dir = argc > 2 ? argv[2] : ".";
    int lineno = 0, fails = 0, r;
    if (argc < 2) {
        fprintf(stderr, "usage: rom1_host SCRIPT [OUTDIR]\n");
        return 2;
    }
    sc = fopen(argv[1], "r");
    if (!sc) {
        perror(argv[1]);
        return 2;
    }
    bank_a_mem = slurp("build/gen/starter/bank_a.bin", &bank_a_len);   /* (the starter's, if built) */
    boot();
    while (fgets(line, sizeof line, sc)) {
        lineno++;
        if ((r = script_line(line, dir, lineno, &fails)) != 0)
            return r;
    }
    wav_close();
    printf("host: %u ms simulated, %u blits, the engine %.2f%% of real time (host CPU)%s\n", now_ms, blits,
           render_budget_ns_total > 0 ? 100.0 * render_ns_total / render_budget_ns_total : 0.0,
           fails ? ", EXPECTATIONS FAILED" : "");
    return fails ? 1 : 0;
}
#endif
