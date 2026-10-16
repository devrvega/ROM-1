/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1's settings: each part's preset and knobs, the effects, the keys', kept in Felucca's settings object
 * (OBJ_GLOB). Saved a few seconds after the UI changes something, when nothing sounds (writing pauses the
 * audio for a moment), or at once with SAVE. A part's preset is kept by its font, bank and program, found
 * again in the bank; with another bank (another id) the parts start afresh. Part of the unity build and of
 * the host simulator. */

settings_t set;
static uint8_t set_dirty;
static uint32_t set_changed;

void settings_dirty(void)
{
    set_dirty = 1;
    set_changed = plat_ms();
}
int settings_is_dirty(void) { return set_dirty; }

static void ui_defaults(void)
{
    set.sel = 0;
    set.page = 0;
    set.kvel = 100;
    set.oct = 0;
    set.keys_out = 0;
    set.lights = 1;
    set.glow = 1;
}

void settings_load(void)
{
    int ok = plat_store_load(OBJ_GLOB, &set, sizeof set) == (int)sizeof set && set.magic == SET_MAGIC &&
             set.format == SET_FORMAT;
    const r1_bank_t *b = r1_bank();
    int i;
    r1_init();                                   /* the parts as after a reset, the effects' defaults */
    if (!ok) {
        memset(&set, 0, sizeof set);
        ui_defaults();
        set.fx = r1_fx;
        set.bank_id = b ? b->id : 0;
        return;
    }
    if (set.sel >= R1_NPART)
        set.sel = 0;
    if (set.kvel < 1 || set.kvel > 127)
        set.kvel = 100;
    if (set.oct < -3 || set.oct > 3)
        set.oct = 0;
    r1_fx = set.fx;
    if (!b || set.bank_id != b->id) {            /* another bank: the parts as they start */
        set.bank_id = b ? b->id : 0;
        return;
    }
    for (i = 0; i < R1_NPART; i++) {
        const set_part_t *s = &set.part[i];
        r1_part_t *p = &r1_part[i];
        int k = r1_find(s->font, i == R1_DRUMS ? 128 : s->bank, s->prog);
        if (k >= 0) {
            const r1_preset_t *pr = r1_preset_at(k);
            p->preset = (uint16_t)k;
            p->font = p->font_pend = pr->font;
            p->bank = (uint8_t)(pr->bank & 127u);
            p->prog = pr->prog;
        }
        p->vol = s->vol & 127u;
        p->pan = s->pan & 127u;
        p->rev = s->rev & 127u;
        p->cho = s->cho & 127u;
        p->cutoff = s->cutoff & 127u;
        p->reso = s->reso & 127u;
        p->attack = s->attack & 127u;
        p->release = s->release & 127u;
        p->rx = (uint8_t)(s->rx != 0);
        p->mute = (uint8_t)(s->mute != 0);
        p->coarse = (int8_t)(s->coarse < -24 ? -24 : s->coarse > 24 ? 24 : s->coarse);
        p->fine = s->fine;
    }
}

int settings_save(void)
{
    const r1_bank_t *b = r1_bank();
    int i, rc;
    set.magic = SET_MAGIC;
    set.format = SET_FORMAT;
    set.bank_id = b ? b->id : 0;
    set.fx = r1_fx;
    for (i = 0; i < R1_NPART; i++) {
        const r1_part_t *p = &r1_part[i];
        set_part_t *s = &set.part[i];
        const r1_preset_t *pr = p->preset != 0xFFFF ? r1_preset_at(p->preset) : 0;
        s->font = pr ? pr->font : 0;
        s->bank = pr ? (uint8_t)(pr->bank & 127u) : 0;
        s->prog = pr ? pr->prog : 0;
        s->vol = p->vol;
        s->pan = p->pan;
        s->rev = p->rev;
        s->cho = p->cho;
        s->cutoff = p->cutoff;
        s->reso = p->reso;
        s->attack = p->attack;
        s->release = p->release;
        s->rx = p->rx;
        s->mute = p->mute;
        s->coarse = p->coarse;
        s->fine = p->fine;
    }
    rc = plat_store_save(OBJ_GLOB, &set, sizeof set);
    if (!rc)
        set_dirty = 0;
    return rc;
}

int settings_autosave(int quiet)
{
    if (!set_dirty || !quiet || plat_ms() - set_changed < 4000u)
        return 0;
    settings_save();
    return 1;
}
