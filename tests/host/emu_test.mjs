// SPDX-License-Identifier: GPL-3.0-only
// The browser build in Node, as the page and its worklet drive it: the starter sounds given, a key played, MIDI
// in played, KEYS OUT on the way out, the screen drawn, the settings saved (the flash the page keeps), and a
// bank of the converter's (two fonts, region B in the flash) put in its place.
//   node tests/host/emu_test.mjs build/emu [TESTDIR]
import fs from "fs";
const dir = process.argv[2], T = process.argv[3] || "build/test";
const wasm = fs.readFileSync(`${dir}/rom1.wasm`);
let mem, failed = 0;
const ok = (c, what) => { console.log(`  ${c ? "ok  " : "FAIL"} ${what}`); if (!c) failed++; };
const wasi = new Proxy({ clock_time_get: (id, p, out) => { new DataView(mem.buffer).setBigUint64(out, 0n, true); return 0; },
  fd_write: (fd, iov, n, out) => { new DataView(mem.buffer).setUint32(out, 0, true); return 0; } }, { get: (t, k) => k in t ? t[k] : () => 0 });
const { instance } = await WebAssembly.instantiate(wasm, { wasi_snapshot_preview1: wasi, env: new Proxy({}, { get: () => () => 0 }) });
const ex = instance.exports; mem = ex.memory;
if (ex._initialize) ex._initialize();
ex.web_flash_erased();
ex.web_master(2800);
function bank(a, b = new Uint8Array(0)) {                // as worklet.js does it
  const pa = ex.web_alloc(a.length);
  new Uint8Array(mem.buffer, pa, a.length).set(a);
  const pb = ex.web_alloc(b.length || 1);
  new Uint8Array(mem.buffer, pb, b.length).set(b);
  return ex.web_bank(pa, a.length, pb, b.length);
}
let peak = 0, outs = [];
function run(blocks, at = () => {}) {
  peak = 0;
  for (let i = 0; i < blocks; i++) {
    at(i);
    ex.web_render(128);
    for (const v of new Float32Array(mem.buffer, ex.web_out_l(), 128)) peak = Math.max(peak, Math.abs(v));
    const n = ex.web_midi_take(), b = new Uint32Array(mem.buffer, ex.web_midi_buf(), 2 * n);
    for (let k = 0; k < n; k++) outs.push(b[2 * k + 1]);
  }
}
const starter = new Uint8Array(fs.readFileSync(`${dir}/starter.bin`));
ok(bank(starter) === 19, "the starter sounds: 19 presets");
run(100);
const blits0 = ex.web_blits();
ok(blits0 >= 3 && peak === 0, `switched on: the screen drawn (its three bands: ${blits0} blits), silent`);
run(200, (i) => ex.web_keys(i < 100 ? 1 : 0));          // the first key, held 0.3 s
ok(peak > 0.02 && peak < 1, `a key: the piano sounds (peak ${peak.toFixed(3)})`);
run(100, (i) => { if (i === 0) ex.web_midi_in((0x9 | 0x99 << 8 | 36 << 16 | 110 << 24) >>> 0); });
ok(peak > 0.02, `MIDI in, channel 10: the kit (peak ${peak.toFixed(3)})`);
// KEYS OUT on: SETUP (GLO held long), knob 3 up; then a key goes out to MIDI
const GLO = 5, K3 = 5;
run(400, (i) => ex.web_buttons(i < 330 ? 1 << GLO : 0));
run(60, (i) => { if (i === 10) ex.web_enc(K3, 1); });
outs = [];
run(100, (i) => ex.web_keys(i < 50 ? 1 << 4 : 0));
ok(outs.some((w) => ((w >>> 8) & 0xF0) === 0x90 && ((w >>> 16) & 127) === 57) && outs.some((w) => ((w >>> 8) & 0xF0) === 0x80),
   `KEYS OUT: the key's note on and off to MIDI (${outs.length} messages)`);
const w0 = ex.web_flash_writes();
run(2500);                                              // (the settings save themselves a few seconds after a change)
ok(ex.web_flash_writes() > w0 && ex.web_flash_len() === 0x24000, `the settings saved: ${ex.web_flash_writes() - w0} writes, the page keeps ${ex.web_flash_len()} B`);
const a = new Uint8Array(fs.readFileSync(`${T}/two/bank_a.bin`)), b = new Uint8Array(fs.readFileSync(`${T}/two/bank_b.bin`));
ok(bank(a, b) === 26 && ex.web_bank_ok_b() === 1, "another bank: two fonts, 26 presets, region B there and right");
run(200, (i) => { if (i === 0) ex.web_midi_in((0x9 | 0x90 << 8 | 69 << 16 | 100 << 24) >>> 0); });
ok(peak > 0.02, `... it plays (peak ${peak.toFixed(3)})`);
process.exit(failed ? 1 : 0);
