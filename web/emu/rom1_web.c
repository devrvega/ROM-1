/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1 in the browser: the host simulator (host/rom1_host.c: the whole app, UI, engine and settings, against
 * a simulated FM-1) compiled to WebAssembly and run in an AudioWorklet (from SEQ-4's, by way of CRT-4's and
 * X0X's). The worklet asks for audio; producing it advances the device's clock, which runs the engine a
 * 128-frame block at a time (as on the FM-1) and the UI every 16 ms. The bank comes from the page (the
 * starter sounds, or yours: region A as the firmware would hold it, region B written to the simulated
 * flash as the upload page writes it); the settings' part of the flash is kept by the page.
 *
 *   web/emu/build.sh   ->  build/emu/rom1.wasm (+ the page) */
#include <stdint.h>

#include "../../host/rom1_host.c"

#undef __attribute__
#define WEB __attribute__((used, visibility("default")))

/* output ring, stereo float, filled by the blocks, drained by web_render */
#define RING 4096u
static float ring[RING * 2];
static uint32_t ring_w, ring_r;
void web_audio(const int32_t *blk, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++) {
        uint32_t i = (ring_w++ % RING) * 2u;
        ring[i] = (float)blk[2 * k] / 8388608.0f;
        ring[i + 1] = (float)blk[2 * k + 1] / 8388608.0f;
    }
}

/* MIDI out (the keys, with KEYS OUT on): USB-MIDI packets, with the device's time (ms) */
#define MRING 512u
static uint32_t mring[MRING][2];
static uint32_t mr_w, mr_r;
void web_midi_out(uint32_t pkt)
{
    if (mr_w - mr_r >= MRING)
        return;
    mring[mr_w % MRING][0] = now_ms;
    mring[mr_w % MRING][1] = pkt;
    mr_w++;
}
static uint32_t mout[2 * 64];
WEB uint32_t *web_midi_buf(void) { return mout; }
WEB uint32_t web_midi_take(void)                  /* up to 64 (time, packet) pairs into web_midi_buf: how many */
{
    uint32_t n = 0;
    while (mr_r != mr_w && n < 64u) {
        mout[2 * n] = mring[mr_r % MRING][0];
        mout[2 * n + 1] = mring[mr_r % MRING][1];
        mr_r++;
        n++;
    }
    return n;
}
/* a message coming in (a USB-MIDI packet) */
WEB void web_midi_in(uint32_t pkt) { min_q[mi_w++ % MQ] = pkt; }

static float out_l[1024], out_r[1024];
WEB float *web_out_l(void) { return out_l; }
WEB float *web_out_r(void) { return out_r; }

/* n frames (<= 1024) into out_l / out_r: the device runs until they exist */
WEB void web_render(uint32_t n)
{
    uint32_t k;
    if (n > 1024u)
        n = 1024u;
    while (ring_w - ring_r < n)
        run_ms(1);
    for (k = 0; k < n; k++) {
        uint32_t i = (ring_r++ % RING) * 2u;
        out_l[k] = ring[i];
        out_r[k] = ring[i + 1];
    }
}

WEB void *web_alloc(uint32_t n) { return malloc(n ? n : 1); }
/* the bank: region A (taken: the page allocated it with web_alloc), region B (copied into the flash, as the
 * upload writes it; none: n_b 0), then the device started again */
WEB int web_bank(uint8_t *a, uint32_t n_a, const uint8_t *b, uint32_t n_b)
{
    nor_init();
    free(bank_a_mem);
    bank_a_mem = a;
    bank_a_len = n_a;
    memset(nor + R1_B_BASE, 0xFF, R1_B_ROOM);
    if (n_b && n_b <= R1_B_ROOM)
        memcpy(nor + R1_B_BASE, b, n_b);
    boot();
    return r1_bank() ? (int)r1_bank()->npreset : -1;
}
WEB int web_bank_ok_b(void) { return r1_bank_ok_b(); }
WEB void web_buttons(uint32_t m) { held_btn = m; }
WEB void web_keys(uint32_t m) { held_keys = m; }
WEB void web_enc(int role, int32_t n)
{
    if (role >= 0 && role < NE)
        enc_acc[role] += n;
}
WEB void web_master(uint32_t v) { master = v > 4096u ? 4096u : v; }
WEB uint32_t web_lit_buttons(void) { return lit_btn; }
WEB uint32_t web_lit_keys(void) { return lit_keys; }
WEB uint16_t *web_fb(void) { return fb; }
WEB uint32_t web_blits(void) { return blits; }
WEB uint32_t web_now(void) { return now_ms; }
WEB uint32_t web_voices(void) { return r1_voices; }

/* the flash the page keeps: the panel's and the settings' objects (0xDC000 on; region B is the bank's) */
#define KEEP_FROM 0xDC000u
WEB uint8_t *web_flash(void) { return nor + KEEP_FROM; }
WEB uint32_t web_flash_len(void) { return (uint32_t)sizeof nor - KEEP_FROM; }
WEB void web_flash_erased(void)                   /* before the page's bytes go in */
{
    memset(nor, 0xFF, sizeof nor);
    nor_ready = 1;
}
WEB uint32_t web_flash_writes(void) { return store_writes; }
