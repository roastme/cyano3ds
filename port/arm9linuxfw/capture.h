#pragma once

/*
 * ARM9 black-box capture (option B).
 *
 * The ARM9 is an independent CPU that owns the SD controller.  It can write
 * raw sectors to the card directly, bypassing the ARM11 Linux block layer.
 * That matters because the 15 s tmpfs->SD flusher (and every other Linux SD
 * writer) wedges on that path under load: once it stalls, all card logs stop
 * and everything after it is trapped in tmpfs and lost on power-off.
 *
 * The ARM11 kernel keeps a 4 KiB capture page in DRAM (see ctr_lcd_fb.c).
 * This module polls that page and mirrors it to a rotating set of sectors in
 * a preallocated card file, so the last snapshots survive exactly when the
 * Linux write path is dead.
 *
 * The page's physical address arrives once, over a PXI manager register write
 * (a channel the virtio transport does not otherwise use).  After that the
 * ARM9 polls the page with its own timer: no PXI, no Linux, no block layer.
 */

#include "common.h"

/* "A9CP", little endian */
#define ARM9CAP_MAGIC		0x50433941u
#define ARM9CAP_VERSION		1u

#define ARM9CAP_SIZE		4096u
#define ARM9CAP_TEXT_OFF	0x100u
#define ARM9CAP_MGR_REG		0x20u	/* manager register index */

/*
 * Must match struct arm9cap in
 * port/kernel/drivers/platform/nintendo3ds/ctr_lcd_fb.c exactly.
 *
 * The ARM11 owns magic..text_off and text[].  The a9_* fields are only ever
 * filled in by the ARM9, in its local copy, just before the SD write.
 */
struct arm9cap {
	u32 magic;		/* ARM9CAP_MAGIC */
	u32 version;		/* ARM9CAP_VERSION */
	u32 seq;		/* bumped by the ARM11 after each update */
	u32 heartbeat;		/* bumped once per ARM11 update */

	u32 sector;		/* first 512-byte sector of the capture file */
	u32 slots;		/* number of 4 KiB slots in that file */
	u32 text_len;		/* valid bytes of text[] */
	u32 text_off;		/* ring write offset into text[] */

	/* ARM9-owned state (filled in by the ARM9, local copy only) */
	u32 a9_seq;
	u32 a9_slot;
	u32 a9_ticks_lo;
	u32 a9_ticks_hi;
	u32 a9_last_addr;
	u32 a9_status;

	u32 reserved[50];	/* pad to ARM9CAP_TEXT_OFF */

	char text[ARM9CAP_SIZE - ARM9CAP_TEXT_OFF];
};

void a9cap_init(void);		/* register the wake-up timer IRQ */
void a9cap_set_addr(u32 phys);	/* store the capture page address */
void a9cap_poll(void);		/* called from the ARM9 main loop */
void a9cap_timer_irq(u32 irqn);
