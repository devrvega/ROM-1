// SPDX-License-Identifier: GPL-3.0-only
//
// A ROM-1 package (.fwsc) with another bank in it, in the browser: region A replaced in the firmware image,
// every checksum and the chip's cipher redone, nothing else touched. The same as tools/rom1pkg.py, byte for
// byte (tests/host/rom1pkg_test.mjs checks it). The package format is Felucca's (tools/fm1pkg_make.py).
//
//   rom1pkg.info(pkg)          -> {product, bankOff, room, bankLen, bankId, fonts, presets, lenB}
//   rom1pkg.patch(pkg, bankA)  -> the new package (Uint8Array)
//
// (One object, so that the pages can inline it next to fm1pkg.js and fm1ota.js.)

export const rom1pkg = (() => {
  const BLOCKS = 20, BLK = 0x30, KEEP = 0x2F;
  const APP_SLOT = 0x8DFBC, KEY = 0x980F;
  const AREA = 0x4000, APP = 0x120;                 // the app area in flash.bin; app.bin in the area
  const MAGIC = [0x52, 0x31, 0x42, 0x4B];           // "R1BK"

  const T = new Uint16Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i << 8;
    for (let k = 0; k < 8; k++) c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
    T[i] = c & 0xFFFF;
  }
  function crc16(d) {
    let c = 0;
    for (let i = 0; i < d.length; i++) c = ((c << 8) & 0xFFFF) ^ T[((c >> 8) ^ d[i]) & 0xFF];
    return c;
  }
  // JieLi's ENC stream cipher, in place
  function enc(b, off, size, key = 0xFFFF) {
    for (let i = 0; i < size; i++) {
      b[off + i] ^= key & 0xFF;
      key = ((key << 1) ^ (key & 0x8000 ? 0x1021 : 0)) & 0xFFFF;
    }
  }
  // the SFC cipher: ENC a 32-byte block at a time, the key mixed with the block's address
  function sfc(b, off, size, base, key) {
    for (let i = 0; i < size; i += 32) enc(b, off + i, Math.min(size - i, 32), key ^ ((off + i - base) >> 2));
  }
  const u16 = (b, o) => b[o] | b[o + 1] << 8;
  const u32 = (b, o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;
  const put16 = (b, o, v) => { b[o] = v & 0xFF; b[o + 1] = (v >> 8) & 0xFF; };

  // the package without its 20 marker bytes, and the identity they spell
  function logical(raw) {
    if (raw.length < BLOCKS * BLK) throw new Error("not an FM-1 package (too short)");
    const out = new Uint8Array(raw.length - BLOCKS);
    let prod = "";
    for (let i = 0; i < BLOCKS; i++) {
      out.set(raw.subarray(i * BLK, i * BLK + KEEP), i * KEEP);
      const m = raw[i * BLK + KEEP];
      if (m !== 0x7D) prod += String.fromCharCode((m - i - 1) & 0xFF);
    }
    out.set(raw.subarray(BLOCKS * BLK), BLOCKS * KEEP);
    return { log: out, prod };
  }
  function withMarkers(log, prod) {
    const raw = new Uint8Array(log.length + BLOCKS);
    for (let i = 0; i < BLOCKS; i++) {
      raw.set(log.subarray(i * KEEP, (i + 1) * KEEP), i * BLK);
      raw[i * BLK + KEEP] = i < prod.length ? (prod.charCodeAt(i) + i + 1) & 0xFF : 0x7D;
    }
    raw.set(log.subarray(BLOCKS * KEEP), BLOCKS * BLK);
    return raw;
  }
  // the ufw's header (deciphered) and file list: [index, type, offset, length]
  function files(log) {
    const hdr = log.slice(0, 0x40);
    enc(hdr, 0, 0x40);
    const n = u16(hdr, 8), list = [];
    for (let i = 0; i < n; i++) {
      const e = log.slice(0x40 + 0x50 * i, 0x40 + 0x50 * (i + 1));
      enc(e, 0, 0x50);
      list.push([i, u16(e, 0), u32(e, 8), u32(e, 12)]);
    }
    return { hdr, list, n };
  }
  // flash.bin's app area, first block (head entry, entries, app.bin, ...), deciphered
  function openArea(log) {
    const f = files(log), fl = f.list.find((x) => x[1] === 0);
    if (!fl) throw new Error("no flash.bin in this package");
    const flash = log.slice(fl[2], fl[2] + fl[3]);
    const region = flash.slice(AREA, AREA + APP + APP_SLOT + 0x10000);
    sfc(region, 0, region.length, 0, KEY);
    return { ...f, fl, flash, region, blk: u32(region, 8) };
  }
  function bankOffset(region) {
    for (let off = 0x1000; off < APP_SLOT; off += 0x1000) {
      const at = APP + off;
      if (MAGIC.every((m, k) => region[at + k] === m)) return off;
    }
    throw new Error("no ROM-1 bank in this package (is it a ROM-1 package?)");
  }
  function setEntryDcrc(area, at, dcrc) {
    put16(area, at + 2, dcrc);
    put16(area, at, crc16(area.subarray(at + 2, at + 32)));
  }

  function info(pkg) {
    const { log, prod } = logical(pkg), a = openArea(log), off = bankOffset(a.region), b = APP + off;
    return { product: prod, bankOff: off, room: APP_SLOT - off, bankLen: u32(a.region, b + 8), bankId: u32(a.region, b + 16),
             fonts: u16(a.region, b + 6), presets: u32(a.region, b + 28), lenB: u32(a.region, b + 20) };
  }

  function patch(pkg, bankA) {
    const { log, prod } = logical(pkg), a = openArea(log), off = bankOffset(a.region), region = a.region;
    if (bankA.length > APP_SLOT - off) throw new Error(`the bank is ${bankA.length} B, the firmware has room for ${APP_SLOT - off} B`);
    region.set(bankA, APP + off);
    region.fill(0xFF, APP + off + bankA.length, APP + APP_SLOT);
    setEntryDcrc(region, 32, crc16(region.subarray(APP, APP + APP_SLOT)));   // the app.bin entry: its data's CRC
    setEntryDcrc(region, 0, crc16(region.subarray(32, a.blk)));              // the area head: the block's CRC
    sfc(region, 0, region.length, 0, KEY);
    a.flash.set(region, AREA);
    log.set(a.flash, a.fl[2]);
    // the ufw: flash.bin's CRC in its entry, the list's CRC, the header's
    const at = 0x40 + 0x50 * a.fl[0], e = log.slice(at, at + 0x50);
    enc(e, 0, 0x50);
    put16(e, 4, crc16(a.flash));
    enc(e, 0, 0x50);
    log.set(e, at);
    const hdr = a.hdr;
    put16(hdr, 2, crc16(log.subarray(0x40, 0x40 + 0x50 * a.n)));
    put16(hdr, 0, crc16(hdr.subarray(2, 0x40)));
    enc(hdr, 0, 0x40);
    log.set(hdr, 0);
    return withMarkers(log, prod);
  }

  return { info, patch };
})();
