/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the Ethernet TX flow control, run with the real m2sdr_eth_flow.c against a model of
 * the board in simulated time: a FIFO that the board starts to empty at the sample rate once it
 * is half full, that drops what does not fit and that stops when it runs empty, as the gateware
 * does. The host of the model is held back by the flow control alone: what it sends waits in
 * its queue for the link, its reads of the level take time and fail, and the process is held
 * now and then.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_eth_flow.h"

#define FIFO_BYTES (256u * 1024u)   /* TX FIFO of the gateware. */
#define BUF_BYTES  8192u            /* One buffer of the stream. */
#define WIRE_RATE  117e6            /* Bytes per second of the stream on a 1 Gbit/s link. */
#define QUEUE_BYTES (128u * 1024u)  /* What the socket and the queue of the host hold. */
#define SEND_S     4e-6             /* Time of the host to send a buffer when its queue has room. */
#define STEP_S     10e-6            /* Step of the model. */

struct model {
    /* Board. */
    double   rate;          /* Bytes per second taken from the FIFO when it runs. */
    double   level;
    int      running;
    uint64_t underflows;    /* Times the FIFO ran empty. */
    double   dropped;       /* Bytes that did not fit. */
    double   taken;         /* Bytes that the board took. */

    /* Host. */
    double   queue;         /* Bytes sent that have not left the host. */
    double   model_time;    /* Time that the board and the queue are at. */
    double   time;
    double   read_s;        /* Time of a read of the level. */
    double   read_fail;     /* Part of the reads that fail. */
    double   hold_prob;     /* Part of the reads and sleeps after which the process is held... */
    double   hold_s;        /* ...for up to this long. */
    double   sleep_late_s;  /* A sleep is late by up to this long. */
    uint32_t seed;
    uint64_t reads;
    double   longest_sleep;
};

static double model_rand(struct model *m)
{
    m->seed = m->seed * 1664525u + 1013904223u;
    return (double)(m->seed >> 8) / 16777216.0;
}

/* The queue of the host empties into the FIFO at the rate of the link, the board empties the
 * FIFO at its rate: both brought to the time of the host. */
static void model_advance(struct model *m)
{
    while (m->model_time < m->time) {
        double step = m->time - m->model_time;
        double moved;

        if (step > STEP_S)
            step = STEP_S;
        m->model_time += step;
        moved = WIRE_RATE * step;
        if (moved > m->queue)
            moved = m->queue;
        m->queue -= moved;
        if (m->level + moved > (double)FIFO_BYTES) {
            m->dropped += m->level + moved - (double)FIFO_BYTES;
            m->level    = (double)FIFO_BYTES;
        } else {
            m->level += moved;
        }
        if (!m->running && m->rate > 0 && m->level >= FIFO_BYTES / 2)
            m->running = 1;
        if (m->running) {
            if (m->rate * step >= m->level) {
                m->taken  += m->level;
                m->level   = 0;
                m->running = 0;
                m->underflows++;
            } else {
                m->taken += m->rate * step;
                m->level -= m->rate * step;
            }
        }
    }
    m->model_time = m->time;
}

static uint64_t model_drops(const struct model *m)
{
    return (uint64_t)ceil(m->dropped / BUF_BYTES);
}

static void model_hold(struct model *m)
{
    if (m->hold_prob > 0 && model_rand(m) < m->hold_prob)
        m->time += m->hold_s * model_rand(m);
}

/* A buffer goes to the queue of the host, at once when there is room for it. */
static void model_send(struct model *m, size_t bytes)
{
    m->time += SEND_S;
    model_advance(m);
    while (m->queue + (double)bytes > (double)QUEUE_BYTES) {
        m->time += STEP_S;
        model_advance(m);
    }
    m->queue += (double)bytes;
}

static int model_level(void *ctx, uint32_t *bytes)
{
    struct model *m = ctx;

    m->reads++;
    m->time += m->read_s;
    if (m->read_fail > 0 && model_rand(m) < m->read_fail)
        return -1;
    model_advance(m);
    *bytes = (uint32_t)(m->level / 8.0) * 8u;
    model_hold(m);   /* Between the read and its time stamp. */
    return 0;
}

static double model_now(void *ctx)
{
    struct model *m = ctx;

    return m->time;
}

static void model_sleep(void *ctx, double seconds)
{
    struct model *m = ctx;

    if (seconds > m->longest_sleep)
        m->longest_sleep = seconds;
    m->time += seconds + m->sleep_late_s * model_rand(m);
    model_hold(m);
}

static void model_init(struct model *m, struct m2sdr_eth_flow *flow, double rate)
{
    const struct m2sdr_eth_flow_ops ops = {
        .level = model_level,
        .now   = model_now,
        .sleep = model_sleep,
        .ctx   = m,
    };

    memset(m, 0, sizeof(*m));
    m->rate         = rate;
    m->time         = 100.0;
    m->model_time   = m->time;
    m->read_s       = 0.0003;
    m->sleep_late_s = 0.00008;
    m->seed         = 1;
    m2sdr_eth_flow_start(flow, &ops, FIFO_BYTES);
}

/* Sends buffers for `seconds` of the model. */
static void model_run(struct model *m, struct m2sdr_eth_flow *flow, double seconds)
{
    const double end = m->time + seconds;

    while (m->time < end) {
        model_send(m, BUF_BYTES);
        m2sdr_eth_flow_wait(flow, BUF_BYTES);
    }
}

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        failures++; \
    } \
} while (0)

/* Bytes per second of a 2T2R SC16 stream. */
static double stream_rate(double sample_rate)
{
    return sample_rate * 8.0;
}

/* A quiet host: nothing dropped, the FIFO never empty, the stream at the rate of the board. */
static void test_steady(void)
{
    static const double sample_rates[] = { 0.24e6, 1.92e6, 7.68e6, 11.52e6 };
    unsigned i;

    for (i = 0; i < sizeof(sample_rates) / sizeof(sample_rates[0]); i++) {
        const double rate = stream_rate(sample_rates[i]);
        struct m2sdr_eth_flow flow;
        struct model m;
        double start, taken;

        model_init(&m, &flow, rate);
        model_run(&m, &flow, 2.0);
        start = m.time;
        taken = m.taken;
        model_run(&m, &flow, 30.0);
        CHECK(model_drops(&m) == 0, "%.2f MSPS: %llu drops", sample_rates[i] / 1e6, (unsigned long long)model_drops(&m));
        CHECK(m.underflows == 0, "%.2f MSPS: %llu underflows", sample_rates[i] / 1e6, (unsigned long long)m.underflows);
        CHECK(fabs(flow.rate / rate - 1.0) < 0.02, "%.2f MSPS: rate %.0f for %.0f", sample_rates[i] / 1e6, flow.rate, rate);
        CHECK(fabs((m.taken - taken) / (rate * (m.time - start)) - 1.0) < 1e-3,
            "%.2f MSPS: %.0f bytes taken in %.3f s", sample_rates[i] / 1e6, m.taken - taken, m.time - start);
        CHECK(flow.waits > 0, "%.2f MSPS: the host was never held back", sample_rates[i] / 1e6);
        CHECK(m.level > 0.5 * FIFO_BYTES, "%.2f MSPS: FIFO at %.0f bytes", sample_rates[i] / 1e6, m.level);
        /* A read every few milliseconds, not one per buffer. */
        CHECK((double)m.reads < 32.0 * 400.0, "%.2f MSPS: %llu reads", sample_rates[i] / 1e6, (unsigned long long)m.reads);
    }
}

/* The start: the FIFO fills and the stream starts with nothing dropped, whatever the rate. */
static void test_start(void)
{
    static const double sample_rates[] = { 0.24e6, 1.92e6, 7.68e6, 11.52e6 };
    unsigned i;

    for (i = 0; i < sizeof(sample_rates) / sizeof(sample_rates[0]); i++) {
        struct m2sdr_eth_flow flow;
        struct model m;

        model_init(&m, &flow, stream_rate(sample_rates[i]));
        model_run(&m, &flow, 0.5);
        CHECK(m.running, "%.2f MSPS: the board did not start", sample_rates[i] / 1e6);
        CHECK(model_drops(&m) == 0, "%.2f MSPS: %llu drops", sample_rates[i] / 1e6, (unsigned long long)model_drops(&m));
        CHECK(m.underflows == 0, "%.2f MSPS: %llu underflows", sample_rates[i] / 1e6, (unsigned long long)m.underflows);
    }
}

/* Reads that fail: the level is followed from the rate until one answers. */
static void test_failed_reads(void)
{
    const double rate = stream_rate(7.68e6);
    struct m2sdr_eth_flow flow;
    struct model m;

    model_init(&m, &flow, rate);
    m.read_fail = 0.3;
    model_run(&m, &flow, 30.0);
    CHECK(model_drops(&m) == 0, "%llu drops", (unsigned long long)model_drops(&m));
    CHECK(m.underflows == 0, "%llu underflows", (unsigned long long)m.underflows);
    CHECK(fabs(flow.rate / rate - 1.0) < 0.02, "rate %.0f for %.0f", flow.rate, rate);
}

/* A process held now and then, between a read and its time stamp or after a sleep, for less
 * than the FIFO holds: the readings of the rate that this spoils are not followed. */
static void test_held_process(void)
{
    const double rate = stream_rate(1.92e6);   /* The FIFO holds 17 ms. */
    struct m2sdr_eth_flow flow;
    struct model m;

    model_init(&m, &flow, rate);
    m.hold_prob    = 0.02;
    m.hold_s       = 0.005;
    m.sleep_late_s = 0.0005;
    model_run(&m, &flow, 60.0);
    CHECK(model_drops(&m) == 0, "%llu drops", (unsigned long long)model_drops(&m));
    CHECK(m.underflows == 0, "%llu underflows", (unsigned long long)m.underflows);
    CHECK(fabs(flow.rate / rate - 1.0) < 0.1, "rate %.0f for %.0f", flow.rate, rate);
}

/* A process held for longer than the FIFO holds: the FIFO runs empty, and the stream comes
 * back with nothing dropped. */
static void test_underflow_recovery(void)
{
    const double rate = stream_rate(7.68e6);
    struct m2sdr_eth_flow flow;
    struct model m;
    uint64_t underflows;

    model_init(&m, &flow, rate);
    model_run(&m, &flow, 5.0);
    CHECK(m.underflows == 0, "%llu underflows before the hold", (unsigned long long)m.underflows);
    m.time += 0.05;
    model_run(&m, &flow, 1.0);
    CHECK(m.underflows == 1, "%llu underflows after the hold", (unsigned long long)m.underflows);
    underflows = m.underflows;
    model_run(&m, &flow, 10.0);
    CHECK(model_drops(&m) == 0, "%llu drops", (unsigned long long)model_drops(&m));
    CHECK(m.underflows == underflows, "%llu underflows after the recovery", (unsigned long long)(m.underflows - underflows));
    CHECK(fabs(flow.rate / rate - 1.0) < 0.02, "rate %.0f for %.0f", flow.rate, rate);
}

/* The sample rate changes under the stream: the new rate is followed. */
static void test_rate_change(void)
{
    struct m2sdr_eth_flow flow;
    struct model m;
    uint64_t underflows, drops;

    model_init(&m, &flow, stream_rate(1.92e6));
    model_run(&m, &flow, 5.0);
    m.rate = stream_rate(3.84e6);
    model_run(&m, &flow, 1.0);
    underflows = m.underflows;
    model_run(&m, &flow, 10.0);
    CHECK(model_drops(&m) == 0, "%llu drops", (unsigned long long)model_drops(&m));
    CHECK(m.underflows == underflows, "%llu underflows at the new rate", (unsigned long long)(m.underflows - underflows));
    CHECK(fabs(flow.rate / m.rate - 1.0) < 0.02, "rate %.0f for %.0f", flow.rate, m.rate);

    /* To a lower rate: until a read shows it, the host counts on the bytes of the old rate. */
    m.rate = stream_rate(0.48e6);
    model_run(&m, &flow, 1.0);
    CHECK(model_drops(&m) <= (uint64_t)(stream_rate(3.84e6) * 0.006 / BUF_BYTES), "%llu drops at the change",
        (unsigned long long)model_drops(&m));
    drops      = model_drops(&m);
    underflows = m.underflows;
    model_run(&m, &flow, 10.0);
    CHECK(model_drops(&m) == drops, "%llu drops at the low rate", (unsigned long long)(model_drops(&m) - drops));
    CHECK(m.underflows == underflows, "%llu underflows at the low rate", (unsigned long long)(m.underflows - underflows));
    CHECK(fabs(flow.rate / m.rate - 1.0) < 0.02, "rate %.0f for %.0f", flow.rate, m.rate);
}

/* A board that takes nothing (a timed buffer that waits for its time): the host is held, a
 * buffer goes after the longest wait, and the stream is followed again when the board starts. */
static void test_stalled_board(void)
{
    const double rate = stream_rate(1.92e6);
    struct m2sdr_eth_flow flow;
    struct model m;
    double start;
    uint64_t drops;

    model_init(&m, &flow, 0.0);
    start = m.time;
    while (m.level + m.queue + BUF_BYTES <= 0.8 * FIFO_BYTES) {
        model_send(&m, BUF_BYTES);
        m2sdr_eth_flow_wait(&flow, BUF_BYTES);
    }
    CHECK(m.time - start < 0.1, "%.3f s to fill the FIFO", m.time - start);
    start = m.time;
    model_send(&m, BUF_BYTES);
    m2sdr_eth_flow_wait(&flow, BUF_BYTES);
    CHECK(m.time - start >= 1.0 && m.time - start < 1.05, "a buffer held for %.3f s", m.time - start);
    CHECK(model_drops(&m) == 0, "%llu drops", (unsigned long long)model_drops(&m));
    CHECK(m.longest_sleep <= 0.0041, "a sleep of %.4f s", m.longest_sleep);

    m.rate    = rate;
    m.running = 1;
    model_run(&m, &flow, 1.0);
    drops = model_drops(&m);
    model_run(&m, &flow, 10.0);
    CHECK(model_drops(&m) == drops, "%llu drops once started", (unsigned long long)(model_drops(&m) - drops));
    CHECK(fabs(flow.rate / rate - 1.0) < 0.02, "rate %.0f for %.0f", flow.rate, rate);
}

/* A board that stops in the stream: until a read shows it, the host counts on bytes that the
 * board does not take, and what it sent on that account is dropped. No more than that is, and
 * the dropped buffers are not taken for a board that runs. */
static void test_stall_in_stream(void)
{
    static const double sample_rates[] = { 0.24e6, 1.92e6, 7.68e6 };
    unsigned i;

    for (i = 0; i < sizeof(sample_rates) / sizeof(sample_rates[0]); i++) {
        const double rate = stream_rate(sample_rates[i]);
        /* The bytes of the time between two reads, and the buffers sent after the longest wait. */
        const uint64_t most = (uint64_t)(rate * 0.006 / BUF_BYTES) + 4;
        struct m2sdr_eth_flow flow;
        struct model m;
        uint64_t drops;

        model_init(&m, &flow, rate);
        model_run(&m, &flow, 5.0);
        m.rate    = 0;
        m.running = 0;
        model_run(&m, &flow, 3.0);
        CHECK(model_drops(&m) <= most, "%.2f MSPS: %llu drops for %llu at most", sample_rates[i] / 1e6,
            (unsigned long long)model_drops(&m), (unsigned long long)most);
        CHECK(flow.rate == 0, "%.2f MSPS: rate %.0f of a stopped board", sample_rates[i] / 1e6, flow.rate);

        m.rate    = rate;
        m.running = 1;
        model_run(&m, &flow, 1.0);
        drops = model_drops(&m);
        model_run(&m, &flow, 10.0);
        CHECK(model_drops(&m) == drops, "%.2f MSPS: %llu drops once started again", sample_rates[i] / 1e6,
            (unsigned long long)(model_drops(&m) - drops));
        CHECK(fabs(flow.rate / rate - 1.0) < 0.02, "%.2f MSPS: rate %.0f for %.0f", sample_rates[i] / 1e6, flow.rate, rate);
    }
}

/* Off: no depth, no functions, or stopped. Nothing is read and nothing waits. */
static void test_off(void)
{
    struct m2sdr_eth_flow flow;
    struct model m;
    double start;

    model_init(&m, &flow, stream_rate(1.92e6));
    m2sdr_eth_flow_stop(&flow);
    start = m.time;
    model_run(&m, &flow, 0.01);
    CHECK(m.reads == 0 && flow.waits == 0, "%llu reads when stopped", (unsigned long long)m.reads);
    CHECK(model_drops(&m) > 0, "the model dropped nothing with the flow control off");
    CHECK(m.time - start < 0.011, "%.3f s", m.time - start);

    m2sdr_eth_flow_start(&flow, &flow.ops, 0);
    CHECK(!flow.active, "active with no depth");
    m2sdr_eth_flow_start(&flow, NULL, FIFO_BYTES);
    CHECK(!flow.active, "active with no functions");
    m2sdr_eth_flow_wait(&flow, BUF_BYTES);
}

int main(void)
{
    test_steady();
    test_start();
    test_failed_reads();
    test_held_process();
    test_underflow_recovery();
    test_rate_change();
    test_stalled_board();
    test_stall_in_stream();
    test_off();
    if (failures) {
        printf("test_m2sdr_eth_flow: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_m2sdr_eth_flow: ok\n");
    return 0;
}
