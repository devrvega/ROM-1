// SPDX-License-Identifier: GPL-3.0-only
// The converter in WebAssembly (web/rom1conv.js + build/web/rom1conv.wasm) against the command line's
// (build/gen/rom1conv): the same fonts, rooms and presets give the same bank, byte for byte. The fonts are
// read as the page reads them, a piece at a time. run_tests.sh makes the native banks first.
//   node tests/host/conv_wasm_test.mjs WASM TESTDIR STARTER.sf2
import { openSync, readSync, fstatSync, readFileSync } from "node:fs";
import { basename } from "node:path";
import { rom1Converter } from "../../web/rom1conv.js";

const [wasmPath, T, starter] = process.argv.slice(2);
const wasm = readFileSync(wasmPath);
let failed = 0, reads = 0;
const ok = (c, what) => { console.log(`  ${c ? "ok  " : "FAIL"} ${what}`); if (!c) failed++; };
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

function source(path) {                      // a font as the page sees one: its name, its size, reads
  const fd = openSync(path), size = fstatSync(fd).size;
  return { name: basename(path), size, read: async (off, len) => {
    const b = Buffer.alloc(len);
    reads++;
    return new Uint8Array(b.buffer, b.byteOffset, readSync(fd, b, 0, len, off));
  } };
}

// [the native bank's folder, its room A, room B, name, fonts (path, or [path, "bank:prog", ...])]
const cases = [
  ["tones", 400000, 0, "ROM-1 bank", [`${T}/tones.sf2`]],
  ["tones4", 20000, 0, "ROM-1 bank", [`${T}/tones.sf2`]],
  ["tonesl", 9000, 0, "ROM-1 bank", [`${T}/tones.sf2`]],
  ["two", 300000, 282624, "Two fonts", [`${T}/tones.sf2`, starter]],
  ["pick", 200000, 0, "Picked", [[`${T}/tones.sf2`, "0:2", "128:0"], [starter, "0:0", "0:48"]]],
];
const cv = await rom1Converter(wasm);
for (const [dir, ra, rb, name, fonts] of cases) {
  cv.reset();
  reads = 0;
  for (const spec of fonts) {
    const [path, ...pick] = Array.isArray(spec) ? spec : [spec];
    const f = await cv.addFont(source(path));
    if (pick.length)
      cv.presets(f).forEach((p, i) => cv.select(f, i, pick.includes(`${p.bank}:${p.prog}`)));
  }
  const plan = cv.plan(ra, rb);
  if (!plan.ok) { ok(false, `${dir}: the plan: ${plan.error}`); continue; }
  const t0 = performance.now(), out = await cv.build(name), ms = performance.now() - t0;
  const a = new Uint8Array(readFileSync(`${T}/${dir}/bank_a.bin`)), b = new Uint8Array(readFileSync(`${T}/${dir}/bank_b.bin`));
  ok(same(out.a, a) && same(out.b, b),
     `${dir}: region A ${out.a.length} B, region B ${out.b.length} B, as rom1conv's (${out.report.presets} presets, ${reads} reads, ${ms.toFixed(0)} ms)`);
}
cv.reset();
await cv.addFont(source(starter));
const p = cv.plan(40000, 0);
ok(!p.ok && p.over > 0 && /room|big|fit/i.test(p.error), `too big: refused, ${p.over} B over ("${p.error}")`);
let threw = "";
try { await cv.addFont({ name: "x.sf2", size: 12, read: async () => new TextEncoder().encode("RIFF\0\0\0\0WAVE") }); } catch (e) { threw = e.message; }
ok(/\.sf2 file/.test(threw), `not a font: refused ("${threw}")`);
process.exit(failed ? 1 : 0);
