/*
 * virtio block device backed by the 3DS SD card - WITH WRITE SUPPORT
 *
 * This is a patched version of arm9linuxfw's source/vdev/sdcard.c.
 *
 * Upstream only implements the read path: it advertises VIRTIO_BLK_F_RO and
 * treats every HOST_TO_VDEV descriptor as the request header, so a
 * VIRTIO_BLK_T_OUT (write) request has its data descriptor misinterpreted as a
 * header and is silently dropped.  The SDMMC layer below it
 * (sdmmc_sdcard_writesectors(), already used by firm_linux_loader's FAT
 * stack) has full write support.
 *
 * Android needs a writable device for /data (and this port wants SD-card
 * boot logs), so this patch:
 *
 *   1. parses the virtio-blk request properly: first descriptor = header
 *      (type + sector), following HOST_TO_VDEV descriptors = write data,
 *      VDEV_TO_HOST descriptors >= 512 bytes = read data, the trailing short
 *      VDEV_TO_HOST descriptor = the status byte;
 *   2. calls sdmmc_sdcard_writesectors() for VIRTIO_BLK_T_OUT;
 *   3. reports VIRTIO_BLK_S_IOERR in the status byte if a write fails, so
 *      Linux surfaces an I/O error instead of silently losing data;
 *   4. drops the VIRTIO_BLK_F_RO feature bit, which is what makes the kernel
 *      consider the device writable in the first place.
 *
 * The sector cursor is now per request (a local variable) instead of being
 * stashed in the virtqueue's private word, so concurrent/queued requests can
 * no longer corrupt each other's offsets.
 *
 * Cache safety: the ARM9 side of this firmware performs no cache maintenance
 * for the data buffer (and enables no caches), and the ARM11 (Linux) side does
 * the DMA maintenance, which is exactly why the existing read path works; the
 * write path uses the same data-transfer machinery.
 *
 * Applies to: linux-3ds/arm9linuxfw (2021-07-06, commit 2069784)
 * Applied by: port/scripts/apply-arm9linuxfw-port.sh
 */

#include "common.h"

#include "hw/sdmmc.h"
#include "virt/manager.h"

/* virtio-blk request types */
#define VIRTIO_BLK_T_IN		0	/* read  */
#define VIRTIO_BLK_T_OUT	1	/* write */
#define VIRTIO_BLK_T_FLUSH	4	/* no-op here: the card flushes itself */
#define VIRTIO_BLK_S_OK		0
#define VIRTIO_BLK_S_IOERR	1

/*
 * Upper bound on how many virtio requests this device services per call.
 *
 * vman_process_pending() calls sdmc_process_vqueue() from inside a
 * CRITICAL_BLOCK, i.e. with ARM9 interrupts disabled, and only re-enables
 * them between calls.  A long run of SD transfers therefore blocks the PXI
 * RX interrupt: an ARM11 register transaction (a virtio notify, or the
 * IRQ-bank read in vpxi_irq_worker) waits for the FIFO, hits its 1 s timeout
 * and is flushed - which loses the virtio completion and hangs every later
 * block request behind it.  On hardware that is the "silent freeze" (the
 * ARM11 timer square keeps blinking, every SD writer stops at once).
 *
 * Re-adding the queue and returning after one request lets the main loop
 * re-enable interrupts between requests; the queue stays in the pending
 * list, so no work is lost (see fix-arm9-sdcard-jobbound.py).
 */
#define SDMC_JOBS_PER_CALL	1

typedef struct {
	u64 capacity;
	u32 size_max;
	u32 seg_max;
	struct virtio_blk_geometry {
			u16 cylinders;
			u8 heads;
			u8 sectors;
	} geometry;
	u32 blk_size;
	struct virtio_blk_topology {
			// # of logical blocks per physical block (log2)
			u8 physical_block_exp;
			// offset of first aligned logical block
			u8 alignment_offset;
			// suggested minimum I/O size in blocks
			u16 min_io_size;
			// optimal (suggested maximum) I/O size in blocks
			u32 opt_io_size;
	} topology;
	u8 writeback;
	u8 unused0[3];
	u32 max_discard_sectors;
	u32 max_discard_seg;
	u32 discard_sector_alignment;
	u32 max_write_zeroes_sectors;
	u32 max_write_zeroes_seg;
	u8 write_zeroes_may_unmap;
	u8 unused1[3];
} PACKED blk_config;

typedef struct {
	u32 resv;
	u32 type;
	u64 sector_offset;
} PACKED vblk_t;

static blk_config sdmc_blk_config;

static void sdmc_hard_reset(vdev_s *vdev) {
	u8 *data = (u8*)&sdmc_blk_config;
	for (uint i = 0; i < sizeof(sdmc_blk_config); i++)
		data[i] = 0;
	sdmmc_sdcard_init();
	sdmc_blk_config.capacity = sdmmc_sdcard_size();
	sdmc_blk_config.blk_size = 512;
}

static u8 sdmc_cfg_read(vdev_s *vdev, uint offset) {
	if (offset < sizeof(sdmc_blk_config))
		return ((u8*)(&sdmc_blk_config))[offset];
	return 0xFF;
}

static void sdmc_process_vqueue(vdev_s *vdev, vqueue_s *vq) {
	vjob_s vjob;
	unsigned jobs = 0;

	while(vqueue_fetch_job_new(vq, &vjob) >= 0) {
		u32 sector = 0;
		u32 type = VIRTIO_BLK_T_IN;
		u8 status = VIRTIO_BLK_S_OK;
		bool header_seen = false;

		do {
			vdesc_s desc;
			vqueue_get_job_desc(vq, &vjob, &desc);

			if (!header_seen) {
				/* virtio-blk: the header is always first */
				const vblk_t *blk = (const vblk_t*)desc.data;
				type = blk->type;
				sector = (u32)blk->sector_offset;
				header_seen = true;
				continue;
			}

			if (desc.dir == HOST_TO_VDEV) {
				/* write data (VIRTIO_BLK_T_OUT) */
				u32 sectors = desc.length >> 9;
				if (sectors) {
					if (sdmmc_sdcard_writesectors(sector, sectors,
								      (const u8*)desc.data))
						status = VIRTIO_BLK_S_IOERR;
					sector += sectors;
				}
			} else if (desc.length < 512) {
				/* the status byte (VDEV_TO_HOST, 1 byte) */
				*(u8*)desc.data = status;
			} else {
				/* read data (VIRTIO_BLK_T_IN) */
				u32 sectors = desc.length >> 9;
				sdmmc_sdcard_readsectors(sector, sectors, desc.data);
				sector += sectors;
				vjob_add_written(&vjob, desc.length);
			}
		} while(vqueue_fetch_job_next(vq, &vjob) >= 0);

		vqueue_push_job(vq, &vjob);

		/*
		 * Bound the IRQs-off window (see SDMC_JOBS_PER_CALL): re-queue
		 * the vq and let the ARM9 main loop run us again after it has
		 * re-enabled interrupts.  No work is lost - the queue is in the
		 * pending list again and vman_process_pending() will dequeue it.
		 */
		if (++jobs >= SDMC_JOBS_PER_CALL) {
			vman_add_pending(vq);
			break;
		}
	}

	vman_notify_host(vdev, VIRQ_VQUEUE);
}

DECLARE_VIRTDEV(
	vdev_sdcard, NULL,
	/* no VIRTIO_BLK_F_RO: Linux may now write to the SD card */
	VDEV_T_BLOCK, 0, 1,
	sdmc_hard_reset,
	sdmc_cfg_read, vdev_cfg_write_stub,
	sdmc_process_vqueue
);
