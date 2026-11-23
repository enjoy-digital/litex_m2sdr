/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the libm2sdr monitoring analysis (pulses) on synthetic signals of known parameters.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../libm2sdr/m2sdr_analysis_monitoring.h"

static int failures;

#define CHECK_NEAR(name, value, expected, tol) do { \
    const double v_ = (value), e_ = (expected); \
    const int ok_ = fabs(v_ - e_) <= (tol); \
    printf("  %-34s %12.4f (expected %10.4f +-%g) %s\n", name, v_, e_, (double)(tol), ok_ ? "ok" : "FAIL"); \
    if (!ok_) failures++; \
} while (0)

#define CHECK(name, cond) do { \
    const int ok_ = (cond); \
    printf("  %-34s %s\n", name, ok_ ? "ok" : "FAIL"); \
    if (!ok_) failures++; \
} while (0)

/* Signals */
/*---------*/

static unsigned long long rng_state = 0x2545f4914f6cdd1dull;

static double rng_uniform(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return ((double)(rng_state >> 11) + 0.5) / 9007199254740992.0;
}

/* Complex Gaussian noise of power `power`. */
static void add_noise(float complex *x, size_t count, double power)
{
    size_t i;

    for (i = 0; i < count; i++) {
        const double r = sqrt(-power * log(rng_uniform()));
        const double a = 2.0 * M_PI * rng_uniform();
        x[i] += (float)(r * cos(a)) + I * (float)(r * sin(a));
    }
}

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* Pulse of amplitude amp whose leading edge crosses 50 % at t0, width w between the 50 %
 * points, raised cosine edges of tr (from 0 to 100 %: 10 to 90 % in 0.5903 tr). Frequency f0
 * at its middle, chirp k; with a code, its chips of +-1 share the width. */
struct pulse_signal {
    double t0, w, tr, amp, f0, k;
    const int *code;
    int nchips;
};

static void add_pulse(float complex *x, size_t count, double fs, const struct pulse_signal *p)
{
    const double tm = p->t0 + 0.5 * p->w;
    long i0 = (long)floor((p->t0 - p->tr) * fs), i1 = (long)ceil((p->t0 + p->w + p->tr) * fs), i;

    if (i0 < 0)
        i0 = 0;
    if (i1 > (long)count)
        i1 = (long)count;
    for (i = i0; i < i1; i++) {
        const double t = (double)i / fs - p->t0, u = (double)i / fs - tm;
        double e;

        if (t < -0.5 * p->tr)
            e = 0;
        else if (t < 0.5 * p->tr)
            e = 0.5 * (1.0 - cos(M_PI * (t + 0.5 * p->tr) / p->tr));
        else if (t < p->w - 0.5 * p->tr)
            e = 1;
        else if (t < p->w + 0.5 * p->tr)
            e = 0.5 * (1.0 + cos(M_PI * (t - p->w + 0.5 * p->tr) / p->tr));
        else
            e = 0;
        if (p->code) {
            int chip = (int)floor(t / p->w * p->nchips);

            chip = chip < 0 ? 0 : chip >= p->nchips ? p->nchips - 1 : chip;
            e *= p->code[chip];
        }
        x[i] += (float complex)(p->amp * e * cexp(I * (2.0 * M_PI * p->f0 * u + M_PI * p->k * u * u)));
    }
}

/* Pulses */
/*--------*/

#define PFS    10.0e6
#define PCOUNT (1u << 18)   /* 26.2 ms. */

static const char *mod_name(enum m2sdr_pulse_modulation m)
{
    return m == M2SDR_PULSE_CW ? "CW" : m == M2SDR_PULSE_LFM ? "LFM" : "other";
}

/* Train of identical pulses at the leading edges t0 + sum of the PRIs taken in turn. */
static int make_train(float complex *x, size_t count, double fs, const struct pulse_signal *p, const double *pri,
                      int npri, double snr_db)
{
    struct pulse_signal q = *p;
    int n = 0;

    memset(x, 0, count * sizeof(*x));
    while (q.t0 + q.w + q.tr < (double)count / fs) {
        add_pulse(x, count, fs, &q);
        q.t0 += pri[n % npri];
        n++;
    }
    add_noise(x, count, p->amp * p->amp * pow(10.0, -snr_db / 10.0));
    return n;
}

static void test_pulse(void)
{
    static const int barker13[13] = { 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1 };
    float complex *x = malloc(PCOUNT * sizeof(*x));
    struct m2sdr_pulse_result *res = malloc(sizeof(*res));
    struct m2sdr_pulse_config cfg;
    struct pulse_signal p;
    double pri[3], t;
    int n, i, ok;

    printf("Pulses:\n");
    if (!x || !res) {
        CHECK("allocation", 0);
        free(x);
        free(res);
        return;
    }

    /* CW pulses, constant PRI. SNR 30 dB on the samples: noise of 3 % of the amplitude, a timing
     * error at the 50 % point of 0.03 / (pi / (2 tr)) = 0.019 tr = 38 ns, and on the 10 % and
     * 90 % points about twice that (the slope is 0.59 of it there). The frequency of the lag-1
     * product over 100 samples: sqrt(2 / SNR) / sqrt(100) rad per sample, 7 kHz rms; the mean
     * over the pulses, 1.5 kHz. The peak of 100 noisy samples: the top plus about 2.5 sigma
     * of the noise along it (0.011): +0.5 dB. */
    p = (struct pulse_signal){ 0.3e-3, 10e-6, 2e-6, 0.5, 1.234e6, 0, NULL, 0 };
    pri[0] = 1e-3;
    n = make_train(x, PCOUNT, PFS, &p, pri, 1, 30.0);
    printf(" CW, width 10 us, PRI 1 ms, SNR 30 dB (%d pulses)\n", n);
    t = now_s();
    CHECK("analyse", m2sdr_pulse_analyse(x, PCOUNT, PFS, NULL, res) == 0);
    printf("  time %.2f ms\n", 1e3 * (now_s() - t));
    CHECK("found", res->found);
    CHECK_NEAR("pulses", res->npulses, n, 0);
    CHECK_NEAR("noise floor (dBFS)", res->noise_dbfs, 20.0 * log10(0.5) - 30.0, 0.3);
    CHECK_NEAR("first start (us)", res->pulses[0].start_s * 1e6, 300.0, 0.05);
    CHECK_NEAR("width (us)", res->width_s * 1e6, 10.0, 0.05);
    CHECK_NEAR("rise 10-90 (us)", res->rise_s * 1e6, 0.5903 * 2.0, 0.08);
    CHECK_NEAR("fall 90-10 (us)", res->fall_s * 1e6, 0.5903 * 2.0, 0.08);
    CHECK_NEAR("peak (dBFS)", res->peak_dbfs, 20.0 * log10(0.5) + 0.5, 0.4);
    CHECK_NEAR("mean power (dBFS)", res->pulses[1].mean_dbfs, 20.0 * log10(0.5), 0.2);
    CHECK_NEAR("frequency (kHz)", res->freq_hz * 1e-3, 1234.0, 5.0);
    CHECK("modulation CW", res->modulation == M2SDR_PULSE_CW);
    CHECK("PRI constant", res->pri_kind == M2SDR_PRI_CONSTANT && res->stagger == 1);
    CHECK_NEAR("PRI (us)", res->pri_s[0] * 1e6, 1000.0, 0.02);
    CHECK_NEAR("PRF (Hz)", res->prf_hz, 1000.0, 0.02);
    CHECK_NEAR("duty cycle (%)", 100.0 * res->duty_cycle, 1.0, 0.01);

    /* Linear FM: 2 MHz in 50 us, -0.5 MHz at the middle, SNR 20 dB. The chirp rate from the
     * lag products over 450 samples: well within 1 %. */
    p = (struct pulse_signal){ 0.1e-3, 50e-6, 1e-6, 0.3, -0.5e6, 2e6 / 50e-6, NULL, 0 };
    pri[0] = 2e-3;
    n = make_train(x, PCOUNT, PFS, &p, pri, 1, 20.0);
    printf(" LFM, 2 MHz in 50 us, PRI 2 ms, SNR 20 dB (%d pulses)\n", n);
    t = now_s();
    CHECK("analyse", m2sdr_pulse_analyse(x, PCOUNT, PFS, NULL, res) == 0);
    printf("  time %.2f ms\n", 1e3 * (now_s() - t));
    CHECK_NEAR("pulses", res->npulses, n, 0);
    CHECK("modulation LFM", res->modulation == M2SDR_PULSE_LFM);
    CHECK_NEAR("chirp (MHz/ms)", res->chirp_hz_per_s * 1e-9, 40.0, 0.4);
    CHECK_NEAR("frequency at middle (kHz)", res->freq_hz * 1e-3, -500.0, 10.0);
    CHECK_NEAR("width (us)", res->width_s * 1e6, 50.0, 0.1);
    CHECK_NEAR("PRI (us)", res->pri_mean_s * 1e6, 2000.0, 0.05);

    /* Staggered PRI 800 / 1000 / 1300 us, 1 us pulses, SNR 25 dB. */
    p = (struct pulse_signal){ 0.1e-3, 1e-6, 0.2e-6, 0.2, 2e6, 0, NULL, 0 };
    pri[0] = 800e-6;
    pri[1] = 1000e-6;
    pri[2] = 1300e-6;
    n = make_train(x, PCOUNT, PFS, &p, pri, 3, 25.0);
    printf(" stagger 800/1000/1300 us, width 1 us, SNR 25 dB (%d pulses)\n", n);
    CHECK("analyse", m2sdr_pulse_analyse(x, PCOUNT, PFS, NULL, res) == 0);
    CHECK_NEAR("pulses", res->npulses, n, 0);
    CHECK("PRI staggered, 3", res->pri_kind == M2SDR_PRI_STAGGER && res->stagger == 3);
    CHECK_NEAR("PRI 1 (us)", res->pri_s[0] * 1e6, 800.0, 0.05);
    CHECK_NEAR("PRI 2 (us)", res->pri_s[1] * 1e6, 1000.0, 0.05);
    CHECK_NEAR("PRI 3 (us)", res->pri_s[2] * 1e6, 1300.0, 0.05);
    CHECK_NEAR("PRF (Hz)", res->prf_hz, 3.0 / 3100e-6, 0.1);
    CHECK_NEAR("width (us)", res->width_s * 1e6, 1.0, 0.05);

    /* A missed pulse breaks the pattern: jitter, with the mean of the intervals. */
    {
        struct pulse_signal q = p;

        memset(x, 0, PCOUNT * sizeof(*x));
        for (i = 0; i < 20; i++, q.t0 += (i % 2 ? 1.0e-3 : 1.1e-3))
            if (i != 7)
                add_pulse(x, PCOUNT, PFS, &q);
        add_noise(x, PCOUNT, p.amp * p.amp * 1e-3);
        CHECK("analyse, missed pulse", m2sdr_pulse_analyse(x, PCOUNT, PFS, NULL, res) == 0);
        CHECK_NEAR("pulses", res->npulses, 19, 0);
        CHECK("PRI jitter", res->pri_kind == M2SDR_PRI_JITTER);
    }

    /* Barker 13, 1 us chips: neither CW nor LFM; the best tone holds 1/13 of it. */
    p = (struct pulse_signal){ 0.2e-3, 13e-6, 0.1e-6, 0.4, 0.3e6, 0, barker13, 13 };
    pri[0] = 3e-3;
    n = make_train(x, PCOUNT, PFS, &p, pri, 1, 25.0);
    printf(" Barker 13, PRI 3 ms, SNR 25 dB (%d pulses)\n", n);
    CHECK("analyse", m2sdr_pulse_analyse(x, PCOUNT, PFS, NULL, res) == 0);
    CHECK_NEAR("pulses", res->npulses, n, 0);
    CHECK("modulation other", res->modulation == M2SDR_PULSE_OTHER);
    printf("  coherence %.3f, modulation %s\n", res->pulses[0].coherence, mod_name(res->modulation));

    /* Weak pulses, 15 dB SNR (5 dB over the threshold): all found, none extra. */
    p = (struct pulse_signal){ 0.05e-3, 20e-6, 1e-6, 0.05, 0.0, 0, NULL, 0 };
    pri[0] = 0.5e-3;
    n = make_train(x, PCOUNT, PFS, &p, pri, 1, 15.0);
    printf(" weak: SNR 15 dB (%d pulses)\n", n);
    CHECK("analyse", m2sdr_pulse_analyse(x, PCOUNT, PFS, NULL, res) == 0);
    CHECK_NEAR("pulses", res->npulses, n, 0);
    CHECK("PRI constant", res->pri_kind == M2SDR_PRI_CONSTANT);
    CHECK_NEAR("width (us)", res->width_s * 1e6, 20.0, 0.3);

    /* A pulse cut by the start of the capture: listed, truncated, out of the train. */
    p = (struct pulse_signal){ -5e-6, 20e-6, 1e-6, 0.5, 0.0, 0, NULL, 0 };
    pri[0] = 1e-3;
    n = make_train(x, PCOUNT, PFS, &p, pri, 1, 30.0);
    CHECK("analyse, cut pulse", m2sdr_pulse_analyse(x, PCOUNT, PFS, NULL, res) == 0);
    CHECK("first truncated", res->nlisted > 1 && res->pulses[0].truncated && !res->pulses[1].truncated);
    CHECK_NEAR("PRI (us)", res->pri_mean_s * 1e6, 1000.0, 0.05);
    CHECK_NEAR("width (us)", res->width_s * 1e6, 20.0, 0.05);

    /* Threshold from the config: pulses 8 dB over the noise found at 5 dB only. */
    p = (struct pulse_signal){ 0.05e-3, 50e-6, 1e-6, 0.05, 0.0, 0, NULL, 0 };
    n = make_train(x, PCOUNT, PFS, &p, pri, 1, 8.0);
    memset(&cfg, 0, sizeof(cfg));
    cfg.threshold_db = 4.0;
    cfg.smooth_s     = 2e-6;
    CHECK("analyse, threshold 4 dB", m2sdr_pulse_analyse(x, PCOUNT, PFS, &cfg, res) == 0);
    CHECK_NEAR("pulses", res->npulses, n, 0);

    /* No pulses: noise, and a CW (its floor is its level). */
    memset(x, 0, PCOUNT * sizeof(*x));
    add_noise(x, PCOUNT, 1e-4);
    ok = m2sdr_pulse_analyse(x, PCOUNT, PFS, NULL, res) == 0 && !res->found;
    CHECK("noise: nothing", ok);
    CHECK_NEAR("noise floor (dBFS)", res->noise_dbfs, -40.0, 0.2);
    for (i = 0; i < (int)PCOUNT; i++)
        x[i] += 0.3f * (float complex)cexp(I * 2.0 * M_PI * 1e5 * i / PFS);
    CHECK("CW: nothing", m2sdr_pulse_analyse(x, PCOUNT, PFS, NULL, res) == 0 && !res->found);

    CHECK("bad sample rate", m2sdr_pulse_analyse(x, PCOUNT, 0.0, NULL, res) != 0);
    CHECK("too short", m2sdr_pulse_analyse(x, 1000, PFS, NULL, res) != 0);
    CHECK("no samples", m2sdr_pulse_analyse(NULL, PCOUNT, PFS, NULL, res) != 0);
    free(x);
    free(res);
}

int main(void)
{
    test_pulse();
    if (failures) {
        printf("test_m2sdr_analysis_monitoring: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis_monitoring: all tests passed\n");
    return 0;
}
