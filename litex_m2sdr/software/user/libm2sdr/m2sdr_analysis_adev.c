/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: frequency and phase of a carrier, Allan deviation within a capture and
 * over a series of captures.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_timing.h"
#include "m2sdr_fft.h"

#define MIN_COUNT       1024
#define MAX_FFT         65536   /* Coarse search: within a tenth of a bin of this, fine enough. */
#define MIN_BLOCKS      8
#define BLOCK_SNR       30.0    /* Of a sub-block: a phase error of 0.13 rad, safe to unwrap. */
#define MIN_PEAK_DB     18.0    /* Peak over the median of the spectrum; noise peaks at about 12 dB. */
#define DC_BINS         3       /* Bins each side of the centre left out of a search of the whole capture. */

/* Allan deviations */
/*------------------*/

/* Overlapping ADEV: sigma^2(tau) = sum (x[i+2m] - 2 x[i+m] + x[i])^2 / (2 tau^2 (n - 2m)).
 * Modified ADEV: the second differences are summed over m consecutive i first,
 * mod sigma^2(tau) = sum_j (sum_{i=j}^{j+m-1} d_i)^2 / (2 m^2 tau^2 (n - 3m + 1)). */
void m2sdr_adev_from_phase(const double *x, size_t n, double tau0_s, struct m2sdr_adev *dev)
{
    size_t m;
    double *d;

    memset(dev, 0, sizeof(*dev));
    if (!x || n < 3 || !(tau0_s > 0.0))
        return;
    d = malloc((n - 2) * sizeof(*d));
    if (!d)
        return;
    for (m = 1; n >= 2 * m + 1 && dev->ntau < M2SDR_ADEV_MAX_TAUS; m *= 2) {
        const size_t nd  = n - 2 * m;
        const double tau = (double)m * tau0_s;
        double sum = 0.0;
        size_t i;

        for (i = 0; i < nd; i++) {
            d[i]  = x[i + 2 * m] - 2.0 * x[i + m] + x[i];
            sum  += d[i] * d[i];
        }
        dev->tau_s[dev->ntau] = tau;
        dev->adev[dev->ntau]  = sqrt(sum / (2.0 * tau * tau * (double)nd));
        dev->terms[dev->ntau] = (int)nd;
        if (nd >= m + 2) {
            /* Running sum of m second differences: n - 3m + 1 of them, 3 at least. */
            const size_t ns = nd - m + 1;
            double run = 0.0, sum2 = 0.0;

            for (i = 0; i < m; i++)
                run += d[i];
            sum2 = run * run;
            for (i = 1; i < ns; i++) {
                run  += d[i + m - 1] - d[i - 1];
                sum2 += run * run;
            }
            dev->mdev[dev->ntau] = sqrt(sum2 / (2.0 * (double)m * (double)m * tau * tau * (double)ns));
        }
        dev->ntau++;
    }
    free(d);
}

/* Carrier estimate */
/*------------------*/

static int cmp_float(const void *a, const void *b)
{
    const float fa = *(const float *)a, fb = *(const float *)b;

    return (fa > fb) - (fa < fb);
}

static double wrap_pi(double p)
{
    return p - 2.0 * M_PI * floor((p + M_PI) / (2.0 * M_PI));
}

/* Strongest bin of a Hann spectrum of the first nfft samples, interpolated: the frequency in
 * Hz from the centre, the peak over the median of the bins searched, and the SNR per sample
 * that it implies. Returns -1 without a line. */
static int coarse_search(const float complex *x, size_t nfft, double fs, const struct m2sdr_carrier_config *cfg,
                         double *freq_hz, double *peak_ratio, double *snr)
{
    struct m2sdr_fft *fft = m2sdr_fft_alloc(nfft, false);
    float complex *in  = malloc(nfft * sizeof(*in));
    float complex *out = malloc(nfft * sizeof(*out));
    float *pw     = malloc(nfft * sizeof(*pw));
    float *sorted = malloc(nfft * sizeof(*sorted));
    const double bin = fs / (double)nfft;
    double lo, hi, sw = 0.0, sw2 = 0.0, best = -1.0, median;
    long k, k0, k1, kb = -1;
    size_t n, ns = 0;
    int ret = -1;

    if (!fft || !in || !out || !pw || !sorted)
        goto out;
    for (n = 0; n < nfft; n++) {
        const double w = 0.5 - 0.5 * cos(2.0 * M_PI * (double)n / (double)nfft);
        in[n]  = x[n] * (float)w;
        sw    += w;
        sw2   += w * w;
    }
    m2sdr_fft_run(fft, in, out);
    for (n = 0; n < nfft; n++) {
        const float complex v = out[(n + nfft / 2) % nfft];   /* Bin n at (n - nfft/2) * bin. */
        pw[n] = crealf(v) * crealf(v) + cimagf(v) * cimagf(v);
    }

    lo = -fs / 2.0;
    hi =  fs / 2.0;
    if (cfg && cfg->search_hz > 0.0) {
        lo = cfg->offset_hz - cfg->search_hz;
        hi = cfg->offset_hz + cfg->search_hz;
    }
    k0 = (long)ceil(lo / bin) + (long)(nfft / 2);
    k1 = (long)floor(hi / bin) + (long)(nfft / 2);
    if (k0 < 1)
        k0 = 1;
    if (k1 > (long)nfft - 2)
        k1 = (long)nfft - 2;
    if (k1 - k0 < 4)
        goto out;
    for (k = k0; k <= k1; k++) {
        const bool dc = !(cfg && cfg->search_hz > 0.0) && labs(k - (long)(nfft / 2)) <= DC_BINS;

        if (dc)
            continue;
        sorted[ns++] = pw[k];
        if (pw[k] > best) {
            best = pw[k];
            kb   = k;
        }
    }
    if (kb < 0 || ns < 4)
        goto out;
    qsort(sorted, ns, sizeof(*sorted), cmp_float);
    /* Median of exponential bins: ln 2 of their mean. */
    median = sorted[ns / 2] / log(2.0);
    if (!(median > 0.0))
        median = 1e-30;
    *peak_ratio = best / median;
    {
        /* Quadratic interpolation of the log power: Hann, within a few hundredths of a bin. */
        const double a = log(pw[kb - 1] + 1e-30), b = log(pw[kb] + 1e-30), c = log(pw[kb + 1] + 1e-30);
        const double den = a - 2.0 * b + c;
        const double p   = den < 0.0 ? 0.5 * (a - c) / den : 0.0;

        *freq_hz = ((double)(kb - (long)(nfft / 2)) + p) * bin;
    }
    /* Line of amplitude A: A^2 (sum w)^2 in its bin; noise sigma^2 sum w^2 in each bin. */
    *snr = *peak_ratio * sw2 / (sw * sw);
    ret  = *peak_ratio >= pow(10.0, MIN_PEAK_DB / 10.0) ? 0 : -1;
out:
    m2sdr_fft_free(fft);
    free(in);
    free(out);
    free(pw);
    free(sorted);
    return ret;
}

int m2sdr_carrier_estimate(const float complex *x, size_t count, double sample_rate, int64_t t0,
                           const struct m2sdr_carrier_config *cfg, struct m2sdr_carrier_result *res)
{
    const double carrier = cfg && cfg->carrier_hz > 0.0 ? cfg->carrier_hz : 0.0;
    const double scale   = carrier > 0.0 ? carrier : 1.0;
    double coarse_hz = 0.0, ratio = 0.0, snr0 = 0.0;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, tmid, slope, icpt, power = 0.0, amp2, noise, eta, rms = 0.0;
    double complex acc = 0.0;
    double *theta, *t, *xs;
    size_t nfft, blen, used, k, n;
    int nb;

    if (!x || !res || count < MIN_COUNT || !(sample_rate > 0.0))
        return -1;
    memset(res, 0, sizeof(*res));
    res->timestamp = (double)t0 + 0.5 * (double)(count - 1);

    for (nfft = 1; nfft * 2 <= count && nfft * 2 <= MAX_FFT; nfft *= 2)
        ;
    if (coarse_search(x, nfft, sample_rate, cfg, &coarse_hz, &ratio, &snr0) != 0)
        return 0;

    /* Sub-blocks: each long enough for BLOCK_SNR, not longer than the coarse FFT (whose error,
     * a tenth of its bin, then turns a sub-block by less than 0.6 rad). */
    if (cfg && cfg->blocks > 0) {
        nb = cfg->blocks;
        if (nb < MIN_BLOCKS)
            nb = MIN_BLOCKS;
        if (nb > M2SDR_ADEV_MAX_BLOCKS)
            nb = M2SDR_ADEV_MAX_BLOCKS;
    } else {
        const double bmin = BLOCK_SNR / (snr0 > 1e-12 ? snr0 : 1e-12);
        const double nbf  = (double)count / (bmin > 1.0 ? bmin : 1.0);

        nb = nbf > M2SDR_ADEV_MAX_BLOCKS ? M2SDR_ADEV_MAX_BLOCKS : (int)nbf;
        if (nb < MIN_BLOCKS)
            nb = MIN_BLOCKS;
    }
    while ((double)count / nb > (double)nfft)
        nb *= 2;
    if (nb > M2SDR_ADEV_MAX_BLOCKS)
        nb = M2SDR_ADEV_MAX_BLOCKS;
    blen = count / (size_t)nb;
    used = blen * (size_t)nb;

    theta = malloc((size_t)nb * sizeof(*theta));
    t     = malloc((size_t)nb * sizeof(*t));
    xs    = malloc((size_t)nb * sizeof(*xs));
    if (!theta || !t || !xs) {
        free(theta);
        free(t);
        free(xs);
        return -1;
    }

    /* Phase of each sub-block, the coarse frequency removed: the phase at its centre. The
     * mixer turns by a double phase increment, exact over long captures. */
    {
        const double w = -2.0 * M_PI * coarse_hz / sample_rate;
        const double complex step = cexp(I * w);
        double complex *sum = malloc((size_t)nb * sizeof(*sum));

        if (!sum) {
            free(theta);
            free(t);
            free(xs);
            return -1;
        }
        for (k = 0; k < (size_t)nb; k++) {
            double complex s = 0.0;
            /* Rotator restarted at each sub-block: exact to the rounding of cexp. */
            double complex rot = cexp(I * w * (double)(k * blen));
            for (n = 0; n < blen; n++) {
                const float complex v = x[k * blen + n];

                s     += (double complex)v * rot;
                rot   *= step;
                power += (double)(crealf(v) * crealf(v) + cimagf(v) * cimagf(v));
            }
            sum[k]   = s;
            theta[k] = carg(s);
            t[k]     = (double)(k * blen) + 0.5 * (double)(blen - 1);
        }
        /* Unwrap. */
        for (k = 1; k < (size_t)nb; k++)
            theta[k] = theta[k - 1] + wrap_pi(theta[k] - theta[k - 1]);

        /* Straight line through the phases. */
        tmid = 0.5 * (t[0] + t[nb - 1]);
        for (k = 0; k < (size_t)nb; k++) {
            const double u = t[k] - tmid;

            sx  += u;
            sy  += theta[k];
            sxx += u * u;
            sxy += u * theta[k];
        }
        slope = (sxy - sx * sy / nb) / (sxx - sx * sx / nb);
        icpt  = (sy - slope * sx) / nb;
        for (k = 0; k < (size_t)nb; k++) {
            const double r = theta[k] - icpt - slope * (t[k] - tmid);

            rms += r * r;
            acc += sum[k] * cexp(-I * (icpt + slope * (t[k] - tmid)));
        }
        free(sum);
    }

    amp2  = creal(acc) / (double)used;
    amp2 *= amp2;
    power /= (double)used;
    noise = power - amp2;
    if (noise < power * 1e-12)
        noise = power * 1e-12;
    eta   = amp2 / noise;

    res->found        = true;
    res->blocks       = nb;
    res->block_len    = blen;
    res->freq_hz      = coarse_hz + slope * sample_rate / (2.0 * M_PI);
    res->freq_mean_hz = coarse_hz + (theta[nb - 1] - theta[0]) / (t[nb - 1] - t[0]) * sample_rate / (2.0 * M_PI);
    /* Cramer-Rao bound of the frequency of a complex tone: 6 / (eta N (N^2 - 1)) (rad/sample)^2. */
    res->freq_std_hz  = sqrt(6.0 / (eta * (double)used * ((double)used * (double)used - 1.0))) *
                        sample_rate / (2.0 * M_PI);
    {
        /* Phase at the timestamp, the mixer and the fit added back. */
        const double tc = 0.5 * (double)(count - 1);

        res->phase_rad = wrap_pi(2.0 * M_PI * fmod(coarse_hz * tc / sample_rate, 1.0) +
                                 icpt + slope * (tc - tmid));
    }
    res->offset_ppb    = carrier > 0.0 ? (res->freq_hz - (cfg ? cfg->offset_hz : 0.0)) / carrier * 1e9 : 0.0;
    res->level_dbfs    = m2sdr_power_db(amp2);
    res->snr_db        = 10.0 * log10(eta);
    res->cn0_dbhz      = 10.0 * log10(eta * sample_rate);
    res->phase_rms_rad = sqrt(rms / nb);

    /* Short taus: the sub-block phases as time error over the nominal carrier. */
    for (k = 0; k < (size_t)nb; k++)
        xs[k] = theta[k] / (2.0 * M_PI * scale);
    m2sdr_adev_from_phase(xs, (size_t)nb, (double)blen / sample_rate, &res->short_tau);

    free(theta);
    free(t);
    free(xs);
    return 0;
}

/* Series over captures */
/*----------------------*/

int m2sdr_adev_series_init(struct m2sdr_adev_series *s, double sample_rate, double carrier_hz,
                           double offset_hz, size_t capacity)
{
    if (!s)
        return -1;
    memset(s, 0, sizeof(*s));
    if (!(sample_rate > 0.0) || !(carrier_hz > 0.0) || capacity < 3)
        return -1;
    s->timestamp = malloc(capacity * sizeof(*s->timestamp));
    s->freq_hz   = malloc(capacity * sizeof(*s->freq_hz));
    if (!s->timestamp || !s->freq_hz) {
        m2sdr_adev_series_free(s);
        return -1;
    }
    s->sample_rate = sample_rate;
    s->carrier_hz  = carrier_hz;
    s->offset_hz   = offset_hz;
    s->capacity    = capacity;
    return 0;
}

void m2sdr_adev_series_free(struct m2sdr_adev_series *s)
{
    if (!s)
        return;
    free(s->timestamp);
    free(s->freq_hz);
    memset(s, 0, sizeof(*s));
}

void m2sdr_adev_series_reset(struct m2sdr_adev_series *s)
{
    if (!s)
        return;
    s->count = 0;
    s->head  = 0;
}

void m2sdr_adev_series_add(struct m2sdr_adev_series *s, double timestamp, double freq_hz)
{
    if (!s || !s->capacity)
        return;
    s->timestamp[s->head] = timestamp;
    s->freq_hz[s->head]   = freq_hz;
    s->head = (s->head + 1) % s->capacity;
    if (s->count < s->capacity)
        s->count++;
}

int m2sdr_adev_series_compute(const struct m2sdr_adev_series *s, struct m2sdr_adev_result *res)
{
    const size_t n = s ? s->count : 0;
    const size_t first = s && s->count == s->capacity ? s->head : 0;
    double *y, *xp, tau0, t_first, mean = 0.0, stt = 0.0, sty = 0.0;
    size_t k;

    if (!s || !res || n < 3)
        return -1;
    memset(res, 0, sizeof(*res));
    y  = malloc(n * sizeof(*y));
    xp = malloc((n + 1) * sizeof(*xp));
    if (!y || !xp) {
        free(y);
        free(xp);
        return -1;
    }
    t_first = s->timestamp[first];
    tau0    = (s->timestamp[(first + n - 1) % s->capacity] - t_first) / (double)(n - 1) / s->sample_rate;
    res->points  = n;
    res->tau0_s  = tau0;
    res->span_s  = tau0 * (double)(n - 1);
    res->regular = tau0 > 0.0;
    for (k = 0; k < n; k++) {
        const size_t i = (first + k) % s->capacity;

        y[k]  = (s->freq_hz[i] - s->offset_hz) / s->carrier_hz;
        mean += y[k];
        if (k > 0) {
            const double dt = (s->timestamp[i] - s->timestamp[(i + s->capacity - 1) % s->capacity]) / s->sample_rate;

            if (fabs(dt - tau0) > 0.01 * tau0)
                res->regular = false;
        }
    }
    mean /= (double)n;
    res->offset_ppb = mean * 1e9;
    res->freq_hz    = s->offset_hz + mean * s->carrier_hz;

    /* Drift: least squares slope of y over the time of each point from the middle. */
    for (k = 0; k < n; k++) {
        const double u = ((double)k - 0.5 * (double)(n - 1)) * tau0;

        stt += u * u;
        sty += u * (y[k] - mean);
    }
    res->drift_ppb_s = stt > 0.0 ? sty / stt * 1e9 : 0.0;

    /* Mean frequencies to phase: x[k+1] = x[k] + y[k] tau0. */
    xp[0] = 0.0;
    for (k = 0; k < n; k++)
        xp[k + 1] = xp[k] + y[k] * tau0;
    if (tau0 > 0.0)
        m2sdr_adev_from_phase(xp, n + 1, tau0, &res->dev);
    free(y);
    free(xp);
    return 0;
}
