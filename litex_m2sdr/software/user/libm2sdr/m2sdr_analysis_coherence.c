/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: cross spectrum and coherence of the two receive channels.
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

/* The samples and the products here are finite: complex products without the NaN and infinity
 * handling of C99 (__mulsc3). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("cx-limited-range")
#endif

#define MIN_SEGMENTS       8
#define DEFAULT_FFT_LEN    1024
#define DEFAULT_MIN_MSC    0.5
#define MIN_COHERENT_BINS  3       /* A tone fills its main lobe: three bins of a Hann window. */
#define DELAY_MIN_SPAN     16      /* Bins between the lowest and the highest coherent one, for a delay. */
#define DC_BINS            1       /* On each side of the DC bin, left out. */
#define MAX_WEIGHT         1e6     /* Of a bin of coherence near 1. */

/* Cross spectrum */
/*----------------*/

void m2sdr_cross_free(struct m2sdr_cross *cs)
{
    if (!cs)
        return;
    m2sdr_fft_free(cs->fft);
    free(cs->s11);
    free(cs->s22);
    free(cs->s21);
    free(cs->win);
    free(cs->work);
    memset(cs, 0, sizeof(*cs));
}

int m2sdr_cross_compute(const float complex *x1, const float complex *x2, size_t count, double sample_rate,
                        size_t fft_len, struct m2sdr_cross *cs)
{
    const size_t hop = fft_len / 2;
    float complex *in, *y1, *y2;
    double sum = 0, scale;
    size_t i, start;
    unsigned segments = 0;

    if (!x1 || !x2 || !cs || fft_len < 8 || (fft_len & (fft_len - 1)) || count < fft_len || !(sample_rate > 0))
        return -1;

    /* Plan, window and buffers: kept while the length stays. */
    if (cs->bins != fft_len || !cs->fft) {
        m2sdr_cross_free(cs);
        cs->fft  = m2sdr_fft_alloc(fft_len, false);
        cs->s11  = malloc(fft_len * sizeof(*cs->s11));
        cs->s22  = malloc(fft_len * sizeof(*cs->s22));
        cs->s21  = malloc(fft_len * sizeof(*cs->s21));
        cs->win  = malloc(fft_len * sizeof(*cs->win));
        cs->work = malloc(3 * fft_len * sizeof(*cs->work));
        if (!cs->fft || !cs->s11 || !cs->s22 || !cs->s21 || !cs->win || !cs->work) {
            m2sdr_cross_free(cs);
            return -1;
        }
        cs->bins = fft_len;
        for (i = 0; i < fft_len; i++)
            cs->win[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)fft_len));
    }
    for (i = 0; i < fft_len; i++)
        sum += cs->win[i];
    in = cs->work;
    y1 = cs->work + fft_len;
    y2 = cs->work + 2 * fft_len;
    memset(cs->s11, 0, fft_len * sizeof(*cs->s11));
    memset(cs->s22, 0, fft_len * sizeof(*cs->s22));
    memset(cs->s21, 0, fft_len * sizeof(*cs->s21));

    for (start = 0; start + fft_len <= count; start += hop) {
        for (i = 0; i < fft_len; i++)
            in[i] = x1[start + i] * cs->win[i];
        m2sdr_fft_run(cs->fft, in, y1);
        for (i = 0; i < fft_len; i++)
            in[i] = x2[start + i] * cs->win[i];
        m2sdr_fft_run(cs->fft, in, y2);
        /* Bins shifted: the lowest frequency first. */
        for (i = 0; i < fft_len; i++) {
            const size_t k = (i + fft_len / 2) & (fft_len - 1);
            const float complex a = y1[k], b = y2[k];

            cs->s11[i] += crealf(a) * crealf(a) + cimagf(a) * cimagf(a);
            cs->s22[i] += crealf(b) * crealf(b) + cimagf(b) * cimagf(b);
            cs->s21[i] += b * conjf(a);
        }
        segments++;
    }
    /* A full-scale tone on a bin: |X|^2 = (sum of the window)^2. */
    scale = 1.0 / (sum * sum * (double)segments);
    for (i = 0; i < fft_len; i++) {
        cs->s11[i] = (float)(cs->s11[i] * scale);
        cs->s22[i] = (float)(cs->s22[i] * scale);
        cs->s21[i] = cs->s21[i] * (float)scale;
    }
    cs->sample_rate = sample_rate;
    cs->bin_hz      = sample_rate / (double)fft_len;
    cs->segments    = segments;
    return 0;
}

/* Coherence */
/*-----------*/

static double wrap_rad(double a)
{
    return a - 2.0 * M_PI * floor((a + M_PI) / (2.0 * M_PI));
}

/* Coarse delay, in samples: peak of the inverse transform of the phase of the coherent bins
 * (PHAT), interpolated, within a quarter of the FFT length. */
static int coarse_delay(const struct m2sdr_cross *cs, const unsigned char *use, double *delay)
{
    const size_t n = cs->bins;
    struct m2sdr_fft *ifft = m2sdr_fft_alloc(n, true);
    float complex *z = calloc(n, sizeof(*z)), *r = malloc(n * sizeof(*r));
    double best = -1;
    long at = 0, t;
    size_t k;

    if (!ifft || !z || !r) {
        m2sdr_fft_free(ifft);
        free(z);
        free(r);
        return -1;
    }
    for (k = 0; k < n; k++) {
        const float m = cabsf(cs->s21[k]);

        if (use[k] && m > 0)
            z[(k + n / 2) & (n - 1)] = cs->s21[k] / m;
    }
    /* S21 turns as exp(-j 2 pi f tau / n): its inverse transform peaks at tau. */
    m2sdr_fft_run(ifft, z, r);
    for (t = -(long)(n / 4); t <= (long)(n / 4); t++) {
        const double v = cabsf(r[(size_t)(t + (long)n) & (n - 1)]);
        if (v > best) {
            best = v;
            at   = t;
        }
    }
    {
        const double a = cabsf(r[(size_t)(at - 1 + (long)n) & (n - 1)]);
        const double b = best;
        const double c = cabsf(r[(size_t)(at + 1 + (long)n) & (n - 1)]);
        const double den = a - 2.0 * b + c;

        *delay = (double)at + (den < 0 ? 0.5 * (a - c) / den : 0.0);
    }
    m2sdr_fft_free(ifft);
    free(z);
    free(r);
    return 0;
}

int m2sdr_coherence_analyse(const float complex *x1, const float complex *x2, size_t count, double sample_rate,
                            const struct m2sdr_coherence_config *cfg, struct m2sdr_coherence_result *res)
{
    struct m2sdr_cross cs;
    const size_t fft_len = cfg && cfg->fft_len ? cfg->fft_len : DEFAULT_FFT_LEN;
    const double min_msc = cfg && cfg->min_coherence > 0 ? cfg->min_coherence : DEFAULT_MIN_MSC;
    double lo = -sample_rate / 2, hi = sample_rate / 2;
    double sum_msc = 0, sum_used = 0, p1 = 0, p2 = 0, u1 = 0, u2 = 0;
    unsigned char *use = NULL;
    double *weight = NULL;
    int k, k_lo = -1, k_hi = -1, n, rc = -1;

    if (!x1 || !x2 || !res || !(sample_rate > 0) || fft_len < 16 || fft_len > M2SDR_COHERENCE_MAX_BINS ||
        (fft_len & (fft_len - 1)) || count < (MIN_SEGMENTS + 1) * fft_len / 2)
        return -1;
    if (cfg && (cfg->lo_hz != 0 || cfg->hi_hz != 0)) {
        if (!(cfg->hi_hz > cfg->lo_hz))
            return -1;
        lo = cfg->lo_hz;
        hi = cfg->hi_hz;
    }
    memset(&cs, 0, sizeof(cs));
    if (m2sdr_cross_compute(x1, x2, count, sample_rate, fft_len, &cs) != 0)
        return -1;
    n      = (int)fft_len;
    use    = calloc(fft_len, sizeof(*use));
    weight = calloc(fft_len, sizeof(*weight));
    if (!use || !weight)
        goto out;
    memset(res, 0, sizeof(*res));
    res->bins     = n;
    res->bin_hz   = cs.bin_hz;
    res->segments = cs.segments;

    /* Per bin. */
    for (k = 0; k < n; k++) {
        const double a = cs.s11[k], b = cs.s22[k];
        const double c = crealf(cs.s21[k]) * crealf(cs.s21[k]) + cimagf(cs.s21[k]) * cimagf(cs.s21[k]);
        const double f = m2sdr_cross_freq(&cs, (size_t)k);
        const double msc = a > 0 && b > 0 ? c / (a * b) : 0.0;

        res->msc[k]           = (float)msc;
        res->gain_bin_db[k]   = (float)(m2sdr_power_db(b) - m2sdr_power_db(a));
        res->phase_bin_deg[k] = (float)(cargf(cs.s21[k]) * 180.0 / M_PI);
        res->rx1_bin_db[k]    = (float)m2sdr_power_db(a);
        res->rx2_bin_db[k]    = (float)m2sdr_power_db(b);
        if (f < lo || f > hi)
            continue;
        res->band_bins++;
        sum_msc += msc;
        p1      += a;
        p2      += b;
        if (abs(k - n / 2) <= DC_BINS || msc < min_msc)
            continue;
        use[k] = 1;
        /* Inverse of the variance of the phase of the bin (Bendat and Piersol). */
        weight[k] = msc < 1.0 ? 2.0 * cs.segments * msc / (1.0 - msc) : MAX_WEIGHT;
        if (weight[k] > MAX_WEIGHT)
            weight[k] = MAX_WEIGHT;
        res->coherent_bins++;
        sum_used += msc;
        u1       += a;
        u2       += b;
        if (k_lo < 0)
            k_lo = k;
        k_hi = k;
    }
    if (!res->band_bins) {
        rc = -1;
        goto out;
    }
    res->coherence = sum_msc / res->band_bins;
    /* Band power: sum of the bins over the noise bandwidth of the Hann window, 1.5 bins. */
    res->rx1_db = m2sdr_power_db(p1 / 1.5);
    res->rx2_db = m2sdr_power_db(p2 / 1.5);
    rc = 0;
    if (res->coherent_bins < MIN_COHERENT_BINS)
        goto out;
    res->found          = true;
    res->coherence_used = sum_used / res->coherent_bins;
    res->gain_db        = m2sdr_power_db(u2) - m2sdr_power_db(u1);
    res->delay_valid    = k_hi - k_lo >= DELAY_MIN_SPAN && res->coherent_bins >= 2 * MIN_COHERENT_BINS;

    /* Delay and phase: a coarse delay, then a line fitted to the phase left, twice. */
    {
        double tau = 0, phi = 0;
        int pass;

        if (res->delay_valid && coarse_delay(&cs, use, &tau) != 0) {
            rc = -1;
            goto out;
        }
        for (pass = 0; pass < (res->delay_valid ? 2 : 1); pass++) {
            double complex acc = 0;
            double sw = 0, sf = 0, sp = 0, sff = 0, sfp = 0;

            /* Mean phase first: the phases fitted are then small, away from the wrap. */
            for (k = 0; k < n; k++) {
                if (use[k])
                    acc += weight[k] * cs.s21[k] * cexp(2.0 * M_PI * I * (k - n / 2) * tau / n) /
                           (cabsf(cs.s21[k]) + 1e-30f);
            }
            phi = carg(acc);
            if (!res->delay_valid)
                break;
            for (k = 0; k < n; k++) {
                double f, p;

                if (!use[k])
                    continue;
                f    = (double)(k - n / 2);
                p    = carg(cs.s21[k] * cexp(2.0 * M_PI * I * f * tau / n - I * phi));
                sw  += weight[k];
                sf  += weight[k] * f;
                sp  += weight[k] * p;
                sff += weight[k] * f * f;
                sfp += weight[k] * f * p;
            }
            {
                const double den = sw * sff - sf * sf;

                if (den > 0) {
                    const double b = (sw * sfp - sf * sp) / den;
                    const double c = (sp - b * sf) / sw;

                    tau -= b * n / (2.0 * M_PI);
                    phi += c;
                }
            }
        }
        res->delay_samples = res->delay_valid ? tau : 0.0;
        res->delay_s       = res->delay_samples / sample_rate;
        res->phase_deg     = wrap_rad(phi) * 180.0 / M_PI;

        /* What is left over frequency. */
        {
            double sg = 0, sph = 0;

            for (k = 0; k < n; k++) {
                const double f = (double)(k - n / 2);
                const double r = carg(cs.s21[k] * cexp(2.0 * M_PI * I * f * res->delay_samples / n - I * phi));

                res->residual_bin_deg[k] = (float)(r * 180.0 / M_PI);
                if (!use[k])
                    continue;
                sg  += (res->gain_bin_db[k] - res->gain_db) * (res->gain_bin_db[k] - res->gain_db);
                sph += r * r;
            }
            res->gain_ripple_db   = sqrt(sg / res->coherent_bins);
            res->phase_ripple_deg = sqrt(sph / res->coherent_bins) * 180.0 / M_PI;
        }
    }

out:
    m2sdr_cross_free(&cs);
    free(use);
    free(weight);
    return rc;
}
