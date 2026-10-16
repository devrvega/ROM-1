/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1's engine on its own: a bank (rom1conv's bank_a.bin, bank_b.bin) played preset by preset, a note
 * held then let go, and a chord; every output sample finite and in range, every voice free again after,
 * the time a render takes. With a third argument, the whole thing as a WAV to listen to.
 *   rom1_test BANK_A BANK_B [OUT.wav] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rom1.h"

void r1_lock(int on) { (void)on; }
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
static FILE *wav;
static uint32_t frames;
static double peak, ns_total, ns_worst;
static int bad;
static void run(uint32_t ms)
{
    static int32_t buf[2 * 128];
    uint32_t blocks = ms * 44100u / 1000u / 128u, b, i;
    for (b = 0; b < blocks; b++) {
        struct timespec t0, t1;
        double ns;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        r1_render(buf, 128, 4096);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        ns_total += ns;
        if (ns > ns_worst)
            ns_worst = ns;
        for (i = 0; i < 256; i++) {
            double x = buf[i] / 8388608.0;
            if (x != x || x > 1.0 || x < -1.0)
                bad++;
            if (fabs(x) > peak)
                peak = fabs(x);
            if (wav) {
                int16_t s = (int16_t)(x * 32767.0);
                fwrite(&s, 2, 1, wav);
            }
        }
        frames += 128;
    }
}
/* a held note's frequency, from its upward zero crossings (the left channel, after its attack) */
static double measure(int preset, int key)
{
    static int32_t buf[2 * 128];
    double t0 = -1, t1 = -1, prev = 0;
    int n = 0, b, i;
    r1_panic();
    r1_program(0, preset);
    r1_cc(0, 10, 0);                                  /* (hard left: the right channel's pan law aside) */
    r1_note(0, key, 100);
    for (b = 0; b < 44100 / 128; b++) {
        r1_render(buf, 128, 4096);
        for (i = 0; i < 128; i++) {
            double x = buf[2 * i], t = b * 128 + i;
            if (b > 100 && prev < 0 && x >= 0) {
                double tc = t - 1 + (-prev) / (x - prev);
                if (t0 < 0)
                    t0 = tc;
                t1 = tc;
                n++;
            }
            prev = x;
        }
    }
    r1_note(0, key, 0);
    r1_cc(0, 10, 64);
    return n > 1 ? (n - 1) * 44100.0 / (t1 - t0) : 0.0;
}

int main(int argc, char **argv)
{
    uint32_t na, nb;
    uint8_t *a, *b;
    int i, fails = 0, quiet = 0;
    if (argc < 3) {
        fprintf(stderr, "usage: rom1_test BANK_A BANK_B [OUT.wav]\n");
        return 2;
    }
    a = slurp(argv[1], &na);
    b = slurp(argv[2], &nb);
    if (!a || r1_bank_attach(a, nb ? b : 0)) {
        printf("FAIL the bank does not attach\n");
        return 1;
    }
    r1_init();
    if (argc > 3 && !strcmp(argv[3], "--freq")) {  /* --freq PRESET KEY [EXPECT_HZ]: within 0.1 % */
        double f = measure(atoi(argv[4]), atoi(argv[5])), want = argc > 6 ? atof(argv[6]) : 0;
        int ok = !want || fabs(f / want - 1.0) < 0.001;
        printf("%s preset %s key %s: %.3f Hz%s%s\n", ok ? "  ok" : "FAIL", argv[4], argv[5], f, want ? ", want " : "",
               want ? argv[6] : "");
        return ok ? 0 : 1;
    }
    if (argc > 3 && (wav = fopen(argv[3], "wb")))
        fseek(wav, 44, SEEK_SET);
    printf("bank: %d fonts, %u presets, %u zones, %u samples; region B %s\n", r1_fonts(), r1_bank()->npreset,
           r1_bank()->nzone, r1_bank()->nsample, r1_bank_ok_b() ? "ok" : (r1_bank()->lenb ? "MISSING" : "none"));
    for (i = 0; i < (int)r1_bank()->npreset; i++) {
        const r1_preset_t *p = r1_preset_at(i);
        int key = p->bank == 128 ? 38 : 60;
        double pk;
        r1_program(0, i);
        peak = 0;
        r1_note(0, key, 100);
        run(400);
        r1_note(0, key, 0);
        run(300);
        pk = peak;
        if (pk < 1e-4)
            quiet++;
        if (i < 4 || pk < 1e-4)
            printf("  %3d:%-3d %-20s peak %.3f%s\n", p->bank, p->prog, p->name, pk, pk < 1e-4 ? "  (silent)" : "");
    }
    /* a chord held, sustain, every voice in use, then all let go */
    r1_program(0, 0);
    for (i = 0; i < 40; i++)
        r1_note(0, 36 + i, 90);
    run(1500);
    for (i = 0; i < 40; i++)
        r1_note(0, 36 + i, 0);
    run(4000);
    if (r1_voices) {
        printf("FAIL %u voices still sounding after the release\n", r1_voices);
        fails++;
    }
    if (bad) {
        printf("FAIL %d samples out of range or not numbers\n", bad);
        fails++;
    }
    printf("%d presets (%d silent); render: %.1f us a 128-frame block on average, %.1f us at worst (%.1f%% of real time)\n",
           (int)r1_bank()->npreset, quiet, ns_total / (frames / 128.0) / 1000.0, ns_worst / 1000.0,
           100.0 * ns_total / 1e9 / (frames / 44100.0));
    if (wav) {
        uint32_t bytes = frames * 4, v;
        uint16_t fmt[2] = {1, 2}, al[2] = {4, 16};
        uint32_t r[2] = {44100, 44100 * 4};
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav);
        v = 36 + bytes;
        fwrite(&v, 4, 1, wav);
        fwrite("WAVEfmt ", 1, 8, wav);
        v = 16;
        fwrite(&v, 4, 1, wav);
        fwrite(fmt, 2, 2, wav);
        fwrite(r, 4, 2, wav);
        fwrite(al, 2, 2, wav);
        fwrite("data", 1, 4, wav);
        fwrite(&bytes, 4, 1, wav);
        fclose(wav);
    }
    return fails ? 1 : 0;
}
