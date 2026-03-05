/* SPDX-License-Identifier: BSD-2-Clause */

#include "simple_fft.h"

#include <stdlib.h>
#include <string.h>

static int is_power_of_two(int x)
{
    return (x > 1) && ((x & (x - 1)) == 0);
}

int simple_fft_init(struct simple_fft_plan *plan, int n)
{
    if (!plan || !is_power_of_two(n))
        return -1;

    memset(plan, 0, sizeof(*plan));

    plan->n = n;
    plan->cfg = kiss_fft_alloc(n, 0, NULL, NULL);
    plan->tmp_in = (kiss_fft_cpx *)malloc((size_t)n * sizeof(kiss_fft_cpx));
    plan->tmp_out = (kiss_fft_cpx *)malloc((size_t)n * sizeof(kiss_fft_cpx));

    if (!plan->cfg || !plan->tmp_in || !plan->tmp_out) {
        simple_fft_destroy(plan);
        return -1;
    }

    return 0;
}

void simple_fft_destroy(struct simple_fft_plan *plan)
{
    if (!plan)
        return;

    if (plan->cfg)
        kiss_fft_free(plan->cfg);
    free(plan->tmp_in);
    free(plan->tmp_out);

    memset(plan, 0, sizeof(*plan));
}

void simple_fft_run(const struct simple_fft_plan *plan,
                    const float *in_re,
                    const float *in_im,
                    float *out_re,
                    float *out_im)
{
    int i;

    if (!plan || !in_re || !in_im || !out_re || !out_im)
        return;
    if (!plan->cfg || !plan->tmp_in || !plan->tmp_out)
        return;

    for (i = 0; i < plan->n; i++) {
        plan->tmp_in[i].r = in_re[i];
        plan->tmp_in[i].i = in_im[i];
    }
    kiss_fft(plan->cfg, plan->tmp_in, plan->tmp_out);
    for (i = 0; i < plan->n; i++) {
        out_re[i] = plan->tmp_out[i].r;
        out_im[i] = plan->tmp_out[i].i;
    }
}
