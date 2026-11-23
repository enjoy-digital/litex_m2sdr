/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: pulsed signals (radar, DFS test patterns, pulsed interferers).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_monitoring.h"

/* The samples and the products here are finite: complex products without the NaN and infinity
 * handling of C99 (__mulsc3). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("cx-limited-range")
#endif

#define FLOOR_BLOCK        64      /* Samples of the blocks of the noise floor. */
#define FLOOR_QUANTILE     0.25    /* Their quantile taken: room for pulses in 3/4 of the blocks. */
#define DEFAULT_THRESHOLD  10.0    /* dB over the floor. With 4 samples averaged, noise exceeds it
                                    * with a probability of about 1e-13 per sample. */
#define DEFAULT_SMOOTH     4       /* Samples. */
#define HYSTERESIS         0.5     /* A detection ends 3 dB under its threshold. */
#define MIN_COHERENCE      0.5     /* Under it, the pulse is neither CW nor LFM. Noise alone
                                    * keeps about 0.7 at 0 dB of SNR, 0.9 at 7 dB. */
#define MIN_CHIRP_BT       2.0     /* Swept bandwidth times width over which a chirp is LFM. */
#define MIN_FREQ_SAMPLES   8       /* Inside a pulse, for its frequency. */

/* Helpers */
/*---------*/

/* k-th smallest of v[0..n-1] (reordered): selection of Wirth. */
static float select_k(float *v, size_t n, size_t k)
{
    size_t l = 0, r = n - 1;

    while (r > l) {
        const float pivot = v[k];
        size_t i = l, j = r;

        do {
            while (v[i] < pivot)
                i++;
            while (pivot < v[j])
                j--;
            if (i <= j) {
                const float t = v[i];
                v[i] = v[j];
                v[j] = t;
                i++;
                if (j == 0)
                    break;
                j--;
            }
        } while (i <= j);
        if (j < k)
            l = i;
        if (k < i)
            r = j;
    }
    return v[k];
}

static double db10(double p)
{
    return 10.0 * log10(p + 1e-30);
}

/* Fractional index where the amplitude crosses `level` between samples i and i + 1. */
static double crossing(const float *amp, size_t i, double level)
{
    const double a = amp[i], b = amp[i + 1];

    if (b == a)
        return (double)i + 0.5;
    return (double)i + (level - a) / (b - a);
}

/* One pulse */
/*-----------*/

/* Measures the pulse detected over [a, b) (power envelope above the threshold). amp is the
 * amplitude of every sample, tmp room for b - a floats. Returns false when it has no edge. */
static bool measure_pulse(const float complex *x, const float *amp, size_t count, double fs, size_t a, size_t b,
                          size_t margin, float *tmp, struct m2sdr_pulse_info *pl)
{
    const size_t lo = a > margin ? a - margin : 0;
    const size_t hi = b + margin < count ? b + margin : count;
    size_t i, i50r, i50f, w0, w1, n;
    double top, h50, h10, h90, t50r, t50f, t10, t90, peak = 0, sum = 0;
    double complex acc = 0, acc1 = 0, coh = 0;
    double abs_sum = 0, k = 0, f, tc;

    memset(pl, 0, sizeof(*pl));
    pl->truncated = a == 0 || b >= count;

    /* Top: the median amplitude over the detection, robust to the noise and to overshoots. */
    for (i = a; i < b; i++)
        tmp[i - a] = amp[i];
    top = select_k(tmp, b - a, (b - a) / 2);
    h50 = 0.5 * top;
    h10 = 0.1 * top;
    h90 = 0.9 * top;

    /* 50 % edges: the first and the last samples above, around the detection. */
    for (i50r = lo; i50r < hi && amp[i50r] < h50; i50r++)
        ;
    for (i50f = hi - 1; i50f > i50r && amp[i50f] < h50; i50f--)
        ;
    if (i50r >= hi)
        return false;
    t50r = i50r > 0 ? crossing(amp, i50r - 1, h50) : 0.0;
    t50f = i50f + 1 < count ? crossing(amp, i50f, h50) : (double)i50f;
    if (i50r == 0 || i50f + 1 >= count)
        pl->truncated = true;

    /* 10 % and 90 % points on each edge, from its 50 % point outwards and inwards. */
    for (i = i50r; i > 0 && amp[i - 1] >= h10; i--)
        ;
    t10 = i > 0 ? crossing(amp, i - 1, h10) : 0.0;
    for (w0 = i50r; w0 <= i50f && amp[w0] < h90; w0++)
        ;
    t90 = w0 > 0 ? crossing(amp, w0 - 1, h90) : 0.0;
    pl->rise_s = (t90 - t10) / fs;
    for (w1 = i50f; w1 > w0 && amp[w1] < h90; w1--)
        ;
    t90 = w1 + 1 < count ? crossing(amp, w1, h90) : (double)w1;
    for (i = i50f; i + 1 < count && amp[i + 1] >= h10; i++)
        ;
    t10 = i + 1 < count ? crossing(amp, i, h10) : (double)i;
    pl->fall_s = (t10 - t90) / fs;

    pl->start_s = t50r / fs;
    pl->width_s = (t50f - t50r) / fs;
    for (i = lo; i < hi; i++)
        if ((double)amp[i] * amp[i] > peak)
            peak = (double)amp[i] * amp[i];
    pl->peak_dbfs = db10(peak);

    /* Mean power and intrapulse: on the top, between the 90 % points; on the 50 % ones when it
     * is short. */
    if (w1 < w0 + MIN_FREQ_SAMPLES) {
        w0 = i50r;
        w1 = i50f;
    }
    for (i = w0; i <= w1; i++)
        sum += (double)amp[i] * amp[i];
    pl->mean_dbfs = db10(sum / (double)(w1 - w0 + 1));
    n = w1 - w0 + 1;
    if (n < 4) {
        pl->coherence = 0;
        pl->modulation = M2SDR_PULSE_OTHER;
        return true;
    }
    /* Chirp rate: z[i] = x[i + 1] x*[i] turns by 2 pi f(t) / fs; z[i + m] z*[i] by
     * 2 pi k m / fs^2. m at half the pulse for the best resolution; unambiguous for sweeps up to
     * the sample rate over the pulse. */
    if (n >= MIN_FREQ_SAMPLES) {
        const size_t m = (n - 1) / 2;

        for (i = w0; i + m + 1 <= w1; i++) {
            const float complex z0 = x[i + 1] * conjf(x[i]);
            const float complex zm = x[i + m + 1] * conjf(x[i + m]);
            acc += zm * conjf(z0);
        }
        k = carg(acc) * fs * fs / (2.0 * M_PI * (double)m);
    }
    /* Frequency at the middle: lag-1 product of the dechirped samples. */
    tc = 0.5 * (double)(w0 + w1);
    {
        double complex prev = 0;

        for (i = w0; i <= w1; i++) {
            const double t = ((double)i - tc) / fs;
            const double complex y = x[i] * cexp(-I * M_PI * k * t * t);

            if (i > w0)
                acc1 += y * conj(prev);
            prev = y;
        }
    }
    f = carg(acc1) * fs / (2.0 * M_PI);
    /* Coherence with the model: the dechirped, detuned samples add up in phase. */
    for (i = w0; i <= w1; i++) {
        const double t = ((double)i - tc) / fs;

        coh      += x[i] * cexp(-I * (M_PI * k * t * t + 2.0 * M_PI * f * t));
        abs_sum  += amp[i];
    }
    pl->freq_hz        = f;
    pl->chirp_hz_per_s = k;
    pl->coherence      = abs_sum > 0 ? cabs(coh) / abs_sum : 0;
    if (pl->coherence < MIN_COHERENCE)
        pl->modulation = M2SDR_PULSE_OTHER;
    else if (fabs(k) * ((double)n / fs) * ((double)n / fs) >= MIN_CHIRP_BT)
        pl->modulation = M2SDR_PULSE_LFM;
    else {
        pl->modulation     = M2SDR_PULSE_CW;
        pl->chirp_hz_per_s = 0;
    }
    return true;
}

/* Train */
/*-------*/

/* PRI pattern of the leading edges toa[0..n-1] of the whole pulses. */
static void analyse_train(const double *toa, int n, double fs, double width_s, struct m2sdr_pulse_result *res)
{
    double d[M2SDR_PULSE_MAX_PULSES];
    double sum = 0, sum2 = 0, tol;
    int nd = n - 1, i, m;

    if (n < 3) {
        res->pri_kind = M2SDR_PRI_NONE;
        return;
    }
    res->pri_min_s = 1e30;
    res->pri_max_s = 0;
    for (i = 0; i < nd; i++) {
        d[i] = toa[i + 1] - toa[i];
        sum  += d[i];
        sum2 += d[i] * d[i];
        if (d[i] < res->pri_min_s)
            res->pri_min_s = d[i];
        if (d[i] > res->pri_max_s)
            res->pri_max_s = d[i];
    }
    res->pri_mean_s = sum / nd;
    res->pri_std_s  = sqrt(fmax(sum2 / nd - res->pri_mean_s * res->pri_mean_s, 0.0));
    res->prf_hz     = 1.0 / res->pri_mean_s;
    res->duty_cycle = width_s / res->pri_mean_s;

    /* The shortest pattern of intervals that repeats at least twice. */
    tol = fmax(2.0 / fs, 0.005 * res->pri_min_s);
    res->pri_kind = M2SDR_PRI_JITTER;
    for (m = 1; m <= M2SDR_PULSE_MAX_STAGGER && 2 * m <= nd; m++) {
        int j, ok = 1;

        for (i = 0; i + m < nd && ok; i++)
            ok = fabs(d[i] - d[i + m]) <= tol;
        if (!ok)
            continue;
        res->pri_kind = m == 1 ? M2SDR_PRI_CONSTANT : M2SDR_PRI_STAGGER;
        res->stagger  = m;
        for (j = 0; j < m; j++) {
            double s = 0;
            int c = 0;

            for (i = j; i < nd; i += m, c++)
                s += d[i];
            res->pri_s[j] = s / c;
        }
        /* The mean over whole patterns. */
        res->pri_mean_s = 0;
        for (j = 0; j < m; j++)
            res->pri_mean_s += res->pri_s[j] / m;
        res->prf_hz     = 1.0 / res->pri_mean_s;
        res->duty_cycle = width_s / res->pri_mean_s;
        break;
    }
}

/* Analysis */
/*----------*/

int m2sdr_pulse_analyse(const float complex *x, size_t count, double sample_rate,
                        const struct m2sdr_pulse_config *cfg, struct m2sdr_pulse_result *res)
{
    float *amp = NULL, *env = NULL, *tmp = NULL;
    double toa[M2SDR_PULSE_MAX_PULSES];
    double power = 0, peak = 0, floor_p, thr, sum, threshold_db = DEFAULT_THRESHOLD;
    double width = 0, pk = 0, rise = 0, fall = 0, freq = 0, chirp = 0;
    size_t i, nb, len = DEFAULT_SMOOTH, min_len = 0;
    int nwhole = 0, nsum = 0, nmod[3] = { 0, 0, 0 }, rc = -1;

    if (!x || !res || !(sample_rate > 0) || count < 1024)
        return -1;
    memset(res, 0, sizeof(*res));
    if (cfg && cfg->threshold_db > 0)
        threshold_db = cfg->threshold_db;
    if (cfg && cfg->smooth_s > 0)
        len = (size_t)fmax(1.0, round(cfg->smooth_s * sample_rate));
    if (cfg && cfg->min_width_s > 0)
        min_len = (size_t)round(cfg->min_width_s * sample_rate);
    if (len > count / 8)
        return -1;

    amp = malloc(count * sizeof(*amp));
    env = malloc(count * sizeof(*env));
    tmp = malloc(count * sizeof(*tmp));
    if (!amp || !env || !tmp)
        goto out;
    for (i = 0; i < count; i++) {
        const double p = crealf(x[i] * conjf(x[i]));

        amp[i] = (float)sqrt(p);
        power += p;
        if (p > peak)
            peak = p;
    }
    res->power_dbfs = db10(power / (double)count);

    /* Noise floor: a low quantile of the power of blocks. The mean of FLOOR_BLOCK samples of
     * noise has a spread of 1/sqrt(FLOOR_BLOCK); its quartile is 0.674 of it under the mean. */
    nb = count / FLOOR_BLOCK;
    for (i = 0; i < nb; i++) {
        size_t j;

        sum = 0;
        for (j = 0; j < FLOOR_BLOCK; j++)
            sum += (double)amp[i * FLOOR_BLOCK + j] * amp[i * FLOOR_BLOCK + j];
        tmp[i] = (float)(sum / FLOOR_BLOCK);
    }
    floor_p = select_k(tmp, nb, (size_t)(FLOOR_QUANTILE * (double)nb)) / (1.0 - 0.674 / sqrt(FLOOR_BLOCK));
    floor_p = fmax(floor_p, peak * 1e-12);   /* A capture without noise. */
    thr = floor_p * pow(10.0, threshold_db / 10.0);
    res->noise_dbfs     = db10(floor_p);
    res->threshold_dbfs = db10(thr);

    /* Detection envelope: power averaged over len samples, centred. */
    sum = 0;
    for (i = 0; i < count + len / 2; i++) {
        if (i < count)
            sum += (double)amp[i] * amp[i];
        if (i >= len)
            sum -= (double)amp[i - len] * amp[i - len];
        if (i >= len / 2)
            env[i - len / 2] = (float)(sum / (double)len);
    }

    /* Detections, with hysteresis. */
    i = 0;
    while (i < count) {
        size_t a, b;
        struct m2sdr_pulse_info pl;

        while (i < count && env[i] <= thr)
            i++;
        if (i >= count)
            break;
        a = i;
        while (i < count && env[i] >= HYSTERESIS * thr)
            i++;
        b = i;
        if (b - a < min_len)
            continue;
        if (!measure_pulse(x, amp, count, sample_rate, a, b, len + 2, tmp, &pl))
            continue;
        if (res->nlisted < M2SDR_PULSE_MAX_PULSES) {
            res->pulses[res->nlisted++] = pl;
            if (!pl.truncated)
                toa[nwhole++] = pl.start_s;
        }
        res->npulses++;
    }
    res->found = res->npulses > 0;

    /* Summary over the whole pulses, or all of them when none is whole. */
    for (i = 0; i < (size_t)res->nlisted; i++) {
        const struct m2sdr_pulse_info *pl = &res->pulses[i];

        if (pl->truncated && nwhole > 0)
            continue;
        width += pl->width_s;
        pk    += pl->peak_dbfs;
        rise  += pl->rise_s;
        fall  += pl->fall_s;
        freq  += pl->freq_hz;
        chirp += pl->chirp_hz_per_s;
        nmod[pl->modulation]++;
        nsum++;
    }
    if (nsum > 0) {
        res->width_s        = width / nsum;
        res->peak_dbfs      = pk / nsum;
        res->rise_s         = rise / nsum;
        res->fall_s         = fall / nsum;
        res->freq_hz        = freq / nsum;
        res->chirp_hz_per_s = chirp / nsum;
        res->modulation     = nmod[1] > nmod[0] ? M2SDR_PULSE_LFM : M2SDR_PULSE_CW;
        if (nmod[2] > nmod[res->modulation])
            res->modulation = M2SDR_PULSE_OTHER;
    }
    analyse_train(toa, nwhole, sample_rate, res->width_s, res);
    rc = 0;
out:
    free(amp);
    free(env);
    free(tmp);
    return rc;
}
