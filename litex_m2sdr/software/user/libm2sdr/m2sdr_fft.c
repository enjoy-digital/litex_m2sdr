/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR radix-2 FFT, private to libm2sdr.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "m2sdr_fft.h"

struct m2sdr_fft {
    size_t n;
    float complex *w;              /* n / 2 twiddles. */
    uint32_t *rev;                 /* Bit-reversed indexes. */
};

void m2sdr_fft_free(struct m2sdr_fft *fft)
{
    if (fft) {
        free(fft->w);
        free(fft->rev);
        free(fft);
    }
}

struct m2sdr_fft *m2sdr_fft_alloc(size_t n, bool inverse)
{
    struct m2sdr_fft *fft = calloc(1, sizeof(*fft));
    unsigned bits = 0, j;
    size_t i;

    if (!fft || n < 2 || (n & (n - 1)) || n > ((size_t)1 << 28))
        goto fail;
    while (((size_t)1 << bits) < n)
        bits++;
    fft->n   = n;
    fft->w   = malloc((n / 2) * sizeof(*fft->w));
    fft->rev = malloc(n * sizeof(*fft->rev));
    if (!fft->w || !fft->rev)
        goto fail;
    for (i = 0; i < n / 2; i++)
        fft->w[i] = (float complex)cexp((inverse ? 2.0 : -2.0) * M_PI * I * (double)i / (double)n);
    for (i = 0; i < n; i++) {
        uint32_t r = 0;
        for (j = 0; j < bits; j++)
            r |= (uint32_t)((i >> j) & 1) << (bits - 1 - j);
        fft->rev[i] = r;
    }
    return fft;

fail:
    m2sdr_fft_free(fft);
    return NULL;
}

void m2sdr_fft_run(const struct m2sdr_fft *fft, const float complex *in, float complex *out)
{
    const size_t n = fft->n;
    size_t i, k, len;

    for (i = 0; i < n; i++)
        out[fft->rev[i]] = in[i];
    for (len = 2; len <= n; len <<= 1) {
        const size_t half = len / 2, step = n / len;
        for (i = 0; i < n; i += len) {
            float complex *a = out + i, *b = out + i + half;
            for (k = 0; k < half; k++) {
                const float complex t = b[k] * fft->w[k * step];
                b[k] = a[k] - t;
                a[k] = a[k] + t;
            }
        }
    }
}
