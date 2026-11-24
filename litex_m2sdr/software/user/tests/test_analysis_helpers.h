/* SPDX-License-Identifier: BSD-2-Clause
 *
 * What the tests of the libm2sdr signal analysis (test_m2sdr_analysis*.c) share: the checks and
 * their failure count, the random generator, the signals more than one test builds, the capture
 * files given to m2sdr_analyzer and the reading of its reports.
 *
 * Each test defines TEST_NAME, its name in the messages, before including this file.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef TEST_ANALYSIS_HELPERS_H
#define TEST_ANALYSIS_HELPERS_H

#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../libm2sdr/m2sdr_fft.h"

#ifndef TEST_NAME
#error "TEST_NAME must be defined before test_analysis_helpers.h is included"
#endif

/* Checks */
/*--------*/

static int failures;

/* Format of the value, the expected one and the tolerance of CHECK_NEAR: a test whose figures
 * do not fit a fixed point defines its own before including this file. */
#ifndef CHECK_NEAR_FMT
#define CHECK_NEAR_FMT "%12.4f (expected %10.4f +-%g)"
#endif

#define CHECK_NEAR(name, value, expected, tol) do { \
    const double v_ = (value), e_ = (expected); \
    const int ok_ = fabs(v_ - e_) <= (tol); \
    printf("  %-34s " CHECK_NEAR_FMT " %s\n", name, v_, e_, (double)(tol), ok_ ? "ok" : "FAIL"); \
    if (!ok_) failures++; \
} while (0)

#define CHECK(name, cond) do { \
    const int ok_ = (cond); \
    printf("  %-34s %s\n", name, ok_ ? "ok" : "FAIL"); \
    if (!ok_) failures++; \
} while (0)

/* End of the tests: the exit code of main(). */
static inline int tests_done(void)
{
    if (failures) {
        printf(TEST_NAME ": %d FAILED\n", failures);
        return 1;
    }
    printf(TEST_NAME ": all tests passed\n");
    return 0;
}

/* End of the checks of a report (`what`: its name, or NULL): the exit code of main(). */
static inline int report_done(const char *what)
{
    if (failures) {
        printf(TEST_NAME ": %d FAILED\n", failures);
        return 1;
    }
    printf(TEST_NAME ": %s%sreport checked\n", what ? what : "", what ? " " : "");
    return 0;
}

/* Time */
/*------*/

static inline double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static inline double now_ms(void)
{
    return 1e3 * now_s();
}

/* Signals */
/*---------*/

/* One generator and one seed for every test: the figures they print do not change from run to
 * run. */
static unsigned long long rng_state = 0x2545f4914f6cdd1dull;

static inline double rng_uniform(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return ((double)(rng_state >> 11) + 0.5) / 9007199254740992.0;
}

static inline double rng_gauss(void)
{
    return sqrt(-2.0 * log(rng_uniform())) * cos(2.0 * M_PI * rng_uniform());
}

/* Complex Gaussian noise of power `power`. */
static inline void add_noise(float complex *x, size_t count, double power)
{
    size_t i;

    for (i = 0; i < count; i++) {
        const double r = sqrt(-power * log(rng_uniform()));
        const double a = 2.0 * M_PI * rng_uniform();
        x[i] += (float)(r * cos(a)) + I * (float)(r * sin(a));
    }
}

/* Tone of amplitude `amp` at freq_hz, sample rate fs. */
static inline void add_tone(float complex *x, size_t count, double freq_hz, double fs, double amp, double phase)
{
    size_t i;

    for (i = 0; i < count; i++)
        x[i] += (float complex)(amp * cexp(I * (2.0 * M_PI * freq_hz * (double)i / fs + phase)));
}

/* Root raised cosine of roll-off alpha, t in symbols. */
static inline double test_rrc(double t, double alpha)
{
    if (fabs(t) < 1e-9)
        return 1.0 - alpha + 4.0 * alpha / M_PI;
    if (fabs(fabs(t) - 1.0 / (4.0 * alpha)) < 1e-9)
        return alpha / sqrt(2.0) * ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * alpha)) +
                                    (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * alpha)));
    return (sin(M_PI * t * (1.0 - alpha)) + 4.0 * alpha * t * cos(M_PI * t * (1.0 + alpha))) /
           (M_PI * t * (1.0 - 16.0 * alpha * alpha * t * t));
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

static inline size_t make_dvbt(float complex *x, size_t max, double fs, const struct dvbt_signal *sig)
{
    static const int dvbt_cp[] = {
           0,   48,   54,   87,  141,  156,  192,  201,  255,  279,  282,  333,  432,  450,  483,
         525,  531,  618,  636,  714,  759,  765,  780,  804,  873,  888,  918,  939,  942,  969,
         984, 1050, 1101, 1107, 1110, 1137, 1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704,
    };
    static const int dvbt_tps[] = {
          34,   50,  209,  346,  413,  569,  595,  688,  790,  901, 1073, 1219, 1262, 1286, 1469,
        1594, 1687,
    };
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

/* Capture files and reports of m2sdr_analyzer */
/*---------------------------------------------*/

/* The Makefile runs m2sdr_analyzer --report --json on a file written by a test, and gives its
 * output back to be checked: this covers the file reader, the conversion and the JSON. */

/* SC16 with 12 significant bits, full scale 1.0 (not clipped: a test may go over it): x1 alone
 * (x2 NULL), or two channels interleaved, RX1 first, as the board streams them (x2 = x1: the
 * same on both). */
static inline int write_sc16(const char *path, const float complex *x1, const float complex *x2, size_t count)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    int c;

    if (!f)
        return 1;
    for (i = 0; i < count; i++) {
        short v[4];

        for (c = 0; c < (x2 ? 2 : 1); c++) {
            const float complex s = c ? x2[i] : x1[i];

            v[2 * c + 0] = (short)lrintf(crealf(s) * 2047.0f);
            v[2 * c + 1] = (short)lrintf(cimagf(s) * 2047.0f);
        }
        if (fwrite(v, (x2 ? 4 : 2) * sizeof(v[0]), 1, f) != 1) {
            fclose(f);
            return 1;
        }
    }
    return fclose(f) == 0 ? 0 : 1;
}

/* A report as text, NULL if it cannot be read. One at a time: the text is static. */
static inline char *read_report(const char *path)
{
    static char text[1 << 20];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        printf(TEST_NAME ": cannot open %s\n", path);
        return NULL;
    }
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return text;
}

/* Number of "name": after the first `after` (a key or any text) of a JSON text. */
static inline double json_number_after(const char *json, const char *after, const char *name)
{
    char key[64];
    const char *p = strstr(json, after);

    if (!p)
        return NAN;
    snprintf(key, sizeof(key), "\"%s\":", name);
    p = strstr(p + strlen(after), key);
    return p ? strtod(p + strlen(key), NULL) : NAN;
}

/* Number of "name": after "section", an object or an array of a JSON text (and after its key:
 * a figure may have the name of its section). */
static inline double json_number(const char *json, const char *section, const char *name)
{
    char key[64];

    snprintf(key, sizeof(key), "\"%s\"", section);
    return json_number_after(json, key, name);
}

#endif /* TEST_ANALYSIS_HELPERS_H */
