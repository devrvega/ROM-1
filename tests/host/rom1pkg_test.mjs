// SPDX-License-Identifier: GPL-3.0-only
// web/rom1pkg.js against tools/rom1pkg.py (tests/pkg_test.py runs this): the same package, the same bank,
// the same bytes out; and info() reads the bank back.
//   node tests/host/rom1pkg_test.mjs PKG.fwsc BANK_A.bin EXPECTED.fwsc
import { readFileSync } from "node:fs";
import { rom1pkg } from "../../web/rom1pkg.js";

const [pkgPath, bankPath, wantPath] = process.argv.slice(2);
const pkg = new Uint8Array(readFileSync(pkgPath)), bank = new Uint8Array(readFileSync(bankPath));
const want = new Uint8Array(readFileSync(wantPath));
let failed = 0;
const ok = (c, what) => { console.log(`  ${c ? "ok  " : "FAIL"} ${what}`); if (!c) failed++; };

const before = rom1pkg.info(pkg);
ok(before.room > 400000 && before.bankOff % 0x1000 === 0, `info: ${before.product}, the bank at ${before.bankOff.toString(16)}, ${before.room} B of room`);
const t0 = performance.now(), got = rom1pkg.patch(pkg, bank), ms = performance.now() - t0;
ok(got.length === want.length && got.every((b, i) => b === want[i]), `patch: the same ${got.length} B as rom1pkg.py (${ms.toFixed(0)} ms)`);
const after = rom1pkg.info(got);
ok(after.bankId === (bank[16] | bank[17] << 8 | bank[18] << 16 | bank[19] << 24) >>> 0 && after.bankLen === bank.length,
   `info after: id ${after.bankId.toString(16)}, ${after.fonts} fonts, ${after.presets} presets`);
ok(rom1pkg.patch(got, readFileSync(bankPath)).length === got.length, "patch takes a Buffer too");
let threw = false;
try { rom1pkg.patch(pkg, new Uint8Array(before.room + 1)); } catch { threw = true; }
ok(threw, "a bank too big is refused");
process.exit(failed ? 1 : 0);
