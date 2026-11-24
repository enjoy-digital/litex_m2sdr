/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the libm2sdr signal analysis on synthetic signals of known parameters.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#define TEST_NAME "test_m2sdr_analysis"
#include "test_analysis_helpers.h"

#include "../libm2sdr/m2sdr_analysis.h"
#include "../libm2sdr/m2sdr_fft.h"
#include "../libm2sdr/m2sdr_lte_seq.h"

#define FS    1.0e6
#define COUNT (1u << 18)

/* Signals */
/*---------*/

/* Tone of level_db, at the sample rate of these tests. */
static void add_tone_db(float complex *x, size_t count, double freq_hz, double level_db, double phase)
{
    add_tone(x, count, freq_hz, FS, pow(10.0, level_db / 20.0), phase);
}

/* Band of flat spectrum: tones every `step_hz` with random phases, total power level_db. */
static void add_band(float complex *x, size_t count, double lo_hz, double hi_hz, double step_hz, double level_db)
{
    const int n = (int)floor((hi_hz - lo_hz) / step_hz + 1e-9);
    int k;

    for (k = 0; k < n; k++)
        add_tone_db(x, count, lo_hz + (k + 0.5) * step_hz, level_db - 10.0 * log10(n), 2.0 * M_PI * rng_uniform());
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
    add_tone_db(x, COUNT, 123456.7, -10.0, 0.3);
    add_tone_db(x, COUNT, -200000.0, -50.0, 1.0);
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
    add_tone_db(x, COUNT, 100000.0, -6.0, 0.0);
    add_tone_db(x, COUNT, 200000.0, -46.0, 0.5);        /* Second harmonic, -40 dBc. */
    add_tone_db(x, COUNT, 300000.0, -66.0, 0.7);        /* Third, -60 dBc. */
    add_tone_db(x, COUNT, -400000.0, -76.0, 0.2);       /* Sixth, folded: 600 kHz - 1 MHz. */
    add_tone_db(x, COUNT, -310000.0, -56.0, 0.9);       /* A spur, -50 dBc. */
    add_tone_db(x, COUNT, 0.0, -30.0, 0.0);             /* DC. */
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
    add_tone_db(x, COUNT, 12345.0, -12.0, 0.0);
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
    add_tone_db(x, PN_COUNT, 200000.0, -6.0206, 0.0);
    add_noise(x, PN_COUNT, 1e-4);
    CHECK("phase noise", m2sdr_phase_noise(x, PN_COUNT, FS, 0.0, 0.0, 0.0, pn) == 0 && pn->found);
    CHECK_NEAR("L at 10 kHz (dBc/Hz)", pn_at(pn, pn->l_dbc_hz, 10000.0), 10.0 * log10(1e-4 / (2.0 * FS * 0.25)), 0.4);
    CHECK_NEAR("AM at 10 kHz (dBc/Hz)", pn_at(pn, pn->am_dbc_hz, 10000.0), 10.0 * log10(1e-4 / (2.0 * FS * 0.25)), 0.4);

    printf("Phase noise, no carrier:\n");
    memset(x, 0, PN_COUNT * sizeof(*x));
    add_noise(x, PN_COUNT, 0.01);
    CHECK("noise", m2sdr_phase_noise(x, PN_COUNT, FS, 0.0, 0.0, 0.0, pn) == 0 && !pn->found);
    add_tone_db(x, PN_COUNT, 10000.0, -20.0, 0.0);     /* As strong as the noise: not enough. */
    CHECK("carrier in as much noise", m2sdr_phase_noise(x, PN_COUNT, FS, 0.0, 0.0, 0.0, pn) == 0 && !pn->found);
    CHECK("too short", m2sdr_phase_noise(x, 1000, FS, 0.0, 0.0, 0.0, pn) != 0);
    free(pn);
    free(x);
}

/* Demodulation */

#define DEMOD_COUNT (1u << 19)

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
    add_tone_db(x, DEMOD_COUNT, 100000.0, -6.0, 0.0);
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
    size_t echo;                /* A second path this many samples later, of amplitude echo_amp; 0: none. */
    double echo_amp;
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
    /* The echo before the noise: the notches of the channel are in the noise. */
    if (sig->echo > 0)
        for (i = pos; i-- > sig->echo;)
            x[i] += (float)sig->echo_amp * x[i - sig->echo];
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

/* EVM of the reference signals where the channel or the noise make some of their estimates
 * small: two ports as on air, a deep notch (an echo of -0.5 dB two samples later at 7.68 MSPS,
 * notches 3.8 MHz apart), then noise at -3 dB (co-channel cells at a cell edge). The EVM is
 * the power of the errors over that of the channel: noise of evm per element over a channel of
 * mean power 1 + a^2 reads evm / sqrt(1 + a^2); the smoothing over 5 pilots adds a little where
 * the channel turns fast. A mean of the ratios of each pilot to its neighbours blows up where
 * they are small: 270 % for 115 % on air. */
static void test_lte_evm(void)
{
    static const struct lte_signal signals[] = {
        { 209, 50, 285, 2, 491.0, 0.10, 2222, 0.0, 2, 2, -0.95 },
        { 209, 25, 285, 2, 491.0, 0.70, 2222, 0.0, 2, 0, 0.0 },
    };
    const size_t max = (size_t)(0.052 * 1920000.0 * 8);
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_lte_result *res = malloc(sizeof(*res));
    unsigned i;

    if (!x || !res) {
        failures++;
        free(x);
        free(res);
        return;
    }
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        const struct lte_signal *sig = &signals[i];
        const double fs = 1920000.0 * (1 << sig->rate_index);
        const size_t count = make_lte(x, (size_t)(0.052 * fs), sig);
        const double expect = sig->evm / sqrt(1.0 + sig->echo_amp * sig->echo_amp);

        printf("LTE, cell %d, two ports, %s, noise %.0f %%:\n", sig->cell_id, sig->echo ? "deep notches" : "flat channel",
               100.0 * sig->evm);
        CHECK("analyse, cell", m2sdr_lte_analyse(x, count, fs, res) == 0 && res->found && res->cell_id == sig->cell_id);
        CHECK_NEAR("reference signal EVM (%)", 100.0 * res->rs_evm, 100.0 * expect, 25.0 * expect);
    }
    free(x);
    free(res);
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
    size_t count;
    int rc;

    if (!x)
        return 1;
    count = make_lte(x, max, &sig);
    rc    = write_sc16(path, x, x, count);
    free(x);
    return rc == 0 && count == max ? 0 : 1;
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
    add_tone_db(x, FILE_COUNT, FILE_FREQ, FILE_LEVEL, 0.0);
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

static int check_report(const char *path)
{
    const char *json = read_report(path);

    if (!json)
        return 1;
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
    return report_done(NULL);
}

static int check_lte_report(const char *path)
{
    const char *json = read_report(path);

    if (!json)
        return 1;
    printf("LTE report of m2sdr_analyzer:\n");
    CHECK_NEAR("cell identity", json_number(json, "lte", "cell_id"), 301, 0);
    CHECK_NEAR("resource blocks", json_number(json, "lte", "n_rb"), 25, 0);
    CHECK_NEAR("antenna ports", json_number(json, "lte", "ports"), 1, 0);
    CHECK_NEAR("carrier offset (Hz)", json_number(json, "lte", "carrier_offset_hz"), 0.0, 2.0);
    CHECK("reference signal EVM under 1 %", json_number(json, "lte", "rs_evm_percent") < 1.0);
    CHECK("data EVM under 1 %", json_number(json, "lte", "data_evm_percent") < 1.0);
    CHECK("data blocks", json_number(json, "lte", "data_blocks") >= 20 * 12 - 12);
    return report_done("LTE");
}

/* DVB-T */
/*-------*/

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
        const struct m2sdr_dvbt_config cfg = { 8192, 8, M2SDR_MOD_64QAM, true, 0 };
        const size_t count = make_dvbt(x, max, fs, &sig);

        (void)count;
        printf(" 40 ms, mode and constellation given\n");
        CHECK("analyse", m2sdr_dvbt_analyse(x, (size_t)(0.04 * fs), fs, &cfg, res) == 0 && res->pilots);
        CHECK("64QAM", res->modulation == M2SDR_MOD_64QAM);
        CHECK_NEAR("MER (dB)", res->mer_db, 21.0, 1.5);
    }
    /* A known offset: searched around it only, the 1/6 MHz one found again. */
    {
        const struct dvbt_signal sig = { 8192, 8, 2, 2, 10, 166667.0, 25.0, 0.0, -99.0 };
        const size_t count = make_dvbt(x, max, fs, &sig);

        CHECK("analyse", m2sdr_dvbt_analyse(x, count, fs, NULL, res) == 0 && res->pilots);
        {
            const struct m2sdr_dvbt_config cfg = { 8192, 8, M2SDR_MOD_64QAM, true, res->carrier_offset };

            printf(" 40 ms, offset known (%d carriers)\n", cfg.carrier_offset);
            CHECK("analyse", m2sdr_dvbt_analyse(x, (size_t)(0.04 * fs), fs, &cfg, res) == 0 && res->pilots);
            CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, sig.freq_hz, 20.0);
            CHECK_NEAR("MER (dB)", res->mer_db, 24.0, 1.5);
        }
        {
            const struct m2sdr_dvbt_config cfg = { 8192, 8, M2SDR_MOD_64QAM, true, 0 };

            printf(" 40 ms, a wrong known offset\n");
            CHECK("analyse", m2sdr_dvbt_analyse(x, (size_t)(0.04 * fs), fs, &cfg, res) == 0 && res->pilots);
            CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, sig.freq_hz, 20.0);
        }
    }
    memset(x, 0, max * sizeof(*x));
    add_noise(x, max, 0.01);
    CHECK("noise", m2sdr_dvbt_analyse(x, max, fs, NULL, res) == 0 && !res->found);
    CHECK("bad sample rate", m2sdr_dvbt_analyse(x, max, 10e6, NULL, res) != 0);
    CHECK("too short", m2sdr_dvbt_analyse(x, 50000, fs, NULL, res) != 0);
    free(x);
    free(res);
}

/* DAB */
/*-----*/

/* DAB mode I frames as EN 300 401 builds them: null symbol, phase reference, 75 symbols of
 * pi/4-DQPSK with random data in place of the FIC and the subchannels. */
struct dab_signal {
    size_t start;               /* Sample of the frame the capture starts with. */
    double freq_hz;
    double snr_db;
    double echo_us, echo_db;    /* A second path; echo_db -99 for none. */
};

static size_t make_dab(float complex *x, size_t max, const struct dab_signal *sig)
{
    enum { N = 2048, G = 504, L = 2552, NUL = 2656, FRAME = 196608, K = 1536 };
    const double fs = M2SDR_DAB_SAMPLE_RATE;
    static const float complex turns[4] = { 0.70710678f + 0.70710678f * I, -0.70710678f + 0.70710678f * I,
                                            -0.70710678f - 0.70710678f * I, 0.70710678f - 0.70710678f * I };
    struct m2sdr_fft *ifft = m2sdr_fft_alloc(N, true);
    float complex *f = malloc(N * sizeof(*f)), *t = malloc(N * sizeof(*t)), *z = malloc(K * sizeof(*z));
    float complex prs[K];
    const size_t total = max + sig->start, nframes = total / FRAME + 1;
    float complex *all = calloc(nframes * FRAME, sizeof(*all));
    const size_t echo = (size_t)lround(sig->echo_us * 1e-6 * fs);
    const double echo_amp = pow(10.0, sig->echo_db / 20.0);
    size_t fr, i;
    double power = 0;
    int s, c;

    m2sdr_dab_phase_reference(prs);
    for (fr = 0; fr < nframes; fr++) {
        float complex *a = all + fr * FRAME;

        for (s = 0; s < 76; s++) {
            memset(f, 0, N * sizeof(*f));
            for (c = 0; c < K; c++) {
                const int k = c < 768 ? c - 768 : c - 767;

                z[c] = s == 0 ? prs[c] : z[c] * turns[(int)(rng_uniform() * 4) & 3];
                f[(k + N) % N] = z[c];
            }
            m2sdr_fft_run(ifft, f, t);
            for (i = 0; i < (size_t)L; i++)
                a[NUL + (size_t)s * L + i] = t[(i + N - G) % N];
        }
    }
    if (sig->echo_db > -99.0)
        for (i = nframes * FRAME; i-- > echo;)
            all[i] += (float)echo_amp * all[i - echo];
    for (i = 0; i < max; i++) {
        x[i] = all[i + sig->start] * (float complex)cexp(I * 2.0 * M_PI * sig->freq_hz * (double)i / fs);
        power += crealf(x[i] * conjf(x[i]));
    }
    /* Power of the symbols: the null symbols take 1.35 % of the time. */
    power /= (double)max * (1.0 - (double)NUL / FRAME);
    for (i = 0; i < max; i++)
        x[i] *= (float)(0.1 / sqrt(power));
    add_noise(x, max, 0.01 * pow(10.0, -sig->snr_db / 10.0));
    m2sdr_fft_free(ifft);
    free(f);
    free(t);
    free(z);
    free(all);
    return max;
}

static void test_dab(void)
{
    static const struct dab_signal signals[] = {
        /* start    freq_hz   snr  echo_us echo_db */
        {      0,       0.0, 25.0,   0.0, -99.0 },
        {  77777,  -12345.6, 20.0,   0.0, -99.0 },
        { 150000,   97000.0, 15.0,  30.0,  -6.0 },   /* Offset, an echo inside the guard interval. */
        {  20000,     300.0, 10.0,   0.0, -99.0 },
    };
    const double fs = M2SDR_DAB_SAMPLE_RATE;
    const size_t max = (size_t)(0.2 * fs);
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_dab_result *res = malloc(sizeof(*res));
    size_t i;

    printf("DAB:\n");
    if (!x || !res) {
        CHECK("allocation", 0);
        free(x);
        free(res);
        return;
    }
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        const struct dab_signal *sig = &signals[i];
        const double a2 = sig->echo_db > -99.0 ? pow(10.0, sig->echo_db / 10.0) : 0.0;

        make_dab(x, max, sig);
        printf(" start %zu, offset %.1f Hz, SNR %.0f dB%s\n", sig->start, sig->freq_hz, sig->snr_db,
               sig->echo_db > -99.0 ? ", echo" : "");
        CHECK("analyse", m2sdr_dab_analyse(x, max, fs, res) == 0);
        CHECK("found, phase reference", res->found && res->prs);
        CHECK("frames", res->frames >= 1);
        CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, sig->freq_hz, 5.0);
        /* The noise is over 2.048 MHz, the carriers over 1.536 MHz: 1.25 dB more on them. With
         * an echo of amplitude a, the equalised carriers keep (1 - a^2) / (1 + a^2) of their SNR. */
        CHECK_NEAR("SNR on the carriers (dB)", res->null_snr_db, sig->snr_db + 1.25, 0.5);
        CHECK_NEAR("MER (dB)", res->mer_db, sig->snr_db + 1.25 + 10.0 * log10((1.0 - a2) / (1.0 + a2)), 1.0);
        if (sig->echo_db > -99.0) {
            CHECK_NEAR("echo delay (us)", res->echo_delay_us, sig->echo_us, 0.5);
            CHECK_NEAR("echo level (dB)", res->echo_level_db, sig->echo_db, 1.5);
        }
    }
    memset(x, 0, max * sizeof(*x));
    add_noise(x, max, 0.01);
    CHECK("noise", m2sdr_dab_analyse(x, max, fs, res) == 0 && !res->found);
    CHECK("bad sample rate", m2sdr_dab_analyse(x, max, 2.4e6, res) != 0);
    CHECK("too short", m2sdr_dab_analyse(x, 100000, fs, res) != 0);
    free(x);
    free(res);
}

static int write_dvbt(const char *path)
{
    static const struct dvbt_signal sig = { 8192, 8, 2, 2, 20, 166667.0, 30.0, 0.0, -99.0 };
    const double fs = 64e6 / 7.0;
    const size_t max = (size_t)(0.16 * fs);
    float complex *x = malloc(max * sizeof(*x));
    size_t count;
    int rc;

    if (!x)
        return 1;
    count = make_dvbt(x, max, fs, &sig);
    rc    = write_sc16(path, x, x, count);
    free(x);
    return rc;
}

static int check_dvbt_report(const char *path)
{
    const char *json = read_report(path);

    if (!json)
        return 1;
    printf("DVB-T report of m2sdr_analyzer:\n");
    CHECK("mode 8k, guard 1/8", strstr(json, "\"mode\": \"8k\"") && strstr(json, "\"guard_interval\": \"1/8\""));
    CHECK("64QAM, code rate 3/4", strstr(json, "\"constellation\": \"64QAM\"") && strstr(json, "\"code_rate_hp\": \"3/4\""));
    CHECK_NEAR("carrier offset (Hz)", json_number(json, "dvbt", "carrier_offset_hz"), 166667.0, 20.0);
    CHECK("MER over 25 dB", json_number(json, "dvbt", "mer_db") > 25.0);
    return report_done("DVB-T");
}

static int write_dab(const char *path)
{
    static const struct dab_signal sig = { 50000, 4321.0, 25.0, 0.0, -99.0 };
    const size_t max = (size_t)(0.25 * M2SDR_DAB_SAMPLE_RATE);
    float complex *x = malloc(max * sizeof(*x));
    int rc;

    if (!x)
        return 1;
    make_dab(x, max, &sig);
    rc = write_sc16(path, x, x, max);
    free(x);
    return rc;
}

static int check_dab_report(const char *path)
{
    const char *json = read_report(path);

    if (!json)
        return 1;
    printf("DAB report of m2sdr_analyzer:\n");
    CHECK("ensemble and phase reference", strstr(json, "\"found\": true") && strstr(json, "\"phase_reference\": true"));
    CHECK_NEAR("carrier offset (Hz)", json_number(json, "dab", "carrier_offset_hz"), 4321.0, 5.0);
    CHECK("MER over 24 dB", json_number(json, "dab", "mer_db") > 24.0);
    CHECK("SNR over 24 dB", json_number(json, "dab", "snr_db") > 24.0);
    return report_done("DAB");
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
    if (argc == 3 && strcmp(argv[1], "--write-dab") == 0)
        return write_dab(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check-dab") == 0)
        return check_dab_report(argv[2]);

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
    test_lte_evm();
    test_dvbt();
    test_dab();
    return tests_done();
}
