/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef SIMPLE_FFT_H
#define SIMPLE_FFT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct simple_fft_plan {
    int n;
    int stages;
    int *bitrev;
    float *tw_re;
    float *tw_im;
};

int simple_fft_init(struct simple_fft_plan *plan, int n);
void simple_fft_destroy(struct simple_fft_plan *plan);
void simple_fft_run(const struct simple_fft_plan *plan,
                    const float *in_re,
                    const float *in_im,
                    float *out_re,
                    float *out_im);

#ifdef __cplusplus
}
#endif

#endif /* SIMPLE_FFT_H */
