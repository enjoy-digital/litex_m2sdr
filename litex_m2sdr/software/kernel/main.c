/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Linux Driver.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

/* ----------------------------------------------------------------------------------------------- */
/*                                        Includes                                                 */
/* ----------------------------------------------------------------------------------------------- */

/* Linux Includes */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/log2.h>
#include <linux/poll.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/version.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#if defined(__arm__) || defined(__aarch64__)
#include <linux/dma-direct.h>
#endif
#include <linux/ptp_clock_kernel.h>

/* LiteX Includes */
#include "litepcie.h"
#include "csr.h"
#include "config.h"
#include "soc.h"
#include "mem.h"

/* ----------------------------------------------------------------------------------------------- */
/*                                       Defines                                                   */
/* ----------------------------------------------------------------------------------------------- */

#define LITEPCIE_NAME "m2sdr"
#define LITEPCIE_MINOR_COUNT 32

#ifndef CSR_BASE
#define CSR_BASE 0x00000000
#endif

/* Debug defines (uncomment for enabling) */
//#define DEBUG_CSR
//#define DEBUG_MSI
//#define DEBUG_POLL
//#define DEBUG_READ
//#define DEBUG_WRITE

/* External functions for LiteUART */
extern int liteuart_init(void);
extern void liteuart_exit(void);

/* External functions for LiteSATA */
extern int litesata_init(void);
extern void litesata_exit(void);

/* LiteSATA MSI notify hooks */
extern void litesata_msi_signal_reader(void);
extern void litesata_msi_signal_writer(void);

/* -----------------------------------------------------------------------------------------------*/
/*                                 Structs and Definitions                                        */
/* -----------------------------------------------------------------------------------------------*/

struct litepcie_dma_chan {
	uint32_t base;
	uint32_t writer_interrupt;
	uint32_t reader_interrupt;
	dma_addr_t reader_handle[DMA_BUFFER_COUNT];
	dma_addr_t writer_handle[DMA_BUFFER_COUNT];
	uint32_t *reader_addr[DMA_BUFFER_COUNT];
	uint32_t *writer_addr[DMA_BUFFER_COUNT];
	int64_t reader_hw_count;
	int64_t reader_hw_count_last;
	int64_t reader_sw_count;
	int64_t writer_hw_count;
	int64_t writer_hw_count_last;
	int64_t writer_sw_count;
	uint8_t writer_enable;
	uint8_t reader_enable;
	uint8_t writer_lock;
	uint8_t reader_lock;
};

struct litepcie_chan {
	struct litepcie_device *litepcie_dev;
	struct litepcie_dma_chan dma;
	struct cdev cdev;
	uint32_t block_size;
	uint32_t core_base;
	wait_queue_head_t wait_rd; /* to wait for an ongoing read */
	wait_queue_head_t wait_wr; /* to wait for an ongoing write */

	int index;
	int minor;
};

/* Structure to hold the LitePCIe device information */
struct litepcie_device {
	struct pci_dev *dev;                          /* PCI device */
	struct platform_device *uart;                 /* UART platform device */
	struct platform_device *sata;                 /* SATA platform device */
	resource_size_t bar0_size;                    /* Size of BAR0 */
	phys_addr_t bar0_phys_addr;                   /* Physical address of BAR0 */
	uint8_t *bar0_addr;                           /* Virtual address of BAR0 */
	struct litepcie_chan chan[DMA_CHANNEL_COUNT]; /* DMA channel information */
	spinlock_t lock;                              /* Spinlock for synchronization */
	int minor_base;                               /* Base minor number for the device */
	int irqs;                                     /* Number of IRQs */
	int channels;                                 /* Number of DMA channels */

	/* PTP/PTM */
	spinlock_t tmreg_lock;
	struct ptp_clock *litepcie_ptp_clock;
	struct system_time_snapshot snapshot;
	struct ptp_clock_info ptp_caps;
	u64 t1_prev;
	u64 t4_prev;
};

struct litepcie_chan_priv {
	struct litepcie_chan *chan;
	bool reader;
	bool writer;
};

static int litepcie_major;
static int litepcie_minor_idx;
static struct class *litepcie_class;
static dev_t litepcie_dev_t;

/* ----------------------------------------------------------------------------------------------- */
/*                               LitePCIe CSR Access                                               */
/* ----------------------------------------------------------------------------------------------- */

/* Function to read a 32-bit value from a LitePCIe device register */
static inline uint32_t litepcie_readl(struct litepcie_device *s, uint32_t addr)
{
	uint32_t val;

	val = readl(s->bar0_addr + addr - CSR_BASE);
#ifdef DEBUG_CSR
	dev_dbg(&s->dev->dev, "csr_read: 0x%08x @ 0x%08x", val, addr);
#endif
	return val;
}

/* Function to write a 32-bit value to a LitePCIe device register */
static inline void litepcie_writel(struct litepcie_device *s, uint32_t addr, uint32_t val)
{
#ifdef DEBUG_CSR
	dev_dbg(&s->dev->dev, "csr_write: 0x%08x @ 0x%08x", val, addr);
#endif
	writel(val, s->bar0_addr + addr - CSR_BASE);
}

/* -----------------------------------------------------------------------------------------------*/
/*                                 Capabilities                                                   */
/* -----------------------------------------------------------------------------------------------*/

#ifdef CSR_SATA_PHY_BASE

/* Check if the SoC has SATA capability */
static bool litepcie_soc_has_sata(struct litepcie_device *s)
{
#ifdef CSR_CAPABILITY_FEATURES_ADDR
	u32 features = litepcie_readl(s, CSR_CAPABILITY_FEATURES_ADDR);
	bool sata_enabled =
		(features >> CSR_CAPABILITY_FEATURES_SATA_OFFSET) &
		((1U << CSR_CAPABILITY_FEATURES_SATA_SIZE) - 1);
	return sata_enabled;
#else
	/* Old bitstreams may not include the capability block; keep old behavior. */
	return false;
#endif
}

#endif

/* -----------------------------------------------------------------------------------------------*/
/*                               LitePCIe Interrupts                                              */
/* -----------------------------------------------------------------------------------------------*/

/* Function to enable a specific interrupt on a LitePCIe device */
static void litepcie_enable_interrupt(struct litepcie_device *s, int irq_num)
{
	uint32_t v;

	/* Read the current interrupt enable register value */
	v = litepcie_readl(s, CSR_PCIE_MSI_ENABLE_ADDR);

	/* Set the bit corresponding to the given interrupt number */
	v |= (1 << irq_num);

	/* Write the updated value back to the register */
	litepcie_writel(s, CSR_PCIE_MSI_ENABLE_ADDR, v);
}

/* Function to disable a specific interrupt on a LitePCIe device */
static void litepcie_disable_interrupt(struct litepcie_device *s, int irq_num)
{
	uint32_t v;

	/* Read the current interrupt enable register value */
	v = litepcie_readl(s, CSR_PCIE_MSI_ENABLE_ADDR);

	/* Clear the bit corresponding to the given interrupt number */
	v &= ~(1 << irq_num);

	/* Write the updated value back to the register */
	litepcie_writel(s, CSR_PCIE_MSI_ENABLE_ADDR, v);
}

/* -----------------------------------------------------------------------------------------------*/
/*                               LitePCIe DMAs                                                    */
/* -----------------------------------------------------------------------------------------------*/

/* Initialize DMA buffers for all channels */
static int litepcie_dma_init(struct litepcie_device *s)
{

	int i, j;
	struct litepcie_dma_chan *dmachan;

	if (!s)
		return -ENODEV;

	/* For each DMA channel */
	for (i = 0; i < s->channels; i++) {
		dmachan = &s->chan[i].dma;
		/* For each DMA buffer */
		for (j = 0; j < DMA_BUFFER_COUNT; j++) {
			/* Allocate reader buffer */
			dmachan->reader_addr[j] = dmam_alloc_coherent(
				&s->dev->dev,
				DMA_BUFFER_SIZE,
				&dmachan->reader_handle[j],
				GFP_KERNEL);
			/* Allocate writer buffer */
			dmachan->writer_addr[j] = dmam_alloc_coherent(
				&s->dev->dev,
				DMA_BUFFER_SIZE,
				&dmachan->writer_handle[j],
				GFP_KERNEL);
			/* Check allocation success */
			if (!dmachan->writer_addr[j] || !dmachan->reader_addr[j]) {
				dev_err(&s->dev->dev, "Failed to allocate DMA buffers\n");
				return -ENOMEM;
			}
		}
	}

	return 0;
}

/* Start DMA writer for a specific channel */
static void litepcie_dma_writer_start(struct litepcie_device *s, int chan_num)
{
	struct litepcie_dma_chan *dmachan;
	int i;

	dmachan = &s->chan[chan_num].dma;

	/* Fill DMA Writer descriptors */
	litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_ENABLE_OFFSET, 0);
	litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_TABLE_FLUSH_OFFSET, 1);
	litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_TABLE_LOOP_PROG_N_OFFSET, 0);
	for (i = 0; i < DMA_BUFFER_COUNT; i++) {
		/* Fill buffer size + parameters */
		litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_TABLE_VALUE_OFFSET,
#ifndef DMA_BUFFER_ALIGNED
			DMA_LAST_DISABLE |
#endif
			(!(i % DMA_BUFFER_PER_IRQ == 0)) * DMA_IRQ_DISABLE | /* Generate an MSI every n buffers */
			DMA_BUFFER_SIZE);
		/* Fill 32-bit Address LSB */
		litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_TABLE_VALUE_OFFSET + 4, (dmachan->writer_handle[i] >>  0) & 0xffffffff);
		/* Write descriptor (and fill 32-bit Address MSB for 64-bit mode) */
		litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_TABLE_WE_OFFSET, (dmachan->writer_handle[i] >> 32) & 0xffffffff);
	}
	litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_TABLE_LOOP_PROG_N_OFFSET, 1);

	/* Clear counters */
	dmachan->writer_hw_count = 0;
	dmachan->writer_hw_count_last = 0;
	dmachan->writer_sw_count = 0;

	/* Start DMA Writer */
	litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_ENABLE_OFFSET, 1);

	/* Start DMA Synchronizer (RX only) */
	litepcie_writel(s, dmachan->base + PCIE_DMA_SYNCHRONIZER_ENABLE_OFFSET, 0b0);
	litepcie_writel(s, dmachan->base + PCIE_DMA_SYNCHRONIZER_ENABLE_OFFSET, 0b10);
}

/* Stop DMA writer for a specific channel */
static void litepcie_dma_writer_stop(struct litepcie_device *s, int chan_num)
{
	struct litepcie_dma_chan *dmachan;

	dmachan = &s->chan[chan_num].dma;

	/* Flush and stop DMA Writer */
	litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_TABLE_LOOP_PROG_N_OFFSET, 0);
	litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_TABLE_FLUSH_OFFSET, 1);
	udelay(1000);
	litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_ENABLE_OFFSET, 0);
	litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_TABLE_FLUSH_OFFSET, 1);

	/* Disable DMA Synchronizer only if the Reader is not active */
	if (!dmachan->reader_enable) {
		litepcie_writel(s, dmachan->base + PCIE_DMA_SYNCHRONIZER_BYPASS_OFFSET, 0b0);
		litepcie_writel(s, dmachan->base + PCIE_DMA_SYNCHRONIZER_ENABLE_OFFSET, 0b0);
	} else {
		dev_dbg(&s->dev->dev, "DMA Reader is active; skipping disabling synchronizer in writer_stop\n");
	}

	/* Clear counters */
	dmachan->writer_hw_count      = 0;
	dmachan->writer_hw_count_last = 0;
	dmachan->writer_sw_count      = 0;
}

/* Start DMA reader for a specific channel */
static void litepcie_dma_reader_start(struct litepcie_device *s, int chan_num)
{
	struct litepcie_dma_chan *dmachan;
	int i;

	dmachan = &s->chan[chan_num].dma;

	/* Fill DMA Reader descriptors */
	litepcie_writel(s, dmachan->base + PCIE_DMA_READER_ENABLE_OFFSET, 0);
	litepcie_writel(s, dmachan->base + PCIE_DMA_READER_TABLE_FLUSH_OFFSET, 1);
	litepcie_writel(s, dmachan->base + PCIE_DMA_READER_TABLE_LOOP_PROG_N_OFFSET, 0);
	for (i = 0; i < DMA_BUFFER_COUNT; i++) {
		/* Fill buffer size + parameters */
		litepcie_writel(s, dmachan->base + PCIE_DMA_READER_TABLE_VALUE_OFFSET,
#ifndef DMA_BUFFER_ALIGNED
			DMA_LAST_DISABLE |
#endif
			(!(i % DMA_BUFFER_PER_IRQ == 0)) * DMA_IRQ_DISABLE | /* Generate an MSI every n buffers */
			DMA_BUFFER_SIZE);
		/* Fill 32-bit Address LSB */
		litepcie_writel(s, dmachan->base + PCIE_DMA_READER_TABLE_VALUE_OFFSET + 4, (dmachan->reader_handle[i] >>  0) & 0xffffffff);
		/* Write descriptor (and fill 32-bit Address MSB for 64-bit mode) */
		litepcie_writel(s, dmachan->base + PCIE_DMA_READER_TABLE_WE_OFFSET, (dmachan->reader_handle[i] >> 32) & 0xffffffff);
	}
	litepcie_writel(s, dmachan->base + PCIE_DMA_READER_TABLE_LOOP_PROG_N_OFFSET, 1);

	/* Clear counters */
	dmachan->reader_hw_count      = 0;
	dmachan->reader_hw_count_last = 0;
	dmachan->reader_sw_count      = 0;

	/* Start DMA reader */
	litepcie_writel(s, dmachan->base + PCIE_DMA_READER_ENABLE_OFFSET, 1);

	/* Start DMA Synchronizer (TX & RX) */
	litepcie_writel(s, dmachan->base + PCIE_DMA_SYNCHRONIZER_ENABLE_OFFSET, 0b0);
	litepcie_writel(s, dmachan->base + PCIE_DMA_SYNCHRONIZER_ENABLE_OFFSET, 0b01);
}

/* Stop DMA reader for a specific channel */
static void litepcie_dma_reader_stop(struct litepcie_device *s, int chan_num)
{
	struct litepcie_dma_chan *dmachan;

	dmachan = &s->chan[chan_num].dma;

	/* Flush and stop DMA reader */
	litepcie_writel(s, dmachan->base + PCIE_DMA_READER_TABLE_LOOP_PROG_N_OFFSET, 0);
	litepcie_writel(s, dmachan->base + PCIE_DMA_READER_TABLE_FLUSH_OFFSET, 1);
	udelay(1000);
	litepcie_writel(s, dmachan->base + PCIE_DMA_READER_ENABLE_OFFSET, 0);
	litepcie_writel(s, dmachan->base + PCIE_DMA_READER_TABLE_FLUSH_OFFSET, 1);

	/* Disable DMA Synchronizer only if the Writer is not active */
	if (!dmachan->writer_enable) {
		litepcie_writel(s, dmachan->base + PCIE_DMA_SYNCHRONIZER_BYPASS_OFFSET, 0b0);
		litepcie_writel(s, dmachan->base + PCIE_DMA_SYNCHRONIZER_ENABLE_OFFSET, 0b0);
	} else {
		dev_dbg(&s->dev->dev, "DMA Writer is active; skipping disabling synchronizer in reader_stop\n");
	}

	/* Clear counters */
	dmachan->reader_hw_count      = 0;
	dmachan->reader_hw_count_last = 0;
	dmachan->reader_sw_count      = 0;
}

/* Stop all DMA channels */
static void litepcie_stop_dma(struct litepcie_device *s)
{
	struct litepcie_dma_chan *dmachan;
	int i;

	for (i = 0; i < s->channels; i++) {
		dmachan = &s->chan[i].dma;
		litepcie_writel(s, dmachan->base + PCIE_DMA_WRITER_ENABLE_OFFSET, 0b0);
		litepcie_writel(s, dmachan->base + PCIE_DMA_READER_ENABLE_OFFSET, 0b0);
	}
}

/* Interrupt handler for LitePCIe events */
static irqreturn_t litepcie_interrupt(int irq, void *data)
{
	struct litepcie_device *s = (struct litepcie_device *) data;
	struct litepcie_chan *chan;
	uint32_t loop_status;
	uint32_t clear_mask, irq_vector, irq_enable;
	int i;

	/* Determine IRQ vector */
#ifdef CSR_PCIE_MSI_CLEAR_ADDR
	/* Single MSI */
	irq_vector = litepcie_readl(s, CSR_PCIE_MSI_VECTOR_ADDR);
	irq_enable = litepcie_readl(s, CSR_PCIE_MSI_ENABLE_ADDR);
#else
	/* MSI MultiVector / MSI-X */
	irq_vector = 0;
	for (i = 0; i < s->irqs; i++) {
		if (irq == pci_irq_vector(s->dev, i)) {
			irq_vector = (1 << i);
			break;
		}
	}
	irq_enable = litepcie_readl(s, CSR_PCIE_MSI_ENABLE_ADDR);
#endif

#ifdef DEBUG_MSI
	dev_dbg(&s->dev->dev, "MSI: 0x%x 0x%x\n", irq_vector, irq_enable);
#endif

	irq_vector &= irq_enable;
	clear_mask = 0;

	/* Handle DMA interrupts */
	for (i = 0; i < s->channels; i++) {
		chan = &s->chan[i];
		/* DMA reader interrupt handling */
		if (irq_vector & (1 << chan->dma.reader_interrupt)) {
			loop_status = litepcie_readl(s, chan->dma.base +
				PCIE_DMA_READER_TABLE_LOOP_STATUS_OFFSET);
			chan->dma.reader_hw_count &= ((~(DMA_BUFFER_COUNT - 1) << 16) & 0xffffffffffff0000);
			chan->dma.reader_hw_count |= (loop_status >> 16) * DMA_BUFFER_COUNT + (loop_status & 0xffff);
			if (chan->dma.reader_hw_count_last > chan->dma.reader_hw_count)
				chan->dma.reader_hw_count += (1 << (ilog2(DMA_BUFFER_COUNT) + 16));
			chan->dma.reader_hw_count_last = chan->dma.reader_hw_count;
#ifdef DEBUG_MSI
			dev_dbg(&s->dev->dev, "MSI DMA%d Reader buf: %lld\n", i,
				chan->dma.reader_hw_count);
#endif
			wake_up_interruptible(&chan->wait_wr);
			clear_mask |= (1 << chan->dma.reader_interrupt);
		}
		/* DMA writer interrupt handling */
		if (irq_vector & (1 << chan->dma.writer_interrupt)) {
			loop_status = litepcie_readl(s, chan->dma.base +
				PCIE_DMA_WRITER_TABLE_LOOP_STATUS_OFFSET);
			chan->dma.writer_hw_count &= ((~(DMA_BUFFER_COUNT - 1) << 16) & 0xffffffffffff0000);
			chan->dma.writer_hw_count |= (loop_status >> 16) * DMA_BUFFER_COUNT + (loop_status & 0xffff);
			if (chan->dma.writer_hw_count_last > chan->dma.writer_hw_count)
				chan->dma.writer_hw_count += (1 << (ilog2(DMA_BUFFER_COUNT) + 16));
			chan->dma.writer_hw_count_last = chan->dma.writer_hw_count;
#ifdef DEBUG_MSI
			dev_dbg(&s->dev->dev, "MSI DMA%d Writer buf: %lld\n", i,
				chan->dma.writer_hw_count);
#endif
			wake_up_interruptible(&chan->wait_rd);
			clear_mask |= (1 << chan->dma.writer_interrupt);
		}
	}

	/* Handle SATA interrupts */
#ifdef SATA_SECTOR2MEM_INTERRUPT
	if (irq_vector & (1 << SATA_SECTOR2MEM_INTERRUPT)) {
		clear_mask |= (1 << SATA_SECTOR2MEM_INTERRUPT);
	}
#endif

#ifdef SATA_MEM2SECTOR_INTERRUPT
	if (irq_vector & (1 << SATA_MEM2SECTOR_INTERRUPT)) {
		clear_mask |= (1 << SATA_MEM2SECTOR_INTERRUPT);
	}
#endif

	/* Clear ALL interrupts */
#ifdef CSR_PCIE_MSI_CLEAR_ADDR
	litepcie_writel(s, CSR_PCIE_MSI_CLEAR_ADDR, clear_mask);
#endif

	/* Signal SATA completions */
#ifdef SATA_SECTOR2MEM_INTERRUPT
	if (irq_vector & (1 << SATA_SECTOR2MEM_INTERRUPT))
		litesata_msi_signal_reader();
#endif
#ifdef SATA_MEM2SECTOR_INTERRUPT
	if (irq_vector & (1 << SATA_MEM2SECTOR_INTERRUPT))
		litesata_msi_signal_writer();
#endif

	return IRQ_HANDLED;
}

/* ----------------------------------------------------------------------------------------------- */
/*                                 Character Device Operations                                     */
/* ----------------------------------------------------------------------------------------------- */

static int litepcie_open(struct inode *inode, struct file *file)
{
	struct litepcie_chan *chan = container_of(inode->i_cdev, struct litepcie_chan, cdev);
	struct litepcie_chan_priv *chan_priv = kzalloc(sizeof(*chan_priv), GFP_KERNEL);

	if (!chan_priv)
		return -ENOMEM;

	chan_priv->chan    = chan;
	file->private_data = chan_priv;

	if (chan->dma.reader_enable == 0) { /* Clear only if disabled */
		chan->dma.reader_hw_count      = 0;
		chan->dma.reader_hw_count_last = 0;
		chan->dma.reader_sw_count      = 0;
	}

	if (chan->dma.writer_enable == 0) { /* Clear only if disabled */
		chan->dma.writer_hw_count      = 0;
		chan->dma.writer_hw_count_last = 0;
		chan->dma.writer_sw_count      = 0;
	}

	return 0;
}

static int litepcie_release(struct inode *inode, struct file *file)
{
	struct litepcie_chan_priv *chan_priv = file->private_data;
	struct litepcie_chan *chan = chan_priv->chan;

	if (chan_priv->reader) {
		/* Disable interrupt */
		litepcie_disable_interrupt(chan->litepcie_dev, chan->dma.reader_interrupt);
		/* Disable DMA */
		litepcie_dma_reader_stop(chan->litepcie_dev, chan->index);
		chan->dma.reader_lock   = 0;
		chan->dma.reader_enable = 0;
	}

	if (chan_priv->writer) {
		/* Disable interrupt */
		litepcie_disable_interrupt(chan->litepcie_dev, chan->dma.writer_interrupt);
		/* Disable DMA */
		litepcie_dma_writer_stop(chan->litepcie_dev, chan->index);
		chan->dma.writer_lock   = 0;
		chan->dma.writer_enable = 0;
	}

	kfree(chan_priv);

	return 0;
}

static ssize_t litepcie_read(struct file *file, char __user *data, size_t size, loff_t *offset)
{
	size_t len;
	int i, ret;
	int overflows;

	struct litepcie_chan_priv *chan_priv = file->private_data;
	struct litepcie_chan      *chan      = chan_priv->chan;
	struct litepcie_device    *s         = chan->litepcie_dev;

	if (file->f_flags & O_NONBLOCK) {
		if (chan->dma.writer_hw_count == chan->dma.writer_sw_count)
			ret = -EAGAIN;
		else
			ret = 0;
	} else {
		ret = wait_event_interruptible(chan->wait_rd,
						   (chan->dma.writer_hw_count - chan->dma.writer_sw_count) > 0);
	}

	if (ret < 0)
		return ret;

	i = 0;
	overflows = 0;
	len = size;
	while (len >= DMA_BUFFER_SIZE) {
		if ((chan->dma.writer_hw_count - chan->dma.writer_sw_count) > 0) {
			if ((chan->dma.writer_hw_count - chan->dma.writer_sw_count) > DMA_BUFFER_COUNT/2) {
				overflows++;
			} else {
				ret = copy_to_user(data + (chan->block_size * i),
						   chan->dma.writer_addr[chan->dma.writer_sw_count % DMA_BUFFER_COUNT],
						   DMA_BUFFER_SIZE);
				if (ret)
					return -EFAULT;
			}
			len -= DMA_BUFFER_SIZE;
			chan->dma.writer_sw_count += 1;
			i++;
		} else {
			break;
		}
	}

	if (overflows)
		dev_err(&s->dev->dev, "Reading too late, %d buffers lost\n", overflows);

#ifdef DEBUG_READ
	dev_dbg(&s->dev->dev, "read: read %ld bytes out of %ld\n", size - len, size);
#endif

	return size - len;
}

static ssize_t litepcie_write(struct file *file, const char __user *data, size_t size, loff_t *offset)
{
	size_t len;
	int i, ret;
	int underflows;

	struct litepcie_chan_priv *chan_priv = file->private_data;
	struct litepcie_chan      *chan      = chan_priv->chan;
	struct litepcie_device    *s         = chan->litepcie_dev;

	if (file->f_flags & O_NONBLOCK) {
		if (chan->dma.reader_hw_count == chan->dma.reader_sw_count)
			ret = -EAGAIN;
		else
			ret = 0;
	} else {
		ret = wait_event_interruptible(chan->wait_wr,
						   (chan->dma.reader_sw_count - chan->dma.reader_hw_count) < DMA_BUFFER_COUNT/2);
	}

	if (ret < 0)
		return ret;

	i          = 0;
	underflows = 0;
	len        = size;
	while (len >= DMA_BUFFER_SIZE) {
		if ((chan->dma.reader_sw_count - chan->dma.reader_hw_count) < DMA_BUFFER_COUNT/2) {
			if ((chan->dma.reader_sw_count - chan->dma.reader_hw_count) < 0) {
				underflows++;
			} else {
				ret = copy_from_user(chan->dma.reader_addr[chan->dma.reader_sw_count % DMA_BUFFER_COUNT],
							 data + (chan->block_size * i), DMA_BUFFER_SIZE);
				if (ret)
					return -EFAULT;
			}
			len -= DMA_BUFFER_SIZE;
			chan->dma.reader_sw_count += 1;
			i++;
		} else {
			break;
		}
	}

	if (underflows)
		dev_err(&s->dev->dev, "Writing too late, %d buffers lost\n", underflows);

#ifdef DEBUG_WRITE
	dev_dbg(&s->dev->dev, "write: write %ld bytes out of %ld\n", size - len, size);
#endif

	return size - len;
}

/* Memory map DMA buffers for user space access */
static int litepcie_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct litepcie_chan_priv *chan_priv = file->private_data;
	struct litepcie_chan *chan = chan_priv->chan;
	struct litepcie_device *s = chan->litepcie_dev;
#if defined(__arm__) || defined(__aarch64__)
	unsigned long pfn;
#else
	int ret;
#endif
	int is_tx, i;

	if (vma->vm_end - vma->vm_start != DMA_BUFFER_TOTAL_SIZE)
		return -EINVAL;

	if (vma->vm_pgoff == 0)
		is_tx = 1;
	else if (vma->vm_pgoff == (DMA_BUFFER_TOTAL_SIZE >> PAGE_SHIFT))
		is_tx = 0;
	else
		return -EINVAL;

	for (i = 0; i < DMA_BUFFER_COUNT; i++) {
#if defined(__arm__) || defined(__aarch64__)
		void *va;
		if (i == 0)
			dev_info(&s->dev->dev, "Using ARM/AArch64 DMA buffer handling");
		if (is_tx)
			va = phys_to_virt(dma_to_phys(&s->dev->dev, chan->dma.reader_handle[i]));
		else
			va = phys_to_virt(dma_to_phys(&s->dev->dev, chan->dma.writer_handle[i]));
		pfn = page_to_pfn(virt_to_page(va));
		/*
		 * Note: the memory is cached, so the user must explicitly
		 * flush the CPU caches on architectures which require it.
		 */
		if (remap_pfn_range(vma, vma->vm_start + i * DMA_BUFFER_SIZE, pfn,
					DMA_BUFFER_SIZE, vma->vm_page_prot)) {
			dev_err(&s->dev->dev, "mmap remap_pfn_range failed\n");
			return -EAGAIN;
		}
#else
		void *cpu_addr;
		dma_addr_t dma_handle;
		struct vm_area_struct sub_vma = *vma; /* Map one chunk at a time */

		if (i == 0)
			dev_info(&s->dev->dev, "Using non-ARM DMA buffer handling");

		if (is_tx) {
			cpu_addr   = chan->dma.reader_addr[i];
			dma_handle = chan->dma.reader_handle[i];
		} else {
			cpu_addr   = chan->dma.writer_addr[i];
			dma_handle = chan->dma.writer_handle[i];
		}

		sub_vma.vm_start = vma->vm_start + (i * DMA_BUFFER_SIZE);
		sub_vma.vm_end   = sub_vma.vm_start + DMA_BUFFER_SIZE;
		sub_vma.vm_pgoff = 0; /* Required: offset inside each coherent buffer */

		ret = dma_mmap_coherent(&s->dev->dev, &sub_vma,
					cpu_addr, dma_handle, DMA_BUFFER_SIZE);
		if (ret) {
			dev_err(&s->dev->dev,
				"dma_mmap_coherent failed for buffer %d (ret=%d)\n", i, ret);
			return ret;
		}
#endif
	}

	return 0;
}

/* Poll for DMA buffer availability */
static unsigned int litepcie_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;

	struct litepcie_chan_priv *chan_priv = file->private_data;
	struct litepcie_chan      *chan      = chan_priv->chan;
#ifdef DEBUG_POLL
	struct litepcie_device    *s         = chan->litepcie_dev;
#endif

	poll_wait(file, &chan->wait_rd, wait);
	poll_wait(file, &chan->wait_wr, wait);

#ifdef DEBUG_POLL
	dev_dbg(&s->dev->dev, "poll: writer hw_count: %10lld / sw_count %10lld\n",
		chan->dma.writer_hw_count, chan->dma.writer_sw_count);
	dev_dbg(&s->dev->dev, "poll: reader hw_count: %10lld / sw_count %10lld\n",
		chan->dma.reader_hw_count, chan->dma.reader_sw_count);
#endif

	if ((chan->dma.writer_hw_count - chan->dma.writer_sw_count) > 2)
		mask |= POLLIN | POLLRDNORM;

	if ((chan->dma.reader_sw_count - chan->dma.reader_hw_count) < DMA_BUFFER_COUNT/2)
		mask |= POLLOUT | POLLWRNORM;

	return mask;
}

/* IOCTL handling for DMA control and registers */
static long litepcie_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	long ret = 0;

	struct litepcie_chan_priv *chan_priv = file->private_data;
	struct litepcie_chan      *chan      = chan_priv->chan;
	struct litepcie_device    *dev       = chan->litepcie_dev;

	switch (cmd) {
	case LITEPCIE_IOCTL_REG:
	{
		struct litepcie_ioctl_reg m;

		if (copy_from_user(&m, (void *)arg, sizeof(m))) {
			ret = -EFAULT;
			break;
		}
		if (m.is_write)
			litepcie_writel(dev, m.addr, m.val);
		else
			m.val = litepcie_readl(dev, m.addr);

		if (copy_to_user((void *)arg, &m, sizeof(m))) {
			ret = -EFAULT;
			break;
		}
	}
	break;
	case LITEPCIE_IOCTL_DMA:
	{
		struct litepcie_ioctl_dma m;

		if (copy_from_user(&m, (void *)arg, sizeof(m))) {
			ret = -EFAULT;
			break;
		}

		/* Loopback control */
		litepcie_writel(chan->litepcie_dev, chan->dma.base + PCIE_DMA_LOOPBACK_ENABLE_OFFSET, m.loopback_enable);
	}
	break;
	case LITEPCIE_IOCTL_DMA_WRITER:
	{
		struct litepcie_ioctl_dma_writer m;

		if (copy_from_user(&m, (void *)arg, sizeof(m))) {
			ret = -EFAULT;
			break;
		}

		if (m.enable != chan->dma.writer_enable) {
			/* Enable / disable DMA */
			if (m.enable) {
				litepcie_dma_writer_start(chan->litepcie_dev, chan->index);
				litepcie_enable_interrupt(chan->litepcie_dev, chan->dma.writer_interrupt);
			} else {
				litepcie_disable_interrupt(chan->litepcie_dev, chan->dma.writer_interrupt);
				litepcie_dma_writer_stop(chan->litepcie_dev, chan->index);
			}
		}

		chan->dma.writer_enable = m.enable;

		m.hw_count = chan->dma.writer_hw_count;
		m.sw_count = chan->dma.writer_sw_count;

		if (copy_to_user((void *)arg, &m, sizeof(m))) {
			ret = -EFAULT;
			break;
		}
	}
	break;
	case LITEPCIE_IOCTL_DMA_READER:
	{
		struct litepcie_ioctl_dma_reader m;

		if (copy_from_user(&m, (void *)arg, sizeof(m))) {
			ret = -EFAULT;
			break;
		}

		if (m.enable != chan->dma.reader_enable) {
			/* Enable / disable DMA */
			if (m.enable) {
				litepcie_dma_reader_start(chan->litepcie_dev, chan->index);
				litepcie_enable_interrupt(chan->litepcie_dev, chan->dma.reader_interrupt);
			} else {
				litepcie_disable_interrupt(chan->litepcie_dev, chan->dma.reader_interrupt);
				litepcie_dma_reader_stop(chan->litepcie_dev, chan->index);
			}
		}

		chan->dma.reader_enable = m.enable;

		m.hw_count = chan->dma.reader_hw_count;
		m.sw_count = chan->dma.reader_sw_count;

		if (copy_to_user((void *)arg, &m, sizeof(m))) {
			ret = -EFAULT;
			break;
		}
	}
	break;
	case LITEPCIE_IOCTL_MMAP_DMA_INFO:
	{
		struct litepcie_ioctl_mmap_dma_info m;

		m.dma_tx_buf_offset = 0;
		m.dma_tx_buf_size   = DMA_BUFFER_SIZE;
		m.dma_tx_buf_count  = DMA_BUFFER_COUNT;

		m.dma_rx_buf_offset = DMA_BUFFER_TOTAL_SIZE;
		m.dma_rx_buf_size   = DMA_BUFFER_SIZE;
		m.dma_rx_buf_count  = DMA_BUFFER_COUNT;

		if (copy_to_user((void *)arg, &m, sizeof(m))) {
			ret = -EFAULT;
			break;
		}
	}
	break;
	case LITEPCIE_IOCTL_MMAP_DMA_WRITER_UPDATE:
	{
		struct litepcie_ioctl_mmap_dma_update m;

		if (copy_from_user(&m, (void *)arg, sizeof(m))) {
			ret = -EFAULT;
			break;
		}

		chan->dma.writer_sw_count = m.sw_count;
	}
	break;
	case LITEPCIE_IOCTL_MMAP_DMA_READER_UPDATE:
	{
		struct litepcie_ioctl_mmap_dma_update m;

		if (copy_from_user(&m, (void *)arg, sizeof(m))) {
			ret = -EFAULT;
			break;
		}

		chan->dma.reader_sw_count = m.sw_count;
	}
	break;
	case LITEPCIE_IOCTL_LOCK:
	{
		struct litepcie_ioctl_lock m;

		if (copy_from_user(&m, (void *)arg, sizeof(m))) {
			ret = -EFAULT;
			break;
		}

		m.dma_reader_status = 1;
		if (m.dma_reader_request) {
			if (chan->dma.reader_lock) {
				m.dma_reader_status = 0;
			} else {
				chan->dma.reader_lock = 1;
				chan_priv->reader = 1;
			}
		}
		if (m.dma_reader_release) {
			chan->dma.reader_lock = 0;
			chan_priv->reader = 0;
		}

		m.dma_writer_status = 1;
		if (m.dma_writer_request) {
			if (chan->dma.writer_lock) {
				m.dma_writer_status = 0;
			} else {
				chan->dma.writer_lock = 1;
				chan_priv->writer = 1;
			}
		}
		if (m.dma_writer_release) {
			chan->dma.writer_lock = 0;
			chan_priv->writer = 0;
		}

		if (copy_to_user((void *)arg, &m, sizeof(m))) {
			ret = -EFAULT;
			break;
		}
	}
	break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

static const struct file_operations litepcie_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = litepcie_ioctl,
	.open           = litepcie_open,
	.release        = litepcie_release,
	.read           = litepcie_read,
	.poll           = litepcie_poll,
	.write          = litepcie_write,
	.mmap           = litepcie_mmap,
};

/* ----------------------------------------------------------------------------------------------- */
/*                                    Device Management                                            */
/* ----------------------------------------------------------------------------------------------- */

/* Allocate character devices for DMA channels */
static int litepcie_alloc_chdev(struct litepcie_device *s)
{
	int i, j;
	int ret;
	int index;

	index         = litepcie_minor_idx;
	s->minor_base = litepcie_minor_idx;
	for (i = 0; i < s->channels; i++) {
		cdev_init(&s->chan[i].cdev, &litepcie_fops);
		ret = cdev_add(&s->chan[i].cdev, MKDEV(litepcie_major, index), 1);
		if (ret < 0) {
			dev_err(&s->dev->dev, "Failed to allocate cdev\n");
			goto fail_alloc;
		}
		index++;
	}

	index = litepcie_minor_idx;
	for (i = 0; i < s->channels; i++) {
		dev_info(&s->dev->dev, "Creating /dev/m2sdr%d\n", index);
		if (!device_create(litepcie_class, NULL, MKDEV(litepcie_major, index), NULL, "m2sdr%d", index)) {
			ret = -EINVAL;
			dev_err(&s->dev->dev, "Failed to create device\n");
			goto fail_create;
		}
		index++;
	}

	litepcie_minor_idx = index;
	return 0;

fail_create:
	index = litepcie_minor_idx;
	for (j = 0; j < i; j++)
		device_destroy(litepcie_class, MKDEV(litepcie_major, index++));

fail_alloc:
	for (i = 0; i < s->channels; i++)
		cdev_del(&s->chan[i].cdev);

	return ret;
}

/* Free character devices for DMA channels */
static void litepcie_free_chdev(struct litepcie_device *s)
{
	int i;

	for (i = 0; i < s->channels; i++) {
		device_destroy(litepcie_class, MKDEV(litepcie_major, s->minor_base + i));
		cdev_del(&s->chan[i].cdev);
	}
}

/* -----------------------------------------------------------------------------------------------*/
/*                                       PTP/PTM                                                  */
/* -----------------------------------------------------------------------------------------------*/

#ifdef CSR_PTM_REQUESTER_BASE

/* Time Control Register Addresses */
/* Write Time Low and High Addresses */
#define TIME_CONTROL_WRITE_TIME_L (CSR_TIME_GEN_WRITE_TIME_ADDR + (4))
#define TIME_CONTROL_WRITE_TIME_H (CSR_TIME_GEN_WRITE_TIME_ADDR + (0))

/* Read Time Low and High Addresses */
#define TIME_CONTROL_READ_TIME_L  (CSR_TIME_GEN_READ_TIME_ADDR + (4))
#define TIME_CONTROL_READ_TIME_H  (CSR_TIME_GEN_READ_TIME_ADDR + (0))

/* Time Control Register Flags */
#define TIME_CONTROL_ENABLE       (1 << CSR_TIME_GEN_CONTROL_ENABLE_OFFSET)
#define TIME_CONTROL_READ         (1 << CSR_TIME_GEN_CONTROL_READ_OFFSET)
#define TIME_CONTROL_WRITE        (1 << CSR_TIME_GEN_CONTROL_WRITE_OFFSET)
#define TIME_CONTROL_SYNC_ENABLE  (1 << CSR_TIME_GEN_CONTROL_SYNC_ENABLE_OFFSET)

/* PTM Offset in Nanoseconds (Adjust based on calibration) */
#define PTM_OFFSET_NS (-500) /* FIXME: Adjust based on calibration */

/* PTM Control Register Flags */
#define PTM_CONTROL_ENABLE  (1 << CSR_PTM_REQUESTER_CONTROL_ENABLE_OFFSET)
#define PTM_CONTROL_TRIGGER (1 << CSR_PTM_REQUESTER_CONTROL_TRIGGER_OFFSET)

/* PTM Status Register Flags */
#define PTM_STATUS_VALID    (1 << CSR_PTM_REQUESTER_STATUS_VALID_OFFSET)
#define PTM_STATUS_BUSY     (1 << CSR_PTM_REQUESTER_STATUS_BUSY_OFFSET)

/* PTM Time Registers */
/* T1 Time Low and High (Local Request Timestamp at Requester) */
#define PTM_T1_TIME_L       (CSR_PTM_REQUESTER_T1_TIME_ADDR + (4))
#define PTM_T1_TIME_H       (CSR_PTM_REQUESTER_T1_TIME_ADDR + (0))

/* T2 Time Low and High (Master Response Timestamp at Responder) */
#define PTM_MASTER_TIME_L   (CSR_PTM_REQUESTER_MASTER_TIME_ADDR + (4))
#define PTM_MASTER_TIME_H   (CSR_PTM_REQUESTER_MASTER_TIME_ADDR + (0))

/* T4 Time Low and High (Local Response Receipt Timestamp at Requester) */
#define PTM_T4_TIME_L       (CSR_PTM_REQUESTER_T4_TIME_ADDR + (4))
#define PTM_T4_TIME_H       (CSR_PTM_REQUESTER_T4_TIME_ADDR + (0))

/* Read a 64-bit value from two 32-bit registers */
static u64 litepcie_read64(struct litepcie_device *dev, uint32_t addr)
{
	/* Read the high and low 32-bit parts and combine them */
	return (((u64) litepcie_readl(dev, addr) << 32) |
		(litepcie_readl(dev, addr + 4) & 0xffffffff));
}

/* Read the current time from the device's time generator */
static int litepcie_read_time(struct litepcie_device *dev, struct timespec64 *ts)
{
	struct timespec64 rd_ts;
	s64 value;

	/* Issue a read command to the time generator */
	litepcie_writel(dev, CSR_TIME_GEN_CONTROL_ADDR,
			(TIME_CONTROL_ENABLE | TIME_CONTROL_READ | TIME_CONTROL_SYNC_ENABLE));

	/* Read the high and low parts of the time value */
	value = (((s64) litepcie_readl(dev, TIME_CONTROL_READ_TIME_H) << 32) |
		(litepcie_readl(dev, TIME_CONTROL_READ_TIME_L) & 0xffffffff));

	/* Adjust the value by subtracting PTM offset */
	value = value - PTM_OFFSET_NS;

	/* Convert the value to timespec64 format */
	rd_ts = ns_to_timespec64(value);
	ts->tv_nsec = rd_ts.tv_nsec;
	ts->tv_sec = rd_ts.tv_sec;

	return 0;
}

/* Write a new time to the device's time generator */
static int litepcie_write_time(struct litepcie_device *dev, const struct timespec64 *ts)
{
	s64 value = timespec64_to_ns(ts);

	/* Adjust the value by adding PTM offset */
	value = value + PTM_OFFSET_NS;

	/* Write the low and high parts of the time value */
	litepcie_writel(dev, TIME_CONTROL_WRITE_TIME_L, (value >>  0) & 0xffffffff);
	litepcie_writel(dev, TIME_CONTROL_WRITE_TIME_H, (value >> 32) & 0xffffffff);

	/* Issue a write command to the time generator */
	litepcie_writel(dev, CSR_TIME_GEN_CONTROL_ADDR,
			(TIME_CONTROL_ENABLE | TIME_CONTROL_WRITE | TIME_CONTROL_SYNC_ENABLE));

	return 0;
}

/* PTP clock operation: Get the current time with timestamping */
static int litepcie_ptp_gettimex64(struct ptp_clock_info *ptp,
				   struct timespec64 *ts,
				   struct ptp_system_timestamp *sts)
{
	struct litepcie_device *dev = container_of(ptp, struct litepcie_device,
							   ptp_caps);
	unsigned long flags;

	/* Acquire lock to ensure consistent time reading */
	spin_lock_irqsave(&dev->tmreg_lock, flags);

	/* Capture system timestamps before and after reading device time */
	ptp_read_system_prets(sts);
	litepcie_read_time(dev, ts);
	ptp_read_system_postts(sts);

	/* Release lock */
	spin_unlock_irqrestore(&dev->tmreg_lock, flags);

	return 0;
}

/* PTP clock operation: Set the device time */
static int litepcie_ptp_settime(struct ptp_clock_info *ptp, const struct timespec64 *ts)
{
	struct litepcie_device *dev = container_of(ptp, struct litepcie_device,
							   ptp_caps);
	unsigned long flags;

	/* Acquire lock to prevent concurrent access */
	spin_lock_irqsave(&dev->tmreg_lock, flags);

	/* Write the new time to the device */
	litepcie_write_time(dev, ts);

	/* Release lock */
	spin_unlock_irqrestore(&dev->tmreg_lock, flags);

	return 0;
}

/* PTP clock operation: Adjust the frequency by scaled parts per million */
static int litepcie_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct litepcie_device *dev = container_of(ptp, struct litepcie_device,
							   ptp_caps);
	unsigned long flags;

	/* Acquire lock to prevent concurrent access */
	spin_lock_irqsave(&dev->tmreg_lock, flags);

	/* Convert scaled_ppm (Q16.16) to signed ppb */
	int64_t ppb = (scaled_ppm * 1000LL) >> 16; /* *1000 / 65536 */

	/* Nominal step = 8 ns << 24 */
	uint32_t add_nom = 0x08000000U;

	/* Compute add_new = add_nom * (1 + ppb / 1e9) */
	uint64_t add_new = div64_u64((uint64_t)add_nom * (1000000000LL + ppb), 1000000000LL);

	/* Update Step with add_new */
	litepcie_writel(dev, CSR_TIME_GEN_TIME_INC_ADDR, (uint32_t) add_new);

	/* Release lock */
	spin_unlock_irqrestore(&dev->tmreg_lock, flags);

	return 0;
}

/* PTP clock operation: Adjust the time by a given delta in nanoseconds */
static int litepcie_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct litepcie_device *dev = container_of(ptp, struct litepcie_device,
							   ptp_caps);
	struct timespec64 now, then = ns_to_timespec64(delta);
	unsigned long flags;

	/* Acquire lock to prevent concurrent access */
	spin_lock_irqsave(&dev->tmreg_lock, flags);

	/* Read current time, add delta, and write back */
	litepcie_read_time(dev, &now);
	now = timespec64_add(now, then);
	litepcie_write_time(dev, &now);

	/* Release lock */
	spin_unlock_irqrestore(&dev->tmreg_lock, flags);
	return 0;
}

/* Obtain a synchronized device and system timestamp */
static int litepcie_phc_get_syncdevicetime(ktime_t *device,
						  struct system_counterval_t *system,
						  void *ctx)
{
	u32 t1_curr_h, t1_curr_l;
	u32 t2_curr_h, t2_curr_l;
	u32 prop_delay;
	u32 reg;
	u64 ptm_master_time;
	struct litepcie_device *dev = ctx;
	u64 t1_curr;
	ktime_t t1, t2_curr;
	int count = 100;

	/* Get a snapshot of system clocks to use as historic value */
	ktime_get_snapshot(&dev->snapshot);

	/* Trigger a PTM request */
	litepcie_writel(dev, CSR_PTM_REQUESTER_CONTROL_ADDR,
		PTM_CONTROL_ENABLE | PTM_CONTROL_TRIGGER);

	/* Wait until PTM request is complete */
	do {
		reg = litepcie_readl(dev, CSR_PTM_REQUESTER_STATUS_ADDR);
		if ((reg & PTM_STATUS_BUSY) == 0)
			break;
	} while (--count);

	if (!count) {
		printk("Exceeded number of tries for PTM cycle\n");
		return -ETIMEDOUT;
	}

	/* Read T1 time (Local Request Timestamp at Requester) */
	t1_curr_l = litepcie_readl(dev, PTM_T1_TIME_L);
	t1_curr_h = litepcie_readl(dev, PTM_T1_TIME_H);
	t1_curr   = ((u64)t1_curr_h << 32 | t1_curr_l);
	t1        = ns_to_ktime(t1_curr);

	/* Read T2 time (Master Response Timestamp at Responder) */
	t2_curr_l = litepcie_readl(dev, PTM_MASTER_TIME_L);
	t2_curr_h = litepcie_readl(dev, PTM_MASTER_TIME_H);
	t2_curr   = ((u64)t2_curr_h << 32 | t2_curr_l);

	/* Read propagation delay (t3 - t2 from downstream port) */
	prop_delay = litepcie_readl(dev, CSR_PTM_REQUESTER_LINK_DELAY_ADDR);

	/* Compute PTM Master Time */
	ptm_master_time = t2_curr - (((dev->t4_prev - dev->t1_prev) - prop_delay) >> 1);

	/* Set device time */
	*device = t1;

#if IS_ENABLED(CONFIG_X86_TSC) && !defined(CONFIG_UML)
	/* Convert ART to TSC */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	*system = convert_art_ns_to_tsc(ptm_master_time);
#else
	system->cycles = ptm_master_time;
	system->cs_id = CSID_X86_ART;
#endif
#else
	*system = (struct system_counterval_t) { };
#endif

	/* Store T4 and T1 for next request */
	dev->t4_prev = litepcie_read64(dev, CSR_PTM_REQUESTER_T4_TIME_ADDR);
	dev->t1_prev = t1_curr;

	return 0;
}

/* PTP clock operation: Get cross timestamp between system and device clocks */
static int litepcie_ptp_getcrosststamp(struct ptp_clock_info *ptp,
					  struct system_device_crosststamp *cts)
{
	struct litepcie_device *dev = container_of(ptp, struct litepcie_device,
							   ptp_caps);

	/* Obtain the cross timestamp using the provided helper */
	return get_device_system_crosststamp(litepcie_phc_get_syncdevicetime,
						 dev, &dev->snapshot, cts);
}

/* PTP clock operation: Enable or disable features (not supported) */
static int litepcie_ptp_enable(struct ptp_clock_info __always_unused *ptp,
				 struct ptp_clock_request __always_unused *request,
				 int __always_unused on)
{
	return -EOPNOTSUPP;
}

/* PTP clock capabilities and function pointers */
static struct ptp_clock_info litepcie_ptp_info = {
	.owner          = THIS_MODULE,
	.name           = LITEPCIE_NAME,
	.max_adj        = 1000000000,
	.n_alarm        = 0,
	.n_ext_ts       = 0,
	.n_per_out      = 0,
	.n_pins         = 0,
	.pps            = 0,
	.gettimex64     = litepcie_ptp_gettimex64,
	.settime64      = litepcie_ptp_settime,
	.adjtime        = litepcie_ptp_adjtime,
	.adjfine        = litepcie_ptp_adjfine,
	.getcrosststamp = litepcie_ptp_getcrosststamp,
	.enable         = litepcie_ptp_enable,
};
#endif

/* -----------------------------------------------------------------------------------------------*/
/*                                 Probe / Remove / Module                                        */
/* -----------------------------------------------------------------------------------------------*/

/* Probe the LitePCIe PCI device */
static int litepcie_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	int ret  = 0;
	int irqs = 0;
	uint8_t rev_id;
	int i;
	char fpga_identifier[256];
	struct litepcie_device *litepcie_dev = NULL;
#ifdef CSR_UART_XOVER_RXTX_ADDR
	struct resource *tty_res = NULL;
#endif
#ifdef CSR_PTM_REQUESTER_BASE
	int count = 100;
#endif

	dev_info(&dev->dev, "\e[1m[Probing device]\e[0m\n");

	/* Allocate memory for the LitePCIe device structure */
	litepcie_dev = devm_kzalloc(&dev->dev, sizeof(struct litepcie_device), GFP_KERNEL);
	if (!litepcie_dev) {
		ret = -ENOMEM;
		goto fail1;
	}

	pci_set_drvdata(dev, litepcie_dev);
	litepcie_dev->dev = dev;
	spin_lock_init(&litepcie_dev->lock);

	/* Enable the PCI device */
	ret = pcim_enable_device(dev);
	if (ret != 0) {
		dev_err(&dev->dev, "Cannot enable device\n");
		goto fail1;
	}

	ret = -EIO;

	/* Check the device version */
	pci_read_config_byte(dev, PCI_REVISION_ID, &rev_id);
	if (rev_id != 0) {
		dev_err(&dev->dev, "Unsupported device version %d\n", rev_id);
		goto fail1;
	}

	/* Check the BAR0 configuration */
	if (!(pci_resource_flags(dev, 0) & IORESOURCE_MEM)) {
		dev_err(&dev->dev, "Invalid BAR0 configuration\n");
		goto fail1;
	}

	/* Request and map BAR0 */
	if (pcim_iomap_regions(dev, BIT(0), LITEPCIE_NAME) < 0) {
		dev_err(&dev->dev, "Could not request regions\n");
		goto fail1;
	}

	litepcie_dev->bar0_addr = pcim_iomap_table(dev)[0];
	if (!litepcie_dev->bar0_addr) {
		dev_err(&dev->dev, "Could not map BAR0\n");
		goto fail1;
	}

	/* Reset LitePCIe core */
#ifdef CSR_CTRL_RESET_ADDR
	litepcie_writel(litepcie_dev, CSR_CTRL_RESET_ADDR, 1);
	msleep(10);
#endif

	/* Read and display the FPGA identifier */
	for (i = 0; i < 256; i++)
		fpga_identifier[i] = litepcie_readl(litepcie_dev, CSR_IDENTIFIER_MEM_BASE + i * 4);
	dev_info(&dev->dev, "Version %s\n", fpga_identifier);

	pci_set_master(dev);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
	ret = pci_set_dma_mask(dev, DMA_BIT_MASK(DMA_ADDR_WIDTH));
#else
	ret = dma_set_mask(&dev->dev, DMA_BIT_MASK(DMA_ADDR_WIDTH));
#endif
	if (ret) {
		dev_err(&dev->dev, "Failed to set DMA mask\n");
		goto fail1;
	}

	/* Allocate IRQs (MSI or MSI-X) */
#ifdef CSR_PCIE_MSI_PBA_ADDR
	/* MSI-X */
	irqs = pci_alloc_irq_vectors(dev, 1, 32, PCI_IRQ_MSIX);
#else
	/* MSI Single / MultiVector */
	irqs = pci_alloc_irq_vectors(dev, 1, 32, PCI_IRQ_MSI);
#endif
	if (irqs < 0) {
		dev_err(&dev->dev, "Failed to enable MSI\n");
		ret = irqs;
		goto fail1;
	}
#ifdef CSR_PCIE_MSI_PBA_ADDR
	dev_info(&dev->dev, "%d MSI-X IRQs allocated.\n", irqs);
#else
	dev_info(&dev->dev, "%d MSI IRQs allocated.\n", irqs);
#endif

	litepcie_dev->irqs = 0;
	for (i = 0; i < irqs; i++) {
		int irq = pci_irq_vector(dev, i);

		/* Request IRQ */
		ret = request_irq(irq, litepcie_interrupt, 0, LITEPCIE_NAME, litepcie_dev);
		if (ret < 0) {
			dev_err(&dev->dev, "Failed to allocate IRQ %d\n", dev->irq);
			while (--i >= 0) {
				irq = pci_irq_vector(dev, i);
				free_irq(irq, dev);
			}
			goto fail2;
		}
		litepcie_dev->irqs += 1;
	}

	litepcie_dev->channels = DMA_CHANNELS;

	/* Create all chardev in /dev */
	ret = litepcie_alloc_chdev(litepcie_dev);
	if (ret) {
		dev_err(&dev->dev, "Failed to allocate character device\n");
		goto fail2;
	}

	for (i = 0; i < litepcie_dev->channels; i++) {
		litepcie_dev->chan[i].index           = i;
		litepcie_dev->chan[i].block_size      = DMA_BUFFER_SIZE;
		litepcie_dev->chan[i].minor           = litepcie_dev->minor_base + i;
		litepcie_dev->chan[i].litepcie_dev    = litepcie_dev;
		litepcie_dev->chan[i].dma.writer_lock = 0;
		litepcie_dev->chan[i].dma.reader_lock = 0;
		init_waitqueue_head(&litepcie_dev->chan[i].wait_rd);
		init_waitqueue_head(&litepcie_dev->chan[i].wait_wr);
		switch (i) {
#ifdef CSR_PCIE_DMA3_BASE
		case 3: {
			litepcie_dev->chan[i].dma.base             = CSR_PCIE_DMA3_BASE;
			litepcie_dev->chan[i].dma.writer_interrupt = PCIE_DMA3_WRITER_INTERRUPT;
			litepcie_dev->chan[i].dma.reader_interrupt = PCIE_DMA3_READER_INTERRUPT;
		}
		break;
#endif
#ifdef CSR_PCIE_DMA2_BASE
		case 2: {
			litepcie_dev->chan[i].dma.base             = CSR_PCIE_DMA2_BASE;
			litepcie_dev->chan[i].dma.writer_interrupt = PCIE_DMA2_WRITER_INTERRUPT;
			litepcie_dev->chan[i].dma.reader_interrupt = PCIE_DMA2_READER_INTERRUPT;
		}
		break;
#endif
#ifdef CSR_PCIE_DMA1_BASE
		case 1: {
			litepcie_dev->chan[i].dma.base             = CSR_PCIE_DMA1_BASE;
			litepcie_dev->chan[i].dma.writer_interrupt = PCIE_DMA1_WRITER_INTERRUPT;
			litepcie_dev->chan[i].dma.reader_interrupt = PCIE_DMA1_READER_INTERRUPT;
		}
		break;
#endif
		default: {
			litepcie_dev->chan[i].dma.base             = CSR_PCIE_DMA0_BASE;
			litepcie_dev->chan[i].dma.writer_interrupt = PCIE_DMA0_WRITER_INTERRUPT;
			litepcie_dev->chan[i].dma.reader_interrupt = PCIE_DMA0_READER_INTERRUPT;
		}
		break;
		}
	}

	/* Allocate all DMA buffers */
	ret = litepcie_dma_init(litepcie_dev);
	if (ret) {
		dev_err(&dev->dev, "Failed to allocate DMA\n");
		goto fail3;
	}

	/* LiteUART platform device */
#ifdef CSR_UART_XOVER_RXTX_ADDR
	tty_res = devm_kzalloc(&dev->dev, sizeof(struct resource), GFP_KERNEL);
	if (!tty_res)
		return -ENOMEM;
	tty_res->start =
		(resource_size_t) litepcie_dev->bar0_addr +
		CSR_UART_XOVER_RXTX_ADDR - CSR_BASE;
	tty_res->flags = IORESOURCE_REG;
	litepcie_dev->uart = platform_device_register_simple("liteuart", litepcie_minor_idx, tty_res, 1);
	if (IS_ERR(litepcie_dev->uart)) {
		ret = PTR_ERR(litepcie_dev->uart);
		goto fail3;
	}
#endif

/* LiteSATA platform device */
#ifdef CSR_SATA_PHY_BASE
if (litepcie_soc_has_sata(litepcie_dev)) {
	struct resource res[5];
	struct platform_device *sata_pdev;
	resource_size_t base = (resource_size_t)litepcie_dev->bar0_addr;

#define FILL_REG_RES(_idx,_name,_csr,_size)                    \
	do {                                                    \
		res[_idx].name  = (_name);                      \
		res[_idx].flags = IORESOURCE_REG;               \
		res[_idx].start = base + (_csr) - CSR_BASE;     \
		res[_idx].end   = res[_idx].start + (_size) - 1;\
	} while (0)

	FILL_REG_RES(0, "ident",  CSR_SATA_IDENTIFY_BASE,   0x100);
	FILL_REG_RES(1, "phy",    CSR_SATA_PHY_BASE,        0x100);
	FILL_REG_RES(2, "reader", CSR_SATA_SECTOR2MEM_BASE, 0x100);
	FILL_REG_RES(3, "writer", CSR_SATA_MEM2SECTOR_BASE, 0x100);

#undef FILL_REG_RES

	/* Register with auto ID and set the PCI device as parent */
	{
		struct platform_device_info pinfo = {
			.parent  = &dev->dev,
			.name    = "litesata",
			.id      = PLATFORM_DEVID_AUTO,
			.res     = res,
			.num_res = ARRAY_SIZE(res),
		};

		sata_pdev = platform_device_register_full(&pinfo);
		if (IS_ERR(sata_pdev)) {
			dev_warn(&dev->dev, "LiteSATA pdev registration failed: %ld\n",
				 PTR_ERR(sata_pdev));
		} else {
			/* Ensure it’s always unregistered when the PCI dev goes away */
			int r = devm_add_action_or_reset(&dev->dev,
				(void (*)(void *))platform_device_unregister, sata_pdev);
			if (r) {
				dev_err(&dev->dev, "failed to hook litesata unregister (%d)\n", r);
				return r;
			}
			dev_info(&dev->dev, "LiteSATA platform device registered (host-DMA)\n");
			litepcie_dev->sata = sata_pdev;
		}

#if (LITESATA_FORCE_POLLING == 0)
        /* Enable LiteSATA completion MSIs */
#ifdef SATA_SECTOR2MEM_INTERRUPT
        litepcie_enable_interrupt(litepcie_dev, SATA_SECTOR2MEM_INTERRUPT);
#endif
#ifdef SATA_MEM2SECTOR_INTERRUPT
        litepcie_enable_interrupt(litepcie_dev, SATA_MEM2SECTOR_INTERRUPT);
#endif
#else
        dev_info(&dev->dev, "LiteSATA: forcing polling; SATA MSIs not enabled\n");
#endif /* LITESATA_FORCE_POLLING */
	}
}
#endif

	/* PTP setup */
#ifdef CSR_PTM_REQUESTER_BASE
	litepcie_dev->ptp_caps = litepcie_ptp_info;
	litepcie_dev->litepcie_ptp_clock = ptp_clock_register(&litepcie_dev->ptp_caps, &dev->dev);
	if (IS_ERR(litepcie_dev->litepcie_ptp_clock)) {
		return PTR_ERR(litepcie_dev->litepcie_ptp_clock);
	}

	/* Display created PTP device */
	dev_info(&dev->dev, "PTP clock registered as /dev/ptp%d\n", ptp_clock_index(litepcie_dev->litepcie_ptp_clock));

	/* Enable timer (time) counter */
	litepcie_writel(litepcie_dev, CSR_TIME_GEN_CONTROL_ADDR, TIME_CONTROL_ENABLE | TIME_CONTROL_SYNC_ENABLE);

	/* Enable PTM control and start first request */
	litepcie_writel(litepcie_dev, CSR_PTM_REQUESTER_CONTROL_ADDR, PTM_CONTROL_ENABLE | PTM_CONTROL_TRIGGER);

	/* Prepare T1 & T4 for next request */
	do {
		if ((litepcie_readl(litepcie_dev, CSR_PTM_REQUESTER_STATUS_ADDR) & PTM_STATUS_BUSY) == 0)
			break;
	} while (--count);

	litepcie_writel(litepcie_dev, CSR_PTM_REQUESTER_CONTROL_ADDR, PTM_CONTROL_ENABLE | PTM_CONTROL_TRIGGER);
	count = 100;
	do {
		if ((litepcie_readl(litepcie_dev, CSR_PTM_REQUESTER_STATUS_ADDR) & PTM_STATUS_BUSY) == 0)
			break;
	} while (--count);

	litepcie_dev->t4_prev = litepcie_read64(litepcie_dev, CSR_PTM_REQUESTER_T4_TIME_ADDR);

	litepcie_dev->t1_prev = (((u64)litepcie_readl(litepcie_dev, PTM_T1_TIME_L) << 32) |
		litepcie_readl(litepcie_dev, PTM_T1_TIME_H));

	spin_lock_init(&litepcie_dev->tmreg_lock);
#endif

	return 0;

fail3:
	litepcie_free_chdev(litepcie_dev);
fail2:
	pci_free_irq_vectors(dev);
fail1:
	return ret;
}

/* Remove the LitePCIe PCI device */
static void litepcie_pci_remove(struct pci_dev *dev)
{
	int i, irq;
	struct litepcie_device *litepcie_dev;

	litepcie_dev = pci_get_drvdata(dev);

	dev_info(&dev->dev, "\e[1m[Removing device]\e[0m\n");

	/* Stop the DMAs */
	litepcie_stop_dma(litepcie_dev);

#ifdef CSR_SATA_PHY_BASE
if (litepcie_soc_has_sata(litepcie_dev)) {
	/* Disable SATA interrupts */
#ifdef SATA_SECTOR2MEM_INTERRUPT
    litepcie_disable_interrupt(litepcie_dev, SATA_SECTOR2MEM_INTERRUPT);
#endif
#ifdef SATA_MEM2SECTOR_INTERRUPT
    litepcie_disable_interrupt(litepcie_dev, SATA_MEM2SECTOR_INTERRUPT);
#endif
}
#endif

	/* Disable all interrupts */
	litepcie_writel(litepcie_dev, CSR_PCIE_MSI_ENABLE_ADDR, 0);

    /* Unregister PTP */
#ifdef CSR_PTM_REQUESTER_BASE
	if (litepcie_dev->litepcie_ptp_clock) {
		ptp_clock_unregister(litepcie_dev->litepcie_ptp_clock);
		litepcie_dev->litepcie_ptp_clock = NULL;
	}
#endif

	/* Free all IRQs */
	for (i = 0; i < litepcie_dev->irqs; i++) {
		irq = pci_irq_vector(dev, i);
		free_irq(irq, litepcie_dev);
	}

#ifdef CSR_UART_XOVER_RXTX_ADDR
	platform_device_unregister(litepcie_dev->uart);
#endif

	litepcie_free_chdev(litepcie_dev);

	pci_free_irq_vectors(dev);
}

/* PCI device ID table */
static const struct pci_device_id litepcie_pci_ids[] = {
	{ PCI_DEVICE(PCIE_XILINX_VENDOR_ID, PCIE_XILINX_DEVICE_ID_S7_GEN2_X1), },
	{ PCI_DEVICE(PCIE_XILINX_VENDOR_ID, PCIE_XILINX_DEVICE_ID_S7_GEN2_X2), },
	{ PCI_DEVICE(PCIE_XILINX_VENDOR_ID, PCIE_XILINX_DEVICE_ID_S7_GEN2_X4), },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, litepcie_pci_ids);

/* PCI driver structure */
static struct pci_driver litepcie_pci_driver = {
	.name     = LITEPCIE_NAME,
	.id_table = litepcie_pci_ids,
	.probe    = litepcie_pci_probe,
	.remove   = litepcie_pci_remove,
};

/* Module initialization function */
static int __init litepcie_module_init(void)
{
	int ret;
	int res;

	res = liteuart_init();
	if (res)
		return res;

	res = litesata_init();
	if (res) {
		return res;
	}

	#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
		litepcie_class = class_create(THIS_MODULE, LITEPCIE_NAME);
	#else
		litepcie_class = class_create(LITEPCIE_NAME);
	#endif
	if (!litepcie_class) {
		ret = -EEXIST;
		pr_err(" Failed to create class\n");
		goto fail_create_class;
	}

	ret = alloc_chrdev_region(&litepcie_dev_t, 0, LITEPCIE_MINOR_COUNT, LITEPCIE_NAME);
	if (ret < 0) {
		pr_err("Could not allocate char device\n");
		goto fail_alloc_chrdev_region;
	}
	litepcie_major = MAJOR(litepcie_dev_t);
	litepcie_minor_idx = MINOR(litepcie_dev_t);

	ret = pci_register_driver(&litepcie_pci_driver);
	if (ret < 0) {
		pr_err("Error while registering PCI driver\n");
		goto fail_register;
	}

	return 0;

fail_register:
	unregister_chrdev_region(litepcie_dev_t, LITEPCIE_MINOR_COUNT);
fail_alloc_chrdev_region:
	class_destroy(litepcie_class);
fail_create_class:
	return ret;
}

/* Module exit function */
static void __exit litepcie_module_exit(void)
{
	pci_unregister_driver(&litepcie_pci_driver);
	unregister_chrdev_region(litepcie_dev_t, LITEPCIE_MINOR_COUNT);
	class_destroy(litepcie_class);

	liteuart_exit();
	litesata_exit();
}

module_init(litepcie_module_init);
module_exit(litepcie_module_exit);

MODULE_LICENSE("GPL");
