/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR filters evaluated at any instant, private to libm2sdr: a kernel known in continuous
 * time, kept in a fine table, gives the filtered signal between the samples. This resamples a
 * capture to a rate tied to a symbol rate, and samples a matched filter at the symbol instants.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_RESAMPLE_H
#define M2SDR_RESAMPLE_H

#include <complex.h>
#include <stddef.h>

/* h(tau) for tau from -half to +half input samples, KERNEL_PER points per sample. */
struct m2sdr_kernel {
    float *tab;
    int    half;
};

#define M2SDR_KERNEL_PER 32

/* Low-pass of DC gain 1: a sinc cut at `cutoff` (cycles per input sample, the -6 dB point)
 * under a Blackman window `half` samples on each side (not a whole number: the same filter at
 * two rates). The transition, to 74 dB down, is 5.5 / (2 half) wide around the cutoff.
 * Returns 0, or -1 on a bad argument or allocation failure. */
int m2sdr_kernel_lowpass(struct m2sdr_kernel *k, double cutoff, double half);

/* Root raised cosine of `sps` input samples per symbol and roll-off alpha, cut `span` symbols
 * on each side. Scaled so that symbols a_k under a unit root raised cosine read a_k at their
 * instants through it. */
int m2sdr_kernel_rrc(struct m2sdr_kernel *k, double sps, double alpha, int span);

void m2sdr_kernel_free(struct m2sdr_kernel *k);

/* x through the kernel at instant t (in input samples): the sum of x[n] h(t - n). Samples
 * outside 0..count-1 count as zero. */
float complex m2sdr_kernel_at(const struct m2sdr_kernel *k, const float complex *x, size_t count, double t);

/* y[m] = x through the kernel at t0 + m step, for m < ny. */
void m2sdr_kernel_run(const struct m2sdr_kernel *k, const float complex *x, size_t count, double t0, double step,
                      float complex *y, size_t ny);

#endif /* M2SDR_RESAMPLE_H */
