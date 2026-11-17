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

#include "m2sdr_cal_dsp.h"
#include "kissfft/kiss_fft.h"

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
    kiss_fft_cfg cfg = kiss_fft_alloc(LTE_N, 1, NULL, NULL);
    kiss_fft_cpx in[LTE_N], out[LTE_N];
    int n;

    memset(in, 0, sizeof(in));
    for (n = 0; n < 62; n++) {
        const double ph = -M_PI * u * (n < 31 ? n * (n + 1.0) : (n + 1.0) * (n + 2.0)) / 63.0;
        const int k = n < 31 ? LTE_N - 31 + n : n - 30;   /* Subcarriers -31..-1 and 1..31. */
        in[k].r = (float)cos(ph);
        in[k].i = (float)sin(ph);
    }
    kiss_fft(cfg, in, out);
    for (n = 0; n < LTE_N; n++)
        p[n] = (out[n].r + I * out[n].i) / (float)sqrt(62.0);
    free(cfg);
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
    float complex pss[3][LTE_N];
    float complex ref[LTE_N];
    float *acc, *energy;
    double cp_quality = 0, frac;
    double best_metric = 0, best_fh = 0, best_match = 0;
    double pss_energy = 0;
    double sum = 0, sum2 = 0;
    int best_root = 0;
    int hyps, r, h, n;

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

    acc    = malloc(LTE_HALF * sizeof(*acc));
    energy = calloc(LTE_HALF, sizeof(*energy));
    if (!acc || !energy) {
        free(acc);
        free(energy);
        return -1;
    }
    for (r = 0; r < 3; r++)
        lte_pss(roots[r], pss[r]);
    for (n = 0; n < LTE_N; n++)
        pss_energy += crealf(pss[0][n]) * crealf(pss[0][n]) + cimagf(pss[0][n]) * cimagf(pss[0][n]);

    /* Energy of the signal under the PSS at every timing, added over the half frames: the
     * correlation is compared to it, so that a burst of power is not taken for a PSS. */
    {
        size_t f, t;
        for (f = 0; f < half_frames; f++) {
            const float complex *p = x + f * LTE_HALF;
            double e = 0;
            for (n = 0; n < LTE_N; n++)
                e += crealf(p[n]) * crealf(p[n]) + cimagf(p[n]) * cimagf(p[n]);
            for (t = 0; t < LTE_HALF; t++) {
                energy[t] += (float)e;
                e += crealf(p[t + LTE_N]) * crealf(p[t + LTE_N]) + cimagf(p[t + LTE_N]) * cimagf(p[t + LTE_N])
                   - crealf(p[t]) * crealf(p[t]) - cimagf(p[t]) * cimagf(p[t]);
                if (e < 0)
                    e = 0;
            }
        }
    }

    /* PSS search: every timing of a half frame, every root, and the offsets that the cyclic
     * prefix allows, one subcarrier apart. The correlations of the half frames are added
     * without their phase. The PSS identifies the carrier and selects the offset. */
    hyps = (int)floor((max_cfo_hz + 0.5 * LTE_SCS) / LTE_SCS);
    for (r = 0; r < 3; r++) {
        for (h = -hyps; h <= hyps; h++) {
            const double fh = frac + h * LTE_SCS;
            double peak = 0, total = 0, match = 0;
            size_t f, t;

            if (fabs(fh) > max_cfo_hz)
                continue;
            for (n = 0; n < LTE_N; n++)
                ref[n] = conjf(pss[r][n] * cexpf(I * (float)(2.0 * M_PI * fh * n / FS)));
            memset(acc, 0, LTE_HALF * sizeof(*acc));
            for (f = 0; f < half_frames; f++) {
                const float complex *p = x + f * LTE_HALF;
                for (t = 0; t < LTE_HALF; t++) {
                    float complex c = 0;
                    for (n = 0; n < LTE_N; n++)
                        c += p[t + n] * ref[n];
                    acc[t] += crealf(c) * crealf(c) + cimagf(c) * cimagf(c);
                }
            }
            for (t = 0; t < LTE_HALF; t++) {
                total += acc[t];
                if (acc[t] > peak) {
                    peak  = acc[t];
                    match = energy[t] > 0 ? acc[t] / (energy[t] * pss_energy) : 0;
                }
            }
            if (total > 0 && peak * LTE_HALF / total > best_metric) {
                best_metric = peak * LTE_HALF / total;
                best_match  = match;
                best_fh     = fh;
                best_root   = r;
            }
        }
    }
    free(acc);
    free(energy);

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

#define CHAN_TAPS 65

int m2sdr_cal_channelize(const float complex *x, size_t count, double offset_hz,
                         float complex *y, size_t *out_count)
{
    const double fs_in = FS * M2SDR_CAL_SCAN_DECIM;
    const size_t ny = count / M2SDR_CAL_SCAN_DECIM > CHAN_TAPS ? (count - CHAN_TAPS) / M2SDR_CAL_SCAN_DECIM : 0;
    float complex taps[CHAN_TAPS];
    double complex mean = 0;
    double tsum = 0;
    size_t i;
    int k;

    if (!x || !y || !out_count || !ny)
        return -1;

    /* Low-pass at 0.7 MHz (the PSS and its neighbourhood, a GSM carrier), moved to the offset:
     * the mix and the filter are one complex filter. */
    for (k = 0; k < CHAN_TAPS; k++) {
        const double t  = k - (CHAN_TAPS - 1) / 2.0;
        const double fc = 0.7e6 / fs_in;
        const double s  = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        const double w  = s * (0.54 - 0.46 * cos(2 * M_PI * k / (CHAN_TAPS - 1)));
        tsum   += w;
        taps[k] = (float complex)(w * cexp(I * 2.0 * M_PI * offset_hz * t / fs_in));
    }
    for (k = 0; k < CHAN_TAPS; k++)
        taps[k] /= (float)tsum;
    for (i = 0; i < ny; i++) {
        const float complex *p = x + i * M2SDR_CAL_SCAN_DECIM;
        float complex a = 0;
        for (k = 0; k < CHAN_TAPS; k++)
            a += p[k] * taps[CHAN_TAPS - 1 - k];
        /* The filter leaves the channel at its offset: bring it to 0 at the output rate. */
        y[i] = a * cexpf(-I * (float)(2.0 * M_PI * fmod(offset_hz * (double)i * M2SDR_CAL_SCAN_DECIM / fs_in, 1.0)));
        mean += y[i];
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
    kiss_fft_cfg cfg;
    kiss_fft_cpx in[GSM_WIN], out[GSM_WIN];
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
    cfg   = kiss_fft_alloc(GSM_WIN, 0, NULL, NULL);
    if (!y || !freqs || !times || !cfg) {
        free(y); free(freqs); free(times); free(cfg);
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
            kiss_fft(cfg, in, out);
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

    free(y); free(freqs); free(times); free(cfg);
    return 0;
}
