// SPDX-License-Identifier: GPL-2.0
/*
 * LiteX LiteSATA block driver — host-DMA version
 *
 * The FPGA DMA engine now reads/writes directly to host memory.
 * For each bio_vec, we map its page with the DMA API and program the DMA
 * registers with that host bus address (preferably <4 GiB if supported).
 *
 * Copyright (c) 2022 Gabriel Somlo <gsomlo@gmail.com>
 * Host-DMA adaptation (c) 2025 EnjoyDigital / contributors
 */

#include <linux/bits.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/litex.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/ktime.h>
#include <asm/processor.h> /* cpu_relax() */
#include <linux/device.h>

#include "litex.h"
#include "litesata.h"

/* ------------------------------------------------------------------------- */
/* CSRs                                                                      */
/* ------------------------------------------------------------------------- */

#define LITESATA_ID_STRT   0x00 /* 1bit, w  */
#define LITESATA_ID_DONE   0x04 /* 1bit, ro */
#define LITESATA_ID_DWIDTH 0x08 /* 16bit, ro*/
#define LITESATA_ID_SRCVLD 0x0c /* 1bit, ro */
#define LITESATA_ID_SRCRDY 0x10 /* 1bit, w  */
#define LITESATA_ID_SRCDAT 0x14 /* 32bit, ro*/

#define LITESATA_PHY_ENA   0x00 /* 1bit, w  */
#define LITESATA_PHY_STS   0x04 /* 4bit, ro */

#define LSPHY_STS_RDY BIT(0)
#define LSPHY_STS_TX  BIT(1)
#define LSPHY_STS_RX  BIT(2)
#define LSPHY_STS_CTL BIT(3)

#define LITESATA_DMA_SECT  0x00 /* 48bit, w  */
#define LITESATA_DMA_NSEC  0x08 /* 16bit, w  */
#define LITESATA_DMA_ADDR  0x0c /* 64bit, w  */
#define LITESATA_DMA_STRT  0x14 /* 1bit, w   */
#define LITESATA_DMA_DONE  0x18 /* 1bit, ro  */
#define LITESATA_DMA_ERR   0x1c /* 1bit, ro  */

#define LITESATA_IRQ_STS   0x00 /* 2bit, ro  */
#define LITESATA_IRQ_PEND  0x04 /* 2bit, rw  */
#define LITESATA_IRQ_ENA   0x08 /* 2bit, w   */

#define LSIRQ_RD BIT(0)
#define LSIRQ_WR BIT(1)

#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif
#define SECTOR_SHIFT 9

/* ------------------------------------------------------------------------- */
/* Stats (optional)                                                          */
/* ------------------------------------------------------------------------- */

#define LITESATA_STATS

#ifdef LITESATA_STATS
#include <linux/debugfs.h>

struct lxs_stats {
	u64 io_cnt;        /* submit_bio() calls processed */
	u64 sectors;       /* sectors moved total (both R/W) */
	u64 ns_prog_dma;   /* time programming DMA CSRs       */
	u64 ns_wait_dma;   /* time waiting for DMA complete   */
};
static struct lxs_stats litesata_stats;
static struct dentry *litesata_dbgdir;

#define LXS_ADD(_field, _delta) do {                           \
	u64 __v = READ_ONCE(litesata_stats._field);             \
	__v += (u64)(_delta);                                   \
	WRITE_ONCE(litesata_stats._field, __v);                 \
} while (0)

static inline u64 lxs_ktime_to_ns(ktime_t kt) { return (u64)ktime_to_ns(kt); }

static void lxs_stats_debugfs_init(void)
{
	litesata_dbgdir = debugfs_create_dir("litesata", NULL);
	if (IS_ERR_OR_NULL(litesata_dbgdir))
		return;

	debugfs_create_u64("io_cnt",      0444, litesata_dbgdir, &litesata_stats.io_cnt);
	debugfs_create_u64("sectors",     0444, litesata_dbgdir, &litesata_stats.sectors);
	debugfs_create_u64("ns_prog_dma", 0444, litesata_dbgdir, &litesata_stats.ns_prog_dma);
	debugfs_create_u64("ns_wait_dma", 0444, litesata_dbgdir, &litesata_stats.ns_wait_dma);
}

static void lxs_stats_debugfs_exit(void)
{
	debugfs_remove_recursive(litesata_dbgdir);
	litesata_dbgdir = NULL;
}
#else
#define LXS_ADD(_f,_d)     do { } while (0)
static inline void lxs_stats_debugfs_init(void) { }
static inline void lxs_stats_debugfs_exit(void) { }
static inline u64 lxs_ktime_to_ns(ktime_t kt) { return 0; }
#endif

/* ------------------------------------------------------------------------- */
/* Types                                                                     */
/* ------------------------------------------------------------------------- */

struct litesata_dev {
	struct device *dev;
	struct mutex lock;

	void __iomem *lsident;
	void __iomem *lsphy;
	void __iomem *lsreader; /* DMA regs for READs (device->host)  */
	void __iomem *lswriter; /* DMA regs for WRITEs (host->device) */
	void __iomem *lsirq;

	struct completion dma_done;
	int irq;

	/* prefer 32-bit DMA if possible */
	bool dma_32bit;
};

/* ------------------------------------------------------------------------- */
/* MMIO helpers                                                              */
/* ------------------------------------------------------------------------- */

static void __iomem *litesata_get_regs(struct platform_device *pdev, const char *name)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_REG, name);
	if (res)
		return (void __iomem *)(uintptr_t)res->start;

	return devm_platform_ioremap_resource_byname(pdev, name);
}

/* ------------------------------------------------------------------------- */
/* DMA kick / completion                                                     */
/* ------------------------------------------------------------------------- */
/* Caller must hold lbd->lock to prevent interleaving transfers. */
static int litesata_do_dma(struct litesata_dev *lbd, void __iomem *regs,
			   dma_addr_t host_bus_addr, sector_t sector, unsigned int nsec)
{
	int irq = lbd->irq;
	ktime_t t_prog0, t_prog1, t_wait0, t_wait1;

	if (irq)
		reinit_completion(&lbd->dma_done);

	/* program DMA (order matters; do not write 0 to STRT to "reset") */
	t_prog0 = ktime_get();
	litex_write64(regs + LITESATA_DMA_SECT, sector);
	litex_write16(regs + LITESATA_DMA_NSEC, nsec);
	litex_write64(regs + LITESATA_DMA_ADDR, (u64)host_bus_addr);
	litex_write8(regs + LITESATA_DMA_STRT, 1);
	t_prog1 = ktime_get();

	/* wait completion */
	t_wait0 = ktime_get();
	if (irq) {
		wait_for_completion(&lbd->dma_done);
	} else {
		while ((litex_read8(regs + LITESATA_DMA_DONE) & 0x01) == 0)
			cpu_relax();
	}
	t_wait1 = ktime_get();

#ifdef LITESATA_STATS
	LXS_ADD(ns_prog_dma, lxs_ktime_to_ns(ktime_sub(t_prog1, t_prog0)));
	LXS_ADD(ns_wait_dma, lxs_ktime_to_ns(ktime_sub(t_wait1, t_wait0)));
	LXS_ADD(sectors, nsec);
#endif

	/* status */
	if ((litex_read8(regs + LITESATA_DMA_ERR) & 0x01) == 0)
		return 0;

	dev_err(lbd->dev, "DMA error on sector %lld (nsec=%u)\n",
		(long long)sector, nsec);
	return -EIO;
}

/* ------------------------------------------------------------------------- */
/* Per-bvec transfer (pure DMA; no memcpy)                                   */
/* ------------------------------------------------------------------------- */

static int litesata_do_bvec(struct litesata_dev *lbd, struct bio_vec *bv,
			    blk_opf_t op, sector_t sector)
{
	void __iomem *regs = op_is_write(op) ? lbd->lswriter : lbd->lsreader;
	enum dma_data_direction dir = op_is_write(op) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;

	unsigned int len  = bv->bv_len;
	unsigned int off  = bv->bv_offset;
	struct page *pg   = bv->bv_page;
	unsigned int nsec = len >> SECTOR_SHIFT;

	/* basic invariants guaranteed by submit_bio() WARNs */
	if ((off & (SECTOR_SIZE - 1)) || (len & (SECTOR_SIZE - 1)) || nsec == 0)
		return -EINVAL;

	/*
	 * Map the buffer for DMA. Each bio_vec points into a single page
	 * (possibly less than a page). We program one DMA per bio_vec.
	 */
	dma_addr_t dma = dma_map_page(lbd->dev, pg, off, len, dir);
	if (dma_mapping_error(lbd->dev, dma)) {
		dev_err(lbd->dev, "dma_map_page failed (len=%u)\n", len);
		return -EIO;
	}

	/*
	 * If we really must keep addresses <4 GiB (32-bit), dma_set_mask()
	 * in probe() will make the API bounce as needed. Still, double-check
	 * and warn if firmware was *strictly* 32-bit and we somehow got >4G.
	 */
	if (lbd->dma_32bit && upper_32_bits(dma)) {
		dev_warn_once(lbd->dev, "32-bit DMA requested but got addr >4G (0x%llx); using IOMMU/bounce.\n",
			      (unsigned long long)dma);
	}

	mutex_lock(&lbd->lock);
	/* Single DMA for the whole bio_vec */
	{
		int err = litesata_do_dma(lbd, regs, dma, sector, nsec);
		mutex_unlock(&lbd->lock);
		dma_unmap_page(lbd->dev, dma, len, dir);
		if (err)
			return err;
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Block ops / submit                                                         */
/* ------------------------------------------------------------------------- */

static void litesata_submit_bio(struct bio *bio)
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
			return;
		}
		sector += (bvec.bv_len >> SECTOR_SHIFT);
	}

#ifdef LITESATA_STATS
	LXS_ADD(io_cnt, 1);
#endif
	bio_endio(bio);
}

static const struct block_device_operations litesata_fops = {
	.owner		= THIS_MODULE,
	.submit_bio	= litesata_submit_bio,
};

/* ------------------------------------------------------------------------- */
/* IRQ path                                                                   */
/* ------------------------------------------------------------------------- */

static irqreturn_t litesata_interrupt(int irq, void *arg)
{
	struct litesata_dev *lbd = arg;
	u32 pend;

	/* Should be either LSIRQ_RD or LSIRQ_WR (never both). */
	pend = litex_read32(lbd->lsirq + LITESATA_IRQ_PEND);

	/* Acknowledge interrupt. */
	litex_write32(lbd->lsirq + LITESATA_IRQ_PEND, pend);

	/* Signal DMA xfer completion. */
	complete(&lbd->dma_done);

	return IRQ_HANDLED;
}

static void litesata_devm_irq_off(void *data)
{
	struct litesata_dev *lbd = data;

	litex_write32(lbd->lsirq + LITESATA_IRQ_ENA, 0);
}

static int litesata_irq_init(struct platform_device *pdev,
			     struct litesata_dev *lbd)
{
	struct device *dev = &pdev->dev;
	int ret;

	ret = platform_get_irq_optional(pdev, 0);
	if (ret < 0 && ret != -ENXIO)
		return ret;
	if (ret > 0)
		lbd->irq = ret;
	else {
		dev_warn(dev, "Failed to get IRQ, using polling\n");
		goto use_polling;
	}

	lbd->lsirq = litesata_get_regs(pdev, "irq");
	if (IS_ERR(lbd->lsirq))
		return PTR_ERR(lbd->lsirq);

	ret = devm_request_irq(dev, lbd->irq, litesata_interrupt, 0, "litesata", lbd);
	if (ret < 0) {
		dev_warn(dev, "IRQ request error %d, using polling\n", ret);
		goto use_polling;
	}

	ret = devm_add_action_or_reset(dev, litesata_devm_irq_off, lbd);
	if (ret)
		return dev_err_probe(dev, ret, "Can't register irq_off action\n");

	/* Clear & enable DMA-completion interrupts. */
	litex_write32(lbd->lsirq + LITESATA_IRQ_PEND, LSIRQ_RD | LSIRQ_WR);
	litex_write32(lbd->lsirq + LITESATA_IRQ_ENA,  LSIRQ_RD | LSIRQ_WR);

	init_completion(&lbd->dma_done);
	return 0;

use_polling:
	lbd->irq = 0;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* IDENT / capacity                                                           */
/* ------------------------------------------------------------------------- */

static int litesata_init_ident(struct litesata_dev *lbd, sector_t *size)
{
	int i;
	u32 data;
	u16 buf[128];
	u8 model[38];

	/* Reset phy. */
	litex_write8(lbd->lsphy + LITESATA_PHY_ENA, 0);
	msleep(1);
	litex_write8(lbd->lsphy + LITESATA_PHY_ENA, 1);
	msleep(100);

	/* Check phy status. */
	if ((litex_read8(lbd->lsphy + LITESATA_PHY_STS) & LSPHY_STS_RDY) == 0)
		return -ENODEV;

	/* Initiate IDENTIFY sequence. */
	litex_write8(lbd->lsident + LITESATA_ID_STRT, 1);
	msleep(100);

	/* Check IDENTIFY status. */
	if ((litex_read8(lbd->lsident + LITESATA_ID_DONE) & 0x01) == 0)
		return -ENODEV;

	/* Read IDENTIFY response into buf (pairs of 16-bit words per 32-bit). */
	for (i = 0; i < 128 && litex_read8(lbd->lsident + LITESATA_ID_SRCVLD); i += 2) {
		data = litex_read32(lbd->lsident + LITESATA_ID_SRCDAT);
		litex_write8(lbd->lsident + LITESATA_ID_SRCRDY, 1);
		buf[i + 0] = (data >>  0) & 0xffff;
		buf[i + 1] = (data >> 16) & 0xffff;
	}

	/* Disk model (ASCII, byte-swapped words). */
	for (i = 0; i < 18; i++) {
		model[2*i + 0] = (buf[27+i] >> 8) & 0xff;
		model[2*i + 1] = (buf[27+i] >> 0) & 0xff;
	}
	model[36] = model[37] = 0;

	/* Capacity in 512B sectors: words 100..103. */
	*size  = 0;
	*size += (((u64)buf[100]) <<  0);
	*size += (((u64)buf[101]) << 16);
	*size += (((u64)buf[102]) << 32);
	*size += (((u64)buf[103]) << 48);

	dev_info(lbd->dev, "%lld bytes; %s\n", (long long)(*size << SECTOR_SHIFT), model);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Probe / module                                                             */
/* ------------------------------------------------------------------------- */

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
	mutex_init(&lbd->lock);

	/*
	 * Prefer 32-bit DMA addresses (to keep FPGA addressing simple).
	 * If not possible, fall back to 64-bit but warn once.
	 */
	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (err) {
		err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
		if (err) {
			dev_err(dev, "No suitable DMA mask (need 32/64-bit)\n");
			return err;
		}
		dev_warn(dev, "Using 64-bit DMA addresses (could exceed 4GiB)\n");
		lbd->dma_32bit = false;
	} else {
		lbd->dma_32bit = true;
	}

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

	/* Configure interrupts (or fall back to polling). */
	err = litesata_irq_init(pdev, lbd);
	if (err)
		return err;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	{
		struct queue_limits lim = {
			.physical_block_size = SECTOR_SIZE,
			.logical_block_size  = SECTOR_SIZE,
		};
		gendisk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	}
#else
	gendisk = blk_alloc_disk(NUMA_NO_NODE);
	if (!IS_ERR_OR_NULL(gendisk) && gendisk->queue) {
		blk_queue_logical_block_size(gendisk->queue, SECTOR_SIZE);
		blk_queue_physical_block_size(gendisk->queue, SECTOR_SIZE);
	}
#endif
	if (IS_ERR(gendisk))
		return PTR_ERR(gendisk);

	err = devm_add_action_or_reset(dev, litesata_devm_put_disk, gendisk);
	if (err)
		return dev_err_probe(dev, err, "Can't register put_disk action\n");

	gendisk->private_data = lbd;
	gendisk->fops = &litesata_fops;
	/* Make disk name unique per instance, e.g. litesata1, litesata2, … */
	snprintf(gendisk->disk_name, DISK_NAME_LEN, "litesata%d", pdev->id);
	set_capacity(gendisk, size);

	err = add_disk(gendisk);
	if (err)
		return err;

	err = devm_add_action_or_reset(dev, litesata_devm_del_disk, gendisk);
	if (err)
		return dev_err_probe(dev, err, "Can't register del_disk action\n");

#ifdef LITESATA_STATS
	lxs_stats_debugfs_init();
#endif

	dev_info(dev, "probe success; sector size = %d, dma_%sbit\n",
		 SECTOR_SIZE, lbd->dma_32bit ? "32" : "64");
	return 0;
}

static const struct of_device_id litesata_match[] = {
	{ .compatible = "litex,litesata" },
	{ }
};
MODULE_DEVICE_TABLE(of, litesata_match);
MODULE_ALIAS("platform:litesata");

static int litesata_remove(struct platform_device *pdev)
{
#ifdef LITESATA_STATS
	lxs_stats_debugfs_exit();
#endif
	return 0;
}

static struct platform_driver litesata_driver = {
	.probe = litesata_probe,
	.remove = litesata_remove,
	.driver = {
		.name = "litesata",
		.of_match_table = litesata_match,
	},
};

/* Build-into-main: expose init/exit rather than being its own module. */
int __init litesata_init(void)
{
	return platform_driver_register(&litesata_driver);
}

void __exit litesata_exit(void)
{
	platform_driver_unregister(&litesata_driver);
#ifdef LITESATA_STATS
	lxs_stats_debugfs_exit();
#endif
}

MODULE_DESCRIPTION("LiteX LiteSATA block driver (host-DMA)");
MODULE_AUTHOR("Gabriel Somlo <gsomlo@gmail.com> / EnjoyDigital");
MODULE_LICENSE("GPL v2");
