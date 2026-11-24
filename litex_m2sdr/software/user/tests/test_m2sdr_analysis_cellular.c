/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the libm2sdr cellular analysis (5G NR SSB, GSM, TETRA) on synthetic signals built
 * to the standards, of known parameters.
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

#include "../libm2sdr/m2sdr_analysis_cellular.h"
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

static int rng_bit(void)
{
    return rng_uniform() < 0.5;
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

/* GSM */
/*-----*/

/* Training sequences of the normal bursts (TS 45.002 5.2.3), TSC 0 to 7. */
static const char *const gsm_tsc[8] = {
    "00100101110000100010010111", "00101101110111100010110111", "01000011101110100100001110",
    "01000111101101000100011110", "00011010111001000001101011", "01001110101100000100111010",
    "10100111110110001010011111", "11101111000100101110111100",
};

/* Integral of the GMSK frequency pulse (BT 0.3, TS 45.004 2.4) over a bit, u in bits. */
static double test_gmsk_q(double u)
{
    /* The Gaussian of the filter over the rectangle of a bit: the integral of the Gaussian
     * CDF, z Phi(z) + phi(z), between the ends of the bit. */
    const double s = sqrt(log(2.0)) / (2.0 * M_PI * 0.3);
    const double z1 = (u - 0.5) / s, z2 = (u + 0.5) / s;
    const double f1 = z1 * 0.5 * erfc(-z1 / M_SQRT2) + exp(-0.5 * z1 * z1) / sqrt(2.0 * M_PI);
    const double f2 = z2 * 0.5 * erfc(-z2 / M_SQRT2) + exp(-0.5 * z2 * z2) / sqrt(2.0 * M_PI);

    return s * (f2 - f1);
}

/* A BCCH carrier (C0) as TS 45.002 builds it: TS0 of the 51-multiframe with FCCH, SCH and
 * normal bursts, TS1 to 7 with normal bursts, every burst modulated on its own with a random
 * starting phase and ramped over its guard time. */
struct gsm_signal {
    double fs;
    int    bsic;
    int    fn0;                 /* Frame number at the start of the capture. */
    double start_bits;          /* Bits of that frame before the capture starts. */
    double freq_hz;
    double snr_db;              /* Over the noise in 340 kHz (the receive filter). */
};

#define GSM_AMP 0.3

static void make_gsm(float complex *x, size_t count, const struct gsm_signal *sig)
{
    const double R = M2SDR_GSM_SYMBOL_RATE;
    const double dur_bits = (double)count / sig->fs * R + sig->start_bits;
    const int frames = (int)(dur_bits / 1250.0) + 2;
    uint8_t coded[78];
    int f, j, k;
    size_t i;

    memset(x, 0, count * sizeof(*x));
    for (f = 0; f < frames; f++) {
        const int fn = sig->fn0 + f;

        for (j = 0; j < 8; j++) {
            const double b0 = f * 1250.0 + j * 156.25 - sig->start_bits;   /* Bit 0 of the burst. */
            uint8_t d[148];
            double ph0 = 2.0 * M_PI * rng_uniform();
            long n0, n1, n;
            double accum[150];

            if (j == 0 && fn % 51 % 10 == 0 && fn % 51 != 50) {
                memset(d, 0, sizeof(d));                          /* FCCH. */
            } else if (j == 0 && fn % 51 % 10 == 1) {
                m2sdr_gsm_sch_encode(sig->bsic, fn, coded);       /* SCH. */
                m2sdr_gsm_sch_burst(coded, d);
            } else {
                const char *tsc = gsm_tsc[sig->bsic & 7];

                for (k = 0; k < 148; k++)
                    d[k] = (uint8_t)rng_bit();
                d[0] = d[1] = d[2] = d[145] = d[146] = d[147] = 0;
                for (k = 0; k < 26; k++)
                    d[61 + k] = (uint8_t)(tsc[k] - '0');
            }
            /* Phase before each bit: the bits before it turned by whole quarter turns. */
            accum[0] = 0;
            for (k = 0; k < 148; k++)
                accum[k + 1] = accum[k] + ((d[k] ^ (k ? d[k - 1] : 0)) ? -1.0 : 1.0);
            n0 = (long)ceil((b0 - 2.5) / R * sig->fs);
            n1 = (long)floor((b0 + 149.5) / R * sig->fs);
            for (n = n0 > 0 ? n0 : 0; n <= n1 && n < (long)count; n++) {
                const double u = (double)n / sig->fs * R - b0;
                const int c = (int)floor(u);
                const int lo = c - 4 < 0 ? 0 : c - 4, hi = c + 5 > 148 ? 148 : c + 5;
                double phase = accum[lo < 148 ? lo : 148], amp = 1.0;
                int q;

                for (q = lo; q < hi; q++)
                    phase += ((d[q] ^ (q ? d[q - 1] : 0)) ? -1.0 : 1.0) * test_gmsk_q(u - q);
                if (u < -0.5)
                    amp = 0.5 + 0.5 * cos(M_PI * (-0.5 - u) / 2.0);
                else if (u > 147.5)
                    amp = 0.5 + 0.5 * cos(M_PI * (u - 147.5) / 2.0);
                x[n] += (float complex)(GSM_AMP * amp * cexp(I * (M_PI / 2.0 * phase + ph0)));
            }
        }
    }
    for (i = 0; i < count; i++)
        x[i] *= (float complex)cexp(I * 2.0 * M_PI * sig->freq_hz * (double)i / sig->fs);
    add_noise(x, count, GSM_AMP * GSM_AMP * pow(10.0, -sig->snr_db / 10.0) * sig->fs / 340e3);
}

/* An OFDM carrier at 1.92 MSPS (LTE-like: 128 points, 15 kHz, cyclic prefix of 9), QPSK on
 * the bins lo to hi, and an echo: no FCCH in it. With an edge of its spectrum in the receive
 * band, the turn of its phase over a bit has a mean that passed the test of the runs. */
static void make_ofdm(float complex *x, size_t count, int lo, int hi, double echo_us, double echo_db)
{
    struct m2sdr_fft *ifft = m2sdr_fft_alloc(128, true);
    float complex f[128], t[128];
    const size_t e = (size_t)lround(echo_us * 1.92);
    const float a = (float)pow(10.0, echo_db / 20.0);
    size_t i, at = 0;
    int k;

    memset(x, 0, count * sizeof(*x));
    while (ifft && at < count) {
        memset(f, 0, sizeof(f));
        for (k = lo; k <= hi; k++)
            if (k)
                f[(k + 128) % 128] = 0.05f * ((rng_bit() ? 1.0f : -1.0f) + I * (rng_bit() ? 1.0f : -1.0f));
        m2sdr_fft_run(ifft, f, t);
        for (k = 0; k < 137 && at < count; k++, at++)
            x[at] = t[(k - 9 + 128) % 128];
    }
    for (i = count; i-- > e;)
        x[i] += a * x[i - e];
    add_noise(x, count, 1e-5);
    m2sdr_fft_free(ifft);
}

static void test_gsm(void)
{
    static const struct gsm_signal signals[] = {
        /*   fs       bsic   fn0     start   freq_hz  snr */
        { 1.92e6,      0x2d, 1000,    300.0,  1234.0, 30.0 },
        { 1.0e6,       0x07, 51 * 26 * 2047 + 49, 900.0, -8000.0, 15.0 },   /* Odd rate, frame numbers wrap. */
        { 2.166667e6,  0x3f, 123456,  0.0,     500.0, 60.0 },    /* 8 samples a bit, clean. */
        { 1.92e6,      0x12, 77777,  10.0,   17000.0, 9.0 },
    };
    const size_t max = (size_t)(0.06 * 2.166667e6) + 1;
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_gsm_result *res = malloc(sizeof(*res));
    size_t i;

    printf("GSM:\n");
    if (!x || !res) {
        CHECK("allocation", 0);
        free(x);
        free(res);
        return;
    }
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        const struct gsm_signal *sig = &signals[i];
        const size_t count = (size_t)(0.06 * sig->fs);
        const double snr = pow(10.0, sig->snr_db / 10.0);
        double t0, el;
        int fn_sch = -1, f;

        make_gsm(x, count, sig);
        printf(" %.3f MSPS, BSIC %d, offset %.0f Hz, SNR %.0f dB\n", sig->fs / 1e6, sig->bsic, sig->freq_hz, sig->snr_db);
        t0 = now_s();
        CHECK("analyse", m2sdr_gsm_analyse(x, count, sig->fs, NULL, res) == 0);
        el = now_s() - t0;
        printf("  time %.1f ms, FCCH %d, SCH %d\n", el * 1e3, res->fcch_bursts, res->nsch);
        CHECK("FCCH, SCH decoded", res->fcch && res->found);
        CHECK("BSIC", res->bsic == sig->bsic && res->ncc == sig->bsic >> 3 && res->bcc == (sig->bsic & 7));
        /* The first SCH of the capture: frame f after the start, f % 51 % 10 == 1. */
        for (f = 0; f < 20; f++) {
            const int fn = sig->fn0 + f;

            if (fn % 51 % 10 == 1 && (f * 1250.0 - sig->start_bits) > 0) {
                fn_sch = fn % (26 * 51 * 2048);
                break;
            }
        }
        CHECK("frame number", res->fn == fn_sch);
        if (res->nsch > 0) {
            /* Bit 0 of that SCH at (f * 1250 - start) bits from the start. */
            const double expect = ((fn_sch - sig->fn0 + 26 * 51 * 2048) % (26 * 51 * 2048) * 1250.0 - sig->start_bits) /
                                  M2SDR_GSM_SYMBOL_RATE * sig->fs;

            /* Timing from the 64 bits of training through a parabola: a twentieth of a bit,
             * and the noise, a tenth of a bit at 9 dB. */
            CHECK_NEAR("SCH start (samples)", res->sch[0].start, expect,
                       (0.05 + 0.3 / sqrt(snr)) * sig->fs / M2SDR_GSM_SYMBOL_RATE);
        }
        /* The frequency from the slope of the phase over a burst: 142 bits (0.52 ms), the noise
         * of the phase 1 / sqrt(2 SNR) radians, about 180 independent samples through the
         * 340 kHz filter: sigma = sqrt(12 / 180) / (2 pi 0.52 ms) / sqrt(2 SNR), 79 Hz / sqrt(2 SNR),
         * over the bursts decoded. Within 3 sigma. */
        {
            int nd = 0, b;

            for (b = 0; b < res->nsch; b++)
                nd += res->sch[b].crc;
            CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, sig->freq_hz,
                       3.0 * 79.0 / sqrt(2.0 * snr) / sqrt(nd > 0 ? nd : 1) + 1.0);
        }
        /* Signal and the noise of the receive band; over the ~180 independent samples of a
         * burst, the product of the two varies by sqrt(2 / (180 SNR)): within 3 sigma. */
        CHECK_NEAR("burst power (dBFS)", res->burst_power_db, 20.0 * log10(GSM_AMP) + 10.0 * log10(1.0 + 1.0 / snr),
                   0.1 + 3.0 * 4.34 * sqrt(2.0 / (180.0 * snr)));
        /* The noise in the receive band against the burst: EVM 1 / sqrt(SNR), half of it as
         * phase (radians). The filter is the same on both sides, the GMSK modelled. */
        CHECK_NEAR("EVM", res->evm, 1.0 / sqrt(snr) + 0.002, 0.15 / sqrt(snr) + 0.003);
        CHECK_NEAR("phase error RMS (deg)", res->phase_err_rms_deg, 180.0 / M_PI / sqrt(2.0 * snr) + 0.1,
                   0.15 * 180.0 / M_PI / sqrt(2.0 * snr) + 0.2);
    }
    memset(x, 0, max * sizeof(*x));
    add_noise(x, max, 0.01);
    CHECK("noise", m2sdr_gsm_analyse(x, max, 2.166667e6, NULL, res) == 0 && !res->found && !res->fcch);
    /* OFDM carriers, their edge in the band, with and without an echo: 40 of these 60 gave
     * an FCCH before the coherence of the burst was asked for. */
    {
        static const int bins[3][2] = { { -60, 60 }, { 0, 40 }, { -40, 0 } };
        const size_t n = (size_t)(0.06 * 1.92e6);
        int c, k, false_fcch = 0;

        for (c = 0; c < 3; c++)
            for (k = 0; k < 20; k++) {
                make_ofdm(x, n, bins[c][0], bins[c][1], k % 2 ? 1.5 : 0.0, -3.0);
                if (m2sdr_gsm_analyse(x, n, 1.92e6, NULL, res) != 0 || res->fcch)
                    false_fcch++;
            }
        CHECK("OFDM carriers: no FCCH", false_fcch == 0);
    }
    for (i = 0; i < max; i++)
        x[i] += (float complex)(0.3 * cexp(I * 2.0 * M_PI * (M2SDR_GSM_SYMBOL_RATE / 4.0 + 300.0) * (double)i / 2.166667e6));
    CHECK("steady tone at the FCCH", m2sdr_gsm_analyse(x, max, 2.166667e6, NULL, res) == 0 && !res->fcch);
    CHECK("bad sample rate", m2sdr_gsm_analyse(x, max, 0.2e6, NULL, res) != 0);
    CHECK("too short", m2sdr_gsm_analyse(x, 10000, 2.166667e6, NULL, res) != 0);
    CHECK("no result", m2sdr_gsm_analyse(x, max, 2.166667e6, NULL, NULL) != 0);
    free(x);
    free(res);
}


/* TETRA */
/*-------*/

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

/* Fields of a SYNC PDU (EN 300 392-2 21.4.4.2 and 18.4.2.1). */
struct tetra_sync {
    int system_code, colour_code, sharing_mode, ts_reserved, dtx, f18, mcc, mnc, nb, cell_load, late;
};

static void put_bits(uint8_t *b, int *at, int value, int n)
{
    int i;

    for (i = n - 1; i >= 0; i--)
        b[(*at)++] = (uint8_t)((value >> i) & 1);
}

static void tetra_sync_pdu(const struct tetra_sync *f, int tn, int fn, int mn, uint8_t *pdu)
{
    int at = 0;

    put_bits(pdu, &at, f->system_code, 4);
    put_bits(pdu, &at, f->colour_code, 6);
    put_bits(pdu, &at, tn - 1, 2);
    put_bits(pdu, &at, fn, 5);
    put_bits(pdu, &at, mn, 6);
    put_bits(pdu, &at, f->sharing_mode, 2);
    put_bits(pdu, &at, f->ts_reserved, 3);
    put_bits(pdu, &at, f->dtx, 1);
    put_bits(pdu, &at, f->f18, 1);
    put_bits(pdu, &at, 0, 1);
    put_bits(pdu, &at, f->mcc, 10);
    put_bits(pdu, &at, f->mnc, 14);
    put_bits(pdu, &at, f->nb, 2);
    put_bits(pdu, &at, f->cell_load, 2);
    put_bits(pdu, &at, f->late, 1);
}

/* A continuous downlink as EN 300 392-2 9.4.4 builds it: every slot a normal burst (training
 * 1 or 2 at random, random bits elsewhere) but the synchronisation burst of frame 18 at
 * timeslot sb_tn, pi/4-DQPSK under a root raised cosine of 0.35. Slot 0 of the signal is
 * timeslot 1 of frame fn0 of multiframe mn0. */
struct tetra_signal {
    double fs;
    double start_symbols;       /* Symbols of the signal before the capture starts. */
    double freq_hz;
    double snr_db;              /* Symbol energy over the noise in the symbol rate (after the matched filter). */
    int    fn0, mn0, sb_tn;
    int    off_tn;              /* A timeslot without carrier (0: none). */
    struct tetra_sync sync;
};

#define TETRA_AMP     0.2
#define TETRA_SLOTS   80

static void make_tetra(float complex *x, size_t count, const struct tetra_signal *sig, uint8_t *kind, uint8_t *train)
{
    const double R = M2SDR_TETRA_SYMBOL_RATE;
    const int nsym = TETRA_SLOTS * 255;
    float complex *S = malloc((size_t)nsym * sizeof(*S));
    const uint8_t *tq = m2sdr_tetra_training(3), *ty = m2sdr_tetra_training(4), *tf = m2sdr_tetra_training(5);
    float complex cur = 1.0f;
    size_t n;
    int s, k;

    for (s = 0; s < TETRA_SLOTS; s++) {
        const int tn = s % 4 + 1, fn = (sig->fn0 - 1 + s / 4) % 18 + 1;
        const int mn = (sig->mn0 - 1 + (sig->fn0 - 1 + s / 4) / 18) % 60 + 1;
        uint8_t b[510];

        for (k = 0; k < 510; k++)
            b[k] = (uint8_t)rng_bit();
        memcpy(b, tq + 10, 12);
        memcpy(b + 500, tq, 10);
        if (fn == 18 && tn == sig->sb_tn) {
            uint8_t pdu[60];

            tetra_sync_pdu(&sig->sync, tn, fn, mn, pdu);
            memcpy(b + 14, tf, 80);
            m2sdr_tetra_bsch_encode(pdu, b + 94);
            memcpy(b + 214, ty, 38);
            kind[s]  = M2SDR_TETRA_BURST_SYNC;
            train[s] = 0;
        } else {
            const int w = rng_bit() + 1;

            memcpy(b + 244, m2sdr_tetra_training(w), 22);
            kind[s]  = M2SDR_TETRA_BURST_NORMAL;
            train[s] = (uint8_t)w;
        }
        if (tn == sig->off_tn) {
            kind[s]  = M2SDR_TETRA_BURST_NONE;
            train[s] = 0;
        }
        for (k = 0; k < 255; k++) {
            static const double turn[4] = { M_PI / 4.0, 3.0 * M_PI / 4.0, -M_PI / 4.0, -3.0 * M_PI / 4.0 };

            cur *= (float complex)cexp(I * turn[(b[2 * k] << 1) | b[2 * k + 1]]);
            S[s * 255 + k] = kind[s] == M2SDR_TETRA_BURST_NONE ? 0 : cur;
        }
    }
    for (n = 0; n < count; n++) {
        const double u = (double)n / sig->fs * R + sig->start_symbols;
        const int k0 = (int)floor(u) - 10, k1 = (int)floor(u) + 10;
        double complex acc = 0;

        for (k = k0 < 0 ? 0 : k0; k <= k1 && k < nsym; k++)
            acc += S[k] * test_rrc(u - k, 0.35);
        x[n] = (float complex)(TETRA_AMP * acc * cexp(I * 2.0 * M_PI * sig->freq_hz * (double)n / sig->fs));
    }
    add_noise(x, count, TETRA_AMP * TETRA_AMP * pow(10.0, -sig->snr_db / 10.0) * sig->fs / R);
    free(S);
}

static void test_tetra(void)
{
    static const struct tetra_signal signals[] = {
        /*  fs     start   freq_hz   snr  fn0 mn0 sb off  sync: sys cc  sh res dtx f18 mcc  mnc    nb cl late */
        { 288e3,  100.3,   345.6,  25.0, 17,  5, 2, 0, { 2, 23, 0, 1, 0, 1, 208, 12345, 1, 2, 0 } },
        { 1.0e6,   37.7, -2600.0,  15.0, 17,  1, 1, 0, { 1, 63, 3, 7, 1, 0, 901,    16, 0, 1, 1 } },
        { 2.0e6,  510.0,  8000.0,  40.0, 17, 60, 4, 3, { 0,  1, 1, 0, 0, 0,   1, 16383, 3, 3, 0 } },
    };
    const size_t max = (size_t)(0.25 * 2.0e6);
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_tetra_result *res = malloc(sizeof(*res));
    uint8_t kind[TETRA_SLOTS], train[TETRA_SLOTS];
    size_t i;

    printf("TETRA:\n");
    if (!x || !res) {
        CHECK("allocation", 0);
        free(x);
        free(res);
        return;
    }
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        const struct tetra_signal *sig = &signals[i];
        const size_t count = (size_t)(0.25 * sig->fs);
        double t0, el, first;
        int sigma, s, kinds_ok = 1, sb_slot = -1;

        make_tetra(x, count, sig, kind, train);
        printf(" %.3f MSPS, offset %.1f Hz, SNR %.0f dB%s\n", sig->fs / 1e6, sig->freq_hz, sig->snr_db,
               sig->off_tn ? ", a timeslot off" : "");
        t0 = now_s();
        CHECK("analyse", m2sdr_tetra_analyse(x, count, sig->fs, res) == 0);
        el = now_s() - t0;
        printf("  time %.1f ms, %d slots, sync quality %.3f\n", el * 1e3, res->nslots, res->sync_quality);
        CHECK("found", res->found);
        /* The first slot analysed, in symbols of the signal: on a slot boundary. */
        first = res->slot_start / sig->fs * M2SDR_TETRA_SYMBOL_RATE + sig->start_symbols;
        sigma = (int)lround(first / 255.0);
        CHECK_NEAR("slot boundary (symbols)", first - 255.0 * sigma, 0.0, 0.05);
        /* The slope of the phase over each slot: 5 blocks of 51 symbols (2.83 ms) whose phase
         * has a standard deviation of about 1.3 / sqrt(2 x 51 SNR) (the fourth power costs a
         * little), the slope of a line through 5 points sqrt(12 / 120) of it per block: 0.41 Hz
         * a slot at 15 dB. Within 4 sigma over the slots. */
        {
            const double sigma = 1.3 / sqrt(2.0 * 51.0 * pow(10.0, sig->snr_db / 10.0)) * sqrt(0.1) /
                                 (2.0 * M_PI * 51.0 / M2SDR_TETRA_SYMBOL_RATE);

            CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, sig->freq_hz,
                       4.0 * sigma / sqrt(res->nslots > 0 ? res->nslots : 1) + 0.02);
        }
        for (s = 0; s < res->nslots && sigma + s < TETRA_SLOTS; s++) {
            if (res->slot_burst[s] != kind[sigma + s] || res->slot_training[s] != train[sigma + s]) {
                printf("  slot %d: burst %d training %d, expected %d %d\n", s, res->slot_burst[s],
                       res->slot_training[s], kind[sigma + s], train[sigma + s]);
                kinds_ok = 0;
            }
            if (kind[sigma + s] == M2SDR_TETRA_BURST_SYNC && sb_slot < 0)
                sb_slot = s;
        }
        CHECK("bursts and training of each slot", kinds_ok && res->nslots >= 15);
        CHECK("synchronisation burst found", sb_slot >= 0 && res->sync_bursts >= 1);
        /* The symbols after the matched filter have the SNR given; the phase over 51 symbols and
         * the gain add 1/51 of the noise, the resampling and the timing a little more. */
        CHECK_NEAR("MER (dB)", res->mer_db, sig->snr_db - 0.1, 0.6);
        CHECK_NEAR("slot power (dBFS)", res->slot_power_db[0], 20.0 * log10(TETRA_AMP) + 10.0 * log10(1.0 + 1.0 / pow(10.0, sig->snr_db / 10.0)), 0.3);
        CHECK("BSCH decoded", res->sync && res->sync_slot == sb_slot);
        if (res->sync) {
            const int abs_slot = sigma + sb_slot;
            const int fn = (sig->fn0 - 1 + abs_slot / 4) % 18 + 1;
            const int mn = (sig->mn0 - 1 + (sig->fn0 - 1 + abs_slot / 4) / 18) % 60 + 1;

            CHECK("colour code, MCC, MNC", res->colour_code == sig->sync.colour_code && res->mcc == sig->sync.mcc &&
                                           res->mnc == sig->sync.mnc && res->system_code == sig->sync.system_code);
            CHECK("timeslot, frame, multiframe", res->tn == sig->sb_tn && res->fn == fn && res->mn == mn);
            CHECK("other fields", res->sharing_mode == sig->sync.sharing_mode &&
                                  res->ts_reserved_frames == sig->sync.ts_reserved && res->u_plane_dtx == sig->sync.dtx &&
                                  res->frame18_ext == sig->sync.f18 && res->neighbour_broadcast == sig->sync.nb &&
                                  res->cell_load == sig->sync.cell_load && res->late_entry == sig->sync.late);
        }
    }
    /* Four slots only: the boundaries of three of them. */
    {
        const struct tetra_signal sig = { 288e3, 1000.0, -1500.0, 20.0, 1, 1, 1, 0, { 0, 5, 0, 0, 0, 0, 1, 1, 0, 0, 0 } };
        const size_t count = (size_t)(4.2 * 255 / M2SDR_TETRA_SYMBOL_RATE * 288e3);

        make_tetra(x, count, &sig, kind, train);
        printf(" four slots\n");
        CHECK("analyse", m2sdr_tetra_analyse(x, count, 288e3, res) == 0);
        CHECK("found", res->found && res->nslots >= 2);
        CHECK_NEAR("MER (dB)", res->mer_db, sig.snr_db - 0.1, 0.8);
    }
    memset(x, 0, max * sizeof(*x));
    add_noise(x, max, 0.01);
    CHECK("noise", m2sdr_tetra_analyse(x, max, 2.0e6, res) == 0 && !res->found && !res->sync);
    CHECK("noise, three slots", m2sdr_tetra_analyse(x, (size_t)(0.045 * 2.0e6), 2.0e6, res) == 0 && !res->found);
    CHECK("bad sample rate", m2sdr_tetra_analyse(x, max, 100e3, res) != 0 && m2sdr_tetra_analyse(x, max, 10e6, res) != 0);
    CHECK("too short", m2sdr_tetra_analyse(x, 50000, 2.0e6, res) != 0);
    CHECK("no result", m2sdr_tetra_analyse(x, max, 2.0e6, NULL) != 0);
    free(x);
    free(res);
}

/* 5G NR */
/*-------*/

/* A downlink as TS 38.211 builds it, over 25 ms: the SSBs of a burst in the first half frame
 * of each 20 ms (case A at 15 kHz, case C at 30 kHz: first symbols 2 and 8 of each 14), PSS,
 * SSS, PBCH DM-RS of the cell and of the index, random QPSK for the PBCH data; around them,
 * random QPSK on a share of the subcarriers of the carrier. Every symbol has a random phase
 * (the upconversion of TS 38.211 5.4 at an unknown frequency). The symbols have the long
 * cyclic prefix at the start of each half millisecond. */
struct nr_signal {
    double scs, fs;
    int    pci, lmax, nssb;     /* SSBs of the burst: indices 0 to nssb - 1. */
    int    ssref_bin;           /* SSREF on this bin of the capture. */
    double freq_hz;             /* Carrier offset on top of it. */
    double snr_db;              /* Per element. */
    double load;                /* Share of the carrier with data. */
    int    carrier_bins;        /* Carrier around the centre. */
    double start_ms;            /* Of the 20 ms period, before the capture starts. */
    double echo_us, echo_db;
};

#define NR_AMP 0.02

/* Sample, in the capture, of the start (cyclic prefix) of symbol q of the period. */
static double nr_symbol_start(const struct nr_signal *sig, long q)
{
    const int n = (int)lround(sig->fs / sig->scs), cp = n * 9 / 128, extra = (int)lround(sig->fs / 1.92e6);
    const int per_half = (int)lround(7 * sig->scs / 15000.0);   /* Symbols in half a millisecond. */
    const long halves = q / per_half;

    return (double)halves * (per_half * (n + cp) + extra) + (double)(q % per_half) * (n + cp) +
           (q % per_half ? extra : 0) - sig->start_ms * 1e-3 * sig->fs;
}

static int nr_ssb_symbol(int i)
{
    /* Cases A and C: symbols {2, 8} + 14 n. */
    return 14 * (i / 2) + (i % 2 ? 8 : 2);
}

static void make_nr(float complex *x, size_t count, const struct nr_signal *sig)
{
    const int n = (int)lround(sig->fs / sig->scs), cp = n * 9 / 128, extra = (int)lround(sig->fs / 1.92e6);
    const int per_period = (int)lround(280 * sig->scs / 15000.0);   /* Symbols in 20 ms. */
    struct m2sdr_fft *ifft = m2sdr_fft_alloc((size_t)n, true);
    float complex *f = malloc((size_t)n * sizeof(*f)), *t = malloc((size_t)n * sizeof(*t));
    const double noise = NR_AMP * NR_AMP * n * pow(10.0, -sig->snr_db / 10.0);
    float d[127], dsss[127];
    float complex dm[144];
    const size_t echo = (size_t)lround(sig->echo_us * 1e-6 * sig->fs);
    const double echo_amp = sig->echo_db > -99 ? pow(10.0, sig->echo_db / 20.0) : 0.0;
    long q;
    size_t i;
    int k;

    memset(x, 0, count * sizeof(*x));
    for (q = 0; ; q++) {
        const long qp = q % per_period;
        const double st = nr_symbol_start(sig, q);
        const int len = n + cp + ((q % (int)lround(7 * sig->scs / 15000.0)) == 0 ? extra : 0);
        const int this_cp = len - n;
        int ssb = -1, l = 0, b;
        const float complex ph = (float complex)cexp(I * 2.0 * M_PI * rng_uniform());

        if (st >= (double)count)
            break;
        for (b = 0; b < sig->nssb; b++)
            if (qp >= nr_ssb_symbol(b) && qp < nr_ssb_symbol(b) + 4) {
                ssb = b;
                l   = (int)(qp - nr_ssb_symbol(b));
            }
        memset(f, 0, (size_t)n * sizeof(*f));
        for (k = -sig->carrier_bins / 2; k < sig->carrier_bins / 2; k++) {
            const int rel = k - sig->ssref_bin + 120;

            if (ssb >= 0 && rel >= 0 && rel < 240)
                continue;
            if (rng_uniform() < sig->load)
                f[(k + n) % n] = (float complex)(NR_AMP * M_SQRT1_2) * ((rng_bit() ? 1.0f : -1.0f) + I * (rng_bit() ? 1.0f : -1.0f));
        }
        if (ssb >= 0) {
            const int ibar = sig->lmax == 4 ? ssb : ssb & 7;
            int m;

            m2sdr_nr_pss(sig->pci % 3, d);
            m2sdr_nr_sss(sig->pci / 3, sig->pci % 3, dsss);
            m2sdr_nr_pbch_dmrs(sig->pci, ibar, dm);
            for (k = 0; k < 240; k++) {
                const int bin = ((sig->ssref_bin - 120 + k) % n + n) % n;
                float complex v = 0;

                if (l == 0 && k >= 56 && k < 183)
                    v = d[k - 56];
                else if (l == 2 && k >= 56 && k < 183)
                    v = dsss[k - 56];
                else if (l == 1 || l == 3 || (l == 2 && (k < 48 || k >= 192)))
                    v = (float)M_SQRT1_2 * ((rng_bit() ? 1.0f : -1.0f) + I * (rng_bit() ? 1.0f : -1.0f));
                f[bin] = (float complex)NR_AMP * v;
            }
            /* DM-RS over the PBCH elements they take (TS 38.211 7.4.3.1). */
            for (m = 0; m < 144; m++) {
                int ll, kk;

                if (m < 60) {
                    ll = 1;
                    kk = sig->pci % 4 + 4 * m;
                } else if (m < 84) {
                    ll = 2;
                    kk = m < 72 ? sig->pci % 4 + 4 * (m - 60) : 192 + sig->pci % 4 + 4 * (m - 72);
                } else {
                    ll = 3;
                    kk = sig->pci % 4 + 4 * (m - 84);
                }
                if (ll == l)
                    f[((sig->ssref_bin - 120 + kk) % n + n) % n] = (float complex)NR_AMP * dm[m];
            }
        }
        m2sdr_fft_run(ifft, f, t);
        for (k = 0; k < len; k++) {
            const long at = (long)floor(st) + k;

            if (at >= 0 && at < (long)count)
                x[at] = t[(k - this_cp + n) % n] * ph;
        }
    }
    if (echo_amp > 0)
        for (i = count; i-- > echo;)
            x[i] += (float)echo_amp * x[i - echo];
    for (i = 0; i < count; i++)
        x[i] *= (float complex)cexp(I * 2.0 * M_PI * sig->freq_hz * (double)i / sig->fs);
    add_noise(x, count, noise);
    m2sdr_fft_free(ifft);
    free(f);
    free(t);
}

static void test_nr(void)
{
    static const struct nr_signal signals[] = {
        /* scs    fs       pci lmax nssb ssref  freq_hz  snr  load bins start  echo */
        { 15e3, 7.68e6,    123,  4,  4,    0,   1234.5, 25.0, 0.0, 300,  0.0, 0.0, -99.0 },
        { 15e3, 7.68e6,   1007,  4,  4,   37,  -6000.0, 15.0, 0.8, 300,  3.3, 0.0, -99.0 },  /* Loaded, off the centre. */
        { 30e3, 15.36e6,   500,  8,  8,  -90,    777.0, 20.0, 0.5, 400, 17.0, 0.0, -99.0 },
        { 30e3, 15.36e6,     2,  8,  8,   12,  13000.0, 30.0, 0.3, 400,  1.1, 0.5, -10.0 },  /* An echo in the prefix. */
        { 15e3, 7.68e6,    777,  4,  4, -100,   -300.0,  6.0, 0.0, 300,  0.0, 0.0, -99.0 },
    };
    const size_t max = (size_t)(0.025 * 15.36e6);
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_nr_result *res = malloc(sizeof(*res));
    struct m2sdr_nr_config cfg;
    size_t i;

    printf("5G NR:\n");
    if (!x || !res) {
        CHECK("allocation", 0);
        free(x);
        free(res);
        return;
    }
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        const struct nr_signal *sig = &signals[i];
        const size_t count = (size_t)(0.025 * sig->fs);
        const double snr = pow(10.0, sig->snr_db / 10.0);
        const double a2 = sig->echo_db > -99 ? pow(10.0, sig->echo_db / 10.0) : 0.0;
        double t0, el, worst_t = 0, worst_f = 0, rsrp = 0, sinr = 0, devm = 0, pevm = 0;
        int expect = 0, j, ids_ok = 1, nb;

        make_nr(x, count, sig);
        printf(" %.0f kHz, %.2f MSPS, PCI %d, SSREF %+.1f kHz, SNR %.0f dB, load %.0f %%%s\n", sig->scs / 1e3,
               sig->fs / 1e6, sig->pci, (sig->ssref_bin * sig->scs + sig->freq_hz) / 1e3, sig->snr_db,
               sig->load * 100.0, sig->echo_db > -99 ? ", echo" : "");
        memset(&cfg, 0, sizeof(cfg));
        cfg.lmax = sig->lmax;
        t0 = now_s();
        CHECK("analyse", m2sdr_nr_analyse(x, count, sig->fs, &cfg, res) == 0);
        el = now_s() - t0;
        /* SSBs whose 4 symbols are in the capture, in the periods it spans. */
        for (j = 0; j < 3 * sig->nssb; j++) {
            const long q = (long)(j / sig->nssb) * (long)lround(280 * sig->scs / 15000.0) + nr_ssb_symbol(j % sig->nssb);

            if (nr_symbol_start(sig, q) >= 0 && nr_symbol_start(sig, q + 4) <= (double)count)
                expect++;
        }
        printf("  time %.1f ms, %d SSBs (%d expected), %.0f kHz\n", el * 1e3, res->nssb, expect, res->scs_hz / 1e3);
        CHECK("found, spacing", res->found && res->scs_hz == sig->scs && res->cells == 1);
        CHECK("SSBs", res->nssb == expect);
        nb = 0;
        for (j = 0; j < res->nssb; j++) {
            const struct m2sdr_nr_ssb *b = &res->ssb[j];
            long q;
            int idx = -1, per;

            /* The SSB of the signal this one is. */
            for (per = 0; per < 3 && idx < 0; per++) {
                int c;

                for (c = 0; c < sig->nssb; c++) {
                    q = (long)per * (long)lround(280 * sig->scs / 15000.0) + nr_ssb_symbol(c);
                    if (fabs(nr_symbol_start(sig, q) - (double)b->start) < 2.0) {
                        idx = c;
                        if (fabs(nr_symbol_start(sig, q) - (double)b->start) > worst_t)
                            worst_t = fabs(nr_symbol_start(sig, q) - (double)b->start);
                    }
                }
            }
            if (idx < 0 || b->pci != sig->pci || b->ssb_index != (sig->lmax == 4 ? idx & 3 : idx) ||
                (sig->lmax == 4 && b->half_frame != 0)) {
                printf("  SSB %d: start %zu PCI %d index %d half %d\n", j, b->start, b->pci, b->ssb_index, b->half_frame);
                ids_ok = 0;
            }
            if (fabs(b->freq_offset_hz - (sig->ssref_bin * sig->scs + sig->freq_hz)) > worst_f)
                worst_f = fabs(b->freq_offset_hz - (sig->ssref_bin * sig->scs + sig->freq_hz));
            rsrp += b->rsrp_db;
            sinr += b->sinr_db;
            devm += b->dmrs_evm * b->dmrs_evm;
            pevm += b->pbch_evm * b->pbch_evm;
            nb++;
        }
        CHECK("PCI, index and timing of each", ids_ok && nb > 0);
        if (nb == 0)
            continue;
        rsrp /= nb;
        sinr /= nb;
        devm = sqrt(devm / nb);
        pevm = sqrt(pevm / nb);
        CHECK_NEAR("timing (samples)", worst_t, 0.0, 1.0);
        /* Offset of an SSB from the halves of its PSS and SSS: 127 elements in each half, the
         * phase between them of standard deviation 1 / sqrt(127 SNR / 2) each, a turn of pi
         * for one spacing: sigma = scs / (pi sqrt(127 SNR)). The worst SSB within 4 sigma, the
         * mean over them within 4 sigma / sqrt(n). An echo leaves a little more. */
        {
            const double sigma = sig->scs / (M_PI * sqrt(127.0 * snr)) + (a2 > 0 ? 0.002 * sig->scs : 0.0);

            CHECK_NEAR("worst SSB offset (Hz)", worst_f, 0.0, 4.0 * sigma);
            CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, sig->ssref_bin * sig->scs + sig->freq_hz,
                       4.0 * sigma / sqrt(nb));
        }
        /* RSRP: the SSS elements, NR_AMP each; with an echo the channel varies across them
         * and its mean power is 1 + a^2. */
        CHECK_NEAR("SS-RSRP (dBFS)", rsrp, 20.0 * log10(NR_AMP) + 10.0 * log10(1.0 + a2), 0.5);
        if (a2 == 0) {
            /* The SINR from the SSS against the mean of 4 neighbours: the SNR per element. */
            CHECK_NEAR("SS-SINR (dB)", sinr, sig->snr_db, 1.0);
            /* DM-RS against the mean of their neighbours: 1 / sqrt(SNR). The PBCH equalised with
             * the mean of 5 pilots and the lines between them: about 1.2 / SNR in power. */
            CHECK_NEAR("DM-RS EVM", devm, 1.0 / sqrt(snr), 0.12 / sqrt(snr));
            if (sig->snr_db >= 10.0)
                CHECK_NEAR("PBCH EVM", pevm, sqrt(1.2 / snr), 0.15 / sqrt(snr));
        }
    }
    /* Two cells: their SSBs 1.5 ms apart, the second 6 dB weaker and 200 subcarriers higher. */
    {
        static const struct nr_signal a = { 15e3, 7.68e6, 301, 4, 4, -100, 100.0, 30.0, 0.0, 300, 0.0, 0.0, -99.0 };
        static const struct nr_signal b = { 15e3, 7.68e6, 302, 4, 4, 100, 100.0, 99.0, 0.0, 300, 18.5, 0.0, -99.0 };
        const size_t count = (size_t)(0.025 * 7.68e6);
        float complex *y = malloc(count * sizeof(*y));
        int na = 0, nb = 0, j;

        if (y) {
            make_nr(x, count, &a);
            make_nr(y, count, &b);
            for (i = 0; i < count; i++)
                x[i] += 0.5f * y[i];
            memset(&cfg, 0, sizeof(cfg));
            cfg.scs_hz = 15e3;
            cfg.lmax   = 4;
            printf(" two cells\n");
            CHECK("analyse", m2sdr_nr_analyse(x, count, 7.68e6, &cfg, res) == 0);
            for (j = 0; j < res->nssb; j++) {
                na += res->ssb[j].pci == 301;
                nb += res->ssb[j].pci == 302;
            }
            printf("  %d SSBs of 301, %d of 302, %d cells\n", na, nb, res->cells);
            CHECK("both cells, the stronger first", res->cells == 2 && na == 8 && nb == 8 && res->ssb[res->best].pci == 301);
            free(y);
        }
    }
    memset(x, 0, max * sizeof(*x));
    add_noise(x, max, 0.01);
    CHECK("noise", m2sdr_nr_analyse(x, max, 15.36e6, NULL, res) == 0 && !res->found && res->nssb == 0);
    memset(&cfg, 0, sizeof(cfg));
    cfg.scs_hz = 30e3;
    CHECK("bad spacing for the rate", m2sdr_nr_analyse(x, max, 7.0e6, &cfg, res) != 0);
    CHECK("bad sample rate", m2sdr_nr_analyse(x, max, 10e6, NULL, res) != 0);
    CHECK("too short", m2sdr_nr_analyse(x, 1000, 15.36e6, NULL, res) != 0);
    CHECK("no result", m2sdr_nr_analyse(x, max, 15.36e6, NULL, NULL) != 0);
    free(x);
    free(res);
}

/* Capture files for the reports of m2sdr_analyzer */
/*--------------------------------------------------*/

/* SC16 of 12 bits, the same samples on both channels. */
static int write_sc16(const char *path, const float complex *x, size_t count)
{
    FILE *f = fopen(path, "wb");
    size_t i;

    if (!f)
        return 1;
    for (i = 0; i < count; i++) {
        const short v[4] = { (short)lrintf(crealf(x[i]) * 2047.0f), (short)lrintf(cimagf(x[i]) * 2047.0f),
                             (short)lrintf(crealf(x[i]) * 2047.0f), (short)lrintf(cimagf(x[i]) * 2047.0f) };

        if (fwrite(v, sizeof(v), 1, f) != 1) {
            fclose(f);
            return 1;
        }
    }
    return fclose(f) == 0 ? 0 : 1;
}

static const struct nr_signal file_nr = { 30e3, 15.36e6, 500, 8, 8, -90, 777.0, 25.0, 0.3, 400, 3.0, 0.0, -99.0 };
static const struct gsm_signal file_gsm = { 1.92e6, 45, 1000, 300.0, 1234.0, 30.0 };
static const struct tetra_signal file_tetra = { 1.0e6, 37.7, -2600.0, 25.0, 17, 1, 1, 0,
                                                { 1, 23, 0, 1, 0, 1, 208, 12345, 1, 2, 0 } };

static int write_file(const char *path, int which)
{
    const double fs = which == 0 ? file_nr.fs : which == 1 ? file_gsm.fs : file_tetra.fs;
    const size_t count = (size_t)((which == 0 ? 0.025 : which == 1 ? 0.1 : 1.15) * fs);
    float complex *x = malloc(count * sizeof(*x));
    uint8_t kind[TETRA_SLOTS], train[TETRA_SLOTS];
    int rc;

    if (!x)
        return 1;
    if (which == 0)
        make_nr(x, count, &file_nr);
    else if (which == 1)
        make_gsm(x, count, &file_gsm);
    else
        make_tetra(x, count, &file_tetra, kind, train);
    rc = write_sc16(path, x, count);
    free(x);
    return rc;
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

static int check_file(const char *path, int which)
{
    static char json[1 << 17];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        printf("test_m2sdr_analysis_cellular: cannot open %s\n", path);
        return 1;
    }
    n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';
    if (which == 0) {
        printf("5G NR report of m2sdr_analyzer:\n");
        CHECK("found", strstr(json, "\"nr\": {\n    \"found\": true") != NULL);
        CHECK_NEAR("PCI", json_number(json, "nr", "pci"), file_nr.pci, 0.0);
        CHECK_NEAR("spacing (Hz)", json_number(json, "nr", "scs_hz"), file_nr.scs, 0.0);
        CHECK("SSBs", json_number(json, "nr", "ssb_count") >= 8);
        CHECK_NEAR("carrier offset (Hz)", json_number(json, "nr", "carrier_offset_hz"),
                   file_nr.ssref_bin * file_nr.scs + file_nr.freq_hz, 150.0);
        CHECK("SS-SINR over 20 dB", json_number(json, "nr", "sinr_db") > 20.0);
    } else if (which == 1) {
        printf("GSM report of m2sdr_analyzer:\n");
        CHECK("FCCH and SCH", strstr(json, "\"fcch\": true") && strstr(json, "\"found\": true"));
        CHECK_NEAR("ARFCN", json_number(json, "gsm", "arfcn"), 62, 0.0);
        CHECK("band P-GSM 900", strstr(json, "\"band\": \"P-GSM 900\"") != NULL);
        CHECK_NEAR("BSIC", json_number(json, "gsm", "bsic"), file_gsm.bsic, 0.0);
        CHECK_NEAR("frame number", json_number(json, "gsm", "fn"), 1010, 0.0);
        CHECK_NEAR("carrier offset (Hz)", json_number(json, "gsm", "carrier_offset_hz"), file_gsm.freq_hz, 10.0);
        CHECK("phase error under 3 deg", json_number(json, "gsm", "phase_error_rms_deg") < 3.0);
    } else {
        printf("TETRA report of m2sdr_analyzer:\n");
        CHECK("slots and SYNC PDU", strstr(json, "\"found\": true") && strstr(json, "\"sync\": true"));
        CHECK_NEAR("colour code", json_number(json, "tetra", "colour_code"), file_tetra.sync.colour_code, 0.0);
        CHECK_NEAR("MCC", json_number(json, "tetra", "mcc"), file_tetra.sync.mcc, 0.0);
        CHECK_NEAR("MNC", json_number(json, "tetra", "mnc"), file_tetra.sync.mnc, 0.0);
        CHECK_NEAR("carrier offset (Hz)", json_number(json, "tetra", "carrier_offset_hz"), file_tetra.freq_hz, 1.0);
        CHECK("MER over 22 dB", json_number(json, "tetra", "mer_db") > 22.0);
    }
    if (failures) {
        printf("test_m2sdr_analysis_cellular: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis_cellular: report checked\n");
    return 0;
}

int main(int argc, char **argv)
{
    static const char *const modes[3] = { "nr", "gsm", "tetra" };
    int m;

    for (m = 0; m < 3 && argc == 3; m++) {
        char opt[32];

        snprintf(opt, sizeof(opt), "--write-%s", modes[m]);
        if (strcmp(argv[1], opt) == 0)
            return write_file(argv[2], m);
        snprintf(opt, sizeof(opt), "--check-%s", modes[m]);
        if (strcmp(argv[1], opt) == 0)
            return check_file(argv[2], m);
    }
    test_nr();
    test_gsm();
    test_tetra();
    if (failures) {
        printf("test_m2sdr_analysis_cellular: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis_cellular: all tests passed\n");
    return 0;
}
