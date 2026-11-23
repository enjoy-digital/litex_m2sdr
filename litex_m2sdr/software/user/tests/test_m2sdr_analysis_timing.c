/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the libm2sdr timing analysis (Allan deviation, GPS L1 C/A acquisition, jamming) on
 * synthetic signals of known parameters.
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

#include "../libm2sdr/m2sdr_analysis_timing.h"

static int failures;

#define CHECK_NEAR(name, value, expected, tol) do { \
    const double v_ = (value), e_ = (expected); \
    const int ok_ = fabs(v_ - e_) <= (tol); \
    printf("  %-34s %12.4g (expected %10.4g +-%g) %s\n", name, v_, e_, (double)(tol), ok_ ? "ok" : "FAIL"); \
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

static double rng_gauss(void)
{
    return sqrt(-2.0 * log(rng_uniform())) * cos(2.0 * M_PI * rng_uniform());
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

/* Allan deviation */
/*-----------------*/

#define ADEV_FS      100e3
#define ADEV_F0      10e6      /* Nominal carrier: a 10 MHz reference tone. */
#define ADEV_OFFSET  12345.6   /* Its place in the capture. */

/* Carrier of amplitude 1 whose frequency, from ADEV_OFFSET, is white with sigma_white Hz per
 * sample plus a random walk of sigma_walk Hz per sample; phase from the sum of the frequency. */
static void make_carrier(float complex *x, size_t count, double sigma_white, double sigma_walk, double *phase,
                         double *walk)
{
    size_t i;

    for (i = 0; i < count; i++) {
        double f;

        *walk += sigma_walk * rng_gauss();
        f      = ADEV_OFFSET + *walk + sigma_white * rng_gauss();
        x[i]   = (float complex)cexp(I * *phase);
        *phase = fmod(*phase + 2.0 * M_PI * f / ADEV_FS, 2.0 * M_PI);
    }
}

static void test_carrier(void)
{
    const size_t count = 65536;
    const struct m2sdr_carrier_config cfg = { ADEV_F0, ADEV_OFFSET, 0.0, 0 };
    float complex *x = malloc(count * sizeof(*x));
    struct m2sdr_carrier_result res;
    double sum = 0.0, sum2 = 0.0, crlb = 0.0, t;
    const int trials = 60;
    int i;

    printf("Carrier estimate:\n");
    if (!x) {
        CHECK("allocation", 0);
        return;
    }
    /* Tone of -10 dBFS, 20 dB SNR, known frequency and phase at the centre of the capture. */
    for (i = 0; i < trials; i++) {
        const double f   = ADEV_OFFSET + 3.21;
        const double ph0 = 2.0 * M_PI * rng_uniform() - M_PI;
        const double tc  = 0.5 * (double)(count - 1);
        size_t n;

        for (n = 0; n < count; n++)
            x[n] = (float complex)(sqrt(0.1) * cexp(I * (ph0 + 2.0 * M_PI * f * ((double)n - tc) / ADEV_FS)));
        add_noise(x, count, 0.001);
        if (m2sdr_carrier_estimate(x, count, ADEV_FS, 1000, &cfg, &res) != 0 || !res.found) {
            CHECK("estimate", 0);
            break;
        }
        sum  += res.freq_hz - f;
        sum2 += (res.freq_hz - f) * (res.freq_hz - f);
        crlb  = res.freq_std_hz;
        if (i == 0) {
            CHECK("found", res.found);
            CHECK_NEAR("timestamp", res.timestamp, 1000.0 + tc, 1e-9);
            /* Cramer-Rao bound at 20 dB over 65536 samples: 6e-5 Hz; the phase at the centre is
             * independent of the frequency, sigma = 1 / sqrt(2 N SNR) = 0.3 mrad. */
            CHECK_NEAR("frequency (Hz)", res.freq_hz, f, 5.0 * res.freq_std_hz);
            CHECK_NEAR("mean frequency (Hz)", res.freq_mean_hz, f, 0.015);
            CHECK_NEAR("phase (rad)", remainder(res.phase_rad - ph0, 2.0 * M_PI), 0.0, 0.003);
            CHECK_NEAR("level (dBFS)", res.level_dbfs, -10.0, 0.05);
            CHECK_NEAR("SNR (dB)", res.snr_db, 20.0, 0.1);
            CHECK_NEAR("C/N0 (dB-Hz)", res.cn0_dbhz, 70.0, 0.1);
            CHECK_NEAR("offset (ppb)", res.offset_ppb, 3.21 / ADEV_F0 * 1e9, 1.5);
            CHECK("blocks", res.blocks >= 8 && res.blocks <= M2SDR_ADEV_MAX_BLOCKS);
        }
    }
    /* Spread over the trials against the bound: 60 trials know a deviation to 9 %. */
    printf("  frequency spread %.3g Hz, mean %.3g Hz, Cramer-Rao bound %.3g Hz\n",
           sqrt(sum2 / trials), sum / trials, crlb);
    CHECK_NEAR("spread over Cramer-Rao bound", sqrt(sum2 / trials) / crlb, 1.0, 0.35);

    /* Short taus from the additive noise: white phase. A sub-block of B samples has a phase error
     * of sigma^2 / (2 B A^2) rad^2, so adev(tau) = sqrt(3) sigma_x / tau and, at m sub-blocks,
     * mdev = sqrt(3 / m) sigma_x / tau. */
    {
        const double sx = sqrt(0.001 / (2.0 * (double)res.block_len * 0.1)) / (2.0 * M_PI * ADEV_F0);
        const struct m2sdr_adev *d = &res.short_tau;

        printf(" short taus: %d sub-blocks of %zu samples\n", res.blocks, res.block_len);
        CHECK("taus", d->ntau >= 6);
        CHECK_NEAR("tau0 (s)", d->tau_s[0], (double)res.block_len / ADEV_FS, 1e-12);
        CHECK_NEAR("adev(tau0) / white PM", d->adev[0] / (sqrt(3.0) * sx / d->tau_s[0]), 1.0, 0.1);
        CHECK_NEAR("adev(4 tau0) / white PM", d->adev[2] / (sqrt(3.0) * sx / d->tau_s[2]), 1.0, 0.12);
        CHECK_NEAR("mdev(4 tau0) / white PM", d->mdev[2] / (sqrt(3.0 / 4.0) * sx / d->tau_s[2]), 1.0, 0.15);
        CHECK_NEAR("mdev(tau0) = adev(tau0)", d->mdev[0], d->adev[0], 1e-6 * d->adev[0]);
    }

    /* Noise only, bad arguments. */
    memset(x, 0, count * sizeof(*x));
    add_noise(x, count, 0.01);
    CHECK("noise", m2sdr_carrier_estimate(x, count, ADEV_FS, 0, &cfg, &res) == 0 && !res.found);
    CHECK("too short", m2sdr_carrier_estimate(x, 1000, ADEV_FS, 0, &cfg, &res) != 0);
    CHECK("bad sample rate", m2sdr_carrier_estimate(x, count, 0.0, 0, &cfg, &res) != 0);
    CHECK("NULL samples", m2sdr_carrier_estimate(NULL, count, ADEV_FS, 0, &cfg, &res) != 0);

    t = now_s();
    for (i = 0; i < 20; i++)
        m2sdr_carrier_estimate(x, count, ADEV_FS, 0, NULL, &res);
    printf("  estimate of 65536 samples: %.2f ms\n", (now_s() - t) / 20 * 1e3);
    free(x);
}

/* Fit of log10(dev) over log10(tau) from index i0 to i1: the slope of a power law. */
static double dev_slope(const struct m2sdr_adev *d, const double *dev, int i0, int i1)
{
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int i, n = i1 - i0 + 1;

    for (i = i0; i <= i1; i++) {
        const double u = log10(d->tau_s[i]), v = log10(dev[i]);

        sx += u; sy += v; sxx += u * u; sxy += u * v;
    }
    return (sxy - sx * sy / n) / (sxx - sx * sx / n);
}

/* Contiguous captures of a carrier with frequency noise, one estimate each, into a series. */
static int run_series(struct m2sdr_adev_series *s, size_t len, int captures, double sigma_white, double sigma_walk)
{
    const struct m2sdr_carrier_config cfg = { ADEV_F0, ADEV_OFFSET, 2000.0, 0 };
    float complex *x = malloc(len * sizeof(*x));
    struct m2sdr_carrier_result res;
    double phase = 0.0, walk = 0.0;
    int c;

    if (!x)
        return -1;
    m2sdr_adev_series_reset(s);
    for (c = 0; c < captures; c++) {
        make_carrier(x, len, sigma_white, sigma_walk, &phase, &walk);
        add_noise(x, len, 1e-8);   /* 80 dB SNR: its white PM is far below the FM. */
        if (m2sdr_carrier_estimate(x, len, ADEV_FS, (int64_t)c * (int64_t)len, &cfg, &res) != 0 || !res.found) {
            free(x);
            return -1;
        }
        m2sdr_adev_series_add(s, res.timestamp, res.freq_mean_hz);
    }
    free(x);
    return 0;
}

static void test_series(void)
{
    const size_t len = 4096;
    const int captures = 256;
    const double tau0 = (double)len / ADEV_FS;
    struct m2sdr_adev_series s;
    struct m2sdr_adev_result *r = malloc(sizeof(*r));
    double t;
    int i;

    printf("Allan deviation over captures:\n");
    if (!r || m2sdr_adev_series_init(&s, ADEV_FS, ADEV_F0, ADEV_OFFSET, 1024) != 0) {
        CHECK("init", 0);
        free(r);
        return;
    }

    /* White FM of sigma_f Hz per sample: the mean over tau has sigma_f / sqrt(tau fs), and
     * adev = that over f0 (h0 = 2 sigma_f^2 / (fs f0^2)). Slope -1/2; mdev / adev tends to
     * 1 / sqrt(2). Overlapping estimates over 256 points know adev(m tau0) to about
     * sqrt(m / 256), 6 % at m = 1, 25 % at m = 16: the tolerances are three times that. */
    {
        const double sigma_f = 0.5;

        printf(" white FM, %g Hz per sample\n", sigma_f);
        CHECK("captures", run_series(&s, len, captures, sigma_f, 0.0) == 0);
        CHECK("compute", m2sdr_adev_series_compute(&s, r) == 0);
        CHECK("regular", r->regular);
        CHECK_NEAR("tau0 (s)", r->tau0_s, tau0, 1e-9);
        CHECK_NEAR("offset (ppb)", r->offset_ppb, 0.0, 3.0 * sigma_f / sqrt(len * captures) / ADEV_F0 * 1e9);
        for (i = 0; i <= 4; i++) {
            const double th = sigma_f / sqrt(r->dev.tau_s[i] * ADEV_FS) / ADEV_F0;
            char name[48];

            snprintf(name, sizeof(name), "adev(%g s) / theory", r->dev.tau_s[i]);
            CHECK_NEAR(name, r->dev.adev[i] / th, 1.0, 3.0 * sqrt((double)(1 << i) / captures));
        }
        CHECK_NEAR("adev slope", dev_slope(&r->dev, r->dev.adev, 0, 4), -0.5, 0.15);
        CHECK_NEAR("mdev / adev at 8 tau0", r->dev.mdev[3] / r->dev.adev[3], sqrt(0.5), 0.12);
    }

    /* Random walk FM, a step of q Hz per sample: diffusion D = q^2 fs, adev^2 = D tau / 3 over
     * f0^2. Slope +1/2. A random walk gives few degrees of freedom: 35 % and slope +-0.15. */
    {
        const double q = 2e-3;

        printf(" random walk FM, %g Hz per sample\n", q);
        rng_state = 0x9e3779b97f4a7c15ull;
        CHECK("captures", run_series(&s, len, captures, 0.0, q) == 0);
        CHECK("compute", m2sdr_adev_series_compute(&s, r) == 0);
        for (i = 1; i <= 4; i++) {
            const double th = sqrt(q * q * ADEV_FS * r->dev.tau_s[i] / 3.0) / ADEV_F0;
            char name[48];

            snprintf(name, sizeof(name), "adev(%g s) / theory", r->dev.tau_s[i]);
            CHECK_NEAR(name, r->dev.adev[i] / th, 1.0, 0.35);
        }
        CHECK_NEAR("adev slope", dev_slope(&r->dev, r->dev.adev, 0, 5), 0.5, 0.15);
        CHECK_NEAR("mdev slope", dev_slope(&r->dev, r->dev.mdev, 0, 4), 0.5, 0.2);
    }

    /* Linear drift, fed directly: offset and drift exact, adev = D tau / sqrt(2), the series
     * kept to its last 1024 points. */
    {
        const double drift = 0.5, offset = 20.0;   /* ppb/s, ppb. */

        printf(" linear drift %g ppb/s, offset %g ppb\n", drift, offset);
        m2sdr_adev_series_reset(&s);
        for (i = 0; i < 1500; i++) {
            const double ts = (double)i * len;
            /* Offset at the middle of the 1024 points kept, 476 to 1499. */
            const double y  = (offset + drift * (ts / ADEV_FS - 987.5 * tau0)) * 1e-9;

            m2sdr_adev_series_add(&s, ts, ADEV_OFFSET + y * ADEV_F0);
        }
        CHECK("compute", m2sdr_adev_series_compute(&s, r) == 0);
        CHECK("last 1024 points", r->points == 1024);
        CHECK_NEAR("offset (ppb)", r->offset_ppb, offset, 1e-6);
        CHECK_NEAR("drift (ppb/s)", r->drift_ppb_s, drift, 1e-6);
        CHECK_NEAR("adev(tau0) / D tau / sqrt 2", r->dev.adev[0] / (drift * 1e-9 * tau0 / sqrt(2.0)), 1.0, 1e-4);
        CHECK_NEAR("adev(64 tau0) / D tau / sqrt 2", r->dev.adev[6] / (drift * 1e-9 * 64 * tau0 / sqrt(2.0)), 1.0, 1e-4);
        m2sdr_adev_series_add(&s, 1e12, ADEV_OFFSET);   /* A capture lost before this one. */
        CHECK("irregular", m2sdr_adev_series_compute(&s, r) == 0 && !r->regular);

        t = now_s();
        for (i = 0; i < 20; i++)
            m2sdr_adev_series_compute(&s, r);
        printf("  compute of 1024 points: %.3f ms\n", (now_s() - t) / 20 * 1e3);
    }

    m2sdr_adev_series_reset(&s);
    m2sdr_adev_series_add(&s, 0.0, 0.0);
    m2sdr_adev_series_add(&s, 1.0, 0.0);
    CHECK("two points", m2sdr_adev_series_compute(&s, r) != 0);
    m2sdr_adev_series_free(&s);
    CHECK("no carrier frequency", m2sdr_adev_series_init(&s, ADEV_FS, 0.0, 0.0, 16) != 0);
    CHECK("capacity 2", m2sdr_adev_series_init(&s, ADEV_FS, ADEV_F0, 0.0, 2) != 0);
    m2sdr_adev_series_free(&s);
    free(r);
}

/* GPS L1 C/A */
/*------------*/

/* First 10 chips of each PRN, octal, the first chip the highest bit (IS-GPS-200, table 3-Ia). */
static const unsigned ca_first10[M2SDR_GNSS_PRNS] = {
    01440, 01620, 01710, 01744, 01133, 01455, 01131, 01454, 01626, 01504, 01642, 01750, 01764, 01772, 01775, 01776,
    01156, 01467, 01633, 01715, 01746, 01763, 01063, 01706, 01743, 01761, 01770, 01774, 01127, 01453, 01625, 01712,
};

static void test_ca_codes(void)
{
    uint8_t c[M2SDR_GNSS_CA_CHIPS], c2[M2SDR_GNSS_CA_CHIPS];
    int p, i, ok_first = 1, ok_balance = 1, worst = 0;

    printf("C/A codes:\n");
    for (p = 1; p <= M2SDR_GNSS_PRNS; p++) {
        unsigned v = 0;
        int ones = 0;

        m2sdr_gnss_ca_code(p, c);
        for (i = 0; i < 10; i++)
            v = (v << 1) | c[i];
        for (i = 0; i < M2SDR_GNSS_CA_CHIPS; i++)
            ones += c[i];
        if (v != ca_first10[p - 1]) {
            printf("  PRN %d: first chips %04o, expected %04o\n", p, v, ca_first10[p - 1]);
            ok_first = 0;
        }
        if (ones != 512)
            ok_balance = 0;
    }
    CHECK("first 10 chips, PRN 1 to 32", ok_first);
    CHECK("512 ones, 511 zeros", ok_balance);
    /* Gold codes: the cross-correlation of two codes takes the values -1, -65 and 63. */
    m2sdr_gnss_ca_code(1, c);
    m2sdr_gnss_ca_code(2, c2);
    for (i = 0; i < M2SDR_GNSS_CA_CHIPS; i++) {
        int s = 0, k;

        for (k = 0; k < M2SDR_GNSS_CA_CHIPS; k++)
            s += (c[k] ^ c2[(k + i) % M2SDR_GNSS_CA_CHIPS]) ? -1 : 1;
        if (s != -1 && s != -65 && s != 63)
            worst = s;
    }
    CHECK("cross-correlation in {-1, -65, 63}", worst == 0);
    CHECK("PRN 0", m2sdr_gnss_ca_code(0, c) != 0);
    CHECK("PRN 33", m2sdr_gnss_ca_code(33, c) != 0);
}

struct gps_sat {
    int    prn;
    double cn0_dbhz;
    double doppler_hz;
    double code_chips;     /* Delay of a code epoch from the first sample. */
};

/* Satellites in complex noise of power `noise`: each C/A code with its code Doppler, 50 bit/s
 * data on code epochs, a random carrier phase. A^2 = C/N0 noise / fs. */
static void make_gps(float complex *x, size_t count, double fs, const struct gps_sat *sats, int nsats, double noise)
{
    uint8_t chips[M2SDR_GNSS_CA_CHIPS];
    int s;

    memset(x, 0, count * sizeof(*x));
    for (s = 0; s < nsats; s++) {
        const double amp  = sqrt(pow(10.0, sats[s].cn0_dbhz / 10.0) * noise / fs);
        const double rate = M2SDR_GNSS_CA_RATE * (1.0 + sats[s].doppler_hz / M2SDR_GNSS_L1_HZ);
        const double tau  = sats[s].code_chips / M2SDR_GNSS_CA_RATE * fs;   /* Samples. */
        const double ph0  = 2.0 * M_PI * rng_uniform();
        int bits[64];
        size_t i;

        for (i = 0; i < 64; i++)
            bits[i] = rng_uniform() < 0.5 ? -1 : 1;
        m2sdr_gnss_ca_code(sats[s].prn, chips);
        for (i = 0; i < count; i++) {
            const double cp  = ((double)i - tau) * rate / fs + 20.0 * M2SDR_GNSS_CA_CHIPS;   /* Never negative. */
            const long chip  = (long)floor(cp);
            const long epoch = chip / M2SDR_GNSS_CA_CHIPS;
            const double v   = (chips[chip % M2SDR_GNSS_CA_CHIPS] ? -1.0 : 1.0) * bits[(epoch / 20) % 64];

            x[i] += (float complex)(amp * v * cexp(I * (ph0 + 2.0 * M_PI * sats[s].doppler_hz * (double)i / fs)));
        }
    }
    add_noise(x, count, noise);
}

static void check_sats(const struct m2sdr_gnss_result *res, const struct gps_sat *sats, int nsats, double fs)
{
    int i, s, extra = 0;

    for (s = 0; s < nsats; s++) {
        const struct m2sdr_gnss_prn *o = NULL;
        /* Per millisecond SNR of the prompt: C/N0 T. The differential Doppler has a phase noise
         * of about sqrt((1 / snr + 1 / (2 snr^2)) / (ms - 1)) rad over 1 ms; tolerance four
         * times that. C/N0 from ms prompts: a relative error of sqrt((2 snr + 1) / ms) / snr,
         * plus up to 1 dB from a code phase off by a tenth of a chip. */
        const double snr = pow(10.0, sats[s].cn0_dbhz / 10.0) * 1e-3;
        const double sd  = sqrt((1.0 / snr + 0.5 / (snr * snr)) / (res->ms - 1)) / (2.0 * M_PI * 1e-3);
        const double sc  = 10.0 * log10(1.0 + 3.0 * sqrt((2.0 * snr + 1.0) / res->ms) / snr) + 1.0;
        char name[64];

        for (i = 0; i < res->nprn; i++)
            if (res->prn[i].prn == sats[s].prn)
                o = &res->prn[i];
        snprintf(name, sizeof(name), "PRN %d detected", sats[s].prn);
        CHECK(name, o && o->detected);
        if (!o || !o->detected)
            continue;
        printf("   metric %.1f dB\n", o->metric_db);
        CHECK_NEAR("    C/N0 (dB-Hz)", o->cn0_dbhz, sats[s].cn0_dbhz, sc);
        CHECK_NEAR("    Doppler (Hz)", o->doppler_hz, sats[s].doppler_hz, 4.0 * sd + 5.0);
        CHECK_NEAR("    code phase (chips)", remainder(o->code_phase_chips - sats[s].code_chips, 1023.0), 0.0,
                   sats[s].cn0_dbhz >= 40.0 ? 0.1 : 0.2);
        CHECK_NEAR("    code phase (samples)", remainder((double)o->code_phase_samples -
                   sats[s].code_chips / M2SDR_GNSS_CA_RATE * fs, fs / 1000.0), 0.0, 1.0);
    }
    for (i = 0; i < res->nprn; i++) {
        int known = 0;

        for (s = 0; s < nsats; s++)
            known |= res->prn[i].prn == sats[s].prn;
        if (res->prn[i].detected && !known) {
            printf("  PRN %d detected, metric %.1f dB\n", res->prn[i].prn, res->prn[i].metric_db);
            extra++;
        }
    }
    CHECK("no other PRN", extra == 0);
}

static void test_gnss(void)
{
    static const struct gps_sat sats[] = {
        /* prn  C/N0   Doppler   code */
        {  3,   48.0,  1234.0,   100.3 },
        { 17,   44.0, -3210.0,   555.6 },
        { 29,   41.0,  4400.0,   900.1 },
        /* Worst place of the search: between two Doppler bins (0.9 dB) and half a sample from
         * the code phase of the search (2.5 dB). 10 ms detect it from about 40 dB-Hz. */
        {  8,   40.0,  -750.0,    12.75 },
    };
    static const struct gps_sat weak[] = {
        { 11,   35.0,  2650.0,   333.3 },
    };
    const double noise = 0.01;   /* -20 dBFS. */
    const size_t max = (size_t)(0.025 * 4.096e6);
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_gnss_result *res = malloc(sizeof(*res));
    struct m2sdr_gnss_config cfg;
    double t;
    int r;

    printf("GPS L1 C/A acquisition:\n");
    if (!x || !res) {
        CHECK("allocation", 0);
        free(x);
        free(res);
        return;
    }
    for (r = 0; r < 2; r++) {
        const double fs = r ? 4.096e6 : 2.048e6;
        const size_t count = (size_t)(0.01 * fs);

        printf(" %.3f MSPS, 10 ms:\n", fs / 1e6);
        make_gps(x, count, fs, sats, 4, noise);
        t = now_s();
        CHECK("analyse", m2sdr_gnss_analyse(x, count, fs, NULL, res) == 0);
        printf("  analysis: %.1f ms\n", (now_s() - t) * 1e3);
        printf("  threshold %.1f dB\n", res->threshold_db);
        CHECK("found", res->found && res->detected == 4);
        check_sats(res, sats, 4, fs);
        CHECK("strongest PRN", res->best >= 0 && res->prn[res->best].prn == 3);
        CHECK("points", res->npoints == 10);
        if (res->best >= 0) {
            int j, bpsk = 1;

            /* At 48 dB-Hz a prompt has an SNR of 18 dB: within 0.4 of +-1. */
            for (j = 0; j < res->npoints; j++)
                bpsk &= fabsf(fabsf(crealf(res->points[j])) - 1.0f) < 0.4f && fabsf(cimagf(res->points[j])) < 0.4f;
            CHECK("points on +-1", bpsk);
            CHECK("code trace peak", res->code_db[(int)floor(100.3 + 0.5) % 1023] > res->threshold_db ||
                                     res->code_db[100] > res->threshold_db);
        }
        CHECK("no jamming", !res->jamming.jammed);
    }

    /* 35 dB-Hz: 20 ms, a subset of PRNs. */
    printf(" 2.048 MSPS, 20 ms, 35 dB-Hz:\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.ms       = 20;
    cfg.prn_mask = 0x0000ffffu;
    make_gps(x, (size_t)(0.02 * 2.048e6), 2.048e6, weak, 1, noise);
    CHECK("analyse", m2sdr_gnss_analyse(x, (size_t)(0.02 * 2.048e6), 2.048e6, &cfg, res) == 0);
    CHECK("16 PRNs searched", res->nprn == 16);
    check_sats(res, weak, 1, 2.048e6);

    /* Noise: no detection for a false alarm probability of 1e-3 over the search. */
    printf(" noise only:\n");
    memset(x, 0, max * sizeof(*x));
    add_noise(x, max, noise);
    CHECK("analyse", m2sdr_gnss_analyse(x, 20480, 2.048e6, NULL, res) == 0);
    CHECK("nothing detected", !res->found && res->detected == 0);
    CHECK("no jamming", !res->jamming.jammed);
    CHECK_NEAR("power (dBFS)", res->jamming.power_dbfs, -20.0, 0.1);
    CHECK_NEAR("noise density (dBFS/Hz)", res->jamming.noise_dbfs_hz, -20.0 - 10.0 * log10(2.048e6), 0.3);
    /* The log of an average of about 40 periodograms is biased by about -1/(2 * 40) nepers. */
    CHECK_NEAR("flatness (dB)", res->jamming.flatness_db, 0.0, 0.3);
    CHECK("J/N below -10 dB", res->jamming.jn_db < -10.0);

    /* CW jammer 20 dB over the noise of the band (80 % of 2.048 MHz) at +300 kHz, with the
     * satellites: the line is found, its J/N measured, the spectrum is no longer flat. */
    printf(" CW jammer, J/N 20 dB at +300 kHz:\n");
    {
        const double fs = 2.048e6, jn = 100.0;
        const double amp = sqrt(jn * noise * 0.8);
        size_t i;

        make_gps(x, 20480, fs, sats, 4, noise);
        for (i = 0; i < 20480; i++)
            x[i] += (float complex)(amp * cexp(I * 2.0 * M_PI * 300e3 * (double)i / fs));
        CHECK("analyse", m2sdr_gnss_analyse(x, 20480, fs, NULL, res) == 0);
        CHECK("jammed", res->jamming.jammed);
        CHECK("one line", res->jamming.njammers == 1);
        if (res->jamming.njammers >= 1) {
            CHECK_NEAR("line (Hz)", res->jamming.jammer_hz[0], 300e3, 2000.0);
            CHECK_NEAR("line J/N (dB)", res->jamming.jammer_jn_db[0], 20.0, 0.5);
            CHECK_NEAR("line power (dBFS)", res->jamming.jammer_dbfs[0], 10.0 * log10(amp * amp), 0.5);
        }
        CHECK_NEAR("J/N (dB)", res->jamming.jn_db, 20.0, 0.5);
        CHECK("flatness below -10 dB", res->jamming.flatness_db < -10.0);
        /* The despread CW raises the noise of the correlations by about J / (N0 * 1.023 MHz *
         * its spectral line), but the 48 dB-Hz satellite stays well over the threshold. */
        /* Its line notched out, the four satellites are found and nothing else: unremoved, the
         * CW correlates with the lines of the C/A spectrum at whole kHz of Doppler. */
        CHECK("line excised", res->excised == 1);
        check_sats(res, sats, 4, fs);
    }

    CHECK("bad sample rate", m2sdr_gnss_analyse(x, max, 2.5e6, NULL, res) != 0);
    CHECK("too short", m2sdr_gnss_analyse(x, 10000, 2.048e6, NULL, res) != 0);
    cfg.ms = 500;
    CHECK("too many ms", m2sdr_gnss_analyse(x, max, 2.048e6, &cfg, res) != 0);
    CHECK("jamming too short", m2sdr_gnss_jamming(x, 1000, 2.048e6, &res->jamming) != 0);
    free(x);
    free(res);
}

int main(void)
{
    test_carrier();
    test_series();
    test_ca_codes();
    test_gnss();
    if (failures) {
        printf("test_m2sdr_analysis_timing: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis_timing: ok\n");
    return 0;
}
