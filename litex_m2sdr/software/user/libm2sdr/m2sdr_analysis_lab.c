/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis, lab instrument: stimuli, and the helpers its analyses share.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_lab.h"
#include "m2sdr_analysis_lab_internal.h"

/* Helpers */
/*---------*/

int m2sdr_lab_align(const struct m2sdr_fft *fwd, const struct m2sdr_fft *inv, const float complex *x,
                    const float complex *ref, size_t period, size_t *lag, double *quality_db,
                    float complex *R)
{
    float complex *a = malloc(3 * period * sizeof(*a));
    float complex *b, *c;
    double sum = 0, best = -1;
    size_t i;

    if (!a)
        return -1;
    b = a + period;
    c = b + period;
    m2sdr_fft_run(fwd, x, a);
    m2sdr_fft_run(fwd, ref, b);
    if (R)
        memcpy(R, b, period * sizeof(*R));
    for (i = 0; i < period; i++)
        a[i] *= conjf(b[i]);
    m2sdr_fft_run(inv, a, c);
    /* c[d] = sum x[i + d] conj(ref[i]): the peak is the lag. */
    *lag = 0;
    for (i = 0; i < period; i++) {
        const double p = crealf(c[i]) * crealf(c[i]) + cimagf(c[i]) * cimagf(c[i]);
        sum += p;
        if (p > best) {
            best = p;
            *lag = i;
        }
    }
    *quality_db = m2sdr_power_db(best / (sum / (double)period + 1e-30));
    free(a);
    return 0;
}

size_t m2sdr_lab_psd_bin(const struct m2sdr_psd *psd, double freq_hz)
{
    double k = floor(freq_hz / psd->bin_hz + (double)(psd->bins / 2) + 0.5);

    if (k < 0)
        k = 0;
    if (k > (double)(psd->bins - 1))
        k = (double)(psd->bins - 1);
    return (size_t)k;
}

double m2sdr_lab_noise_bin(const struct m2sdr_psd *psd)
{
    /* A bin holds the density times the resolution bandwidth. */
    const double density = m2sdr_psd_noise_density_db(psd, -0.5 * psd->sample_rate, 0.5 * psd->sample_rate);

    return pow(10.0, density / 10.0) * psd->rbw_hz;
}

double m2sdr_lab_line_power(const struct m2sdr_psd *psd, size_t k, size_t lobe, double noise_bin, double *floor_out)
{
    const size_t a = k > lobe ? k - lobe : 0;
    const size_t b = k + lobe < psd->bins ? k + lobe : psd->bins - 1;
    const double noise = noise_bin * (double)(b - a + 1) / psd->enbw_bins;
    double sum = 0;
    size_t i;

    for (i = a; i <= b; i++)
        sum += psd->power[i];
    if (floor_out)
        *floor_out = noise;
    /* A tone spreads over the lobe; the bins sum to its power times the ENBW of the window. */
    sum = sum / psd->enbw_bins - noise;
    return sum > 1e-20 ? sum : 1e-20;
}

static uint32_t xorshift32(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* Mean power of x set to level_dbfs; the PAPR it then has. */
static double set_level(float complex *x, size_t n, double level_dbfs)
{
    double power = 0, peak = 0, scale;
    size_t i;

    for (i = 0; i < n; i++) {
        const double p = crealf(x[i]) * crealf(x[i]) + cimagf(x[i]) * cimagf(x[i]);
        power += p;
        if (p > peak)
            peak = p;
    }
    power /= (double)n;
    if (!(power > 0))
        return 0.0;
    scale = sqrt(pow(10.0, level_dbfs / 10.0) / power);
    for (i = 0; i < n; i++)
        x[i] *= (float)scale;
    return 10.0 * log10(peak / power);
}

/* Stimuli */
/*---------*/

/* Whole cycles in the buffer: the phase of sample i is 2 pi (cycles * i mod count) / count,
 * exact over any length. */
static void add_cycles(float complex *x, size_t count, long cycles, double amp)
{
    const long n = (long)count;
    const long c = ((cycles % n) + n) % n;
    size_t i;

    for (i = 0; i < count; i++) {
        const long p = (long)(((unsigned long long)c * i) % (unsigned long long)n);
        x[i] += (float complex)(amp * cexp(I * 2.0 * M_PI * (double)p / (double)n));
    }
}

int m2sdr_lab_tone(float complex *x, size_t count, double sample_rate, double freq_hz, double level_dbfs,
                   double *freq_out)
{
    long cycles;

    if (!x || count < 2 || !(sample_rate > 0) || !(fabs(freq_hz) <= 0.5 * sample_rate) || !isfinite(level_dbfs))
        return -1;
    cycles = lround(freq_hz * (double)count / sample_rate);
    memset(x, 0, count * sizeof(*x));
    add_cycles(x, count, cycles, pow(10.0, level_dbfs / 20.0));
    if (freq_out)
        *freq_out = (double)cycles * sample_rate / (double)count;
    return 0;
}

int m2sdr_lab_two_tone(float complex *x, size_t count, double sample_rate, double centre_hz, double spacing_hz,
                       double level_dbfs, double *f1_out, double *f2_out)
{
    const double amp = pow(10.0, level_dbfs / 20.0);
    long c1, c2;

    if (!x || count < 2 || !(sample_rate > 0) || !(spacing_hz > 0) || !isfinite(level_dbfs) ||
        !(fabs(centre_hz) + 0.5 * spacing_hz <= 0.5 * sample_rate))
        return -1;
    c1 = lround((centre_hz - 0.5 * spacing_hz) * (double)count / sample_rate);
    c2 = lround((centre_hz + 0.5 * spacing_hz) * (double)count / sample_rate);
    if (c2 <= c1)
        return -1;
    memset(x, 0, count * sizeof(*x));
    add_cycles(x, count, c1, amp);
    add_cycles(x, count, c2, amp);
    if (f1_out)
        *f1_out = (double)c1 * sample_rate / (double)count;
    if (f2_out)
        *f2_out = (double)c2 * sample_rate / (double)count;
    return 0;
}

int m2sdr_lab_noise(float complex *x, size_t period, double sample_rate, double bw_hz, double level_dbfs,
                    uint32_t seed, double *papr_db)
{
    struct m2sdr_fft *inv;
    float complex *spec;
    uint32_t s = seed ? seed : 0x9e3779b9u;
    size_t b, used = 0;
    double papr;

    if (!x || !m2sdr_lab_pow2(period) || period < 64 || !(sample_rate > 0) || !(bw_hz > 0) ||
        bw_hz > sample_rate || !isfinite(level_dbfs))
        return -1;
    inv  = m2sdr_fft_alloc(period, true);
    spec = calloc(period, sizeof(*spec));
    if (!inv || !spec) {
        m2sdr_fft_free(inv);
        free(spec);
        return -1;
    }
    for (b = 0; b < period; b++) {
        const double f = (double)m2sdr_lab_signed_bin(b, period) * sample_rate / (double)period;
        const double phase = 2.0 * M_PI * (double)xorshift32(&s) / 4294967296.0;
        if (b != 0 && fabs(f) <= 0.5 * bw_hz) {
            spec[b] = (float complex)cexp(I * phase);
            used++;
        }
    }
    if (!used) {
        m2sdr_fft_free(inv);
        free(spec);
        return -1;
    }
    m2sdr_fft_run(inv, spec, x);
    papr = set_level(x, period, level_dbfs);
    if (papr_db)
        *papr_db = papr;
    m2sdr_fft_free(inv);
    free(spec);
    return 0;
}

int m2sdr_lab_qam(float complex *x, size_t period, enum m2sdr_modulation modulation, unsigned sps,
                  double rolloff, double level_dbfs, uint32_t seed, double *papr_db)
{
    struct m2sdr_fft *fwd, *inv;
    float complex points[256], *buf;
    uint32_t s = seed ? seed : 0x9e3779b9u;
    const double rs = 1.0 / (double)sps;    /* Symbol rate over the sample rate. */
    size_t i;
    int npoints;
    double papr;

    if (!x || !m2sdr_lab_pow2(period) || period < 64 || !m2sdr_lab_pow2(sps) || sps > period / 16 ||
        !(rolloff >= 0.05 && rolloff <= 1.0) || !isfinite(level_dbfs))
        return -1;
    npoints = m2sdr_constellation(modulation, points);
    if (npoints <= 0)
        return -1;
    fwd = m2sdr_fft_alloc(period, false);
    inv = m2sdr_fft_alloc(period, true);
    buf = calloc(period, sizeof(*buf));
    if (!fwd || !inv || !buf) {
        m2sdr_fft_free(fwd);
        m2sdr_fft_free(inv);
        free(buf);
        return -1;
    }

    /* Symbols at their instants, then the root raised cosine in frequency: the pulse is periodic
     * with the signal, without truncation. */
    for (i = 0; i < period; i += sps)
        buf[i] = points[(xorshift32(&s) >> 8) % (uint32_t)npoints];
    m2sdr_fft_run(fwd, buf, x);
    for (i = 0; i < period; i++) {
        const double f  = fabs((double)m2sdr_lab_signed_bin(i, period) / (double)period);
        const double f1 = 0.5 * (1.0 - rolloff) * rs, f2 = 0.5 * (1.0 + rolloff) * rs;
        double rc = 0;
        if (f <= f1)
            rc = 1.0;
        else if (f < f2)
            rc = 0.5 * (1.0 + cos(M_PI / (rolloff * rs) * (f - f1)));
        x[i] *= (float)sqrt(rc);
    }
    memcpy(buf, x, period * sizeof(*buf));
    m2sdr_fft_run(inv, buf, x);
    papr = set_level(x, period, level_dbfs);
    if (papr_db)
        *papr_db = papr;
    m2sdr_fft_free(fwd);
    m2sdr_fft_free(inv);
    free(buf);
    return 0;
}
