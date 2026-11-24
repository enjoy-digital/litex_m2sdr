/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis, lab instrument: helpers shared by its analyses, private to libm2sdr.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYSIS_LAB_INTERNAL_H
#define M2SDR_ANALYSIS_LAB_INTERNAL_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>

#include "m2sdr_analysis.h"
#include "m2sdr_fft.h"

/* Correlation peak over its mean power, dB, for a capture to hold the stimulus: noise reads
 * about ln(period), 10 to 11 dB for 2^14 to 2^16 samples. */
#define M2SDR_LAB_MIN_SYNC_DB 16.0

static inline bool m2sdr_lab_pow2(size_t n)
{
    return n >= 2 && (n & (n - 1)) == 0;
}

/* Signed index of bin b of a transform of n points: from -n/2 to n/2 - 1. */
static inline long m2sdr_lab_signed_bin(size_t b, size_t n)
{
    return b < n / 2 ? (long)b : (long)b - (long)n;
}

/* Lag of the capture on the reference: x[lag + i] follows ref[i]. From the circular correlation
 * of the first period of x (period samples at least) with ref, by the plans fwd and inv of
 * period points; R (may be NULL) takes the transform of ref. Returns 0, or -1 on an allocation
 * failure. */
int m2sdr_lab_align(const struct m2sdr_fft *fwd, const struct m2sdr_fft *inv, const float complex *x,
                    const float complex *ref, size_t period, size_t *lag, double *quality_db,
                    float complex *R);

/* Power of the line of bin k: its main lobe, `lobe` bins on each side, the noise of that band
 * (noise_bin per bin, linear) removed; and that band's noise in *floor_out (may be NULL). Linear. */
double m2sdr_lab_line_power(const struct m2sdr_psd *psd, size_t k, size_t lobe, double noise_bin, double *floor_out);

/* Bin of a frequency, from the centre of the capture, clamped to the spectrum. */
size_t m2sdr_lab_psd_bin(const struct m2sdr_psd *psd, double freq_hz);

/* Noise per bin of a spectrum, linear, from the median of its bins. */
double m2sdr_lab_noise_bin(const struct m2sdr_psd *psd);

#endif /* M2SDR_ANALYSIS_LAB_INTERNAL_H */
