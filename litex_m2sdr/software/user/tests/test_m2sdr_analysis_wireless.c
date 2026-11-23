/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the libm2sdr wireless analysis (ADS-B, LoRa, Wi-Fi) on synthetic signals built to
 * the standards, of known parameters.
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

#include "../libm2sdr/m2sdr_analysis_wireless.h"

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

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

/* ADS-B */
/*-------*/

static void hex_bytes(const char *hex, uint8_t *out, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        unsigned v;

        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* A pulse of amplitude amp from t0 to t1 seconds, through a receiver that integrates over each
 * sample (box filter): a sample fully inside reads amp. */
static void add_pulse(float complex *x, size_t count, double fs, double t0, double t1, double amp,
                      double freq_hz, double phase)
{
    size_t n = (size_t)floor(t0 * fs);

    for (; n < count && (double)n / fs < t1; n++) {
        const double a = fmax(t0, (double)n / fs), b = fmin(t1, (double)(n + 1) / fs);
        const double tm = 0.5 * (a + b);

        if (b > a)
            x[n] += (float complex)(amp * (b - a) * fs * cexp(I * (2.0 * M_PI * freq_hz * tm + phase)));
    }
}

/* Mode S extended squitter: preamble, then 112 bits of pulse position modulation. */
static void add_squitter(float complex *x, size_t count, double fs, double t, const uint8_t msg[14],
                         double level_db, double freq_hz)
{
    static const double preamble_us[4] = { 0.0, 1.0, 3.5, 4.5 };
    const double amp = pow(10.0, level_db / 20.0), phase = 2.0 * M_PI * rng_uniform();
    int k;

    for (k = 0; k < 4; k++)
        add_pulse(x, count, fs, t + preamble_us[k] * 1e-6, t + (preamble_us[k] + 0.5) * 1e-6, amp, freq_hz, phase);
    for (k = 0; k < 112; k++) {
        const int bit = (msg[k >> 3] >> (7 - (k & 7))) & 1;
        const double a = t + (8.0 + k + (bit ? 0.0 : 0.5)) * 1e-6;

        add_pulse(x, count, fs, a, a + 0.5e-6, amp, freq_hz, phase);
    }
}

/* Messages of "The 1090 MHz Riddle" (J. Sun), with the values it decodes: identification,
 * an even and an odd airborne position of one aircraft, ground and air velocities. */
#define ADSB_IDENT    "8D4840D6202CC371C32CE0576098"   /* 4840D6, TC 4, "KLM1023". */
#define ADSB_EVEN     "8D40621D58C382D690C8AC2863A7"   /* 40621D, 38000 ft, newest of the pair: */
#define ADSB_ODD      "8D40621D58C386435CC412692AD6"   /* 52.25720 N, 3.91937 E. */
#define ADSB_GROUND   "8D485020994409940838175B284F"   /* 159.20 kt, track 182.88 deg, -832 ft/min. */
#define ADSB_AIR      "8DA05F219B06B6AF189400CBC33F"   /* Heading 243.98 deg, 375 kt TAS, -2304 ft/min. */

static void test_adsb_decode(void)
{
    struct m2sdr_adsb_message m;
    uint8_t msg[14];

    printf("ADS-B, fields of known messages:\n");
    hex_bytes(ADSB_IDENT, msg, 14);
    CHECK("CRC-24 of a right message", m2sdr_adsb_crc(msg, 112) == 0);
    CHECK("decode", m2sdr_adsb_decode(msg, &m) == 0);
    CHECK("DF17, ICAO 4840D6", m.df == 17 && m.icao == 0x4840d6);
    CHECK("TC 4, callsign KLM1023", m.tc == 4 && strcmp(m.callsign, "KLM1023") == 0);
    msg[5] ^= 0x10;
    CHECK("CRC-24 of a wrong bit", m2sdr_adsb_crc(msg, 112) != 0);

    hex_bytes(ADSB_EVEN, msg, 14);
    CHECK("even position: CRC", m2sdr_adsb_crc(msg, 112) == 0);
    m2sdr_adsb_decode(msg, &m);
    CHECK("TC 11, CPR even", m.tc == 11 && m.has_cpr && m.cpr_odd == 0);
    CHECK("altitude 38000 ft", m.has_altitude && m.altitude_ft == 38000);
    hex_bytes(ADSB_ODD, msg, 14);
    CHECK("odd position: CRC", m2sdr_adsb_crc(msg, 112) == 0);
    m2sdr_adsb_decode(msg, &m);
    CHECK("CPR odd", m.has_cpr && m.cpr_odd == 1);

    hex_bytes(ADSB_GROUND, msg, 14);
    CHECK("ground velocity: CRC", m2sdr_adsb_crc(msg, 112) == 0);
    m2sdr_adsb_decode(msg, &m);
    CHECK("TC 19, subtype 1", m.tc == 19 && m.velocity_subtype == 1 && m.has_velocity && m.has_track);
    CHECK_NEAR("ground speed (kt)", m.speed_kt, 159.20, 0.01);
    CHECK_NEAR("track (deg)", m.track_deg, 182.88, 0.01);
    CHECK("vertical rate -832 ft/min", m.has_vrate && m.vrate_fpm == -832);

    hex_bytes(ADSB_AIR, msg, 14);
    CHECK("air velocity: CRC", m2sdr_adsb_crc(msg, 112) == 0);
    m2sdr_adsb_decode(msg, &m);
    CHECK("TC 19, subtype 3, TAS", m.tc == 19 && m.velocity_subtype == 3 && m.airspeed_true);
    CHECK_NEAR("air speed (kt)", m.speed_kt, 375.0, 0.01);
    CHECK_NEAR("heading (deg)", m.track_deg, 243.98, 0.01);
    CHECK("vertical rate -2304 ft/min", m.has_vrate && m.vrate_fpm == -2304);
}

struct adsb_signal {
    double fs;
    double level_db;      /* Of the pulses. */
    double noise_db;      /* Per sample. */
    double offset;        /* Start of the messages off the sample grid, in samples. */
    double freq_hz;
};

/* The first message of an address and a type code, of an even CPR frame if it has one. */
static const struct m2sdr_adsb_message *adsb_find(const struct m2sdr_adsb_result *res, uint32_t icao, int tc)
{
    int i;

    for (i = 0; i < res->nmessages; i++)
        if (res->message[i].icao == icao && res->message[i].tc == tc && !res->message[i].cpr_odd)
            return &res->message[i];
    return NULL;
}

static void test_adsb(void)
{
    static const struct adsb_signal signals[] = {
        /*  fs     level  noise offset  freq_hz */
        { 2.0e6,  -20.0, -35.0, 0.0,      0.0 },
        { 2.0e6,  -30.0, -42.0, 0.0,  150.0e3 },
        { 2.0e6,  -25.0, -40.0, 0.25, -80.0e3 },   /* Pulses across two samples. */
        { 2.0e6,  -20.0, -45.0, 0.5,   50.0e3 },   /* Half a sample off: see the text. */
        { 2.4e6,  -25.0, -40.0, 0.3,   30.0e3 },
        { 4.0e6,  -25.0, -40.0, 0.5,      0.0 },
    };
    static const char *const hex[5] = { ADSB_IDENT, ADSB_ODD, ADSB_GROUND, ADSB_EVEN, ADSB_AIR };
    const double dur = 0.05;
    const size_t max = (size_t)(dur * 4.0e6);
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_adsb_result *res = malloc(sizeof(*res));
    struct m2sdr_adsb_state *st = calloc(1, sizeof(*st));
    struct m2sdr_adsb_config cfg;
    const struct m2sdr_adsb_message *m;
    uint8_t msg[14];
    size_t i, count;
    int k;

    printf("ADS-B:\n");
    if (!x || !res || !st) {
        CHECK("allocation", 0);
        free(x);
        free(res);
        free(st);
        return;
    }
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        const struct adsb_signal *sig = &signals[i];
        const double noise_p = pow(10.0, sig->noise_db / 10.0);
        double t0, level, snr;

        count = (size_t)(dur * sig->fs);
        memset(x, 0, count * sizeof(*x));
        /* Five messages, 5 ms apart, the even position last: the newest of the pair. */
        for (k = 0; k < 5; k++) {
            hex_bytes(hex[k], msg, 14);
            add_squitter(x, count, sig->fs, (1000.0 + 5000.0 * k) * 1e-6 + sig->offset / sig->fs, msg,
                         sig->level_db, sig->freq_hz);
        }
        add_noise(x, count, noise_p);
        printf(" %.1f MSPS, pulses %.0f dBFS, SNR %.0f dB, %.2f sample off, %.0f kHz\n", sig->fs / 1e6,
               sig->level_db, sig->level_db - sig->noise_db, sig->offset, sig->freq_hz / 1e3);
        t0 = now_ms();
        CHECK("analyse", m2sdr_adsb_analyse(x, count, sig->fs, 10.0, NULL, NULL, res) == 0);
        printf("  time of one analysis: %.2f ms for %.0f ms of samples\n", now_ms() - t0, dur * 1e3);
        CHECK("five messages", res->messages == 5 && res->nmessages == 5);
        /* Pulses on the samples: no bit wrong at these SNR (the halves of a bit differ by over
         * 4 noise rms). Across samples, the contrast is lower: a bit corrected now and then. */
        printf("  corrected: %d\n", res->corrected);
        CHECK("bits corrected", sig->offset == 0.0 ? res->corrected == 0 : res->corrected <= 1);
        CHECK_NEAR("message rate (/s)", res->message_rate, 5.0 / dur, 1e-6);
        /* The median of the samples is noise: their power within the spread of the median of
         * 65536 exponential samples (well under 0.1 dB). */
        CHECK_NEAR("noise floor (dBFS)", res->noise_dbfs, sig->noise_db, 0.2);
        if (res->nmessages < 5)
            continue;
        CHECK_NEAR("time of the first message (us)", (res->message[0].time_s - 10.0) * 1e6,
                   1000.0 + sig->offset / sig->fs * 1e6, 1e6 / sig->fs);
        m = adsb_find(res, 0x4840d6, 4);
        CHECK("identification KLM1023", m && strcmp(m->callsign, "KLM1023") == 0);
        m = adsb_find(res, 0x485020, 19);
        CHECK("ground velocity", m && fabs(m->speed_kt - 159.20) < 0.01 && m->vrate_fpm == -832);
        m = adsb_find(res, 0xa05f21, 19);
        CHECK("air velocity", m && fabs(m->speed_kt - 375.0) < 0.01);
        m = adsb_find(res, 0x40621d, 11);
        /* The even frame comes last: the pair decoded within the capture, without a state. */
        CHECK("position from the pair", m && m->cpr_odd == 0 && m->has_position);
        if (m) {
            CHECK_NEAR("latitude (deg)", m->lat_deg, 52.25720, 1e-4);
            CHECK_NEAR("longitude (deg)", m->lon_deg, 3.91937, 1e-4);
        }
        level = snr = 0;
        for (k = 0; k < 5; k++) {
            level += res->message[k].level_dbfs / 5.0;
            snr   += res->message[k].snr_db / 5.0;
        }
        if (sig->offset == 0.0) {
            /* Pulses on whole samples: the chips read their power exactly, and the noise its own
             * over the 116 chips without a pulse of each message: 9 % (0.4 dB) rms for one,
             * 0.2 dB over five. */
            CHECK_NEAR("pulse level (dBFS)", level, sig->level_db, 0.3);
            CHECK_NEAR("SNR (dB)", snr, sig->level_db - sig->noise_db, 0.5);
        } else {
            /* Pulses across samples: the box filter spreads each pulse on two samples, a chip
             * reads (1 - d)^3 + d^3 of its power at 2 MSPS, 6 dB low at d = 0.5 sample; less
             * at higher rates. */
            CHECK("pulse level within 6.5 dB below", level <= sig->level_db + 0.3 && level >= sig->level_db - 6.5);
        }
    }

    /* A wrong bit: the parity corrects it, the fields are those of the message sent. */
    count = (size_t)(dur * 2.0e6);
    memset(x, 0, count * sizeof(*x));
    hex_bytes(ADSB_GROUND, msg, 14);
    msg[6] ^= 0x04;
    add_squitter(x, count, 2.0e6, 2e-3, msg, -25.0, 0.0);
    add_noise(x, count, pow(10.0, -40.0 / 10.0));
    printf(" One wrong bit\n");
    CHECK("analyse", m2sdr_adsb_analyse(x, count, 2.0e6, 0.0, NULL, NULL, res) == 0);
    CHECK("corrected", res->messages == 1 && res->corrected == 1 && res->message[0].corrected);
    CHECK("its fields", res->messages == 1 && res->message[0].icao == 0x485020 &&
                        fabs(res->message[0].speed_kt - 159.20) < 0.01);
    memset(&cfg, 0, sizeof(cfg));
    cfg.no_correction = true;
    CHECK("not kept without correction", m2sdr_adsb_analyse(x, count, 2.0e6, 0.0, &cfg, NULL, res) == 0 &&
                                        res->messages == 0);

    /* The pair over two captures, 2 s apart: the state keeps the odd frame. */
    printf(" CPR pair over two captures\n");
    memset(x, 0, count * sizeof(*x));
    hex_bytes(ADSB_ODD, msg, 14);
    add_squitter(x, count, 2.0e6, 3e-3, msg, -25.0, 0.0);
    add_noise(x, count, pow(10.0, -40.0 / 10.0));
    CHECK("first capture: no position", m2sdr_adsb_analyse(x, count, 2.0e6, 100.0, NULL, st, res) == 0 &&
                                       res->messages == 1 && !res->message[0].has_position);
    memset(x, 0, count * sizeof(*x));
    hex_bytes(ADSB_EVEN, msg, 14);
    add_squitter(x, count, 2.0e6, 3e-3, msg, -25.0, 0.0);
    add_noise(x, count, pow(10.0, -40.0 / 10.0));
    CHECK("second capture: position", m2sdr_adsb_analyse(x, count, 2.0e6, 102.0, NULL, st, res) == 0 &&
                                     res->messages == 1 && res->message[0].has_position);
    CHECK("aircraft kept", st->aircraft == 1 && st->ac[0].icao == 0x40621d && st->ac[0].messages == 2 &&
                           st->ac[0].has_position && st->ac[0].altitude_ft == 38000);
    if (res->messages == 1) {
        CHECK_NEAR("latitude (deg)", res->message[0].lat_deg, 52.25720, 1e-4);
        CHECK_NEAR("longitude (deg)", res->message[0].lon_deg, 3.91937, 1e-4);
    }
    /* One frame near a receiver position: the local decoding gives the same place. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.has_reference = true;
    cfg.ref_lat_deg   = 52.0;
    cfg.ref_lon_deg   = 4.5;
    CHECK("single frame near the receiver", m2sdr_adsb_analyse(x, count, 2.0e6, 0.0, &cfg, NULL, res) == 0 &&
                                           res->messages == 1 && res->message[0].has_position);
    if (res->messages == 1) {
        CHECK_NEAR("latitude (deg)", res->message[0].lat_deg, 52.25720, 1e-4);
        CHECK_NEAR("longitude (deg)", res->message[0].lon_deg, 3.91937, 1e-4);
    }

    /* Noise only, and the time of a long capture. */
    count = (size_t)(dur * 4.0e6);
    memset(x, 0, count * sizeof(*x));
    add_noise(x, count, 0.001);
    CHECK("noise: no message", m2sdr_adsb_analyse(x, count, 4.0e6, 0.0, NULL, NULL, res) == 0 && res->messages == 0);
    CHECK("bad sample rate", m2sdr_adsb_analyse(x, count, 1.0e6, 0.0, NULL, NULL, res) != 0);
    CHECK("too short", m2sdr_adsb_analyse(x, 200, 2.0e6, 0.0, NULL, NULL, res) != 0);
    CHECK("NULL samples", m2sdr_adsb_analyse(NULL, count, 2.0e6, 0.0, NULL, NULL, res) != 0);
    free(x);
    free(res);
    free(st);

    {
        /* 0.5 s at 2 MSPS with 200 messages: a busy sky. */
        const double fs = 2.0e6;
        const size_t n = (size_t)(0.5 * fs);
        float complex *y = calloc(n, sizeof(*y));
        struct m2sdr_adsb_result *r = malloc(sizeof(*r));
        double t0;

        if (y && r) {
            for (k = 0; k < 200; k++) {
                hex_bytes(hex[k % 5], msg, 14);
                add_squitter(y, n, fs, (500.0 + 2400.0 * k) * 1e-6, msg, -20.0 - (k % 7), 0.0);
            }
            add_noise(y, n, pow(10.0, -40.0 / 10.0));
            t0 = now_ms();
            CHECK("busy sky: analyse", m2sdr_adsb_analyse(y, n, fs, 0.0, NULL, NULL, r) == 0);
            printf("  time of one analysis: %.2f ms for 500 ms of samples\n", now_ms() - t0);
            CHECK("busy sky: 200 messages", r->messages == 200);
        }
        free(y);
        free(r);
    }
}

int main(void)
{
    test_adsb_decode();
    test_adsb();
    if (failures) {
        printf("test_m2sdr_analysis_wireless: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis_wireless: all tests passed\n");
    return 0;
}
