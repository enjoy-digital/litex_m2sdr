/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LitePCIe library
 *
 * This file is part of LitePCIe.
 *
 * Copyright (C) 2018-2026 / EnjoyDigital  / florent@enjoy-digital.fr
 *
 */

#ifndef LITEPCIE_LIB_DMA_H
#define LITEPCIE_LIB_DMA_H

#include <stdint.h>
#include <poll.h>
#include "litepcie.h"

struct litepcie_dma_ctrl {
    uint8_t use_reader, use_writer, loopback, zero_copy, shared_fd;
    struct pollfd fds;
    char *buf_rd, *buf_wr;
    uint8_t reader_enable;
    uint8_t writer_enable;
    int64_t reader_hw_count, reader_sw_count;
    int64_t writer_hw_count, writer_sw_count;
    unsigned buffers_available_read, buffers_available_write;
    unsigned usr_read_buf_offset, usr_write_buf_offset;
    /* TX staging cursors: buffers filled by the user vs flushed to the
     * kernel. Monotonic; slot = count % wr_buf_count. */
    uint64_t usr_write_fill_count, usr_write_flush_count;
    struct litepcie_ioctl_mmap_dma_info mmap_dma_info;
    struct litepcie_ioctl_mmap_dma_update mmap_dma_update;
    /* DMA ring geometry, read from the kernel (LITEPCIE_IOCTL_MMAP_DMA_INFO) in
     * litepcie_dma_init(). Userspace uses these instead of the compile-time
     * DMA_BUFFER_SIZE / DMA_BUFFER_COUNT macros so it adapts to whatever the
     * loaded kernel module was built with. rd_* is the RX (device->host) path,
     * wr_* is the TX (host->device) path. */
    uint64_t rd_buf_size, rd_buf_count;
    uint64_t wr_buf_size, wr_buf_count;
};

void litepcie_dma_set_loopback(int fd, uint8_t loopback_enable);
void litepcie_dma_reader(int fd, uint8_t enable, int64_t *hw_count, int64_t *sw_count);
void litepcie_dma_writer(int fd, uint8_t enable, int64_t *hw_count, int64_t *sw_count);

uint8_t litepcie_request_dma(int fd, uint8_t reader, uint8_t writer);
void litepcie_release_dma(int fd, uint8_t reader, uint8_t writer);

int litepcie_dma_init(struct litepcie_dma_ctrl *dma, const char *device_name, uint8_t zero_copy);
void litepcie_dma_cleanup(struct litepcie_dma_ctrl *dma);
void litepcie_dma_process(struct litepcie_dma_ctrl *dma);
char *litepcie_dma_next_read_buffer(struct litepcie_dma_ctrl *dma);
char *litepcie_dma_next_write_buffer(struct litepcie_dma_ctrl *dma);

#endif /* LITEPCIE_LIB_DMA_H */
