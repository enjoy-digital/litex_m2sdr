/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LitePCIe driver
 *
 * This file is part of LitePCIe.
 *
 * Copyright (C) 2018-2026 / EnjoyDigital  / florent@enjoy-digital.fr
 *
 */

#ifndef __HW_CONFIG_H
#define __HW_CONFIG_H
#include "soc.h"

/* PCIe PHY Vendor IDs */

#define PCIE_XILINX_VENDOR_ID  0x10ee

/* PCIe PHY Device IDs */

#define PCIE_XILINX_DEVICE_ID_S7_GEN2_X1   0x7021
#define PCIE_XILINX_DEVICE_ID_S7_GEN2_X2   0x7022
#define PCIE_XILINX_DEVICE_ID_S7_GEN2_X4   0x7024

/* /!\ Keep in sync with csr.h  /!\ */

/* DMA Flags */
#define DMA_IRQ_DISABLE  (1<<24)
#define DMA_LAST_DISABLE (1<<25)

#define DMA_CHANNEL_COUNT      DMA_CHANNELS

/* DMA_BUFFER_COUNT / DMA_BUFFER_PER_IRQ below are COMPILE-TIME MAXIMA: they size the static DMA
 * descriptor arrays and the DMA mmap region, and they are the DEFAULTS for the runtime module
 * parameters of the same name (main.c). The ACTIVE host-RAM ring depth and IRQ-coalescing used at
 * run time are `dma_buffer_count` and `dma_buffer_per_irq`, set at insmod and validated in
 * litepcie_dma_init() (power of two, 1..DMA_BUFFER_COUNT, and dma_buffer_per_irq divides
 * dma_buffer_count). The defaults 256/8 give stock high-throughput streaming; low-latency timed TX
 * opts into a small ring at load time, e.g.:
 *     insmod m2sdr.ko dma_buffer_count=8 dma_buffer_per_irq=2
 *
 * Why the ring depth matters: the active ring depth sets the timed-TX pipeline-latency floor -- a
 * timed frame traverses the host-RAM ring before the free-running LOOP-mode reader presents it to
 * the hardware gate, so it cannot air sooner than the ring-traversal latency after submission.
 * Minimum viable lead vs dma_buffer_count (30.72 MSPS 2T2R SC16 = 8 B/sample => 33.3 us/buffer;
 * measured with timed_tx_selftest; 1T1R is 4 B/sample so ring-lat and the floors below double):
 *     count  ring-lat  min-viable-lead
 *      256    ~8.5 ms      ~10 ms        <- default; ample host-jitter headroom for streaming
 *       32    ~1.1 ms      ~1.5 ms
 *       16    ~0.55 ms     ~0.7 ms
 *        8    ~0.27 ms     ~0.3-0.4 ms   <- clears a 5G-SA k2=1 (0.5 ms) grant->PUSCH lead at 2T2R
 * TRADE-OFF of a small ring: it absorbs proportionally less host jitter (dma_buffer_count/2 x buffer
 * air-time; 8 buffers ~= 133 us @30.72 2T2R), so a small-ring consumer MUST be real-time -- a longer
 * stall underruns (the gate then drops the late frame, paced to the RFIC rate so it does not storm
 * the ring, but the frame is missed). Higher sample rates buffer proportionally less wall-clock at a given
 * depth. Smaller dma_buffer_per_irq refreshes the host's reader/writer-position view more finely
 * (finer timed-TX pacing) at the cost of a higher interrupt rate. */
#define DMA_BUFFER_PER_IRQ     8
#define DMA_BUFFER_COUNT       256
#define DMA_BUFFER_SIZE        8192
#define DMA_BUFFER_TOTAL_SIZE (DMA_BUFFER_COUNT*DMA_BUFFER_SIZE)
//#define DMA_BUFFER_ALIGNED

/* DMA Offsets */
#define PCIE_DMA_WRITER_ENABLE_OFFSET               0x0000
#define PCIE_DMA_WRITER_TABLE_VALUE_OFFSET          0x0004
#define PCIE_DMA_WRITER_TABLE_WE_OFFSET             0x000c
#define PCIE_DMA_WRITER_TABLE_LOOP_PROG_N_OFFSET    0x0010
#define PCIE_DMA_WRITER_TABLE_LOOP_STATUS_OFFSET    0x0014
#define PCIE_DMA_WRITER_TABLE_LEVEL_OFFSET          0x0018
#define PCIE_DMA_WRITER_TABLE_FLUSH_OFFSET          0x001c
#define PCIE_DMA_READER_ENABLE_OFFSET               0x0020
#define PCIE_DMA_READER_TABLE_VALUE_OFFSET          0x0024
#define PCIE_DMA_READER_TABLE_WE_OFFSET             0x002c
#define PCIE_DMA_READER_TABLE_LOOP_PROG_N_OFFSET    0x0030
#define PCIE_DMA_READER_TABLE_LOOP_STATUS_OFFSET    0x0034
#define PCIE_DMA_READER_TABLE_LEVEL_OFFSET          0x0038
#define PCIE_DMA_READER_TABLE_FLUSH_OFFSET          0x003c
#define PCIE_DMA_LOOPBACK_ENABLE_OFFSET             0x0040
#define PCIE_DMA_SYNCHRONIZER_BYPASS_OFFSET         0x0044
#define PCIE_DMA_SYNCHRONIZER_ENABLE_OFFSET         0x0048
#define PCIE_DMA_BUFFERING_READER_FIFO_DEPTH_OFFSET 0x004C
#define PCIE_DMA_BUFFERING_READER_FIFO_LEVEL_OFFSET 0x0050
#define PCIE_DMA_BUFFERING_WRITER_FIFO_DEPTH_OFFSET 0x0054
#define PCIE_DMA_BUFFERING_WRITER_FIFO_LEVEL_OFFSET 0x0058

/* /!\ Keep in sync with csr.h  /!\ */

#endif /* __HW_CONFIG_H */
