/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis, lab instrument: compression (gain against the input level, P1dB).
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

#define TONE_FFT      4096
#define TONE_LOBE     6       /* Bins of the main lobe of the flat-top window, on each side. */
#define TONE_SEARCH   0.01    /* Of the sample rate, on each side of the tone. */
#define MIN_TONE_DB   10.0    /* Over the noise of its band. */
#define LINEAR_STEPS  3       /* Lowest steps of the small-signal gain. */

/* Tone level */
/*------------*/

int m2sdr_compression_tone(const float complex *x, size_t count, double sample_rate, double freq_hz,
                           double *level_dbfs)
{
    struct m2sdr_psd psd;
    struct m2sdr_psd_peak peak;
    size_t fft_len = TONE_FFT;
    double noise, floor_power, power;
    int rc = -1;

    if (!x || !level_dbfs || !(sample_rate > 0) || !(fabs(freq_hz) < 0.5 * sample_rate) || count < 256)
        return -1;
    while (fft_len > count)
        fft_len /= 2;

    /* Flat-top: the level of the tone wherever it falls between the bins. */
    memset(&psd, 0, sizeof(psd));
    if (m2sdr_psd_compute(x, count, sample_rate, fft_len, M2SDR_WINDOW_FLATTOP, &psd) != 0)
        goto out;
    if (m2sdr_psd_peak(&psd, freq_hz - TONE_SEARCH * sample_rate, freq_hz + TONE_SEARCH * sample_rate, &peak) != 0)
        goto out;
    noise       = m2sdr_lab_noise_bin(&psd);
    power       = m2sdr_lab_line_power(&psd, m2sdr_lab_psd_bin(&psd, peak.freq_hz), TONE_LOBE, noise, &floor_power);
    *level_dbfs = m2sdr_power_db(power);
    rc = power > floor_power * pow(10.0, MIN_TONE_DB / 10.0) ? 0 : 1;
out:
    m2sdr_psd_free(&psd);
    return rc;
}

/* Fit */
/*-----*/

struct step_level {
    double in, out;
};

static int cmp_step(const void *a, const void *b)
{
    const double x = ((const struct step_level *)a)->in, y = ((const struct step_level *)b)->in;
    return (x > y) - (x < y);
}

/* Input where the compression first reaches `target`, interpolated between the steps around it. */
static bool crossing(const struct m2sdr_compression_result *res, double target, double *in)
{
    int i;

    for (i = 1; i < res->steps; i++) {
        const double c0 = res->compression_db[i - 1], c1 = res->compression_db[i];
        if (c1 >= target && c0 < target) {
            *in = res->input_dbfs[i - 1] + (target - c0) * (res->input_dbfs[i] - res->input_dbfs[i - 1]) / (c1 - c0);
            return true;
        }
    }
    return false;
}

int m2sdr_compression_fit(const double *input_dbfs, const double *output_dbfs, int n,
                          struct m2sdr_compression_result *res)
{
    struct step_level st[M2SDR_COMPRESSION_MAX_STEPS] = { 0 };
    double g0 = 0;
    int i, lin;

    if (!input_dbfs || !output_dbfs || !res || n < 0 || n > M2SDR_COMPRESSION_MAX_STEPS)
        return -1;
    memset(res, 0, sizeof(*res));
    for (i = 0; i < n; i++) {
        if (!isfinite(input_dbfs[i]) || !isfinite(output_dbfs[i]))
            return -1;
        st[i].in  = input_dbfs[i];
        st[i].out = output_dbfs[i];
    }
    qsort(st, (size_t)n, sizeof(st[0]), cmp_step);
    res->steps = n;
    if (n < 2)
        return 0;

    /* Small-signal gain: the lowest steps, where the device is taken as linear. */
    lin = n < LINEAR_STEPS ? n : LINEAR_STEPS;
    for (i = 0; i < lin; i++)
        g0 += st[i].out - st[i].in;
    g0 /= lin;
    res->found                = true;
    res->small_signal_gain_db = g0;
    res->max_output_dbfs      = -1e9;
    res->max_compression_db   = -1e9;
    for (i = 0; i < n; i++) {
        res->input_dbfs[i]     = st[i].in;
        res->output_dbfs[i]    = st[i].out;
        res->gain_db[i]        = st[i].out - st[i].in;
        res->compression_db[i] = g0 - res->gain_db[i];
        if (st[i].out > res->max_output_dbfs)
            res->max_output_dbfs = st[i].out;
        if (res->compression_db[i] > res->max_compression_db)
            res->max_compression_db = res->compression_db[i];
    }

    /* The output at a compression point is the input plus the gain less the compression. */
    res->p1db = crossing(res, 1.0, &res->p1db_in_dbfs);
    if (res->p1db)
        res->p1db_out_dbfs = res->p1db_in_dbfs + g0 - 1.0;
    res->p3db = crossing(res, 3.0, &res->p3db_in_dbfs);
    if (res->p3db)
        res->p3db_out_dbfs = res->p3db_in_dbfs + g0 - 3.0;
    return 0;
}

int m2sdr_compression_analyse(const struct m2sdr_compression_step *steps, int n, double sample_rate, double freq_hz,
                              struct m2sdr_compression_result *res)
{
    double in[M2SDR_COMPRESSION_MAX_STEPS], out[M2SDR_COMPRESSION_MAX_STEPS];
    int i, used = 0;

    if (!steps || !res || n < 1 || n > M2SDR_COMPRESSION_MAX_STEPS)
        return -1;
    for (i = 0; i < n; i++) {
        double level;
        const int rc = m2sdr_compression_tone(steps[i].x, steps[i].count, sample_rate, freq_hz, &level);
        if (rc < 0)
            return -1;
        if (rc == 0) {
            in[used]  = steps[i].input_dbfs;
            out[used] = level;
            used++;
        }
    }
    return m2sdr_compression_fit(in, out, used, res);
}
