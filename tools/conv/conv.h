/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1's converter: .sf2 files in, a bank out (firmware/src/synth/r1bank.h). The same code runs in the
 * command-line tool (rom1conv.c), the tests and the browser (web/load, compiled to WebAssembly).
 *
 * It never needs a whole file at once: give it each font's preset tables (the "pdta" list), choose
 * presets, plan (it works out which samples are needed, at what quality, to fit the room), then hand it
 * just those samples' data, then build.
 *   cv_font_add      a font's pdta list: its presets and samples
 *   cv_select        a preset in or out (all are in to begin with)
 *   cv_plan          the quality that fits: 0 ok, < 0 too big (cv_over: by how much at the lowest quality)
 *   cv_need / cv_give    the spans of 16-bit sample data the plan uses: their place in the font's "smpl"
 *                    chunk, and their data
 *   cv_build         regions A and B (cv_out_a, cv_out_b), and a report (cv_report: JSON) */
#pragma once
#include <stdint.h>

void cv_reset(void);
int cv_font_add(const uint8_t *pdta, uint32_t n, uint32_t smpl_bytes, const char *name);   /* its index, < 0 bad */
int cv_fonts(void);
const char *cv_font_name(int f);
int cv_presets(int f);
const char *cv_preset_name(int f, int p);
int cv_preset_bank(int f, int p);
int cv_preset_prog(int f, int p);
uint32_t cv_preset_bytes(int f, int p);                 /* its samples, 16-bit at their own rate */
void cv_select(int f, int p, int on);
int cv_selected(int f, int p);

int cv_plan(uint32_t cap_a, uint32_t cap_b);            /* the room in each region */
uint32_t cv_over(void);
int cv_plan_level(int lvl, uint32_t *bytes);           /* the plan's samples at a quality level (0 16-bit at the
                                                           sample's rate .. 6 4-bit at 8 kHz), and their bytes */
uint32_t cv_planned(void);                              /* bytes the plan uses (descriptions and samples) */
int cv_needs(void);
void cv_need(int i, uint32_t *font, uint32_t *off, uint32_t *bytes);   /* off, bytes: in the smpl chunk */
int cv_give(int i, const uint8_t *pcm16, uint32_t bytes);
int cv_build(uint32_t id, const char *name);            /* 0 ok */
uint32_t cv_bank_id(const char *name);                  /* the id cv_build is given: the name's and the plan's */
const uint8_t *cv_out_a(uint32_t *len);
const uint8_t *cv_out_b(uint32_t *len);
const char *cv_report(void);
const char *cv_error(void);

/* a whole .sf2 in memory: where its pdta list and smpl chunk are (the tool's and the tests' way in) */
int cv_riff(const uint8_t *f, uint32_t n, uint32_t *pdta, uint32_t *pdta_n, uint32_t *smpl, uint32_t *smpl_n);
