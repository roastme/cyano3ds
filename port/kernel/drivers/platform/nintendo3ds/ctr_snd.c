// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctr_snd.c - Nintendo 3DS audio
 *
 * The 3DS has two independent audio engines feeding the same TSC2117/AIC3010
 * codec over two I2S lines (3dbrew: I2S_Registers):
 *
 *   - I2S1: the (undocumented, firmware-driven) XpertTeak DSP, the microphone
 *           and the legacy GBA/DS sound hardware;
 *   - I2S2: the CSND hardware - the DSi "sound" engine: 32 DMA channels that
 *           read PCM8/PCM16/IMA-ADPCM/PSG sample data straight out of main
 *           memory, with a hardware sample-rate converter per channel.
 *
 * This driver uses CSND/I2S2, i.e. no DSP firmware is required and the ARM11
 * can play PCM on its own.
 *
 * It is split in two halves:
 *
 *   ctr_snd_codec_init(map)  - the codec bring-up.  Called by ctr_tsc's probe
 *                              (see port/scripts/fix-tsc-snd.py) *before* the
 *                              touchscreen child is populated, exactly like
 *                              the OEM codec module initialises sound first
 *                              and touch after.  Register sequences are ported
 *                              from profi200's open_agb_firm libn3ds
 *                              (drivers/codec.c), GPLv2, which runs the same
 *                              silicon bare-metal.
 *
 *   the platform driver       - CSND programming + an ALSA PCM playback
 *                              device, plus a short boot-time test tone so the
 *                              hardware path can be verified without a
 *                              userspace audio stack.
 *
 * The ALSA ring buffer doubles as the CSND DMA buffer: the codec plays it in a
 * hardware loop while the application fills it ahead of the read pointer.
 */

#define DRIVER_NAME "3ds-snd"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

/* ------------------------------------------------------------------------- */
/* Hardware addresses                                                        */
/* ------------------------------------------------------------------------- */

#define CSND_BASE		0x10103000
#define CSND_SIZE		0x1000
#define I2S_BASE		0x10145000
#define I2S_SIZE		0x8
#define PDN_I2S_CNT		0x10141220	/* PDN_REGS_BASE + 0x220 */

/* CSND global registers */
#define CSND_MASTER_VOL		0x000
#define CSND_CNT		0x002
#define CSND_CH_BASE		0x400
#define CSND_CH_STRIDE		0x20

#define CSND_CNT_MUTE		BIT(0)
#define CSND_CNT_RS_FILTER_EN	BIT(14)
#define CSND_CNT_EN		BIT(15)

/* CSND channel register offsets (relative to the channel slot) */
#define CSNDCH_CNT		0x00
#define CSNDCH_SR		0x02
#define CSNDCH_VOL		0x04	/* u32: R in [15:0], L in [31:16] */
#define CSNDCH_CAPVOL		0x08
#define CSNDCH_ST_ADDR		0x0C
#define CSNDCH_SIZE		0x10
#define CSNDCH_LP_ADDR		0x14
#define CSNDCH_ST_ADPCM		0x18
#define CSNDCH_LP_ADPCM		0x1C

#define CSND_CH_LERP		BIT(6)
#define CSND_CH_RPT_LOOP	(1u << 10)
#define CSND_CH_RPT_ONE_SHOT	(2u << 10)
#define CSND_CH_FMT_PCM8	(0u << 12)
#define CSND_CH_FMT_PCM16	(1u << 12)
#define CSND_CH_PLAYING		BIT(14)
#define CSND_CH_START		BIT(15)

/* I2S2_CNT (the CSND line) */
#define I2S2_FREQ_47KHZ		BIT(13)
#define I2S2_MCLK2_16MHZ	BIT(14)
#define I2S2_EN			BIT(15)

/* PDN I2S clock control */
#define PDN_I2S_CNT_CLK2_EN	BIT(1)

/*
 * The codec is a TSC2117/AIC3010 accessed through the TSC SPI controller.
 * ctr_tsc's regmap packs the register as [page:index] in a 15-bit field (the
 * bus splits it back into a bank select + a 7-bit register address); the touch
 * driver uses exactly the same encoding for its bank 0x67 registers.
 */
#define CDC(page, reg)		(((page) << 7) | (reg))

/*
 * Codec pages/registers used here.  Pages 100/101/103/251/255 are CTR ("3DS")
 * specific; open_agb_firm names them CDC_REG_100_xx etc.
 */
#define CDC_SOFT_RST_CTR	CDC(100, 1)
#define CDC_100_34		CDC(100, 34)	/* PLL enable / status */
#define CDC_100_37		CDC(100, 37)	/* DAC status flags */
#define CDC_100_38		CDC(100, 38)
#define CDC_100_49		CDC(100, 49)	/* I2S mute/volume ctrl */
#define CDC_100_67		CDC(100, 67)	/* headset detect timing */
#define CDC_HEADSET_SEL		CDC(100, 69)
#define CDC_100_117		CDC(100, 117)
#define CDC_100_118		CDC(100, 118)	/* DAC power */
#define CDC_100_119		CDC(100, 119)	/* I2S2 mute */
#define CDC_100_120		CDC(100, 120)	/* I2S2 volume */
#define CDC_100_121		CDC(100, 121)
#define CDC_100_122		CDC(100, 122)	/* I2S1 volume */
#define CDC_100_123		CDC(100, 123)	/* I2S2 digital volume */
#define CDC_100_124		CDC(100, 124)	/* I2S2 frequency select */

#define CDC_101_10		CDC(101, 10)	/* DAC routing */
#define CDC_101_11		CDC(101, 11)	/* headphone driver power */
#define CDC_101_12		CDC(101, 12)	/* headphone unmute */
#define CDC_101_17		CDC(101, 17)	/* speaker amp power */
#define CDC_101_18		CDC(101, 18)	/* speaker L unmute */
#define CDC_101_19		CDC(101, 19)	/* speaker R unmute */
#define CDC_101_22		CDC(101, 22)	/* analog vol HPL */
#define CDC_101_23		CDC(101, 23)	/* analog vol HPR */
#define CDC_101_27		CDC(101, 27)	/* analog vol SPL */
#define CDC_101_28		CDC(101, 28)	/* analog vol SPR */
#define CDC_101_119		CDC(101, 119)
#define CDC_101_122		CDC(101, 122)	/* VREF */

#define CDC_0_2			CDC(0, 2)	/* vendor id */
#define CDC_0_3			CDC(0, 3)	/* revision */
#define CDC_DAC_NDAC_VAL	CDC(0, 11)
#define CDC_GPI1_GPI2_PIN_CTRL	CDC(0, 57)
#define CDC_DAC_DATA_PATH_SETUP	CDC(0, 63)
#define CDC_DAC_VOLUME_CTRL	CDC(0, 64)
#define CDC_DAC_L_VOLUME_CTRL	CDC(0, 65)
#define CDC_DAC_R_VOLUME_CTRL	CDC(0, 66)

/*
 * Codec calibration: the neutral values Android3DS/Octoblimp's ctr_csnd.c
 * (ported from Aurora3DS's Audio11.c, which is verified to make sound on this
 * hardware) uses instead of the per-console HWCAL block.
 */
#define DRIVER_GAIN_HP		1
#define DRIVER_GAIN_SP		1
#define ANALOG_VOLUME_HP	0
#define ANALOG_VOLUME_SP	0

/* I2S1_CNT bit 12: microphone timing (needed by the MIC block) */
#define I2S1_MIC_TIMING		BIT(12)

/*
 * Both I2S lines run at 32.73 kHz, as in Octoblimp (N3DS_MIC_CLOCK_32K): the
 * microphone's sample clock is a divider of it, and with line 2 at 47.61 kHz
 * the MIC block delivered half the documented rate.  The CSND resamples
 * every stream to whatever line 2 runs at, so playback works either way.
 */
static bool i2s2_47k;
module_param(i2s2_47k, bool, 0444);
MODULE_PARM_DESC(i2s2_47k, "run the CSND I2S line at 47.61 kHz instead of 32.73 kHz");

/* ------------------------------------------------------------------------- */
/* Codec helpers                                                             */
/* ------------------------------------------------------------------------- */

static int cdc_w(struct regmap *map, u16 pr, u8 val)
{
	return regmap_write(map, pr, val);
}

static int cdc_r(struct regmap *map, u16 pr, u8 *val)
{
	unsigned int tmp;
	int err = regmap_read(map, pr, &tmp);
	*val = tmp & 0xff;
	return err;
}

static int cdc_mask(struct regmap *map, u16 pr, u8 val, u8 mask)
{
	u8 d;
	int err = cdc_r(map, pr, &d);
	if (err)
		return err;
	d = (d & ~mask) | (val & mask);
	return cdc_w(map, pr, d);
}

static int cdc_wait_mask(struct regmap *map, u16 pr, u8 val, u8 mask)
{
	int i;
	for (i = 0; i < 100; i++) {
		u8 d;
		int err = cdc_r(map, pr, &d);
		if (err)
			return err;
		if ((d & mask) == val)
			return 0;
		usleep_range(1000, 2000);
	}
	return 0;
}

static int ctr_set_i2s_freq(struct regmap *map, int line, bool khz47)
{
	if (line == 1) {
		/* I2S1 / DAC: the NDAC divider. */
		return cdc_w(map, CDC_DAC_NDAC_VAL, khz47 ? 0x85 : 0x87);
	}
	/* I2S2: bit 0 of page 100 reg 124. */
	return cdc_mask(map, CDC_100_124, khz47 ? 0 : 1, 1);
}

static void ctr_pdn_mclk(bool enable)
{
	void __iomem *reg = ioremap(PDN_I2S_CNT, 1);
	if (!reg)
		return;
	writeb(enable ? PDN_I2S_CNT_CLK2_EN : 0, reg);
	iounmap(reg);
}

/*
 * Codec bring-up.  Runs from ctr_tsc's probe, after its chip reset and
 * before the touchscreen child is populated.
 *
 * This is the analog output sequence of Android3DS/Octoblimp's ctr_csnd.c
 * codec_init(), a one-for-one port of Aurora3DS's Audio11.c (itself from
 * profi200's libn3ds codec.c), which is verified to produce sound on this
 * hardware.  The order of the writes is load-bearing.  It does no second
 * (CTR) soft reset, uploads no EQ
 * coefficients, does not touch p100.49/p100.123, runs I2S line 2 at
 * 32.73 kHz and uses Octoblimp's speaker/headphone gains.
 */
int ctr_snd_codec_init(struct regmap *map)
{
	u8 id = 0, rev = 0, hpval;
	int err;
	void __iomem *i2s;

	pr_info("initialising TSC2117/AIC3010 audio codec (Octoblimp sequence)\n");

	/* Turn the codec MCLK on (the same bit ctr_tsc already sets). */
	ctr_pdn_mclk(true);

	err = cdc_r(map, CDC_0_2, &id);
	if (!err)
		err = cdc_r(map, CDC_0_3, &rev);
	if (err)
		return err;
	pr_info("codec vendor/rev %02x/%02x\n", id, rev);
	/*
	 * Octoblimp refuses to program a codec whose ID reads all-0 or all-1
	 * (a dead SPI link).  This driver only warns: the 3Beans emulator does
	 * not model the ID registers but does gate CSND output on the I2S
	 * setup below.  Real consoles read 11/36 here.
	 */
	if ((id == 0x00 && rev == 0x00) || (id == 0xff && rev == 0xff))
		pr_warn("codec ID/rev reads all-0x%02x: SPI link to the codec not working? programming it anyway\n",
			id);

	/* Headset detection timing + reset the detection state machine. */
	cdc_w(map, CDC_100_67, 0x11);
	cdc_mask(map, CDC_101_119, 1, 1);
	/* Don't force the GPIO pins; VREF on; PLL on. */
	cdc_mask(map, CDC_GPI1_GPI2_PIN_CTRL, 0x66, 0x66);
	cdc_w(map, CDC_101_122, 1);
	cdc_mask(map, CDC_100_34, 0x18, 0x18);

	/* Force the speaker path rather than reading the headphone-jack GPIO. */
	cdc_mask(map, CDC_HEADSET_SEL, 0x20, 0x30);
	cdc_mask(map, CDC_100_67, 0, 0x80);
	cdc_mask(map, CDC_100_67, 0x80, 0x80);

	/* Codec-side I2S dividers: line 1 32 kHz, line 2 32 kHz (or 47 kHz). */
	ctr_set_i2s_freq(map, 1, false);
	ctr_set_i2s_freq(map, 2, i2s2_47k);

	/* SoC I2S controller.  Bits 12-14 are only writable while disabled. */
	i2s = ioremap(I2S_BASE, I2S_SIZE);
	if (!i2s)
		return -ENOMEM;
	writew(0, i2s + 0);
	writew(0, i2s + 2);
	/* I2S1: enable | 16 MHz MCLK | 32.7 kHz | mic timing | legacy vol 32 */
	writew(I2S2_EN | I2S2_MCLK2_16MHZ | I2S1_MIC_TIMING | (32 << 6), i2s + 0);
	/* I2S2 (CSND): enable | 16 MHz MCLK | 32.7 kHz (47.6 kHz if asked) */
	writew(I2S2_EN | I2S2_MCLK2_16MHZ | (i2s2_47k ? I2S2_FREQ_47KHZ : 0),
	       i2s + 2);
	pr_info("I2S1_CNT=0x%04x I2S2_CNT=0x%04x (line 2 at %s kHz)\n",
		readw(i2s + 0), readw(i2s + 2), i2s2_47k ? "47.61" : "32.73");
	iounmap(i2s);

	/* Output stage: power the DAC, unmute both lines, power the drivers. */
	cdc_mask(map, CDC_101_17, 0x10, 0x1C);
	cdc_w(map, CDC_100_122, 0);	/* I2S1 volume 0 dB */
	cdc_w(map, CDC_100_120, 0);	/* I2S2 volume 0 dB */
	cdc_mask(map, CDC_100_118, 0xC0, 0xC0);
	msleep(10);
	cdc_wait_mask(map, CDC_100_37, 0x88, 0x88);
	cdc_w(map, CDC_101_10, 0xA);

	cdc_mask(map, CDC_DAC_DATA_PATH_SETUP, 0xC0, 0xC0);	/* I2S1 unmute */
	cdc_w(map, CDC_DAC_VOLUME_CTRL, 0);
	cdc_mask(map, CDC_100_119, 0, 0xC);			/* I2S2 unmute */

	/* Headphone driver.  Two vendor/revision variants exist. */
	if ((id & 0xF) <= 1 && ((rev & 0x70) >> 4) <= 2)
		hpval = 0x3C;
	else
		hpval = 0x1C;
	cdc_w(map, CDC_101_11, hpval);
	cdc_w(map, CDC_101_12, (DRIVER_GAIN_HP << 3) | 4);
	cdc_w(map, CDC_101_22, ANALOG_VOLUME_HP);
	cdc_w(map, CDC_101_23, ANALOG_VOLUME_HP);

	/* Speaker driver: power on, unmute, analog volume. */
	cdc_mask(map, CDC_101_17, 0xC0, 0xC0);
	cdc_w(map, CDC_101_18, (DRIVER_GAIN_SP << 2) | 2);
	cdc_w(map, CDC_101_19, (DRIVER_GAIN_SP << 2) | 2);
	cdc_w(map, CDC_101_27, ANALOG_VOLUME_SP);
	cdc_w(map, CDC_101_28, ANALOG_VOLUME_SP);

	/* Let the headphone/speaker outputs settle. */
	msleep(38);

	cdc_r(map, CDC_101_11, &id);
	pr_info("codec ready (readback p101.11 = 0x%02x, wrote 0x%02x)\n", id, hpval);
	return 0;
}
EXPORT_SYMBOL_GPL(ctr_snd_codec_init);

/* ------------------------------------------------------------------------- */
/* CSND                                                                      */
/* ------------------------------------------------------------------------- */

static inline void csnd_w16(void __iomem *base, u32 off, u16 v)
{
	writew(v, base + off);
}

static inline u16 csnd_r16(void __iomem *base, u32 off)
{
	return readw(base + off);
}

static inline void csnd_w32(void __iomem *base, u32 off, u32 v)
{
	writel(v, base + off);
}

static inline u32 csnd_r32(void __iomem *base, u32 off)
{
	return readl(base + off);
}

static inline u32 csnd_ch_off(unsigned int ch)
{
	return CSND_CH_BASE + ch * CSND_CH_STRIDE;
}

/*
 * Sample-rate register value.  The CSND counter ticks once per 16-bit sample
 * and the register is 0x10000 - clock/rate with the CSND clock at 67.027964
 * MHz -- the same value libctru/libn3ds use.  This driver feeds the channel a
 * mono buffer, so one tick is one frame and the requested rate is programmed
 * as-is.  (It is *not* a stereo frame rate: feeding the interleaved stereo
 * ring here with the frame rate made the ring drain at exactly half speed,
 * measured from the live st_addr position.)
 */
static u16 csnd_sr_reg(unsigned int sample_rate)
{
	/* rounded: 44100 Hz -> 1520 = 44097 Hz (truncating gave 44126 Hz) */
	u32 div = DIV_ROUND_CLOSEST(67027964u, sample_rate);
	if (div > 0xffff)
		div = 0xffff;
	return (u16)(0x10000 - div);
}

static void csnd_init(void __iomem *base)
{
	unsigned int i;

	csnd_w16(base, CSND_MASTER_VOL, 0x8000);
	csnd_w16(base, CSND_CNT, CSND_CNT_EN | CSND_CNT_RS_FILTER_EN);

	for (i = 0; i < 32; i++)
		csnd_w16(base, csnd_ch_off(i) + CSNDCH_CNT, 0);
	csnd_w16(base, 0x800, 0);	/* capture 0 */
	csnd_w16(base, 0x810, 0);	/* capture 1 */
}

static void csnd_stop_ch(void __iomem *base, unsigned int ch)
{
	csnd_w16(base, csnd_ch_off(ch) + CSNDCH_CNT, 0);
}

static void csnd_setup_ch(void __iomem *base, unsigned int ch,
			  unsigned int rate, phys_addr_t addr, u32 size,
			  u16 flags)
{
	u32 off = csnd_ch_off(ch);

	csnd_w16(base, off + CSNDCH_SR, csnd_sr_reg(rate));
	csnd_w32(base, off + CSNDCH_VOL, 0x80008000);	/* full L/R */
	csnd_w32(base, off + CSNDCH_CAPVOL, 0x80008000);
	csnd_w32(base, off + CSNDCH_ST_ADDR, (u32)addr);
	csnd_w32(base, off + CSNDCH_SIZE, size);
	csnd_w32(base, off + CSNDCH_LP_ADDR, (u32)addr);
	csnd_w32(base, off + CSNDCH_ST_ADPCM, 0);
	csnd_w32(base, off + CSNDCH_LP_ADPCM, 0);
	csnd_w16(base, off + CSNDCH_CNT, CSND_CH_START | flags);
}

static void csnd_play(void __iomem *base, unsigned int ch, bool playing)
{
	u32 off = csnd_ch_off(ch);
	u16 cnt = csnd_r16(base, off + CSNDCH_CNT);

	cnt = (cnt & ~CSND_CH_PLAYING) | (playing ? CSND_CH_PLAYING : 0);
	csnd_w16(base, off + CSNDCH_CNT, cnt);
}

/* ------------------------------------------------------------------------- */
/* ALSA PCM                                                                  */
/* ------------------------------------------------------------------------- */

#define CTR_SND_MAX_BUFFER	(128 * 1024)
#define CTR_SND_PERIOD_MIN	512
#define CTR_SND_CHANNEL		0
#define CTR_SND_OUT_REGS	5
#define CTR_SND_OUTMON_MS	100	/* output-stage check period */
#define CTR_SND_OUTMON_MAX	20	/* re-arms per boot */

struct ctr_snd {
	struct device *dev;
	void __iomem *csnd;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *substream;

	struct hrtimer timer;
	ktime_t base_time;		/* stream start, for the wall-clock pointer */
	ktime_t period_time;
	unsigned int rate;
	unsigned int period_frames;
	unsigned int buffer_frames;
	unsigned long pos;		/* last reported frame position */
	unsigned int hw_last;		/* previous st_addr reading (frames) */
	bool hw_live;			/* st_addr advances -> real position */
	ktime_t hw_moved;		/* when st_addr last changed */
	bool hw_cand;			/* st_addr rate measurement running */
	ktime_t hw_t0;			/* ... since this time */
	unsigned int hw_prev;		/* ... last st_addr reading (frames) */
	u64 hw_total;			/* ... frames st_addr moved since hw_t0 */
	bool hw_rate_warned;		/* logged a failed rate check once */
	unsigned long diag_next;	/* jiffies of next position diagnostic */
	int diag_left;
	atomic_t running;

	/*
	 * A CSND channel plays one 16-bit stream -- it is not an interleaved
	 * stereo engine (libcwav's stereo path is the separate planar
	 * "DirectSound" interface).  The ALSA ring stays interleaved stereo for
	 * Android; copy_user() downmixes it into this mono DMA buffer, which is
	 * what the CSND loops.
	 */
	void *mono_buf;
	dma_addr_t mono_addr;
	unsigned int mono_bytes;

	/* the codec, through ctr_tsc's regmap (the parent device) */
	struct regmap *codec;

	/* output-stage monitor (N3DS_CSND_OUTPUT_REARM) */
	struct delayed_work outmon;
	int out_base[CTR_SND_OUT_REGS];
	bool out_base_ok;
	unsigned int rearms;

	/* microphone capture (N3DS_MIC_REAL) */
	void __iomem *mic;
	struct snd_pcm_substream *cap_ss;
	struct hrtimer cap_timer;
	atomic_t cap_running;
	unsigned int cap_pos;		/* next frame written in the ALSA ring */
	unsigned int cap_period_fill;	/* frames since the last period */
	bool adc_frontend_done;
	int cap_idx;			/* MIC_CNT rate divider index */
	bool cap_idx_auto;
	u8 cap_live;			/* which bits of a FIFO word carry samples */
	s8 cap_shift;
	u32 cap_in_rate;
	u32 rs_step, rs_pos;		/* Q16 resampler */
	s32 rs_prev;
	bool cap_seen_data;
	unsigned int cap_dry_ticks;
	unsigned int cap_overruns;
	/* the format probe over the first MIC_PROBE_WORDS words */
	ktime_t pr_t0;
	u32 pr_words, pr_lo_dead, pr_hi_dead, pr_same;
	u32 pr_ones[32], pr_nsign[32], pr_eq[32];
	bool pr_done;
};

static const struct snd_pcm_hardware ctr_snd_pcm_hw = {
	/*
	 * No MMAP: the CSND plays a downmixed mono buffer, so every write has to
	 * go through ->copy_user().  Android's ALSA HAL uses snd_pcm_writei()
	 * (RW mode) anyway.
	 */
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_8000_48000,
	.rate_min = 8000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = CTR_SND_MAX_BUFFER,
	.period_bytes_min = CTR_SND_PERIOD_MIN * 4,
	.period_bytes_max = CTR_SND_MAX_BUFFER / 2,
	.periods_min = 2,
	.periods_max = 16,
};

/*
 * The period is completed directly from the timer callback.  That is only
 * safe if the timer is a *soft* hrtimer (HRTIMER_MODE_REL_SOFT): its callback
 * then runs in softirq context, which is the documented-safe context for
 * snd_pcm_period_elapsed(), and hrtimer_cancel() from another context can
 * synchronise with it through the softirq expiry lock instead of spinning.
 *
 * snd_pcm_period_elapsed() can stop the stream on an underrun or when a
 * drain completes, which calls our trigger(STOP).  trigger(STOP) must NOT
 * call hrtimer_cancel() while this very callback is executing (it would spin
 * forever waiting for itself), hence the hrtimer_callback_running() guard.
 * This mirrors sound/drivers/dummy.c's proven hrtimer PCM backend.
 */
static enum hrtimer_restart ctr_snd_timer(struct hrtimer *t)
{
	struct ctr_snd *snd = container_of(t, struct ctr_snd, timer);

	if (!atomic_read(&snd->running))
		return HRTIMER_NORESTART;

	snd_pcm_period_elapsed(snd->substream);

	/* The stream may have been stopped by period_elapsed (XRUN/drain). */
	if (!atomic_read(&snd->running))
		return HRTIMER_NORESTART;

	hrtimer_forward_now(t, snd->period_time);
	return HRTIMER_RESTART;
}

static int ctr_snd_pcm_open(struct snd_pcm_substream *ss)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);

	snd->substream = ss;
	snd->pos = 0;
	atomic_set(&snd->running, 0);
	ss->runtime->hw = ctr_snd_pcm_hw;
	return 0;
}

static int ctr_snd_pcm_close(struct snd_pcm_substream *ss)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);

	atomic_set(&snd->running, 0);
	if (!hrtimer_callback_running(&snd->timer))
		hrtimer_cancel(&snd->timer);
	csnd_play(snd->csnd, CTR_SND_CHANNEL, false);
	csnd_stop_ch(snd->csnd, CTR_SND_CHANNEL);
	snd->substream = NULL;
	return 0;
}

static int ctr_snd_pcm_hw_params(struct snd_pcm_substream *ss,
				 struct snd_pcm_hw_params *params)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);
	unsigned int period, rate;
	long sec;
	unsigned long nsecs;
	int err;

	err = snd_pcm_lib_malloc_pages(ss, params_buffer_bytes(params));
	if (err < 0)
		return err;

	/* (Re)allocate the mono buffer the CSND actually reads. */
	if (snd->mono_buf) {
		dma_free_coherent(snd->dev, snd->mono_bytes, snd->mono_buf,
				  snd->mono_addr);
		snd->mono_buf = NULL;
	}
	snd->mono_bytes = params_buffer_bytes(params) / 2;
	snd->mono_buf = dma_alloc_coherent(snd->dev, snd->mono_bytes,
					   &snd->mono_addr, GFP_KERNEL);
	if (!snd->mono_buf) {
		snd_pcm_lib_free_pages(ss);
		return -ENOMEM;
	}

	snd->rate = rate = params_rate(params);
	snd->buffer_frames = params_buffer_size(params);
	snd->period_frames = period = params_period_size(params);

	/* Precompute the period interval once, so the hrtimer callback does no
	 * division and cannot be perturbed by a changing rate. */
	sec = period / rate;
	nsecs = div_u64((u64)(period % rate) * NSEC_PER_SEC + rate - 1, rate);
	snd->period_time = ktime_set(sec, nsecs);

	dev_info(snd->dev, "hw_params: %u Hz, %u frames, %u frame periods, mono_addr=%08x mono_bytes=%u\n",
		 rate, snd->buffer_frames, period,
		 (u32)(phys_addr_t)snd->mono_addr, snd->mono_bytes);
	return 0;
}

static int ctr_snd_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);

	/*
	 * The CSND DMA reads the ALSA ring buffer directly, so the channel must
	 * be stopped and the timer cancelled *before* the pages are freed.  The
	 * ALSA core calls hw_free() before ops->close(), so do the teardown
	 * here as well as in close().
	 */
	atomic_set(&snd->running, 0);
	if (!hrtimer_callback_running(&snd->timer))
		hrtimer_cancel(&snd->timer);
	csnd_play(snd->csnd, CTR_SND_CHANNEL, false);
	csnd_stop_ch(snd->csnd, CTR_SND_CHANNEL);

	if (snd->mono_buf) {
		dma_free_coherent(snd->dev, snd->mono_bytes, snd->mono_buf,
				  snd->mono_addr);
		snd->mono_buf = NULL;
	}

	return snd_pcm_lib_free_pages(ss);
}

static int ctr_snd_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);

	atomic_set(&snd->running, 0);
	if (!hrtimer_callback_running(&snd->timer))
		hrtimer_cancel(&snd->timer);
	csnd_stop_ch(snd->csnd, CTR_SND_CHANNEL);
	/* No stale samples if the stream is started before the first write. */
	if (snd->mono_buf)
		memset(snd->mono_buf, 0, snd->mono_bytes);
	/*
	 * The CSND resamples the mono buffer to the codec's fixed I2S2 clock
	 * (47.6 kHz), so CSND_CH_LERP is needed to interpolate instead of the
	 * default nearest-neighbour drop/repeat.  The mono buffer holds one
	 * 16-bit sample per frame, so the rate is programmed as-is.
	 */
	csnd_setup_ch(snd->csnd, CTR_SND_CHANNEL, snd->rate,
		      (phys_addr_t)snd->mono_addr, snd->mono_bytes,
		      CSND_CH_FMT_PCM16 | CSND_CH_RPT_LOOP | CSND_CH_LERP);
	snd->base_time = ktime_get();
	snd->pos = 0;
	snd->hw_last = 0;
	snd->hw_live = false;
	snd->hw_moved = 0;
	snd->hw_cand = false;
	snd->hw_rate_warned = false;
	return 0;
}

static int ctr_snd_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		snd->base_time = ktime_get();
		atomic_set(&snd->running, 1);
		csnd_play(snd->csnd, CTR_SND_CHANNEL, true);
		snd->diag_next = jiffies + HZ / 2;
		snd->diag_left = 10;
		hrtimer_start(&snd->timer, snd->period_time,
			      HRTIMER_MODE_REL_SOFT);
		if (snd->out_base_ok)
			mod_delayed_work(system_wq, &snd->outmon,
					 msecs_to_jiffies(CTR_SND_OUTMON_MS));
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		atomic_set(&snd->running, 0);
		/*
		 * When period_elapsed() stopped the stream (XRUN/drain) this runs
		 * from the timer callback itself; cancelling then would spin on the
		 * currently-executing callback.  The callback notices running==0
		 * and returns HRTIMER_NORESTART instead.
		 */
		if (!hrtimer_callback_running(&snd->timer))
			hrtimer_cancel(&snd->timer);
		csnd_play(snd->csnd, CTR_SND_CHANNEL, false);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/*
 * The CSND keeps the address of the sample it is currently playing in the
 * channel's st_addr register (the same register the driver programs with the
 * start address; libn3ds' CSND_getChPos() reads it back as the playback
 * position).  When that works we have an exact hardware pointer.
 *
 * It cannot be trusted unconditionally: 3dbrew says the position is only
 * copied out by the CSND *service* (which this driver bypasses, writing the
 * channel registers directly).  So the register is validated first -- it must
 * be observed to advance past the start address.  Until then the pointer is
 * derived from the wall clock, i.e. the sound/drivers/dummy.c hrtimer
 * pattern.
 *
 * A wall-clock pointer is still far better than advancing a counter in the
 * timer callback: the callback is re-armed with
 * hrtimer_forward_now(), so every late expiry permanently pushes the counter
 * behind the hardware and the application then overwrites audio the CSND has
 * not played yet -- clicks, repeats and an apparently "skipping" stream.
 */
static inline unsigned int ctr_snd_circ_dist(unsigned int a, unsigned int b,
					     unsigned int size)
{
	unsigned int d = (a > b) ? a - b : b - a;
	return (d > size / 2) ? size - d : d;
}

/*
 * Trust st_addr only while it advances at the stream's rate.  Its movement
 * between pointer calls (always less than half the ring: the hrtimer calls
 * this every period) is summed and compared with the elapsed time: after
 * 100 ms within 3% the hardware pointer goes live, and the check repeats
 * every second.  (3Beans' CSND runs on a clock of its own, several times off
 * the guest's timekeeping: followed blindly, that pointer underran every
 * stream within half a second.)
 */
static void ctr_snd_rate_check(struct ctr_snd *snd,
			       struct snd_pcm_runtime *rt,
			       unsigned int hw, ktime_t now)
{
	s64 us;
	u64 expect, diff;

	if (!snd->hw_cand) {
		snd->hw_cand = true;
		snd->hw_t0 = now;
		snd->hw_prev = hw;
		snd->hw_total = 0;
		return;
	}
	snd->hw_total += (hw + rt->buffer_size - snd->hw_prev) % rt->buffer_size;
	snd->hw_prev = hw;

	us = ktime_us_delta(now, snd->hw_t0);
	if (us < (snd->hw_live ? 1000000 : 100000))
		return;
	expect = div_u64((u64)us * rt->rate, USEC_PER_SEC);
	diff = snd->hw_total > expect ? snd->hw_total - expect : expect - snd->hw_total;
	if (diff * 100 <= expect * 3) {
		if (!snd->hw_live)
			dev_info(snd->dev,
				 "CSND position register advances at the stream rate (%llu frames in %lld us); using hardware pointer\n",
				 snd->hw_total, us);
		snd->hw_live = true;
	} else {
		if (snd->hw_live || !snd->hw_rate_warned)
			dev_info(snd->dev,
				 "CSND position moved %llu frames in %lld us, expected %llu: using the clock pointer\n",
				 snd->hw_total, us, expect);
		snd->hw_rate_warned = true;
		snd->hw_live = false;
	}
	snd->hw_t0 = now;
	snd->hw_total = 0;
}

static snd_pcm_uframes_t ctr_snd_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	u64 delta_ns, frames;
	u32 reg, off;
	snd_pcm_uframes_t hw, sw;
	bool hw_ok;

	if (!atomic_read(&snd->running))
		return snd->pos;

	/* Wall-clock estimate of how far the CSND has read. */
	delta_ns = ktime_to_ns(ktime_sub(ktime_get(), snd->base_time));
	frames = div_u64(delta_ns * rt->rate, NSEC_PER_SEC);
	/*
	 * do_div() divides in place and *returns* the remainder, so this is
	 * "sw = frames % buffer_size".  Storing the quotient instead (the
	 * previous revision) made the pointer step once per full ring wrap,
	 * which stalls the application on avail -- no sound at all.
	 */
	sw = (snd_pcm_uframes_t)do_div(frames, rt->buffer_size);

	/* Exact position, if the hardware maintains st_addr. */
	reg = 0;
	off = 0;
	hw = 0;
	if (snd->mono_bytes) {
		reg = readl(snd->csnd + csnd_ch_off(CTR_SND_CHANNEL) +
			    CSNDCH_ST_ADDR);
		off = (u32)(reg - (u32)(phys_addr_t)snd->mono_addr) %
			snd->mono_bytes;
		hw = off / 2;		/* mono: 2 bytes per frame */
	}

	/*
	 * Only trust the register when it agrees with the wall clock; a
	 * register that merely reads back the programmed start address (or
	 * garbage) is rejected and the clock pointer is used instead.
	 */
	hw_ok = ctr_snd_circ_dist(hw, sw, rt->buffer_size) < rt->buffer_size / 2;
	if (hw != snd->hw_last) {
		ktime_t now = ktime_get();

		snd->hw_last = hw;
		snd->hw_moved = now;
		if (hw_ok || snd->hw_cand)
			ctr_snd_rate_check(snd, rt, hw, now);
	} else if (snd->hw_live &&
		   ktime_ms_delta(ktime_get(), snd->hw_moved) > 100) {
		/* the register stopped while the stream runs: back to the clock */
		snd->hw_live = false;
		dev_warn(snd->dev,
			 "CSND position register stuck at %lu; using the clock pointer\n",
			 (unsigned long)hw);
	}

	/*
	 * Bring-up diagnostic: dump the raw st_addr and both positions for a few
	 * seconds so the real CSND consumption rate can be read off the log.
	 * The mono buffer advances 2 bytes per frame, so the deltas of reg and sw
	 * over a known interval give the ratio hardware/requested rate.
	 */
	if (snd->diag_left > 0 && time_after_eq(jiffies, snd->diag_next)) {
		snd->diag_next = jiffies + HZ / 2;
		snd->diag_left--;
		dev_info(snd->dev, "pos: addr=%08x off=%u hw=%lu sw=%lu live=%d\n",
			 reg, off, (unsigned long)hw, (unsigned long)sw, snd->hw_live);
	}

	/*
	 * Report where the CSND really reads once st_addr is known to work.
	 * The wall clock is only an estimate of it: on hardware it is off by
	 * up to 131 frames at every stream start and drifts 26 frames/s (the
	 * CSND runs at 44126 Hz for 44100), and with the hardware behind the
	 * clock pointer, Android overwrites samples the CSND has not played
	 * yet -- glitchy, crackling audio.  The measured rate is right to
	 * within 0.1%, so following st_addr does not slow anything.
	 */
	snd->pos = snd->hw_live ? hw : sw;
	return snd->pos;
}

/*
 * The ALSA ring is interleaved stereo S16; the CSND plays one mono stream.
 * Downmix each block as the application writes it -- this is the only copy
 * that has to happen, and it runs in process context (the ALSA core drops the
 * stream lock around the transfer).
 */
static int ctr_snd_copy_user(struct snd_pcm_substream *ss, int channel,
			     unsigned long hwoff, void __user *buf,
			     unsigned long bytes)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);
	s16 __user *src = buf;
	s16 *dst;
	unsigned long frames = bytes / 4;
	unsigned long i;

	if (!snd->mono_buf)
		return -EINVAL;

	dst = (s16 *)((u8 *)snd->mono_buf + hwoff / 2);
	for (i = 0; i < frames; i++) {
		s16 l, r;

		if (get_user(l, src + 2 * i) || get_user(r, src + 2 * i + 1))
			return -EFAULT;
		dst[i] = (s16)(((int)l + (int)r) >> 1);
	}
	return 0;
}

static int ctr_snd_fill_silence(struct snd_pcm_substream *ss, int channel,
				unsigned long hwoff, unsigned long bytes)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);

	if (snd->mono_buf)
		memset((u8 *)snd->mono_buf + hwoff / 2, 0, bytes / 2);
	return 0;
}

static const struct snd_pcm_ops ctr_snd_pcm_ops = {
	.open = ctr_snd_pcm_open,
	.close = ctr_snd_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = ctr_snd_pcm_hw_params,
	.hw_free = ctr_snd_pcm_hw_free,
	.prepare = ctr_snd_pcm_prepare,
	.trigger = ctr_snd_pcm_trigger,
	.pointer = ctr_snd_pcm_pointer,
	.copy_user = ctr_snd_copy_user,
	.fill_silence = ctr_snd_fill_silence,
	.mmap = snd_pcm_lib_default_mmap,
};

/* ------------------------------------------------------------------------- */
/* Output-stage monitor (from Octoblimp's ctr_csnd.c, N3DS_CSND_OUTPUT_REARM) */
/* ------------------------------------------------------------------------- */

/*
 * On real hardware Octoblimp heard the first syllable of a clip and then
 * nothing, while every sample had demonstrably reached CSND: the codec's
 * output stage had gone down underneath the stream and stayed down.  So
 * while a stream plays, re-read the power/mute bits codec init left every
 * 100 ms and, if a power bit that was set has cleared (or a mute bit that was
 * clear has set), log it and bring the stage back up, in codec init's order.
 * Only bits compared against the init readback count, and the re-arms are
 * capped per boot.
 */
static const struct {
	u16 reg;
	u8 set;		/* bits that must stay set (power) */
	u8 clear;	/* bits that must stay clear (mute) */
	const char *name;
} ctr_snd_out_regs[CTR_SND_OUT_REGS] = {
	{ CDC_101_17,			0xC0, 0x00, "p101.17" },  /* speaker driver */
	{ CDC_100_118,			0xC0, 0x00, "p100.118" }, /* DAC power */
	{ CDC_100_37,			0x88, 0x00, "p100.37" },  /* DAC up flags */
	{ CDC_DAC_DATA_PATH_SETUP,	0xC0, 0x00, "p0.63" },    /* DAC line 1 on */
	{ CDC_DAC_VOLUME_CTRL,		0x00, 0x0C, "p0.64" },    /* DAC mute bits */
};

static void ctr_snd_out_read(struct ctr_snd *snd, int *v)
{
	int i;

	for (i = 0; i < CTR_SND_OUT_REGS; i++) {
		u8 b;

		v[i] = cdc_r(snd->codec, ctr_snd_out_regs[i].reg, &b) ? -1 : b;
	}
}

static void ctr_snd_out_baseline(struct ctr_snd *snd)
{
	int i;

	ctr_snd_out_read(snd, snd->out_base);
	snd->out_base_ok = true;
	for (i = 0; i < CTR_SND_OUT_REGS; i++)
		if (snd->out_base[i] < 0)
			snd->out_base_ok = false;
	dev_info(snd->dev,
		 "output stage at init: p101.17=0x%02x p100.118=0x%02x p100.37=0x%02x p0.63=0x%02x p0.64=0x%02x%s\n",
		 snd->out_base[0] & 0xff, snd->out_base[1] & 0xff,
		 snd->out_base[2] & 0xff, snd->out_base[3] & 0xff,
		 snd->out_base[4] & 0xff,
		 snd->out_base_ok ? "" : " (a read failed: monitor off)");
}

static void ctr_snd_outmon_work(struct work_struct *work)
{
	struct ctr_snd *snd = container_of(to_delayed_work(work),
					   struct ctr_snd, outmon);
	int now[CTR_SND_OUT_REGS];
	u8 lost[CTR_SND_OUT_REGS];
	bool any = false;
	int i;

	if (!atomic_read(&snd->running) || !snd->out_base_ok)
		return;

	ctr_snd_out_read(snd, now);
	for (i = 0; i < CTR_SND_OUT_REGS; i++) {
		u8 base = (u8)snd->out_base[i], cur = (u8)now[i];

		lost[i] = 0;
		if (now[i] < 0)
			continue;
		lost[i] = (base & ctr_snd_out_regs[i].set & (u8)~cur) |
			  ((u8)~base & ctr_snd_out_regs[i].clear & cur);
		any |= lost[i] != 0;
	}

	if (any) {
		if (snd->rearms >= CTR_SND_OUTMON_MAX) {
			dev_warn_once(snd->dev,
				      "output stage still dropping after %u re-arms; standing down\n",
				      snd->rearms);
			return;
		}
		snd->rearms++;
		dev_warn(snd->dev,
			 "output stage dropped (p101.17=0x%02x p100.118=0x%02x p100.37=0x%02x p0.63=0x%02x p0.64=0x%02x), re-arm %u\n",
			 now[0] & 0xff, now[1] & 0xff, now[2] & 0xff,
			 now[3] & 0xff, now[4] & 0xff, snd->rearms);
		if (lost[1] || lost[2]) {
			cdc_mask(snd->codec, CDC_100_118, 0xC0, 0xC0);
			msleep(10);
			cdc_wait_mask(snd->codec, CDC_100_37, 0x88, 0x88);
		}
		if (lost[3])
			cdc_mask(snd->codec, CDC_DAC_DATA_PATH_SETUP, 0xC0, 0xC0);
		if (lost[4])
			cdc_mask(snd->codec, CDC_DAC_VOLUME_CTRL, 0x00, 0x0C);
		if (lost[0] || lost[1] || lost[2])
			cdc_mask(snd->codec, CDC_101_17, 0xC0, 0xC0);
	}

	if (atomic_read(&snd->running))
		mod_delayed_work(system_wq, &snd->outmon,
				 msecs_to_jiffies(CTR_SND_OUTMON_MS));
}

/* ------------------------------------------------------------------------- */
/* Microphone capture (from Octoblimp's ctr_csnd.c, N3DS_MIC_REAL)          */
/* ------------------------------------------------------------------------- */

/*
 * The MIC block at 0x10162000 is the DSi's microphone block (3dbrew "MIC
 * Registers", GBATEK "DSi Microphone and SoundExt"):
 *
 *   +0x00 u16 MIC_CNT   bits 0-1 format, 2-3 rate divider (F/1..F/4 of the
 *                       I2S1 clock), 8 FIFO empty, 9 half, 10 full,
 *                       11 overrun, 12 FIFO clear (needs bit 15 clear),
 *                       15 enable
 *   +0x04 u32 MIC_DATA  a 16-word FIFO
 *
 * With I2S1 at 32.73 kHz the rates are 32730/16360/10910/8180 Hz.  The FIFO
 * holds ~4 ms at 8180 Hz, so a 1 ms soft hrtimer drains it -- bounded by the
 * FIFO depth, so it cannot become an unbounded softirq loop -- and never reads
 * it while bit 8 says it is empty.  An overrun latches and stops sampling
 * until the FIFO is cleared, so it restarts the block.
 *
 * Octoblimp found on hardware that the words do not always hold two samples
 * as documented, so the first MIC_PROBE_WORDS words of each recording are
 * measured (words per second, dead halves, a single sample sitting above
 * filler bits) and decoded with what was found.  The live samples are then
 * resampled (linear) to the rate the ALSA stream asked for.
 *
 * The analog front end is GBATEK's DSi mic_enable_tsc_init_list plus the CTR
 * bias/PGA values libn3ds programs: a 10 Hz high-pass, MICBIAS = AVDD,
 * PGA +21.5 dB, ADC powered and unmuted.
 */
#define MIC_BASE		0x10162000
#define MIC_SIZE		8
#define MIC_CNT			0x00
#define MIC_DATA		0x04
#define MIC_FMT_NORMAL		2u
#define MIC_CNT_RATE(n)		(((n) & 3u) << 2)
#define MIC_CNT_FIFO_EMPTY	BIT(8)
#define MIC_CNT_FIFO_OVERRUN	BIT(11)
#define MIC_CNT_FIFO_CLEAR	BIT(12)
#define MIC_CNT_ENABLE		BIT(15)

#define MIC_POLL_US		1000
#define MIC_DRY_TICKS		(1000000 / MIC_POLL_US)

#define MIC_LIVE_LO		1	/* bits 0-15 only */
#define MIC_LIVE_HI		2	/* bits 16-31 only */
#define MIC_LIVE_BOTH		3	/* two samples, low half first */
#define MIC_LIVE_SLOT		4	/* one sample at bits shift..shift+15 */

#define MIC_PROBE_WORDS		1024
#define MIC_PROBE_DEAD_PCT	90
#define MIC_WORDS_MAX		4300
#define MIC_RATE_LOW		6000
#define MIC_RATE_WANT		7500
#define MIC_SOC_CLOCK		268111856ull

#define CDC_MIC_BIAS		CDC(1, 0x2E)
#define CDC_MIC_PGA		CDC(1, 0x2F)
#define CDC_ADC_IN_P		CDC(1, 0x30)
#define CDC_ADC_IN_M		CDC(1, 0x31)
#define CDC_MIC_IIR		CDC(4, 8)
#define CDC_ADC_POWER		CDC(0, 0x51)
#define CDC_ADC_MUTE		CDC(0, 0x52)
#define CDC_101_51		CDC(101, 51)	/* microphone bias */
#define CDC_101_65		CDC(101, 65)	/* PGA gain correction */
#define CDC_101_66		CDC(101, 66)	/* quick charge */

static const u32 mic_hw_rates[4] = { 32730, 16360, 10910, 8180 };

static int mic_rate_idx = 3;
module_param(mic_rate_idx, int, 0644);
MODULE_PARM_DESC(mic_rate_idx, "initial MIC rate divider (0-3 = 32730/16360/10910/8180 Hz)");

static int mic_idx_clamp(int idx)
{
	return (idx < 0 || idx > 3) ? 3 : idx;
}

/* libn3ds maskWaitReg(): the analog block can take a few writes to latch. */
static void cdc_mask_latch(struct ctr_snd *snd, u16 reg, u8 val, u8 mask)
{
	u8 v = 0;
	int i;

	for (i = 0; i < 64; i++) {
		cdc_mask(snd->codec, reg, val, mask);
		if (!cdc_r(snd->codec, reg, &v) && (v & mask) == val)
			return;
	}
	dev_warn(snd->dev, "codec p%u.%u never latched 0x%02x (reads 0x%02x)\n",
		 reg >> 7, reg & 0x7f, val, v);
}

static void ctr_snd_adc_on(struct ctr_snd *snd)
{
	/* 32737, -32737, 32705 big-endian: N0, N1, D1 of a 10 Hz high-pass */
	static const u8 hp10[6] = { 0x7F, 0xE1, 0x80, 0x1F, 0x7F, 0xC1 };
	unsigned int i;

	/* the coefficient RAM is only safe to write with the ADC off */
	if (!snd->adc_frontend_done) {
		cdc_mask(snd->codec, CDC_100_34, 0x00, 0x04);
		for (i = 0; i < ARRAY_SIZE(hp10); i++)
			cdc_w(snd->codec, CDC_MIC_IIR + i, hp10[i]);
		cdc_mask(snd->codec, CDC_ADC_IN_P, 0x40, 0xC0);
		cdc_mask(snd->codec, CDC_ADC_IN_M, 0x40, 0xC0);
		cdc_w(snd->codec, CDC_101_51, 3);	/* bias */
		cdc_mask_latch(snd, CDC_101_65, 0, 0x3F);	/* PGA gain corr. */
		cdc_mask_latch(snd, CDC_101_66, 2, 0x03);	/* quick charge */
		cdc_w(snd->codec, CDC_MIC_PGA, 0x2B);	/* +21.5 dB */
		cdc_w(snd->codec, CDC_MIC_BIAS, 0x03);	/* MICBIAS = AVDD */
		snd->adc_frontend_done = true;
	}
	cdc_w(snd->codec, CDC_ADC_POWER, 0x80);
	cdc_w(snd->codec, CDC_ADC_MUTE, 0x00);
}

static void ctr_snd_adc_off(struct ctr_snd *snd)
{
	cdc_w(snd->codec, CDC_ADC_MUTE, 0x80);
	cdc_w(snd->codec, CDC_ADC_POWER, 0x00);
}

/* rs_step: Q16 input samples per output frame */
static void mic_set_in_rate(struct ctr_snd *snd, u32 rate, u32 out_rate)
{
	rate = clamp_t(u32, rate, out_rate / 8, out_rate * 8);
	snd->cap_in_rate = rate;
	snd->rs_step = (u32)div_u64((u64)rate << 16, out_rate);
}

static void mic_probe_reset(struct ctr_snd *snd)
{
	snd->pr_t0 = ktime_get();
	snd->pr_words = 0;
	snd->pr_done = false;
	snd->pr_lo_dead = 0;
	snd->pr_hi_dead = 0;
	snd->pr_same = 0;
	memset(snd->pr_ones, 0, sizeof(snd->pr_ones));
	memset(snd->pr_nsign, 0, sizeof(snd->pr_nsign));
	memset(snd->pr_eq, 0, sizeof(snd->pr_eq));
}

/* disable, clear, enable: the start sequence and the overrun recovery */
static void mic_hw_start(struct ctr_snd *snd)
{
	u16 base = MIC_FMT_NORMAL | MIC_CNT_RATE(snd->cap_idx);

	writew(0, snd->mic + MIC_CNT);
	writew(base | MIC_CNT_FIFO_CLEAR, snd->mic + MIC_CNT);
	writew(base | MIC_CNT_ENABLE, snd->mic + MIC_CNT);
	mic_probe_reset(snd);
}

static void mic_hw_stop(struct ctr_snd *snd)
{
	writew(0, snd->mic + MIC_CNT);
}

static void mic_probe_word(struct ctr_snd *snd, u32 w)
{
	u32 ns = w ^ ((w & BIT(31)) ? ~0u : 0u);	/* bit k != bit 31 */
	u32 eq = ~(w ^ (w >> 1));			/* bit k == bit k+1 */
	u16 lo = w & 0xffff, hi = w >> 16;
	int k;

	if (snd->pr_done)
		return;
	snd->pr_words++;
	if (lo == 0x0000 || lo == 0xffff)
		snd->pr_lo_dead++;
	if (hi == 0x0000 || hi == 0xffff)
		snd->pr_hi_dead++;
	if (hi == lo)
		snd->pr_same++;
	for (k = 0; k < 32; k++) {
		snd->pr_ones[k] += (w >> k) & 1;
		snd->pr_nsign[k] += (ns >> k) & 1;
		snd->pr_eq[k] += (eq >> k) & 1;
	}
}

static u32 mic_pct(u32 part, u32 whole)
{
	return whole ? (u32)div_u64((u64)part * 100, whole) : 0;
}

static bool mic_bit_filler(struct ctr_snd *snd, int k, u32 n)
{
	u32 ones = mic_pct(snd->pr_ones[k], n);

	return mic_pct(snd->pr_nsign[k], n) <= 2 || ones <= 2 || ones >= 98 ||
	       mic_pct(snd->pr_eq[k], n) >= 98;
}

static bool mic_bit_active(struct ctr_snd *snd, int k, u32 n)
{
	u32 ones = mic_pct(snd->pr_ones[k], n);

	return mic_pct(snd->pr_nsign[k], n) >= 15 && ones >= 10 && ones <= 90;
}

/* a measured rate, snapped to F/n of either I2S clock when within 2% */
static u32 mic_snap_rate(u32 meas)
{
	static const u32 div[2] = { 8192, 5632 };
	u32 best = meas, best_err = meas / 50 + 1;
	int c, n;

	for (c = 0; c < 2; c++)
		for (n = 1; n <= 32; n++) {
			u32 r = (u32)div_u64(MIC_SOC_CLOCK, div[c] * n);
			u32 err = r > meas ? r - meas : meas - r;

			if (err < best_err) {
				best_err = err;
				best = r;
			}
		}
	return best;
}

/* returns true when it changed the divider and the block needs a restart */
static bool mic_probe_decide(struct ctr_snd *snd, u32 out_rate)
{
	u32 n = snd->pr_words;
	u64 ns = ktime_to_ns(ktime_sub(ktime_get(), snd->pr_t0));
	u32 wps = (u32)div64_u64((u64)n * NSEC_PER_SEC, max_t(u64, ns, 1));
	u32 lo_dead = mic_pct(snd->pr_lo_dead, n);
	u32 hi_dead = mic_pct(snd->pr_hi_dead, n);
	u32 same = mic_pct(snd->pr_same, n);
	u8 live = MIC_LIVE_BOTH;
	int shift = 0, t, k;
	u32 meas, rate;
	bool restart = false;

	snd->pr_done = true;

	/* one sample with filler under it and sign fill over it */
	for (t = 0; t < 16 && mic_bit_filler(snd, t, n); t++)
		;
	if (t > 0 && t < 16 && mic_bit_active(snd, t, n)) {
		for (k = t + 16; k < 31; k++)
			if (mic_pct(snd->pr_nsign[k], n) > 2)
				break;
		if (k >= 31) {
			live = MIC_LIVE_SLOT;
			shift = t;
		}
	}
	if (live != MIC_LIVE_SLOT) {
		if (lo_dead >= MIC_PROBE_DEAD_PCT && hi_dead >= MIC_PROBE_DEAD_PCT)
			live = MIC_LIVE_BOTH;	/* silence either way */
		else if (hi_dead >= MIC_PROBE_DEAD_PCT || same >= MIC_PROBE_DEAD_PCT)
			live = MIC_LIVE_LO;
		else if (lo_dead >= MIC_PROBE_DEAD_PCT)
			live = MIC_LIVE_HI;
	}

	meas = wps * (live == MIC_LIVE_BOTH ? 2 : 1);
	rate = mic_snap_rate(meas);

	/* too few live samples per second: pick a faster divider, once */
	if (!snd->cap_idx_auto && rate < MIC_RATE_LOW &&
	    !(lo_dead >= MIC_PROBE_DEAD_PCT && hi_dead >= MIC_PROBE_DEAD_PCT)) {
		u32 f0 = rate * (snd->cap_idx + 1);
		u32 w0 = wps * (snd->cap_idx + 1);
		int i, best = snd->cap_idx;

		for (i = 3; i >= 0; i--) {
			if (w0 / (i + 1) > MIC_WORDS_MAX)
				break;
			best = i;
			if (f0 / (i + 1) >= MIC_RATE_WANT)
				break;
		}
		snd->cap_idx_auto = true;
		if (best != snd->cap_idx) {
			rate = f0 / (best + 1);
			snd->cap_idx = best;
			restart = true;
		}
	}

	snd->cap_live = live;
	snd->cap_shift = (s8)shift;
	mic_set_in_rate(snd, rate, out_rate);
	dev_info(snd->dev,
		 "mic: %u words in %llu ms = %u words/s, dead lo %u%% hi %u%% same %u%% -> %s%s, %u Hz in, %u Hz out%s\n",
		 n, div_u64(ns, NSEC_PER_MSEC), wps, lo_dead, hi_dead, same,
		 live == MIC_LIVE_LO ? "low half" :
		 live == MIC_LIVE_HI ? "high half" :
		 live == MIC_LIVE_SLOT ? "one sample at bit " : "two samples",
		 live == MIC_LIVE_SLOT ? "" : "", rate, out_rate,
		 restart ? " (divider changed, restarting)" : "");
	if (live == MIC_LIVE_SLOT)
		dev_info(snd->dev, "mic: sample LSB at bit %d\n", shift);
	return restart;
}

/* one live sample in, 0..8 frames out at the stream rate (linear) */
static void mic_push(struct ctr_snd *snd, struct snd_pcm_runtime *rt, s16 s)
{
	s32 prev = snd->rs_prev;

	while (snd->rs_pos < 0x10000) {
		s16 out = (s16)(prev + (s32)(((s64)((s32)s - prev) *
					     snd->rs_pos) >> 16));
		s16 *dst = (s16 *)rt->dma_area +
			   (size_t)snd->cap_pos * rt->channels;
		unsigned int c;

		for (c = 0; c < rt->channels; c++)
			dst[c] = out;
		if (++snd->cap_pos >= rt->buffer_size)
			snd->cap_pos = 0;
		snd->cap_period_fill++;
		snd->rs_pos += snd->rs_step;
	}
	snd->rs_pos -= 0x10000;
	snd->rs_prev = s;
}

static void mic_drain(struct ctr_snd *snd, struct snd_pcm_runtime *rt)
{
	unsigned int word, got = 0;
	u16 cnt = 0;

	/* 16 words is the whole FIFO; the 17th read notices it went empty */
	for (word = 0; word < 17; word++) {
		u32 w;

		cnt = readw(snd->mic + MIC_CNT);
		if (cnt & MIC_CNT_FIFO_EMPTY)
			break;
		w = readl(snd->mic + MIC_DATA);
		mic_probe_word(snd, w);
		got++;

		switch (snd->cap_live) {
		case MIC_LIVE_LO:
			mic_push(snd, rt, (s16)(w & 0xffff));
			break;
		case MIC_LIVE_HI:
			mic_push(snd, rt, (s16)(w >> 16));
			break;
		case MIC_LIVE_SLOT:
			mic_push(snd, rt, (s16)(w >> snd->cap_shift));
			break;
		default:
			mic_push(snd, rt, (s16)(w & 0xffff));
			mic_push(snd, rt, (s16)(w >> 16));
			break;
		}
	}

	if (got) {
		snd->cap_dry_ticks = 0;
		if (!snd->cap_seen_data) {
			snd->cap_seen_data = true;
			dev_info(snd->dev, "mic: first samples out of the FIFO (MIC_CNT 0x%04x)\n",
				 cnt);
		}
	} else if (!snd->cap_seen_data && ++snd->cap_dry_ticks == MIC_DRY_TICKS) {
		dev_warn(snd->dev,
			 "mic: FIFO still empty one second after enabling (MIC_CNT 0x%04x): the block is not sampling\n",
			 cnt);
	}

	if (cnt & MIC_CNT_FIFO_OVERRUN) {
		snd->cap_overruns++;
		mic_hw_start(snd);
	} else if (!snd->pr_done && snd->pr_words >= MIC_PROBE_WORDS) {
		if (mic_probe_decide(snd, rt->rate))
			mic_hw_start(snd);
	}
}

static enum hrtimer_restart ctr_snd_cap_timer(struct hrtimer *t)
{
	struct ctr_snd *snd = container_of(t, struct ctr_snd, cap_timer);
	struct snd_pcm_substream *ss = snd->cap_ss;

	if (!atomic_read(&snd->cap_running) || !ss)
		return HRTIMER_NORESTART;

	mic_drain(snd, ss->runtime);
	if (snd->cap_period_fill >= ss->runtime->period_size) {
		snd->cap_period_fill %= ss->runtime->period_size;
		snd_pcm_period_elapsed(ss);
		if (!atomic_read(&snd->cap_running))
			return HRTIMER_NORESTART;
	}

	hrtimer_forward_now(t, ns_to_ktime(MIC_POLL_US * NSEC_PER_USEC));
	return HRTIMER_RESTART;
}

static const struct snd_pcm_hardware ctr_snd_cap_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min = 8000,
	.rate_max = 48000,
	.channels_min = 1,
	.channels_max = 2,
	.buffer_bytes_max = 64 * 1024,
	.period_bytes_min = 256,
	.period_bytes_max = 32 * 1024,
	.periods_min = 2,
	.periods_max = 64,
};

static int ctr_snd_cap_open(struct snd_pcm_substream *ss)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);

	if (!snd->mic || !snd->codec)
		return -ENODEV;
	snd->cap_ss = ss;
	atomic_set(&snd->cap_running, 0);
	ss->runtime->hw = ctr_snd_cap_hw;
	return 0;
}

static void ctr_snd_cap_halt(struct ctr_snd *snd)
{
	atomic_set(&snd->cap_running, 0);
	if (!hrtimer_callback_running(&snd->cap_timer))
		hrtimer_cancel(&snd->cap_timer);
	mic_hw_stop(snd);
}

static int ctr_snd_cap_close(struct snd_pcm_substream *ss)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);

	ctr_snd_cap_halt(snd);
	ctr_snd_adc_off(snd);
	snd->cap_ss = NULL;
	return 0;
}

static int ctr_snd_cap_hw_params(struct snd_pcm_substream *ss,
				 struct snd_pcm_hw_params *params)
{
	return snd_pcm_lib_malloc_pages(ss, params_buffer_bytes(params));
}

static int ctr_snd_cap_hw_free(struct snd_pcm_substream *ss)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);

	ctr_snd_cap_halt(snd);
	return snd_pcm_lib_free_pages(ss);
}

static int ctr_snd_cap_prepare(struct snd_pcm_substream *ss)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;

	ctr_snd_cap_halt(snd);
	ctr_snd_adc_on(snd);		/* codec writes sleep: not in trigger */
	memset(rt->dma_area, 0, frames_to_bytes(rt, rt->buffer_size));
	snd->cap_pos = 0;
	snd->cap_period_fill = 0;
	/* a new recording: the documented format until the probe decides */
	snd->cap_idx = mic_idx_clamp(mic_rate_idx);
	snd->cap_idx_auto = false;
	snd->cap_live = MIC_LIVE_BOTH;
	snd->cap_shift = 0;
	snd->rs_pos = 0;
	snd->rs_prev = 0;
	snd->cap_dry_ticks = 0;
	snd->cap_overruns = 0;
	mic_set_in_rate(snd, mic_hw_rates[snd->cap_idx], rt->rate);
	dev_info(snd->dev, "capture: %u Hz, %u channel(s), %lu frame periods\n",
		 rt->rate, rt->channels, (unsigned long)rt->period_size);
	return 0;
}

static int ctr_snd_cap_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		mic_hw_start(snd);
		atomic_set(&snd->cap_running, 1);
		hrtimer_start(&snd->cap_timer,
			      ns_to_ktime(MIC_POLL_US * NSEC_PER_USEC),
			      HRTIMER_MODE_REL_SOFT);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		ctr_snd_cap_halt(snd);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static snd_pcm_uframes_t ctr_snd_cap_pointer(struct snd_pcm_substream *ss)
{
	struct ctr_snd *snd = snd_pcm_substream_chip(ss);

	return snd->cap_pos;
}

static const struct snd_pcm_ops ctr_snd_cap_ops = {
	.open = ctr_snd_cap_open,
	.close = ctr_snd_cap_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = ctr_snd_cap_hw_params,
	.hw_free = ctr_snd_cap_hw_free,
	.prepare = ctr_snd_cap_prepare,
	.trigger = ctr_snd_cap_trigger,
	.pointer = ctr_snd_cap_pointer,
	.mmap = snd_pcm_lib_default_mmap,
};

/* ------------------------------------------------------------------------- */
/* Boot-time test tone                                                       */
/* ------------------------------------------------------------------------- */

/* One period of a sine, amplitude 12000, 32 entries. */
static const s16 ctr_snd_sine32[32] = {
	0, 2341, 4592, 6667, 8485, 9978, 11087, 11769,
	12000, 11769, 11087, 9978, 8485, 6667, 4592, 2341,
	0, -2341, -4592, -6667, -8485, -9978, -11087, -11769,
	-12000, -11769, -11087, -9978, -8485, -6667, -4592, -2341,
};

static int __maybe_unused ctr_snd_test_tone(struct ctr_snd *snd)
{
	const unsigned int rate = 32768;
	const unsigned int frames = 4096;	/* ~125 ms, looped */
	const unsigned int freq = 440;
	void *buf;
	dma_addr_t phys;
	unsigned int i;
	u32 phase = 0;
	/* table steps per sample: 32*freq/rate (fits in 32 bits for the
	 * 8 kHz..48 kHz range). */
	const u32 step = ((u32)freq * 32 * 65536) / rate;

	buf = dma_alloc_coherent(snd->dev, frames * 2, &phys, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < frames; i++) {
		s16 v = ctr_snd_sine32[(phase >> 16) & 31];
		((s16 *)buf)[i] = v;
		phase += step;
	}

	csnd_stop_ch(snd->csnd, CTR_SND_CHANNEL);
	csnd_setup_ch(snd->csnd, CTR_SND_CHANNEL, rate, phys, frames * 2,
		      CSND_CH_FMT_PCM16 | CSND_CH_RPT_LOOP | CSND_CH_LERP);
	csnd_play(snd->csnd, CTR_SND_CHANNEL, true);
	dev_info(snd->dev, "playing boot test tone (440 Hz)\n");
	msleep(500);
	csnd_play(snd->csnd, CTR_SND_CHANNEL, false);
	csnd_stop_ch(snd->csnd, CTR_SND_CHANNEL);

	dma_free_coherent(snd->dev, frames * 2, buf, phys);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Probe / remove                                                            */
/* ------------------------------------------------------------------------- */

static int ctr_snd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ctr_snd *snd;
	int err;

	snd = devm_kzalloc(dev, sizeof(*snd), GFP_KERNEL);
	if (!snd)
		return -ENOMEM;
	snd->dev = dev;

	snd->csnd = devm_ioremap(dev, CSND_BASE, CSND_SIZE);
	if (!snd->csnd) {
		dev_err(dev, "cannot map CSND registers\n");
		return -ENOMEM;
	}

	/*
	 * The child of an SPI device has no DMA mask configured by the OF core;
	 * ALSA's SNDRV_DMA_TYPE_DEV and our coherent test-tone buffer need one.
	 * Physical == bus address on the 3DS (no IOMMU).
	 */
	err = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (err)
		dev_warn(dev, "cannot set DMA mask: %d\n", err);

	csnd_init(snd->csnd);
	hrtimer_init(&snd->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	snd->timer.function = ctr_snd_timer;
	atomic_set(&snd->running, 0);

	/* the codec regmap belongs to ctr_tsc, our parent device */
	snd->codec = dev->parent ? dev_get_regmap(dev->parent, NULL) : NULL;
	if (!snd->codec)
		dev_warn(dev, "no codec regmap: no output monitor, no microphone\n");
	INIT_DELAYED_WORK(&snd->outmon, ctr_snd_outmon_work);
	if (snd->codec)
		ctr_snd_out_baseline(snd);

	snd->mic = devm_ioremap(dev, MIC_BASE, MIC_SIZE);
	if (!snd->mic)
		dev_warn(dev, "cannot map the MIC block: no microphone\n");
	hrtimer_init(&snd->cap_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	snd->cap_timer.function = ctr_snd_cap_timer;
	atomic_set(&snd->cap_running, 0);

	err = snd_card_new(dev, -1, "nintendo3ds", THIS_MODULE, 0, &snd->card);
	if (err < 0) {
		dev_err(dev, "snd_card_new failed: %d\n", err);
		return err;
	}
	strscpy(snd->card->driver, DRIVER_NAME, sizeof(snd->card->driver));
	strscpy(snd->card->shortname, "Nintendo 3DS audio", sizeof(snd->card->shortname));
	strscpy(snd->card->longname, "Nintendo 3DS CSND/TSC2117", sizeof(snd->card->longname));

	err = snd_pcm_new(snd->card, "3ds-pcm", 0, 1, 1, &snd->pcm);
	if (err < 0)
		goto err_card;
	/*
	 * The PCM ops reach the driver through snd_pcm_substream_chip(),
	 * i.e. substream->private_data, which the ALSA core copies from
	 * pcm->private_data (sound/core/pcm.c: snd_pcm_attach_substream()).
	 * Without this assignment the very first open() dereferences NULL in
	 * ctr_snd_pcm_open() and oopses the kernel; the oops also leaves
	 * pcm->open_mutex held, so every later open (AudioFlinger's) blocks
	 * forever and the whole Android boot stalls at AudioService.
	 */
	snd->pcm->private_data = snd;
	snd_pcm_set_ops(snd->pcm, SNDRV_PCM_STREAM_PLAYBACK, &ctr_snd_pcm_ops);
	snd_pcm_set_ops(snd->pcm, SNDRV_PCM_STREAM_CAPTURE, &ctr_snd_cap_ops);
	snd_pcm_lib_preallocate_pages_for_all(snd->pcm, SNDRV_DMA_TYPE_DEV,
					      dev, 64 * 1024, 64 * 1024);

	err = snd_card_register(snd->card);
	if (err < 0)
		goto err_card;

	platform_set_drvdata(pdev, snd);

	dev_info(dev, "registered ALSA card (CSND/I2S2 playback, MIC capture, no DSP firmware needed)\n");

	/* The boot-time test tone (ctr_snd_test_tone) is compiled in for
	 * debugging but not played: it is loud and surprising.  Call
	 * ctr_snd_test_tone(snd) here to hear it. */

	return 0;

err_card:
	snd_card_free(snd->card);
	return err;
}

static int ctr_snd_remove(struct platform_device *pdev)
{
	struct ctr_snd *snd = platform_get_drvdata(pdev);

	atomic_set(&snd->running, 0);
	hrtimer_cancel(&snd->timer);
	cancel_delayed_work_sync(&snd->outmon);
	ctr_snd_cap_halt(snd);
	csnd_play(snd->csnd, CTR_SND_CHANNEL, false);
	snd_card_free(snd->card);
	return 0;
}

static const struct of_device_id ctr_snd_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{}
};
MODULE_DEVICE_TABLE(of, ctr_snd_of_match);

static struct platform_driver ctr_snd_driver = {
	.probe = ctr_snd_probe,
	.remove = ctr_snd_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ctr_snd_of_match,
	},
};
module_platform_driver(ctr_snd_driver);

MODULE_AUTHOR("Android-on-3DS port");
MODULE_DESCRIPTION("Nintendo 3DS CSND/TSC2117 ALSA audio driver");
MODULE_LICENSE("GPL v2");
