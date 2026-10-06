#!/usr/bin/env python3
"""
nwm-to-ath6k.py - turn a dumped Nintendo NWM ".code" into firmware files that
mainline ath6kl can load on the AR6014G.

The Nintendo 3DS Wi-Fi is an Atheros AR6014G, which is in the AR6002 "hw2"
family.  Nintendo's NWM system module carries the exact Xtensa blob the chip
needs, but split into the AR6002 SDK's files:

    NWM block        AR6002 SDK file          uploaded to (AR6014)   how
    ---------------  -----------------------  ---------------------  ---------
    stub_data        eeprom.data              0x00524c00             raw
    stub_code        eeprom.bin               0x00527000             raw + exec
    main_type1/4/5   athwlan.bin.z77          0x00524c00             LZ stream
    database         data.patch.hw2_0.bin     0x0053fe18             raw

Those destinations are not guesses: they are the literal pool constants of the
NWM ARM11 downloader (`main.dst = 0x00524c00`, `stub.code.dst = 0x00527000`,
`database.dst = 0x0053fe18`), and its descriptor builder marks only the main
image as `flag=1` (BMI_LZ_DATA / compressed) while the stub, its data and the
patch go in raw.  The execute address is `stub.code.dst | 0x400000`, i.e.
0x00927000 -- exactly GBATEK's AR6014 "BMI_DONE entry".

This tool carves the six blocks (same recipe as `nwm-extract.py`) and writes
an `ath6k/AR6014/` directory tree:

    athwlan.bin.z77        the selected "Main" image (default Type4)
    data.patch.hw2_0.bin   the Database (ROM patch list)
    eeprom.bin             the Stub.code (uploaded + executed)
    eeprom.data            the Stub.data
    manifest.json          offsets/sizes/SHA-256 of everything
    README.txt             this mapping, on disk next to the files

It is deliberately dumb about the LZ stream: the AR6014 ROM decompresses the
Main image, so the host simply streams Nintendo's bytes (see
`ath6kl_bmi_fast_download()`), and no host-side z77 codec is needed.

Usage:
    python3 nwm-to-ath6k.py [--type {1,4,5}]  (default now 4, see below) [--out DIR] nwm.code
    python3 nwm-to-ath6k.py --blocks nwm.code.blocks --out DIR
"""

import argparse
import hashlib
import json
import pathlib
import struct
import sys

# --- pool constants (GBATEK "3DS Files - Module NWM") -----------------------
MAIN_DST = 0x00524C00          # main.dst == stub.data.dst
MAGIC_3ED = 0x000003ED
DATABASE_DST = 0x0053FE18
STUBCODE_DST = 0x00527000

# invariable sizes used to validate the code base
KNOWN_STUB_DATA = 0x38
KNOWN_STUB_CODE = 0x316
KNOWN_DATABASE = 0x1E8

# AR6014 execute address for the stub: the Xtensa instruction window is the
# data address *plus* 0x400000 (AR6002: eeprom.bin@0x513950 executes at
# 0x913950), i.e. 0x00927000 for the 3DS -- GBATEK's AR6014 BMI_DONE entry.
STUB_EXEC = STUBCODE_DST + 0x00400000   # 0x00927000

BLOCK_NAMES = ("stub_data", "stub_code", "database",
               "main_type1", "main_type4", "main_type5")


def u32(buf, off):
    if off < 0 or off + 4 > len(buf):
        return None
    return struct.unpack_from("<I", buf, off)[0]


def _pair(buf, a_off, b_off):
    a, b = u32(buf, a_off), u32(buf, b_off)
    if not a or not b:
        return None
    lo, hi = min(a, b), max(a, b)
    if lo == 0 or hi - lo == 0:
        return None
    return lo, hi


def parse_newer(buf, p):
    blocks = {}
    if u32(buf, p + 4) != MAGIC_3ED:
        return None
    blocks["main_type1"] = _pair(buf, p + 0x08, p + 0x0C)
    blocks["main_type4"] = _pair(buf, p + 0x10, p + 0x14)
    blocks["main_type5"] = _pair(buf, p + 0x18, p + 0x1C)
    if u32(buf, p + 0x28) == DATABASE_DST:
        blocks["database"] = _pair(buf, p + 0x20, p + 0x24)
    if u32(buf, p + 0x34) == STUBCODE_DST:
        blocks["stub_code"] = _pair(buf, p + 0x2C, p + 0x30)
        blocks["stub_data"] = _pair(buf, p + 0x38, p + 0x3C)
    return blocks


def parse_older(buf, p):
    blocks = {}
    if u32(buf, p + 0x1C) != MAGIC_3ED:
        return None
    blocks["main_type5"] = _pair(buf, p + 0x04, p + 0x08)
    if u32(buf, p + 0x0C) == DATABASE_DST:
        blocks["database"] = _pair(buf, p + 0x10, p + 0x14)
        if u32(buf, p + 0x18) == STUBCODE_DST:
            blocks["stub_code"] = _pair(buf, p + 0x1C, p + 0x20)
            blocks["stub_data"] = _pair(buf, p + 0x24, p + 0x28)
        blocks["main_type1"] = _pair(buf, p + 0x38, p + 0x3C)
        blocks["main_type4"] = _pair(buf, p + 0x40, p + 0x44)
    else:
        blocks["main_type1"] = _pair(buf, p + 0x0C, p + 0x10)
        blocks["main_type4"] = _pair(buf, p + 0x14, p + 0x18)
    return blocks


def score(blocks, buf, base):
    good, total = 0, 0
    for rng in blocks.values():
        if not rng:
            continue
        lo, hi = rng
        s, e = lo - base, hi - base
        if 0 <= s < e <= len(buf):
            good += 1
            total += e - s
    for name, want in (("stub_data", KNOWN_STUB_DATA),
                       ("stub_code", KNOWN_STUB_CODE),
                       ("database", KNOWN_DATABASE)):
        rng = blocks.get(name)
        if rng and (rng[1] - rng[0]) == want:
            good += 3
    return good, total


def choose_base_and_pool(buf):
    offs = [o for o in range(0, len(buf) - 4, 4) if u32(buf, o) == MAIN_DST]
    if not offs:
        return None, None, None
    best = None
    for p in offs:
        for parser in (parse_newer, parse_older):
            blocks = parser(buf, p)
            if not blocks:
                continue
            for bi, base in enumerate([0x10000, 0x100000, 0x0, 0x8000, 0x20000]):
                sc, total = score(blocks, buf, base)
                key = (sc, total, -bi)
                if best is None or key > best[0]:
                    best = (key, base, p, blocks, parser.__name__)
    if best is None:
        return None, None, None
    _, base, p, blocks, layout = best
    return base, (p, layout), blocks


def carve_from_code(code_path):
    buf = code_path.read_bytes()
    base, pool, blocks = choose_base_and_pool(buf)
    if base is None:
        sys.exit("%s: no NWM literal pool found (decompressed .code needed?)"
                 % code_path)
    out = {}
    for name in BLOCK_NAMES:
        rng = blocks.get(name)
        if not rng:
            continue
        lo, hi = rng
        s, e = lo - base, hi - base
        if not (0 <= s < e <= len(buf)):
            sys.exit("%s: %s address range outside file" % (code_path, name))
        out[name] = buf[s:e]
    return out, base, pool


def carve_from_blocks(blocks_dir):
    out = {}
    for name in BLOCK_NAMES:
        p = blocks_dir / (name + ".bin")
        if p.is_file():
            out[name] = p.read_bytes()
    if not out:
        sys.exit("%s: no *.bin blocks found" % blocks_dir)
    return out, None, None


# The destinations the AR6014 downloader writes each block to, and how.
# `compressed` is the ARM11 descriptor flag (1 == BMI_LZ_DATA).
LOAD_MAP = {
    "eeprom.data":         ("stub_data",   MAIN_DST,      False),
    "eeprom.bin":          ("stub_code",   STUBCODE_DST,  False),
    "athwlan.bin.z77":     ("main_type%d", MAIN_DST,      True),
    "data.patch.hw2_0.bin": ("database",   DATABASE_DST,  False),
}


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("code", nargs="?", help="decompressed NWM .code file")
    ap.add_argument("--blocks", help="directory of already-carved *.bin blocks")
    ap.add_argument("--type", choices=("1", "4", "5"), default="4",
                    help="which Main image to use (default 4 = normal internet; "
                         "newer Type1 revisions hang, see GBATEK NWM)")
    ap.add_argument("--out", default="ath6k/AR6014",
                    help="output directory (default: ath6k/AR6014)")
    args = ap.parse_args()

    if args.blocks:
        blocks, base, pool = carve_from_blocks(pathlib.Path(args.blocks))
    elif args.code:
        blocks, base, pool = carve_from_code(pathlib.Path(args.code))
    else:
        ap.error("give a .code file or --blocks DIR")

    main_name = "main_type%s" % args.type
    if main_name not in blocks:
        sys.exit("Main type %s not present in this dump (have: %s)"
                 % (args.type, ", ".join(sorted(blocks))))

    # sanity: the invariable blocks must have their documented sizes
    for name, want in (("stub_data", KNOWN_STUB_DATA),
                       ("stub_code", KNOWN_STUB_CODE),
                       ("database", KNOWN_DATABASE)):
        if name in blocks and len(blocks[name]) != want:
            print("WARNING: %s is %d bytes, expected 0x%x"
                  % (name, len(blocks[name]), want), file=sys.stderr)

    outdir = pathlib.Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)

    manifest = {
        "source": args.code or args.blocks,
        "code_base": ("0x%x" % base) if base is not None else None,
        "main_type": args.type,
        "stub_exec_addr": "0x%x" % STUB_EXEC,
        "files": {},
    }

    def emit(fname, block):
        data = blocks[block]
        (outdir / fname).write_bytes(data)
        manifest["files"][fname] = {
            "block": block, "size": len(data), "sha256": sha256(data),
        }
        print("  %-22s %7d bytes  <- %s" % (fname, len(data), block))

    emit("eeprom.data", "stub_data")
    emit("eeprom.bin", "stub_code")
    emit("data.patch.hw2_0.bin", "database")
    emit("athwlan.bin.z77", main_name)

    # Also drop the other Main variants, for switching types on the card.
    for variant in ("main_type1", "main_type4", "main_type5"):
        if variant in blocks and variant != main_name:
            data = blocks[variant]
            (outdir / (variant + ".bin")).write_bytes(data)
            manifest["files"][variant + ".bin"] = {
                "block": variant, "size": len(data), "sha256": sha256(data),
            }

    readme = """\
ath6k/AR6014 - firmware for the Nintendo 3DS AR6014G (AR6002/hw2 family)

Built by port/scripts/nwm-to-ath6k.py from a GodMode9 NWM dump.
The CARVED block -> destination map (from the NWM ARM11 downloader's pool):

  eeprom.data         <- Stub.data   -> 0x00524c00  (raw BMI_WRITE_MEMORY)
  eeprom.bin          <- Stub.code   -> 0x00527000  (raw, then BMI_EXECUTE 0x00927000)
  athwlan.bin.z77     <- Main type%s  -> 0x00524c00  (BMI_LZ_DATA stream)
  data.patch.hw2_0.bin<- Database    -> 0x0053fe18  (raw), hi_dset_list_head=0x0053fe18

Host-interest base for the AR6014 is 0x00520400 (the Xtensa Main images
reference it directly).  The Main image is already compressed for the
AR6014 ROM's own decompressor -- do NOT re-compress it.

Main type4 = normal internet (default; GBATEK: newer revisions of Type1
hang), type1 = older standard internet, type5 = MacFilter/GameID (no normal
internet).  To switch, copy main_typeN.bin over athwlan.bin.z77.
""" % args.type
    (outdir / "README.txt").write_text(readme)
    manifest["files"]["README.txt"] = {"generated": True}
    (outdir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    print("wrote %s/" % outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
