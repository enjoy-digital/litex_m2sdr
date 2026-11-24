/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: direction finding with two antennas (phase interferometer).
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

#define SPEED_OF_LIGHT   299792458.0
#define DEFAULT_FFT_LEN  4096
#define MIN_SEGMENTS     8
#define LOBE_BINS        1       /* On each side of the peak: the main lobe of the Hann window. */
#define DC_BINS          1       /* On each side of the DC bin, left out: LO leakage. */
#define MIN_SNR_DB       10.0
#define MIN_COHERENCE    0.5
#define FINE_STEP_DEG    0.02    /* Search of the MUSIC peak, */
#define SEARCH_DEG       10.0    /* this far around the angle. */

static double wrap_rad(double a)
{
    return a - 2.0 * M_PI * floor((a + M_PI) / (2.0 * M_PI));
}

static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Pseudo-spectra */
/*---------------*/

/* Covariance of the two channels, each scaled to unit power: [[1, conj(rho)], [rho, 1]], so
 * that a gain between the receivers does not bend the steering vector. Its eigenvalues are
 * 1 +- |rho|, and the eigenvector of the smaller one is (1, -exp(j arg rho)) / sqrt(2). */
struct df_cov {
    double complex rho;
    double l1, l2;
    double complex e[2];         /* Noise eigenvector. */
};

static void df_cov_init(struct df_cov *c, double complex rho)
{
    const double m = cabs(rho);

    c->rho  = rho;
    c->l1   = 1.0 + m;
    c->l2   = 1.0 - m;
    c->e[0] = M_SQRT1_2;
    c->e[1] = m > 0 ? -M_SQRT1_2 * rho / m : M_SQRT1_2;
}

/* For a phase psi of RX2 against RX1 (steering vector (1, exp(j psi))). */
static double df_beamscan(const struct df_cov *c, double psi)
{
    /* a^H R a / |a|^2 = 1 + Re(rho exp(-j psi)). */
    return 1.0 + creal(c->rho * cexp(-I * psi));
}

static double df_music(const struct df_cov *c, double psi)
{
    const double complex p = conj(c->e[0]) + conj(c->e[1]) * cexp(I * psi);
    const double proj = 0.5 * (creal(p) * creal(p) + cimag(p) * cimag(p));

    /* 1 / |e^H a|^2, the share of the noise eigenvalue added: the peak is as sharp as the
     * signal stands out of the noise, and stays finite. */
    return 1.0 / (proj + c->l2 / (c->l1 + c->l2));
}

/* Analysis */
/*----------*/

int m2sdr_df_analyse(const float complex *x1, const float complex *x2, size_t count, double sample_rate,
                     const struct m2sdr_df_config *cfg, struct m2sdr_df_result *res)
{
    struct m2sdr_cross cs;
    size_t fft_len, k, kc = 0, k0, k1;
    double r11 = 0, r22 = 0, best = -1, noise, kd = 0, std;
    double complex r21 = 0;
    float *sorted = NULL;
    int i, nb, rc = -1;

    if (!x1 || !x2 || !res || !cfg || !(sample_rate > 0) || !(cfg->spacing_m > 0) || !(cfg->rf_freq_hz > 0))
        return -1;
    fft_len = cfg->fft_len ? cfg->fft_len : DEFAULT_FFT_LEN;
    if (fft_len < 64 || fft_len > 65536 || (fft_len & (fft_len - 1)) || count < (MIN_SEGMENTS + 1) * fft_len / 2 ||
        fabs(cfg->freq_hz) >= sample_rate / 2 || cfg->bandwidth_hz < 0)
        return -1;
    memset(&cs, 0, sizeof(cs));
    if (m2sdr_cross_compute(x1, x2, count, sample_rate, fft_len, &cs) != 0)
        return -1;
    memset(res, 0, sizeof(*res));

    /* Noise of RX1 per bin: the median of the bins, which a signal narrower than half of the
     * capture does not raise; the median of an exponential law is ln 2 of its mean. */
    sorted = malloc(fft_len * sizeof(*sorted));
    if (!sorted)
        goto out;
    memcpy(sorted, cs.s11, fft_len * sizeof(*sorted));
    qsort(sorted, fft_len, sizeof(*sorted), cmp_float);
    noise = sorted[fft_len / 2] / M_LN2;

    /* The signal: the bin asked for, or the strongest one off DC. */
    if (cfg->freq_hz != 0) {
        kc = (size_t)lround(cfg->freq_hz / cs.bin_hz) + fft_len / 2;
    } else {
        for (k = 0; k < fft_len; k++) {
            const double p = cs.s11[k] + cs.s22[k];

            if ((k + DC_BINS >= fft_len / 2 && k <= fft_len / 2 + DC_BINS) || p <= best)
                continue;
            best = p;
            kc   = k;
        }
    }
    if (cfg->bandwidth_hz > 0) {
        const size_t half = (size_t)floor(0.5 * cfg->bandwidth_hz / cs.bin_hz);
        k0 = kc > half ? kc - half : 0;
        k1 = kc + half < fft_len ? kc + half : fft_len - 1;
    } else {
        k0 = kc > LOBE_BINS ? kc - LOBE_BINS : 0;
        k1 = kc + LOBE_BINS < fft_len ? kc + LOBE_BINS : fft_len - 1;
    }
    for (k = k0, nb = 0; k <= k1; k++) {
        if (k + DC_BINS >= fft_len / 2 && k <= fft_len / 2 + DC_BINS)
            continue;
        r11 += cs.s11[k];
        r22 += cs.s22[k];
        r21 += cs.s21[k];
        nb++;
    }
    rc = 0;
    if (!nb || !(r11 > 0) || !(r22 > 0))
        goto out;

    res->freq_hz      = m2sdr_cross_freq(&cs, kc);
    res->level_db     = m2sdr_power_db(r11 / 1.5);        /* Over the noise bandwidth of the Hann window. */
    res->snr_db       = m2sdr_power_db(r11 / (nb * noise) - 1.0);
    res->coherence    = (creal(r21) * creal(r21) + cimag(r21) * cimag(r21)) / (r11 * r22);
    res->gain_db      = m2sdr_power_db(r22) - m2sdr_power_db(r11);
    res->phase_raw_deg = carg(r21) * 180.0 / M_PI;
    res->phase_deg    = wrap_rad((res->phase_raw_deg - cfg->cal_phase_deg) * M_PI / 180.0) * 180.0 / M_PI;
    res->wavelength_m = SPEED_OF_LIGHT / (cfg->rf_freq_hz + res->freq_hz);
    res->spacing_wl   = cfg->spacing_m / res->wavelength_m;

    /* Phase: variance (1 - C) / (2 K C) for K segments (Bendat and Piersol); the bins of the
     * main lobe share their noise, counted as one. */
    std = res->coherence < 1.0 ? sqrt((1.0 - res->coherence) / (2.0 * cs.segments * res->coherence)) : 0.0;
    res->phase_std_deg = std * 180.0 / M_PI;
    if (res->snr_db < MIN_SNR_DB || res->coherence < MIN_COHERENCE)
        goto out;
    res->found = true;

    /* Candidates: phi + 2 pi m = 2 pi (d / lambda) sin(angle), |sin| <= 1. */
    {
        const double phi = res->phase_deg * M_PI / 180.0;
        const int mmax = (int)ceil(res->spacing_wl) + 1;
        int m;

        kd = 2.0 * M_PI * res->spacing_wl;
        for (m = -mmax; m <= mmax && res->candidates < M2SDR_DF_MAX_CANDIDATES; m++) {
            const double s = (phi + 2.0 * M_PI * m) / kd;

            if (fabs(s) <= 1.0)
                res->candidate_deg[res->candidates++] = asin(s) * 180.0 / M_PI;
        }
        if (!res->candidates) {
            /* Closer than lambda / 2: a phase beyond what the spacing can give, from noise or a
             * wrong calibration. The nearest end-fire angle. */
            res->candidate_deg[res->candidates++] = phi > 0 ? 90.0 : -90.0;
        }
        qsort(res->candidate_deg, (size_t)res->candidates, sizeof(res->candidate_deg[0]), cmp_double);
        res->ambiguous = res->candidates > 1;
        res->angle_deg = res->candidate_deg[0];
        for (i = 1; i < res->candidates; i++) {
            if (fabs(res->candidate_deg[i]) < fabs(res->angle_deg))
                res->angle_deg = res->candidate_deg[i];
        }
        {
            const double c = cos(res->angle_deg * M_PI / 180.0);
            res->angle_std_deg = c > 1e-3 ? std / (kd * c) * 180.0 / M_PI : 90.0;
        }
    }

    /* Pseudo-spectra, calibration taken off the covariance. */
    {
        struct df_cov cov;
        double bmax = 0, mmax = 0, peak = -1, at = 0, a;
        double bs[M2SDR_DF_POINTS], mu[M2SDR_DF_POINTS];

        df_cov_init(&cov, r21 / sqrt(r11 * r22) * cexp(-I * cfg->cal_phase_deg * M_PI / 180.0));
        for (i = 0; i < M2SDR_DF_POINTS; i++) {
            const double psi = kd * sin((i - 90) * M_PI / 180.0);

            bs[i] = df_beamscan(&cov, psi);
            mu[i] = df_music(&cov, psi);
            if (bs[i] > bmax)
                bmax = bs[i];
            if (mu[i] > mmax)
                mmax = mu[i];
        }
        for (i = 0; i < M2SDR_DF_POINTS; i++) {
            res->beamscan_db[i] = (float)m2sdr_power_db(bs[i] / bmax);
            res->music_db[i]    = (float)m2sdr_power_db(mu[i] / mmax);
        }
        /* The peak of the lobe of angle_deg (the others are its grating lobes) on a fine grid,
         * then a parabola through its neighbours. */
        for (a = fmax(-90.0, res->angle_deg - SEARCH_DEG); a <= fmin(90.0, res->angle_deg + SEARCH_DEG) + 1e-9;
             a += FINE_STEP_DEG) {
            const double v = df_music(&cov, kd * sin(a * M_PI / 180.0));
            if (v > peak) {
                peak = v;
                at   = a;
            }
        }
        {
            const double ym = df_music(&cov, kd * sin((at - FINE_STEP_DEG) * M_PI / 180.0));
            const double yp = df_music(&cov, kd * sin((at + FINE_STEP_DEG) * M_PI / 180.0));
            const double den = ym - 2.0 * peak + yp;

            res->music_peak_deg = at + (den < 0 ? 0.5 * (ym - yp) / den * FINE_STEP_DEG : 0.0);
        }
    }

out:
    m2sdr_cross_free(&cs);
    free(sorted);
    return rc;
}

double m2sdr_df_calibration(const struct m2sdr_df_result *res, double known_angle_deg)
{
    double geometry;

    if (!res)
        return 0.0;
    geometry = 2.0 * M_PI * res->spacing_wl * sin(known_angle_deg * M_PI / 180.0);
    return wrap_rad(res->phase_raw_deg * M_PI / 180.0 - geometry) * 180.0 / M_PI;
}
