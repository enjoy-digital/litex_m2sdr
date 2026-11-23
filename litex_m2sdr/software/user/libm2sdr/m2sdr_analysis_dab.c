/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: DAB/DAB+ reception (EN 300 401), transmission mode I.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis.h"
#include "m2sdr_fft.h"

/* The samples and the products here are finite: complex products written out, without the NaN
 * and infinity handling of C99 (__mulsc3) that GCC calls for a * b. */
static inline float complex cmul(float complex a, float complex b)
{
    return (crealf(a) * crealf(b) - cimagf(a) * cimagf(b)) + I * (crealf(a) * cimagf(b) + cimagf(a) * crealf(b));
}

/* a b*. */
static inline float complex cmulc(float complex a, float complex b)
{
    return (crealf(a) * crealf(b) + cimagf(a) * cimagf(b)) + I * (cimagf(a) * crealf(b) - crealf(a) * cimagf(b));
}

static inline float cnorm(float complex a)
{
    return crealf(a) * crealf(a) + cimagf(a) * cimagf(a);
}

static inline double complex cmul_d(double complex a, double complex b)
{
    return (creal(a) * creal(b) - cimag(a) * cimag(b)) + I * (creal(a) * cimag(b) + cimag(a) * creal(b));
}

/* Transmission mode I at 2.048 MSPS. */
#define DAB_N              2048    /* Useful part of a symbol: 1 ms. */
#define DAB_G              504     /* Guard interval: 246 us. */
#define DAB_L              (DAB_N + DAB_G)
#define DAB_NULL           2656    /* Null symbol: 1.297 ms. */
#define DAB_SYMBOLS        76      /* Phase reference symbol and 75 data symbols. */
#define DAB_FRAME          (DAB_NULL + DAB_SYMBOLS * DAB_L)   /* 196608: 96 ms. */
#define DAB_K              1536    /* Carriers, k = -768 to 768 without 0. */
#define DAB_KMAX           768

#define ADV                (DAB_G / 8)  /* FFT window start before the end of the guard interval. */
#define TIMING_SPAN        128     /* Samples searched on each side of the timing of the null symbol. */
#define MAX_OFFSET         250     /* Whole carriers searched: the room left in 2.048 MHz. */
#define MIN_NULL_DB        3.0     /* The null symbol of an ensemble; noise reads about 1 dB. */
#define MIN_SYNC_QUALITY   0.10    /* Guard intervals of an ensemble; noise reads about 0.03. */
#define MIN_PRS_QUALITY    0.25    /* Phase reference symbol; noise reads about 0.1. */
#define ECHO_FFT           4096    /* Points of the impulse response: 0.244 us each. */
#define ECHO_MAIN_LOBE     12      /* Points of ECHO_FFT around the strongest path left out of the search of an echo. */
#define NULL_EDGE          128     /* Samples at each end of the null symbol left out of its power. */

/* Phase reference symbol (EN 300 401, 14.3.2): phi_k = pi/2 (h[i][k - k'] + n) over ranges of
 * 32 carriers, mode I. */
static const int8_t prs_h[4][32] = {
    { 0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1, 0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1 },
    { 0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0, 0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0 },
    { 0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3, 0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3 },
    { 0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2, 0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2 },
};

/* i and n of each range, from k = -768 upwards: k' is the first carrier of the range. */
static const uint8_t prs_in[48][2] = {
    { 0, 1 }, { 1, 2 }, { 2, 0 }, { 3, 1 }, { 0, 3 }, { 1, 2 }, { 2, 2 }, { 3, 3 },
    { 0, 2 }, { 1, 1 }, { 2, 2 }, { 3, 3 }, { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 3 },
    { 0, 2 }, { 1, 2 }, { 2, 2 }, { 3, 1 }, { 0, 1 }, { 1, 3 }, { 2, 1 }, { 3, 2 },
    { 0, 3 }, { 3, 1 }, { 2, 1 }, { 1, 1 }, { 0, 2 }, { 3, 2 }, { 2, 1 }, { 1, 0 },
    { 0, 2 }, { 3, 2 }, { 2, 3 }, { 1, 3 }, { 0, 0 }, { 3, 2 }, { 2, 1 }, { 1, 3 },
    { 0, 3 }, { 3, 3 }, { 2, 3 }, { 1, 0 }, { 0, 3 }, { 3, 0 }, { 2, 1 }, { 1, 1 },
};

/* Carrier of index c (0 to 1535, the lowest first): k = -768 to -1, then 1 to 768. */
static int carrier_k(int c)
{
    return c < DAB_KMAX ? c - DAB_KMAX : c - DAB_KMAX + 1;
}

/* Phase reference of carrier index c. */
static float complex prs_value(int c)
{
    const int k = carrier_k(c);
    const int range = k < 0 ? (k + DAB_KMAX) / 32 : 24 + (k - 1) / 32;
    const int first = k < 0 ? -DAB_KMAX + 32 * range : 1 + 32 * (range - 24);
    const int q = prs_h[prs_in[range][0]][k - first] + prs_in[range][1];
    static const float complex quarter[4] = { 1.0f, I, -1.0f, -I };

    return quarter[q & 3];
}

void m2sdr_dab_phase_reference(float complex prs[DAB_K])
{
    int c;

    for (c = 0; c < DAB_K; c++)
        prs[c] = prs_value(c);
}

/* FFT bin of carrier index c, the ensemble off by `off` carriers. */
static int carrier_bin(int c, int off)
{
    return ((carrier_k(c) + off) % DAB_N + DAB_N) % DAB_N;
}

/* Symbol s (0: phase reference) of the frame whose null symbol starts at t, in frequency: the
 * fractional offset eps removed, and the turn the integer offset gives each symbol by its start. */
static void symbol_fft(const struct m2sdr_fft *fft, const float complex *x, size_t a, double eps, int off,
                       float complex *in, float complex *out)
{
    double complex rot = cexp(I * 2.0 * M_PI * eps * (double)a / DAB_N);
    const double complex step = cexp(I * 2.0 * M_PI * eps / DAB_N);
    const float complex turn = (float complex)cexp(-I * 2.0 * M_PI * (double)off * (double)(a % DAB_N) / DAB_N);
    int i;

    for (i = 0; i < DAB_N; i++) {
        in[i] = cmul(x[a + (size_t)i], (float complex)rot);
        rot = cmul_d(rot, step);
    }
    m2sdr_fft_run(fft, in, out);
    for (i = 0; i < DAB_N; i++)
        out[i] = cmul(out[i], turn);
}

int m2sdr_dab_analyse(const float complex *x, size_t count, double sample_rate, struct m2sdr_dab_result *res)
{
    struct m2sdr_fft *fft = NULL, *efft = NULL;
    float complex *in = NULL, *y = NULL, *h = NULL, *ein = NULL, *eout = NULL, *yd = NULL, *hsum = NULL, *z = NULL;
    float complex prs[DAB_K];
    double *fold = NULL, *err = NULL, *ref = NULL, *hpow = NULL, *echo = NULL;
    double complex *cum = NULL;
    double power = 0, best_q = -1;
    double complex best_m = 0;
    size_t i, t_null, frame0;
    int f, s, c, d, best_d = 0, nfr, off = 0, taken = 0, stride, nused = 0, rc = -1;
    double eps;

    if (!x || !res)
        return -1;
    memset(res, 0, sizeof(*res));
    res->guard_us = DAB_G / M2SDR_DAB_SAMPLE_RATE * 1e6;
    if (fabs(sample_rate - M2SDR_DAB_SAMPLE_RATE) > 2e-3 * M2SDR_DAB_SAMPLE_RATE || count < (size_t)(0.11 * M2SDR_DAB_SAMPLE_RATE))
        return -1;
    for (i = 0; i < count; i++)
        power += cnorm(x[i]);
    res->power_dbfs = 10.0 * log10(power / (double)count + 1e-30);

    /* Null symbol: the least power over its length, the power folded on the frame. */
    fold = calloc(DAB_FRAME, sizeof(*fold));
    if (!fold)
        goto out;
    {
        const size_t nf = count / DAB_FRAME;
        const size_t span = nf ? nf * DAB_FRAME : count;
        double m = 0, best = INFINITY, mean = 0;
        size_t best_t = 0;

        for (i = 0; i < span; i++)
            fold[i % DAB_FRAME] += cnorm(x[i]);
        for (i = 0; i < DAB_NULL; i++)
            m += fold[i];
        for (i = 0; i < DAB_FRAME; i++) {
            if (m < best) {
                best   = m;
                best_t = i;
            }
            mean += fold[i];
            m += fold[(i + DAB_NULL) % DAB_FRAME] - fold[i];
        }
        mean = mean / DAB_FRAME * DAB_NULL;
        res->null_depth_db = best > 0 ? 10.0 * log10(mean / best) : 99.0;
        t_null = best_t;
    }

    /* Timing and fractional offset: the guard intervals of the symbols of every whole frame,
     * around the timing of the null symbol, from running sums of x against itself 1 ms later. */
    cum = malloc((count - DAB_N + 1) * sizeof(*cum));
    if (!cum)
        goto out;
    cum[0] = 0;
    for (i = 0; i + DAB_N < count; i++)
        cum[i + 1] = cum[i] + cmulc(x[i], x[i + DAB_N]);
    frame0 = t_null >= (size_t)TIMING_SPAN ? t_null : t_null + DAB_FRAME;
    nfr = 0;
    while (frame0 + (size_t)(nfr + 1) * DAB_FRAME + TIMING_SPAN + DAB_N <= count)
        nfr++;
    if (nfr == 0) {
        rc = 0;
        goto out;
    }
    for (d = -TIMING_SPAN; d <= TIMING_SPAN; d++) {
        double complex m = 0;

        for (f = 0; f < nfr; f++)
            for (s = 0; s < DAB_SYMBOLS; s++) {
                const size_t a = (size_t)((long)(frame0 + (size_t)f * DAB_FRAME + DAB_NULL + (size_t)s * DAB_L) + d);

                m += cum[a + DAB_G] - cum[a];
            }
        if (cabs(m) > best_q) {
            best_q = cabs(m);
            best_m = m;
            best_d = d;
        }
    }
    {
        double pw = 0;

        for (f = 0; f < nfr; f++)
            for (s = 0; s < DAB_SYMBOLS; s++) {
                const size_t a = (size_t)((long)(frame0 + (size_t)f * DAB_FRAME + DAB_NULL + (size_t)s * DAB_L) + best_d);
                int k;

                for (k = 0; k < DAB_G; k++)
                    pw += 0.5 * (cnorm(x[a + k]) + cnorm(x[a + k + DAB_N]));
            }
        res->sync_quality = pw > 0 ? best_q / pw : 0;
    }
    eps = carg(best_m) / (2.0 * M_PI);
    res->found = res->null_depth_db >= MIN_NULL_DB && res->sync_quality >= MIN_SYNC_QUALITY;
    if (!res->found) {
        rc = 0;
        goto out;
    }

    fft  = m2sdr_fft_alloc(DAB_N, false);
    efft = m2sdr_fft_alloc(ECHO_FFT, true);
    in   = malloc(DAB_N * sizeof(*in));
    y    = malloc(DAB_N * sizeof(*y));
    h    = malloc((size_t)nfr * DAB_K * sizeof(*h));
    ein  = malloc(ECHO_FFT * sizeof(*ein));
    eout = malloc(ECHO_FFT * sizeof(*eout));
    err  = calloc(DAB_K * 3, sizeof(*err));
    echo = calloc(ECHO_FFT, sizeof(*echo));
    yd   = malloc((size_t)(DAB_SYMBOLS - 1) * DAB_K * sizeof(*yd));
    hsum = malloc(DAB_K * sizeof(*hsum));
    z    = malloc(DAB_K * sizeof(*z));
    if (!fft || !efft || !in || !y || !h || !ein || !eout || !err || !echo || !yd || !hsum || !z)
        goto out;
    ref  = err + DAB_K;
    hpow = err + 2 * DAB_K;
    m2sdr_dab_phase_reference(prs);

    /* Integer offset: the phase reference against the standard one, carrier to carrier so the
     * delay of the channel does not matter. */
    {
        double best = -1;
        int o;
        float complex *yp = malloc((size_t)nfr * DAB_N * sizeof(*yp));

        if (!yp)
            goto out;
        for (f = 0; f < nfr; f++)
            symbol_fft(fft, x, (size_t)((long)(frame0 + (size_t)f * DAB_FRAME + DAB_NULL) + best_d) + DAB_G - ADV, eps, 0,
                       in, yp + (size_t)f * DAB_N);
        for (o = -MAX_OFFSET; o <= MAX_OFFSET; o++) {
            double num = 0, den = 0;

            for (f = 0; f < nfr; f++) {
                const float complex *a = yp + (size_t)f * DAB_N;
                double complex m = 0;

                for (c = 0; c + 1 < DAB_K; c++) {
                    const float complex v = cmulc(a[carrier_bin(c + 1, o)], a[carrier_bin(c, o)]);

                    m   += cmulc(v, cmulc(prs[c + 1], prs[c]));
                    den += cabsf(v);
                }
                num += cabs(m);
            }
            if (den > 0 && num / den > best) {
                best = num / den;
                off  = o;
            }
        }
        free(yp);
        res->prs_quality    = best;
        res->carrier_offset = off;
        res->freq_error_hz  = (off - eps) * (M2SDR_DAB_SAMPLE_RATE / DAB_N);
        res->frames         = nfr;
    }
    if (res->prs_quality < MIN_PRS_QUALITY) {
        rc = 0;
        goto out;
    }
    res->prs = true;

    /* Each frame: the channel from its phase reference; the data symbols equalised with it and
     * their points decided; the channel again from all 76 symbols, known by then, which leaves
     * its noise 19 dB lower; then the data symbols against their points. Even symbols are on the
     * axes: turned by pi/4 onto the points of the odd ones. Each symbol loses its common phase. */
    stride = (int)((size_t)nfr * (DAB_SYMBOLS - 1) * DAB_K / M2SDR_DAB_MAX_POINTS) + 1;
    for (f = 0; f < nfr; f++) {
        const size_t t = (size_t)((long)(frame0 + (size_t)f * DAB_FRAME + DAB_NULL) + best_d);
        float complex *hf = h + (size_t)f * DAB_K;
        long n = 0;
        int pass;

        symbol_fft(fft, x, t + DAB_G - ADV, eps, off, in, y);
        for (c = 0; c < DAB_K; c++) {
            hf[c]   = cmulc(y[carrier_bin(c, off)], prs[c]);
            hsum[c] = hf[c];
        }
        for (s = 1; s < DAB_SYMBOLS; s++) {
            symbol_fft(fft, x, t + (size_t)s * DAB_L + DAB_G - ADV, eps, off, in, y);
            for (c = 0; c < DAB_K; c++)
                yd[(size_t)(s - 1) * DAB_K + (size_t)c] = y[carrier_bin(c, off)];
        }
        for (pass = 0; pass < 2; pass++) {
            if (pass == 1)
                for (c = 0; c < DAB_K; c++)
                    hf[c] = hsum[c] / (float)DAB_SYMBOLS;
            for (s = 1; s < DAB_SYMBOLS; s++) {
                const float complex onto = s % 2 ? 1.0f : (float complex)cexp(I * M_PI / 4.0);
                const float complex *ys = yd + (size_t)(s - 1) * DAB_K;
                double complex common = 0;
                float complex back;

                for (c = 0; c < DAB_K; c++) {
                    const float hh = cnorm(hf[c]);

                    z[c] = hh > 0 ? cmul(cmulc(ys[c], hf[c]) / hh, onto) : 0;
                    common += cmulc(z[c], (crealf(z[c]) >= 0 ? 1.0f : -1.0f) + I * (cimagf(z[c]) >= 0 ? 1.0f : -1.0f));
                }
                back = (float complex)cexp(-I * carg(common));
                for (c = 0; c < DAB_K; c++) {
                    const float complex v = cmul(z[c], back);
                    const float complex q = ((crealf(v) >= 0 ? 1.0f : -1.0f) + I * (cimagf(v) >= 0 ? 1.0f : -1.0f)) *
                                            (float)M_SQRT1_2;

                    if (pass == 0) {
                        /* y = h q, with the turn of the symbol and its common phase. */
                        hsum[c] += cmul(cmul(cmulc(ys[c], q), onto), back);
                        continue;
                    }
                    err[c] += cnorm(v - q);
                    ref[c] += 1.0;
                    if (taken < M2SDR_DAB_MAX_POINTS && n++ % stride == 0)
                        res->points[taken++] = v;
                }
                if (pass == 1)
                    res->symbols++;
            }
        }
        for (c = 0; c < DAB_K; c++)
            hpow[c] += cnorm(hf[c]);

        /* Impulse response: the channel over the carriers, the gap of carrier 0 filled. */
        memset(ein, 0, ECHO_FFT * sizeof(*ein));
        for (c = 0; c <= DAB_K; c++) {
            const float complex v = c < DAB_KMAX ? hf[c] : c == DAB_KMAX ? 0.5f * (hf[c - 1] + hf[c]) : hf[c - 1];

            ein[c] = v * (float)(0.5 - 0.5 * cos(2.0 * M_PI * (c + 0.5) / (DAB_K + 1)));
        }
        m2sdr_fft_run(efft, ein, eout);
        for (i = 0; i < ECHO_FFT; i++)
            echo[i] += cnorm(eout[i]);
        nused++;
    }
    res->npoints = taken;

    /* Signal and noise on the carriers: what the null symbols leave on them is noise. Comparable
     * with the MER, which also loses what the receiver and the transmitter add. */
    {
        double noise = 0, sig = 0;

        for (f = 0; f < nfr; f++) {
            const size_t t = (size_t)((long)(frame0 + (size_t)f * DAB_FRAME) + best_d) + (DAB_NULL - DAB_N) / 2;

            symbol_fft(fft, x, t, eps, off, in, y);
            for (c = 0; c < DAB_K; c++)
                noise += cnorm(y[carrier_bin(c, off)]);
        }
        for (c = 0; c < DAB_K; c++)
            sig += hpow[c];
        res->null_snr_db = noise > 0 && sig > 0 ? 10.0 * log10(sig / noise) : 0.0;
    }

    /* MER, over everything and per group of carriers; the channel per group. */
    {
        double te = 0, tr = 0, hmean = 0;
        int m;

        for (c = 0; c < DAB_K; c++) {
            te    += err[c];
            tr    += ref[c];
            hmean += hpow[c];
        }
        res->mer_db = te > 0 ? 10.0 * log10(tr / te) : 99.0;
        hmean /= DAB_K;
        res->bins = M2SDR_DAB_BINS;
        res->carriers_per_bin = (double)DAB_K / M2SDR_DAB_BINS;
        for (m = 0; m < M2SDR_DAB_BINS; m++) {
            const int c0 = (int)(m * res->carriers_per_bin), c1 = (int)((m + 1) * res->carriers_per_bin);
            double be = 0, br = 0, bh = 0;

            for (c = c0; c < c1 && c < DAB_K; c++) {
                be += err[c];
                br += ref[c];
                bh += hpow[c];
            }
            res->mer_bin_db[m]     = (float)(be > 0 ? 10.0 * log10(br / be) : 99.0);
            res->channel_bin_db[m] = (float)(hmean > 0 && bh > 0 ? 10.0 * log10(bh / (c1 - c0) / hmean) : -99.0);
        }
    }

    /* Echo profile: an eighth of the range before the strongest path, the rest after it. */
    if (nused > 0) {
        const double step_s = 1.0 / (ECHO_FFT * (M2SDR_DAB_SAMPLE_RATE / DAB_N));
        const int per = ECHO_FFT / M2SDR_DAB_ECHO_BINS;
        double peak = 0, second = 0;
        int bp = 0, bs = -1, m, k;

        for (k = 0; k < ECHO_FFT; k++)
            if (echo[k] > peak) {
                peak = echo[k];
                bp   = k;
            }
        for (k = 0; k < ECHO_FFT; k++) {
            const int dist = abs(((k - bp) % ECHO_FFT + ECHO_FFT + ECHO_FFT / 2) % ECHO_FFT - ECHO_FFT / 2);

            if (dist > ECHO_MAIN_LOBE && echo[k] > second) {
                second = echo[k];
                bs     = k;
            }
        }
        for (m = 0; m < M2SDR_DAB_ECHO_BINS; m++) {
            double v = 0;

            for (k = 0; k < per; k++) {
                const int b = ((bp - ECHO_FFT / 8 + m * per + k) % ECHO_FFT + ECHO_FFT) % ECHO_FFT;

                if (echo[b] > v)
                    v = echo[b];
            }
            res->echo_db[m] = (float)(10.0 * log10(v / peak + 1e-12));
        }
        res->echo_start_us = -(ECHO_FFT / 8) * step_s * 1e6;
        res->echo_step_us  = per * step_s * 1e6;
        res->echo_level_db = -99.0;
        if (bs >= 0) {
            int rel = ((bs - bp) % ECHO_FFT + ECHO_FFT) % ECHO_FFT;

            if (rel >= ECHO_FFT * 7 / 8)
                rel -= ECHO_FFT;
            res->echo_delay_us = rel * step_s * 1e6;
            res->echo_level_db = 10.0 * log10(second / peak + 1e-12);
        }
    }
    rc = 0;
out:
    free(fold);
    free(cum);
    free(in);
    free(y);
    free(h);
    free(ein);
    free(eout);
    free(err);
    free(echo);
    free(yd);
    free(hsum);
    free(z);
    m2sdr_fft_free(fft);
    m2sdr_fft_free(efft);
    return rc;
}
