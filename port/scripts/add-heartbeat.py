#!/usr/bin/env python3
"""Very early ARM11 heartbeat for the Nintendo 3DS port.

The kernel console only appears about 4 seconds into boot, so a crash before
that leaves the loader's last message frozen on the top screen and tells us
nothing at all - which is exactly the failure mode reported from hardware.

This makes the zImage decompressor paint a bright green band across the top of
the top screen (physical VRAM 0x18000000, 24bpp BGR, 720-byte stride) as the
very first thing the ARM11 executes, while the MMU is still off and the loader
is still scanning that buffer out:

  * green band visible, then boot log   -> kernel runs (any hang is in Linux)
  * green band visible, then nothing    -> kernel started but crashed early
  * no green band, loader text stays   -> the ARM11 never reached `start`

The store is a plain physical write (MMU off), so it works before any driver
exists.  r7/r8 (saved machine-id / atags) and r9 (saved CPSR) are preserved, so
the boot parameters are untouched.

Idempotent: re-running does nothing if the patch is already present.
"""
import pathlib
import os
import sys

kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/p3ds/src/linux-3ds"))
p = kd / "arch/arm/boot/compressed/head.S"
s = p.read_text()

if "3DS bring-up heartbeat" in s:
    print("    head.S: ARM11-entry heartbeat already present")
    raise SystemExit(0)

anchor = "\t\tmov\tr8, r2\t\t\t@ save atags pointer\n"
if anchor not in s:
    raise SystemExit("error: head.S: anchor 'save atags pointer' not found")

heartbeat = anchor + """
\t\t/*
\t\t * 3DS bring-up heartbeat (see port/scripts/add-heartbeat.py):
\t\t * paint a green band into the top-screen VRAM while the MMU is
\t\t * still off, so a pre-console hang is distinguishable from the
\t\t * ARM11 never starting at all.  r7/r8/r9 are preserved.
\t\t */
\t\tmov\tr0, #0x18000000\t\t@ top-screen VRAM (physical)
\t\tmov\tr1, #0x0000ff00\t\t@ green (BGR)
\t\tmov\tr2, #0x4000\t\t@ 16384 words (~91 scan lines)
99:\t\tstr\tr1, [r0], #4
\t\tsubs\tr2, r2, #1
\t\tbne\t99b
"""

p.write_text(s.replace(anchor, heartbeat, 1))
print("    head.S: ARM11-entry heartbeat added (green band on the top screen)")
