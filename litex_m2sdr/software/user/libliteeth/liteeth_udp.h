/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteEth UDP helpers.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2025-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#ifndef LITEETH_UDP_H
#define LITEETH_UDP_H

#include <stdint.h>
#include <stddef.h>
#include <poll.h>
#include <sys/types.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LITEETH_BUFFER_SIZE
#define LITEETH_BUFFER_SIZE   32768
#endif

#ifndef LITEETH_BUFFER_COUNT
#define LITEETH_BUFFER_COUNT  256
#endif

struct liteeth_udp_ctrl {
    /* Config */
    int           rx_enable;
    int           tx_enable;
    size_t        buf_size;
    size_t        buf_count;

    /* Socket */
    int           sock;
    struct pollfd pfd;
    struct sockaddr_in remote;   /* TX destination */
    int           have_remote;

    /* RX ring */
    uint8_t      *buf_rd;
    volatile int64_t writer_hw_count;
    volatile int64_t writer_sw_count;
    int           buffers_available_read;
    size_t        usr_read_buf_offset;

    /* TX ring */
    uint8_t      *buf_wr;
    volatile int64_t reader_hw_count;
    volatile int64_t reader_sw_count;
    int           buffers_available_write;
    size_t        usr_write_buf_offset;

    /* Internal RX assemble state */
    size_t        rx_assembling_bytes;

    /* Options */
    int           nonblock;
    int           so_rcvbuf_bytes;
    int           so_sndbuf_bytes;
};

int  liteeth_udp_init(struct liteeth_udp_ctrl *u,
                      const char *listen_ip, uint16_t listen_port,
                      const char *remote_ip,  uint16_t remote_port,
                      int rx_enable, int tx_enable,
                      size_t buffer_size, size_t buffer_count,
                      int nonblock);

void liteeth_udp_cleanup(struct liteeth_udp_ctrl *u);
void liteeth_udp_process(struct liteeth_udp_ctrl *u, int timeout_ms);

uint8_t *liteeth_udp_next_read_buffer (struct liteeth_udp_ctrl *u);
uint8_t *liteeth_udp_next_write_buffer(struct liteeth_udp_ctrl *u);
int      liteeth_udp_write_submit     (struct liteeth_udp_ctrl *u);

static inline int    liteeth_udp_buffers_available_read (struct liteeth_udp_ctrl *u){ return u->buffers_available_read;  }
static inline int    liteeth_udp_buffers_available_write(struct liteeth_udp_ctrl *u){ return u->buffers_available_write; }
static inline size_t liteeth_udp_buffer_size            (struct liteeth_udp_ctrl *u){ return u->buf_size; }
static inline size_t liteeth_udp_buffer_count           (struct liteeth_udp_ctrl *u){ return u->buf_count; }

int liteeth_udp_set_so_rcvbuf(struct liteeth_udp_ctrl *u, int bytes);
int liteeth_udp_set_so_sndbuf(struct liteeth_udp_ctrl *u, int bytes);

#ifdef __cplusplus
}
#endif
#endif /* LITEETH_UDP_H */
