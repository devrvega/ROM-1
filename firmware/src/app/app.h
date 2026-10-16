/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1 app: what is saved (the settings: each part's preset and knobs, the effects, the keys') and the
 * app's entry points. Included by the unity build after gfx.c (drawing helpers in scope) and by the host
 * simulator. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "plat.h"
#include "../synth/rom1.h"

#ifndef EN_VERSION                       /* a release build passes its own (tools/build.py --release) */
#define EN_VERSION "DEV"
#endif
#define SET_MAGIC 0x31533152u            /* "R1S1" */
#define SET_FORMAT 1u

typedef struct {                         /* a part, as saved */
    uint8_t font, bank, prog;            /* its preset (found again by these in the bank) */
    uint8_t vol, pan, rev, cho;
    uint8_t cutoff, reso, attack, release;
    uint8_t rx, mute;
    int8_t coarse, fine;
    uint8_t rsv;
} set_part_t;

typedef struct {
    uint32_t magic, format;
    uint32_t bank_id;                    /* the bank they were for (another bank: the parts start afresh) */
    uint8_t sel;                         /* the part on screen (the keys play it) */
    uint8_t page;
    uint8_t kvel;                        /* the keys' velocity */
    int8_t oct;                          /* the keys' octave */
    uint8_t keys_out;                    /* the keys' notes out over USB too */
    uint8_t lights, glow;
    uint8_t rsv0;
    r1_fx_t fx;
    uint8_t rsv1[2];
    set_part_t part[R1_NPART];
    uint8_t rsv[32];
} settings_t;

extern settings_t set;

void settings_load(void);                /* from flash, into the engine (its parts, its effects) */
int settings_save(void);                 /* the engine's parts and effects now, and the UI's: 0 ok */
void settings_dirty(void);
int settings_is_dirty(void);
int settings_autosave(int quiet);        /* a few seconds after the last change, when quiet: 1 saved */

void ui_init(void);
void ui_frame(void);
void ui_input_only(void);
void ui_say(const char *a, const char *b);
void ui_bank_changed(void);              /* a new bank attached: the screen and the parts start over */
