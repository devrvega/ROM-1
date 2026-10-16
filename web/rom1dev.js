// SPDX-License-Identifier: GPL-3.0-only
//
// ROM-1's own SysEx (F0 7D 52 31 cmd .. F7: firmware/src/app/main_fm1.c) from the browser: its status, and
// region B of a bank written to the FM-1's data area. The same as tools/rom1_load.py. It talks over a link
// as fm1ota.js's Updater.find() gives one ({send(bytes), read(ms), drain()}).
//
//   await rom1dev.status(link)                         {cpu, voices, bankId, lenA, lenB, bOk, ...} or null
//   await rom1dev.sendB(link, bytes, bankId, progress) true when the FM-1 checked it and plays it
//
// (One object, so that the pages can inline it next to fm1ota.js, whose pack7 is another.)

export const rom1dev = (() => {
  const HDR = [0x7D, 0x52, 0x31];
  const PAGE = 256;
  const put35 = (v) => [0, 1, 2, 3, 4].map((k) => Math.floor(v / 2 ** (7 * k)) & 0x7F);
  const get35 = (d, i = 0) => [0, 1, 2, 3, 4].reduce((s, k) => s + (d[i + k] & 0x7F) * 2 ** (7 * k), 0) >>> 0;
  // 7-bit packing: each 7 bytes after a byte of their high bits
  function pack7(data) {
    const out = [];
    for (let i = 0; i < data.length; i += 7) {
      const g = data.subarray(i, i + 7);
      let hi = 0;
      g.forEach((b, j) => { hi |= (b >> 7) << j; });
      out.push(hi);
      g.forEach((b) => out.push(b & 0x7F));
    }
    return out;
  }
  async function ask(link, cmd, args = [], ms = 3000) {
    link.drain();
    if (!link.send([0xF0, ...HDR, cmd, ...args, 0xF7])) return null;
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const p = await link.read(Math.max(1, end - Date.now()));
      if (!p) break;
      if (p[1] === HDR[0] && p[2] === HDR[1] && p[3] === HDR[2] && p[4] === cmd) return p.slice(5, -1);
    }
    return null;
  }
  const KEYS = ["cpu", "worstUs", "late", "renders", "bench", "voices", "notes", "frames", "uiUs", "uiMax", "midiIn",
                "bankId", "lenA", "lenB", "bOk", "flashOk", "writeRc", "boots", "voiceCap"];
  async function status(link) {
    let d = null;
    for (let i = 0; i < 3 && !d; i++) d = await ask(link, 1, [], 1000);   // (the first message after opening can go astray)
    if (!d) return null;
    const s = {};
    KEYS.forEach((k, i) => { if (5 * i + 5 <= d.length) s[k] = get35(d, 5 * i); });
    s.bOk = !!s.bOk;
    return s;
  }
  async function sendB(link, data, bankId, progress = () => {}) {
    const r = await ask(link, 0x20, [...put35(data.length), ...put35(bankId)]);
    if (!r || get35(r) !== 0) throw new Error("the FM-1 did not begin region B (is it running ROM-1?)");
    for (let off = 0; off < data.length; off += PAGE) {
      const msg = [...put35(off), ...pack7(data.subarray(off, off + PAGE))];
      let done = false;
      for (let t = 0; t < 3 && !done; t++) {
        const a = await ask(link, 0x21, msg, 2000);
        done = !!a && get35(a) === off;
      }
      if (!done) throw new Error(`region B stopped at byte ${off}`);
      progress(Math.min(1, (off + PAGE) / data.length));
    }
    const e = await ask(link, 0x22, [], 10000);
    return !!e && get35(e) === 1;
  }
  return { status, sendB, pack7, put35, get35 };
})();
