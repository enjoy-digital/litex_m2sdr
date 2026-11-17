/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR Ethernet TX flow control.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_eth_flow.h"

#define ETH_FLOW_POLL_S     0.004     /* Between two reads of the FIFO level. */
#define ETH_FLOW_MIN_DT_S   0.002     /* Shortest interval that gives a rate. */
#define ETH_FLOW_MAX_WAIT_S 1.0       /* A buffer is sent after this long, room or not. */
#define ETH_FLOW_MIN_RATES  5         /* Readings of the rate before it is used. */
#define ETH_FLOW_HIGH       0.8       /* Part of the FIFO above which the host waits. */
#define ETH_FLOW_LINK_RATE  100e6     /* Bytes per second of the stream on the slowest link. */
#define ETH_FLOW_FULL       0.95      /* Part of the FIFO above which it is taken as full. */
#define ETH_FLOW_RATE_TOL   0.25      /* A reading this far from the rate: the rate is not relied on. */
#define ETH_FLOW_BLIND_WAIT_S 0.0005  /* Wait before a new read, when there is no rate. */
#define ETH_FLOW_READ_MAX_S 0.001     /* A read longer than this is asked again... */
#define ETH_FLOW_READ_TRIES 3         /* ...this many times at most. */

void m2sdr_eth_flow_start(struct m2sdr_eth_flow *flow, const struct m2sdr_eth_flow_ops *ops, uint32_t fifo_bytes)
{
    memset(flow, 0, sizeof(*flow));
    if (!ops || !ops->level || !ops->now || !ops->sleep || fifo_bytes == 0)
        return;
    flow->ops        = *ops;
    flow->fifo_bytes = fifo_bytes;
    flow->active     = 1;
}

void m2sdr_eth_flow_stop(struct m2sdr_eth_flow *flow)
{
    flow->active = 0;
}

static int eth_flow_cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* The level of the FIFO, with the time it was read at. A read that took long has no known
 * time (the process was held before, in, or after it): it is asked again. */
static int eth_flow_read(struct m2sdr_eth_flow *flow, uint32_t *level, double *now)
{
    int attempt, rc = -1;

    for (attempt = 0; attempt < ETH_FLOW_READ_TRIES; attempt++) {
        const double before = flow->ops.now(flow->ops.ctx);

        rc   = flow->ops.level(flow->ops.ctx, level);
        *now = flow->ops.now(flow->ops.ctx);
        if (rc == 0 && *now - before <= ETH_FLOW_READ_MAX_S)
            break;
    }
    return rc;
}

/* Bytes sent that have not reached the board at `now`: the socket and the queue of the host
 * hold what is sent faster than the link takes it, and a read of the level does not see it.
 * The link is taken at its slowest, so this is the most that can be on its way. */
static double eth_flow_queued(struct m2sdr_eth_flow *flow, double now, size_t bytes)
{
    if (flow->queue_time > 0 && now > flow->queue_time)
        flow->queued -= ETH_FLOW_LINK_RATE * (now - flow->queue_time);
    if (flow->queued < 0)
        flow->queued = 0;
    flow->queued    += (double)bytes;
    flow->queue_time = now;
    return flow->queued;
}

/* Level of the FIFO now, and from it and the bytes sent since an earlier read, the rate at
 * which the board empties it. Returns the time of the read.
 *
 * The rate kept is the median of the last readings: one reading is wrong when the board took
 * nothing for a while (its FIFO refills after it ran empty, a timed buffer waits for its time),
 * and a mean would follow it. */
static double eth_flow_poll(struct m2sdr_eth_flow *flow)
{
    uint32_t level = 0;
    double now = 0;

    if (eth_flow_read(flow, &level, &now) != 0)
        return now;
    /* What is on its way is as good as in the FIFO. */
    level += (uint32_t)eth_flow_queued(flow, now, 0);
    if (flow->rate_time <= 0) {
        flow->rate_time  = now;
        flow->rate_level = level;
        flow->rate_sent  = 0;
    } else if (now - flow->rate_time >= ETH_FLOW_MIN_DT_S) {
        /* Closer to the last reading than that, there is no rate to read. */
        const double taken = (double)flow->rate_level + (double)flow->rate_sent - (double)level;
        double sorted[M2SDR_ETH_FLOW_RATES];
        unsigned n;

        /* A FIFO seen full drops what is sent, and what it drops would count as taken. */
        if (taken > 0 && (double)level < ETH_FLOW_FULL * (double)flow->fifo_bytes)
            flow->rate_last = taken / (now - flow->rate_time);
        else
            flow->rate_last = 0.0;
        flow->rates[flow->rate_count % M2SDR_ETH_FLOW_RATES] = flow->rate_last;
        flow->rate_count++;
        n = flow->rate_count < M2SDR_ETH_FLOW_RATES ? flow->rate_count : M2SDR_ETH_FLOW_RATES;
        memcpy(sorted, flow->rates, n * sizeof(sorted[0]));
        qsort(sorted, n, sizeof(sorted[0]), eth_flow_cmp_double);
        /* The first readings count the bytes still on their way to the board as taken by it:
         * no rate until there are enough of them. */
        flow->rate = n >= ETH_FLOW_MIN_RATES ? sorted[n / 2] : 0.0;
        if (flow->rate < 1.0)
            flow->rate = 0;
        flow->rate_time  = now;
        flow->rate_level = level;
        flow->rate_sent  = 0;
    }
    flow->level     = level;
    flow->sent      = 0;
    flow->poll_time = now;
    return now;
}

/* The rate that the level is followed with between two reads: the rate of the board when its
 * last reading agrees with it, else none, and the stream is held on the reads alone. The board
 * takes nothing while its FIFO refills to the half after it ran empty or while a timed buffer
 * waits for its time, and its rate changes with the sample rate: counting on bytes that it does
 * not take would overflow its FIFO. */
static double eth_flow_drain(const struct m2sdr_eth_flow *flow)
{
    if (flow->rate <= 0 || fabs(flow->rate_last - flow->rate) > ETH_FLOW_RATE_TOL * flow->rate)
        return 0.0;
    return flow->rate;
}

/* Wait while the FIFO holds more than four fifths of its depth. The stream starts when it is
 * half full, a datagram is dropped when it is full, and what it holds is the time the host can
 * be late by: it is kept as full as the reads allow. */
void m2sdr_eth_flow_wait(struct m2sdr_eth_flow *flow, size_t bytes)
{
    double start, now, high;
    int held = 0;

    if (!flow->active)
        return;
    flow->sent      += bytes;
    flow->rate_sent += bytes;
    high  = ETH_FLOW_HIGH * (double)flow->fifo_bytes;
    start = flow->ops.now(flow->ops.ctx);
    now   = start;
    (void)eth_flow_queued(flow, now, bytes);
    for (;;) {
        double drain, level, wait;

        if (flow->poll_time <= 0 || now - flow->poll_time >= ETH_FLOW_POLL_S)
            now = eth_flow_poll(flow);
        drain = eth_flow_drain(flow);
        level = (double)flow->level + (double)flow->sent - drain * (now - flow->poll_time);
        if (level > high && drain <= 0 && now > flow->poll_time) {
            /* Nothing to follow the level with: it is read. */
            now   = eth_flow_poll(flow);
            drain = eth_flow_drain(flow);
            level = (double)flow->level + (double)flow->sent - drain * (now - flow->poll_time);
        }
        if (level <= high || now - start >= ETH_FLOW_MAX_WAIT_S)
            break;
        /* Until the level is back at the mark, or for a short time and a new read. */
        wait = drain > 0 ? (level - high) / drain : ETH_FLOW_BLIND_WAIT_S;
        if (wait < 0.0001)
            wait = 0.0001;
        if (wait > ETH_FLOW_POLL_S)
            wait = ETH_FLOW_POLL_S;
        flow->ops.sleep(flow->ops.ctx, wait);
        held = 1;
        now  = flow->ops.now(flow->ops.ctx);
    }
    if (held)
        flow->waits++;
}
