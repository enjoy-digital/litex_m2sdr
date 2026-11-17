/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR Ethernet TX flow control.
 *
 * Holds the host back on the level of the TX FIFO of the board, so that the stream is sent at
 * the rate of the sample clock. The board and the clock are reached through three functions:
 * the stream code gives the register and the clock of the host, the tests a model of the board.
 *
 * Private to libm2sdr.
 */

#ifndef M2SDR_LIB_ETH_FLOW_H
#define M2SDR_LIB_ETH_FLOW_H

#include <stddef.h>
#include <stdint.h>

#define M2SDR_ETH_FLOW_RATES 9          /* Readings of the rate that are kept. */

struct m2sdr_eth_flow_ops {
    /* Bytes in the TX FIFO now. Returns 0, or -1 when the level could not be read. */
    int    (*level)(void *ctx, uint32_t *bytes);
    /* Monotonic time in seconds, above zero. */
    double (*now)(void *ctx);
    void   (*sleep)(void *ctx, double seconds);
    void   *ctx;
};

struct m2sdr_eth_flow {
    struct m2sdr_eth_flow_ops ops;
    int      active;                       /* 0: off or not available, 1: the host is held back. */
    uint32_t fifo_bytes;                   /* Depth of the FIFO. */
    uint32_t level;                        /* Bytes in it at the last read. */
    double   poll_time;                    /* Time of that read, seconds; 0: none yet. */
    double   rate;                         /* Bytes per second that the board takes; 0: not known. */
    double   rates[M2SDR_ETH_FLOW_RATES];  /* The last readings of it. */
    unsigned rate_count;
    double   rate_last;                    /* The last reading. */
    double   rate_time;                    /* Read that the next reading is counted from: its time, */
    uint32_t rate_level;                   /* its level, */
    uint64_t rate_sent;                    /* and the bytes sent since. */
    uint64_t sent;                         /* Bytes sent since the last read. */
    double   queued;                       /* Bytes sent that have not left the host, at most, */
    double   queue_time;                   /* at this time. */
    uint64_t waits;                        /* Buffers held back. */
};

/* Start of a stream on a FIFO of `fifo_bytes`; 0 leaves the flow control off. */
void m2sdr_eth_flow_start(struct m2sdr_eth_flow *flow, const struct m2sdr_eth_flow_ops *ops, uint32_t fifo_bytes);
void m2sdr_eth_flow_stop(struct m2sdr_eth_flow *flow);
/* After a buffer of `bytes` is sent: returns when the FIFO has room for the next one. */
void m2sdr_eth_flow_wait(struct m2sdr_eth_flow *flow, size_t bytes);

#endif /* M2SDR_LIB_ETH_FLOW_H */
