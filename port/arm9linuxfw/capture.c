#include "common.h"

#include "arm/arm.h"
#include "hw/timer.h"
#include "hw/irq.h"
#include "hw/sdmmc.h"

#include "capture.h"

/* write a snapshot at most this often */
#define ARM9CAP_INTERVAL_MS	2000u

static u32 a9cap_phys;
static u64 a9cap_last_poll;
static u32 a9cap_seq;   /* the ARM9's own counter: the shared page keeps zeroing
			  * its a9_* fields, so they must not be read back */
static u32 a9cap_slot;
static struct arm9cap a9cap_buf;

void a9cap_set_addr(u32 phys)
{
	a9cap_phys = phys;
}

void a9cap_timer_irq(u32 irqn)
{
	/*
	 * Nothing to do: irq_process() (source/hw/irq.c) has already acknowledged
	 * the timer pending bit.  The IRQ exists only to wake the main loop out of
	 * arm_wait_for_interrupt() so a9cap_poll() runs even when no PXI
	 * transaction arrives.
	 */
}

void a9cap_init(void)
{
	irq_enable(IRQ_TIMER0, a9cap_timer_irq);
}

/* freestanding build: copy words, no libc */
static void a9cap_copy(volatile u32 *dst, const volatile u32 *src, u32 words)
{
	for (u32 i = 0; i < words; i++)
		dst[i] = src[i];
}

void a9cap_poll(void)
{
	const volatile struct arm9cap *sh;
	u64 now;
	u32 seq, sector;

	if (!a9cap_phys)
		return;

	now = timer_get_ticks();
	if (now - a9cap_last_poll < timer_ms_to_ticks(ARM9CAP_INTERVAL_MS))
		return;
	a9cap_last_poll = now;

	sh = (const volatile struct arm9cap *)a9cap_phys;
	if (sh->magic != ARM9CAP_MAGIC || sh->version != ARM9CAP_VERSION)
		return;
	if (!sh->slots || !sh->sector)
		return;

	/* the card must have been brought up by Linux's virtio-blk reset first */
	if (!sdmmc_sdcard_size())
		return;

	/*
	 * The ARM11 never changes the sector/slot configuration after setup, but
	 * it is read from shared DRAM - sanity-check it once per snapshot.
	 */
	if (sh->slots > 64u)
		return;

	seq = sh->seq;
	if (seq & 1u)
		return;		/* ARM11 is mid-update */
	a9cap_copy((volatile u32 *)&a9cap_buf,
		   (const volatile u32 *)sh, sizeof(a9cap_buf) / 4u);
	arm_sync_barrier();

	/* if the ARM11 updated the page while we copied it, skip this round */
	if (sh->seq != seq)
		return;

	a9cap_seq++;
	a9cap_buf.a9_seq = a9cap_seq;
	a9cap_slot++;
	if (a9cap_slot >= a9cap_buf.slots)
		a9cap_slot = 0;
	a9cap_buf.a9_slot = a9cap_slot;
	a9cap_buf.a9_ticks_lo = (u32)now;
	a9cap_buf.a9_ticks_hi = (u32)(now >> 32);

	sector = a9cap_buf.sector + a9cap_slot * (ARM9CAP_SIZE / 512u);
	a9cap_buf.a9_status = (u32)sdmmc_sdcard_writesectors(
		sector, ARM9CAP_SIZE / 512u, (const u8 *)&a9cap_buf);
}
