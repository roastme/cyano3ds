#!/usr/bin/env python3
"""
nwm-extract.py - carve the Xtensa Wi-Fi firmware out of a dumped NWM ".code".

Input is the *decompressed* ExeFS ".code" produced by the GodMode9 script
``port/gm9/DumpNWMWifi.lua`` (which writes it to 0:/gm9/out/wifi/*.code on the
SD card).  That file is ARM11 code plus six Xtensa datablocks.  The ARM11 code
references the datablocks through its literal pool, so -- as described in
GBATEK "3DS Files - Module NWM" -- we find the pool by its magic values and read
the block start/end addresses out of it:

  * the "main.dst" value is the constant 0x00524C00
    (the pool either has 0x000003ED right after it, "newer" layout, or 0x1C
    bytes later, "older" layout);
  * "database.dst" is 0x0053FE18, "stub.code.dst" is 0x00527000.

The pool stores virtual addresses; subtract the code base to get file offsets.
GBATEK says 0x10000.  We auto-detect the base by requiring the always-constant
block sizes (Stub.data 0x38, Stub.code 0x316, Database 0x1E8) and/or a sensible
tiling of the file, and we report which one was chosen.

Usage:
    python3 nwm-extract.py nwm.code [more.code ...]
    python3 nwm-extract.py --out DIR nwm.code

For each input this writes DIR/<stem>/<block>.bin (stub_data, stub_code,
database, main_type1, main_type4, main_type5) plus blocks.json.  Those are the
raw Xtensa images; converting them to whatever ath6kl expects is a separate
step (see docs/WIFI.md).
"""

import argparse
import json
import pathlib
import struct
import sys

MAIN_DST = 0x00524C00          # main.dst (and stub.data.dst)
MAGIC_3ED = 0x000003ED         # follows main.dst (newer) / at +0x1C (older)
DATABASE_DST = 0x0053FE18
STUBCODE_DST = 0x00527000

# Sizes that never change (GBATEK): used to validate the code base.
KNOWN_STUB_DATA = 0x38
KNOWN_STUB_CODE = 0x316
KNOWN_DATABASE = 0x1E8


def u32(buf, off):
    if off < 0 or off + 4 > len(buf):
        return None
    return struct.unpack_from("<I", buf, off)[0]


def find_pool_candidates(buf):
    """Return every 4-aligned offset whose word is MAIN_DST."""
    out = []
    step = 4
    raw = buf
    for off in range(0, len(raw) - 4, step):
        if struct.unpack_from("<I", raw, off)[0] == MAIN_DST:
            out.append(off)
    return out


def _pair(buf, a_off, b_off):
    """Read a (end, start) address pair and return (lo_addr, hi_addr)."""
    a, b = u32(buf, a_off), u32(buf, b_off)
    if not a or not b:
        return None
    lo, hi = min(a, b), max(a, b)
    if lo == 0 or hi - lo == 0:
        return None
    return lo, hi


def parse_newer(buf, p):
    """main.dst at p, magic 0x3ED at p+4.  Returns {name: (lo,hi)}."""
    blocks = {}
    if u32(buf, p + 4) != MAGIC_3ED:
        return None
    blocks["main_type1"] = _pair(buf, p + 0x08, p + 0x0C)
    blocks["main_type4"] = _pair(buf, p + 0x10, p + 0x14)
    blocks["main_type5"] = _pair(buf, p + 0x18, p + 0x1C)
    # Optional extended ("large pool") entries.
    if u32(buf, p + 0x28) == DATABASE_DST:
        blocks["database"] = _pair(buf, p + 0x20, p + 0x24)
    if u32(buf, p + 0x34) == STUBCODE_DST:
        blocks["stub_code"] = _pair(buf, p + 0x2C, p + 0x30)
        blocks["stub_data"] = _pair(buf, p + 0x38, p + 0x3C)
    return blocks


def parse_older(buf, p):
    """main.dst at p, magic 0x3ED at p+0x1C.  Returns {name: (lo,hi)}."""
    blocks = {}
    if u32(buf, p + 0x1C) != MAGIC_3ED:
        return None
    blocks["main_type5"] = _pair(buf, p + 0x04, p + 0x08)
    if u32(buf, p + 0x0C) == DATABASE_DST:
        # older "large pool"
        blocks["database"] = _pair(buf, p + 0x10, p + 0x14)
        if u32(buf, p + 0x18) == STUBCODE_DST:
            blocks["stub_code"] = _pair(buf, p + 0x1C, p + 0x20)
            blocks["stub_data"] = _pair(buf, p + 0x24, p + 0x28)
        blocks["main_type1"] = _pair(buf, p + 0x38, p + 0x3C)
        blocks["main_type4"] = _pair(buf, p + 0x40, p + 0x44)
    else:
        # older "small pool"
        blocks["main_type1"] = _pair(buf, p + 0x0C, p + 0x10)
        blocks["main_type4"] = _pair(buf, p + 0x14, p + 0x18)
    return blocks


def score(blocks, buf, base):
    """How well do these (lo,hi) addresses map to real file ranges?"""
    good = 0
    total = 0
    for name, rng in blocks.items():
        if rng is None:
            continue
        lo, hi = rng
        s, e = lo - base, hi - base
        if 0 <= s < e <= len(buf):
            good += 1
            total += e - s
    # Strong bonus for the invariable block sizes.
    if blocks.get("stub_data"):
        lo, hi = blocks["stub_data"]
        if hi - lo == KNOWN_STUB_DATA:
            good += 3
    if blocks.get("stub_code"):
        lo, hi = blocks["stub_code"]
        if hi - lo == KNOWN_STUB_CODE:
            good += 3
    if blocks.get("database"):
        lo, hi = blocks["database"]
        if hi - lo == KNOWN_DATABASE:
            good += 3
    return good, total


def choose_base_and_pool(buf):
    cands = find_pool_candidates(buf)
    if not cands:
        return None, None, None
    bases = [0x10000, 0x100000, 0x0, 0x8000, 0x20000]
    best = None  # (score, total, -base_index, base, p, blocks)
    for p in cands:
        for parser in (parse_newer, parse_older):
            blocks = parser(buf, p)
            if not blocks:
                continue
            for bi, base in enumerate(bases):
                sc, total = score(blocks, buf, base)
                key = (sc, total, -bi)
                if best is None or key > best[0]:
                    best = (key, base, p, blocks, parser.__name__)
    if best is None:
        return None, None, None
    _, base, p, blocks, layout = best
    return base, (p, layout), blocks


def extract(buf, blocks, base, outdir, stem):
    outdir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "source": str(stem),
        "code_size": len(buf),
        "base": "0x%x" % base,
        "blocks": {},
    }
    for name in ("stub_data", "stub_code", "database",
                 "main_type1", "main_type4", "main_type5"):
        rng = blocks.get(name)
        if not rng:
            continue
        lo, hi = rng
        s, e = lo - base, hi - base
        if not (0 <= s < e <= len(buf)):
            manifest["blocks"][name] = {
                "addr": "0x%x..0x%x" % (lo, hi),
                "error": "address range outside the file",
            }
            continue
        data = buf[s:e]
        path = outdir / (name + ".bin")
        path.write_bytes(data)
        manifest["blocks"][name] = {
            "addr": "0x%x..0x%x" % (lo, hi),
            "file_offset": "0x%x..0x%x" % (s, e),
            "size": len(data),
            "out": path.name,
            "sha256": __import__("hashlib").sha256(data).hexdigest(),
        }
        print("  %-11s %6d bytes  -> %s" % (name, len(data), path))
    (outdir / "blocks.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("code", nargs="+", help="decompressed NWM .code file(s)")
    ap.add_argument("--out", default=None,
                    help="output directory (default: <code>.blocks next to each input)")
    args = ap.parse_args()

    rc = 0
    for fn in args.code:
        path = pathlib.Path(fn)
        if not path.is_file():
            print("no such file: %s" % fn, file=sys.stderr)
            rc = 1
            continue
        buf = path.read_bytes()
        print("%s (%d bytes)" % (path, len(buf)))
        base, pool, blocks = choose_base_and_pool(buf)
        if base is None:
            print("  no NWM literal pool found (is this a decompressed NWM .code?)",
                  file=sys.stderr)
            rc = 1
            continue
        p, layout = pool
        print("  pool at 0x%x (%s), code base 0x%x" % (p, layout, base))
        outdir = (pathlib.Path(args.out) / path.stem) if args.out \
            else path.with_suffix(path.suffix + ".blocks")
        extract(buf, blocks, base, outdir, path)
    return rc


if __name__ == "__main__":
    sys.exit(main())
