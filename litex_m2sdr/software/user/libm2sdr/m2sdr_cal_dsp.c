/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR reference clock calibration: carrier frequency offset of cellular downlink signals.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_cal.h"
#include "m2sdr_fft.h"

/* FFT */
/*-----*/

/* The short blocks used here (64 to 256 points), as pairs of floats. */
typedef struct { float r, i; } cal_cpx;

#define cal_fft       m2sdr_fft
#define cal_fft_alloc m2sdr_fft_alloc
#define cal_fft_free  m2sdr_fft_free

static void cal_fft_run(const struct m2sdr_fft *fft, const cal_cpx *in, cal_cpx *out, int n)
{
    float complex vi[256], vo[256];
    int i;

    for (i = 0; i < n; i++)
        vi[i] = in[i].r + I * in[i].i;
    m2sdr_fft_run(fft, vi, vo);
    for (i = 0; i < n; i++) {
        out[i].r = crealf(vo[i]);
        out[i].i = cimagf(vo[i]);
    }
}

#define FS M2SDR_CAL_SAMPLE_RATE

/* Helpers */
/*---------*/

void m2sdr_cal_iq_to_complex(const int16_t *iq, size_t count, unsigned stride, unsigned bits,
                             float complex *x)
{
    const float scale = 1.0f / (float)(1u << (bits - 1));
    double complex mean = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        x[i] = (iq[i * stride] + I * iq[i * stride + 1]) * scale;
        mean += x[i];
    }
    if (count)
        mean /= (double)count;
    for (i = 0; i < count; i++)
        x[i] -= (float complex)mean;
}

double m2sdr_cal_refclk_ppm(double cfo_hz, double freq_hz)
{
    return freq_hz > 0.0 ? -cfo_hz / freq_hz * 1e6 : 0.0;
}

static double mean_power_db(const float complex *x, size_t count)
{
    double p = 0;
    size_t i;

    for (i = 0; i < count; i++)
        p += creal(x[i]) * creal(x[i]) + cimag(x[i]) * cimag(x[i]);
    return 10.0 * log10(p / (double)(count ? count : 1) + 1e-20);
}

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *v, int n)
{
    qsort(v, (size_t)n, sizeof(*v), cmp_double);
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* Channel numbers */
/*-----------------*/

/* 3GPP TS 36.101 table 5.7.3-1: band, F_DL_low (MHz), N_Offs-DL, last N_DL. */
static const struct { unsigned band; double f_low; unsigned n_offs, n_last; } lte_bands[] = {
    {  1, 2110.0,     0,   599 },
    {  2, 1930.0,   600,  1199 },
    {  3, 1805.0,  1200,  1949 },
    {  4, 2110.0,  1950,  2399 },
    {  5,  869.0,  2400,  2649 },
    {  7, 2620.0,  2750,  3449 },
    {  8,  925.0,  3450,  3799 },
    { 12,  729.0,  5010,  5179 },
    { 13,  746.0,  5180,  5279 },
    { 20,  791.0,  6150,  6449 },
    { 28,  758.0,  9210,  9659 },
    { 32, 1452.0,  9920, 10359 },
    { 38, 2570.0, 37750, 38249 },
    { 40, 2300.0, 38650, 39649 },
    { 41, 2496.0, 39650, 41589 },
    { 42, 3400.0, 41590, 43589 },
    { 43, 3600.0, 43590, 45589 },
};
#define LTE_BANDS (sizeof(lte_bands) / sizeof(lte_bands[0]))

double m2sdr_cal_earfcn_to_hz(unsigned earfcn)
{
    size_t i;

    for (i = 0; i < LTE_BANDS; i++) {
        if (earfcn >= lte_bands[i].n_offs && earfcn <= lte_bands[i].n_last)
            return (lte_bands[i].f_low + 0.1 * (earfcn - lte_bands[i].n_offs)) * 1e6;
    }
    return 0.0;
}

int m2sdr_cal_hz_to_earfcn(double freq_hz, unsigned *band)
{
    size_t i;

    for (i = 0; i < LTE_BANDS; i++) {
        const double n = (freq_hz / 1e6 - lte_bands[i].f_low) / 0.1;
        if (n > -0.01 && fabs(n - round(n)) < 0.01 &&
            lte_bands[i].n_offs + (unsigned)llround(n) <= lte_bands[i].n_last) {
            if (band)
                *band = lte_bands[i].band;
            return (int)(lte_bands[i].n_offs + (unsigned)llround(n));
        }
    }
    return -1;
}

int m2sdr_cal_hz_to_arfcn(double freq_hz)
{
    double n = (freq_hz / 1e6 - 935.0) / 0.2;

    if (fabs(n - round(n)) < 0.01 && n > -0.01 && n < 124.01)
        return (int)llround(n);
    if (fabs(n - round(n)) < 0.01 && n > -49.01 && n < 0)
        return 1024 + (int)llround(n);
    n = (freq_hz / 1e6 - 1805.2) / 0.2;
    if (fabs(n - round(n)) < 0.01 && n > -0.01 && n < 373.01)
        return 512 + (int)llround(n);
    return -1;
}

double m2sdr_cal_arfcn_to_hz(unsigned arfcn)
{
    if (arfcn <= 124)                       /* P-GSM 900 (and ARFCN 0 of E-GSM). */
        return (935.0 + 0.2 * arfcn) * 1e6;
    if (arfcn >= 975 && arfcn <= 1023)      /* E-GSM 900. */
        return (935.0 + 0.2 * ((double)arfcn - 1024.0)) * 1e6;
    if (arfcn >= 512 && arfcn <= 885)       /* DCS 1800. */
        return (1805.2 + 0.2 * (arfcn - 512)) * 1e6;
    return 0.0;
}

/* LTE */
/*-----*/

#define LTE_N        128                 /* FFT size at 1.92 MSPS. */
#define LTE_SLOT     960                 /* Samples per slot (0.5 ms). */
#define LTE_HALF     9600                /* Samples per half frame (5 ms): PSS period. */
#define LTE_PSS_WINDOW 144               /* Timings searched on each side of the first test. */
#define LTE_MAX_HYPS   64                /* Offsets tried, one subcarrier apart. */
#define LTE_PSS_CLOSE  0.6               /* Peaks above this share of the best are candidates. */
#define LTE_PSS_LEVELS 960               /* Timings that give the level around the peak. */
#define PRE_DECIM    2                   /* The first PSS test runs at 0.96 MSPS. */
#define PRE_N        (LTE_N / PRE_DECIM)
#define PRE_HALF     (LTE_HALF / PRE_DECIM)
#define PRE_TAPS     23                  /* Low-pass before the decimation. */
#define PRE_MIN_MATCH  0.05              /* Share of the energy under the peak that correlates. */
#define PRE_MIN_METRIC 5.0               /* Peak over mean of the first test. */
#define LTE_CP       9                   /* Cyclic prefix of the symbols 1 to 6 of a slot. */
#define LTE_SCS      15000.0             /* Subcarrier spacing: ambiguity of the CP estimate. */
#define LTE_PSS_HALF_FRAMES 8            /* Half frames accumulated by the PSS search. */
#define LTE_PSS_MIN_METRIC  8.0          /* PSS peak over mean for a detection. */
#define LTE_PSS_MIN_MATCH   0.07         /* Share of the signal energy that correlates with the PSS. */
#define LTE_BLOCK_SLOTS     40           /* Slots per partial CP estimate (20 ms). */
#define LTE_CP_MIN_QUALITY  3.0          /* CP correlation peak over its median. */

/* PSS of root u in the time domain, LTE_N samples (TS 36.211 6.11.1). */
static void lte_pss(unsigned u, float complex *p)
{
    struct cal_fft *cfg = cal_fft_alloc(LTE_N, 1);
    cal_cpx in[LTE_N], out[LTE_N];
    int n;

    memset(in, 0, sizeof(in));
    for (n = 0; n < 62; n++) {
        const double ph = -M_PI * u * (n < 31 ? n * (n + 1.0) : (n + 1.0) * (n + 2.0)) / 63.0;
        const int k = n < 31 ? LTE_N - 31 + n : n - 30;   /* Subcarriers -31..-1 and 1..31. */
        in[k].r = (float)cos(ph);
        in[k].i = (float)sin(ph);
    }
    cal_fft_run(cfg, in, out, LTE_N);
    for (n = 0; n < LTE_N; n++)
        p[n] = (out[n].r + I * out[n].i) / (float)sqrt(62.0);
    cal_fft_free(cfg);
}

/* First PSS test, at half the sample rate. There the 62 subcarriers of the PSS fill the band,
 * and a frequency offset of whole subcarriers only moves the correlation peak in time (a
 * property of its Zadoff-Chu sequence): one offset finds the PSS whatever the multiple of the
 * subcarrier spacing, at a sixteenth of the cost of the full search over the offsets. Returns
 * the best peak over mean of the three roots, 0 when the peak holds too little of the energy
 * under it (a burst of power, not a PSS), its root in *best_root and its timing in the half
 * frame, in samples of the full rate, in *best_time. */
static double lte_pss_first_test(const float complex *x, size_t count, size_t half_frames, double frac,
                                 int *best_root, size_t *best_time)
{
    static const unsigned roots[3] = { 25, 29, 34 };
    const size_t nd = half_frames * PRE_HALF + PRE_N;
    struct cal_fft *cfg;
    cal_cpx in[PRE_N], out[PRE_N];
    float complex ref[PRE_N];
    float complex *xd;
    float taps[PRE_TAPS];
    float *acc, *energy;
    double tsum = 0, best = 0, best_match = 0;
    size_t i, f, t;
    int k, n, r;

    xd     = malloc(nd * sizeof(*xd));
    acc    = malloc(PRE_HALF * sizeof(*acc));
    energy = calloc(PRE_HALF, sizeof(*energy));
    cfg    = cal_fft_alloc(PRE_N, 1);
    if (!xd || !acc || !energy || !cfg) {
        free(xd); free(acc); free(energy); cal_fft_free(cfg);
        return -1.0;
    }

    /* Low-pass at 480 kHz (the PSS ends at 465 kHz), with the offset removed, and decimation. */
    for (k = 0; k < PRE_TAPS; k++) {
        const double tk = k - (PRE_TAPS - 1) / 2.0;
        const double v  = tk == 0 ? 0.5 : sin(0.5 * M_PI * tk) / (M_PI * tk);
        taps[k] = (float)(v * (0.54 - 0.46 * cos(2 * M_PI * k / (PRE_TAPS - 1))));
        tsum   += taps[k];
    }
    for (k = 0; k < PRE_TAPS; k++)
        taps[k] /= (float)tsum;
    for (i = 0; i < nd; i++) {
        float complex a = 0;
        if (i * PRE_DECIM + PRE_TAPS > count) {
            xd[i] = 0;
            continue;
        }
        for (k = 0; k < PRE_TAPS; k++)
            a += x[i * PRE_DECIM + k] * taps[k];
        xd[i] = a * cexpf(-I * (float)(2.0 * M_PI * fmod(frac * (double)i * PRE_DECIM / FS, 1.0)));
    }

    /* Energy under the PSS at every timing, added over the half frames. */
    for (f = 0; f < half_frames; f++) {
        const float complex *p = xd + f * PRE_HALF;
        double e = 0;
        for (n = 0; n < PRE_N; n++)
            e += crealf(p[n]) * crealf(p[n]) + cimagf(p[n]) * cimagf(p[n]);
        for (t = 0; t < PRE_HALF; t++) {
            energy[t] += (float)e;
            e += crealf(p[t + PRE_N]) * crealf(p[t + PRE_N]) + cimagf(p[t + PRE_N]) * cimagf(p[t + PRE_N])
               - crealf(p[t]) * crealf(p[t]) - cimagf(p[t]) * cimagf(p[t]);
            if (e < 0)
                e = 0;
        }
    }

    for (r = 0; r < 3; r++) {
        double peak = 0, total = 0, match = 0, ref_energy = 0;
        size_t tpeak = 0;

        memset(in, 0, sizeof(in));
        for (n = 0; n < 62; n++) {
            const double ph = -M_PI * roots[r] * (n < 31 ? n * (n + 1.0) : (n + 1.0) * (n + 2.0)) / 63.0;
            const int bin = n < 31 ? PRE_N - 31 + n : n - 30;
            in[bin].r = (float)cos(ph);
            in[bin].i = (float)sin(ph);
        }
        cal_fft_run(cfg, in, out, PRE_N);
        for (n = 0; n < PRE_N; n++) {
            ref[n] = out[n].r - I * out[n].i;
            ref_energy += (double)out[n].r * out[n].r + (double)out[n].i * out[n].i;
        }
        memset(acc, 0, PRE_HALF * sizeof(*acc));
        for (f = 0; f < half_frames; f++) {
            const float complex *p = xd + f * PRE_HALF;
            for (t = 0; t < PRE_HALF; t++) {
                float complex c = 0;
                for (n = 0; n < PRE_N; n++)
                    c += p[t + n] * ref[n];
                acc[t] += crealf(c) * crealf(c) + cimagf(c) * cimagf(c);
            }
        }
        for (t = 0; t < PRE_HALF; t++) {
            total += acc[t];
            if (acc[t] > peak) {
                peak  = acc[t];
                tpeak = t;
                match = energy[t] > 0 ? acc[t] / (energy[t] * ref_energy) : 0;
            }
        }
        if (total > 0 && peak * PRE_HALF / total > best) {
            best       = peak * PRE_HALF / total;
            best_match = match;
            *best_root = r;
            *best_time = tpeak * PRE_DECIM;
        }
    }
    free(xd); free(acc); free(energy); cal_fft_free(cfg);
    return best_match >= PRE_MIN_MATCH ? best : 0.0;
}

/* Cyclic prefix correlation of `slots` slots from x, folded on the slot: argument of the peak. */
static double lte_cp_phase(const float complex *x, size_t slots, double *peak_over_median,
                           double *profile)
{
    double complex fold[LTE_SLOT];
    double complex win[LTE_SLOT];
    double mag[LTE_SLOT];
    double complex best = 0;
    double best_mag = 0;
    size_t s;
    int n, k;

    memset(fold, 0, sizeof(fold));
    for (s = 0; s < slots; s++) {
        const float complex *p = x + s * LTE_SLOT;
        for (n = 0; n < LTE_SLOT; n++)
            fold[n] += conjf(p[n]) * p[n + LTE_N];
    }
    for (n = 0; n < LTE_SLOT; n++) {
        double complex acc = 0;
        for (k = 0; k < LTE_CP; k++)
            acc += fold[(n + k) % LTE_SLOT];
        win[n] = acc;
        mag[n] = cabs(acc);
        if (mag[n] > best_mag)
            best_mag = mag[n];
    }
    /* The seven symbols of the slot turn by the same phase: the local maximum of each one is
     * added. Around it the window also holds products of data, which are noise. */
    for (n = 0; n < LTE_SLOT; n++) {
        bool top = mag[n] > 0.5 * best_mag;
        for (k = -LTE_CP; top && k <= LTE_CP; k++) {
            const int m = (n + k + LTE_SLOT) % LTE_SLOT;
            if (mag[m] > mag[n] || (mag[m] == mag[n] && m < n && k < 0))
                top = false;
        }
        if (top)
            best += win[n];
    }
    if (profile)
        memcpy(profile, mag, sizeof(mag));
    if (peak_over_median) {
        const double med = median(mag, LTE_SLOT);
        *peak_over_median = med > 0 ? best_mag / med : 0;
    }
    return carg(best);
}

int m2sdr_cal_lte_estimate(const float complex *x, size_t count, double max_cfo_hz,
                           struct m2sdr_cal_estimate *est)
{
    static const unsigned roots[3] = { 25, 29, 34 };
    size_t half_frames, slots, blocks, b;
    size_t first_time = 0;
    float complex pss[LTE_N];
    float complex ref[LTE_N];
    double cp_quality = 0, frac;
    double best_metric = 0, best_fh = 0, best_match = 0, best_peak = 0;
    double pss_energy = 0;
    double sum = 0, sum2 = 0;
    struct { double peak, metric, match, fh; long time; } cand[LTE_MAX_HYPS] = { { 0 } };
    int ncand = 0;
    int best_root = 0;
    int hyps, h, n;

    if (!x || !est || max_cfo_hz <= 0)
        return -1;
    memset(est, 0, sizeof(*est));
    if (count < 2 * LTE_HALF + LTE_N)
        return -1;
    est->power_db = mean_power_db(x, count);

    /* Cyclic prefix first: the phase turned over one FFT length gives the offset modulo the
     * subcarrier spacing, and a capture without a clear cyclic prefix is not an LTE carrier. */
    slots = (count - LTE_N) / LTE_SLOT;
    frac  = lte_cp_phase(x, slots, &cp_quality, NULL) / (2.0 * M_PI) * LTE_SCS;
    est->quality = 0;
    est->cp_quality = cp_quality;
    if (cp_quality < LTE_CP_MIN_QUALITY)
        return 0;

    half_frames = (count - LTE_N) / LTE_HALF;
    if (half_frames > LTE_PSS_HALF_FRAMES)
        half_frames = LTE_PSS_HALF_FRAMES;

    /* Most of the points of a scan stop here: an LTE carrier, but not its centre. */
    {
        const double first = lte_pss_first_test(x, count, half_frames, frac, &best_root, &first_time);
        if (first < 0)
            return -1;
        if (first < PRE_MIN_METRIC) {
            est->quality = first;
            return 0;
        }
    }

    /* Full rate, for the root of the first test: the offsets that the cyclic prefix allows,
     * one subcarrier apart. An offset of whole subcarriers moved the peak of the first test by
     * less than a symbol, so the PSS is searched in a window around it; timings spread over the
     * half frame give the level that the peak is compared to. The correlations of the half
     * frames are added without their phase. The PSS identifies the carrier and selects the
     * offset. */
    lte_pss(roots[best_root], pss);
    for (n = 0; n < LTE_N; n++)
        pss_energy += crealf(pss[n]) * crealf(pss[n]) + cimagf(pss[n]) * cimagf(pss[n]);
    hyps = (int)floor((max_cfo_hz + 0.5 * LTE_SCS) / LTE_SCS);
    for (h = -hyps; h <= hyps; h++) {
        const double fh = frac + h * LTE_SCS;
        double peak = 0, level = 0, energy = 0;
        long tpeak = 0;
        int nlevel = 0, w;
        size_t f;

        if (fabs(fh) > max_cfo_hz)
            continue;
        for (n = 0; n < LTE_N; n++)
            ref[n] = conjf(pss[n] * cexpf(I * (float)(2.0 * M_PI * fh * n / FS)));
        for (w = -LTE_PSS_WINDOW; w < LTE_PSS_WINDOW + LTE_PSS_LEVELS; w++) {
            const bool in_window = w < LTE_PSS_WINDOW;
            /* Window: around the first test. Level: timings spread over the half frame. */
            const long t = in_window ? (long)first_time + w
                                     : (long)(w - LTE_PSS_WINDOW) * (LTE_HALF / LTE_PSS_LEVELS);
            double a = 0, e = 0;

            if (t < 0 || t >= LTE_HALF)
                continue;
            if (!in_window && labs(t - (long)first_time) < 2 * LTE_PSS_WINDOW)
                continue;
            for (f = 0; f < half_frames; f++) {
                const float complex *p = x + f * LTE_HALF + t;
                float cr = 0, ci = 0;
                for (n = 0; n < LTE_N; n++) {
                    const float pr = crealf(p[n]), pi = cimagf(p[n]);
                    const float rr = crealf(ref[n]), ri = cimagf(ref[n]);
                    cr += pr * rr - pi * ri;
                    ci += pr * ri + pi * rr;
                    e  += pr * pr + pi * pi;
                }
                a += (double)cr * cr + (double)ci * ci;
            }
            if (in_window) {
                if (a > peak) {
                    peak   = a;
                    energy = e;
                    tpeak  = t;
                }
            } else {
                level += a;
                nlevel++;
            }
        }
        if (nlevel && level > 0 && ncand < LTE_MAX_HYPS) {
            cand[ncand].peak   = peak;
            cand[ncand].metric = peak * nlevel / level;
            cand[ncand].match  = energy > 0 ? peak / (energy * pss_energy) : 0;
            cand[ncand].fh     = fh;
            cand[ncand].time   = tpeak;
            if (peak > best_peak)
                best_peak = peak;
            ncand++;
        }
    }

    /* The PSS correlates almost as well one subcarrier away, at another time: this is the
     * ambiguity of its Zadoff-Chu sequence between frequency and time, and noise can make the
     * wrong offset win by its peak. But only the right one puts the PSS on a symbol: of the
     * offsets with a peak close to the best, the one whose PSS is preceded by its cyclic
     * prefix, the copy of its last samples, is kept. */
    {
        double best_cp = -1;
        int c;

        for (c = 0; c < ncand; c++) {
            double complex cp = 0;
            size_t f;
            int k;

            if (cand[c].peak < LTE_PSS_CLOSE * best_peak)
                continue;
            for (f = 0; f < half_frames; f++) {
                const float complex *p = x + f * LTE_HALF + cand[c].time;
                if (f * LTE_HALF + (size_t)cand[c].time < LTE_CP)
                    continue;
                for (k = 1; k <= LTE_CP; k++)
                    cp += conjf(p[-k]) * p[LTE_N - k];
            }
            if (cabs(cp) > best_cp) {
                best_cp     = cabs(cp);
                best_metric = cand[c].metric;
                best_match  = cand[c].match;
                best_fh     = cand[c].fh;
            }
        }
    }

    est->quality = best_metric;
    est->match   = best_match;
    est->id      = (int)roots[best_root];
    if (best_metric < LTE_PSS_MIN_METRIC || best_match < LTE_PSS_MIN_MATCH)
        return 0;

    /* Spread of the cyclic prefix estimate over blocks of the capture. */
    blocks = slots / LTE_BLOCK_SLOTS;
    for (b = 0; b < blocks; b++) {
        double d = lte_cp_phase(x + b * LTE_BLOCK_SLOTS * LTE_SLOT, LTE_BLOCK_SLOTS, NULL, NULL)
                   / (2.0 * M_PI) * LTE_SCS - frac;
        d -= LTE_SCS * round(d / LTE_SCS);
        sum  += d;
        sum2 += d * d;
    }
    est->found     = true;
    est->cfo_hz    = best_fh;
    est->count     = (int)blocks;
    est->spread_hz = blocks > 1 ? sqrt(fmax(0.0, sum2 / blocks - (sum / blocks) * (sum / blocks))) : 0.0;
    return 0;
}

/* Channel of a wide capture */
/*---------------------------*/

#define CHAN_TAPS 33

int m2sdr_cal_channelize(const float complex *x, size_t count, double offset_hz,
                         float complex *y, size_t *out_count)
{
    const double fs_in = FS * M2SDR_CAL_SCAN_DECIM;
    const size_t ny = count / M2SDR_CAL_SCAN_DECIM > CHAN_TAPS ? (count - CHAN_TAPS) / M2SDR_CAL_SCAN_DECIM : 0;
    /* Phase step of the mix per input sample, and per output sample for the block start. */
    const double complex step = cexp(-I * 2.0 * M_PI * offset_hz / fs_in);
    float taps[CHAN_TAPS];
    float complex rot[CHAN_TAPS];
    double complex mean = 0, phase = 1.0, step_out = 1.0;
    double tsum = 0;
    size_t i;
    int k;

    if (!x || !y || !out_count || !ny)
        return -1;

    /* Low-pass at 0.6 MHz: the PSS of an LTE carrier (0.47 MHz) and a GSM carrier pass, and
     * what folds back at the output rate lands above them. */
    for (k = 0; k < CHAN_TAPS; k++) {
        const double t  = k - (CHAN_TAPS - 1) / 2.0;
        const double fc = 0.6e6 / fs_in;
        const double v  = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        taps[k] = (float)(v * (0.54 - 0.46 * cos(2 * M_PI * k / (CHAN_TAPS - 1))));
        tsum   += taps[k];
    }
    /* The mix, relative to the first sample of a block, goes in the coefficients; the phase
     * of the block start is applied to the output. */
    {
        double complex r = 1.0;
        for (k = 0; k < CHAN_TAPS; k++) {
            taps[k] /= (float)tsum;
            rot[k]   = (float complex)(taps[k] * r);
            r       *= step;
        }
    }
    for (k = 0; k < M2SDR_CAL_SCAN_DECIM; k++)
        step_out *= step;
    for (i = 0; i < ny; i++) {
        const float complex *p = x + i * M2SDR_CAL_SCAN_DECIM;
        float re = 0, im = 0;
        for (k = 0; k < CHAN_TAPS; k++) {
            const float pr = crealf(p[k]), pi = cimagf(p[k]);
            const float rr = crealf(rot[k]), ri = cimagf(rot[k]);
            re += pr * rr - pi * ri;
            im += pr * ri + pi * rr;
        }
        y[i]   = (float complex)((re + I * im) * phase);
        mean  += y[i];
        phase *= step_out;
        if ((i & 0xfff) == 0xfff)
            phase /= cabs(phase);          /* The recurrence keeps its modulus. */
    }
    mean /= (double)ny;
    for (i = 0; i < ny; i++)
        y[i] -= (float complex)mean;
    *out_count = ny;
    return 0;
}

/* GSM */
/*-----*/

#define GSM_DECIM       4                          /* 1.92 MSPS to 480 kSPS. */
#define GSM_FS          (FS / GSM_DECIM)
#define GSM_TAPS        33                         /* Low-pass of the decimation. */
#define GSM_WIN         256                        /* 533 us: an FCCH burst lasts 546 us. */
#define GSM_HOP         32
#define GSM_TONE_RATIO  0.6                        /* Share of the window energy in the peak. */
#define GSM_LAG         24                         /* Lag of the fine estimate: period 20 kHz. */
#define GSM_GUARD       32                         /* Samples left out at each end of the window. */
#define GSM_FRAME_S     (60.0 / 13.0 * 1e-3)       /* TDMA frame: 4.615 ms. */
#define GSM_MIN_BURSTS  3
#define GSM_MAX_BURSTS  4096

int m2sdr_cal_gsm_estimate(const float complex *x, size_t count, double max_cfo_hz,
                           struct m2sdr_cal_estimate *est)
{
    const size_t ny = count / GSM_DECIM > GSM_TAPS ? count / GSM_DECIM - GSM_TAPS : 0;
    float complex *y;
    float taps[GSM_TAPS];
    float win[GSM_WIN];
    struct cal_fft *cfg;
    cal_cpx in[GSM_WIN], out[GSM_WIN];
    double *freqs, *times;
    int nbursts = 0;
    size_t i, w, nwin;
    double tsum = 0;
    int k;
    /* Burst being followed: windows in a row that hold the tone. */
    int run = 0;
    size_t run_best = 0;
    double run_ratio = 0;
    double run_bin = 0;

    if (!x || !est || max_cfo_hz <= 0)
        return -1;
    memset(est, 0, sizeof(*est));
    if (ny < 4 * GSM_WIN)
        return -1;
    est->power_db = mean_power_db(x, count);

    y     = malloc(ny * sizeof(*y));
    freqs = malloc(GSM_MAX_BURSTS * sizeof(*freqs));
    times = malloc(GSM_MAX_BURSTS * sizeof(*times));
    cfg   = cal_fft_alloc(GSM_WIN, 0);
    if (!y || !freqs || !times || !cfg) {
        free(y); free(freqs); free(times); cal_fft_free(cfg);
        return -1;
    }

    /* Channel filter and decimation: windowed sinc, cut-off 120 kHz. */
    for (k = 0; k < GSM_TAPS; k++) {
        const double t  = k - (GSM_TAPS - 1) / 2.0;
        const double fc = 120e3 / FS;
        const double s  = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        taps[k] = (float)(s * (0.54 - 0.46 * cos(2 * M_PI * k / (GSM_TAPS - 1))));
        tsum += taps[k];
    }
    for (k = 0; k < GSM_TAPS; k++)
        taps[k] /= (float)tsum;
    for (i = 0; i < ny; i++) {
        float complex a = 0;
        for (k = 0; k < GSM_TAPS; k++)
            a += x[i * GSM_DECIM + k] * taps[k];
        y[i] = a;
    }
    for (k = 0; k < GSM_WIN; k++)
        win[k] = (float)(0.5 - 0.5 * cos(2 * M_PI * k / (GSM_WIN - 1)));

    /* Windows that hold a tone near the FCCH frequency; a run of them is a burst. */
    nwin = (ny - GSM_WIN) / GSM_HOP;
    for (w = 0; w <= nwin; w++) {
        bool tone = false;
        double bin = 0, ratio = 0;

        if (w < nwin) {
            double total = 0, peak = 0;
            int kpeak = 0;

            for (k = 0; k < GSM_WIN; k++) {
                const float complex v = y[w * GSM_HOP + k] * win[k];
                in[k].r = crealf(v);
                in[k].i = cimagf(v);
            }
            cal_fft_run(cfg, in, out, GSM_WIN);
            for (k = 0; k < GSM_WIN; k++) {
                const double p = (double)out[k].r * out[k].r + (double)out[k].i * out[k].i;
                total += p;
                if (p > peak) {
                    peak  = p;
                    kpeak = k;
                }
            }
            /* The Hann window spreads a tone over three bins. */
            peak += (double)out[(kpeak + 1) % GSM_WIN].r * out[(kpeak + 1) % GSM_WIN].r +
                    (double)out[(kpeak + 1) % GSM_WIN].i * out[(kpeak + 1) % GSM_WIN].i +
                    (double)out[(kpeak + GSM_WIN - 1) % GSM_WIN].r * out[(kpeak + GSM_WIN - 1) % GSM_WIN].r +
                    (double)out[(kpeak + GSM_WIN - 1) % GSM_WIN].i * out[(kpeak + GSM_WIN - 1) % GSM_WIN].i;
            bin  = (kpeak < GSM_WIN / 2 ? kpeak : kpeak - GSM_WIN) * GSM_FS / GSM_WIN;
            ratio = total > 0 ? peak / total : 0;
            tone = ratio > GSM_TONE_RATIO &&
                   fabs(bin - M2SDR_CAL_GSM_FCCH_HZ) < max_cfo_hz + GSM_FS / GSM_WIN;
        }

        if (tone) {
            /* The window that holds the most of the tone is the one on the burst. */
            if (!run || ratio > run_ratio) {
                run_best  = w;
                run_ratio = ratio;
                run_bin   = bin;
            }
            run++;
            continue;
        }
        if (run) {
            /* End of a run: an FCCH burst fills the window for (546 - 533) us and most of it
             * for a few hops around; a continuous tone gives a much longer run. */
            const size_t centre = run_best * GSM_HOP;
            const double dur_s  = ((double)(run - 1) * GSM_HOP + GSM_WIN) / GSM_FS;

            if (dur_s < 2.5 * 546e-6 && nbursts < GSM_MAX_BURSTS) {
                /* Fine frequency: phase turned over GSM_LAG samples in the middle of the
                 * burst, unwrapped with the bin of the peak. */
                double complex a = 0;
                double f;
                for (k = GSM_GUARD; k + GSM_LAG < GSM_WIN - GSM_GUARD; k++)
                    a += conjf(y[centre + k]) * y[centre + k + GSM_LAG];
                f  = carg(a) / (2.0 * M_PI) * GSM_FS / GSM_LAG;
                f += GSM_FS / GSM_LAG * round((run_bin - f) / (GSM_FS / GSM_LAG));
                freqs[nbursts] = f;
                times[nbursts] = (double)(centre + GSM_WIN / 2) / GSM_FS;
                nbursts++;
            }
            run = 0;
        }
    }

    if (nbursts >= GSM_MIN_BURSTS) {
        /* The FCCH bursts are 10 or 11 frames apart: gaps on the frame grid tell them from
         * other tones. */
        int on_grid = 0;
        double sum = 0, sum2 = 0, med;
        int b;

        for (b = 1; b < nbursts; b++) {
            const double frames = (times[b] - times[b - 1]) / GSM_FRAME_S;
            if (fabs(frames - round(frames)) < 0.1 && round(frames) >= 10)
                on_grid++;
        }
        for (b = 0; b < nbursts; b++) {
            sum  += freqs[b];
            sum2 += freqs[b] * freqs[b];
        }
        est->spread_hz = sqrt(fmax(0.0, sum2 / nbursts - (sum / nbursts) * (sum / nbursts)));
        med            = median(freqs, nbursts);
        est->quality   = (double)on_grid / (nbursts - 1);
        est->count     = nbursts;
        est->cfo_hz    = med - M2SDR_CAL_GSM_FCCH_HZ;
        est->found     = est->quality >= 0.5;
    } else {
        est->count = nbursts;
    }

    free(y); free(freqs); free(times); cal_fft_free(cfg);
    return 0;
}
