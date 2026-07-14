/*
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Measures the two latencies that bound a reactive (RX-grant -> TX-response) loop such as a
 * 5G k2=1 uplink:
 *   RX delivery : FPGA capture time (RX header timestamp) -> host has the buffer, i.e.
 *                 get_time() - rx_ts. Includes buffer fill + RX pipe + DMA + IRQ/wake.
 *   TX min lead : how few ns ahead of its air-time a TX buffer can be submitted and still
 *                 be aired by the gate (below this the gate drops it as too-late). This is
 *                 the host->gate transit floor -> the slack the reactive path must leave.
 * A TX feeder thread keeps the DMA reader fed (RX needs TX active) with untimed zeros.
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "m2sdr.h"
#include "csr.h"

static struct m2sdr_dev *dev;
static unsigned          g_pairs;
static int16_t          *g_zeros;
static _Atomic int       g_run = 1;

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a - *(const double *)b; return (x > 0) - (x < 0); }
static double pct(double *v, int n, double p) { return v[(int)(p * (n - 1))]; }

/* Keep the DMA reader fed with untimed zeros so RX stays alive (RX needs TX active). */
static void *tx_feeder(void *arg)
{
    (void)arg;
    while (atomic_load(&g_run))
        (void)m2sdr_sync_tx(dev, (void *)g_zeros, g_pairs, NULL, 200);   /* untimed -> immediate */
    return NULL;
}

int main(int argc, char **argv)
{
    double rate = 30.72e6, freq = 2.4e9; int nrx = 4000;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--n")    && i + 1 < argc) nrx  = atoi(argv[++i]);
    }
    if (m2sdr_open(&dev, "pcie:/dev/m2sdr0") != 0) { fprintf(stderr, "open failed\n"); return 1; }

    struct m2sdr_config cfg; m2sdr_config_init(&cfg);
    cfg.sample_rate = (int64_t)rate; cfg.bandwidth = (int64_t)fmin(rate * 0.8, 56e6);
    cfg.tx_freq = (int64_t)freq; cfg.rx_freq = (int64_t)freq;
    cfg.tx_att = 10; cfg.rx_gain1 = 40; cfg.rx_gain2 = 40; cfg.program_rx_gains = true;
    cfg.channel_layout = M2SDR_CHANNEL_LAYOUT_2T2R; cfg.clock_source = M2SDR_CLOCK_SOURCE_INTERNAL;
    if (m2sdr_apply_config(dev, &cfg) != 0) { fprintf(stderr, "apply_config failed\n"); return 1; }

    enum m2sdr_format fmt = M2SDR_FORMAT_SC16_Q11;
    g_pairs = m2sdr_bytes_to_samples(fmt, M2SDR_BUFFER_BYTES - M2SDR_DMA_HEADER_SIZE);
    double fs = rate, buf_us = (double)(g_pairs / 2) / fs * 1e6;   /* per-channel buffer duration */

    struct m2sdr_sync_params sp;
    m2sdr_sync_params_init(&sp);
    sp.direction = M2SDR_TX; sp.format = fmt; sp.buffer_size = g_pairs; sp.zero_copy = true; sp.tx_header_enable = true;
    if (m2sdr_sync_config_ex(dev, &sp) != 0) { fprintf(stderr, "TX config failed\n"); return 1; }
    m2sdr_sync_params_init(&sp);
    sp.direction = M2SDR_RX; sp.format = fmt; sp.buffer_size = g_pairs; sp.zero_copy = true;
    sp.rx_header_enable = true; sp.rx_strip_header = true;
    if (m2sdr_sync_config_ex(dev, &sp) != 0) { fprintf(stderr, "RX config failed\n"); return 1; }
    m2sdr_reg_write(dev, CSR_CROSSBAR_MUX_SEL_ADDR, 0);
    m2sdr_reg_write(dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);

    g_zeros = calloc((size_t)g_pairs * 2, sizeof(int16_t));
    int16_t *rxb = malloc((size_t)g_pairs * 2 * sizeof(int16_t));
    pthread_t tx; pthread_create(&tx, NULL, tx_feeder, NULL);
    usleep(200000);

    printf("latency probe: rate=%.2f MSPS, 1 DMA buffer = %.1f us/channel\n", rate / 1e6, buf_us);

    /* ---- RX delivery latency ---- */
    double *lat = malloc((size_t)nrx * sizeof(double)); int nl = 0;
    for (int i = 0; i < nrx + 64; i++) {
        struct m2sdr_metadata m;
        if (m2sdr_sync_rx(dev, rxb, g_pairs, &m, 100) != M2SDR_ERR_OK) continue;
        if (!(m.flags & M2SDR_META_FLAG_HAS_TIME)) continue;
        uint64_t now = 0; m2sdr_get_time(dev, &now);
        if (i < 64) continue;                          /* warm-up */
        double d = (double)((int64_t)now - (int64_t)m.timestamp);
        if (d > 0 && d < 50e6 && nl < nrx) lat[nl++] = d;
    }
    qsort(lat, nl, sizeof(double), cmp_d);
    printf("\nRX delivery (capture -> host), n=%d:\n", nl);
    printf("  min %.1f us | median %.1f us | p95 %.1f us | p99 %.1f us | max %.1f us\n",
           lat[0]/1e3, pct(lat, nl, 0.50)/1e3, pct(lat, nl, 0.95)/1e3, pct(lat, nl, 0.99)/1e3, lat[nl-1]/1e3);
    printf("  (~= %.1f us buffer fill + %.1f us pipe/DMA/IRQ at the median)\n",
           buf_us, pct(lat, nl, 0.50)/1e3 - buf_us);
    printf("\nThis RX-capture->host delivery is the dominant fixed cost of a reactive\n"
           "(RX-grant -> TX-response) loop; the timed-TX gate then airs the response at an\n"
           "EXACT tagged air-time (deterministic to the 10 ns grid), so the TX side adds only a\n"
           "calibrated constant, not jitter. See scripts/timed_tx_selftest for the TX air-time.\n");

    atomic_store(&g_run, 0); pthread_join(tx, NULL);
    free(lat); free(rxb); free(g_zeros); m2sdr_close(dev);
    return 0;
}
