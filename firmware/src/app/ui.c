/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1 UI: the panel and the screen. Part of the unity build (after gfx.c and settings.c); the host
 * simulator includes it too.
 *
 *   keys        play the part on screen (F3 up, OCT- / OCT+ the octave)
 *   ALGORITHM   the part, 1-16 (part n listens on MIDI channel n)
 *   PRESETS     its preset (GLO held: the next font's)
 *   SELECT      the page: PART, SOUND, EFFECTS, SETUP, BANK
 *   KNOBS 1-4   the page's four values
 *   HOME        the PART page        LFO, ENV  the SOUND page      FX   EFFECTS      SEQ  BANK
 *   GLO         held: the SETUP page; with another: its second job
 *   PLAY        a phrase on the part (again: stop)      ARP   hold (the sustain pedal) for the part
 *   SEL         the part muted (GLO: soloed)            EDIT  the part's knobs back (GLO: everything, a GM reset)
 *   REC         all notes off                           SAVE  the settings saved now
 * The screen is a backlit module LCD: the sixteen parts' meters, the preset's name in big dots, the page's
 * knobs. */
#include "dotfont.h"
#include "../synth/fastmath.h"

enum { PG_PART, PG_SOUND, PG_FX, PG_SETUP, PG_BANK, NPAGE };
static const char *const PAGE_NAME[NPAGE] = {"PART", "SOUND", "EFFECTS", "SETUP", "BANK"};
#define HELP_MS 900u
#define SETUP_MS 700u
#define AUD_MS 160u

static struct {
    uint32_t btn, keys, btn_used;
    uint32_t down_t[NB];
    uint32_t enc_t[NE];
    uint8_t enc_run[NE];
    uint32_t turn_t, act_t;
    uint8_t help;
    char msg[2][24];
    uint32_t msg_until;
    uint32_t sig[3];
    uint8_t key_note[NKEYS], key_part[NKEYS];
    uint8_t aud, aud_i, aud_part;
    int16_t aud_note;
    uint32_t aud_t;
    uint8_t hold;
    uint8_t touched;
    uint32_t touch_t;
    uint32_t frame;
} ui;

static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}
static void scopy(char *d, const char *s, int n)
{
    int i;
    for (i = 0; i < n - 1 && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}
static char *itoa_u(uint32_t v, char *b)
{
    char t[12];
    int n = 0;
    char *r = b;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        *b++ = t[--n];
    *b = 0;
    return r;
}
static char *itoa_s(int v, char *b)
{
    char *r = b;
    if (v > 0)
        *b++ = '+';
    else if (v < 0) {
        *b++ = '-';
        v = -v;
    }
    itoa_u((uint32_t)v, b);
    return r;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static uint32_t hash(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t hash_s(uint32_t h, const char *s)
{
    while (*s)
        h = hash(h, (uint8_t)*s++);
    return hash(h, 0);
}
static void say(const char *a, const char *b)
{
    scopy(ui.msg[0], a ? a : "", 24);
    scopy(ui.msg[1], b ? b : "", 24);
    ui.msg_until = plat_ms() + 1400u;
}
void ui_say(const char *a, const char *b) { say(a, b); }
static int btn_held(int b) { return (int)(ui.btn >> b & 1u); }
static int shift(void) { return btn_held(B_GLO); }
static r1_part_t *part(void) { return &r1_part[set.sel & 15u]; }

/* knob acceleration: fast turns go further on big ranges */
static uint8_t accel_off;                        /* the host simulator's "spin": exact steps */
static int32_t accel(int role, int32_t e, int range)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role], a = (uint32_t)(e < 0 ? -e : e), m;
    ui.enc_t[role] = now;
    if (!a)
        return e;
    if (a > 1)
        dt /= a;
    ui.enc_run[role] = (uint8_t)(dt < 60u ? (ui.enc_run[role] + a > 255u ? 255u : ui.enc_run[role] + a) : a);
    if (range <= 24 || accel_off || ui.enc_run[role] < 4u)
        return e;
    m = dt < 12u ? 8u : dt < 25u ? 5u : dt < 45u ? 3u : 1u;
    if (range < 200 && m > 4u)
        m = 4u;
    return e * (int32_t)m;
}

/* -------------------------------------------------------------------------------- the knobs --- */
static const char *const REV_NAME[5] = {"Room", "Hall", "Large hall", "Plate", "Off"};
static const char *knob_label(int k)
{
    static const char *const L[NPAGE][4] = {{"VOLUME", "PAN", "REVERB", "CHORUS"},
                                             {"CUTOFF", "RESONANCE", "ATTACK", "RELEASE"},
                                             {"REVERB", "TIME", "REV LEVEL", "CHORUS"},
                                             {"VELOCITY", "TRANSPOSE", "KEYS OUT", "LIGHTS"},
                                             {"", "", "", ""}};
    return L[set.page % NPAGE][k & 3];
}
static int knob_get(int k)
{
    r1_part_t *p = part();
    switch (set.page) {
    case PG_PART: return k == 0 ? p->vol : k == 1 ? p->pan : k == 2 ? p->rev : p->cho;
    case PG_SOUND: return k == 0 ? p->cutoff : k == 1 ? p->reso : k == 2 ? p->attack : p->release;
    case PG_FX: return k == 0 ? r1_fx.rev_type : k == 1 ? r1_fx.rev_time : k == 2 ? r1_fx.rev_level : r1_fx.cho_level;
    case PG_SETUP: return k == 0 ? set.kvel : k == 1 ? p->coarse : k == 2 ? set.keys_out : set.lights;
    default: return 0;
    }
}
static void knob_range(int k, int *lo, int *hi)
{
    *lo = 0;
    *hi = 127;
    if (set.page == PG_FX && k == 0)
        *hi = 4;
    if (set.page == PG_SETUP) {
        if (k == 0)
            *lo = 1;
        if (k == 1) {
            *lo = -24;
            *hi = 24;
        }
        if (k == 2)
            *hi = 1;
        if (k == 3)
            *hi = 2;
    }
}
static void knob_text(int k, char *b)
{
    int v = knob_get(k);
    b[0] = 0;
    switch (set.page) {
    case PG_PART:
        if (k == 1) {
            if (v == 64)
                scopy(b, "C", 8);
            else {
                b[0] = v < 64 ? 'L' : 'R';
                itoa_u((uint32_t)(v < 64 ? 64 - v : v - 64), b + 1);
            }
        } else
            itoa_u((uint32_t)v, b);
        break;
    case PG_SOUND: itoa_s(v - 64, b); break;
    case PG_FX:
        if (k == 0)
            scopy(b, REV_NAME[v % 5], 12);
        else
            itoa_u((uint32_t)v, b);
        break;
    case PG_SETUP:
        if (k == 0)
            itoa_u((uint32_t)v, b);
        else if (k == 1)
            itoa_s(v, b);
        else if (k == 2)
            scopy(b, v ? "On" : "Off", 8);
        else {
            static const char *const LT[3] = {"Off", "On", "Keys"};
            scopy(b, LT[v % 3], 8);
        }
        break;
    default: break;
    }
}
static void knob_turn(int k, int32_t e)
{
    int lo, hi, v;
    r1_part_t *p = part();
    int s = set.sel & 15;
    if (set.page == PG_BANK)
        return;
    knob_range(k, &lo, &hi);
    v = clampi(knob_get(k) + (int)accel(EN_K1 + k, e, hi - lo), lo, hi);
    switch (set.page) {
    case PG_PART: r1_cc(s, k == 0 ? 7 : k == 1 ? 10 : k == 2 ? 91 : 93, v); break;
    case PG_SOUND: r1_cc(s, k == 0 ? 74 : k == 1 ? 71 : k == 2 ? 73 : 72, v); break;
    case PG_FX:
        if (k == 0)
            r1_fx.rev_type = (uint8_t)v;
        else if (k == 1)
            r1_fx.rev_time = (uint8_t)v;
        else if (k == 2)
            r1_fx.rev_level = (uint8_t)v;
        else
            r1_fx.cho_level = (uint8_t)v;
        break;
    case PG_SETUP:
        if (k == 0)
            set.kvel = (uint8_t)v;
        else if (k == 1)
            p->coarse = (int8_t)v;
        else if (k == 2)
            set.keys_out = (uint8_t)v;
        else
            set.lights = (uint8_t)v;
        break;
    default: break;
    }
    settings_dirty();
}

/* --------------------------------------------------------------------------------- presets --- */
static int preset_font_range(int font, int *first, int *count)
{
    const r1_font_t *f = r1_font_at(font);
    if (!f)
        return 0;
    *first = f->first;
    *count = f->count;
    return f->count > 0;
}
static void preset_set(int i)
{
    const r1_preset_t *p = r1_preset_at(i);
    char b[24];
    if (!p)
        return;
    r1_program(set.sel, i);
    part()->preset = (uint16_t)i;                /* (the screen at once; the engine takes it at its next block) */
    settings_dirty();
    scopy(b, p->name, 21);
    say(b, 0);
}
static void preset_turn(int d)
{
    int cur = part()->preset, first, count;
    const r1_preset_t *p = r1_preset_at(cur);
    if (!r1_bank() || !p)
        return;
    if (shift()) {                               /* the next font's first (a kit on the drum part) */
        int f = (p->font + r1_fonts() + d) % r1_fonts(), i;
        if (!preset_font_range(f, &first, &count))
            return;
        for (i = first; i < first + count; i++)
            if ((r1_preset_at(i)->bank == 128) == (part()->drums != 0))
                break;
        preset_set(i < first + count ? i : first);
        ui.btn_used |= 1u << B_GLO;
        return;
    }
    if (!preset_font_range(p->font, &first, &count))
        return;
    preset_set(first + (cur - first + count + d) % count);
}

/* ---------------------------------------------------------------------------------- the keys --- */
static void midi_out(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (set.keys_out)
        plat_midi_out(st >> 4 | st << 8 | d1 << 16 | d2 << 24);
}
static void key_down(int k)
{
    int note = clampi(53 + k + 12 * set.oct, 0, 127);
    ui.key_note[k] = (uint8_t)(note + 1);
    ui.key_part[k] = set.sel;
    r1_note(set.sel, note, set.kvel);
    midi_out(0x90u | set.sel, (uint32_t)note, set.kvel);
    ui.help = 0;
}
static void key_up(int k)
{
    if (!ui.key_note[k])
        return;
    r1_note(ui.key_part[k], ui.key_note[k] - 1, 0);
    midi_out(0x80u | ui.key_part[k], ui.key_note[k] - 1u, 0);
    ui.key_note[k] = 0;
}
static void keys_off(void)
{
    int k;
    for (k = 0; k < NKEYS; k++)
        key_up(k);
}

/* the audition: a phrase on the part (an arpeggio, or a beat on a kit) */
static void aud_stop(void)
{
    if (ui.aud_note >= 0)
        r1_note(ui.aud_part, ui.aud_note, 0);
    ui.aud_note = -1;
    ui.aud = 0;
}
static void aud_tick(void)
{
    static const int8_t ARP[8] = {0, 4, 7, 12, 16, 12, 7, 4};
    static const int8_t BEAT[8] = {36, 42, 38, 42, 36, 36, 38, 46};
    uint32_t now = plat_ms();
    if (!ui.aud || now - ui.aud_t < AUD_MS)
        return;
    ui.aud_t = now;
    if (ui.aud_note >= 0)
        r1_note(ui.aud_part, ui.aud_note, 0);
    ui.aud_part = set.sel;
    ui.aud_note = (int16_t)(part()->drums ? BEAT[ui.aud_i & 7] : 60 + 12 * set.oct + ARP[ui.aud_i & 7]);
    r1_note(ui.aud_part, ui.aud_note, set.kvel);
    ui.aud_i++;
}

/* ------------------------------------------------------------------------------- the buttons --- */
static void set_page(int p)
{
    set.page = (uint8_t)p;
    ui.help = 0;
    settings_dirty();
}
static void part_reset(int s)
{
    static const uint8_t CC[10][2] = {{7, 100}, {10, 64}, {91, 40}, {93, 0}, {74, 64}, {71, 64}, {73, 64}, {72, 64},
                                      {11, 127}, {1, 0}};
    int i;
    for (i = 0; i < 10; i++)
        r1_cc(s, CC[i][0], CC[i][1]);
    r1_part[s].coarse = 0;
}
static void btn_down(int b)
{
    if (b != B_GLO && shift())
        ui.btn_used |= 1u << B_GLO;
    switch (b) {
    case B_PLAY:
        if (ui.aud)
            aud_stop();
        else {
            ui.aud = 1;
            ui.aud_i = 0;
            ui.aud_note = -1;
            ui.aud_t = plat_ms() - AUD_MS;
        }
        ui.btn_used |= 1u << b;
        break;
    case B_OCTDN:
    case B_OCTUP: {
        char t[6];
        keys_off();
        set.oct = (int8_t)clampi(set.oct + (b == B_OCTUP ? 1 : -1), -3, 3);
        settings_dirty();
        say("OCTAVE", itoa_s(set.oct, t));
        ui.btn_used |= 1u << b;
        break;
    }
    default: break;
    }
}
static void btn_tap(int b)
{
    int s = set.sel & 15;
    switch (b) {
    case B_HOME: set_page(PG_PART); break;
    case B_LFO:
    case B_ENV: set_page(PG_SOUND); break;
    case B_FX: set_page(PG_FX); break;
    case B_SEQ: set_page(PG_BANK); break;
    case B_REC:
        keys_off();
        aud_stop();
        r1_panic();
        say("ALL NOTES", "OFF");
        break;
    case B_SAVE: say(settings_save() ? "NOT SAVED" : "SAVED", 0); break;
    case B_ARP:
        ui.hold = (uint8_t)!ui.hold;
        r1_cc(s, 64, ui.hold ? 127 : 0);
        say("HOLD", ui.hold ? "ON" : "OFF");
        break;
    case B_SEL:
        if (shift()) {                           /* solo: the others muted (again: all back) */
            int i, solo = 1;
            for (i = 0; i < R1_NPART; i++)
                if (i != s && !r1_part[i].mute)
                    solo = 0;
            for (i = 0; i < R1_NPART; i++)
                r1_part[i].mute = (uint8_t)(solo ? 0 : i != s);
            say(solo ? "ALL PARTS" : "SOLO", 0);
        } else {
            r1_part[s].mute ^= 1u;
            say(r1_part[s].mute ? "MUTED" : "UNMUTED", 0);
        }
        settings_dirty();
        break;
    case B_EDIT:
        if (shift()) {
            keys_off();
            aud_stop();
            r1_reset();
            say("GM RESET", 0);
        } else {
            part_reset(s);
            say("PART RESET", 0);
        }
        settings_dirty();
        break;
    default: break;
    }
}

static const char *const BTN_NAME[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                         "PLAY", "REC", "OCT-", "OCT+"};
static const char *const HELP[NB][3] = {
    {"Effects: reverb", "kind, time, level;", "the chorus"},
    {"Mutes the part.", "GLO+SEL: solo", "(again: all back)"},
    {"The SOUND page:", "cutoff, resonance,", "attack, release"},
    {"The SOUND page:", "cutoff, resonance,", "attack, release"},
    {"Resets the part's", "knobs. GLO+EDIT:", "all parts (GM)"},
    {"", "", ""},
    {"The PART page:", "volume, pan,", "reverb, chorus"},
    {"Saves the parts,", "the effects and", "the keys' settings"},
    {"Hold: the sustain", "pedal for the", "part (again: off)"},
    {"The BANK page:", "the fonts loaded,", "the room they use"},
    {"", "", ""},
    {"All notes off", "(every part)", ""},
    {"The keys down an", "octave", ""},
    {"The keys up an", "octave", ""},
};
static void hold_tick(void)
{
    uint32_t now = plat_ms(), b = ui.btn;
    int i;
    if (ui.help) {
        if (b != 1u << (ui.help - 1) || ui.keys || (int32_t)(ui.turn_t - ui.down_t[ui.help - 1]) > 0)
            ui.help = 0;
        return;
    }
    if (!b || (b & (b - 1u)) || ui.keys)
        return;
    for (i = 0; i < NB; i++) {
        if (b != 1u << i || (ui.btn_used & b) || (int32_t)(ui.turn_t - ui.down_t[i]) > 0)
            continue;
        if (i == B_GLO && now - ui.down_t[i] >= SETUP_MS) {
            set_page(PG_SETUP);
            ui.btn_used |= b;
        } else if (i != B_GLO && i != B_PLAY && now - ui.down_t[i] >= HELP_MS) {
            ui.help = (uint8_t)(i + 1);
            ui.btn_used |= b;
        }
    }
}

static void input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), ch, now = plat_ms();
    int i;
    int32_t e;
    ch = btn ^ ui.btn;
    ui.btn = btn;
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i;
        if (!(ch & m))
            continue;
        if (btn & m) {
            ui.btn_used &= ~m;
            ui.down_t[i] = now;
            btn_down(i);
        } else if (!(ui.btn_used & m))
            btn_tap(i);
    }
    ch = keys ^ ui.keys;
    ui.keys = keys;
    for (i = 0; i < NKEYS; i++)
        if (ch >> i & 1u) {
            if (keys >> i & 1u)
                key_down(i);
            else
                key_up(i);
        }
    if (btn || keys || ch)
        ui.act_t = now;
    if ((e = plat_enc(EN_ALGO)) != 0) {          /* the part */
        int s = clampi(set.sel + (e > 0 ? 1 : -1), 0, R1_NPART - 1);
        if (s != set.sel) {
            keys_off();
            if (ui.hold)
                r1_cc(set.sel, 64, 0);
            ui.hold = 0;
            set.sel = (uint8_t)s;
            settings_dirty();
        }
        ui.turn_t = ui.act_t = now;
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        preset_turn(e > 0 ? 1 : -1);
        ui.turn_t = ui.act_t = now;
    }
    if ((e = plat_enc(EN_SELECT)) != 0) {
        set_page((set.page + NPAGE + (e > 0 ? 1 : -1)) % NPAGE);
        ui.turn_t = ui.act_t = now;
    }
    for (i = 0; i < 4; i++)
        if ((e = plat_enc(EN_K1 + i)) != 0) {
            knob_turn(i, e);
            ui.touched = (uint8_t)(i + 1);
            ui.touch_t = now;
            ui.turn_t = ui.act_t = now;
        }
    hold_tick();
}

/* --------------------------------------------------------------------------------- the lights --- */
static int blink(void) { return (int)((plat_ms() / 200u) & 1u); }
static void leds(void)
{
    uint32_t b = 0, k = 0;
    if (ui.aud)
        b |= 1u << B_PLAY;
    if (ui.hold)
        b |= 1u << B_ARP;
    if (part()->mute)
        b |= 1u << B_SEL;
    if (settings_is_dirty())
        b |= 1u << B_SAVE;
    if (shift())
        b |= 1u << B_GLO;
    switch (set.page) {
    case PG_PART: b |= 1u << B_HOME; break;
    case PG_SOUND: b |= 1u << B_LFO | 1u << B_ENV; break;
    case PG_FX: b |= 1u << B_FX; break;
    case PG_BANK: b |= 1u << B_SEQ; break;
    default: b |= (uint32_t)blink() << B_GLO; break;
    }
    if (set.oct > 0)
        b |= 1u << B_OCTUP;
    if (set.oct < 0)
        b |= 1u << B_OCTDN;
    if (set.lights)
        k = ui.keys;
    if (set.lights == 2)
        b = 0;
    plat_glow(set.glow && set.lights == 1);       /* Off and Keys: the unlit buttons dark too */
    plat_play_red(0);
    plat_leds(set.lights ? b : 0, k);
}

/* ------------------------------------------------------------------------------ the screen --- */
#define LCD_BG RGB(240, 166, 58)
#define LCD_OFF RGB(224, 150, 48)
#define LCD_INK RGB(50, 26, 6)
#define LCD_MID RGB(160, 96, 28)
#define LCD_EDGE RGB(30, 20, 12)

/* a glyph in dots: size 0 a pixel a dot, 1 two by two, 2 two by two three apart (the big one); off: the
 * unlit dots' colour (0 none) */
static void dot_glyph(int32_t x, int32_t y, int c, int size, uint16_t ink, uint16_t off)
{
    const uint8_t *g = DOT5X7[(c < 32 || c > 126) ? '?' - 32 : c - 32];
    int r, col, pitch = size == 0 ? 1 : size == 1 ? 2 : 3, d = size == 0 ? 1 : 2;
    for (r = 0; r < 7; r++)
        for (col = 0; col < 5; col++) {
            int on = g[r] >> (4 - col) & 1;
            if (on || off)
                cv_rect(x + col * pitch, y + r * pitch, d, d, on ? ink : off);
        }
}
static int32_t dot_w(int size) { return size == 0 ? 6 : size == 1 ? 12 : 18; }
static int32_t dot_text(int32_t x, int32_t y, const char *s, int size, uint16_t ink, uint16_t off)
{
    for (; *s; s++, x += dot_w(size))
        dot_glyph(x, y, (uint8_t)*s, size, ink, off);
    return x;
}
static void dot_text_r(int32_t rx, int32_t y, const char *s, int size, uint16_t ink)
{
    dot_text(rx - slen(s) * dot_w(size) + 1, y, s, size, ink, 0);
}
static void dot_text_c(int32_t cx, int32_t y, const char *s, int size, uint16_t ink)
{
    dot_text(cx - (slen(s) * dot_w(size) - 1) / 2, y, s, size, ink, 0);
}
static void frame_sides(int32_t h)
{
    cv_rect(0, 0, 3, h, LCD_EDGE);
    cv_rect(237, 0, 3, h, LCD_EDGE);
}

/* band 1: the status, the sixteen parts' meters */
static void draw_meters(void)
{
    uint32_t h = 7;
    int i, segs[R1_NPART];
    char t[16];
    for (i = 0; i < R1_NPART; i++) {
        float m = r1_meter[i];
        segs[i] = r1_part[i].mute ? -1 : (int)(fm_sqrtf(m < 0.0f ? 0.0f : m) * 12.0f + 0.5f);
        if (segs[i] > 12)
            segs[i] = 12;
        h = hash(h, (uint32_t)(segs[i] + 2));
    }
    h = hash(hash(hash(h, set.sel), r1_voices), (uint32_t)(r1_activity != 0));
    if (h == ui.sig[0])
        return;
    ui.sig[0] = h;
    cv_begin(240, 72, LCD_BG);
    frame_sides(72);
    cv_rect(0, 0, 240, 3, LCD_EDGE);
    t[0] = 0;
    dot_text(8, 6, "PART", 0, LCD_INK, 0);
    itoa_u((uint32_t)set.sel + 1u, t);
    dot_text(36, 6, t, 0, LCD_INK, 0);
    dot_text(56, 6, part()->drums ? "DRUMS" : "", 0, LCD_MID, 0);
    itoa_u(r1_voices, t);
    dot_text_r(200, 6, t, 0, LCD_INK);
    dot_text(204, 6, "V", 0, LCD_MID, 0);
    cv_rect(222, 6, 9, 7, r1_activity ? LCD_INK : LCD_OFF);   /* MIDI in: lit while notes come */
    r1_activity = 0;
    for (i = 0; i < R1_NPART; i++) {             /* each part: a bar of twelve segments, its number under it */
        int32_t x = 9 + i * 14, s;
        for (s = 0; s < 12; s++)
            cv_rect(x, 54 - s * 3, 11, 2, s < segs[i] ? LCD_INK : LCD_OFF);
        itoa_u((uint32_t)i + 1u, t);
        if (i == set.sel) {
            cv_rect(x - 1, 58, 13, 10, LCD_INK);
            dot_text_c(x + 5, 60, t, 0, LCD_BG);
        } else
            dot_text_c(x + 5, 60, t, 0, segs[i] < 0 ? LCD_OFF : LCD_MID);
    }
    cv_blit(0, 0);
}

/* band 2: the preset's name in big dots (or a message, or a help card) */
static void name_lines(const char *s, char *a, char *b)
{
    int n = slen(s), cut = n;
    if (n > 12) {                                /* two lines, broken at a space if there is one */
        cut = 12;
        while (cut > 4 && s[cut] != ' ')
            cut--;
        if (s[cut] != ' ')
            cut = 12;
    }
    scopy(a, s, cut + 1 > 13 ? 13 : cut + 1);
    while (s[cut] == ' ')
        cut++;
    scopy(b, s + cut, 13);
}
static void draw_name(void)
{
    const r1_preset_t *p = r1_preset_at(part()->preset);
    const r1_font_t *f = p ? r1_font_at(p->font) : 0;
    char l1[24], l2[24], fn[32], bp[16], t[8];
    uint32_t h = 11;
    int msg = ui.msg_until && (int32_t)(plat_ms() - ui.msg_until) < 0;
    if (ui.help) {
        scopy(l1, BTN_NAME[ui.help - 1], 24);
        l2[0] = 0;
    } else if (msg) {
        scopy(l1, ui.msg[0], 13);
        scopy(l2, ui.msg[1], 13);
    } else if (p) {
        char nm[24];
        scopy(nm, p->name, 21);
        name_lines(nm, l1, l2);
    } else {
        scopy(l1, "NO SOUNDS", 13);
        scopy(l2, "LOAD A BANK", 13);
    }
    scopy(fn, f ? f->name : "", 25);
    bp[0] = 0;
    if (p) {
        itoa_u(p->bank, bp);
        scopy(bp + slen(bp), ":", 2);
        itoa_u(p->prog, t);
        scopy(bp + slen(bp), t, 8);
    }
    h = hash(hash_s(hash_s(hash_s(hash_s(h, l1), l2), fn), bp), ui.help);
    if (h == ui.sig[1])
        return;
    ui.sig[1] = h;
    cv_begin(240, 84, LCD_BG);
    frame_sides(84);
    cv_rect(8, 0, 224, 1, LCD_MID);
    if (ui.help) {
        int i;
        dot_text(10, 6, l1, 1, LCD_INK, 0);
        for (i = 0; i < 3; i++)
            dot_text(10, 28 + i * 16, HELP[ui.help - 1][i], 1, LCD_INK, 0);
    } else {
        char c1[13], c2[13];
        int i;
        for (i = 0; i < 12; i++) {               /* twelve cells a line, every dot there, lit or not */
            c1[i] = i < slen(l1) ? l1[i] : ' ';
            c2[i] = i < slen(l2) ? l2[i] : ' ';
        }
        c1[12] = c2[12] = 0;
        dot_text(12, 6, c1, 2, LCD_INK, LCD_OFF);
        dot_text(12, 30, c2, 2, LCD_INK, LCD_OFF);
        if (fn[0]) {
            if (slen(fn) > 26)
                fn[26] = 0;
            dot_text(10, 60, fn, 0, LCD_MID, 0);
        }
        dot_text_r(230, 60, bp, 0, LCD_INK);
    }
    cv_rect(8, 76, 224, 1, LCD_MID);
    cv_blit(0, 72);
}

/* band 3: the page and its four knobs (the BANK page: what is loaded) */
static void draw_page(void)
{
    char v[4][16], t[40];
    uint32_t h = 13;
    int k, touched = ui.touched && plat_ms() - ui.touch_t < 1500u ? ui.touched - 1 : -1;
    for (k = 0; k < 4; k++) {
        knob_text(k, v[k]);
        h = hash(hash_s(hash_s(h, v[k]), knob_label(k)), (uint32_t)knob_get(k));
    }
    h = hash(hash(hash(h, set.page), (uint32_t)touched), set.sel);
    if (set.page == PG_BANK)
        h = hash(hash(h, r1_bank() ? r1_bank()->id : 0), (uint32_t)r1_bank_ok_b());
    if (h == ui.sig[2])
        return;
    ui.sig[2] = h;
    cv_begin(240, 84, LCD_BG);
    frame_sides(84);
    cv_rect(0, 81, 240, 3, LCD_EDGE);
    dot_text(10, 4, PAGE_NAME[set.page % NPAGE], 1, LCD_INK, 0);
    for (k = 0; k < NPAGE; k++)                  /* the pages, as dots */
        cv_rect(196 + k * 7, 8, 5, 5, k == set.page ? LCD_INK : LCD_OFF);
    if (set.page == PG_BANK) {
        const r1_bank_t *b = r1_bank();
        if (!b) {
            dot_text(10, 28, "No bank in the firmware", 0, LCD_INK, 0);
        } else {
            itoa_u(b->nfont, t);
            dot_text(10, 26, "FONTS", 0, LCD_MID, 0);
            dot_text(46, 26, t, 0, LCD_INK, 0);
            itoa_u(b->npreset, t);
            dot_text(100, 26, "PRESETS", 0, LCD_MID, 0);
            dot_text(148, 26, t, 0, LCD_INK, 0);
            dot_text(10, 40, "IN FIRMWARE", 0, LCD_MID, 0);
            itoa_u((b->len + 1023u) / 1024u, t);
            scopy(t + slen(t), " KB", 4);
            dot_text(100, 40, t, 0, LCD_INK, 0);
            dot_text(10, 54, "IN DATA AREA", 0, LCD_MID, 0);
            if (!b->lenb)
                scopy(t, "-", 4);
            else {
                itoa_u((b->lenb + 1023u) / 1024u, t);
                scopy(t + slen(t), r1_bank_ok_b() ? " KB" : " KB MISSING", 12);
            }
            dot_text(100, 54, t, 0, LCD_INK, 0);
            {
                char nm[25];
                scopy(nm, b->name, 25);
                dot_text(10, 68, nm, 0, LCD_MID, 0);
            }
        }
        dot_text_r(230, 68, "v" EN_VERSION, 0, LCD_MID);   /* (ROM-1's version: the bug reports ask for it) */
        cv_blit(0, 156);
        return;
    }
    for (k = 0; k < 4; k++) {
        int32_t x = 4 + k * 58, cx = x + 29;
        int lo, hi, val = knob_get(k), n, s;
        uint16_t ink = k == touched ? LCD_INK : LCD_INK;
        if (k == touched)
            cv_rect(x + 2, 22, 54, 56, LCD_OFF);
        dot_text_c(cx, 26, knob_label(k), 0, LCD_MID);
        if (slen(v[k]) <= 4)
            dot_text_c(cx, 40, v[k], 1, ink);
        else
            dot_text_c(cx, 44, v[k], 0, ink);
        knob_range(k, &lo, &hi);
        n = hi > lo ? (val - lo) * 10 / (hi - lo) : 0;
        for (s = 0; s < 10; s++)                 /* where it is: ten segments */
            cv_rect(x + 6 + s * 5, 64, 4, 6, s <= n && (hi - lo > 1 || val) ? LCD_INK : LCD_OFF);
    }
    cv_blit(0, 156);
}

void ui_bank_changed(void)
{
    ui.sig[0] = ui.sig[1] = ui.sig[2] = 0;
    keys_off();
    aud_stop();
}

void ui_init(void)
{
    memset(&ui, 0, sizeof ui);
    ui.aud_note = -1;
    lcd_fill(0, 0, 240, 240, LCD_BG);
}

void (*ui_mark)(int part);                       /* a development build's clock: where a frame's time goes */
#define UI_MARK_(p)                                                                                          \
    do {                                                                                                     \
        if (ui_mark)                                                                                         \
            ui_mark(p);                                                                                      \
    } while (0)

void ui_frame(void)
{
    UI_MARK_(0);
    input();
    aud_tick();
    UI_MARK_(1);
    draw_meters();
    UI_MARK_(2);
    draw_name();
    UI_MARK_(3);
    draw_page();
    UI_MARK_(4);
    leds();
    UI_MARK_(5);
    ui.frame++;
}

void ui_input_only(void)
{
    input();
    aud_tick();
}
