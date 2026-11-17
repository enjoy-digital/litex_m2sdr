/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: sequences and broadcast channel coding of the LTE downlink
 * (3GPP TS 36.211 and 36.212).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <string.h>

#include "m2sdr_lte_seq.h"

/* Synchronisation signals */
/*-------------------------*/

void m2sdr_lte_pss(int n_id_2, float complex *d)
{
    static const int roots[3] = { 25, 29, 34 };
    const double u = roots[n_id_2 % 3];
    int n;

    /* TS 36.211 6.11.1.1: a Zadoff-Chu sequence of length 63 without its middle element. */
    for (n = 0; n < 31; n++)
        d[n] = (float complex)cexp(-I * M_PI * u * n * (n + 1) / 63.0);
    for (n = 31; n < 62; n++)
        d[n] = (float complex)cexp(-I * M_PI * u * (n + 1) * (n + 2) / 63.0);
}

/* m-sequence of length 31 from its feedback taps, as +1 and -1. */
static void m_sequence(unsigned taps, int *out)
{
    int x[31 + 5], i, t;

    x[0] = x[1] = x[2] = x[3] = 0;
    x[4] = 1;
    for (i = 0; i < 26; i++) {
        int v = 0;
        for (t = 0; t < 5; t++) {
            if (taps & (1u << t))
                v ^= x[i + t];
        }
        x[i + 5] = v;
    }
    for (i = 0; i < 31; i++)
        out[i] = 1 - 2 * x[i];
}

void m2sdr_lte_sss(int n_id_1, int n_id_2, bool subframe5, float *d)
{
    int s[31], c[31], z[31];
    int q_prime, q, m_prime, m0, m1, n;

    /* TS 36.211 6.11.2.1. */
    m_sequence(0x05, s);         /* x(i+5) = x(i+2) + x(i). */
    m_sequence(0x09, c);         /* x(i+5) = x(i+3) + x(i). */
    m_sequence(0x17, z);         /* x(i+5) = x(i+4) + x(i+2) + x(i+1) + x(i). */
    q_prime = n_id_1 / 30;
    q       = (n_id_1 + q_prime * (q_prime + 1) / 2) / 30;
    m_prime = n_id_1 + q * (q + 1) / 2;
    m0      = m_prime % 31;
    m1      = (m0 + m_prime / 31 + 1) % 31;
    for (n = 0; n < 31; n++) {
        const int s0 = s[(n + m0) % 31], s1 = s[(n + m1) % 31];
        const int c0 = c[(n + n_id_2) % 31], c1 = c[(n + n_id_2 + 3) % 31];
        const int z0 = z[(n + (m0 % 8)) % 31], z1 = z[(n + (m1 % 8)) % 31];

        if (!subframe5) {
            d[2 * n]     = (float)(s0 * c0);
            d[2 * n + 1] = (float)(s1 * c1 * z0);
        } else {
            d[2 * n]     = (float)(s1 * c0);
            d[2 * n + 1] = (float)(s0 * c1 * z1);
        }
    }
}

/* Pseudo-random sequences */
/*-------------------------*/

void m2sdr_lte_gold(uint32_t c_init, uint8_t *c, size_t count)
{
    /* TS 36.211 7.2: two m-sequences of length 31, the first 1600 outputs dropped. Bit i of a
     * register is x(n + i). */
    uint32_t x1 = 1, x2 = c_init;
    size_t n;

    for (n = 0; n < 1600 + count; n++) {
        const uint32_t b1 = ((x1 >> 3) ^ x1) & 1u;
        const uint32_t b2 = ((x2 >> 3) ^ (x2 >> 2) ^ (x2 >> 1) ^ x2) & 1u;

        if (n >= 1600)
            c[n - 1600] = (uint8_t)((x1 ^ x2) & 1u);
        x1 = (x1 >> 1) | (b1 << 30);
        x2 = (x2 >> 1) | (b2 << 30);
    }
}

void m2sdr_lte_crs(int cell_id, int ns, int l, float complex *out)
{
    /* TS 36.211 6.10.1.1, normal cyclic prefix: N_CP = 1. */
    const uint32_t c_init = (1u << 10) * (uint32_t)(7 * (ns + 1) + l + 1) * (uint32_t)(2 * cell_id + 1) +
                            2u * (uint32_t)cell_id + 1u;
    uint8_t c[4 * LTE_MAX_RB];
    const float a = (float)M_SQRT1_2;
    int m;

    m2sdr_lte_gold(c_init, c, sizeof(c));
    for (m = 0; m < 2 * LTE_MAX_RB; m++)
        out[m] = a * (float)(1 - 2 * c[2 * m]) + I * a * (float)(1 - 2 * c[2 * m + 1]);
}

/* Broadcast channel */
/*-------------------*/

#define CONV_STREAM  64          /* A stream of 40 bits in its interleaver of 32 columns. */
#define CONV_NULL    0xff

static const uint8_t conv_perm[32] = {
    1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31,
    0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30,
};

/* TS 36.212 5.1.1: CRC of 16 bits, D^16 + D^12 + D^5 + 1. */
static uint16_t crc16(const uint8_t *bits, int count)
{
    uint16_t crc = 0;
    int i;

    for (i = 0; i < count; i++) {
        const int top = ((crc >> 15) & 1) ^ (bits[i] & 1);
        crc = (uint16_t)(crc << 1);
        if (top)
            crc ^= 0x1021;
    }
    return crc;
}

static uint16_t ports_mask(int ports)
{
    return ports == 2 ? 0xffff : ports == 4 ? 0x5555 : 0x0000;
}

/* Outputs of the tail-biting code of TS 36.212 5.1.3.1 for a bit and the six bits before it,
 * the latest in bit 5 of `state` and the oldest in bit 0: generators 133, 171 and 165 in octal. */
static void conv_outputs(int bit, int state, int *out)
{
    const int r = (bit << 6) | state;
    const int c0 = bit, c1 = (r >> 5) & 1, c2 = (r >> 4) & 1, c3 = (r >> 3) & 1;
    const int c4 = (r >> 2) & 1, c5 = (r >> 1) & 1, c6 = r & 1;

    out[0] = c0 ^ c2 ^ c3 ^ c5 ^ c6;
    out[1] = c0 ^ c1 ^ c2 ^ c3 ^ c6;
    out[2] = c0 ^ c1 ^ c2 ^ c4 ^ c6;
}

/* Position in the circular buffer (3 streams of 64, nulls included) of each of the entries
 * that carry a bit, and the stream bit it carries: 120 entries. */
static void rate_match_map(int *stream, int *index)
{
    int s, col, row, n = 0;

    /* TS 36.212 5.1.4.2.1: 24 nulls ahead of the 40 bits, written by rows in 2 rows of 32,
     * columns permuted, read by columns. */
    for (s = 0; s < 3; s++) {
        for (col = 0; col < 32; col++) {
            for (row = 0; row < 2; row++) {
                const int y = row * 32 + conv_perm[col];
                if (y >= CONV_STREAM - LTE_BCH_BITS) {
                    stream[n] = s;
                    index[n]  = y - (CONV_STREAM - LTE_BCH_BITS);
                    n++;
                }
            }
        }
    }
}

void m2sdr_lte_bch_encode(const uint8_t *mib, int ports, int cell_id, uint8_t *bits)
{
    uint8_t a[LTE_BCH_BITS], d[3][LTE_BCH_BITS], c[LTE_PBCH_BITS];
    int stream[3 * LTE_BCH_BITS], index[3 * LTE_BCH_BITS];
    uint16_t crc;
    int i, k, state = 0;

    memcpy(a, mib, LTE_MIB_BITS);
    crc = crc16(mib, LTE_MIB_BITS) ^ ports_mask(ports);
    for (i = 0; i < 16; i++)
        a[LTE_MIB_BITS + i] = (uint8_t)((crc >> (15 - i)) & 1);

    /* Tail biting: the register starts with the last six bits. */
    for (i = 0; i < 6; i++)
        state |= a[LTE_BCH_BITS - 1 - i] << (5 - i);
    for (k = 0; k < LTE_BCH_BITS; k++) {
        int out[3];

        conv_outputs(a[k], state, out);
        d[0][k] = (uint8_t)out[0];
        d[1][k] = (uint8_t)out[1];
        d[2][k] = (uint8_t)out[2];
        state = (state >> 1) | (a[k] << 5);
    }

    /* The circular buffer holds 120 bits: it is read 16 times over. */
    rate_match_map(stream, index);
    m2sdr_lte_gold((uint32_t)cell_id, c, LTE_PBCH_BITS);
    for (k = 0; k < LTE_PBCH_BITS; k++) {
        const int e = k % (3 * LTE_BCH_BITS);
        bits[k] = d[stream[e]][index[e]] ^ c[k];
    }
}

/* Viterbi decoder of the tail-biting code: the block is decoded three times in a row, and the
 * decisions of the middle one are kept, where the start no longer matters. */
static void conv_decode(const float soft[3][LTE_BCH_BITS], uint8_t *bits)
{
    enum { STATES = 64, LEN = 3 * LTE_BCH_BITS };
    static const float big = 1e30f;
    float metric[STATES], next[STATES];
    uint8_t prev[LEN][STATES];
    int k, s, b, best = 0;

    for (s = 0; s < STATES; s++)
        metric[s] = 0;
    for (k = 0; k < LEN; k++) {
        const int at = k % LTE_BCH_BITS;

        for (s = 0; s < STATES; s++)
            next[s] = -big;
        for (s = 0; s < STATES; s++) {
            for (b = 0; b < 2; b++) {
                const int to = (s >> 1) | (b << 5);
                int out[3];
                float m;

                conv_outputs(b, s, out);
                /* A soft bit is positive for a 0. */
                m = metric[s] + (out[0] ? -soft[0][at] : soft[0][at]) + (out[1] ? -soft[1][at] : soft[1][at]) +
                    (out[2] ? -soft[2][at] : soft[2][at]);
                if (m > next[to]) {
                    next[to]    = m;
                    prev[k][to] = (uint8_t)s;
                }
            }
        }
        memcpy(metric, next, sizeof(metric));
    }
    for (s = 1; s < STATES; s++) {
        if (metric[s] > metric[best])
            best = s;
    }
    for (k = LEN - 1; k >= 0; k--) {
        /* The bit that led to a state is its bit 5. */
        if (k >= LTE_BCH_BITS && k < 2 * LTE_BCH_BITS)
            bits[k - LTE_BCH_BITS] = (uint8_t)((best >> 5) & 1);
        best = prev[k][best];
    }
}

int m2sdr_lte_bch_decode(const float *soft, int cell_id, uint8_t *mib, int *ports, int *phase)
{
    static const int port_counts[3] = { 1, 2, 4 };
    int stream[3 * LTE_BCH_BITS], index[3 * LTE_BCH_BITS];
    uint8_t c[LTE_PBCH_BITS], a[LTE_BCH_BITS];
    float d[3][LTE_BCH_BITS];
    int p, k, i;

    rate_match_map(stream, index);
    m2sdr_lte_gold((uint32_t)cell_id, c, LTE_PBCH_BITS);
    for (p = 0; p < 4; p++) {
        uint16_t crc, got = 0;

        /* Descrambling for this position, then the four copies of the buffer added up. */
        memset(d, 0, sizeof(d));
        for (k = 0; k < LTE_PBCH_FRAME; k++) {
            const int e = k % (3 * LTE_BCH_BITS);
            d[stream[e]][index[e]] += c[p * LTE_PBCH_FRAME + k] ? -soft[k] : soft[k];
        }
        conv_decode((const float (*)[LTE_BCH_BITS])d, a);
        crc = crc16(a, LTE_MIB_BITS);
        for (i = 0; i < 16; i++)
            got = (uint16_t)((got << 1) | a[LTE_MIB_BITS + i]);
        for (i = 0; i < 3; i++) {
            if ((crc ^ ports_mask(port_counts[i])) == got) {
                /* All zeros pass the CRC of one port: not a block. */
                int ones = 0;
                for (k = 0; k < LTE_BCH_BITS; k++)
                    ones += a[k];
                if (!ones)
                    continue;
                memcpy(mib, a, LTE_MIB_BITS);
                *ports = port_counts[i];
                *phase = p;
                return 0;
            }
        }
    }
    return -1;
}
