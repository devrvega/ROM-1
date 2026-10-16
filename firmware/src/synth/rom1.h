/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1's engine: a sound module for the bank (r1bank.h). Sixteen parts, one a MIDI channel, each playing a
 * preset; 32 voices; reverb and chorus. It renders in the audio interrupt (r1_render), where it also
 * takes the MIDI coming in (r1_midi); the UI asks for notes and changes through a queue (r1_note, r1_cc,
 * r1_program ...) and reads the parts and the meters directly. Host-testable: no hardware here. */
#pragma once
#include <stdint.h>
#include "r1bank.h"

#define R1_NPART 16
#define R1_NVOICE 32
#define R1_FS 44100.0f
#define R1_DRUMS 9                     /* the part on MIDI channel 10: drum kits (bank 128) */

typedef struct {                       /* a part: MIDI's controllers, and the UI's own */
    uint8_t font, bank, prog;          /* bank: the font's bank (CC 32); the drum part plays bank 128 */
    uint8_t vol, expr, pan;            /* CC 7, 11, 10 */
    uint8_t rev, cho;                  /* CC 91, 93 */
    uint8_t cutoff, reso, attack, release;   /* CC 74, 71, 73, 72: 64 changes nothing */
    uint8_t mod, sustain, pressure;    /* CC 1, 64; channel pressure */
    uint8_t pbrange;                   /* pitch bend range, semitones (RPN 0) */
    int16_t bend;                      /* -8192 .. 8191 */
    int8_t coarse, fine;               /* RPN 2 (semitones), RPN 1 (cents) */
    uint8_t drums;                     /* plays drum kits */
    uint8_t rx;                        /* takes MIDI */
    uint8_t mute, solo;
    uint8_t font_pend, rpn_lsb, rpn_msb;   /* (MIDI's bank select waits for a program change) */
    uint16_t preset;                   /* the bank's preset playing, 0xFFFF none */
} r1_part_t;

typedef struct {                       /* the effects */
    uint8_t rev_type;                  /* 0 room, 1 hall, 2 large hall, 3 plate, 4 off */
    uint8_t rev_time;                  /* 0..127 */
    uint8_t rev_level;                 /* the return, 0..127 */
    uint8_t cho_level, cho_rate, cho_depth;
} r1_fx_t;

extern r1_part_t r1_part[R1_NPART];
extern r1_fx_t r1_fx;
extern volatile float r1_meter[R1_NPART];   /* each part's level now, 0..1 (the screen's bars) */
extern volatile uint8_t r1_voices;          /* voices sounding */
extern volatile uint32_t r1_notes;          /* notes started since boot */
extern volatile uint16_t r1_activity;       /* parts that took a note lately (a bit each, the UI clears it) */

void r1_init(void);
/* the bank: region A and B where the engine can read them (B may be 0); 0 ok, < 0 region A is bad; region B
 * is checked too and left out (its samples silent) if it is not the one region A expects */
int r1_bank_attach(const uint8_t *a, const uint8_t *b);
int r1_bank_ok_b(void);                     /* region B there and right */
const r1_bank_t *r1_bank(void);
int r1_fonts(void);
const r1_font_t *r1_font_at(int f);
const r1_preset_t *r1_preset_at(int i);      /* the bank's i-th (all fonts, sorted) */
int r1_find(int font, int bank, int prog);   /* a preset's index, or the nearest there is; -1 none */

void r1_render(int32_t *out, uint32_t n, uint32_t gain_q12);   /* n frames, stereo, 24-bit in 32 */
void r1_load(uint32_t pct);                 /* the last render's time, % of its own: sets the cap */
uint32_t r1_voice_cap(void);                /* the voices allowed now */
void r1_midi(uint32_t st, uint32_t d1, uint32_t d2);           /* (from the audio interrupt) */

/* the UI's requests (queued, done at the next render) */
void r1_note(int part, int key, int vel);    /* vel 0: off */
void r1_cc(int part, int cc, int val);
void r1_program(int part, int preset);       /* a preset by its index in the bank */
void r1_panic(void);
void r1_reset(void);                         /* the parts as after a GM reset */
int r1_sysex(const uint8_t *p, uint32_t n);  /* a SysEx (the bytes between F0 and F7): GM's, GS's reset */
void r1_lock(int on);                        /* the app's: the audio interrupt held off */
