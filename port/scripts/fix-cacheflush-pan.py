#!/usr/bin/env python3
"""
fix-cacheflush-pan.py - make the cacheflush(2) syscall work with
CONFIG_CPU_SW_DOMAIN_PAN.

WHY
---
This kernel has CONFIG_CPU_SW_DOMAIN_PAN=y.  With software PAN, DACR_INIT
sets DOMAIN_USER to DOMAIN_NOACCESS while the kernel runs, and user access is
only re-enabled around explicit uaccess windows (uaccess_save_and_enable()).

The cacheflush syscall walks a *user* range in
arch/arm/kernel/traps.c:__do_cache_op() -> flush_icache_user_range() ->
v6_coherent_user_range(), which executes cache-maintenance `mcr` instructions
on those user addresses.  Because user access has not been enabled, those
`mcr`s take a *domain fault*.  A domain fault is routed to do_bad() ->
do_bad_area() -> __do_kernel_fault() -> die(); it never consults the USER()
__ex_table fixup in v6_coherent_user_range(), so the kernel oopses and kills
the task.  When the page happens to be mapped the abort may be fixed up, but
the cache maintenance never ran, so self-modifying code (libpixelflinger's
JIT scanlines) executes from a stale instruction cache.

This is upstream commit "ARM: fix cacheflush with PAN" (Russell King),
Fixes: a5e090acbf54 ("ARM: software-based priviledged-no-access support"):

    static inline int
    __do_cache_op(unsigned long start, unsigned long end)
    {
    +       unsigned int ua_flags;
            int ret;

            do {
                    ...
    +               ua_flags = uaccess_save_and_enable();
                    ret = flush_icache_user_range(start, start + chunk);
    +               uaccess_restore(ua_flags);
                    if (ret)
                            return ret;
                    ...

Idempotent.
"""

import sys
import pathlib


MARK = "uaccess_save_and_enable"


def main():
    if len(sys.argv) < 2:
        print("usage: fix-cacheflush-pan.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "arch/arm/kernel/traps.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    with p.open("r", newline="") as f:
        s = f.read()

    if MARK in s:
        print("    traps.c: cacheflush already wrapped in uaccess (PAN fix)")
        return 0

    old = (
        "static inline int\n"
        "__do_cache_op(unsigned long start, unsigned long end)\n"
        "{\n"
        "\tint ret;\n"
        "\n"
        "\tdo {\n"
        "\t\tunsigned long chunk = min(PAGE_SIZE, end - start);\n"
        "\n"
        "\t\tif (fatal_signal_pending(current))\n"
        "\t\t\treturn 0;\n"
        "\n"
        "\t\tret = flush_icache_user_range(start, start + chunk);\n"
        "\t\tif (ret)\n"
        "\t\t\treturn ret;\n"
    )
    new = (
        "static inline int\n"
        "__do_cache_op(unsigned long start, unsigned long end)\n"
        "{\n"
        "\tunsigned int ua_flags;\n"
        "\tint ret;\n"
        "\n"
        "\tdo {\n"
        "\t\tunsigned long chunk = min(PAGE_SIZE, end - start);\n"
        "\n"
        "\t\tif (fatal_signal_pending(current))\n"
        "\t\t\treturn 0;\n"
        "\n"
        "\t\t/*\n"
        "\t\t * CONFIG_CPU_SW_DOMAIN_PAN keeps DOMAIN_USER inaccessible in\n"
        "\t\t * kernel mode; the cache-maintenance instructions below touch\n"
        "\t\t * user addresses, so enable user access around them (a domain\n"
        "\t\t * fault here is unhandled and oopses the kernel).\n"
        "\t\t */\n"
        "\t\tua_flags = uaccess_save_and_enable();\n"
        "\t\tret = flush_icache_user_range(start, start + chunk);\n"
        "\t\tuaccess_restore(ua_flags);\n"
        "\t\tif (ret)\n"
        "\t\t\treturn ret;\n"
    )

    if old not in s:
        print("error: traps.c: __do_cache_op not found (unexpected version)",
              file=sys.stderr)
        return 1

    s = s.replace(old, new, 1)
    p.write_text(s, newline="")
    assert MARK in s
    print("    traps.c: cacheflush wrapped in uaccess_save_and_enable/restore")
    return 0


if __name__ == "__main__":
    sys.exit(main())
