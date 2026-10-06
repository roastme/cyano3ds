// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Nintendo 3DS Secure Digital Host Controller driver
 *
 *  Copyright (C) 2021 Santiago Herrera
 *
 *  Based on toshsd.c, copyright (C) 2014 Ondrej Zary and 2007 Richard Betts
 */

#define DRIVER_NAME "3ds-sdhc"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/io.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>
#include <linux/timer.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>

#include "ctr_sdhc.h"

#define SDHC_ERR_MASK                                                          \
	(SDHC_ERR_BAD_CMD | SDHC_ERR_CRC_FAIL | SDHC_ERR_STOP_BIT |            \
	 SDHC_ERR_DATATIMEOUT | SDHC_ERR_TX_OVERFLOW | SDHC_ERR_RX_UNDERRUN |  \
	 SDHC_ERR_CMD_TIMEOUT | SDHC_ERR_ILLEGAL_ACC)

#define SDHC_DEFAULT_IRQMASK                                                   \
	(SDHC_STAT_CMDRESPEND | SDHC_STAT_DATA_END | SDHC_STAT_RX_READY |      \
	 SDHC_STAT_TX_REQUEST | SDHC_STAT_CARDREMOVE | SDHC_STAT_CARDINSERT |  \
	 SDHC_ERR_MASK)

static void ctr_sdhc_respend_irq(struct ctr_sdhc *host);

#define SDHC_SDIO_STAT_IOIRQ	BIT(0)
#define SDHC_SDIO_MASK_ALL	0xc007
/* N3DS_SDIO_POLL_REALTIME: this was 10 while the kernel clock ran 3x fast
 * (N3DS_TWD_PERIPHCLK), so the AR6014 IRQ fallback that Wi-Fi and wireless
 * adb were proven on really ran every 3.3 ms.  Keep that real cadence. */
#define SDHC_SDIO_POLL_MS	3

/* N3DS_SDIO_INLINE_SHORT_PIO: all AR6014 register and HTC mailbox
 * transactions are at most 128 bytes. Copy those in the hard IRQ and keep
 * 512-byte storage-style transfers on the threaded path. */
static unsigned int pio_inline_max = 128;
module_param(pio_inline_max, uint, 0644);
MODULE_PARM_DESC(pio_inline_max,
                 "largest FIFO block copied in hard IRQ context");

/* N3DS_SDHC_CMD53_TRACE_CONTROL: successful per-request transport logging is
 * useful for controller bring-up but floods the visible console and adds I/O
 * to the firmware bootstrap.  Keep it opt-in; timeout and error reports below
 * remain unconditional. */
static bool cmd53_success_trace;
module_param(cmd53_success_trace, bool, 0644);
MODULE_PARM_DESC(cmd53_success_trace,
                 "trace the first 64 successful AR6014 CMD53 requests");

static void ctr_sdhc_sdio_arm(struct ctr_sdhc *host, bool enable)
{
	u16 stat = ioread16(host->regs + SDHC_CARD_IRQ_STAT);

	/* This block follows the TMIO status/mask convention: clear only the
	 * defined status bits and never set EXPUB52/EXWT by writing ~BIT(0). */
	iowrite16(stat & ~SDHC_SDIO_MASK_ALL,
		  host->regs + SDHC_CARD_IRQ_STAT);
	iowrite16(enable ? (SDHC_SDIO_MASK_ALL & ~SDHC_SDIO_STAT_IOIRQ)
			   : SDHC_SDIO_MASK_ALL,
		  host->regs + SDHC_CARD_IRQ_MASK);
}

static void ctr_sdhc_reset(struct ctr_sdhc *host)
{
	/* reset controller */
	iowrite16(0, host->regs + SDHC_SOFTRESET);
	iowrite16(1, host->regs + SDHC_SOFTRESET);

	/* clear registers */
	iowrite16(0, host->regs + SDHC_CARD_PORTSEL);
	iowrite16(0, host->regs + SDHC_CARD_CLKCTL);
	iowrite32(0, host->regs + SDHC_ERROR_STATUS);
	iowrite16(0, host->regs + SDHC_STOP_INTERNAL);

	iowrite16(0, host->regs + SDHC_DATA16_BLK_CNT);
	iowrite16(0, host->regs + SDHC_DATA16_BLK_LEN);

	iowrite16(0, host->regs + SDHC_DATA32_BLK_CNT);
	iowrite16(0, host->regs + SDHC_DATA32_BLK_LEN);

	/* Use the controller's 16-bit FIFO path.  Enabling DATA_CTL bit 1 also
	 * enables the alternate word-FIFO mode, while this driver still services
	 * the normal RX_READY/TX_REQUEST status and DATA16_FIFO_PORT.  Hardware
	 * proved that mixing those modes suppresses FIFO-ready on the first CMD53. */
	iowrite16(0, host->regs + SDHC_DATA_CTL);
	iowrite16(0, host->regs + SDHC_DATA32_CTL);

	/* set interrupt masks */
	iowrite32(~SDHC_DEFAULT_IRQMASK, host->regs + SDHC_IRQ_MASK);
	iowrite32(0, host->regs + SDHC_IRQ_STAT);

	iowrite16(SDHC_CARD_OPTION_1BIT | SDHC_CARD_OPTION_NOC2,
		  host->regs + SDHC_CARD_OPTION);
}

static void __ctr_sdhc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct ctr_sdhc *host = mmc_priv(mmc);

	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		mdelay(1);
		iowrite16(0, host->regs + SDHC_CARD_CLKCTL);
		return;
	case MMC_POWER_UP:
		break;
	case MMC_POWER_ON:
		mdelay(20);
		break;
	}

	if (ios->clock) {
		u16 clk_ctl;
		int clk_div = -1;
		unsigned clk_fit = clk_get_rate(host->sdclk) / 2;

		while ((ios->clock < clk_fit) && (clk_div < 7)) {
			clk_div++;
			clk_fit >>= 1;
		}

		clk_ctl = BIT(clk_div + 2) >> 2;
		clk_ctl |= SDHC_CARD_CLKCTL_PIN_ENABLE;
		iowrite16(clk_ctl, host->regs + SDHC_CARD_CLKCTL);
		mdelay(5);
	} else {
		iowrite16(0, host->regs + SDHC_CARD_CLKCTL);
	}

	switch (ios->bus_width) {
	default:
		dev_err(host->dev, "invalid bus width %d\n", ios->bus_width);
		break;

	case MMC_BUS_WIDTH_1:
		iowrite16(SDHC_CARD_OPTION_RETRIES(14) |
				  SDHC_CARD_OPTION_TIMEOUT(14) |
				  SDHC_CARD_OPTION_NOC2 | SDHC_CARD_OPTION_1BIT,
			  host->regs + SDHC_CARD_OPTION);
		break;
	case MMC_BUS_WIDTH_4:
		iowrite16(SDHC_CARD_OPTION_RETRIES(14) |
				  SDHC_CARD_OPTION_TIMEOUT(14) |
				  SDHC_CARD_OPTION_NOC2 | SDHC_CARD_OPTION_4BIT,
			  host->regs + SDHC_CARD_OPTION);
		break;
	}
}

static void ctr_sdhc_finish_request(struct ctr_sdhc *host)
{
	struct mmc_request *mrq = host->mrq;

	if (!mrq) {
		/* A terminal error or a late IRQ may race a prior completion. */
		dev_warn(host->dev, "Spurious request completion\n");
		host->cmd = NULL;
		host->data = NULL;
		host->transport_trace_active = false;
		return;
	}

	del_timer(&host->timeout_timer);

	if (host->transport_trace_active) {
		dev_info(host->dev,
			"AR6002 SDHC CMD53 request done seq=%u\n",
			host->transport_trace_active_seq);
		host->transport_trace_active = false;
	}

	host->mrq = NULL;
	host->cmd = NULL;
	host->data = NULL;

	mmc_request_done(host->mmc, mrq);
}

/* Last-resort recovery for a lost FIFO/data-end edge.  The ath6k synchronous
 * wait is bounded independently, but without a controller watchdog its worker
 * can remain stuck in mmc_wait_for_req_done() forever and retain the SDIO
 * host. */
static void ctr_sdhc_timeout(struct timer_list *t)
{
	struct ctr_sdhc *host = from_timer(host, t, timeout_timer);
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	if (host->mrq) {
		dev_err(host->dev,
			"AR6002 SDHC request timeout stat=%08x data16_ctl=%04x data32_ctl=%04x\n",
			ioread32(host->regs + SDHC_IRQ_STAT),
			ioread16(host->regs + SDHC_DATA_CTL),
			ioread16(host->regs + SDHC_DATA32_CTL));

		if (host->data) {
			host->data->error = -ETIMEDOUT;
			host->data->bytes_xfered = 0;
			sg_miter_stop(&host->sg_miter);
		}
		if (host->cmd)
			host->cmd->error = -ETIMEDOUT;
		else if (host->mrq->cmd)
			host->mrq->cmd->error = -ETIMEDOUT;

		ctr_sdhc_reset(host);
		__ctr_sdhc_set_ios(host->mmc, &host->mmc->ios);
		ctr_sdhc_finish_request(host);
	}
	spin_unlock_irqrestore(&host->lock, flags);
}

static void ctr_sdhc_data_end_irq(struct ctr_sdhc *host)
{
	struct mmc_data *data = host->data;

	host->data = NULL;

	if (!data) {
		dev_warn(host->dev, "Spurious data end IRQ\n");
		return;
	}

	if (data->error == 0)
		data->bytes_xfered = data->blocks * data->blksz;
	else
		data->bytes_xfered = 0;

	if (host->transport_trace_active)
		dev_info(host->dev,
			"AR6002 SDHC CMD53 data complete seq=%u error=%d xfr=%u\n",
			host->transport_trace_active_seq, data->error,
			data->bytes_xfered);

	dev_dbg(host->dev, "Completed data request xfr=%d\n",
		data->bytes_xfered);

	ctr_sdhc_finish_request(host);
}

/* Drain/fill complete controller blocks.  One SDIO block can span multiple
 * scatterlist segments (the ath6k receive path commonly uses 4 + 124 bytes),
 * and the edge-triggered IRQ can latch DATA_END or the next FIFO-ready state
 * while the current block is copied. */
static void ctr_sdhc_pio(struct ctr_sdhc *host)
{
	struct mmc_data *data = host->data;
	struct sg_mapping_iter *sg_miter = &host->sg_miter;
	u8 *buf;
	u32 int_reg;
	int count, remaining;

	for (;;) {
		remaining = data->blksz;
		while (remaining > 0 && sg_miter_next(sg_miter)) {
			buf = sg_miter->addr;
			count = min_t(int, sg_miter->length, remaining);
			if (WARN_ONCE(count & 1,
				      "odd PIO chunk %d (segment %u, remaining %d)\n",
				      count, (unsigned int)sg_miter->length,
				      remaining))
				count &= ~1;
			if (!count)
				break;

			if (data->flags & MMC_DATA_READ)
				ioread16_rep(host->regs + SDHC_DATA16_FIFO_PORT,
					     buf, count >> 1);
			else
				iowrite16_rep(host->regs + SDHC_DATA16_FIFO_PORT,
					      buf, count >> 1);

			sg_miter->consumed = count;
			remaining -= count;
		}

		WARN_ONCE(remaining > 0 && remaining < data->blksz,
			  "sg exhausted mid-block, %d bytes stranded\n",
			  remaining);

		/* Catch status that latched while the IRQ line was already high. */
		int_reg = ioread32(host->regs + SDHC_IRQ_STAT);
		if (!(int_reg & (SDHC_STAT_DATA_END | SDHC_STAT_CMDRESPEND |
				 SDHC_STAT_RX_READY | SDHC_STAT_TX_REQUEST)))
			break;

		iowrite32(~(int_reg & SDHC_DEFAULT_IRQMASK),
			  host->regs + SDHC_IRQ_STAT);
		if (host->transport_trace_active)
			dev_info(host->dev,
				 "AR6002 SDHC CMD53 PIO latched seq=%u status=%08x remaining=%d\n",
				 host->transport_trace_active_seq, int_reg,
				 remaining);

		if (int_reg & SDHC_STAT_CMDRESPEND)
			ctr_sdhc_respend_irq(host);

		if (int_reg & SDHC_STAT_DATA_END) {
			sg_miter_stop(sg_miter);
			ctr_sdhc_data_end_irq(host);
			return;
		}

		data = host->data;
		if (!data)
			break;
	}
	sg_miter_stop(sg_miter);
}

static irqreturn_t ctr_sdhc_thread_irq(int irq, void *dev_id)
{
	struct ctr_sdhc *host = dev_id;
	unsigned long flags;
	irqreturn_t ret = IRQ_HANDLED;

	spin_lock_irqsave(&host->lock, flags);
	if (host->data) {
		ctr_sdhc_pio(host);
	} else {
		dev_dbg(host->dev, "Spurious Data IRQ\n");
		if (host->cmd) {
			host->cmd->error = -EIO;
			ctr_sdhc_finish_request(host);
		}
		ret = IRQ_NONE;
	}
	spin_unlock_irqrestore(&host->lock, flags);
	return ret;
}

static void ctr_sdhc_respend_irq(struct ctr_sdhc *host)
{
	struct mmc_command *cmd = host->cmd;
	u32 response[4], *respbuf, i, reg;

	if (!host->cmd) {
		dev_err(host->dev, "Spurious CMD irq\n");
		return;
	}

	respbuf = (u32*)cmd->resp;
	host->cmd = NULL;

	for (i = 0, reg = SDHC_CMD_RESPONSE; i < 4; i++, reg += 4)
		response[i] = ioread32(host->regs + reg);

	if (cmd->flags & MMC_RSP_PRESENT && cmd->flags & MMC_RSP_136) {
		respbuf[0] = (response[3] << 8) | (response[2] >> 24);
		respbuf[1] = (response[2] << 8) | (response[1] >> 24);
		respbuf[2] = (response[1] << 8) | (response[0] >> 24);
		respbuf[3] = response[0] << 8;
	} else if (cmd->flags & MMC_RSP_PRESENT) {
		respbuf[0] = response[0];
	}

	if (!host->init_trace_done &&
	    (cmd->opcode == SD_IO_SEND_OP_COND ||
	     cmd->opcode == MMC_ALL_SEND_CID ||
	     cmd->opcode == MMC_SET_RELATIVE_ADDR ||
	     cmd->opcode == MMC_SELECT_CARD)) {
		dev_info(host->dev,
			"SDIO init CMD%d response %08x %08x %08x %08x error=%d\n",
			cmd->opcode, respbuf[0], respbuf[1], respbuf[2], respbuf[3],
			cmd->error);
		if (cmd->opcode == MMC_SELECT_CARD)
			host->init_trace_done = true;
	}

	dev_dbg(host->dev, "Command IRQ complete %d %d %x\n", cmd->opcode,
		cmd->error, cmd->flags);
	if (host->transport_trace_active &&
	    cmd->opcode == SD_IO_RW_EXTENDED)
		dev_info(host->dev,
			"AR6002 SDHC CMD53 command complete seq=%u error=%d data=%d\n",
			host->transport_trace_active_seq, cmd->error,
			host->data != NULL);

	/* If there is data to handle we will
	 * finish the request in the data end irq handler.*/
	if (host->data)
		return;

	ctr_sdhc_finish_request(host);
}

static irqreturn_t ctr_sdhc_irq(int irq, void *dev_id)
{
	struct ctr_sdhc *host = dev_id;
	u32 ack, int_reg;
	u16 int_data, int_data16;
	int error, loops = 0, ret = IRQ_NONE;
	bool trace_irq;

	spin_lock(&host->lock);

	/* This GIC input is edge-triggered.  Re-read until no actionable status
	 * remains so a FIFO/data edge that latches while the line is already high
	 * cannot disappear between the first status read and acknowledgement. */
	while (loops++ < 16) {
		error = 0;
		int_reg = ioread32(host->regs + SDHC_IRQ_STAT);
		int_data16 = ioread16(host->regs + SDHC_DATA_CTL);
		int_data = ioread16(host->regs + SDHC_DATA32_CTL);

		if (!(int_reg & SDHC_DEFAULT_IRQMASK))
			break;

		ret = IRQ_HANDLED;
		trace_irq = host->transport_trace_active &&
			(host->transport_irq_trace_count < 128);
		if (trace_irq) {
			host->transport_irq_trace_count++;
			dev_info(host->dev,
				"AR6002 SDHC CMD53 IRQ seq=%u pass=%d status=%08x data16_ctl=%04x data32_ctl=%04x\n",
				host->transport_trace_active_seq, loops, int_reg,
				int_data16, int_data);
		}

		/* The FIFO thread must still be able to observe response/data-end
		 * bits that arrived beside FIFO-ready; leave those asserted until its
		 * tail polling loop consumes them. */
		ack = int_reg & SDHC_DEFAULT_IRQMASK;
		if (int_reg & (SDHC_STAT_RX_READY | SDHC_STAT_TX_REQUEST))
			ack &= ~(SDHC_STAT_CMDRESPEND | SDHC_STAT_DATA_END);
		iowrite32(~ack, host->regs + SDHC_IRQ_STAT);
		if (trace_irq)
			dev_info(host->dev,
				"AR6002 SDHC CMD53 IRQ ack seq=%u status=%08x ack=%08x\n",
				host->transport_trace_active_seq, int_reg, ack);

		if (int_reg & (SDHC_STAT_CARDREMOVE | SDHC_STAT_CARDINSERT)) {
			if (int_reg & SDHC_STAT_CARDPRESENT)
				ctr_sdhc_reset(host);
			mmc_detect_change(host->mmc, 1);
		}

		if (int_reg & SDHC_ERR_CMD_TIMEOUT)
			error = -ETIMEDOUT;
		else if (int_reg & SDHC_ERR_CRC_FAIL)
			error = -EILSEQ;
		else if (int_reg & SDHC_ERR_MASK) {
			dev_err(host->dev, "buffer error: %08X\n",
				int_reg & SDHC_ERR_MASK);
			dev_err(host->dev, "detail error status %08X\n",
				ioread32(host->regs + SDHC_ERROR_STATUS));
			error = -EIO;
		}

		if (error) {
			if (host->cmd)
				host->cmd->error = error;
			if (host->data)
				host->data->error = error;
			if (host->cmd &&
			    host->cmd->opcode == SD_IO_RW_EXTENDED)
				dev_err(host->dev,
					"AR6002 SDHC CMD53 error status=%08x error=%d\n",
					int_reg, error);

			ctr_sdhc_reset(host);
			__ctr_sdhc_set_ios(host->mmc, &host->mmc->ios);
			if (host->data) {
				host->data->bytes_xfered = 0;
				sg_miter_stop(&host->sg_miter);
			}
			ctr_sdhc_finish_request(host);
			break;
		}

		if (int_reg & (SDHC_STAT_RX_READY | SDHC_STAT_TX_REQUEST)) {
			if (host->data && host->data->blksz <= pio_inline_max)
				ctr_sdhc_pio(host);
			else
				ret = IRQ_WAKE_THREAD;
			break;
		}

		if (int_reg & SDHC_STAT_CMDRESPEND)
			ctr_sdhc_respend_irq(host);
		if (int_reg & SDHC_STAT_DATA_END)
			ctr_sdhc_data_end_irq(host);
	}

	spin_unlock(&host->lock);
	return ret;
}

static irqreturn_t ctr_sdhc_sdio_irq(int irq, void *data)
{
	struct ctr_sdhc *host = data;
	u16 stat = ioread16(host->regs + SDHC_CARD_IRQ_STAT);
	u32 mask = stat & SDHC_SDIO_STAT_IOIRQ;
	iowrite16(stat & ~SDHC_SDIO_MASK_ALL,
		  host->regs + SDHC_CARD_IRQ_STAT);
	if (mask) {
		/* Keep a level-triggered card IRQ from retriggering until the MMC
		 * work item has drained the function and acknowledges it. */
		iowrite16(~0, host->regs + SDHC_CARD_IRQ_MASK);
		mmc_signal_sdio_irq(host->mmc);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

static void ctr_sdhc_sdio_poll(struct timer_list *t)
{
	struct ctr_sdhc *host = from_timer(host, t, sdio_timer);

	if (!host->sdio_irq_on)
		return;

	/*
	 * N3DS_SDIO_IRQ_TIMER_FALLBACK: the physical card-IRQ edge is not
	 * reliable on 3DS hardware.  With MMC_CAP_SDIO_IRQ set this signal
	 * bypasses the CCCR pending read and dispatches the single ath6k
	 * function directly.  This is the path used by the independently
	 * hardware-proven AR6014 bring-up and prevents the WMI READY mailbox
	 * notification from being lost after HTC setup.
	 */
	mmc_signal_sdio_irq(host->mmc);
	mod_timer(&host->sdio_timer,
		  jiffies + msecs_to_jiffies(SDHC_SDIO_POLL_MS));
}

static void ctr_sdhc_start_cmd(struct ctr_sdhc *host, struct mmc_command *cmd)
{
	struct mmc_data *data = host->data;
	int c = cmd->opcode;

	if (!host->init_trace_done &&
	    (cmd->opcode == SD_IO_SEND_OP_COND ||
	     cmd->opcode == MMC_ALL_SEND_CID ||
	     cmd->opcode == MMC_SET_RELATIVE_ADDR ||
	     cmd->opcode == MMC_SELECT_CARD))
		dev_info(host->dev, "SDIO init CMD%d arg=%08x flags=%08x\n",
			cmd->opcode, cmd->arg, cmd->flags);

	dev_dbg(host->dev, "Command opcode: %d\n", cmd->opcode);

	if (cmd->opcode == MMC_STOP_TRANSMISSION) {
		iowrite16(SDHC_STOP_INTERNAL_ISSUE,
			  host->regs + SDHC_STOP_INTERNAL);

		cmd->resp[0] = cmd->opcode;
		cmd->resp[1] = 0;
		cmd->resp[2] = 0;
		cmd->resp[3] = 0;

		ctr_sdhc_finish_request(host);
		return;
	}

	switch (mmc_resp_type(cmd)) {
	case MMC_RSP_NONE:
		c |= SDHC_CMDRSP_NONE;
		break;
	case MMC_RSP_R1:
		c |= SDHC_CMDRSP_R1;
		break;
	case MMC_RSP_R1B:
		c |= SDHC_CMDRSP_R1B;
		break;
	case MMC_RSP_R2:
		c |= SDHC_CMDRSP_R2;
		break;
	case MMC_RSP_R3:
		c |= SDHC_CMDRSP_R3;
		break;

	default:
		dev_err(host->dev, "Unknown response type %d\n",
			mmc_resp_type(cmd));
		break;
	}

	host->cmd = cmd;

	if (cmd->opcode == SD_IO_RW_EXTENDED)
		c |= SDHC_CMD_SECURE;

	if (cmd->opcode == SD_IO_RW_DIRECT)
		c |= SDHC_CMD_SECURE;

	if (cmd->opcode == MMC_APP_CMD)
		c |= SDHC_CMDTYPE_APP;

	if (cmd->opcode == MMC_GO_IDLE_STATE)
		c |= SDHC_CMDRSP_NONE;

	if (data) {
		c |= SDHC_CMD_DATA_XFER;

		/* This TMIO-derived controller expects its stop/multi transfer path
		 * for every MMC data request, including one-block CMD53 byte-mode
		 * transfers.  The public 3DS transport that completes firmware upload
		 * and scanning uses this programming; the prior >1 experiment still
		 * hung on the first 256-byte mailbox read. */
		if (data->blocks > 0) {
			iowrite16(SDHC_STOP_INTERNAL_ENABLE,
				  host->regs + SDHC_STOP_INTERNAL);
			c |= SDHC_CMD_DATA_MULTI;
		} else {
			iowrite16(0, host->regs + SDHC_STOP_INTERNAL);
		}

		if (data->flags & MMC_DATA_READ)
			c |= SDHC_CMD_DATA_READ;
	}

	iowrite32(cmd->arg, host->regs + SDHC_CMD_PARAM);
	iowrite16(c, host->regs + SDHC_CMD);
}

static void ctr_sdhc_start_data(struct ctr_sdhc *host, struct mmc_data *data)
{
	unsigned int flags = SG_MITER_ATOMIC;

	dev_dbg(host->dev,
		"setup data transfer: blocksize %08x "
		"nr_blocks %d, offset: %08x\n",
		data->blksz, data->blocks, data->sg->offset);

	host->data = data;

	if (data->flags & MMC_DATA_READ)
		flags |= SG_MITER_TO_SG;
	else
		flags |= SG_MITER_FROM_SG;

	sg_miter_start(&host->sg_miter, data->sg, data->sg_len, flags);

	iowrite16(data->blksz, host->regs + SDHC_DATA16_BLK_LEN);
	iowrite16(data->blocks, host->regs + SDHC_DATA16_BLK_CNT);
}

/* Process requests from the MMC layer */
static void ctr_sdhc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct ctr_sdhc *host = mmc_priv(mmc);
	unsigned long flags;

	/* Phase 2 (WiFi bring-up): this was a second, independent check of
	 * the same physical card-detect register bit that ctr_sdhc_get_cd()
	 * already checks -- and unlike get_cd(), the MMC core has no way to
	 * bypass THIS one via MMC_CAP_NONREMOVABLE, since it runs inside the
	 * driver's own request() callback. It silently failed every single
	 * request with -ENOMEDIUM before the command ever reached the
	 * hardware, which is why nothing showed up in dmesg at all (no
	 * command was ever sent) and why the MMC core logged nothing (an
	 * expected "no card" outcome, not an error). This controller only
	 * ever drives the permanently-attached AR6014G chip (see Kconfig
	 * help text) -- there is no removable-card scenario where this
	 * check is legitimate, so it's removed entirely rather than made
	 * conditional.
	 */

	spin_lock_irqsave(&host->lock, flags);

	WARN_ON(host->mrq != NULL);

	host->mrq = mrq;
	host->transport_trace_active = false;
	if (cmd53_success_trace && mrq->cmd &&
	    mrq->cmd->opcode == SD_IO_RW_EXTENDED &&
	    host->transport_trace_count < 64) {
		host->transport_trace_active = true;
		host->transport_trace_active_seq = ++host->transport_trace_seq;
		host->transport_trace_count++;
		dev_info(host->dev,
			"AR6002 SDHC CMD53 submit seq=%u arg=%08x flags=%08x len=%u blocks=%u blksz=%u\n",
			host->transport_trace_active_seq, mrq->cmd->arg,
			mrq->cmd->flags,
			mrq->data ? mrq->data->blksz * mrq->data->blocks : 0,
			mrq->data ? mrq->data->blocks : 0,
			mrq->data ? mrq->data->blksz : 0);
	}

	/* N3DS_HIF_SYNC_VS_SDHC_WATCHDOG: this per-command hardware watchdog
	 * must stay SHORTER than ath6k_legacy's HIF_SYNC_REQUEST_TIMEOUT_MS
	 * (hif.c), since that layer's synchronous requests can only ever be
	 * recovered by this watchdog resetting a wedged CMD53 -- it has no
	 * way to cancel one itself. If the ordering is ever flipped again the
	 * software layer gives up on still-in-flight, still-recoverable
	 * transactions and logs spurious "HIF DSR transient ... poller will
	 * retry" noise instead of observing a clean reset. Keep both bounds
	 * in sync when changing either one. */
	mod_timer(&host->timeout_timer, jiffies + 5 * HZ);

	if (mrq->data)
		ctr_sdhc_start_data(host, mrq->data);

	ctr_sdhc_start_cmd(host, mrq->cmd);

	spin_unlock_irqrestore(&host->lock, flags);
}

static void ctr_sdhc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct ctr_sdhc *host = mmc_priv(mmc);
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	__ctr_sdhc_set_ios(mmc, ios);
	spin_unlock_irqrestore(&host->lock, flags);
}

static int ctr_sdhc_get_ro(struct mmc_host *mmc)
{
	struct ctr_sdhc *host = mmc_priv(mmc);
	return !(ioread16(host->regs + SDHC_IRQ_STAT) & SDHC_STAT_WRITEPROT);
}

static int ctr_sdhc_get_cd(struct mmc_host *mmc)
{
	struct ctr_sdhc *host = mmc_priv(mmc);
	return !!(ioread16(host->regs + SDHC_IRQ_STAT) & SDHC_STAT_CARDPRESENT);
}

static void ctr_sdhc_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct ctr_sdhc *host = mmc_priv(mmc);

	/* CARD_IRQ_CTL is a TMIO transaction-control register, not an SDIO
	 * interrupt enable.  Writing it here stalls a later CMD53. */
	if (enable) {
		ctr_sdhc_sdio_arm(host, true);
		host->sdio_irq_on = true;
		mod_timer(&host->sdio_timer,
			  jiffies + msecs_to_jiffies(SDHC_SDIO_POLL_MS));

		/* Re-arming an already asserted level produces no new edge. */
		if (ioread16(host->regs + SDHC_CARD_IRQ_STAT) &
		    SDHC_SDIO_STAT_IOIRQ)
			mmc_signal_sdio_irq(mmc);
	} else {
		host->sdio_irq_on = false;
		del_timer(&host->sdio_timer);
		ctr_sdhc_sdio_arm(host, false);
	}
}

static void ctr_sdhc_ack_sdio_irq(struct mmc_host *mmc)
{
	/* sdio_irq_work() uses this callback after the ath6k handler returns. */
	ctr_sdhc_enable_sdio_irq(mmc, 1);
}

/* N3DS_WIFI_SDIO_RECOVERY: fwmode is loaded only at target boot.  A
 * failed AP/STA module transition can leave the soldered AR6014 alive while
 * the MMC core still believes the old SDIO card is enumerated.  Expose one
 * root-only, synchronous recovery edge: remove the card, power-cycle it, and
 * let the normal MMC core enumerate a fresh function. */
static ssize_t wifi_recover_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count)
{
	struct ctr_sdhc *host = dev_get_drvdata(dev);
	int ret;

	if (!host || !sysfs_streq(buf, "1"))
		return -EINVAL;

	mutex_lock(&host->recovery_lock);
	dev_info(dev, "WiFi SDIO recovery begin\n");
	if (host->host_registered) {
		mmc_remove_host(host->mmc);
		host->host_registered = false;
	}
	del_timer_sync(&host->timeout_timer);
	del_timer_sync(&host->sdio_timer);
	host->sdio_irq_on = false;
	gpiod_set_value_cansleep(host->wifi_en, 0);
	msleep(100);
	ctr_sdhc_reset(host);
	gpiod_set_value_cansleep(host->wifi_en, 1);
	msleep(50);
	host->mmc->rescan_entered = 0;
	host->init_trace_done = false;
	ret = mmc_add_host(host->mmc);
	if (!ret) {
		host->host_registered = true;
		flush_delayed_work(&host->mmc->detect);
		if (!host->mmc->card)
			ret = -ENODEV;
	}
	if (!ret) {
		dev_info(dev, "WiFi SDIO recovery complete; card present\n");
	} else {
		dev_err(dev, "WiFi SDIO recovery re-enumeration failed: %d\n", ret);
	}
	mutex_unlock(&host->recovery_lock);
	return ret ? ret : count;
}

static DEVICE_ATTR(wifi_recover, 0200, NULL, wifi_recover_store);

static const struct mmc_host_ops ctr_sdhc_ops = {
	.request = ctr_sdhc_request,
	.set_ios = ctr_sdhc_set_ios,
	.get_ro = ctr_sdhc_get_ro,
	.get_cd = ctr_sdhc_get_cd,
	.enable_sdio_irq = ctr_sdhc_enable_sdio_irq,
	.ack_sdio_irq = ctr_sdhc_ack_sdio_irq,
};

#ifdef CONFIG_PM_SLEEP
static int ctr_sdhc_pm_suspend(struct device *dev)
{
	struct ctr_sdhc *host = dev_get_drvdata(dev);
	del_timer_sync(&host->timeout_timer);
	del_timer_sync(&host->sdio_timer);
	iowrite32(~0, host->regs + SDHC_IRQ_MASK);
	iowrite16(0, host->regs + SDHC_CARD_CLKCTL);
	return 0;
}

static int ctr_sdhc_pm_resume(struct device *dev)
{
	struct ctr_sdhc *host = dev_get_drvdata(dev);
	ctr_sdhc_reset(host);
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static int ctr_sdhc_probe(struct platform_device *pdev)
{
	int ret;
	struct clk *sdclk;
	struct device *dev;
	struct mmc_host *mmc;
	unsigned long clkrate;
	struct ctr_sdhc *host;
	struct gpio_desc *wifi_en;

	dev = &pdev->dev;

	/* The permanently attached AR6002/AR6014 WLAN target is held in reset
	 * until its SDIO host is ready. Make the DT power/reset line mandatory:
	 * probing without it leaves the target absent while looking like a live
	 * MMC host. The line is asserted low for 100 ms and released high for
	 * 50 ms before mmc_add_host() starts SDIO enumeration. */
	wifi_en = devm_gpiod_get(dev, "wifi-enable", GPIOD_OUT_LOW);
	if (IS_ERR(wifi_en)) {
		ret = PTR_ERR(wifi_en);
		return dev_err_probe(dev, ret,
				     "WiFi power GPIO unavailable\n");
	}

	dev_info(dev, "WiFi power GPIO low; holding reset for 100 ms\n");
	msleep(100);
	gpiod_set_value_cansleep(wifi_en, 1);
	dev_info(dev, "WiFi power GPIO high; waiting 50 ms before SDIO\n");
	msleep(50);

	sdclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sdclk))
		return PTR_ERR(sdclk);

	ret = clk_prepare_enable(sdclk);
	if (ret)
		return ret;
	clkrate = clk_get_rate(sdclk);

	mmc = mmc_alloc_host(sizeof(struct ctr_sdhc), dev);
	if (!mmc)
		return -ENOMEM;

	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->sdclk = sdclk;
	host->wifi_en = wifi_en;
	mutex_init(&host->recovery_lock);
	host->host_registered = false;

	host->dev = dev;
	platform_set_drvdata(pdev, host);

	host->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->regs)) {
		ret = -ENOMEM;
		goto free_mmc;
	}

	mmc->ops = &ctr_sdhc_ops;
	/* Phase 2 (WiFi bring-up): this controller only ever drives the permanently-attached AR6014G WiFi chip (see Kconfig help text), never a real removable card. The stock driver never called mmc_of_parse() so the device-tree non-removable property was silently ignored, and get_cd() checks a card-detect register bit that a soldered-on chip with no card slot never sets -- so the MMC core always believed no card was present and never probed it. Force MMC_CAP_NONREMOVABLE directly, matching this driver existing style of hardcoding caps rather than DT-parsing them. */
	/* The physical card-IRQ edge is unreliable, but MMC_CAP_SDIO_IRQ is
	 * still required so the 10 ms fallback can dispatch IRQ work without
	 * depending on the card's CCCR pending indication. */
	mmc->caps = MMC_CAP_4_BIT_DATA | MMC_CAP_NONREMOVABLE |
		    MMC_CAP_SDIO_IRQ;
	mmc->ocr_avail = MMC_VDD_32_33;
	mmc->max_blk_size = 0x200;
	mmc->max_blk_count = 0xFFFF;

	mmc->f_min = clkrate / 512;
	mmc->f_max = clkrate / 2;

	spin_lock_init(&host->lock);
	timer_setup(&host->timeout_timer, ctr_sdhc_timeout, 0);
	timer_setup(&host->sdio_timer, ctr_sdhc_sdio_poll, 0);

	ctr_sdhc_reset(host);
	dev_info(host->dev,
		"AR6002 SDHC PIO edge fix=v3 data16_ctl=%04x data32_ctl=%04x blk32=%u\n",
		ioread16(host->regs + SDHC_DATA_CTL),
		ioread16(host->regs + SDHC_DATA32_CTL),
		ioread16(host->regs + SDHC_DATA32_BLK_CNT));
	dev_info(host->dev,
		 "AR6002 SDHC IRQ timer fallback=v4 period=%ums cap=hardware (N3DS_SDIO_POLL_REALTIME)\n",
		 SDHC_SDIO_POLL_MS);

	ret = devm_request_threaded_irq(dev, platform_get_irq(pdev, 0),
					ctr_sdhc_irq, ctr_sdhc_thread_irq,
					IRQF_SHARED, DRIVER_NAME, host);
	if (ret)
		goto free_mmc;

	ret = devm_request_irq(dev, platform_get_irq(pdev, 1),
			       ctr_sdhc_sdio_irq, 0, DRIVER_NAME, host);
	if (ret)
		goto free_mmc;

	ret = mmc_add_host(mmc);
	if (ret)
		goto free_mmc;
	host->host_registered = true;
	ret = device_create_file(dev, &dev_attr_wifi_recover);
	if (ret) {
		mmc_remove_host(mmc);
		host->host_registered = false;
		goto free_mmc;
	}
	pm_suspend_ignore_children(&pdev->dev, 1);
	return 0;

free_mmc:
	mmc_free_host(mmc);
	return ret;
}

static const struct dev_pm_ops ctr_sdhc_pm_ops = { SET_SYSTEM_SLEEP_PM_OPS(
	ctr_sdhc_pm_suspend, ctr_sdhc_pm_resume) };

static const struct of_device_id ctr_sdhc_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{},
};
MODULE_DEVICE_TABLE(of, ctr_sdhc_of_match);

static struct platform_driver ctr_sdhc_driver = {
	.probe = ctr_sdhc_probe,

	.driver = { .name = DRIVER_NAME,
		    .owner = THIS_MODULE,
		    .of_match_table = of_match_ptr(ctr_sdhc_of_match),
		    .pm = &ctr_sdhc_pm_ops },
};

module_platform_driver(ctr_sdhc_driver);

MODULE_DESCRIPTION("Nintendo 3DS SDHC driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
