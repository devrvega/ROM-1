#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/rom1pkg.py against the build's package: the bank the build put in, patched in again, gives the
package byte for byte; another bank goes in with the code untouched and is found again (run_tests.sh then
has the update's own check, tests/ota_test.c, read the patched package as the FM-1 would). With node, the
upload page's web/rom1pkg.js patches the same bytes. Needs ./build.sh (build/rom1.fwsc) and the converter.
  python3 tests/pkg_test.py PKG.fwsc STARTER.sf2 OTHER.sf2 OUTDIR"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import rom1pkg  # noqa: E402

failed = 0


def ok(cond, what):
    global failed
    print(f"  {'ok  ' if cond else 'FAIL'} {what}")
    failed += not cond


def convert(fonts, room, name, out):
    out.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(ROOT / "build/gen/rom1conv"), "-a", str(room), "-b", "0", "-n", name, "-o", str(out),
                        *map(str, fonts)], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stdout + r.stderr)
    return (out / "bank_a.bin").read_bytes()


def code_of(pkg):
    log, _ = rom1pkg.logical(pkg)
    _, _, _, flash = rom1pkg.open_area(log)
    region, _ = rom1pkg.decipher_area(flash)
    app = region[rom1pkg.APP:rom1pkg.APP + rom1pkg.APP_SLOT]
    return app[:rom1pkg.bank_offset(app)]


def main():
    if len(sys.argv) < 5:
        raise SystemExit(__doc__)
    pkg_path, starter, other, out = Path(sys.argv[1]), sys.argv[2], sys.argv[3], Path(sys.argv[4])
    pkg = pkg_path.read_bytes()
    info = rom1pkg.info(pkg_path)
    ok(info["product"].startswith("FM-1_0"), f"the package's identity: {info['product']}")
    ok(info["bank_off"] % 0x1000 == 0 and info["room"] > 400000, f"the bank at {info['bank_off']:#x}, {info['room']} B of room")
    same = rom1pkg.patch(pkg, convert([starter], info["room"], "ROM-1 Starter", out / "starter"))
    ok(same == pkg, "the build's own bank patched in again: the package byte for byte")
    bank = convert([other, starter], info["room"], "Two fonts", out / "other")
    new = rom1pkg.patch(pkg, bank)
    (out / "patched.fwsc").write_bytes(new)
    ok(len(new) == len(pkg), "another bank: the package the same size")
    ok(code_of(new) == code_of(pkg), "... the code untouched")
    got = rom1pkg.info(out / "patched.fwsc")
    want_id = int.from_bytes(bank[16:20], "little")
    ok(got["bank_id"] == want_id and got["fonts"] == 2 and got["bank_len"] == len(bank),
       f"... found again: id {got['bank_id']:#x}, {got['fonts']} fonts, {got['presets']} presets")
    try:
        rom1pkg.patch(pkg, bytes(info["room"] + 1))
        ok(False, "a bank too big is refused")
    except SystemExit:
        ok(True, "a bank too big is refused")
    node = shutil.which("node")
    js = ROOT / "web/rom1pkg.js"
    if node and js.exists():
        r = subprocess.run([node, str(ROOT / "tests/host/rom1pkg_test.mjs"), str(pkg_path), str(out / "other/bank_a.bin"),
                            str(out / "patched.fwsc")], capture_output=True, text=True)
        sys.stdout.write(r.stdout + r.stderr)
        ok(r.returncode == 0, "web/rom1pkg.js patches the same bytes")
    else:
        print("  skip web/rom1pkg.js (needs node)")
    print("package test " + ("FAILED" if failed else "passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
