// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctr_cpufreq.c - Nintendo 3DS (New 3DS LGR1/LGR2) CPU clock + cpufreq
 *
 * Part of the Android port to the Nintendo 3DS.
 *
 *
 * WHY THIS DRIVER EXISTS
 * ----------------------
 * The New 3DS ARM11 can run at three clock rates.  The rate is selected by
 * PDN_LGR_SOCMODE (0x10141300, also called CFG11_MPCORE_CLKCNT), whose bits
 * 0-2 are the SoC mode:
 *
 *   0 = O3DS        + 268 MHz
 *   1 = LGR2 (retail New 3DS) + 268 MHz
 *   5 = LGR2                  + 804 MHz   (3x)
 *   2 = LGR1 (prototype)      + 268 MHz
 *   3 = LGR1                  + 536 MHz   (2x)
 *
 * firm_linux_loader upclocks to the fast mode, then downclocks to 268 MHz to
 * bring up cores 2/3 and never restores the fast mode, so Linux runs at
 * 268 MHz.  A 3x CPU speedup is the single biggest performance win available
 * for this port.
 *
 *
 * THE TWD TIMER IS THE TRICKY PART
 * --------------------------------
 * The ARM11 MPCore TWD timer (the kernel's clockevent) is clocked from
 * PERIPHCLK, which the ARM11 MPCore TRM specifies as the CPU clock / 2.
 * Raising the CPU clock therefore also raises the timer clock, so the
 * clockevent must be told about the new rate or jiffies (and every timeout
 * derived from it) runs 3x fast.  The mainline TWD driver
 * (arch/arm/kernel/smp_twd.c) already handles this through the common clock
 * framework's rate-change notifier, so this driver exposes the CPU clock as a
 * real clk and lets a "fixed-factor-clock" (/2) child drive the timer's
 * notifier.  See the device tree bindings "nintendo,3ds-cpuclk" and
 * "3ds:twd".
 *
 *
 * THE SWITCH HANDSHAKE
 * --------------------
 * All powered-on cores must be in WFI when the mode is written.  The write is
 * followed by a WFI loop until bit 15 (change acknowledge) reads back set;
 * writing the value back clears it.  IRQ 88 exists to wake the core, but the
 * periodic tick is enough, so no IRQ plumbing is needed (and maxcpus=1 means
 * core 0 is the only one running - the bootloader leaves cores 1-3 parked in
 * WFI).
 */

#define pr_fmt(fmt) "ctr-cpufreq: " fmt

#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define CTR_SOCMODE_MASK	0x0007
#define CTR_SOCMODE_ACK		BIT(15)

/* ARM11 base clock: exactly twice the 134.055928 MHz ARM9 clock. */
#define CTR_CPU_BASE_HZ		268111856UL

/* ------------------------------------------------------------------------ */
/* common clock provider                                                    */
/* ------------------------------------------------------------------------ */

struct ctr_cpuclk {
	struct clk_hw	hw;
	void __iomem	*base;
	u8		mode_slow;
	u8		mode_fast;
	u8		current_mode;
	unsigned long	rate_slow;
	unsigned long	rate_fast;
};

static struct ctr_cpuclk *ctr_cpu;
static struct device_node *ctr_cpu_np;

static inline struct ctr_cpuclk *to_ctr_cpuclk(struct clk_hw *hw)
{
	return container_of(hw, struct ctr_cpuclk, hw);
}

static unsigned long ctr_cpuclk_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	struct ctr_cpuclk *c = to_ctr_cpuclk(hw);

	/*
	 * Report the last *successfully* switched mode, not the raw register:
	 * a switch that timed out leaves the requested value in the register
	 * but the hardware is still at the old clock, and the TWD must not be
	 * told the wrong rate.
	 */
	return c->current_mode == c->mode_fast ? c->rate_fast : c->rate_slow;
}

static int ctr_cpuclk_determine_rate(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	struct ctr_cpuclk *c = to_ctr_cpuclk(hw);

	req->rate = req->rate >= (c->rate_slow + c->rate_fast) / 2 ?
		c->rate_fast : c->rate_slow;
	return 0;
}

static int ctr_cpuclk_wait_ack(struct ctr_cpuclk *c)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(100);

	while (!(readw(c->base) & CTR_SOCMODE_ACK)) {
		if (time_after(jiffies, timeout)) {
			pr_err("clock switch timed out (socmode=0x%04x)\n",
			       readw(c->base));
			return -ETIMEDOUT;
		}
		/*
		 * The switch only takes effect while the core is in WFI.  The
		 * periodic tick (and IRQ 88, if it ever becomes enabled) wakes
		 * us again; we simply re-check the ack bit.
		 */
		wfi();
	}
	return 0;
}

static int ctr_cpuclk_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	struct ctr_cpuclk *c = to_ctr_cpuclk(hw);
	u16 raw;
	u8 old_mode = c->current_mode;
	u8 mode;

	mode = rate >= (c->rate_slow + c->rate_fast) / 2 ?
		c->mode_fast : c->mode_slow;

	if (mode == old_mode)
		return 0;

	raw = readw(c->base);
	writew((raw & ~CTR_SOCMODE_MASK) | mode, c->base);
	/* make sure the write reached the peripheral before entering WFI */
	readw(c->base);

	if (ctr_cpuclk_wait_ack(c)) {
		/*
		 * The switch did not complete.  Put the register back and keep
		 * reporting the old rate so the TWD timer is left alone.
		 * (This is what happens today on the New 3DS: the bootloader
		 * parks cores 1-3 in a wfe loop, but the SoC mode switch needs
		 * every powered-on core in wfi, so a runtime switch from Linux
		 * cannot complete.  The upclock is done in firm_linux_loader
		 * instead, before the secondaries are released.)
		 */
		writew((readw(c->base) & ~CTR_SOCMODE_MASK) | old_mode,
		       c->base);
		readw(c->base);
		return -ETIMEDOUT;
	}

	/* write the ack bit back to clear it */
	writew(readw(c->base), c->base);

	c->current_mode = mode;
	pr_info("ARM11 clock now %lu MHz (socmode %u)\n",
		(mode == c->mode_fast ? c->rate_fast : c->rate_slow) / 1000000,
		mode);
	return 0;
}

static const struct clk_ops ctr_cpuclk_ops = {
	.recalc_rate	= ctr_cpuclk_recalc_rate,
	.determine_rate	= ctr_cpuclk_determine_rate,
	.set_rate	= ctr_cpuclk_set_rate,
};

static void __init ctr_cpuclk_init(struct device_node *np)
{
	struct ctr_cpuclk *c;
	struct clk_init_data init = { };
	void __iomem *socinfo;
	u16 info;
	int ret;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return;

	c->base = of_iomap(np, 0);
	socinfo = of_iomap(np, 1);
	if (!c->base || !socinfo) {
		pr_err("failed to map registers\n");
		goto err;
	}
	info = readw(socinfo);
	iounmap(socinfo);

	if (info & BIT(2)) {		/* retail New 3DS (LGR2) */
		c->mode_slow = 1;
		c->mode_fast = 5;
		c->rate_slow = CTR_CPU_BASE_HZ;
		c->rate_fast = CTR_CPU_BASE_HZ * 3;
	} else if (info & BIT(1)) {	/* prototype New 3DS (LGR1) */
		c->mode_slow = 2;
		c->mode_fast = 3;
		c->rate_slow = CTR_CPU_BASE_HZ;
		c->rate_fast = CTR_CPU_BASE_HZ * 2;
	} else {			/* Old 3DS: fixed 268 MHz */
		c->mode_slow = 0;
		c->mode_fast = 0;
		c->rate_slow = CTR_CPU_BASE_HZ;
		c->rate_fast = CTR_CPU_BASE_HZ;
	}

	/*
	 * Sample the mode the bootloader left us in *before* registering the
	 * clock: the clk core caches recalc_rate()'s answer, and the TWD timer
	 * child must see the real (post-bootloader) rate.
	 */
	c->current_mode = readw(c->base) & CTR_SOCMODE_MASK;

	init.name = "cpu";
	init.ops = &ctr_cpuclk_ops;
	init.flags = CLK_IS_CRITICAL;
	init.num_parents = 0;
	c->hw.init = &init;

	ret = clk_hw_register(NULL, &c->hw);
	if (ret) {
		pr_err("failed to register clock: %d\n", ret);
		goto err;
	}

	ret = of_clk_add_hw_provider(np, of_clk_hw_simple_get, &c->hw);
	if (ret) {
		pr_err("failed to add clock provider: %d\n", ret);
		clk_hw_unregister(&c->hw);
		goto err;
	}

	ctr_cpu = c;
	ctr_cpu_np = np;
	pr_info("registered: %lu/%lu MHz, running at %lu MHz (SOCINFO=0x%x)\n",
		c->rate_slow / 1000000, c->rate_fast / 1000000,
		ctr_cpuclk_recalc_rate(&c->hw, 0) / 1000000, info);
	return;

err:
	if (c->base)
		iounmap(c->base);
	kfree(c);
}
CLK_OF_DECLARE(ctr_cpuclk, "nintendo,3ds-cpuclk", ctr_cpuclk_init);

/* ------------------------------------------------------------------------ */
/* cpufreq driver                                                           */
/* ------------------------------------------------------------------------ */

static struct cpufreq_frequency_table ctr_freq_table[3];

static irqreturn_t ctr_clk_irq(int irq, void *data)
{
	/* The ack bit is polled; this only enables the wake-up line. */
	return IRQ_HANDLED;
}

static int ctr_cpufreq_init(struct cpufreq_policy *policy)
{
	policy->clk = ctr_cpu->hw.clk;
	cpufreq_generic_init(policy, ctr_freq_table, 20 * 1000); /* 20 us */
	return 0;
}

static int ctr_cpufreq_target_index(struct cpufreq_policy *policy,
				    unsigned int index)
{
	unsigned long freq = policy->freq_table[index].frequency;

	return clk_set_rate(policy->clk, freq * 1000);
}

static struct cpufreq_driver ctr_cpufreq_driver = {
	.name		= "ctr-cpufreq",
	.flags		= CPUFREQ_NEED_INITIAL_FREQ_CHECK,
	.init		= ctr_cpufreq_init,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= ctr_cpufreq_target_index,
	.get		= cpufreq_generic_get,
	.attr		= cpufreq_generic_attr,
};

static int __init ctr_cpufreq_register(void)
{
	/* Nothing to scale on a fixed-clock (old 3DS) system. */
	if (!ctr_cpu || ctr_cpu->rate_fast == ctr_cpu->rate_slow)
		return 0;

	/*
	 * IRQ 88 (the PDN clock-change interrupt) is the wake-up source the
	 * bootloader enables around its own switch.  Enable it here too so a
	 * runtime switch has a chance to complete; it is only a wake-up, the
	 * ack bit itself is polled.
	 */
	if (ctr_cpu_np) {
		int irq = irq_of_parse_and_map(ctr_cpu_np, 0);

		if (irq > 0 && request_irq(irq, ctr_clk_irq, IRQF_NO_THREAD,
					   "ctr-cpufreq", NULL))
			pr_warn("could not claim clk-change IRQ %d\n", irq);
	}

	ctr_freq_table[0].frequency = ctr_cpu->rate_slow / 1000;
	ctr_freq_table[1].frequency = ctr_cpu->rate_fast / 1000;
	ctr_freq_table[2].frequency = CPUFREQ_TABLE_END;

	pr_info("registering cpufreq driver (%u-%u kHz)\n",
		ctr_freq_table[0].frequency, ctr_freq_table[1].frequency);
	return cpufreq_register_driver(&ctr_cpufreq_driver);
}
device_initcall(ctr_cpufreq_register);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Nintendo 3DS ARM11 CPU clock and cpufreq driver");
