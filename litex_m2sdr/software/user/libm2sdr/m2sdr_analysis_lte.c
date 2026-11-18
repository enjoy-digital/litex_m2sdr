/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: LTE downlink, FDD with a normal cyclic prefix.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis.h"
#include "m2sdr_fft.h"
#include "m2sdr_lte_seq.h"

#define BASE_RATE        1920000.0   /* The synchronisation is done at this rate. */
#define BASE_N           128         /* FFT length at the base rate. */
#define BASE_CP0         10          /* Cyclic prefix of the first symbol of a slot, */
#define BASE_CP          9           /* and of the others. */
#define BASE_SLOT        960
#define BASE_HALF_FRAME  9600
#define BASE_FRAME       19200
#define SLOT_SYMBOLS     7
#define PSS_SYMBOL       6           /* In slots 0 and 10. */
#define SSS_SYMBOL       5
#define PSS_HALF_FRAMES  2           /* Added up in the search. */
#define PSS_HYP_HZ       5000.0      /* Spacing of the frequency offsets tried, */
#define PSS_HYPS         7           /* -15 to +15 kHz. */
#define PSS_MIN_QUALITY  6.0         /* Peak of the correlation over its mean. */
#define SSS_MIN_MATCH    0.35        /* Of 1 for a clean SSS. */
#define MIN_SECONDS      0.025
#define DATA_MIN_RATIO   0.05        /* A block in use has this much of the power of a reference element. */
#define DATA_MAX_RATIO   0.30        /* Two ports: a block is read below this EVM over the spacing of the points. */
#define DATA_FAMILY      4           /* Constellations tried on a block. */
#define SMOOTH_HALF      2           /* Pilots on each side in the smoothing of the channel. */

static const int rb_of_rate[5] = { 6, 15, 25, 50, 100 };

/* Geometry of a capture: everything in samples of the capture. */
struct lte_dims {
    int    d;                    /* Rate over the base rate: 1, 2, 4, 8 or 16. */
    int    n;                    /* FFT length. */
    int    adv;                  /* The FFT window starts this early in the cyclic prefix. */
    double sample_rate;
};

/* Start of the useful part of symbol l of a slot, from the start of the slot. */
static size_t symbol_start(const struct lte_dims *dim, int l)
{
    return (size_t)dim->d * (size_t)(BASE_CP0 + l * (BASE_N + BASE_CP));
}

/* Synchronisation */
/*-----------------*/

/* x at the base rate: a low-pass under a Hann window, then one sample of d. The filter is
 * centred, so a sample of the output is the one of the input at d times its index. */
static float complex *decimate(const float complex *x, size_t count, int d, size_t *out_count)
{
    const int half = 8 * d;
    const size_t n = count / (size_t)d;
    float complex *y = malloc((n ? n : 1) * sizeof(*y));
    float *taps = malloc((size_t)(2 * half + 1) * sizeof(*taps));
    double sum = 0;
    size_t j;
    int i;

    if (!y || !taps) {
        free(y);
        free(taps);
        return NULL;
    }
    *out_count = n;
    if (d == 1) {
        memcpy(y, x, n * sizeof(*y));
        free(taps);
        return y;
    }
    for (i = -half; i <= half; i++) {
        const double u = (double)i * 0.9 / d;
        const double sinc = i ? sin(M_PI * u) / (M_PI * u) : 1.0;
        taps[i + half] = (float)(sinc * (0.5 + 0.5 * cos(M_PI * i / (half + 1.0))));
        sum += taps[i + half];
    }
    for (i = 0; i <= 2 * half; i++)
        taps[i] = (float)(taps[i] / sum);
    for (j = 0; j < n; j++) {
        const long c = (long)(j * (size_t)d);
        float complex acc = 0;

        for (i = -half; i <= half; i++) {
            if (c + i >= 0 && c + i < (long)count)
                acc += x[c + i] * taps[i + half];
        }
        y[j] = acc;
    }
    free(taps);
    return y;
}

/* PSS in time at the base rate: its 62 subcarriers around an unused DC. */
static int pss_time(int n_id_2, float complex *p)
{
    struct m2sdr_fft *fft = m2sdr_fft_alloc(BASE_N, true);
    float complex d[LTE_SYNC_LEN], in[BASE_N];
    int i;

    if (!fft)
        return -1;
    memset(in, 0, sizeof(in));
    m2sdr_lte_pss(n_id_2, d);
    for (i = 0; i < LTE_SYNC_LEN; i++) {
        const int k = i < 31 ? i - 31 : i - 30;
        in[(k + BASE_N) % BASE_N] = d[i];
    }
    m2sdr_fft_run(fft, in, p);
    m2sdr_fft_free(fft);
    return 0;
}

struct lte_sync {
    int    n_id_2;
    size_t pss;                  /* Start of a PSS at the base rate, in the first half frame. */
    double cfo_hz;               /* First value of the carrier offset. */
    double quality;
};

/* PSS: the three sequences at several carrier offsets, slid over a half frame and added over
 * the half frames. */
static int pss_search(const float complex *xd, size_t nd, struct lte_sync *sync)
{
    float complex pss[3][BASE_N], ref[BASE_N];
    float *acc = malloc(BASE_HALF_FRAME * sizeof(*acc));
    double best = 0;
    int u, h, n, hf, best_u = 0, best_h = 0;
    size_t t, best_t = 0;

    if (!acc || nd < (size_t)(PSS_HALF_FRAMES * BASE_HALF_FRAME + BASE_N)) {
        free(acc);
        return -1;
    }
    for (u = 0; u < 3; u++) {
        if (pss_time(u, pss[u]) != 0) {
            free(acc);
            return -1;
        }
    }
    sync->quality = 0;
    for (u = 0; u < 3; u++) {
        for (h = 0; h < PSS_HYPS; h++) {
            const double f = (h - PSS_HYPS / 2) * PSS_HYP_HZ;
            double mean = 0, peak = 0;
            size_t at = 0;

            for (n = 0; n < BASE_N; n++)
                ref[n] = conjf(pss[u][n] * (float complex)cexp(2.0 * M_PI * I * f * n / BASE_RATE));
            for (t = 0; t < BASE_HALF_FRAME; t++) {
                float v = 0;

                for (hf = 0; hf < PSS_HALF_FRAMES; hf++) {
                    const float complex *p = xd + (size_t)hf * BASE_HALF_FRAME + t;
                    float re = 0, im = 0;

                    for (n = 0; n < BASE_N; n++) {
                        re += crealf(p[n]) * crealf(ref[n]) - cimagf(p[n]) * cimagf(ref[n]);
                        im += crealf(p[n]) * cimagf(ref[n]) + cimagf(p[n]) * crealf(ref[n]);
                    }
                    v += re * re + im * im;
                }
                acc[t] = v;
                mean  += v;
                if (v > peak) {
                    peak = v;
                    at   = t;
                }
            }
            mean /= BASE_HALF_FRAME;
            if (peak > best) {
                best          = peak;
                best_u        = u;
                best_h        = h;
                best_t        = at;
                sync->quality = mean > 0 ? peak / mean : 0;
            }
        }
    }
    free(acc);
    sync->n_id_2 = best_u;
    sync->pss    = best_t;

    /* Carrier offset from the two halves of the PSS, over the half frames: the hypothesis
     * only tells which turn of the phase. */
    {
        double complex turn = 0;
        const double f = (best_h - PSS_HYPS / 2) * PSS_HYP_HZ;
        double fine;

        for (hf = 0; hf < PSS_HALF_FRAMES; hf++) {
            const float complex *p = xd + (size_t)hf * BASE_HALF_FRAME + best_t;
            double complex a = 0, b = 0;

            for (n = 0; n < BASE_N / 2; n++)
                a += p[n] * conjf(pss[best_u][n]) * cexp(-2.0 * M_PI * I * f * n / BASE_RATE);
            for (; n < BASE_N; n++)
                b += p[n] * conjf(pss[best_u][n]) * cexp(-2.0 * M_PI * I * f * n / BASE_RATE);
            turn += b * conj(a);
        }
        fine = carg(turn) / (2.0 * M_PI * (BASE_N / 2) / BASE_RATE);
        sync->cfo_hz = f + fine;
    }
    return 0;
}

/* The 62 subcarriers of a synchronisation symbol that starts at `start`, the carrier offset
 * removed. */
static int sync_symbol(const float complex *xd, size_t start, double cfo_hz, float complex *out)
{
    struct m2sdr_fft *fft = m2sdr_fft_alloc(BASE_N, false);
    float complex in[BASE_N], y[BASE_N];
    int i, n;

    if (!fft)
        return -1;
    for (n = 0; n < BASE_N; n++)
        in[n] = xd[start + (size_t)n] * (float complex)cexp(-2.0 * M_PI * I * cfo_hz * (double)(start + (size_t)n) / BASE_RATE);
    m2sdr_fft_run(fft, in, y);
    for (i = 0; i < LTE_SYNC_LEN; i++) {
        const int k = i < 31 ? i - 31 : i - 30;
        out[i] = y[(k + BASE_N) % BASE_N] / (float)BASE_N;
    }
    m2sdr_fft_free(fft);
    return 0;
}

/* SSS, one symbol ahead of the PSS: equalised by the PSS, then compared with the sequences of
 * every cell group in both subframes. Returns the match of the best, 0 to 1. */
static double sss_search(const float complex *xd, size_t slot, const struct lte_sync *sync, double cfo_hz,
                         int *n_id_1, bool *subframe5, double *pss_power, double *sss_power)
{
    float complex ypss[LTE_SYNC_LEN], ysss[LTE_SYNC_LEN], dpss[LTE_SYNC_LEN];
    float z[LTE_SYNC_LEN], d[LTE_SYNC_LEN];
    double norm = 0, best = -1, pp = 0, ps = 0;
    int i, id, sf;

    if (sync_symbol(xd, slot + BASE_CP0 + PSS_SYMBOL * (BASE_N + BASE_CP), cfo_hz, ypss) != 0 ||
        sync_symbol(xd, slot + BASE_CP0 + SSS_SYMBOL * (BASE_N + BASE_CP), cfo_hz, ysss) != 0)
        return -1;
    m2sdr_lte_pss(sync->n_id_2, dpss);
    for (i = 0; i < LTE_SYNC_LEN; i++) {
        const float complex h = ypss[i] * conjf(dpss[i]);
        const float complex v = ysss[i] * conjf(h);

        z[i]  = crealf(v);
        norm += cabsf(v);
        pp   += crealf(ypss[i]) * crealf(ypss[i]) + cimagf(ypss[i]) * cimagf(ypss[i]);
        ps   += crealf(ysss[i]) * crealf(ysss[i]) + cimagf(ysss[i]) * cimagf(ysss[i]);
    }
    *pss_power = pp / LTE_SYNC_LEN;
    *sss_power = ps / LTE_SYNC_LEN;
    if (!(norm > 0))
        return 0;
    for (id = 0; id < 168; id++) {
        for (sf = 0; sf < 2; sf++) {
            double m = 0;

            m2sdr_lte_sss(id, sync->n_id_2, sf != 0, d);
            for (i = 0; i < LTE_SYNC_LEN; i++)
                m += z[i] * d[i];
            m /= norm;
            if (m > best) {
                best       = m;
                *n_id_1    = id;
                *subframe5 = sf != 0;
            }
        }
    }
    return best;
}

/* Resource grid */
/*---------------*/

struct lte_grid {
    int    rb;                   /* Resource blocks analysed. */
    int    k;                    /* Subcarriers: 12 * rb. */
    float complex *re;           /* [140][k]: resource elements, 1.0 for a full-scale subcarrier. */
    float complex *h0;           /* [140][k]: channel of port 0. */
    float complex *h1;           /* [140][k]: channel of port 1. */
    double rs_power[2];          /* Of a reference element of each port. */
    double cfo_residual_hz;      /* Carrier offset left, from the reference signals of port 0. */
    double rs_evm;
    int    nrs;
    float complex rs_points[M2SDR_LTE_MAX_POINTS];
};

static void grid_free(struct lte_grid *g)
{
    free(g->re);
    free(g->h0);
    free(g->h1);
    memset(g, 0, sizeof(*g));
}

/* Carrier offset from the cyclic prefixes of a frame: modulo the subcarrier spacing. */
static double cp_cfo(const float complex *x, size_t frame, const struct lte_dims *dim)
{
    const int cp = BASE_CP * dim->d;
    double complex acc = 0;
    int q, n;

    for (q = 0; q < M2SDR_LTE_FRAME_SYMBOLS; q++) {
        const size_t u = frame + (size_t)(q / SLOT_SYMBOLS) * BASE_SLOT * (size_t)dim->d + symbol_start(dim, q % SLOT_SYMBOLS);
        const float complex *p = x + u - (size_t)cp;

        /* The middle half of the prefix: its start holds the end of the symbol before. */
        for (n = cp / 4; n < 3 * cp / 4; n++)
            acc += conjf(p[n]) * p[n + dim->n];
    }
    return carg(acc) / (2.0 * M_PI) * 15000.0;
}

/* FFT of the 140 symbols of the frame that starts at `frame`. */
static int grid_build(struct lte_grid *g, const float complex *x, size_t frame, const struct lte_dims *dim,
                      double cfo_hz, int rb)
{
    struct m2sdr_fft *fft = m2sdr_fft_alloc((size_t)dim->n, false);
    float complex *in = malloc((size_t)dim->n * sizeof(*in));
    float complex *out = malloc((size_t)dim->n * sizeof(*out));
    const float complex rot = (float complex)cexp(-2.0 * M_PI * I * cfo_hz / dim->sample_rate);
    int q, a, n, rc = -1;

    memset(g, 0, sizeof(*g));
    g->rb = rb;
    g->k  = 12 * rb;
    g->re = malloc((size_t)M2SDR_LTE_FRAME_SYMBOLS * (size_t)g->k * sizeof(*g->re));
    g->h0 = malloc((size_t)M2SDR_LTE_FRAME_SYMBOLS * (size_t)g->k * sizeof(*g->h0));
    g->h1 = malloc((size_t)M2SDR_LTE_FRAME_SYMBOLS * (size_t)g->k * sizeof(*g->h1));
    if (!fft || !in || !out || !g->re || !g->h0 || !g->h1)
        goto out;
    for (q = 0; q < M2SDR_LTE_FRAME_SYMBOLS; q++) {
        const size_t u = frame + (size_t)(q / SLOT_SYMBOLS) * BASE_SLOT * (size_t)dim->d + symbol_start(dim, q % SLOT_SYMBOLS);
        const size_t w = u - (size_t)dim->adv;
        /* The phase of the carrier offset follows the sample index, across the symbols. */
        float complex osc = (float complex)cexp(-2.0 * M_PI * I * cfo_hz * (double)(w - frame) / dim->sample_rate);

        for (n = 0; n < dim->n; n++) {
            in[n] = x[w + (size_t)n] * osc;
            osc  *= rot;
        }
        m2sdr_fft_run(fft, in, out);
        for (a = 0; a < g->k; a++) {
            const int k = a < g->k / 2 ? a - g->k / 2 : a - g->k / 2 + 1;
            /* The window starts early: the phase slope this gives is removed. */
            const float complex slope = (float complex)cexp(2.0 * M_PI * I * (double)k * dim->adv / dim->n);

            g->re[(size_t)q * (size_t)g->k + (size_t)a] = out[(k + dim->n) % dim->n] * slope / (float)dim->n;
        }
    }
    rc = 0;

out:
    m2sdr_fft_free(fft);
    free(in);
    free(out);
    if (rc != 0)
        grid_free(g);
    return rc;
}

/* Channel of a port from its reference signals: least squares at the pilots, a mean over
 * their neighbours, then lines between the pilots and between the symbols that carry them. */
static int grid_channel(struct lte_grid *g, int cell_id, int port, float complex *h)
{
    const int pilots = 2 * g->rb, vshift = cell_id % 6;
    float complex *ls = malloc((size_t)pilots * sizeof(*ls));
    float complex *sm = malloc((size_t)pilots * sizeof(*sm));
    float complex *prev = malloc((size_t)pilots * sizeof(*prev));
    float complex crs[2 * LTE_MAX_RB];
    double complex drift = 0;
    int pilot_q[2 * M2SDR_LTE_FRAME_SLOTS], npq = 0;
    double power = 0, err = 0, slope = 0;
    size_t count = 0, kept = 0;
    int ns, li, j, a, q, i;

    if (!ls || !sm || !prev) {
        free(ls);
        free(sm);
        free(prev);
        return -1;
    }
    for (ns = 0; ns < M2SDR_LTE_FRAME_SLOTS; ns++) {
        for (li = 0; li < 2; li++) {
            const int l = li ? 4 : 0;
            /* TS 36.211 6.10.1.2: port 0 at offset 0 in symbol 0 and 3 in symbol 4, port 1 the
             * other way round. */
            const int v = (li ^ port) ? 3 : 0;
            const int s = (v + vshift) % 6;
            float complex *row;

            q   = ns * SLOT_SYMBOLS + l;
            row = h + (size_t)q * (size_t)g->k;
            m2sdr_lte_crs(cell_id, ns, l, crs);
            for (j = 0; j < pilots; j++) {
                ls[j]  = g->re[(size_t)q * (size_t)g->k + (size_t)(s + 6 * j)] * conjf(crs[j - g->rb + LTE_MAX_RB]);
                power += crealf(ls[j]) * crealf(ls[j]) + cimagf(ls[j]) * cimagf(ls[j]);
            }
            count += (size_t)pilots;
            /* The same pilots half a millisecond later: what is left of the carrier offset. */
            if (port == 0 && li == 0) {
                if (ns > 0) {
                    for (j = 0; j < pilots; j++)
                        drift += ls[j] * conjf(prev[j]);
                }
                memcpy(prev, ls, (size_t)pilots * sizeof(*ls));
            }
            /* A timing offset turns the phase evenly along the subcarriers: this slope is
             * taken off before the mean over the neighbours, and put back after. */
            {
                double complex turn = 0;

                for (j = 0; j + 1 < pilots; j++)
                    turn += ls[j + 1] * conjf(ls[j]);
                slope = cabs(turn) > 0 ? carg(turn) : 0.0;
                for (j = 0; j < pilots; j++)
                    ls[j] *= (float complex)cexp(-I * slope * (double)j);
            }
            for (j = 0; j < pilots; j++) {
                const int j0 = j - SMOOTH_HALF < 0 ? 0 : j - SMOOTH_HALF;
                const int j1 = j + SMOOTH_HALF >= pilots ? pilots - 1 : j + SMOOTH_HALF;
                float complex sum = 0;

                for (i = j0; i <= j1; i++)
                    sum += ls[i];
                sm[j] = sum / (float)(j1 - j0 + 1);
                /* EVM of a pilot against the mean of its neighbours, itself left out; the
                 * noise of that mean is taken off. */
                if (port == 0 && j1 > j0) {
                    const float complex others = (sum - ls[j]) / (float)(j1 - j0);
                    const double m = crealf(others) * crealf(others) + cimagf(others) * cimagf(others);

                    if (m > 0) {
                        const float complex e = ls[j] / others - 1.0f;
                        err += (crealf(e) * crealf(e) + cimagf(e) * cimagf(e)) / (1.0 + 1.0 / (j1 - j0));
                        kept++;
                        if (g->nrs < M2SDR_LTE_MAX_POINTS && ((kept - 1) % 8) == 0)
                            g->rs_points[g->nrs++] = ls[j] / others * crs[j - g->rb + LTE_MAX_RB];
                    }
                }
            }
            /* Lines between the pilots, the first and the last held to the edges. */
            for (a = 0; a < g->k; a++) {
                const double pos = ((double)a - (double)s) / 6.0;
                int j0 = (int)floor(pos);
                float t;

                if (j0 < 0)
                    j0 = 0;
                if (j0 > pilots - 2)
                    j0 = pilots - 2;
                t = (float)(pos - (double)j0);
                if (t < 0)
                    t = 0;
                if (t > 1)
                    t = 1;
                row[a] = (sm[j0] * (1.0f - t) + sm[j0 + 1] * t) * (float complex)cexp(I * slope * pos);
            }
            pilot_q[npq++] = q;
        }
    }
    g->rs_power[port] = count ? power / (double)count : 0.0;
    if (port == 0) {
        g->rs_evm          = kept ? sqrt(err / (double)kept) : 0.0;
        g->cfo_residual_hz = cabs(drift) > 0 ? carg(drift) / (2.0 * M_PI * 0.0005) : 0.0;
    }
    free(prev);

    /* The other symbols: on the line between the two symbols with pilots around them. */
    for (q = 0; q < M2SDR_LTE_FRAME_SYMBOLS; q++) {
        int before = -1, after = -1;

        for (i = 0; i < npq; i++) {
            if (pilot_q[i] == q)
                break;
            if (pilot_q[i] < q)
                before = pilot_q[i];
            if (pilot_q[i] > q && after < 0)
                after = pilot_q[i];
        }
        if (i < npq)
            continue;
        if (before < 0)
            before = after;
        if (after < 0)
            after = before;
        {
            const float t = after != before ? (float)(q - before) / (float)(after - before) : 0.0f;
            const float complex *hb = h + (size_t)before * (size_t)g->k, *ha = h + (size_t)after * (size_t)g->k;
            float complex *row = h + (size_t)q * (size_t)g->k;

            for (a = 0; a < g->k; a++)
                row[a] = hb[a] * (1.0f - t) + ha[a] * t;
        }
    }
    free(ls);
    free(sm);
    return 0;
}

/* Broadcast channel */
/*-------------------*/

/* PBCH of the frame: its 240 elements in their order, with the channel of both ports. */
static int pbch_elements(const struct lte_grid *g, int cell_id, float complex *y, float complex *h0, float complex *h1)
{
    int l, i, n = 0;

    for (l = 0; l < 4; l++) {
        const int q = SLOT_SYMBOLS + l;      /* Slot 1. */

        for (i = 0; i < 72; i++) {
            const int a = g->k / 2 - 36 + i;

            /* The reference signals of four ports keep their elements in symbols 0 and 1. */
            if (l < 2 && (a % 3) == (cell_id % 3))
                continue;
            y[n]  = g->re[(size_t)q * (size_t)g->k + (size_t)a];
            h0[n] = g->h0[(size_t)q * (size_t)g->k + (size_t)a];
            h1[n] = g->h1[(size_t)q * (size_t)g->k + (size_t)a];
            n++;
        }
    }
    return n;
}

static int pbch_decode(const struct lte_grid *g, int cell_id, struct m2sdr_lte_result *res)
{
    float complex y[240], h0[240], h1[240], z[240];
    float soft[LTE_PBCH_FRAME];
    uint8_t mib[LTE_MIB_BITS];
    int n, i, hyp, ports = 0, phase = 0;

    n = pbch_elements(g, cell_id, y, h0, h1);
    if (n != 240)
        return -1;
    for (hyp = 1; hyp <= 2; hyp++) {
        if (hyp == 1) {
            for (i = 0; i < n; i++) {
                const float w = crealf(h0[i]) * crealf(h0[i]) + cimagf(h0[i]) * cimagf(h0[i]);
                z[i]            = w > 0 ? y[i] * conjf(h0[i]) / w : 0;
                soft[2 * i]     = crealf(z[i]) * w;
                soft[2 * i + 1] = cimagf(z[i]) * w;
            }
        } else {
            /* Transmit diversity on two ports (TS 36.211 6.3.4.3): port 0 sends x0, x1 and
             * port 1 sends -conj(x1), conj(x0) on two neighbouring elements. */
            for (i = 0; i + 1 < n; i += 2) {
                const float w = 0.5f * (crealf(h0[i]) * crealf(h0[i]) + cimagf(h0[i]) * cimagf(h0[i]) +
                                        crealf(h1[i]) * crealf(h1[i]) + cimagf(h1[i]) * cimagf(h1[i]) +
                                        crealf(h0[i + 1]) * crealf(h0[i + 1]) + cimagf(h0[i + 1]) * cimagf(h0[i + 1]) +
                                        crealf(h1[i + 1]) * crealf(h1[i + 1]) + cimagf(h1[i + 1]) * cimagf(h1[i + 1]));
                const float complex x0 = conjf(h0[i]) * y[i] + h1[i + 1] * conjf(y[i + 1]);
                const float complex x1 = conjf(h0[i + 1]) * y[i + 1] - h1[i] * conjf(y[i]);

                /* The symbols are sent at 1/sqrt(2) on each port. */
                z[i]     = w > 0 ? x0 / w * (float)M_SQRT2 : 0;
                z[i + 1] = w > 0 ? x1 / w * (float)M_SQRT2 : 0;
                soft[2 * i]     = crealf(x0);
                soft[2 * i + 1] = cimagf(x0);
                soft[2 * i + 2] = crealf(x1);
                soft[2 * i + 3] = cimagf(x1);
            }
        }
        if (m2sdr_lte_bch_decode(soft, cell_id, mib, &ports, &phase) == 0 && (ports == hyp || (hyp == 2 && ports == 4)))
            break;
    }
    if (hyp > 2)
        return -1;
    {
        static const int bandwidths[8] = { 6, 15, 25, 50, 75, 100, 0, 0 };
        double complex gain = 0;
        double den = 0, err = 0;
        int sfn = 0;

        res->mib            = true;
        res->ports          = ports;
        res->n_rb           = bandwidths[(mib[0] << 2) | (mib[1] << 1) | mib[2]];
        res->phich_duration = mib[3];
        res->phich_resource = (mib[4] << 1) | mib[5];
        for (i = 0; i < 8; i++)
            sfn = (sfn << 1) | mib[6 + i];
        res->sfn = sfn * 4 + phase;

        /* EVM against QPSK, after a gain by least squares. */
        for (i = 0; i < n; i++) {
            const float complex d = (float)M_SQRT1_2 * ((crealf(z[i]) >= 0 ? 1.0f : -1.0f) + I * (cimagf(z[i]) >= 0 ? 1.0f : -1.0f));
            gain += z[i] * conjf(d);
            den  += 1.0;
        }
        gain /= den;
        for (i = 0; i < n; i++) {
            const float complex v = cabs(gain) > 0 ? z[i] / (float complex)gain : z[i];
            const float complex d = (float)M_SQRT1_2 * ((crealf(v) >= 0 ? 1.0f : -1.0f) + I * (cimagf(v) >= 0 ? 1.0f : -1.0f));
            const float complex e = v - d;

            res->pbch_points[i] = v;
            err += crealf(e) * crealf(e) + cimagf(e) * cimagf(e);
        }
        res->npbch    = n;
        res->pbch_evm = sqrt(err / (double)n);
    }
    return 0;
}

/* Data */
/*------*/

struct lte_constellation {
    float complex points[256];
    int    count;
    double dmin;
};

static void lte_constellation_init(struct lte_constellation *c, enum m2sdr_modulation modulation)
{
    int i, j;

    c->count = m2sdr_constellation(modulation, c->points);
    c->dmin  = 1e9;
    for (i = 0; i < c->count; i++) {
        for (j = i + 1; j < c->count; j++) {
            const double d = cabsf(c->points[i] - c->points[j]);
            if (d < c->dmin)
                c->dmin = d;
        }
    }
}

static float complex lte_slice(const struct lte_constellation *c, float complex y)
{
    float best = 1e30f;
    int i, at = 0;

    for (i = 0; i < c->count; i++) {
        const float dr = crealf(y) - crealf(c->points[i]), di = cimagf(y) - cimagf(c->points[i]);
        const float d = dr * dr + di * di;
        if (d < best) {
            best = d;
            at   = i;
        }
    }
    return c->points[at];
}

/* EVM of n equalised symbols against a constellation, their scale found by least squares;
 * *gain is the scale. */
static double block_evm(const float complex *z, int n, const struct lte_constellation *c, float complex *gain)
{
    double power = 0, se = 0, sd = 0;
    float complex g;
    int i, pass;

    for (i = 0; i < n; i++)
        power += crealf(z[i]) * crealf(z[i]) + cimagf(z[i]) * cimagf(z[i]);
    if (!(power > 0))
        return 1e9;
    g = (float)sqrt(power / n);
    for (pass = 0; pass < 3; pass++) {
        double complex num = 0;
        double den = 0;

        se = sd = 0;
        for (i = 0; i < n; i++) {
            const float complex v = z[i] / g;
            const float complex d = lte_slice(c, v);
            const float complex e = v - d;

            num += z[i] * conjf(d);
            den += crealf(d) * crealf(d) + cimagf(d) * cimagf(d);
            se  += crealf(e) * crealf(e) + cimagf(e) * cimagf(e);
            sd  += crealf(d) * crealf(d) + cimagf(d) * cimagf(d);
        }
        if (pass < 2 && den > 0 && cabs(num) > 0)
            g = (float complex)(num / den);
    }
    *gain = g;
    return sd > 0 ? sqrt(se / sd) : 1e9;
}

/* Is the element (slot ns, symbol l, subcarrier a) free for data, in a cell of one or two ports? */
static bool data_element(const struct lte_grid *g, int cell_id, int cell_rb, int ports, int ns, int l, int a)
{
    const int control = cell_rb <= 10 ? 4 : 3;
    const bool centre = a >= g->k / 2 - 36 && a < g->k / 2 + 36;

    if ((ns & 1) == 0 && l < control)
        return false;                                   /* Control region, taken at its largest. */
    if ((l == 0 || l == 4) && (a % 6) == ((l ? 3 : 0) + cell_id) % 6)
        return false;                                   /* Reference signal of port 0. */
    if (ports > 1 && (l == 0 || l == 4) && (a % 3) == cell_id % 3)
        return false;                                   /* Reference signal of port 1. */
    if (centre && (ns == 0 || ns == 10) && (l == SSS_SYMBOL || l == PSS_SYMBOL))
        return false;                                   /* Synchronisation signals. */
    if (centre && ns == 1 && l < 4)
        return false;                                   /* PBCH. */
    return true;
}

/* The constellation a block fits: the smallest one that fits well, or else the best. Returns
 * its EVM over the spacing of its points. */
static double block_fit(const float complex *z, int n, const struct lte_constellation *cons, int *modulation, double *evm,
                        float complex *gain)
{
    double best = 1e9;
    int m;

    *modulation = -1;
    for (m = 0; m < DATA_FAMILY; m++) {
        float complex g = 1;
        const double e = block_evm(z, n, &cons[m], &g);
        const double ratio = e / cons[m].dmin;

        if (ratio < best) {
            best        = ratio;
            *modulation = m;
            *evm        = e;
            *gain       = g;
        }
        if (ratio < 0.2)
            break;
    }
    return best;
}

/* The symbols of a block of n elements, sent in one of the ways a cell of two ports has for one
 * layer; the elements of each symbol of the slot start at first[]. Returns their number.
 *   way 0    : transmit diversity (TS 36.211 6.3.4.3), as the PBCH.
 *   way 1..4 : the same symbol on both ports, the second one turned by 1, -1, j or -j
 *              (TS 36.211 6.3.4.2.3, one layer). */
static int data_layer(const float complex *y, const float complex *h0, const float complex *h1, const int *first, int way,
                      float complex *z)
{
    static const float complex turn[4] = { 1, -1, I, -I };
    int l, i, n = 0;

    for (l = 0; l < SLOT_SYMBOLS; l++) {
        if (way == 0) {
            for (i = first[l]; i + 1 < first[l + 1]; i += 2) {
                const float w = 0.5f * (crealf(h0[i]) * crealf(h0[i]) + cimagf(h0[i]) * cimagf(h0[i]) +
                                        crealf(h1[i]) * crealf(h1[i]) + cimagf(h1[i]) * cimagf(h1[i]) +
                                        crealf(h0[i + 1]) * crealf(h0[i + 1]) + cimagf(h0[i + 1]) * cimagf(h0[i + 1]) +
                                        crealf(h1[i + 1]) * crealf(h1[i + 1]) + cimagf(h1[i + 1]) * cimagf(h1[i + 1]));
                const float complex x0 = conjf(h0[i]) * y[i] + h1[i + 1] * conjf(y[i + 1]);
                const float complex x1 = conjf(h0[i + 1]) * y[i + 1] - h1[i] * conjf(y[i]);

                z[n++] = w > 0 ? x0 / w : 0;
                z[n++] = w > 0 ? x1 / w : 0;
            }
        } else {
            for (i = first[l]; i < first[l + 1]; i++) {
                const float complex h = h0[i] + turn[way - 1] * h1[i];

                z[n++] = cabsf(h) > 0 ? y[i] / h : 0;
            }
        }
    }
    return n;
}

static void data_analyse(const struct lte_grid *g, int cell_id, int cell_rb, int ports, struct m2sdr_lte_result *res)
{
    static const enum m2sdr_modulation family[DATA_FAMILY] = { M2SDR_MOD_QPSK, M2SDR_MOD_16QAM, M2SDR_MOD_64QAM,
                                                               M2SDR_MOD_256QAM };
    struct lte_constellation cons[DATA_FAMILY];
    float complex y[12 * SLOT_SYMBOLS], h0[12 * SLOT_SYMBOLS], h1[12 * SLOT_SYMBOLS];
    float complex z[12 * SLOT_SYMBOLS], zb[12 * SLOT_SYMBOLS];
    int used[M2SDR_MOD_COUNT] = { 0 }, first[SLOT_SYMBOLS + 1];
    /* Blocks of the cell against the ones analysed: half a block apart when one has an odd
     * number of them and the other an even one. */
    const int offset = cell_rb > g->rb ? (6 * (cell_rb - g->rb)) % 12 : 0;
    const int blocks = (g->k - offset) / 12;
    const int ways = ports == 1 ? 1 : 5;
    double se = 0;
    int ns, rb, l, a, i, m, total = 0, stride = 1, seen = 0;

    for (m = 0; m < DATA_FAMILY; m++)
        lte_constellation_init(&cons[m], family[m]);
    for (ns = 0; ns < M2SDR_LTE_FRAME_SLOTS; ns++) {
        for (rb = 0; rb < blocks; rb++) {
            double power = 0, best_ratio = 1e9, best_evm = 0;
            float complex best_gain = 1;
            int n = 0, nb = 0, best = -1, best_way = 0, way;

            for (l = 0; l < SLOT_SYMBOLS; l++) {
                const size_t row = (size_t)(ns * SLOT_SYMBOLS + l) * (size_t)g->k;

                first[l] = n;
                for (i = 0; i < 12; i++) {
                    a = offset + 12 * rb + i;
                    if (!data_element(g, cell_id, cell_rb, ports, ns, l, a))
                        continue;
                    y[n]  = g->re[row + (size_t)a];
                    h0[n] = g->h0[row + (size_t)a];
                    h1[n] = g->h1[row + (size_t)a];
                    power += crealf(y[n]) * crealf(y[n]) + cimagf(y[n]) * cimagf(y[n]);
                    n++;
                }
            }
            first[SLOT_SYMBOLS] = n;
            if (n < 24 || power / n < DATA_MIN_RATIO * g->rs_power[0])
                continue;
            for (way = 0; way < ways; way++) {
                double evm = 0, ratio;
                float complex gain = 1;
                int nz = 0;

                if (ports == 1) {
                    for (i = 0; i < n; i++)
                        z[nz++] = cabsf(h0[i]) > 0 ? y[i] / h0[i] : 0;
                } else {
                    nz = data_layer(y, h0, h1, first, way, z);
                }
                if (nz < 24)
                    continue;
                ratio = block_fit(z, nz, cons, &m, &evm, &gain);
                if (m >= 0 && ratio < best_ratio) {
                    best_ratio = ratio;
                    best       = m;
                    best_evm   = evm;
                    best_gain  = gain;
                    best_way   = way;
                    nb         = nz;
                    memcpy(zb, z, (size_t)nz * sizeof(z[0]));
                }
            }
            if (best < 0)
                continue;
            if (ports > 1 && best_ratio >= DATA_MAX_RATIO) {
                res->data_unknown++;                    /* Two layers, or a way that is not tried. */
                res->rb_unknown[ns][rb] = 1;
                continue;
            }
            if (ports > 1 && best_way == 0)
                res->data_diversity++;
            else if (ports > 1)
                res->data_precoded++;
            res->rb_modulation[ns][rb] = (uint8_t)family[best];
            res->rb_evm[ns][rb]        = (float)best_evm;
            used[family[best]]++;
            se += best_evm * best_evm;
            total++;
            /* A sample of the symbols for the constellation: thinned as the blocks add up. */
            for (i = 0; i < nb; i++) {
                if ((seen++ % stride) != 0)
                    continue;
                if (res->ndata == M2SDR_LTE_MAX_POINTS) {
                    int k;
                    for (k = 0; k < M2SDR_LTE_MAX_POINTS / 2; k++)
                        res->data_points[k] = res->data_points[2 * k];
                    res->ndata = M2SDR_LTE_MAX_POINTS / 2;
                    stride    *= 2;
                }
                res->data_points[res->ndata++] = zb[i] / best_gain;
            }
        }
    }
    res->data        = true;
    res->data_blocks = total;
    res->data_evm    = total ? sqrt(se / total) : 0.0;
    for (m = 0; m < M2SDR_MOD_COUNT; m++) {
        if (used[m] > used[res->data_modulation])
            res->data_modulation = (enum m2sdr_modulation)m;
    }
}

/* Analysis */
/*----------*/

int m2sdr_lte_analyse(const float complex *x, size_t count, double sample_rate, struct m2sdr_lte_result *res)
{
    struct lte_dims dim;
    struct lte_sync sync;
    struct lte_grid grid;
    float complex *xd = NULL;
    size_t nd = 0, slot, frame;
    double match, pss_power = 0, sss_power = 0, cfo, fine;
    bool subframe5 = false;
    int idx, n_id_1 = 0, rb, q, a, rc = -1;
    long frame_base;

    if (!x || !res || !(sample_rate > 0))
        return -1;
    memset(&grid, 0, sizeof(grid));
    for (idx = 0; idx < 5; idx++) {
        if (fabs(sample_rate - BASE_RATE * (1 << idx)) < 1.0)
            break;
    }
    if (idx == 5 || (double)count < MIN_SECONDS * sample_rate)
        return -1;
    memset(res, 0, sizeof(*res));
    dim.d           = 1 << idx;
    dim.n           = BASE_N * dim.d;
    dim.adv         = 4 * dim.d;
    dim.sample_rate = sample_rate;

    /* PSS and SSS at the base rate. */
    xd = decimate(x, count, dim.d, &nd);
    if (!xd)
        return -1;
    if (pss_search(xd, nd, &sync) != 0)
        goto out;
    res->sync_quality = sync.quality;
    res->n_id_2       = sync.n_id_2;
    if (sync.quality < PSS_MIN_QUALITY) {
        rc = 0;
        goto out;
    }
    /* The slot of the PSS: it is its last symbol. The first one whose SSS is in the capture. */
    {
        long s = (long)sync.pss - (BASE_CP0 + PSS_SYMBOL * (BASE_N + BASE_CP));
        while (s < 0)
            s += BASE_HALF_FRAME;
        slot = (size_t)s;
    }
    if (slot + BASE_SLOT > nd) {
        rc = 0;
        goto out;
    }
    /* The SSS of each PSS of the search, the best kept: a signal can start within the capture. */
    {
        size_t at, best_slot = slot;
        int o;

        match = -1;
        for (o = 0; o <= PSS_HALF_FRAMES; o++) {
            double pp = 0, ps = 0, m;
            bool sf5 = false;
            int id = 0;

            at = slot + (size_t)o * BASE_HALF_FRAME;
            if (at + BASE_SLOT > nd)
                break;
            m = sss_search(xd, at, &sync, sync.cfo_hz, &id, &sf5, &pp, &ps);
            if (m > match) {
                match     = m;
                best_slot = at;
                n_id_1    = id;
                subframe5 = sf5;
                pss_power = pp;
                sss_power = ps;
            }
        }
        slot = best_slot;
    }
    if (match < SSS_MIN_MATCH) {
        rc = 0;                  /* TDD and the extended cyclic prefix have their SSS elsewhere. */
        goto out;
    }
    res->n_id_1  = n_id_1;
    res->cell_id = 3 * n_id_1 + sync.n_id_2;
    res->pss_db  = m2sdr_power_db(pss_power);
    res->sss_db  = m2sdr_power_db(sss_power);

    /* First whole frame of the capture. */
    frame_base = (long)slot - (subframe5 ? BASE_HALF_FRAME : 0);
    while (frame_base < 1)
        frame_base += BASE_FRAME;
    frame = (size_t)frame_base * (size_t)dim.d;
    if (frame + (size_t)BASE_FRAME * (size_t)dim.d + (size_t)dim.n > count) {
        rc = 0;
        goto out;
    }
    res->found        = true;
    res->frame_offset = frame;

    /* Carrier offset: the cyclic prefixes give it finely modulo 15 kHz, the PSS which one. */
    fine = cp_cfo(x, frame, &dim);
    cfo  = fine + 15000.0 * round((sync.cfo_hz - fine) / 15000.0);
    res->freq_error_hz = cfo;

    /* The six central blocks first, for the PBCH: it tells how wide the carrier is. */
    if (grid_build(&grid, x, frame, &dim, cfo, 6) != 0 ||
        grid_channel(&grid, res->cell_id, 0, grid.h0) != 0 || grid_channel(&grid, res->cell_id, 1, grid.h1) != 0)
        goto out;
    (void)pbch_decode(&grid, res->cell_id, res);
    grid_free(&grid);

    rb = rb_of_rate[idx];
    if (res->mib && res->n_rb > 0 && res->n_rb < rb)
        rb = res->n_rb;
    if (grid_build(&grid, x, frame, &dim, cfo, rb) != 0 ||
        grid_channel(&grid, res->cell_id, 0, grid.h0) != 0 || grid_channel(&grid, res->cell_id, 1, grid.h1) != 0)
        goto out;
    res->analysed_rb = rb;
    res->subcarriers = grid.k;
    res->rsrp_db     = m2sdr_power_db(grid.rs_power[0]);
    res->rs_evm      = grid.rs_evm;
    res->freq_error_hz += grid.cfo_residual_hz;
    res->nrs         = grid.nrs;
    memcpy(res->rs_points, grid.rs_points, (size_t)grid.nrs * sizeof(grid.rs_points[0]));
    {
        double total = 0;

        for (q = 0; q < M2SDR_LTE_FRAME_SYMBOLS; q++) {
            for (a = 0; a < grid.k; a++) {
                const float complex v = grid.re[(size_t)q * (size_t)grid.k + (size_t)a];
                const double p = crealf(v) * crealf(v) + cimagf(v) * cimagf(v);

                res->grid_db[q][a] = (float)m2sdr_power_db(p);
                total += p;
            }
        }
        res->rssi_db = m2sdr_power_db(total / M2SDR_LTE_FRAME_SYMBOLS);
    }
    if (res->mib && res->ports <= 2)
        data_analyse(&grid, res->cell_id, res->n_rb, res->ports, res);
    rc = 0;

out:
    grid_free(&grid);
    free(xd);
    return rc;
}
