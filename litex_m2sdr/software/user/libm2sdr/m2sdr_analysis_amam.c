/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis, lab instrument: AM/AM, AM/PM, EVM and ACLR of a wideband signal.
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

#define DEFAULT_LINEAR_RMS 0.5
#define MIN_BIN_SAMPLES    100      /* Of the bin of the compression figures. */
#define BAND_DB            60.0     /* Bins of the reference within this of its strongest: its band. */

/* Power of the bins whose centres are within the channel of bw bins around bin `centre` (signed
 * bins, fractions allowed), of the transforms of the periods summed in py. */
static double band_power(const double *py, size_t period, double centre, double bw, double norm)
{
    const long lo = (long)ceil(centre - 0.5 * bw + 0.5 - 1e-9), hi = (long)floor(centre + 0.5 * bw - 0.5 + 1e-9);
    double sum = 0;
    long k;

    for (k = lo; k <= hi; k++)
        sum += py[(size_t)k & (period - 1)];
    return sum / norm;
}

int m2sdr_amam_analyse(const float complex *x, size_t count, double sample_rate, const float complex *ref,
                       size_t period, const struct m2sdr_amam_config *cfg, struct m2sdr_amam_result *res)
{
    struct m2sdr_fft *fwd = NULL, *inv = NULL;
    float complex *R = NULL, *y = NULL, *xa = NULL, *tmp = NULL;
    double *py = NULL;
    double complex bnum[M2SDR_AMAM_BINS], num = 0, c_slope = 0, g0;
    double bden[M2SDR_AMAM_BINS], den = 0, ey = 0, peak2 = 0, rmax = 0, rpow = 0, linear_rms, delta, ypeak = 0;
    long ilo = 0, ihi = -1;
    size_t lag, m, j, i, total, step;
    int b, rc = -1;

    if (!x || !ref || !res || !(sample_rate > 0) || !m2sdr_lab_pow2(period) || period < 64 || count < 2 * period)
        return -1;
    memset(res, 0, sizeof(*res));
    linear_rms = cfg && cfg->linear_rms > 0 ? cfg->linear_rms : DEFAULT_LINEAR_RMS;

    fwd = m2sdr_fft_alloc(period, false);
    inv = m2sdr_fft_alloc(period, true);
    R   = malloc(period * sizeof(*R));
    y   = malloc(period * sizeof(*y));
    xa  = malloc(period * sizeof(*xa));
    tmp = malloc(period * sizeof(*tmp));
    py  = calloc(period, sizeof(*py));
    if (!fwd || !inv || !R || !y || !xa || !tmp || !py)
        goto out;

    /* Whole samples of the delay. */
    if (m2sdr_lab_align(fwd, inv, x, ref, period, &lag, &res->sync_quality, R) != 0)
        goto out;
    rc = 0;
    if (res->sync_quality < M2SDR_LAB_MIN_SYNC_DB)
        goto out;
    m = (count - lag) / period;
    res->periods = (int)m;

    /* Cross-spectrum of the periods with the reference, and the power of each bin of the output
     * for the ACLR. */
    memset(tmp, 0, period * sizeof(*tmp));
    for (j = 0; j < m; j++) {
        m2sdr_fft_run(fwd, x + lag + j * period, y);
        for (i = 0; i < period; i++) {
            tmp[i] += y[i] * conjf(R[i]);
            py[i]  += (double)(crealf(y[i]) * crealf(y[i]) + cimagf(y[i]) * cimagf(y[i]));
        }
    }
    for (i = 0; i < period; i++)
        rpow = fmax(rpow, (double)(crealf(R[i]) * crealf(R[i]) + cimagf(R[i]) * cimagf(R[i])));

    /* Fraction of the delay: the slope of the phase of the cross-spectrum over the band of the
     * reference (a delay d turns bin k by -2 pi k d / period), a least-squares line weighted by
     * the power of the reference. The whole band sets the slope: the distortion of the device,
     * random from bin to bin, averages out. Twice: the second fit is on what the first left. */
    {
        int pass;
        long k;
        for (k = -(long)period / 2; k < (long)period / 2; k++) {
            const size_t a = (size_t)k & (period - 1);
            const double ra = crealf(R[a]) * crealf(R[a]) + cimagf(R[a]) * cimagf(R[a]);
            if (ra > rpow * pow(10.0, -BAND_DB / 10.0)) {
                if (ihi < ilo)
                    ilo = k;
                ihi = k;
            }
        }
        delta = 0;
        for (pass = 0; pass < 2; pass++) {
            double complex c0 = 0;
            double sw = 0, sx = 0, sp = 0, sxx = 0, sxp = 0;
            for (k = ilo; k <= ihi; k++) {
                const size_t a = (size_t)k & (period - 1);
                c0 += (double complex)tmp[a] * cexp(I * 2.0 * M_PI * (double)k * delta / (double)period);
            }
            c0 /= cabs(c0) + 1e-30;
            for (k = ilo; k <= ihi; k++) {
                const size_t a = (size_t)k & (period - 1);
                const double w = crealf(R[a]) * crealf(R[a]) + cimagf(R[a]) * cimagf(R[a]);
                const double ph = carg((double complex)tmp[a] * conj(c0) *
                                       cexp(I * 2.0 * M_PI * (double)k * delta / (double)period));
                if (w < rpow * 1e-3)
                    continue;
                sw  += w;
                sx  += w * (double)k;
                sp  += w * ph;
                sxx += w * (double)k * (double)k;
                sxp += w * (double)k * ph;
            }
            if (sw > 0 && sw * sxx - sx * sx > 0)
                delta -= (sw * sxp - sx * sp) / (sw * sxx - sx * sx) * (double)period / (2.0 * M_PI);
        }
    }
    /* Reference delayed by the fraction: its transform turned, back to time, unscaled inverse. */
    for (i = 0; i < period; i++) {
        const double k = (double)m2sdr_lab_signed_bin(i, period);
        tmp[i] = R[i] * (float complex)cexp(-I * 2.0 * M_PI * k * delta / (double)period) / (float)period;
    }
    m2sdr_fft_run(inv, tmp, xa);
    res->delay_samples = fmod((double)lag + delta + (double)period, (double)period);

    for (i = 0; i < period; i++) {
        const double p = crealf(xa[i]) * crealf(xa[i]) + cimagf(xa[i]) * cimagf(xa[i]);
        peak2 = fmax(peak2, p);
        den  += p;
    }
    res->input_rms_dbfs  = m2sdr_power_db(den / (double)period);
    res->input_peak_dbfs = m2sdr_power_db(peak2);
    rmax = sqrt(peak2);
    den  = 0;

    /* Per bin of input amplitude: the least-squares complex gain of the output over the input,
     * free of the bias of the noise. */
    memset(bnum, 0, sizeof(bnum));
    memset(bden, 0, sizeof(bden));
    total = m * period;
    for (i = 0; i < total; i++) {
        const float complex u = xa[i & (period - 1)], v = x[lag + i];
        const double a2 = crealf(u) * crealf(u) + cimagf(u) * cimagf(u);
        const double v2 = crealf(v) * crealf(v) + cimagf(v) * cimagf(v);
        const double complex p = (double complex)(v * conjf(u));
        int k = (int)(sqrt(a2) / rmax * M2SDR_AMAM_BINS);
        if (k >= M2SDR_AMAM_BINS)
            k = M2SDR_AMAM_BINS - 1;
        bnum[k] += p;
        bden[k] += a2;
        res->bin_count[k]++;
        num += p;
        den += a2;
        ey  += v2;
        ypeak = fmax(ypeak, v2);
    }
    res->output_rms_dbfs  = m2sdr_power_db(ey / (double)total);
    res->output_peak_dbfs = m2sdr_power_db(ypeak);
    if (!(den > 0) || cabs(num) <= 0)
        goto out;
    res->found = true;

    /* EVM after the least-squares gain: the residual is |y|^2 less the part along x. */
    {
        const double resid = ey - creal(num * conj(num)) / den;
        const double g2 = creal(num * conj(num)) / (den * den);
        res->evm_rms = resid > 0 ? sqrt(resid / (g2 * den)) : 0.0;
        res->evm_db  = 20.0 * log10(res->evm_rms + 1e-12);
    }

    /* Small-signal gain: the gains of the bins below linear_rms, a line against the mean square
     * amplitude of each bin, weighted by its energy, at zero amplitude. */
    {
        const double edge = linear_rms * sqrt(den / (double)total);
        double sw = 0, su = 0, suu = 0;
        double complex sg = 0, sug = 0;
        int nb = 0;
        for (b = 0; b < M2SDR_AMAM_BINS; b++) {
            const double top = (double)(b + 1) / M2SDR_AMAM_BINS * rmax;
            if (top > edge || !res->bin_count[b] || !(bden[b] > 0))
                continue;
            {
                const double w = bden[b], u = bden[b] / res->bin_count[b];
                const double complex g = bnum[b] / bden[b];
                sw  += w;
                su  += w * u;
                suu += w * u * u;
                sg  += w * g;
                sug += w * u * g;
                nb++;
            }
        }
        if (nb >= 2 && sw * suu - su * su > 0) {
            c_slope = (sw * sug - su * sg) / (sw * suu - su * su);
            g0      = (sg - c_slope * su) / sw;
        } else if (nb == 1) {
            g0 = sg / sw;
        } else {
            g0 = num / den;
        }
        if (cabs(g0) <= 0)
            g0 = num / den;
    }
    res->gain_db   = 20.0 * log10(cabs(g0));
    res->phase_deg = carg(g0) * 180.0 / M_PI;

    res->bins = M2SDR_AMAM_BINS;
    for (b = 0; b < M2SDR_AMAM_BINS; b++) {
        if (!res->bin_count[b] || !(bden[b] > 0)) {
            res->bin_in_dbfs[b] = res->bin_out_dbfs[b] = -200.0f;
            continue;
        }
        {
            const double complex g = bnum[b] / bden[b] / g0;
            const double in_db = m2sdr_power_db(bden[b] / res->bin_count[b]);
            res->bin_in_dbfs[b]   = (float)in_db;
            res->bin_gain_db[b]   = (float)(20.0 * log10(cabs(g) + 1e-30));
            res->bin_phase_deg[b] = (float)(carg(g) * 180.0 / M_PI);
            res->bin_out_dbfs[b]  = (float)(in_db + res->bin_gain_db[b]);
            if (res->bin_count[b] >= MIN_BIN_SAMPLES) {
                res->compression_db      = -res->bin_gain_db[b];
                res->am_pm_deg           = res->bin_phase_deg[b];
                res->compression_in_dbfs = in_db;
            }
        }
    }

    /* Points, spread over the capture. */
    step = total / M2SDR_AMAM_MAX_POINTS;
    if (step < 1)
        step = 1;
    for (i = 0; i < total && res->npoints < M2SDR_AMAM_MAX_POINTS; i += step) {
        const float complex u = xa[i & (period - 1)];
        const double complex v = (double complex)x[lag + i] / g0;
        res->point_in[res->npoints]        = cabsf(u);
        res->point_out[res->npoints]       = (float)cabs(v);
        res->point_phase_deg[res->npoints] = cabsf(u) > 0 ? (float)(carg(v * conj((double complex)u)) * 180.0 / M_PI) : 0.0f;
        res->npoints++;
    }

    /* ACLR, from the bins of the periods: the channel is the band of the reference unless given,
     * centred on it, the adjacent channels at the spacing while they fit. */
    if (ihi >= ilo) {
        const double bin_hz = sample_rate / (double)period;
        const double centre = 0.5 * (double)(ilo + ihi);
        const double bw  = cfg && cfg->channel_bw_hz > 0 ? cfg->channel_bw_hz / bin_hz : (double)(ihi - ilo + 1);
        const double sp  = cfg && cfg->channel_spacing_hz > 0 ? cfg->channel_spacing_hz / bin_hz : bw;
        const double norm = (double)m * (double)period * (double)period;
        int n;

        res->channel_bw_hz      = bw * bin_hz;
        res->channel_spacing_hz = sp * bin_hz;
        if (centre - 0.5 * bw >= -(double)(period / 2) && centre + 0.5 * bw <= (double)(period / 2)) {
            res->aclr            = true;
            res->acpr.channel_db = m2sdr_power_db(band_power(py, period, centre, bw, norm));
            for (n = 0; n < M2SDR_ACPR_MAX_ADJACENT; n++) {
                const double lo = centre - (n + 1) * sp, hi = centre + (n + 1) * sp;
                if (lo - 0.5 * bw < -(double)(period / 2) || hi + 0.5 * bw > (double)(period / 2))
                    break;
                res->acpr.lower_dbc[n] = m2sdr_power_db(band_power(py, period, lo, bw, norm)) - res->acpr.channel_db;
                res->acpr.upper_dbc[n] = m2sdr_power_db(band_power(py, period, hi, bw, norm)) - res->acpr.channel_db;
                res->acpr.adjacent     = n + 1;
            }
        }
    }

out:
    m2sdr_fft_free(fwd);
    m2sdr_fft_free(inv);
    free(R);
    free(y);
    free(xa);
    free(tmp);
    free(py);
    return rc;
}
