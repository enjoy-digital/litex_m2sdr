/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the libm2sdr monitoring analysis (pulses, EMC) on synthetic signals of known
 * parameters.
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
#include "../libm2sdr/m2sdr_fft.h"

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

/* EMC */
/*-----*/

/* Impulses of amplitude a (one sample each, band limited at fractional times) at `rate` per
 * second over a capture of exactly one second: their spectrum is a * rate on the bins that are
 * multiples of rate, 0 elsewhere. single: one impulse at t1 instead. */
static int make_impulses(float complex *x, size_t n, double a, int rate, bool single, double t1)
{
    struct m2sdr_fft *ifft = m2sdr_fft_alloc(n, true);
    float complex *spec = malloc(n * sizeof(*spec));
    size_t k;

    if (!ifft || !spec) {
        m2sdr_fft_free(ifft);
        free(spec);
        return -1;
    }
    for (k = 0; k < n; k++) {
        const long kk = k < n / 2 ? (long)k : (long)k - (long)n;

        if (single)
            spec[k] = (float complex)(a / (double)n * cexp(-I * 2.0 * M_PI * (double)kk * t1));
        else
            spec[k] = (kk % rate == 0) ? (float complex)(a * rate / (double)n) : 0.0f;
    }
    m2sdr_fft_run(ifft, spec, x);
    m2sdr_fft_free(ifft);
    free(spec);
    return 0;
}

/* Pulse response of the quasi-peak detector (CISPR 16-1-1, the table of the pulse response
 * by band): the reference impulse area, at the reference repetition frequency, reads as a sine
 * of 66 dBuV e.m.f. (2 mV rms, an envelope of 2.83 mV), and the curve relative to it with its
 * tolerance. Values from the standard as remembered: check against the text before relying on
 * them (see the report). */
struct qp_point {
    int    rate;               /* Hz; 0: isolated impulse. */
    double rel_db, tol_db;
};

struct qp_band {
    enum m2sdr_emc_band band;
    const char *name;
    double fs;
    double area_vs;            /* Reference impulse area, V s. */
    int    ref_rate;
    double ref_tol_db;
    struct qp_point points[8];
    int    npoints;
};

static void test_emc_pulse_response(const struct qp_band *bd, float complex *x)
{
    const size_t n = (size_t)bd->fs;   /* One second. */
    struct m2sdr_emc_result *res = malloc(sizeof(*res));
    struct m2sdr_emc_config cfg;
    double ref_ratio, ratio_ref_meas = 0;
    int p;

    if (!res) {
        CHECK("allocation", 0);
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.band     = bd->band;
    cfg.start_hz = 0.0;
    cfg.stop_hz  = 0.0;
    cfg.step_hz  = 1.0;   /* One channel at the centre. */
    printf(" %s: quasi-peak pulse response\n", bd->name);
    /* QP over peak at the reference rate: the peak of the envelope of an impulse of area A is
     * 2 A Bimp (the positive frequencies of the e.m.f.), the QP that of the sine: 2 sqrt(2) mV. */
    m2sdr_emc_analyse(x, 4096, bd->fs, &cfg, res);   /* For impulse_bw_hz. */
    ref_ratio = 20.0 * log10(2.0 * sqrt(2.0) * 1e-3 / (2.0 * bd->area_vs * res->impulse_bw_hz));
    for (p = -1; p < bd->npoints; p++) {
        const int rate = p < 0 ? bd->ref_rate : bd->points[p].rate;
        char name[64];
        double ratio;

        cfg.single = rate == 0;
        if (make_impulses(x, n, 0.1, rate ? rate : 1, rate == 0, 0.1) != 0 ||
            m2sdr_emc_analyse(x, n, bd->fs, &cfg, res) != 0 || res->nchannels != 1) {
            CHECK("analyse", 0);
            continue;
        }
        ratio = res->qp_dbfs[0] - res->peak_dbfs[0];
        if (p < 0) {
            /* Peak: 0.1 Bimp / fs. */
            CHECK_NEAR("peak (dBFS)", res->peak_dbfs[0], 20.0 * log10(0.1 * res->impulse_bw_hz / bd->fs), 0.05);
            snprintf(name, sizeof(name), "QP/peak at %d Hz (dB)", rate);
            CHECK_NEAR(name, ratio, ref_ratio, bd->ref_tol_db);
            ratio_ref_meas = ratio;
            continue;
        }
        if (rate)
            snprintf(name, sizeof(name), "QP at %d Hz, rel. (dB)", rate);
        else
            snprintf(name, sizeof(name), "QP isolated, rel. (dB)");
        CHECK_NEAR(name, ratio - ratio_ref_meas, bd->points[p].rel_db, bd->points[p].tol_db);
        CHECK("peak >= QP >= AV", res->peak_dbfs[0] >= res->qp_dbfs[0] && res->qp_dbfs[0] >= res->av_dbfs[0]);
    }
    free(res);
}

static void test_emc(void)
{
    static const struct qp_band bands[] = {
        { M2SDR_EMC_BAND_A, "band A", 16384.0, 13.5e-6, 25, 1.5,
          { { 100, 4.0, 1.0 }, { 60, 3.0, 1.0 }, { 10, -4.0, 1.0 }, { 5, -7.5, 1.5 },
            { 2, -13.0, 2.0 }, { 1, -17.0, 2.0 }, { 0, -19.0, 2.0 } }, 7 },
        { M2SDR_EMC_BAND_B, "band B", 1048576.0, 0.316e-6, 100, 1.5,
          { { 1000, 4.5, 1.0 }, { 20, -6.5, 1.0 }, { 10, -10.0, 1.5 }, { 2, -20.5, 2.0 },
            { 1, -22.5, 2.0 }, { 0, -23.5, 2.0 } }, 6 },
        { M2SDR_EMC_BAND_CD, "band C/D", 1048576.0, 0.044e-6, 100, 1.5,
          { { 1000, 8.0, 1.0 }, { 20, -9.0, 1.0 }, { 10, -14.0, 1.5 }, { 2, -26.0, 2.0 },
            { 1, -28.5, 2.0 }, { 0, -31.5, 2.0 } }, 6 },
    };
    const double fs = 1048576.0;
    const size_t count = 1u << 17;    /* 125 ms. */
    float complex *x = malloc((1u << 20) * sizeof(*x));
    struct m2sdr_emc_result *res = malloc(sizeof(*res));
    struct m2sdr_emc_config cfg;
    double t, df, pch;
    size_t i;
    unsigned b;
    int c;

    printf("EMC:\n");
    if (!x || !res) {
        CHECK("allocation", 0);
        free(x);
        free(res);
        return;
    }

    /* Lines: linear in log frequency (CISPR 32 class B conducted, QP: 66 to 56 dBuV from 0.15 to
     * 0.5 MHz), and a step (CISPR 32 class B radiated at 10 m: 30 then 37 dBuV/m at 230 MHz). */
    {
        static const struct m2sdr_emc_point slope[] = { { 150e3, 66.0 }, { 500e3, 56.0 }, { 5e6, 56.0 }, { 5e6, 60.0 },
                                                        { 30e6, 60.0 } };
        static const struct m2sdr_emc_point step[] = { { 30e6, 30.0 }, { 230e6, 30.0 }, { 230e6, 37.0 }, { 1e9, 37.0 } };

        printf(" limit lines\n");
        CHECK_NEAR("150 kHz", m2sdr_emc_line_db(slope, 5, 150e3), 66.0, 1e-9);
        CHECK_NEAR("300 kHz", m2sdr_emc_line_db(slope, 5, 300e3), 66.0 - 10.0 * log10(2.0) / log10(500.0 / 150.0), 1e-9);
        CHECK_NEAR("below the line", m2sdr_emc_line_db(slope, 5, 10e3), 66.0, 1e-9);
        CHECK_NEAR("at a step: the lower", m2sdr_emc_line_db(step, 4, 230e6), 30.0, 1e-9);
        CHECK_NEAR("over a step", m2sdr_emc_line_db(step, 4, 230.001e6), 37.0, 1e-9);
        CHECK_NEAR("above the line", m2sdr_emc_line_db(step, 4, 2e9), 37.0, 1e-9);
        CHECK("no points", isnan(m2sdr_emc_line_db(step, 0, 1e6)));
    }

    /* CW on a bin, band B: the three detectors read its level; one RBW away, the Gaussian is
     * 2^-4: -24.08 dB. Noise at -80 dBFS per sample, 9.9 kHz in the channel: -100 dBFS. */
    df = fs / (double)count;
    memset(x, 0, count * sizeof(*x));
    for (i = 0; i < count; i++)
        x[i] = 0.1f * (float complex)cexp(I * 2.0 * M_PI * 12000.0 * df * (double)i / fs);
    add_noise(x, count, 1e-8);
    memset(&cfg, 0, sizeof(cfg));
    cfg.band      = M2SDR_EMC_BAND_AUTO;
    cfg.centre_hz = 1e6;
    cfg.start_hz  = 12000.0 * df - 9000.0;
    cfg.stop_hz   = 12000.0 * df + 9000.0;
    cfg.step_hz   = 4500.0;
    cfg.offset_db = 100.0;    /* -20 dBFS reads 80 dBuV. */
    cfg.nqp_limit = 1;
    cfg.qp_limit[0] = (struct m2sdr_emc_point){ 1e6, 56.0 };
    cfg.nav_limit = 1;
    cfg.av_limit[0] = (struct m2sdr_emc_point){ 1e6, 46.0 };
    printf(" CW of -20 dBFS, band B\n");
    CHECK("analyse", m2sdr_emc_analyse(x, count, fs, &cfg, res) == 0);
    CHECK("band B from the centre", res->band == M2SDR_EMC_BAND_B && res->rbw_hz == 9e3);
    CHECK_NEAR("impulse bandwidth (Hz)", res->impulse_bw_hz, 9e3 * sqrt(M_PI / M_LN2) / 2.0, 1.0);
    CHECK_NEAR("channels", res->nchannels, 5, 0);
    CHECK_NEAR("channel frequency (Hz)", res->freq_hz[2], 1e6 + 12000.0 * df, 0.5);
    CHECK_NEAR("peak (dBFS)", res->peak_dbfs[2], -20.0, 0.02);
    CHECK_NEAR("quasi-peak (dBFS)", res->qp_dbfs[2], -20.0, 0.02);
    CHECK_NEAR("average (dBFS)", res->av_dbfs[2], -20.0, 0.02);
    CHECK_NEAR("QP, 9 kHz off (dBFS)", res->qp_dbfs[0], -20.0 - 24.08, 0.05);
    CHECK_NEAR("AV, 9 kHz off (dBFS)", res->av_dbfs[4], -20.0 - 24.08, 0.05);
    CHECK_NEAR("QP margin (dB)", res->margin_qp_db[2], 56.0 - 80.0, 0.02);
    CHECK("fails", res->limits && !res->pass && res->worst_detector == 2);
    CHECK_NEAR("worst margin (dB)", res->worst_margin_db, 46.0 - 80.0, 0.02);
    CHECK_NEAR("worst frequency (Hz)", res->worst_freq_hz, res->freq_hz[2], 0.5);
    cfg.offset_db = 50.0;
    CHECK("analyse", m2sdr_emc_analyse(x, count, fs, &cfg, res) == 0);
    CHECK("passes 50 dB lower", res->pass);
    CHECK_NEAR("worst margin (dB)", res->worst_margin_db, 46.0 - 30.0, 0.02);
    /* An antenna factor line on top of the offset. */
    cfg.nfactor   = 2;
    cfg.factor[0] = (struct m2sdr_emc_point){ 0.5e6, 10.0 };
    cfg.factor[1] = (struct m2sdr_emc_point){ 2e6, 30.0 };
    CHECK("analyse", m2sdr_emc_analyse(x, count, fs, &cfg, res) == 0);
    CHECK_NEAR("margin with factor (dB)", res->margin_qp_db[2],
               56.0 - (30.0 + m2sdr_emc_line_db(cfg.factor, 2, res->freq_hz[2])), 0.02);

    /* Noise: the envelope is Rayleigh. In the channel, the power of the noise times the noise
     * bandwidth of the Gaussian, rbw sqrt(pi / (2 ln 2)) / 2 = 0.7527 rbw; its mean envelope
     * sqrt(pi / 4) of its rms: -1.05 dB. The peak and the QP are over it. */
    memset(x, 0, count * sizeof(*x));
    add_noise(x, count, 1e-4);
    memset(&cfg, 0, sizeof(cfg));
    cfg.band = M2SDR_EMC_BAND_B;
    pch = 1e-4 * 9e3 * sqrt(M_PI / (2.0 * M_LN2)) / 2.0 / fs;
    t = now_s();
    CHECK("analyse, noise", m2sdr_emc_analyse(x, count, fs, &cfg, res) == 0);
    printf("  time %.1f ms, %d channels, 125 ms at 1.05 MSPS, band B\n", 1e3 * (now_s() - t), res->nchannels);
    {
        double av = 0, qp = 0, pk = 0;

        for (c = 0; c < res->nchannels; c++) {
            av += res->av_dbfs[c];
            qp += res->qp_dbfs[c];
            pk += res->peak_dbfs[c];
        }
        av /= res->nchannels;
        qp /= res->nchannels;
        pk /= res->nchannels;
        CHECK_NEAR("average (dBFS)", av, 10.0 * log10(pch) + 20.0 * log10(sqrt(M_PI / 4.0)), 0.1);
        CHECK("QP over AV, peak over QP", qp > av + 1.0 && pk > qp + 3.0);
        printf("  peak %.2f, QP %.2f, AV %.2f dBFS, rms %.2f dBFS\n", pk, qp, av, 10.0 * log10(pch));
        CHECK("no limits", !res->limits && res->pass);
    }

    /* Pulse responses. */
    for (b = 0; b < sizeof(bands) / sizeof(bands[0]); b++)
        test_emc_pulse_response(&bands[b], x);

    /* Speed: band C/D over 20 MSPS, 52 ms, channels every 60 kHz. */
    {
        const double fs20 = 20e6;
        const size_t n20 = 1u << 20;

        memset(x, 0, n20 * sizeof(*x));
        add_noise(x, n20, 1e-4);
        memset(&cfg, 0, sizeof(cfg));
        cfg.band = M2SDR_EMC_BAND_CD;
        t = now_s();
        CHECK("analyse, 20 MSPS", m2sdr_emc_analyse(x, n20, fs20, &cfg, res) == 0);
        printf("  time %.1f ms, %d channels, 52 ms at 20 MSPS, band C/D\n", 1e3 * (now_s() - t), res->nchannels);
    }

    /* Bad arguments. */
    memset(&cfg, 0, sizeof(cfg));
    CHECK("bad sample rate", m2sdr_emc_analyse(x, count, 0.0, NULL, res) != 0);
    CHECK("too short", m2sdr_emc_analyse(x, 1000, fs, NULL, res) != 0);
    cfg.band = M2SDR_EMC_BAND_A;
    CHECK("band A, capture too short", m2sdr_emc_analyse(x, 8192, fs, &cfg, res) != 0);
    cfg.band = M2SDR_EMC_BAND_CD;
    CHECK("band C/D, rate too low", m2sdr_emc_analyse(x, count, 300e3, &cfg, res) != 0);
    cfg.band     = M2SDR_EMC_BAND_B;
    cfg.start_hz = -0.5 * fs;
    cfg.stop_hz  = 0.0;
    CHECK("channel at the edge", m2sdr_emc_analyse(x, count, fs, &cfg, res) != 0);
    cfg.start_hz = 0.0;
    cfg.step_hz  = 1.0;
    cfg.stop_hz  = 1e5;
    CHECK("too many channels", m2sdr_emc_analyse(x, count, fs, &cfg, res) != 0);
    cfg.stop_hz   = 0.0;
    cfg.nqp_limit = 2;
    cfg.qp_limit[0] = (struct m2sdr_emc_point){ 2e6, 50.0 };
    cfg.qp_limit[1] = (struct m2sdr_emc_point){ 1e6, 50.0 };
    CHECK("limit not ascending", m2sdr_emc_analyse(x, count, fs, &cfg, res) != 0);
    cfg.band = (enum m2sdr_emc_band)7;
    cfg.nqp_limit = 0;
    CHECK("bad band", m2sdr_emc_analyse(x, count, fs, &cfg, res) != 0);
    free(x);
    free(res);
}

/* Captures for m2sdr_analyzer */
/*------------------------------*/

/* SC16, one channel, 12 bits: what make test-analyzer gives m2sdr_analyzer --file. */
static int write_sc16(const char *path, const float complex *x, size_t n)
{
    FILE *f = fopen(path, "wb");
    size_t i;

    if (!f)
        return 1;
    for (i = 0; i < n; i++) {
        const short v[2] = { (short)lrintf(crealf(x[i]) * 2047.0f), (short)lrintf(cimagf(x[i]) * 2047.0f) };

        if (fwrite(v, sizeof(v), 1, f) != 1) {
            fclose(f);
            return 1;
        }
    }
    return fclose(f) == 0 ? 0 : 1;
}

static char *read_text(const char *path)
{
    static char text[1 << 20];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        printf("test_m2sdr_analysis_monitoring: cannot open %s\n", path);
        return NULL;
    }
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return text;
}

/* Number after "name": in the section "section" of a JSON report. */
static double json_number(const char *json, const char *section, const char *name)
{
    char key[64];
    const char *p;

    snprintf(key, sizeof(key), "\"%s\"", section);
    p = strstr(json, key);
    if (!p)
        return NAN;
    snprintf(key, sizeof(key), "\"%s\":", name);
    p = strstr(p, key);
    return p ? strtod(p + strlen(key), NULL) : NAN;
}

static int report_done(const char *what)
{
    if (failures) {
        printf("test_m2sdr_analysis_monitoring: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis_monitoring: %s report checked\n", what);
    return 0;
}

/* Pulses: LFM pulses of 20 us sweeping 1 MHz, 1 MHz over the centre, staggered PRI
 * 800/1000/1300 us, 30 dB of SNR, 60 ms at 10 MSPS (the analyzer takes 50). */
static int write_pulse(const char *path)
{
    const size_t n = (size_t)(0.06 * PFS);
    float complex *x = malloc(n * sizeof(*x));
    const struct pulse_signal p = { 0.1e-3, 20e-6, 1e-6, 0.5, 1e6, 1e6 / 20e-6, NULL, 0 };
    const double pri[3] = { 800e-6, 1000e-6, 1300e-6 };
    int rc;

    if (!x)
        return 1;
    make_train(x, n, PFS, &p, pri, 3, 30.0);
    rc = write_sc16(path, x, n);
    free(x);
    return rc;
}

static int check_pulse_report(const char *path)
{
    const char *json = read_text(path), *p;
    double pri[3] = { NAN, NAN, NAN };
    int i;

    if (!json)
        return 1;
    printf("Pulse report of m2sdr_analyzer:\n");
    CHECK("found", strstr(json, "\"found\": true") != NULL);
    CHECK("stagger, LFM", strstr(json, "\"pri\": \"stagger\"") && strstr(json, "\"modulation\": \"lfm\""));
    CHECK_NEAR("stagger", json_number(json, "pulse", "stagger"), 3, 0);
    p = strstr(json, "\"pri_us\": [");
    for (i = 0; p && i < 3; i++) {
        char *end;

        p = strchr(p, i ? ',' : '[');
        if (!p)
            break;
        pri[i] = strtod(p + 1, &end);
        p = end;
    }
    CHECK_NEAR("PRI 1 (us)", pri[0], 800.0, 0.05);
    CHECK_NEAR("PRI 2 (us)", pri[1], 1000.0, 0.05);
    CHECK_NEAR("PRI 3 (us)", pri[2], 1300.0, 0.05);
    CHECK_NEAR("width (us)", json_number(json, "pulse", "width_us"), 20.0, 0.05);
    /* The 12-bit samples add nothing to the noise of 30 dB under 0.5. */
    CHECK_NEAR("offset (kHz)", json_number(json, "pulse", "offset_hz") / 1e3, 1000.0, 10.0);
    CHECK_NEAR("chirp (MHz/ms)", json_number(json, "pulse", "chirp_hz_per_s") / 1e9, 50.0, 0.5);
    CHECK_NEAR("peak (dBFS)", json_number(json, "pulse", "peak_db"), 20.0 * log10(0.5) + 0.5, 0.5);
    return report_done("pulse");
}

/* EMC: band C/D at 1.048576 MSPS, 2^18 samples (250 ms): a CW of -20 dBFS at +195.712 kHz (on
 * a channel of the default span, and on a bin) and impulses of 0.05 at 100 Hz, 25 in the
 * capture: a whole number for the steady state. Limit line: -40 flat; factors: 5 dB flat. */
#define EMC_FS     1048576.0
#define EMC_N      (1u << 18)
#define EMC_CW_HZ  195712.0

static int write_emc(const char *path, const char *limit, const char *factors)
{
    float complex *x = malloc(EMC_N * sizeof(*x));
    FILE *f;
    size_t i;
    int rc;

    if (!x || make_impulses(x, EMC_N, 0.05, 25, false, 0.0) != 0)
        return 1;
    for (i = 0; i < EMC_N; i++)
        x[i] += 0.1f * (float complex)cexp(I * 2.0 * M_PI * EMC_CW_HZ * (double)i / EMC_FS);
    rc = write_sc16(path, x, EMC_N);
    free(x);
    f = fopen(limit, "w");
    if (!f)
        return 1;
    fprintf(f, "# QP limit, dBuV/m\n30e6 -40\n1e9 -40\n");
    fclose(f);
    f = fopen(factors, "w");
    if (!f)
        return 1;
    fprintf(f, "# antenna factor and cable\n30e6 5.0 # flat\n1e9 5.0\n");
    return fclose(f) == 0 ? rc : 1;
}

static int check_emc_report(const char *path)
{
    const char *json = read_text(path);
    const double bimp = 120e3 * sqrt(M_PI / M_LN2) / 2.0;
    double peak0, qp0;

    if (!json)
        return 1;
    printf("EMC report of m2sdr_analyzer:\n");
    CHECK("band C/D, dBuV/m", strstr(json, "\"band\": \"cd\"") && strstr(json, "\"unit\": \"dBuV/m\""));
    /* Default span: 100 RBW clamped to the capture, every 60 kHz: 12 channels. */
    CHECK_NEAR("channels", json_number(json, "emc", "nchannels"), 12, 0);
    CHECK_NEAR("strongest (Hz)", json_number(json, "emc", "max_peak_hz"), 100e6 + EMC_CW_HZ, 2.0);
    /* The CW and the peak of an impulse in its channel, in phase at some point: 0.1 + 0.05 Bimp / fs. */
    CHECK_NEAR("its peak (dBuV/m)", json_number(json, "emc", "max_peak_db"), 20.0 * log10(0.1 + 0.05 * bimp / EMC_FS) + 5.0,
               0.1);
    CHECK("fails", strstr(json, "\"pass\": false") != NULL);
    /* QP of the CW, -20 + 5, against -40: the impulses add little to it. */
    CHECK_NEAR("worst margin (dB)", json_number(json, "emc", "worst_margin_db"), -40.0 - (-20.0 + 5.0), 0.2);
    CHECK_NEAR("at (Hz)", json_number(json, "emc", "worst_freq_hz"), 100e6 + EMC_CW_HZ, 2.0);
    /* First channel, impulses only: peak 0.05 Bimp / fs, QP/peak at 100 Hz as in test_emc (CISPR
     * reference: -12.0 dB +-1.5). */
    peak0 = json_number(json, "channels", "peak_db");
    qp0   = json_number(json, "channels", "qp_db");
    CHECK_NEAR("impulses: peak (dBuV/m)", peak0, 20.0 * log10(0.05 * bimp / EMC_FS) + 5.0, 0.2);
    CHECK_NEAR("impulses: QP/peak (dB)", qp0 - peak0, -11.99, 1.5);
    return report_done("EMC");
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--write-pulse") == 0)
        return write_pulse(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check-pulse") == 0)
        return check_pulse_report(argv[2]);
    if (argc == 5 && strcmp(argv[1], "--write-emc") == 0)
        return write_emc(argv[2], argv[3], argv[4]);
    if (argc == 3 && strcmp(argv[1], "--check-emc") == 0)
        return check_emc_report(argv[2]);
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--write-pulse F | --check-pulse F | --write-emc F LIMIT FACTORS | --check-emc F]\n", argv[0]);
        return 1;
    }
    test_pulse();
    test_emc();
    if (failures) {
        printf("test_m2sdr_analysis_monitoring: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis_monitoring: all tests passed\n");
    return 0;
}
