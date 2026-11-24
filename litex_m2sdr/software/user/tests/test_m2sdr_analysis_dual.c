/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the libm2sdr two-channel analyses (coherence, direction finding, LTE 2x2 channel) on
 * synthetic signals of known parameters.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#define TEST_NAME "test_m2sdr_analysis_dual"
#include "test_analysis_helpers.h"

#include "../libm2sdr/m2sdr_analysis_dual.h"
#include "../libm2sdr/m2sdr_fft.h"
#include "../libm2sdr/m2sdr_lte_seq.h"

#define COUNT (1u << 18)

/* Signals */
/*---------*/

/* y = gain * exp(j phase) * (x delayed by tau samples, plus echo * x delayed by tau + echo_tau),
 * circularly, by the FFT of the whole block (count a power of two). */
static int delay_copy(const float complex *x, float complex *y, size_t count, double gain_db, double phase_deg,
                      double tau, double echo, double echo_tau)
{
    struct m2sdr_fft *fwd = m2sdr_fft_alloc(count, false), *inv = m2sdr_fft_alloc(count, true);
    float complex *t = malloc(count * sizeof(*t));
    const double g = pow(10.0, gain_db / 20.0);
    size_t k;

    if (!fwd || !inv || !t) {
        m2sdr_fft_free(fwd);
        m2sdr_fft_free(inv);
        free(t);
        return -1;
    }
    m2sdr_fft_run(fwd, x, t);
    for (k = 0; k < count; k++) {
        const double f = k < count / 2 ? (double)k : (double)k - (double)count;
        const double complex h = g * cexp(I * phase_deg * M_PI / 180.0) * cexp(-2.0 * M_PI * I * f * tau / count) *
                                 (1.0 + echo * cexp(-2.0 * M_PI * I * f * echo_tau / count));
        t[k] *= (float complex)(h / (double)count);
    }
    m2sdr_fft_run(inv, t, y);
    m2sdr_fft_free(fwd);
    m2sdr_fft_free(inv);
    free(t);
    return 0;
}

static double norm2(double complex z)
{
    return creal(z) * creal(z) + cimag(z) * cimag(z);
}

/* Coherence */
/*-----------*/

static void test_coherence(void)
{
    const double fs = 10e6;
    float complex *s = malloc(COUNT * sizeof(*s)), *x1 = malloc(COUNT * sizeof(*x1)), *x2 = malloc(COUNT * sizeof(*x2));
    struct m2sdr_coherence_result *res = malloc(sizeof(*res));
    struct m2sdr_coherence_config cfg = { 1024, 0, 0, 0 };
    double t0;
    int k;

    if (!s || !x1 || !x2 || !res) {
        failures++;
        goto out;
    }

    /* White noise of -20 dBFS on both, RX2 1.5 dB lower, 37 degrees ahead and 2.3 samples (230 ns)
     * behind; each receiver adds its own noise 20 dB below the signal it gets. */
    printf("Coherence, wideband source, gain -1.5 dB, phase 37 deg, delay 2.3 samples:\n");
    memset(s, 0, COUNT * sizeof(*s));
    add_noise(s, COUNT, 0.01);
    if (delay_copy(s, x2, COUNT, -1.5, 37.0, 2.3, 0.0, 0.0) != 0) {
        failures++;
        goto out;
    }
    memcpy(x1, s, COUNT * sizeof(*s));
    add_noise(x1, COUNT, 1e-4);
    add_noise(x2, COUNT, 1e-4 * pow(10.0, -0.15));
    t0 = now_ms();
    CHECK("analyse", m2sdr_coherence_analyse(x1, x2, COUNT, fs, &cfg, res) == 0);
    printf("  time: %.1f ms for %u samples\n", now_ms() - t0, COUNT);
    CHECK("found", res->found);
    /* All but DC and its neighbours, and the bin at -fs/2: it straddles the wrap of the
     * spectrum, where a fractional delay turns its two halves apart. */
    CHECK("all bins coherent", res->coherent_bins == res->band_bins - 4);
    /* Coherence of two receivers of SNR r1, r2: 1 / ((1 + 1/r1) (1 + 1/r2)) = 0.9803 at 20 dB.
     * A delay of tau samples shifts the segments of one channel against the other: under a Hann
     * window the loss goes with (tau / N)^2, 1e-5 here. The estimate is biased by (1 - C)^2 / K,
     * nothing either, and its spread over 1020 bins is small. */
    CHECK_NEAR("coherence", res->coherence_used, 0.9803, 0.002);
    /* Gain: the noise is 20 dB below the signal on both, so sqrt(S22 / S11) is the gain; over
     * 2^18 samples its spread is about 4.34 / sqrt(2^18) dB. */
    CHECK_NEAR("gain (dB)", res->gain_db, -1.5, 0.02);
    /* Phase of a bin: standard deviation sqrt((1 - C) / (2 K C)) = 0.0045 rad (K = 511); the
     * line through 1021 bins has 1/32 of it at its centre, the slope even less. */
    CHECK_NEAR("phase (deg)", res->phase_deg, 37.0, 0.05);
    CHECK("delay found", res->delay_valid);
    CHECK_NEAR("delay (samples)", res->delay_samples, 2.3, 0.002);
    CHECK_NEAR("delay (ns)", res->delay_s * 1e9, 230.0, 0.2);
    CHECK_NEAR("phase ripple (deg)", res->phase_ripple_deg, 0.0045 * 180.0 / M_PI, 0.1);
    /* Gain of a bin: the ratio of two spectra of K segments, each within about 1 / sqrt(K), the
     * common signal cancelling most of it: well under 0.2 dB RMS. */
    CHECK("gain ripple (dB)", res->gain_ripple_db < 0.2);
    CHECK_NEAR("RX1 power (dBFS)", res->rx1_db, 10.0 * log10(0.0101), 0.05);
    CHECK_NEAR("RX2 power (dBFS)", res->rx2_db, 10.0 * log10(0.0101) - 1.5, 0.05);
    CHECK_NEAR("bin phase at +1 MHz (deg)", res->phase_bin_deg[512 + 102],
               remainder(37.0 - 360.0 * 102 * 2.3 / 1024.0, 360.0), 1.0);

    /* A band only, and a delay of 40 samples: beyond the coarse step. */
    printf("Coherence, band -2..3 MHz, delay -40.7 samples:\n");
    if (delay_copy(s, x2, COUNT, 3.0, -120.0, -40.7, 0.0, 0.0) != 0) {
        failures++;
        goto out;
    }
    add_noise(x2, COUNT, 1e-4 * pow(10.0, 0.3));
    cfg.lo_hz = -2e6;
    cfg.hi_hz = 3e6;
    CHECK("analyse", m2sdr_coherence_analyse(x1, x2, COUNT, fs, &cfg, res) == 0);
    CHECK("found", res->found);
    CHECK_NEAR("bins of the band", res->band_bins, 513, 1);
    CHECK_NEAR("gain (dB)", res->gain_db, 3.0, 0.03);
    CHECK_NEAR("phase (deg)", res->phase_deg, -120.0, 0.2);
    CHECK_NEAR("delay (samples)", res->delay_samples, -40.7, 0.01);
    cfg.lo_hz = cfg.hi_hz = 0;

    /* An echo on RX2: 0.3 of the signal 5 samples later, a ripple of the gain over frequency
     * between 20 log10(0.7) and 20 log10(1.3). */
    printf("Coherence, echo of -10.5 dB, 5 samples, on RX2:\n");
    if (delay_copy(s, x2, COUNT, 0.0, 0.0, 0.0, 0.3, 5.0) != 0) {
        failures++;
        goto out;
    }
    add_noise(x2, COUNT, 1e-4);
    CHECK("analyse", m2sdr_coherence_analyse(x1, x2, COUNT, fs, &cfg, res) == 0);
    {
        double worst = 0, rms = 0, mean = 0;
        int n = 0;

        for (k = 0; k < res->bins; k++) {
            const double f = k - res->bins / 2;
            const double e = 20.0 * log10(cabs(1.0 + 0.3 * cexp(-2.0 * M_PI * I * f * 5.0 / res->bins)));

            if (abs(k - res->bins / 2) <= 1)
                continue;
            /* Per bin, the ratio of spectra of 511 segments: within 0.3 dB, the noise of each
             * receiver differing with the gain. */
            if (fabs(res->gain_bin_db[k] - e) > worst)
                worst = fabs(res->gain_bin_db[k] - e);
            mean += e;
            n++;
        }
        mean /= n;
        for (k = 0; k < res->bins; k++) {
            const double f = k - res->bins / 2;
            const double e = 20.0 * log10(cabs(1.0 + 0.3 * cexp(-2.0 * M_PI * I * f * 5.0 / res->bins)));
            if (abs(k - res->bins / 2) > 1)
                rms += (e - res->gain_db) * (e - res->gain_db);
        }
        rms = sqrt(rms / n);
        CHECK_NEAR("gain over frequency, worst (dB)", worst, 0.0, 0.3);
        CHECK_NEAR("gain ripple (dB)", res->gain_ripple_db, rms, 0.1);
        /* sqrt(sum S22 / sum S11) is the RMS of the response: 10 log10(1 + 0.3^2) = 0.374 dB. */
        CHECK_NEAR("gain (dB)", res->gain_db, 10.0 * log10(1.09), 0.03);
    }

    /* A tone: gain and phase, no delay. */
    printf("Coherence, one tone at 1.234 MHz:\n");
    memset(x1, 0, COUNT * sizeof(*x1));
    memset(x2, 0, COUNT * sizeof(*x2));
    add_tone(x1, COUNT, 1.234e6, fs, 0.1, 0.0);
    add_tone(x2, COUNT, 1.234e6, fs, 0.05, -1.0);
    add_noise(x1, COUNT, 1e-6);
    add_noise(x2, COUNT, 1e-6);
    CHECK("analyse", m2sdr_coherence_analyse(x1, x2, COUNT, fs, &cfg, res) == 0);
    CHECK("found", res->found);
    CHECK("no delay from one tone", !res->delay_valid);
    CHECK_NEAR("gain (dB)", res->gain_db, 20.0 * log10(0.5), 0.01);
    CHECK_NEAR("phase (deg)", res->phase_deg, -180.0 / M_PI, 0.05);

    /* Noise alone, independent on both: coherence about 1/K (K = 511 segments, half
     * overlapped: a little more), no coherent bin. */
    printf("Coherence, independent noise:\n");
    memset(x1, 0, COUNT * sizeof(*x1));
    memset(x2, 0, COUNT * sizeof(*x2));
    add_noise(x1, COUNT, 0.01);
    add_noise(x2, COUNT, 0.01);
    CHECK("analyse", m2sdr_coherence_analyse(x1, x2, COUNT, fs, &cfg, res) == 0);
    CHECK("not found", !res->found && res->coherent_bins == 0);
    CHECK("coherence about 1/K", res->coherence > 0.5 / res->segments && res->coherence < 2.0 / res->segments);

    printf("Coherence, bad arguments:\n");
    cfg.fft_len = 1000;
    CHECK("length not a power of two", m2sdr_coherence_analyse(x1, x2, COUNT, fs, &cfg, res) != 0);
    cfg.fft_len = 1024;
    CHECK("fewer than eight segments", m2sdr_coherence_analyse(x1, x2, 4096, fs, &cfg, res) != 0);
    cfg.lo_hz = 1e6;
    cfg.hi_hz = -1e6;
    CHECK("empty band", m2sdr_coherence_analyse(x1, x2, COUNT, fs, &cfg, res) != 0);
    CHECK("no second channel", m2sdr_coherence_analyse(x1, NULL, COUNT, fs, NULL, res) != 0);

out:
    free(s);
    free(x1);
    free(x2);
    free(res);
}

/* Direction finding */
/*-------------------*/

#define DF_FS   1e6
#define DF_RF   2.4e9
#define DF_C    299792458.0

/* A plane wave from angle_deg (positive towards RX2) at freq_hz from the centre: RX2 ahead of
 * RX1 by 2 pi d sin(angle) / lambda, plus the phase of the board. */
static void add_wave(float complex *x1, float complex *x2, size_t count, double freq_hz, double amp, double angle_deg,
                     double spacing_m, double board_deg)
{
    const double lambda = DF_C / (DF_RF + freq_hz);
    const double phi = 2.0 * M_PI * spacing_m / lambda * sin(angle_deg * M_PI / 180.0) + board_deg * M_PI / 180.0;
    const double phase = 2.0 * M_PI * rng_uniform();

    add_tone(x1, count, freq_hz, DF_FS, amp, phase);
    add_tone(x2, count, freq_hz, DF_FS, amp, phase + phi);
}

static void test_df(void)
{
    const double lambda = DF_C / (DF_RF + 150e3);
    float complex *x1 = malloc(COUNT * sizeof(*x1)), *x2 = malloc(COUNT * sizeof(*x2));
    struct m2sdr_df_result *res = malloc(sizeof(*res));
    struct m2sdr_df_config cfg;
    double t0;
    int i, at;

    if (!x1 || !x2 || !res) {
        failures++;
        goto out;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.spacing_m     = lambda / 2;
    cfg.rf_freq_hz    = DF_RF;
    cfg.cal_phase_deg = 40.0;

    /* A tone of -20 dBFS from 30 degrees, noise of -40 dBFS on each receiver: 54 dB of SNR in a
     * bin of 4096 (1.5 bins of noise bandwidth). The phase is then within a few thousandths of
     * a degree: the tolerances are those of the arithmetic. */
    printf("Direction finding, lambda/2, 30 deg, board phase 40 deg:\n");
    memset(x1, 0, COUNT * sizeof(*x1));
    memset(x2, 0, COUNT * sizeof(*x2));
    add_wave(x1, x2, COUNT, 150e3, 0.1, 30.0, cfg.spacing_m, 40.0);
    add_noise(x1, COUNT, 1e-4);
    add_noise(x2, COUNT, 1e-4);
    t0 = now_ms();
    CHECK("analyse", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) == 0);
    printf("  time: %.1f ms for %u samples\n", now_ms() - t0, COUNT);
    CHECK("found", res->found);
    CHECK_NEAR("frequency (Hz)", res->freq_hz, 150e3, DF_FS / 4096);
    CHECK_NEAR("level (dBFS)", res->level_db, -20.0, 1.5);   /* Off the bin: the loss of the Hann window, up to 1.4 dB. */
    CHECK("SNR above 45 dB", res->snr_db > 45.0);
    CHECK("coherence", res->coherence > 0.999);
    CHECK_NEAR("raw phase (deg)", res->phase_raw_deg, 90.0 + 40.0, 0.05);
    CHECK_NEAR("phase (deg)", res->phase_deg, 90.0, 0.05);
    CHECK("one candidate", res->candidates == 1 && !res->ambiguous);
    CHECK_NEAR("angle (deg)", res->angle_deg, 30.0, 0.05);
    CHECK("angle deviation small", res->angle_std_deg < 0.05);
    CHECK_NEAR("MUSIC peak (deg)", res->music_peak_deg, 30.0, 0.05);
    for (i = 0, at = 0; i < M2SDR_DF_POINTS; i++) {
        if (res->beamscan_db[i] > res->beamscan_db[at])
            at = i;
    }
    CHECK_NEAR("beamscan peak (deg)", at - 90, 30.0, 1.0);
    CHECK_NEAR("MUSIC at the peak (dB)", res->music_db[120], 0.0, 0.5);
    CHECK("MUSIC deep away from it", res->music_db[90] < -20.0 && res->music_db[60] < -20.0);
    /* Beamscan of two elements: 1 + cos(psi - phi), down 3 dB where the phases differ by 90
     * degrees: sin(angle) = 0.5 - 0.5, at 0 degrees. */
    CHECK_NEAR("beamscan at broadside (dB)", res->beamscan_db[90], -3.01, 0.05);

    /* Calibration: the source at broadside, then at a known 20 degrees. */
    printf("Direction finding, calibration:\n");
    memset(x1, 0, COUNT * sizeof(*x1));
    memset(x2, 0, COUNT * sizeof(*x2));
    add_wave(x1, x2, COUNT, 150e3, 0.1, 0.0, cfg.spacing_m, -155.0);
    add_noise(x1, COUNT, 1e-4);
    add_noise(x2, COUNT, 1e-4);
    CHECK("analyse", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) == 0);
    CHECK_NEAR("offset from broadside (deg)", m2sdr_df_calibration(res, 0.0), -155.0, 0.05);
    memset(x1, 0, COUNT * sizeof(*x1));
    memset(x2, 0, COUNT * sizeof(*x2));
    add_wave(x1, x2, COUNT, 150e3, 0.1, 20.0, cfg.spacing_m, -155.0);
    add_noise(x1, COUNT, 1e-4);
    add_noise(x2, COUNT, 1e-4);
    CHECK("analyse", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) == 0);
    CHECK_NEAR("offset from 20 deg (deg)", m2sdr_df_calibration(res, 20.0), -155.0, 0.05);
    cfg.cal_phase_deg = m2sdr_df_calibration(res, 20.0);
    CHECK("analyse calibrated", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) == 0);
    CHECK_NEAR("angle (deg)", res->angle_deg, 20.0, 0.05);

    /* 1.5 lambda: phi + 2 pi m = 3 pi sin(angle), three angles for one phase. */
    printf("Direction finding, 1.5 lambda, -20 deg:\n");
    cfg.spacing_m     = 1.5 * lambda;
    cfg.cal_phase_deg = 0.0;
    memset(x1, 0, COUNT * sizeof(*x1));
    memset(x2, 0, COUNT * sizeof(*x2));
    add_wave(x1, x2, COUNT, 150e3, 0.1, -20.0, cfg.spacing_m, 0.0);
    add_noise(x1, COUNT, 1e-4);
    add_noise(x2, COUNT, 1e-4);
    CHECK("analyse", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) == 0);
    CHECK("ambiguous", res->ambiguous);
    {
        /* 3 pi sin(-20) = -3.224 rad: -0.0823 wrapped, then -0.0823 + 2 pi m over 3 pi. */
        const double phi = remainder(3.0 * M_PI * sin(-20.0 * M_PI / 180.0), 2.0 * M_PI);
        int m, expected = 0, hit = 0;

        for (m = -3; m <= 3; m++)
            expected += fabs((phi + 2.0 * M_PI * m) / (3.0 * M_PI)) <= 1.0;
        for (i = 0; i < res->candidates; i++)
            hit += fabs(res->candidate_deg[i] - -20.0) < 0.05;
        CHECK_NEAR("candidates", res->candidates, expected, 0);
        CHECK("the true angle among them", hit == 1);
        CHECK_NEAR("nearest broadside (deg)", res->angle_deg, asin(phi / (3.0 * M_PI)) * 180.0 / M_PI, 0.05);
        CHECK("candidates in order", res->candidates == 3 && res->candidate_deg[0] < res->candidate_deg[1] &&
              res->candidate_deg[1] < res->candidate_deg[2]);
    }

    /* Two sources, the weaker one asked for by its frequency. */
    printf("Direction finding, two sources, the one at -210 kHz:\n");
    cfg.spacing_m = lambda / 2;
    memset(x1, 0, COUNT * sizeof(*x1));
    memset(x2, 0, COUNT * sizeof(*x2));
    add_wave(x1, x2, COUNT, 150e3, 0.1, 30.0, cfg.spacing_m, 0.0);
    add_wave(x1, x2, COUNT, -210e3, 0.02, -50.0, cfg.spacing_m, 0.0);
    add_noise(x1, COUNT, 1e-4);
    add_noise(x2, COUNT, 1e-4);
    CHECK("analyse", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) == 0);
    CHECK_NEAR("strongest (deg)", res->angle_deg, 30.0, 0.05);
    cfg.freq_hz = -210e3;
    CHECK("analyse", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) == 0);
    /* The wavelength of 2.4 GHz - 210 kHz differs from the one of 150 kHz by 1.5e-4: the
     * spacing in wavelengths moves that little. */
    CHECK_NEAR("chosen (deg)", res->angle_deg, -50.0, 0.05);
    cfg.freq_hz = 0;

    /* A weak source: 10 dB of SNR per sample at a 256 point FFT, 1023 segments. The phase of
     * the tone spreads by sqrt((1 - C) / (2 K C)); the error stays within four of it. */
    printf("Direction finding, weak source:\n");
    cfg.fft_len = 256;
    memset(x1, 0, COUNT * sizeof(*x1));
    memset(x2, 0, COUNT * sizeof(*x2));
    add_wave(x1, x2, COUNT, 150e3, 0.02, -35.0, cfg.spacing_m, 0.0);
    add_noise(x1, COUNT, 1e-3);
    add_noise(x2, COUNT, 1e-3);
    CHECK("analyse", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) == 0);
    CHECK("found", res->found);
    printf("  angle %.3f deg, deviation %.3f deg, coherence %.4f\n", res->angle_deg, res->angle_std_deg, res->coherence);
    CHECK("angle within four deviations", fabs(res->angle_deg - -35.0) < 4.0 * res->angle_std_deg);
    CHECK("deviation below a degree", res->angle_std_deg > 0.0 && res->angle_std_deg < 1.0);
    cfg.fft_len = 0;

    printf("Direction finding, noise only:\n");
    memset(x1, 0, COUNT * sizeof(*x1));
    memset(x2, 0, COUNT * sizeof(*x2));
    add_noise(x1, COUNT, 1e-4);
    add_noise(x2, COUNT, 1e-4);
    CHECK("analyse", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) == 0);
    CHECK("not found", !res->found);

    printf("Direction finding, bad arguments:\n");
    cfg.spacing_m = 0;
    CHECK("no spacing", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) != 0);
    cfg.spacing_m  = lambda / 2;
    cfg.rf_freq_hz = 0;
    CHECK("no frequency", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) != 0);
    cfg.rf_freq_hz = DF_RF;
    CHECK("no config", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, NULL, res) != 0);
    CHECK("too short", m2sdr_df_analyse(x1, x2, 8192, DF_FS, &cfg, res) != 0);
    cfg.freq_hz = 600e3;
    CHECK("frequency out of the capture", m2sdr_df_analyse(x1, x2, COUNT, DF_FS, &cfg, res) != 0);

out:
    free(x1);
    free(x2);
    free(res);
}

/* LTE 2x2 channel */
/*-----------------*/

#define CELL_FRAMES 5           /* At most: the captures are shorter but the one of a file. */

/* H[rx][port] at subcarrier sub (from the centre, 15 kHz apart): a flat part and an echo
 * delay_s later. */
struct mimo_channel {
    double complex h[2][2];
    double complex echo[2][2];
    double delay_s;
};

static double complex channel_at(const struct mimo_channel *ch, int r, int p, int sub)
{
    return ch->h[r][p] + ch->echo[r][p] * cexp(-2.0 * M_PI * I * sub * 15000.0 * ch->delay_s);
}

struct mimo_cell {
    int    cell_id, n_rb, sfn;
    int    rate_index;          /* Sample rate: 1.92 MSPS << rate_index; the subcarriers beyond it are cut. */
    int    ports;               /* 1 or 2. */
    double cfo_hz;
    double evm;                 /* Noise of each receiver, as the EVM it gives on a resource element. */
    size_t delay;               /* Samples ahead of the first frame. */
    double amp;                 /* Of a resource element. */
};

/* Frames of a cell on two receivers: synchronisation signals on port 0, reference signals of
 * each port, PBCH (transmit diversity with two ports), and QPSK data on every other element,
 * a layer per port. Each port goes through its column of the channel. Returns the number of
 * samples written on each. */
static size_t make_cell(float complex *x[2], size_t max, const struct mimo_cell *cell, const struct mimo_channel *ch)
{
    static float complex qpsk[4], crs[2 * LTE_MAX_RB], pss[LTE_SYNC_LEN];
    static float sss[LTE_SYNC_LEN];
    static uint8_t bch[LTE_PBCH_BITS];
    const int d = 1 << cell->rate_index, n = 128 * d, k = 12 * cell->n_rb;
    const double fs = 1920000.0 * d;
    const bool two = cell->ports == 2;
    const float r = two ? (float)M_SQRT1_2 : 1.0f;
    float complex *g[2] = { calloc((size_t)k, sizeof(float complex)), calloc((size_t)k, sizeof(float complex)) };
    float complex *fd = malloc((size_t)n * sizeof(*fd)), *td = malloc((size_t)n * sizeof(*td));
    struct m2sdr_fft *fft = m2sdr_fft_alloc((size_t)n, true);
    uint8_t mib[LTE_MIB_BITS];
    size_t pos = cell->delay, i;
    int frame, ns, l, a, j, rx, p, pbch_at;

    if (!g[0] || !g[1] || !fd || !td || !fft) {
        failures++;
        return 0;
    }
    m2sdr_constellation(M2SDR_MOD_QPSK, qpsk);
    m2sdr_lte_pss(cell->cell_id % 3, pss);
    for (rx = 0; rx < 2; rx++)
        memset(x[rx], 0, max * sizeof(*x[rx]));

    for (frame = 0; frame < CELL_FRAMES; frame++) {
        const int sfn = cell->sfn + frame;
        const int bw = cell->n_rb == 6 ? 0 : cell->n_rb == 15 ? 1 : cell->n_rb == 25 ? 2 : cell->n_rb == 50 ? 3 : 5;

        memset(mib, 0, sizeof(mib));
        mib[0] = (uint8_t)((bw >> 2) & 1);
        mib[1] = (uint8_t)((bw >> 1) & 1);
        mib[2] = (uint8_t)(bw & 1);
        mib[4] = 1;
        for (j = 0; j < 8; j++)
            mib[6 + j] = (uint8_t)(((sfn >> 2) >> (7 - j)) & 1);
        m2sdr_lte_bch_encode(mib, two ? 2 : 1, cell->cell_id, bch);
        pbch_at = (sfn & 3) * LTE_PBCH_FRAME;

        for (ns = 0; ns < 20; ns++) {
            for (l = 0; l < 7; l++) {
                const int cp = (l ? 9 : 10) * d;
                const bool centre_sym = (ns == 0 || ns == 10) && (l == 5 || l == 6);
                float complex x0 = 0, x1 = 0;

                memset(g[0], 0, (size_t)k * sizeof(*g[0]));
                memset(g[1], 0, (size_t)k * sizeof(*g[1]));
                if (centre_sym && l == 5)
                    m2sdr_lte_sss(cell->cell_id / 3, cell->cell_id % 3, ns == 10, sss);
                if (l == 0 || l == 4)
                    m2sdr_lte_crs(cell->cell_id, ns, l, crs);
                for (a = 0; a < k; a++) {
                    const bool centre = a >= k / 2 - 36 && a < k / 2 + 36;

                    if ((l == 0 || l == 4) && (a % 6) == ((l ? 3 : 0) + cell->cell_id) % 6) {
                        g[0][a] = crs[a / 6 - cell->n_rb + LTE_MAX_RB];
                    } else if (two && (l == 0 || l == 4) && (a % 6) == ((l ? 0 : 3) + cell->cell_id) % 6) {
                        g[1][a] = crs[a / 6 - cell->n_rb + LTE_MAX_RB];
                    } else if (centre && centre_sym) {
                        const int at = a - (k / 2 - 31);
                        if (at >= 0 && at < LTE_SYNC_LEN)
                            g[0][a] = l == 6 ? pss[at] : sss[at];
                    } else if (centre && ns == 1 && l < 4) {
                        if (l < 2 && (a % 3) == (cell->cell_id % 3))
                            continue;
                        if (!two) {
                            g[0][a] = (float)M_SQRT1_2 * ((1 - 2 * bch[pbch_at]) + I * (1 - 2 * bch[pbch_at + 1]));
                        } else if ((pbch_at & 2) == 0) {
                            x0 = (float)M_SQRT1_2 * ((1 - 2 * bch[pbch_at]) + I * (1 - 2 * bch[pbch_at + 1]));
                            x1 = (float)M_SQRT1_2 * ((1 - 2 * bch[pbch_at + 2]) + I * (1 - 2 * bch[pbch_at + 3]));
                            g[0][a] = r * x0;
                            g[1][a] = -r * conjf(x1);
                        } else {
                            g[0][a] = r * x1;
                            g[1][a] = r * conjf(x0);
                        }
                        pbch_at += 2;
                    } else if ((ns & 1) == 0 && l < 3) {
                        continue;                           /* Control region: left empty. */
                    } else if ((a & 1) == 0) {
                        g[0][a] = r * qpsk[(int)(rng_uniform() * 4) % 4];
                        if (two)
                            g[1][a] = r * qpsk[(int)(rng_uniform() * 4) % 4];
                    }
                }
                for (rx = 0; rx < 2; rx++) {
                    memset(fd, 0, (size_t)n * sizeof(*fd));
                    for (a = 0; a < k; a++) {
                        const int sub = a < k / 2 ? a - k / 2 : a - k / 2 + 1;
                        double complex v = 0;

                        if (sub <= -n / 2 || sub >= n / 2)
                            continue;                       /* Beyond the band of the capture. */
                        for (p = 0; p < (two ? 2 : 1); p++)
                            v += channel_at(ch, rx, p, sub) * g[p][a];
                        fd[(sub + n) % n] = (float complex)v;
                    }
                    m2sdr_fft_run(fft, fd, td);
                    if (pos + (size_t)(cp + n) > max)
                        goto done;
                    for (j = 0; j < cp; j++)
                        x[rx][pos + (size_t)j] = (float)cell->amp * td[n - cp + j];
                    for (j = 0; j < n; j++)
                        x[rx][pos + (size_t)(cp + j)] = (float)cell->amp * td[j];
                }
                pos += (size_t)(cp + n);
            }
        }
    }
done:
    /* One LO for both receivers: the same carrier offset; their own noise. An element is amp
     * after the FFT of the analysis: noise of evm * amp per element is evm^2 * amp^2 * n per
     * sample. */
    for (rx = 0; rx < 2; rx++) {
        for (i = 0; i < pos; i++)
            x[rx][i] *= (float complex)cexp(2.0 * M_PI * I * cell->cfo_hz * (double)i / fs);
        add_noise(x[rx], pos, cell->evm * cell->evm * cell->amp * cell->amp * n);
    }
    m2sdr_fft_free(fft);
    free(g[0]);
    free(g[1]);
    free(fd);
    free(td);
    return pos;
}

/* What the analysis should read for block b of the cell: the mean of the channel over the
 * pilots of each port in the block (two subcarriers in each of symbols 0 and 4). */
static void expected_block(const struct mimo_channel *ch, const struct mimo_cell *cell, int b, float complex h[2][2])
{
    const int k = 12 * cell->n_rb;
    int r, p, li, j;

    for (r = 0; r < 2; r++) {
        for (p = 0; p < 2; p++) {
            double complex sum = 0;

            for (li = 0; li < 2; li++) {
                const int s = (((li ^ p) ? 3 : 0) + cell->cell_id) % 6;
                for (j = 0; j < 2; j++) {
                    const int a = 12 * b + s + 6 * j;
                    const int sub = a < k / 2 ? a - k / 2 : a - k / 2 + 1;
                    sum += channel_at(ch, r, p, sub);
                }
            }
            h[r][p] = (float complex)(sum / 4.0 * cell->amp);
        }
    }
}

static void test_mimo(void)
{
    const size_t max = (size_t)(0.032 * 1920000.0 * 8);
    float complex *x[2] = { malloc(max * sizeof(float complex)), malloc(max * sizeof(float complex)) };
    struct m2sdr_mimo_result *res = malloc(sizeof(*res));
    double t0;
    int b;

    if (!x[0] || !x[1] || !res) {
        failures++;
        goto out;
    }

    /* A well-conditioned channel, flat: singular values and condition number from H. */
    {
        static const struct mimo_cell cell = { 388, 25, 100, 2, 2, 812.0, 0.02, 3001, 0.02 };
        struct mimo_channel ch;
        float complex h[2][2];
        double s[2], snr = 0;
        const double fs = 1920000.0 * 4;
        size_t count;
        int r, p;

        memset(&ch, 0, sizeof(ch));
        ch.h[0][0] = 1.0;
        ch.h[0][1] = 0.4 * cexp(I * 1.1);
        ch.h[1][0] = 0.3 * cexp(-I * 2.0);
        ch.h[1][1] = 0.8 * cexp(I * 0.5);
        printf("LTE 2x2, cell 388, 25 blocks, flat channel:\n");
        count = make_cell(x, (size_t)(0.032 * fs), &cell, &ch);
        t0 = now_ms();
        CHECK("analyse", m2sdr_mimo_analyse(x[0], x[1], count, fs, res) == 0);
        printf("  time: %.1f ms for %zu samples\n", now_ms() - t0, count);
        CHECK("found", res->found && res->rx == 0);
        CHECK_NEAR("cell identity", res->cell_id, 388, 0);
        CHECK("two ports, MIB", res->ports == 2 && res->mimo);
        CHECK_NEAR("blocks", res->rb, 25, 0);
        CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, 812.0, 5.0);
        for (r = 0; r < 2; r++) {
            for (p = 0; p < 2; p++) {
                char name[40];

                snprintf(name, sizeof(name), "path RX%d port %d (dBFS)", r + 1, p);
                /* 80 pilots per block and 25 blocks: the noise of the mean is far below 0.1 dB. */
                CHECK_NEAR(name, res->path_db[r][p], 20.0 * log10(cabs(ch.h[r][p]) * cell.amp), 0.1);
                snr += cabs(ch.h[r][p]) * cabs(ch.h[r][p]) / 4.0;
            }
        }
        expected_block(&ch, &cell, 0, h);
        m2sdr_svd2((const float complex (*)[2])h, s);
        CHECK_NEAR("s1^2 (dB)", res->sv_db[0], 20.0 * log10(s[0]), 0.1);
        CHECK_NEAR("s2^2 (dB)", res->sv_db[1], 20.0 * log10(s[1]), 0.1);
        CHECK_NEAR("condition number (dB)", res->cond_db, 20.0 * log10(s[0] / s[1]), 0.1);
        CHECK("condition the same on all blocks", res->cond_max_db - res->cond_db < 0.3);
        /* The noise of the pilots: each receiver adds 0.02 of an element, so the SNR of a pilot
         * is the mean |H|^2 over 0.02^2; it is estimated from 19 x 100 degrees of freedom. */
        CHECK_NEAR("pilot SNR (dB)", res->snr_db, 10.0 * log10(snr / (0.02 * 0.02)), 0.5);
        CHECK("two layers", res->rank == 2 && res->rank2_rb == 25);
        CHECK("capacity of two layers higher", res->capacity[1] > res->capacity[0]);
        CHECK_NEAR("receive correlation", res->correlation_rx,
                   cabs(ch.h[0][0] * conj(ch.h[1][0]) + ch.h[0][1] * conj(ch.h[1][1])) /
                   sqrt((norm2(ch.h[0][0]) + norm2(ch.h[0][1])) * (norm2(ch.h[1][0]) + norm2(ch.h[1][1]))), 0.01);
        /* H, its phase relative: H[0][1] / H[0][0] and H[1][1] / H[0][0] carry the geometry. */
        CHECK_NEAR("H01 / H00 (deg)", carg(res->h[10][0][1] / res->h[10][0][0]) * 180.0 / M_PI, 1.1 * 180.0 / M_PI, 1.0);
        CHECK_NEAR("H11 / H00 (deg)", carg(res->h[10][1][1] / res->h[10][0][0]) * 180.0 / M_PI, 0.5 * 180.0 / M_PI, 1.0);
    }

    /* A keyhole: H = u v^T, of rank one. */
    {
        static const struct mimo_cell cell = { 77, 15, 900, 1, 2, -3000.0, 0.02, 777, 0.02 };
        struct mimo_channel ch;
        const double fs = 1920000.0 * 2;
        size_t count;

        memset(&ch, 0, sizeof(ch));
        ch.h[0][0] = 1.0;
        ch.h[0][1] = 0.7 * cexp(I * 0.3);
        ch.h[1][0] = 0.6 * cexp(I * 2.0);
        ch.h[1][1] = 0.6 * 0.7 * cexp(I * 2.3);
        printf("LTE 2x2, cell 77, 15 blocks, channel of rank one:\n");
        count = make_cell(x, (size_t)(0.032 * fs), &cell, &ch);
        CHECK("analyse", m2sdr_mimo_analyse(x[0], x[1], count, fs, res) == 0);
        CHECK("found, two ports", res->found && res->mimo);
        /* s2 is then the noise of the estimate of H, the mean of 80 pilots: about 0.02 / sqrt(80)
         * of an element against s1 of about 1.4: some 35 dB, the noise varying from block to
         * block. */
        CHECK("condition above 25 dB", res->cond_db > 25.0);
        CHECK("one layer", res->rank == 1 && res->rank2_rb == 0);
        CHECK("receive correlation 1", res->correlation_rx > 0.99);
        CHECK("transmit correlation 1", res->correlation_tx > 0.99);
    }

    /* A frequency-selective channel: an echo of 2 us on two of the paths; the condition number
     * changes over the blocks as the echo turns. */
    {
        static const struct mimo_cell cell = { 141, 25, 5, 2, 2, 150.0, 0.01, 12345, 0.02 };
        struct mimo_channel ch;
        const double fs = 1920000.0 * 4;
        double worst = 0, sv0 = 0, cond = 0, cond_max = 0;
        size_t count;

        memset(&ch, 0, sizeof(ch));
        ch.h[0][0]    = 0.9;
        ch.h[0][1]    = 0.5 * cexp(I * 0.4);
        ch.h[1][0]    = 0.5 * cexp(-I * 1.0);
        ch.h[1][1]    = 0.9 * cexp(I * 2.0);
        ch.echo[0][1] = 0.4;
        ch.echo[1][1] = 0.7 * cexp(I * (2.0 + M_PI));
        ch.delay_s    = 2e-6;
        printf("LTE 2x2, cell 141, 25 blocks, echo of 2 us:\n");
        count = make_cell(x, (size_t)(0.032 * fs), &cell, &ch);
        CHECK("analyse", m2sdr_mimo_analyse(x[0], x[1], count, fs, res) == 0);
        CHECK("found, two ports", res->found && res->mimo && res->rb == 25);
        for (b = 0; b < res->rb; b++) {
            float complex h[2][2];
            double s[2], e;

            expected_block(&ch, &cell, b, h);
            m2sdr_svd2((const float complex (*)[2])h, s);
            e = fabs(res->cond_rb_db[b] - 20.0 * log10(s[0] / s[1]));
            if (e > worst)
                worst = e;
            sv0  += s[0] * s[0] / res->rb;
            cond += 20.0 * log10(s[0] / s[1]) / res->rb;
            if (20.0 * log10(s[0] / s[1]) > cond_max)
                cond_max = 20.0 * log10(s[0] / s[1]);
        }
        /* Per block, H from 80 pilots at an SNR of 40 dB and more. The frame found starts a
         * sample early (the synchronisation is at 1.92 MSPS): the phase turns by 2 pi / 512 per
         * subcarrier, which weighs the pilots of a block, 9 subcarriers apart at most, a little
         * differently where the channel changes fast along them: a few tenths of a dB on the
         * condition number, more where s2 is small. */
        CHECK_NEAR("condition per block, worst (dB)", worst, 0.0, 1.0);
        CHECK_NEAR("mean condition (dB)", res->cond_db, cond, 0.3);
        CHECK_NEAR("s1^2 (dB)", res->sv_db[0], 10.0 * log10(sv0), 0.2);
        CHECK_NEAR("highest condition (dB)", res->cond_max_db, cond_max, 0.5);
    }

    /* A cell wider than the capture: 15 blocks at 1.92 MSPS, of which the 5 whole in the six
     * central ones are measured, from block 5 of the cell, half a block above the band. */
    {
        static const struct mimo_cell cell = { 0, 15, 300, 0, 2, -500.0, 0.02, 999, 0.02 };
        struct mimo_channel ch;
        const double fs = 1920000.0;
        size_t count;

        memset(&ch, 0, sizeof(ch));
        ch.h[0][0] = 1.0;
        ch.h[0][1] = 0.5;
        ch.h[1][0] = 0.5 * I;
        ch.h[1][1] = -0.8;
        printf("LTE 2x2, cell 0, 15 blocks at 1.92 MSPS:\n");
        count = make_cell(x, (size_t)(0.032 * fs), &cell, &ch);
        CHECK("analyse", m2sdr_mimo_analyse(x[0], x[1], count, fs, res) == 0);
        CHECK("found, two ports", res->found && res->mimo && res->n_rb == 15);
        CHECK("blocks 5 to 9 of the cell", res->rb == 5 && res->first_rb == 5);
        {
            float complex h[2][2];
            double s[2];

            expected_block(&ch, &cell, 7, h);
            m2sdr_svd2((const float complex (*)[2])h, s);
            CHECK_NEAR("condition number (dB)", res->cond_db, 20.0 * log10(s[0] / s[1]), 0.2);
            CHECK_NEAR("path RX2 port 0 (dBFS)", res->path_db[1][0], 20.0 * log10(0.5 * cell.amp), 0.2);
        }
    }

    /* The cell on RX2 only: found there. */
    {
        static const struct mimo_cell cell = { 250, 25, 7, 2, 2, 0.0, 0.02, 4000, 0.02 };
        struct mimo_channel ch;
        const double fs = 1920000.0 * 4;
        size_t count;

        memset(&ch, 0, sizeof(ch));
        ch.h[1][0] = 1.0;
        ch.h[1][1] = 0.7;
        printf("LTE 2x2, cell 250 on RX2 only:\n");
        count = make_cell(x, (size_t)(0.032 * fs), &cell, &ch);
        CHECK("analyse", m2sdr_mimo_analyse(x[0], x[1], count, fs, res) == 0);
        CHECK("found on RX2", res->found && res->rx == 1 && res->cell_id == 250);
        CHECK("RX1 paths at the noise", res->path_db[0][0] < 20.0 * log10(cell.amp) - 25.0);
        CHECK("one layer", res->rank == 1);
    }

    /* A cell of one port: no 2x2 figures. */
    {
        static const struct mimo_cell cell = { 17, 25, 40, 2, 1, 0.0, 0.02, 2000, 0.02 };
        struct mimo_channel ch;
        const double fs = 1920000.0 * 4;
        size_t count;

        memset(&ch, 0, sizeof(ch));
        ch.h[0][0] = 1.0;
        ch.h[1][0] = 0.5;
        printf("LTE 2x2, cell 17, one port:\n");
        count = make_cell(x, (size_t)(0.032 * fs), &cell, &ch);
        CHECK("analyse", m2sdr_mimo_analyse(x[0], x[1], count, fs, res) == 0);
        CHECK("found, one port", res->found && res->ports == 1 && !res->mimo && res->rank == 1);
        CHECK_NEAR("path RX2 port 0 (dBFS)", res->path_db[1][0], 20.0 * log10(0.5 * cell.amp), 0.1);
    }

    printf("LTE 2x2, noise only:\n");
    memset(x[0], 0, max * sizeof(float complex));
    memset(x[1], 0, max * sizeof(float complex));
    add_noise(x[0], 200000, 0.01);
    add_noise(x[1], 200000, 0.01);
    CHECK("not found", m2sdr_mimo_analyse(x[0], x[1], 200000, 7680000.0, res) == 0 && !res->found);

    printf("LTE 2x2, bad arguments:\n");
    CHECK("bad sample rate", m2sdr_mimo_analyse(x[0], x[1], 200000, 2000000.0, res) != 0);
    CHECK("too short", m2sdr_mimo_analyse(x[0], x[1], 20000, 1920000.0, res) != 0);
    CHECK("no second channel", m2sdr_mimo_analyse(x[0], NULL, 200000, 1920000.0, res) != 0);

out:
    free(x[0]);
    free(x[1]);
    free(res);
}

/* Capture files */
/*---------------*/

#define FILE_COUNT (1u << 19)

/* Coherence: the wideband source of test_coherence(), 10 MSPS. */
static int write_coherence(const char *path)
{
    float complex *x1 = calloc(FILE_COUNT, sizeof(*x1)), *x2 = calloc(FILE_COUNT, sizeof(*x2));
    int rc = 1;

    if (!x1 || !x2)
        goto out;
    add_noise(x1, FILE_COUNT, 0.01);
    if (delay_copy(x1, x2, FILE_COUNT, -1.5, 37.0, 2.3, 0.0, 0.0) != 0)
        goto out;
    add_noise(x1, FILE_COUNT, 1e-4);
    add_noise(x2, FILE_COUNT, 1e-4 * pow(10.0, -0.15));
    rc = write_sc16(path, x1, x2, FILE_COUNT);
out:
    free(x1);
    free(x2);
    return rc;
}

/* Direction finding, 1 MSPS at 2.4 GHz: a tone from `angle` degrees, board phase 40 degrees. */
static int write_df(const char *path, double angle)
{
    const double lambda = DF_C / (DF_RF + 150e3);
    float complex *x1 = calloc(FILE_COUNT, sizeof(*x1)), *x2 = calloc(FILE_COUNT, sizeof(*x2));
    int rc = 1;

    if (!x1 || !x2)
        goto out;
    add_wave(x1, x2, FILE_COUNT, 150e3, 0.1, angle, lambda / 2, 40.0);
    add_noise(x1, FILE_COUNT, 1e-4);
    add_noise(x2, FILE_COUNT, 1e-4);
    rc = write_sc16(path, x1, x2, FILE_COUNT);
out:
    free(x1);
    free(x2);
    return rc;
}

/* LTE 2x2: the flat channel of test_mimo(), 7.68 MSPS, five frames. */
static int write_mimo(const char *path)
{
    static const struct mimo_cell cell = { 388, 25, 100, 2, 2, 812.0, 0.02, 3001, 0.02 };
    const size_t max = (size_t)(0.05 * 7680000.0);
    float complex *x[2] = { malloc(max * sizeof(float complex)), malloc(max * sizeof(float complex)) };
    struct mimo_channel ch;
    size_t count;
    int rc = 1;

    if (!x[0] || !x[1])
        goto out;
    memset(&ch, 0, sizeof(ch));
    ch.h[0][0] = 1.0;
    ch.h[0][1] = 0.4 * cexp(I * 1.1);
    ch.h[1][0] = 0.3 * cexp(-I * 2.0);
    ch.h[1][1] = 0.8 * cexp(I * 0.5);
    count = make_cell(x, max, &cell, &ch);
    rc    = write_sc16(path, x[0], x[1], count);
out:
    free(x[0]);
    free(x[1]);
    return rc;
}

/* The tolerances are those of the tests above, with the 12 bits of the file added: their
 * noise is 60 dB below a full-scale sample, far under the noise of the signals. */
static int check_coherence(const char *path)
{
    const char *json = read_report(path);

    if (!json)
        return 1;
    printf("Coherence report of m2sdr_analyzer:\n");
    CHECK("two channels, found", strstr(json, "\"two_channels\": true") && strstr(json, "\"found\": true"));
    CHECK_NEAR("coherence", json_number(json, "coherence", "coherence"), 0.9803, 0.003);
    CHECK_NEAR("gain (dB)", json_number(json, "coherence", "gain_db"), -1.5, 0.02);
    CHECK_NEAR("phase (deg)", json_number(json, "coherence", "phase_deg"), 37.0, 0.05);
    CHECK_NEAR("delay (ns)", json_number(json, "coherence", "delay_ns"), 230.0, 0.3);
    return report_done("coherence");
}

static int check_df(const char *path, double cal, double angle)
{
    const char *json = read_report(path);

    if (!json)
        return 1;
    printf("Direction finding report of m2sdr_analyzer:\n");
    CHECK("found", strstr(json, "\"found\": true") != NULL);
    CHECK_NEAR("calibration (deg)", json_number(json, "df", "calibration_deg"), cal, 0.05);
    CHECK_NEAR("angle (deg)", json_number(json, "df", "angle_deg"), angle, 0.05);
    CHECK_NEAR("MUSIC peak (deg)", json_number(json, "df", "music_peak_deg"), angle, 0.05);
    return report_done("direction finding");
}

static int check_mimo(const char *path)
{
    const char *json = read_report(path);

    if (!json)
        return 1;
    printf("LTE 2x2 report of m2sdr_analyzer:\n");
    CHECK("found, two ports", strstr(json, "\"found\": true") && strstr(json, "\"mimo\": true"));
    CHECK_NEAR("cell identity", json_number(json, "mimo", "cell_id"), 388, 0);
    CHECK_NEAR("blocks", json_number(json, "mimo", "rb"), 25, 0);
    /* 20 log10(s1 / s2) of the channel of the file. */
    CHECK_NEAR("condition (dB)", json_number(json, "mimo", "condition_db"), 5.3854, 0.15);
    CHECK_NEAR("rank", json_number(json, "mimo", "rank"), 2, 0);
    return report_done("LTE 2x2");
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--write-coherence") == 0)
        return write_coherence(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check-coherence") == 0)
        return check_coherence(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--write-df") == 0)
        return write_df(argv[2], 30.0);
    if (argc == 3 && strcmp(argv[1], "--write-df-broadside") == 0)
        return write_df(argv[2], 0.0);
    /* A calibration of 40 degrees, given or stored, gives the angle of the file. */
    if (argc == 3 && strcmp(argv[1], "--check-df") == 0)
        return check_df(argv[2], 40.0, 30.0);
    if (argc == 3 && strcmp(argv[1], "--write-mimo") == 0)
        return write_mimo(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check-mimo") == 0)
        return check_mimo(argv[2]);
    test_coherence();
    test_df();
    test_mimo();
    return tests_done();
}
