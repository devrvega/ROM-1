/* SPDX-License-Identifier: GPL-3.0-only
 * Boot, audio ISR and main loop adapted from Felucca's main.c / audio.c, Copyright (C) 2026
 * Leo Kuroshita (@kurogedelic), Hugelton Instruments. */
/* ROM-1 on the FM-1 (from IND-1's, AMB-1's and X0X's): boot (WDT first, boot-loop guard, guards), LCD, input (TIMER5
 * 10 kHz), audio (ALNK0), USB; then the main loop: UI at ~60 frames/s, input polled in between. */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];

/* --------------------------------------------------------------- audio --- */
/* ROM-1 renders 128-frame halves (2.9 ms): a note from MIDI or the keys sounds within that */
#define HALF_FRAMES 128u                    /* I2S half buffer: 2.9 ms at 44.1 kHz */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define FS 44100u
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));

#define DBG_MAGIC 0x44424731u                       /* "DBG1": read with `fm1t memr` */
struct en_dbg {
    uint32_t magic, halves, max_us, nested, in_audio, late, timer_irqs, ui_frames;
    uint32_t last_us, cpu_q8, boots, stage;
    uint32_t crash_seen;                            /* the crash count already reported at a boot */
} en_dbg __attribute__((section(".noinit")));
static uint8_t safe_mode;                           /* two failed boots: no audio, USB on (safe_main) */
static uint32_t render_max_us, midi_rx_count;

static void render_block(int32_t *o)                /* ALNK0: the MIDI in, the engine; measured */
{
    uint32_t t0 = fm1_ticks(), us, pkt, k = 0, budget = HALF_FRAMES * 1000000u / FS, i;
    en_dbg.in_audio = 1;
    while (k < 64u && plat_midi_in(&pkt)) {         /* the USB port's and the TRS jack's alike */
        r1_midi(pkt >> 8 & 0xFFu, pkt >> 16 & 0x7Fu, pkt >> 24 & 0x7Fu);
        midi_rx_count++;
        k++;
    }
    if (uploading)
        for (i = 0; i < HALF_WORDS; i++)
            o[i] = 0;
    else
        r1_render(o, HALF_FRAMES, master_q12);
    us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    r1_load(us * 100u / budget);
    en_dbg.cpu_q8 = (en_dbg.cpu_q8 * 31u + (us * 256u) / budget) / 32u;
    audio_cpu_pct = (en_dbg.cpu_q8 * 100u) >> 8;
    en_dbg.halves++;
    en_dbg.last_us = us;
    if (us > en_dbg.max_us)
        en_dbg.max_us = us;
    if (us > render_max_us)
        render_max_us = us;
    en_dbg.in_audio = 0;
}

void fm1_alnk0_irq(void)                            /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half();
        render_block(&abuf[half * HALF_WORDS]);
        fm1_audio_ack_half();
        if (fm1_audio_free_half() != half) {
            en_dbg.late++;                         /* the DMA moved on while we rendered */
            audio_xruns++;
        }
    }
}
extern void isr_alnk0(void);

static void audio_init(void)
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}

static void audio_silence(void)                     /* IRQs off (flash erase): the DMA would loop stale audio */
{
    uint32_t i;
    for (i = 0; i < sizeof abuf / sizeof abuf[0]; i++)
        abuf[i] = 0;
}

/* ROM-1's SysEx (F0 7D 52 31 cmd .. F7, 7D the non-commercial ID; values as five 7-bit groups):
 *   01  the status: load %, slowest render (us), renders late, renders, a bench build, voices, notes, the
 *       screen's frames, its time (us, summed) and slowest frame, MIDI in, region A's id and length, region
 *       B's expected length, B there and right, flash ok, the last write's result, the boots counted, the
 *       voices allowed now (fewer while the renders run heavy)
 *   03  the slowest and the late counted again       04 s  the screen not drawn for s seconds
 *   20 len id   region B: writing begins (no sound until 22)
 *   21 off data region B: bytes at off (7-bit packed: each 7 bytes after a byte of their high bits), at most
 *       a page (256 B), within it; a sector is erased as its first byte comes. Answered 21 off (or 7F err)
 *   22          region B written: checked, and played (answered 22 with B there and right)
 * General MIDI's and GS's resets reset the parts. A bench build (./build.sh --bench, -DROM1_BENCH) also
 * takes 02 k (0: every voice at once on all the parts; 1: a chord a part, a drum beat) and 06 (notes off). */
#ifdef ROM1_BENCH
extern void (*ui_mark)(int part);
static uint32_t ui_part[10], ui_mark_t;           /* where the screen's frames go (SysEx 0A) */
static void dev_mark(int p)
{
    uint32_t t = fm1_ticks();
    if (p)
        ui_part[p] += t - ui_mark_t;
    ui_mark_t = t;
}
#endif
static void dev_put(uint8_t *b, uint32_t v)
{
    uint32_t k;
    for (k = 0; k < 5u; k++)
        b[k] = (uint8_t)((v >> (7u * k)) & 0x7Fu);
}
static uint32_t dev_get(const uint8_t *b)
{
    uint32_t k, v = 0;
    for (k = 0; k < 5u; k++)
        v |= (uint32_t)(b[k] & 0x7Fu) << (7u * k);
    return v;
}
static uint32_t ui_pause_until;                     /* SysEx 04: the screen not drawn, to see what it costs */
static uint32_t ui_us, ui_us_max;                   /* the screen's frames: their time, the slowest (SysEx 01) */
static void dev_reply(uint8_t *r, uint32_t cmd, uint32_t nvals)
{
    r[0] = 0xF0;
    r[1] = 0x7D;
    r[2] = 0x52;
    r[3] = 0x31;
    r[4] = (uint8_t)cmd;
    r[5 + 5u * nvals] = 0xF7;
    ota_frame_done();
    ota_wire_send(r, 6u + 5u * nvals);
}
static uint8_t up_buf[256] __attribute__((aligned(4)));
static void dev_sysex(void)
{
    const uint8_t *p;
    uint32_t n;
    static uint8_t r[6 + 5 * 24 + 1];
    if (!ota_frame_get(&p, &n))
        return;
    if (r1_sysex(p, n)) {                           /* General MIDI's and GS's resets */
        ota_frame_done();
        return;
    }
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x52u || p[2] != 0x31u)
        return;                                     /* not ours: left for the updater */
    switch (p[3]) {
    case 1: {
        const r1_bank_t *b = r1_bank();
        uint32_t v[19], k;
        v[0] = (en_dbg.cpu_q8 * 100u) >> 8;
        v[1] = render_max_us;
        v[2] = en_dbg.late;
        v[3] = en_dbg.halves;
#ifdef ROM1_BENCH
        v[4] = 1u;
#else
        v[4] = 0u;
#endif
        v[5] = r1_voices;
        v[6] = r1_notes;
        v[7] = en_dbg.ui_frames;
        v[8] = ui_us;
        v[9] = ui_us_max;
        v[10] = midi_rx_count;
        v[11] = b ? b->id : 0;
        v[12] = b ? b->len : 0;
        v[13] = b ? b->lenb : 0;
        v[14] = (uint32_t)r1_bank_ok_b();
        v[15] = flash_ok;
        v[16] = (uint32_t)(bank_rc + 100);
        v[17] = en_dbg.boots;
        v[18] = r1_voice_cap();
        for (k = 0; k < 19u; k++)
            dev_put(r + 5 + 5 * k, v[k]);
        dev_reply(r, 1, 19);
        return;
    }
    case 3:
        en_dbg.max_us = 0;
        en_dbg.late = 0;
        render_max_us = 0;
        ui_us_max = 0;
        break;
    case 4:
        ui_pause_until = fm1_ms + 1000u * (n >= 5u ? p[4] : 5u);
        break;
    case 0x20:                                      /* region B: begin */
        if (n >= 14u) {
            dev_put(r + 5, up_begin(dev_get(p + 4), dev_get(p + 9)) ? 1u : 0u);
            dev_reply(r, 0x20, 1);
            return;
        }
        break;
    case 0x21: {                                    /* region B: a page's bytes */
        uint32_t off, m = 0, i, rc = 1;
        if (n < 9u || !uploading)
            break;
        off = dev_get(p + 4);
        for (i = 9; i < n && m < 256u;) {           /* 7-bit packed: a byte of high bits, then up to seven */
            uint32_t hi = p[i++], j;
            for (j = 0; j < 7u && i < n && m < 256u; j++)
                up_buf[m++] = (uint8_t)(p[i++] | ((hi >> j & 1u) << 7));
        }
        rc = (uint32_t)up_data(off, up_buf, m);
        dev_put(r + 5, rc ? 0x7FFFFFFu : off);
        dev_reply(r, 0x21, 1);
        return;
    }
    case 0x22:                                      /* region B: done; checked and played */
        dev_put(r + 5, (uint32_t)up_end());
        dev_reply(r, 0x22, 1);
        return;
#ifdef ROM1_BENCH
    case 2:
        if (n >= 5u) {
            int i, j;
            ota_frame_done();
            if (p[4] == 0) {                        /* everything: every part a four-note chord, held */
                for (i = 0; i < R1_NPART; i++)
                    for (j = 0; j < 4; j++)
                        r1_note(i, 48 + i + j * 5, 100);
            } else {
                for (i = 0; i < 8; i++)
                    r1_note(i, 60 + i * 2, 90);
                r1_note(R1_DRUMS, 36, 110);
                r1_note(R1_DRUMS, 42, 90);
            }
            return;
        }
        break;
    case 6:
        r1_panic();
        break;
    case 10: {                                      /* the screen's parts: ticks (24 MHz) each, the transfers' wait */
        uint32_t k;
        ui_mark = dev_mark;
        for (k = 0; k < 10u; k++)
            dev_put(r + 5 + 5 * k, ui_part[k]);
        dev_put(r + 55, lcd_wait_t);
        dev_reply(r, 10, 11);
        return;
    }
#endif
    default: break;
    }
    ota_frame_done();
}

/* ------------------------------------------------------------ timer --- */
static uint8_t usb_due;
void fm1_timer5_irq(void)
{
    static uint32_t sub;
    fm1_timer5_ack();
    en_dbg.timer_irqs++;
    if (en_dbg.in_audio)
        en_dbg.nested++;
    fm1_input_tick();
    /* 2 kHz: all USB SIE traffic lives here. This ISR now nests into the audio render (so the key
     * matrix keeps its rhythm, below); USB shares the MIDI queues with the audio ISR, so a poll that
     * falls inside the render waits for the first tick outside it, as it did before the nesting. */
    if (sub % 5u == 0u) {
        usb_due = 1;
#if FELUCCA_UAC
        /* the USB audio stream cannot wait for the render (one packet per 1 ms frame, a render takes
         * up to ~11.6 ms): it runs nested too. It touches only EP4 (INDEX is set on every access) and
         * the consumer side of its ring, and usb_poll never runs nested, so the two never interleave */
        if (en_dbg.in_audio)
            uac_service();
#endif
    }
    if (usb_due && !en_dbg.in_audio) {
        usb_due = 0;
        usb_poll();
#if EN_TRS
        uart_midi_poll();                           /* the TRS jack: same context, same queue */
#endif
    }
    if (++sub == 10u)
        sub = 0;
    {
        static uint32_t last, acc;
        uint32_t now = fm1_ticks();
        acc += now - last;
        last = now;
        while (acc >= 1000u * FM1_TICKS_PER_US) {
            acc -= 1000u * FM1_TICKS_PER_US;
            fm1_ms++;
        }
    }
}
extern void isr_timer5(void);

/* ------------------------------------------------------------- boot --- */
#define BOOTGUARD_MAGIC 0x42475244u
struct bootguard_s bootguard __attribute__((section(".noinit")));   /* declared in main.c (ota_commit) */

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, RGB(160, 0, 0));
    draw_text_box(0, 8, 240, &FONT_S, "ROM-1 CRASH", C_WHITE, 1);
    hexs(b, c->vec);
    draw_text_box(10, 40, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->pc);
    draw_text_box(10, 60, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->emu);
    draw_text_box(10, 84, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->dbg);
    draw_text_box(10, 102, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->rets);
    draw_text_box(10, 120, 220, &FONT_S, b, C_WHITE, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

static void enter_uboot(const char *why)
{
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 110, 240, &FONT_S, why, C_WHITE, 1);
    fm1_delay_ms(20);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;                          /* intentional reset: not a failed boot */
    fm1_enter_uboot();
}

/* SAFE MODE: ROM-1 crashed (or hung) twice while starting. Nothing that makes sound runs: no audio,
 * no engine, no UI; USB is on, so the web installer (ROM-1, or the stock firmware) and M-UPGRADE can
 * reach it. PLAY tries ROM-1 again. The chip's own update mode stays the last resort (fm1_cstart). */
static void safe_main(void)
{
    char b[24];
    uint32_t t0, play;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 18, 240, &FONT_B, "ROM-1 SAFE MODE", C_HI, 1);
    draw_text_box(0, 52, 240, &FONT_S, "IT CRASHED TWICE", C_WHITE, 1);
    draw_text_box(0, 72, 240, &FONT_S, "WHILE STARTING.", C_WHITE, 1);
    draw_text_box(0, 102, 240, &FONT_S, "NO SOUND. USB IS ON:", C_WHITE, 1);
    draw_text_box(0, 122, 240, &FONT_S, "REINSTALL FROM THE", C_WHITE, 1);
    draw_text_box(0, 142, 240, &FONT_S, "WEB INSTALLER.", C_WHITE, 1);
    draw_text_box(0, 172, 240, &FONT_S, "PLAY: TRY AGAIN", C_HI, 1);
    if (fm1_crash.magic == FM1_CRASH_MAGIC) {
        b[0] = 'P';
        b[1] = 'C';
        b[2] = ' ';
        hexs(b + 3, fm1_crash.pc);
        draw_text_box(0, 208, 240, &FONT_S, b, C_GRAY, 1);
    }
    fm1_input_init();
    panel_init();
    usb_start();
    fm1_timer5_start(isr_timer5, 1);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    play = 1u << panel.btn[B_PLAY];
    t0 = fm1_ms;
    for (;;) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (bootguard.pending && fm1_ms - t0 > 10000u)
            bootguard.pending = 0;                  /* safe mode itself is up: not another failed boot */
        if ((fm1_in.buttons & play) && fm1_ms - t0 > 500u) {
            bootguard.failed = 0;                   /* try ROM-1 again, from scratch */
            bootguard.pending = 0;
            draw_text_box(0, 172, 240, &FONT_S, "STARTING ROM-1...", C_HI, 1);
            usb_detach();
            fm1_delay_ms(30);
            fm1_reboot();
        }
        ota_service();
        if (usb.ota_req) {
            usb.ota_req = 0;
            if (flash_ok)
                ota_session();                      /* an install commits and resets; else back here */
        }
        if (usb.uboot_req)
            enter_uboot("UBOOT (USB)");
    }
}

static void splash(void)                            /* the name, on the module's LCD */
{
    int32_t w;
    lcd_fill(0, 0, 240, 240, RGB(30, 20, 12));
    cv_begin(232, 120, RGB(240, 166, 58));
    w = text_w(&FONT_L, "ROM-1");
    cv_text((232 - w) / 2, 24, &FONT_L, "ROM-1", RGB(50, 26, 6));
    w = text_w(&FONT_S, "Plays .sf2 sounds, 16 parts");
    cv_text((232 - w) / 2, 66, &FONT_S, "Plays .sf2 sounds, 16 parts", RGB(110, 62, 16));
    w = text_w(&FONT_XS, "v" EN_VERSION);
    cv_text((232 - w) / 2, 92, &FONT_XS, "v" EN_VERSION, RGB(110, 62, 16));
    cv_blit(4, 60);
    lcd_sync();
}

static void fm1_main(void)
{
    int32_t knob = 512 * 16;
    uint32_t last_frame = 0;
    {
        uint32_t f = irq_save();
        flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;   /* the expected 1 MiB part, else RAM only */
        irq_restore(f);
        if (flash_ok)
            ota_boot_cleanup();
    }
    fl_plain_window_init();                         /* region B (the data area) readable as it is, through XIP */
    lcd_init();
    palette_set(1);
    splash();
    if (en_dbg.magic != DBG_MAGIC) {
        memset(&en_dbg, 0, sizeof en_dbg);
        en_dbg.magic = DBG_MAGIC;
    }
    en_dbg.in_audio = 0;                            /* (kept through a reset: a crash in the audio interrupt left it
                                                     * set, and USB waits for it to clear; safe mode has no audio) */
    if (safe_mode)
        safe_main();                                /* never returns */
    if (fm1_crash.magic == FM1_CRASH_MAGIC && fm1_crash.count != en_dbg.crash_seen) {
        char b[16];                                 /* a crash since the last boot: say so, a moment */
        en_dbg.crash_seen = fm1_crash.count;
        b[0] = 'P';
        b[1] = 'C';
        b[2] = ' ';
        hexs(b + 3, fm1_crash.pc);
        draw_text_box(0, 186, 240, &FONT_S, "RESTARTED AFTER A CRASH", RGB(255, 80, 60), 1);
        draw_text_box(0, 206, 240, &FONT_S, b, C_GRAY, 1);
        fm1_wdt_feed();
        fm1_delay_ms(2500);
        fm1_wdt_feed();
    }
    en_dbg.boots++;
    en_dbg.max_us = 0;
    fm1_input_init();
    fm1_adc_init();
    panel_init();
    led_pos_init();
    r1_bank_attach(plat_bank_a(), plat_bank_b());  /* (a moment: both regions' CRCs) */
    settings_load();
    audio_init();
    usb_start();
#if EN_TRS
    uart_midi_init();                               /* before TIMER5: it read-modify-writes port H too */
#endif
    /* above ALNK0 (3): the key / encoder / LED matrix is scanned one column per tick and must keep
     * its rhythm through a render (X0X's finding) */
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(30);
    if ((fm1_in.buttons & 3u) == 3u)
        panel_setup();                              /* OCT- + OCT+ held at power-on */
    fm1_delay_ms(400);
    ui_init();

    for (;;) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {     /* a crash or hang in the first 30 s counts */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        {
            int32_t a = fm1_adc_read(FM1_ADC_MASTER);
            if (a >= 0) {
                uint32_t k10;
                knob += (a * 16 - knob) / 8;
                k10 = (uint32_t)(knob / 16);
                master_q12 = (k10 * k10) >> 8;           /* 0 .. ~4096 */
            }
        }
        {   /* OCT- + OCT+ held 5 s: update mode (Felucca's, unchanged) */
            static uint32_t t0, shown;
            uint32_t both = (1u << panel.btn[B_OCTDN]) | (1u << panel.btn[B_OCTUP]);
            if ((fm1_in.buttons & both) != both) {
                if (shown)
                    ui_say("UPDATE MODE", "CANCELLED");
                shown = 0;
                t0 = fm1_ms;
            } else if (fm1_ms - t0 > 2000u && fm1_ms - t0 <= 5000u) {
                uint32_t left = (5000u - (fm1_ms - t0) + 999u) / 1000u;
                if (left != shown) {
                    char d[4] = {(char)('0' + left), '.', '.', 0};
                    ui_say("UPDATE MODE IN", d);
                    shown = left;
                }
            } else if (fm1_ms - t0 > 5000u) {
                r1_panic();
                fm1_delay_ms(10);
                enter_uboot("UBOOT");
            }
        }
        dev_sysex();                                /* ROM-1's own (and MIDI's resets) */
        ota_service();                              /* M-UPGRADE handshake (other SysEx is dropped) */
        if (usb.ota_req) {
            usb.ota_req = 0;
            r1_panic();
            fm1_delay_ms(10);
            if (settings_is_dirty())
                settings_save();
            if (flash_ok)
                ota_session();                      /* returns only if nothing was committed */
            ui_init();
        }
        if (usb.uboot_req) {
            r1_panic();
            fm1_delay_ms(10);
            enter_uboot("UBOOT (USB)");
        }
#if EN_CDC
        cdc_task();
#endif
        settings_autosave(!r1_voices && !uploading);
        if (fm1_ms - last_frame >= 16u && (int32_t)(fm1_ms - ui_pause_until) >= 0) {   /* ~60 frames/s at most */
            uint32_t t0 = fm1_ticks(), us;
            last_frame = fm1_ms;
            en_dbg.ui_frames++;
            ui_frame();
            us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
            ui_us += us;
            if (us > ui_us_max)
                ui_us_max = us;
        } else {
            ui_input_only();
        }
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 4u) {                   /* safe mode failed too: the chip's own update mode */
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    p3 |= (bootguard.failed >= 2u) << 8;            /* two failed boots: safe mode (kept past the .bss clear) */
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    safe_mode = (uint8_t)(p3 >> 8);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
