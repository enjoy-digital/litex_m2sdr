/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: Wi-Fi OFDM at 20 MHz (802.11a/g, legacy preamble of 802.11n/ac/ax).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_wireless.h"
#include "m2sdr_fft.h"

/* The samples and the products here are finite: complex products without the NaN and infinity
 * handling of C99 (__mulsc3). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("cx-limited-range")
#endif

#define NFFT               64
#define SYM_LEN            80      /* 16 of guard interval and 64: 4 us. */
#define T1_OFF             192     /* First L-LTF symbol from the start of the L-STF. */
#define SIG_FFT            144     /* FFT part of the L-SIG from the first L-LTF symbol. */
#define PREAMBLE_US        20.0    /* L-STF, L-LTF, L-SIG. */
#define ADV                3       /* FFT windows this far into the guard interval: echoes before the
                                    * strongest path stay inside. */
#define AC_LAG             16      /* Period of the L-STF. */
#define AC_WIN             48
#define MIN_AC             0.6     /* Autocorrelation of an L-STF at 2 dB SNR; noise stays under 0.3. */
#define MIN_PLATEAU        32      /* Samples of it in a row (an L-STF gives 97). */
#define MIN_LTF            0.5     /* Normalised correlation with the L-LTF. */
#define LTF_FROM           114     /* L-LTF searched from there after the middle of the plateau, */
#define LTF_SEARCH         81      /* over this many places: the plateau runs from 40 samples
                                    * before the L-STF to 116 after its start at a high SNR,
                                    * shorter at both ends at a low one. */
#define SLOT               80      /* 4 us slots of the energy airtime. */
#define BUSY_RATIO         4.0     /* 6 dB over the floor. */
#define MAX_SYMBOLS        1366    /* 4095 bytes at 6 Mbps. */
#define DATA_CARRIERS      48
#define HT_QBPSK_RATIO     4.0     /* Power of the Q of a QBPSK symbol over its I. */

/* Tables */
/*--------*/

/* L-LTF, carriers -26 to 26 (0 at 26). */
static const int8_t ltf_seq[53] = {
    1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1,
    0,
    1, -1, -1, 1, 1, -1, 1, -1, 1, -1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, 1, -1, 1, 1, 1, 1,
};

/* L-SIG RATE field (R1 to R4, R1 the MSB here) and what it sets. */
struct wifi_rate {
    uint8_t rbits;
    int     mbps;
    enum m2sdr_modulation mod;
    int     nbpsc, ncbps, ndbps;
    int     punct;              /* 0: 1/2, 2: 2/3, 3: 3/4. */
};

static const struct wifi_rate rates[8] = {
    { 0xd,  6, M2SDR_MOD_BPSK,  1,  48,  24, 0 },
    { 0xf,  9, M2SDR_MOD_BPSK,  1,  48,  36, 3 },
    { 0x5, 12, M2SDR_MOD_QPSK,  2,  96,  48, 0 },
    { 0x7, 18, M2SDR_MOD_QPSK,  2,  96,  72, 3 },
    { 0x9, 24, M2SDR_MOD_16QAM, 4, 192,  96, 0 },
    { 0xb, 36, M2SDR_MOD_16QAM, 4, 192, 144, 3 },
    { 0x1, 48, M2SDR_MOD_64QAM, 6, 288, 192, 2 },
    { 0x3, 54, M2SDR_MOD_64QAM, 6, 288, 216, 3 },
};

/* Carrier of trace index c (0 to 51): -26 to -1, then 1 to 26. */
static int carrier_k(int c)
{
    return c < 26 ? c - 26 : c - 25;
}

static int is_pilot(int k)
{
    return k == -21 || k == -7 || k == 7 || k == 21;
}

/* Pilot polarity p_0 to p_126: the scrambler from all ones, 1 giving -1. */
static void make_polarity(int8_t pol[127])
{
    unsigned s = 0x7f;
    int n;

    for (n = 0; n < 127; n++) {
        const unsigned t = ((s >> 6) ^ (s >> 3)) & 1u;

        pol[n] = t ? -1 : 1;
        s = ((s << 1) | t) & 0x7fu;
    }
}

/* CRC */
/*-----*/

uint32_t m2sdr_wifi_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    int b;

    if (!data)
        return 0;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* HT-SIG CRC-8 (x^8 + x^2 + x + 1, register from all ones, sent inverted): of bits[0..n). */
static unsigned ht_crc8(const uint8_t *bits, int n)
{
    unsigned c = 0xff;
    int i;

    for (i = 0; i < n; i++) {
        const unsigned fb = ((c >> 7) & 1u) ^ bits[i];

        c = (c << 1) & 0xffu;
        if (fb)
            c ^= 0x07u;
    }
    return ~c & 0xffu;
}

/* Coding */
/*--------*/

/* Outputs A (133 octal) and B (171 octal) of the convolutional code for the 7 bits w: the new
 * bit in bit 6, the previous ones below it. */
static int parity7(unsigned v)
{
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (int)(v & 1u);
}

/* Viterbi of the rate 1/2 code, K = 7: soft[2 t] and soft[2 t + 1] the A and B of bit t (> 0
 * for a 1, 0 for a punctured one), the code ending in state 0 (tail). dec holds nbits words.
 *
 * State: the last 6 bits, the newest in bit 5. Old states 2j and 2j + 1 lead to new states j
 * (bit 0) and j + 32 (bit 1); both generators take the new bit, so that a 1 inverts the two
 * outputs of a 0: a butterfly of two branch metrics. */
static void viterbi(const float *soft, int nbits, uint8_t *out, uint64_t *dec)
{
    static uint8_t pair[64];        /* Outputs A, B (bits 1, 0) of a 0 bit from each old state. */
    static bool init;
    float m[64], nm[64];
    int t, s, j, ns;

    if (!init) {
        for (s = 0; s < 64; s++)
            pair[s] = (uint8_t)(parity7((unsigned)s & 0x5bu) << 1 | parity7((unsigned)s & 0x79u));
        init = true;
    }
    for (s = 0; s < 64; s++)
        m[s] = -1e30f;
    m[0] = 0;
    for (t = 0; t < nbits; t++) {
        const float la = soft[2 * t], lb = soft[2 * t + 1];
        const float bm[4] = { -la - lb, -la + lb, la - lb, la + lb };
        uint64_t d = 0;

        for (j = 0; j < 32; j++) {
            const float u0 = bm[pair[2 * j]], u1 = bm[pair[2 * j + 1]];
            const float a0 = m[2 * j] + u0, a1 = m[2 * j + 1] + u1;
            const float b0 = m[2 * j] - u0, b1 = m[2 * j + 1] - u1;

            nm[j]      = a1 > a0 ? a1 : a0;
            nm[j + 32] = b1 > b0 ? b1 : b0;
            d |= (uint64_t)(a1 > a0) << j | (uint64_t)(b1 > b0) << (j + 32);
        }
        dec[t] = d;
        /* Metrics kept small: the best one taken off now and then. */
        if ((t & 63) == 63) {
            float top = nm[0];

            for (s = 1; s < 64; s++)
                top = nm[s] > top ? nm[s] : top;
            for (s = 0; s < 64; s++)
                nm[s] -= top;
        }
        memcpy(m, nm, sizeof(m));
    }
    ns = 0;
    for (t = nbits - 1; t >= 0; t--) {
        out[t] = (uint8_t)(((unsigned)ns >> 5) & 1u);
        ns = (int)((((unsigned)ns & 31u) << 1) | ((dec[t] >> ns) & 1u));
    }
}

/* Coded bit k of a symbol is sent at position j: the two permutations of the interleaver. */
static int interleave(int k, int ncbps, int nbpsc)
{
    const int s = nbpsc / 2 > 1 ? nbpsc / 2 : 1;
    const int i = (ncbps / 16) * (k % 16) + k / 16;

    return s * (i / s) + (i + ncbps - (16 * i) / ncbps) % s;
}

/* Soft bits of a point z (constellation of mean power 1) with weight w, > 0 for a 1: one per
 * bit, max-log, per axis. */
static void demap(float complex z, float w, int nbpsc, float *llr)
{
    float yi = crealf(z), yq = cimagf(z);

    switch (nbpsc) {
    case 1:
        llr[0] = yi * w;
        break;
    case 2:
        llr[0] = yi * w;
        llr[1] = yq * w;
        break;
    case 4:
        yi *= sqrtf(10.0f);
        yq *= sqrtf(10.0f);
        llr[0] = yi * w;
        llr[1] = (2.0f - fabsf(yi)) * w;
        llr[2] = yq * w;
        llr[3] = (2.0f - fabsf(yq)) * w;
        break;
    default:
        yi *= sqrtf(42.0f);
        yq *= sqrtf(42.0f);
        llr[0] = yi * w;
        llr[1] = (4.0f - fabsf(yi)) * w;
        llr[2] = (2.0f - fabsf(fabsf(yi) - 4.0f)) * w;
        llr[3] = yq * w;
        llr[4] = (4.0f - fabsf(yq)) * w;
        llr[5] = (2.0f - fabsf(fabsf(yq) - 4.0f)) * w;
        break;
    }
}

/* Nearest point of the constellation of nbpsc bits per carrier (mean power 1). */
static float complex nearest(float complex z, int nbpsc)
{
    const int levels = nbpsc == 6 ? 8 : nbpsc == 4 ? 4 : 2;
    const float scale = nbpsc == 6 ? sqrtf(42.0f) : nbpsc == 4 ? sqrtf(10.0f) : nbpsc == 2 ? sqrtf(2.0f) : 1.0f;
    float v[2] = { crealf(z) * scale, cimagf(z) * scale };
    int a;

    for (a = 0; a < 2; a++) {
        float q = 2.0f * floorf(v[a] / 2.0f) + 1.0f;

        if (q > levels - 1)
            q = (float)(levels - 1);
        if (q < 1 - levels)
            q = (float)(1 - levels);
        v[a] = q;
    }
    if (nbpsc == 1)
        return v[0];
    return (v[0] + I * v[1]) / scale;
}

/* Channel Estimate */
/*------------------*/

/* The raw estimate of a carrier carries half the noise of a carrier: at a fade, a draw of it
 * under the channel makes the equalised points of that carrier explode. The channel is a short
 * response: the estimate is projected on the responses of a few taps (least squares), which
 * keeps of its noise the diagonal of the projection (under 1 at every carrier). A short model
 * (12 taps from -1: echoes to 350 ns after ADV) leaves a quarter of the noise, two thirds at the
 * edges of the band; when what it leaves out is more than noise, the channel is longer and a
 * long model (22 taps from -2: the guard interval and some) is taken, two fifths of the noise. */
#define SHORT_FIRST        (-1)
#define SHORT_TAPS         12
#define LONG_FIRST         (-2)
#define LONG_TAPS          22
#define MODEL_MARGIN       1.5     /* Residual of the short model over that of noise alone: longer. */

static float complex proj_short[M2SDR_WIFI_CARRIERS][M2SDR_WIFI_CARRIERS];
static float complex proj_long[M2SDR_WIFI_CARRIERS][M2SDR_WIFI_CARRIERS];
static bool proj_init;

/* P = A (A^H A)^-1 A^H for taps first to first + taps - 1. */
static void make_projection(float complex p[M2SDR_WIFI_CARRIERS][M2SDR_WIFI_CARRIERS], int first, int taps)
{
    double complex a[M2SDR_WIFI_CARRIERS][LONG_TAPS], g[LONG_TAPS][2 * LONG_TAPS];
    int c, m, n, r;

    for (c = 0; c < M2SDR_WIFI_CARRIERS; c++)
        for (m = 0; m < taps; m++)
            a[c][m] = cexp(-I * 2.0 * M_PI * carrier_k(c) * (m + first) / NFFT);
    /* (A^H A)^-1 by Gauss-Jordan, next to the identity. */
    for (m = 0; m < taps; m++) {
        for (n = 0; n < taps; n++) {
            double complex v = 0;

            for (c = 0; c < M2SDR_WIFI_CARRIERS; c++)
                v += conj(a[c][m]) * a[c][n];
            g[m][n]        = v;
            g[m][taps + n] = m == n;
        }
    }
    for (m = 0; m < taps; m++) {
        int piv = m;

        for (r = m + 1; r < taps; r++)
            if (cabs(g[r][m]) > cabs(g[piv][m]))
                piv = r;
        for (n = 0; n < 2 * taps; n++) {
            const double complex t = g[m][n];

            g[m][n]   = g[piv][n];
            g[piv][n] = t;
        }
        {
            const double complex d = g[m][m];

            for (n = 0; n < 2 * taps; n++)
                g[m][n] /= d;
        }
        for (r = 0; r < taps; r++) {
            const double complex f = g[r][m];

            if (r == m)
                continue;
            for (n = 0; n < 2 * taps; n++)
                g[r][n] -= f * g[m][n];
        }
    }
    for (c = 0; c < M2SDR_WIFI_CARRIERS; c++) {
        for (r = 0; r < M2SDR_WIFI_CARRIERS; r++) {
            double complex v = 0;

            for (m = 0; m < taps; m++)
                for (n = 0; n < taps; n++)
                    v += a[c][m] * g[m][taps + n] * conj(a[r][n]);
            p[c][r] = (float complex)v;
        }
    }
}

static void project(const float complex p[M2SDR_WIFI_CARRIERS][M2SDR_WIFI_CARRIERS],
                    const float complex *in, float complex *out)
{
    int c, r;

    for (c = 0; c < M2SDR_WIFI_CARRIERS; c++) {
        float complex v = 0;

        for (r = 0; r < M2SDR_WIFI_CARRIERS; r++)
            v += p[c][r] * in[r];
        out[c] = v;
    }
}

/* Channel of the raw estimate h, noise var on each carrier, in place. */
static void channel_fit(float complex h[M2SDR_WIFI_CARRIERS], double var)
{
    float complex fit[M2SDR_WIFI_CARRIERS];
    double res = 0;
    int c;

    project(proj_short, h, fit);
    for (c = 0; c < M2SDR_WIFI_CARRIERS; c++) {
        const float complex d = h[c] - fit[c];

        res += crealf(d * conjf(d));
    }
    /* Noise alone leaves 52 - 12 of its 52 dimensions. */
    if (res > MODEL_MARGIN * (M2SDR_WIFI_CARRIERS - SHORT_TAPS) * var)
        project(proj_long, h, fit);
    memcpy(h, fit, sizeof(fit));
}

/* Packet */
/*--------*/

struct packet_ctx {
    const float complex *x;
    size_t   count;
    size_t   t1;                /* First L-LTF symbol. */
    double   w;                 /* Carrier offset, radians per sample. */
    const struct m2sdr_fft *fft;
    const int8_t *pol;
    float complex h[M2SDR_WIFI_CARRIERS];
    float    weight[M2SDR_WIFI_CARRIERS];   /* |H|^2 / noise: of the soft bits. */
    double   sigma2;            /* Noise of a bin. */
    /* Slope of the phase over the carriers: a sampling offset makes it grow by `drift` each
     * symbol from the L-LTF. Fitted over the symbols of the packet, a constant left out: the
     * pilots have their own error of the channel estimate, the same in every symbol. */
    double   drift;
    /* EVM over the symbols, and per carrier. */
    double   err, ref;
    double   car_err[M2SDR_WIFI_CARRIERS];
    int      car_n[M2SDR_WIFI_CARRIERS];
    float complex *pts;
    int      npts;
};

/* FFT of the 64 samples from a, the carrier offset taken off. Returns false past the capture. */
static bool sym_fft(const struct packet_ctx *pc, size_t a, float complex out[NFFT])
{
    float complex in[NFFT];
    double complex rot, step;
    int i;

    if (a + NFFT > pc->count)
        return false;
    rot  = cexp(-I * pc->w * ((double)a - (double)pc->t1));
    step = cexp(-I * pc->w);
    for (i = 0; i < NFFT; i++) {
        in[i] = pc->x[a + (size_t)i] * (float complex)rot;
        rot  *= step;
    }
    m2sdr_fft_run(pc->fft, in, out);
    return true;
}

/* OFDM symbol i after the L-LTF (0: L-SIG): its 52 carriers equalised, turned by the phase of
 * its pilots and by the slope of the drift over the carriers. raw (may be NULL): the slope its
 * pilots show. Returns false past the capture. */
static bool ofdm_symbol(const struct packet_ctx *pc, int i, float complex z[M2SDR_WIFI_CARRIERS], double *raw)
{
    static const float pilot_base[4] = { 1.0f, 1.0f, 1.0f, -1.0f };   /* Carriers -21, -7, 7, 21. */
    float complex y[NFFT], acc = 0;
    double sk = 0, skk = 0;
    int c, p = 0;

    if (!sym_fft(pc, pc->t1 + SIG_FFT - ADV + (size_t)(i * SYM_LEN), y))
        return false;
    for (c = 0; c < M2SDR_WIFI_CARRIERS; c++)
        z[c] = y[(carrier_k(c) + NFFT) % NFFT] / pc->h[c];
    /* Pilots: common phase, then the slope of what is left over the carriers. */
    for (c = 0; c < M2SDR_WIFI_CARRIERS; c++) {
        if (!is_pilot(carrier_k(c)))
            continue;
        acc += z[c] * pilot_base[p] * pc->pol[i % 127] * pc->weight[c];
        p++;
    }
    for (c = 0, p = 0; c < M2SDR_WIFI_CARRIERS; c++) {
        if (!is_pilot(carrier_k(c)))
            continue;
        sk  += carrier_k(c) * carg(z[c] * pilot_base[p] * pc->pol[i % 127] * conjf(acc)) * pc->weight[c];
        skk += (double)carrier_k(c) * carrier_k(c) * pc->weight[c];
        p++;
    }
    if (raw)
        *raw = skk > 0 ? sk / skk : 0.0;
    {
        const double slope = (i + 1) * pc->drift;
        const float complex step = (float complex)cexp(-I * slope);
        float complex rot = (float complex)cexp(-I * (carg(acc) + slope * carrier_k(0)));

        for (c = 0; c < M2SDR_WIFI_CARRIERS; c++) {
            z[c] *= rot;
            rot  *= c == 25 ? step * step : step;     /* Carrier 0 skipped. */
        }
    }
    return true;
}

/* Drift of the slope over symbols 0 to nsym: least squares of the slopes of their pilots on
 * the symbol, a constant and a step. Fewer than 8 symbols: none (40 ppm make 0.003 radian a
 * carrier over 8 symbols). */
static void fit_drift(struct packet_ctx *pc, int nsym)
{
    float complex z[M2SDR_WIFI_CARRIERS];
    double sx = 0, sy = 0, sxx = 0, sxy = 0, r;
    int i, n = 0;

    pc->drift = 0;
    if (nsym + 1 < 8)
        return;
    for (i = 0; i <= nsym && ofdm_symbol(pc, i, z, &r); i++, n++) {
        sx  += i + 1;
        sy  += r;
        sxx += (double)(i + 1) * (i + 1);
        sxy += (i + 1) * r;
    }
    if (n >= 8)
        pc->drift = (n * sxy - sx * sy) / (n * sxx - sx * sx);
}

/* Data carriers of a symbol, in order, and their weights. */
static void data_carriers(const struct packet_ctx *pc, const float complex z[M2SDR_WIFI_CARRIERS],
                          float complex d[DATA_CARRIERS], float wd[DATA_CARRIERS], int idx[DATA_CARRIERS])
{
    int c, n = 0;

    for (c = 0; c < M2SDR_WIFI_CARRIERS; c++) {
        if (is_pilot(carrier_k(c)))
            continue;
        d[n]   = z[c];
        wd[n]  = pc->weight[c];
        idx[n] = c;
        n++;
    }
}

/* EVM of the data carriers of a symbol against the constellation of nbpsc bits; rot turns them
 * first (QBPSK). */
static void evm_add(struct packet_ctx *pc, const float complex d[DATA_CARRIERS], const int idx[DATA_CARRIERS],
                    int nbpsc, float complex rot)
{
    int n;

    for (n = 0; n < DATA_CARRIERS; n++) {
        const float complex v = d[n] * rot, r = nearest(v, nbpsc);
        const float complex e = v - r;
        const double e2 = crealf(e * conjf(e));

        pc->err += e2;
        pc->ref += crealf(r * conjf(r));
        pc->car_err[idx[n]] += e2;
        pc->car_n[idx[n]]++;
        if (pc->pts && pc->npts < M2SDR_WIFI_MAX_POINTS)
            pc->pts[pc->npts++] = v;
    }
}

/* Bits of BPSK 1/2 symbols (L-SIG, HT-SIG): nsym symbols from symbol i, Q instead of I for
 * QBPSK. Returns 0, or -1 past the capture. */
static int decode_bpsk(struct packet_ctx *pc, int i, int nsym, bool qbpsk, uint8_t *bits)
{
    float soft[2 * 48], coded[48];
    uint64_t dec[48];
    float complex z[M2SDR_WIFI_CARRIERS], d[DATA_CARRIERS];
    float wd[DATA_CARRIERS];
    int idx[DATA_CARRIERS], s, k;

    for (s = 0; s < nsym; s++) {
        if (!ofdm_symbol(pc, i + s, z, NULL))
            return -1;
        data_carriers(pc, z, d, wd, idx);
        for (k = 0; k < DATA_CARRIERS; k++)
            coded[k] = (qbpsk ? cimagf(d[k]) : crealf(d[k])) * wd[k];
        for (k = 0; k < DATA_CARRIERS; k++)
            soft[48 * s + k] = coded[interleave(k, 48, 1)];
        evm_add(pc, d, idx, 1, qbpsk ? -I : 1.0f);
    }
    viterbi(soft, 24 * nsym, bits, dec);
    return 0;
}

/* Q over I power of the data carriers of symbol i: QBPSK reads high. */
static double q_ratio(const struct packet_ctx *pc, int i)
{
    float complex z[M2SDR_WIFI_CARRIERS], d[DATA_CARRIERS];
    float wd[DATA_CARRIERS];
    int idx[DATA_CARRIERS], k;
    double pi = 0, pq = 0;

    if (!ofdm_symbol(pc, i, z, NULL))
        return 0.0;
    data_carriers(pc, z, d, wd, idx);
    for (k = 0; k < DATA_CARRIERS; k++) {
        pi += crealf(d[k]) * crealf(d[k]) * wd[k];
        pq += cimagf(d[k]) * cimagf(d[k]) * wd[k];
    }
    return pq / (pi + 1e-30);
}

static void read_mac(const uint8_t *psdu, int len, struct m2sdr_wifi_packet *pk)
{
    pk->frame_control = (uint16_t)(psdu[0] | psdu[1] << 8);
    pk->type          = (psdu[0] >> 2) & 3;
    pk->subtype       = (psdu[0] >> 4) & 15;
    pk->naddr         = 0;
    if (len >= 10) {
        memcpy(pk->addr1, psdu + 4, 6);
        pk->naddr = 1;
    }
    /* Control frames: RTS (11) and the block acks carry a TA, CTS (12) and ACK (13) do not. */
    if (len >= 16 && !(pk->type == 1 && (pk->subtype == 12 || pk->subtype == 13))) {
        memcpy(pk->addr2, psdu + 10, 6);
        pk->naddr = 2;
    }
    if (len >= 24 && pk->type != 1) {
        memcpy(pk->addr3, psdu + 16, 6);
        pk->naddr = 3;
    }
    /* Beacon, probe response: timestamp, interval, capability, then the elements; SSID is 0. */
    if (pk->type == 0 && (pk->subtype == 8 || pk->subtype == 5)) {
        int a = 36;

        while (a + 2 <= len) {
            const int id = psdu[a], l = psdu[a + 1];

            if (a + 2 + l > len)
                break;
            if (id == 0 && l <= 32) {
                int j;

                memcpy(pk->ssid, psdu + a + 2, (size_t)l);
                pk->ssid[l] = '\0';
                for (j = 0; j < l; j++)
                    if (pk->ssid[j] < 32 || pk->ssid[j] > 126)
                        pk->ssid[j] = '.';
                break;
            }
            a += 2 + l;
        }
    }
}

/* Data of a legacy packet: symbols 1 to nsym after the L-SIG. */
static int decode_psdu(struct packet_ctx *pc, const struct wifi_rate *rt, int nsym, struct m2sdr_wifi_packet *pk)
{
    const int nbits = nsym * rt->ndbps, ncoded = nsym * rt->ncbps;
    float *rx = malloc((size_t)ncoded * sizeof(*rx)), *soft = calloc((size_t)(2 * nbits), sizeof(*soft));
    uint8_t *bits = malloc((size_t)nbits), *psdu = malloc((size_t)pk->length + 1);
    uint64_t *dec = malloc((size_t)nbits * sizeof(*dec));
    float complex z[M2SDR_WIFI_CARRIERS], d[DATA_CARRIERS];
    float wd[DATA_CARRIERS], llr[6], coded[288];
    int idx[DATA_CARRIERS], s, k, b, t, rc = -1;
    uint8_t sc[7];
    uint32_t fcs;

    if (!rx || !soft || !bits || !psdu || !dec)
        goto out;
    for (s = 0; s < nsym; s++) {
        if (!ofdm_symbol(pc, 1 + s, z, NULL))
            goto out;
        data_carriers(pc, z, d, wd, idx);
        for (k = 0; k < DATA_CARRIERS; k++) {
            demap(d[k], wd[k], rt->nbpsc, llr);
            for (b = 0; b < rt->nbpsc; b++)
                coded[k * rt->nbpsc + b] = llr[b];
        }
        for (k = 0; k < rt->ncbps; k++)
            rx[s * rt->ncbps + k] = coded[interleave(k, rt->ncbps, rt->nbpsc)];
        evm_add(pc, d, idx, rt->nbpsc, 1.0f);
    }
    /* Depuncturing: 2/3 sends A0 B0 A1 of A0 B0 A1 B1, 3/4 sends A0 B0 A1 B2 of A0 B0 A1 B1 A2 B2. */
    for (k = 0, t = 0; t < 2 * nbits; t++) {
        const int ph = rt->punct == 3 ? t % 6 : rt->punct == 2 ? t % 4 : 0;
        const bool sent = rt->punct == 3 ? (ph != 3 && ph != 4) : rt->punct == 2 ? ph != 3 : true;

        if (sent)
            soft[t] = rx[k++];
    }
    viterbi(soft, nbits, bits, dec);
    /* Descrambler: the first 7 bits of SERVICE are zeros, what is received of them is the
     * sequence x^7 + x^4 + 1 itself. */
    for (t = 0; t < 7; t++)
        sc[t] = bits[t];
    for (t = 0; t < nbits; t++) {
        const uint8_t v = t < 7 ? sc[t] : (uint8_t)(sc[(t - 7) % 7] ^ sc[(t - 4) % 7]);

        if (t >= 7)
            sc[t % 7] = v;
        bits[t] ^= v;
    }
    for (b = 7; b < 16 && bits[b] == 0; b++)
        ;
    pk->decoded = true;
    if (b < 16)
        goto done;      /* SERVICE reserved bits not zero: wrong decoding. */
    for (k = 0; k < pk->length; k++) {
        uint8_t v = 0;

        for (b = 0; b < 8; b++)
            v |= (uint8_t)(bits[16 + 8 * k + b] << b);
        psdu[k] = v;
    }
    if (pk->length >= 14) {
        fcs = (uint32_t)psdu[pk->length - 4] | (uint32_t)psdu[pk->length - 3] << 8 |
              (uint32_t)psdu[pk->length - 2] << 16 | (uint32_t)psdu[pk->length - 1] << 24;
        pk->fcs_ok = m2sdr_wifi_crc32(psdu, (size_t)pk->length - 4) == fcs;
        if (pk->fcs_ok)
            read_mac(psdu, pk->length - 4, pk);
    }
done:
    rc = 0;
out:
    free(rx);
    free(soft);
    free(bits);
    free(psdu);
    free(dec);
    return rc;
}

/* A packet whose first L-LTF symbol is at t1 and carrier offset w: everything past the timing.
 * Returns its length in samples from the start of its L-STF. */
static size_t analyse_packet(struct packet_ctx *pc, const struct m2sdr_wifi_config *cfg, struct m2sdr_wifi_packet *pk)
{
    float complex y1[NFFT], y2[NFFT];
    uint8_t sig[24], ht[48];
    double hp = 0, np = 0, power = 0;
    const struct wifi_rate *rt = NULL;
    int c, r, k, nsym = 0, parity = 0;
    unsigned rb = 0;
    size_t i, len = (size_t)T1_OFF + 128;

    /* Channel and noise of the two L-LTF symbols. */
    if (!sym_fft(pc, pc->t1 - ADV, y1) || !sym_fft(pc, pc->t1 + NFFT - ADV, y2))
        return len;
    for (c = 0; c < M2SDR_WIFI_CARRIERS; c++) {
        const int b = (carrier_k(c) + NFFT) % NFFT;
        const float complex d = y1[b] - y2[b];

        pc->h[c] = 0.5f * (y1[b] + y2[b]) * (float)ltf_seq[carrier_k(c) + 26];
        hp += crealf(pc->h[c] * conjf(pc->h[c]));
        np += 0.5 * crealf(d * conjf(d));
    }
    hp /= M2SDR_WIFI_CARRIERS;
    pc->sigma2 = np / M2SDR_WIFI_CARRIERS + 1e-30;
    pk->snr_db = 10.0 * log10(fmax(hp - 0.5 * pc->sigma2, 1e-30) / pc->sigma2);
    channel_fit(pc->h, 0.5 * pc->sigma2);
    for (c = 0; c < M2SDR_WIFI_CARRIERS; c++) {
        pc->weight[c] = (float)(crealf(pc->h[c] * conjf(pc->h[c])) / pc->sigma2);
        if (cabsf(pc->h[c]) < 1e-20f)
            pc->h[c] = 1e-20f;
    }
    for (i = pc->t1 - 32; i < pc->t1 + 128; i++)
        power += crealf(pc->x[i] * conjf(pc->x[i]));
    pk->rssi_dbfs     = m2sdr_power_db(power / 160.0);
    pk->freq_error_hz = pc->w / (2.0 * M_PI) * M2SDR_WIFI_SAMPLE_RATE;

    /* L-SIG: RATE (R1 first), reserved, LENGTH (LSB first), even parity, tail. */
    if (decode_bpsk(pc, 0, 1, false, sig) != 0)
        return len;
    for (k = 0; k < 4; k++)
        rb = rb << 1 | sig[k];
    for (k = 0; k < 17; k++)
        parity ^= sig[k];
    for (k = 0; k < 12; k++)
        pk->length |= sig[5 + k] << k;
    for (r = 0; r < 8 && rates[r].rbits != rb; r++)
        ;
    pk->signal_ok = r < 8 && sig[4] == 0 && parity == sig[17] && pk->length > 0;
    for (k = 18; k < 24; k++)
        pk->signal_ok = pk->signal_ok && sig[k] == 0;
    if (!pk->signal_ok)
        return len;
    rt             = &rates[r];
    pk->rate_mbps  = rt->mbps;
    pk->modulation = rt->mod;
    nsym           = (16 + 8 * pk->length + 6 + rt->ndbps - 1) / rt->ndbps;
    pk->duration_s = PREAMBLE_US * 1e-6 + nsym * 4e-6;
    len            = (size_t)(T1_OFF + 128 + SYM_LEN * (1 + nsym));

    /* Format: HT-SIG (QBPSK, two symbols) or VHT-SIG-A (BPSK then QBPSK) after a 6 Mbps L-SIG. */
    if (rt->mbps == 6 && nsym >= 2) {
        const double q1 = q_ratio(pc, 1), q2 = q_ratio(pc, 2);

        if (q1 > HT_QBPSK_RATIO) {
            /* HT-SIG: its points join those of the L-SIG (both BPSK once turned). Not a legacy
             * packet in any case. */
            pk->modulation = M2SDR_MOD_BPSK;
            if (decode_bpsk(pc, 1, 2, true, ht) == 0 && ht_crc8(ht, 34) == ((unsigned)ht[34] << 7 |
                (unsigned)ht[35] << 6 | (unsigned)ht[36] << 5 | (unsigned)ht[37] << 4 | (unsigned)ht[38] << 3 |
                (unsigned)ht[39] << 2 | (unsigned)ht[40] << 1 | (unsigned)ht[41])) {
                pk->format      = M2SDR_WIFI_HT;
                pk->ht_mcs      = 0;
                for (k = 0; k < 7; k++)
                    pk->ht_mcs |= ht[k] << k;
                pk->ht_40mhz    = ht[7] != 0;
                pk->ht_length   = 0;
                for (k = 0; k < 16; k++)
                    pk->ht_length |= ht[8 + k] << k;
                pk->ht_short_gi = ht[24 + 7] != 0;
            }
            return len;
        } else if (q1 < 1.0 / HT_QBPSK_RATIO && q2 > HT_QBPSK_RATIO) {
            pk->format     = M2SDR_WIFI_VHT;
            pk->modulation = M2SDR_MOD_BPSK;
            return len;
        }
    }
    /* The constellation of the data, without the L-SIG when it differs. The L-SIG and the
     * SIG fields above are read without the drift: a few symbols. */
    if (rt->nbpsc != 1)
        pc->npts = 0;
    if (!(cfg && cfg->no_psdu) && nsym <= MAX_SYMBOLS && pc->t1 + SIG_FFT + (size_t)(SYM_LEN * (nsym + 1)) <= pc->count) {
        fit_drift(pc, nsym);
        decode_psdu(pc, rt, nsym, pk);
    }
    return len;
}

/* Analysis */
/*----------*/

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;

    return (x > y) - (x < y);
}

int m2sdr_wifi_analyse(const float complex *x, size_t count, double sample_rate,
                       const struct m2sdr_wifi_config *cfg, struct m2sdr_wifi_result *res)
{
    struct m2sdr_fft *fft = NULL, *ifft = NULL;
    float complex ltf[NFFT], lf[NFFT];
    float complex *pts = NULL;
    int8_t pol[127];
    double *slot = NULL, power = 0, best_rssi = -1e30, busy_end = 0, busy = 0;
    double complex cs = 0;
    double p1 = 0, p2 = 0, ltf_e = 0;
    size_t i, n, nslots, run = 0, run_start = 0, last;
    int k, rc = -1;

    if (!x || !res || fabs(sample_rate - M2SDR_WIFI_SAMPLE_RATE) > 1e-3 * M2SDR_WIFI_SAMPLE_RATE ||
        (double)count < 100e-6 * M2SDR_WIFI_SAMPLE_RATE)
        return -1;
    memset(res, 0, sizeof(*res));
    res->trace_packet = -1;
    make_polarity(pol);
    if (!proj_init) {
        make_projection(proj_short, SHORT_FIRST, SHORT_TAPS);
        make_projection(proj_long, LONG_FIRST, LONG_TAPS);
        proj_init = true;
    }
    fft    = m2sdr_fft_alloc(NFFT, false);
    ifft   = m2sdr_fft_alloc(NFFT, true);
    nslots = count / SLOT;
    slot   = malloc(nslots * sizeof(*slot));
    pts    = malloc(M2SDR_WIFI_MAX_POINTS * sizeof(*pts));
    if (!fft || !ifft || !slot || !pts)
        goto out;

    /* Power, and the energy airtime over 4 us slots. */
    for (n = 0; n < nslots; n++) {
        double e = 0;

        for (i = n * SLOT; i < (n + 1) * SLOT; i++)
            e += crealf(x[i] * conjf(x[i]));
        slot[n] = e / SLOT;
        power  += e;
    }
    res->power_dbfs = m2sdr_power_db(power / (double)(nslots * SLOT));
    {
        double floor_p;
        size_t nb = 0;

        qsort(slot, nslots, sizeof(*slot), cmp_double);
        /* The tenth of slots of noise alone reads 1 - 1.28 / sqrt(80) of its mean (the mean of
         * 80 exponential samples): taken back. */
        floor_p = slot[nslots / 10] / (1.0 - 1.2816 / sqrt((double)SLOT)) + 1e-30;
        for (n = 0; n < nslots; n++)
            nb += slot[n] > BUSY_RATIO * floor_p;
        res->noise_dbfs           = m2sdr_power_db(floor_p);
        res->energy_busy_fraction = (double)nb / (double)nslots;
    }

    /* L-LTF in time, of the scale of the samples: correlation only. */
    for (k = 0; k < NFFT; k++)
        lf[k] = 0;
    for (k = -26; k <= 26; k++)
        lf[(k + NFFT) % NFFT] = (float)ltf_seq[k + 26];
    m2sdr_fft_run(ifft, lf, ltf);
    for (k = 0; k < NFFT; k++)
        ltf_e += crealf(ltf[k] * conjf(ltf[k]));

    /* L-STF: autocorrelation at 16 samples over 48, running. */
    for (i = 0; i < AC_WIN && i + AC_LAG < count; i++) {
        cs += x[i] * conjf(x[i + AC_LAG]);
        p1 += crealf(x[i] * conjf(x[i]));
        p2 += crealf(x[i + AC_LAG] * conjf(x[i + AC_LAG]));
    }
    last = count - AC_WIN - AC_LAG;
    for (n = 0; n < last; n++) {
        const double m = cabs(cs) / (sqrt(p1 * p2) + 1e-30);

        if (m > MIN_AC) {
            if (run++ == 0)
                run_start = n;
        } else {
            if (run >= MIN_PLATEAU) {
                /* A plateau: coarse offset over it, then the L-LTF searched where it should be. */
                struct packet_ctx pc;
                struct m2sdr_wifi_packet pk;
                float complex seg[LTF_SEARCH + 2 * NFFT];
                double complex acc = 0, rot = 1, step;
                double best = 0, w, e;
                size_t t, j, lo = run_start + run / 2 + LTF_FROM, t1 = 0, plen, nseg = 0;

                for (j = run_start; j < run_start + run && j + AC_LAG < count; j++)
                    acc += x[j] * conjf(x[j + AC_LAG]);
                w    = -carg(acc) / AC_LAG;
                step = cexp(-I * w);
                for (j = 0; j < LTF_SEARCH + 2 * NFFT && lo + j < count; j++, nseg++) {
                    seg[j] = x[lo + j] * (float complex)rot;
                    rot   *= step;
                }
                for (t = 0; t + 2 * NFFT <= nseg; t++) {
                    float complex c1 = 0, c2 = 0;
                    float ey = 0;

                    for (j = 0; j < NFFT; j++) {
                        c1 += seg[t + j] * conjf(ltf[j]);
                        c2 += seg[t + j + NFFT] * conjf(ltf[j]);
                        ey += crealf(seg[t + j] * conjf(seg[t + j])) + crealf(seg[t + j + NFFT] * conjf(seg[t + j + NFFT]));
                    }
                    e = (cabsf(c1) + cabsf(c2)) / (2.0 * sqrt(ltf_e * 0.5 * ey) + 1e-30);
                    if (e > best) {
                        best = e;
                        t1   = lo + t;
                    }
                }
                if (best >= MIN_LTF && t1 >= T1_OFF) {
                    double complex f = 0;

                    /* Fine offset: the two L-LTF symbols, 64 samples apart. */
                    for (j = 0; j < NFFT; j++)
                        f += x[t1 + j] * conjf(x[t1 + j + NFFT]) * cexp(I * w * NFFT);
                    w += -carg(f) / NFFT;
                    memset(&pc, 0, sizeof(pc));
                    memset(&pk, 0, sizeof(pk));
                    pc.x     = x;
                    pc.count = count;
                    pc.t1    = t1;
                    pc.w     = w;
                    pc.fft   = fft;
                    pc.pol   = pol;
                    pc.pts   = pts;
                    pk.time_s = (double)(t1 - T1_OFF) / M2SDR_WIFI_SAMPLE_RATE;
                    plen = analyse_packet(&pc, cfg, &pk);
                    pk.evm_db = 10.0 * log10(pc.err / (pc.ref + 1e-30) + 1e-30);
                    if (pk.signal_ok) {
                        res->signal_ok++;
                        if (pk.rssi_dbfs > best_rssi) {
                            double hm = 0;

                            best_rssi              = pk.rssi_dbfs;
                            res->trace_packet      = res->npackets < M2SDR_WIFI_MAX_PACKETS ? res->npackets : -1;
                            res->points_modulation = pk.modulation;
                            res->npoints           = pc.npts;
                            memcpy(res->points, pts, (size_t)pc.npts * sizeof(*pts));
                            for (k = 0; k < M2SDR_WIFI_CARRIERS; k++)
                                hm += crealf(pc.h[k] * conjf(pc.h[k])) / M2SDR_WIFI_CARRIERS;
                            for (k = 0; k < M2SDR_WIFI_CARRIERS; k++) {
                                res->channel_db[k]     = (float)(10.0 * log10(crealf(pc.h[k] * conjf(pc.h[k])) / (hm + 1e-30) + 1e-30));
                                res->evm_carrier_db[k] = pc.car_n[k] ? (float)(10.0 * log10(pc.car_err[k] / pc.car_n[k] + 1e-30)) : 0.0f;
                            }
                        }
                        /* Airtime: the union of the packets. */
                        {
                            const double s0 = fmax(pk.time_s, busy_end), e0 = pk.time_s + pk.duration_s;

                            if (e0 > s0)
                                busy += e0 - s0;
                            if (e0 > busy_end)
                                busy_end = e0;
                        }
                    }
                    if (pk.fcs_ok) {
                        res->fcs_ok++;
                        if (pk.type == 0 && pk.subtype == 8) {
                            int b;

                            for (b = 0; b < res->nbss && memcmp(res->bss[b].bssid, pk.addr3, 6) != 0; b++)
                                ;
                            if (b == res->nbss && b < M2SDR_WIFI_MAX_BSS) {
                                memcpy(res->bss[b].bssid, pk.addr3, 6);
                                res->nbss++;
                            }
                            if (b < res->nbss) {
                                memcpy(res->bss[b].ssid, pk.ssid, sizeof(pk.ssid));
                                res->bss[b].beacons++;
                                res->bss[b].rssi_dbfs = pk.rssi_dbfs;
                                res->bss[b].snr_db    = pk.snr_db;
                            }
                        }
                    }
                    res->packets++;
                    if (res->npackets < M2SDR_WIFI_MAX_PACKETS)
                        res->packet[res->npackets++] = pk;
                    /* Past the packet, its running sums started again. */
                    j = t1 - T1_OFF + plen;
                    if (j > n && j < last) {
                        n  = j;
                        cs = 0;
                        p1 = p2 = 0;
                        for (i = n + 1; i < n + 1 + AC_WIN; i++) {
                            cs += x[i] * conjf(x[i + AC_LAG]);
                            p1 += crealf(x[i] * conjf(x[i]));
                            p2 += crealf(x[i + AC_LAG] * conjf(x[i + AC_LAG]));
                        }
                        run = 0;
                        continue;
                    } else if (j >= last) {
                        break;
                    }
                }
            }
            run = 0;
        }
        /* Window from n + 1. */
        cs += x[n + AC_WIN] * conjf(x[n + AC_WIN + AC_LAG]) - x[n] * conjf(x[n + AC_LAG]);
        p1 += crealf(x[n + AC_WIN] * conjf(x[n + AC_WIN])) - crealf(x[n] * conjf(x[n]));
        p2 += crealf(x[n + AC_WIN + AC_LAG] * conjf(x[n + AC_WIN + AC_LAG])) - crealf(x[n + AC_LAG] * conjf(x[n + AC_LAG]));
        if (p1 < 0)
            p1 = 0;
        if (p2 < 0)
            p2 = 0;
    }
    res->busy_fraction = busy / ((double)count / M2SDR_WIFI_SAMPLE_RATE);
    rc = 0;
out:
    m2sdr_fft_free(fft);
    m2sdr_fft_free(ifft);
    free(slot);
    free(pts);
    return rc;
}
