/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteEth UDP helpers.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2025-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "liteeth_udp.h"

/* Utilities */

static int set_nonblock(int fd, int nb)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (nb) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

static int bind_any(int sock, const char *ip, uint16_t port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = (ip && *ip) ? 0 : htonl(INADDR_ANY);
    if (ip && *ip) {
        if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1)
            return -1;
    }
    return bind(sock, (struct sockaddr *)&sa, sizeof(sa));
}

static void *alloc_aligned(size_t size)
{
    void *p = NULL;
    if (posix_memalign(&p, 64, size) != 0) return NULL;
    return p;
}

/* Public API */

int liteeth_udp_init(struct liteeth_udp_ctrl *u,
                     const char *listen_ip, uint16_t listen_port,
                     const char *remote_ip,  uint16_t remote_port,
                     int rx_enable, int tx_enable,
                     size_t buffer_size, size_t buffer_count,
                     int nonblock)
{
    memset(u, 0, sizeof(*u));

    /* config */
    u->rx_enable = rx_enable    ? 1 : 0;
    u->tx_enable = tx_enable    ? 1 : 0;
    u->buf_size  = buffer_size  ? buffer_size  : LITEETH_BUFFER_SIZE;
    u->buf_count = buffer_count ? buffer_count : LITEETH_BUFFER_COUNT;
    u->nonblock  = nonblock     ? 1 : 0;

    /* rings */
    if (u->rx_enable) {
        u->buf_rd = (uint8_t *)alloc_aligned(u->buf_size * u->buf_count);
        if (!u->buf_rd) {
            fprintf(stderr, "liteeth_udp: RX alloc failed\n");
            goto fail_mem;
        }
        memset(u->buf_rd, 0, u->buf_size * u->buf_count);
    }

    if (u->tx_enable) {
        u->buf_wr = (uint8_t *)alloc_aligned(u->buf_size * u->buf_count);
        if (!u->buf_wr) {
            fprintf(stderr, "liteeth_udp: TX alloc failed\n");
            goto fail_mem;
        }
        memset(u->buf_wr, 0, u->buf_size * u->buf_count);
        /* keep a flowing ring; not a hard quota */
        u->buffers_available_write = (int)u->buf_count;
    }

    /* socket */
    u->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (u->sock < 0) {
        perror("liteeth_udp: socket");
        goto fail_sock;
    }

    int yes = 1;
    (void)setsockopt(u->sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (u->nonblock) {
        if (set_nonblock(u->sock, 1) < 0) {
            perror("liteeth_udp: fcntl(O_NONBLOCK)");
            goto fail_sock;
        }
    }
    if (u->so_rcvbuf_bytes > 0)
        (void)setsockopt(u->sock, SOL_SOCKET, SO_RCVBUF, &u->so_rcvbuf_bytes, sizeof(int));
    if (u->so_sndbuf_bytes > 0)
        (void)setsockopt(u->sock, SOL_SOCKET, SO_SNDBUF, &u->so_sndbuf_bytes, sizeof(int));

    if (bind_any(u->sock, listen_ip, listen_port) < 0) {
        perror("liteeth_udp: bind");
        goto fail_sock;
    }

    if (remote_ip && *remote_ip) {
        memset(&u->remote, 0, sizeof(u->remote));
        u->remote.sin_family = AF_INET;
        u->remote.sin_port   = htons(remote_port);
        if (inet_pton(AF_INET, remote_ip, &u->remote.sin_addr) != 1) {
            fprintf(stderr, "liteeth_udp: inet_pton(remote_ip) failed\n");
            goto fail_sock;
        }
        u->have_remote = 1;
    }

    /* poll fd */
    u->pfd.fd     = u->sock;
    u->pfd.events = (u->rx_enable ? POLLIN  : 0) |
                    (u->tx_enable ? POLLOUT : 0);

    return 0;

fail_sock:
    if (u->sock >= 0) close(u->sock);
fail_mem:
    free(u->buf_wr);
    free(u->buf_rd);
    memset(u, 0, sizeof(*u));
    return -1;
}

void liteeth_udp_cleanup(struct liteeth_udp_ctrl *u)
{
    if (!u) return;
    if (u->sock >= 0) close(u->sock);
    free(u->buf_rd);
    free(u->buf_wr);
    memset(u, 0, sizeof(*u));
}

/* RX Path */

/* accumulate exactly buf_size bytes into slot */
static int rx_fill_slot(struct liteeth_udp_ctrl *u, size_t slot)
{
    uint8_t *dst = u->buf_rd + slot * u->buf_size + u->rx_assembling_bytes;

    while (u->rx_assembling_bytes < u->buf_size) {
        ssize_t nb = recvfrom(u->sock, dst,
                              u->buf_size - u->rx_assembling_bytes,
                              0, NULL, NULL);
        if (nb < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0; /* try later */
            perror("liteeth_udp: recvfrom");
            return -1;
        }
        u->rx_assembling_bytes += (size_t)nb;
        dst += nb;
    }

    u->rx_assembling_bytes = 0;
    return 1; /* full slot ready */
}

void liteeth_udp_process(struct liteeth_udp_ctrl *u, int timeout_ms)
{
    if (!u) return;

    int ret = poll(&u->pfd, 1, timeout_ms);
    if (ret < 0) {
        perror("liteeth_udp: poll");
        return;
    } else if (ret == 0) {
        /* timeout */
        return;
    }

    if (u->rx_enable && (u->pfd.revents & POLLIN)) {
        while (u->buffers_available_read < (int)u->buf_count) {
            size_t slot = (u->writer_sw_count + u->buffers_available_read) % u->buf_count;
            int f = rx_fill_slot(u, slot);
            if (f <= 0) break; /* 0: not enough yet, -1: error already reported */
            u->buffers_available_read++;
        }
    }

    /* TX: user-driven via liteeth_udp_next_write_buffer + liteeth_udp_write_submit() */
}

uint8_t *liteeth_udp_next_read_buffer(struct liteeth_udp_ctrl *u)
{
    if (!u->buffers_available_read)
        return NULL;

    uint8_t *ret = u->buf_rd + (u->usr_read_buf_offset * u->buf_size);

    u->usr_read_buf_offset = (u->usr_read_buf_offset + 1) % u->buf_count;
    u->buffers_available_read--;
    u->writer_sw_count++;

    return ret;
}

/* TX Path */

uint8_t *liteeth_udp_next_write_buffer(struct liteeth_udp_ctrl *u)
{
    if (!u->tx_enable)
        return NULL;

    /* always hand out current slot; user must submit to advance */
    return u->buf_wr + (u->usr_write_buf_offset * u->buf_size);
}

int liteeth_udp_write_submit(struct liteeth_udp_ctrl *u)
{
    if (!u->tx_enable || !u->have_remote)
        return -1;

    uint8_t *src = u->buf_wr + (u->usr_write_buf_offset * u->buf_size);
    size_t   left = u->buf_size;

    while (left) {
        ssize_t nb = sendto(u->sock, src, left, 0,
                            (struct sockaddr *)&u->remote, sizeof(u->remote));
        if (nb < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue; /* retry */
            perror("liteeth_udp: sendto");
            return -1;
        }
        src  += nb;
        left -= (size_t)nb;
    }

    u->usr_write_buf_offset = (u->usr_write_buf_offset + 1) % u->buf_count;
    u->reader_sw_count++;

    /* keep ring flowing; writer reuses slots indefinitely */
    return 0;
}

/* Socket Tunning */

int liteeth_udp_set_so_rcvbuf(struct liteeth_udp_ctrl *u, int bytes)
{
    u->so_rcvbuf_bytes = bytes;
    if (u->sock >= 0)
        return setsockopt(u->sock, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(int));
    return 0;
}

int liteeth_udp_set_so_sndbuf(struct liteeth_udp_ctrl *u, int bytes)
{
    u->so_sndbuf_bytes = bytes;
    if (u->sock >= 0)
        return setsockopt(u->sock, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(int));
    return 0;
}
