/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis, lab instrument: sweep (scalar network analysis with the group delay).
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

/* The samples and the products here are finite: complex products without the NaN and infinity
 * handling of C99 (__mulsc3). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("cx-limited-range")
#endif

#define DEFAULT_POINTS 101
#define DEFAULT_PERIOD 65536
#define MAX_EDGE       0.49    /* Of the sample rate: the points stay inside the band of the radio. */
#define MIN_DWELL      32      /* Samples of a tone. */
#define CHIRP_MARGIN   0.05    /* Of the span, on each side: the edge points see a flat chirp. */

/* Plan */
/*------*/

struct sweep_plan {
    enum m2sdr_sweep_stimulus stimulus;
    int    points;
    size_t period, dwell, settle;
    double start, step, amp;
    double chirp_lo, chirp_hi;
};

static int sweep_plan(const struct m2sdr_sweep_config *cfg, double fs, struct sweep_plan *p)
{
    if (!cfg || !(fs > 0))
        return -1;
    memset(p, 0, sizeof(*p));
    p->stimulus = cfg->stimulus;
    p->points   = cfg->points ? cfg->points : DEFAULT_POINTS;
    p->period   = cfg->period ? cfg->period : DEFAULT_PERIOD;
    p->amp      = pow(10.0, cfg->level_dbfs / 20.0);
    if ((p->stimulus != M2SDR_SWEEP_STEPPED && p->stimulus != M2SDR_SWEEP_CHIRP) ||
        p->points < 2 || p->points > M2SDR_SWEEP_MAX_POINTS || !m2sdr_lab_pow2(p->period) || p->period < 256 ||
        !isfinite(cfg->level_dbfs) || !(cfg->stop_hz > cfg->start_hz) ||
        cfg->start_hz < -MAX_EDGE * fs || cfg->stop_hz > MAX_EDGE * fs)
        return -1;
    p->start = cfg->start_hz;
    p->step  = (cfg->stop_hz - cfg->start_hz) / (p->points - 1);
    if (p->stimulus == M2SDR_SWEEP_STEPPED) {
        p->dwell  = p->period / (size_t)p->points;
        p->settle = cfg->settle ? cfg->settle : p->dwell / 4;
        if (p->dwell < MIN_DWELL || p->settle + MIN_DWELL / 2 > p->dwell)
            return -1;
    } else {
        /* A point takes the bins within half a step of it: one bin at least. */
        const double margin = CHIRP_MARGIN * (cfg->stop_hz - cfg->start_hz) + 2.0 * fs / (double)p->period;
        if (p->step < fs / (double)p->period)
            return -1;
        p->chirp_lo = fmax(cfg->start_hz - margin, -MAX_EDGE * fs);
        p->chirp_hi = fmin(cfg->stop_hz + margin, MAX_EDGE * fs);
    }
    return 0;
}

static double point_freq(const struct sweep_plan *p, int k)
{
    return p->start + k * p->step;
}

/* Stimulus */
/*----------*/

static void sweep_fill(const struct sweep_plan *p, double fs, float complex *x)
{
    size_t i;

    if (p->stimulus == M2SDR_SWEEP_STEPPED) {
        /* Phase continuous from tone to tone: less splatter at the steps. The tail of the period
         * past the last whole tone continues it. */
        double phase = 0;
        for (i = 0; i < p->period; i++) {
            size_t k = i / p->dwell;
            if (k >= (size_t)p->points)
                k = (size_t)p->points - 1;
            x[i]   = (float complex)(p->amp * cexp(I * phase));
            phase += 2.0 * M_PI * point_freq(p, (int)k) / fs;
            if (phase > M_PI)
                phase -= 2.0 * M_PI;
            else if (phase < -M_PI)
                phase += 2.0 * M_PI;
        }
    } else {
        /* f(i) = lo + (hi - lo) i / period. */
        const double rate = (p->chirp_hi - p->chirp_lo) / (double)p->period;
        for (i = 0; i < p->period; i++) {
            const double t = (double)i;
            const double cycles = (p->chirp_lo * t + 0.5 * rate * t * t) / fs;
            x[i] = (float complex)(p->amp * cexp(I * 2.0 * M_PI * (cycles - floor(cycles))));
        }
    }
}

int m2sdr_sweep_stimulus(const struct m2sdr_sweep_config *cfg, double sample_rate, float complex *x)
{
    struct sweep_plan p;

    if (!x || sweep_plan(cfg, sample_rate, &p) != 0)
        return -1;
    sweep_fill(&p, sample_rate, x);
    return 0;
}

/* Calibration */
/*-------------*/

static bool cal_matches(const struct m2sdr_sweep_cal *cal, int points, double start, double stop)
{
    const double tol = 1e-6 * (1.0 + fabs(stop - start));

    return cal->valid && cal->points == points && fabs(cal->start_hz - start) <= tol && fabs(cal->stop_hz - stop) <= tol;
}

int m2sdr_sweep_cal_store(const struct m2sdr_sweep_result *thru, struct m2sdr_sweep_cal *cal)
{
    if (!thru || !cal)
        return -1;
    memset(cal, 0, sizeof(*cal));
    if (!thru->found || thru->points < 2)
        return -1;
    cal->valid         = true;
    cal->points        = thru->points;
    cal->start_hz      = thru->freq_hz[0];
    cal->stop_hz       = thru->freq_hz[thru->points - 1];
    cal->timing_known  = thru->raw_timing;
    cal->delay_samples = thru->raw_delay;
    memcpy(cal->h, thru->raw, (size_t)thru->points * sizeof(cal->h[0]));
    return 0;
}

static double wrap_pi(double a)
{
    return a - 2.0 * M_PI * floor((a + M_PI) / (2.0 * M_PI));
}

int m2sdr_sweep_cal_apply(const struct m2sdr_sweep_cal *cal, struct m2sdr_sweep_result *res)
{
    double phi[M2SDR_SWEEP_MAX_POINTS], gd[M2SDR_SWEEP_MAX_POINTS];
    double delay, fs, mean = 0, sw = 0, sww = 0, sp = 0, swp = 0;
    bool timing;
    int k, n;

    if (!res || res->points < 2 || res->points > M2SDR_SWEEP_MAX_POINTS || !(res->sample_rate > 0))
        return -1;
    n = res->points;
    if (cal && !cal_matches(cal, n, res->freq_hz[0], res->freq_hz[n - 1]))
        return -1;
    res->calibrated = cal != NULL;
    if (!res->found)
        return 0;
    fs     = res->sample_rate;
    delay  = res->raw_delay;
    timing = res->raw_timing;
    if (cal) {
        delay -= cal->delay_samples;
        timing = timing && cal->timing_known;
    }
    /* Delays are modulo the period: that of a path is from 0 to the period, that of a device
     * (path less thru) the nearest to zero. */
    if (cal && res->period) {
        const double p = (double)res->period;
        delay -= p * floor(delay / p + 0.5);
    }

    /* S21, its phase unwrapped from point to point: the alignment left a small slope. */
    res->mag_min_db = 1e9;
    res->mag_max_db = -1e9;
    for (k = 0; k < n; k++) {
        double complex r = res->raw[k];
        if (cal)
            r = cabsf(cal->h[k]) > 0 ? r / (double complex)cal->h[k] : 0.0;
        res->mag_db[k] = m2sdr_power_db(creal(r) * creal(r) + cimag(r) * cimag(r));
        if (res->mag_db[k] < res->mag_min_db)
            res->mag_min_db = res->mag_db[k];
        if (res->mag_db[k] > res->mag_max_db)
            res->mag_max_db = res->mag_db[k];
        phi[k] = carg(r);
        if (k > 0)
            phi[k] = phi[k - 1] + wrap_pi(phi[k] - phi[k - 1]);
    }

    /* Group delay, samples: -dphi/domega, centred differences inside the span. */
    for (k = 0; k < n; k++) {
        const int a = k > 0 ? k - 1 : 0, b = k < n - 1 ? k + 1 : n - 1;
        const double dw = 2.0 * M_PI * (res->freq_hz[b] - res->freq_hz[a]) / fs;
        gd[k] = -(phi[b] - phi[a]) / dw;
        mean += gd[k];
    }
    mean /= n;

    res->delay_valid = timing;
    if (timing) {
        res->delay_s = delay / fs;
        for (k = 0; k < n; k++) {
            const double w = 2.0 * M_PI * res->freq_hz[k] / fs;
            res->group_delay_s[k] = (delay + gd[k]) / fs;
            res->phase_deg[k]     = wrap_pi(phi[k] - w * delay) * 180.0 / M_PI;
        }
    } else {
        /* Without the timing, the linear part of the phase is not known: its deviation from the
         * least-squares line, and the group delay from its mean. */
        double slope, icept;
        res->delay_s = 0;
        for (k = 0; k < n; k++) {
            const double w = 2.0 * M_PI * res->freq_hz[k] / fs;
            sw  += w;
            sww += w * w;
            sp  += phi[k];
            swp += w * phi[k];
        }
        slope = (n * swp - sw * sp) / (n * sww - sw * sw);
        icept = (sp - slope * sw) / n;
        for (k = 0; k < n; k++) {
            const double w = 2.0 * M_PI * res->freq_hz[k] / fs;
            res->group_delay_s[k] = (gd[k] - mean) / fs;
            res->phase_deg[k]     = (phi[k] - icept - slope * w) * 180.0 / M_PI;
        }
    }
    return 0;
}

/* Analysis */
/*----------*/

int m2sdr_sweep_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_sweep_config *cfg,
                        const struct m2sdr_sweep_cal *cal, struct m2sdr_sweep_result *res)
{
    struct sweep_plan p;
    struct m2sdr_fft *fwd = NULL, *inv = NULL;
    float complex *s = NULL, *S = NULL, *y = NULL;
    double complex *sum = NULL;
    double *sum2 = NULL, *den = NULL;
    long *bin0 = NULL, *bin1 = NULL;
    size_t lag, m, j, i;
    int k, rc = -1;

    if (!x || !res || sweep_plan(cfg, sample_rate, &p) != 0 || count < 2 * p.period)
        return -1;
    memset(res, 0, sizeof(*res));
    res->points      = p.points;
    res->period      = p.period;
    res->sample_rate = sample_rate;
    for (k = 0; k < p.points; k++)
        res->freq_hz[k] = point_freq(&p, k);
    if (cal && !cal_matches(cal, p.points, res->freq_hz[0], res->freq_hz[p.points - 1]))
        return -1;
    res->calibrated = cal != NULL;

    fwd  = m2sdr_fft_alloc(p.period, false);
    inv  = m2sdr_fft_alloc(p.period, true);
    s    = malloc(p.period * sizeof(*s));
    S    = malloc(p.period * sizeof(*S));
    y    = malloc(p.period * sizeof(*y));
    sum  = calloc((size_t)p.points, sizeof(*sum));
    sum2 = calloc((size_t)p.points, sizeof(*sum2));
    den  = calloc((size_t)p.points, sizeof(*den));
    bin0 = calloc((size_t)p.points, sizeof(*bin0));
    bin1 = calloc((size_t)p.points, sizeof(*bin1));
    if (!fwd || !inv || !s || !S || !y || !sum || !sum2 || !den || !bin0 || !bin1)
        goto out;
    sweep_fill(&p, sample_rate, s);

    /* Alignment: the capture is the stimulus, looped, delayed by the path. */
    if (m2sdr_lab_align(fwd, inv, x, s, p.period, &lag, &res->sync_quality, S) != 0)
        goto out;
    if (res->sync_quality < M2SDR_LAB_MIN_SYNC_DB) {
        rc = 0;
        goto out;
    }
    m = (count - lag) / p.period;

    if (p.stimulus == M2SDR_SWEEP_STEPPED) {
        /* Each point: the correlation of the settled part of its tone with the tone sent. */
        for (k = 0; k < p.points; k++) {
            const size_t a = (size_t)k * p.dwell + p.settle, b = (size_t)(k + 1) * p.dwell;
            for (i = a; i < b; i++)
                den[k] += crealf(s[i]) * crealf(s[i]) + cimagf(s[i]) * cimagf(s[i]);
        }
        for (j = 0; j < m; j++) {
            const float complex *xp = x + lag + j * p.period;
            for (k = 0; k < p.points; k++) {
                const size_t a = (size_t)k * p.dwell + p.settle, b = (size_t)(k + 1) * p.dwell;
                float complex acc = 0;
                for (i = a; i < b; i++)
                    acc += xp[i] * conjf(s[i]);
                {
                    const double complex h = (double complex)acc / den[k];
                    sum[k]  += h;
                    sum2[k] += creal(h) * creal(h) + cimag(h) * cimag(h);
                }
            }
        }
    } else {
        /* Each point: the least-squares ratio of the transforms over the bins within half a
         * step of it, the nearest bin when there is none. */
        const double bin_hz = sample_rate / (double)p.period;
        for (k = 0; k < p.points; k++) {
            const double f = point_freq(&p, k);
            long b;
            bin0[k] = (long)ceil((f - 0.5 * p.step) / bin_hz - 1e-9);
            bin1[k] = (long)ceil((f + 0.5 * p.step) / bin_hz - 1e-9) - 1;
            if (bin1[k] < bin0[k])
                bin0[k] = bin1[k] = lround(f / bin_hz);
            for (b = bin0[k]; b <= bin1[k]; b++) {
                const float complex v = S[(size_t)b & (p.period - 1)];
                den[k] += crealf(v) * crealf(v) + cimagf(v) * cimagf(v);
            }
        }
        for (j = 0; j < m; j++) {
            m2sdr_fft_run(fwd, x + lag + j * p.period, y);
            for (k = 0; k < p.points; k++) {
                double complex acc = 0, h;
                long b;
                for (b = bin0[k]; b <= bin1[k]; b++) {
                    const size_t q = (size_t)b & (p.period - 1);
                    acc += (double complex)(y[q] * conjf(S[q]));
                }
                h        = acc / den[k];
                sum[k]  += h;
                sum2[k] += creal(h) * creal(h) + cimag(h) * cimag(h);
            }
        }
    }

    /* Mean over the periods, and its spread: the noise of the point. */
    res->periods = (int)m;
    for (k = 0; k < p.points; k++) {
        const double complex h = sum[k] / (double)m;
        const double h2 = creal(h) * creal(h) + cimag(h) * cimag(h);
        res->raw[k] = (float complex)h;
        if (m >= 2) {
            const double var = (sum2[k] / (double)m - h2) * (double)m / (double)(m - 1);
            res->snr_db[k] = var > 0 ? m2sdr_power_db(h2 / (var / (double)m)) : 200.0;
        }
    }
    res->raw_timing = cfg->timing_known;
    res->raw_delay  = (double)lag;
    if (cfg->timing_known)
        res->raw_delay = (double)((lag + cfg->capture_offset) % p.period);
    res->found = true;
    rc = m2sdr_sweep_cal_apply(cal, res);

out:
    m2sdr_fft_free(fwd);
    m2sdr_fft_free(inv);
    free(s);
    free(S);
    free(y);
    free(sum);
    free(sum2);
    free(den);
    free(bin0);
    free(bin1);
    return rc;
}
