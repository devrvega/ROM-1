// SPDX-License-Identifier: GPL-3.0-only
// web/rom1dev.js against a simulated FM-1: region B's SysEx (20 begin, 21 pages, 22 end) decoded as
// firmware/src/app/main_fm1.c decodes it (dev_get, the 7-bit groups, up_data's rules: in order, within a
// page), with a lost answer on the way; the status's values; a refusal.
//   node tests/host/rom1dev_test.mjs
import { rom1dev } from "../../web/rom1dev.js";

let failed = 0;
const ok = (c, what) => { console.log(`  ${c ? "ok  " : "FAIL"} ${what}`); if (!c) failed++; };
const get35 = (p, i) => [0, 1, 2, 3, 4].reduce((s, k) => s + p[i + k] * 2 ** (7 * k), 0) >>> 0;

function fakeFm1({ dropAt = -1, refuse = false } = {}) {
  const flash = new Uint8Array(0x45000).fill(0xFF), q = [];
  let len = 0, next = 0, last = -1, uploading = false, dropped = false, bankId = 0x1234ABCD;
  const reply = (cmd, vals) => q.push([0xF0, 0x7D, 0x52, 0x31, cmd, ...vals.flatMap(rom1dev.put35), 0xF7]);
  return {
    flash,
    drain() { q.length = 0; },
    async read() { return q.shift() || null; },
    send(m) {
      const p = m.slice(1, -1), n = p.length;
      if (p[0] !== 0x7D || p[1] !== 0x52 || p[2] !== 0x31) return true;
      if (p[3] === 1) reply(1, [12, 900, 0, 5000, 0, 3, 40, 60, 100, 9000, 7, bankId, 479000, len, uploading ? 0 : 1, 1, 100, 2, 28]);
      if (p[3] === 0x20) {
        len = get35(p, 4); next = 0; uploading = !refuse;
        reply(0x20, [refuse ? 1 : 0]);
      }
      if (p[3] === 0x21) {
        const off = get35(p, 4), buf = [];
        for (let i = 9; i < n && buf.length < 256;) {
          const hi = p[i++];
          for (let j = 0; j < 7 && i < n && buf.length < 256; j++) buf.push(p[i++] | ((hi >> j & 1) << 7));
        }
        const again = uploading && off === last && off + buf.length === next &&
          buf.every((b, i) => flash[off + i] === b);                       // the last page again: written already
        const good = again || (uploading && off === next && (off & 0xFF) + buf.length <= 256);
        if (good && !again) { flash.set(buf, off); last = off; next = off + buf.length; }
        if (off === dropAt && !dropped) { dropped = true; return true; }   // the answer lost (the page went in)
        reply(0x21, [good ? off : 0x7FFFFFF]);
      }
      if (p[3] === 0x22) { uploading = false; reply(0x22, [next === len ? 1 : 0]); }
      return true;
    },
  };
}

const data = new Uint8Array(70000).map((_, i) => (i * 37 + (i >> 8)) & 0xFF);
const fm = fakeFm1({ dropAt: 256 * 40 });
const st = await rom1dev.status(fm);
ok(st && st.bankId === 0x1234ABCD && st.voiceCap === 28 && st.bOk === true && st.cpu === 12, "status: the values in order");
let last = 0;
const okB = await rom1dev.sendB(fm, data, 0x1234ABCD, (f) => { last = f; });
ok(okB && last === 1 && fm.flash.subarray(0, data.length).every((b, i) => b === data[i]),
   `region B: ${data.length} B written and checked (an answer lost on the way: the page sent again, taken as written)`);
let msg = "";
try { await rom1dev.sendB(fakeFm1({ refuse: true }), data, 1); } catch (e) { msg = e.message; }
ok(/did not begin/.test(msg), `a refusal: "${msg}"`);
const pk = rom1dev.pack7(new Uint8Array([0x80, 1, 0xFF, 3, 4, 5, 6, 0x87, 9]));
ok(JSON.stringify(pk) === JSON.stringify([0x05, 0, 1, 0x7F, 3, 4, 5, 6, 0x01, 7, 9]), "pack7: a byte of high bits, then 7 bytes");
process.exit(failed ? 1 : 0);
