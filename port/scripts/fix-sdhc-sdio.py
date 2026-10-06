#!/usr/bin/env python3
"""
fix-sdhc-sdio.py - fix the SDIO data-transfer hang in the 3DS SDHC driver.

Symptom (docs/WIFI.md):

    ath6kl gets all the way through the firmware upload and WMI_READY, then the
    driver hangs forever reading the first HTC packet.  ksdioirqd/mmc0 holds
    the MMC host inside mmc_wait_for_req_done() and the ath6kl probe waits for
    the host in __mmc_claim_host().  wlan0 is never registered, so Android's
    WifiService fails to load the driver and Settings shows "Error".

Cause:

    ctr_sdhc_irq() acknowledges *all* pending status bits at the top, then
    returns through the RX_READY/TX_REQUEST (FIFO) path:

        iowrite32(~(int_reg & SDHC_DEFAULT_IRQMASK), SDHC_IRQ_STAT);
        if (int_reg & (RX_READY | TX_REQUEST)) {
                ret = IRQ_WAKE_THREAD;
                goto irq_end;              /* DATAEND already acked */
        }
        ...
        if (int_reg & DATA_END)
                ctr_sdhc_data_end_irq(host);

    When a read's whole block already sits in the FIFO, RX_READY and DATAEND
    are latched together.  The controller's IRQ latch is *edge-triggered*
    (GBATEK "DSi SD/MMC I/O Ports: Interrupt/Status": "IF2.bit8 gets set only
    on No-IRQ-to-IRQ transitions ... IRQ(s) would get lost"), so a DATAEND that
    is acknowledged but not processed is lost forever and the request never
    completes.

Fix:

    Keep acknowledging the whole pending set (the edge has to be taken) but
    remember DATAEND; the threaded FIFO handler moves the bytes and then
    finishes the request.  Track transferred bytes so a multi-block read is
    only completed after the last block left the FIFO.

This script is idempotent.
"""

import sys
import pathlib

def die(msg):
    sys.exit("fix-sdhc-sdio.py: " + msg)

def replace_once(s, old, new, what):
    if new in s:
        return s, False
    if old not in s:
        die("%s: anchor not found" % what)
    return s.replace(old, new, 1), True

def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else "/root/p3ds/src/linux-3ds")
    c = kd / "drivers/platform/nintendo3ds/ctr_sdhc.c"
    h = kd / "drivers/platform/nintendo3ds/ctr_sdhc.h"
    if not c.exists() or not h.exists():
        die("no 3DS SDHC driver at %s" % c)

    # ------------------------------------------------------------------
    # ctr_sdhc.h: remember a latched DATAEND + count transferred bytes
    # ------------------------------------------------------------------
    s = h.read_text()
    if "data_end_pending" not in s:
        old = ("\tstruct sg_mapping_iter sg_miter;\n"
               "};\n")
        new = ("\tstruct sg_mapping_iter sg_miter;\n"
               "\n"
               "\t/* The controller's IRQ latch is edge-triggered: DATAEND can\n"
               "\t * be latched together with RX_READY when the whole block is\n"
               "\t * already in the FIFO, so it has to be remembered and\n"
               "\t * processed after the threaded handler drained the FIFO. */\n"
               "\tbool data_end_pending;\n"
               "\tunsigned int xfered;\n"
               "};\n")
        s, ch = replace_once(s, old, new, "ctr_sdhc.h struct")
        if ch:
            h.write_text(s)
        print("    ctr_sdhc.h: data_end_pending/xfered %s"
              % ("added" if ch else "present"))
    else:
        print("    ctr_sdhc.h: already patched")

    s = c.read_text()
    changed = []

    # ------------------------------------------------------------------
    # ctr_sdhc.c: forward-declare the DATAEND helper (the threaded handler
    # is defined before ctr_sdhc_irq and calls it)
    # ------------------------------------------------------------------
    old = "static void ctr_sdhc_data_end_irq(struct ctr_sdhc *host)\n{\n"
    new = ("static void ctr_sdhc_data_end_pending(struct ctr_sdhc *host);\n"
           "\n" + old)
    if "static void ctr_sdhc_data_end_pending(struct ctr_sdhc *host);" not in s:
        s, ch = replace_once(s, old, new, "forward declaration")
        if ch:
            changed.append("forward declaration")

    # ------------------------------------------------------------------
    # reset the per-request bookkeeping when data starts
    # ------------------------------------------------------------------
    old = ("\thost->data = data;\n"
           "\n"
           "\tif (data->flags & MMC_DATA_READ)\n")
    new = ("\thost->data = data;\n"
           "\thost->xfered = 0;\n"
           "\thost->data_end_pending = false;\n"
           "\n"
           "\tif (data->flags & MMC_DATA_READ)\n")
    if "host->xfered = 0;" not in s:
        s, ch = replace_once(s, old, new, "ctr_sdhc_start_data")
        if ch:
            changed.append("start_data resets bookkeeping")

    # ------------------------------------------------------------------
    # threaded handler: count bytes, process a remembered DATAEND
    # ------------------------------------------------------------------
    old = ("\tsg_miter->consumed = count;\n"
           "\tsg_miter_stop(sg_miter);\n"
           "\n"
           "done:\n"
           "\tspin_unlock_irqrestore(&host->lock, flags);\n"
           "\treturn IRQ_HANDLED;\n"
           "}\n")
    new = ("\thost->xfered += count;\n"
           "\tsg_miter->consumed = count;\n"
           "\tsg_miter_stop(sg_miter);\n"
           "\n"
           "done:\n"
           "\t/* DATAEND can be latched in the same status read as RX_READY;\n"
           "\t * the hard IRQ only remembered it.  Finish the request once\n"
           "\t * every byte of it actually left/entered the FIFO. */\n"
           "\tif (host->data_end_pending)\n"
           "\t\tctr_sdhc_data_end_pending(host);\n"
           "\n"
           "\tspin_unlock_irqrestore(&host->lock, flags);\n"
           "\treturn IRQ_HANDLED;\n"
           "}\n")
    if "host->xfered += count;" not in s:
        s, ch = replace_once(s, old, new, "ctr_sdhc_thread_irq")
        if ch:
            changed.append("thread handler processes DATAEND")

    # ------------------------------------------------------------------
    # helper + fixed hard IRQ
    # ------------------------------------------------------------------
    old_irq = (
        "static irqreturn_t ctr_sdhc_irq(int irq, void *dev_id)\n"
        "{\n"
        "\tstruct ctr_sdhc *host = dev_id;\n"
        "\tu32 int_reg, int_data;\n"
        "\tint error = 0, ret = IRQ_HANDLED;\n"
        "\n"
        "\tspin_lock(&host->lock);\n"
        "\n"
        "\tint_reg = ioread32(host->regs + SDHC_IRQ_STAT);\n"
        "\tint_data = ioread16(host->regs + SDHC_DATA32_CTL);\n"
        "\n"
        "\tdev_dbg(host->dev, \"IRQ status: %x\\n\", int_reg);\n"
        "\n"
        "\tif (!int_reg) {\n"
        "\t\tret = IRQ_NONE;\n"
        "\t\tgoto irq_end;\n"
        "\t}\n"
        "\n"
        "\tiowrite32(~(int_reg & SDHC_DEFAULT_IRQMASK),\n"
        "\t\t  host->regs + SDHC_IRQ_STAT);\n"
        "\n"
        "\tif (int_reg & (SDHC_STAT_CARDREMOVE | SDHC_STAT_CARDINSERT)) {\n"
        "\t\tif (int_reg & SDHC_STAT_CARDPRESENT)\n"
        "\t\t\tctr_sdhc_reset(host);\n"
        "\t\tmmc_detect_change(host->mmc, 1);\n"
        "\t}\n"
        "\n"
        "\tif (int_reg & SDHC_ERR_CMD_TIMEOUT) {\n"
        "\t\terror = -ETIMEDOUT;\n"
        "\t} else if (int_reg & SDHC_ERR_CRC_FAIL) {\n"
        "\t\terror = -EILSEQ;\n"
        "\t} else if (int_reg & SDHC_ERR_MASK) {\n"
        "\t\tdev_err(host->dev, \"buffer error: %08X\\n\",\n"
        "\t\t\tint_reg & SDHC_ERR_MASK);\n"
        "\t\tdev_err(host->dev, \"detail error status %08X\\n\",\n"
        "\t\t\tioread32(host->regs + SDHC_ERROR_STATUS));\n"
        "\t\terror = -EIO;\n"
        "\t}\n"
        "\n"
        "\tif (error) {\n"
        "\t\tif (host->cmd)\n"
        "\t\t\thost->cmd->error = error;\n"
        "\n"
        "\t\tif (error != -ETIMEDOUT) {\n"
        "\t\t\tctr_sdhc_reset(host);\n"
        "\t\t\t__ctr_sdhc_set_ios(host->mmc, &host->mmc->ios);\n"
        "\t\t\tgoto irq_end;\n"
        "\t\t}\n"
        "\t}\n"
        "\n"
        "\tif (int_reg & (SDHC_STAT_RX_READY | SDHC_STAT_TX_REQUEST)) {\n"
        "\t\tret = IRQ_WAKE_THREAD;\n"
        "\t\tgoto irq_end;\n"
        "\t}\n"
        "\n"
        "\tif (int_reg & SDHC_STAT_CMDRESPEND)\n"
        "\t\tctr_sdhc_respend_irq(host);\n"
        "\n"
        "\tif (int_reg & SDHC_STAT_DATA_END)\n"
        "\t\tctr_sdhc_data_end_irq(host);\n"
        "\n"
        "irq_end:\n"
        "\tspin_unlock(&host->lock);\n"
        "\treturn ret;\n"
        "}\n")

    new_irq = (
        "/*\n"
        " * Finish a request whose DATAEND was latched together with a FIFO\n"
        " * request.  For a read the last block can still be sitting in the\n"
        " * controller FIFO, so wait until the threaded handler has moved all\n"
        " * of the bytes before completing.\n"
        " */\n"
        "static void ctr_sdhc_data_end_pending(struct ctr_sdhc *host)\n"
        "{\n"
        "\tstruct mmc_data *data = host->data;\n"
        "\n"
        "\tif (data && host->xfered < data->blocks * data->blksz)\n"
        "\t\treturn;\n"
        "\n"
        "\thost->data_end_pending = false;\n"
        "\tctr_sdhc_data_end_irq(host);\n"
        "}\n"
        "\n"
        "static irqreturn_t ctr_sdhc_irq(int irq, void *dev_id)\n"
        "{\n"
        "\tstruct ctr_sdhc *host = dev_id;\n"
        "\tu32 int_reg, int_data;\n"
        "\tint error = 0, ret = IRQ_HANDLED;\n"
        "\n"
        "\tspin_lock(&host->lock);\n"
        "\n"
        "\tint_reg = ioread32(host->regs + SDHC_IRQ_STAT);\n"
        "\tint_data = ioread16(host->regs + SDHC_DATA32_CTL);\n"
        "\n"
        "\tdev_dbg(host->dev, \"IRQ status: %x\\n\", int_reg);\n"
        "\n"
        "\tif (!int_reg) {\n"
        "\t\tret = IRQ_NONE;\n"
        "\t\tgoto irq_end;\n"
        "\t}\n"
        "\n"
        "\t/*\n"
        "\t * The IRQ latch is edge-triggered: every enabled bit not\n"
        "\t * acknowledged here is lost.  Acknowledge the whole pending set,\n"
        "\t * but remember DATAEND - it is latched together with RX_READY\n"
        "\t * when the block already fits in the FIFO, and must be processed\n"
        "\t * only after the threaded handler drained the FIFO.\n"
        "\t */\n"
        "\tiowrite32(~(int_reg & SDHC_DEFAULT_IRQMASK),\n"
        "\t\t  host->regs + SDHC_IRQ_STAT);\n"
        "\n"
        "\tif (int_reg & SDHC_STAT_DATA_END)\n"
        "\t\thost->data_end_pending = true;\n"
        "\n"
        "\tif (int_reg & (SDHC_STAT_CARDREMOVE | SDHC_STAT_CARDINSERT)) {\n"
        "\t\tif (int_reg & SDHC_STAT_CARDPRESENT)\n"
        "\t\t\tctr_sdhc_reset(host);\n"
        "\t\tmmc_detect_change(host->mmc, 1);\n"
        "\t}\n"
        "\n"
        "\tif (int_reg & SDHC_ERR_CMD_TIMEOUT) {\n"
        "\t\terror = -ETIMEDOUT;\n"
        "\t} else if (int_reg & SDHC_ERR_CRC_FAIL) {\n"
        "\t\terror = -EILSEQ;\n"
        "\t} else if (int_reg & SDHC_ERR_MASK) {\n"
        "\t\tdev_err(host->dev, \"buffer error: %08X\\n\",\n"
        "\t\t\tint_reg & SDHC_ERR_MASK);\n"
        "\t\tdev_err(host->dev, \"detail error status %08X\\n\",\n"
        "\t\t\tioread32(host->regs + SDHC_ERROR_STATUS));\n"
        "\t\terror = -EIO;\n"
        "\t}\n"
        "\n"
        "\tif (error) {\n"
        "\t\thost->data_end_pending = false;\n"
        "\t\tif (host->cmd)\n"
        "\t\t\thost->cmd->error = error;\n"
        "\n"
        "\t\tif (error != -ETIMEDOUT) {\n"
        "\t\t\tctr_sdhc_reset(host);\n"
        "\t\t\t__ctr_sdhc_set_ios(host->mmc, &host->mmc->ios);\n"
        "\t\t\tgoto irq_end;\n"
        "\t\t}\n"
        "\t}\n"
        "\n"
        "\t/* Safe while data is still in the FIFO: respend_irq() only\n"
        "\t * records cmd->resp and defers completion to the data path. */\n"
        "\tif (int_reg & SDHC_STAT_CMDRESPEND)\n"
        "\t\tctr_sdhc_respend_irq(host);\n"
        "\n"
        "\tif (int_reg & (SDHC_STAT_RX_READY | SDHC_STAT_TX_REQUEST)) {\n"
        "\t\tret = IRQ_WAKE_THREAD;\n"
        "\t\tgoto irq_end;\n"
        "\t}\n"
        "\n"
        "\tif (host->data_end_pending)\n"
        "\t\tctr_sdhc_data_end_pending(host);\n"
        "\n"
        "irq_end:\n"
        "\tspin_unlock(&host->lock);\n"
        "\treturn ret;\n"
        "}\n")

    if "host->data_end_pending = true;" in s:
        print("    ctr_sdhc.c: irq handler already patched")
    elif old_irq in s:
        s = s.replace(old_irq, new_irq, 1)
        changed.append("edge-safe DATAEND handling")
    else:
        die("ctr_sdhc.c: ctr_sdhc_irq anchor not found")

    if changed:
        c.write_text(s)
        print("    ctr_sdhc.c: " + ", ".join(changed))
    else:
        print("    ctr_sdhc.c: already patched")

    print("fix-sdhc-sdio.py: done")

if __name__ == "__main__":
    main()
