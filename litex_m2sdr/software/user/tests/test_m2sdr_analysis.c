/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the libm2sdr signal analysis on synthetic signals of known parameters.
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

#include "../libm2sdr/m2sdr_analysis.h"

#define FS    1.0e6
#define COUNT (1u << 18)

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

static void add_tone(float complex *x, size_t count, double freq_hz, double level_db, double phase)
{
    const double amp = pow(10.0, level_db / 20.0);
    size_t i;

    for (i = 0; i < count; i++)
        x[i] += (float complex)(amp * cexp(I * (2.0 * M_PI * freq_hz * (double)i / FS + phase)));
}

/* Band of flat spectrum: tones every `step_hz` with random phases, total power level_db. */
static void add_band(float complex *x, size_t count, double lo_hz, double hi_hz, double step_hz, double level_db)
{
    const int n = (int)floor((hi_hz - lo_hz) / step_hz + 1e-9);
    int k;

    for (k = 0; k < n; k++)
        add_tone(x, count, lo_hz + (k + 0.5) * step_hz, level_db - 10.0 * log10(n), 2.0 * M_PI * rng_uniform());
}

/* Tests */
/*-------*/

static void test_tone(float complex *x)
{
    static const enum m2sdr_window windows[] = {
        M2SDR_WINDOW_HANN, M2SDR_WINDOW_FLATTOP, M2SDR_WINDOW_BLACKMAN_HARRIS,
    };
    static const char *names[] = { "Hann", "flat-top", "Blackman-Harris" };
    struct m2sdr_psd psd;
    struct m2sdr_psd_peak peak, peaks[4];
    struct m2sdr_tone tone;
    unsigned w;
    int n;

    /* Tone between two bins, the worst case for its level; noise of -100 dBFS/Hz. */
    memset(x, 0, COUNT * sizeof(*x));
    add_tone(x, COUNT, 123456.7, -10.0, 0.3);
    add_tone(x, COUNT, -200000.0, -50.0, 1.0);
    add_noise(x, COUNT, pow(10.0, -100.0 / 10.0) * FS);

    memset(&psd, 0, sizeof(psd));
    for (w = 0; w < 3; w++) {
        printf("Tone, %s window:\n", names[w]);
        CHECK("psd", m2sdr_psd_compute(x, COUNT, FS, 4096, windows[w], &psd) == 0);
        CHECK("peak", m2sdr_psd_peak(&psd, -FS / 2, FS / 2, &peak) == 0);
        CHECK_NEAR("peak frequency (Hz)", peak.freq_hz, 123456.7, 0.1 * psd.bin_hz);
        CHECK_NEAR("peak level (dBFS)", peak.power_db, -10.0, windows[w] == M2SDR_WINDOW_FLATTOP ? 0.02 : 0.3);
        CHECK_NEAR("tone power, band (dBFS)", m2sdr_psd_band_power_db(&psd, 113456.7, 133456.7), -10.0, 0.05);
        CHECK_NEAR("noise density (dBFS/Hz)", m2sdr_psd_noise_density_db(&psd, -FS / 2, FS / 2), -100.0, 0.3);
        CHECK_NEAR("noise power, 100 kHz (dBFS)", m2sdr_psd_band_power_db(&psd, 250000.0, 350000.0), -50.0, 0.3);
        n = m2sdr_psd_peaks(&psd, -FS / 2, FS / 2, 10.0, peaks, 4);
        CHECK("two peaks", n == 2);
        if (n == 2) {
            CHECK_NEAR("second peak (Hz)", peaks[1].freq_hz, -200000.0, 0.2 * psd.bin_hz);
            CHECK_NEAR("second peak (dBFS)", peaks[1].power_db, -50.0, 0.5);
        }
    }

    printf("Tone figures:\n");
    memset(x, 0, COUNT * sizeof(*x));
    add_tone(x, COUNT, 100000.0, -6.0, 0.0);
    add_tone(x, COUNT, 200000.0, -46.0, 0.5);        /* Second harmonic, -40 dBc. */
    add_tone(x, COUNT, 300000.0, -66.0, 0.7);        /* Third, -60 dBc. */
    add_tone(x, COUNT, -400000.0, -76.0, 0.2);       /* Sixth, folded: 600 kHz - 1 MHz. */
    add_tone(x, COUNT, -310000.0, -56.0, 0.9);       /* A spur, -50 dBc. */
    add_tone(x, COUNT, 0.0, -30.0, 0.0);             /* DC. */
    add_noise(x, COUNT, pow(10.0, -66.0 / 10.0));    /* SNR 60 dB. */
    CHECK("psd", m2sdr_psd_compute(x, COUNT, FS, 16384, M2SDR_WINDOW_BLACKMAN_HARRIS, &psd) == 0);
    CHECK("tone", m2sdr_psd_tone(&psd, &tone) == 0);
    CHECK_NEAR("frequency (Hz)", tone.freq_hz, 100000.0, 1.0);
    CHECK_NEAR("level (dBFS)", tone.level_db, -6.0, 0.05);
    CHECK_NEAR("second harmonic (dBc)", tone.harmonic_dbc[0], -40.0, 0.2);
    CHECK_NEAR("third harmonic (dBc)", tone.harmonic_dbc[1], -60.0, 0.5);
    CHECK_NEAR("sixth harmonic (Hz)", tone.harmonic_hz[4], -400000.0, 1.0);
    CHECK_NEAR("sixth harmonic (dBc)", tone.harmonic_dbc[4], -70.0, 1.5);
    CHECK_NEAR("THD (dBc)", tone.thd_dbc, -39.95, 0.2);
    CHECK_NEAR("SFDR (dBc)", tone.sfdr_dbc, 40.0, 0.3);
    CHECK_NEAR("spur frequency (Hz)", tone.spur_freq_hz, 200000.0, psd.bin_hz);
    /* The spur at -310 kHz is not a harmonic: it counts as noise, 10^-5 next to 10^-6. */
    CHECK_NEAR("SNR (dB)", tone.snr_db, 60.0 - 10.0 * log10(11.0), 0.3);
    CHECK_NEAR("noise density (dBFS/Hz)", tone.noise_db_hz, -66.0 - 60.0 + 10.0 * log10(11.0), 0.3);
    m2sdr_psd_free(&psd);
}

static void test_channel(float complex *x)
{
    struct m2sdr_psd psd;
    struct m2sdr_acpr acpr;
    double lo, hi;

    printf("Channel:\n");
    memset(x, 0, COUNT * sizeof(*x));
    add_band(x, COUNT, -50000.0, 50000.0, 500.0, -20.0);     /* Channel, 100 kHz at 0. */
    add_band(x, COUNT, -200000.0, -100000.0, 500.0, -55.0);  /* Lower adjacent, -35 dBc. */
    add_band(x, COUNT, 100000.0, 200000.0, 500.0, -60.0);    /* Upper adjacent, -40 dBc. */
    add_band(x, COUNT, 250000.0, 350000.0, 500.0, -70.0);    /* Upper alternate, -50 dBc. */
    add_noise(x, COUNT, pow(10.0, -90.0 / 10.0));

    memset(&psd, 0, sizeof(psd));
    CHECK("psd", m2sdr_psd_compute(x, COUNT, FS, 8192, M2SDR_WINDOW_HANN, &psd) == 0);
    CHECK_NEAR("channel power (dBFS)", m2sdr_psd_band_power_db(&psd, -50000.0, 50000.0), -20.0, 0.1);
    CHECK("obw", m2sdr_psd_obw(&psd, -75000.0, 75000.0, 0.99, &lo, &hi) == 0);
    CHECK_NEAR("occupied bandwidth (Hz)", hi - lo, 99000.0, 1000.0);
    CHECK_NEAR("occupied band centre (Hz)", 0.5 * (lo + hi), 0.0, 500.0);
    CHECK("acpr", m2sdr_psd_acpr(&psd, 0.0, 100000.0, 150000.0, &acpr) == 0);
    CHECK("two adjacent channels", acpr.adjacent == 2);
    CHECK_NEAR("ACPR channel (dBFS)", acpr.channel_db, -20.0, 0.1);
    CHECK_NEAR("lower adjacent (dBc)", acpr.lower_dbc[0], -35.0, 0.3);
    CHECK_NEAR("upper adjacent (dBc)", acpr.upper_dbc[0], -40.0, 0.3);
    CHECK_NEAR("upper alternate (dBc)", acpr.upper_dbc[1], -50.0, 0.5);
    CHECK("channel out of the span", m2sdr_psd_acpr(&psd, 480000.0, 100000.0, 150000.0, &acpr) != 0);
    m2sdr_psd_free(&psd);
}

static void test_stats(float complex *x)
{
    struct m2sdr_iq_stats st;
    size_t i;

    printf("Time domain, tone:\n");
    memset(x, 0, COUNT * sizeof(*x));
    add_tone(x, COUNT, 12345.0, -12.0, 0.0);
    CHECK("stats", m2sdr_iq_stats(x, COUNT, 0.999, &st) == 0);
    CHECK_NEAR("mean power (dBFS)", st.mean_db, -12.0, 0.01);
    CHECK_NEAR("PAPR (dB)", st.papr_db, 0.0, 0.01);
    CHECK_NEAR("gain imbalance (dB)", st.gain_imbalance_db, 0.0, 0.01);
    CHECK_NEAR("quadrature error (deg)", st.quadrature_deg, 0.0, 0.05);
    CHECK_NEAR("clipped", st.clipped, 0.0, 0.0);

    printf("Time domain, impaired tone:\n");
    /* I larger by 1 dB, Q leaning 3 degrees towards I, a DC offset, and some clipping. */
    for (i = 0; i < COUNT; i++) {
        const double ph = 2.0 * M_PI * 12345.0 * (double)i / FS;
        const double a = 0.5 * pow(10.0, 1.0 / 20.0) * cos(ph) + 0.01;
        const double b = 0.5 * sin(ph + 3.0 * M_PI / 180.0) - 0.02;
        x[i] = (float)a + I * (float)b;
    }
    CHECK("stats", m2sdr_iq_stats(x, COUNT, 0.55, &st) == 0);
    CHECK_NEAR("gain imbalance (dB)", st.gain_imbalance_db, 1.0, 0.01);
    CHECK_NEAR("quadrature error (deg)", st.quadrature_deg, 3.0, 0.05);
    CHECK_NEAR("DC I", st.dc_i, 0.01, 1e-4);
    CHECK_NEAR("DC Q", st.dc_q, -0.02, 1e-4);
    CHECK_NEAR("DC (dBFS)", st.dc_db, 10.0 * log10(0.01 * 0.01 + 0.02 * 0.02), 0.05);
    /* I, of amplitude 0.561 around 0.01, is above 0.55 for acos(0.54 / 0.561) / pi of the time,
     * and below -0.55 for acos(0.56 / 0.561) / pi. */
    CHECK_NEAR("clipped", st.clipped, (acos(0.54 / 0.561) + acos(0.56 / 0.561)) / M_PI, 0.002);

    printf("Time domain, noise:\n");
    memset(x, 0, COUNT * sizeof(*x));
    add_noise(x, COUNT, 0.01);
    CHECK("stats", m2sdr_iq_stats(x, COUNT, 0.999, &st) == 0);
    CHECK_NEAR("mean power (dBFS)", st.mean_db, -20.0, 0.05);
    /* Power of complex Gaussian noise is exponential: P(p > a * mean) = exp(-a). */
    CHECK_NEAR("CCDF at 0 dB", st.ccdf[0], exp(-1.0), 0.005);
    CHECK_NEAR("CCDF at 3 dB", st.ccdf[12], exp(-pow(10.0, 0.3)), 0.003);
    CHECK_NEAR("CCDF at 6 dB", st.ccdf[24], exp(-pow(10.0, 0.6)), 0.001);
    CHECK("PAPR of noise above 9 dB", st.papr_db > 9.0 && st.papr_db < 13.0);
}

static void test_arguments(float complex *x)
{
    struct m2sdr_psd psd;
    struct m2sdr_psd_peak peak;
    struct m2sdr_iq_stats st;
    double lo, hi;

    printf("Arguments:\n");
    memset(&psd, 0, sizeof(psd));
    CHECK("length not a power of two", m2sdr_psd_compute(x, COUNT, FS, 1000, M2SDR_WINDOW_HANN, &psd) != 0);
    CHECK("fewer samples than the FFT", m2sdr_psd_compute(x, 512, FS, 1024, M2SDR_WINDOW_HANN, &psd) != 0);
    CHECK("empty spectrum", m2sdr_psd_peak(&psd, -1.0, 1.0, &peak) != 0);
    CHECK("psd", m2sdr_psd_compute(x, COUNT, FS, 1024, M2SDR_WINDOW_HANN, &psd) == 0);
    CHECK("segments", psd.segments == 2 * (COUNT / 1024) - 1);
    CHECK_NEAR("Hann ENBW (bins)", psd.enbw_bins, 1.5, 1e-6);
    CHECK("range out of the spectrum", m2sdr_psd_peak(&psd, 2 * FS, 3 * FS, &peak) != 0);
    CHECK("reversed range", m2sdr_psd_peak(&psd, 1000.0, -1000.0, &peak) != 0);
    CHECK("obw fraction", m2sdr_psd_obw(&psd, -1000.0, 1000.0, 1.5, &lo, &hi) != 0);
    CHECK("no samples", m2sdr_iq_stats(x, 0, 0.999, &st) != 0);
    /* The same struct with another length. */
    CHECK("psd, new length", m2sdr_psd_compute(x, COUNT, FS, 4096, M2SDR_WINDOW_FLATTOP, &psd) == 0);
    CHECK_NEAR("flat-top ENBW (bins)", psd.enbw_bins, 3.77, 0.01);
    m2sdr_psd_free(&psd);
    m2sdr_psd_free(&psd);
}

/* Phase noise */

#define PN_COUNT (1u << 20)

static double rng_gauss(void)
{
    return sqrt(-2.0 * log(rng_uniform())) * cos(2.0 * M_PI * rng_uniform());
}

/* Value of a trace at an offset, between its points. */
static double pn_at(const struct m2sdr_phase_noise *pn, const double *trace, double offset_hz)
{
    int i;

    for (i = 1; i < pn->points; i++) {
        if (pn->offset_hz[i] >= offset_hz) {
            const double t = log(offset_hz / pn->offset_hz[i - 1]) / log(pn->offset_hz[i] / pn->offset_hz[i - 1]);
            return trace[i - 1] + t * (trace[i] - trace[i - 1]);
        }
    }
    return NAN;
}

static void test_phase_noise(void)
{
    float complex *x = malloc(PN_COUNT * sizeof(*x));
    struct m2sdr_phase_noise *pn = malloc(sizeof(*pn));
    const double f0 = 50012.3;
    double walk = 0;
    size_t i;

    if (!x || !pn) {
        failures++;
        return;
    }

    /* White phase noise of 0.01 rad and white amplitude noise of 0.003, both RMS, and a
     * sinusoidal phase of 0.002 rad at 20 kHz: flat at sigma^2 / fs, a line at (b/2)^2. */
    printf("Phase noise, white:\n");
    for (i = 0; i < PN_COUNT; i++) {
        const double t = (double)i / FS;
        const double ph = 2.0 * M_PI * f0 * t + 0.4 + 0.01 * rng_gauss() + 0.002 * sin(2.0 * M_PI * 20000.0 * t);
        x[i] = (float complex)(0.5 * (1.0 + 0.003 * rng_gauss()) * cexp(I * ph));
    }
    CHECK("phase noise", m2sdr_phase_noise(x, PN_COUNT, FS, 1e9, 0.0, 0.0, pn) == 0);
    CHECK("carrier found", pn->found);
    CHECK_NEAR("carrier offset (Hz)", pn->carrier_hz, f0, 0.01);
    CHECK_NEAR("carrier level (dBFS)", pn->carrier_db, -6.02, 0.05);
    CHECK("five decades of offsets", pn->points >= 5 * M2SDR_PN_POINTS_PER_DECADE - 10 &&
          pn->offset_hz[0] < 10.0 && pn->offset_hz[pn->points - 1] > 300000.0);
    CHECK_NEAR("L at 1 kHz (dBc/Hz)", pn_at(pn, pn->l_dbc_hz, 1000.0), -100.0, 0.7);
    CHECK_NEAR("L at 100 kHz (dBc/Hz)", pn_at(pn, pn->l_dbc_hz, 100000.0), -100.0, 0.3);
    CHECK_NEAR("AM at 1 kHz (dBc/Hz)", pn_at(pn, pn->am_dbc_hz, 1000.0), 10.0 * log10(9e-6 / FS), 0.7);
    CHECK_NEAR("AM at 100 kHz (dBc/Hz)", pn_at(pn, pn->am_dbc_hz, 100000.0), 10.0 * log10(9e-6 / FS), 0.3);
    CHECK_NEAR("integrated phase (rad)", pn->rms_rad, sqrt(1e-4 + 2e-6), 3e-4);
    CHECK_NEAR("jitter at 1 GHz (fs)", pn->jitter_s * 1e15, 1e15 * sqrt(1e-4 + 2e-6) / (2.0 * M_PI * 1e9), 60.0);
    CHECK("one spur", pn->nspurs >= 1);
    CHECK_NEAR("spur offset (Hz)", pn->spur_hz[0], 20000.0, 1.0);
    CHECK_NEAR("spur level (dBc)", pn->spur_dbc[0], -60.0, 0.5);
    /* Between two limits: 2 * L * width. */
    CHECK("phase noise, limits", m2sdr_phase_noise(x, PN_COUNT, FS, 0.0, 1000.0, 11000.0, pn) == 0);
    CHECK_NEAR("integrated 1 to 11 kHz (rad)", pn->rms_rad, sqrt(2.0 * 1e-10 * 10000.0), 1.5e-4);
    CHECK_NEAR("no jitter without a frequency", pn->jitter_s, 0.0, 0.0);

    /* Random walk of the phase, 1e-4 rad per sample: sigma^2 * fs / (2 pi f)^2, 20 dB per
     * decade, on a carrier that is not on a bin. */
    printf("Phase noise, random walk:\n");
    for (i = 0; i < PN_COUNT; i++) {
        walk += 1e-4 * rng_gauss();
        x[i] = (float complex)(0.25 * cexp(I * (2.0 * M_PI * -123456.7 * (double)i / FS + walk)));
    }
    CHECK("phase noise", m2sdr_phase_noise(x, PN_COUNT, FS, 0.0, 0.0, 0.0, pn) == 0 && pn->found);
    CHECK_NEAR("L at 100 Hz (dBc/Hz)", pn_at(pn, pn->l_dbc_hz, 100.0), 10.0 * log10(1e-8 * FS / (4.0 * M_PI * M_PI * 1e4)), 2.0);
    CHECK_NEAR("L at 1 kHz (dBc/Hz)", pn_at(pn, pn->l_dbc_hz, 1000.0), 10.0 * log10(1e-8 * FS / (4.0 * M_PI * M_PI * 1e6)), 1.0);
    CHECK_NEAR("L at 10 kHz (dBc/Hz)", pn_at(pn, pn->l_dbc_hz, 10000.0), 10.0 * log10(1e-8 * FS / (4.0 * M_PI * M_PI * 1e8)), 0.5);
    CHECK_NEAR("L at 100 kHz (dBc/Hz)", pn_at(pn, pn->l_dbc_hz, 100000.0), 10.0 * log10(1e-8 * FS / (4.0 * M_PI * M_PI * 1e10)), 0.7);
    CHECK("AM far below", pn_at(pn, pn->am_dbc_hz, 1000.0) < -140.0);

    /* Clean carrier in additive noise: the noise is phase for one half and amplitude for the
     * other, N / (2 fs A^2) each. */
    printf("Phase noise, additive noise:\n");
    memset(x, 0, PN_COUNT * sizeof(*x));
    add_tone(x, PN_COUNT, 200000.0, -6.0206, 0.0);
    add_noise(x, PN_COUNT, 1e-4);
    CHECK("phase noise", m2sdr_phase_noise(x, PN_COUNT, FS, 0.0, 0.0, 0.0, pn) == 0 && pn->found);
    CHECK_NEAR("L at 10 kHz (dBc/Hz)", pn_at(pn, pn->l_dbc_hz, 10000.0), 10.0 * log10(1e-4 / (2.0 * FS * 0.25)), 0.4);
    CHECK_NEAR("AM at 10 kHz (dBc/Hz)", pn_at(pn, pn->am_dbc_hz, 10000.0), 10.0 * log10(1e-4 / (2.0 * FS * 0.25)), 0.4);

    printf("Phase noise, no carrier:\n");
    memset(x, 0, PN_COUNT * sizeof(*x));
    add_noise(x, PN_COUNT, 0.01);
    CHECK("noise", m2sdr_phase_noise(x, PN_COUNT, FS, 0.0, 0.0, 0.0, pn) == 0 && !pn->found);
    add_tone(x, PN_COUNT, 10000.0, -20.0, 0.0);     /* As strong as the noise: not enough. */
    CHECK("carrier in as much noise", m2sdr_phase_noise(x, PN_COUNT, FS, 0.0, 0.0, 0.0, pn) == 0 && !pn->found);
    CHECK("too short", m2sdr_phase_noise(x, 1000, FS, 0.0, 0.0, 0.0, pn) != 0);
    free(pn);
    free(x);
}

/* Report mode of m2sdr_analyzer */
/*-------------------------------*/

/* The Makefile runs m2sdr_analyzer --report --json on a file written here, and gives its
 * output back to be checked: this covers the file reader, the conversion and the JSON. */

#define FILE_COUNT  (1u << 17)
#define FILE_FREQ   123456.7
#define FILE_LEVEL  -10.0

static int write_capture(const char *path)
{
    float complex *x = calloc(FILE_COUNT, sizeof(*x));
    FILE *f = fopen(path, "wb");
    size_t i;

    if (!x || !f)
        return 1;
    /* One channel, SC16 with 12 significant bits. */
    add_tone(x, FILE_COUNT, FILE_FREQ, FILE_LEVEL, 0.0);
    add_band(x, FILE_COUNT, -250000.0, -150000.0, 500.0, -30.0);
    add_noise(x, FILE_COUNT, pow(10.0, -60.0 / 10.0));
    for (i = 0; i < FILE_COUNT; i++) {
        const short iq[2] = { (short)lrintf(crealf(x[i]) * 2048.0f), (short)lrintf(cimagf(x[i]) * 2048.0f) };
        if (fwrite(iq, sizeof(iq), 1, f) != 1)
            return 1;
    }
    free(x);
    return fclose(f) == 0 ? 0 : 1;
}

/* Number after "name": in the object `section` of a JSON text. */
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

static int check_report(const char *path)
{
    static char json[16384];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        printf("test_m2sdr_analysis: cannot open %s\n", path);
        return 1;
    }
    n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';

    printf("Report of m2sdr_analyzer:\n");
    CHECK_NEAR("centre (Hz)", json_number(json, "source", "center_hz"), 100e6, 0.5);
    CHECK_NEAR("tone frequency (Hz)", json_number(json, "tone", "freq_hz"), 100e6 + FILE_FREQ, 2.0);
    CHECK_NEAR("tone offset (Hz)", json_number(json, "tone", "offset_hz"), FILE_FREQ, 2.0);
    CHECK_NEAR("tone level (dBFS)", json_number(json, "tone", "level_db"), FILE_LEVEL, 0.1);
    CHECK_NEAR("channel power (dBFS)", json_number(json, "channel", "power_db"), -30.0, 0.2);
    CHECK_NEAR("occupied bandwidth (Hz)", json_number(json, "channel", "obw_hz"), 99000.0, 1500.0);
    CHECK_NEAR("mean power (dBFS)", json_number(json, "time", "mean_db"),
               10.0 * log10(pow(10.0, FILE_LEVEL / 10.0) + 1e-3 + 1e-6), 0.1);
    CHECK_NEAR("clipped", json_number(json, "time", "clipped"), 0.0, 0.0);
    if (failures) {
        printf("test_m2sdr_analysis: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis: report checked\n");
    return 0;
}

int main(int argc, char **argv)
{
    float complex *x;

    if (argc == 3 && strcmp(argv[1], "--write") == 0)
        return write_capture(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check") == 0)
        return check_report(argv[2]);

    x = malloc(COUNT * sizeof(*x));
    if (!x)
        return 1;
    test_tone(x);
    test_channel(x);
    test_stats(x);
    test_arguments(x);
    free(x);
    test_phase_noise();
    if (failures) {
        printf("test_m2sdr_analysis: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis: all tests passed\n");
    return 0;
}
