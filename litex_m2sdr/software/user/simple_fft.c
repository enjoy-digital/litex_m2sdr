/* SPDX-License-Identifier: BSD-2-Clause */

#include "simple_fft.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int is_power_of_two(int x)
{
    return (x > 1) && ((x & (x - 1)) == 0);
}

static int ilog2_int(int x)
{
    int n = 0;
    while ((1 << n) < x)
        n++;
    return n;
}

static int reverse_bits(int x, int bits)
{
    int y = 0;
    int i;
    for (i = 0; i < bits; i++) {
        y = (y << 1) | (x & 1);
        x >>= 1;
    }
    return y;
}

int simple_fft_init(struct simple_fft_plan *plan, int n)
{
    int k;

    if (!plan || !is_power_of_two(n))
        return -1;

    memset(plan, 0, sizeof(*plan));

    plan->n = n;
    plan->stages = ilog2_int(n);

    plan->bitrev = (int *)malloc((size_t)n * sizeof(int));
    plan->tw_re = (float *)malloc((size_t)(n / 2) * sizeof(float));
    plan->tw_im = (float *)malloc((size_t)(n / 2) * sizeof(float));

    if (!plan->bitrev || !plan->tw_re || !plan->tw_im) {
        simple_fft_destroy(plan);
        return -1;
    }

    for (k = 0; k < n; k++)
        plan->bitrev[k] = reverse_bits(k, plan->stages);

    for (k = 0; k < n / 2; k++) {
        double phase = -2.0 * M_PI * (double)k / (double)n;
        plan->tw_re[k] = (float)cos(phase);
        plan->tw_im[k] = (float)sin(phase);
    }

    return 0;
}

void simple_fft_destroy(struct simple_fft_plan *plan)
{
    if (!plan)
        return;

    free(plan->bitrev);
    free(plan->tw_re);
    free(plan->tw_im);

    memset(plan, 0, sizeof(*plan));
}

void simple_fft_run(const struct simple_fft_plan *plan,
                    const float *in_re,
                    const float *in_im,
                    float *out_re,
                    float *out_im)
{
    int n;
    int stage;
    int i;

    if (!plan || !in_re || !in_im || !out_re || !out_im)
        return;

    n = plan->n;

    for (i = 0; i < n; i++) {
        int r = plan->bitrev[i];
        out_re[i] = in_re[r];
        out_im[i] = in_im[r];
    }

    for (stage = 1; stage <= plan->stages; stage++) {
        int half = 1 << (stage - 1);
        int span = half << 1;
        int tw_stride = n / span;
        int block;

        for (block = 0; block < n; block += span) {
            int j;
            for (j = 0; j < half; j++) {
                int even = block + j;
                int odd = even + half;
                int tw = j * tw_stride;

                float w_re = plan->tw_re[tw];
                float w_im = plan->tw_im[tw];
                float odd_re = out_re[odd];
                float odd_im = out_im[odd];

                float t_re = w_re * odd_re - w_im * odd_im;
                float t_im = w_re * odd_im + w_im * odd_re;
                float e_re = out_re[even];
                float e_im = out_im[even];

                out_re[even] = e_re + t_re;
                out_im[even] = e_im + t_im;
                out_re[odd] = e_re - t_re;
                out_im[odd] = e_im - t_im;
            }
        }
    }
}
