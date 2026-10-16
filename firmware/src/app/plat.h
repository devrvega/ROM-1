/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1 platform interface (from X0X's, by way of AMB-1, IND-1, LCD-4, CRT-4 and SEQ-4): everything the app
 * needs from the hardware.
 *
 * The device implementation (plat_fm1.c) wraps Felucca's HAL and panel map; the host implementation
 * (host/rom1_host.c) is a simulator fed by a script, which is what lets the whole app (UI, engine, settings,
 * the bank's upload) run and be tested on a Mac. Drawing goes through Felucca's lcd_fill / lcd_blit (lcd.c
 * on the device, a framebuffer on the host) and gfx.c on top. */
#pragma once
#include <stdint.h>

/* big zero-initialised buffers go in the .pool RAM region on the device */
#ifdef EN_HOST
#define EN_POOL
#else
#define EN_POOL __attribute__((section(".pool")))
#endif

/* panel controls, by printed label (Felucca's panel.c map turns these into matrix ids; Felucca
 * calls the second button SCL, the panel prints SEL) */
enum { B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC,
       B_OCTDN, B_OCTUP, NB };
enum { EN_SELECT, EN_ALGO, EN_PRESET, EN_K1, EN_K2, EN_K3, EN_K4, NE };

#define NKEYS 27                       /* F3 .. G5 */

uint32_t plat_ms(void);
uint32_t plat_buttons(void);           /* held: bit B_* */
uint32_t plat_keys(void);              /* held: bit k = key k (0 = F3) */
int32_t plat_enc(int role);            /* detents since the last call, + = clockwise; EN_* */
uint32_t plat_master(void);            /* MASTER pot, 0..4096 gain Q12 */
void plat_leds(uint32_t buttons, uint32_t keys);   /* lit: bit B_*, bit key */
void plat_glow(int on);                /* the unlit buttons glow dimly, readable in the dark (call before plat_leds) */
void plat_play_red(int red);           /* PLAY lights green; red (its other LED) instead (before plat_leds) */

/* USB MIDI: 4-byte USB-MIDI event packets; cable 0 is USB, 14 the TRS jack's (coming in) */
int plat_midi_in(uint32_t *pkt);       /* 1 = got one (called from the audio interrupt) */
int plat_midi_out(uint32_t pkt);       /* queue one (the main loop only): 1 queued, 0 not (no host, or full) */
int plat_usb_up(void);                 /* a host is there */

/* persistent storage: Felucca's objects (A/B sectors, CRC, torn-write safe) for the settings and the panel's
 * calibration */
int plat_store_load(uint32_t obj, void *dst, uint32_t max);   /* bytes loaded, < 0 = none / bad */
int plat_store_save(uint32_t obj, const void *src, uint32_t len);   /* 0 = ok */
#define PLAT_STORE_MAX 3840u           /* bytes per object */
enum { OBJ_GLOB, OBJ_PANEL, OBJ_NOBJ };

/* the bank (synth/r1bank.h): region A in the app image, region B in the data area, both where the engine
 * reads them; region B is written in 4 KiB sectors (erased, then programmed in 256-byte pages) */
const uint8_t *plat_bank_a(void);      /* 0: no bank in the firmware */
const uint8_t *plat_bank_b(void);
int plat_bank_b_erase(uint32_t off);   /* off from region B's start, a multiple of 4096: 0 ok */
int plat_bank_b_prog(uint32_t off, const void *src, uint32_t n);   /* n <= 256, within a page: 0 ok */

/* the audio interrupt: its load in %, renders late */
uint32_t plat_cpu_pct(void);
uint32_t plat_xruns(void);
