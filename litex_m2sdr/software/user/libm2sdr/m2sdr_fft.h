/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR radix-2 FFT, private to libm2sdr: the library has no FFT dependency.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_FFT_H
#define M2SDR_FFT_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>

struct m2sdr_fft;

/* Plan of n points, n a power of two from 2. The inverse is not scaled. NULL on a bad size or
 * an allocation failure. */
struct m2sdr_fft *m2sdr_fft_alloc(size_t n, bool inverse);
void m2sdr_fft_free(struct m2sdr_fft *fft);

/* Transform of in to out, both of n points; they must not overlap. */
void m2sdr_fft_run(const struct m2sdr_fft *fft, const float complex *in, float complex *out);

#endif /* M2SDR_FFT_H */
