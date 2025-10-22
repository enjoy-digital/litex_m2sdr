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
#include <linux/interrupt.h>
#include <linux/litex.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/version.h>
#include <linux/string.h>

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

#define LITESATA_IRQ_STS   0x00 // 2bit, ro
#define LITESATA_IRQ_PEND  0x04 // 2bit, rw
#define LITESATA_IRQ_ENA   0x08 // 2bit, w

#define LSIRQ_RD BIT(0)
#define LSIRQ_WR BIT(1)

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
	void __iomem *lsirq;

	void __iomem *shbuf_rd;
	void __iomem *shbuf_wr;
	u64 shbuf_rd_bus;
	u64 shbuf_wr_bus;

	struct completion dma_done;
	int irq;
};

static void __iomem *litesata_get_regs(struct platform_device *pdev, const char *name)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_REG, name);
	if (res)
		return (void __iomem *)(uintptr_t)res->start;

	return devm_platform_ioremap_resource_byname(pdev, name);
}

static u64 litesata_get_busaddr(struct platform_device *pdev, const char *name)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	return res ? (u64)res->start : 0;
}

/* caller must hold lbd->lock to prevent interleaving transfers */
static int litesata_do_dma(struct litesata_dev *lbd, void __iomem *regs,
			   u64 soc_bus_addr, sector_t sector, unsigned int count)
{
	int irq = lbd->irq;

	if (irq)
		reinit_completion(&lbd->dma_done);

	/* NOTE: do *not* start by writing 0 to LITESATA_DMA_STRT!!! */
	litex_write64(regs + LITESATA_DMA_SECT, sector);
	litex_write16(regs + LITESATA_DMA_NSEC, count);
	litex_write64(regs + LITESATA_DMA_ADDR, soc_bus_addr);
	litex_write8(regs + LITESATA_DMA_STRT, 1);

	if (irq)
		wait_for_completion(&lbd->dma_done);

	// FIXME: should we implement a timeout here?
	// (or some other way to improve polling mode)?
	while ((litex_read8(regs + LITESATA_DMA_DONE) & 0x01) == 0);

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
	void __iomem *regs;
	unsigned int len, off, nsec, i;
	struct page *pg;

	if (!lbd->shbuf_rd || !lbd->shbuf_wr || !lbd->shbuf_rd_bus || !lbd->shbuf_wr_bus)
		return -ENODEV;

	if (op_is_write(op))
		regs = lbd->lswriter;
	else
		regs = lbd->lsreader;

	len  = bv->bv_len;
	off  = bv->bv_offset;
	pg   = bv->bv_page;
	nsec = len >> SECTOR_SHIFT;

	for (i = 0; i < nsec; i++) {
		void *kaddr = kmap_local_page(pg);
		void *src   = (char *)kaddr + off + (i << SECTOR_SHIFT);
		int err;

		mutex_lock(&lbd->lock);
		if (op_is_write(op)) {
			memcpy_toio(lbd->shbuf_wr, src, SECTOR_SIZE);
			err = litesata_do_dma(lbd, regs, lbd->shbuf_wr_bus, sector + i, 1);
		} else {
			err = litesata_do_dma(lbd, regs, lbd->shbuf_rd_bus, sector + i, 1);
			if (!err)
				memcpy_fromio(src, lbd->shbuf_rd, SECTOR_SIZE);
		}
		mutex_unlock(&lbd->lock);

		kunmap_local(kaddr);

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

		/* Don't support un-aligned buffer */
		WARN_ON_ONCE((bvec.bv_offset & (SECTOR_SIZE - 1)) ||
				(bvec.bv_len & (SECTOR_SIZE - 1)));

		err = litesata_do_bvec(lbd, &bvec, op, sector);
		if (err) {
			dev_err(lbd->dev, "error %s sectors %Ld..%Ld\n",
				op_is_write(op) ? "writing" : "reading",
				sector, sector + (bvec.bv_len >> SECTOR_SHIFT));
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

static irqreturn_t litesata_interrupt(int irq, void *arg)
{
	struct litesata_dev *lbd = arg;
	u32 pend;

	/* should be either LSIRQ_RD or LSIRQ_WR (but never *both*)! */
	pend = litex_read32(lbd->lsirq + LITESATA_IRQ_PEND);

	/* acknowledge interrupt */
	litex_write32(lbd->lsirq + LITESATA_IRQ_PEND, pend);

	/* signal DMA xfer completion */
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

	ret = devm_request_irq(dev, lbd->irq, litesata_interrupt, 0,
			       "litesata", lbd);
	if (ret < 0) {
		dev_warn(dev, "IRQ request error %d, using polling\n", ret);
		goto use_polling;
	}

	ret = devm_add_action_or_reset(dev, litesata_devm_irq_off, lbd);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Can't register irq_off action\n");

	/* Clear & enable DMA-completion interrupts */
	litex_write32(lbd->lsirq + LITESATA_IRQ_PEND, LSIRQ_RD | LSIRQ_WR);
	litex_write32(lbd->lsirq + LITESATA_IRQ_ENA, LSIRQ_RD | LSIRQ_WR);

	init_completion(&lbd->dma_done);

	return 0;

use_polling:
	lbd->irq = 0;
	return 0;
}

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
	for(i = 0;
	    i < 128 && litex_read8(lbd->lsident + LITESATA_ID_SRCVLD);
	    i += 2) {
		data = litex_read32(lbd->lsident + LITESATA_ID_SRCDAT);
		litex_write8(lbd->lsident + LITESATA_ID_SRCRDY, 1);
		buf[i + 0] = ((data >>  0) & 0xffff);
		buf[i + 1] = ((data >> 16) & 0xffff);
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
	dev_info(lbd->dev, "%Ld bytes; %s\n", *size << SECTOR_SHIFT, model);
	return 0;
}

static void litesata_devm_put_disk(void *gendisk)
{
	put_disk(gendisk);
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

	lbd->lsident = litesata_get_regs(pdev, "ident");
	if (IS_ERR(lbd->lsident))
		return PTR_ERR(lbd->lsident);

	lbd->lsphy = litesata_get_regs(pdev, "phy");
	if (IS_ERR(lbd->lsphy))
		return PTR_ERR(lbd->lsphy);

	lbd->lsreader = litesata_get_regs(pdev, "reader");
	if (IS_ERR(lbd->lsreader))
		return PTR_ERR(lbd->lsreader);

	lbd->lswriter = litesata_get_regs(pdev, "writer");
	if (IS_ERR(lbd->lswriter))
		return PTR_ERR(lbd->lswriter);

	lbd->shbuf_rd = litesata_get_regs(pdev, "buf_rd");
	if (IS_ERR(lbd->shbuf_rd)) lbd->shbuf_rd = NULL;
	lbd->shbuf_wr = litesata_get_regs(pdev, "buf_wr");
	if (IS_ERR(lbd->shbuf_wr)) lbd->shbuf_wr = NULL;

	lbd->shbuf_rd_bus = litesata_get_busaddr(pdev, "buf_rd_bus");
	lbd->shbuf_wr_bus = litesata_get_busaddr(pdev, "buf_wr_bus");

	/* Initialize disk, get model and size */
	err = litesata_init_ident(lbd, &size);
	if (err)
		return err;

	/* Configure interrupts */
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
		return dev_err_probe(dev, err,
				     "Can't register put_disk action\n");

	gendisk->private_data = lbd;
	gendisk->fops = &litesata_fops;
	strcpy(gendisk->disk_name, "litesata");
	set_capacity(gendisk, size);

	err = add_disk(gendisk);
	if (err)
		return err;

	dev_info(dev, "probe success; sector size = %d\n", SECTOR_SIZE);
	return 0;
}

static const struct of_device_id litesata_match[] = {
	{ .compatible = "litex,litesata" },
	{ }
};
MODULE_DEVICE_TABLE(of, litesata_match);
MODULE_ALIAS("platform:litesata");

static struct platform_driver litesata_driver = {
	.probe = litesata_probe,
	.driver = {
		.name = "litesata",
		.of_match_table = litesata_match,
	},
};

/* Build-into-main: expose init/exit rather than being its own module */
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
