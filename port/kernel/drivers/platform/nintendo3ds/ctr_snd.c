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

/* Codec calibration defaults (launch-day o3DS values, from libn3ds hw_cal.c). */
#define DRIVER_GAIN_HP		0	/* 0 dB */
#define DRIVER_GAIN_SP		1	/* 12 dB */
#define ANALOG_VOLUME_HP	0	/* 0 dB */
#define ANALOG_VOLUME_SP	7	/* -3.5 dB */

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

/* Write an array of big-endian 16-bit coefficients starting at (page, reg). */
static int cdc_write_be16(struct regmap *map, int page, int reg,
			  const s16 *vals, int n)
{
	u8 *buf;
	int i, err;

	buf = kmalloc(n * 2, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		buf[2 * i] = (u16)vals[i] >> 8;
		buf[2 * i + 1] = (u16)vals[i] & 0xff;
	}
	err = regmap_raw_write(map, CDC(page, reg), buf, n * 2);
	kfree(buf);
	return err;
}

/* Filter coefficient tables (default calibration, logical values). */
static const s16 filter_sp32[15] = {
	32767, -27535, 22413, 30870, -29096,
	-14000, 30000, -14000, 0, 0,
	32736, -16368, 0, 16352, 0,
};
static const s16 filter_sp47[15] = {
	32767, -28995, 25277, 31456, -30200,
	-14402, 30000, -14402, 0, 0,
	32745, -16372, 0, 16361, 0,
};
static const s16 filter_hp32[15] = {
	32767, 0, 0, 0, 0,
	32767, 0, 0, 0, 0,
	32736, -16368, 0, 16352, 0,
};
static const s16 filter_hp47[15] = {
	32767, 0, 0, 0, 0,
	32767, 0, 0, 0, 0,
	32745, -16372, 0, 16361, 0,
};
/* CdcPrbP25Filters: a first-order IIR followed by five biquads. */
static const s16 filter_free_iir[3] = { 32767, 0, 0 };
static const s16 filter_free_b[25] = {
	-12959, -8785, 32767, 8785, 12959,
	-12959, -8785, 32767, 8785, 12959,
	-12959, -8785, 32767, 8785, 12959,
	32767, 0, 0, 0, 0,
	32767, 0, 0, 0, 0,
};

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
 * Codec bring-up.  Runs from ctr_tsc's probe (before the touchscreen child),
 * so a soft reset here cannot clobber the touchscreen configuration.
 */
int ctr_snd_codec_init(struct regmap *map)
{
	u8 vendor, rev, hpval;
	int err;
	void __iomem *i2s;

	pr_info("initialising TSC2117/AIC3010 audio codec\n");

	/* Turn the codec MCLK on (the same bit ctr_tsc already sets). */
	ctr_pdn_mclk(true);

	/* CTR-specific software reset. */
	err = cdc_w(map, CDC_SOFT_RST_CTR, 1);
	if (err)
		return err;
	msleep(40);

	/* Headset detection timing + reset the detection state machine. */
	cdc_w(map, CDC_100_67, 0x11);
	cdc_mask(map, CDC_101_119, 1, 1);

	/* Don't force the GPIO pins; VREF on; PLL on. */
	cdc_mask(map, CDC_GPI1_GPI2_PIN_CTRL, 0x66, 0x66);
	cdc_w(map, CDC_101_122, 1);
	cdc_mask(map, CDC_100_34, 0x18, 0x18);

	/* I2S mute/volume control (part of the codec module's sound setup). */
	cdc_mask(map, CDC_100_49, 0x44, 0x44);

	/*
	 * Headset: force the speaker output.  We cannot easily read the jack
	 * GPIO from here; speakers are what matters for bring-up and the
	 * codec's automatic switching can be enabled later.
	 */
	cdc_mask(map, CDC_HEADSET_SEL, 0x20, 0x30);
	cdc_mask(map, CDC_100_67, 0, 0x80);
	cdc_mask(map, CDC_100_67, 0x80, 0x80);

	/* Codec-side I2S dividers: I2S1 32 kHz, I2S2 47.6 kHz. */
	ctr_set_i2s_freq(map, 1, false);
	ctr_set_i2s_freq(map, 2, true);

	/* I2S interface setup.  Bits 12-14 are only writable while disabled. */
	i2s = ioremap(I2S_BASE, I2S_SIZE);
	if (!i2s)
		return -ENOMEM;
	writew(0, i2s + 0);
	writew(0, i2s + 2);
	/* I2S1: enable | 16 MHz MCLK | 32.7 kHz | legacy volume 32 | DSP vol 0 */
	writew(I2S2_EN | I2S2_MCLK2_16MHZ | 0 | (32 << 6), i2s + 0);
	/* I2S2: enable | 16 MHz MCLK | 47.6 kHz  (== 0xE000) */
	writew(I2S2_EN | I2S2_MCLK2_16MHZ | I2S2_FREQ_47KHZ, i2s + 2);

	/* Speaker driver power-up time. */
	cdc_mask(map, CDC_101_17, 0x10, 0x1C);
	/* Per-line digital volumes: write 0 dB. */
	cdc_w(map, CDC_100_122, 0);	/* I2S1 */
	cdc_w(map, CDC_100_120, 0);	/* I2S2 */
	cdc_w(map, CDC_100_123, 0);	/* I2S2 (was the shutter volume) */

	/* Page 9/8: I2S1 "filterFree" (kept for completeness, mute while
	 * writing). */
	cdc_mask(map, CDC_100_119, 0xC, 0xC);	/* mute I2S2 while writing */
	cdc_write_be16(map, 10, 2, filter_free_iir, 3);
	cdc_write_be16(map, 10, 12, filter_free_b, 25);
	/* Page 12: speaker EQ (32 kHz and 47.6 kHz variants). */
	cdc_write_be16(map, 12, 2, filter_sp32, 15);
	cdc_write_be16(map, 12, 66, filter_sp32, 15);
	cdc_write_be16(map, 12, 32, filter_sp47, 15);
	cdc_write_be16(map, 12, 96, filter_sp47, 15);
	/* Page 11: headphone EQ. */
	cdc_write_be16(map, 11, 2, filter_hp32, 15);
	cdc_write_be16(map, 11, 66, filter_hp32, 15);
	cdc_write_be16(map, 11, 32, filter_hp47, 15);
	cdc_write_be16(map, 11, 96, filter_hp47, 15);

	/* Power the DAC on and wait for the flags. */
	cdc_mask(map, CDC_100_118, 0xC0, 0xC0);
	msleep(10);
	cdc_wait_mask(map, CDC_100_37, 0x88, 0x88);

	/* Route the DAC and unmute both I2S lines. */
	cdc_w(map, CDC_101_10, 0xA);
	cdc_mask(map, CDC_DAC_DATA_PATH_SETUP, 0xC0, 0xC0);	/* I2S1 unmute */
	cdc_w(map, CDC_DAC_VOLUME_CTRL, 0);
	cdc_mask(map, CDC_100_119, 0, 0xC);			/* I2S2 unmute */

	/* Headphone driver.  Two vendor/revision variants exist. */
	cdc_r(map, CDC_0_2, &vendor);
	cdc_r(map, CDC_0_3, &rev);
	pr_info("codec vendor/rev %02x/%02x\n", vendor, rev);
	if ((vendor & 0xF) <= 1 && ((rev & 0x70) >> 4) <= 2)
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

	iounmap(i2s);
	pr_info("codec ready\n");
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
	u32 div = 67027964u / sample_rate;
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
 * timer callback (as an earlier revision did): the callback is re-armed with
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
		snd->hw_last = hw;
		if (hw_ok && !snd->hw_live) {
			snd->hw_live = true;
			dev_info(snd->dev,
				 "CSND position register advances; using hardware pointer\n");
		}
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
	 * Tape the application to the wall clock, not to st_addr: the hardware
	 * position is a useful sanity check (see the log above) but if the CSND
	 * is actually consuming the ring at a different rate than the requested
	 * one, following st_addr would make the whole pipeline run at that wrong
	 * rate (uniformly slow playback).
	 */
	snd->pos = sw;
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

	err = snd_card_new(dev, -1, "nintendo3ds", THIS_MODULE, 0, &snd->card);
	if (err < 0) {
		dev_err(dev, "snd_card_new failed: %d\n", err);
		return err;
	}
	strscpy(snd->card->driver, DRIVER_NAME, sizeof(snd->card->driver));
	strscpy(snd->card->shortname, "Nintendo 3DS audio", sizeof(snd->card->shortname));
	strscpy(snd->card->longname, "Nintendo 3DS CSND/TSC2117", sizeof(snd->card->longname));

	err = snd_pcm_new(snd->card, "3ds-pcm", 0, 1, 0, &snd->pcm);
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
	snd_pcm_lib_preallocate_pages_for_all(snd->pcm, SNDRV_DMA_TYPE_DEV,
					      dev, 64 * 1024, 64 * 1024);

	err = snd_card_register(snd->card);
	if (err < 0)
		goto err_card;

	platform_set_drvdata(pdev, snd);

	dev_info(dev, "registered ALSA card (CSND/I2S2, no DSP firmware needed)\n");

	/* The boot-time test tone (ctr_snd_test_tone) is kept compiled in
	 * for bring-up but is no longer played: it was loud and surprising.
	 * Re-enable by calling ctr_snd_test_tone(snd) here. */

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
