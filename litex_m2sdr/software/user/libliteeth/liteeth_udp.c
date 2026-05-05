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
#include <time.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "liteeth_udp.h"

#define LITEETH_UDP_TX_CHUNK_BYTES 1024u

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

static int refresh_socket_buffer(struct liteeth_udp_ctrl *u, int optname, int *dst)
{
    socklen_t len = sizeof(*dst);

    if (!u || !dst || u->sock < 0)
        return -1;
    if (getsockopt(u->sock, SOL_SOCKET, optname, dst, &len) < 0)
        return -1;
    return 0;
}

static int rx_source_allowed(const struct liteeth_udp_ctrl *u,
                             const struct sockaddr_storage *src,
                             socklen_t src_len)
{
    const struct sockaddr_in *sin;

    if (!u || !u->rx_source_filter_enable)
        return 1;
    if (!src || src->ss_family != AF_INET || src_len < sizeof(*sin))
        return 0;

    sin = (const struct sockaddr_in *)src;
    if (sin->sin_addr.s_addr != u->rx_source.sin_addr.s_addr)
        return 0;
    if (u->rx_source_port_filter_enable &&
        sin->sin_port != u->rx_source.sin_port)
        return 0;

    return 1;
}

static int64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static ssize_t udp_recv_dontwait(struct liteeth_udp_ctrl *u, void *dst, size_t len)
{
    for (;;) {
        struct sockaddr_storage src_addr;
        socklen_t src_len = sizeof(src_addr);
#ifdef SO_RXQ_OVFL
        char cmsgbuf[CMSG_SPACE(sizeof(uint32_t))];
        struct iovec iov;
        struct msghdr msg;
        ssize_t nb;

        memset(&src_addr, 0, sizeof(src_addr));
        memset(&iov, 0, sizeof(iov));
        memset(&msg, 0, sizeof(msg));
        memset(cmsgbuf, 0, sizeof(cmsgbuf));

        iov.iov_base = dst;
        iov.iov_len = len;
        msg.msg_name = &src_addr;
        msg.msg_namelen = src_len;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);

        nb = recvmsg(u->sock, &msg, MSG_DONTWAIT);
        src_len = msg.msg_namelen;
        if (nb >= 0) {
            struct cmsghdr *cmsg;
            for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_RXQ_OVFL) {
                    uint32_t dropped = 0;
                    memcpy(&dropped, CMSG_DATA(cmsg), sizeof(dropped));
                    if (u->rxq_ovfl_valid) {
                        u->rx_kernel_drops += (uint32_t)(dropped - u->rxq_ovfl_last);
                    } else {
                        u->rx_kernel_drops += dropped;
                        u->rxq_ovfl_valid = 1;
                    }
                    u->rxq_ovfl_last = dropped;
                }
            }
        }
#else
        ssize_t nb;

        memset(&src_addr, 0, sizeof(src_addr));
        nb = recvfrom(u->sock, dst, len, MSG_DONTWAIT,
                      (struct sockaddr *)&src_addr, &src_len);
#endif
        if (nb >= 0 && !rx_source_allowed(u, &src_addr, src_len)) {
            u->rx_source_drops++;
            continue;
        }
        return nb;
    }
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
#ifdef SO_RXQ_OVFL
    (void)setsockopt(u->sock, SOL_SOCKET, SO_RXQ_OVFL, &yes, sizeof(yes));
#endif

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
    (void)refresh_socket_buffer(u, SO_RCVBUF, &u->so_rcvbuf_actual_bytes);
    (void)refresh_socket_buffer(u, SO_SNDBUF, &u->so_sndbuf_actual_bytes);

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
    /* TX buffer ownership is user-driven and sendto() handles writability when
     * buffers are submitted. Polling POLLOUT here wakes RX users immediately on
     * an empty socket and makes them report a false timeout before packets
     * arrive. */
    u->pfd.events = u->rx_enable ? POLLIN : 0;

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
        ssize_t nb = udp_recv_dontwait(u, dst, u->buf_size - u->rx_assembling_bytes);
        if (nb < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0; /* try later */
            u->rx_recv_errors++;
            perror("liteeth_udp: recvfrom");
            return -1;
        }
        if (nb == 0)
            return 0;
        u->rx_assembling_bytes += (size_t)nb;
        dst += nb;
    }

    u->rx_assembling_bytes = 0;
    return 1; /* full slot ready */
}

void liteeth_udp_process(struct liteeth_udp_ctrl *u, int timeout_ms)
{
    if (!u) return;
    if (!u->rx_enable) return;

    int produced = 0;
    const int wait_forever = timeout_ms < 0;
    const int64_t deadline = wait_forever ? 0 : get_time_ms() + timeout_ms;

    while (u->buffers_available_read < (int)u->buf_count) {
        size_t slot = (u->writer_sw_count + u->buffers_available_read) % u->buf_count;
        int f = rx_fill_slot(u, slot);
        if (f < 0)
            return;
        if (f > 0) {
            u->buffers_available_read++;
            u->rx_buffers++;
            produced = 1;
            continue;
        }

        if (produced || timeout_ms == 0)
            return;

        int remaining_ms = -1;
        if (!wait_forever) {
            int64_t now = get_time_ms();
            if (now >= deadline)
                return;
            remaining_ms = (int)(deadline - now);
        }

        int ret = poll(&u->pfd, 1, remaining_ms);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            perror("liteeth_udp: poll");
            return;
        } else if (ret == 0) {
            return;
        }

        if (u->pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return;
        if (!(u->pfd.revents & POLLIN))
            return;
    }

    if (u->buffers_available_read >= (int)u->buf_count)
        u->rx_ring_full_events++;

    /* TX: user-driven via liteeth_udp_next_write_buffer + liteeth_udp_write_submit() */
}

void liteeth_udp_flush_rx(struct liteeth_udp_ctrl *u)
{
    uint8_t tmp[2048];
    uint64_t flushed = 0;

    if (!u || !u->rx_enable)
        return;

    flushed = (uint64_t)u->buffers_available_read * u->buf_size + u->rx_assembling_bytes;

    u->writer_hw_count = 0;
    u->writer_sw_count = 0;
    u->buffers_available_read = 0;
    u->usr_read_buf_offset = 0;
    u->rx_assembling_bytes = 0;

    if (u->sock < 0)
        return;

    while (1) {
        ssize_t nb = udp_recv_dontwait(u, tmp, sizeof(tmp));
        if (nb > 0) {
            flushed += (uint64_t)nb;
            continue;
        }
        if (nb < 0 && errno == EINTR)
            continue;
        break;
    }

    if (flushed > 0) {
        u->rx_flushes++;
        u->rx_flush_bytes += flushed;
    }
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
    size_t left = u->buf_size;

    while (left) {
        size_t chunk = left;
        uint8_t *chunk_src = src;

        if (chunk > LITEETH_UDP_TX_CHUNK_BYTES)
            chunk = LITEETH_UDP_TX_CHUNK_BYTES;

        while (chunk) {
            ssize_t nb = sendto(u->sock, chunk_src, chunk, 0,
                                (struct sockaddr *)&u->remote, sizeof(u->remote));
            if (nb < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue; /* retry */
                u->tx_send_errors++;
                perror("liteeth_udp: sendto");
                return -1;
            }
            chunk_src += nb;
            chunk     -= (size_t)nb;
        }

        if (left > LITEETH_UDP_TX_CHUNK_BYTES) {
            src  += LITEETH_UDP_TX_CHUNK_BYTES;
            left -= LITEETH_UDP_TX_CHUNK_BYTES;
        } else {
            left = 0;
        }
    }

    u->usr_write_buf_offset = (u->usr_write_buf_offset + 1) % u->buf_count;
    u->reader_sw_count++;
    u->tx_buffers++;
    u->tx_bytes += u->buf_size;

    /* keep ring flowing; writer reuses slots indefinitely */
    return 0;
}

int liteeth_udp_set_rx_source_filter(struct liteeth_udp_ctrl *u,
                                     const char *ip,
                                     uint16_t port)
{
    if (!u)
        return -1;
    if (!ip || !*ip) {
        memset(&u->rx_source, 0, sizeof(u->rx_source));
        u->rx_source_filter_enable = 0;
        u->rx_source_port_filter_enable = 0;
        return 0;
    }

    memset(&u->rx_source, 0, sizeof(u->rx_source));
    u->rx_source.sin_family = AF_INET;
    u->rx_source.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &u->rx_source.sin_addr) != 1)
        return -1;

    u->rx_source_filter_enable = 1;
    u->rx_source_port_filter_enable = port ? 1 : 0;
    return 0;
}

/* Socket Tunning */

int liteeth_udp_set_so_rcvbuf(struct liteeth_udp_ctrl *u, int bytes)
{
    u->so_rcvbuf_bytes = bytes;
    if (u->sock >= 0) {
        int rc = setsockopt(u->sock, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(int));
        (void)refresh_socket_buffer(u, SO_RCVBUF, &u->so_rcvbuf_actual_bytes);
        return rc;
    }
    return 0;
}

int liteeth_udp_set_so_sndbuf(struct liteeth_udp_ctrl *u, int bytes)
{
    u->so_sndbuf_bytes = bytes;
    if (u->sock >= 0) {
        int rc = setsockopt(u->sock, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(int));
        (void)refresh_socket_buffer(u, SO_SNDBUF, &u->so_sndbuf_actual_bytes);
        return rc;
    }
    return 0;
}

int liteeth_udp_get_so_rcvbuf(struct liteeth_udp_ctrl *u, int *bytes)
{
    if (!u || !bytes)
        return -1;
    if (u->sock >= 0)
        (void)refresh_socket_buffer(u, SO_RCVBUF, &u->so_rcvbuf_actual_bytes);
    *bytes = u->so_rcvbuf_actual_bytes;
    return 0;
}

int liteeth_udp_get_so_sndbuf(struct liteeth_udp_ctrl *u, int *bytes)
{
    if (!u || !bytes)
        return -1;
    if (u->sock >= 0)
        (void)refresh_socket_buffer(u, SO_SNDBUF, &u->so_sndbuf_actual_bytes);
    *bytes = u->so_sndbuf_actual_bytes;
    return 0;
}
