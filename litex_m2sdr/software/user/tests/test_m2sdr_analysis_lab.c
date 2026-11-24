/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the lab analyses of libm2sdr (sweep, compression, IMD, AM/AM) on synthetic devices
 * of known response: a one-pole filter, Rapp and Saleh amplifiers, a polynomial non-linearity.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#define TEST_NAME "test_m2sdr_analysis_lab"
#include "test_analysis_helpers.h"

#include "../libm2sdr/m2sdr_analysis.h"
#include "../libm2sdr/m2sdr_analysis_lab.h"
#include "../libm2sdr/m2sdr_fft.h"

#define FS 1.0e6

/* Signals */
/*---------*/

static double db(double power)
{
    return 10.0 * log10(power);
}

/* x delayed by d samples (circular, d may be fractional): its transform turned. */
static void delay_periodic(float complex *x, size_t n, double d)
{
    struct m2sdr_fft *fwd = m2sdr_fft_alloc(n, false), *inv = m2sdr_fft_alloc(n, true);
    float complex *t = malloc(n * sizeof(*t));
    size_t i;

    m2sdr_fft_run(fwd, x, t);
    for (i = 0; i < n; i++) {
        const double k = i < n / 2 ? (double)i : (double)i - (double)n;
        t[i] *= (float complex)cexp(-I * 2.0 * M_PI * k * d / (double)n) / (float)n;
    }
    m2sdr_fft_run(inv, t, x);
    m2sdr_fft_free(fwd);
    m2sdr_fft_free(inv);
    free(t);
}

/* Stimuli */
/*---------*/

static void test_stimuli(void)
{
    const size_t n = 16384;
    float complex *x = malloc(4 * n * sizeof(*x));
    double f, f1, f2, papr, p = 0;
    size_t i;

    printf("Stimuli:\n");
    CHECK("tone", m2sdr_lab_tone(x, 1000, FS, 123456.0, -6.0, &f) == 0);
    /* 1000 samples at 1 MSPS: whole cycles every 1 kHz. */
    CHECK_NEAR("tone frequency, rounded (Hz)", f, 123000.0, 1e-6);
    CHECK_NEAR("tone level (dBFS)", db(crealf(x[17] * conjf(x[17]))), -6.0, 1e-4);
    /* Looped: the step from the last sample to the first is one sample of the tone. */
    CHECK_NEAR("tone loops (rad)", carg(x[0] * conjf(x[999])), 2.0 * M_PI * f / FS, 1e-5);
    CHECK("two tones", m2sdr_lab_two_tone(x, n, FS, 100e3, 10e3, -20.0, &f1, &f2) == 0);
    CHECK_NEAR("two tones, spacing (Hz)", f2 - f1, 10e3, FS / n);
    for (i = 0; i < n; i++)
        p += crealf(x[i] * conjf(x[i]));
    CHECK_NEAR("two tones, power (dBFS)", db(p / n), -20.0 + db(2.0), 1e-3);

    CHECK("noise", m2sdr_lab_noise(x, n, FS, 200e3, -15.0, 1, &papr) == 0);
    p = 0;
    for (i = 0; i < n; i++)
        p += crealf(x[i] * conjf(x[i]));
    CHECK_NEAR("noise power (dBFS)", db(p / n), -15.0, 1e-3);
    /* Near-Gaussian: the peak of 16384 Rayleigh samples is about ln(16384) = 9.7 above the mean. */
    CHECK("noise PAPR 8 to 13 dB", papr > 8.0 && papr < 13.0);
    {
        struct m2sdr_psd psd;
        double lo, hi;
        memset(&psd, 0, sizeof(psd));
        m2sdr_psd_compute(x, n, FS, 1024, M2SDR_WINDOW_BLACKMAN_HARRIS, &psd);
        m2sdr_psd_obw(&psd, -FS / 2, FS / 2, 0.99, &lo, &hi);
        CHECK_NEAR("noise OBW 99 % (kHz)", (hi - lo) / 1e3, 198.0, 2.0);
        m2sdr_psd_free(&psd);
    }

    /* QAM: the demodulator of libm2sdr reads it back without error. */
    {
        struct m2sdr_demod_config dc = { M2SDR_MOD_16QAM, M2SDR_PULSE_RRC, FS / 8.0, 0.35, 0 };
        struct m2sdr_demod_result *dr = malloc(sizeof(*dr));
        CHECK("qam", m2sdr_lab_qam(x, n, M2SDR_MOD_16QAM, 8, 0.35, -15.0, 7, &papr) == 0);
        p = 0;
        for (i = 0; i < n; i++)
            p += crealf(x[i] * conjf(x[i]));
        CHECK_NEAR("qam power (dBFS)", db(p / n), -15.0, 1e-3);
        for (i = n; i < 4 * n; i++)
            x[i] = x[i % n];
        CHECK("qam demod", m2sdr_demod(x, 4 * n, FS, &dc, dr) == 0 && dr->locked);
        /* The pulse is exact (in frequency): what remains is the float arithmetic and the
         * timing of the demodulator. */
        CHECK("qam EVM below 1 %", dr->evm_rms < 0.01);
        printf("  qam EVM %.3f %%, PAPR %.1f dB\n", 100.0 * dr->evm_rms, papr);
        free(dr);
    }

    CHECK("bad: noise period", m2sdr_lab_noise(x, 1000, FS, 200e3, -15.0, 1, NULL) == -1);
    CHECK("bad: qam sps", m2sdr_lab_qam(x, n, M2SDR_MOD_QPSK, 6, 0.35, -15.0, 1, NULL) == -1);
    CHECK("bad: tone frequency", m2sdr_lab_tone(x, n, FS, 0.6 * FS, -6.0, NULL) == -1);
    CHECK("bad: two tones spacing", m2sdr_lab_two_tone(x, n, FS, 0.0, 0.0, -6.0, NULL, NULL) == -1);
    free(x);
}

/* Sweep */
/*-------*/

/* Path between TX and RX: the gain and the latency of the radio, and a one-pole filter
 * v[n] = a v[n - 1] + (1 - a) x[n] for the device (a = 0: a thru). */
struct path {
    double gain_db, phase_deg;
    size_t latency;
    double pole;
};

/* Capture of `count` samples of the looped stimulus through the path, the first one received
 * while the stimulus sample `start` (from the start of TX) was sent. The filter runs from the
 * start of TX: settled by the time of the capture. */
static void run_path(const float complex *stim, size_t period, const struct path *p, size_t start,
                     float complex *out, size_t count, double noise)
{
    const float complex g = (float complex)(pow(10.0, p->gain_db / 20.0) * cexp(I * p->phase_deg * M_PI / 180.0));
    double complex v = 0;
    size_t n;

    for (n = 0; n < start + count - p->latency; n++) {
        v = p->pole * v + (1.0 - p->pole) * stim[n % period];
        if (n + p->latency >= start)
            out[n + p->latency - start] = g * (float complex)v;
    }
    add_noise(out, count, noise);
}

/* One-pole filter: H = (1 - a) / (1 - a e^-jw), group delay (a cos w - a^2) / (1 - 2 a cos w + a^2). */
static double complex pole_h(double a, double f)
{
    return (1.0 - a) / (1.0 - a * cexp(-I * 2.0 * M_PI * f / FS));
}

static double pole_gd(double a, double f)
{
    const double c = cos(2.0 * M_PI * f / FS);
    return (a * c - a * a) / (1.0 - 2.0 * a * c + a * a);
}

static void test_sweep_kind(enum m2sdr_sweep_stimulus kind)
{
    struct m2sdr_sweep_config cfg;
    struct m2sdr_sweep_result *res = malloc(sizeof(*res));
    struct m2sdr_sweep_cal *cal = malloc(sizeof(*cal));
    const struct path thru = { -3.0, 40.0, 23, 0.0 };
    const struct path dut  = { -3.0, 40.0, 23, 0.5 };
    const size_t period = 65536, count = 4 * period;
    float complex *stim = malloc(period * sizeof(*stim)), *x = malloc(count * sizeof(*x));
    double err_mag = 0, err_gd = 0, err_ph = 0, err_dev = 0, mean_gd = 0, t0, t1;
    int k, n;

    printf("Sweep, %s:\n", kind == M2SDR_SWEEP_STEPPED ? "stepped" : "chirp");
    memset(&cfg, 0, sizeof(cfg));
    cfg.stimulus   = kind;
    cfg.start_hz   = -0.4 * FS;
    cfg.stop_hz    = 0.4 * FS;
    cfg.points     = 101;
    cfg.period     = period;
    cfg.level_dbfs = -6.0;
    CHECK("stimulus", m2sdr_sweep_stimulus(&cfg, FS, stim) == 0);
    CHECK_NEAR("stimulus level (dBFS)", db(crealf(stim[1000] * conjf(stim[1000]))), -6.0, 1e-4);

    /* Thru: the radio alone. Noise of -70 dBFS against tones of -9 dBFS at the receiver: 61 dB a
     * sample, and more than 25 dB of correlation gain on each point, so the figures below are
     * set by the arithmetic, a few thousandths of a dB. */
    cfg.timing_known   = true;
    cfg.capture_offset = (4 * period + 1234) % period;
    run_path(stim, period, &thru, 4 * period + 1234, x, count, 1e-7);
    t0 = now_ms();
    CHECK("thru", m2sdr_sweep_analyse(x, count, FS, &cfg, NULL, res) == 0 && res->found);
    t1 = now_ms();
    printf("  one sweep, %zu samples: %.1f ms\n", count, t1 - t0);
    n = res->points;
    CHECK("points", n == 101);
    CHECK_NEAR("thru |S21| min (dB)", res->mag_min_db, -3.0, 0.02);
    CHECK_NEAR("thru |S21| max (dB)", res->mag_max_db, -3.0, 0.02);
    CHECK("thru delay valid", res->delay_valid);
    CHECK_NEAR("thru delay (samples)", res->delay_s * FS, 23.0, 0.01);
    CHECK_NEAR("thru group delay, centre (samples)", res->group_delay_s[n / 2] * FS, 23.0, 0.02);
    CHECK_NEAR("thru phase, centre (deg)", res->phase_deg[n / 2], 40.0, 0.2);
    CHECK("periods averaged", res->periods == 3);
    CHECK("snr of the points over 40 dB", res->snr_db[n / 2] > 40.0);
    CHECK("calibration", m2sdr_sweep_cal_store(res, cal) == 0);

    /* Device, captured at another offset: S21 over the thru is the filter alone. */
    cfg.capture_offset = (4 * period + 777) % period;
    run_path(stim, period, &dut, 4 * period + 777, x, count, 1e-7);
    CHECK("device", m2sdr_sweep_analyse(x, count, FS, &cfg, cal, res) == 0 && res->found && res->calibrated);
    for (k = 0; k < n; k++) {
        const double complex h = pole_h(0.5, res->freq_hz[k]);
        const double e = fabs(res->mag_db[k] - 20.0 * log10(cabs(h)));
        double ep = fabs(res->phase_deg[k] - carg(h) * 180.0 / M_PI);
        if (ep > 180.0)
            ep = 360.0 - ep;
        err_mag = fmax(err_mag, e);
        err_ph  = fmax(err_ph, ep);
        /* Centred differences inside the span; the ends are one-sided. */
        if (k > 0 && k < n - 1)
            err_gd = fmax(err_gd, fabs(res->group_delay_s[k] * FS - pole_gd(0.5, res->freq_hz[k])));
    }
    printf("  |S21| from %.2f to %.2f dB\n", res->mag_min_db, res->mag_max_db);
    /* The chirp takes the mean of the bins within half a step of a point: the curvature of the
     * response over that step (8 kHz) is below 0.01 dB here. The centred difference of the phase
     * over two steps errs by the third derivative of the phase: below 0.01 sample for this filter. */
    CHECK_NEAR("device |S21| error, max (dB)", err_mag, 0.0, 0.02);
    CHECK_NEAR("device phase error, max (deg)", err_ph, 0.0, 0.2);
    CHECK_NEAR("device group delay error (samples)", err_gd, 0.0, 0.02);
    CHECK_NEAR("device delay (samples)", res->delay_s * FS, 0.0, 0.01);

    /* Without the timing: the magnitude, and the group delay from its mean. */
    cfg.timing_known = false;
    cal->timing_known = false;
    CHECK("device, no timing", m2sdr_sweep_analyse(x, count, FS, &cfg, cal, res) == 0 && res->found && !res->delay_valid);
    for (k = 1; k < n - 1; k++)
        mean_gd += pole_gd(0.5, res->freq_hz[k]);
    mean_gd /= n - 2;
    err_mag = 0;
    {
        double mean_meas = 0;
        for (k = 1; k < n - 1; k++)
            mean_meas += res->group_delay_s[k] * FS;
        mean_meas /= n - 2;
        for (k = 1; k < n - 1; k++) {
            err_dev = fmax(err_dev, fabs((res->group_delay_s[k] * FS - mean_meas) - (pole_gd(0.5, res->freq_hz[k]) - mean_gd)));
            err_mag = fmax(err_mag, fabs(res->mag_db[k] - 20.0 * log10(cabs(pole_h(0.5, res->freq_hz[k])))));
        }
    }
    CHECK_NEAR("no timing |S21| error (dB)", err_mag, 0.0, 0.02);
    CHECK_NEAR("no timing group delay ripple err", err_dev, 0.0, 0.03);

    /* Raw S21 of the path: the gain of the radio times the filter. */
    CHECK("device, raw", m2sdr_sweep_analyse(x, count, FS, &cfg, NULL, res) == 0 && !res->calibrated);
    CHECK_NEAR("raw |S21| at centre (dB)", res->mag_db[n / 2], -3.0, 0.02);

    /* Noise only: nothing found. */
    memset(x, 0, count * sizeof(*x));
    add_noise(x, count, 1e-3);
    CHECK("noise only", m2sdr_sweep_analyse(x, count, FS, &cfg, NULL, res) == 0 && !res->found);
    printf("  noise only: sync %.1f dB\n", res->sync_quality);

    /* Bad arguments. */
    CHECK("bad: short capture", m2sdr_sweep_analyse(x, period, FS, &cfg, NULL, res) == -1);
    {
        struct m2sdr_sweep_config bad = cfg;
        bad.points = 80;
        CHECK("bad: calibration of other points", m2sdr_sweep_analyse(x, count, FS, &bad, cal, res) == -1);
        bad = cfg;
        bad.period = 50000;
        CHECK("bad: period", m2sdr_sweep_analyse(x, count, FS, &bad, NULL, res) == -1);
        bad = cfg;
        bad.stop_hz = 0.6 * FS;
        CHECK("bad: span", m2sdr_sweep_stimulus(&bad, FS, stim) == -1);
        bad = cfg;
        bad.points = 1;
        CHECK("bad: points", m2sdr_sweep_stimulus(&bad, FS, stim) == -1);
    }
    free(stim);
    free(x);
    free(res);
    free(cal);
}

static void test_sweep(void)
{
    test_sweep_kind(M2SDR_SWEEP_STEPPED);
    test_sweep_kind(M2SDR_SWEEP_CHIRP);
}

/* Compression */
/*-------------*/

/* Rapp amplifier: |out| = G |in| / (1 + (G |in| / Asat)^2p)^(1/2p), no AM/PM. */
#define RAPP_G    0.5
#define RAPP_SAT  0.3
#define RAPP_P    2.0

static float complex rapp(float complex x, double g, double sat, double p)
{
    const double r = cabsf(x);
    return (float complex)(x * (g / pow(1.0 + pow(g * r / sat, 2.0 * p), 1.0 / (2.0 * p))));
}

static void test_compression(void)
{
    enum { STEPS = 61, COUNT = 16384 };
    struct m2sdr_compression_step steps[STEPS];
    struct m2sdr_compression_result *res = malloc(sizeof(*res));
    float complex *buf = malloc((size_t)STEPS * COUNT * sizeof(*buf));
    const double g0 = 20.0 * log10(RAPP_G);
    /* Inputs where G |in| / Asat = (10^(c p / 10) - 1)^(1/2p) for a compression of c dB. */
    const double p1 = 20.0 * log10(RAPP_SAT / RAPP_G * pow(pow(10.0, RAPP_P / 10.0) - 1.0, 1.0 / (2.0 * RAPP_P)));
    const double p3 = 20.0 * log10(RAPP_SAT / RAPP_G * pow(pow(10.0, 3.0 * RAPP_P / 10.0) - 1.0, 1.0 / (2.0 * RAPP_P)));
    double f, t0, t1, level;
    int s;
    size_t i;

    printf("Compression, Rapp amplifier (G %.2f dB, Asat %.2f, p %.0f):\n", g0, RAPP_SAT, RAPP_P);
    /* Steps of 0.5 dB from -30 to 0 dBFS. A tone of constant envelope goes through a memoryless
     * amplifier as a tone: its level is the AM/AM curve. */
    for (s = 0; s < STEPS; s++) {
        float complex *x = buf + (size_t)s * COUNT;
        steps[s].input_dbfs = -30.0 + 0.5 * s;
        steps[s].x          = x;
        steps[s].count      = COUNT;
        m2sdr_lab_tone(x, COUNT, FS, 123e3, steps[s].input_dbfs, &f);
        for (i = 0; i < COUNT; i++)
            x[i] = rapp(x[i], RAPP_G, RAPP_SAT, RAPP_P);
        add_noise(x, COUNT, 1e-9);
    }
    t0 = now_ms();
    CHECK("tone level", m2sdr_compression_tone(steps[0].x, COUNT, FS, f, &level) == 0);
    t1 = now_ms();
    printf("  one step, %d samples: %.2f ms\n", COUNT, t1 - t0);
    /* Flat-top: 0.01 dB wherever the tone falls; the noise of its band is 50 dB below. */
    CHECK_NEAR("lowest step level (dBFS)", level, -30.0 + g0, 0.02);
    CHECK("analyse", m2sdr_compression_analyse(steps, STEPS, FS, f, res) == 0 && res->found);
    CHECK("steps", res->steps == STEPS);
    CHECK_NEAR("small-signal gain (dB)", res->small_signal_gain_db, g0, 0.02);
    /* The compression is smooth in dB of input: linear interpolation over 0.5 dB errs by its
     * curvature, h^2 / 8 c'' / c', about 0.01 dB of input here. */
    CHECK("P1dB found", res->p1db);
    CHECK_NEAR("P1dB in (dBFS)", res->p1db_in_dbfs, p1, 0.05);
    CHECK_NEAR("P1dB out (dBFS)", res->p1db_out_dbfs, p1 + g0 - 1.0, 0.05);
    CHECK("P3dB found", res->p3db);
    CHECK_NEAR("P3dB in (dBFS)", res->p3db_in_dbfs, p3, 0.05);
    /* At 0 dBFS in, G |in| / Asat = 1.67: the output is Asat / (1 + 1.67^-4)^(1/4) from saturation. */
    CHECK_NEAR("highest output (dBFS)", res->max_output_dbfs,
               20.0 * log10(RAPP_G / pow(1.0 + pow(RAPP_G / RAPP_SAT, 4.0), 0.25)), 0.02);

    /* From levels directly, unsorted. */
    {
        const double in[4]  = { -10.0, -20.0, -30.0, -5.0 };
        const double out[4] = { -0.5, -10.0, -20.0, -7.0 };
        CHECK("fit", m2sdr_compression_fit(in, out, 4, res) == 0 && res->found);
        CHECK_NEAR("fit gain (dB)", res->small_signal_gain_db, (10.0 + 10.0 + 9.5) / 3.0, 1e-9);
        CHECK_NEAR("fit P1dB in (dBFS)", res->p1db_in_dbfs, -10.0 + (1.0 - 1.0 / 3.0) * 5.0 / (71.0 / 6.0 - 1.0 / 3.0), 1e-9);
    }

    /* Noise only: no tone, no gain. */
    for (s = 0; s < STEPS; s++) {
        memset(buf + (size_t)s * COUNT, 0, COUNT * sizeof(*buf));
        add_noise(buf + (size_t)s * COUNT, COUNT, 1e-3);
    }
    CHECK("noise only", m2sdr_compression_analyse(steps, STEPS, FS, f, res) == 0 && !res->found && res->steps == 0);
    CHECK("noise only, tone", m2sdr_compression_tone(steps[0].x, COUNT, FS, f, &level) == 1);
    CHECK("bad: no steps", m2sdr_compression_analyse(steps, 0, FS, f, res) == -1);
    CHECK("bad: frequency", m2sdr_compression_tone(steps[0].x, COUNT, FS, FS, &level) == -1);
    CHECK("bad: short", m2sdr_compression_tone(steps[0].x, 100, FS, f, &level) == -1);
    free(buf);
    free(res);
}

/* IMD */
/*-----*/

/* y = g (a1 x + a3 x |x|^2 + a5 x |x|^4). With two tones of amplitude A: the tones come out at
 * a1 A + 3 a3 A^3 + 10 a5 A^5, IM3 at a3 A^3 + 5 a5 A^5, IM5 at a5 A^5 (expansion of |x|^2 and
 * |x|^4 over the two tones); nothing beyond IM5. */
static void test_imd(void)
{
    const size_t count = 1u << 18;
    const double complex a1 = 1.0, a3 = -2.0 * cexp(I * 0.2), a5 = 5.0 * cexp(-I * 0.4);
    const double complex g = 0.7 * cexp(I * 0.5);
    const double A = 0.1, pin = 20.0 * log10(A);
    float complex *x = malloc(count * sizeof(*x));
    struct m2sdr_imd_config cfg;
    struct m2sdr_imd_result res;
    double f1, f2, t0, t1, tone, im3, im5, iip3;
    size_t i;

    printf("IMD, polynomial a1 1, a3 2, a5 5:\n");
    m2sdr_lab_two_tone(x, count, FS, 100e3, 10e3, pin, &f1, &f2);
    for (i = 0; i < count; i++) {
        const double complex v = x[i];
        const double r2 = creal(v * conj(v));
        x[i] = (float complex)(g * (a1 * v + a3 * v * r2 + a5 * v * r2 * r2));
        /* Spur at -270 kHz, -85 dBFS, and the DC of the receiver at -50 dBFS. */
        x[i] += (float complex)(pow(10.0, -85.0 / 20.0) * cexp(I * 2.0 * M_PI * -270e3 * (double)i / FS));
        x[i] += 0.003f;
    }
    add_noise(x, count, 1e-10);

    tone = 20.0 * log10(cabs(g * (a1 * A + 3.0 * a3 * A * A * A + 10.0 * a5 * pow(A, 5))));
    im3  = 20.0 * log10(cabs(g * (a3 * A * A * A + 5.0 * a5 * pow(A, 5))));
    im5  = 20.0 * log10(cabs(g * a5 * pow(A, 5)));
    iip3 = pin + (tone - im3) / 2.0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.f1_hz       = f1;
    cfg.f2_hz       = f2;
    cfg.input_known = true;
    cfg.input_dbfs  = pin;
    t0 = now_ms();
    CHECK("analyse", m2sdr_imd_analyse(x, count, FS, &cfg, &res) == 0 && res.found);
    t1 = now_ms();
    printf("  one analysis, %zu samples: %.1f ms\n", count, t1 - t0);
    /* Lines 40 dB and more over the noise of their band: their lobes give them within a few
     * hundredths of a dB. */
    CHECK_NEAR("tone 1 (dBFS)", res.tone1_dbfs, tone, 0.03);
    CHECK_NEAR("tone 2 (dBFS)", res.tone2_dbfs, tone, 0.03);
    CHECK_NEAR("f1 (Hz)", res.f1_hz, f1, 2.0);
    CHECK_NEAR("IM3 low (dBFS)", res.im3_low_dbfs, im3, 0.05);
    CHECK_NEAR("IM3 high (dBFS)", res.im3_high_dbfs, im3, 0.05);
    CHECK_NEAR("IM5 low (dBFS)", res.im5_low_dbfs, im5, 0.05);
    CHECK_NEAR("IM5 high (dBFS)", res.im5_high_dbfs, im5, 0.05);
    CHECK("IM3, IM5 measured", res.im3_measured && res.im5_measured);
    CHECK_NEAR("gain (dB)", res.gain_db, tone - pin, 0.03);
    CHECK_NEAR("IIP3 (dBFS)", res.iip3_dbfs, iip3, 0.05);
    CHECK_NEAR("OIP3 (dBFS)", res.oip3_dbfs, iip3 + tone - pin, 0.05);
    CHECK_NEAR("IIP5 (dBFS)", res.iip5_dbfs, pin + (tone - im5) / 4.0, 0.05);
    /* The small-signal intercept is sqrt(a1 / a3) in amplitude. At these levels the tones are
     * compressed by 3 a3 A^2 (0.5 dB) and IM3 raised by 5 a5 A^2 / a3: the measured one is
     * within a dB of it. */
    CHECK_NEAR("IIP3 against small-signal (dBFS)", res.iip3_dbfs, 10.0 * log10(cabs(a1 / a3)), 1.0);
    for (i = 0; i < (size_t)res.nspurs; i++)
        printf("  spur %.0f Hz, %.1f dBFS\n", res.spur_hz[i], res.spur_dbfs[i]);
    CHECK("one spur (DC left out)", res.nspurs == 1);
    if (res.nspurs >= 1) {
        CHECK_NEAR("spur (Hz)", res.spur_hz[0], -270e3, 30.0);
        CHECK_NEAR("spur (dBFS)", res.spur_dbfs[0], -85.0, 0.1);
    }

    /* Without the input level: output figures only. */
    cfg.input_known = false;
    CHECK("no input level", m2sdr_imd_analyse(x, count, FS, &cfg, &res) == 0 && !res.input_known && res.iip3_dbfs == 0);

    /* A linear device: IM3 under the noise, an intercept that is a bound. */
    m2sdr_lab_two_tone(x, count, FS, 100e3, 10e3, pin, &f1, &f2);
    add_noise(x, count, 1e-10);
    CHECK("linear", m2sdr_imd_analyse(x, count, FS, &cfg, &res) == 0 && res.found && !res.im3_measured);
    CHECK("linear: IM3 at the floor", fabs(res.im3_low_dbfs - res.floor_dbfs) < 1e-9);

    /* Noise only. */
    memset(x, 0, count * sizeof(*x));
    add_noise(x, count, 1e-3);
    CHECK("noise only", m2sdr_imd_analyse(x, count, FS, &cfg, &res) == 0 && !res.found);
    CHECK("bad: tones swapped", (cfg.f1_hz = f2, cfg.f2_hz = f1, m2sdr_imd_analyse(x, count, FS, &cfg, &res) == -1));
    cfg.f1_hz = 100e3;
    cfg.f2_hz = 100.1e3;
    CHECK("bad: tones too close", m2sdr_imd_analyse(x, count, FS, &cfg, &res) == -1);
    cfg.f1_hz = f1;
    cfg.f2_hz = f2;
    CHECK("bad: short", m2sdr_imd_analyse(x, 512, FS, &cfg, &res) == -1);
    free(x);
}

/* AM/AM */
/*-------*/

/* Saleh amplifier (TWT, Saleh 1981): A(r) = aa r / (1 + ba r^2), P(r) = ap r^2 / (1 + bp r^2). */
#define SALEH_AA 2.1587
#define SALEH_BA 1.1517
#define SALEH_AP 4.0033
#define SALEH_BP 9.1040

static float complex saleh(float complex x)
{
    const double r = cabsf(x), r2 = r * r;
    return (float complex)(x * SALEH_AA / (1.0 + SALEH_BA * r2) * cexp(I * SALEH_AP * r2 / (1.0 + SALEH_BP * r2)));
}

/* Capture of the reference through: a delay of `delay` samples (before the amplifier: the
 * transmitter), the amplifier (or none), the gain of the receiver, noise; the first sample at
 * `offset` of the period. The ACLR the capture should give, from its own transform, too. */
static void amam_capture(const float complex *ref, size_t period, double delay, bool amp, double complex g,
                         size_t offset, float complex *x, size_t count, double noise)
{
    float complex *u = malloc(period * sizeof(*u));
    size_t i;

    memcpy(u, ref, period * sizeof(*u));
    delay_periodic(u, period, delay);
    for (i = 0; i < period; i++)
        u[i] = (float complex)g * (amp ? saleh(u[i]) : u[i]);
    for (i = 0; i < count; i++)
        x[i] = u[(offset + i) % period];
    add_noise(x, count, noise);
    free(u);
}

static void test_amam(void)
{
    const size_t period = 16384, count = 4 * period + 1000, offset = 5000;
    const double complex g = 0.4 * cexp(I * 1.0);
    const double noise = 1e-7, delay = 17.3;
    float complex *ref = malloc(period * sizeof(*ref)), *x = malloc(count * sizeof(*x));
    struct m2sdr_amam_result *res = malloc(sizeof(*res));
    double err_g = 0, err_p = 0, t0, t1, pu = 0, evm_model, gm_abs;
    int b, checked = 0;
    size_t i;

    printf("AM/AM, Saleh amplifier on OFDM-like noise:\n");
    m2sdr_lab_noise(ref, period, FS, 150e3, -12.0, 3, NULL);
    amam_capture(ref, period, delay, true, g, offset, x, count, noise);
    t0 = now_ms();
    CHECK("analyse", m2sdr_amam_analyse(x, count, FS, ref, period, NULL, res) == 0 && res->found);
    t1 = now_ms();
    printf("  one analysis, %zu samples: %.1f ms\n", count, t1 - t0);
    /* x[lag + i] follows ref[i] for lag = delay - offset, modulo the period. */
    CHECK_NEAR("delay (samples)", res->delay_samples, fmod(delay - (double)offset + 2.0 * period, (double)period), 0.01);
    CHECK("periods", res->periods == 3);
    /* Small-signal: the gain of the receiver times aa, extrapolated to zero amplitude from the
     * bins below half the RMS, where the gain is linear in r^2 to within bp^2 r^4 (0.1 deg). */
    CHECK_NEAR("small-signal gain (dB)", res->gain_db, 20.0 * log10(0.4 * SALEH_AA), 0.02);
    CHECK_NEAR("small-signal phase (deg)", res->phase_deg, 1.0 * 180.0 / M_PI, 0.3);
    for (b = 0; b < res->bins; b++) {
        double r, r2;
        if (res->bin_count[b] < 500)
            continue;
        r  = pow(10.0, res->bin_in_dbfs[b] / 20.0);
        r2 = r * r;
        err_g = fmax(err_g, fabs(res->bin_gain_db[b] + 20.0 * log10(1.0 + SALEH_BA * r2)));
        err_p = fmax(err_p, fabs(res->bin_phase_deg[b] - SALEH_AP * r2 / (1.0 + SALEH_BP * r2) * 180.0 / M_PI));
        checked++;
    }
    printf("  %d bins checked, compression %.2f dB and AM/PM %.1f deg at %.1f dBFS\n",
           checked, res->compression_db, res->am_pm_deg, res->compression_in_dbfs);
    /* Each bin: thousands of samples at 58 dB of SNR (noise -70 dBFS against -12 dBFS), and its
     * width (1/64 of the peak) changes the curves by 1e-4 dB: the error is that of the
     * small-signal extrapolation. */
    CHECK("bins checked", checked > 30);
    CHECK_NEAR("AM/AM error, max (dB)", err_g, 0.0, 0.03);
    CHECK_NEAR("AM/PM error, max (deg)", err_p, 0.0, 0.3);
    CHECK_NEAR("compression at the top (dB)", res->compression_db,
               20.0 * log10(1.0 + SALEH_BA * pow(10.0, res->compression_in_dbfs / 10.0)), 0.03);

    /* EVM: that of the model after its least-squares gain, and the noise over the output. */
    {
        float complex *u = malloc(period * sizeof(*u));
        double complex num = 0;
        double den = 0, e = 0;
        memcpy(u, ref, period * sizeof(*u));
        delay_periodic(u, period, delay);
        for (i = 0; i < period; i++) {
            const double complex y = saleh(u[i]);
            num += y * conj((double complex)u[i]);
            den += creal(u[i] * conj(u[i]));
        }
        for (i = 0; i < period; i++) {
            const double complex d = saleh(u[i]) - num / den * u[i];
            e += creal(d * conj(d));
        }
        gm_abs    = cabs(num / den);
        pu        = den / period;
        evm_model = e / (gm_abs * gm_abs * den);
        free(u);
    }
    {
        const double expected = sqrt(evm_model + noise / (0.16 * gm_abs * gm_abs * pu));
        /* The residual is averaged over 49152 samples: its power within 1 %. */
        CHECK_NEAR("EVM (%)", 100.0 * res->evm_rms, 100.0 * expected, 0.01 * 100.0 * expected);
    }
    CHECK("ACLR", res->aclr && res->acpr.adjacent == 2);
    CHECK_NEAR("channel bandwidth (kHz)", res->channel_bw_hz / 1e3, 150.0, 0.1);
    printf("  ACLR %.1f / %.1f dBc, alternate %.1f / %.1f dBc, EVM %.2f %%\n", res->acpr.lower_dbc[0],
           res->acpr.upper_dbc[0], res->acpr.lower_dbc[1], res->acpr.upper_dbc[1], 100.0 * res->evm_rms);
    CHECK("ACLR raised by the amplifier", res->acpr.lower_dbc[0] > -40.0 && res->acpr.upper_dbc[0] > -40.0);
    CHECK("npoints", res->npoints == M2SDR_AMAM_MAX_POINTS);

    /* Linear device: flat curves, and an ACLR that is the noise in the adjacent channel. */
    amam_capture(ref, period, delay, false, g, offset, x, count, noise);
    CHECK("linear", m2sdr_amam_analyse(x, count, FS, ref, period, NULL, res) == 0 && res->found);
    CHECK_NEAR("linear compression (dB)", res->compression_db, 0.0, 0.01);
    CHECK_NEAR("linear delay (samples)", res->delay_samples, fmod(delay - (double)offset + 2.0 * period, (double)period), 0.002);
    {
        const double w = res->channel_bw_hz / FS * period;   /* Bins of a channel. */
        const double nb = noise * w / period, chan = 0.16 * pu + nb;
        /* The noise of a channel of 2457 bins over 3 periods: within 0.05 dB. */
        CHECK_NEAR("linear ACLR (dBc)", res->acpr.lower_dbc[0], db(nb / chan), 0.2);
        CHECK_NEAR("linear EVM (%)", 100.0 * res->evm_rms, 100.0 * sqrt(noise / (0.16 * pu)), 0.02 * 100.0 * sqrt(noise / (0.16 * pu)));
    }

    /* QAM reference through a Rapp amplifier: AM/AM against its curve, no AM/PM. */
    printf("AM/AM, Rapp amplifier on 16QAM:\n");
    m2sdr_lab_qam(ref, period, M2SDR_MOD_16QAM, 8, 0.35, -12.0, 9, NULL);
    {
        float complex *u = malloc(period * sizeof(*u));
        memcpy(u, ref, period * sizeof(*u));
        delay_periodic(u, period, 3.6);
        for (i = 0; i < period; i++)
            u[i] = rapp(u[i], 1.0, 0.35, 3.0);
        for (i = 0; i < count; i++)
            x[i] = u[(offset + i) % period];
        add_noise(x, count, noise);
        free(u);
    }
    CHECK("analyse", m2sdr_amam_analyse(x, count, FS, ref, period, NULL, res) == 0 && res->found);
    CHECK_NEAR("delay (samples)", res->delay_samples, fmod(3.6 - (double)offset + 2.0 * period, (double)period), 0.01);
    err_g = err_p = 0;
    checked = 0;
    for (b = 0; b < res->bins; b++) {
        double r;
        if (res->bin_count[b] < 500)
            continue;
        r = pow(10.0, res->bin_in_dbfs[b] / 20.0);
        err_g = fmax(err_g, fabs(res->bin_gain_db[b] + 20.0 / 6.0 * log10(1.0 + pow(r / 0.35, 6.0))));
        err_p = fmax(err_p, fabs(res->bin_phase_deg[b]));
        checked++;
    }
    printf("  %d bins checked, compression %.2f dB at %.1f dBFS\n", checked, res->compression_db, res->compression_in_dbfs);
    CHECK("bins checked", checked > 30);
    CHECK_NEAR("AM/AM error, max (dB)", err_g, 0.0, 0.03);
    CHECK_NEAR("AM/PM, max (deg)", err_p, 0.0, 0.2);
    /* The band of an RRC of roll-off 0.35 at 125 ksymbols/s: 168.75 kHz. */
    CHECK_NEAR("channel bandwidth (kHz)", res->channel_bw_hz / 1e3, 168.75, 0.2);

    /* Noise only, bad arguments. */
    memset(x, 0, count * sizeof(*x));
    add_noise(x, count, 1e-3);
    CHECK("noise only", m2sdr_amam_analyse(x, count, FS, ref, period, NULL, res) == 0 && !res->found);
    printf("  noise only: sync %.1f dB\n", res->sync_quality);
    CHECK("bad: period", m2sdr_amam_analyse(x, count, FS, ref, 10000, NULL, res) == -1);
    CHECK("bad: short", m2sdr_amam_analyse(x, period, FS, ref, period, NULL, res) == -1);
    CHECK("bad: no reference", m2sdr_amam_analyse(x, count, FS, NULL, period, NULL, res) == -1);
    free(ref);
    free(x);
    free(res);
}

/* Report tests: captures for m2sdr_analyzer --report */
/*-----------------------------------------------------*/

/* The stimulus written by m2sdr_analyzer --lab-stimulus-out (SC16 Q11, two channels): the
 * first channel, full scale 1.0. */
static float complex *read_stimulus(const char *path, size_t *period)
{
    FILE *f = fopen(path, "rb");
    float complex *x = NULL;
    long bytes;
    size_t i;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    *period = (size_t)bytes / 8;
    x = malloc(*period * sizeof(*x));
    for (i = 0; x && i < *period; i++) {
        short v[4];
        if (fread(v, sizeof(v), 1, f) != 1) {
            free(x);
            x = NULL;
            break;
        }
        x[i] = (v[0] + I * v[1]) / 2048.0f;
    }
    fclose(f);
    return x;
}

static int write_capture_file(const char *path, const float complex *x, size_t count)
{
    FILE *f = fopen(path, "wb");
    size_t i;

    if (!f)
        return 1;
    for (i = 0; i < count; i++) {
        const short re = (short)lrintf(fminf(fmaxf(crealf(x[i]), -1.0f), 1.0f) * 2047.0f);
        const short im = (short)lrintf(fminf(fmaxf(cimagf(x[i]), -1.0f), 1.0f) * 2047.0f);
        const short v[4] = { re, im, re, im };
        if (fwrite(v, sizeof(v), 1, f) != 1)
            return 1;
    }
    return fclose(f) == 0 ? 0 : 1;
}

/* Sweep: a thru (the radio: -3 dB, 40 degrees, 23 samples) and the one-pole filter behind it,
 * captured at other offsets of the stimulus. */
static int write_sweep(const char *stim_path, const char *thru_path, const char *dut_path)
{
    const struct path thru = { -3.0, 40.0, 23, 0.0 };
    const struct path dut  = { -3.0, 40.0, 23, 0.5 };
    size_t period, count;
    float complex *stim = read_stimulus(stim_path, &period), *x;
    int rc;

    if (!stim)
        return 1;
    count = 5 * period;
    x = malloc(count * sizeof(*x));
    run_path(stim, period, &thru, 4 * period + 1234, x, count, 1e-7);
    rc = write_capture_file(thru_path, x, count);
    run_path(stim, period, &dut, 4 * period + 777, x, count, 1e-7);
    rc |= write_capture_file(dut_path, x, count);
    free(x);
    free(stim);
    return rc;
}

static int check_sweep(const char *path)
{
    const char *json = read_report(path);
    double gd_lo = 1e9, gd_hi = -1e9, f;

    if (!json)
        return 1;
    printf("Sweep report:\n");
    CHECK("found", strstr(json, "\"found\": true") != NULL);
    CHECK("calibrated", strstr(json, "\"calibrated\": true") != NULL);
    /* Default span: 0.8 of 1 MSPS, 201 points; the one-pole filter from 0 dB at the centre to
     * |H(400 kHz)| at the edges. The 12-bit capture adds -70 dBFS of noise: 0.02 dB. */
    CHECK_NEAR("|S21| centre (dB)", json_number(json, "sweep", "mag_centre_db"), 0.0, 0.02);
    CHECK_NEAR("|S21| max (dB)", json_number(json, "sweep", "mag_max_db"), 0.0, 0.02);
    CHECK_NEAR("|S21| min (dB)", json_number(json, "sweep", "mag_min_db"), 20.0 * log10(cabs(pole_h(0.5, 400e3))), 0.03);
    /* The group delay without the timing is given from its mean: its peak to peak over the span
     * inside the end points (one step in), samples of 1 us. */
    for (f = -0.4e6 + 4e3; f <= 0.4e6 - 4e3 + 1.0; f += 4e3) {
        gd_lo = fmin(gd_lo, pole_gd(0.5, f));
        gd_hi = fmax(gd_hi, pole_gd(0.5, f));
    }
    CHECK_NEAR("group delay p-p (ns)", json_number(json, "sweep", "group_delay_pp_ns"), (gd_hi - gd_lo) * 1e3, 10.0);
    return report_done("sweep");
}

/* Compression: the Rapp amplifier of the tests above on the staircase. */
static int write_compression(const char *stim_path, const char *out_path)
{
    size_t period, count, i;
    float complex *stim = read_stimulus(stim_path, &period), *x;
    int rc;

    if (!stim)
        return 1;
    count = 3 * period;
    x = malloc(count * sizeof(*x));
    for (i = 0; i < count; i++)
        x[i] = rapp(stim[(i + 5000) % period], RAPP_G, RAPP_SAT, RAPP_P);
    add_noise(x, count, 1e-8);
    rc = write_capture_file(out_path, x, count);
    free(x);
    free(stim);
    return rc;
}

static int check_compression(const char *path)
{
    const char *json = read_report(path);
    const double g0 = 20.0 * log10(RAPP_G);
    const double p1 = 20.0 * log10(RAPP_SAT / RAPP_G * pow(pow(10.0, RAPP_P / 10.0) - 1.0, 1.0 / (2.0 * RAPP_P)));

    if (!json)
        return 1;
    printf("Compression report:\n");
    CHECK("found", strstr(json, "\"found\": true") != NULL);
    CHECK_NEAR("steps", json_number(json, "compression", "steps"), 31.0, 0.0);
    /* 1 dB steps: the interpolation errs by 0.05 dB at most (see test_compression()), and the
     * 12 bits of the file add 0.01 dB at the lowest steps. */
    CHECK_NEAR("small-signal gain (dB)", json_number(json, "compression", "small_signal_gain_db"), g0, 0.03);
    CHECK_NEAR("P1dB in (dBFS)", json_number(json, "compression", "p1db_in_db"), p1, 0.1);
    CHECK_NEAR("P1dB out (dBFS)", json_number(json, "compression", "p1db_out_db"), p1 + g0 - 1.0, 0.1);
    return report_done("compression");
}

/* IMD: y = a1 x + a3 x |x|^2 + a5 x |x|^4 on the two tones of -12 dBFS each. */
#define FILE_A3 (-0.1)
#define FILE_A5 (-0.5)

static int write_imd(const char *stim_path, const char *out_path)
{
    size_t period, count, i;
    float complex *stim = read_stimulus(stim_path, &period), *x;
    int rc;

    if (!stim)
        return 1;
    count = 5 * period;
    x = malloc(count * sizeof(*x));
    for (i = 0; i < count; i++) {
        const double complex v = stim[(i + 999) % period];
        const double r2 = creal(v * conj(v));
        x[i] = (float complex)(v + FILE_A3 * v * r2 + FILE_A5 * v * r2 * r2);
    }
    add_noise(x, count, 1e-9);
    rc = write_capture_file(out_path, x, count);
    free(x);
    free(stim);
    return rc;
}

static int check_imd(const char *path)
{
    const char *json = read_report(path);
    const double A = pow(10.0, -12.0 / 20.0);
    const double tone = 20.0 * log10(fabs(A + 3.0 * FILE_A3 * pow(A, 3) + 10.0 * FILE_A5 * pow(A, 5)));
    const double im3  = 20.0 * log10(fabs(FILE_A3 * pow(A, 3) + 5.0 * FILE_A5 * pow(A, 5)));
    const double im5  = 20.0 * log10(fabs(FILE_A5 * pow(A, 5)));

    if (!json)
        return 1;
    printf("IMD report:\n");
    CHECK("found", strstr(json, "\"found\": true") != NULL);
    /* Lines 30 dB and more over the noise of their band; the 12-bit stimulus and capture put a
     * floor near -100 dBFS, and their rounding puts lines of their own on the products (periodic
     * tones): IM5 (-66 dBFS) within 0.2 dB. */
    CHECK_NEAR("tone 1 (dBFS)", json_number(json, "imd", "tone1_db"), tone, 0.05);
    CHECK_NEAR("IM3 low (dBFS)", json_number(json, "imd", "im3_low_db"), im3, 0.05);
    CHECK_NEAR("IM5 high (dBFS)", json_number(json, "imd", "im5_high_db"), im5, 0.2);
    CHECK_NEAR("IIP3 (dBFS)", json_number(json, "imd", "iip3_db"), -12.0 + (tone - im3) / 2.0, 0.05);
    return report_done("IMD");
}

/* AM/AM: the Saleh amplifier on the OFDM-like noise of -15 dBFS. */
static int write_amam(const char *stim_path, const char *out_path)
{
    size_t period, count, i;
    float complex *stim = read_stimulus(stim_path, &period), *x;
    int rc;

    if (!stim)
        return 1;
    count = 10 * period;          /* The capture of the spectrum is longer than four periods. */
    x = malloc(count * sizeof(*x));
    for (i = 0; i < count; i++)
        x[i] = 0.4f * saleh(stim[(i + 3333) % period]);
    add_noise(x, count, 1e-8);
    rc = write_capture_file(out_path, x, count);
    free(x);
    free(stim);
    return rc;
}

static int check_amam(const char *path)
{
    const char *json = read_report(path);
    const double in = json_number(json ? json : "", "amam", "compression_in_db");

    if (!json)
        return 1;
    printf("AM/AM report:\n");
    CHECK("found", strstr(json, "\"found\": true") != NULL);
    /* Gain: 0.4 aa; compression at the top bin: Saleh at the RMS amplitude of that bin. */
    CHECK_NEAR("gain (dB)", json_number(json, "amam", "gain_db"), 20.0 * log10(0.4 * SALEH_AA), 0.03);
    CHECK_NEAR("compression at the top (dB)", json_number(json, "amam", "compression_db"),
               20.0 * log10(1.0 + SALEH_BA * pow(10.0, in / 10.0)), 0.05);
    CHECK_NEAR("AM/PM at the top (deg)", json_number(json, "amam", "am_pm_deg"),
               SALEH_AP * pow(10.0, in / 10.0) / (1.0 + SALEH_BP * pow(10.0, in / 10.0)) * 180.0 / M_PI, 0.4);
    CHECK("ACLR", strstr(json, "\"aclr_lower_dbc\"") != NULL);
    return report_done("AM/AM");
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--write-sweep") == 0)
        return write_sweep(argv[2], argv[3], argv[4]);
    if (argc == 3 && strcmp(argv[1], "--check-sweep") == 0)
        return check_sweep(argv[2]);
    if (argc == 4 && strcmp(argv[1], "--write-compression") == 0)
        return write_compression(argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "--check-compression") == 0)
        return check_compression(argv[2]);
    if (argc == 4 && strcmp(argv[1], "--write-imd") == 0)
        return write_imd(argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "--check-imd") == 0)
        return check_imd(argv[2]);
    if (argc == 4 && strcmp(argv[1], "--write-amam") == 0)
        return write_amam(argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "--check-amam") == 0)
        return check_amam(argv[2]);
    test_stimuli();
    test_sweep();
    test_compression();
    test_imd();
    test_amam();
    return tests_done();
}
