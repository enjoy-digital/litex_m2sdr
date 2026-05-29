/*
 * Etherbone C Library.
 *
 * Copyright (c) 2020-2025 LiteX-Hub community.
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Copied from https://github.com/litex-hub/wishbone-utils/tree/master/libeb-c
 */

#if defined(__FreeBSD__)
#include <sys/endian.h>
#else
#include <endian.h>
#endif
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "etherbone.h"

#define EB_DEFAULT_TIMEOUT_MS 1000
#define EB_DIRECT_RX_SETTLE_US 20000
#define EB_READ_ATTEMPTS 3
#define EB_MAX_DISCARDS 32
#define EB_MAX_INVALID_READ_REPLIES 8
#define EB_MAX_BURST_WORDS 255
#define EB_PACKET_HEADER_BYTES 8
#define EB_RECORD_HEADER_BYTES 4
#define EB_ADDR_BYTES 4
#define EB_MIN_PACKET_BYTES (EB_PACKET_HEADER_BYTES + EB_RECORD_HEADER_BYTES + EB_ADDR_BYTES)

/* Byte offsets into an Etherbone packet: header | record header | address | data. */
#define EB_ADDR_OFFSET (EB_PACKET_HEADER_BYTES + EB_RECORD_HEADER_BYTES) /* 12 */
#define EB_DATA_OFFSET (EB_ADDR_OFFSET + EB_ADDR_BYTES)                  /* 16 */

struct eb_connection {
    int fd;
    int read_fd;
    int is_direct;
    int timeout_ms;
    int last_error;
    uint32_t read_counter;
    struct addrinfo* addr;
};

static int eb_fail(struct eb_connection *conn, int err)
{
    if (conn)
        conn->last_error = err;
    return err;
}

static bool eb_sockaddr_matches(const struct eb_connection *conn,
                                const struct sockaddr *addr,
                                socklen_t addr_len)
{
    const struct sockaddr *peer;

    if (!conn || !conn->addr || !conn->addr->ai_addr || !addr)
        return false;

    peer = conn->addr->ai_addr;
    if (addr->sa_family != peer->sa_family)
        return false;

    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)addr;
        const struct sockaddr_in *b = (const struct sockaddr_in *)peer;

        if (addr_len < sizeof(*a) || conn->addr->ai_addrlen < sizeof(*b))
            return false;

        return a->sin_port == b->sin_port &&
               a->sin_addr.s_addr == b->sin_addr.s_addr;
    }

    return false;
}

static bool eb_validate_read_reply(const uint8_t *pkt, size_t len)
{
    if (!pkt || len != 20)
        return false;
    if (pkt[0] != 0x4e || pkt[1] != 0x6f)
        return false;
    if (pkt[2] != 0x10 || pkt[3] != 0x44)
        return false;
    if (pkt[4] != 0 || pkt[5] != 0 || pkt[6] != 0 || pkt[7] != 0)
        return false;
    if (pkt[9] != 0x0f)
        return false;

    return true;
}

static void eb_put_be32(uint8_t *dst, uint32_t value)
{
    value = htobe32(value);
    memcpy(dst, &value, sizeof(value));
}

static uint32_t eb_get_be32(const uint8_t *src)
{
    uint32_t value;

    memcpy(&value, src, sizeof(value));
    return be32toh(value);
}

static size_t eb_bulk_packet_len(size_t count)
{
    return EB_MIN_PACKET_BYTES + count * sizeof(uint32_t);
}

static int eb_validate_bulk_count(struct eb_connection *conn, const void *vals, size_t count)
{
    if (!conn || !vals || count == 0 || count > EB_MAX_BURST_WORDS)
        return eb_fail(conn, EB_ERR_IO);
    return EB_ERR_OK;
}

static void eb_fill_header(uint8_t *pkt)
{
    memset(pkt, 0, EB_MIN_PACKET_BYTES);
    pkt[0] = 0x4e; /* Magic byte 0. */
    pkt[1] = 0x6f; /* Magic byte 1. */
    pkt[2] = 0x10; /* Version 1. */
    pkt[3] = 0x44; /* 32-bit port, 32-bit address. */
}

static void eb_fill_record_header(uint8_t *pkt, uint8_t wcount, uint8_t rcount)
{
    pkt[8]  = 0x00; /* Unsupported Etherbone flags stay clear. */
    pkt[9]  = 0x0f; /* 32-bit byte enable. */
    pkt[10] = wcount;
    pkt[11] = rcount;
}

static bool eb_validate_bulk_read_reply(const uint8_t *pkt,
                                        size_t len,
                                        uint32_t expected_base_addr,
                                        size_t expected_count)
{
    if (!pkt || expected_count == 0 || expected_count > EB_MAX_BURST_WORDS)
        return false;
    if (len != eb_bulk_packet_len(expected_count))
        return false;
    if (pkt[0] != 0x4e || pkt[1] != 0x6f)
        return false;
    if (pkt[2] != 0x10 || pkt[3] != 0x44)
        return false;
    if (pkt[4] != 0 || pkt[5] != 0 || pkt[6] != 0 || pkt[7] != 0)
        return false;
    if (pkt[8] != 0x00 || pkt[9] != 0x0f)
        return false;
    if (pkt[10] != expected_count || pkt[11] != 0)
        return false;
    if (eb_get_be32(&pkt[EB_ADDR_OFFSET]) != expected_base_addr)
        return false;
    return true;
}

static bool eb_validate_bulk_read_reply_header(const uint8_t *pkt,
                                               size_t len,
                                               uint32_t *base_ret_addr,
                                               size_t *count)
{
    uint32_t c;

    if (!pkt || len < EB_MIN_PACKET_BYTES + sizeof(uint32_t))
        return false;
    if ((len - EB_MIN_PACKET_BYTES) % sizeof(uint32_t) != 0)
        return false;
    if (pkt[0] != 0x4e || pkt[1] != 0x6f)
        return false;
    if (pkt[2] != 0x10 || pkt[3] != 0x44)
        return false;
    if (pkt[4] != 0 || pkt[5] != 0 || pkt[6] != 0 || pkt[7] != 0)
        return false;
    if (pkt[8] != 0x00 || pkt[9] != 0x0f)
        return false;
    if (pkt[11] != 0)
        return false;
    c = pkt[10];
    if (c == 0 || c > EB_MAX_BURST_WORDS)
        return false;
    if (len != eb_bulk_packet_len(c))
        return false;

    if (base_ret_addr)
        *base_ret_addr = eb_get_be32(&pkt[EB_ADDR_OFFSET]);
    if (count)
        *count = c;
    return true;
}

static int eb_wait_fd(struct eb_connection *conn, int fd, short events)
{
    struct pollfd pfd;
    int ret;

    if (!conn || fd < 0)
        return eb_fail(conn, EB_ERR_IO);

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = events;

    do {
        ret = poll(&pfd, 1, conn->timeout_ms);
    } while (ret < 0 && errno == EAGAIN);

    if (ret == 0)
        return eb_fail(conn, EB_ERR_TIMEOUT);
    if (ret < 0) {
        if (errno == EINTR)
            return eb_fail(conn, EB_ERR_INTERRUPTED);
        fprintf(stderr, "socket poll error: %s\n", strerror(errno));
        return eb_fail(conn, EB_ERR_IO);
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        return eb_fail(conn, EB_ERR_IO);
    if ((pfd.revents & events) == 0)
        return eb_fail(conn, EB_ERR_IO);

    return EB_ERR_OK;
}

int eb_set_timeout(struct eb_connection *conn, int timeout_ms)
{
    if (!conn || timeout_ms < 0)
        return EB_ERR_IO;

    conn->timeout_ms = timeout_ms;
    return EB_ERR_OK;
}

int eb_get_last_error(struct eb_connection *conn)
{
    return conn ? conn->last_error : EB_ERR_IO;
}

static void eb_drain_direct_rx(struct eb_connection *conn)
{
    uint8_t discard[256];

    if (!conn || !conn->is_direct || conn->read_fd < 0)
        return;

    for (;;) {
        int ret = recvfrom(conn->read_fd, discard, sizeof(discard), MSG_DONTWAIT, NULL, NULL);

        if (ret > 0)
            continue;
        if (ret == 0)
            return;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        return;
    }
}

int eb_unfill_read32(uint8_t wb_buffer[20]) {
    int buffer;
    uint32_t intermediate;
    memcpy(&intermediate, &wb_buffer[16], sizeof(intermediate));
    intermediate = be32toh(intermediate);
    memcpy(&buffer, &intermediate, sizeof(intermediate));
    return buffer;
}

int eb_fill_readwrite32(uint8_t wb_buffer[20], uint32_t data, uint32_t address, int is_read) {
    memset(wb_buffer, 0, 20);
    wb_buffer[0] = 0x4e;	// Magic byte 0
    wb_buffer[1] = 0x6f;	// Magic byte 1
    wb_buffer[2] = 0x10;	// Version 1, all other flags 0
    wb_buffer[3] = 0x44;	// Address is 32-bits, port is 32-bits
    wb_buffer[4] = 0;		// Padding
    wb_buffer[5] = 0;		// Padding
    wb_buffer[6] = 0;		// Padding
    wb_buffer[7] = 0;		// Padding

    // Record
    wb_buffer[8] = 0;		// No Wishbone flags are set (cyc, wca, wff, etc.)
    wb_buffer[9] = 0x0f;	// Byte enable

    if (is_read) {
        wb_buffer[10] = 0;  // Write count
        wb_buffer[11] = 1;	// Read count
        data = htobe32(address);
        memcpy(&wb_buffer[16], &data, sizeof(data));
    }
    else {
        wb_buffer[10] = 1;	// Write count
        wb_buffer[11] = 0;  // Read count
        address = htobe32(address);
        memcpy(&wb_buffer[12], &address, sizeof(address));

        data = htobe32(data);
        memcpy(&wb_buffer[16], &data, sizeof(data));
    }
    return 20;
}

int eb_fill_write32(uint8_t wb_buffer[20], uint32_t data, uint32_t address) {
    return eb_fill_readwrite32(wb_buffer, data, address, 0);
}

int eb_fill_read32(uint8_t wb_buffer[20], uint32_t address) {
    return eb_fill_readwrite32(wb_buffer, 0, address, 1);
}

int eb_send(struct eb_connection *conn, const void *bytes, size_t len) {
    if (conn->is_direct) {
        int ret;
        do {
            if (eb_wait_fd(conn, conn->fd, POLLOUT) != EB_ERR_OK)
                return -1;
            ret = sendto(conn->fd, bytes, len, 0, conn->addr->ai_addr, conn->addr->ai_addrlen);
        } while (ret < 0 && errno == EAGAIN);
        if (ret < 0) {
            if (errno == EINTR) {
                eb_fail(conn, EB_ERR_INTERRUPTED);
                return ret;
            }
            fprintf(stderr, "socket send error: %s\n", strerror(errno));
            eb_fail(conn, EB_ERR_IO);
            return ret;
        }
        if ((size_t)ret != len) {
            fprintf(stderr, "short UDP send: %d/%zu\n", ret, len);
            eb_fail(conn, EB_ERR_IO);
            return -1;
        }
        conn->last_error = EB_ERR_OK;
        return ret;
    }

    size_t sent = 0;
    while (sent < len) {
        if (eb_wait_fd(conn, conn->fd, POLLOUT) != EB_ERR_OK)
            return -1;
        int ret = write(conn->fd, (const uint8_t *)bytes + sent, len - sent);
        if (ret < 0) {
            if (errno == EAGAIN)
                continue;
            if (errno == EINTR)
                return eb_fail(conn, EB_ERR_INTERRUPTED);
            fprintf(stderr, "socket send error: %s\n", strerror(errno));
            eb_fail(conn, EB_ERR_IO);
            return ret;
        }
        sent += (size_t)ret;
    }
    conn->last_error = EB_ERR_OK;
    return (int)sent;
}

int eb_recv(struct eb_connection *conn, void *bytes, size_t max_len) {
    int ret;
    if (conn->is_direct) {
        int discarded = 0;

        for (;;) {
            struct sockaddr_storage src;
            socklen_t src_len = sizeof(src);

            if (eb_wait_fd(conn, conn->read_fd, POLLIN) != EB_ERR_OK)
                return -1;
            ret = recvfrom(conn->read_fd, bytes, max_len, 0,
                           (struct sockaddr *)&src, &src_len);
            if (ret < 0 && errno == EAGAIN)
                continue;
            if (ret < 0)
                break;
            if (ret >= 0 && !eb_sockaddr_matches(conn, (struct sockaddr *)&src, src_len)) {
                if (++discarded >= EB_MAX_DISCARDS)
                    return eb_fail(conn, EB_ERR_TIMEOUT);
                continue;
            }
            break;
        }
        if (ret < 0) {
            if (errno == EINTR)
                return eb_fail(conn, EB_ERR_INTERRUPTED);
            fprintf(stderr, "socket recv error: %s\n", strerror(errno));
            eb_fail(conn, EB_ERR_IO);
        } else {
            conn->last_error = EB_ERR_OK;
        }
        return ret;
    }

    do {
        if (eb_wait_fd(conn, conn->fd, POLLIN) != EB_ERR_OK)
            return -1;
        ret = read(conn->fd, bytes, max_len);
    } while (ret < 0 && errno == EAGAIN);
    if (ret < 0) {
        if (errno == EINTR)
            return eb_fail(conn, EB_ERR_INTERRUPTED);
        fprintf(stderr, "socket recv error: %s\n", strerror(errno));
        eb_fail(conn, EB_ERR_IO);
    } else {
        conn->last_error = EB_ERR_OK;
    }
    return ret;
}

int eb_write32_checked(struct eb_connection *conn, uint32_t val, uint32_t addr) {
    uint8_t raw_pkt[20];
    eb_fill_write32(raw_pkt, val, addr);
    if (eb_send(conn, raw_pkt, sizeof(raw_pkt)) < 0) {
        fprintf(stderr, "eb_write32: send failed\n");
        return eb_get_last_error(conn);
    }
    return EB_ERR_OK;
}

void eb_write32(struct eb_connection *conn, uint32_t val, uint32_t addr) {
    (void)eb_write32_checked(conn, val, addr);
}

int eb_write32_bulk_checked(struct eb_connection *conn, uint32_t addr, const uint32_t *vals, size_t count)
{
    uint8_t raw_pkt[EB_MIN_PACKET_BYTES + EB_MAX_BURST_WORDS * sizeof(uint32_t)];
    size_t len;

    if (eb_validate_bulk_count(conn, vals, count) != EB_ERR_OK)
        return EB_ERR_IO;

    len = eb_bulk_packet_len(count);
    eb_fill_header(raw_pkt);
    /* Write record only (rcount = 0): Etherbone sends no reply, so this is
     * fire-and-forget and we cannot confirm the writes landed. */
    eb_fill_record_header(raw_pkt, (uint8_t)count, 0);
    eb_put_be32(&raw_pkt[EB_ADDR_OFFSET], addr);
    for (size_t i = 0; i < count; i++)
        eb_put_be32(&raw_pkt[EB_DATA_OFFSET + 4 * i], vals[i]);

    if (eb_send(conn, raw_pkt, len) < 0) {
        fprintf(stderr, "eb_write32_bulk: send failed\n");
        return eb_get_last_error(conn);
    }
    return EB_ERR_OK;
}

static int eb_recv_read_reply(struct eb_connection *conn, uint8_t raw_pkt[20])
{
    if (conn->is_direct) {
        int invalid = 0;

        for (;;) {
            int count = eb_recv(conn, raw_pkt, 20);

            if (count < 0)
                return eb_get_last_error(conn);
            if (eb_validate_read_reply(raw_pkt, (size_t)count))
                return EB_ERR_OK;
            if (++invalid >= EB_MAX_INVALID_READ_REPLIES)
                return eb_fail(conn, EB_ERR_IO);
        }
    } else {
        // If we are connected via TCP we need to take into account any size
        // read because it is a stream oriented protocol.
        int ret;
        uint8_t *p   = raw_pkt;
        uint8_t *end = raw_pkt + 20;

        while (p < end) {
            ret = eb_recv(conn, p, end - p);

            if (ret < 0) {
                int err = eb_get_last_error(conn);

                if (err == EB_ERR_TIMEOUT || err == EB_ERR_INTERRUPTED)
                    return err;
                fprintf(stderr, "socket read error: %s\n", strerror(errno));
                return eb_fail(conn, EB_ERR_IO);
            }
            if (ret == 0)
                return eb_fail(conn, EB_ERR_IO);

            p += ret;
        }

        if (!eb_validate_read_reply(raw_pkt, 20))
            return eb_fail(conn, EB_ERR_IO);
    }

    return EB_ERR_OK;
}

static int eb_read32_once(struct eb_connection *conn, uint32_t addr, uint32_t *val)
{
    uint8_t raw_pkt[20];

    if (!val)
        return eb_fail(conn, EB_ERR_IO);

    eb_fill_read32(raw_pkt, addr);

    if (conn->is_direct)
        eb_drain_direct_rx(conn);

    if (eb_send(conn, raw_pkt, sizeof(raw_pkt)) < 0) {
        fprintf(stderr, "eb_read32: send failed\n");
        return eb_get_last_error(conn);
    }

    {
        int err = eb_recv_read_reply(conn, raw_pkt);
        if (err != EB_ERR_OK)
            return err;
    }

    *val = eb_unfill_read32(raw_pkt);
    conn->last_error = EB_ERR_OK;
    return EB_ERR_OK;
}

int eb_read32_checked(struct eb_connection *conn, uint32_t addr, uint32_t *val) {
    int err = EB_ERR_IO;
    int attempts = (conn && conn->is_direct) ? EB_READ_ATTEMPTS : 1;

    for (int attempt = 0; attempt < attempts; attempt++) {
        err = eb_read32_once(conn, addr, val);
        if (err == EB_ERR_OK || err == EB_ERR_INTERRUPTED)
            return err;
        if (conn && conn->is_direct)
            eb_drain_direct_rx(conn);
    }

    return eb_fail(conn, err);
}

static int eb_recv_bulk_read_reply(struct eb_connection *conn,
                                   uint8_t *raw_pkt,
                                   size_t len,
                                   uint32_t base_ret_addr,
                                   size_t count)
{
    if (conn->is_direct) {
        int invalid = 0;

        for (;;) {
            int received = eb_recv(conn, raw_pkt, len);

            if (received < 0)
                return eb_get_last_error(conn);
            if (eb_validate_bulk_read_reply(raw_pkt, (size_t)received, base_ret_addr, count))
                return EB_ERR_OK;
            if (++invalid >= EB_MAX_INVALID_READ_REPLIES)
                return eb_fail(conn, EB_ERR_IO);
        }
    } else {
        uint8_t *p   = raw_pkt;
        uint8_t *end = raw_pkt + len;

        while (p < end) {
            int received = eb_recv(conn, p, (size_t)(end - p));

            if (received < 0) {
                int err = eb_get_last_error(conn);

                if (err == EB_ERR_TIMEOUT || err == EB_ERR_INTERRUPTED)
                    return err;
                fprintf(stderr, "socket read error: %s\n", strerror(errno));
                return eb_fail(conn, EB_ERR_IO);
            }
            if (received == 0)
                return eb_fail(conn, EB_ERR_IO);
            p += received;
        }
        if (!eb_validate_bulk_read_reply(raw_pkt, len, base_ret_addr, count))
            return eb_fail(conn, EB_ERR_IO);
    }

    return EB_ERR_OK;
}

/* Allocate the next reply-matching tag. 0 is reserved as a "no reply parsed
 * yet" sentinel in the pipelined reader, so it is skipped on wrap-around. */
static uint32_t eb_next_read_counter(struct eb_connection *conn)
{
    uint32_t base_ret_addr = ++conn->read_counter;

    if (base_ret_addr == 0)
        base_ret_addr = ++conn->read_counter;
    return base_ret_addr;
}

static int eb_read32_bulk_once(struct eb_connection *conn, uint32_t addr, uint32_t *vals, size_t count)
{
    uint8_t raw_pkt[EB_MIN_PACKET_BYTES + EB_MAX_BURST_WORDS * sizeof(uint32_t)];
    uint32_t base_ret_addr;
    size_t len;

    if (eb_validate_bulk_count(conn, vals, count) != EB_ERR_OK)
        return EB_ERR_IO;

    len = eb_bulk_packet_len(count);
    base_ret_addr = eb_next_read_counter(conn);

    eb_fill_header(raw_pkt);
    eb_fill_record_header(raw_pkt, 0, (uint8_t)count);
    eb_put_be32(&raw_pkt[EB_ADDR_OFFSET], base_ret_addr);
    for (size_t i = 0; i < count; i++)
        eb_put_be32(&raw_pkt[EB_DATA_OFFSET + 4 * i], addr + (uint32_t)(4 * i));

    if (conn->is_direct)
        eb_drain_direct_rx(conn);

    if (eb_send(conn, raw_pkt, len) < 0) {
        fprintf(stderr, "eb_read32_bulk: send failed\n");
        return eb_get_last_error(conn);
    }

    {
        int err = eb_recv_bulk_read_reply(conn, raw_pkt, len, base_ret_addr, count);
        if (err != EB_ERR_OK)
            return err;
    }

    for (size_t i = 0; i < count; i++)
        vals[i] = eb_get_be32(&raw_pkt[EB_DATA_OFFSET + 4 * i]);
    conn->last_error = EB_ERR_OK;
    return EB_ERR_OK;
}

int eb_read32_bulk_checked(struct eb_connection *conn, uint32_t addr, uint32_t *vals, size_t count)
{
    int err = EB_ERR_IO;
    int attempts = (conn && conn->is_direct) ? EB_READ_ATTEMPTS : 1;

    for (int attempt = 0; attempt < attempts; attempt++) {
        err = eb_read32_bulk_once(conn, addr, vals, count);
        if (err == EB_ERR_OK || err == EB_ERR_INTERRUPTED)
            return err;
        if (conn && conn->is_direct)
            eb_drain_direct_rx(conn);
    }

    return eb_fail(conn, err);
}

struct eb_pipeline_read {
    uint32_t base_ret_addr;
    size_t offset;
    size_t count;
};

static int eb_send_bulk_read_request(struct eb_connection *conn,
                                     uint32_t addr,
                                     size_t count,
                                     uint32_t base_ret_addr)
{
    uint8_t raw_pkt[EB_MIN_PACKET_BYTES + EB_MAX_BURST_WORDS * sizeof(uint32_t)];
    size_t len;

    if (!conn || count == 0 || count > EB_MAX_BURST_WORDS)
        return eb_fail(conn, EB_ERR_IO);

    len = eb_bulk_packet_len(count);
    eb_fill_header(raw_pkt);
    eb_fill_record_header(raw_pkt, 0, (uint8_t)count);
    eb_put_be32(&raw_pkt[EB_ADDR_OFFSET], base_ret_addr);
    for (size_t i = 0; i < count; i++)
        eb_put_be32(&raw_pkt[EB_DATA_OFFSET + 4 * i], addr + (uint32_t)(4 * i));

    if (eb_send(conn, raw_pkt, len) < 0) {
        fprintf(stderr, "eb_read32_bulk_pipeline: send failed\n");
        return eb_get_last_error(conn);
    }
    return EB_ERR_OK;
}

static int eb_read32_bulk_pipeline_once(struct eb_connection *conn,
                                        uint32_t addr,
                                        uint32_t *vals,
                                        size_t count,
                                        size_t burst_words,
                                        size_t window)
{
    struct eb_pipeline_read *pending;
    uint8_t *raw_pkt;
    size_t max_len;
    size_t sent = 0;
    size_t completed = 0;
    size_t pending_count = 0;
    int invalid = 0;
    int rc = EB_ERR_IO;

    if (!conn || !vals || count == 0 || burst_words == 0 ||
        burst_words > EB_MAX_BURST_WORDS || window == 0)
        return eb_fail(conn, EB_ERR_IO);

    pending = calloc(window, sizeof(*pending));
    if (!pending)
        return eb_fail(conn, EB_ERR_IO);
    max_len = eb_bulk_packet_len(burst_words);
    raw_pkt = malloc(max_len);
    if (!raw_pkt) {
        free(pending);
        return eb_fail(conn, EB_ERR_IO);
    }

    eb_drain_direct_rx(conn);

    while (completed < count) {
        while (pending_count < window && sent < count) {
            size_t chunk = count - sent;
            uint32_t base_ret_addr;

            if (chunk > burst_words)
                chunk = burst_words;
            base_ret_addr = eb_next_read_counter(conn);
            rc = eb_send_bulk_read_request(conn,
                addr + (uint32_t)(4 * sent), chunk, base_ret_addr);
            if (rc != EB_ERR_OK)
                goto out;

            pending[pending_count].base_ret_addr = base_ret_addr;
            pending[pending_count].offset = sent;
            pending[pending_count].count = chunk;
            pending_count++;
            sent += chunk;
        }

        for (;;) {
            uint32_t base_ret_addr = 0;
            size_t reply_count = 0;
            size_t pending_index = pending_count;
            int received = eb_recv(conn, raw_pkt, max_len);

            if (received < 0) {
                rc = eb_get_last_error(conn);
                goto out;
            }
            if (!eb_validate_bulk_read_reply_header(raw_pkt, (size_t)received,
                                                    &base_ret_addr, &reply_count)) {
                if (++invalid >= EB_MAX_INVALID_READ_REPLIES) {
                    rc = eb_fail(conn, EB_ERR_IO);
                    goto out;
                }
                continue;
            }

            for (size_t i = 0; i < pending_count; i++) {
                if (pending[i].base_ret_addr == base_ret_addr &&
                    pending[i].count == reply_count) {
                    pending_index = i;
                    break;
                }
            }
            if (pending_index == pending_count) {
                if (++invalid >= EB_MAX_INVALID_READ_REPLIES) {
                    rc = eb_fail(conn, EB_ERR_IO);
                    goto out;
                }
                continue;
            }

            for (size_t i = 0; i < reply_count; i++)
                vals[pending[pending_index].offset + i] = eb_get_be32(&raw_pkt[EB_DATA_OFFSET + 4 * i]);

            completed += reply_count;
            pending[pending_index] = pending[pending_count - 1];
            pending_count--;
            invalid = 0;
            break;
        }
    }

    conn->last_error = EB_ERR_OK;
    rc = EB_ERR_OK;

out:
    free(raw_pkt);
    free(pending);
    return rc;
}

int eb_read32_bulk_pipeline_checked(struct eb_connection *conn, uint32_t addr, uint32_t *vals,
                                    size_t count, size_t burst_words, size_t window)
{
    int err = EB_ERR_IO;

    if (!conn || !vals || burst_words == 0 || burst_words > EB_MAX_BURST_WORDS || window == 0)
        return eb_fail(conn, EB_ERR_IO);
    if (count == 0)
        return EB_ERR_OK;

    if (!conn->is_direct || window <= 1 || count <= burst_words) {
        for (size_t done = 0; done < count;) {
            size_t chunk = count - done;

            if (chunk > burst_words)
                chunk = burst_words;
            err = eb_read32_bulk_checked(conn, addr + (uint32_t)(4 * done),
                                         vals + done, chunk);
            if (err != EB_ERR_OK)
                return err;
            done += chunk;
        }
        return EB_ERR_OK;
    }

    for (int attempt = 0; attempt < EB_READ_ATTEMPTS; attempt++) {
        err = eb_read32_bulk_pipeline_once(conn, addr, vals, count, burst_words, window);
        if (err == EB_ERR_OK || err == EB_ERR_INTERRUPTED)
            return err;
        eb_drain_direct_rx(conn);
    }

    return eb_fail(conn, err);
}

uint32_t eb_read32(struct eb_connection *conn, uint32_t addr) {
    uint32_t val = 0xffffffffu;

    if (eb_read32_checked(conn, addr, &val) != EB_ERR_OK)
        return 0xffffffffu;

    return val;
}

struct eb_connection *eb_connect(const char *addr, const char *port, int is_direct) {

    struct addrinfo hints;
    struct addrinfo* res = 0;
    int err;
    int sock = -1;
    int rx_socket = -1;
    int tx_socket = -1;

    struct eb_connection *conn = malloc(sizeof(struct eb_connection));
    if (!conn) {
        perror("couldn't allocate memory for eb_connection");
        return NULL;
    }
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->read_fd = -1;
    conn->timeout_ms = EB_DEFAULT_TIMEOUT_MS;
    conn->last_error = EB_ERR_OK;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = is_direct ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_protocol = is_direct ? IPPROTO_UDP : IPPROTO_TCP;
    hints.ai_flags = AI_ADDRCONFIG;
    err = getaddrinfo(addr, port, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "failed to resolve remote socket address (err=%d / %s)\n", err, gai_strerror(err));
        free(conn);
        return NULL;
    }

    conn->is_direct = is_direct;

    if (is_direct) {
        // Rx half
        struct sockaddr_in si_me;

        memset((char *) &si_me, 0, sizeof(si_me));
        si_me.sin_family = res->ai_family;
        si_me.sin_port = ((struct sockaddr_in *)res->ai_addr)->sin_port;
        si_me.sin_addr.s_addr = htobe32(INADDR_ANY);

        if ((rx_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) == -1) {
            fprintf(stderr, "Unable to create Rx socket: %s\n", strerror(errno));
            goto error;
        }
        /* Enable address reuse on RX Socket */
        int opt = 1;
        if (setsockopt(rx_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            fprintf(stderr, "setsockopt(SO_REUSEADDR) on rx_socket failed: %s\n", strerror(errno));
            goto error;
        }
        if (bind(rx_socket, (struct sockaddr*)&si_me, sizeof(si_me)) == -1) {
            fprintf(stderr, "Unable to bind Rx socket to port: %s\n", strerror(errno));
            goto error;
        }

        // Tx half
        tx_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (tx_socket == -1) {
            fprintf(stderr, "Unable to create socket: %s\n", strerror(errno));
            goto error;
        }
        /* Enable address reuse on TX Socket */
        if (setsockopt(tx_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            fprintf(stderr, "setsockopt(SO_REUSEADDR) on tx_socket failed: %s\n", strerror(errno));
            goto error;
        }

        conn->read_fd = rx_socket;
        conn->fd = tx_socket;
        conn->addr = res;
        eb_drain_direct_rx(conn);
        usleep(EB_DIRECT_RX_SETTLE_US);
        eb_drain_direct_rx(conn);
    }
    else {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == -1) {
            fprintf(stderr, "failed to create socket: %s\n", strerror(errno));
            goto error;
        }

        int connection = connect(sock, res->ai_addr, res->ai_addrlen);
        if (connection == -1) {
            fprintf(stderr, "unable to create socket: %s\n", strerror(errno));
            goto error;
        }

        int ret;
        int val = 1;

        ret = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
        if (ret < 0) {
            fprintf(stderr, "setsockopt error: %s\n", strerror(errno));
            goto error;
        }

        conn->fd = sock;
        conn->addr = res;
    }

    return conn;

error:
    if (rx_socket >= 0)
        close(rx_socket);
    if (tx_socket >= 0)
        close(tx_socket);
    if (sock >= 0)
        close(sock);
    if (res)
        freeaddrinfo(res);
    free(conn);
    return NULL;
}

void eb_disconnect(struct eb_connection **conn) {
    if (!conn || !*conn)
        return;

    if ((*conn)->addr)
        freeaddrinfo((*conn)->addr);
    if ((*conn)->fd >= 0)
        close((*conn)->fd);
    if ((*conn)->read_fd >= 0)
        close((*conn)->read_fd);
    free(*conn);
    *conn = NULL;
    return;
}
