/* SPDX-License-Identifier: GPL-3.0-only */
/* ROM-1: the bank's region B written over USB (the upload page's second step; main_fm1.c's SysEx 20-22),
 * a page at a time, each sector erased as its first byte comes; while it is written the engine leaves region
 * B out and the audio is silent, then both regions are checked and played. Part of the unity build and of
 * the host simulator. */

static uint32_t up_len, up_id, up_next, up_last;
static volatile uint8_t uploading;                  /* region B being written: no sound */

static void bank_attach(void)
{
    r1_lock(1);
    r1_bank_attach(plat_bank_a(), uploading ? 0 : plat_bank_b());
    r1_lock(0);
}

int up_begin(uint32_t len, uint32_t id)             /* 0 ok */
{
    if (len > R1_B_ROOM)
        return -1;
    up_len = len;
    up_id = id;
    up_next = 0;
    up_last = 0xFFFFFFFFu;
    uploading = 1;
    r1_panic();
    bank_attach();
    return 0;
}
int up_data(uint32_t off, const uint8_t *b, uint32_t m)   /* 0 ok: in order, within a page */
{
    int rc;
    if (uploading && off == up_last && off + m == up_next)   /* the last page again (its answer went astray) */
        return memcmp(plat_bank_b() + off, b, m) ? -1 : 0;
    if (!uploading || off != up_next || off + m > R1_B_ROOM || (off & 0xFFu) + m > 256u)
        return -1;
    if (!(off & 0xFFFu) && (rc = plat_bank_b_erase(off)) != 0)
        return rc;
    if ((rc = plat_bank_b_prog(off, b, m)) != 0)
        return rc;
    up_last = off;
    up_next = off + m;
    return 0;
}
int up_end(void)                                    /* 1: region B there and right */
{
    uploading = 0;
    bank_attach();
    ui_bank_changed();
    return r1_bank_ok_b();
}
