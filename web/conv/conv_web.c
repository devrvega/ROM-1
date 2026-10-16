/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1's converter (tools/conv/conv.c) for the browser: the same code, compiled to WebAssembly, its functions
 * exported as they are, and a few helpers for the page's side (web/rom1conv.js): memory to hand data over in,
 * and the out-parameters as an array.
 *   web/conv/build.sh  ->  build/web/rom1conv.wasm */
#include <stdlib.h>
#include "../../tools/conv/conv.h"

#define WEB __attribute__((used, visibility("default")))

WEB void *web_alloc(uint32_t n) { return malloc(n ? n : 1); }
WEB void web_free(void *p) { free(p); }

static uint32_t out3[3];
WEB uint32_t *web_need(int i)                            /* the font, the offset in its smpl chunk, the bytes */
{
    cv_need(i, &out3[0], &out3[1], &out3[2]);
    return out3;
}
WEB const uint8_t *web_out_a(void) { return cv_out_a(&out3[0]); }
WEB const uint8_t *web_out_b(void) { return cv_out_b(&out3[1]); }
WEB uint32_t web_out_len(int b) { return out3[b ? 1 : 0]; }

WEB void web_reset(void) { cv_reset(); }
WEB int web_font_add(const uint8_t *pdta, uint32_t n, uint32_t smpl_bytes, const char *name) { return cv_font_add(pdta, n, smpl_bytes, name); }
WEB int web_fonts(void) { return cv_fonts(); }
WEB const char *web_font_name(int f) { return cv_font_name(f); }
WEB int web_presets(int f) { return cv_presets(f); }
WEB const char *web_preset_name(int f, int p) { return cv_preset_name(f, p); }
WEB int web_preset_bank(int f, int p) { return cv_preset_bank(f, p); }
WEB int web_preset_prog(int f, int p) { return cv_preset_prog(f, p); }
WEB uint32_t web_preset_bytes(int f, int p) { return cv_preset_bytes(f, p); }
WEB void web_select(int f, int p, int on) { cv_select(f, p, on); }
WEB int web_selected(int f, int p) { return cv_selected(f, p); }
WEB int web_plan(uint32_t cap_a, uint32_t cap_b) { return cv_plan(cap_a, cap_b); }
WEB uint32_t web_over(void) { return cv_over(); }
WEB uint32_t web_planned(void) { return cv_planned(); }
WEB int web_plan_level(int lvl) { return cv_plan_level(lvl, &out3[2]); }   /* (the bytes: web_need's third) */
WEB uint32_t web_plan_level_bytes(void) { return out3[2]; }
WEB int web_needs(void) { return cv_needs(); }
WEB int web_give(int i, const uint8_t *pcm16, uint32_t bytes) { return cv_give(i, pcm16, bytes); }
WEB uint32_t web_bank_id(const char *name) { return cv_bank_id(name); }
WEB int web_build(uint32_t id, const char *name) { return cv_build(id, name); }
WEB const char *web_report(void) { return cv_report(); }
WEB const char *web_error(void) { return cv_error(); }
