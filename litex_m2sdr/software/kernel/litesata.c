// SPDX-License-Identifier: GPL-2.0
/*
 * LiteX LiteSATA block driver + LitePCIe glue
 *
 * PCIe-host staging mode:
 *  - copies 512B sectors over BAR0 to SoC SRAM,
 *  - programs LiteSATA DMA with the *SoC bus* address of that SRAM.
 *
 * MSI model: the PCIe core raises MSIs for LiteSATA completions:
 *   - SATA_SECTOR2MEM_INTERRUPT  (reader completion)
 *   - SATA_MEM2SECTOR_INTERRUPT  (writer completion)
 * The PCI driver’s ISR acks them and calls the hooks below.
 *
 * The LiteSATA part handles the DMA and block layer. One DMA at a time
 * (guarded by a mutex) so a single completion object is sufficient for
 * both directions.
 *
 * Hardening:
 * - Strict 32-bit DMA mask (no fallback to 64-bit).
 * - Per-I/O enforcement: any DMA >= 4GiB bounces through a GFP_DMA32
 *   coherent buffer (or errors if bouncing disabled).
 * - Optional knobs to force polling, wait for MSI with timeout + fallback,
 *   insert an IRQ arming delay, and early-poll window to catch fast DONE.
 *
 * Copyright (c) 2022 Gabriel Somlo <gsomlo@gmail.com>
 * Copyright (c) 2025 EnjoyDigital
 */

#include <linux/bits.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/highmem.h>
#include <linux/litex.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/completion.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/compiler.h>
#include <linux/moduleparam.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/atomic.h>

#include "litex.h"
#include "litesata.h"

/* -------- Optional test mode: force polling instead of MSI -------- */
#ifndef LITESATA_FORCE_POLLING
#define LITESATA_FORCE_POLLING 0   /* set to 1 to force polling at build time */
#endif

#ifndef LITESATA_DMA_WAIT_TIMEOUT_MS
#define LITESATA_DMA_WAIT_TIMEOUT_MS 10 /* timeout per DMA before polling */
#endif

/* DONE wait timeouts (ms). */
#ifndef LITESATA_DONE_TIMEOUT_MSI_MS
#define LITESATA_DONE_TIMEOUT_MSI_MS 10
#endif
#ifndef LITESATA_DONE_TIMEOUT_POLL_MS
#define LITESATA_DONE_TIMEOUT_POLL_MS 1000
#endif

/* Extra hardening knobs (runtime-configurable via module params) */
static bool litesata_force_polling = LITESATA_FORCE_POLLING;
module_param_named(force_polling, litesata_force_polling, bool, 0644);
MODULE_PARM_DESC(force_polling, "Force polling (ignore MSI completions)");

static unsigned int litesata_msi_timeout_ms = LITESATA_DMA_WAIT_TIMEOUT_MS;
module_param_named(msi_timeout_ms, litesata_msi_timeout_ms, uint, 0644);
MODULE_PARM_DESC(msi_timeout_ms, "Timeout (ms) to wait for MSI before polling");

static unsigned int litesata_irq_arm_delay_us = 0;
module_param_named(irq_arm_delay_us, litesata_irq_arm_delay_us, uint, 0644);
MODULE_PARM_DESC(irq_arm_delay_us, "Delay (us) after STRT before waiting on MSI");

static unsigned int litesata_early_poll_us = 0;
module_param_named(early_poll_us, litesata_early_poll_us, uint, 0644);
MODULE_PARM_DESC(early_poll_us, "Early poll window (us) after STRT to catch fast DONE");

/* 32-bit DMA enforcement & optional software bounce */
static bool litesata_strict_32bit = true;
module_param_named(strict_32bit, litesata_strict_32bit, bool, 0644);
MODULE_PARM_DESC(strict_32bit, "Require 32-bit DMA mask at probe (no 64-bit fallback)");

static bool litesata_force_bounce = false;
module_param_named(force_bounce, litesata_force_bounce, bool, 0644);
MODULE_PARM_DESC(force_bounce, "Always use 32-bit coherent bounce buffers for I/O");

static bool litesata_no_bounce = false;
module_param_named(no_bounce, litesata_no_bounce, bool, 0644);
MODULE_PARM_DESC(no_bounce, "Never bounce (debug only; will error if DMA addr >= 4GiB)");

/* -------- LiteSATA register layout -------- */

#define LITESATA_ID_STRT   0x00 // 1bit, w
#define LITESATA_ID_DONE   0x04 // 1bit, ro
#define LITESATA_ID_DWIDTH 0x08 // 16bit, ro
#define LITESATA_ID_SRCVLD 0x0c // 1bit, ro
#define LITESATA_ID_SRCRDY 0x10 // 1bit, w
#define LITESATA_ID_SRCDAT 0x14 // 32bit, ro

#define LITESATA_PHY_ENA   0x00 // 1bit, w
#define LITESATA_PHY_STS   0x04 // 4bit, ro

#define LSPHY_STS_RDY BIT(0)
#define LSPHY_STS_TX  BIT(1)
#define LSPHY_STS_RX  BIT(2)
#define LSPHY_STS_CTL BIT(3)

#define LITESATA_DMA_SECT  0x00 // 48bit, w
#define LITESATA_DMA_NSEC  0x08 // 16bit, w
#define LITESATA_DMA_ADDR  0x0c // 64bit, w
#define LITESATA_DMA_STRT  0x14 // 1bit, w
#define LITESATA_DMA_DONE  0x18 // 1bit, ro
#define LITESATA_DMA_ERR   0x1c // 1bit, ro

#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif
#define SECTOR_SHIFT 9
#define LITESATA_MAX_SECTORS (4096)            /* 2 MiB */
#define LITESATA_MAX_SEGMENTS (64)
#define LITESATA_MAX_SEGMENT_SIZE (2 * 1024 * 1024)

struct litesata_dev {
	struct device *dev;

	struct mutex lock_reader;
	struct mutex lock_writer;

	void __iomem *lsident;
	void __iomem *lsphy;
	void __iomem *lsreader;
	void __iomem *lswriter;

	struct completion dma_done_reader; /* completed by MSI notify hooks */
	struct completion dma_done_writer; /* completed by MSI notify hooks */
	bool dma_32bit;
};

static atomic64_t litesata_msi_reader_cnt = ATOMIC64_INIT(0);
static atomic64_t litesata_msi_writer_cnt = ATOMIC64_INIT(0);

/* ------------------------------------------------------------------ */
/* Single-instance MSI notification hooks (minimal integration):
 * The PCI driver calls these when the corresponding MSI fires.
 * If you add multi-instance later, replace this with a registry keyed
 * by PCI device or platform device.
 */
static struct litesata_dev *lbd_global;

void litesata_msi_signal_reader(void) /* SECTOR2MEM done */
{
	struct litesata_dev *lbd = READ_ONCE(lbd_global);
	if (lbd) {
		atomic64_inc(&litesata_msi_reader_cnt);
		complete(&lbd->dma_done_reader);
	}
}

EXPORT_SYMBOL_GPL(litesata_msi_signal_reader);

void litesata_msi_signal_writer(void) /* MEM2SECTOR done */
{
	struct litesata_dev *lbd = READ_ONCE(lbd_global);
	if (lbd) {
		atomic64_inc(&litesata_msi_writer_cnt);
		complete(&lbd->dma_done_writer);
	}
}
EXPORT_SYMBOL_GPL(litesata_msi_signal_writer);

static void __iomem *litesata_get_regs(struct platform_device *pdev, const char *name)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_REG, name);
	if (res)
		return (void __iomem *)(uintptr_t)res->start;

	return devm_platform_ioremap_resource_byname(pdev, name);
}

/* Bounce-buffer helpers (strict 32-bit coherent memory) */
struct litesata_bounce {
	void           *cpu;
	dma_addr_t      dma;
	size_t          len;
};

static int litesata_bounce_alloc(struct litesata_dev *lbd, size_t len, struct litesata_bounce *b)
{
	b->cpu = dma_alloc_coherent(lbd->dev, len, &b->dma, GFP_KERNEL | GFP_DMA32);
	if (!b->cpu)
		return -ENOMEM;
	b->len = len;
	return 0;
}

static void litesata_bounce_free(struct litesata_dev *lbd, struct litesata_bounce *b)
{
	if (b->cpu)
		dma_free_coherent(lbd->dev, b->len, b->cpu, b->dma);
}

/* DONE is the ground truth; always wait for it with a bound timeout. */
static int litesata_wait_done(struct litesata_dev *lbd, void __iomem *regs, unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	unsigned int spins = 0;

	while ((litex_read8(regs + LITESATA_DMA_DONE) & 0x01) == 0) {
		cpu_relax();
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		if ((++spins & 0xFFFF) == 0)
			cond_resched();
	}
	return 0;
}

/* caller must hold lbd->lock to prevent interleaving transfers */
static int litesata_do_dma(struct litesata_dev *lbd, void __iomem *regs,
			   dma_addr_t host_bus_addr, sector_t sector, unsigned int count,
			   struct completion *done)
{
	bool use_msi = !litesata_force_polling && READ_ONCE(lbd_global) != NULL;
	int ret;
	u64 t_start_ns = 0;

	t_start_ns = ktime_get_ns();

	if (use_msi)
		reinit_completion(done);

	/* Program DMA */
	litex_write64(regs + LITESATA_DMA_SECT, sector);
	litex_write16(regs + LITESATA_DMA_NSEC, count);
	litex_write64(regs + LITESATA_DMA_ADDR, (u64)host_bus_addr);
	/* Make sure addr/len visible to device before STRT */
	wmb();
	litex_write8(regs + LITESATA_DMA_STRT, 1);

	/* Optional fixed arming delay */
	if (use_msi && litesata_irq_arm_delay_us)
		udelay(litesata_irq_arm_delay_us);

	/* Early poll “grace window”: catches ultra-fast completions */
	if (litesata_early_poll_us) {
		u64 start = ktime_get_ns();
		u64 budget = (u64)litesata_early_poll_us * 1000ULL;
		while (likely(ktime_get_ns() - start < budget)) {
			if (litex_read8(regs + LITESATA_DMA_DONE) & 0x01)
				goto check_err_and_exit; /* finished already */
			cpu_relax();
		}
	}

	/*
	 * MSI is only a hint. We may get stray/early MSIs. Always gate on DONE.
	 * Avoid an "early fallback" window that effectively turns MSI into polling.
	 */
	if (use_msi) {
		unsigned long to = msecs_to_jiffies(LITESATA_DONE_TIMEOUT_MSI_MS);
		if (!wait_for_completion_timeout(done, to)) {
			dev_warn_ratelimited(lbd->dev,
				"No MSI within %ums; falling back to polling\n",
				LITESATA_DONE_TIMEOUT_MSI_MS);
			use_msi = false;
		} else {
			dev_dbg_ratelimited(lbd->dev, "MSI completion observed\n");
		}
	}

#if 0
	dev_info(lbd->dev, "MSI counts: r=%lld w=%lld\n",
		(long long)atomic64_read(&litesata_msi_reader_cnt),
		(long long)atomic64_read(&litesata_msi_writer_cnt));
#endif
	/* Always wait for DONE with a bound timeout (no infinite hang). */
	ret = litesata_wait_done(lbd, regs,
				 use_msi ? LITESATA_DONE_TIMEOUT_MSI_MS
					 : LITESATA_DONE_TIMEOUT_POLL_MS);
	if (ret) {
		dev_err(lbd->dev,
			"DMA DONE timeout (%s) sector=%lld nsec=%u addr=0x%llx\n",
			use_msi ? "msi" : "poll",
			(long long)sector, count,
			(unsigned long long)host_bus_addr);
		return ret;
	}

check_err_and_exit:
	/* Always verify hardware error bit */
	if ((litex_read8(regs + LITESATA_DMA_ERR) & 0x01) == 0)
	{
		u64 t_end_ns = ktime_get_ns();
		u64 bytes = (u64)count * SECTOR_SIZE;
		u64 dur_ns = (t_end_ns > t_start_ns) ? (t_end_ns - t_start_ns) : 0;
		u64 mbps = dur_ns ? (bytes * 1000ULL * 1000ULL * 1000ULL) / (dur_ns * 1024ULL * 1024ULL) : 0;
		dev_dbg_ratelimited(lbd->dev,
			"DMA ok: %u sectors (%llu bytes), %llu ns, ~%llu MiB/s\n",
			count, (unsigned long long)bytes,
			(unsigned long long)dur_ns,
			(unsigned long long)mbps);
		return 0;
	}

	dev_err(lbd->dev, "failed transferring sector %lld\n", (long long)sector);
	return -EIO;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
typedef unsigned int blk_opf_t;
#endif

/* Process a single bvec of a bio. */
static int litesata_do_bvec(struct litesata_dev *lbd, struct bio_vec *bv,
			    blk_opf_t op, sector_t sector)
{
	void __iomem *regs = op_is_write(op) ? lbd->lswriter : lbd->lsreader;
	struct mutex *lock = op_is_write(op) ? &lbd->lock_writer : &lbd->lock_reader;
	struct completion *done = op_is_write(op) ? &lbd->dma_done_writer : &lbd->dma_done_reader;
	enum dma_data_direction dir = op_is_write(op) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;

	unsigned int len  = bv->bv_len;
	unsigned int off  = bv->bv_offset;
	struct page *pg   = bv->bv_page;
	unsigned int nsec = len >> SECTOR_SHIFT;

	/* basic invariants guaranteed by submit_bio() WARNs */
	if ((off & (SECTOR_SIZE - 1)) || (len & (SECTOR_SIZE - 1)) || nsec == 0)
		return -EINVAL;

	/* Choose DMA address: direct map or bounce */
	dma_addr_t dma = (dma_addr_t)0;
	bool need_bounce = litesata_force_bounce;
	struct litesata_bounce bounce = { 0 };

	if (!need_bounce) {
		dma = dma_map_page(lbd->dev, pg, off, len, dir);
		if (dma_mapping_error(lbd->dev, dma)) {
			dev_err(lbd->dev, "dma_map_page failed (len=%u)\n", len);
			return -EIO;
		}
		/* Enforce < 4GiB */
		if (upper_32_bits(dma)) {
			need_bounce = true;
			if (litesata_no_bounce) {
				dma_unmap_page(lbd->dev, dma, len, dir);
				dev_err(lbd->dev, "DMA addr >= 4GiB (0x%llx) and bouncing disabled\n",
					(unsigned long long)dma);
				return -EIO;
			}
		}
	}

	if (need_bounce) {
		int ret = litesata_bounce_alloc(lbd, len, &bounce);
		if (ret) {
			dev_err(lbd->dev, "Failed to alloc 32-bit bounce buffer (%u bytes)\n", len);
			if (!litesata_force_bounce && dma)
				dma_unmap_page(lbd->dev, dma, len, dir);
			return ret;
		}
		/* For writes: copy source into bounce before DMA */
		if (op_is_write(op)) {
			void *src = kmap_local_page(pg) + off;
			memcpy(bounce.cpu, src, len);
			kunmap_local(src);
		}
		/* If we had a direct map, drop it; we use the bounce now */
		if (!litesata_force_bounce && dma && !upper_32_bits(dma))
			dma_unmap_page(lbd->dev, dma, len, dir);
		dma = bounce.dma; /* definitely <4GiB due to GFP_DMA32 */
	}

	mutex_lock(lock);
	{
		int err = litesata_do_dma(lbd, regs, dma, sector, nsec, done);
		mutex_unlock(lock);

		/* For reads: copy back from bounce to the bio page */
		if (need_bounce && !op_is_write(op) && !err) {
			void *dst = kmap_local_page(pg) + off;
			memcpy(dst, bounce.cpu, len);
			kunmap_local(dst);
		}

		/* Free / unmap */
		if (need_bounce) {
			litesata_bounce_free(lbd, &bounce);
		} else {
			dma_unmap_page(lbd->dev, dma, len, dir);
		}

		if (err)
			return err;
	}
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
static blk_qc_t litesata_submit_bio(struct bio *bio)
#else
static void litesata_submit_bio(struct bio *bio)
#endif
{
	struct litesata_dev *lbd = bio->bi_bdev->bd_disk->private_data;
	blk_opf_t op = bio_op(bio);
	sector_t sector = bio->bi_iter.bi_sector;
	struct bio_vec bvec;
	struct bvec_iter iter;

	bio_for_each_segment(bvec, bio, iter) {
		int err;

		/* Don't support un-aligned buffers. */
		WARN_ON_ONCE((bvec.bv_offset & (SECTOR_SIZE - 1)) ||
			     (bvec.bv_len & (SECTOR_SIZE - 1)));

		err = litesata_do_bvec(lbd, &bvec, op, sector);
		if (err) {
			dev_err(lbd->dev, "error %s sectors %lld..%lld\n",
				op_is_write(op) ? "writing" : "reading",
				(long long)sector,
				(long long)(sector + (bvec.bv_len >> SECTOR_SHIFT) - 1));
			bio_io_error(bio);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
			return BLK_QC_T_NONE;
#else
			return;
#endif
		}
		sector += (bvec.bv_len >> SECTOR_SHIFT);
	}

	bio_endio(bio);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
	return BLK_QC_T_NONE;
#endif
}

static const struct block_device_operations litesata_fops = {
	.owner		= THIS_MODULE,
	.submit_bio	= litesata_submit_bio,
};

static int litesata_init_ident(struct litesata_dev *lbd, sector_t *size)
{
	int i;
	u32 data;
	u16 buf[128];
	u8 model[38];

	/* reset phy */
	litex_write8(lbd->lsphy + LITESATA_PHY_ENA, 0);
	msleep(1);
	litex_write8(lbd->lsphy + LITESATA_PHY_ENA, 1);
	msleep(100);
	/* check phy status */
	if ((litex_read8(lbd->lsphy + LITESATA_PHY_STS) & LSPHY_STS_RDY) == 0)
		return -ENODEV;

	/* initiate `identify` sequence */
	litex_write8(lbd->lsident + LITESATA_ID_STRT, 1);
	msleep(100);
	/* check `identify` status */
	if ((litex_read8(lbd->lsident + LITESATA_ID_DONE) & 0x01) == 0)
		return -ENODEV;

	/* read `identify` response into buf */
	for (i = 0; i < 128 && litex_read8(lbd->lsident + LITESATA_ID_SRCVLD); i += 2) {
		data = litex_read32(lbd->lsident + LITESATA_ID_SRCDAT);
		litex_write8(lbd->lsident + LITESATA_ID_SRCRDY, 1);
		buf[i + 0] = (data >>  0) & 0xffff;
		buf[i + 1] = (data >> 16) & 0xffff;
	}
	/* get disk model */
	for (i = 0; i < 18; i++) {
		model[2*i + 0] = (buf[27+i] >> 8) & 0xff;
		model[2*i + 1] = (buf[27+i] >> 0) & 0xff;
	}
	model[36] = model[37] = 0;
	/* get disk capacity */
	*size = 0;
	*size += (((u64) buf[100]) <<  0);
	*size += (((u64) buf[101]) << 16);
	*size += (((u64) buf[102]) << 32);
	*size += (((u64) buf[103]) << 48);

	/* success */
	dev_info(lbd->dev, "%lld bytes; %s\n", (long long)(*size << SECTOR_SHIFT), model);
	return 0;
}

static void litesata_devm_put_disk(void *gendisk)
{
	put_disk(gendisk);
}

static void litesata_devm_del_disk(void *gendisk)
{
	del_gendisk(gendisk);
}

static int litesata_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct litesata_dev *lbd;
	struct gendisk *gendisk;
	sector_t size;
	int err = -ENOMEM;

	lbd = devm_kzalloc(dev, sizeof(*lbd), GFP_KERNEL);
	if (!lbd)
		return -ENOMEM;

	lbd->dev = dev;
	mutex_init(&lbd->lock_reader);
	mutex_init(&lbd->lock_writer);
	init_completion(&lbd->dma_done_reader);
	init_completion(&lbd->dma_done_writer);

	/* Enforce 32-bit DMA addresses; fail otherwise. */
	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (err) {
		dev_err(dev, "This platform cannot provide 32-bit DMA; refusing to bind\n");
		return err;
	}
	lbd->dma_32bit = true;

	lbd->lsident  = litesata_get_regs(pdev, "ident");
	if (IS_ERR(lbd->lsident))
		return PTR_ERR(lbd->lsident);

	lbd->lsphy    = litesata_get_regs(pdev, "phy");
	if (IS_ERR(lbd->lsphy))
		return PTR_ERR(lbd->lsphy);

	lbd->lsreader = litesata_get_regs(pdev, "reader");
	if (IS_ERR(lbd->lsreader))
		return PTR_ERR(lbd->lsreader);

	lbd->lswriter = litesata_get_regs(pdev, "writer");
	if (IS_ERR(lbd->lswriter))
		return PTR_ERR(lbd->lswriter);

	/* Identify disk, get model and size. */
	err = litesata_init_ident(lbd, &size);
	if (err)
		return err;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	{
		struct queue_limits lim = {
			.physical_block_size = SECTOR_SIZE,
			.logical_block_size  = SECTOR_SIZE,
			.max_hw_sectors      = LITESATA_MAX_SECTORS,
			.max_segments        = LITESATA_MAX_SEGMENTS,
			.max_segment_size    = LITESATA_MAX_SEGMENT_SIZE,
		};
		gendisk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	}
#else
	gendisk = blk_alloc_disk(NUMA_NO_NODE);
	if (!IS_ERR_OR_NULL(gendisk) && gendisk->queue) {
		blk_queue_logical_block_size(gendisk->queue, SECTOR_SIZE);
		blk_queue_physical_block_size(gendisk->queue, SECTOR_SIZE);
		blk_queue_max_hw_sectors(gendisk->queue, LITESATA_MAX_SECTORS);
		blk_queue_max_segments(gendisk->queue, LITESATA_MAX_SEGMENTS);
		blk_queue_max_segment_size(gendisk->queue, LITESATA_MAX_SEGMENT_SIZE);
	}
#endif
	if (IS_ERR(gendisk))
		return PTR_ERR(gendisk);

	err = devm_add_action_or_reset(dev, litesata_devm_put_disk, gendisk);
	if (err)
		return dev_err_probe(dev, err, "Can't register put_disk action\n");

	gendisk->private_data = lbd;
	gendisk->fops = &litesata_fops;
	snprintf(gendisk->disk_name, DISK_NAME_LEN, "litesata%d", pdev->id);
	set_capacity(gendisk, size);

	err = add_disk(gendisk);
	if (err)
		return err;

	err = devm_add_action_or_reset(dev, litesata_devm_del_disk, gendisk);
	if (err)
		return dev_err_probe(dev, err, "Can't register del_disk action\n");

	/* Minimal single-instance registration for MSI notify hooks */
	WRITE_ONCE(lbd_global, lbd);

	dev_info(dev,
		 "probe success; sector size = %d, dma_%sbit (%s), bounce=%s, strict32=%s\n",
		 SECTOR_SIZE, lbd->dma_32bit ? "32" : "64",
		 litesata_force_polling ? "POLLING" : "MSI-driven+poll",
		 litesata_force_bounce ? "forced" : (litesata_no_bounce ? "disabled" : "auto"),
		 litesata_strict_32bit ? "on" : "off");
	return 0;
}

static const struct of_device_id litesata_match[] = {
	{ .compatible = "litex,litesata" },
	{ }
};
MODULE_DEVICE_TABLE(of, litesata_match);
MODULE_ALIAS("platform:litesata");

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static int litesata_remove(struct platform_device *pdev)
#else
static void litesata_remove(struct platform_device *pdev)
#endif
{
	/* Clear the global hook to avoid stray completes after remove */
	WRITE_ONCE(lbd_global, NULL);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif
}

static struct platform_driver litesata_driver = {
	.probe = litesata_probe,
	.remove = litesata_remove,
	.driver = {
		.name = "litesata",
		.of_match_table = litesata_match,
	},
};

/* Built-into-main: expose init/exit rather than being its own module */
int __init litesata_init(void)
{
	return platform_driver_register(&litesata_driver);
}

void __exit litesata_exit(void)
{
	platform_driver_unregister(&litesata_driver);
}

MODULE_DESCRIPTION("LiteX LiteSATA block driver");
MODULE_AUTHOR("Gabriel Somlo <gsomlo@gmail.com>");
MODULE_LICENSE("GPL v2");
