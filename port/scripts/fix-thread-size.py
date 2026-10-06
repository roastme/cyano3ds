#!/usr/bin/env python3
"""
fix-thread-size.py - 16 KiB kernel stacks

WHY THIS EXISTS
---------------
The port hit a *silent* hard freeze under heavy Android framework load: the
timer-interrupt square on the top screen stops changing, there is no oops, no
hung-task report, no softlockup report and no stack-protector message, and the
kernel-side heartbeat on the SD stops at the same instant.

One of the few failures that looks exactly like that is a kernel stack
overflow: ARM's default is an 8 KiB stack, and an overflow inside the
interrupt/softirq path corrupts whatever lies below the stack (on this kernel
`thread_info` itself, since CONFIG_THREAD_INFO_IN_TASK is off) *without* ever
returning through the stack-protector epilogue.

ARM already uses 16 KiB stacks whenever KASAN is enabled, so this is the proven
configuration; it costs 8 KiB of kernel memory per task (about 0.8 MiB here).

Idempotent: running it twice changes nothing.
"""

import sys
import pathlib

OLD = """#ifdef CONFIG_KASAN
/*
 * KASan uses a lot of extra stack space so the thread size order needs to
 * be increased.
 */
#define THREAD_SIZE_ORDER\t2
#else
#define THREAD_SIZE_ORDER\t1
#endif
"""

NEW = """/*
 * 16 KiB kernel stacks (see fix-thread-size.py in the port).
 *
 * The port had a silent hard freeze under heavy Android framework load with no
 * oops/hung-task/softlockup/stack-protector report; a kernel stack overflow in
 * the interrupt path produces exactly that signature.  ARM already uses 16 KiB
 * stacks for KASAN, so this is the proven configuration.
 */
#define THREAD_SIZE_ORDER\t2
"""

MARK = "16 KiB kernel stacks"


def main():
    if len(sys.argv) < 2:
        print("usage: fix-thread-size.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "arch/arm/include/asm/thread_info.h"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1
    s = p.read_text()
    if MARK in s:
        print("    thread_info.h: already using 16 KiB stacks")
        return 0
    if OLD not in s:
        print("error: thread_info.h: THREAD_SIZE_ORDER block not found",
              file=sys.stderr)
        return 1
    p.write_text(s.replace(OLD, NEW, 1))
    print("    thread_info.h: THREAD_SIZE_ORDER 1 -> 2 (16 KiB stacks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
