// SPDX-License-Identifier: GPL-2.0
/*
 * LiteX LiteSATA block driver
 *
 * Copyright (c) 2022 Gabriel Somlo <gsomlo@gmail.com>
 */

/* FIXME: create devicetree documentation entry:

Example DTS node (adjust with your own addresses from csr.csv):
	...
	soc {
		...
                litesata0: litesata@12003000 {
                        compatible = "litex,litesata";
                        reg = <0x12003000 0x100>,
                                <0x12004800 0x100>,
                                <0x12005000 0x100>,
                                <0x12004000 0x100>,
                                <0x12003800 0x100>;
                        reg-names = "ident", "phy", "reader", "writer", "irq";
                        // "reader" == "sector2mem"; "writer" == "mem2sector"
                        interrupt-parent = <&L1>;
                        interrupts = <4>;
                };
		...

	}
	...

*/

#include <linux/bits.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/litex.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

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

struct litesata_dev {
	struct device *dev;

	struct mutex lock;

	void __iomem *lsident;
	void __iomem *lsphy;
	void __iomem *lsreader;
	void __iomem *lswriter;
	void __iomem *lsirq;

	struct completion dma_done;
	int irq;
};

/* caller must hold lbd->lock to prevent interleaving transfers */
static int litesata_do_dma(struct litesata_dev *lbd, void __iomem *regs,
			   dma_addr_t dma, sector_t sector, unsigned int count)
{
	int irq = lbd->irq;

	if (irq)
		reinit_completion(&lbd->dma_done);

	/* NOTE: do *not* start by writing 0 to LITESATA_DMA_STRT!!! */
	litex_write64(regs + LITESATA_DMA_SECT, sector);
	litex_write16(regs + LITESATA_DMA_NSEC, count);
	litex_write64(regs + LITESATA_DMA_ADDR, dma);
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
	struct device *dev = lbd->dev;
	dma_addr_t dma;
	void __iomem *regs;
	int err;
	enum dma_data_direction dir;
	unsigned int count;

	if (op_is_write(op)) {
		dir = DMA_TO_DEVICE;
		regs = lbd->lswriter;
	} else {
		dir = DMA_FROM_DEVICE;
		regs = lbd->lsreader;
	}

	dma = dma_map_bvec(dev, bv, dir, 0);
	err = dma_mapping_error(dev, dma);
	if (err)
		return err;

	count = bv->bv_len >> SECTOR_SHIFT;
	scoped_guard(mutex, &lbd->lock)
		err = litesata_do_dma(lbd, regs, dma, sector, count);
	if (err)
		return err;

	dma_unmap_page(dev, dma, bv->bv_len, dir);
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

	lbd->lsirq = devm_platform_ioremap_resource_byname(pdev, "irq");
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
	struct queue_limits lim = {
		.physical_block_size = SECTOR_SIZE,
		.logical_block_size  = SECTOR_SIZE,
	};
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

	lbd->lsident = devm_platform_ioremap_resource_byname(pdev, "ident");
	if (IS_ERR(lbd->lsident))
		return PTR_ERR(lbd->lsident);

	lbd->lsphy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(lbd->lsphy))
		return PTR_ERR(lbd->lsphy);

	lbd->lsreader = devm_platform_ioremap_resource_byname(pdev, "reader");
	if (IS_ERR(lbd->lsreader))
		return PTR_ERR(lbd->lsreader);

	lbd->lswriter = devm_platform_ioremap_resource_byname(pdev, "writer");
	if (IS_ERR(lbd->lswriter))
		return PTR_ERR(lbd->lswriter);

	/* Initialize disk, get model and size */
	err = litesata_init_ident(lbd, &size);
	if (err)
		return err;

	/* Configure interrupts */
	err = litesata_irq_init(pdev, lbd);
	if (err)
		return err;

	gendisk = blk_alloc_disk(&lim, NUMA_NO_NODE);
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

static struct platform_driver litesata_driver = {
	.probe = litesata_probe,
	.driver = {
		.name = "litesata",
		.of_match_table = litesata_match,
	},
};
module_platform_driver(litesata_driver);

MODULE_DESCRIPTION("LiteX LiteSATA block driver");
MODULE_AUTHOR("Gabriel Somlo <gsomlo@gmail.com>");
MODULE_LICENSE("GPL v2");
