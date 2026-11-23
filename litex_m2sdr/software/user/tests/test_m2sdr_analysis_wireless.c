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
        /* Pulses on the samples: a bit is wrong with a probability of exp(-snr / 2) / 2, 2e-4 at
         * 12 dB, a message every 50. Across samples, the contrast is lower. One corrected at most. */
        printf("  corrected: %d\n", res->corrected);
        CHECK("bits corrected", res->corrected <= 1);
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
             * 0.2 dB over five; three times that, and 0.2 dB for the halves told apart by their
             * power at 12 dB. */
            CHECK_NEAR("pulse level (dBFS)", level, sig->level_db, 0.3);
            CHECK_NEAR("SNR (dB)", snr, sig->level_db - sig->noise_db, 0.8);
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

/* LoRa */
/*------*/

struct lora_signal {
    int    sf;
    double bw;
    double start_s;
    double freq_hz;
    double snr_db;          /* In the bandwidth. */
    int    preamble;
    int    sync_word;
    int    symbols;
};

/* A LoRa packet at fs: preamble of up-chirps, the two symbols of the sync word ((sw >> 4) * 8 and
 * (sw & 15) * 8), 2.25 down-chirps, data symbols; continuous phase. Symbol s starts at
 * -bw/2 + s bw / 2^sf and wraps at bw/2. */
static void add_lora(float complex *x, size_t count, double fs, const struct lora_signal *sig,
                     const uint16_t *data, double amp)
{
    const int n = 1 << sig->sf;
    const double t_sym = n / sig->bw;
    const int nseg = sig->preamble + 2 + 3 + sig->symbols;
    double phase = 2.0 * M_PI * rng_uniform();
    size_t i;

    for (i = (size_t)ceil(sig->start_s * fs); i < count; i++) {
        const double t = ((double)i + 0.5) / fs - sig->start_s;
        double u = t / t_sym, fr;
        int seg = (int)floor(u), s = 0, down = 0;

        if (seg >= sig->preamble + 4) {
            /* The quarter down-chirp, then the data from 0.25 symbol later. */
            if (u < sig->preamble + 4.25) {
                seg  = sig->preamble + 4;
                down = 1;
            } else {
                u  -= 0.25;
                seg = (int)floor(u);
                if (seg >= nseg - 1)
                    break;
                s = data[seg - sig->preamble - 4];
            }
        } else if (seg >= sig->preamble + 2) {
            down = 1;
        } else if (seg >= sig->preamble) {
            s = seg == sig->preamble ? (sig->sync_word >> 4) * 8 : (sig->sync_word & 15) * 8;
        }
        u -= floor(u);
        fr = down ? sig->bw / 2.0 - sig->bw * u : sig->bw * fmod((double)s / n + u, 1.0) - sig->bw / 2.0;
        x[i] += (float complex)(amp * cexp(I * phase));
        phase += 2.0 * M_PI * (fr + sig->freq_hz) / fs;
    }
}

/* Packets of sigs[], the noise of the SNR of the first one; the SNR of the others is checked when
 * they do not overlap (else it reads the SINR). */
static void test_lora_signal(float complex *x, size_t count, double fs, const struct lora_signal *sigs, int nsig,
                             const struct m2sdr_lora_config *cfg, bool check_others)
{
    struct m2sdr_lora_result *res = malloc(sizeof(*res));
    uint16_t data[4][M2SDR_LORA_MAX_SYMBOLS];
    double amp = 0.1, t0;
    int s, k;

    if (!res) {
        CHECK("allocation", 0);
        return;
    }
    memset(x, 0, count * sizeof(*x));
    for (s = 0; s < nsig; s++) {
        for (k = 0; k < sigs[s].symbols; k++)
            data[s][k] = (uint16_t)(rng_uniform() * (1 << sigs[s].sf));
        add_lora(x, count, fs, &sigs[s], data[s], amp * pow(10.0, (sigs[s].snr_db - sigs[0].snr_db) / 20.0));
    }
    /* Noise over fs, of power amp^2 / snr in the bandwidth of the first packet. */
    add_noise(x, count, amp * amp / pow(10.0, sigs[0].snr_db / 10.0) * fs / sigs[0].bw);
    t0 = now_ms();
    CHECK("analyse", m2sdr_lora_analyse(x, count, fs, cfg, res) == 0);
    printf("  time of one analysis: %.1f ms for %.0f ms of samples\n", now_ms() - t0, count / fs * 1e3);
    printf("  packets: %d\n", res->packets);
    CHECK("packets", res->packets == nsig && res->npackets == nsig);
    for (s = 0; s < nsig && s < res->npackets; s++) {
        const struct lora_signal *sig = &sigs[s];
        const struct m2sdr_lora_packet *pk = &res->packet[s];
        const double t_sym = (1 << sig->sf) / sig->bw, bin_hz = sig->bw / (1 << sig->sf);
        int wrong = 0;

        printf("  SF%d, %.0f kHz, offset %.1f kHz, SNR %.1f dB\n", sig->sf, sig->bw / 1e3, sig->freq_hz / 1e3, sig->snr_db);
        CHECK("SF and bandwidth", pk->sf == sig->sf && pk->bw_hz == sig->bw);
        /* The peaks are interpolated to a small part of a bin at these SNR. */
        CHECK_NEAR("carrier offset (Hz)", pk->freq_error_hz, sig->freq_hz, 0.1 * bin_hz);
        /* The timing comes from the same peaks: a small part of a sample of the bandwidth. */
        CHECK_NEAR("start (us)", pk->time_s * 1e6, sig->start_s * 1e6, 0.5e6 / sig->bw);
        if (s == 0 || check_others) {
            /* The peak of a chirp of SNR r holds N^2 A^2, with N sigma^2 of noise: its power
             * varies by sqrt(2 / (N r)), over the P chirps of the preamble by sqrt(P) less. The
             * noise of a bin is the median of N bins: 1.2 / sqrt(N P). Three times that, and
             * 0.3 dB for the chirp cut at the band edges (its SNR stops near 21 dB at SF7). */
            const double nn = 1 << sig->sf, r = pow(10.0, sig->snr_db / 10.0);
            const double rel = sqrt(2.0 / (nn * r * sig->preamble) + 1.44 / (nn * sig->preamble) * pow(1.0 + 1.0 / (nn * r), 2.0));

            CHECK_NEAR("SNR (dB)", pk->snr_db, sig->snr_db, 3.0 * 4.34 * rel + 0.3);
        }
        if (nsig == 1)
            /* The chirp and the noise of the bandwidth: amp^2 (1 + 1 / snr), within 0.2 dB. */
            CHECK_NEAR("RSSI (dBFS)", pk->rssi_dbfs, 10.0 * log10(amp * amp * (1.0 + pow(10.0, -sig->snr_db / 10.0))), 0.2);
        CHECK("preamble, sync word", pk->preamble_symbols == sig->preamble && pk->sync && pk->sync_word == sig->sync_word);
        for (k = 0; k < sig->symbols && k < pk->symbols; k++)
            wrong += pk->symbol[k] != data[s][k];
        CHECK("data symbols", pk->symbols == sig->symbols && wrong == 0 && pk->complete);
        CHECK_NEAR("airtime (ms)", pk->airtime_s * 1e3, (sig->preamble + 4.25 + sig->symbols) * t_sym * 1e3, 1e3 / sig->bw);
    }
    free(res);
}

static void test_lora(void)
{
    static const struct lora_signal single[] = {
        /* sf  bw       start      freq_hz  snr  pre  sync  symbols */
        {  7, 125e3, 0.0100037,  12300.0,  0.0,   8, 0x34, 20 },
        {  9, 250e3, 0.0033333, -20000.0, -8.0,   8, 0x12, 30 },
        {  8, 500e3, 0.0011111,  40000.0,  5.0,  12, 0x34, 40 },
        { 12, 125e3, 0.0500000,   3000.0, -15.0,  8, 0x34, 10 },
    };
    static const struct lora_signal pair[] = {
        /* Two SFs on the same channel at once: orthogonal. The preamble of the second is under
         * the data of the first: its SNR reads the SINR. */
        {  7, 125e3, 0.0200000,   1000.0,  3.0,   8, 0x34, 40 },
        { 10, 125e3, 0.0300000,   1000.0,  0.0,   8, 0x34,  8 },
    };
    const double fs = 1e6;
    const size_t max = (size_t)(0.85 * fs);   /* The SF12 packet and one symbol after it. */
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_lora_result *res = malloc(sizeof(*res));
    struct m2sdr_lora_config cfg;
    size_t i;

    printf("LoRa:\n");
    if (!x || !res) {
        CHECK("allocation", 0);
        free(x);
        free(res);
        return;
    }
    for (i = 0; i < sizeof(single) / sizeof(single[0]); i++) {
        const size_t count = single[i].sf == 12 ? max : max / 4;

        test_lora_signal(x, count, fs, &single[i], 1, NULL, true);
    }
    printf(" two packets at once\n");
    test_lora_signal(x, max / 2, fs, pair, 2, NULL, false);
    printf(" known SF and bandwidth\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.sf    = 9;
    cfg.bw_hz = 250e3;
    test_lora_signal(x, max / 4, fs, &single[1], 1, &cfg, true);

    memset(x, 0, max * sizeof(*x));
    add_noise(x, max, 0.01);
    CHECK("noise: no packet", m2sdr_lora_analyse(x, max, fs, NULL, res) == 0 && res->packets == 0);
    CHECK("bad sample rate", m2sdr_lora_analyse(x, max, 1.1e6, NULL, res) != 0);
    CHECK("bad SF", (cfg.sf = 6, m2sdr_lora_analyse(x, max, fs, &cfg, res) != 0));
    CHECK("too short", m2sdr_lora_analyse(x, 1000, fs, NULL, res) != 0);
    free(x);
    free(res);
}

/* Wi-Fi */
/*-------*/

#define WIFI_FS 20.0e6

/* 802.11 OFDM transmitter, to the standard: L-STF, L-LTF, L-SIG, then the data (scrambler,
 * convolutional code and puncturing, interleaver, Gray mapping, pilots) or an HT-SIG. Samples
 * of mean power 1. */

static const int8_t wifi_ltf[53] = {
    1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1,
    0,
    1, -1, -1, 1, 1, -1, 1, -1, 1, -1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, 1, -1, 1, 1, 1, 1,
};

struct wifi_rate_tx {
    int mbps, rbits, nbpsc, ncbps, ndbps, punct;
};

static const struct wifi_rate_tx wifi_rates[8] = {
    {  6, 0xd, 1,  48,  24, 0 }, {  9, 0xf, 1,  48,  36, 3 }, { 12, 0x5, 2,  96,  48, 0 },
    { 18, 0x7, 2,  96,  72, 3 }, { 24, 0x9, 4, 192,  96, 0 }, { 36, 0xb, 4, 192, 144, 3 },
    { 48, 0x1, 6, 288, 192, 2 }, { 54, 0x3, 6, 288, 216, 3 },
};

static int8_t wifi_pol[127];

static void wifi_init(void)
{
    unsigned st = 0x7f;
    int n;

    for (n = 0; n < 127; n++) {
        const unsigned t = ((st >> 6) ^ (st >> 3)) & 1u;

        wifi_pol[n] = t ? -1 : 1;
        st = ((st << 1) | t) & 0x7fu;
    }
}

/* Rate 1/2, K = 7, generators 133 and 171 (octal), from state 0; then punctured. Returns the
 * coded bits. */
static int wifi_encode(const uint8_t *bits, int n, int punct, uint8_t *out)
{
    unsigned st = 0;
    int i, m = 0;

    for (i = 0; i < n; i++) {
        const unsigned w = ((unsigned)bits[i] << 6) | st;
        const int a = __builtin_parity(w & 0x5bu), b = __builtin_parity(w & 0x79u);
        const int ph3 = (2 * i) % 6, ph2 = (2 * i) % 4;

        st = w >> 1;
        /* 3/4: A0 B0 A1 B2 of A0 B0 A1 B1 A2 B2; 2/3: A0 B0 A1 of A0 B0 A1 B1. */
        if (punct == 3) {
            if (ph3 != 4)
                out[m++] = (uint8_t)a;
            if (ph3 != 2)
                out[m++] = (uint8_t)b;
        } else if (punct == 2) {
            out[m++] = (uint8_t)a;
            if (ph2 != 2)
                out[m++] = (uint8_t)b;
        } else {
            out[m++] = (uint8_t)a;
            out[m++] = (uint8_t)b;
        }
    }
    return m;
}

static float complex wifi_map(const uint8_t *b, int nbpsc)
{
    switch (nbpsc) {
    case 1:
        return 2.0f * b[0] - 1.0f;
    case 2:
        return ((2.0f * b[0] - 1.0f) + I * (2.0f * b[1] - 1.0f)) / sqrtf(2.0f);
    case 4:
        return ((2.0f * b[0] - 1.0f) * (b[1] ? 1.0f : 3.0f) + I * (2.0f * b[2] - 1.0f) * (b[3] ? 1.0f : 3.0f)) / sqrtf(10.0f);
    default: {
        const float mi = b[1] ? (b[2] ? 3.0f : 1.0f) : (b[2] ? 5.0f : 7.0f);
        const float mq = b[4] ? (b[5] ? 3.0f : 1.0f) : (b[5] ? 5.0f : 7.0f);

        return ((2.0f * b[0] - 1.0f) * mi + I * (2.0f * b[3] - 1.0f) * mq) / sqrtf(42.0f);
    }
    }
}

/* 64-point IFFT of carriers -26..26 (X[k + 26]), scaled to mean power 1 over 52 carriers. */
static void wifi_ifft(const float complex X[53], float complex out[64])
{
    int n, k;

    for (n = 0; n < 64; n++) {
        double complex v = 0;

        for (k = -26; k <= 26; k++)
            v += X[k + 26] * cexp(I * 2.0 * M_PI * k * n / 64.0);
        out[n] = (float complex)(v / sqrt(52.0));
    }
}

/* OFDM symbol i (0: L-SIG) of 48 coded and interleaved bits per BPSK carrier... of ncbps bits,
 * nbpsc per carrier, rot on the data carriers (j for QBPSK): 80 samples. */
static void wifi_symbol(const uint8_t *coded, int ncbps, int nbpsc, int i, float complex rot, float complex *out)
{
    static const float pilots[4] = { 1.0f, 1.0f, 1.0f, -1.0f };
    uint8_t il[288];
    float complex X[53] = { 0 }, t[64];
    int k, c = 0, p = 0;

    for (k = 0; k < ncbps; k++) {
        const int s = nbpsc / 2 > 1 ? nbpsc / 2 : 1;
        const int a = (ncbps / 16) * (k % 16) + k / 16;
        const int j = s * (a / s) + (a + ncbps - (16 * a) / ncbps) % s;

        il[j] = coded[k];
    }
    for (k = -26; k <= 26; k++) {
        if (k == 0)
            continue;
        if (k == -21 || k == -7 || k == 7 || k == 21) {
            X[k + 26] = pilots[p++] * wifi_pol[i % 127];
            continue;
        }
        X[k + 26] = wifi_map(il + nbpsc * c, nbpsc) * rot;
        c++;
    }
    wifi_ifft(X, t);
    for (k = 0; k < 16; k++)
        out[k] = t[48 + k];
    for (k = 0; k < 64; k++)
        out[16 + k] = t[k];
}

/* L-STF, L-LTF: 320 samples. */
static void wifi_preamble(float complex *out)
{
    static const int stf_k[12] = { -24, -20, -16, -12, -8, -4, 4, 8, 12, 16, 20, 24 };
    static const int stf_s[12] = { 1, -1, 1, -1, -1, 1, -1, -1, 1, 1, 1, 1 };   /* (1 + j) times. */
    float complex X[53] = { 0 }, t[64];
    int k;

    for (k = 0; k < 12; k++)
        X[stf_k[k] + 26] = (float)(sqrt(13.0 / 6.0) * stf_s[k]) * (1.0f + I);
    wifi_ifft(X, t);
    for (k = 0; k < 160; k++)
        out[k] = t[k % 64];
    for (k = 0; k < 53; k++)
        X[k] = wifi_ltf[k];
    wifi_ifft(X, t);
    for (k = 0; k < 32; k++)
        out[160 + k] = t[32 + k];
    for (k = 0; k < 64; k++)
        out[192 + k] = out[256 + k] = t[k];
}

static void wifi_signal(int rbits, int length, float complex *out)
{
    uint8_t b[24] = { 0 }, coded[48];
    int k, par = 0;

    for (k = 0; k < 4; k++)
        b[k] = (uint8_t)((rbits >> (3 - k)) & 1);
    for (k = 0; k < 12; k++)
        b[5 + k] = (uint8_t)((length >> k) & 1);
    for (k = 0; k < 17; k++)
        par ^= b[k];
    b[17] = (uint8_t)par;
    wifi_encode(b, 24, 0, coded);
    wifi_symbol(coded, 48, 1, 0, 1.0f, out);
}

/* A legacy packet: returns its samples. */
static size_t wifi_packet(const struct wifi_rate_tx *rt, const uint8_t *psdu, int len, unsigned seed, float complex *out)
{
    const int nsym = (16 + 8 * len + 6 + rt->ndbps - 1) / rt->ndbps, nbits = nsym * rt->ndbps;
    uint8_t *bits = calloc((size_t)nbits, 1), *coded = malloc((size_t)nsym * rt->ncbps);
    unsigned st = seed & 0x7fu;
    int k, s;

    wifi_preamble(out);
    wifi_signal(rt->rbits, len, out + 320);
    for (k = 0; k < 8 * len; k++)
        bits[16 + k] = (uint8_t)((psdu[k >> 3] >> (k & 7)) & 1);
    for (k = 0; k < nbits; k++) {
        const unsigned t = ((st >> 6) ^ (st >> 3)) & 1u;

        st = ((st << 1) | t) & 0x7fu;
        bits[k] ^= (uint8_t)t;
    }
    /* The tail is sent as zeros, not scrambled. */
    for (k = 0; k < 6; k++)
        bits[16 + 8 * len + k] = 0;
    wifi_encode(bits, nbits, rt->punct, coded);
    for (s = 0; s < nsym; s++)
        wifi_symbol(coded + s * rt->ncbps, rt->ncbps, rt->nbpsc, 1 + s, 1.0f, out + 400 + 80 * s);
    free(bits);
    free(coded);
    return (size_t)(400 + 80 * nsym);
}

/* CRC-8 of the HT-SIG: x^8 + x^2 + x + 1 from all ones, sent inverted, c7 first. */
static unsigned wifi_crc8(const uint8_t *b, int n)
{
    unsigned c = 0xff;
    int i;

    for (i = 0; i < n; i++) {
        const unsigned fb = ((c >> 7) & 1u) ^ b[i];

        c = (c << 1) & 0xffu;
        if (fb)
            c ^= 0x07u;
    }
    return ~c & 0xffu;
}

/* HT mixed format: L-SIG of 6 Mbps covering `nsym` symbols, HT-SIG (QBPSK), then QPSK symbols
 * for the rest (HT-STF, HT-LTF and data are not built: the analysis reads up to the HT-SIG).
 * vht: a BPSK then a QBPSK symbol instead (VHT-SIG-A), not coded. */
static size_t wifi_ht_packet(int mcs, int ht_len, bool sgi, int nsym, bool vht, float complex *out)
{
    uint8_t b[48] = { 0 }, coded[96], fill[96];
    int k, s, crc;

    wifi_preamble(out);
    wifi_signal(0xd, (nsym * 24 - 22) / 8, out + 320);
    for (k = 0; k < 7; k++)
        b[k] = (uint8_t)((mcs >> k) & 1);
    for (k = 0; k < 16; k++)
        b[8 + k] = (uint8_t)((ht_len >> k) & 1);
    b[24] = 1;          /* Smoothing. */
    b[25] = 1;          /* Not sounding. */
    b[26] = 1;          /* Reserved. */
    b[31] = sgi;
    crc = (int)wifi_crc8(b, 34);
    for (k = 0; k < 8; k++)
        b[34 + k] = (uint8_t)((crc >> (7 - k)) & 1);
    wifi_encode(b, 48, 0, coded);
    for (s = 0; s < 2; s++)
        wifi_symbol(coded + 48 * s, 48, 1, 1 + s, (vht && s == 0) ? 1.0f : I, out + 400 + 80 * s);
    for (s = 2; s < nsym; s++) {
        for (k = 0; k < 96; k++)
            fill[k] = rng_uniform() < 0.5;
        wifi_symbol(fill, 96, 2, 1 + s, 1.0f, out + 400 + 80 * s);
    }
    return (size_t)(400 + 80 * nsym);
}

/* MAC frames, FCS appended: returns the PSDU length. */
static int wifi_beacon(uint8_t *f, const uint8_t bssid[6], const char *ssid)
{
    static const uint8_t rates_ie[10] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    const int l = (int)strlen(ssid);
    uint32_t fcs;
    int n = 0, k;

    f[n++] = 0x80;
    f[n++] = 0x00;
    f[n++] = 0;
    f[n++] = 0;
    for (k = 0; k < 6; k++)
        f[n++] = 0xff;
    memcpy(f + n, bssid, 6);
    n += 6;
    memcpy(f + n, bssid, 6);
    n += 6;
    f[n++] = 0x10;
    f[n++] = 0x00;
    for (k = 0; k < 8; k++)
        f[n++] = (uint8_t)(k * 17);     /* Timestamp. */
    f[n++] = 0x64;                      /* Interval: 100 TU. */
    f[n++] = 0x00;
    f[n++] = 0x01;                      /* Capability. */
    f[n++] = 0x04;
    f[n++] = 0;
    f[n++] = (uint8_t)l;
    memcpy(f + n, ssid, (size_t)l);
    n += l;
    memcpy(f + n, rates_ie, sizeof(rates_ie));
    n += (int)sizeof(rates_ie);
    fcs = m2sdr_wifi_crc32(f, (size_t)n);
    for (k = 0; k < 4; k++)
        f[n++] = (uint8_t)(fcs >> (8 * k));
    return n;
}

static int wifi_data_frame(uint8_t *f, const uint8_t a1[6], const uint8_t a2[6], int body)
{
    uint32_t fcs;
    int n = 0, k;

    f[n++] = 0x08;      /* Data. */
    f[n++] = 0x01;      /* To DS. */
    f[n++] = 0x2c;
    f[n++] = 0x00;
    memcpy(f + n, a1, 6);
    n += 6;
    memcpy(f + n, a2, 6);
    n += 6;
    memcpy(f + n, a1, 6);
    n += 6;
    f[n++] = 0x20;
    f[n++] = 0x00;
    for (k = 0; k < body; k++)
        f[n++] = (uint8_t)(rng_uniform() * 256.0);
    fcs = m2sdr_wifi_crc32(f, (size_t)n);
    for (k = 0; k < 4; k++)
        f[n++] = (uint8_t)(fcs >> (8 * k));
    return n;
}

static int wifi_ack(uint8_t *f, const uint8_t ra[6])
{
    uint32_t fcs;
    int n = 0, k;

    f[n++] = 0xd4;
    f[n++] = 0x00;
    f[n++] = 0;
    f[n++] = 0;
    memcpy(f + n, ra, 6);
    n += 6;
    fcs = m2sdr_wifi_crc32(f, (size_t)n);
    for (k = 0; k < 4; k++)
        f[n++] = (uint8_t)(fcs >> (8 * k));
    return n;
}

/* Packet through a channel of taps h (energy 1), at amplitude amp, carrier offset freq_hz,
 * added to x from sample `at`. */
static void wifi_add(float complex *x, size_t count, const float complex *pk, size_t len, size_t at,
                     const float complex *h, int ntaps, double amp, double freq_hz)
{
    const double ph = 2.0 * M_PI * rng_uniform();
    size_t n;
    int m;

    for (n = 0; n < len + (size_t)ntaps - 1 && at + n < count; n++) {
        float complex v = 0;

        for (m = 0; m < ntaps; m++)
            if (n >= (size_t)m && n - (size_t)m < len)
                v += h[m] * pk[n - (size_t)m];
        x[at + n] += (float complex)(amp * cexp(I * (2.0 * M_PI * freq_hz * (double)(at + n) / WIFI_FS + ph))) * v;
    }
}

static double median3(double a, double b, double c)
{
    return a > b ? (b > c ? b : (a > c ? c : a)) : (a > c ? a : (b > c ? c : b));
}

/* Mean over the 52 carriers of 1 / |H|^2, of the taps h: the EVM after equalisation grows by it. */
static double wifi_inv_channel(const float complex *h, int ntaps)
{
    double s = 0;
    int k, m;

    for (k = -26; k <= 26; k++) {
        double complex v = 0;

        if (k == 0)
            continue;
        for (m = 0; m < ntaps; m++)
            v += h[m] * cexp(-I * 2.0 * M_PI * k * m / 64.0);
        s += 1.0 / (creal(v) * creal(v) + cimag(v) * cimag(v));
    }
    return s / 52.0;
}

static const uint8_t wifi_bssid[6] = { 0x02, 0x4d, 0x32, 0x53, 0x44, 0x52 };
static const uint8_t wifi_sta[6]   = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x07 };

struct wifi_case {
    int    rate;            /* Index in wifi_rates. */
    int    kind;            /* 0 beacon, 1 data (300 bytes), 2 ACK. */
    double snr_db;          /* Over the 20 MHz. */
    double freq_hz;
    int    taps;            /* 1: flat; 3: echoes. */
};

static void test_wifi_case(float complex *x, float complex *pk, const struct wifi_case *wc, struct m2sdr_wifi_result *res)
{
    static const float complex echo[6] = { 0.9f, 0.0f, 0.0f, 0.35f * I, 0.0f, -0.2f + 0.1f * I };
    float complex h[6];
    const size_t count = 40000, at = 1234;
    const struct wifi_rate_tx *rt = &wifi_rates[wc->rate];
    const double amp = 0.1, noise = amp * amp / pow(10.0, wc->snr_db / 10.0);
    const double snr_c = wc->snr_db + 10.0 * log10(64.0 / 52.0);
    uint8_t f[512];
    double e = 0, t0;
    size_t len;
    int ntaps = wc->taps == 1 ? 1 : 6, n, m;

    for (m = 0; m < ntaps; m++)
        e += crealf(echo[m] * conjf(echo[m]));
    for (m = 0; m < ntaps; m++)
        h[m] = ntaps == 1 ? 1.0f : echo[m] / (float)sqrt(e);
    n   = wc->kind == 0 ? wifi_beacon(f, wifi_bssid, "M2SDR-Test") : wc->kind == 1 ? wifi_data_frame(f, wifi_bssid, wifi_sta, 300) : wifi_ack(f, wifi_sta);
    len = wifi_packet(rt, f, n, 0x5d, pk);
    memset(x, 0, count * sizeof(*x));
    wifi_add(x, count, pk, len, at, h, ntaps, amp, wc->freq_hz);
    add_noise(x, count, noise);
    printf(" %d Mbps %s, SNR %.0f dB, offset %.1f kHz%s\n", rt->mbps, wc->kind == 0 ? "beacon" : wc->kind == 1 ? "data" : "ACK",
           wc->snr_db, wc->freq_hz / 1e3, ntaps > 1 ? ", echoes" : "");
    t0 = now_ms();
    CHECK("analyse", m2sdr_wifi_analyse(x, count, WIFI_FS, NULL, res) == 0);
    printf("  time of one analysis: %.2f ms for %.1f ms of samples\n", now_ms() - t0, count / WIFI_FS * 1e3);
    CHECK("one packet, L-SIG right", res->packets == 1 && res->npackets == 1 && res->signal_ok == 1);
    if (res->npackets != 1)
        return;
    {
        const struct m2sdr_wifi_packet *p = &res->packet[0];
        /* The fine offset over the 64-sample period of the L-LTF: an rms of
         * fs / (2 pi 64 sqrt(64 snr)) at the per-carrier SNR; five times that. */
        const double f_rms = WIFI_FS / (2.0 * M_PI * 64.0 * sqrt(64.0 * pow(10.0, snr_c / 10.0)));
        /* EVM after equalisation by an L-LTF of two symbols, the channel estimate projected on
         * 12 taps (the echoes fit): noise of (1 + 0.5 * 12 / 52) / (snr |H|^2), and the common
         * phase of four pilots of that noise: 1 / 8 more. */
        const double evm = 10.0 * log10((1.0 + 0.5 * 12.0 / 52.0) * 1.125 * wifi_inv_channel(h, ntaps)) - snr_c;

        double v[3][3];     /* SNR, RSSI, EVM of three packets. */
        int r, q;

        CHECK_NEAR("start (us)", p->time_s * 1e6, at / WIFI_FS * 1e6, 0.05);
        CHECK_NEAR("carrier offset (Hz)", p->freq_error_hz, wc->freq_hz, 5.0 * f_rms);
        CHECK("rate, length", p->rate_mbps == rt->mbps && p->length == n && p->format == M2SDR_WIFI_LEGACY);
        CHECK_NEAR("duration (us)", p->duration_s * 1e6, (len) / WIFI_FS * 1e6, 1e-6);
        /* The figures of one packet hang on draws of noise that stay over the packet (the noise
         * of the L-LTF, the channel estimate at the faded carriers, which weigh most in the
         * EVM): the medians of three packets of new noise are checked. */
        v[0][0] = p->snr_db;
        v[1][0] = p->rssi_dbfs;
        v[2][0] = p->evm_db;
        for (r = 1; r < 3; r++) {
            memset(x, 0, count * sizeof(*x));
            wifi_add(x, count, pk, len, at, h, ntaps, amp, wc->freq_hz);
            add_noise(x, count, noise);
            if (m2sdr_wifi_analyse(x, count, WIFI_FS, NULL, res) != 0 || res->npackets != 1)
                res->packet[0].snr_db = res->packet[0].rssi_dbfs = res->packet[0].evm_db = 0.0;
            v[0][r] = res->packet[0].snr_db;
            v[1][r] = res->packet[0].rssi_dbfs;
            v[2][r] = res->packet[0].evm_db;
        }
        for (q = 0; q < 3; q++)
            v[q][0] = median3(v[q][0], v[q][1], v[q][2]);
        /* The noise of 52 carrier differences: 14 % (0.6 dB) rms for one packet, 0.45 dB for
         * the median of three; three times that. The channel mean |H|^2 is 1. */
        CHECK_NEAR("SNR per carrier, median (dB)", v[0][0], snr_c, 1.4);
        /* 160 samples of the L-LTF, signal and noise: at 7 dB, 0.2 dB rms for one packet,
         * 0.15 for the median; three times that. */
        CHECK_NEAR("RSSI, median (dBFS)", v[1][0], 10.0 * log10(amp * amp + noise), 0.45);
        /* The slope fit adds a little: 1 dB. */
        CHECK_NEAR("EVM, median (dB)", v[2][0], evm, 1.0);
        CHECK("decoded, FCS right", p->decoded && p->fcs_ok);
        if (wc->kind == 0) {
            CHECK("beacon of the BSS, its SSID", p->type == 0 && p->subtype == 8 && p->naddr == 3 &&
                  memcmp(p->addr3, wifi_bssid, 6) == 0 && strcmp(p->ssid, "M2SDR-Test") == 0);
            CHECK("BSS list", res->nbss == 1 && res->bss[0].beacons == 1 && strcmp(res->bss[0].ssid, "M2SDR-Test") == 0);
        } else if (wc->kind == 1) {
            CHECK("data frame, addresses", p->type == 2 && p->naddr == 3 && memcmp(p->addr1, wifi_bssid, 6) == 0 &&
                  memcmp(p->addr2, wifi_sta, 6) == 0);
        } else {
            CHECK("ACK, its address", p->type == 1 && p->subtype == 13 && p->naddr == 1 && memcmp(p->addr1, wifi_sta, 6) == 0);
        }
        CHECK("trace of the packet", res->trace_packet == 0 && res->npoints > 0 &&
              res->points_modulation == (rt->nbpsc == 1 ? M2SDR_MOD_BPSK : rt->nbpsc == 2 ? M2SDR_MOD_QPSK :
                                         rt->nbpsc == 4 ? M2SDR_MOD_16QAM : M2SDR_MOD_64QAM));
        if (ntaps > 1) {
            /* The channel trace follows the taps: within 0.5 dB on average over the carriers out
             * of the fades (10 dB under the mean), where the noise of the estimate stays small. */
            double dev = 0;
            int k, nk = 0;

            for (k = 0; k < 52; k++) {
                const int kk = k < 26 ? k - 26 : k - 25;
                double complex v = 0;
                double g;

                for (m = 0; m < ntaps; m++)
                    v += h[m] * cexp(-I * 2.0 * M_PI * kk * m / 64.0);
                g = 10.0 * log10(creal(v) * creal(v) + cimag(v) * cimag(v));
                if (g > -10.0) {
                    dev += fabs(res->channel_db[k] - g);
                    nk++;
                }
            }
            CHECK_NEAR("channel trace, mean error (dB)", dev / nk, 0.0, 0.5);
        }
    }
}

static void test_wifi(void)
{
    static const struct wifi_case cases[] = {
        /* rate kind  snr   freq_hz taps */
        { 0, 0, 20.0,  57300.0, 3 },
        { 0, 2, 10.0, -12000.0, 1 },
        { 0, 0,  7.0,  30000.0, 1 },   /* 8 dB per carrier: BPSK 1/2 still decodes. */
        { 1, 1, 15.0, 100000.0, 1 },
        { 2, 1, 15.0, -80000.0, 3 },
        { 3, 1, 18.0,   5000.0, 1 },
        { 4, 1, 22.0, -30000.0, 3 },
        { 5, 1, 25.0,  30000.0, 1 },
        { 6, 1, 28.0, -150000.0, 1 },
        { 7, 1, 30.0, 200000.0, 3 },
    };
    const size_t count = 40000, max_pk = 40000;
    float complex *x = malloc(count * sizeof(*x)), *pk = malloc(max_pk * sizeof(*pk));
    struct m2sdr_wifi_result *res = malloc(sizeof(*res));
    struct m2sdr_wifi_config cfg;
    const float complex flat = 1.0f;
    uint8_t f[512];
    size_t i, len, at, total;
    double t0, airtime;
    int n;

    printf("Wi-Fi:\n");
    if (!x || !pk || !res) {
        CHECK("allocation", 0);
        free(x);
        free(pk);
        free(res);
        return;
    }
    wifi_init();
    CHECK("CRC-32 of \"123456789\"", m2sdr_wifi_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        test_wifi_case(x, pk, &cases[i], res);

    /* A sampling clock 40 ppm fast over a 1500-byte packet at 12 Mbps (1 ms): 0.8 sample of
     * drift, a phase slope over the carriers the pilots follow. Resampled by a windowed sinc. */
    printf(" sampling offset of 40 ppm, 1500 bytes at 12 Mbps\n");
    {
        uint8_t big[1600];
        float complex *q = malloc(max_pk * sizeof(*q));
        const double eps = 40e-6;

        n   = wifi_data_frame(big, wifi_bssid, wifi_sta, 1500);
        len = wifi_packet(&wifi_rates[2], big, n, 0x47, pk);
        if (q) {
            for (i = 0; i < len; i++) {
                const double t = (double)i * (1.0 + eps);
                const long c = (long)floor(t);
                float complex v = 0;
                long m;

                for (m = c - 15; m <= c + 16; m++) {
                    const double d = t - (double)m;
                    const double sn = fabs(d) < 1e-12 ? 1.0 : sin(M_PI * d) / (M_PI * d);
                    const double wn = 0.42 + 0.5 * cos(M_PI * d / 16.0) + 0.08 * cos(2.0 * M_PI * d / 16.0);

                    if (m >= 0 && (size_t)m < len && fabs(d) < 16.0)
                        v += pk[m] * (float)(sn * wn);
                }
                q[i] = v;
            }
            memset(x, 0, count * sizeof(*x));
            wifi_add(x, count, q, len, 400, &flat, 1, 0.1, 20000.0);
            add_noise(x, count, 0.01 / pow(10.0, 25.0 / 10.0));
            CHECK("analyse", m2sdr_wifi_analyse(x, count, WIFI_FS, NULL, res) == 0);
            CHECK("FCS right", res->packets == 1 && res->fcs_ok == 1);
            /* The fit takes the slope; the EVM stays near that of the noise (-26.6 dB) */
            CHECK("EVM under -22 dB", res->packets == 1 && res->packet[0].evm_db < -22.0);
            if (res->packets == 1)
                printf("  EVM %.1f dB\n", res->packet[0].evm_db);
        }
        free(q);
    }

    /* HT mixed format: the HT-SIG read; VHT: told apart. */
    printf(" HT-SIG, MCS 7, 1500 bytes, short GI\n");
    memset(x, 0, count * sizeof(*x));
    len = wifi_ht_packet(7, 1500, true, 60, false, pk);
    wifi_add(x, count, pk, len, 500, &flat, 1, 0.1, 25000.0);
    add_noise(x, count, 0.01 / pow(10.0, 20.0 / 10.0));
    CHECK("analyse", m2sdr_wifi_analyse(x, count, WIFI_FS, NULL, res) == 0);
    CHECK("HT, its HT-SIG", res->packets == 1 && res->packet[0].format == M2SDR_WIFI_HT && res->packet[0].ht_mcs == 7 &&
          res->packet[0].ht_length == 1500 && res->packet[0].ht_short_gi && !res->packet[0].ht_40mhz);
    CHECK_NEAR("duration from the L-SIG (us)", res->packet[0].duration_s * 1e6, len / WIFI_FS * 1e6, 1e-6);
    printf(" VHT-SIG-A\n");
    memset(x, 0, count * sizeof(*x));
    len = wifi_ht_packet(0, 100, false, 30, true, pk);
    wifi_add(x, count, pk, len, 700, &flat, 1, 0.1, -25000.0);
    add_noise(x, count, 0.01 / pow(10.0, 20.0 / 10.0));
    CHECK("VHT", m2sdr_wifi_analyse(x, count, WIFI_FS, NULL, res) == 0 && res->packets == 1 &&
                 res->packet[0].format == M2SDR_WIFI_VHT);

    /* A busy channel: beacon, data, ACK, data, 2 ms; the airtime from the L-SIGs. */
    printf(" four packets, airtime\n");
    memset(x, 0, count * sizeof(*x));
    at = 300;
    airtime = 0;
    total = 0;
    for (i = 0; i < 4; i++) {
        const int r = i == 0 ? 0 : i == 2 ? 2 : 5;

        n = i == 0 ? wifi_beacon(f, wifi_bssid, "M2SDR-Test") : i == 2 ? wifi_ack(f, wifi_sta) : wifi_data_frame(f, wifi_bssid, wifi_sta, 200);
        len = wifi_packet(&wifi_rates[r], f, n, 0x11 + (unsigned)i, pk);
        wifi_add(x, count, pk, len, at, &flat, 1, 0.1, 10000.0);
        airtime += len / WIFI_FS;
        total   += len;
        at += len + 200 + 100 * i;
    }
    add_noise(x, count, 0.01 / pow(10.0, 25.0 / 10.0));
    t0 = now_ms();
    CHECK("analyse", m2sdr_wifi_analyse(x, count, WIFI_FS, NULL, res) == 0);
    printf("  time of one analysis: %.2f ms for 2 ms of samples\n", now_ms() - t0);
    CHECK("four packets, FCS right", res->packets == 4 && res->signal_ok == 4 && res->fcs_ok == 4);
    CHECK_NEAR("airtime", res->busy_fraction, airtime / (count / WIFI_FS), 1e-9);
    /* Energy: the 4 us slots with a packet, two more at most at the ends of each. */
    CHECK_NEAR("energy airtime", res->energy_busy_fraction, (double)total / count, 8.0 * 80.0 / count);
    CHECK_NEAR("noise floor (dBFS)", res->noise_dbfs, 10.0 * log10(0.01) - 25.0, 0.5);
    memset(&cfg, 0, sizeof(cfg));
    cfg.no_psdu = true;
    t0 = now_ms();
    CHECK("without the data", m2sdr_wifi_analyse(x, count, WIFI_FS, &cfg, res) == 0 && res->packets == 4 &&
                              res->signal_ok == 4 && res->fcs_ok == 0);
    printf("  time of one analysis: %.2f ms for 2 ms of samples\n", now_ms() - t0);

    /* Noise, a tone (an L-STF autocorrelation, no L-LTF), bad arguments. */
    memset(x, 0, count * sizeof(*x));
    add_noise(x, count, 0.01);
    CHECK("noise: no packet", m2sdr_wifi_analyse(x, count, WIFI_FS, NULL, res) == 0 && res->packets == 0);
    for (i = 0; i < count; i++)
        x[i] += 0.1f * (float complex)cexp(I * 2.0 * M_PI * 1.25e6 * (double)i / WIFI_FS);
    CHECK("tone: no packet", m2sdr_wifi_analyse(x, count, WIFI_FS, NULL, res) == 0 && res->packets == 0);
    CHECK("bad sample rate", m2sdr_wifi_analyse(x, count, 30.72e6, NULL, res) != 0);
    CHECK("too short", m2sdr_wifi_analyse(x, 1000, WIFI_FS, NULL, res) != 0);
    free(x);
    free(pk);
    free(res);

    {
        /* 50 ms with 100 data packets at 54 Mbps (1500 bytes): a busy channel. */
        const size_t nn = (size_t)(0.05 * WIFI_FS);
        float complex *y = calloc(nn, sizeof(*y)), *q = malloc(max_pk * sizeof(*q));
        struct m2sdr_wifi_result *r = malloc(sizeof(*r));
        uint8_t big[1600];

        if (y && q && r) {
            at = 100;
            for (i = 0; i < 100 && at < nn; i++) {
                n   = wifi_data_frame(big, wifi_bssid, wifi_sta, 1500);
                len = wifi_packet(&wifi_rates[7], big, n, 0x33, q);
                wifi_add(y, nn, q, len, at, &flat, 1, 0.1, 10000.0);
                at += len + 2000;
            }
            add_noise(y, nn, 0.01 / pow(10.0, 30.0 / 10.0));
            t0 = now_ms();
            CHECK("busy channel: analyse", m2sdr_wifi_analyse(y, nn, WIFI_FS, NULL, r) == 0);
            printf("  time of one analysis: %.1f ms for 50 ms of samples, %d packets\n", now_ms() - t0, r->packets);
            CHECK("busy channel: FCS right", r->packets == (int)i && r->fcs_ok == (int)i);
        }
        free(y);
        free(q);
        free(r);
    }
}

int main(void)
{
    test_adsb_decode();
    test_adsb();
    test_lora();
    test_wifi();
    if (failures) {
        printf("test_m2sdr_analysis_wireless: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis_wireless: all tests passed\n");
    return 0;
}
