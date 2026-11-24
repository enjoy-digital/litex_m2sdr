/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: GSM downlink, frequency correction and synchronisation bursts
 * (3GPP TS 45.002, 45.003, 45.004).
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

#define GSM_R            M2SDR_GSM_SYMBOL_RATE
#define GSM_SPS          4                  /* Samples a bit of the analysis. */
#define GSM_FS4          (GSM_SPS * GSM_R)
#define GSM_CUTOFF_HZ    170e3              /* Receive filter: the GMSK (+-125 kHz at 99 %) passes. */
#define GSM_HALF_S       (2.75 / 60e3)      /* Its window: a transition 60 kHz wide. */
#define GSM_FRAME_BITS   1250               /* TDMA frame: 8 slots of 156.25 bits. */
#define GSM_MIN_FS       0.5e6
#define GSM_MAX_FS       8e6
#define GSM_DEFAULT_MAX_OFFSET 20e3

#define FCCH_WIN         128                /* Bits of the tone summed: it lasts 148. */
#define FCCH_MIN         0.5                /* Coherence of the tone; noise and data read under 0.1. */
/* Coherence of the burst over its 148 bits against itself 6 bits earlier: S / (S + N) for a
 * tone of signal to noise S / N in the receive band, 0.89 at 9 dB, 0.67 at 3 dB (where the SCH
 * no longer decodes). Other signals pass the test of the runs a few bits at a time: an OFDM or
 * a WCDMA carrier whose spectrum is not even in the band, around 0.1 and under 0.5 on air. */
#define FCCH_MIN_BURST   0.6
#define FCCH_MIN_RUN     10                 /* Bits over the threshold: a burst gives about 150, */
#define FCCH_MAX_RUN     300                /* a steady tone the whole capture. */
#define FCCH_LAG         24                 /* Samples of the fine estimate: 6 bits. */
#define FCCH_GAP         16                 /* Bits under the threshold bridged in a run. */
#define SCH_SEARCH       8                  /* Bits searched on each side of the place of the SCH. */
#define SCH_MIN_MATCH    0.5                /* Training of the SCH found, of 1. */
#define TRAIN_FIRST      42                 /* Extended training sequence: bits 42 to 105. */
#define TRAIN_BITS       64
#define MEAS_FIRST       3                  /* Bits of the burst measured: the tails left out, */
#define MEAS_END         145                /* which also take the edges of the receive filter. */
#define REF_EXTRA        12                 /* Bits modelled on each side of the burst for the filter. */
#define MAX_FCCH         64

/* Synchronisation burst */
/*-----------------------*/

/* Extended training sequence of the SCH (TS 45.002 5.2.5): 0xB962040F2D45761B. */
static const uint8_t sch_train[TRAIN_BITS] = {
    1, 0, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
    0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1,
};

/* Parity of the SCH (TS 45.003 4.7): g(D) = D^10 + D^8 + D^6 + D^5 + D^4 + D^2 + 1, the
 * remainder inverted. p[0] goes with D^9. */
static void sch_parity(const uint8_t *d, uint8_t *p)
{
    unsigned reg = 0;
    int i;

    for (i = 0; i < 25; i++) {
        const unsigned fb = (unsigned)(d[i] & 1) ^ ((reg >> 9) & 1u);

        reg = (reg << 1) & 0x3ffu;
        if (fb)
            reg ^= 0x175u;
    }
    for (i = 0; i < 10; i++)
        p[i] = (uint8_t)(((reg >> (9 - i)) & 1u) ^ 1u);
}

/* Convolutional code of rate 1/2 (TS 45.003 4.1.3): G0 = 1 + D^3 + D^4, G1 = 1 + D + D^3 + D^4.
 * State: the last four inputs, u(k-1) in bit 0. */
static void conv_out(unsigned state, unsigned u, uint8_t *c0, uint8_t *c1)
{
    const unsigned u1 = state & 1u, u3 = (state >> 2) & 1u, u4 = (state >> 3) & 1u;

    *c0 = (uint8_t)(u ^ u3 ^ u4);
    *c1 = (uint8_t)(u ^ u1 ^ u3 ^ u4);
}

/* 25 information bits of the SCH (TS 44.018 9.1.30, sent bit 1 of each octet first). */
static void sch_info(int bsic, int t1, int t2, int t3p, uint8_t *d)
{
    int i;

    d[0] = (uint8_t)((t1 >> 9) & 1);
    d[1] = (uint8_t)((t1 >> 10) & 1);
    for (i = 0; i < 6; i++)
        d[2 + i] = (uint8_t)((bsic >> i) & 1);      /* BCC, then NCC, low bit first. */
    for (i = 0; i < 8; i++)
        d[8 + i] = (uint8_t)((t1 >> (1 + i)) & 1);
    d[16] = (uint8_t)((t3p >> 1) & 1);
    d[17] = (uint8_t)((t3p >> 2) & 1);
    for (i = 0; i < 5; i++)
        d[18 + i] = (uint8_t)((t2 >> i) & 1);
    d[23] = (uint8_t)(t1 & 1);
    d[24] = (uint8_t)(t3p & 1);
}

void m2sdr_gsm_sch_encode(int bsic, int fn, uint8_t *coded)
{
    uint8_t u[39];
    unsigned state = 0;
    const int t3 = fn % 51;
    int k;

    memset(u, 0, sizeof(u));
    sch_info(bsic & 0x3f, (fn / (26 * 51)) & 0x7ff, fn % 26, (t3 - 1) / 10, u);
    sch_parity(u, u + 25);
    for (k = 0; k < 39; k++) {
        conv_out(state, u[k], &coded[2 * k], &coded[2 * k + 1]);
        state = ((state << 1) | u[k]) & 0xfu;
    }
}

void m2sdr_gsm_sch_burst(const uint8_t *coded, uint8_t *bits)
{
    memset(bits, 0, M2SDR_GSM_BURST_BITS);
    memcpy(bits + 3, coded, 39);
    memcpy(bits + TRAIN_FIRST, sch_train, TRAIN_BITS);
    memcpy(bits + 106, coded + 39, 39);
}

/* Viterbi decoding of the 78 soft bits (positive: 0), the code ending in state 0. Returns 0
 * with the BSIC and the frame number when the parity is right, -1 otherwise. */
static int sch_decode(const float *soft, int *bsic, int *t1, int *t2, int *t3p)
{
    float metric[16], next[16];
    uint8_t from[39][16];
    uint8_t u[39], p[10];
    int k, s, i;

    for (s = 0; s < 16; s++)
        metric[s] = s ? -1e30f : 0.0f;
    for (k = 0; k < 39; k++) {
        for (s = 0; s < 16; s++)
            next[s] = -1e30f;
        for (s = 0; s < 16; s++) {
            unsigned b;

            if (metric[s] < -1e29f)
                continue;
            for (b = 0; b < 2; b++) {
                const unsigned ns = (((unsigned)s << 1) | b) & 0xfu;
                uint8_t c0, c1;
                float m;

                conv_out((unsigned)s, b, &c0, &c1);
                m = metric[s] + (c0 ? -soft[2 * k] : soft[2 * k]) + (c1 ? -soft[2 * k + 1] : soft[2 * k + 1]);
                if (m > next[ns]) {
                    next[ns]    = m;
                    from[k][ns] = (uint8_t)s;
                }
            }
        }
        memcpy(metric, next, sizeof(metric));
    }
    /* Back from state 0: the input of each step is the low bit of the state it led to. */
    s = 0;
    for (k = 38; k >= 0; k--) {
        u[k] = (uint8_t)(s & 1);
        s    = from[k][s];
    }
    sch_parity(u, p);
    for (i = 0; i < 10; i++)
        if (p[i] != u[25 + i])
            return -1;
    *bsic = 0;
    for (i = 0; i < 6; i++)
        *bsic |= u[2 + i] << i;
    *t1 = (u[1] << 10) | (u[0] << 9) | u[23];
    for (i = 0; i < 8; i++)
        *t1 |= u[8 + i] << (1 + i);
    *t2 = 0;
    for (i = 0; i < 5; i++)
        *t2 |= u[18 + i] << i;
    *t3p = (u[17] << 2) | (u[16] << 1) | u[24];
    return 0;
}

/* GMSK */
/*------*/

/* Phase of the GMSK (BT 0.3, TS 45.004) over one bit: the integral of its frequency pulse, from
 * 0 to 1, u in bits from the middle of the bit. */
static double gmsk_q(double u)
{
    /* Gaussian of standard deviation sqrt(ln 2) / (2 pi BT) bits over a rectangle of a bit:
     * the integral of the Gaussian CDF over a bit, z Phi(z) + phi(z) at both ends. */
    const double s = sqrt(log(2.0)) / (2.0 * M_PI * 0.3);
    const double z1 = (u - 0.5) / s, z2 = (u + 0.5) / s;
    const double f1 = z1 * 0.5 * erfc(-z1 / M_SQRT2) + exp(-0.5 * z1 * z1) / sqrt(2.0 * M_PI);
    const double f2 = z2 * 0.5 * erfc(-z2 / M_SQRT2) + exp(-0.5 * z2 * z2) / sqrt(2.0 * M_PI);

    if (u < -4.0)
        return 0.0;
    if (u > 4.0)
        return 1.0;
    return s * (f2 - f1);
}

/* Ideal burst at GSM_SPS samples a bit, sample 0 on the middle of bit 0, n samples: the bits d
 * (nbits of them, differentially encoded here: a = 1 - 2 (d_i ^ d_i-1)). */
static void gmsk_wave(const uint8_t *d, int nbits, float complex *r, int n)
{
    int m, i;

    for (m = 0; m < n; m++) {
        const double u = (double)m / GSM_SPS;
        double phase = 0;
        const int i0 = (int)floor(u) - 4 < 0 ? 0 : (int)floor(u) - 4;
        const int i1 = (int)floor(u) + 5 > nbits ? nbits : (int)floor(u) + 5;

        /* Bits before i0 have turned the phase by their whole quarter turn. */
        for (i = 0; i < i0; i++)
            phase += (d[i] ^ (i ? d[i - 1] : 0)) ? -1.0 : 1.0;
        for (i = i0; i < i1; i++)
            phase += ((d[i] ^ (i ? d[i - 1] : 0)) ? -1.0 : 1.0) * gmsk_q(u - i);
        r[m] = (float complex)cexp(I * M_PI / 2.0 * phase);
    }
}

/* Analysis */
/*----------*/

static double wrap_pi(double a)
{
    while (a > M_PI)
        a -= 2.0 * M_PI;
    while (a < -M_PI)
        a += 2.0 * M_PI;
    return a;
}

/* A quarter turn back for each bit. */
static const float complex quarter_back[4] = { 1.0f, -I, -1.0f, I };

struct gsm_ctx {
    const float complex *x;
    size_t count;
    double step;                 /* Input samples per sample of the analysis. */
    struct m2sdr_kernel lp;      /* Receive filter at the input rate, */
    struct m2sdr_kernel lp4;     /* and the same at GSM_FS4. */
};

/* Received signal at instant t (samples of the analysis), the carrier f (Hz) taken off. */
static float complex rx_at(const struct gsm_ctx *c, double t, double f)
{
    return m2sdr_kernel_at(&c->lp, c->x, c->count, t * c->step) * (float complex)cexp(-I * 2.0 * M_PI * f * t / GSM_FS4);
}

/* One SCH whose bit 0 is near sample t0 of y (the analysis rate). Returns 0 with it in *sch,
 * -1 when its training is not found. */
static int sch_measure(const struct gsm_ctx *c, const float complex *y, size_t ny, double t0, double f,
                       struct m2sdr_gsm_sch *sch, float *perr, float complex *pts, int *npts)
{
    const int nref = (M2SDR_GSM_BURST_BITS + 2 * REF_EXTRA) * GSM_SPS;
    float complex z[M2SDR_GSM_BURST_BITS], h = 0;
    float soft[78];
    uint8_t coded[78], bits[M2SDR_GSM_BURST_BITS], ext[M2SDR_GSM_BURST_BITS + 2 * REF_EXTRA];
    float complex *ref = malloc((size_t)nref * sizeof(*ref)), *rf = malloc((size_t)nref * sizeof(*rf));
    float complex *yb = malloc((size_t)nref * sizeof(*yb));
    double best = -1, cm1 = 0, cp1 = 0, tb;
    long tau, best_tau = 0;
    int k, m, bsic, t1, t2, t3p, rc = -1;

    if (!ref || !rf || !yb)
        goto out;

    /* Training: the samples at the end of each bit (k + 1/2), turned back by a quarter turn a
     * bit, are +-1 times the channel (main Laurent pulse of the GMSK). */
    for (tau = (long)floor(t0) - SCH_SEARCH * GSM_SPS; tau <= (long)floor(t0) + SCH_SEARCH * GSM_SPS; tau++) {
        double complex acc = 0;
        double mag = 0;

        if (tau < 0 || (size_t)tau + (size_t)(M2SDR_GSM_BURST_BITS * GSM_SPS) >= ny)
            continue;
        for (k = TRAIN_FIRST; k < TRAIN_FIRST + TRAIN_BITS; k++) {
            const size_t n = (size_t)tau + (size_t)(GSM_SPS * k + GSM_SPS / 2);
            const float complex v = y[n] * (float complex)cexp(-I * 2.0 * M_PI * f * (double)n / GSM_FS4) * quarter_back[k & 3];

            acc += sch_train[k - TRAIN_FIRST] ? -v : v;
            mag += cabsf(v);
        }
        if (mag > 0 && cabs(acc) / mag > best) {
            best     = cabs(acc) / mag;
            best_tau = tau;
        }
    }
    if (best < SCH_MIN_MATCH)
        goto out;
    /* Between the samples: a parabola through the correlation around its peak. */
    {
        double v[3];
        int d;

        for (d = -1; d <= 1; d++) {
            double complex acc = 0;

            for (k = TRAIN_FIRST; k < TRAIN_FIRST + TRAIN_BITS; k++) {
                    const double t = (double)(best_tau + d) + GSM_SPS * k + GSM_SPS / 2;
                const float complex s = rx_at(c, t, f) * quarter_back[k & 3];

                acc += sch_train[k - TRAIN_FIRST] ? -s : s;
            }
            v[d + 1] = cabs(acc);
        }
        cm1 = v[0] - v[1];
        cp1 = v[2] - v[1];
        tb  = (double)best_tau + ((cm1 + cp1) < 0 ? fmax(-1.0, fmin(1.0, 0.5 * (cm1 - cp1) / (cm1 + cp1))) : 0.0);
    }

    /* Bits: the channel from the training, then each bit from its real part. */
    for (k = 0; k < M2SDR_GSM_BURST_BITS; k++) {
        z[k] = rx_at(c, tb + GSM_SPS * k + GSM_SPS / 2, f) * quarter_back[k & 3];
    }
    for (k = TRAIN_FIRST; k < TRAIN_FIRST + TRAIN_BITS; k++)
        h += sch_train[k - TRAIN_FIRST] ? -z[k] : z[k];
    h /= TRAIN_BITS;
    if (!(cabsf(h) > 0))
        goto out;
    for (k = 0; k < M2SDR_GSM_BURST_BITS; k++) {
        const float s = crealf(z[k] * conjf(h)) / crealf(h * conjf(h));

        bits[k] = s < 0;
        if (k >= 3 && k < 42)
            soft[k - 3] = s;
        else if (k >= 106 && k < 145)
            soft[k - 106 + 39] = s;
        if (pts && *npts < M2SDR_GSM_MAX_POINTS && k >= MEAS_FIRST && k < MEAS_END)
            pts[(*npts)++] = z[k] * conjf(h) / cabsf(h) / cabsf(h);
    }
    memset(sch, 0, sizeof(*sch));
    sch->start = tb * c->step;
    sch->bsic  = sch->ncc = sch->bcc = sch->fn = -1;
    if (sch_decode(soft, &bsic, &t1, &t2, &t3p) == 0 && t3p <= 4) {
        sch->crc  = true;
        sch->bsic = bsic;
        sch->ncc  = bsic >> 3;
        sch->bcc  = bsic & 7;
        sch->t1   = t1;
        sch->t2   = t2;
        sch->t3   = 10 * t3p + 1;
        /* TS 45.002 3.3.2.2. */
        sch->fn   = 51 * (((sch->t3 - t2) % 26 + 26) % 26) + sch->t3 + 51 * 26 * t1;
        m2sdr_gsm_sch_encode(bsic, sch->fn, coded);
        m2sdr_gsm_sch_burst(coded, bits);
    } else {
        /* Not decoded: the bits decided, with the known ones. */
        memset(bits, 0, 3);
        memset(bits + 145, 0, 3);
        memcpy(bits + TRAIN_FIRST, sch_train, TRAIN_BITS);
    }

    /* The ideal burst through the same receive filter, the bits around it taken as tail bits,
     * against the received one: the phase error after a line (frequency and phase) is fitted. */
    memset(ext, 0, sizeof(ext));
    memcpy(ext + REF_EXTRA, bits, M2SDR_GSM_BURST_BITS);
    gmsk_wave(ext, M2SDR_GSM_BURST_BITS + 2 * REF_EXTRA, ref, nref);
    for (m = 0; m < nref; m++) {
        rf[m] = m2sdr_kernel_at(&c->lp4, ref, (size_t)nref, (double)m);
        yb[m] = rx_at(c, tb + (double)(m - REF_EXTRA * GSM_SPS), f);
    }
    {
        const int m0 = (REF_EXTRA + MEAS_FIRST) * GSM_SPS, m1 = (REF_EXTRA + MEAS_END) * GSM_SPS;
        double sx = 0, sy = 0, sxx = 0, sxy = 0, a, b, prev = 0, unwrap = 0, e2 = 0, r2 = 0, p2 = 0, peak = 0, power = 0;
        double complex g = 0;
        double *psi = malloc((size_t)(m1 - m0) * sizeof(*psi));
        const int nm = m1 - m0;

        if (!psi)
            goto out;
        for (m = m0; m < m1; m++) {
            const double v = carg(yb[m] * conjf(rf[m]));

            if (m > m0)
                unwrap += wrap_pi(v - prev);
            else
                unwrap = v;
            prev = v;
            psi[m - m0] = unwrap;
            sx  += m;
            sy  += unwrap;
            sxx += (double)m * m;
            sxy += (double)m * unwrap;
            power += crealf(yb[m] * conjf(yb[m]));
        }
        b = (nm * sxy - sx * sy) / (nm * sxx - sx * sx);
        a = (sy - b * sx) / nm;
        for (m = m0; m < m1; m++) {
            const double e = psi[m - m0] - (a + b * m);

            p2  += e * e;
            if (fabs(e) > peak)
                peak = fabs(e);
            g   += yb[m] * cexp(-I * (a + b * m)) * conjf(rf[m]);
            r2  += crealf(rf[m] * conjf(rf[m]));
        }
        g /= r2;
        for (m = m0; m < m1; m++) {
            const double complex e = yb[m] * cexp(-I * (a + b * m)) - g * rf[m];

            e2 += creal(e * conj(e));
        }
        for (k = 0; k < M2SDR_GSM_BURST_BITS && perr; k++) {
            const int mm = (REF_EXTRA + k) * GSM_SPS;

            perr[k] = mm >= m0 && mm < m1 ? (float)((psi[mm - m0] - (a + b * mm)) * 180.0 / M_PI) : 0.0f;
        }
        free(psi);
        sch->phase_err_rms_deg  = sqrt(p2 / nm) * 180.0 / M_PI;
        sch->phase_err_peak_deg = peak * 180.0 / M_PI;
        sch->evm                = sqrt(e2 / (creal(g * conj(g)) * r2));
        sch->power_db           = 10.0 * log10(power / nm + 1e-30);
        sch->freq_hz            = f + b * GSM_FS4 / (2.0 * M_PI);
    }
    rc = 0;
out:
    free(ref);
    free(rf);
    free(yb);
    return rc;
}

/* FCCH of a run of tone windows. The tone, its quarter turn a bit taken off, against itself
 * 6 bits earlier is constant over the burst, whatever the carrier offset: its sum over the
 * 148 bits gives the timing (the bits around do not add up), and its phase the frequency (the
 * filtered noise of samples one bit apart is correlated, which pulls the coarse estimate of the
 * runs; 6 bits apart it is not), unwrapped with the coarse estimate. */
static int fcch_refine(const float complex *y, size_t ny, size_t run0, size_t run, double coarse, double *t, double *f,
                       double *quality)
{
    const size_t len = M2SDR_GSM_BURST_BITS * GSM_SPS;
    const size_t lo = run0 > len + FCCH_LAG ? run0 - len : FCCH_LAG;
    const size_t hi = run0 + run + 32 * GSM_SPS + len < ny ? run0 + run + 32 * GSM_SPS : (ny > len ? ny - len : 0);
    const float complex back = (float complex)cexp(-I * M_PI / 2.0 * (double)FCCH_LAG / GSM_SPS);
    double complex *ps, best_v = 0;
    double *pa, best = -1, fine;
    size_t n, at = lo;

    if (hi <= lo)
        return -1;
    ps = malloc((hi + len - lo + 1) * sizeof(*ps));
    pa = malloc((hi + len - lo + 1) * sizeof(*pa));
    if (!ps || !pa) {
        free(ps);
        free(pa);
        return -1;
    }
    ps[0] = 0;
    pa[0] = 0;
    for (n = lo; n < hi + len; n++) {
        const float complex v = y[n] * conjf(y[n - FCCH_LAG]) * back;

        ps[n - lo + 1] = ps[n - lo] + v;
        pa[n - lo + 1] = pa[n - lo] + cabsf(v);
    }
    /* The products of the window [n, n + len) whose both samples are in it. */
    for (n = lo; n <= hi; n++) {
        const double complex v = ps[n - lo + len] - ps[n - lo + FCCH_LAG];

        if (cabs(v) > best) {
            best   = cabs(v);
            best_v = v;
            at     = n;
        }
    }
    *quality = pa[at - lo + len] - pa[at - lo + FCCH_LAG] > 0 ? best / (pa[at - lo + len] - pa[at - lo + FCCH_LAG]) : 0.0;
    free(ps);
    free(pa);
    /* The window holds bits 0 to 147 from half a bit before the middle of bit 0. */
    *t    = (double)at + GSM_SPS / 2.0;
    fine  = carg(best_v) / (2.0 * M_PI) * GSM_FS4 / FCCH_LAG;
    fine += GSM_FS4 / FCCH_LAG * round((coarse - fine) / (GSM_FS4 / FCCH_LAG));
    *f    = fine;
    return 0;
}

int m2sdr_gsm_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_gsm_config *cfg,
                      struct m2sdr_gsm_result *res)
{
    struct gsm_ctx c;
    float complex *y = NULL;
    double complex *cs = NULL;
    double *ca = NULL;
    double fcch_t[MAX_FCCH], fcch_f[MAX_FCCH], fcch_w[MAX_FCCH];
    const double max_off = cfg && cfg->max_offset_hz > 0 ? cfg->max_offset_hz : GSM_DEFAULT_MAX_OFFSET;
    double power = 0, f0 = 0, wsum = 0;
    size_t ny, i;
    int nf = 0, s, rc = -1, decoded = 0, used = 0;

    if (!x || !res)
        return -1;
    memset(res, 0, sizeof(*res));
    memset(&c, 0, sizeof(c));
    res->bsic = res->ncc = res->bcc = res->fn = -1;
    if (!(sample_rate >= GSM_MIN_FS) || sample_rate > GSM_MAX_FS ||
        count < (size_t)(2.0 * GSM_FRAME_BITS / GSM_R * sample_rate))
        return -1;
    for (i = 0; i < count; i++)
        power += crealf(x[i] * conjf(x[i]));
    res->power_dbfs = 10.0 * log10(power / (double)count + 1e-30);

    /* Receive filter and 4 samples a bit. */
    c.x     = x;
    c.count = count;
    c.step  = sample_rate / GSM_FS4;
    ny      = (size_t)((double)(count - 1) / c.step) + 1;
    y       = malloc(ny * sizeof(*y));
    cs      = malloc((ny + 1) * sizeof(*cs));
    ca      = malloc((ny + 1) * sizeof(*ca));
    if (!y || !cs || !ca ||
        m2sdr_kernel_lowpass(&c.lp, GSM_CUTOFF_HZ / sample_rate, GSM_HALF_S * sample_rate) != 0 ||
        m2sdr_kernel_lowpass(&c.lp4, GSM_CUTOFF_HZ / GSM_FS4, GSM_HALF_S * GSM_FS4) != 0)
        goto out;
    m2sdr_kernel_run(&c.lp, x, count, 0.0, c.step, y, ny);

    /* FCCH: all its bits turn the phase by a quarter turn. The turn over a bit z, squared:
     * random bits turn it by about +-pi/2 or 0 (two opposite bits cancel), which leaves a
     * mean of z but not of z^2, while the FCCH gives z^2 = -1 times the carrier offset. Summed
     * over FCCH_WIN bits, from running sums. */
    cs[0] = 0;
    ca[0] = 0;
    for (i = 0; i < ny; i++) {
        const float complex z = i >= GSM_SPS ? y[i] * conjf(y[i - GSM_SPS]) : 0;
        const double complex v = (double complex)z * z;

        cs[i + 1] = cs[i] + v;
        ca[i + 1] = ca[i] + cabs(v);
    }
    {
        const size_t w = FCCH_WIN * GSM_SPS;
        size_t run0 = 0, run = 0, gap = 0, at_best = 0;
        double run_best = 0;

        /* Runs of windows over the threshold, short dips (noise) inside them bridged. */
        for (i = GSM_SPS; i + w <= ny; i++) {
            const double complex v = cs[i + w] - cs[i];
            const double a = ca[i + w] - ca[i];
            const double q = a > 0 ? cabs(v) / a : 0;
            const double f = carg(-v) / (4.0 * M_PI) * GSM_R;
            const bool tone = q >= FCCH_MIN && fabs(f) <= max_off;

            if (tone) {
                if (!run)
                    run0 = i;
                run = i - run0 + 1;
                gap = 0;
                if (q > run_best) {
                    run_best = q;
                    at_best  = i;
                }
                if (i + w < ny)
                    continue;
            } else if (run && ++gap < FCCH_GAP * GSM_SPS && i + w < ny) {
                continue;
            }
            if (run >= FCCH_MIN_RUN * GSM_SPS && run <= FCCH_MAX_RUN * GSM_SPS && nf < MAX_FCCH) {
                const double complex vm = cs[at_best + w] - cs[at_best];
                double t, f2, quality;

                if (fcch_refine(y, ny, run0, run, carg(-vm) / (4.0 * M_PI) * GSM_R, &t, &f2, &quality) == 0 &&
                    quality >= FCCH_MIN_BURST) {
                    /* One burst seen as two runs: the better one. */
                    if (nf > 0 && fabs(t - fcch_t[nf - 1]) < 100.0 * GSM_SPS) {
                        if (quality > fcch_w[nf - 1]) {
                            fcch_t[nf - 1] = t;
                            fcch_f[nf - 1] = f2;
                            fcch_w[nf - 1] = quality;
                        }
                    } else {
                        fcch_t[nf] = t;
                        fcch_f[nf] = f2;
                        fcch_w[nf] = quality;
                        nf++;
                    }
                }
            }
            run      = 0;
            gap      = 0;
            run_best = 0;
        }
    }
    res->fcch_bursts = nf;
    if (nf == 0) {
        rc = 0;
        goto out;
    }
    res->fcch = true;
    for (s = 0; s < nf; s++) {
        f0   += fcch_f[s] * fcch_w[s];
        wsum += fcch_w[s];
        res->fcch_quality += fcch_w[s] / nf;
    }
    f0 /= wsum;
    res->freq_error_hz = f0;

    /* SCH: one frame after each FCCH. */
    {
        double sf = 0, pw = 0, pr = 0, pp = 0, ev = 0;

        for (s = 0; s < nf && res->nsch < M2SDR_GSM_MAX_SCH; s++) {
            struct m2sdr_gsm_sch *sch = &res->sch[res->nsch];
            const double t = fcch_t[s] + (double)GSM_FRAME_BITS * GSM_SPS;
            float perr[M2SDR_GSM_BURST_BITS];

            if (t + (M2SDR_GSM_BURST_BITS + SCH_SEARCH + REF_EXTRA) * GSM_SPS >= (double)ny ||
                t - (SCH_SEARCH + REF_EXTRA) * GSM_SPS < 0)
                continue;
            if (sch_measure(&c, y, ny, t, fcch_f[s], sch, perr, res->points, &res->npoints) != 0)
                continue;
            sch->time_s = sch->start / sample_rate;
            if (sch->crc && decoded == 0) {
                res->bsic = sch->bsic;
                res->ncc  = sch->ncc;
                res->bcc  = sch->bcc;
                res->fn   = sch->fn;
            }
            /* The trace of the first burst decoded, else of the first one. */
            if ((sch->crc && decoded == 0) || res->nsch == 0)
                memcpy(res->phase_err_deg, perr, sizeof(perr));
            decoded += sch->crc;
            res->nsch++;
        }
        /* Figures over the decoded bursts, else over all of them. */
        for (s = 0; s < res->nsch; s++) {
            const struct m2sdr_gsm_sch *sch = &res->sch[s];

            if (decoded && !sch->crc)
                continue;
            sf += sch->freq_hz;
            pw += pow(10.0, sch->power_db / 10.0);
            pr += sch->phase_err_rms_deg * sch->phase_err_rms_deg;
            ev += sch->evm * sch->evm;
            if (sch->phase_err_peak_deg > pp)
                pp = sch->phase_err_peak_deg;
            used++;
        }
        if (used) {
            res->freq_error_hz      = sf / used;
            res->burst_power_db     = 10.0 * log10(pw / used + 1e-30);
            res->phase_err_rms_deg  = sqrt(pr / used);
            res->phase_err_peak_deg = pp;
            res->evm                = sqrt(ev / used);
            res->snr_db             = res->evm > 0 ? -20.0 * log10(res->evm) : 99.0;
        }
    }
    res->found = decoded > 0;
    rc = 0;
out:
    free(y);
    free(cs);
    free(ca);
    m2sdr_kernel_free(&c.lp);
    m2sdr_kernel_free(&c.lp4);
    return rc;
}
