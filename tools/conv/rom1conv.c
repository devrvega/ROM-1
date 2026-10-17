/* SPDX-License-Identifier: GPL-3.0-only */
/* rom1conv: .sf2 files to a ROM-1 bank, from the command line (the browser's page does the same with the
 * same converter, conv.c).
 *
 *   rom1conv [-a ROOM_A] [-b ROOM_B] [-n NAME] [-o DIR] [-l] FONT.sf2[@BANK:PROG,BANK:PROG...] ...
 *     -a, -b   the room in the firmware (region A) and in the data area (region B), bytes (K and M work)
 *     -o       where bank_a.bin, bank_b.bin and report.json go (default .)
 *     -l       list the fonts' presets (bank:program name, full-quality KB) and stop
 *     @...     only these presets of that font (bank 128: drum kits)
 * Exit 0 written, 1 too big (it says by how much at the lowest quality), 2 a bad file or usage. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "conv.h"

static uint32_t size_arg(const char *s)
{
    char *e;
    double v = strtod(s, &e);
    if (*e == 'k' || *e == 'K')
        v *= 1024.0;
    else if (*e == 'm' || *e == 'M')
        v *= 1048576.0;
    return (uint32_t)v;
}

static uint8_t *slurp(const char *path, uint32_t *n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *b;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)(sz > 0 ? sz : 1));
    if (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = 0;
    }
    fclose(f);
    *n = (uint32_t)sz;
    return b;
}

int main(int argc, char **argv)
{
    uint32_t cap_a = 420u * 1024u, cap_b = 0x45000u, id;
    const char *out = ".", *name = "ROM-1 bank";
    int list = 0, i, nfiles = 0, r;
    uint8_t *files[64];
    uint32_t smpl_at[64];
    char path[1024];
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a") && i + 1 < argc)
            cap_a = size_arg(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc)
            cap_b = size_arg(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            name = argv[++i];
        else if (!strcmp(argv[i], "-l"))
            list = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: rom1conv [-a ROOM_A] [-b ROOM_B] [-n NAME] [-o DIR] [-l] FONT.sf2[@BANK:PROG,...] ...\n");
            return 2;
        } else {
            char fp[1024], *at, *base;
            uint32_t n, pd, pdn, sm, smn;
            int f, p;
            snprintf(fp, sizeof fp, "%s", argv[i]);
            at = strrchr(fp, '@');
            if (at && strchr(at, ':'))
                *at++ = 0;
            else
                at = 0;
            if (nfiles >= 64 || !(files[nfiles] = slurp(fp, &n))) {
                fprintf(stderr, "rom1conv: cannot read %s\n", fp);
                return 2;
            }
            if (cv_riff(files[nfiles], n, &pd, &pdn, &sm, &smn)) {
                fprintf(stderr, "rom1conv: %s: not an .sf2 file\n", fp);
                return 2;
            }
            base = strrchr(fp, '/');
            base = base ? base + 1 : fp;
            {
                char nm[64];
                char *dot;
                snprintf(nm, sizeof nm, "%s", base);
                dot = strrchr(nm, '.');
                if (dot)
                    *dot = 0;
                f = cv_font_add(files[nfiles] + pd, pdn, smn, nm);
            }
            if (f < 0) {
                fprintf(stderr, "rom1conv: %s: %s\n", fp, cv_error());
                return 2;
            }
            smpl_at[f] = sm;
            nfiles++;
            if (at) {                                 /* only the presets named */
                for (p = 0; p < cv_presets(f); p++)
                    cv_select(f, p, 0);
                while (*at) {
                    int b = atoi(at), pg;
                    char *c = strchr(at, ':');
                    if (!c)
                        break;
                    pg = atoi(c + 1);
                    for (p = 0; p < cv_presets(f); p++)
                        if (cv_preset_bank(f, p) == b && cv_preset_prog(f, p) == pg)
                            cv_select(f, p, 1);
                    at = strchr(c, ',');
                    if (!at)
                        break;
                    at++;
                }
            }
        }
    }
    if (!nfiles) {
        fprintf(stderr, "usage: rom1conv [-a ROOM_A] [-b ROOM_B] [-n NAME] [-o DIR] [-l] FONT.sf2[@BANK:PROG,...] ...\n");
        return 2;
    }
    if (list) {
        int f, p;
        for (f = 0; f < cv_fonts(); f++) {
            printf("%s: %d presets\n", cv_font_name(f), cv_presets(f));
            for (p = 0; p < cv_presets(f); p++)
                printf("  %3d:%-3d %-20s %6u KB\n", cv_preset_bank(f, p), cv_preset_prog(f, p), cv_preset_name(f, p),
                       (unsigned)(cv_preset_bytes(f, p) + 1023u) / 1024u);
        }
        return 0;
    }
    r = cv_plan(cap_a, cap_b);
    if (r < 0) {
        fprintf(stderr, "rom1conv: %s\n", cv_error());
        return r == -2 ? 1 : 2;
    }
    for (i = 0; i < cv_needs(); i++) {
        uint32_t f, off, bytes;
        cv_need(i, &f, &off, &bytes);
        if (cv_give(i, files[f] + smpl_at[f] + off, bytes)) {
            fprintf(stderr, "rom1conv: %s\n", cv_error());
            return 2;
        }
    }
    id = cv_bank_id(name);
    if (cv_build(id, name)) {
        fprintf(stderr, "rom1conv: %s\n", cv_error());
        return 1;
    }
    {
        uint32_t la, lb;
        const uint8_t *a = cv_out_a(&la), *b = cv_out_b(&lb);
        FILE *f;
        snprintf(path, sizeof path, "%s/bank_a.bin", out);
        if (!(f = fopen(path, "wb")) || fwrite(a, 1, la, f) != la) {
            fprintf(stderr, "rom1conv: cannot write %s\n", path);
            return 2;
        }
        fclose(f);
        snprintf(path, sizeof path, "%s/bank_b.bin", out);
        if (!(f = fopen(path, "wb")) || (lb && fwrite(b, 1, lb, f) != lb)) {
            fprintf(stderr, "rom1conv: cannot write %s\n", path);
            return 2;
        }
        fclose(f);
        snprintf(path, sizeof path, "%s/report.json", out);
        if ((f = fopen(path, "w"))) {
            fputs(cv_report(), f);
            fputc('\n', f);
            fclose(f);
        }
        printf("bank: region A %u B of %u, region B %u B of %u\n%s\n", (unsigned)la, (unsigned)cap_a, (unsigned)lb,
               (unsigned)cap_b, cv_report());
    }
    for (i = 0; i < nfiles; i++)                      /* (all given back: the sanitizers' leak check) */
        free(files[i]);
    cv_reset();
    return 0;
}
