/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteEth UDP listen port: two streams on a host (two boards, or two programs on a board) do
 * not share a port. The second one gets a free port, and each socket receives what is sent to
 * its own port. Datagrams sent over the loopback interface.
 *
 * This file is part of LiteX-M2SDR.
 */

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "liteeth_udp.h"

#define PORT      42346
#define PKT_BYTES 64

/* Sends one datagram of this value to a port, and tells which of the two sockets got it:
 * 0, 1, or -1 for none, -2 for both. */
static int deliver(int tx_sock, uint16_t port, uint8_t value, struct liteeth_udp_ctrl *u)
{
    struct sockaddr_in addr;
    uint8_t pkt[PKT_BYTES];
    int got = -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    memset(pkt, value, sizeof(pkt));
    if (sendto(tx_sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&addr, sizeof(addr)) != sizeof(pkt))
        return -1;
    usleep(20000);

    for (int i = 0; i < 2; i++) {
        uint8_t rx[PKT_BYTES];

        if (recv(u[i].sock, rx, sizeof(rx), MSG_DONTWAIT) == sizeof(rx) && rx[0] == value)
            got = (got == -1) ? i : -2;
    }
    return got;
}

int main(void)
{
    struct liteeth_udp_ctrl u[2];
    uint16_t port[2];
    int tx_sock, got[2];

    for (int i = 0; i < 2; i++) {
        if (liteeth_udp_init(&u[i], "127.0.0.1", PORT, "127.0.0.1", PORT, 1, 0, PKT_BYTES, 4, 0) < 0) {
            fprintf(stderr, "test_liteeth_udp_port: liteeth_udp_init %d failed\n", i);
            return 1;
        }
        port[i] = liteeth_udp_listen_port(&u[i]);
    }

    /* The first gets PORT, unless another run of the tests holds it; the second a free one. */
    if (port[0] == 0 || port[1] == 0 || port[1] == port[0] || port[1] == PORT) {
        fprintf(stderr, "test_liteeth_udp_port: ports %u %u (expected %u or a free one, then another free one)\n",
            port[0], port[1], PORT);
        return 1;
    }

    tx_sock = socket(AF_INET, SOCK_DGRAM, 0);
    got[0] = deliver(tx_sock, port[0], 0xa0, u);
    got[1] = deliver(tx_sock, port[1], 0xa1, u);
    close(tx_sock);
    liteeth_udp_cleanup(&u[0]);
    liteeth_udp_cleanup(&u[1]);

    if (got[0] != 0 || got[1] != 1) {
        fprintf(stderr, "test_liteeth_udp_port: datagrams received by %d %d (expected 0 1)\n",
            got[0], got[1]);
        return 1;
    }

    printf("test_liteeth_udp_port: ok\n");
    return 0;
}
