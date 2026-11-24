/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the FM broadcast analysis on synthetic signals of known parameters: the modulator
 * of m2sdr_fm.h (stereo multiplex and RDS of known content), and FM written here from a
 * multiplex of tones, whose phase is their exact integral.
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

#include "../libm2sdr/m2sdr_analysis_fm.h"

#define FS     960000.0
#define REF2   (19000.0 * 19000.0 / 2.0)   /* BS.412 0 dBr, Hz^2. */

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

/* A multiplex of tones: deviation amp_hz * cos(2 pi f t + phase). */
struct tone {
    double hz, amp_hz, phase;
};

static double mpx_value(const struct tone *t, int n, double time)
{
    double v = 0.0;
    int i;

    for (i = 0; i < n; i++)
        v += t[i].amp_hz * cos(2.0 * M_PI * t[i].hz * time + t[i].phase);
    return v;
}

/* FM of the tones, carrier at offset_hz and level_db (dBFS). The phase is the exact
 * integral of the frequency: what an analog modulator does. */
static void make_fm(float complex *x, size_t count, double fs, double offset_hz, double level_db,
                    const struct tone *t, int n)
{
    const double amp = pow(10.0, level_db / 20.0);
    size_t k;
    int i;

    for (k = 0; k < count; k++) {
        const double time = (double)k / fs;
        double phase = 2.0 * M_PI * offset_hz * time;

        for (i = 0; i < n; i++)
            phase += t[i].amp_hz / t[i].hz * sin(2.0 * M_PI * t[i].hz * time + t[i].phase);
        x[k] = (float complex)(amp * cexp(I * phase));
    }
}

/* Through the modulator of libm2sdr: stereo, 75 kHz, no emphasis (the levels are then those
 * of the audio), 16 bits, a tone of `amp` (full scale 1) on the left channel only. */
static size_t make_tx(float complex *x, size_t count, const struct m2sdr_fm_config *cfg,
                      double tone_hz, double amp)
{
    struct m2sdr_fm_tx *tx = m2sdr_fm_tx_new(cfg);
    unsigned interp;
    double arate;
    size_t frames, f, k;
    float *audio;
    int16_t *iq;

    if (!tx)
        return 0;
    interp = m2sdr_fm_tx_interpolation(tx);
    arate  = m2sdr_fm_tx_audio_rate(tx);
    frames = count / interp;
    audio  = malloc(2 * frames * sizeof(*audio));
    iq     = malloc(2 * frames * interp * sizeof(*iq));
    if (!audio || !iq) {
        free(audio);
        free(iq);
        m2sdr_fm_tx_free(tx);
        return 0;
    }
    for (f = 0; f < frames; f++) {
        audio[2 * f + 0] = (float)(amp * sin(2.0 * M_PI * tone_hz * (double)f / arate));
        audio[2 * f + 1] = 0.0f;
    }
    m2sdr_fm_tx_process(tx, audio, frames, iq);
    for (k = 0; k < frames * interp; k++)
        x[k] = (iq[2 * k] + I * iq[2 * k + 1]) / 32767.0f;
    free(audio);
    free(iq);
    m2sdr_fm_tx_free(tx);
    return frames * interp;
}

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* Tests */
/*-------*/

/* Stereo with RDS from the modulator, 1 kHz on the left only. The modulator gives the pilot
 * 10 % of 75 kHz (7.5 kHz), RDS 4 % (3 kHz, a stream of identical bits) and the audio the
 * rest, 86 %: a tone of amplitude a on the left is a * 0.86 * 75 kHz / 2 in L+R and the same
 * in L-R. */
static void test_tx_stereo_rds(float complex *x, size_t cap)
{
    static const char *ps = "M2SDR FM", *rt = "LiteX-M2SDR FM analysis test";
    struct m2sdr_fm_config cfg;
    struct m2sdr_fm_analysis_config acfg;
    struct m2sdr_fm_result *res = malloc(sizeof(*res));
    const double amp = 0.5, tone = amp * 0.86 * 75000.0 / 2.0;
    double expect_ms;
    size_t n, i;

    printf("FM stereo + RDS from m2sdr_fm_tx, 1 kHz left only, 0.96 MSPS:\n");
    m2sdr_fm_config_init(&cfg);
    cfg.sample_rate = FS;
    cfg.tau         = 0.0;
    cfg.stereo      = true;
    cfg.bits        = 16;
    cfg.rds.enable  = true;
    cfg.rds.pi      = 0xF201;
    cfg.rds.pty     = 10;
    cfg.rds.tp      = true;
    cfg.rds.ta      = false;
    cfg.rds.music   = true;
    strcpy(cfg.rds.ps, ps);
    strcpy(cfg.rds.rt, rt);
    n = make_tx(x, (size_t)(4.0 * FS) < cap ? (size_t)(4.0 * FS) : cap, &cfg, 1000.0, amp);
    CHECK("modulator", n > 0 && res);
    if (!n || !res) {
        free(res);
        return;
    }

    memset(&acfg, 0, sizeof(acfg));
    acfg.tau     = 50e-6;
    acfg.tone_hz = 1000.0;
    CHECK("analyse", m2sdr_fm_analyse(x, (size_t)(2.0 * FS), FS, &acfg, res) == 0);
    CHECK("found", res->found);
    CHECK_NEAR("mpx rate (Hz)", res->mpx_rate, 240000.0, 0.0);
    /* Full scale 16-bit sine table: 0 dBFS within its quantisation. */
    CHECK_NEAR("carrier (dBFS)", res->carrier_dbfs, 0.0, 0.01);
    CHECK_NEAR("carrier offset (Hz)", res->carrier_offset_hz, 0.0, 1.0);
    /* The tx interpolation filter is flat within 0.001 dB at 19 kHz and the two droop
     * compensations agree exactly at 38 kHz and within 0.1 % at 19 kHz. */
    CHECK("pilot", res->pilot && res->stereo);
    CHECK_NEAR("pilot deviation (Hz)", res->pilot_dev_hz, 7500.0, 15.0);
    CHECK_NEAR("pilot frequency error (Hz)", res->pilot_freq_error_hz, 0.0, 0.01);
    CHECK("tone", res->tone);
    CHECK_NEAR("tone L+R (Hz)", res->tone_mono_hz, tone, 0.003 * tone);
    CHECK_NEAR("tone L-R (Hz)", res->tone_stereo_hz, tone, 0.003 * tone);
    CHECK_NEAR("tone left (Hz)", res->tone_left_hz, 2.0 * tone, 0.003 * 2.0 * tone);
    /* Separation of the modulator and of this analysis: only filter ripples between L+R
     * and L-R, below 0.01 dB, so above 50 dB. */
    CHECK("separation > 50 dB", res->separation_db > 50.0);
    printf("  separation %.1f dB, right %.1f Hz\n", res->separation_db, res->tone_right_hz);
    /* L+R and L-R RMS of the same tone: 0 dB. */
    CHECK_NEAR("L+R RMS (Hz)", res->mono_rms_hz, tone / sqrt(2.0), 0.01 * tone);
    CHECK_NEAR("L-R over L+R (dB)", res->stereo_ratio_db, 0.0, 0.1);
    /* Power: L+R tone T^2/2, L-R on its subcarrier T^2/4, pilot 7500^2/2, RDS 3000^2/4 (the
     * shaped biphase data, random, has the power of the stream of identical bits:
     * the integral of cos^2(pi f td / 4) sin^2(pi f td / 2) over |f| < 2 / td is 1 / td). */
    expect_ms = tone * tone * 0.75 + 7500.0 * 7500.0 / 2.0 + 3000.0 * 3000.0 / 4.0;
    CHECK_NEAR("MPX power (dBr)", res->mpx_power_dbr, 10.0 * log10(expect_ms / REF2), 0.05);
    /* Peak: below 2 T + pilot + the peaks of RDS (data peaks up to twice its level). */
    CHECK("peak deviation", res->peak_dev_hz > 2.0 * tone && res->peak_dev_hz < 2.0 * tone + 7500.0 + 6000.0);
    CHECK("percentiles", res->dev_p999_hz <= res->peak_dev_hz && res->dev_p99_hz <= res->dev_p999_hz &&
                         res->dev_p99_hz > tone);
    printf("  peak %.0f Hz, p99.9 %.0f Hz, p99 %.0f Hz\n", res->peak_dev_hz, res->dev_p999_hz, res->dev_p99_hz);
    CHECK("RDS", res->rds);
    CHECK_NEAR("RDS level (Hz)", res->rds_dev_hz, 3000.0, 60.0);
    CHECK("RDS PI", res->rds_info.pi_valid && res->rds_info.pi == 0xF201);
    CHECK("RDS PTY, TP, TA, music", res->rds_info.pty == 10 && res->rds_info.tp && !res->rds_info.ta &&
                                    res->rds_info.music);
    CHECK("RDS PS", res->rds_info.ps_valid && strcmp(res->rds_info.ps, ps) == 0);
    CHECK("RDS RT", res->rds_info.rt_valid && strcmp(res->rds_info.rt, rt) == 0);
    printf("  PS '%s' RT '%s'\n", res->rds_info.ps, res->rds_info.rt);
    /* 2 s: 22.8 groups, less the sync. The modulator sends 0A and 2A in turn. */
    CHECK("RDS groups", res->rds_groups >= 20 && res->rds_groups <= 23);
    CHECK("RDS 0A and 2A in turn", res->rds_type_groups[0] + res->rds_type_groups[4] == res->rds_groups &&
          llabs((long long)res->rds_type_groups[0] - (long long)res->rds_type_groups[4]) <= 1);
    CHECK("RDS blocks", res->rds_blocks >= 4 * res->rds_groups);
    CHECK_NEAR("RDS block error rate", res->rds_bler, 0.0, 0.0);
    CHECK("RDS points", res->npoints == M2SDR_FM_MAX_POINTS);
    {
        double e = 0.0;
        int k;

        /* Clean bits: on the real axis, |re| near 1. */
        for (k = 0; k < res->npoints; k++)
            e += cimagf(res->points[k]) * cimagf(res->points[k]);
        /* The transmitter and the receiver filters leave the bits clean: on the real axis. */
        CHECK("RDS points on the real axis", res->npoints > 0 && e / res->npoints < 1e-3);
        /* The shaped biphase symbol through its matched filter is not free of intersymbol
         * interference: a clean signal reads about 29 dB. */
        CHECK("RDS MER > 27 dB", res->rds_mer_db > 27.0);
        printf("  RDS MER %.1f dB\n", res->rds_mer_db);
    }
    /* Audio SNR: the noise is the quantisation of the 16-bit modulator, above 70 dB. */
    CHECK("audio SNR > 70 dB", res->audio_snr_db > 70.0);
    printf("  audio SNR %.1f dB, CNR %.1f dB\n", res->audio_snr_db, res->cnr_db);
    {
        /* The trace: pilot at -8.1 dBr. */
        const int k = (int)lrint(19000.0 / M2SDR_FM_MPX_BIN_HZ);

        CHECK_NEAR("trace at the pilot (dBr)", res->mpx_db[k], 20.0 * log10(7500.0 / 19000.0), 0.1);
    }

    /* The same in 0.4 s pieces with a decoder kept between them: PS and RT build up over the
     * captures, each too short for them alone. */
    printf("FM stereo + RDS, 0.4 s pieces, persistent RDS decoder:\n");
    {
        const size_t piece = (size_t)(0.4 * FS);
        bool alone_rt = false;
        uint64_t groups = 0;

        acfg.rds = m2sdr_fm_rds_rx_new(m2sdr_fm_analysis_mpx_rate(FS));
        CHECK("decoder", acfg.rds != NULL);
        for (i = 0; acfg.rds && (i + 1) * piece <= n; i++) {
            CHECK("analyse piece", m2sdr_fm_analyse(x + i * piece, piece, FS, &acfg, res) == 0);
            groups += res->rds_groups;
            if (i == 0)
                alone_rt = res->rds_info.rt_valid;
        }
        CHECK("RT not complete in one piece", !alone_rt);
        CHECK("RDS PS over pieces", res->rds_info.ps_valid && strcmp(res->rds_info.ps, ps) == 0);
        CHECK("RDS RT over pieces", res->rds_info.rt_valid && strcmp(res->rds_info.rt, rt) == 0);
        CHECK("groups counted per piece", res->rds_info.groups == groups);
        CHECK_NEAR("RDS block error rate, last piece", res->rds_bler, 0.0, 0.0);
        m2sdr_fm_rds_rx_free(acfg.rds);
    }

    /* Low CNR: whatever is decoded is right. 8 dB of CNR in 240 kHz. */
    printf("FM stereo + RDS, CNR 8 dB:\n");
    n = (size_t)(2.0 * FS);
    add_noise(x, n, pow(10.0, -8.0 / 10.0) * FS / 240000.0);
    acfg.rds = NULL;
    CHECK("analyse", m2sdr_fm_analyse(x, n, FS, &acfg, res) == 0);
    CHECK_NEAR("CNR (dB)", res->cnr_db, 8.0, 0.5);
    CHECK("no wrong PI", !res->rds_info.pi_valid || res->rds_info.pi == 0xF201);
    printf("  found %d, blocks %llu, errors %llu, BLER %.3f\n", res->found,
           (unsigned long long)res->rds_blocks, (unsigned long long)res->rds_block_errors, res->rds_bler);
    free(res);
}

/* Stereo multiplex of tones: L+R 1 kHz, L-R the same tone at 0.9 of its level (a gain error
 * of the L-R path: left (1 + 0.9), right (1 - 0.9), 25.58 dB of separation), a pilot of
 * 6.8 kHz at 19001.5 Hz, a mono 7 kHz tone; carrier at +3210 Hz, -20 dBFS, C/N0 80 dB-Hz. */
static void test_tones_stereo(float complex *x, size_t cap, double fs)
{
    const double m = 20000.0, g = 0.9, fp = 19001.5, php = 0.7, c_dbfs = -20.0, cn0 = 80.0;
    struct tone t[5];
    struct m2sdr_fm_analysis_config acfg;
    struct m2sdr_fm_result *res = malloc(sizeof(*res));
    const size_t n = (size_t)(1.0 * fs) < cap ? (size_t)(1.0 * fs) : cap;
    double ms = 0.0, peak = 0.0, k, snr, t0, dt;
    size_t i, grid;
    float *dense;

    printf("FM tones, stereo with L-R at 0.9, offset +3210 Hz, -20 dBFS, C/N0 80 dB-Hz, %.2f MSPS:\n", fs / 1e6);
    if (!res)
        return;
    /* -g M cos(w1 t) sin(2 psi) = (g M / 2) [cos(2 psi + w1 t + pi/2) + cos(2 psi - w1 t + pi/2)],
     * psi = 2 pi fp t + php the phase of the pilot cos(psi). */
    t[0] = (struct tone){ 1000.0, m, 0.0 };
    t[1] = (struct tone){ fp, 6800.0, php };
    t[2] = (struct tone){ 2.0 * fp + 1000.0, g * m / 2.0, 2.0 * php + M_PI / 2.0 };
    t[3] = (struct tone){ 2.0 * fp - 1000.0, g * m / 2.0, 2.0 * php + M_PI / 2.0 };
    t[4] = (struct tone){ 7000.0, 15000.0, 1.3 };
    make_fm(x, n, fs, 3210.0, c_dbfs, t, 5);
    add_noise(x, n, pow(10.0, (c_dbfs - cn0) / 10.0) * fs);

    /* Expected deviation figures from the multiplex on a grid 8 times finer than the
     * analysis rate: the analysis sees it at 240 kHz, whose peaks are within 1 %. */
    grid  = (size_t)(0.98 * 240000.0 * 8.0);
    dense = malloc(grid * sizeof(*dense));
    if (!dense) {
        free(res);
        return;
    }
    for (i = 0; i < grid; i++) {
        dense[i] = (float)fabs(mpx_value(t, 5, (double)i / (240000.0 * 8.0)));
        if (dense[i] > peak)
            peak = dense[i];
    }
    for (i = 0; i < 5; i++)
        ms += t[i].amp_hz * t[i].amp_hz / 2.0;

    memset(&acfg, 0, sizeof(acfg));
    acfg.tau     = 50e-6;
    acfg.tone_hz = 1000.0;
    t0 = now_s();
    CHECK("analyse", m2sdr_fm_analyse(x, n, fs, &acfg, res) == 0);
    dt = now_s() - t0;
    CHECK("found", res->found);
    CHECK_NEAR("carrier offset (Hz)", res->carrier_offset_hz, 3210.0, 1.0);
    CHECK_NEAR("carrier (dBFS)", res->carrier_dbfs, c_dbfs, 0.05);
    /* C/N0 from the moments: the variance of the estimate over 240k samples at 26 dB of CNR
     * is below 0.05 dB; the filter edges add AM of about -45 dBc. */
    CHECK_NEAR("C/N0 (dB-Hz)", res->cn0_dbhz, cn0, 0.3);
    CHECK("pilot", res->pilot && res->stereo);
    CHECK_NEAR("pilot deviation (Hz)", res->pilot_dev_hz, 6800.0, 10.0);
    /* Phase noise of the pilot pieces: the noise in 100 Hz around 19 kHz is about 50 dB
     * below the pilot, a few mrad per piece, a few mHz over 1 s. */
    CHECK_NEAR("pilot frequency (Hz)", res->pilot_freq_hz, fp, 0.02);
    CHECK("tone", res->tone);
    CHECK_NEAR("tone L+R (Hz)", res->tone_mono_hz, m, 0.002 * m);
    CHECK_NEAR("tone L-R (Hz)", res->tone_stereo_hz, g * m, 0.002 * m);
    CHECK_NEAR("tone left (Hz)", res->tone_left_hz, (1.0 + g) * m, 0.002 * m);
    CHECK_NEAR("tone right (Hz)", res->tone_right_hz, (1.0 - g) * m, 0.002 * m);
    CHECK_NEAR("separation (dB)", res->separation_db, 20.0 * log10((1.0 + g) / (1.0 - g)), 0.2);
    CHECK_NEAR("MPX power (dBr)", res->mpx_power_dbr, 10.0 * log10(ms / REF2), 0.03);
    /* The peak takes the noise too: f^2 N0 / C up to 60 kHz, sigma = 850 Hz at 80 dB-Hz, and
     * the largest of 240k samples of it is about 4.5 sigma. */
    {
        const double sigma = sqrt(pow(10.0, -cn0 / 10.0) * pow(60000.0, 3.0) / 3.0);

        CHECK("peak deviation", res->peak_dev_hz > 0.99 * peak && res->peak_dev_hz < peak + 6.0 * sigma);
        printf("  peak %.0f Hz, of the tones %.0f Hz, noise sigma %.0f Hz\n", res->peak_dev_hz, peak, sigma);
    }
    {
        /* Percentiles of the grid. */
        uint32_t *h = calloc(4001, sizeof(*h));
        double p99 = 0.0, p999 = 0.0, acc = 0.0;
        int b;

        if (h) {
            for (i = 0; i < grid; i++) {
                const int bin = (int)(dense[i] / 50.0);
                h[bin < 4000 ? bin : 4000]++;
            }
            for (b = 4000; b >= 0; b--) {
                acc += h[b];
                if (p999 == 0.0 && acc >= 0.001 * grid)
                    p999 = (b + 0.5) * 50.0;
                if (p99 == 0.0 && acc >= 0.01 * grid)
                    p99 = (b + 0.5) * 50.0;
            }
            free(h);
        }
        CHECK_NEAR("deviation p99 (Hz)", res->dev_p99_hz, p99, 0.005 * p99);
        CHECK_NEAR("deviation p99.9 (Hz)", res->dev_p999_hz, p999, 0.005 * p999);
    }
    /* L+R: the two mono tones; L-R: 0.9 of the 1 kHz tone. */
    CHECK_NEAR("L+R RMS (Hz)", res->mono_rms_hz, sqrt((m * m + 15000.0 * 15000.0) / 2.0), 50.0);
    CHECK_NEAR("L-R RMS (Hz)", res->stereo_rms_hz, g * m / sqrt(2.0), 50.0);
    /* Audio SNR: the discriminator noise is f^2 N0 / C one-sided, through the 50 us
     * de-emphasis over 30 Hz..15 kHz, from a sine of 75 kHz. The estimate is the median of
     * about 40 bins of 7 averaged spectra: within 0.5 dB. */
    k   = pow(10.0, -cn0 / 10.0);
    {
        const double a = 2.0 * M_PI * 50e-6;
        const double integ = (15000.0 - 30.0) / (a * a) - (atan(a * 15000.0) - atan(a * 30.0)) / (a * a * a);

        snr = 10.0 * log10(75000.0 * 75000.0 / 2.0 / (k * integ));
    }
    CHECK_NEAR("audio SNR (dB)", res->audio_snr_db, snr, 0.5);
    CHECK("no RDS", !res->rds && res->rds_dev_hz < 150.0);
    printf("  RDS level read %.0f Hz on noise; %.1f ms for %.1f s\n", res->rds_dev_hz, 1e3 * dt, res->duration_s);
    free(dense);
    free(res);
}

/* Mono: one tone of 50 kHz, no pilot. */
static void test_mono(float complex *x, size_t cap)
{
    const struct tone t = { 1000.0, 50000.0, 0.2 };
    struct m2sdr_fm_analysis_config acfg;
    struct m2sdr_fm_result *res = malloc(sizeof(*res));
    const size_t n = (size_t)(0.25 * FS) < cap ? (size_t)(0.25 * FS) : cap;

    printf("FM mono, 50 kHz at 1 kHz, 0.25 s:\n");
    if (!res)
        return;
    make_fm(x, n, FS, -1500.0, -6.0, &t, 1);
    memset(&acfg, 0, sizeof(acfg));
    acfg.tau     = 50e-6;
    acfg.tone_hz = 1000.0;
    CHECK("analyse", m2sdr_fm_analyse(x, n, FS, &acfg, res) == 0);
    CHECK("found", res->found);
    CHECK("no pilot, mono", !res->pilot && !res->stereo && !res->tone);
    CHECK_NEAR("carrier offset (Hz)", res->carrier_offset_hz, -1500.0, 1.0);
    CHECK_NEAR("peak deviation (Hz)", res->peak_dev_hz, 50000.0, 100.0);
    CHECK_NEAR("MPX power (dBr)", res->mpx_power_dbr, 20.0 * log10(50000.0 / 19000.0), 0.02);
    CHECK("no RDS", !res->rds && res->rds_groups == 0);

    /* The same station 150 kHz off, told by offset_hz; a second station at -250 kHz from the
     * centre, 10 dB stronger, is outside the channel filter. */
    printf("FM mono at +150 kHz, cfg offset, stronger station at -250 kHz:\n");
    {
        float complex *y = malloc(n * sizeof(*y));
        const struct tone t2 = { 3000.0, 60000.0, 0.0 };
        size_t i;

        if (y) {
            make_fm(x, n, FS, 150000.0 + 800.0, -16.0, &t, 1);
            make_fm(y, n, FS, -250000.0, -6.0, &t2, 1);
            for (i = 0; i < n; i++)
                x[i] += y[i];
            free(y);
            acfg.offset_hz = 150000.0;
            CHECK("analyse", m2sdr_fm_analyse(x, n, FS, &acfg, res) == 0);
            CHECK("found", res->found);
            CHECK_NEAR("carrier offset (Hz)", res->carrier_offset_hz, 150800.0, 2.0);
            CHECK_NEAR("carrier (dBFS)", res->carrier_dbfs, -16.0, 0.1);
            CHECK_NEAR("peak deviation (Hz)", res->peak_dev_hz, 50000.0, 250.0);
            CHECK_NEAR("MPX power (dBr)", res->mpx_power_dbr, 20.0 * log10(50000.0 / 19000.0), 0.05);
        }
    }
    free(res);
}

static void test_noise_and_arguments(float complex *x, size_t cap)
{
    struct m2sdr_fm_analysis_config acfg;
    struct m2sdr_fm_result *res = malloc(sizeof(*res));
    const size_t n = (size_t)(0.25 * FS) < cap ? (size_t)(0.25 * FS) : cap;

    printf("Noise only:\n");
    if (!res)
        return;
    memset(x, 0, n * sizeof(*x));
    add_noise(x, n, 1e-3);
    CHECK("analyse", m2sdr_fm_analyse(x, n, FS, NULL, res) == 0);
    CHECK("not found", !res->found && !res->pilot && !res->rds);
    CHECK_NEAR("power (dBFS)", res->power_dbfs, -30.0, 0.1);
    printf("  CNR read %.1f dB\n", res->cnr_db);

    printf("Bad arguments:\n");
    memset(&acfg, 0, sizeof(acfg));
    CHECK("NULL samples", m2sdr_fm_analyse(NULL, n, FS, NULL, res) == -1);
    CHECK("NULL result", m2sdr_fm_analyse(x, n, FS, NULL, NULL) == -1);
    CHECK("rate 200 kHz", m2sdr_fm_analyse(x, n, 200000.0, NULL, res) == -1);
    CHECK("40 ms", m2sdr_fm_analyse(x, (size_t)(0.04 * FS), FS, NULL, res) == -1);
    acfg.offset_hz = 400000.0;
    CHECK("offset out of the capture", m2sdr_fm_analyse(x, n, FS, &acfg, res) == -1);
    acfg.offset_hz = 0.0;
    acfg.tone_hz   = 20000.0;
    CHECK("tone above the audio", m2sdr_fm_analyse(x, n, FS, &acfg, res) == -1);
    acfg.tone_hz = 0.0;
    acfg.tau     = -1.0;
    CHECK("negative tau", m2sdr_fm_analyse(x, n, FS, &acfg, res) == -1);
    CHECK("RDS decoder rate", m2sdr_fm_rds_rx_new(100000.0) == NULL);
    free(res);
}

/* Time of one analysis of 0.5 s (live view). */
static void test_timing(float complex *x, size_t cap)
{
    static const double rates[] = { 960000.0, 1920000.0 };
    const struct tone t[2] = { { 1000.0, 40000.0, 0.0 }, { 19000.0, 7000.0, 0.0 } };
    struct m2sdr_fm_result *res = malloc(sizeof(*res));
    unsigned r, k;

    printf("Timing:\n");
    if (!res)
        return;
    for (r = 0; r < 2; r++) {
        const size_t n = (size_t)(0.5 * rates[r]);
        double t0, best = 1e9;

        if (n > cap)
            continue;
        make_fm(x, n, rates[r], 0.0, -10.0, t, 2);
        for (k = 0; k < 3; k++) {
            t0 = now_s();
            m2sdr_fm_analyse(x, n, rates[r], NULL, res);
            if (now_s() - t0 < best)
                best = now_s() - t0;
        }
        printf("  0.5 s at %.2f MSPS: %.1f ms\n", rates[r] / 1e6, 1e3 * best);
        CHECK("found", res->found && res->pilot);
    }
    free(res);
}

/* Report of m2sdr_analyzer */
/*--------------------------*/

#define FILE_OFFSET_HZ 2500.0
#define FILE_PI        0xC0DE
#define FILE_PS        "M2SDR FM"
#define FILE_RT        "M2SDR test"   /* Three groups 2A: complete in the 1 s of a report. */

/* 1.2 s at 0.96 MSPS of the modulator, stereo with RDS and 1 kHz on the left at half scale,
 * +2.5 kHz from the centre: raw SC16, 12 bits, two channels (the same on both). */
static int write_fm(const char *path)
{
    const size_t max = (size_t)(1.2 * FS);
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_fm_config cfg;
    FILE *f = fopen(path, "wb");
    size_t n, i;

    if (!x || !f)
        return 1;
    m2sdr_fm_config_init(&cfg);
    cfg.sample_rate = FS;
    cfg.tau         = 0.0;
    cfg.stereo      = true;
    cfg.bits        = 16;
    cfg.rds.enable  = true;
    cfg.rds.pi      = FILE_PI;
    cfg.rds.pty     = 5;
    strcpy(cfg.rds.ps, FILE_PS);
    strcpy(cfg.rds.rt, FILE_RT);
    n = make_tx(x, max, &cfg, 1000.0, 0.5);
    if (n == 0)
        return 1;
    for (i = 0; i < n; i++) {
        const float complex v = 0.9f * x[i] * (float complex)cexp(I * 2.0 * M_PI * FILE_OFFSET_HZ * (double)i / FS);
        const short s[4] = { (short)lrintf(crealf(v) * 2047.0f), (short)lrintf(cimagf(v) * 2047.0f),
                             (short)lrintf(crealf(v) * 2047.0f), (short)lrintf(cimagf(v) * 2047.0f) };

        if (fwrite(s, sizeof(s), 1, f) != 1)
            return 1;
    }
    free(x);
    return fclose(f) == 0 ? 0 : 1;
}

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

/* The report of the file of write_fm(), run with --fm --fm-tone 1000. The figures are those of
 * test_tx_stereo_rds(); 12 bits instead of 16 only lower the noise floor. */
static int check_fm(const char *path)
{
    static char json[32768];
    const double tone = 0.5 * 0.86 * 75000.0 / 2.0;
    const double ms   = tone * tone * 0.75 + 7500.0 * 7500.0 / 2.0 + 3000.0 * 3000.0 / 4.0;
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        printf("test_m2sdr_analysis_fm: cannot open %s\n", path);
        return 1;
    }
    n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';
    printf("FM report of m2sdr_analyzer:\n");
    CHECK("station, pilot and RDS", strstr(json, "\"found\": true") && strstr(json, "\"pilot\": true") &&
                                    strstr(json, "\"rds\": true"));
    CHECK_NEAR("carrier offset (Hz)", json_number(json, "fm", "carrier_offset_hz"), FILE_OFFSET_HZ, 1.0);
    CHECK_NEAR("carrier (dBFS)", json_number(json, "fm", "carrier_db"), 20.0 * log10(0.9 * 2047.0 / 2048.0), 0.05);
    CHECK_NEAR("pilot deviation (Hz)", json_number(json, "fm", "pilot_dev_hz"), 7500.0, 20.0);
    CHECK_NEAR("pilot frequency error (Hz)", json_number(json, "fm", "pilot_freq_error_hz"), 0.0, 0.02);
    CHECK_NEAR("MPX power (dBr)", json_number(json, "fm", "mpx_power_dbr"), 10.0 * log10(ms / REF2), 0.05);
    CHECK("separation > 40 dB", json_number(json, "fm", "separation_db") > 40.0);
    CHECK_NEAR("RDS level (Hz)", json_number(json, "fm", "rds_dev_hz"), 3000.0, 60.0);
    CHECK_NEAR("RDS block error rate", json_number(json, "fm", "rds_bler"), 0.0, 0.0);
    CHECK("RDS PI, PS, RT", strstr(json, "\"pi\": \"C0DE\"") && strstr(json, "\"ps\": \"" FILE_PS "\"") &&
                            strstr(json, "\"rt\": \"" FILE_RT "\""));
    printf("test_m2sdr_analysis_fm: FM report %s (%d failures)\n", failures ? "FAIL" : "checked", failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    const size_t cap = (size_t)(2.0 * 1920000.0);
    float complex *x;

    if (argc == 3 && strcmp(argv[1], "--write-fm") == 0)
        return write_fm(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check-fm") == 0)
        return check_fm(argv[2]);
    x = malloc(cap * sizeof(*x));
    if (!x)
        return 1;
    test_tx_stereo_rds(x, cap);
    test_tones_stereo(x, cap, FS);
    test_tones_stereo(x, cap, 1920000.0);
    test_mono(x, cap);
    test_noise_and_arguments(x, cap);
    test_timing(x, cap);
    free(x);
    printf("test_m2sdr_analysis_fm: %s (%d failures)\n", failures ? "FAIL" : "ok", failures);
    return failures ? 1 : 0;
}
