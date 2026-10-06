//------------------------------------------------------------------------------
// <copyright file="hif.c" company="Atheros">
//    Copyright (c) 2004-2010 Atheros Corporation.  All rights reserved.
// 
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
//
//------------------------------------------------------------------------------
//==============================================================================
// HIF layer reference implementation for Linux Native MMC stack
//
// Author(s): ="Atheros"
//==============================================================================
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sd.h>
#include <linux/kthread.h>

/* by default setup a bounce buffer for the data packets, if the underlying host controller driver
   does not use DMA you may be able to skip this step and save the memory allocation and transfer time */
#define HIF_USE_DMA_BOUNCE_BUFFER 1
#include "hif_internal.h"
#define ATH_MODULE_NAME hif
#include "a_debug.h"
#include "hw/mbox_host_reg.h"

#if HIF_USE_DMA_BOUNCE_BUFFER
/* macro to check if DMA buffer is WORD-aligned and DMA-able.  Most host controllers assume the
 * buffer is DMA'able and will bug-check otherwise (i.e. buffers on the stack).  
 * virt_addr_valid check fails on stack memory.  
 */
#define BUFFER_NEEDS_BOUNCE(buffer)  (((unsigned long)(buffer) & 0x3) || !virt_addr_valid((buffer)))
#else
#define BUFFER_NEEDS_BOUNCE(buffer)   (false)
#endif

/* ATHENV */
#if defined(CONFIG_PM)
#define dev_to_sdio_func(d)	container_of(d, struct sdio_func, dev)
#define to_sdio_driver(d)      container_of(d, struct sdio_driver, drv)
#endif /* CONFIG_PM */
static void delHifDevice(struct hif_device * device);
static int Func0_CMD52WriteByte(struct mmc_card *card, unsigned int address, unsigned char byte);
static int Func0_CMD52ReadByte(struct mmc_card *card, unsigned int address, unsigned char *byte);

static int hifEnableFunc(struct hif_device *device, struct sdio_func *func);
static int hifDisableFunc(struct hif_device *device, struct sdio_func *func);
OSDRV_CALLBACKS osdrvCallbacks;

int reset_sdio_on_unload = 0;
module_param(reset_sdio_on_unload, int, 0644);

extern u32 nohifscattersupport;

/* A synchronous mailbox-status read must not strand the HTC startup task
 * forever if the SDIO async worker or controller stops completing requests.
 * This is deliberately longer than HTC_TARGET_RESPONSE_TIMEOUT (2 seconds),
 * so a healthy poll still owns the normal HTC timeout and this is a lower
 * layer escape hatch for a genuinely wedged bus operation.
 *
 * N3DS_HIF_SYNC_VS_SDHC_WATCHDOG: this bound MUST stay longer than the
 * 3ds-sdhc platform driver's own per-command hardware watchdog
 * (ctr_sdhc.c: mod_timer(&host->timeout_timer, jiffies + 5*HZ), i.e. 5000ms).
 * async_task() holds sdio_claim_host() for an entire queued batch and calls
 * the blocking __HIFReadWrite() directly -- the only thing that can ever
 * bound a genuinely wedged CMD53 is that 5s controller-level watchdog
 * (ctr_sdhc_timeout() -> ctr_sdhc_reset()). The caller waiting here in
 * HIFReadWrite() (down_timeout on busrequest->sem_req) has no way to cancel
 * or reset that in-flight transaction; it can only detach and let the
 * worker free the request later (see "AR6002 HIF reclaimed timed-out sync
 * req" below). Previously this was 3000ms, i.e. *shorter* than the
 * controller's own recovery window: every periodic 10ms HOST_INT_STATUS
 * poll (addr=0x400) that happened to queue up behind one slow-but-still-
 * recoverable hardware transaction gave up at 3s and logged a spurious
 * "AR6002 HIF DSR transient status=-1; poller will retry", while the
 * *actual* transaction (and every other poll queued behind it, since
 * async_task drains its batch serially under one host claim) kept
 * occupying the bus for up to 2 more seconds until ctr_sdhc's watchdog
 * finally fired. That inverted ordering is what produced repeating
 * "poller will retry" bursts plus "3ds-sdhc ... request timeout" /
 * "MMC stack returned : -110" pairs once the device had been associated
 * and idle-polling long enough for a transaction to run slow. Keep this
 * strictly greater than the SDHC watchdog above so the hardware layer is
 * always the one that recovers a wedged command; the software layer should
 * only ever see a clean post-reset completion, never a live one it has to
 * abandon mid-flight. */
#define HIF_SYNC_REQUEST_TIMEOUT_MS 6000

static struct hif_device *ath6kl_alloc_hifdev(struct sdio_func *func)
{
	struct hif_device *hifdevice;

	hifdevice = kzalloc(sizeof(struct hif_device), GFP_KERNEL);

#if HIF_USE_DMA_BOUNCE_BUFFER
	hifdevice->dma_buffer = kmalloc(HIF_DMA_BUFFER_SIZE, GFP_KERNEL);
#endif
	hifdevice->func = func;
	hifdevice->powerConfig = HIF_DEVICE_POWER_UP;
	sdio_set_drvdata(func, hifdevice);

	return hifdevice;
}

static struct hif_device *ath6kl_get_hifdev(struct sdio_func *func)
{
	return (struct hif_device *) sdio_get_drvdata(func);
}

static const struct sdio_device_id ath6kl_hifdev_ids[] = {
	{ SDIO_DEVICE(MANUFACTURER_CODE, (MANUFACTURER_ID_AR6002_BASE | 0x0)) },
	{ SDIO_DEVICE(MANUFACTURER_CODE, (MANUFACTURER_ID_AR6002_BASE | 0x1)) },
	{ SDIO_DEVICE(MANUFACTURER_CODE, (MANUFACTURER_ID_AR6003_BASE | 0x0)) },
	{ SDIO_DEVICE(MANUFACTURER_CODE, (MANUFACTURER_ID_AR6003_BASE | 0x1)) },
	{ /* null */                                         },
};

MODULE_DEVICE_TABLE(sdio, ath6kl_hifdev_ids);

static int ath6kl_hifdev_probe(struct sdio_func *func,
			       const struct sdio_device_id *id)
{
	int ret;
	struct hif_device *device;
	int count;

	/* This is identity-only telemetry; do not issue any extra SDIO/BMI
	 * transaction here.  Keep it at error visibility so a filtered boot
	 * trace still records which function actually matched the driver. */
	AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
			("ath6kl SDIO probe identity: function: 0x%X, Vendor ID: 0x%X, "
			 "Device ID: 0x%X, block size: 0x%X/0x%X\n",
			func->num, func->vendor, func->device,
			func->max_blksize, func->cur_blksize));

	ath6kl_alloc_hifdev(func);
	device = ath6kl_get_hifdev(func);

	device->id = id;
	device->is_disabled = true;

	spin_lock_init(&device->lock);
	spin_lock_init(&device->asynclock);
	init_completion(&device->startup_completion);
	complete(&device->startup_completion);

	DL_LIST_INIT(&device->ScatterReqHead);

	/* Try to allow scatter unless globally overridden */
	if (!nohifscattersupport)
		device->scatter_enabled = true;

	A_MEMZERO(device->busRequest, sizeof(device->busRequest));

	for (count = 0; count < BUS_REQUEST_MAX_NUM; count++) {
		sema_init(&device->busRequest[count].sem_req, 0);
		kfree(hifFreeBusRequest(device, &device->busRequest[count]));
	}

	sema_init(&device->sem_async, 0);

	ret = hifEnableFunc(device, func);

	return ret;
}

static void ath6kl_hifdev_remove(struct sdio_func *func)
{
	int status = 0;
	struct hif_device *device;

	device = ath6kl_get_hifdev(func);
	/* N3DS_ATH6KL_STARTUP_TEARDOWN: kthread_create() is asynchronous.
	 * sdio_unregister_driver() may otherwise return while AR6K startup still
	 * executes module text, making a subsequent insmod a use-after-unload.
	 * Join the owned task before invoking removal callbacks or freeing HIF. */
	wait_for_completion(&device->startup_completion);
	if (device->claimedContext != NULL)
		status = osdrvCallbacks.
			deviceRemovedHandler(device->claimedContext, device);

	if (device->is_disabled)
		device->is_disabled = false;
	else
		status = hifDisableFunc(device, func);

	CleanupHIFScatterResources(device);

	delHifDevice(device);
}

#if defined(CONFIG_PM)
static int ath6kl_hifdev_suspend(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	int status = 0;
	struct hif_device *device;

	device = ath6kl_get_hifdev(func);

	if (device && device->claimedContext &&
	    osdrvCallbacks.deviceSuspendHandler) {
		/* set true first for PowerStateChangeNotify(..) */
		device->is_suspend = true;
		status = osdrvCallbacks.
			deviceSuspendHandler(device->claimedContext);
		if (status)
			device->is_suspend = false;
	}

	CleanupHIFScatterResources(device);

	switch (status) {
	case 0:
		return 0;
	case A_EBUSY:
		/* Hack for kernel in order to support deep sleep and wow */
		return -EBUSY;
	default:
		return -1;
	}
}

static int ath6kl_hifdev_resume(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	int status = 0;
	struct hif_device *device;

	device = ath6kl_get_hifdev(func);
	if (device && device->claimedContext &&
	    osdrvCallbacks.deviceSuspendHandler) {
		status = osdrvCallbacks.
			deviceResumeHandler(device->claimedContext);
		if (status == 0)
			device->is_suspend = false;
	}

	return status;
}

static const struct dev_pm_ops ath6kl_hifdev_pmops = {
	.suspend = ath6kl_hifdev_suspend,
	.resume = ath6kl_hifdev_resume,
};
#endif /* CONFIG_PM */

static struct sdio_driver ath6kl_hifdev_driver = {
	.name = "ath6kl_hifdev",
	.id_table = ath6kl_hifdev_ids,
	.probe = ath6kl_hifdev_probe,
	.remove = ath6kl_hifdev_remove,
#if defined(CONFIG_PM)
	.drv = {
		.pm = &ath6kl_hifdev_pmops,
	},
#endif
};

/* make sure we only unregister when registered. */
static int registered = 0;

extern u32 onebitmode;
extern u32 busspeedlow;
extern u32 debughif;

static void ResetAllCards(void);

#ifdef DEBUG

ATH_DEBUG_INSTANTIATE_MODULE_VAR(hif,
                                 "hif",
                                 "(Linux MMC) Host Interconnect Framework",
                                 ATH_DEBUG_MASK_DEFAULTS,
                                 0,
                                 NULL);
                                 
#endif


/* ------ Functions ------ */
int HIFInit(OSDRV_CALLBACKS *callbacks)
{
	int r;
	AR_DEBUG_ASSERT(callbacks != NULL);

	A_REGISTER_MODULE_DEBUG_INFO(hif);

	/* store the callback handlers */
	osdrvCallbacks = *callbacks;

	/* Register with bus driver core */
	registered = 1;

	r = sdio_register_driver(&ath6kl_hifdev_driver);
	if (r < 0)
		return r;

	return 0;
}

static int
__HIFReadWrite(struct hif_device *device,
             u32 address,
             u8 *buffer,
             u32 length,
             u32 request,
             void *context)
{
    u8 opcode;
    int    status = 0;
    int     ret;
    u8 *tbuffer;
    bool   bounced = false;

    AR_DEBUG_ASSERT(device != NULL);
    AR_DEBUG_ASSERT(device->func != NULL);

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Device: 0x%p, buffer:0x%p (addr:0x%X)\n", 
                    device, buffer, address));

    do {
        if (request & HIF_EXTENDED_IO) {
            //AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Command type: CMD53\n"));
        } else {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
                            ("AR6000: Invalid command type: 0x%08x\n", request));
            status = A_EINVAL;
            break;
        }

        if (request & HIF_BLOCK_BASIS) {
            /* round to whole block length size */
            length = (length / HIF_MBOX_BLOCK_SIZE) * HIF_MBOX_BLOCK_SIZE;
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE,
                            ("AR6000: Block mode (BlockLen: %d)\n",
                            length));
        } else if (request & HIF_BYTE_BASIS) {
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE,
                            ("AR6000: Byte mode (BlockLen: %d)\n",
                            length));
        } else {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
                            ("AR6000: Invalid data mode: 0x%08x\n", request));
            status = A_EINVAL;
            break;
        }

#if 0
        /* useful for checking register accesses */
        if (length & 0x3) {
            A_PRINTF(KERN_ALERT"AR6000: HIF (%s) is not a multiple of 4 bytes, addr:0x%X, len:%d\n",
                                request & HIF_WRITE ? "write":"read", address, length);
        }
#endif

        if (request & HIF_WRITE) {
            if ((address >= HIF_MBOX_START_ADDR(0)) &&
                (address <= HIF_MBOX_END_ADDR(3)))
            {
    
                AR_DEBUG_ASSERT(length <= HIF_MBOX_WIDTH);
    
                /*
                 * Mailbox write. Adjust the address so that the last byte
                 * falls on the EOM address.
                 */
                address += (HIF_MBOX_WIDTH - length);
            }
        }

        if (request & HIF_FIXED_ADDRESS) {
            opcode = CMD53_FIXED_ADDRESS;
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Address mode: Fixed 0x%X\n", address));
        } else if (request & HIF_INCREMENTAL_ADDRESS) {
            opcode = CMD53_INCR_ADDRESS;
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Address mode: Incremental 0x%X\n", address));
        } else {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
                            ("AR6000: Invalid address mode: 0x%08x\n", request));
            status = A_EINVAL;
            break;
        }

        if (request & HIF_WRITE) {
#if HIF_USE_DMA_BOUNCE_BUFFER
            if (BUFFER_NEEDS_BOUNCE(buffer)) {
                AR_DEBUG_ASSERT(device->dma_buffer != NULL);
                tbuffer = device->dma_buffer;
                    /* copy the write data to the dma buffer */
                AR_DEBUG_ASSERT(length <= HIF_DMA_BUFFER_SIZE);
                memcpy(tbuffer, buffer, length);
                bounced = true;
            } else {
                tbuffer = buffer;    
            }
#else
	        tbuffer = buffer;
#endif
            if (opcode == CMD53_FIXED_ADDRESS) {
                ret = sdio_writesb(device->func, address, tbuffer, length);
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: writesb ret=%d address: 0x%X, len: %d, 0x%X\n",
						  ret, address, length, *(int *)tbuffer));
            } else {
                ret = sdio_memcpy_toio(device->func, address, tbuffer, length);
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: writeio ret=%d address: 0x%X, len: %d, 0x%X\n",
						  ret, address, length, *(int *)tbuffer));
            }
        } else if (request & HIF_READ) {
#if HIF_USE_DMA_BOUNCE_BUFFER
            if (BUFFER_NEEDS_BOUNCE(buffer)) {
                AR_DEBUG_ASSERT(device->dma_buffer != NULL);
                AR_DEBUG_ASSERT(length <= HIF_DMA_BUFFER_SIZE);
                tbuffer = device->dma_buffer;
                bounced = true;
            } else {
                tbuffer = buffer;    
            }
#else
            tbuffer = buffer;
#endif
            if (opcode == CMD53_FIXED_ADDRESS) {
                ret = sdio_readsb(device->func, tbuffer, address, length);
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: readsb ret=%d address: 0x%X, len: %d, 0x%X\n",
						  ret, address, length, *(int *)tbuffer));
            } else {
                ret = sdio_memcpy_fromio(device->func, tbuffer, address, length);
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: readio ret=%d address: 0x%X, len: %d, 0x%X\n",
						  ret, address, length, *(int *)tbuffer));
            }
#if HIF_USE_DMA_BOUNCE_BUFFER
            if (bounced) {
    	           /* copy the read data from the dma buffer */
                memcpy(buffer, tbuffer, length);
            }
#endif
        } else {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
                            ("AR6000: Invalid direction: 0x%08x\n", request));
            status = A_EINVAL;
            break;
        }

        if (ret) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
                            ("AR6000: SDIO bus operation failed! MMC stack returned : %d \n", ret));
            status = A_ERROR;
        }
    } while (false);

    return status;
}

void AddToAsyncList(struct hif_device *device, BUS_REQUEST *busrequest)
{
    unsigned long flags;
    BUS_REQUEST *async;
    BUS_REQUEST *active;
    
    spin_lock_irqsave(&device->asynclock, flags);
    active = device->asyncreq;
    if (active == NULL) {
        device->asyncreq = busrequest;
        device->asyncreq->inusenext = NULL;
    } else {
        for (async = device->asyncreq;
             async != NULL;
             async = async->inusenext) {
             active =  async;
        }
        active->inusenext = busrequest;
        busrequest->inusenext = NULL;
    }
    spin_unlock_irqrestore(&device->asynclock, flags);
}

static void hifCopySyncReadback(BUS_REQUEST *busrequest)
{
    if (busrequest->sync_is_read && busrequest->sync_caller_buffer &&
        busrequest->sync_buffer && busrequest->sync_copy_length) {
        memcpy(busrequest->sync_caller_buffer, busrequest->sync_buffer,
               busrequest->sync_copy_length);
    }
}


/* N3DS_HIF_SYNC_CALLER_FASTPATH: the AR6014 scan-result fallback
 * reads target RAM through tens of thousands of four-byte diagnostic-window
 * transactions. Sending each synchronous transfer through async_task costs
 * two context switches. When its queue is empty, sdio_claim_host() supplies
 * the same serialization and the caller can execute the transfer directly. */
static bool hif_sync_fastpath = true;
module_param(hif_sync_fastpath, bool, 0644);
MODULE_PARM_DESC(hif_sync_fastpath,
                 "run synchronous HIF requests directly while the async queue is idle");

/* queue a read/write request */
static unsigned int n3ds_hif_sync_trace_budget = 16;

int
HIFReadWrite(struct hif_device *device,
             u32 address,
             u8 *buffer,
             u32 length,
             u32 request,
             void *context)
{
    int    status = 0;
    int    wait_status;
    unsigned long flags;
    bool   completed;
    bool   trace_host_status;
    BUS_REQUEST *busrequest;


    AR_DEBUG_ASSERT(device != NULL);
    AR_DEBUG_ASSERT(device->func != NULL);

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Device: %p addr:0x%X\n", device,address));

    do {            
        if ((request & HIF_ASYNCHRONOUS) || (request & HIF_SYNCHRONOUS)){
            /* serialize all requests through the async thread */
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Execution mode: %s\n", 
                        (request & HIF_ASYNCHRONOUS)?"Async":"Synch"));

            if ((request & HIF_SYNCHRONOUS) && hif_sync_fastpath &&
                !in_interrupt() && !irqs_disabled()) {
                bool idle;

                spin_lock_irqsave(&device->asynclock, flags);
                idle = device->asyncreq == NULL;
                spin_unlock_irqrestore(&device->asynclock, flags);

                if (idle) {
                    sdio_claim_host(device->func);
                    status = __HIFReadWrite(device, address, buffer, length,
                                            request & ~HIF_SYNCHRONOUS,
                                            NULL);
                    sdio_release_host(device->func);
                    return status;
                }
            }

            busrequest = hifAllocateBusRequest(device);
            if (busrequest == NULL) {
                AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, 
                    ("AR6000: no async bus requests available (%s, addr:0x%X, len:%d) \n", 
                        request & HIF_READ ? "READ":"WRITE", address, length));
                return A_ERROR;
            }
            busrequest->address = address;
            busrequest->buffer = buffer;
            busrequest->length = length;
            busrequest->request = request;
            busrequest->context = context;
            busrequest->sync_completed = false;
            busrequest->sync_timed_out = false;
            busrequest->sync_buffer = NULL;
            busrequest->sync_caller_buffer = NULL;
            busrequest->sync_buffer_length = 0;
            busrequest->sync_copy_length = 0;
            busrequest->sync_is_read = false;

            if (request & HIF_SYNCHRONOUS) {
                /* The worker may outlive this caller after the bounded wait.
                 * Never let it retain a caller-owned or stack buffer. Keep
                 * this bounded to the largest transfer supported by the
                 * existing HIF DMA bounce path. */
                if (length > HIF_DMA_BUFFER_SIZE || (length && !buffer)) {
                    AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("AR6002 HIF sync buffer rejected len=%u max=%u\n",
                         length, HIF_DMA_BUFFER_SIZE));
                    kfree(hifFreeBusRequest(device, busrequest));
                    return A_ERROR;
                }

                busrequest->sync_caller_buffer = buffer;
                busrequest->sync_buffer_length = length;
                busrequest->sync_copy_length = length;
                if (request & HIF_BLOCK_BASIS) {
                    busrequest->sync_copy_length =
                        (length / HIF_MBOX_BLOCK_SIZE) * HIF_MBOX_BLOCK_SIZE;
                }
                busrequest->sync_is_read = !!(request & HIF_READ);

                if (length) {
                    busrequest->sync_buffer = kmalloc(length, GFP_ATOMIC);
                    if (!busrequest->sync_buffer) {
                        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                            ("AR6002 HIF sync buffer allocation failed len=%u\n",
                             length));
                        kfree(hifFreeBusRequest(device, busrequest));
                        return A_ERROR;
                    }
                    if (request & HIF_WRITE) {
                        memcpy(busrequest->sync_buffer, buffer, length);
                    }
                }
                busrequest->buffer = busrequest->sync_buffer;
            }
            
            AddToAsyncList(device, busrequest);
            
            if (request & HIF_SYNCHRONOUS) {
				/* N3DS_BOUNDED_HIF_SYNC_TRACE: the 10 ms SDIO IRQ
				 * fallback intentionally reads this table forever.  Keep
				 * enough startup samples for diagnosis without flooding the
				 * top screen and persistent trace file. */
                trace_host_status =
					(address == HOST_INT_STATUS_ADDRESS) &&
					n3ds_hif_sync_trace_budget;
                if (trace_host_status) {
					n3ds_hif_sync_trace_budget--;
                    AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("AR6002 HIF sync queued addr=0x%X len=%u req=0x%X timeout=%dms\n",
                         address, length, request, HIF_SYNC_REQUEST_TIMEOUT_MS));
                }

                /* wait for completion */
                up(&device->sem_async);
                wait_status = down_timeout(&busrequest->sem_req,
                                           msecs_to_jiffies(HIF_SYNC_REQUEST_TIMEOUT_MS));
                if (wait_status != 0) {
                    /* If the worker completed just as the timeout expired,
                     * consume its already-posted semaphore and return the
                     * real bus status. Otherwise transfer ownership to the
                     * worker, which will reclaim this request on completion. */
                    spin_lock_irqsave(&device->lock, flags);
                    completed = busrequest->sync_completed;
                    if (completed) {
                        status = busrequest->status;
                        down_trylock(&busrequest->sem_req);
                    } else {
                        busrequest->sync_timed_out = true;
                    }
                    spin_unlock_irqrestore(&device->lock, flags);

                    if (!completed) {
                        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                            ("AR6002 HIF sync timeout addr=0x%X len=%u wait=%d\n",
                             address, length, wait_status));
                        return A_ERROR;
                    }

                    AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("AR6002 HIF sync completed at timeout boundary addr=0x%X status=%d\n",
                         address, status));
                    hifCopySyncReadback(busrequest);
                    kfree(hifFreeBusRequest(device, busrequest));
                    return status;
                }

                status = busrequest->status;
                hifCopySyncReadback(busrequest);
                if (trace_host_status) {
                    AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("AR6002 HIF sync complete addr=0x%X status=%d\n",
                         address, status));
                }
                kfree(hifFreeBusRequest(device, busrequest));
                return status;
            } else {
                AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: queued async req: 0x%lX\n", (unsigned long)busrequest));
                up(&device->sem_async);
                return A_PENDING;
            }
        } else {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR,
                            ("AR6000: Invalid execution mode: 0x%08x\n", (unsigned int)request));
            status = A_EINVAL;
            break;
        }
    } while(0);

    return status;
}
/* thread to serialize all requests, both sync and async */
static int async_task(void *param)
{
    struct hif_device *device;
    BUS_REQUEST *request;
    int status;
    unsigned long flags;

    device = (struct hif_device *)param;
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async task\n"));
    set_current_state(TASK_INTERRUPTIBLE);
    while(!device->async_shutdown) {
        /* wait for work */
        if (down_interruptible(&device->sem_async) != 0) {
            /* interrupted, exit */
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async task interrupted\n"));
            break;
        }
        if (device->async_shutdown) {
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async task stopping\n"));
            break;
        }
        /* we want to hold the host over multiple cmds if possible, but holding the host blocks card interrupts */
        sdio_claim_host(device->func);
        spin_lock_irqsave(&device->asynclock, flags);
        /* pull the request to work on */
        while (device->asyncreq != NULL) {
            request = device->asyncreq;
            if (request->inusenext != NULL) {
                device->asyncreq = request->inusenext;
            } else {
                device->asyncreq = NULL;
            }
            spin_unlock_irqrestore(&device->asynclock, flags);
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async_task processing req: 0x%lX\n", (unsigned long)request));
            
            if (request->pScatterReq != NULL) {
                A_ASSERT(device->scatter_enabled);
                    /* this is a queued scatter request, pass the request to scatter routine which
                     * executes it synchronously, note, no need to free the request since scatter requests
                     * are maintained on a separate list */
                status = DoHifReadWriteScatter(device,request);
            } else {                
                    /* call HIFReadWrite in sync mode to do the work */
                status = __HIFReadWrite(device, request->address, request->buffer,
                                      request->length, request->request & ~HIF_SYNCHRONOUS, NULL);
                if (request->request & HIF_ASYNCHRONOUS) {
                    void *context = request->context;
                    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async_task freeing req: 0x%lX\n", (unsigned long)request));
                    kfree(hifFreeBusRequest(device, request));
                    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async_task completion routine req: 0x%lX\n", (unsigned long)request));
                    device->htcCallbacks.rwCompletionHandler(context, status);
                } else {
                    bool free_request = false;

                    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: async_task completing sync req: 0x%lX\n", (unsigned long)request));
                    /* Publish completion and the status while holding the
                     * device lock. The caller can then safely race a lower
                     * layer timeout without freeing a request still owned by
                     * this worker. */
                    spin_lock_irqsave(&device->lock, flags);
                    request->status = status;
                    request->sync_completed = true;
                    if (request->sync_timed_out) {
                        free_request = true;
                    } else {
                        up(&request->sem_req);
                    }
                    spin_unlock_irqrestore(&device->lock, flags);

                    if (free_request) {
                        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                            ("AR6002 HIF reclaimed timed-out sync req: 0x%lX status=%d\n",
                             (unsigned long)request, status));
                        kfree(hifFreeBusRequest(device, request));
                    }
                }
            }
            spin_lock_irqsave(&device->asynclock, flags);
        }
        spin_unlock_irqrestore(&device->asynclock, flags);
        sdio_release_host(device->func);
    }

    complete_and_exit(&device->async_completion, 0);
    return 0;
}

static s32 IssueSDCommand(struct hif_device *device, u32 opcode, u32 arg, u32 flags, u32 *resp)
{
    struct mmc_command cmd;
    s32 err;
    struct mmc_host *host;
    struct sdio_func *func;

    func = device->func;
    host = func->card->host;

    memset(&cmd, 0, sizeof(struct mmc_command)); 
    cmd.opcode = opcode;
    cmd.arg = arg;
    cmd.flags = flags;
    err = mmc_wait_for_cmd(host, &cmd, 3);

    if ((!err) && (resp)) {
        *resp = cmd.resp[0];
    }

    return err;
}

int ReinitSDIO(struct hif_device *device)
{
    /* Phase 2 (WiFi bring-up, legacy AR6002 driver port): this manual
     * bus reinit sequence hand-rolls SD/SDIO card bring-up (CMD0/CMD5/
     * CMD3/CMD7, high-speed CCCR negotiation) using mmc_host/mmc_card
     * fields (host->ocr, MMC_STATE_HIGHSPEED, mmc_card_set_highspeed(),
     * mmc_card_highspeed()) that were removed outright from the kernel's
     * MMC core between this driver's original era and now -- not
     * renamed, genuinely gone, since the core manages this state
     * internally today. It's only reached from the HIF_DEVICE_POWER_UP
     * path after a full HIF_DEVICE_POWER_CUT (a suspend/power-cycle
     * recovery case), which isn't part of the initial association
     * bring-up this porting pass is targeting -- normal enumeration
     * already works via the standard mmc core path (confirmed: real
     * hardware reports "mmc0: new SDIO card at address 0001"). Stubbed
     * out rather than hand-porting untested low-level bus sequencing
     * against removed internals; revisit if/when suspend/resume power
     * cycling is actually being exercised.
     */
    return 0;
}

int
PowerStateChangeNotify(struct hif_device *device, HIF_DEVICE_POWER_CHANGE_TYPE config)
{
    int status = 0;
#if defined(CONFIG_PM)
	struct sdio_func *func = device->func;
    int old_reset_val;
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: +PowerStateChangeNotify %d\n", config));
    switch (config) {
       case HIF_DEVICE_POWER_DOWN:
       case HIF_DEVICE_POWER_CUT:
            old_reset_val = reset_sdio_on_unload;
            reset_sdio_on_unload = 1;
            status = hifDisableFunc(device, func);
            reset_sdio_on_unload = old_reset_val;
            if (!device->is_suspend) {
                struct mmc_host *host = func->card->host;
	            host->ios.clock = 0;
	            host->ios.vdd = 0;
                host->ios.bus_mode = MMC_BUSMODE_OPENDRAIN;
                host->ios.chip_select = MMC_CS_DONTCARE;
                host->ios.power_mode = MMC_POWER_OFF;
                host->ios.bus_width = MMC_BUS_WIDTH_1;
                host->ios.timing = MMC_TIMING_LEGACY;
                host->ops->set_ios(host, &host->ios);
            }
            break;
       case HIF_DEVICE_POWER_UP:
            if (device->powerConfig == HIF_DEVICE_POWER_CUT) {
                status = ReinitSDIO(device);
            }
            if (status == 0) {
                status = hifEnableFunc(device, func);
            }
            break;
    } 
    device->powerConfig = config;

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: -PowerStateChangeNotify\n"));
#endif
    return status;
}

int
HIFConfigureDevice(struct hif_device *device, HIF_DEVICE_CONFIG_OPCODE opcode,
                   void *config, u32 configLen)
{
    u32 count;
    int status = 0;
    
    switch(opcode) {
        case HIF_DEVICE_GET_MBOX_BLOCK_SIZE:
            ((u32 *)config)[0] = HIF_MBOX0_BLOCK_SIZE;
            ((u32 *)config)[1] = HIF_MBOX1_BLOCK_SIZE;
            ((u32 *)config)[2] = HIF_MBOX2_BLOCK_SIZE;
            ((u32 *)config)[3] = HIF_MBOX3_BLOCK_SIZE;
            break;

        case HIF_DEVICE_GET_MBOX_ADDR:
            for (count = 0; count < 4; count ++) {
                ((u32 *)config)[count] = HIF_MBOX_START_ADDR(count);
            }
            
            if (configLen >= sizeof(struct hif_device_mbox_info)) {    
                SetExtendedMboxWindowInfo((u16)device->func->device,
                                          (struct hif_device_mbox_info *)config);
            }
                        
            break;
        case HIF_DEVICE_GET_IRQ_PROC_MODE:
            *((HIF_DEVICE_IRQ_PROCESSING_MODE *)config) = HIF_DEVICE_IRQ_SYNC_ONLY;
            break;
       case HIF_CONFIGURE_QUERY_SCATTER_REQUEST_SUPPORT:
            if (!device->scatter_enabled) {
                return A_ENOTSUP;    
            }
            status = SetupHIFScatterSupport(device, (struct hif_device_scatter_support_info *)config);
            if (status) {
                device->scatter_enabled = false;
            }
            break; 
        case HIF_DEVICE_GET_OS_DEVICE:
                /* pass back a pointer to the SDIO function's "dev" struct */
            ((struct hif_device_os_device_info *)config)->pOSDevice = &device->func->dev;
            break; 
        case HIF_DEVICE_POWER_STATE_CHANGE:
            status = PowerStateChangeNotify(device, *(HIF_DEVICE_POWER_CHANGE_TYPE *)config);
            break;
        default:
            AR_DEBUG_PRINTF(ATH_DEBUG_WARN,
                            ("AR6000: Unsupported configuration opcode: %d\n", opcode));
            status = A_ERROR;
    }

    return status;
}

void
HIFShutDownDevice(struct hif_device *device)
{
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: +HIFShutDownDevice\n"));
    if (device != NULL) {
        AR_DEBUG_ASSERT(device->func != NULL);
    } else {
            /* since we are unloading the driver anyways, reset all cards in case the SDIO card
             * is externally powered and we are unloading the SDIO stack.  This avoids the problem when
             * the SDIO stack is reloaded and attempts are made to re-enumerate a card that is already
             * enumerated */
        AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: HIFShutDownDevice, resetting\n"));
        ResetAllCards();

        /* Unregister with bus driver core */
        if (registered) {
            registered = 0;
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE,
                            ("AR6000: Unregistering with the bus driver\n"));
            sdio_unregister_driver(&ath6kl_hifdev_driver);
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE,
                            ("AR6000: Unregistered\n"));
        }
    }
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: -HIFShutDownDevice\n"));
}

static void
hifIRQHandler(struct sdio_func *func)
{
    int status;
    struct hif_device *device;
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: +hifIRQHandler\n"));

    device = ath6kl_get_hifdev(func);
    atomic_set(&device->irqHandling, 1);
    /* release the host during ints so we can pick it back up when we process cmds */
    sdio_release_host(device->func);
    status = device->htcCallbacks.dsrHandler(device->htcCallbacks.context);
    sdio_claim_host(device->func);
    atomic_set(&device->irqHandling, 0);
    /* N3DS_HIF_DSR_NONFATAL_RECOVERY: the physical AR6014 can return a
     * transient SDIO error while diagnostic traffic is in flight.  This old
     * vendor debug assertion halted the entire kernel.  Leave the interrupt
     * boundary clean and let the installed 10 ms card-IRQ poller retry. */
    if (status != 0 && status != A_ECANCELED)
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 HIF DSR transient status=%d; poller will retry\n",
             status));
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: -hifIRQHandler\n"));
}

/* handle HTC startup via thread*/
static int startup_task(void *param)
{
    struct hif_device *device;

    device = (struct hif_device *)param;
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: call HTC from startup_task\n"));
        /* start  up inform DRV layer */
    if ((osdrvCallbacks.deviceInsertedHandler(osdrvCallbacks.context,device)) != 0) {
        AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Device rejected\n"));
    }
    complete(&device->startup_completion);
    return 0;
}

#if defined(CONFIG_PM)
static int enable_task(void *param)
{
    struct hif_device *device;
    device = (struct hif_device *)param;
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: call  from resume_task\n"));

        /* start  up inform DRV layer */
    if (device &&
        device->claimedContext &&
        osdrvCallbacks.devicePowerChangeHandler &&
        osdrvCallbacks.devicePowerChangeHandler(device->claimedContext, HIF_DEVICE_POWER_UP) != 0)
    {
        AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: Device rejected\n"));
    }

    /* N3DS_ATH6KL_STARTUP_TEARDOWN: hifEnableFunc() calls reinit_completion()
     * on startup_completion for both the first-load (startup_task) and PM
     * resume (enable_task) paths, but only startup_task used to signal it.
     * A CONFIG_PM_AUTOSLEEP-triggered resume while Wi-Fi was already loaded
     * left startup_completion permanently unsignaled, so a later rmmod's
     * wait_for_completion() in the remove path blocked forever -- observed
     * as Mobile Data hanging at "starting" indefinitely with no error. */
    complete(&device->startup_completion);
    return 0;
}
#endif

void
HIFAckInterrupt(struct hif_device *device)
{
    AR_DEBUG_ASSERT(device != NULL);

    /* Acknowledge our function IRQ */
}

void
HIFUnMaskInterrupt(struct hif_device *device)
{
    int ret;

    AR_DEBUG_ASSERT(device != NULL);
    AR_DEBUG_ASSERT(device->func != NULL);

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: HIFUnMaskInterrupt\n"));

    /* Register the IRQ Handler */
    sdio_claim_host(device->func);
    ret = sdio_claim_irq(device->func, hifIRQHandler);
    sdio_release_host(device->func);
    AR_DEBUG_ASSERT(ret == 0);
}

void HIFMaskInterrupt(struct hif_device *device)
{
    int ret;
    AR_DEBUG_ASSERT(device != NULL);
    AR_DEBUG_ASSERT(device->func != NULL);

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: HIFMaskInterrupt\n"));

    /* Mask our function IRQ */
    sdio_claim_host(device->func);
    while (atomic_read(&device->irqHandling)) {        
        sdio_release_host(device->func);
        schedule_timeout(HZ/10);
        sdio_claim_host(device->func);
    }
    ret = sdio_release_irq(device->func);
    sdio_release_host(device->func);
    AR_DEBUG_ASSERT(ret == 0);
}

BUS_REQUEST *hifAllocateBusRequest(struct hif_device *device)
{
    BUS_REQUEST *busrequest;
    unsigned long flag;

    /* Acquire lock */
    spin_lock_irqsave(&device->lock, flag);

    /* Remove first in list */
    if((busrequest = device->s_busRequestFreeQueue) != NULL)
    {
        device->s_busRequestFreeQueue = busrequest->next;
    }
    /* Release lock */
    spin_unlock_irqrestore(&device->lock, flag);
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: hifAllocateBusRequest: 0x%p\n", busrequest));
    return busrequest;
}

u8 *
hifFreeBusRequest(struct hif_device *device, BUS_REQUEST *busrequest)
{
    unsigned long flag;
    u8 *sync_buffer;

    AR_DEBUG_ASSERT(busrequest != NULL);
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: hifFreeBusRequest: 0x%p\n", busrequest));
    /* Acquire lock */
    spin_lock_irqsave(&device->lock, flag);

    /* The request is only released after the worker has finished using its
     * payload (or before it is queued for a new owner). Detach the private
     * storage under the same lock, then free it outside the lock. */
    sync_buffer = busrequest->sync_buffer;
    busrequest->sync_buffer = NULL;
    busrequest->sync_caller_buffer = NULL;
    busrequest->sync_buffer_length = 0;
    busrequest->sync_copy_length = 0;
    busrequest->sync_is_read = false;

    /* Insert first in list */
    busrequest->next = device->s_busRequestFreeQueue;
    busrequest->inusenext = NULL;
    device->s_busRequestFreeQueue = busrequest;

    /* Release lock. The caller must free the detached payload only after
     * this function returns, so no allocator call occurs under the spinlock. */
    spin_unlock_irqrestore(&device->lock, flag);
    return sync_buffer;
}

static int hifDisableFunc(struct hif_device *device, struct sdio_func *func)
{
    int ret;
    int status = 0;

    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: +hifDisableFunc\n"));
    device = ath6kl_get_hifdev(func);
    if (!IS_ERR(device->async_task)) {
        init_completion(&device->async_completion);
        device->async_shutdown = 1;
        up(&device->sem_async);
        wait_for_completion(&device->async_completion);
        device->async_task = NULL;
    }
    /* Disable the card */
    sdio_claim_host(device->func);
    ret = sdio_disable_func(device->func);
    if (ret) {
        status = A_ERROR;
    } 

    if (reset_sdio_on_unload) {
        /* reset the SDIO interface.  This is useful in automated testing where the card
         * does not need to be removed at the end of the test.  It is expected that the user will 
         * also unload/reload the host controller driver to force the bus driver to re-enumerate the slot */
        AR_DEBUG_PRINTF(ATH_DEBUG_WARN, ("AR6000: reseting SDIO card back to uninitialized state \n"));
        
        /* NOTE : sdio_f0_writeb() cannot be used here, that API only allows access
         *        to undefined registers in the range of: 0xF0-0xFF */
         
        ret = Func0_CMD52WriteByte(device->func->card, SDIO_CCCR_ABORT, (1 << 3)); 
        if (ret) {
            status = A_ERROR;
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("AR6000: reset failed : %d \n",ret));    
        }
    }

    sdio_release_host(device->func);

    if (status == 0) {
        device->is_disabled = true;
    }
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: -hifDisableFunc\n"));

    return status;
}

static int hifEnableFunc(struct hif_device *device, struct sdio_func *func)
{
    struct task_struct* pTask;
    const char *taskName = NULL;
    int (*taskFunc)(void *) = NULL;
    int ret = 0;
    
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: +hifEnableFunc\n"));
    device = ath6kl_get_hifdev(func);

    if (device->is_disabled) {
       /* enable the SDIO function */
        sdio_claim_host(func);

        if ((device->id->device & MANUFACTURER_ID_AR6K_BASE_MASK) >= MANUFACTURER_ID_AR6003_BASE) {
            /* enable 4-bit ASYNC interrupt on AR6003 or later devices */
            ret = Func0_CMD52WriteByte(func->card, CCCR_SDIO_IRQ_MODE_REG, SDIO_IRQ_MODE_ASYNC_4BIT_IRQ);
            if (ret) {
                AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("AR6000: failed to enable 4-bit ASYNC IRQ mode %d \n",ret));
                sdio_release_host(func);
                return ret;
            }
            AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: 4-bit ASYNC IRQ mode enabled\n"));
        }
        /* give us some time to enable, in ms */
        func->enable_timeout = 100;
        ret = sdio_enable_func(func);
        if (ret) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), Unable to enable AR6K: 0x%X\n",
					  __FUNCTION__, ret));
            sdio_release_host(func);
            return ret;
        }
        ret = sdio_set_block_size(func, HIF_MBOX_BLOCK_SIZE);
        sdio_release_host(func);
        if (ret) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), Unable to set block size 0x%x  AR6K: 0x%X\n",
					  __FUNCTION__, HIF_MBOX_BLOCK_SIZE, ret));
            return ret;
        }
        device->is_disabled = false;
        /* create async I/O thread */
        if (!device->async_task) {
            device->async_shutdown = 0;
            device->async_task = kthread_create(async_task,
                                           (void *)device,
                                           "AR6K Async");
           if (IS_ERR(device->async_task)) {
               AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), to create async task\n", __FUNCTION__));
                return -ENOMEM;
           }
           AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: start async task\n"));
           wake_up_process(device->async_task );    
        }
    }

    if (!device->claimedContext) {
        taskFunc = startup_task;
        taskName = "AR6K startup";
        ret = 0;
#if defined(CONFIG_PM)
    } else {
        taskFunc = enable_task;
        taskName = "AR6K enable";
        ret = -ENOMEM;
#endif /* CONFIG_PM */
    }
    /* create resume thread */
    pTask = kthread_create(taskFunc, (void *)device, taskName);
    if (IS_ERR(pTask)) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, ("AR6000: %s(), to create enabel task\n", __FUNCTION__));
        return -ENOMEM;
    }
    reinit_completion(&device->startup_completion);
    wake_up_process(pTask);
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: -hifEnableFunc\n"));

    /* task will call the enable func, indicate pending */
    return ret;
}

/*
 * This should be moved to AR6K HTC layer.
 */
int hifWaitForPendingRecv(struct hif_device *device)
{
    s32 cnt = 10;
    u8 host_int_status;
    int status = 0;

    do {            		    
        while (atomic_read(&device->irqHandling)) {
	        /* wait until irq handler finished all the jobs */
			schedule_timeout(HZ/10);
	    }
		/* check if there is any pending irq due to force done */
		host_int_status = 0;
	    status = HIFReadWrite(device, HOST_INT_STATUS_ADDRESS,
				    (u8 *)&host_int_status, sizeof(host_int_status),
			  	     HIF_RD_SYNC_BYTE_INC, NULL);
	    host_int_status = !status ? (host_int_status & (1 << 0)) : 0;
		if (host_int_status) {
	        schedule(); /* schedule for next dsrHandler */
		}
	} while (host_int_status && --cnt > 0);

    if (host_int_status && cnt == 0) {
         AR_DEBUG_PRINTF(ATH_DEBUG_ERROR, 
                            ("AR6000: %s(), Unable clear up pending IRQ before the system suspended\n", __FUNCTION__));
     }

    return 0;
}
    
static void
delHifDevice(struct hif_device * device)
{
    AR_DEBUG_ASSERT(device!= NULL);
    AR_DEBUG_PRINTF(ATH_DEBUG_TRACE, ("AR6000: delHifDevice; 0x%p\n", device));
    kfree(device->dma_buffer);
    kfree(device);
}

static void ResetAllCards(void)
{
}

void HIFClaimDevice(struct hif_device  *device, void *context)
{
    device->claimedContext = context;
}

void HIFReleaseDevice(struct hif_device  *device)
{
    device->claimedContext = NULL;
}

int HIFAttachHTC(struct hif_device *device, HTC_CALLBACKS *callbacks)
{
    if (device->htcCallbacks.context != NULL) {
            /* already in use! */
        return A_ERROR;
    }
    device->htcCallbacks = *callbacks;
    return 0;
}

void HIFDetachHTC(struct hif_device *device)
{
    A_MEMZERO(&device->htcCallbacks,sizeof(device->htcCallbacks));
}

#define SDIO_SET_CMD52_ARG(arg,rw,func,raw,address,writedata) \
    (arg) = (((rw) & 1) << 31)           | \
            (((func) & 0x7) << 28)       | \
            (((raw) & 1) << 27)          | \
            (1 << 26)                    | \
            (((address) & 0x1FFFF) << 9) | \
            (1 << 8)                     | \
            ((writedata) & 0xFF)
            
#define SDIO_SET_CMD52_READ_ARG(arg,func,address) \
    SDIO_SET_CMD52_ARG(arg,0,(func),0,address,0x00)
#define SDIO_SET_CMD52_WRITE_ARG(arg,func,address,value) \
    SDIO_SET_CMD52_ARG(arg,1,(func),0,address,value)
    
static int Func0_CMD52WriteByte(struct mmc_card *card, unsigned int address, unsigned char byte)
{
    struct mmc_command ioCmd;
    unsigned long      arg;
    
    memset(&ioCmd,0,sizeof(ioCmd));
    SDIO_SET_CMD52_WRITE_ARG(arg,0,address,byte);
    ioCmd.opcode = SD_IO_RW_DIRECT;
    ioCmd.arg = arg;
    ioCmd.flags = MMC_RSP_R5 | MMC_CMD_AC;
    
    return mmc_wait_for_cmd(card->host, &ioCmd, 0);
}

static int Func0_CMD52ReadByte(struct mmc_card *card, unsigned int address, unsigned char *byte)
{
    struct mmc_command ioCmd;
    unsigned long      arg;
    s32 err;
    
    memset(&ioCmd,0,sizeof(ioCmd));
    SDIO_SET_CMD52_READ_ARG(arg,0,address);
    ioCmd.opcode = SD_IO_RW_DIRECT;
    ioCmd.arg = arg;
    ioCmd.flags = MMC_RSP_R5 | MMC_CMD_AC;

    err = mmc_wait_for_cmd(card->host, &ioCmd, 0);

    if ((!err) && (byte)) {
        *byte =  ioCmd.resp[0] & 0xFF;
    }

    return err;
}
