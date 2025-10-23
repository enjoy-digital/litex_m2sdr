// SPDX-License-Identifier: GPL-2.0
/*
 * LiteX LiteSATA block driver
 *
 * PCIe-host staging mode:
 *  - copies 512B sectors over BAR0 to SoC SRAM,
 *  - programs LiteSATA DMA with the *SoC bus* address of that SRAM.
 *
 * Copyright (c) 2022 Gabriel Somlo <gsomlo@gmail.com>
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
#include <linux/dma-mapping.h>

#include "litex.h"
#include "litesata.h"

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

struct litesata_dev {
	struct device *dev;

	struct mutex lock;

	void __iomem *lsident;
	void __iomem *lsphy;
	void __iomem *lsreader;
	void __iomem *lswriter;

	struct completion dma_done; /* completed by MSI notify hooks */
	bool dma_32bit;
};

/* Single-instance MSI notification hook (minimal integration):
 * The PCI driver calls these when the corresponding MSI fires.
 * If you add multi-instance later, replace this with a small registry.
 */
static struct litesata_dev *lbd_global;

void litesata_msi_signal_reader(void) /* SECTOR2MEM done */
{
	if (lbd_global)
		complete(&lbd_global->dma_done);
}
EXPORT_SYMBOL_GPL(litesata_msi_signal_reader);

void litesata_msi_signal_writer(void) /* MEM2SECTOR done */
{
	if (lbd_global)
		complete(&lbd_global->dma_done);
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

/* caller must hold lbd->lock to prevent interleaving transfers */
static int litesata_do_dma(struct litesata_dev *lbd, void __iomem *regs,
			   dma_addr_t host_bus_addr, sector_t sector, unsigned int count)
{
	/* If MSI notify hooks are wired (PCI ISR will call us), use completion;
	 * otherwise poll the DONE bit like before.
	 */
	bool use_msi = (lbd_global != NULL);

	if (use_msi)
		reinit_completion(&lbd->dma_done);

	/* NOTE: do *not* start by writing 0 to LITESATA_DMA_STRT!!! */
	litex_write64(regs + LITESATA_DMA_SECT, sector);
	litex_write16(regs + LITESATA_DMA_NSEC, count);
	litex_write64(regs + LITESATA_DMA_ADDR, (u64)host_bus_addr);
	litex_write8(regs + LITESATA_DMA_STRT, 1);

	if (use_msi) {
		/* Wait until PCI ISR notifies us */
		wait_for_completion(&lbd->dma_done);
	} else {
		/* Polling fallback */
		while ((litex_read8(regs + LITESATA_DMA_DONE) & 0x01) == 0)
			cpu_relax();
	}

	/* check if DMA xfer successful */
	if ((litex_read8(regs + LITESATA_DMA_ERR) & 0x01) == 0)
		return 0;

	dev_err(lbd->dev, "failed transfering sector %Ld\n", sector);
	return -EIO;
}

/* Process a single bvec of a bio. */
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
		dev_warn_once(lbd->dev,
			      "32-bit DMA requested but got addr >4G (0x%llx); using IOMMU/bounce.\n",
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

	bio_endio(bio);
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
	// FIXME: make buf be u32[64], read in-place, and use le/be/2cpu
	for (i = 0; i < 128 && litex_read8(lbd->lsident + LITESATA_ID_SRCVLD); i += 2) {
		data = litex_read32(lbd->lsident + LITESATA_ID_SRCDAT);
		litex_write8(lbd->lsident + LITESATA_ID_SRCRDY, 1);
		buf[i + 0] = (data >>  0) & 0xffff;
		buf[i + 1] = (data >> 16) & 0xffff;
	}
	/* get disk model */
	// FIXME: there's gotta be a better way to do this :)
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
	mutex_init(&lbd->lock);
	init_completion(&lbd->dma_done);

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

	/* Minimal single-instance registration for MSI notify hooks */
	lbd_global = lbd;

	dev_info(dev, "probe success; sector size = %d, dma_%sbit (MSI-driven, poll fallback)\n",
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
