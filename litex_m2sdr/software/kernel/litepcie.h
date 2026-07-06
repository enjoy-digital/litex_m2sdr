/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LitePCIe driver
 *
 * This file is part of LitePCIe.
 *
 * Copyright (C) 2018-2026 / EnjoyDigital  / florent@enjoy-digital.fr
 *
 */

#ifndef _LINUX_LITEPCIE_H
#define _LINUX_LITEPCIE_H

#include <linux/types.h>

#include "csr.h"
#include "config.h"

struct litepcie_ioctl_reg {
	uint32_t addr;
	uint32_t val;
	uint8_t is_write;
};

struct litepcie_ioctl_dma {
	uint8_t loopback_enable;
};

struct litepcie_ioctl_dma_writer {
	uint8_t enable;
	int64_t hw_count;
	int64_t sw_count;
};

struct litepcie_ioctl_dma_reader {
	uint8_t enable;
	int64_t hw_count;
	int64_t sw_count;
};

struct litepcie_ioctl_lock {
	uint8_t dma_reader_request;
	uint8_t dma_writer_request;
	uint8_t dma_reader_release;
	uint8_t dma_writer_release;
	uint8_t dma_reader_status;
	uint8_t dma_writer_status;
};

struct litepcie_ioctl_mmap_dma_info {
	uint64_t dma_tx_buf_offset;
	uint64_t dma_tx_buf_size;
	uint64_t dma_tx_buf_count;

	uint64_t dma_rx_buf_offset;
	uint64_t dma_rx_buf_size;
	uint64_t dma_rx_buf_count;
};

struct litepcie_ioctl_mmap_dma_update {
	int64_t sw_count;
};

enum litepcie_dma_stats_direction {
	LITEPCIE_DMA_STATS_WRITER = 0,
	LITEPCIE_DMA_STATS_READER = 1,
};

struct litepcie_ioctl_dma_stats {
	uint8_t direction;       /* enum litepcie_dma_stats_direction.              */
	uint8_t clear;           /* Reset counters/high-water mark before reading.  */
	uint16_t reserved0;
	uint32_t reserved1;
	uint64_t buffer_size;
	uint64_t buffer_count;
	int64_t hw_count;
	int64_t sw_count;
	uint64_t ring_level;
	uint64_t ring_high_water;
	uint64_t overflow_events;
	uint64_t overflow_buffers;
	uint64_t underflow_events;
	uint64_t underflow_buffers;
	uint64_t reserved[8];
};

enum litepcie_ioctl_sata_dma_direction {
	LITEPCIE_SATA_DMA_HOST_TO_DEVICE = 0,
	LITEPCIE_SATA_DMA_DEVICE_TO_HOST = 1,
};

#define LITEPCIE_SATA_DMA_MAX_SECTORS 4096U

struct litepcie_ioctl_sata_dma {
	uint64_t user_addr;   /* In:  user buffer (>= nsectors * 512 bytes).         */
	uint64_t sector;      /* In:  first SATA sector (64-bit LBA).                */
	uint32_t nsectors;    /* In:  number of 512-byte sectors to transfer.        */
	int32_t  timeout_ms;  /* In:  per-chunk timeout, < 0 to wait forever.        */
	uint32_t direction;   /* In:  enum litepcie_ioctl_sata_dma_direction.        */
	uint32_t transferred; /* Out: sectors actually transferred.                 */
	int32_t  status;      /* Out: 0 on success, else negative errno.             */
	uint32_t reserved;    /* Reserved for future use, must be 0.                 */
};

#define LITEPCIE_IOCTL 'S'

#define LITEPCIE_IOCTL_REG               _IOWR(LITEPCIE_IOCTL,  0, struct litepcie_ioctl_reg)

#define LITEPCIE_IOCTL_DMA                       _IOW(LITEPCIE_IOCTL,  20, struct litepcie_ioctl_dma)
#define LITEPCIE_IOCTL_DMA_WRITER                _IOWR(LITEPCIE_IOCTL, 21, struct litepcie_ioctl_dma_writer)
#define LITEPCIE_IOCTL_DMA_READER                _IOWR(LITEPCIE_IOCTL, 22, struct litepcie_ioctl_dma_reader)
#define LITEPCIE_IOCTL_MMAP_DMA_INFO             _IOR(LITEPCIE_IOCTL,  24, struct litepcie_ioctl_mmap_dma_info)
#define LITEPCIE_IOCTL_LOCK                      _IOWR(LITEPCIE_IOCTL, 25, struct litepcie_ioctl_lock)
#define LITEPCIE_IOCTL_MMAP_DMA_WRITER_UPDATE    _IOW(LITEPCIE_IOCTL,  26, struct litepcie_ioctl_mmap_dma_update)
#define LITEPCIE_IOCTL_MMAP_DMA_READER_UPDATE    _IOW(LITEPCIE_IOCTL,  27, struct litepcie_ioctl_mmap_dma_update)
#define LITEPCIE_IOCTL_SATA_DMA                  _IOWR(LITEPCIE_IOCTL, 28, struct litepcie_ioctl_sata_dma)
#define LITEPCIE_IOCTL_DMA_STATS                 _IOWR(LITEPCIE_IOCTL, 29, struct litepcie_ioctl_dma_stats)

#endif /* _LINUX_LITEPCIE_H */
