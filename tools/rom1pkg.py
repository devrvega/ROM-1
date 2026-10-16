#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A ROM-1 package (.fwsc) with another bank in it: region A replaced in the firmware image, every checksum
and the chip's cipher redone, nothing else touched (the upload page does the same in the browser:
web/rom1pkg.js). The package format is Felucca's (tools/fm1pkg_make.py).

  rom1pkg.py info PKG.fwsc
  rom1pkg.py patch PKG.fwsc BANK_A.bin OUT.fwsc

The bank sits from a 4 KiB boundary of app.bin (after the code) to the end of the app slot; it is found
by its magic ("R1BK") at those boundaries."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm1pkg_make import APP_SLOT, KEY, crc16, enc, sfc  # noqa: E402

BLOCKS, BLK, KEEP = 20, 0x30, 0x2F
AREA = 0x4000                       # the app area in flash.bin
APP = 0x120                         # app.bin in the area
R1_MAGIC = b"R1BK"


def logical(raw):
    """the package without its 20 marker bytes, and the identity they spell"""
    out, prod = bytearray(), ""
    for i in range(BLOCKS):
        out += raw[i * BLK:i * BLK + KEEP]
        m = raw[i * BLK + KEEP]
        if m != 0x7D:
            prod += chr((m - i - 1) & 0xFF)
    out += raw[BLOCKS * BLK:]
    return out, prod


def with_markers(log, prod):
    raw = bytearray()
    for i in range(BLOCKS):
        ch = prod[i] if i < len(prod) else None
        raw += log[i * KEEP:(i + 1) * KEEP] + bytes([(ord(ch) + i + 1) & 0xFF if ch else 0x7D])
    raw += log[BLOCKS * KEEP:]
    return bytes(raw)


def files(log):
    """the ufw's file entries: (index into the list, offset, length)"""
    hdr = bytearray(log[0:0x40])
    enc(hdr, 0, 0x40)
    n = struct.unpack_from("<H", hdr, 8)[0]
    out = []
    for i in range(n):
        e = bytearray(log[0x40 + 0x50 * i:0x40 + 0x50 * (i + 1)])
        enc(e, 0, 0x50)
        typ, idx, crc, _, off, ln, _ = struct.unpack_from("<HHHHIII", e, 0)
        out.append((i, typ, off, ln))
    return hdr, out


def entry_fields(area, at):
    """an area entry's body: dcrc, offset, size"""
    return struct.unpack_from("<HII", area, at + 2)


def set_entry_dcrc(area, at, dcrc):
    struct.pack_into("<H", area, at + 2, dcrc)
    struct.pack_into("<H", area, at, crc16(area[at + 2:at + 32]))


def open_area(log):
    hdr, fl = files(log)
    i, typ, off, ln = next(f for f in fl if f[1] == 0)          # flash.bin
    flash = bytearray(log[off:off + ln])
    return hdr, fl, (i, off, ln), flash


def decipher_area(flash):
    """the app area's first block (head entry, entries, app.bin, cfg_tool), deciphered"""
    region = bytearray(flash[AREA:AREA + APP + APP_SLOT + 0x10000])
    sfc(region, 0, len(region), 0, KEY)
    _, _, blk = struct.unpack_from("<HII", region, 2)
    return region, blk


def bank_offset(app):
    for off in range(0x1000, APP_SLOT, 0x1000):
        if app[off:off + 4] == R1_MAGIC:
            return off
    raise SystemExit("rom1pkg: no ROM-1 bank in this package")


def info(path):
    raw = Path(path).read_bytes()
    log, prod = logical(raw)
    _, _, _, flash = open_area(log)
    region, _ = decipher_area(flash)
    app = region[APP:APP + APP_SLOT]
    off = bank_offset(app)
    magic, ver, nfont, ln, crc, bid, lenb, crcb, npre = struct.unpack_from("<4sHHIIIIII", app, off)
    return {"product": prod, "bank_off": off, "room": APP_SLOT - off, "bank_len": ln, "bank_id": bid,
            "fonts": nfont, "presets": npre, "lenb": lenb}


def patch(pkg, bank_a):
    log, prod = logical(pkg)
    log = bytearray(log)
    hdr, fl, (fi, foff, fln), flash = open_area(log)
    region, blk = decipher_area(flash)
    app = region[APP:APP + APP_SLOT]
    off = bank_offset(app)
    if len(bank_a) > APP_SLOT - off:
        raise SystemExit(f"rom1pkg: the bank is {len(bank_a)} B, the firmware has room for {APP_SLOT - off} B")
    new_app = bytearray(app[:off]) + bytearray(bank_a) + b"\xff" * (APP_SLOT - off - len(bank_a))
    region[APP:APP + APP_SLOT] = new_app
    set_entry_dcrc(region, 32, crc16(new_app))                 # the app.bin entry: its data's CRC
    set_entry_dcrc(region, 0, crc16(region[32:blk]))           # the area head: the block's CRC
    sfc(region, 0, len(region), 0, KEY)
    flash[AREA:AREA + len(region)] = region
    log[foff:foff + fln] = flash
    # the ufw: flash.bin's CRC in its entry, the list's CRC, the header's
    e = bytearray(log[0x40 + 0x50 * fi:0x40 + 0x50 * (fi + 1)])
    enc(e, 0, 0x50)
    struct.pack_into("<H", e, 4, crc16(flash))
    enc(e, 0, 0x50)
    log[0x40 + 0x50 * fi:0x40 + 0x50 * (fi + 1)] = e
    n = struct.unpack_from("<H", hdr, 8)[0]
    struct.pack_into("<H", hdr, 2, crc16(log[0x40:0x40 + 0x50 * n]))
    struct.pack_into("<H", hdr, 0, crc16(hdr[2:0x40]))
    enc(hdr, 0, 0x40)
    log[0:0x40] = hdr
    return with_markers(log, prod)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "info":
        for k, v in info(sys.argv[2]).items():
            print(f"{k}: {v:#x}" if k in ("bank_off", "bank_id") else f"{k}: {v}")
    elif len(sys.argv) >= 5 and sys.argv[1] == "patch":
        out = patch(Path(sys.argv[2]).read_bytes(), Path(sys.argv[3]).read_bytes())
        Path(sys.argv[4]).write_bytes(out)
        print(f"{sys.argv[4]}: {len(out)} B")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
