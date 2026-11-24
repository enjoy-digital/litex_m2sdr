/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: 2x2 channel of an LTE downlink cell of two antenna ports, on RX1 and
 * RX2, from its cell-specific reference signals.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_dual.h"
#include "m2sdr_fft.h"
#include "m2sdr_lte_seq.h"

/* The samples and the products here are finite: complex products without the NaN and infinity
 * handling of C99 (__mulsc3). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("cx-limited-range")
#endif

/* Frame of TS 36.211 4.1 and 6.12 at 1.92 MSPS, normal cyclic prefix, as in m2sdr_analysis_lte.c. */
#define BASE_RATE        1920000.0
#define BASE_N           128
#define BASE_CP0         10
#define BASE_CP          9
#define BASE_SLOT        960
#define SLOTS            M2SDR_LTE_FRAME_SLOTS
#define SLOT_SYMBOLS     7
#define PORT1_COHERENCE  0.5     /* Without the MIB: port 1 is there when its pilots hold still from slot to slot. */

/* Singular values */
/*-----------------*/

void m2sdr_svd2(const float complex h[2][2], double s[2])
{
    /* s1^2 + s2^2 = |H|_F^2 and s1 s2 = |det H|. */
    double f = 0, d, disc;
    int r, c;

    for (r = 0; r < 2; r++) {
        for (c = 0; c < 2; c++)
            f += crealf(h[r][c]) * crealf(h[r][c]) + cimagf(h[r][c]) * cimagf(h[r][c]);
    }
    d    = cabs((double complex)h[0][0] * h[1][1] - (double complex)h[0][1] * h[1][0]);
    disc = 0.25 * f * f - d * d;
    disc = disc > 0 ? sqrt(disc) : 0.0;
    s[0] = sqrt(0.5 * f + disc);
    s[1] = 0.5 * f - disc > 0 ? sqrt(0.5 * f - disc) : 0.0;
}

static double correlation(float complex a0, float complex a1, float complex b0, float complex b1)
{
    const double na = crealf(a0) * crealf(a0) + cimagf(a0) * cimagf(a0) + crealf(a1) * crealf(a1) + cimagf(a1) * cimagf(a1);
    const double nb = crealf(b0) * crealf(b0) + cimagf(b0) * cimagf(b0) + crealf(b1) * crealf(b1) + cimagf(b1) * cimagf(b1);

    return na > 0 && nb > 0 ? cabsf(a0 * conjf(b0) + a1 * conjf(b1)) / sqrt(na * nb) : 0.0;
}

/* Pilots */
/*-------*/

/* Least-squares estimates of the reference signals of ports 0 and 1, per slot, block, channel
 * and port, summed: est[((ns * blocks + b) * 2 + rx) * 2 + port]. */
static int pilots(const float complex *const x[2], size_t frame, int d, double sample_rate, double cfo_hz, int cell_id,
                  int rb, int offset, int blocks, float complex *est, int *cnt)
{
    const int n = BASE_N * d, adv = 4 * d, k = 12 * rb, vshift = cell_id % 6;
    struct m2sdr_fft *fft = m2sdr_fft_alloc((size_t)n, false);
    float complex *in = malloc((size_t)n * sizeof(*in)), *out = malloc((size_t)n * sizeof(*out));
    const float complex rot = (float complex)cexp(-2.0 * M_PI * I * cfo_hz / sample_rate);
    float complex crs[2 * LTE_MAX_RB];
    int ns, li, rx, p, j, i;

    if (!fft || !in || !out) {
        m2sdr_fft_free(fft);
        free(in);
        free(out);
        return -1;
    }
    for (ns = 0; ns < SLOTS; ns++) {
        for (li = 0; li < 2; li++) {
            const int l = li ? 4 : 0;
            const size_t u = frame + (size_t)ns * BASE_SLOT * (size_t)d + (size_t)d * (size_t)(BASE_CP0 + l * (BASE_N + BASE_CP));
            const size_t w = u - (size_t)adv;

            m2sdr_lte_crs(cell_id, ns, l, crs);
            for (rx = 0; rx < 2; rx++) {
                /* The phase of the carrier offset follows the sample index, across the symbols. */
                float complex osc = (float complex)cexp(-2.0 * M_PI * I * cfo_hz * (double)(w - frame) / sample_rate);

                for (i = 0; i < n; i++) {
                    in[i] = x[rx][w + (size_t)i] * osc;
                    osc  *= rot;
                }
                m2sdr_fft_run(fft, in, out);
                for (p = 0; p < 2; p++) {
                    /* TS 36.211 6.10.1.2: port 0 at offset 0 in symbol 0 and 3 in symbol 4, port 1
                     * the other way round, both shifted by the cell. */
                    const int s = (((li ^ p) ? 3 : 0) + vshift) % 6;

                    for (j = 0; j < 2 * rb; j++) {
                        const int a = s + 6 * j, b = (a - offset) / 12;
                        const int sub = a < k / 2 ? a - k / 2 : a - k / 2 + 1;
                        /* The window starts early: the phase slope this gives is taken off. */
                        const float complex slope = (float complex)cexp(2.0 * M_PI * I * (double)sub * adv / n);
                        size_t e;

                        if (a < offset || b >= blocks)
                            continue;
                        e       = (((size_t)ns * (size_t)blocks + (size_t)b) * 2 + (size_t)rx) * 2 + (size_t)p;
                        est[e] += out[(sub + n) % n] * slope / (float)n * conjf(crs[j - rb + LTE_MAX_RB]);
                        cnt[e]++;
                    }
                }
            }
        }
    }
    m2sdr_fft_free(fft);
    free(in);
    free(out);
    return 0;
}

/* Analysis */
/*----------*/

int m2sdr_mimo_analyse(const float complex *x1, const float complex *x2, size_t count, double sample_rate,
                       struct m2sdr_mimo_result *res)
{
    const float complex *const x[2] = { x1, x2 };
    struct m2sdr_lte_result *lte;
    float complex *est = NULL, *mean = NULL;
    int *cnt = NULL;
    double noise = 0, signal = 0, port1 = 0, port1_slots = 0;
    int idx, d, rb, cell_rb, offset, blocks, ns, b, r, p, rc = -1;
    size_t cells, e;

    if (!x1 || !x2 || !res || !(sample_rate > 0))
        return -1;
    for (idx = 0; idx < 5; idx++) {
        if (fabs(sample_rate - BASE_RATE * (1 << idx)) < 1.0)
            break;
    }
    if (idx == 5)
        return -1;
    d   = 1 << idx;
    lte = malloc(sizeof(*lte));
    if (!lte)
        return -1;

    /* The cell, its frame and its carrier offset. */
    memset(res, 0, sizeof(*res));
    if (m2sdr_lte_analyse(x1, count, sample_rate, lte) != 0)
        goto out;
    if (!lte->found) {
        if (m2sdr_lte_analyse(x2, count, sample_rate, lte) != 0)
            goto out;
        res->rx = 1;
    }
    rc = 0;
    if (!lte->found || lte->analysed_rb <= 0)
        goto out;
    rb      = lte->analysed_rb;
    cell_rb = lte->mib && lte->n_rb > 0 ? lte->n_rb : rb;
    /* Blocks of the cell against the ones analysed: half a block apart when one has an odd
     * number of them and the other an even one. */
    offset  = cell_rb > rb ? (6 * (cell_rb - rb)) % 12 : 0;
    blocks  = (12 * rb - offset) / 12;
    cells   = (size_t)SLOTS * (size_t)blocks * 4;
    est     = calloc(cells, sizeof(*est));
    cnt     = calloc(cells, sizeof(*cnt));
    mean    = calloc((size_t)blocks * 4, sizeof(*mean));
    if (!est || !cnt || !mean) {
        rc = -1;
        goto out;
    }
    if (pilots(x, lte->frame_offset, d, sample_rate, lte->freq_error_hz, lte->cell_id, rb, offset, blocks, est, cnt) != 0) {
        rc = -1;
        goto out;
    }
    for (e = 0; e < cells; e++)
        est[e] = cnt[e] ? est[e] / (float)cnt[e] : 0;

    /* What is left of the carrier offset turns every estimate of a slot alike: each slot is
     * turned onto the mean of the frame, then the mean is taken again. */
    for (ns = 0; ns < SLOTS; ns++) {
        for (b = 0; b < 4 * blocks; b++)
            mean[b] += est[(size_t)ns * (size_t)blocks * 4 + (size_t)b] / (float)SLOTS;
    }
    for (ns = 0; ns < SLOTS; ns++) {
        float complex *row = est + (size_t)ns * (size_t)blocks * 4;
        double complex c = 0;

        for (b = 0; b < 4 * blocks; b++)
            c += row[b] * conjf(mean[b]);
        if (cabs(c) > 0) {
            const float complex turn = (float complex)(conj(c) / cabs(c));
            for (b = 0; b < 4 * blocks; b++)
                row[b] *= turn;
        }
    }
    memset(mean, 0, (size_t)blocks * 4 * sizeof(*mean));
    for (ns = 0; ns < SLOTS; ns++) {
        for (b = 0; b < 4 * blocks; b++)
            mean[b] += est[(size_t)ns * (size_t)blocks * 4 + (size_t)b] / (float)SLOTS;
    }

    /* Noise of an element: the spread of the estimates of a slot around the mean, times the
     * pilots each one averages. Port 1 is there when its estimates hold still. */
    for (b = 0; b < blocks; b++) {
        for (r = 0; r < 2; r++) {
            for (p = 0; p < 2; p++) {
                const size_t m = (size_t)(b * 2 + r) * 2 + (size_t)p;
                double v = 0, s2 = 0;

                for (ns = 0; ns < SLOTS; ns++) {
                    const size_t at = (size_t)ns * (size_t)blocks * 4 + m;
                    const float complex dv = est[at] - mean[m];

                    v  += (crealf(dv) * crealf(dv) + cimagf(dv) * cimagf(dv)) * cnt[at];
                    s2 += crealf(est[at]) * crealf(est[at]) + cimagf(est[at]) * cimagf(est[at]);
                }
                noise  += v / (SLOTS - 1);
                signal += crealf(mean[m]) * crealf(mean[m]) + cimagf(mean[m]) * cimagf(mean[m]);
                if (p == 1) {
                    port1       += (crealf(mean[m]) * crealf(mean[m]) + cimagf(mean[m]) * cimagf(mean[m])) * SLOTS;
                    port1_slots += s2;
                }
            }
        }
    }
    noise  /= 4.0 * blocks;
    signal /= 4.0 * blocks;

    res->found         = true;
    res->cell_id       = lte->cell_id;
    res->ports         = lte->mib ? lte->ports : 0;
    res->n_rb          = cell_rb;
    res->freq_error_hz = lte->freq_error_hz;
    res->frame_offset  = lte->frame_offset;
    res->rb            = blocks;
    res->first_rb      = cell_rb > rb ? (6 * (cell_rb - rb) + offset) / 12 : 0;
    res->snr_db        = noise > 0 ? m2sdr_power_db(signal / noise) : 200.0;
    res->mimo          = lte->mib ? lte->ports >= 2 : port1_slots > 0 && port1 / port1_slots > PORT1_COHERENCE;

    /* Per block. */
    {
        const double rho = noise > 0 ? 1.0 / noise : 1e20;
        double path[2][2] = { { 0, 0 }, { 0, 0 } }, sv[2] = { 0, 0 }, c1 = 0, c2 = 0;
        double cond = 0, cr = 0, ct = 0;

        for (b = 0; b < blocks; b++) {
            float complex (*h)[2] = res->h[b];
            double s[2];

            for (r = 0; r < 2; r++) {
                for (p = 0; p < 2; p++) {
                    const float complex v = mean[(size_t)(b * 2 + r) * 2 + (size_t)p];
                    const double pw = crealf(v) * crealf(v) + cimagf(v) * cimagf(v);

                    h[r][p]                   = v;
                    res->path_rb_db[b][r][p]  = (float)m2sdr_power_db(pw);
                    path[r][p]               += pw / blocks;
                }
            }
            if (!res->mimo)
                continue;
            m2sdr_svd2((const float complex (*)[2])h, s);
            res->sv_rb_db[b][0] = (float)m2sdr_power_db(s[0] * s[0]);
            res->sv_rb_db[b][1] = (float)m2sdr_power_db(s[1] * s[1]);
            res->cond_rb_db[b]  = (float)(s[1] > 0 ? 20.0 * log10(s[0] / s[1]) : 200.0);
            {
                /* One layer gets the power of both ports, two layers one port each. */
                const double one = log2(1.0 + 2.0 * rho * s[0] * s[0]);
                const double two = log2(1.0 + rho * s[0] * s[0]) + log2(1.0 + rho * s[1] * s[1]);

                res->rank_rb[b] = two > one ? 2 : 1;
                res->rank2_rb  += two > one;
                c1 += one / blocks;
                c2 += two / blocks;
            }
            sv[0] += s[0] * s[0] / blocks;
            sv[1] += s[1] * s[1] / blocks;
            cond  += res->cond_rb_db[b] / blocks;
            if (res->cond_rb_db[b] > res->cond_max_db)
                res->cond_max_db = res->cond_rb_db[b];
            cr += correlation(h[0][0], h[0][1], h[1][0], h[1][1]) / blocks;
            ct += correlation(h[0][0], h[1][0], h[0][1], h[1][1]) / blocks;
        }
        for (r = 0; r < 2; r++) {
            for (p = 0; p < 2; p++)
                res->path_db[r][p] = m2sdr_power_db(path[r][p]);
        }
        if (res->mimo) {
            res->sv_db[0]       = m2sdr_power_db(sv[0]);
            res->sv_db[1]       = m2sdr_power_db(sv[1]);
            res->cond_db        = cond;
            res->capacity[0]    = c1;
            res->capacity[1]    = c2;
            res->rank           = c2 > c1 ? 2 : 1;
            res->correlation_rx = cr;
            res->correlation_tx = ct;
        } else {
            res->rank = 1;
        }
    }

out:
    free(lte);
    free(est);
    free(cnt);
    free(mean);
    return rc;
}
