/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the libm2sdr DVB-T2 analysis on synthetic multiplexes of known parameters.
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

#include "../libm2sdr/m2sdr_analysis_dvbt2.h"
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

/* DVB-T2 */
/*--------*/

static const int t2_sizes[6]  = { 1024, 2048, 4096, 8192, 16384, 32768 };
static const int t2_normal[6] = { 853, 1705, 3409, 6817, 13633, 27265 };
static const int t2_ext[6]    = { 0, 0, 0, 6913, 13921, 27841 };
static const int t2_p2[6]     = { 16, 8, 4, 2, 1, 1 };
static const int t2_guard[7][2] = { { 1, 128 }, { 1, 32 }, { 1, 16 }, { 19, 256 }, { 1, 8 }, { 19, 128 }, { 1, 4 } };
static const int t2_pp[8][2]  = { { 3, 4 }, { 6, 2 }, { 6, 4 }, { 12, 2 }, { 12, 4 }, { 24, 2 }, { 24, 4 }, { 6, 16 } };
static const float t2_pp_a[8] = { 4.0f / 3, 4.0f / 3, 7.0f / 4, 7.0f / 4, 7.0f / 3, 7.0f / 3, 7.0f / 3, 7.0f / 3 };
static const double t2_angle[4] = { 29.0, 16.8, 8.6, 3.576334 };

/* T2 frames built as EN 302 755 sets them, SISO: a P1 (C-A-B, the shift of one 1k carrier),
 * the P2 symbols, then data symbols with the scattered pilots of the pattern and the edge
 * pilots, random cells in place of the coded PLP. What is not the standard's, the analysis not
 * relying on it: the 384 active carriers of the P1 and their values (random), the continual
 * pilots (1 % of the carriers at random, of the scattered pilots' amplitude), the sequence p[l]
 * of the pilots per symbol (random), the cells of the P2 (BPSK, pilots every third carrier). A
 * rotated constellation takes the Q of each cell from the cell before (cyclic Q delay). A frame
 * closing symbol, when asked for, has pilots on every Dx-th carrier. */
struct dvbt2_signal {
    int    n;                   /* FFT length. */
    bool   ext;                 /* Extended carriers. */
    int    g;                   /* Guard interval: index in t2_guard. */
    int    pp;                  /* Pattern, 1 to 8. */
    int    side;                /* Of the constellation: 2 (QPSK) to 16 (256QAM). */
    bool   rotated;
    int    frame_syms;          /* Data symbols of a frame. */
    bool   closing;             /* The last of them a frame closing symbol. */
    long   lead;                /* Samples of the capture before the second P1 of the stream (negative: after it). */
    double freq_hz;             /* Carrier offset. */
    double snr_db;
    double echo_us, echo_db;    /* A second path; echo_db -99 for none. */
};

static int t2_index(int n)
{
    int i;

    for (i = 0; i < 5 && t2_sizes[i] != n; i++)
        ;
    return i;
}

static size_t t2_frame_len(const struct dvbt2_signal *sig)
{
    const int i = t2_index(sig->n), l = sig->n + sig->n / t2_guard[sig->g][1] * t2_guard[sig->g][0];

    return 2048 + (size_t)(t2_p2[i] + sig->frame_syms) * (size_t)l;
}

struct t2_stream {
    float complex *x;
    size_t max, pos, start;
};

static void t2_put(struct t2_stream *st, const float complex *v, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++, st->pos++)
        if (st->pos >= st->start && st->pos - st->start < st->max)
            st->x[st->pos - st->start] = v[i];
}

static size_t make_dvbt2(float complex *x, size_t max, double fs, const struct dvbt2_signal *sig)
{
    const int si = t2_index(sig->n), n = sig->n;
    const int k = sig->ext ? t2_ext[si] : t2_normal[si], kext = (k - t2_normal[si]) / 2;
    const int gl = n / t2_guard[sig->g][1] * t2_guard[sig->g][0], l = n + gl, np2 = t2_p2[si];
    const int dx = t2_pp[sig->pp - 1][0], dy = t2_pp[sig->pp - 1][1], dd = dx * dy;
    const float asp = t2_pp_a[sig->pp - 1], ap2 = sqrtf(37.0f) / 5.0f;
    const int m = sig->side;
    const float scale = (float)(1.0 / sqrt(2.0 * (m * m - 1) / 3.0));
    const float complex rot = sig->rotated ?
        (float complex)cexp(I * t2_angle[m == 2 ? 0 : m == 4 ? 1 : m == 8 ? 2 : 3] * M_PI / 180.0) : 1.0f;
    struct m2sdr_fft *ifft = m2sdr_fft_alloc((size_t)n, true), *ifft1 = m2sdr_fft_alloc(1024, true);
    float complex *f = malloc((size_t)n * sizeof(*f)), *t = malloc((size_t)(n + gl) * sizeof(*t));
    float complex p1a[1024], p1f[1024], p1[2048], prev = 0;
    uint8_t *w = malloc((size_t)k), *cp = calloc((size_t)k, 1), active[853];
    const size_t frame = t2_frame_len(sig);
    const size_t echo = (size_t)lround(sig->echo_us * 1e-6 * fs);
    const double echo_amp = pow(10.0, sig->echo_db / 20.0);
    struct t2_stream st = { x, max, 0, (size_t)((long)frame - sig->lead) };
    unsigned reg = 0x7ff;
    size_t i, count = max;
    double power = 0;
    int c, s, na = 0;

    for (c = 0; c < k; c++) {
        w[c] = (uint8_t)((reg >> 10) & 1u);
        reg = ((reg << 1) | (w[c] ^ ((reg >> 8) & 1u))) & 0x7ffu;
        cp[c] = c > 0 && c < k - 1 && ((unsigned)c * 2654435761u >> 16) % 100 == 7;
    }
    memset(active, 0, sizeof(active));
    while (na < 384) {
        const int a = 44 + (int)(rng_uniform() * (809 - 44));

        if (!active[a]) {
            active[a] = 1;
            na++;
        }
    }
    memset(x, 0, max * sizeof(*x));
    while (st.pos < st.start + max) {
        /* P1: A on the active carriers, then C-A-B. */
        memset(p1f, 0, sizeof(p1f));
        for (c = 0; c < 853; c++)
            if (active[c])
                p1f[(c - 426 + 1024) % 1024] = (float)sqrt(k / 384.0) * (rng_uniform() < 0.5 ? -1.0f : 1.0f);
        m2sdr_fft_run(ifft1, p1f, p1a);
        for (i = 0; i < 542; i++)
            p1[i] = p1a[i] * (float complex)cexp(I * 2.0 * M_PI * (double)i / 1024.0);
        for (i = 0; i < 1024; i++)
            p1[542 + i] = p1a[i];
        for (i = 0; i < 482; i++)
            p1[1566 + i] = p1a[542 + i] * (float complex)cexp(I * 2.0 * M_PI * (double)(1566 + i) / 1024.0);
        t2_put(&st, p1, 2048);
        for (s = 0; s < np2 + sig->frame_syms; s++) {
            const int pl = rng_uniform() < 0.5;

            memset(f, 0, (size_t)n * sizeof(*f));
            for (c = 0; c < k; c++) {
                const float sgn = (w[c] ^ pl) ? -1.0f : 1.0f;
                float complex v;

                if (s < np2) {
                    v = (c % 3 == 0 || c == k - 1) ? ap2 * sgn : (rng_uniform() < 0.5 ? -1.0f : 1.0f);
                } else if (c == 0 || c == k - 1 || cp[c] || ((c - kext) % dd + dd) % dd == dx * (s % dy) ||
                           (sig->closing && s == np2 + sig->frame_syms - 1 && ((c - kext) % dx + dx) % dx == 0)) {
                    v = asp * sgn;
                } else {
                    const float complex q = scale * ((2 * (int)(rng_uniform() * m) - m + 1) +
                                                     I * (2 * (int)(rng_uniform() * m) - m + 1)) * rot;

                    v = sig->rotated ? crealf(q) + I * cimagf(prev) : q;
                    prev = q;
                }
                f[(c - (k - 1) / 2 + n) % n] = v;
            }
            m2sdr_fft_run(ifft, f, t + gl);
            memcpy(t, t + n, (size_t)gl * sizeof(*t));
            t2_put(&st, t, (size_t)l);
        }
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
    m2sdr_fft_free(ifft1);
    free(f);
    free(t);
    free(w);
    free(cp);
    return count;
}

/* DVB-T */
/*-------*/

/* A DVB-T multiplex, as test_m2sdr_analysis.c builds it, sets its pilots and TPS, with random data in place of
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

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static enum m2sdr_modulation t2_mod(int side)
{
    return side == 2 ? M2SDR_MOD_QPSK : side == 4 ? M2SDR_MOD_16QAM : side == 8 ? M2SDR_MOD_64QAM : M2SDR_MOD_256QAM;
}

static void test_dvbt2(void)
{
    static const struct dvbt2_signal signals[] = {
        /*  n     ext    g pp side rot   frame closing  lead   freq_hz   snr echo_us echo_db */
        { 32768, true,  0, 7, 16, true,   60, false,  20000,      0.0, 35.0,  0.0, -99.0 },  /* UK, France: 32k ext 1/128 PP7 256QAM rotated. */
        { 32768, false, 2, 4, 16, false,  40, false,  50000, 166667.0, 32.0,  0.0, -99.0 },
        {  8192, false, 4, 2,  8, false, 100, true,  100000,  -2500.0, 25.0, 20.0,  -6.0 },  /* Two P1s, an echo. */
        { 16384, true,  3, 8,  4, true,   50, false,  30000,   3000.0, 33.0,  0.0, -99.0 },
        {  1024, false, 6, 1,  2, false, 200, false,   5000,  12000.0, 15.0,  0.0, -99.0 },
        {  2048, false, 1, 3,  8, false, 120, true,    5000,   -700.0, 28.0,  0.0, -99.0 },
        {  4096, false, 2, 5,  4, false,  80, true,   10000,  12000.0, 22.0,  0.0, -99.0 },
        {  8192, true,  5, 6, 16, false,  60, false,  40000, -166667.0, 34.0,  0.0, -99.0 },
        {  8192, false, 4, 2,  4, false, 200, false, -50000,    500.0, 20.0,  0.0, -99.0 },  /* No P1 in the capture. */
        {  8192, false, 4, 1,  8, false, 200, false, -50000,  -1200.0, 22.0,  0.0, -99.0 },  /* PP1 like DVB-T, no P1: still T2. */
    };
    const double fs = 64e6 / 7.0;
    const size_t max = (size_t)(0.15 * fs);
    float complex *x = malloc(max * sizeof(*x));
    struct m2sdr_dvbt2_result *res = malloc(sizeof(*res));
    size_t i;

    printf("DVB-T2:\n");
    if (!x || !res) {
        CHECK("allocation", 0);
        free(x);
        free(res);
        return;
    }
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        const struct dvbt2_signal *sig = &signals[i];
        const size_t count = make_dvbt2(x, max, fs, sig), frame = t2_frame_len(sig);
        const int dx = t2_pp[sig->pp - 1][0];
        const double a2 = sig->echo_db > -99.0 ? pow(10.0, sig->echo_db / 10.0) : 0.0;
        const double echo_db = 10.0 * log10((1.0 - a2) / (1.0 + a2));
        const float asp = t2_pp_a[sig->pp - 1];
        const int dk = (sig->ext ? t2_ext[t2_index(sig->n)] : t2_normal[t2_index(sig->n)]) - 2;
        double t;
        int np1 = 0;

        while (sig->lead + (long)(np1 * frame) < (long)count - 2048 && np1 < 8)
            np1++;
        if (sig->lead < 0)
            np1 = 0;
        printf(" %dk%s, guard %d/%d, PP%d, %s%s, offset %.0f Hz, SNR %.0f dB%s\n", sig->n / 1024,
               sig->ext ? " ext" : "", t2_guard[sig->g][0], t2_guard[sig->g][1], sig->pp,
               m2sdr_modulation_name(t2_mod(sig->side)), sig->rotated ? " rotated" : "", sig->freq_hz, sig->snr_db,
               sig->echo_db > -99.0 ? ", echo" : "");
        t = now_ms();
        CHECK("analyse", m2sdr_dvbt2_analyse(x, count, fs, NULL, res) == 0);
        printf("  %-34s %12.1f\n", "time (ms)", now_ms() - t);
        CHECK("found, pilots, not DVB-T", res->found && res->pilots && !res->dvbt);
        CHECK_NEAR("FFT length", res->fft_len, sig->n, 0);
        CHECK("carriers", res->extended == sig->ext);
        CHECK("guard interval", res->guard_num == t2_guard[sig->g][0] && res->guard_den == t2_guard[sig->g][1]);
        CHECK_NEAR("pattern", res->pattern, sig->pp, 0);
        CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, sig->freq_hz, 20.0);
        CHECK_NEAR("P1 found", res->p1_count, np1, 0);
        /* The P1 correlation peaks at its first sample; an echo or noise move it by a few. */
        if (np1 && res->p1_count)
            CHECK_NEAR("P1 sample", (double)res->p1_sample[0], (double)sig->lead, 4.0);
        if (np1 >= 2)
            CHECK_NEAR("frame (ms)", res->frame_ms, frame / fs * 1e3, 0.002);
        if (np1) {
            /* Fractional from the P1: modulo a 1k carrier (8.9 kHz), its phase over C-A-B to a few
             * thousandths of a turn. An echo within the C and B windows (542 and 482 samples) adds
             * to them in different shares: a bias of up to a tenth of a radian for one at -6 dB. */
            const double step = fs / 1024.0, r = remainder(res->p1_freq_hz - sig->freq_hz, step);

            CHECK_NEAR("P1 frequency (modulo, Hz)", r, 0.0, sig->echo_db > -99.0 ? 150.0 : 50.0);
        }
        /* Continual pilots: the spread of the received values, not of the equalised ones, so the
         * noise of the channel estimate is not in it: the SNR, less what the echo takes (an echo
         * of amplitude a leaves the equalised carriers (1 - a^2) / (1 + a^2) of the power). */
        CHECK("pilot MER valid", res->pilot_mer_valid && res->continual_pilots >= 4);
        CHECK_NEAR("pilot MER (dB)", res->pilot_mer_db, sig->snr_db + echo_db, 1.0);
        CHECK("constellation", sig->rotated && sig->side == 16 ? res->modulation == M2SDR_MOD_AUTO :
                                                               res->modulation == t2_mod(sig->side));
        CHECK("rotated", res->rotated == sig->rotated);
        CHECK("MER valid", res->mer_valid == !sig->rotated);
        if (!sig->rotated) {
            /* The channel of a data cell comes from two pilots of noise 1 / a^2 of the cells', the
             * linear interpolation weighs them (1 - r)^2 + r^2: 0.7 on average. */
            const double g = 1.0 + 0.7 / (asp * asp);

            CHECK_NEAR("MER (dB)", res->mer_db, sig->snr_db - 10.0 * log10(g) + echo_db, 1.0);
        }
        if (sig->echo_db > -99.0) {
            CHECK_NEAR("echo delay (us)", res->echo_delay_us, sig->echo_us, 0.5);
            CHECK_NEAR("echo level (dB)", res->echo_level_db, sig->echo_db, 1.5);
        }
        /* Continual pilots: 1 % of the carriers, the two edge ones. */
        CHECK_NEAR("continual pilots", res->continual_pilots, 2 + dk / 100, 0.3 * dk / 100 + 2);
        (void)dx;
    }

    /* 40 ms with what an earlier capture found. */
    {
        const struct dvbt2_signal sig = { 8192, false, 4, 2, 8, false, 100, false, 100000, 166667.0, 25.0, 0.0, -99.0 };
        const size_t count = make_dvbt2(x, max, fs, &sig);
        struct m2sdr_dvbt2_config cfg = { 8192, 1, 8, 2, M2SDR_MOD_64QAM, false, false, 0 };
        double t;

        (void)count;
        printf(" 40 ms, mode, pattern and constellation given\n");
        t = now_ms();
        CHECK("analyse", m2sdr_dvbt2_analyse(x, (size_t)(0.04 * fs), fs, &cfg, res) == 0 && res->pilots);
        printf("  %-34s %12.1f\n", "time (ms)", now_ms() - t);
        CHECK("64QAM", res->modulation == M2SDR_MOD_64QAM && res->mer_valid);
        CHECK_NEAR("MER (dB)", res->mer_db, 25.0 - 10.0 * log10(1.0 + 0.7 / (16.0 / 9.0)), 1.0);
        cfg.offset_known   = true;
        cfg.carrier_offset = res->carrier_offset;
        printf(" 40 ms, offset known (%d carriers)\n", cfg.carrier_offset);
        CHECK("analyse", m2sdr_dvbt2_analyse(x, (size_t)(0.04 * fs), fs, &cfg, res) == 0 && res->pilots);
        CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, sig.freq_hz, 20.0);
        cfg.carrier_offset = 0;
        printf(" 40 ms, a wrong known offset\n");
        CHECK("analyse", m2sdr_dvbt2_analyse(x, (size_t)(0.04 * fs), fs, &cfg, res) == 0 && res->pilots);
        CHECK_NEAR("carrier offset (Hz)", res->freq_error_hz, sig.freq_hz, 20.0);
    }

    /* DVB-T: the pilots of PP1, the same PRBS, no P1: not DVB-T2, told by its continual pilots. */
    {
        static const struct dvbt_signal dvbt[] = {
            /*  n     g  con hp start  freq_hz   snr echo_us echo_db */
            { 8192,  8,  2,  2,   0,   338.0,  18.0,  1.6, -24.0 },   /* As the multiplex of the check on air. */
            { 8192,  4,  1,  1,  13, 166667.0, 25.0,  0.0, -99.0 },
            { 2048, 32,  0,  0,  60, -12000.0, 12.0,  0.0, -99.0 },
        };

        for (i = 0; i < sizeof(dvbt) / sizeof(dvbt[0]); i++) {
            const size_t count = make_dvbt(x, max, fs, &dvbt[i]);

            printf(" DVB-T %dk, guard 1/%d, SNR %.0f dB\n", dvbt[i].n / 1024, dvbt[i].g, dvbt[i].snr_db);
            CHECK("analyse", m2sdr_dvbt2_analyse(x, count, fs, NULL, res) == 0);
            CHECK("not found, DVB-T", !res->found && res->dvbt && !res->pilots && res->pattern == 0);
            CHECK("no pilot MER", !res->pilot_mer_valid && !res->mer_valid);
        }
    }

    memset(x, 0, max * sizeof(*x));
    add_noise(x, max, 0.01);
    CHECK("noise", m2sdr_dvbt2_analyse(x, max, fs, NULL, res) == 0 && !res->pilots && res->p1_count == 0);
    CHECK("bad sample rate", m2sdr_dvbt2_analyse(x, max, 10e6, NULL, res) != 0);
    CHECK("too short", m2sdr_dvbt2_analyse(x, 5000, fs, NULL, res) != 0);
    CHECK("no samples", m2sdr_dvbt2_analyse(NULL, max, fs, NULL, res) != 0);
    CHECK("no result", m2sdr_dvbt2_analyse(x, max, fs, NULL, NULL) != 0);
    free(x);
    free(res);
}

/* Report of m2sdr_analyzer */
/*--------------------------*/

/* Synthetic captures for make test-analyzer, SC16 on two channels at 64/7 MSPS: 0 a 32k extended
 * multiplex of rotated 256QAM (as UK and France), 1 an 8k one of 64QAM with an echo. */
static const struct dvbt2_signal report_signals[2] = {
    { 32768, true,  0, 7, 16, true,   60, false, 200000,      0.0, 35.0,  0.0, -99.0 },
    {  8192, false, 4, 2,  8, false, 100, true,  300000, 166667.0, 28.0, 15.0, -10.0 },
};

static int write_capture(const char *path, int which)
{
    const double fs = 64e6 / 7.0;
    const size_t max = (size_t)(0.26 * fs);
    float complex *x = malloc(max * sizeof(*x));
    FILE *f = fopen(path, "wb");
    size_t count, i;

    if (!x || !f)
        return 1;
    count = make_dvbt2(x, max, fs, &report_signals[which]);
    for (i = 0; i < count; i++) {
        const short v[4] = { (short)lrintf(crealf(x[i]) * 2047.0f), (short)lrintf(cimagf(x[i]) * 2047.0f),
                             (short)lrintf(crealf(x[i]) * 2047.0f), (short)lrintf(cimagf(x[i]) * 2047.0f) };
        if (fwrite(v, sizeof(v), 1, f) != 1)
            return 1;
    }
    free(x);
    return fclose(f) == 0 ? 0 : 1;
}

static double json_number(const char *json, const char *name)
{
    char key[64];
    const char *p = strstr(json, "\"dvbt2\"");

    if (!p)
        return NAN;
    snprintf(key, sizeof(key), "\"%s\":", name);
    p = strstr(p, key);
    return p ? strtod(p + strlen(key), NULL) : NAN;
}

static int check_report(const char *path, int which)
{
    static char json[32768];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        printf("test_m2sdr_analysis_dvbt2: cannot open %s\n", path);
        return 1;
    }
    n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';
    printf("DVB-T2 report of m2sdr_analyzer (%s):\n", which ? "8k, 64QAM, echo" : "32k extended, rotated 256QAM");
    CHECK("found, pilots", strstr(json, "\"found\": true") && strstr(json, "\"pilots\": true"));
    CHECK("not decoded: S1/S2, L1", strstr(json, "\"s1s2_decoded\": false") && strstr(json, "\"l1_decoded\": false"));
    {
        /* The analyzer looks at 0.25 s of the file. */
        const struct dvbt2_signal *sig = &report_signals[which];
        const size_t frame = t2_frame_len(sig), span = (size_t)(0.25 * 64e6 / 7.0);
        int np1 = 0;

        while ((size_t)sig->lead + (size_t)np1 * frame + 2048 <= span)
            np1++;
        CHECK_NEAR("P1 count", json_number(json, "p1_count"), np1, 0);
        if (np1 >= 2)
            CHECK_NEAR("frame (ms)", json_number(json, "frame_ms"), frame / (64e6 / 7.0) * 1e3, 0.002);
    }
    if (which == 0) {
        CHECK("32k extended, guard 1/128, PP7", strstr(json, "\"fft_size\": \"32k\"") && strstr(json, "\"extended\": true") &&
                                               strstr(json, "\"guard_interval\": \"1/128\"") && strstr(json, "\"pattern\": \"PP7\""));
        CHECK("rotated, no data MER", strstr(json, "\"rotated\": true") && strstr(json, "\"mer_db\": null"));
        /* 35 dB, less the quantisation of SC16 (a level of 0.1 full scale: 205 steps, -58 dB). */
        CHECK_NEAR("pilot MER (dB)", json_number(json, "pilot_mer_db"), 35.0, 1.0);
        CHECK_NEAR("carrier offset (Hz)", json_number(json, "carrier_offset_hz"), 0.0, 20.0);
    } else {
        /* An echo of amplitude a leaves (1 - a^2) / (1 + a^2) of the power: -0.9 dB at -10 dB. */
        const double echo_db = 10.0 * log10((1.0 - 0.1) / (1.0 + 0.1));

        CHECK("8k, guard 1/8, PP2", strstr(json, "\"fft_size\": \"8k\"") && strstr(json, "\"extended\": false") &&
                                    strstr(json, "\"guard_interval\": \"1/8\"") && strstr(json, "\"pattern\": \"PP2\""));
        CHECK("64QAM, not rotated", strstr(json, "\"constellation\": \"64QAM\"") && strstr(json, "\"rotated\": false"));
        CHECK_NEAR("pilot MER (dB)", json_number(json, "pilot_mer_db"), 28.0 + echo_db, 1.0);
        /* The data cells: the noise of the channel estimate on top, 10 log(1 + 0.7 / (4/3)^2). */
        CHECK_NEAR("MER (dB)", json_number(json, "mer_db"), 28.0 + echo_db - 1.5, 1.0);
        CHECK_NEAR("carrier offset (Hz)", json_number(json, "carrier_offset_hz"), 166667.0, 20.0);
        CHECK_NEAR("echo delay (us)", json_number(json, "echo_delay_us"), 15.0, 0.5);
        CHECK_NEAR("echo level (dB)", json_number(json, "echo_level_db"), -10.0, 1.5);
    }
    if (failures) {
        printf("test_m2sdr_analysis_dvbt2: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis_dvbt2: report checked\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--write") == 0)
        return write_capture(argv[3], atoi(argv[2]) != 0);
    if (argc == 4 && strcmp(argv[1], "--check") == 0)
        return check_report(argv[3], atoi(argv[2]) != 0);
    test_dvbt2();
    if (failures) {
        printf("test_m2sdr_analysis_dvbt2: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_analysis_dvbt2: all tests passed\n");
    return 0;
}
