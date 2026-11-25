/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Generator Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 * Description:
 * This utility generates tone, white noise, or PRBS signals into interleaved 16-bit I/Q samples for
 * use with the M2SDR software-defined radio. It runs in real-time using DMA to transmit the generated signal.
 *
 * Note: Configure the RF settings first using m2sdr_rf before running this utility.
 *
 * Usage Example:
 *     ./m2sdr_rf -samplerate 30.72e6 -tx_freq 100e6 -tx-att 10
 *     ./m2sdr_gen -s 30.72e6 -t tone -f 1e6 -a 1.0
 *     ./m2sdr_gen -s 30.72e6 -t white -a 1.0
 *     ./m2sdr_gen -s 30.72e6 -t prbs -a 1.0
 *
 * TX control uses positive attenuation values across both the native and SoapySDR paths.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>

#include "liblitepcie.h"
#include "m2sdr.h"
#include "m2sdr_analysis.h"
#include "m2sdr_cli.h"
#include "m2sdr_tool.h"
/* With SSE2, kiss_fft is used in its SIMD mode: an FFT per lane, four OFDM symbols per call,
 * each lane with the operations of the scalar FFT. The tool is then linked with the kiss_fft
 * object built that way (see the Makefile). Both are built with -ffp-contract=off: the samples
 * do not depend on where the compiler would fuse a product and a sum.
 * M2SDR_GEN_NO_SIMD leaves all of it out: the build the tests compare this one with. */
#if defined(__SSE2__) && !defined(M2SDR_GEN_NO_SIMD)
#define GEN_SSE2
#define USE_SIMD
#include <emmintrin.h>
#endif
#include "kissfft/kiss_fft.h"
#include "config.h"
#include "csr.h"

/* This file names its samples I and Q: the imaginary unit of <complex.h> is not used here. */
#undef I

/* Variables */
/*-----------*/

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    keep_running = 0;
}

/* PRBS bit generator (PRBS31: x^31 + x^28 + 1) */
static uint32_t prbs_lfsr = 0xFFFFFFFFu; /* Initial seed, all 1s */

/* The next 12 bits, first one in the MSB: each is the XOR of bits 30 and 27 of the register
 * shifted so far, and bits 16 to 30 are there from the start, so the 12 come in one step. */
static int32_t get_prbs_12bits(void) {
    const uint32_t bits = ((prbs_lfsr >> 19) ^ (prbs_lfsr >> 16)) & 0xFFF;
    prbs_lfsr = (prbs_lfsr << 12) | bits;
    return (int32_t)bits;
}

struct ofdm_state {
    int fft_size;
    int cp_len;
    int occupied_carriers;
    int bits_per_symbol;
    uint32_t lfsr;
    kiss_fft_cfg ifft_cfg;
    kiss_fft_cpx *freq_bins;
    kiss_fft_cpx *time_bins;
    float *symbol_i;
    float *symbol_q;
    int symbol_len;
    int sample_index;
    int lane;                        /* SIMD: symbol of the last four being given, */
    int pos;                         /* and the place in it. */
};

#ifndef USE_SIMD
static void *xcalloc(size_t nmemb, size_t size)
{
    void *ptr = calloc(nmemb, size);
    if (!ptr) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return ptr;
}
#endif

static uint32_t prng_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0)
        x = 1;
    *state = x;
    return x;
}

/* One carrier, to its real and imaginary parts. */
static inline void ofdm_modulate_symbol(struct ofdm_state *st, float *r, float *i)
{
    if (st->bits_per_symbol == 2) {
        /* From a table: the bits are random, a branch on them is mispredicted half the time. */
        static const float levels[2] = {-0.70710678f, 0.70710678f};
        *r = levels[prng_next(&st->lfsr) & 1];
        *i = levels[prng_next(&st->lfsr) & 1];
        return;
    }

    if (st->bits_per_symbol == 4) {
        static const float levels[4] = {-3.0f, -1.0f, 3.0f, 1.0f};
        *r = levels[prng_next(&st->lfsr) & 3] * 0.31622777f;
        *i = levels[prng_next(&st->lfsr) & 3] * 0.31622777f;
        return;
    }

    *r = (float)(int)(2 * (prng_next(&st->lfsr) & 1)) - 1.0f;
    *i = 0.0f;
}

#ifdef USE_SIMD

#define OFDM_LANES 4

/* Four symbols, one per lane: their carriers are drawn a symbol after the other, as when they
 * are made one at a time, and each lane goes through what ofdm_generate_symbol() does below.
 * They stay in time_bins, a lane per symbol: ofdm_next_sample() reads them there, the end of a
 * symbol first as its cyclic prefix. */
static void ofdm_generate_symbols(struct ofdm_state *st)
{
    const int n = st->fft_size, half = st->occupied_carriers / 2;
    float max_mag[OFDM_LANES] = { 0 };
    __m128d max_lo = _mm_setzero_pd(), max_hi = _mm_setzero_pd(), thr_lo, thr_hi;
    __m128 vmax;

    memset(st->freq_bins, 0, sizeof(*st->freq_bins) * n);
    for (int lane = 0; lane < OFDM_LANES; lane++) {
        for (int i = 0; i < half; i++) {
            ofdm_modulate_symbol(st, (float *)&st->freq_bins[1 + i].r + lane, (float *)&st->freq_bins[1 + i].i + lane);
            ofdm_modulate_symbol(st, (float *)&st->freq_bins[n - half + i].r + lane,
                                 (float *)&st->freq_bins[n - half + i].i + lane);
        }
    }

    kiss_fft(st->ifft_cfg, st->freq_bins, st->time_bins);

    /* 1 / N: a product when N is a power of two, where it gives what the division gives. */
    if ((n & (n - 1)) == 0) {
        const __m128 inv = _mm_set1_ps(1.0f / (float)n);

        for (int i = 0; i < n; i++) {
            st->time_bins[i].r = _mm_mul_ps(st->time_bins[i].r, inv);
            st->time_bins[i].i = _mm_mul_ps(st->time_bins[i].i, inv);
        }
    } else {
        const __m128 size = _mm_set1_ps((float)n);

        for (int i = 0; i < n; i++) {
            st->time_bins[i].r = _mm_div_ps(st->time_bins[i].r, size);
            st->time_bins[i].i = _mm_div_ps(st->time_bins[i].i, size);
        }
    }
    /* The largest magnitude of each symbol: hypotf() only where it can be, see below. */
    for (int i = 0; i < n; i++) {
        const __m128 r = st->time_bins[i].r, q = st->time_bins[i].i;
        const __m128d r_lo = _mm_cvtps_pd(r), r_hi = _mm_cvtps_pd(_mm_movehl_ps(r, r));
        const __m128d q_lo = _mm_cvtps_pd(q), q_hi = _mm_cvtps_pd(_mm_movehl_ps(q, q));

        max_lo = _mm_max_pd(max_lo, _mm_add_pd(_mm_mul_pd(r_lo, r_lo), _mm_mul_pd(q_lo, q_lo)));
        max_hi = _mm_max_pd(max_hi, _mm_add_pd(_mm_mul_pd(r_hi, r_hi), _mm_mul_pd(q_hi, q_hi)));
    }
    thr_lo = _mm_mul_pd(max_lo, _mm_set1_pd(1.0 - 1e-6));
    thr_hi = _mm_mul_pd(max_hi, _mm_set1_pd(1.0 - 1e-6));
    for (int i = 0; i < n; i++) {
        const __m128 r = st->time_bins[i].r, q = st->time_bins[i].i;
        const __m128d r_lo = _mm_cvtps_pd(r), r_hi = _mm_cvtps_pd(_mm_movehl_ps(r, r));
        const __m128d q_lo = _mm_cvtps_pd(q), q_hi = _mm_cvtps_pd(_mm_movehl_ps(q, q));
        const int near = _mm_movemask_pd(_mm_cmpge_pd(_mm_add_pd(_mm_mul_pd(r_lo, r_lo), _mm_mul_pd(q_lo, q_lo)), thr_lo)) |
                         _mm_movemask_pd(_mm_cmpge_pd(_mm_add_pd(_mm_mul_pd(r_hi, r_hi), _mm_mul_pd(q_hi, q_hi)), thr_hi)) << 2;

        for (int lane = 0; near && lane < OFDM_LANES; lane++) {
            if (near & (1 << lane)) {
                const float mag = hypotf(((const float *)&st->time_bins[i].r)[lane], ((const float *)&st->time_bins[i].i)[lane]);

                if (mag > max_mag[lane])
                    max_mag[lane] = mag;
            }
        }
    }
    for (int lane = 0; lane < OFDM_LANES; lane++) {
        if (max_mag[lane] < 1e-9f)
            max_mag[lane] = 1.0f;
    }
    vmax = _mm_loadu_ps(max_mag);
    for (int i = 0; i < n; i++) {
        st->time_bins[i].r = _mm_div_ps(st->time_bins[i].r, vmax);
        st->time_bins[i].i = _mm_div_ps(st->time_bins[i].i, vmax);
    }
    st->lane = 0;
    st->pos  = 0;
}

static void ofdm_next_sample(struct ofdm_state *st, float *i_out, float *q_out)
{
    int k;

    if (st->lane >= OFDM_LANES)
        ofdm_generate_symbols(st);
    k = st->pos < st->cp_len ? st->fft_size - st->cp_len + st->pos : st->pos - st->cp_len;
    *i_out = ((const float *)&st->time_bins[k].r)[st->lane];
    *q_out = ((const float *)&st->time_bins[k].i)[st->lane];
    if (++st->pos >= st->symbol_len) {
        st->pos = 0;
        st->lane++;
    }
}

#else /* USE_SIMD */

static void ofdm_generate_symbol(struct ofdm_state *st)
{
    float max_mag = 0.0f;
    int half = st->occupied_carriers / 2;

    memset(st->freq_bins, 0, sizeof(*st->freq_bins) * st->fft_size);

    for (int i = 0; i < half; i++) {
        ofdm_modulate_symbol(st, &st->freq_bins[1 + i].r, &st->freq_bins[1 + i].i);
        ofdm_modulate_symbol(st, &st->freq_bins[st->fft_size - half + i].r, &st->freq_bins[st->fft_size - half + i].i);
    }

    kiss_fft(st->ifft_cfg, st->freq_bins, st->time_bins);

    /* 1 / N: a product when N is a power of two, where it gives what the division gives. */
    if ((st->fft_size & (st->fft_size - 1)) == 0) {
        const float inv = 1.0f / (float)st->fft_size;

        for (int i = 0; i < st->fft_size; i++) {
            st->time_bins[i].r *= inv;
            st->time_bins[i].i *= inv;
        }
    } else {
        for (int i = 0; i < st->fft_size; i++) {
            st->time_bins[i].r /= st->fft_size;
            st->time_bins[i].i /= st->fft_size;
        }
    }
    /* The largest magnitude: hypotf() only where it can be, the samples whose squared magnitude
     * is within 1e-6 of the largest one, instead of on every sample. */
    {
        double max_sq = 0.0;

        for (int i = 0; i < st->fft_size; i++) {
            const double sq = (double)st->time_bins[i].r * st->time_bins[i].r +
                              (double)st->time_bins[i].i * st->time_bins[i].i;
            if (sq > max_sq)
                max_sq = sq;
        }
        for (int i = 0; i < st->fft_size; i++) {
            const double sq = (double)st->time_bins[i].r * st->time_bins[i].r +
                              (double)st->time_bins[i].i * st->time_bins[i].i;
            if (sq >= max_sq * (1.0 - 1e-6)) {
                const float mag = hypotf(st->time_bins[i].r, st->time_bins[i].i);
                if (mag > max_mag)
                    max_mag = mag;
            }
        }
    }

    if (max_mag < 1e-9f)
        max_mag = 1.0f;

    /* The symbol over its peak, then its end again in front of it as the cyclic prefix. */
    {
        float *const body_i = st->symbol_i + st->cp_len, *const body_q = st->symbol_q + st->cp_len;
        int i = 0;

        for (; i < st->fft_size; i++) {
            body_i[i] = st->time_bins[i].r / max_mag;
            body_q[i] = st->time_bins[i].i / max_mag;
        }
        memcpy(st->symbol_i, body_i + st->fft_size - st->cp_len, (size_t)st->cp_len * sizeof(*body_i));
        memcpy(st->symbol_q, body_q + st->fft_size - st->cp_len, (size_t)st->cp_len * sizeof(*body_q));
    }

    st->sample_index = 0;
}

static void ofdm_next_sample(struct ofdm_state *st, float *i_out, float *q_out)
{
    if (st->sample_index >= st->symbol_len)
        ofdm_generate_symbol(st);
    *i_out = st->symbol_i[st->sample_index];
    *q_out = st->symbol_q[st->sample_index];
    st->sample_index++;
}

#endif /* USE_SIMD */

static void ofdm_init(struct ofdm_state *st, int fft_size, int cp_len, int occupied_carriers, const char *modulation, uint32_t seed)
{
    memset(st, 0, sizeof(*st));
    st->fft_size = fft_size;
    st->cp_len = cp_len;
    st->occupied_carriers = occupied_carriers;
    st->symbol_len = fft_size + cp_len;
    st->lfsr = seed ? seed : 1;

    if (strcmp(modulation, "qpsk") == 0)
        st->bits_per_symbol = 2;
    else if (strcmp(modulation, "16qam") == 0)
        st->bits_per_symbol = 4;
    else {
        fprintf(stderr, "Unsupported OFDM modulation: %s\n", modulation);
        exit(1);
    }

    /* kiss_fft allocates its state and the bins as it needs them (aligned for the SIMD mode). */
    st->ifft_cfg  = kiss_fft_alloc(st->fft_size, 1, NULL, NULL);
    st->freq_bins = KISS_FFT_MALLOC(st->fft_size * sizeof(*st->freq_bins));
    st->time_bins = KISS_FFT_MALLOC(st->fft_size * sizeof(*st->time_bins));
    if (!st->ifft_cfg || !st->freq_bins || !st->time_bins) {
        fprintf(stderr, "Failed to allocate OFDM IFFT state\n");
        exit(1);
    }
#ifdef USE_SIMD
    ofdm_generate_symbols(st);
#else
    st->symbol_i = xcalloc(st->symbol_len, sizeof(*st->symbol_i));
    st->symbol_q = xcalloc(st->symbol_len, sizeof(*st->symbol_q));
    ofdm_generate_symbol(st);
#endif
}

static void ofdm_cleanup(struct ofdm_state *st)
{
    if (!st)
        return;
    kiss_fft_free(st->ifft_cfg);
    KISS_FFT_FREE(st->freq_bins);
    KISS_FFT_FREE(st->time_bins);
    free(st->symbol_i);
    free(st->symbol_q);
    memset(st, 0, sizeof(*st));
}

#ifdef CSR_GPIO_BASE
static void m2sdr_write32(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    if (m2sdr_reg_write(dev, addr, val) != 0) {
        fprintf(stderr, "CSR write failed @0x%08x\n", addr);
        exit(1);
    }
}
#endif

/* Single-carrier PSK or QAM under a root raised cosine */
/*-----------------------------------------------------*/

#define QAM_SPAN   8                 /* Symbols of the pulse on each side. */
#define QAM_PHASES 64                /* Table points per symbol. */
#define QAM_PEAK   1.8               /* Peaks of the filtered signal over its corner symbols. */
#define QAM_BLOCK  4                 /* Samples computed together. */
#define QAM_KEPT   64                /* Blocks whose pulses are kept. */

struct qam_state {
    float complex points[256];
    int    npoints;
    double step;                     /* Symbols per sample. */
    double frac;                     /* Position in the current symbol. */
    float complex symbols[2 * QAM_SPAN + QAM_BLOCK];  /* The current ones, and room for the new ones of a block. */
    float  pulse[2 * QAM_SPAN * QAM_PHASES + 2];
    float  scale;
    uint32_t lfsr;
    /* The pulse under each symbol depends on the position in the symbol only, and most of the
     * time goes into it. A block of QAM_BLOCK samples is known by the position of its first
     * one: its pulses, where its new symbols fall and the position after it are kept. A symbol
     * rate that divides the sample rate (the default is a sixteenth) comes back to the same few
     * positions, bit for bit, and finds its blocks here; another rate computes each one. */
    struct qam_kept {
        double frac;                 /* Position of the first sample; -1: none. */
        double frac_end;             /* Position after the block. */
        int    shift[QAM_BLOCK];     /* New symbols before each sample, */
        int    shifts;               /* and in the block. */
        float  w[2 * QAM_SPAN][QAM_BLOCK];   /* By symbol, then by sample. */
    } kept[QAM_KEPT];
    float  out_i[QAM_BLOCK], out_q[QAM_BLOCK];        /* Samples of the last block, */
    int    out_pos;                                   /* and the next one to give. */
};

/* Settings of --signal qam. */
static enum m2sdr_modulation qam_modulation = M2SDR_MOD_QPSK;
static double qam_symbol_rate = 0.0;      /* 0: a sixteenth of the sample rate. */
static double qam_rolloff     = 0.35;

static double qam_rrc(double t, double alpha)
{
    if (fabs(t) < 1e-9)
        return 1.0 - alpha + 4.0 * alpha / M_PI;
    if (fabs(fabs(t) - 1.0 / (4.0 * alpha)) < 1e-9)
        return alpha / sqrt(2.0) * ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * alpha)) +
                                    (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * alpha)));
    return (sin(M_PI * t * (1.0 - alpha)) + 4.0 * alpha * t * cos(M_PI * t * (1.0 + alpha))) /
           (M_PI * t * (1.0 - 16.0 * alpha * alpha * t * t));
}

static float complex qam_random_symbol(struct qam_state *st)
{
    st->lfsr ^= st->lfsr << 13;
    st->lfsr ^= st->lfsr >> 17;
    st->lfsr ^= st->lfsr << 5;
    return st->points[(st->lfsr >> 8) % (uint32_t)st->npoints];
}

static void qam_init(struct qam_state *st, double sample_rate, uint32_t seed)
{
    float corner = 0;
    int i;

    memset(st, 0, sizeof(*st));
    st->npoints = m2sdr_constellation(qam_modulation, st->points);
    st->step    = (qam_symbol_rate > 0 ? qam_symbol_rate : sample_rate / 16.0) / sample_rate;
    st->lfsr    = seed | 1u;
    for (i = 0; i <= 2 * QAM_SPAN * QAM_PHASES; i++)
        st->pulse[i] = (float)qam_rrc((double)i / QAM_PHASES - QAM_SPAN, qam_rolloff);
    for (i = 0; i < st->npoints; i++) {
        if (cabsf(st->points[i]) > corner)
            corner = cabsf(st->points[i]);
    }
    st->scale = 1.0f / (corner * (float)QAM_PEAK);
    for (i = 0; i < QAM_KEPT; i++)
        st->kept[i].frac = -1.0;
    st->out_pos = QAM_BLOCK;
    for (i = 0; i < 2 * QAM_SPAN; i++)
        st->symbols[i] = qam_random_symbol(st);
}

/* fminf(fmaxf(x, -1), 1), NaN to -1 included, without the calls to libm. */
static inline float gen_clamp1(float x)
{
    return x > 1.0f ? 1.0f : x >= -1.0f ? x : -1.0f;
}

/* The pulse under each of the 2 * QAM_SPAN symbols for a sample at `frac` in its symbol, to
 * w[0], w[stride], ... */
static void qam_pulse(const struct qam_state *st, double frac, float *w, int stride)
{
    int j = 0;

#ifdef GEN_SSE2
    /* Two symbols at a time, the same operations as below. */
    {
        const __m128d vfrac = _mm_set1_pd(frac), vphases = _mm_set1_pd((double)QAM_PHASES);

        for (; j < 2 * QAM_SPAN; j += 2) {
            const __m128d c = _mm_set_pd((double)(2 * QAM_SPAN - 2 - j), (double)(2 * QAM_SPAN - 1 - j));
            const __m128d u = _mm_mul_pd(_mm_add_pd(vfrac, c), vphases);
            const __m128i k = _mm_cvttpd_epi32(u);
            const __m128  t = _mm_cvtpd_ps(_mm_sub_pd(u, _mm_cvtepi32_pd(k)));
            const int k0 = _mm_cvtsi128_si32(k), k1 = _mm_cvtsi128_si32(_mm_srli_si128(k, 4));
            const __m128 p0 = _mm_set_ps(0.0f, 0.0f, st->pulse[k1], st->pulse[k0]);
            const __m128 p1 = _mm_set_ps(0.0f, 0.0f, st->pulse[k1 + 1], st->pulse[k0 + 1]);
            const __m128 r  = _mm_add_ps(p0, _mm_mul_ps(t, _mm_sub_ps(p1, p0)));

            _mm_store_ss(w + stride * j, r);
            _mm_store_ss(w + stride * (j + 1), _mm_shuffle_ps(r, r, 1));
        }
    }
#endif
    for (; j < 2 * QAM_SPAN; j++) {
        /* Symbol j is frac + QAM_SPAN - 1 - j symbols before the sample. */
        const double u = (frac + (double)(2 * QAM_SPAN - 1 - j)) * QAM_PHASES;
        const int k = (int)u;
        w[stride * j] = st->pulse[k] + (float)(u - k) * (st->pulse[k + 1] - st->pulse[k]);
    }
}

/* The block that starts at st->frac: the one kept, or computed in its place. */
static const struct qam_kept *qam_block(struct qam_state *st)
{
    const double frac0 = st->frac;
    struct qam_kept *b;
    uint64_t bits;
    double frac = frac0;
    int l;

    /* All the bits of the position choose the place: positions a rounding apart, as a rate
     * that does not divide the sample rate gives, do not take the place of each other. */
    memcpy(&bits, &frac0, sizeof(bits));
    b = &st->kept[(bits * 0x9E3779B97F4A7C15ull) >> 58];      /* 6 bits: QAM_KEPT places. */
    if (b->frac == frac0)
        return b;
    b->shifts = 0;
    for (l = 0; l < QAM_BLOCK; l++) {
        qam_pulse(st, frac, &b->w[0][l], QAM_BLOCK);
        b->shift[l] = b->shifts;
        frac += st->step;
        if (frac >= 1.0) {
            b->shifts++;
            frac -= 1.0;
        }
    }
    b->frac     = frac0;
    b->frac_end = frac;
    return b;
}

/* QAM_BLOCK samples: each is the symbols within QAM_SPAN of it, each under the pulse, summed
 * in the order of the symbols. The sums of a sample wait on each other, the ones of different
 * samples do not: taken together, a sample per lane, they cost a fraction of one after the
 * other, and give the same samples. The new symbols of the block go after the current ones.
 * A symbol rate above the sample rate (several new symbols per sample, of no use) is left to
 * qam_next_single(). */
static void qam_next_block(struct qam_state *st)
{
    const struct qam_kept *b = qam_block(st);
    const float scale = st->scale;
    float re[QAM_BLOCK], im[QAM_BLOCK];
    int l, j;

    for (l = 0; l < b->shifts; l++)
        st->symbols[2 * QAM_SPAN + l] = qam_random_symbol(st);
#if defined(GEN_SSE2) && QAM_BLOCK == 4
    /* A sample per lane: each lane adds its products in the order of the symbols. */
    {
        __m128 vre = _mm_setzero_ps(), vim = _mm_setzero_ps();

        if (b->shifts == 0) {
            /* The same symbols for the four samples. */
            for (j = 0; j < 2 * QAM_SPAN; j++) {
                const __m128 vw = _mm_loadu_ps(b->w[j]);

                vre = _mm_add_ps(vre, _mm_mul_ps(_mm_set1_ps(crealf(st->symbols[j])), vw));
                vim = _mm_add_ps(vim, _mm_mul_ps(_mm_set1_ps(cimagf(st->symbols[j])), vw));
            }
        } else {
            const float complex *s0 = st->symbols + b->shift[0], *s1 = st->symbols + b->shift[1];
            const float complex *s2 = st->symbols + b->shift[2], *s3 = st->symbols + b->shift[3];

            for (j = 0; j < 2 * QAM_SPAN; j++) {
                const __m128 vw = _mm_loadu_ps(b->w[j]);

                vre = _mm_add_ps(vre, _mm_mul_ps(_mm_setr_ps(crealf(s0[j]), crealf(s1[j]), crealf(s2[j]), crealf(s3[j])), vw));
                vim = _mm_add_ps(vim, _mm_mul_ps(_mm_setr_ps(cimagf(s0[j]), cimagf(s1[j]), cimagf(s2[j]), cimagf(s3[j])), vw));
            }
        }
        _mm_storeu_ps(re, vre);
        _mm_storeu_ps(im, vim);
    }
#else
    for (l = 0; l < QAM_BLOCK; l++)
        re[l] = im[l] = 0;
    for (j = 0; j < 2 * QAM_SPAN; j++) {
        for (l = 0; l < QAM_BLOCK; l++) {
            re[l] += crealf(st->symbols[b->shift[l] + j]) * b->w[j][l];
            im[l] += cimagf(st->symbols[b->shift[l] + j]) * b->w[j][l];
        }
    }
#endif
    if (b->shifts)
        memmove(st->symbols, st->symbols + b->shifts, 2 * QAM_SPAN * sizeof(st->symbols[0]));
    for (l = 0; l < QAM_BLOCK; l++) {
        st->out_i[l] = gen_clamp1(re[l] * scale);
        st->out_q[l] = gen_clamp1(im[l] * scale);
    }
    st->frac    = b->frac_end;
    st->out_pos = 0;
}

/* One sample, with as many new symbols after it as the symbol rate asks for. */
static void qam_next_single(struct qam_state *st)
{
    float w[2 * QAM_SPAN], re = 0, im = 0;
    int j;

    qam_pulse(st, st->frac, w, 1);

    for (j = 0; j < 2 * QAM_SPAN; j++) {
        re += crealf(st->symbols[j]) * w[j];
        im += cimagf(st->symbols[j]) * w[j];
    }
    st->frac += st->step;
    while (st->frac >= 1.0) {
        memmove(st->symbols, st->symbols + 1, (2 * QAM_SPAN - 1) * sizeof(st->symbols[0]));
        st->symbols[2 * QAM_SPAN - 1] = qam_random_symbol(st);
        st->frac -= 1.0;
    }
    st->out_pos = QAM_BLOCK - 1;
    st->out_i[st->out_pos] = gen_clamp1(re * st->scale);
    st->out_q[st->out_pos] = gen_clamp1(im * st->scale);
}

static void qam_next_sample(struct qam_state *st, float *i_out, float *q_out)
{
    if (st->out_pos >= QAM_BLOCK) {
        if (st->step <= 1.0)
            qam_next_block(st);
        else
            qam_next_single(st);
    }
    *i_out = st->out_i[st->out_pos];
    *q_out = st->out_q[st->out_pos];
    st->out_pos++;
}

struct gen_signal_state {
    const char *signal_type;
    double amplitude;
    double phi;
    double omega;
    double pps_period_samples;
    double pps_high_samples;
    double pps_freq;
    uint8_t gpio_pin;
    uint8_t use_8bit;
    uint64_t sample_count;
    uint32_t lfsr;
    struct ofdm_state *ofdm;
    struct qam_state *qam;
    /* Tone: cos and sin of phi, turned by omega from one sample to the next. */
    double tone_c, tone_s, tone_co, tone_so, tone_omega;
    unsigned tone_run;             /* Samples before they are taken from phi again. */
};

/* The tone is turned by a rotation for GEN_TONE_RESYNC samples, then taken again from the
 * phase with cos() and sin(): the rounding of the rotations stays under 1e-12 of full scale.
 * That can only change a sample that falls on a step of the conversion to integers, so a value
 * within GEN_TONE_GUARD of a step (of 12 bits: an integer, truncated; of 8 bits: a half,
 * rounded) is taken from cos() and sin() too: the samples are the ones of cos() and sin() on
 * each sample, at a fraction of their cost. The guard also covers the rounding of the value
 * to float before the conversion (1.3e-4 of a step at most). */
#define GEN_TONE_RESYNC 1024
#ifndef GEN_TONE_GUARD
#define GEN_TONE_GUARD  1e-3       /* 2: cos() and sin() on every sample, the reference of the tests. */
#endif

static inline int gen_tone_near_step(double value, int use_8bit)
{
    const double y = value * (use_8bit ? 127.0 : 2047.0);
    const double d = fabs(y - (double)(long)y);       /* 0 to 1 from the integer toward zero. */

    return use_8bit ? fabs(d - 0.5) < GEN_TONE_GUARD : (d < GEN_TONE_GUARD || d > 1.0 - GEN_TONE_GUARD);
}

enum gen_signal {
    GEN_SIGNAL_NONE,
    GEN_SIGNAL_TONE,
    GEN_SIGNAL_OFDM,
    GEN_SIGNAL_QAM,
    GEN_SIGNAL_WHITE,
    GEN_SIGNAL_PRBS,
};

static enum gen_signal gen_signal_of(const char *type)
{
    if (strcmp(type, "tone") == 0)
        return GEN_SIGNAL_TONE;
    if (strcmp(type, "ofdm") == 0)
        return GEN_SIGNAL_OFDM;
    if (strcmp(type, "qam") == 0)
        return GEN_SIGNAL_QAM;
    if (strcmp(type, "white") == 0)
        return GEN_SIGNAL_WHITE;
    if (strcmp(type, "prbs") == 0)
        return GEN_SIGNAL_PRBS;
    return GEN_SIGNAL_NONE;
}

static int8_t gen_float_to_sc8(float sample)
{
    if (!isfinite(sample))
        return 0;
    float scaled = sample * 127.0f;
    if (scaled > 127.0f)
        return 127;
    if (scaled < -128.0f)
        return -128;
    /* lroundf(): half of its sign added in double, where the sum is exact, then truncated. */
    return (int8_t)(int)((double)scaled + copysign(0.5, (double)scaled));
}

static void gen_fill_payload(uint8_t *payload, size_t payload_bytes, struct gen_signal_state *st)
{
    int bytes_per_frame = st->use_8bit ? 4 : 8;
    size_t num_frames = payload_bytes / (size_t)bytes_per_frame;
    size_t generated_bytes = num_frames * (size_t)bytes_per_frame;

    /* What does not change in the buffer is read once: the signal, and the place in the PPS
     * period, which then moves by one per sample (exactly: it is a fmod() of whole samples). */
    const enum gen_signal signal = gen_signal_of(st->signal_type);
    double pps_phase = st->pps_freq > 0 ? fmod(st->sample_count, st->pps_period_samples) : 0.0;

    for (size_t j = 0; j < num_frames; j++) {
        float I = 0.0f;
        float Q = 0.0f;

        if (signal == GEN_SIGNAL_TONE) {
            if (st->tone_run == 0 || st->tone_omega != st->omega) {
                st->tone_c     = cos(st->phi);
                st->tone_s     = sin(st->phi);
                st->tone_co    = cos(st->omega);
                st->tone_so    = sin(st->omega);
                st->tone_omega = st->omega;
                st->tone_run   = GEN_TONE_RESYNC;
            }
            {
                double vi = st->tone_c * st->amplitude;
                double vq = st->tone_s * st->amplitude;

                if (gen_tone_near_step(vi, st->use_8bit) || gen_tone_near_step(vq, st->use_8bit)) {
                    vi = cos(st->phi) * st->amplitude;
                    vq = sin(st->phi) * st->amplitude;
                }
                I = vi;
                Q = vq;
            }
            /* The phase stays within a turn, for a negative frequency too: left to grow, it
             * loses its resolution as the tone goes on. */
            st->phi += st->omega;
            if (st->phi >= 2 * M_PI)
                st->phi -= 2 * M_PI;
            else if (st->phi < 0)
                st->phi += 2 * M_PI;
            {
                const double c = st->tone_c * st->tone_co - st->tone_s * st->tone_so;

                st->tone_s = st->tone_s * st->tone_co + st->tone_c * st->tone_so;
                st->tone_c = c;
                st->tone_run--;
            }
        } else if (signal == GEN_SIGNAL_OFDM) {
            ofdm_next_sample(st->ofdm, &I, &Q);
            I *= st->amplitude;
            Q *= st->amplitude;
        } else if (signal == GEN_SIGNAL_QAM) {
            qam_next_sample(st->qam, &I, &Q);
            I *= st->amplitude;
            Q *= st->amplitude;
        } else if (signal == GEN_SIGNAL_WHITE) {
            /* Fast LFSR-based noise avoids rand() in the hot loop. */
            st->lfsr ^= st->lfsr << 13;
            st->lfsr ^= st->lfsr >> 17;
            st->lfsr ^= st->lfsr << 5;
            int16_t ni = (int16_t)(st->lfsr & 0xFFFF);
            int16_t nq = (int16_t)((st->lfsr >> 16) & 0xFFFF);
            I = (ni / 32768.0f) * st->amplitude;
            Q = (nq / 32768.0f) * st->amplitude;
        } else if (signal == GEN_SIGNAL_PRBS) {
            const int32_t value_I = get_prbs_12bits();
            const int32_t value_Q = get_prbs_12bits();

            I = ((float)(value_I - 2048) / 2048.0f) * st->amplitude;
            Q = ((float)(value_Q - 2048) / 2048.0f) * st->amplitude;
        }

        if (st->use_8bit) {
            int8_t I_int = gen_float_to_sc8(I);
            int8_t Q_int = gen_float_to_sc8(Q);
            ((int8_t *)payload)[4 * j + 0] = I_int;
            ((int8_t *)payload)[4 * j + 1] = Q_int;
            ((int8_t *)payload)[4 * j + 2] = I_int;
            ((int8_t *)payload)[4 * j + 3] = Q_int;
        } else {
            int16_t I_int = (int16_t)(I * 2047);
            int16_t Q_int = (int16_t)(Q * 2047);
            uint16_t gpio1 = 0, gpio2 = 0;

            if (st->pps_freq > 0) {
                if (pps_phase < st->pps_high_samples) {
                    gpio1 = 1 << st->gpio_pin;
                    gpio2 = 1 << st->gpio_pin;
                }
                pps_phase += 1.0;
                if (pps_phase >= st->pps_period_samples)
                    pps_phase = fmod(st->sample_count + 1, st->pps_period_samples);
            }

            ((int16_t *)payload)[4 * j + 0] = (I_int & 0xFFF) | (gpio1 << 12);
            ((int16_t *)payload)[4 * j + 1] = (Q_int & 0xFFF) | (gpio1 << 12);
            ((int16_t *)payload)[4 * j + 2] = (I_int & 0xFFF) | (gpio2 << 12);
            ((int16_t *)payload)[4 * j + 3] = (Q_int & 0xFFF) | (gpio2 << 12);
        }

        st->sample_count++;
    }

    if (generated_bytes < payload_bytes)
        memset(payload + generated_bytes, 0, payload_bytes - generated_bytes);
}

#define GEN_PACE_LEAD_S 0.004      /* Samples sent ahead of their time over Ethernet. */

/* Signal (DMA TX) with GPIO PPS */
/*-----------------------------*/

static void m2sdr_gen(const char *device_id, double sample_rate, double frequency, double amplitude, uint8_t zero_copy, double pps_freq, uint8_t gpio_pin, const char *signal_type, uint8_t enable_header, uint8_t use_8bit, int ofdm_fft_size, int ofdm_cp_len, int ofdm_occupied_carriers, const char *ofdm_modulation, uint32_t ofdm_seed) {
    static struct litepcie_dma_ctrl dma = {.use_reader = 1};

    int i = 0;
    int64_t last_time;
    uint64_t total_buffers = 0;
    uint64_t last_buffers = 0;
    uint64_t sw_underflows = 0;
    int64_t hw_count_stop = 0;
    struct m2sdr_dev *dev = NULL;
    enum m2sdr_transport_kind transport = M2SDR_TRANSPORT_KIND_UNKNOWN;
    int fd = -1;
    struct ofdm_state ofdm;
    int ofdm_enabled = strcmp(signal_type, "ofdm") == 0;
    int ofdm_inited = 0;
    int use_pcie_dma = 0;
    int stream_configured = 0;
    int exit_status = 1;
    bool pace_init = false, pace_tx = false;
    uint64_t fpga_underflows = 0;
    uint64_t paced_samples = 0;
    struct timespec pace_start = { 0, 0 };
    enum m2sdr_format format = use_8bit ? M2SDR_FORMAT_SC8_Q7 : M2SDR_FORMAT_SC16_Q11;
    unsigned payload_bytes = M2SDR_BUFFER_BYTES - (enable_header ? M2SDR_HEADER_BYTES : 0);
    unsigned samples_per_buf = m2sdr_bytes_to_samples(format, payload_bytes);
    struct gen_signal_state gen;

    if (samples_per_buf == 0) {
        fprintf(stderr, "Invalid TX buffer size\n");
        exit(1);
    }

    if (amplitude < 0.0)
        amplitude = 0.0;
    if (amplitude > 1.0)
        amplitude = 1.0;

    /* Open device for CSR access */
    if (m2sdr_open(&dev, device_id) != 0) {
        fprintf(stderr, "Could not open device: %s\n", device_id);
        exit(1);
    }
    if (m2sdr_get_transport(dev, &transport) != 0) {
        fprintf(stderr, "m2sdr_get_transport failed\n");
        goto cleanup;
    }

    if (m2sdr_set_bitmode(dev, use_8bit ? true : false) != 0) {
        fprintf(stderr, "m2sdr_set_bitmode failed\n");
        goto cleanup;
    }

    if (m2sdr_set_tx_header(dev, enable_header ? true : false) != 0) {
        fprintf(stderr, "m2sdr_set_tx_header failed\n");
        goto cleanup;
    }

#ifdef CSR_GPIO_BASE

    /* Enable GPIO in Packer/Unpacker mode if PPS is requested */
    if (pps_freq > 0) {
        uint32_t control = (1 << CSR_GPIO_CONTROL_ENABLE_OFFSET) | (1 << CSR_GPIO_CONTROL_LOOPBACK_OFFSET); /* ENABLE=1, SOURCE=0 (DMA), LOOPBACK=1 */
        m2sdr_write32(dev, CSR_GPIO_CONTROL_ADDR, control);
        double pps_period_s = 1.0 / pps_freq;
        double pps_high_s = pps_period_s * 0.2;
        printf("GPIO Enabled for PPS/Toggle at %.2f Hz (20%% high: %.3fs, 80%% low: %.3fs) on bit %d\n",
               pps_freq, pps_high_s, pps_period_s - pps_high_s, gpio_pin);
    }

#endif

    /* Print parameters */
    printf("Starting signal generation with parameters:\n");
    printf("  Device: %s\n", device_id);
    printf("  Transport: %s\n",
           transport == M2SDR_TRANSPORT_KIND_LITEPCIE ? "LitePCIe" :
           transport == M2SDR_TRANSPORT_KIND_LITEETH ? "LiteEth" :
           transport == M2SDR_TRANSPORT_KIND_LITEUSB ? "LiteUSB" : "unknown");
    printf("  Sample Rate: %.0f Hz\n", sample_rate);
    printf("  Signal Type: %s\n", signal_type);
    if (strcmp(signal_type, "tone") == 0) {
        printf("  Frequency: %.0f Hz\n", frequency);
    } else if (ofdm_enabled) {
        printf("  OFDM FFT Size: %d\n", ofdm_fft_size);
        printf("  OFDM CP Length: %d\n", ofdm_cp_len);
        printf("  OFDM Occupied Carriers: %d\n", ofdm_occupied_carriers);
        printf("  OFDM Modulation: %s\n", ofdm_modulation);
        printf("  OFDM Seed: %" PRIu32 "\n", ofdm_seed);
    }
    printf("  Amplitude: %.2f\n", amplitude);
    printf("  Zero-Copy Mode: %d\n", zero_copy);

    if (transport == M2SDR_TRANSPORT_KIND_LITEPCIE) {
        fd = m2sdr_get_fd(dev);
        if (fd < 0) {
            fprintf(stderr, "No LitePCIe fd available\n");
            goto cleanup;
        }

        dma.shared_fd = 1;
        dma.fds.fd = fd;
        if (litepcie_dma_init(&dma, "", zero_copy)) {
            fprintf(stderr, "litepcie_dma_init failed\n");
            goto cleanup;
        }

        dma.reader_enable = 1;
        use_pcie_dma = 1;
    } else {
        struct m2sdr_sync_params params;
        int rc;

        m2sdr_sync_params_init(&params);
        params.direction = M2SDR_TX;
        params.format = format;
        params.buffer_size = samples_per_buf;
        params.timeout_ms = 1000;
        params.zero_copy = zero_copy ? true : false;
        params.tx_header_enable = enable_header ? true : false;

        rc = m2sdr_sync_config_ex(dev, &params);
        if (rc != 0) {
            fprintf(stderr, "m2sdr_sync_config_ex failed: %s\n", m2sdr_strerror(rc));
            goto cleanup;
        }
        stream_configured = 1;
    }

    if (ofdm_enabled) {
        ofdm_init(&ofdm, ofdm_fft_size, ofdm_cp_len, ofdm_occupied_carriers, ofdm_modulation, ofdm_seed);
        ofdm_inited = 1;
    }

    /* Tone generation parameters */
    srand(time(NULL));
    memset(&gen, 0, sizeof(gen));
    gen.signal_type = signal_type;
    gen.amplitude = amplitude;
    gen.phi = 0.0;
    gen.omega = 2 * M_PI * frequency / sample_rate;
    gen.pps_freq = pps_freq;
    gen.pps_period_samples = pps_freq > 0 ? sample_rate / pps_freq : 0;
    gen.pps_high_samples = gen.pps_period_samples * 0.2;
    gen.gpio_pin = gpio_pin;
    gen.use_8bit = use_8bit;
    gen.lfsr = (uint32_t)rand() | 1u;
    gen.ofdm = &ofdm;
    {
        static struct qam_state qam;

        if (strcmp(signal_type, "qam") == 0) {
            qam_init(&qam, sample_rate, gen.lfsr);
            printf("  QAM: %s, %.0f symbols/s, roll-off %.2f\n", m2sdr_modulation_name(qam_modulation),
                   qam.step * sample_rate, qam_rolloff);
        }
        gen.qam = &qam;
    }

    /* Test Loop */
    last_time = get_time_ms();
    for (;;) {
        if (use_pcie_dma) {
            litepcie_dma_process(&dma);

            if (!keep_running) {
                hw_count_stop = dma.reader_sw_count + 16;
                break;
            }

            while (1) {
                char *buf_wr = litepcie_dma_next_write_buffer(&dma);
                if (!buf_wr)
                    break;

                if (dma.reader_sw_count - dma.reader_hw_count < 0)
                    sw_underflows += (dma.reader_hw_count - dma.reader_sw_count);

                if (enable_header) {
                    uint64_t ts = 0;
                    m2sdr_get_time(dev, &ts);
                    m2sdr_tool_write_dma_header((uint8_t *)buf_wr, ts);
                }

                gen_fill_payload((uint8_t *)buf_wr + (enable_header ? M2SDR_HEADER_BYTES : 0),
                                 payload_bytes,
                                 &gen);
            }
            total_buffers = (uint64_t)dma.reader_sw_count;
        } else {
            void *buf_wr = NULL;
            unsigned num_samples = 0;
            struct m2sdr_metadata meta = {0};

            if (!pace_init) {
                enum m2sdr_transport_kind kind = M2SDR_TRANSPORT_KIND_UNKNOWN;

                pace_init = true;
                pace_tx   = m2sdr_get_transport(dev, &kind) == 0 && kind == M2SDR_TRANSPORT_KIND_LITEETH;
#ifdef CSR_ETH_TX_STATUS_FIFO_LEVEL_ADDR
                /* A gateware with the level of its TX FIFO: the library holds the stream back on
                 * it, at the rate of the board, and the clock of the host is left out. */
                if (pace_tx) {
                    struct m2sdr_liteeth_udp_stats udp_stats;

                    memset(&udp_stats, 0, sizeof(udp_stats));
                    if (m2sdr_liteeth_get_udp_stats(dev, &udp_stats) == 0 && udp_stats.tx_flow_control)
                        pace_tx = false;
                }
#endif
                clock_gettime(CLOCK_MONOTONIC, &pace_start);
            }
            int rc;
            size_t buf_payload_bytes;

            if (!keep_running)
                break;

            rc = m2sdr_get_buffer(dev, M2SDR_TX, &buf_wr, &num_samples, 1000);
            if (rc == M2SDR_ERR_TIMEOUT)
                continue;
            if (rc != 0) {
                fprintf(stderr, "m2sdr_get_buffer failed: %s\n", m2sdr_strerror(rc));
                goto cleanup;
            }

            buf_payload_bytes = m2sdr_samples_to_bytes(format, num_samples);
            gen_fill_payload((uint8_t *)buf_wr, buf_payload_bytes, &gen);

            if (enable_header) {
                uint64_t ts = 0;
                if (m2sdr_get_time(dev, &ts) == 0) {
                    meta.timestamp = ts;
                    meta.flags = M2SDR_META_FLAG_HAS_TIME;
                }
            }

            rc = m2sdr_submit_buffer(dev, M2SDR_TX, buf_wr, num_samples,
                                     enable_header ? &meta : NULL);
            if (rc != 0) {
                fprintf(stderr, "m2sdr_submit_buffer failed: %s\n", m2sdr_strerror(rc));
                goto cleanup;
            }
            total_buffers++;

            /* Ethernet with no flow control: the board drops what its TX FIFO cannot take. The
             * samples are sent at their rate, a few milliseconds ahead, on the clock of the
             * host; the FIFO takes the difference with the clock of the board for minutes. */
            if (pace_tx) {
                struct timespec now;
                double ahead;

                paced_samples += num_samples;
                clock_gettime(CLOCK_MONOTONIC, &now);
                ahead = (double)paced_samples / (2.0 * sample_rate) -
                        ((double)(now.tv_sec - pace_start.tv_sec) + 1e-9 * (double)(now.tv_nsec - pace_start.tv_nsec));
                if (ahead > GEN_PACE_LEAD_S)
                    usleep((useconds_t)((ahead - GEN_PACE_LEAD_S) * 1e6));
            }
        }

        /* Statistics every 200ms */
        int64_t duration = get_time_ms() - last_time;
        if (duration > 200) {
            /* Print banner every 10 lines */
            if (i % 10 == 0)
                printf("\e[1mSPEED(Gbps)   BUFFERS   SIZE(MB)   UNDERFLOWS\e[0m\n");
            i++;
            /* Off PCIe, the underflows are the ones the FPGA counts at the RFIC. */
            if (!use_pcie_dma) {
                struct m2sdr_stream_stats tx_stats;

                memset(&tx_stats, 0, sizeof(tx_stats));
                if (m2sdr_get_stream_stats(dev, M2SDR_TX, &tx_stats) == 0) {
                    sw_underflows       = (int64_t)(tx_stats.underflow_events - fpga_underflows);
                    fpga_underflows     = tx_stats.underflow_events;
                }
            }
            /* Print statistics */
            printf("%10.2f %10" PRIu64 " %10" PRIu64 " %10" PRIu64 "\n",
                   (double)(total_buffers - last_buffers) * M2SDR_BUFFER_BYTES * 8 / ((double)duration * 1e6),
                   total_buffers,
                   (total_buffers * M2SDR_BUFFER_BYTES) / 1024 / 1024,
                   sw_underflows);
            /* Update time/count/underflows */
            last_time = get_time_ms();
            last_buffers = total_buffers;
            sw_underflows = 0;
        }
    }

    /* Wait end of DMA transfer */
    while (use_pcie_dma && dma.reader_hw_count < hw_count_stop) {
        dma.reader_enable = 1;
        litepcie_dma_process(&dma);
    }

    exit_status = 0;

cleanup:
    /* Cleanup DMA and close device */
    if (use_pcie_dma)
        litepcie_dma_cleanup(&dma);
    if (stream_configured)
        (void)m2sdr_stream_release(dev, M2SDR_TX);
    if (ofdm_inited)
        ofdm_cleanup(&ofdm);
#ifdef CSR_GPIO_BASE
    if (dev)
        (void)m2sdr_reg_write(dev, CSR_GPIO_CONTROL_ADDR, 0);
#endif
    if (dev)
        m2sdr_close(dev);
    if (exit_status != 0)
        exit(1);
}

/* Help */
/*------*/

static void help(void) {
    printf("M2SDR Signal Generator Utility\n"
           "usage: m2sdr_gen [options]\n"
           "\n"
           "Options:\n"
           "  -h, --help                     Show this help message.\n"
           "  -d, --device DEV               Use explicit device id.\n"
           "  -c, --device-num N             Use /dev/m2sdrN (default: 0).\n"
           "  -i, --ip ADDR                  Target IP address for Etherbone.\n"
           "      --port PORT                Port number (default: 1234).\n"
           "      --sample-rate HZ           Set sample rate in Hz (default: 30720000).\n"
           "      --signal TYPE              Set signal type: tone, white, prbs, ofdm, or qam.\n"
           "      --tone-freq HZ             Set tone frequency in Hz (default: 1000).\n"
           "      --ofdm-fft-size N          OFDM FFT size (default: 1024).\n"
           "      --ofdm-cp-len N            OFDM cyclic prefix length (default: 32).\n"
           "      --ofdm-carriers N          OFDM occupied carriers (default: 200).\n"
           "      --ofdm-modulation MOD      OFDM modulation: qpsk or 16qam (default: qpsk).\n"
           "      --ofdm-seed N              OFDM PRBS seed (default: 1).\n"
           "      --qam-modulation MOD       qam: bpsk, qpsk, 8psk, 16qam, 32qam, 64qam, 128qam or 256qam\n"
           "                                 (default: qpsk). Random symbols under a root raised cosine.\n"
           "      --qam-symbol-rate HZ       qam: symbols per second (default: sample rate / 16).\n"
           "      --qam-rolloff A            qam: roll-off of the pulse (default: 0.35).\n"
           "      --amplitude A              Set amplitude (0.0 to 1.0, default: 1.0).\n"
           "      --pps-freq HZ              Enable PPS/toggle on GPIO.\n"
           "      --gpio-pin N               Select GPIO pin for PPS (0-3, default: 0).\n"
           "                                 The board has two GPIO pins: 0 (TP1) and 1 (TP2).\n"
           "      --enable-header            Enable TX DMA header.\n"
           "      --format FMT               Sample format: sc16 or sc8 (default: sc16).\n"
           "      --zero-copy                Request zero-copy DMA mode.\n"
           "      --8bit                     Legacy alias for --format sc8.\n"
           );
    exit(1);
}

/* Main */
/*------*/

int main(int argc, char **argv) {
    m2sdr_cli_handle_version(argc, argv);
    int c;
    int option_index = 0;
    static uint8_t m2sdr_device_zero_copy = 0;
    double sample_rate = 30720000.0;
    double frequency = 1000.0;
    double amplitude = 1.0;
    double pps_freq = 0.0; /* Disabled by default */
    uint8_t gpio_pin = 0;  /* Default to GPIO bit 0 */
    uint8_t enable_header = 0;
    uint8_t use_8bit = 0;
    int ofdm_fft_size = 1024;
    int ofdm_cp_len = 32;
    int ofdm_occupied_carriers = 200;
    uint32_t ofdm_seed = 1;
    char signal_type[16] = "tone"; /* Default to tone */
    char ofdm_modulation[16] = "qpsk";
    struct m2sdr_cli_device cli_dev;
    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "device", required_argument, NULL, 'd' },
        { "device-num", required_argument, NULL, 'c' },
        { "ip", required_argument, NULL, 'i' },
        { "port", required_argument, NULL, 8 },
        { "sample-rate", required_argument, NULL, 's' },
        { "signal", required_argument, NULL, 't' },
        { "tone-freq", required_argument, NULL, 'f' },
        { "amplitude", required_argument, NULL, 'a' },
        { "zero-copy", no_argument, NULL, 'z' },
        { "pps-freq", required_argument, NULL, 'p' },
        { "gpio-pin", required_argument, NULL, 'g' },
        { "enable-header", no_argument, NULL, 'H' },
        { "ofdm-fft-size", required_argument, NULL, 3 },
        { "ofdm-cp-len", required_argument, NULL, 4 },
        { "ofdm-carriers", required_argument, NULL, 5 },
        { "ofdm-modulation", required_argument, NULL, 6 },
        { "ofdm-seed", required_argument, NULL, 7 },
        { "qam-modulation", required_argument, NULL, 9 },
        { "qam-symbol-rate", required_argument, NULL, 10 },
        { "qam-rolloff", required_argument, NULL, 11 },
        { "format", required_argument, NULL, 1 },
        { "8bit", no_argument, NULL, 2 },
        { NULL, 0, NULL, 0 }
    };

    signal(SIGINT, intHandler);
    m2sdr_cli_device_init(&cli_dev);

    /* Parameters */
    for (;;) {
        c = getopt_long(argc, argv, "hd:c:i:s:t:f:a:zp:g:H", options, &option_index);
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            help();
            return 0;
        case 'd':
        case 'c':
        case 'i':
            if (m2sdr_cli_handle_device_option(&cli_dev, c, optarg) != 0)
                exit(1);
            break;
        case 's':
            if (m2sdr_cli_parse_double_range(optarg, 1.0, 1.0e15, &sample_rate) != 0) {
                m2sdr_cli_error("invalid sample rate '%s'", optarg);
                exit(1);
            }
            break;
        case 't':
            strncpy(signal_type, optarg, sizeof(signal_type) - 1);
            signal_type[sizeof(signal_type) - 1] = '\0';
            if (strcmp(signal_type, "tone") != 0 && strcmp(signal_type, "white") != 0 && strcmp(signal_type, "prbs") != 0 &&
                strcmp(signal_type, "ofdm") != 0 && strcmp(signal_type, "qam") != 0) {
                m2sdr_cli_invalid_choice("signal", signal_type, "tone, white, prbs, ofdm, or qam");
                exit(1);
            }
            break;
        case 'f':
            if (m2sdr_cli_parse_double_range(optarg, 0.0, 1.0e15, &frequency) != 0) {
                m2sdr_cli_error("invalid tone frequency '%s'", optarg);
                exit(1);
            }
            break;
        case 'a':
            if (m2sdr_cli_parse_double_range(optarg, 0.0, 1.0, &amplitude) != 0) {
                m2sdr_cli_error("invalid amplitude '%s' (expected 0.0 to 1.0)", optarg);
                exit(1);
            }
            break;
        case 'z':
            m2sdr_device_zero_copy = 1;
            break;
        case 'p':
            if (m2sdr_cli_parse_double_range(optarg, 1e-9, 1.0e12, &pps_freq) != 0) {
                m2sdr_cli_error("invalid PPS/toggle frequency '%s'", optarg);
                exit(1);
            }
            break;
        case 'g':
            {
                unsigned pin;

                if (m2sdr_cli_parse_uint_range(optarg, 0, 3, &pin) != 0) {
                    m2sdr_cli_error("invalid GPIO pin '%s' (expected 0 to 3)", optarg);
                    exit(1);
                }
                gpio_pin = (uint8_t)pin;
            }
            break;
        case 1:
            {
                enum m2sdr_format format;

                if (m2sdr_cli_parse_format(optarg, &format) != 0 || format == M2SDR_FORMAT_BFP8_Q11) {
                    m2sdr_cli_invalid_choice("format", optarg, "sc16 or sc8");
                    return 1;
                }
                use_8bit = (format == M2SDR_FORMAT_SC8_Q7);
            }
            break;
        case 2:
            use_8bit = 1;
            break;
        case 'H':
            enable_header = 1;
            break;
        case 3:
            if (m2sdr_cli_parse_int_range(optarg, 1, 65536, &ofdm_fft_size) != 0) {
                m2sdr_cli_error("invalid OFDM FFT size '%s'", optarg);
                return 1;
            }
            break;
        case 4:
            if (m2sdr_cli_parse_int_range(optarg, 0, 65535, &ofdm_cp_len) != 0) {
                m2sdr_cli_error("invalid OFDM CP length '%s'", optarg);
                return 1;
            }
            break;
        case 5:
            if (m2sdr_cli_parse_int_range(optarg, 1, 65535, &ofdm_occupied_carriers) != 0) {
                m2sdr_cli_error("invalid OFDM occupied-carrier count '%s'", optarg);
                return 1;
            }
            break;
        case 6:
            strncpy(ofdm_modulation, optarg, sizeof(ofdm_modulation) - 1);
            ofdm_modulation[sizeof(ofdm_modulation) - 1] = '\0';
            if (strcmp(ofdm_modulation, "qpsk") != 0 && strcmp(ofdm_modulation, "16qam") != 0) {
                m2sdr_cli_invalid_choice("ofdm modulation", ofdm_modulation, "qpsk or 16qam");
                return 1;
            }
            break;
        case 7:
            if (m2sdr_cli_parse_u32(optarg, &ofdm_seed) != 0) {
                m2sdr_cli_error("invalid OFDM seed '%s'", optarg);
                return 1;
            }
            break;
        case 9: {
            int m;

            for (m = M2SDR_MOD_BPSK; m < M2SDR_MOD_COUNT; m++) {
                if (strcasecmp(optarg, m2sdr_modulation_name((enum m2sdr_modulation)m)) == 0)
                    break;
            }
            if (m == M2SDR_MOD_COUNT) {
                m2sdr_cli_invalid_choice("qam-modulation", optarg, "bpsk, qpsk, 8psk, 16qam, 32qam, 64qam, 128qam or 256qam");
                return 1;
            }
            qam_modulation = (enum m2sdr_modulation)m;
            break;
        }
        case 10:
            if (m2sdr_cli_parse_double_range(optarg, 1.0, 1.0e9, &qam_symbol_rate) != 0) {
                m2sdr_cli_error("invalid symbol rate '%s'", optarg);
                return 1;
            }
            break;
        case 11:
            if (m2sdr_cli_parse_double_range(optarg, 0.05, 1.0, &qam_rolloff) != 0) {
                m2sdr_cli_error("invalid roll-off '%s' (expected 0.05 to 1)", optarg);
                return 1;
            }
            break;
        case 8:
            if (m2sdr_cli_handle_device_option(&cli_dev, 'p', optarg) != 0)
                exit(1);
            break;
        default:
            exit(1);
        }
    }

    if (ofdm_fft_size <= 0 || ofdm_cp_len < 0 || ofdm_cp_len >= ofdm_fft_size) {
        fprintf(stderr, "Invalid OFDM parameters: require fft-size > 0 and 0 <= cp-len < fft-size\n");
        return 1;
    }
    if (ofdm_occupied_carriers <= 0 || ofdm_occupied_carriers >= ofdm_fft_size || (ofdm_occupied_carriers & 1)) {
        fprintf(stderr, "Invalid OFDM occupied carriers: require an even value in [2, fft-size-1]\n");
        return 1;
    }

    /* Show help only if no options are provided */
    if (argc == 1) {
        help();
    }

    if (!m2sdr_cli_finalize_device(&cli_dev))
        return 1;

    /* Generate and play tone with optional PPS */
    m2sdr_gen(m2sdr_cli_device_id(&cli_dev), sample_rate, frequency, amplitude, m2sdr_device_zero_copy, pps_freq, gpio_pin, signal_type, enable_header, use_8bit, ofdm_fft_size, ofdm_cp_len, ofdm_occupied_carriers, ofdm_modulation, ofdm_seed);

    return 0;
}
