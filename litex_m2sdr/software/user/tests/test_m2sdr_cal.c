/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the calibration estimators on synthetic LTE and GSM downlink signals.
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

#include "../m2sdr_cal_dsp.h"
#include "../m2sdr_cal_store.h"
#include "../kissfft/kiss_fft.h"

#define FS M2SDR_CAL_SAMPLE_RATE

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static unsigned rng_state = 12345;
static double rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return ((rng_state >> 8) + 0.5) / 16777216.0;
}
static double complex noise(double sigma)
{
    const double r = sigma * sqrt(-log(rnd())), a = 2 * M_PI * rnd();
    return r * cos(a) + I * r * sin(a);
}

/* Frequency shift and noise, in place. */
static void channel(float complex *x, size_t n, double cfo_hz, double sigma)
{
    size_t i;
    for (i = 0; i < n; i++)
        x[i] = (float complex)(x[i] * cexp(I * 2 * M_PI * cfo_hz * (double)i / FS) + noise(sigma));
}

/* LTE FDD downlink, 6 resource blocks, random QPSK on every used subcarrier, PSS of N_ID_2 in
 * the last symbol of the slots 0 and 10. `offset` samples of the frame are skipped. */
static float complex *lte_signal(size_t n, int nid2, size_t offset)
{
    static const unsigned roots[3] = { 25, 29, 34 };
    const size_t total = n + offset + 2 * 960;
    float complex *x = calloc(total, sizeof(*x));
    kiss_fft_cfg cfg = kiss_fft_alloc(128, 1, NULL, NULL);
    kiss_fft_cpx in[128], out[128];
    size_t pos = 0;
    int slot = 0, sym, k;

    while (pos + 960 <= total) {
        for (sym = 0; sym < 7; sym++) {
            const int cp = sym == 0 ? 10 : 9;
            memset(in, 0, sizeof(in));
            if (sym == 6 && (slot % 10) == 0) {
                for (k = 0; k < 62; k++) {
                    const double ph = -M_PI * roots[nid2] * (k < 31 ? k * (k + 1.0) : (k + 1.0) * (k + 2.0)) / 63.0;
                    const int bin = k < 31 ? 128 - 31 + k : k - 30;
                    in[bin].r = (float)cos(ph);
                    in[bin].i = (float)sin(ph);
                }
            } else {
                for (k = 1; k <= 36; k++) {
                    in[k].r       = rnd() < 0.5 ? -0.7071f : 0.7071f;
                    in[k].i       = rnd() < 0.5 ? -0.7071f : 0.7071f;
                    in[128 - k].r = rnd() < 0.5 ? -0.7071f : 0.7071f;
                    in[128 - k].i = rnd() < 0.5 ? -0.7071f : 0.7071f;
                }
            }
            kiss_fft(cfg, in, out);
            for (k = 0; k < cp; k++)
                x[pos + k] = (out[128 - cp + k].r + I * out[128 - cp + k].i) * 0.01f;
            for (k = 0; k < 128; k++)
                x[pos + cp + k] = (out[k].r + I * out[k].i) * 0.01f;
            pos += cp + 128;
        }
        slot = (slot + 1) % 20;
    }
    free(cfg);
    memmove(x, x + offset, n * sizeof(*x));
    return x;
}

/* GSM carrier with a BCCH: constant envelope, random +-90 degrees per bit, and an FCCH burst
 * (148 bits of +90 degrees: a tone at +67.7 kHz) every 10 frames. With `cw`, a continuous tone
 * at the FCCH frequency instead. */
static float complex *gsm_signal(size_t n, int cw)
{
    const double bit_s = 48.0 / 13e6;                 /* 3.69 us. */
    const double slot_bits = 156.25;
    float complex *x = calloc(n, sizeof(*x));
    double phase = 0;
    long last_bit = -1;
    double step = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        const double t = (double)i / FS;
        const long bit = (long)(t / bit_s);
        if (bit != last_bit) {
            const double in_frame = fmod((double)bit, 8 * slot_bits);
            const long frame = (long)((double)bit / (8 * slot_bits));
            const int fcch = (frame % 10) == 0 && in_frame >= 3 && in_frame < 3 + 142;
            step = (cw || fcch || rnd() < 0.5) ? 1 : -1;
            last_bit = bit;
        }
        phase += step * (M_PI / 2) / (bit_s * FS);
        x[i] = (float complex)(0.2 * cexp(I * phase));
    }
    return x;
}

static void test_lte(double cfo_hz, int nid2, double sigma, size_t offset, double tol_hz)
{
    static const int roots[3] = { 25, 29, 34 };
    const size_t n = (size_t)(0.2 * FS);
    float complex *x = lte_signal(n, nid2, offset);
    struct m2sdr_cal_estimate est;

    channel(x, n, cfo_hz, sigma);
    CHECK(m2sdr_cal_lte_estimate(x, n, 30e3, &est) == 0, "lte estimate failed");
    printf("LTE cfo %+9.1f Hz, N_ID_2 %d, noise %.3f: found %d, root %d, metric %6.1f, cfo %+9.1f Hz, spread %.1f Hz\n",
           cfo_hz, nid2, sigma, est.found, est.id, est.quality, est.cfo_hz, est.spread_hz);
    CHECK(est.found, "LTE signal not found");
    CHECK(est.id == roots[nid2], "PSS root %d, expected %d", est.id, roots[nid2]);
    CHECK(fabs(est.cfo_hz - cfo_hz) < tol_hz, "LTE cfo %.1f Hz, expected %.1f Hz", est.cfo_hz, cfo_hz);
    free(x);
}

static void test_gsm(double cfo_hz, double sigma, double tol_hz)
{
    const size_t n = (size_t)(0.5 * FS);
    float complex *x = gsm_signal(n, 0);
    struct m2sdr_cal_estimate est;

    channel(x, n, cfo_hz, sigma);
    CHECK(m2sdr_cal_gsm_estimate(x, n, 20e3, &est) == 0, "gsm estimate failed");
    printf("GSM cfo %+9.1f Hz, noise %.3f: found %d, %d bursts, on grid %.2f, cfo %+9.1f Hz, spread %.1f Hz\n",
           cfo_hz, sigma, est.found, est.count, est.quality, est.cfo_hz, est.spread_hz);
    CHECK(est.found, "GSM signal not found");
    CHECK(est.count >= 9 && est.count <= 12, "%d FCCH bursts in 0.5 s", est.count);
    CHECK(fabs(est.cfo_hz - cfo_hz) < tol_hz, "GSM cfo %.1f Hz, expected %.1f Hz", est.cfo_hz, cfo_hz);
    free(x);
}

static void test_rejections(void)
{
    const size_t n = (size_t)(0.2 * FS);
    float complex *x = calloc(n, sizeof(*x));
    struct m2sdr_cal_estimate est;

    /* Noise alone is neither. */
    channel(x, n, 0, 0.05);
    CHECK(m2sdr_cal_lte_estimate(x, n, 30e3, &est) == 0 && !est.found, "LTE found in noise (metric %.1f)", est.quality);
    CHECK(m2sdr_cal_gsm_estimate(x, n, 20e3, &est) == 0 && !est.found, "GSM found in noise");
    free(x);

    /* A continuous tone at the FCCH frequency is not a GSM carrier. */
    x = gsm_signal(n, 1);
    channel(x, n, -1000, 0.01);
    CHECK(m2sdr_cal_gsm_estimate(x, n, 20e3, &est) == 0 && !est.found, "GSM found in a continuous tone (%d bursts)", est.count);
    free(x);

    /* A GSM carrier is not an LTE one, and the reverse. */
    x = gsm_signal(n, 0);
    channel(x, n, 500, 0.01);
    CHECK(m2sdr_cal_lte_estimate(x, n, 30e3, &est) == 0 && !est.found, "LTE found in a GSM carrier (metric %.1f)", est.quality);
    free(x);
    x = lte_signal(n, 1, 0);
    channel(x, n, 500, 0.01);
    CHECK(m2sdr_cal_gsm_estimate(x, n, 20e3, &est) == 0 && !est.found, "GSM found in an LTE carrier (%d bursts)", est.count);
    free(x);
}

static void test_numbers(void)
{
    CHECK(fabs(m2sdr_cal_earfcn_to_hz(6300) - 806e6) < 1, "EARFCN 6300");      /* Band 20. */
    CHECK(fabs(m2sdr_cal_earfcn_to_hz(1300) - 1815e6) < 1, "EARFCN 1300");     /* Band 3. */
    CHECK(fabs(m2sdr_cal_earfcn_to_hz(3350) - 2680e6) < 1, "EARFCN 3350");     /* Band 7. */
    CHECK(m2sdr_cal_earfcn_to_hz(70000) == 0.0, "EARFCN out of the bands");
    CHECK(fabs(m2sdr_cal_arfcn_to_hz(85) - 952e6) < 1, "ARFCN 85");
    CHECK(fabs(m2sdr_cal_arfcn_to_hz(1000) - 930.2e6) < 1, "ARFCN 1000");
    CHECK(fabs(m2sdr_cal_arfcn_to_hz(512) - 1805.2e6) < 1, "ARFCN 512");
    CHECK(m2sdr_cal_arfcn_to_hz(300) == 0.0, "ARFCN out of the bands");
    {
        unsigned band = 0;
        CHECK(m2sdr_cal_hz_to_earfcn(1815e6, &band) == 1300 && band == 3, "1815 MHz to EARFCN");
        CHECK(m2sdr_cal_hz_to_earfcn(806e6, &band) == 6300 && band == 20, "806 MHz to EARFCN");
        CHECK(m2sdr_cal_hz_to_earfcn(1815.05e6, NULL) == -1, "off the raster");
        CHECK(m2sdr_cal_hz_to_arfcn(952e6) == 85 && m2sdr_cal_hz_to_arfcn(930.2e6) == 1000 &&
              m2sdr_cal_hz_to_arfcn(1805.2e6) == 512 && m2sdr_cal_hz_to_arfcn(700e6) == -1, "frequency to ARFCN");
    }
    /* A clock 3 ppm fast receives an 816 MHz carrier 2448 Hz low. */
    CHECK(fabs(m2sdr_cal_refclk_ppm(-2448.0, 816e6) - 3.0) < 1e-9, "ppm sign");
}

static void test_iq(void)
{
    const int16_t iq[] = { 1024, -1024, 7, 7,   -1024, 1024, 7, 7 };   /* 2T2R: channel A then B. */
    float complex x[2];

    m2sdr_cal_iq_to_complex(iq, 2, 4, 12, x);
    CHECK(fabs(creal(x[0]) - 0.5) < 1e-6 && fabs(cimag(x[0]) + 0.5) < 1e-6, "iq scale");
    CHECK(cabs(x[0] + x[1]) < 1e-6, "mean not removed");
}

/* A tone 1 kHz above a channel 1.3 MHz from the centre of a wide capture comes out at 1 kHz with
 * its amplitude; a tone 1.5 MHz further is filtered out. */
static void test_channelize(void)
{
    const double fs_in = FS * M2SDR_CAL_SCAN_DECIM;
    const size_t n = 200000;
    float complex *x = malloc(n * sizeof(*x));
    float complex *y = malloc(n / M2SDR_CAL_SCAN_DECIM * sizeof(*y));
    double complex a = 0;
    double p = 0;
    size_t ny = 0, i;

    for (i = 0; i < n; i++)
        x[i] = (float complex)(cexp(I * 2 * M_PI * (1.3e6 + 1000.0) * (double)i / fs_in) +
                               cexp(I * 2 * M_PI * (2.8e6) * (double)i / fs_in));
    CHECK(m2sdr_cal_channelize(x, n, 1.3e6, y, &ny) == 0 && ny > n / M2SDR_CAL_SCAN_DECIM - 100, "channelize");
    for (i = 0; i + 1 < ny; i++) {
        a += conj(y[i]) * y[i + 1];
        p += creal(y[i]) * creal(y[i]) + cimag(y[i]) * cimag(y[i]);
    }
    /* The tone outside the channel leaves a trace that moves this simple estimate by a few Hz. */
    CHECK(fabs(carg(a) / (2 * M_PI) * FS - 1000.0) < 5.0, "channel tone at %.1f Hz", carg(a) / (2 * M_PI) * FS);
    CHECK(fabs(p / (double)ny - 1.0) < 0.05, "channel power %.3f", p / (double)ny);
    free(x);
    free(y);
}

static void test_store(void)
{
    struct m2sdr_cal_record rec, back;
    uint8_t buf[M2SDR_CAL_RECORD_BYTES];

    memset(&rec, 0, sizeof(rec));
    rec.refclk_ppm    = 3.0471;
    rec.spread_ppm    = 0.021;
    rec.temperature_c = 61.5;
    rec.unix_time     = 1794850000;
    rec.signals       = 4;
    rec.fpga_dna      = 0x0123456789abcdULL;
    snprintf(rec.note, sizeof(rec.note), "LTE 816 MHz, GSM 952 MHz");

    m2sdr_cal_record_encode(&rec, buf);
    CHECK(m2sdr_cal_record_decode(buf, &back) == 0, "decode of an encoded record");
    CHECK(back.refclk_ppm == rec.refclk_ppm && back.signals == 4 && back.fpga_dna == rec.fpga_dna &&
          strcmp(back.note, rec.note) == 0, "record fields");
    buf[20] ^= 1;
    CHECK(m2sdr_cal_record_decode(buf, &back) != 0, "corrupted record accepted");
    memset(buf, 0xff, sizeof(buf));
    CHECK(m2sdr_cal_record_decode(buf, &back) != 0, "erased flash accepted");
}

int main(void)
{
    test_numbers();
    test_iq();
    test_channelize();
    test_store();
    test_lte(-2450.0, 2, 0.002, 1234, 5.0);
    test_lte(+7400.0, 0, 0.01, 0, 5.0);       /* Near half the subcarrier spacing. */
    test_lte(-23750.0, 1, 0.01, 5000, 5.0);   /* Beyond one subcarrier. */
    test_lte(0.0, 2, 0.03, 9000, 25.0);       /* SNR about 9 dB. */
    test_lte(+300.0, 0, 0.085, 77, 60.0);     /* SNR about 0 dB. */
    test_gsm(-2900.0, 0.01, 15.0);
    test_gsm(+12000.0, 0.02, 20.0);
    test_gsm(0.0, 0.1, 60.0);                 /* SNR about 3 dB in the 1.92 MHz capture. */
    test_rejections();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_m2sdr_cal: all tests passed\n");
    return 0;
}
