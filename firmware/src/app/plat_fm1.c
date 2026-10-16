/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1 platform on the FM-1: plat.h over Felucca's HAL, panel map, USB rings, storage and the bank's
 * regions. Part of the unity build (main.c), after the HAL, usb.c, storage.c and panel.c. */

uint32_t plat_ms(void) { return fm1_ms; }

uint32_t plat_buttons(void)
{
    uint32_t raw = fm1_in.buttons, out = 0, i;
    for (i = 0; i < NB; i++)
        if (raw & (1u << panel.btn[i]))
            out |= 1u << i;
    return out;
}

uint32_t plat_keys(void) { return fm1_in.notes & ((1u << NKEYS) - 1u); }

int32_t plat_enc(int role)
{
    if (role < 0 || role >= NE)
        return 0;
    return fm1_enc_take(panel.enc[role]) * panel.dir[role];
}

static volatile uint32_t master_q12 = 2048;
uint32_t plat_master(void) { return master_q12; }

/* LEDs: the picture is built off-line and copied one byte per column (Felucca: clearing
 * and relighting would let the 10 kHz scan catch the dark gap and flicker) */
static uint8_t led_pos[FM1_NKEY];
static void led_pos_init(void)
{
    uint32_t id, p, r;
    for (id = 0; id < FM1_NKEY; id++) {
        led_pos[id] = 0xFF;
        for (p = 0; p < FM1_NCOL; p++)
            for (r = 1; r < 5u; r++)
                if (FM1_KEYMAP[r][p] == (int8_t)id)
                    led_pos[id] = (uint8_t)((p << 3) | r);
    }
}
/* Glow: every button not lit is lit dim, on one scan frame in GLOW_DIV (the HAL's fm1_led_dim) */
#define GLOW_DIV 4u
static uint8_t glow_on = 1;
void plat_glow(int on) { glow_on = (uint8_t)(on != 0); }
/* PLAY has two LEDs: its red at the button's own position, a green at the keyless matrix position
 * column 8, row PA5 (found on the device). Green, unless recording. */
static uint8_t play_red;
void plat_play_red(int red) { play_red = (uint8_t)(red != 0); }
void plat_leds(uint32_t buttons, uint32_t keys)
{
    uint8_t nl[FM1_NCOL] = {0}, dl[FM1_NCOL] = {0};
    uint32_t i, c;
    for (i = 0; i < NB + NKEYS; i++) {
        uint32_t id = i < NB ? panel.btn[i] : 14u + (i - NB);
        int on = i < NB ? (buttons >> i) & 1u : (keys >> (i - NB)) & 1u;
        uint8_t q = led_pos[id];
        if (i == B_PLAY && !play_red)            /* PLAY's green LED instead (keyless position col 8, row PA5) */
            q = (uint8_t)((8u << 3) | 1u);
        if (q == 0xFF)
            continue;
        if (on)
            nl[q >> 3] |= (uint8_t)(1u << (q & 7u));
        else if (i < NB && glow_on)
            dl[q >> 3] |= (uint8_t)(1u << (q & 7u));
    }
    for (c = 0; c < FM1_NCOL; c++) {
        fm1_led[c] = nl[c];
        fm1_led_dim[c] = dl[c];
    }
    fm1_led_dim_div = glow_on ? GLOW_DIV : 0u;
}

static volatile uint32_t audio_cpu_pct, audio_xruns, midi_drops;

/* MIDI: Felucca's rings carry 4-byte USB-MIDI event packets, byte 0 = cable / CIN */
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = midi_in_q[mi_r % MQ];
    mi_r++;
    return 1;
}
static uint32_t midi_out_hw;                     /* the USB ring's most in it at once (SysEx 01) */
int plat_midi_out(uint32_t pkt)
{
    uint32_t used = mo_w - mo_r;
    if (used > midi_out_hw)
        midi_out_hw = used;
    return midi_out_event(pkt);
}
int plat_usb_up(void) { return usb.config && !usb.suspended; }
void r1_lock(int on)                           /* the engine held off while the UI swaps the bank */
{
    static uint32_t f;
    if (on)
        f = irq_save();
    else
        irq_restore(f);
}

int plat_store_load(uint32_t obj, void *dst, uint32_t max) { return flash_ok ? st_load(obj, dst, max) : -1; }
static int32_t store_rc[OBJ_NOBJ];               /* each object's last save (the SysEx status reports them) */
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    int rc = flash_ok ? st_save(obj, src, len) : -9;
    if (obj < OBJ_NOBJ)
        store_rc[obj] = rc;
    return rc;
}
/* the bank: region A from the next 4 KiB after the app's code (tools/build.py puts it there), region B in
 * the data area (readable through the plain XIP window, fl_plain_window_init at boot) */
extern uint32_t _image_end[];
const uint8_t *plat_bank_a(void)
{
    uint32_t end = (uint32_t)(uintptr_t)_image_end - FM1_XIP(0x4120u);   /* (app.bin's offset 0: flash 0x4120) */
    uint32_t off = (end + 0xFFFu) & ~0xFFFu;
    const uint8_t *p = fm1_xip_ptr(0x4120u + off);
    const r1_bank_t *h = (const r1_bank_t *)(const void *)p;
    return h->magic == R1_MAGIC ? p : 0;
}
const uint8_t *plat_bank_b(void) { return flash_ok ? fm1_xip_ptr(R1_B_BASE) : 0; }
static int32_t bank_rc;                         /* the last write's result (the SysEx status reports it) */
int plat_bank_b_erase(uint32_t off)
{
    if (!flash_ok || off >= R1_B_ROOM || (off & 0xFFFu))
        return bank_rc = -8;
    return bank_rc = st_erase(R1_B_BASE + off);
}
int plat_bank_b_prog(uint32_t off, const void *src, uint32_t n)
{
    if (!flash_ok || off + n > R1_B_ROOM || (off & 0xFFu) + n > 256u)
        return bank_rc = -8;
    return bank_rc = st_prog(R1_B_BASE + off, src, n);
}

uint32_t plat_cpu_pct(void) { return audio_cpu_pct; }
uint32_t plat_xruns(void) { return audio_xruns; }
