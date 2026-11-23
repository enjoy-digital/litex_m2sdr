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
#include "../libm2sdr/m2sdr_fft.h"
#include "../libm2sdr/m2sdr_lte_seq.h"

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

/* Demodulation */

#define DEMOD_COUNT (1u << 19)

static double test_rrc(double t, double alpha)
{
    if (fabs(t) < 1e-9)
        return 1.0 - alpha + 4.0 * alpha / M_PI;
    if (fabs(fabs(t) - 1.0 / (4.0 * alpha)) < 1e-9)
        return alpha / sqrt(2.0) * ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * alpha)) +
                                    (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * alpha)));
    return (sin(M_PI * t * (1.0 - alpha)) + 4.0 * alpha * t * cos(M_PI * t * (1.0 + alpha))) /
           (M_PI * t * (1.0 - 16.0 * alpha * alpha * t * t));
}

struct mod_signal {
    enum m2sdr_modulation modulation;
    double symbol_rate, rolloff, freq_hz;
    double evm;                 /* Noise added, as the EVM it gives after the matched filter. */
    double gain_db, quad_deg;   /* Transmitter impairments: I over Q, angle error. */
    double complex offset;      /* Added to the symbols. */
};

/* Random symbols under a root raised cosine, of amplitude 0.25, at FS. */
static void make_modulated(float complex *x, size_t count, const struct mod_signal *sig)
{
    static float complex points[256];
    const int npoints = m2sdr_constellation(sig->modulation, points);
    const double sps = FS / sig->symbol_rate;
    const size_t nsym = (size_t)((double)count / sps) + 40;
    double complex *a = malloc(nsym * sizeof(*a));
    const double gi = pow(10.0, sig->gain_db / 20.0), q = sig->quad_deg * M_PI / 180.0;
    size_t n, k;

    if (!a) {
        failures++;
        return;
    }
    for (k = 0; k < nsym; k++) {
        const float complex p = points[(int)(rng_uniform() * npoints) % npoints];
        /* I scaled, Q leaning towards I. */
        a[k] = gi * crealf(p) + cimagf(p) * sin(q) + I * cimagf(p) * cos(q) + sig->offset;
    }
    for (n = 0; n < count; n++) {
        const double t = (double)n / sps + 3.37;          /* In symbols, off the sample grid. */
        const long k0 = (long)floor(t) - 12, k1 = (long)floor(t) + 12;
        double complex v = 0;
        long kk;

        for (kk = k0 < 0 ? 0 : k0; kk <= k1 && kk < (long)nsym; kk++)
            v += a[kk] * test_rrc(t - (double)kk, sig->rolloff);
        x[n] = (float complex)(0.25 * v * cexp(I * (2.0 * M_PI * sig->freq_hz * (double)n / FS + 0.7)));
    }
    free(a);
    /* Noise of the full band that leaves `evm` after the filter matched to the symbols. */
    add_noise(x, count, 0.0625 * sig->evm * sig->evm * sps);
}

static void test_demod(void)
{
    static const struct mod_signal signals[] = {
        { M2SDR_MOD_BPSK,   50000.0,  0.35,   1234.0, 0.05, 0, 0, 0 },
        { M2SDR_MOD_QPSK,   50000.0,  0.35,  -4321.0, 0.05, 0, 0, 0 },
        { M2SDR_MOD_8PSK,   31250.0,  0.22,  20000.0, 0.03, 0, 0, 0 },
        { M2SDR_MOD_16QAM,  37000.0,  0.22, -80000.0, 0.03, 0, 0, 0 },
        { M2SDR_MOD_32QAM,  64000.0,  0.50,    500.0, 0.02, 0, 0, 0 },
        { M2SDR_MOD_64QAM, 100000.0,  0.15, 150000.0, 0.02, 0, 0, 0 },
        { M2SDR_MOD_128QAM, 25000.0,  0.35,  -1000.0, 0.01, 0, 0, 0 },
        { M2SDR_MOD_256QAM, 20000.0,  0.35,   3000.0, 0.01, 0, 0, 0 },
    };
    float complex *x = malloc(DEMOD_COUNT * sizeof(*x));
    struct m2sdr_demod_result *res = malloc(sizeof(*res));
    struct m2sdr_demod_config cfg;
    struct mod_signal sig;
    char name[64];
    unsigned i;

    if (!x || !res) {
        failures++;
        return;
    }

    /* Everything found from the signal. */
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        memset(&cfg, 0, sizeof(cfg));
        memset(x, 0, DEMOD_COUNT * sizeof(*x));
        make_modulated(x, DEMOD_COUNT, &signals[i]);
        printf("Demodulation, %s, %.0f symbols/s, roll-off %.2f, automatic:\n",
               m2sdr_modulation_name(signals[i].modulation), signals[i].symbol_rate, signals[i].rolloff);
        CHECK("demod", m2sdr_demod(x, DEMOD_COUNT, FS, &cfg, res) == 0);
        CHECK("signal and lock", res->signal && res->locked);
        snprintf(name, sizeof(name), "modulation (%s)", m2sdr_modulation_name(res->modulation));
        CHECK(name, res->modulation == signals[i].modulation);
        CHECK_NEAR("symbol rate (Hz)", res->symbol_rate, signals[i].symbol_rate, 1e-5 * signals[i].symbol_rate);
        CHECK_NEAR("roll-off", res->rolloff, signals[i].rolloff, 0.001);
        CHECK_NEAR("carrier offset (Hz)", res->freq_offset_hz, signals[i].freq_hz, 2e-4 * signals[i].symbol_rate);
        CHECK_NEAR("EVM (%)", 100.0 * res->evm_rms, 100.0 * signals[i].evm, 15.0 * signals[i].evm);
        CHECK_NEAR("MER (dB)", res->mer_db, -20.0 * log10(signals[i].evm), 1.3);
        CHECK_NEAR("signal power (dBFS)", res->power_db, 20.0 * log10(0.25), 0.5);
        CHECK("symbols and eye", res->symbols >= 2000 && res->eye_traces == M2SDR_DEMOD_EYE_TRACES);
    }

    /* Transmitter impairments on 16QAM. The rotation of the constellation is not resolved, so
     * I and Q can come out swapped: the signs are not checked. */
    printf("Demodulation, impairments:\n");
    memset(&cfg, 0, sizeof(cfg));
    sig = signals[3];
    sig.evm      = 0.005;
    sig.gain_db  = 0.5;
    sig.quad_deg = 2.0;
    sig.offset   = 0.02 - 0.01 * I;
    memset(x, 0, DEMOD_COUNT * sizeof(*x));
    make_modulated(x, DEMOD_COUNT, &sig);
    CHECK("demod", m2sdr_demod(x, DEMOD_COUNT, FS, &cfg, res) == 0 && res->locked);
    CHECK_NEAR("gain imbalance (dB)", fabs(res->gain_imbalance_db), 0.5, 0.05);
    CHECK_NEAR("quadrature error (deg)", fabs(res->quadrature_deg), 2.0, 0.15);
    CHECK_NEAR("origin offset (dB)", res->iq_offset_db, 10.0 * log10(0.02 * 0.02 + 0.01 * 0.01), 0.5);
    /* The impairments are in the EVM: gain and angle errors, and the offset. */
    CHECK("EVM above the noise", res->evm_rms > 0.03 && res->evm_rms < 0.06);

    /* Settings given. */
    printf("Demodulation, settings given:\n");
    memset(x, 0, DEMOD_COUNT * sizeof(*x));
    make_modulated(x, DEMOD_COUNT, &signals[5]);
    memset(&cfg, 0, sizeof(cfg));
    cfg.modulation  = M2SDR_MOD_64QAM;
    cfg.symbol_rate = 100000.0;
    cfg.rolloff     = 0.15;
    cfg.max_symbols = 1000;
    CHECK("demod", m2sdr_demod(x, DEMOD_COUNT, FS, &cfg, res) == 0 && res->locked);
    CHECK("symbols", res->symbols == 1000);
    CHECK_NEAR("EVM (%)", 100.0 * res->evm_rms, 2.0, 0.3);
    /* A symbol rate off by 20 ppm is taken by the timing. */
    cfg.symbol_rate = 100002.0;
    cfg.max_symbols = 0;
    CHECK("demod, rate off", m2sdr_demod(x, DEMOD_COUNT, FS, &cfg, res) == 0 && res->locked);
    CHECK_NEAR("symbol rate (Hz)", res->symbol_rate, 100000.0, 0.5);
    CHECK_NEAR("EVM (%)", 100.0 * res->evm_rms, 2.0, 0.3);
    /* The wrong modulation does not lock. */
    cfg.symbol_rate = 0;
    cfg.modulation  = M2SDR_MOD_QPSK;
    CHECK("64QAM read as QPSK", m2sdr_demod(x, DEMOD_COUNT, FS, &cfg, res) == 0 && res->signal && !res->locked);

    printf("Demodulation, no signal:\n");
    memset(&cfg, 0, sizeof(cfg));
    memset(x, 0, DEMOD_COUNT * sizeof(*x));
    add_noise(x, DEMOD_COUNT, 0.01);
    CHECK("noise", m2sdr_demod(x, DEMOD_COUNT, FS, &cfg, res) == 0 && !res->signal);
    add_tone(x, DEMOD_COUNT, 100000.0, -6.0, 0.0);
    CHECK("tone", m2sdr_demod(x, DEMOD_COUNT, FS, &cfg, res) == 0 && !res->locked);
    cfg.rolloff = 3.0;
    CHECK("bad roll-off", m2sdr_demod(x, DEMOD_COUNT, FS, &cfg, res) != 0);
    free(res);
    free(x);
}

/* LTE downlink */

#define LTE_TEST_FRAMES 5

struct lte_signal {
    int    cell_id, n_rb, sfn;
    int    rate_index;          /* Sample rate: 1.92 MSPS << rate_index. */
    double cfo_hz;
    double evm;                 /* Noise added, as the EVM it gives on a resource element. */
    size_t delay;               /* Samples ahead of the first frame. */
    double amp;                 /* Of a resource element; 0: 0.02. */
    int    ports;               /* Antenna ports: 2, or else 1. */
};

/* Port 1 of a cell of two ports, as one receiver gets it against port 0. */
static float complex lte_port1(int a, int k)
{
    return 0.6f * (float complex)cexp(I * (0.7 + 0.002 * (double)(a - k / 2)));
}

/* Frames of a cell: synchronisation signals, reference signals, PBCH, and data in the lower
 * half of the blocks, QPSK, 16QAM and 64QAM in turn by slot. Returns the number of samples
 * written.
 *
 * With two antenna ports, the PBCH is in transmit diversity and the blocks of data are, in turn:
 * transmit diversity, one precoded layer (twice, with two precoders), and two layers of 64QAM,
 * that one receiver cannot read. */
static size_t make_lte(float complex *x, size_t max, const struct lte_signal *sig)
{
    static float complex qpsk[4], qam16[16], qam64[64], crs[2 * LTE_MAX_RB], pss[LTE_SYNC_LEN];
    static float sss[LTE_SYNC_LEN];
    static uint8_t bch[LTE_PBCH_BITS];
    const int d = 1 << sig->rate_index, n = 128 * d, k = 12 * sig->n_rb;
    const double fs = 1920000.0 * d, amp = sig->amp > 0 ? sig->amp : 0.02;
    const float complex *cons[3] = { qpsk, qam16, qam64 };
    const int ncons[3] = { 4, 16, 64 };
    const bool two = sig->ports == 2;
    const float r = two ? (float)M_SQRT1_2 : 1.0f;
    float complex *grid = calloc((size_t)k, sizeof(*grid)), *g1 = calloc((size_t)k, sizeof(*g1));
    float complex *fd = malloc((size_t)n * sizeof(*fd)), *td = malloc((size_t)n * sizeof(*td));
    struct m2sdr_fft *fft = m2sdr_fft_alloc((size_t)n, true);
    uint8_t mib[LTE_MIB_BITS] = { 0 };
    size_t pos = sig->delay, i;
    int frame, ns, l, a, j, pbch_at;

    if (!grid || !g1 || !fd || !td || !fft) {
        failures++;
        return 0;
    }
    m2sdr_constellation(M2SDR_MOD_QPSK, qpsk);
    m2sdr_constellation(M2SDR_MOD_16QAM, qam16);
    m2sdr_constellation(M2SDR_MOD_64QAM, qam64);
    m2sdr_lte_pss(sig->cell_id % 3, pss);
    memset(x, 0, max * sizeof(*x));

    for (frame = 0; frame < LTE_TEST_FRAMES; frame++) {
        const int sfn = sig->sfn + frame;
        /* MIB: bandwidth, PHICH (normal, one), the eight high bits of the frame number. */
        const int bw = sig->n_rb == 6 ? 0 : sig->n_rb == 15 ? 1 : sig->n_rb == 25 ? 2 : sig->n_rb == 50 ? 3 : 5;

        memset(mib, 0, sizeof(mib));
        mib[0] = (uint8_t)((bw >> 2) & 1);
        mib[1] = (uint8_t)((bw >> 1) & 1);
        mib[2] = (uint8_t)(bw & 1);
        mib[4] = 1;
        for (j = 0; j < 8; j++)
            mib[6 + j] = (uint8_t)(((sfn >> 2) >> (7 - j)) & 1);
        m2sdr_lte_bch_encode(mib, two ? 2 : 1, sig->cell_id, bch);
        pbch_at = (sfn & 3) * LTE_PBCH_FRAME;

        for (ns = 0; ns < 20; ns++) {
            for (l = 0; l < 7; l++) {
                const int cp = (l ? 9 : 10) * d;
                const bool centre_sym = (ns == 0 || ns == 10) && (l == 5 || l == 6);

                static const float complex turn[4] = { 1, -1, I, -I };
                float complex x0 = 0, x1 = 0;
                bool second = false;

                memset(grid, 0, (size_t)k * sizeof(*grid));
                memset(g1, 0, (size_t)k * sizeof(*g1));
                if (centre_sym && l == 5)
                    m2sdr_lte_sss(sig->cell_id / 3, sig->cell_id % 3, ns == 10, sss);
                if (l == 0 || l == 4)
                    m2sdr_lte_crs(sig->cell_id, ns, l, crs);
                for (a = 0; a < k; a++) {
                    const bool centre = a >= k / 2 - 36 && a < k / 2 + 36;

                    if ((a % 12) == 0)
                        second = false;                     /* Pairs of transmit diversity: within a block. */
                    if ((l == 0 || l == 4) && (a % 6) == ((l ? 3 : 0) + sig->cell_id) % 6) {
                        grid[a] = crs[a / 6 - sig->n_rb + LTE_MAX_RB];
                    } else if (two && (l == 0 || l == 4) && (a % 6) == ((l ? 0 : 3) + sig->cell_id) % 6) {
                        g1[a] = crs[a / 6 - sig->n_rb + LTE_MAX_RB];
                    } else if (centre && centre_sym) {
                        const int at = a - (k / 2 - 31);
                        if (at >= 0 && at < LTE_SYNC_LEN)
                            grid[a] = l == 6 ? pss[at] : sss[at];
                    } else if (centre && ns == 1 && l < 4) {
                        if (l < 2 && (a % 3) == (sig->cell_id % 3))
                            continue;
                        if (!two) {
                            grid[a] = (float)M_SQRT1_2 * ((1 - 2 * bch[pbch_at]) + I * (1 - 2 * bch[pbch_at + 1]));
                        } else if ((pbch_at & 2) == 0) {
                            x0 = (float)M_SQRT1_2 * ((1 - 2 * bch[pbch_at]) + I * (1 - 2 * bch[pbch_at + 1]));
                            x1 = (float)M_SQRT1_2 * ((1 - 2 * bch[pbch_at + 2]) + I * (1 - 2 * bch[pbch_at + 3]));
                            grid[a] = r * x0;
                            g1[a]   = -r * conjf(x1);
                        } else {
                            grid[a] = r * x1;
                            g1[a]   = r * conjf(x0);
                        }
                        pbch_at += 2;
                    } else if ((ns & 1) == 0 && l < 3) {
                        continue;                           /* Control region: left empty. */
                    } else if (a / 12 < sig->n_rb / 2) {
                        const int m = ns % 3, way = (a / 12) % 4;

                        if (!two) {
                            grid[a] = cons[m][(int)(rng_uniform() * ncons[m]) % ncons[m]];
                        } else if (way == 0 && !second) {
                            x0 = cons[m][(int)(rng_uniform() * ncons[m]) % ncons[m]];
                            x1 = cons[m][(int)(rng_uniform() * ncons[m]) % ncons[m]];
                            grid[a] = r * x0;
                            g1[a]   = -r * conjf(x1);
                            second  = true;
                        } else if (way == 0) {
                            grid[a] = r * x1;
                            g1[a]   = r * conjf(x0);
                            second  = false;
                        } else if (way < 3) {
                            grid[a] = r * cons[m][(int)(rng_uniform() * ncons[m]) % ncons[m]];
                            g1[a]   = turn[(ns + way) % 4] * grid[a];
                        } else {
                            grid[a] = r * qam64[(int)(rng_uniform() * 64) % 64];
                            g1[a]   = r * qam64[(int)(rng_uniform() * 64) % 64];
                        }
                    }
                }
                if (two) {
                    for (a = 0; a < k; a++)
                        grid[a] += lte_port1(a, k) * g1[a];
                }
                memset(fd, 0, (size_t)n * sizeof(*fd));
                for (a = 0; a < k; a++) {
                    const int sub = a < k / 2 ? a - k / 2 : a - k / 2 + 1;
                    fd[(sub + n) % n] = grid[a];
                }
                m2sdr_fft_run(fft, fd, td);
                if (pos + (size_t)(cp + n) > max)
                    goto done;
                for (j = 0; j < cp; j++)
                    x[pos++] = (float)amp * td[n - cp + j];
                for (j = 0; j < n; j++)
                    x[pos++] = (float)amp * td[j];
            }
        }
    }
done:
    for (i = 0; i < pos; i++)
        x[i] *= (float complex)cexp(2.0 * M_PI * I * sig->cfo_hz * (double)i / fs);
    /* An element is amp after the FFT of the analysis: noise of evm * amp per element is
     * evm^2 * amp^2 * n per sample. */
    if (sig->evm > 0) {
        const double power = sig->evm * sig->evm * amp * amp * n;
        for (i = 0; i < pos; i++) {
            const double r = sqrt(-power * log(rng_uniform())), ph = 2.0 * M_PI * rng_uniform();
            x[i] += (float)(r * cos(ph)) + I * (float)(r * sin(ph));
        }
    }
    m2sdr_fft_free(fft);
    free(grid);
    free(g1);
    free(fd);
    free(td);
    return pos;
}

static void test_lte(void)
{
    static const struct lte_signal signals[] = {
        {   0,  6, 100, 0,    0.0, 0.02,  1234 },
        { 388, 15, 517, 1,  812.0, 0.03,  7777 },
        { 503, 25,   2, 2, -6400.0, 0.02, 30001 },
        { 141, 25, 1023, 2, 11300.0, 0.05,   500 },
        { 250, 50,  77, 3,  -150.0, 0.02, 99999 },
        { 388, 25, 295, 2,  1078.0, 0.015, 4321, 0.0, 2 },
        {  77, 15, 900, 1, -3000.0, 0.015,   100, 0.0, 2 },
    };
    const size_t max = (size_t)(0.052 * 1920000.0 * 8);
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_lte_result *res = malloc(sizeof(*res));
    unsigned i;
    int ns, rb;

    if (!x || !res) {
        failures++;
        return;
    }
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        const struct lte_signal *sig = &signals[i];
        const double fs = 1920000.0 * (1 << sig->rate_index);
        const size_t count = make_lte(x, (size_t)(0.052 * fs), sig);
        const int ports = sig->ports == 2 ? 2 : 1;
        int wrong = 0, empty_used = 0, used = 0, readable = 0, layers = 0, layers_read = 0, diversity = 0;

        printf("LTE, cell %d, %d blocks, %.2f MSPS, offset %.0f Hz, %d antenna port%s:\n", sig->cell_id, sig->n_rb, fs / 1e6,
               sig->cfo_hz, ports, ports == 1 ? "" : "s");
        CHECK("analyse", m2sdr_lte_analyse(x, count, fs, res) == 0);
        CHECK("cell found", res->found);
        CHECK_NEAR("cell identity", res->cell_id, sig->cell_id, 0);
        CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, sig->cfo_hz, 5.0);
        CHECK("MIB", res->mib);
        CHECK_NEAR("antenna ports", res->ports, ports, 0);
        CHECK_NEAR("resource blocks", res->n_rb, sig->n_rb, 0);
        /* The frame analysed is the first whole one: the first or the second sent. */
        CHECK("frame number", res->sfn == sig->sfn % 1024 || res->sfn == (sig->sfn + 1) % 1024);
        CHECK_NEAR("blocks analysed", res->analysed_rb, sig->n_rb, 0);
        CHECK_NEAR("RSRP (dBFS)", res->rsrp_db, 20.0 * log10(0.02), 0.3);
        CHECK_NEAR("PSS power (dBFS)", res->pss_db, 20.0 * log10(0.02), 0.5);
        CHECK_NEAR("reference signal EVM (%)", 100.0 * res->rs_evm, 100.0 * sig->evm, 30.0 * sig->evm);
        /* Two ports: the symbols are weaker than a reference element, by the channel of port 1,
         * and by the precoder for a block of one layer. */
        if (ports == 1) {
            CHECK_NEAR("PBCH EVM (%)", 100.0 * res->pbch_evm, 100.0 * sig->evm, 40.0 * sig->evm);
            CHECK("data analysed", res->data);
            CHECK_NEAR("data EVM (%)", 100.0 * res->data_evm, 100.0 * sig->evm, 40.0 * sig->evm);
        } else {
            CHECK_NEAR("PBCH EVM (%)", 100.0 * res->pbch_evm, 121.0 * sig->evm, 40.0 * sig->evm);
            CHECK("data analysed", res->data);
            CHECK("data EVM", res->data_evm > sig->evm && res->data_evm < 3.0 * sig->evm);
        }
        /* The lower half of the blocks carries QPSK, 16QAM, 64QAM by slot; the rest is empty. */
        for (ns = 0; ns < 20; ns++) {
            static const enum m2sdr_modulation expect[3] = { M2SDR_MOD_QPSK, M2SDR_MOD_16QAM, M2SDR_MOD_64QAM };
            for (rb = 0; rb < sig->n_rb; rb++) {
                /* The block around the centre holds only synchronisation in some slots. */
                if (rb < sig->n_rb / 2 && ports == 2 && (rb % 4) == 3) {
                    layers++;
                    if (res->rb_modulation[ns][rb] != M2SDR_MOD_AUTO)
                        layers_read++;
                } else if (rb < sig->n_rb / 2) {
                    readable++;
                    if (ports == 2 && (rb % 4) == 0)
                        diversity++;
                    if (res->rb_modulation[ns][rb] != M2SDR_MOD_AUTO) {
                        used++;
                        if (res->rb_modulation[ns][rb] != expect[ns % 3])
                            wrong++;
                    }
                } else if (res->rb_modulation[ns][rb] != M2SDR_MOD_AUTO) {
                    empty_used++;
                }
            }
        }
        CHECK("blocks in use found", used >= readable - 12 && res->data_blocks == used + empty_used);
        /* The carrier is analysed whole: its blocks are the ones of the grid. */
        CHECK("blocks of the cell", res->data_rb == sig->n_rb && res->data_first_rb == 0 && res->data_offset == 0);
        {
            int n, other = 0;

            for (n = 0; n < res->ndata; n++) {
                const uint8_t m = res->data_points_modulation[n];
                if (m != M2SDR_MOD_QPSK && m != M2SDR_MOD_16QAM && m != M2SDR_MOD_64QAM)
                    other++;
            }
            CHECK("modulation of the symbols", res->ndata > 0 && other == 0);
        }
        CHECK("their modulation", wrong == 0);
        CHECK("empty blocks left out", empty_used == 0);
        if (ports == 2) {
            CHECK("blocks of two layers not read", layers_read == 0 && res->data_unknown >= layers - 6 &&
                  res->data_unknown <= layers);
            CHECK("blocks in transmit diversity", res->data_diversity >= diversity - 6 && res->data_diversity <= diversity);
            CHECK("blocks of one precoded layer", res->data_diversity + res->data_precoded == res->data_blocks);
        }
    }

    printf("LTE, no cell:\n");
    memset(x, 0, max * sizeof(*x));
    add_noise(x, 200000, 0.01);
    CHECK("noise", m2sdr_lte_analyse(x, 200000, 1920000.0, res) == 0 && !res->found);
    /* A wide signal that is not a cell: its PSS and SSS pass now and then, its reference signals
     * do not. */
    {
        int phantoms = 0, n;

        for (n = 0; n < 12; n++) {
            memset(x, 0, max * sizeof(*x));
            add_noise(x, 400000, 0.01);
            if (m2sdr_lte_analyse(x, 400000, 7680000.0, res) != 0 || res->found)
                phantoms++;
        }
        CHECK("wide noise, no cell", phantoms == 0);
    }
    CHECK("bad sample rate", m2sdr_lte_analyse(x, 200000, 2000000.0, res) != 0);
    CHECK("too short", m2sdr_lte_analyse(x, 20000, 1920000.0, res) != 0);
    free(res);
    free(x);
}

/* Frames of a cell as a capture file, to be analysed or sent: SC16, the same on two channels,
 * five frames that can be played in a loop. */
static int write_lte(const char *path)
{
    static const struct lte_signal sig = { 301, 25, 400, 2, 0.0, 0.0, 0, 0.012 };
    const size_t max = 5 * 76800;
    float complex *x = malloc(max * sizeof(*x));
    FILE *f = fopen(path, "wb");
    size_t count, i;

    if (!x || !f)
        return 1;
    count = make_lte(x, max, &sig);
    for (i = 0; i < count; i++) {
        const short v[4] = { (short)lrintf(crealf(x[i]) * 2047.0f), (short)lrintf(cimagf(x[i]) * 2047.0f),
                             (short)lrintf(crealf(x[i]) * 2047.0f), (short)lrintf(cimagf(x[i]) * 2047.0f) };
        if (fwrite(v, sizeof(v), 1, f) != 1)
            return 1;
    }
    free(x);
    return fclose(f) == 0 && count == max ? 0 : 1;
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

static int check_lte_report(const char *path)
{
    static char json[32768];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        printf("test_m2sdr_analysis: cannot open %s\n", path);
        return 1;
    }
    n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';
    printf("LTE report of m2sdr_analyzer:\n");
    CHECK_NEAR("cell identity", json_number(json, "lte", "cell_id"), 301, 0);
    CHECK_NEAR("resource blocks", json_number(json, "lte", "n_rb"), 25, 0);
    CHECK_NEAR("antenna ports", json_number(json, "lte", "ports"), 1, 0);
    CHECK_NEAR("carrier offset (Hz)", json_number(json, "lte", "carrier_offset_hz"), 0.0, 2.0);
    CHECK("reference signal EVM under 1 %", json_number(json, "lte", "rs_evm_percent") < 1.0);
    CHECK("data EVM under 1 %", json_number(json, "lte", "data_evm_percent") < 1.0);
    CHECK("data blocks", json_number(json, "lte", "data_blocks") >= 20 * 12 - 12);
    if (failures) {
        printf("test_m2sdr_analysis: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis: LTE report checked\n");
    return 0;
}

/* DVB-T */
/*-------*/

/* A DVB-T multiplex built as EN 300 744 sets its pilots and TPS, with random data in place of
 * the coded stream: what the analysis reads. */
struct dvbt_signal {
    int    n;                   /* 2048 or 8192. */
    int    g;                   /* Guard interval: 4 to 32. */
    int    con;                 /* TPS constellation field: 0 QPSK, 1 16QAM, 2 64QAM. */
    int    hp;                  /* TPS code rate field: 0 (1/2) to 4 (7/8). */
    int    start;               /* Symbol of the frame the capture starts with. */
    double freq_hz;             /* Carrier offset. */
    double snr_db;
    double echo_us, echo_db;    /* A second path; echo_db -99 for none. */
};

static const int dvbt_cp[] = {
       0,   48,   54,   87,  141,  156,  192,  201,  255,  279,  282,  333,  432,  450,  483,
     525,  531,  618,  636,  714,  759,  765,  780,  804,  873,  888,  918,  939,  942,  969,
     984, 1050, 1101, 1107, 1110, 1137, 1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704,
};
static const int dvbt_tps[] = {
      34,   50,  209,  346,  413,  569,  595,  688,  790,  901, 1073, 1219, 1262, 1286, 1469,
    1594, 1687,
};

static size_t make_dvbt(float complex *x, size_t max, double fs, const struct dvbt_signal *sig)
{
    const int k = sig->n == 8192 ? 6817 : 1705, gl = sig->n / sig->g, l = sig->n + gl;
    const int side = sig->con == 0 ? 2 : sig->con == 1 ? 4 : 8;
    const double scale = 1.0 / sqrt(2.0 * (side * side - 1) / 3.0);
    struct m2sdr_fft *ifft = m2sdr_fft_alloc((size_t)sig->n, true);
    float complex *f = malloc((size_t)sig->n * sizeof(*f)), *t = malloc((size_t)sig->n * sizeof(*t));
    uint8_t *w = malloc((size_t)k), *kind = calloc((size_t)k, 1), tps[68];
    float tps_state = 1.0f;
    const size_t nsym = max / (size_t)l;
    const size_t echo = (size_t)lround(sig->echo_us * 1e-6 * fs);
    const double echo_amp = pow(10.0, sig->echo_db / 20.0);
    unsigned reg = 0x7ff;
    size_t s, i, count = nsym * (size_t)l;
    double power = 0;
    int c, b;

    for (c = 0; c < k; c++) {
        w[c] = (uint8_t)((reg >> 10) & 1u);
        reg = ((reg << 1) | (w[c] ^ ((reg >> 8) & 1u))) & 0x7ffu;
    }
    for (b = 0; b < k; b += 1704) {
        for (c = 0; c < (int)(sizeof(dvbt_cp) / sizeof(dvbt_cp[0])); c++)
            if (b + dvbt_cp[c] < k)
                kind[b + dvbt_cp[c]] = 1;
        for (c = 0; c < (int)(sizeof(dvbt_tps) / sizeof(dvbt_tps[0])); c++)
            if (b + dvbt_tps[c] < k)
                kind[b + dvbt_tps[c]] = 2;
    }
    memset(x, 0, max * sizeof(*x));
    for (s = 0; s < nsym; s++) {
        const int fsym = (int)((s + (size_t)sig->start) % 68), frame = (int)(((s + (size_t)sig->start) / 68) % 4);

        if (fsym == 0 || s == 0) {
            /* TPS of the frame: s0, the sync word (inverted in even frames), length 23, then the parameters. */
            const unsigned sync = frame % 2 ? 0xca11u : 0x35eeu;
            unsigned v[] = { 0x17, (unsigned)frame, (unsigned)sig->con, 0, (unsigned)sig->hp, (unsigned)sig->hp,
                             (unsigned)(sig->g == 32 ? 0 : sig->g == 16 ? 1 : sig->g == 8 ? 2 : 3), sig->n == 8192 ? 1u : 0u };
            const int bits[] = { 6, 2, 2, 3, 3, 3, 2, 2 };
            int pos = 17, j;

            memset(tps, 0, sizeof(tps));
            for (j = 0; j < 16; j++)
                tps[1 + j] = (uint8_t)((sync >> (15 - j)) & 1u);
            for (j = 0; j < 8; j++) {
                int q;

                for (q = bits[j] - 1; q >= 0; q--)
                    tps[pos++] = (uint8_t)((v[j] >> q) & 1u);
            }
        }
        tps_state = fsym == 0 ? 1.0f : tps[fsym] ? -tps_state : tps_state;
        memset(f, 0, (size_t)sig->n * sizeof(*f));
        for (c = 0; c < k; c++) {
            float complex v;

            if (kind[c] == 1 || (c >= 3 * (fsym % 4) && (c - 3 * (fsym % 4)) % 12 == 0))
                v = (4.0f / 3.0f) * (w[c] ? -1.0f : 1.0f);
            else if (kind[c] == 2)
                v = tps_state * (w[c] ? -1.0f : 1.0f);
            else
                v = (float complex)(scale * ((2 * (int)(rng_uniform() * side) - side + 1) +
                                             I * (2 * (int)(rng_uniform() * side) - side + 1)));
            f[(c - (k - 1) / 2 + sig->n) % sig->n] = v;
        }
        m2sdr_fft_run(ifft, f, t);
        for (i = 0; i < (size_t)l; i++)
            x[s * (size_t)l + i] = t[(i + (size_t)sig->n - (size_t)gl) % (size_t)sig->n];
    }
    if (sig->echo_db > -99.0)
        for (i = count; i-- > echo;)
            x[i] += (float)echo_amp * x[i - echo];
    for (i = 0; i < count; i++) {
        x[i] *= (float complex)cexp(I * 2.0 * M_PI * sig->freq_hz * (double)i / fs);
        power += crealf(x[i] * conjf(x[i]));
    }
    power /= (double)count;
    for (i = 0; i < count; i++)
        x[i] *= (float)(0.1 / sqrt(power));
    add_noise(x, count, 0.01 * pow(10.0, -sig->snr_db / 10.0));
    m2sdr_fft_free(ifft);
    free(f);
    free(t);
    free(w);
    free(kind);
    return count;
}

static void test_dvbt(void)
{
    static const struct dvbt_signal signals[] = {
        /*  n     g  con hp start  freq_hz   snr echo_us echo_db */
        { 8192,  8,  2,  2,   0,       0.0, 30.0,  0.0, -99.0 },   /* France: 8k 64QAM 3/4 1/8. */
        { 8192,  8,  2,  2,  40,  166667.0, 25.0, 20.0,  -6.0 },   /* 1/6 MHz offset, an echo. */
        { 8192,  4,  1,  1,  13,   -2500.0, 20.0,  0.0, -99.0 },
        { 2048, 32,  0,  0,  60,   12000.0, 15.0,  0.0, -99.0 },
    };
    static const enum m2sdr_modulation mods[3] = { M2SDR_MOD_QPSK, M2SDR_MOD_16QAM, M2SDR_MOD_64QAM };
    static const char *const rates[5] = { "1/2", "2/3", "3/4", "5/6", "7/8" };
    const double fs = 64e6 / 7.0;
    const size_t max = (size_t)(0.15 * fs);
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_dvbt_result *res = malloc(sizeof(*res));
    size_t i;

    printf("DVB-T:\n");
    if (!x || !res) {
        CHECK("allocation", 0);
        free(x);
        free(res);
        return;
    }
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        const struct dvbt_signal *sig = &signals[i];
        const size_t count = make_dvbt(x, max, fs, sig);

        printf(" %dk, guard 1/%d, offset %.0f Hz, SNR %.0f dB%s\n", sig->n / 1024, sig->g, sig->freq_hz, sig->snr_db,
               sig->echo_db > -99.0 ? ", echo" : "");
        CHECK("analyse", m2sdr_dvbt_analyse(x, count, fs, NULL, res) == 0);
        CHECK("found, pilots, TPS", res->found && res->pilots && res->tps);
        CHECK_NEAR("FFT length", res->fft_len, sig->n, 0);
        CHECK_NEAR("guard interval", res->guard, sig->g, 0);
        CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, sig->freq_hz, 20.0);
        CHECK("TPS mode and guard", res->tps_fft_len == sig->n && res->tps_guard == sig->g);
        CHECK("TPS constellation", res->tps_modulation == mods[sig->con] && res->modulation == mods[sig->con]);
        CHECK("TPS code rate", strcmp(res->code_rate_hp, rates[sig->hp]) == 0);
        /* The channel is estimated on noisy pilots: the MER reads a little under the SNR. An
         * echo of amplitude a leaves the equalised carriers (1 - a^2) / (1 + a^2) of it. */
        {
            const double a2 = sig->echo_db > -99.0 ? pow(10.0, sig->echo_db / 10.0) : 0.0;

            CHECK_NEAR("MER (dB)", res->mer_db, sig->snr_db - 1.0 + 10.0 * log10((1.0 - a2) / (1.0 + a2)), 1.5);
        }
        if (sig->echo_db > -99.0) {
            CHECK_NEAR("echo delay (us)", res->echo_delay_us, sig->echo_us, 0.5);
            CHECK_NEAR("echo level (dB)", res->echo_level_db, sig->echo_db, 1.5);
        }
    }
    /* Constellation without the TPS: the one the data fits best. */
    {
        const struct dvbt_signal sig = { 8192, 8, 1, 2, 0, 0.0, 25.0, 0.0, -99.0 };
        const size_t count = make_dvbt(x, max, fs, &sig);

        printf(" no TPS (too short a capture)\n");
        CHECK("analyse", m2sdr_dvbt_analyse(x, count / 4, fs, NULL, res) == 0 && res->pilots && !res->tps);
        CHECK("16QAM found", res->modulation == M2SDR_MOD_16QAM);
    }
    /* 40 ms with what an earlier capture found: the MER without the TPS. */
    {
        const struct dvbt_signal sig = { 8192, 8, 2, 2, 30, 0.0, 22.0, 0.0, -99.0 };
        const struct m2sdr_dvbt_config cfg = { 8192, 8, M2SDR_MOD_64QAM };
        const size_t count = make_dvbt(x, max, fs, &sig);

        (void)count;
        printf(" 40 ms, mode and constellation given\n");
        CHECK("analyse", m2sdr_dvbt_analyse(x, (size_t)(0.04 * fs), fs, &cfg, res) == 0 && res->pilots);
        CHECK("64QAM", res->modulation == M2SDR_MOD_64QAM);
        CHECK_NEAR("MER (dB)", res->mer_db, 21.0, 1.5);
    }
    memset(x, 0, max * sizeof(*x));
    add_noise(x, max, 0.01);
    CHECK("noise", m2sdr_dvbt_analyse(x, max, fs, NULL, res) == 0 && !res->found);
    CHECK("bad sample rate", m2sdr_dvbt_analyse(x, max, 10e6, NULL, res) != 0);
    CHECK("too short", m2sdr_dvbt_analyse(x, 50000, fs, NULL, res) != 0);
    free(x);
    free(res);
}

static int write_dvbt(const char *path)
{
    static const struct dvbt_signal sig = { 8192, 8, 2, 2, 20, 166667.0, 30.0, 0.0, -99.0 };
    const double fs = 64e6 / 7.0;
    const size_t max = (size_t)(0.16 * fs);
    float complex *x = malloc(max * sizeof(*x));
    FILE *f = fopen(path, "wb");
    size_t count, i;

    if (!x || !f)
        return 1;
    count = make_dvbt(x, max, fs, &sig);
    for (i = 0; i < count; i++) {
        const short v[4] = { (short)lrintf(crealf(x[i]) * 2047.0f), (short)lrintf(cimagf(x[i]) * 2047.0f),
                             (short)lrintf(crealf(x[i]) * 2047.0f), (short)lrintf(cimagf(x[i]) * 2047.0f) };
        if (fwrite(v, sizeof(v), 1, f) != 1)
            return 1;
    }
    free(x);
    return fclose(f) == 0 ? 0 : 1;
}

static int check_dvbt_report(const char *path)
{
    static char json[32768];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        printf("test_m2sdr_analysis: cannot open %s\n", path);
        return 1;
    }
    n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';
    printf("DVB-T report of m2sdr_analyzer:\n");
    CHECK("mode 8k, guard 1/8", strstr(json, "\"mode\": \"8k\"") && strstr(json, "\"guard_interval\": \"1/8\""));
    CHECK("64QAM, code rate 3/4", strstr(json, "\"constellation\": \"64QAM\"") && strstr(json, "\"code_rate_hp\": \"3/4\""));
    CHECK_NEAR("carrier offset (Hz)", json_number(json, "dvbt", "carrier_offset_hz"), 166667.0, 20.0);
    CHECK("MER over 25 dB", json_number(json, "dvbt", "mer_db") > 25.0);
    if (failures) {
        printf("test_m2sdr_analysis: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis: DVB-T report checked\n");
    return 0;
}

int main(int argc, char **argv)
{
    float complex *x;

    if (argc == 3 && strcmp(argv[1], "--write") == 0)
        return write_capture(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check") == 0)
        return check_report(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--write-lte") == 0)
        return write_lte(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check-lte") == 0)
        return check_lte_report(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--write-dvbt") == 0)
        return write_dvbt(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check-dvbt") == 0)
        return check_dvbt_report(argv[2]);

    x = malloc(COUNT * sizeof(*x));
    if (!x)
        return 1;
    test_tone(x);
    test_channel(x);
    test_stats(x);
    test_arguments(x);
    free(x);
    test_phase_noise();
    test_demod();
    test_lte();
    test_dvbt();
    if (failures) {
        printf("test_m2sdr_analysis: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis: all tests passed\n");
    return 0;
}
