/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteEth UDP RX frame synchronization: buffers aligned on the frames of the FPGA (RX header
 * sync word) through an unaligned start and lost datagrams. Datagrams sent over the loopback
 * interface.
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

#define PORT       42345
#define PKT_BYTES  1024
#define FRAME_PKTS 8
#define FRAME_BYTES (PKT_BYTES*FRAME_PKTS)
#define SYNC       0x5aa55aa55aa55aa5ULL

static int tx_sock;
static struct sockaddr_in tx_addr;

/* Datagram p of frame f: the first one starts with the sync word, then the frame number; every
 * datagram carries its (frame, packet) after that. */
static void send_packet(unsigned f, unsigned p)
{
    uint8_t pkt[PKT_BYTES];
    uint64_t sync = SYNC, frame = f;
    uint32_t tag[2] = {f, p};

    memset(pkt, 0x11, sizeof(pkt));
    if (p == 0) {
        memcpy(pkt, &sync, 8);
        memcpy(pkt + 8, &frame, 8);
    }
    memcpy(pkt + 16, tag, sizeof(tag));
    sendto(tx_sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&tx_addr, sizeof(tx_addr));
}

/* Next buffer: returns its frame number, or -1 without buffer; checks that it is one frame. */
static int next_frame(struct liteeth_udp_ctrl *u)
{
    uint8_t *buf;
    uint64_t sync, frame;

    liteeth_udp_process(u, 200);
    buf = liteeth_udp_next_read_buffer(u);
    if (!buf)
        return -1;
    memcpy(&sync, buf, 8);
    memcpy(&frame, buf + 8, 8);
    if (sync != SYNC)
        return -2;
    for (unsigned p = 0; p < FRAME_PKTS; p++) {
        uint32_t tag[2];
        memcpy(tag, buf + p*PKT_BYTES + 16, sizeof(tag));
        if (tag[0] != frame || tag[1] != p)
            return -3;
    }
    return (int)frame;
}

int main(void)
{
    struct liteeth_udp_ctrl u;
    int got[4];

    if (liteeth_udp_init(&u, "127.0.0.1", PORT, "127.0.0.1", PORT, 1, 0, FRAME_BYTES, 16, 1) < 0) {
        fprintf(stderr, "liteeth_udp_init failed\n");
        return 1;
    }
    liteeth_udp_set_rx_frame_sync(&u, 1, SYNC);

    tx_sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&tx_addr, 0, sizeof(tx_addr));
    tx_addr.sin_family = AF_INET;
    tx_addr.sin_port   = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &tx_addr.sin_addr);

    /* The reception starts inside frame 0: its end is dropped, frame 1 is whole. */
    for (unsigned p = 5; p < FRAME_PKTS; p++) send_packet(0, p);
    for (unsigned p = 0; p < FRAME_PKTS; p++) send_packet(1, p);
    /* Frame 2 loses a datagram in the middle, frame 3 its first one, frame 4 its last one.
     * Frames 2 and 3 together fill a buffer that is not a frame: it must not be given. */
    for (unsigned p = 0; p < FRAME_PKTS; p++) if (p != 3) send_packet(2, p);
    for (unsigned p = 1; p < FRAME_PKTS; p++) send_packet(3, p);
    for (unsigned p = 0; p < FRAME_PKTS - 1; p++) send_packet(4, p);
    /* Frames 5 and 6 are whole; frame 6 is given once the start of frame 7 is there. */
    for (unsigned p = 0; p < FRAME_PKTS; p++) send_packet(5, p);
    for (unsigned p = 0; p < FRAME_PKTS; p++) send_packet(6, p);
    send_packet(7, 0);
    usleep(20000);

    for (int i = 0; i < 4; i++)
        got[i] = next_frame(&u);

    close(tx_sock);
    if (got[0] != 1 || got[1] != 5 || got[2] != 6 || got[3] != -1) {
        fprintf(stderr, "test_liteeth_udp_frame_sync: got frames %d %d %d %d, expected 1 5 6 -1\n",
            got[0], got[1], got[2], got[3]);
        liteeth_udp_cleanup(&u);
        return 1;
    }
    if (u.rx_frame_resyncs != 2 || u.rx_frame_sync_drops == 0) {
        fprintf(stderr, "test_liteeth_udp_frame_sync: resyncs %llu (expected 2), drops %llu\n",
            (unsigned long long)u.rx_frame_resyncs, (unsigned long long)u.rx_frame_sync_drops);
        liteeth_udp_cleanup(&u);
        return 1;
    }
    liteeth_udp_cleanup(&u);
    printf("test_liteeth_udp_frame_sync: ok\n");
    return 0;
}
