/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: TETRA continuous downlink (ETSI EN 300 392-2).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_cellular.h"
#include "m2sdr_resample.h"

/* The samples and the products here are finite: complex products without the NaN and infinity
 * handling of C99 (__mulsc3). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("cx-limited-range")
#endif

#define TETRA_R          M2SDR_TETRA_SYMBOL_RATE
#define TETRA_SPS        8                  /* Samples a symbol of the analysis. */
#define TETRA_FS8        (TETRA_SPS * TETRA_R)
#define SLOT_SYM         M2SDR_TETRA_SLOT_SYMBOLS
#define SLOT_SAMPLES     (SLOT_SYM * TETRA_SPS)
#define ROLLOFF          0.35
#define RRC_SPAN         8                  /* Symbols of the matched filter on each side. */
#define LP_CUTOFF_HZ     60e3               /* Before the resampling: flat to 15 kHz, */
#define LP_HALF_S        (2.75 / 90e3)      /* 74 dB down from 105 kHz (what folds onto the channel). */
#define MIN_FS           TETRA_FS8
#define MAX_FS           4e6
#define MIN_SYNC         0.25               /* Training at the slot boundaries, of 1, */
#define MIN_SYNC_STAT    20.0               /* and its power over that of noise (exponential of mean 1). */
#define MIN_NORMAL       0.6                /* Training of a normal burst (11 symbols). */
#define MIN_SB           0.5                /* Frequency correction and training of a sync burst (59 symbols). */
#define NONE_DB          15.0               /* A slot this far under the strongest has no carrier. */
#define PHASE_BLOCK      51                 /* Symbols under one phase. */

/* Burst structure */
/*-----------------*/

/* Training sequences of the downlink bursts (EN 300 392-2 9.4.4.3.2 and 9.4.4.3.4). */
static const uint8_t train_n[22] = { 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0 };
static const uint8_t train_p[22] = { 0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0 };
static const uint8_t train_q[22] = { 1, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1 };
static const uint8_t train_y[38] = {
    1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1,
};
/* Frequency correction field (9.4.4.3.1): 8 ones, 64 zeros, 8 ones. */
static const uint8_t train_f[80] = {
    1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1,
};

/* Symbols of the slot (from 0) of each field. The q sequence spans the boundary: q1 to q10 end
 * a slot (symbols 250 to 254), q11 to q22 start the next one (0 to 5). */
#define SYM_TRAIN_NORMAL 122
#define SYM_FREQ         7
#define SYM_SB           47
#define SYM_Y            107

const uint8_t *m2sdr_tetra_training(int which)
{
    switch (which) {
    case 1: return train_n;
    case 2: return train_p;
    case 3: return train_q;
    case 4: return train_y;
    case 5: return train_f;
    default: return NULL;
    }
}

/* Turn of pi/4-DQPSK of a pair of bits (5.5.2.3): 00 +pi/4, 01 +3pi/4, 10 -pi/4, 11 -3pi/4. */
static float complex dqpsk_turn(int b1, int b2)
{
    static const double turn[4] = { M_PI / 4.0, 3.0 * M_PI / 4.0, -M_PI / 4.0, -3.0 * M_PI / 4.0 };

    return (float complex)cexp(I * turn[(b1 << 1) | b2]);
}

/* Synchronisation block */
/*-----------------------*/

#define BSCH_TYPE1   60
#define BSCH_TYPE2   80                     /* With the CRC and the tail. */
#define BSCH_BITS    120

/* CRC of 16 bits (8.2.3.3): G(X) = X^16 + X^12 + X^5 + 1, preset and complemented. */
static unsigned crc16(const uint8_t *b, int n)
{
    unsigned crc = 0xffffu;
    int i;

    for (i = 0; i < n; i++) {
        const unsigned fb = ((crc >> 15) & 1u) ^ (unsigned)(b[i] & 1);

        crc = (crc << 1) & 0xffffu;
        if (fb)
            crc ^= 0x1021u;
    }
    return crc;
}

/* Mother code of rate 1/4 (8.2.3.1.1): G1 = 1 + D + D^4, G2 = 1 + D^2 + D^3 + D^4,
 * G3 = 1 + D + D^2 + D^4, G4 = 1 + D + D^3 + D^4. state holds b(k-1) in bit 0 to b(k-4). */
static void mother_out(unsigned state, unsigned b, uint8_t *v)
{
    const unsigned d1 = state & 1u, d2 = (state >> 1) & 1u, d3 = (state >> 2) & 1u, d4 = (state >> 3) & 1u;

    v[0] = (uint8_t)(b ^ d1 ^ d4);
    v[1] = (uint8_t)(b ^ d2 ^ d3 ^ d4);
    v[2] = (uint8_t)(b ^ d1 ^ d2 ^ d4);
    v[3] = (uint8_t)(b ^ d1 ^ d3 ^ d4);
}

/* Rate 2/3 puncturing (8.2.3.1.3): of each 8 bits of the mother code, 1, 2 and 5 are kept. */
static int punct_pos(int i)
{
    static const int p[3] = { 0, 1, 4 };

    return 8 * (i / 3) + p[i % 3];
}

/* Scrambling sequence (8.2.5.2) of an extended colour code of zero (the BSCH): p(k) from
 * c(x) = 1 + x + x^2 + x^4 + x^5 + x^7 + x^8 + x^10 + x^11 + x^12 + x^16 + x^22 + x^23 + x^26 +
 * x^32, p(-31) = p(-30) = 1 and the 30 bits of the colour code before. */
static void scramble_seq(uint32_t e, uint8_t *p, int n)
{
    static const int taps[14] = { 1, 2, 4, 5, 7, 8, 10, 11, 12, 16, 22, 23, 26, 32 };
    /* Bit 32 - i of the register is p(k - i). */
    uint32_t reg = (e << 2) | 3u;
    int k, t;

    for (k = 0; k < n; k++) {
        uint32_t bit = 0;

        for (t = 0; t < 14; t++)
            bit ^= (reg >> (32 - taps[t])) & 1u;
        reg  = (reg >> 1) | (bit << 31);
        p[k] = (uint8_t)bit;
    }
}

void m2sdr_tetra_bsch_encode(const uint8_t *pdu, uint8_t *sb)
{
    uint8_t t2[BSCH_TYPE2], v[4 * BSCH_TYPE2], t3[BSCH_BITS], pseq[BSCH_BITS];
    unsigned state = 0, crc;
    int i;

    memset(t2, 0, sizeof(t2));
    for (i = 0; i < BSCH_TYPE1; i++)
        t2[i] = pdu[i] & 1;
    crc = crc16(t2, BSCH_TYPE1) ^ 0xffffu;
    for (i = 0; i < 16; i++)
        t2[BSCH_TYPE1 + i] = (uint8_t)((crc >> (15 - i)) & 1u);
    for (i = 0; i < BSCH_TYPE2; i++) {
        mother_out(state, t2[i], v + 4 * i);
        state = ((state << 1) | t2[i]) & 0xfu;
    }
    for (i = 0; i < BSCH_BITS; i++)
        t3[i] = v[punct_pos(i)];
    /* Block interleaving (8.2.4.1), K = 120, a = 11: b(k) = a(i), k = 1 + (a i mod K). */
    scramble_seq(0, pseq, BSCH_BITS);
    for (i = 1; i <= BSCH_BITS; i++)
        sb[(11 * i) % BSCH_BITS] = t3[i - 1];
    for (i = 0; i < BSCH_BITS; i++)
        sb[i] ^= pseq[i];
}

/* The reverse, from soft bits (positive: 0). Returns 0 with the 60 bits of the PDU when the
 * CRC is right. */
static int bsch_decode(const float *soft, uint8_t *pdu)
{
    float sc[BSCH_BITS], v[4 * BSCH_TYPE2], metric[16], next[16];
    uint8_t pseq[BSCH_BITS], from[BSCH_TYPE2][16], t2[BSCH_TYPE2];
    int i, k, s;

    scramble_seq(0, pseq, BSCH_BITS);
    for (i = 0; i < BSCH_BITS; i++)
        sc[i] = pseq[i] ? -soft[i] : soft[i];
    memset(v, 0, sizeof(v));
    for (i = 1; i <= BSCH_BITS; i++)
        v[punct_pos(i - 1)] = sc[(11 * i) % BSCH_BITS];
    for (s = 0; s < 16; s++)
        metric[s] = s ? -1e30f : 0.0f;
    for (k = 0; k < BSCH_TYPE2; k++) {
        for (s = 0; s < 16; s++)
            next[s] = -1e30f;
        for (s = 0; s < 16; s++) {
            unsigned b;

            if (metric[s] < -1e29f)
                continue;
            for (b = 0; b < 2; b++) {
                const unsigned ns = (((unsigned)s << 1) | b) & 0xfu;
                uint8_t o[4];
                float m = metric[s];
                int j;

                mother_out((unsigned)s, b, o);
                for (j = 0; j < 4; j++)
                    m += o[j] ? -v[4 * k + j] : v[4 * k + j];
                if (m > next[ns]) {
                    next[ns]    = m;
                    from[k][ns] = (uint8_t)s;
                }
            }
        }
        memcpy(metric, next, sizeof(metric));
    }
    s = 0;
    for (k = BSCH_TYPE2 - 1; k >= 0; k--) {
        t2[k] = (uint8_t)(s & 1);
        s     = from[k][s];
    }
    /* The CRC over the PDU and its complemented CRC leaves 0x1d0f. */
    if (crc16(t2, BSCH_TYPE1 + 16) != 0x1d0fu)
        return -1;
    memcpy(pdu, t2, BSCH_TYPE1);
    return 0;
}

static int bits_value(const uint8_t *b, int n)
{
    int v = 0, i;

    for (i = 0; i < n; i++)
        v = (v << 1) | (b[i] & 1);
    return v;
}

/* SYNC PDU of the MAC (21.4.4.2) and D-MLE-SYNC (18.4.2.1). */
static void sync_fields(const uint8_t *b, struct m2sdr_tetra_result *res)
{
    res->system_code         = bits_value(b, 4);
    res->colour_code         = bits_value(b + 4, 6);
    res->tn                  = bits_value(b + 10, 2) + 1;
    res->fn                  = bits_value(b + 12, 5);
    res->mn                  = bits_value(b + 17, 6);
    res->sharing_mode        = bits_value(b + 23, 2);
    res->ts_reserved_frames  = bits_value(b + 25, 3);
    res->u_plane_dtx         = b[28];
    res->frame18_ext         = b[29];
    res->mcc                 = bits_value(b + 31, 10);
    res->mnc                 = bits_value(b + 41, 14);
    res->neighbour_broadcast = bits_value(b + 55, 2);
    res->cell_load           = bits_value(b + 57, 2);
    res->late_entry          = b[59];
}

/* Analysis */
/*----------*/

/* Correlation of the symbol to symbol turns dd (one per symbol, dd[k] for symbol k) with the
 * turns of a sequence of bits from symbol `at`: 0 to 1. */
static double turns_match(const float complex *dd, int at, const uint8_t *bits, int nbits)
{
    double complex acc = 0;
    double mag = 0;
    int j;

    for (j = 0; j < nbits / 2; j++) {
        const float complex v = dd[at + j];

        acc += v * conjf(dqpsk_turn(bits[2 * j], bits[2 * j + 1]));
        mag += cabsf(v);
    }
    return mag > 0 ? cabs(acc) / mag : 0.0;
}

int m2sdr_tetra_analyse(const float complex *x, size_t count, double sample_rate, struct m2sdr_tetra_result *res)
{
    struct m2sdr_kernel lp, rrc;
    float complex *a = NULL, *m = NULL, *dd = NULL;
    float complex q_turn[11];
    double power = 0, step, best = -1, stat = 0, f_coarse = 0, eps, f;
    size_t na, i;
    long tau, best_tau = 0;
    int j, rc = -1;
    double complex best_c = 0;

    if (!x || !res)
        return -1;
    memset(res, 0, sizeof(*res));
    memset(&lp, 0, sizeof(lp));
    memset(&rrc, 0, sizeof(rrc));
    res->sync_slot = -1;
    if (!(sample_rate >= MIN_FS) || sample_rate > MAX_FS || count < (size_t)(3.0 * SLOT_SYM / TETRA_R * sample_rate))
        return -1;
    for (i = 0; i < count; i++)
        power += crealf(x[i] * conjf(x[i]));
    res->power_dbfs = 10.0 * log10(power / (double)count + 1e-30);

    /* 8 samples a symbol, then the matched filter. */
    step = sample_rate / TETRA_FS8;
    na   = (size_t)((double)(count - 1) / step) + 1;
    a    = malloc(na * sizeof(*a));
    m    = malloc(na * sizeof(*m));
    dd   = malloc(na * sizeof(*dd));
    if (!a || !m || !dd || m2sdr_kernel_lowpass(&lp, LP_CUTOFF_HZ / sample_rate, LP_HALF_S * sample_rate) != 0 ||
        m2sdr_kernel_rrc(&rrc, TETRA_SPS, ROLLOFF, RRC_SPAN) != 0)
        goto out;
    m2sdr_kernel_run(&lp, x, count, 0.0, step, a, na);
    m2sdr_kernel_run(&rrc, a, na, 0.0, 1.0, m, na);
    for (i = 0; i < na; i++)
        dd[i] = i >= TETRA_SPS ? m[i] * conjf(m[i - TETRA_SPS]) : 0;

    /* Slot boundaries: the q sequence across them, coherently over every boundary (the turns
     * keep the carrier offset of a symbol only). */
    for (j = 0; j < 11; j++)
        q_turn[j] = conjf(dqpsk_turn(train_q[2 * j], train_q[2 * j + 1]));
    for (tau = 0; tau < SLOT_SAMPLES; tau++) {
        double complex acc = 0;
        double mag = 0, pw = 0;
        long b;

        for (b = tau; b + 6 * TETRA_SPS < (long)na; b += SLOT_SAMPLES) {
            if (b - 5 * TETRA_SPS < TETRA_SPS)
                continue;
            for (j = 0; j < 11; j++) {
                const float complex v = dd[b + (j - 5) * TETRA_SPS];

                acc += v * q_turn[j];
                mag += cabsf(v);
                pw  += crealf(v * conjf(v));
            }
        }
        if (mag > 0 && cabs(acc) / mag > best) {
            best     = cabs(acc) / mag;
            best_tau = tau;
            best_c   = acc;
            stat     = creal(acc * conj(acc)) / pw;
        }
    }
    res->sync_quality = best;
    /* Few boundaries leave a high correlation to noise: its power over that of the terms
     * tells it apart whatever their number. */
    if (best < MIN_SYNC || stat < MIN_SYNC_STAT) {
        rc = 0;
        goto out;
    }
    res->found = true;
    f_coarse   = carg(best_c) / (2.0 * M_PI) * TETRA_R;

    /* The coarse offset taken off before the matched filter, which it would otherwise cut. */
    {
        const double complex step_rot = cexp(-I * 2.0 * M_PI * f_coarse / TETRA_FS8);
        double complex rot = 1.0;

        for (i = 0; i < na; i++) {
            a[i] *= (float complex)rot;
            rot  *= step_rot;
            if ((i & 0xfff) == 0xfff)
                rot /= cabs(rot);
        }
        m2sdr_kernel_run(&rrc, a, na, 0.0, 1.0, m, na);
        for (i = 0; i < na; i++)
            dd[i] = i >= TETRA_SPS ? m[i] * conjf(m[i - TETRA_SPS]) : 0;
    }

    /* Instants of the symbols from the square of the signal (Oerder and Meyr), the one
     * nearest to the boundary found. */
    {
        double complex acc = 0;

        for (i = 0; i < na; i++)
            acc += crealf(m[i] * conjf(m[i])) * cexp(-I * 2.0 * M_PI * (double)(i % TETRA_SPS) / TETRA_SPS);
        eps = -carg(acc) / (2.0 * M_PI) * TETRA_SPS;
        eps = eps + TETRA_SPS * round(((double)best_tau - eps) / TETRA_SPS);
    }

    /* Carrier: the fourth power of the turns at the instants turned by pi/4 has the carrier
     * offset only (4 x the turn of a symbol), unwrapped with the boundaries. */
    {
        double complex acc = 0;
        double fine;
        const double period = TETRA_R / 4.0;

        for (i = 0; ; i++) {
            const double t = eps + (double)i * TETRA_SPS;
            size_t n;
            float complex v, v2;

            if (t >= (double)na - 1)
                break;
            if (t < TETRA_SPS)
                continue;
            n  = (size_t)lround(t);
            v  = dd[n] * (float complex)cexp(-I * M_PI / 4.0);
            v2 = v * v;
            if (cabsf(v) > 0)
                acc += v2 * v2 / (cabsf(v) * cabsf(v) * cabsf(v));
        }
        /* What is left of the offset, within +-R/8 of the coarse one. */
        fine = carg(acc) / (8.0 * M_PI) * TETRA_R;
        f    = fine - period * round(fine / period);
        res->freq_error_hz = f_coarse + f;
    }

    /* Slots. */
    {
        /* The first boundary with the q bits of the slot before it and the filter around. */
        double t0 = eps;
        double best_p = 0, te = 0, tr = 0, peak = 0;
        int s, taken = 0;
        float complex r[SLOT_SYM + 1], dds[SLOT_SYM + 1];
        float plevel[M2SDR_TETRA_MAX_SLOTS], speak[M2SDR_TETRA_MAX_SLOTS];
        double slope[M2SDR_TETRA_MAX_SLOTS], fres = 0;

        while (t0 - 6 * TETRA_SPS - rrc.half < 0)
            t0 += SLOT_SAMPLES;
        while (t0 - SLOT_SAMPLES - 6 * TETRA_SPS - rrc.half >= 0)
            t0 -= SLOT_SAMPLES;
        res->slot_start = t0 * step;
        for (s = 0; s < M2SDR_TETRA_MAX_SLOTS; s++) {
            const double ts = t0 + (double)s * SLOT_SAMPLES;
            double pw = 0, cn, cp, csb;
            int k;

            if (ts + (SLOT_SYM - 1) * TETRA_SPS + rrc.half >= (double)na)
                break;
            /* Symbols -1 to 254, the carrier taken off. */
            for (k = -1; k < SLOT_SYM; k++) {
                const double t = ts + (double)k * TETRA_SPS;

                r[k + 1] = m2sdr_kernel_at(&rrc, a, na, t) * (float complex)cexp(-I * 2.0 * M_PI * f * t / TETRA_FS8);
            }
            for (k = 0; k < SLOT_SYM; k++) {
                dds[k] = r[k + 1] * conjf(r[k]);
                pw    += crealf(r[k + 1] * conjf(r[k + 1]));
            }
            pw /= SLOT_SYM;
            plevel[s] = (float)pw;
            if (pw > best_p)
                best_p = pw;
            res->slot_power_db[s] = (float)(10.0 * log10(pw + 1e-30));
            cn  = turns_match(dds, SYM_TRAIN_NORMAL, train_n, 22);
            cp  = turns_match(dds, SYM_TRAIN_NORMAL, train_p, 22);
            {
                /* Frequency correction and training together. */
                const double c1 = turns_match(dds, SYM_FREQ, train_f, 80), c2 = turns_match(dds, SYM_Y, train_y, 38);

                csb = (40.0 * c1 + 19.0 * c2) / 59.0;
            }
            res->slot_burst[s]    = M2SDR_TETRA_BURST_OTHER;
            res->slot_training[s] = 0;
            if (csb >= MIN_SB && csb >= cn && csb >= cp) {
                res->slot_burst[s] = M2SDR_TETRA_BURST_SYNC;
            } else if (cn >= MIN_NORMAL || cp >= MIN_NORMAL) {
                res->slot_burst[s]    = M2SDR_TETRA_BURST_NORMAL;
                res->slot_training[s] = cn >= cp ? 1 : 2;
            }

            /* MER: the symbols turned back by pi/4 each are on a QPSK, its phase taken every
             * PHASE_BLOCK symbols from its fourth power, its gain from the points decided. */
            {
                double se = 0, sr = 0, sp = 0, th[(SLOT_SYM + PHASE_BLOCK - 1) / PHASE_BLOCK];
                int b0, nb = 0;

                for (b0 = 0; b0 < SLOT_SYM; b0 += PHASE_BLOCK) {
                    const int b1 = b0 + PHASE_BLOCK < SLOT_SYM ? b0 + PHASE_BLOCK : SLOT_SYM;
                    float complex z[PHASE_BLOCK], d[PHASE_BLOCK];
                    double complex acc = 0;
                    double g = 0;
                    float complex back;

                    for (k = b0; k < b1; k++) {
                        float complex z2;

                        z[k - b0] = r[k + 1] * (float complex)cexp(-I * M_PI / 4.0 * (double)(k % 8));
                        z2 = z[k - b0] * z[k - b0];
                        acc += z2 * z2;
                    }
                    th[nb++] = carg(acc) / 4.0;
                    back     = (float complex)cexp(-I * carg(acc) / 4.0);
                    for (k = b0; k < b1; k++) {
                        const float complex v = z[k - b0] * back;

                        /* Nearest of 1, j, -1, -j. */
                        d[k - b0] = fabsf(crealf(v)) >= fabsf(cimagf(v)) ? (crealf(v) >= 0 ? 1.0f : -1.0f) :
                                                                           (cimagf(v) >= 0 ? I : -I);
                        z[k - b0] = v;
                        g += crealf(v * conjf(d[k - b0]));
                    }
                    g /= (b1 - b0);
                    if (!(g > 0))
                        continue;
                    for (k = b0; k < b1; k++) {
                        const float complex e = z[k - b0] / (float)g - d[k - b0];
                        const double e2 = crealf(e * conjf(e));

                        se += e2;
                        sr += 1.0;
                        if (e2 > sp)
                            sp = e2;
                        if (taken < M2SDR_TETRA_MAX_POINTS && ((s * SLOT_SYM + k) % 4) == 0)
                            res->points[taken++] = z[k - b0] / (float)g * (float complex)cexp(I * M_PI / 4.0);
                    }
                }
                res->slot_mer_db[s] = (float)(se > 0 ? 10.0 * log10(sr / se) : 99.0);
                speak[s] = (float)sp;
                /* What is left of the carrier offset: the slope of the phases of the blocks,
                 * unwrapped (a quarter turn), over the slot. */
                {
                    double sx = 0, sy = 0, sxx = 0, sxy = 0, prev = th[0], acc_th = th[0];
                    int q;

                    for (q = 0; q < nb; q++) {
                        double v = th[q];

                        if (q > 0) {
                            double dlt = v - prev;

                            dlt -= M_PI / 2.0 * round(dlt / (M_PI / 2.0));
                            prev    = v;
                            acc_th += dlt;
                        }
                        v    = acc_th;
                        sx  += q;
                        sy  += v;
                        sxx += (double)q * q;
                        sxy += (double)q * v;
                    }
                    slope[s] = nb > 2 ? (nb * sxy - sx * sy) / (nb * sxx - sx * sx) / (2.0 * M_PI * PHASE_BLOCK) * TETRA_R : 0.0;
                }
            }

            /* Synchronisation block: soft bits of the turns. */
            if (res->slot_burst[s] == M2SDR_TETRA_BURST_SYNC) {
                float soft[BSCH_BITS];
                uint8_t pdu[BSCH_TYPE1];
                double norm = 0;

                for (k = 0; k < BSCH_BITS / 2; k++)
                    norm += cabsf(dds[SYM_SB + k]);
                norm = norm > 0 ? norm / (BSCH_BITS / 2) : 1.0;
                for (k = 0; k < BSCH_BITS / 2; k++) {
                    soft[2 * k]     = (float)(cimagf(dds[SYM_SB + k]) / norm);
                    soft[2 * k + 1] = (float)(crealf(dds[SYM_SB + k]) / norm);
                }
                if (bsch_decode(soft, pdu) == 0) {
                    if (!res->sync) {
                        sync_fields(pdu, res);
                        res->sync      = true;
                        res->sync_slot = s;
                    }
                    res->sync_decoded++;
                }
            }
        }
        res->nslots  = s;
        res->npoints = taken;
        /* Slots without a carrier, then the figures over those with one. */
        for (s = 0; s < res->nslots; s++) {
            if (plevel[s] < best_p * pow(10.0, -NONE_DB / 10.0)) {
                res->slot_burst[s]    = M2SDR_TETRA_BURST_NONE;
                res->slot_training[s] = 0;
                continue;
            }
            if (res->slot_burst[s] == M2SDR_TETRA_BURST_NORMAL)
                res->normal_bursts++;
            if (res->slot_burst[s] == M2SDR_TETRA_BURST_SYNC)
                res->sync_bursts++;
            te += pow(10.0, -res->slot_mer_db[s] / 10.0);
            tr += 1.0;
            if (speak[s] > peak)
                peak = speak[s];
            fres += slope[s];
        }
        if (tr > 0)
            res->freq_error_hz += fres / tr;
        res->mer_db   = te > 0 ? 10.0 * log10(tr / te) : 0.0;
        res->evm_rms  = tr > 0 ? sqrt(te / tr) : 0.0;
        res->evm_peak = sqrt(peak);
    }
    rc = 0;
out:
    free(a);
    free(m);
    free(dd);
    m2sdr_kernel_free(&lp);
    m2sdr_kernel_free(&rrc);
    return rc;
}
