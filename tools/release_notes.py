#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A release's notes: its section of CHANGELOG.md (the lines under "## VERSION ..." up to the next "## "),
then how to install it. Used by .github/workflows/release.yml.

  tools/release_notes.py VERSION [REPOSITORY]      (REPOSITORY: owner/name, for the links)"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]


def main(version, repo=""):
    text = (SRC / "CHANGELOG.md").read_text(encoding="utf-8")
    m = re.search(rf"^## {re.escape(version)}\b[^\n]*\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    body = m.group(1).strip() if m else f"ROM-1 {version}."
    owner, _, name = repo.partition("/")
    site = f"https://{owner.lower()}.github.io/{name}/" if repo else ""
    out = [body, "", "## Install", ""]
    if site:
        out.append(f"- **From the browser** (Chrome or Edge, the FM-1 on USB): the [web installer]({site}install/) "
                   f"(the starter sounds), or [your sounds]({site}sounds/) (your .sf2 files, converted in the browser). "
                   f"Or [try it in the browser first]({site}emu/).")
    out.append(f"- **From the command line:** `python3 tools/fm1_install.py rom1-{version}.fwsc` (the starter "
               f"sounds), or `python3 tools/rom1_load.py --package rom1-{version}.fwsc FONT.sf2 ...` (yours; "
               "Python 3 with `pip3 install mido python-rtmidi`, and a C compiler).")
    out.append("- **Going back:** the installer's \"Back to the stock firmware\", or M-VAVE's M-UPGRADE.")
    out.append("")
    out.append(f"The firmware is `rom1-{version}.fwsc` below; its source is this tag. Installing other firmware is "
               "at your own risk; see the manual's Troubleshooting.")
    print("\n".join(out))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(*sys.argv[1:3])
