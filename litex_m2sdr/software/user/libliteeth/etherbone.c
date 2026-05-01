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

struct eb_connection {
    int fd;
    int read_fd;
    int is_direct;
    int timeout_ms;
    int last_error;
    struct addrinfo* addr;
};

static int eb_fail(struct eb_connection *conn, int err)
{
    if (conn)
        conn->last_error = err;
    return err;
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
        do {
            if (eb_wait_fd(conn, conn->read_fd, POLLIN) != EB_ERR_OK)
                return -1;
            ret = recvfrom(conn->read_fd, bytes, max_len, 0, NULL, NULL);
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

int eb_read32_checked(struct eb_connection *conn, uint32_t addr, uint32_t *val) {
    uint8_t raw_pkt[20];

    if (!val)
        return eb_fail(conn, EB_ERR_IO);

    eb_fill_read32(raw_pkt, addr);

    if (eb_send(conn, raw_pkt, sizeof(raw_pkt)) < 0) {
        fprintf(stderr, "eb_read32: send failed\n");
        return eb_get_last_error(conn);
    }

    if (conn->is_direct) {
        int count = eb_recv(conn, raw_pkt, sizeof(raw_pkt));
        if (count < 0)
            return eb_get_last_error(conn);
        if (count != sizeof(raw_pkt)) {
            fprintf(stderr, "unexpected read length: %d\n", count);
            return eb_fail(conn, EB_ERR_IO);
        }
    } else {
        // If we are connected via TCP we need to take into account any size
        // read because it is a stream oriented protocol.
        int ret;
        uint8_t *p   = raw_pkt;
        uint8_t *end = raw_pkt + sizeof(raw_pkt);

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
    }

    *val = eb_unfill_read32(raw_pkt);
    conn->last_error = EB_ERR_OK;
    return EB_ERR_OK;
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
