// SPDX-License-Identifier: GPL-2.0
/*
 * Nintendo 3DS sleep mode: CPU hotplug and PDN suspend-to-RAM
 *
 * Part of the Android port to the Nintendo 3DS.
 *
 * The sequence follows open_agb_firm's lid sleep (Dominik Leicht's libn3ds
 * "deep sleep" support, profi200/open_agb_firm PR 295 and libn3ds PR 9):
 *
 *   - every core but the boot core parks: it marks itself DORMANT in the SCU
 *     CPU status register and waits for interrupt;
 *   - the graphics hardware is quiesced (display controllers stopped, VRAM
 *     banks disabled, GPU clock gated), because the whole-system PDN sleep
 *     powers VRAM down and resets the graphics blocks;
 *   - the boot core enables the PDN wake sources "shell opened" and "MCU
 *     interrupt" (GPIO3 pin 9), marks itself DORMANT, sets PDN_CNT.SLEEP and
 *     waits for interrupt;
 *   - PDN wake resumes execution right after that WFI, in the same context, so
 *     no resume trampoline is needed; the graphics state is then rebuilt.
 *
 * The MCU side (LCD power, the "entering sleep" notification and the power
 * LED) needs I2C and is done by the lid driver's suspend/resume callbacks
 * (drivers/platform/nintendo3ds/ctr_lid.c), which run before this with
 * interrupts still on.
 *
 * Linux has no way to keep a secondary core "idle but online" through a
 * system suspend, so secondaries go through CPU hotplug.  The 3DS cannot
 * power a single core off and reset it, so a dead core parks in a WFI loop
 * with SMP coherency and its data cache off (like the ARM development
 * platforms' "immitation" hotplug) and is released by ctr_smp_boot_secondary()
 * when it is brought back online.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>
#include <linux/suspend.h>
#include <linux/vmalloc.h>

#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/proc-fns.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>

#include <mach/platsmp.h>

/* PDN (power down controller) */
#define PDN_BASE			0x10141000
#define PDN_CNT				0x000
#define  PDN_CNT_SLEEP			BIT(0)
#define  PDN_CNT_VRAM_OFF		BIT(15)	/* status: VRAM powered down */
#define PDN_WAKE_ENABLE			0x008
#define PDN_WAKE_REASON			0x00C	/* write 1 to acknowledge */
#define  PDN_WAKE_SHELL_OPENED		BIT(3)
#define  PDN_WAKE_GPIO_3_9		BIT(26)	/* the MCU interrupt line */
#define PDN_GPU_CNT			0x200
#define  PDN_GPU_CNT_NORST_ALL		0x7F
#define  PDN_GPU_CNT_CLK_EN		BIT(16)

/* GX: PSC/PDC register block */
#define GX_BASE				0x10400000
#define GX_GPU_CLK			0x004
#define GX_PSC_VRAM			0x030
#define  GX_PSC_VRAM_BANK_DIS_ALL	(0xF << 8)
#define GX_PSC_DMA_PRIO0		0x050
#define GX_PSC_DMA_PRIO1		0x054
#define GX_PDC(n)			(0x400 + (n) * 0x100)
#define  PDC_CNT			0x74
#define   PDC_CNT_EN			BIT(0)
#define   PDC_CNT_NO_IRQ_ALL		(BIT(8) | BIT(9) | BIT(10))
#define  PDC_SWAP			0x78
#define   PDC_SWAP_NEXT			BIT(0)
#define   PDC_SWAP_RST_FIFO		BIT(8)
#define   PDC_SWAP_IRQ_ACK_ALL		(BIT(16) | BIT(17) | BIT(18))
#define  PDC_COLOR_LUT_IDX		0x80
#define  PDC_COLOR_LUT_DATA		0x84
#define  PDC_LUT_SIZE			256

/* GPIO3 data: pin 9 is the active-low MCU interrupt line */
#define GPIO3_DATA			0x10147020
#define  GPIO3_MCU_IRQ			BIT(9)

/* ARM11 MPCore private timer (TWD, PPI 29) and the GIC's pending-clear */
#define TWD_BASE			0x17E00600
#define TWD_LOAD			0x00
#define TWD_COUNTER			0x04
#define TWD_CTRL			0x08
#define  TWD_CTRL_EN			BIT(0)
#define  TWD_CTRL_AUTO_RELOAD		BIT(1)
#define  TWD_CTRL_IRQ_EN		BIT(2)
#define TWD_INTSTAT			0x0C
#define GIC_DIST_PENDING_CLEAR		0x17E01280
#define  GIC_PPI_TWD			BIT(29)
/* screen-off sleep heartbeat: 4M PERIPHCLK ticks, 10-30 ms at 134-402 MHz */
#define CTR_LIGHT_SLEEP_TICK		4000000

static void __iomem *ctr_scu_base;

/*
 * Lid sleep comes in two kinds:
 *
 *   screen-off (default): the lid driver turns the panels and backlights off
 *   over the MCU, and the CPU waits in WFI until the MCU interrupt line says
 *   "shell opened".  Nothing is powered down, so nothing has to be rebuilt.
 *
 *   deep (ctr_deep_sleep=1): whole-system PDN sleep, as open_agb_firm does
 *   it.  VRAM and the graphics blocks lose power.  The first time it really
 *   engaged on hardware (2026-10-14) the console did not come back: opening
 *   the lid left it dark and it had to be powered off.  libn3ds rebuilds
 *   much more after PDN wake than this file does (a cold graphics reset,
 *   the LCD block and the LCD panel controllers over I2C, core 1 parked
 *   dormant before sleeping), so it stays opt-in until that is done.
 */
static bool ctr_deep_sleep;
core_param(ctr_deep_sleep, ctr_deep_sleep, bool, 0644);

bool ctr_pm_sleep_is_deep(void)
{
	return ctr_deep_sleep;
}

/* ---------------------------------------------------------------------- */
/* CPU hotplug                                                            */
/* ---------------------------------------------------------------------- */

void ctr_pm_set_scu_base(void __iomem *scu_base)
{
	ctr_scu_base = scu_base;
}

#ifdef CONFIG_HOTPLUG_CPU
static bool ctr_cpu_parked[NR_CPUS];
static volatile int ctr_cpu_release = -1;

/*
 * Runs on a live core once the dying one has reported its death, so the
 * flag is set before any later ctr_smp_boot_secondary() for that core.
 */
int ctr_cpu_kill(unsigned int cpu)
{
	ctr_cpu_parked[cpu] = true;
	return 1;
}

/*
 * Called from ctr_smp_boot_secondary(): true if the core is parked in
 * ctr_cpu_die() (not in the boot ROM's wait loop) and has been released.
 */
bool ctr_cpu_unpark(unsigned int cpu)
{
	if (!ctr_cpu_parked[cpu])
		return false;
	ctr_cpu_parked[cpu] = false;

	/* the parked core runs with its data cache off: write to memory */
	ctr_cpu_release = cpu_logical_map(cpu);
	sync_cache_w(&ctr_cpu_release);
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));
	return true;
}

void ctr_cpu_die(unsigned int cpu)
{
	unsigned int v, spurious = 0;

	if (ctr_scu_base)
		scu_power_mode(ctr_scu_base, SCU_PM_DORMANT);

	/* invalidate the I-cache, leave SMP coherency, turn the D-cache off */
	asm volatile(
		"mcr	p15, 0, %1, c7, c5, 0\n"
	"	mcr	p15, 0, %1, c7, c10, 4\n"
	"	mrc	p15, 0, %0, c1, c0, 1\n"
	"	bic	%0, %0, #0x20\n"
	"	mcr	p15, 0, %0, c1, c0, 1\n"
	"	mrc	p15, 0, %0, c1, c0, 0\n"
	"	bic	%0, %0, %2\n"
	"	mcr	p15, 0, %0, c1, c0, 0\n"
	  : "=&r" (v)
	  : "r" (0), "Ir" (CR_C)
	  : "cc");

	for (;;) {
		cpu_do_idle();
		if (ctr_cpu_release == cpu_logical_map(cpu))
			break;
		spurious++;
	}

	asm volatile(
		"mrc	p15, 0, %0, c1, c0, 0\n"
	"	orr	%0, %0, %1\n"
	"	mcr	p15, 0, %0, c1, c0, 0\n"
	"	mrc	p15, 0, %0, c1, c0, 1\n"
	"	orr	%0, %0, #0x20\n"
	"	mcr	p15, 0, %0, c1, c0, 1\n"
	  : "=&r" (v)
	  : "Ir" (CR_C)
	  : "cc");

	if (ctr_scu_base)
		scu_power_mode(ctr_scu_base, SCU_PM_NORMAL);
	ctr_cpu_release = -1;

	if (spurious)
		pr_debug("CPU%u: %u spurious wakeups while parked\n", cpu, spurious);
	/* returning makes arch_cpu_idle_dead() restart secondary_start_kernel */
}
#endif /* CONFIG_HOTPLUG_CPU */

/* ---------------------------------------------------------------------- */
/* PDN suspend                                                            */
/* ---------------------------------------------------------------------- */

#ifdef CONFIG_SUSPEND
static void __iomem *ctr_pdn;
static void __iomem *ctr_gx;
static void __iomem *ctr_gpio3;
static void __iomem *ctr_twd;
static void __iomem *ctr_gic_pending_clear;

/* registers lost when the graphics blocks are reset across PDN sleep */
static const u16 ctr_pdc_regs[] = {
	0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20,	/* H timing */
	0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44,	/* V timing */
	0x48, 0x4C, 0x5C, 0x60, 0x64,	/* polarity, border, picture */
	0x68, 0x6C, 0x70, 0x90, 0x94, 0x98, 0x9C, /* buffers, format, stride */
};

static struct {
	u32 gpu_cnt, gpu_clk, psc_vram, dma_prio0, dma_prio1;
	u32 pdc[2][ARRAY_SIZE(ctr_pdc_regs)];
	u32 pdc_cnt[2], pdc_swap[2];
	u32 lut[2][PDC_LUT_SIZE];
} ctr_gfx;

/*
 * VRAM is powered down during PDN sleep.  Keep a copy of the scanout buffers
 * the kernel knows about (the top-screen console framebuffer and the bottom
 * screen's private VRAM window) so both screens come back as they were.
 */
#define CTR_MAX_VRAM_SAVE	4
static struct {
	void __iomem *io;
	void *save;
	size_t size;
} ctr_vram[CTR_MAX_VRAM_SAVE];
static int ctr_vram_count;

static void __init ctr_vram_add(phys_addr_t start, resource_size_t size)
{
	if (ctr_vram_count == CTR_MAX_VRAM_SAVE ||
	    start < 0x18000000 || start + size > 0x18600000)
		return;
	ctr_vram[ctr_vram_count].io = ioremap(start, size);
	ctr_vram[ctr_vram_count].save = vmalloc(size);
	if (!ctr_vram[ctr_vram_count].io || !ctr_vram[ctr_vram_count].save) {
		if (ctr_vram[ctr_vram_count].io)
			iounmap(ctr_vram[ctr_vram_count].io);
		vfree(ctr_vram[ctr_vram_count].save);
		pr_warn("ctr-pm: cannot keep VRAM %pa across sleep\n", &start);
		return;
	}
	ctr_vram[ctr_vram_count++].size = size;
}

static void __init ctr_vram_init(void)
{
	struct device_node *np;
	struct resource res;

	for_each_compatible_node(np, NULL, "simple-framebuffer")
		if (!of_address_to_resource(np, 0, &res))
			ctr_vram_add(res.start, resource_size(&res));
	for_each_compatible_node(np, NULL, "nintendo,3ds-lcd")
		if (!of_address_to_resource(np, 1, &res))
			ctr_vram_add(res.start, resource_size(&res));
}

static void ctr_gfx_sleep(void)
{
	int i, n;

	ctr_gfx.gpu_cnt = readl(ctr_pdn + PDN_GPU_CNT);
	ctr_gfx.gpu_clk = readl(ctr_gx + GX_GPU_CLK);
	ctr_gfx.psc_vram = readl(ctr_gx + GX_PSC_VRAM);
	ctr_gfx.dma_prio0 = readl(ctr_gx + GX_PSC_DMA_PRIO0);
	ctr_gfx.dma_prio1 = readl(ctr_gx + GX_PSC_DMA_PRIO1);

	for (n = 0; n < 2; n++) {
		void __iomem *pdc = ctr_gx + GX_PDC(n);

		for (i = 0; i < ARRAY_SIZE(ctr_pdc_regs); i++)
			ctr_gfx.pdc[n][i] = readl(pdc + ctr_pdc_regs[i]);
		ctr_gfx.pdc_cnt[n] = readl(pdc + PDC_CNT);
		ctr_gfx.pdc_swap[n] = readl(pdc + PDC_SWAP);
		writel(0, pdc + PDC_COLOR_LUT_IDX);
		for (i = 0; i < PDC_LUT_SIZE; i++)
			ctr_gfx.lut[n][i] = readl(pdc + PDC_COLOR_LUT_DATA);

		/* stop the display controller */
		writel(PDC_CNT_NO_IRQ_ALL, pdc + PDC_CNT);
		writel(PDC_SWAP_IRQ_ACK_ALL | PDC_SWAP_RST_FIFO, pdc + PDC_SWAP);
	}
	/* at least one scanline (~40 us) for the PDC DMA to stop */
	udelay(50);

	for (i = 0; i < ctr_vram_count; i++)
		memcpy_fromio(ctr_vram[i].save, ctr_vram[i].io, ctr_vram[i].size);

	/* disable the VRAM banks (required for PDN sleep) and gate the GPU clock */
	writel(ctr_gfx.psc_vram | GX_PSC_VRAM_BANK_DIS_ALL, ctr_gx + GX_PSC_VRAM);
	writel(ctr_gfx.gpu_cnt & ~PDN_GPU_CNT_CLK_EN, ctr_pdn + PDN_GPU_CNT);
}

static void ctr_gfx_wake(void)
{
	int i, n;

	/* the graphics blocks come back reset: clock on, pulse every reset */
	writel(PDN_GPU_CNT_CLK_EN, ctr_pdn + PDN_GPU_CNT);
	udelay(1);
	writel(ctr_gfx.gpu_cnt | PDN_GPU_CNT_CLK_EN | PDN_GPU_CNT_NORST_ALL,
	       ctr_pdn + PDN_GPU_CNT);

	writel(ctr_gfx.gpu_clk, ctr_gx + GX_GPU_CLK);
	writel(ctr_gfx.psc_vram & ~GX_PSC_VRAM_BANK_DIS_ALL, ctr_gx + GX_PSC_VRAM);
	writel(ctr_gfx.dma_prio0, ctr_gx + GX_PSC_DMA_PRIO0);
	writel(ctr_gfx.dma_prio1, ctr_gx + GX_PSC_DMA_PRIO1);

	for (i = 0; i < ctr_vram_count; i++)
		memcpy_toio(ctr_vram[i].io, ctr_vram[i].save, ctr_vram[i].size);
	wmb();

	for (n = 0; n < 2; n++) {
		void __iomem *pdc = ctr_gx + GX_PDC(n);

		for (i = 0; i < ARRAY_SIZE(ctr_pdc_regs); i++)
			writel(ctr_gfx.pdc[n][i], pdc + ctr_pdc_regs[i]);
		writel(0, pdc + PDC_COLOR_LUT_IDX);
		for (i = 0; i < PDC_LUT_SIZE; i++)
			writel(ctr_gfx.lut[n][i], pdc + PDC_COLOR_LUT_DATA);
		writel((ctr_gfx.pdc_swap[n] & PDC_SWAP_NEXT) | PDC_SWAP_IRQ_ACK_ALL,
		       pdc + PDC_SWAP);
		writel(ctr_gfx.pdc_cnt[n], pdc + PDC_CNT);
	}
}

/*
 * Screen-off sleep: wait in WFI until the MCU interrupt line is asserted
 * (the lid driver has masked every MCU interrupt but "shell opened").
 *
 * WFI only returns for an interrupt that reaches the CPU interface, and by
 * now every device interrupt is suspended: the MCU's GPIO interrupt may be
 * masked already if it fired during the suspend.  So the private timer runs
 * as a heartbeat (per-CPU interrupts are never suspended) and the line is
 * checked by reading the pin after every wake-up, not by its interrupt.
 */
static int ctr_pm_enter_light(void)
{
	u32 load = readl(ctr_twd + TWD_LOAD);
	u32 ctrl = readl(ctr_twd + TWD_CTRL);
	unsigned long wakeups = 0;

	writel(0, ctr_twd + TWD_CTRL);
	writel(1, ctr_twd + TWD_INTSTAT);
	writel(CTR_LIGHT_SLEEP_TICK, ctr_twd + TWD_LOAD);
	writel(CTR_LIGHT_SLEEP_TICK, ctr_twd + TWD_COUNTER);
	writel(TWD_CTRL_EN | TWD_CTRL_AUTO_RELOAD | TWD_CTRL_IRQ_EN,
	       ctr_twd + TWD_CTRL);

	while (readw(ctr_gpio3) & GPIO3_MCU_IRQ) {
		dsb();
		cpu_do_idle();
		writel(1, ctr_twd + TWD_INTSTAT);
		writel(GIC_PPI_TWD, ctr_gic_pending_clear);
		wakeups++;
	}

	/* timekeeping_resume() reprograms the clockevent; leave it as found */
	writel(0, ctr_twd + TWD_CTRL);
	writel(1, ctr_twd + TWD_INTSTAT);
	writel(GIC_PPI_TWD, ctr_gic_pending_clear);
	writel(load, ctr_twd + TWD_LOAD);
	writel(ctrl, ctr_twd + TWD_CTRL);

	pr_info("ctr-pm: woke up (screen-off sleep, MCU line after %lu heartbeats)\n",
		wakeups);
	return 0;
}

static int ctr_pm_enter(suspend_state_t state)
{
	u32 wake_enable, reason;

	/* an MCU event (lid opened, button) raced with the suspend: stay up */
	if (!(readw(ctr_gpio3) & GPIO3_MCU_IRQ)) {
		pr_info("ctr-pm: MCU interrupt pending, not sleeping\n");
		return -EBUSY;
	}

	if (!ctr_deep_sleep)
		return ctr_pm_enter_light();

	ctr_gfx_sleep();

	/* clear stale wake reasons, then arm only lid-open and the MCU line */
	reason = readl(ctr_pdn + PDN_WAKE_REASON);
	if (reason)
		writel(reason, ctr_pdn + PDN_WAKE_REASON);
	wake_enable = readl(ctr_pdn + PDN_WAKE_ENABLE);
	writel(PDN_WAKE_SHELL_OPENED | PDN_WAKE_GPIO_3_9,
	       ctr_pdn + PDN_WAKE_ENABLE);

	flush_cache_all();
	if (ctr_scu_base)
		scu_power_mode(ctr_scu_base, SCU_PM_DORMANT);

	/* keep the VRAM-off status bit, assert only SLEEP */
	writew((readw(ctr_pdn + PDN_CNT) & PDN_CNT_VRAM_OFF) | PDN_CNT_SLEEP,
	       ctr_pdn + PDN_CNT);
	dsb();
	isb();
	cpu_do_idle();

	/* PDN wake: execution continues here; rejoin coherency first */
	if (ctr_scu_base)
		scu_power_mode(ctr_scu_base, SCU_PM_NORMAL);

	reason = readl(ctr_pdn + PDN_WAKE_REASON);
	if (reason)
		writel(reason, ctr_pdn + PDN_WAKE_REASON);
	writel(wake_enable, ctr_pdn + PDN_WAKE_ENABLE);

	ctr_gfx_wake();

	pr_info("ctr-pm: woke up (PDN wake reason %#x%s%s)\n", reason,
		(reason & PDN_WAKE_SHELL_OPENED) ? ", shell opened" : "",
		(reason & PDN_WAKE_GPIO_3_9) ? ", MCU" : "");
	return 0;
}

static const struct platform_suspend_ops ctr_pm_ops = {
	.valid	= suspend_valid_only_mem,
	.enter	= ctr_pm_enter,
};

static int __init ctr_pm_init(void)
{
	struct device_node *np;

	if (!of_machine_is_compatible("nintendo,3ds"))
		return 0;

	if (!ctr_scu_base) {
		np = of_find_compatible_node(NULL, NULL, "arm,arm11mp-scu");
		if (np) {
			ctr_scu_base = of_iomap(np, 0);
			of_node_put(np);
		}
	}

	ctr_pdn = ioremap(PDN_BASE, 0x400);
	ctr_gx = ioremap(GX_BASE, 0x600);
	ctr_gpio3 = ioremap(GPIO3_DATA, 2);
	ctr_twd = ioremap(TWD_BASE, 0x20);
	ctr_gic_pending_clear = ioremap(GIC_DIST_PENDING_CLEAR, 4);
	if (!ctr_pdn || !ctr_gx || !ctr_gpio3 || !ctr_twd || !ctr_gic_pending_clear) {
		pr_err("ctr-pm: cannot map PDN/GX/GPIO registers, no sleep mode\n");
		return -ENOMEM;
	}
	ctr_vram_init();

	suspend_set_ops(&ctr_pm_ops);
	pr_info("ctr-pm: sleep mode ready: %s (%d VRAM buffer%s kept for deep sleep)\n",
		ctr_deep_sleep ? "deep PDN sleep" : "screen-off sleep (ctr_deep_sleep=1 for PDN sleep)",
		ctr_vram_count, ctr_vram_count == 1 ? "" : "s");
	return 0;
}
late_initcall(ctr_pm_init);
#endif /* CONFIG_SUSPEND */
