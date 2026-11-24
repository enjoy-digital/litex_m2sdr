/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR filters evaluated at any instant, private to libm2sdr.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>

#include "m2sdr_resample.h"

/* Table */
/*-------*/

static int kernel_alloc(struct m2sdr_kernel *k, int half)
{
    k->half = half;
    /* Two points more than the span: the interpolation reads one point past its index. */
    k->tab  = calloc((size_t)(2 * half * M2SDR_KERNEL_PER + 2), sizeof(*k->tab));
    return k->tab ? 0 : -1;
}

void m2sdr_kernel_free(struct m2sdr_kernel *k)
{
    if (!k)
        return;
    free(k->tab);
    k->tab  = NULL;
    k->half = 0;
}

int m2sdr_kernel_lowpass(struct m2sdr_kernel *k, double cutoff, double half)
{
    int i;

    if (!k || !(half >= 1.0) || half > 1e5 || !(cutoff > 0) || cutoff >= 0.5 || kernel_alloc(k, (int)ceil(half)) != 0)
        return -1;
    for (i = 0; i <= 2 * k->half * M2SDR_KERNEL_PER; i++) {
        const double tau = (double)i / M2SDR_KERNEL_PER - k->half;
        const double u   = 2.0 * cutoff * tau;
        const double s   = fabs(u) < 1e-12 ? 1.0 : sin(M_PI * u) / (M_PI * u);
        const double w   = 0.42 + 0.5 * cos(M_PI * tau / half) + 0.08 * cos(2.0 * M_PI * tau / half);

        k->tab[i] = fabs(tau) < half ? (float)(2.0 * cutoff * s * w) : 0.0f;
    }
    return 0;
}

/* Unit-energy root raised cosine, t in symbols. */
static double rrc(double t, double alpha)
{
    if (fabs(t) < 1e-9)
        return 1.0 - alpha + 4.0 * alpha / M_PI;
    if (fabs(fabs(t) - 1.0 / (4.0 * alpha)) < 1e-9)
        return alpha / sqrt(2.0) * ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * alpha)) +
                                    (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * alpha)));
    return (sin(M_PI * t * (1.0 - alpha)) + 4.0 * alpha * t * cos(M_PI * t * (1.0 + alpha))) /
           (M_PI * t * (1.0 - 16.0 * alpha * alpha * t * t));
}

int m2sdr_kernel_rrc(struct m2sdr_kernel *k, double sps, double alpha, int span)
{
    int i, half;

    if (!k || !(sps >= 1.0) || !(alpha > 0) || alpha > 1.0 || span < 1)
        return -1;
    half = (int)ceil(span * sps);
    if (kernel_alloc(k, half) != 0)
        return -1;
    /* The sum over the samples of a symbol period stands for the integral over the symbol:
     * 1 / sps per sample. */
    for (i = 0; i <= 2 * half * M2SDR_KERNEL_PER; i++) {
        const double tau = (double)i / M2SDR_KERNEL_PER - half;

        k->tab[i] = fabs(tau) <= span * sps ? (float)(rrc(tau / sps, alpha) / sps) : 0.0f;
    }
    return 0;
}

/* Evaluation */
/*------------*/

float complex m2sdr_kernel_at(const struct m2sdr_kernel *k, const float complex *x, size_t count, double t)
{
    long n0 = (long)ceil(t - k->half), n1 = (long)floor(t + k->half), n;
    double u;
    long base;
    float f, re = 0, im = 0;

    if (n0 < 0)
        n0 = 0;
    if (n1 > (long)count - 1)
        n1 = (long)count - 1;
    if (n1 < n0)
        return 0;
    /* Index of sample n in the table: (t - n + half) per; its fraction is the same for every n. */
    u    = (t - (double)n0 + k->half) * M2SDR_KERNEL_PER;
    base = (long)floor(u);
    f    = (float)(u - (double)base);
    for (n = n0; n <= n1; n++, base -= M2SDR_KERNEL_PER) {
        const float h = k->tab[base] + f * (k->tab[base + 1] - k->tab[base]);

        re += crealf(x[n]) * h;
        im += cimagf(x[n]) * h;
    }
    return re + I * im;
}

void m2sdr_kernel_run(const struct m2sdr_kernel *k, const float complex *x, size_t count, double t0, double step,
                      float complex *y, size_t ny)
{
    size_t m;

    for (m = 0; m < ny; m++)
        y[m] = m2sdr_kernel_at(k, x, count, t0 + (double)m * step);
}
