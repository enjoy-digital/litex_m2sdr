/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: 5G NR synchronisation signal blocks (3GPP TS 38.211, 38.213, 38.215).
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
#include "m2sdr_fft.h"
#include "m2sdr_lte_seq.h"

#define NR_SEQ          127                 /* Subcarriers of the PSS and of the SSS. */
#define NR_SYNC_FIRST   56                  /* Their first subcarrier in the SSB. */
#define NR_SSREF        120                 /* Subcarrier of the SSB at SSREF. */
#define NR_DMRS         144
#define NR_PBCH_DATA    432
#define MIN_FFT         256
#define MAX_FFT         4096
#define PSS_MIN_METRIC  16.0                /* Exponential of mean 1 for noise: 1e-7 a trial. */
#define SSS_MIN_MATCH   0.4
#define DMRS_MIN_MATCH  0.4
#define MAX_CAND        96
#define SMOOTH_HALF     2                   /* Pilots on each side in the smoothing of the channel. */

/* Sequences */
/*-----------*/

void m2sdr_nr_pss(int n_id_2, float *d)
{
    /* TS 38.211 7.4.2.2.1: x(i+7) = x(i+4) + x(i), [x(6) .. x(0)] = [1 1 1 0 1 1 0]. */
    uint8_t x[NR_SEQ];
    int i;

    x[0] = 0; x[1] = 1; x[2] = 1; x[3] = 0; x[4] = 1; x[5] = 1; x[6] = 1;
    for (i = 0; i + 7 < NR_SEQ; i++)
        x[i + 7] = x[i + 4] ^ x[i];
    for (i = 0; i < NR_SEQ; i++)
        d[i] = (float)(1 - 2 * x[(i + 43 * n_id_2) % NR_SEQ]);
}

void m2sdr_nr_sss(int n_id_1, int n_id_2, float *d)
{
    /* TS 38.211 7.4.2.3.1: x0(i+7) = x0(i+4) + x0(i), x1(i+7) = x1(i+1) + x1(i), both from
     * [x(6) .. x(0)] = [0 0 0 0 0 0 1]. */
    uint8_t x0[NR_SEQ], x1[NR_SEQ];
    const int m0 = 15 * (n_id_1 / 112) + 5 * n_id_2, m1 = n_id_1 % 112;
    int i;

    memset(x0, 0, sizeof(x0));
    memset(x1, 0, sizeof(x1));
    x0[0] = x1[0] = 1;
    for (i = 0; i + 7 < NR_SEQ; i++) {
        x0[i + 7] = x0[i + 4] ^ x0[i];
        x1[i + 7] = x1[i + 1] ^ x1[i];
    }
    for (i = 0; i < NR_SEQ; i++)
        d[i] = (float)((1 - 2 * x0[(i + m0) % NR_SEQ]) * (1 - 2 * x1[(i + m1) % NR_SEQ]));
}

void m2sdr_nr_pbch_dmrs(int pci, int i_ssb_bar, float complex *r)
{
    /* TS 38.211 7.4.1.4.1. */
    const uint32_t c_init = (1u << 11) * (uint32_t)(i_ssb_bar + 1) * (uint32_t)(pci / 4 + 1) +
                            (1u << 6) * (uint32_t)(i_ssb_bar + 1) + (uint32_t)(pci % 4);
    uint8_t c[2 * NR_DMRS];
    const float a = (float)M_SQRT1_2;
    int m;

    m2sdr_lte_gold(c_init, c, sizeof(c));
    for (m = 0; m < NR_DMRS; m++)
        r[m] = a * (float)(1 - 2 * c[2 * m]) + I * a * (float)(1 - 2 * c[2 * m + 1]);
}

/* Elements of the PBCH DM-RS (TS 38.211 7.4.3.1): symbol and subcarrier of r(m). */
static void dmrs_place(int pci, int m, int *l, int *k)
{
    const int v = pci % 4;

    if (m < 60) {
        *l = 1;
        *k = v + 4 * m;
    } else if (m < 84) {
        *l = 2;
        *k = m < 72 ? v + 4 * (m - 60) : 192 + v + 4 * (m - 72);
    } else {
        *l = 3;
        *k = v + 4 * (m - 84);
    }
}

/* A PBCH data element: symbols 1 and 3, and the ends of symbol 2. */
static bool pbch_data(int pci, int l, int k)
{
    if (l == 0 || (l == 2 && k >= 48 && k < 192))
        return false;
    return (k % 4) != pci % 4;
}

/* Search */
/*--------*/

struct nr_dims {
    int    n;                   /* FFT length. */
    int    cp;                  /* Normal cyclic prefix: 144/2048 of the symbol. */
    int    len;                 /* n + cp. */
    int    adv;                 /* The FFT window starts this early in the prefix. */
    double fs, scs;
};

struct nr_cand {
    long   p;                   /* Window of the search. */
    int    s;                   /* SSREF bin. */
    int    u;                   /* N_ID_2. */
    double metric;
};

static int cand_cmp(const void *a, const void *b)
{
    const double ma = ((const struct nr_cand *)a)->metric, mb = ((const struct nr_cand *)b)->metric;

    return ma < mb ? 1 : ma > mb ? -1 : 0;
}

/* PSS candidates: windows of the samples a quarter of a symbol apart, the products of
 * neighbouring bins of their spectra against those of the PSS (real, +-1), for every place of
 * the PSS, as a circular correlation through the FFT. The products leave out the phase that
 * a timing error turns along the subcarriers. */
static int pss_search(const float complex *x, size_t count, const struct nr_dims *dim, int smin, int smax,
                      struct nr_cand *cand, int *ncand)
{
    const int n = dim->n, hop = n / 4;
    struct m2sdr_fft *fwd = m2sdr_fft_alloc((size_t)n, false), *inv = m2sdr_fft_alloc((size_t)n, true);
    float complex *y = malloc((size_t)n * sizeof(*y)), *dk = malloc((size_t)n * sizeof(*dk));
    float complex *dfft = malloc((size_t)n * sizeof(*dfft)), *c = malloc((size_t)n * sizeof(*c));
    float complex *corr = malloc((size_t)n * sizeof(*corr));
    float complex *q[3] = { NULL, NULL, NULL };
    double *e = malloc((size_t)(2 * n + 1) * sizeof(*e));
    float d[NR_SEQ];
    int total = 0, u, rc = -1, k;
    long p;

    for (u = 0; u < 3; u++)
        q[u] = malloc((size_t)n * sizeof(*q[u]));
    if (!fwd || !inv || !y || !dk || !dfft || !c || !corr || !e || !q[0] || !q[1] || !q[2])
        goto out;
    /* Spectra of the products of the PSS, conjugated. */
    for (u = 0; u < 3; u++) {
        memset(dk, 0, (size_t)n * sizeof(*dk));
        m2sdr_nr_pss(u, d);
        for (k = 0; k + 1 < NR_SEQ; k++)
            dk[k] = d[k] * d[k + 1];
        m2sdr_fft_run(fwd, dk, q[u]);
        for (k = 0; k < n; k++)
            q[u][k] = conjf(q[u][k]);
    }
    for (p = 0; p + n <= (long)count; p += hop) {
        m2sdr_fft_run(fwd, x + p, y);
        for (k = 0; k + 1 < n; k++)
            dk[k] = y[k] * conjf(y[k + 1]);
        dk[n - 1] = y[n - 1] * conjf(y[0]);
        /* Energy of the products under each place, from running sums over twice the circle. */
        e[0] = 0;
        for (k = 0; k < n; k++)
            e[k + 1] = e[k] + crealf(dk[k] * conjf(dk[k]));
        for (k = 0; k < n; k++)
            e[n + k + 1] = e[n + k] + (e[k + 1] - e[k]);
        m2sdr_fft_run(fwd, dk, dfft);
        for (u = 0; u < 3; u++) {
            for (k = 0; k < n; k++)
                c[k] = dfft[k] * q[u][k];
            /* The correlation, n times too large. */
            m2sdr_fft_run(inv, c, corr);
            {
                int s;
                double prev = 0, cur = 0;

                for (s = smin - 1; s <= smax + 1; s++) {
                    const int a = ((s - 64) % n + n) % n;
                    const double en = e[a + NR_SEQ - 1] - e[a];
                    const double v = en > 0 ? crealf(corr[a] * conjf(corr[a])) / ((double)n * n) / en : 0.0;

                    /* Local maxima over the threshold, one bin late. */
                    if (s > smin && s - 1 <= smax && cur >= PSS_MIN_METRIC && cur >= prev && cur > v) {
                        if (total < MAX_CAND * 16) {
                            cand[total].p      = p;
                            cand[total].s      = s - 1;
                            cand[total].u      = u;
                            cand[total].metric = cur;
                            total++;
                        }
                    }
                    prev = cur;
                    cur  = v;
                }
            }
        }
    }
    *ncand = total;
    rc = 0;
out:
    m2sdr_fft_free(fwd);
    m2sdr_fft_free(inv);
    free(y);
    free(dk);
    free(dfft);
    free(c);
    free(corr);
    free(e);
    for (u = 0; u < 3; u++)
        free(q[u]);
    return rc;
}

/* One SSB */
/*---------*/

/* Time samples of a sequence of 127 on the subcarriers from SSB subcarrier 56, SSREF at bin s. */
static void sync_time(const struct m2sdr_fft *inv, int n, int s, const float *d, float complex *in, float complex *out)
{
    int i;

    memset(in, 0, (size_t)n * sizeof(*in));
    for (i = 0; i < NR_SEQ; i++)
        in[((s - NR_SSREF + NR_SYNC_FIRST + i) % n + n) % n] = d[i];
    m2sdr_fft_run(inv, in, out);
}

/* Carrier offset (Hz) of the samples at x + at from the known ones t: the turn of the phase
 * between the two halves of the symbol. The power of a symbol is not even over it, so the
 * halves are not exactly half a symbol apart: the estimate is short by a share of the offset,
 * and taken again with the offset found taken off. */
static double halves_cfo(const float complex *x, size_t at, const float complex *t, int n, double fs, double *weight)
{
    double f = 0;
    int it, i;

    for (it = 0; it < 4; it++) {
        const double complex step = cexp(-I * 2.0 * M_PI * f / fs);
        double complex a = 0, b = 0, rot = 1.0;

        for (i = 0; i < n; i++) {
            const double complex v = x[at + (size_t)i] * conjf(t[i]) * rot;

            if (i < n / 2)
                a += v;
            else
                b += v;
            rot *= step;
        }
        *weight = cabs(a) * cabs(b);
        f += carg(b * conj(a)) / (2.0 * M_PI * (n / 2) / fs);
    }
    return f;
}

/* The 4 x 240 elements of the SSB whose PSS starts (useful part) at t0, the carrier f taken off,
 * SSREF on bin 0, 1.0 for a full-scale subcarrier. */
static void ssb_grid(const float complex *x, size_t t0, const struct nr_dims *dim, double f, const struct m2sdr_fft *fwd,
                     float complex *in, float complex *out, float complex g[M2SDR_NR_SSB_SYMBOLS][M2SDR_NR_SSB_CARRIERS])
{
    int l, i, k;

    for (l = 0; l < M2SDR_NR_SSB_SYMBOLS; l++) {
        const size_t w = t0 + (size_t)l * (size_t)dim->len - (size_t)dim->adv;
        const double complex step = cexp(-I * 2.0 * M_PI * f / dim->fs);
        double complex rot = cexp(-I * 2.0 * M_PI * f * (double)w / dim->fs);

        for (i = 0; i < dim->n; i++) {
            in[i] = x[w + (size_t)i] * (float complex)rot;
            rot  *= step;
        }
        m2sdr_fft_run(fwd, in, out);
        for (k = 0; k < M2SDR_NR_SSB_CARRIERS; k++) {
            const int bin = k - NR_SSREF;
            /* The window starts early: the phase slope this gives is removed. */
            const float complex slope = (float complex)cexp(2.0 * M_PI * I * (double)bin * dim->adv / dim->n);

            g[l][k] = out[(bin % dim->n + dim->n) % dim->n] * slope / (float)dim->n;
        }
    }
}

/* Smoothing of the estimates of the channel at pilots, a phase slope taken off first; the
 * error of each against the mean of its neighbours (itself left out), the noise of that mean
 * taken off. Returns the number of pilots of the error. */
static int pilots_smooth(const float complex *ls, int np, float complex *sm, double *err, double *sig,
                         float complex *ratio)
{
    double complex turn = 0;
    double slope;
    float complex flat[NR_SEQ];
    int j, i, kept = 0;

    for (j = 0; j + 1 < np; j++)
        turn += ls[j + 1] * conjf(ls[j]);
    slope = cabs(turn) > 0 ? carg(turn) : 0.0;
    for (j = 0; j < np; j++)
        flat[j] = ls[j] * (float complex)cexp(-I * slope * (double)j);
    for (j = 0; j < np; j++) {
        const int j0 = j - SMOOTH_HALF < 0 ? 0 : j - SMOOTH_HALF;
        const int j1 = j + SMOOTH_HALF >= np ? np - 1 : j + SMOOTH_HALF;
        float complex sum = 0;

        for (i = j0; i <= j1; i++)
            sum += flat[i];
        sm[j] = sum / (float)(j1 - j0 + 1) * (float complex)cexp(I * slope * (double)j);
        if (j1 > j0) {
            const float complex others = (sum - flat[j]) / (float)(j1 - j0);
            const double mo = crealf(others * conjf(others));
            const int nb = j1 - j0;

            if (mo > 0) {
                const float complex e = flat[j] / others - 1.0f;

                if (err)
                    err[0] += crealf(e * conjf(e)) / (1.0 + 1.0 / nb);
                if (sig) {
                    const float complex d = flat[j] - others;

                    /* Noise of an element, and the power of the channel. */
                    sig[0] += crealf(d * conjf(d)) / (1.0 + 1.0 / nb);
                    sig[1] += crealf(flat[j] * conjf(flat[j]));
                }
                if (ratio)
                    ratio[kept] = flat[j] / others;
                kept++;
            }
        }
    }
    return kept;
}

/* Channel at subcarrier k from smoothed pilots at subcarriers k0 + step j: lines between them,
 * held at the ends. */
static float complex pilots_at(const float complex *sm, int np, int k0, int step, int k)
{
    const double pos = (double)(k - k0) / step;
    int j0 = (int)floor(pos);
    float t;

    if (np == 1)
        return sm[0];
    if (j0 < 0)
        j0 = 0;
    if (j0 > np - 2)
        j0 = np - 2;
    t = (float)(pos - j0);
    if (t < 0)
        t = 0;
    if (t > 1)
        t = 1;
    return sm[j0] * (1.0f - t) + sm[j0 + 1] * t;
}

struct nr_work {
    struct m2sdr_fft *fwd, *inv;
    float complex *in, *out, *pt;
    float complex g[M2SDR_NR_SSB_SYMBOLS][M2SDR_NR_SSB_CARRIERS];
};

/* Analysis of a candidate. Returns 1 with the SSB in *ssb (and its grid in w->g), 0 when it is
 * not one, -1 on an allocation failure. */
static int ssb_analyse(const float complex *x, size_t count, const struct nr_dims *dim, const struct nr_cand *c,
                       int lmax, struct nr_work *w, struct m2sdr_nr_ssb *ssb, struct m2sdr_nr_result *res)
{
    const int n = dim->n;
    float d[NR_SEQ], dsss[NR_SEQ];
    float complex dm[NR_DMRS];
    /* The window kept is the strongest of those on the PSS, which may hold half of it only
     * (a carrier offset of half a subcarrier weakens them all). */
    long t, t_lo = c->p - n / 2 - dim->cp, t_hi = c->p + n / 2 + dim->cp, t0 = -1;
    double best = -1, f, wa, wb, fa, fb;
    int s = c->s, i, l, k, id, best_id = 0, best_i = 0;

    if (t_lo < dim->cp)
        t_lo = dim->cp;
    if (t_hi + 3L * dim->len + n > (long)count)
        t_hi = (long)count - 3L * dim->len - n;
    if (t_hi < t_lo)
        return 0;

    /* Timing: the PSS in time against the samples, every other sample, then around the best.
     * Its band is under half of the sample rate: the peak is two samples wide at least. */
    m2sdr_nr_pss(c->u, d);
    sync_time(w->inv, n, s, d, w->in, w->pt);
    {
        long lo = t_lo, hi = t_hi, st = 2;
        int pass;

        for (pass = 0; pass < 2; pass++) {
            for (t = lo; t <= hi; t += st) {
                float re = 0, im = 0;

                for (i = 0; i < n; i++) {
                    const float complex a = x[t + i], b = w->pt[i];

                    re += crealf(a) * crealf(b) + cimagf(a) * cimagf(b);
                    im += cimagf(a) * crealf(b) - crealf(a) * cimagf(b);
                }
                if (re * re + im * im > best) {
                    best = re * re + im * im;
                    t0   = t;
                }
            }
            if (t0 < 0)
                return 0;
            lo = t0 - 1 < t_lo ? t_lo : t0 - 1;
            hi = t0 + 1 > t_hi ? t_hi : t0 + 1;
            st = 1;
        }
    }

    /* Carrier: the halves of the PSS against it on bin s, then the whole bins. */
    fa = s * dim->scs + halves_cfo(x, (size_t)t0, w->pt, n, dim->fs, &wa);
    f  = fa;
    s  = (int)lround(f / dim->scs);

    /* Cell group: the SSS against the channel of the PSS (each symbol has its own phase). */
    ssb_grid(x, (size_t)t0, dim, f, w->fwd, w->in, w->out, w->g);
    {
        float complex z[NR_SEQ];
        double norm = 0, second = 0;

        m2sdr_nr_pss(c->u, d);
        for (i = 0; i < NR_SEQ; i++) {
            z[i]  = w->g[2][NR_SYNC_FIRST + i] * conjf(w->g[0][NR_SYNC_FIRST + i] * d[i]);
            norm += cabsf(z[i]);
        }
        best = -1;
        for (id = 0; id < 336 && norm > 0; id++) {
            double complex acc = 0;
            double v;

            m2sdr_nr_sss(id, c->u, dsss);
            for (i = 0; i < NR_SEQ; i++)
                acc += z[i] * dsss[i];
            v = cabs(acc) / norm;
            if (v > best) {
                second  = best;
                best    = v;
                best_id = id;
            } else if (v > second) {
                second = v;
            }
        }
        if (best < SSS_MIN_MATCH)
            return 0;
    }
    memset(ssb, 0, sizeof(*ssb));
    ssb->n_id_1     = best_id;
    ssb->n_id_2     = c->u;
    ssb->pci        = 3 * best_id + c->u;
    ssb->pss_metric = c->metric;
    ssb->sss_match  = best;

    /* Carrier again with the SSS, then the grid with it. */
    m2sdr_nr_sss(best_id, c->u, dsss);
    sync_time(w->inv, n, s, dsss, w->in, w->out);
    fb = s * dim->scs + halves_cfo(x, (size_t)(t0 + 2L * dim->len), w->out, n, dim->fs, &wb);
    f  = wa + wb > 0 ? (fa * wa + fb * wb) / (wa + wb) : fa;
    ssb_grid(x, (size_t)t0, dim, f, w->fwd, w->in, w->out, w->g);
    ssb->freq_offset_hz = f;
    ssb->start          = (size_t)(t0 - dim->cp);
    ssb->time_s         = (double)ssb->start / dim->fs;

    /* Index: the 8 sequences of the DM-RS, symbol by symbol. */
    {
        double second = 0, bm = -1;

        for (id = 0; id < 8; id++) {
            double complex acc[4] = { 0, 0, 0, 0 };
            double norm = 0, v;

            m2sdr_nr_pbch_dmrs(ssb->pci, id, dm);
            for (i = 0; i < NR_DMRS; i++) {
                dmrs_place(ssb->pci, i, &l, &k);
                acc[l] += w->g[l][k] * conjf(dm[i]);
                norm   += cabsf(w->g[l][k]);
            }
            v = norm > 0 ? (cabs(acc[1]) + cabs(acc[2]) + cabs(acc[3])) / norm : 0.0;
            if (v > bm) {
                second = bm;
                bm     = v;
                best_i = id;
            } else if (v > second) {
                second = v;
            }
        }
        if (bm < DMRS_MIN_MATCH)
            return 0;
        ssb->dmrs_match = bm;
        if (lmax == 4) {
            ssb->ssb_index  = best_i & 3;
            ssb->half_frame = best_i >> 2;
        } else {
            ssb->ssb_index  = best_i;
            ssb->half_frame = -1;
        }
    }

    /* Figures. */
    {
        float complex ls[NR_SEQ], sm[NR_SEQ], ratio[NR_SEQ];
        double sig[2] = { 0, 0 }, err = 0, pw = 0, pe = 0, pr = 0;
        int kept, j;

        /* SSS: SS-SINR and SS-RSRP (TS 38.215 5.1.1 and 5.1.5). */
        for (i = 0; i < NR_SEQ; i++)
            ls[i] = w->g[2][NR_SYNC_FIRST + i] * dsss[i];
        kept = pilots_smooth(ls, NR_SEQ, sm, NULL, sig, NULL);
        if (kept > 0) {
            const double noise = sig[0] / kept, power = sig[1] / kept - noise;

            ssb->sinr_db = power > 0 && noise > 0 ? 10.0 * log10(power / noise) : 99.0;
            ssb->rsrp_db = 10.0 * log10(fmax(power, 1e-30));
        }
        for (i = 0; i < NR_SEQ; i++)
            pw += crealf(w->g[0][NR_SYNC_FIRST + i] * conjf(w->g[0][NR_SYNC_FIRST + i]));
        ssb->pss_db = 10.0 * log10(pw / NR_SEQ + 1e-30);

        /* DM-RS: each run of pilots of a symbol on its own; then the PBCH data equalised with
         * the lines between the smoothed pilots. */
        m2sdr_nr_pbch_dmrs(ssb->pci, best_i, dm);
        kept = 0;
        for (l = 1; l <= 3; l++) {
            /* Runs: 60 pilots in symbols 1 and 3, two of 12 in symbol 2. */
            const int nrun = l == 2 ? 2 : 1, np = l == 2 ? 12 : 60;
            int run;

            for (run = 0; run < nrun; run++) {
                const int m0 = l == 1 ? 0 : l == 3 ? 84 : 60 + 12 * run;
                const int k_lo = (l == 2 && run) ? 192 : 0, k_hi = (l == 2 && !run) ? 48 : 240;
                const int k0 = ssb->pci % 4 + k_lo;
                float complex lsd[60], smd[60];
                int got;

                for (j = 0; j < np; j++)
                    lsd[j] = w->g[l][k0 + 4 * j] * conjf(dm[m0 + j]);   /* |r| = 1. */
                got = pilots_smooth(lsd, np, smd, &err, NULL, ratio);
                for (j = 0; j < got && res->ndmrs < M2SDR_NR_MAX_POINTS; j += 2)
                    res->dmrs_points[res->ndmrs++] = ratio[j] * dm[m0 + j];
                kept += got;
                /* PBCH data of the run. */
                for (k = k_lo; k < k_hi; k++) {
                    float complex h, z, q;

                    if (!pbch_data(ssb->pci, l, k))
                        continue;
                    h = pilots_at(smd, np, k0, 4, k);
                    if (!(crealf(h * conjf(h)) > 0))
                        continue;
                    z = w->g[l][k] / h;
                    q = ((crealf(z) >= 0 ? 1.0f : -1.0f) + I * (cimagf(z) >= 0 ? 1.0f : -1.0f)) * (float)M_SQRT1_2;
                    pe += crealf((z - q) * conjf(z - q));
                    pr += 1.0;
                    if (res->npoints < M2SDR_NR_MAX_POINTS && ((int)pr % 2) == 0)
                        res->points[res->npoints++] = z;
                }
            }
        }
        ssb->dmrs_evm = kept ? sqrt(err / kept) : 0.0;
        ssb->pbch_evm = pr > 0 ? sqrt(pe / pr) : 0.0;
    }
    return 1;
}

/* Analysis */
/*----------*/

static int analyse_scs(const float complex *x, size_t count, double fs, double scs, const struct m2sdr_nr_config *cfg,
                       struct m2sdr_nr_result *res)
{
    struct nr_dims dim;
    struct nr_work *w = NULL;
    struct nr_cand *cand = NULL, keep[MAX_CAND];
    int ncand = 0, nkeep = 0, i, j, smin, smax, rc = -1;
    const int lmax = cfg && cfg->lmax ? cfg->lmax : 8;
    float complex (*grids)[M2SDR_NR_SSB_SYMBOLS][M2SDR_NR_SSB_CARRIERS] = NULL;

    dim.fs  = fs;
    dim.scs = scs;
    dim.n   = (int)lround(fs / scs);
    dim.cp  = dim.n * 9 / 128;
    dim.len = dim.n + dim.cp;
    dim.adv = dim.cp / 4;
    res->scs_hz  = scs;
    res->fft_len = dim.n;
    smin = -dim.n / 2 + NR_SSREF;
    smax = dim.n / 2 - NR_SSREF;
    if (cfg && cfg->search_hz > 0) {
        const int lim = (int)ceil(cfg->search_hz / scs);

        if (-lim > smin)
            smin = -lim;
        if (lim < smax)
            smax = lim;
    }
    if (smin > smax)
        return 0;

    cand  = malloc(MAX_CAND * 16 * sizeof(*cand));
    w     = calloc(1, sizeof(*w));
    grids = malloc(MAX_CAND * sizeof(*grids));
    if (!cand || !w || !grids)
        goto out;
    w->fwd = m2sdr_fft_alloc((size_t)dim.n, false);
    w->inv = m2sdr_fft_alloc((size_t)dim.n, true);
    w->in  = malloc((size_t)dim.n * sizeof(*w->in));
    w->out = malloc((size_t)dim.n * sizeof(*w->out));
    w->pt  = malloc((size_t)dim.n * sizeof(*w->pt));
    if (!w->fwd || !w->inv || !w->in || !w->out || !w->pt)
        goto out;
    if (pss_search(x, count, &dim, smin, smax, cand, &ncand) != 0)
        goto out;

    /* The strongest first; one per SSB and N_ID_2: the same one in the windows around it.
     * The sequences match an SSB in part from other places: the three are shifts of one
     * m-sequence, and so are the products of its neighbours and, with a carrier offset of half
     * a subcarrier, of its elements two apart (a match 31 subcarriers off, sometimes stronger
     * than the right one). These are kept, and left out by their SSS and DM-RS. */
    qsort(cand, (size_t)ncand, sizeof(*cand), cand_cmp);
    for (i = 0; i < ncand && nkeep < MAX_CAND; i++) {
        bool dup = false;

        for (j = 0; j < nkeep && !dup; j++) {
            const long dp = labs(cand[i].p - keep[j].p);
            const int ds = abs(cand[i].s - keep[j].s);

            if (dp < dim.len && ds <= 2 && cand[i].u == keep[j].u)
                dup = true;
        }
        if (!dup)
            keep[nkeep++] = cand[i];
    }

    res->nssb = 0;
    for (i = 0; i < nkeep && res->nssb < M2SDR_NR_MAX_SSB; i++) {
        struct m2sdr_nr_ssb ssb;
        int got = ssb_analyse(x, count, &dim, &keep[i], lmax, w, &ssb, res);
        bool dup = false;

        if (got < 0)
            goto out;
        if (got == 0)
            continue;
        /* The same SSB from two windows. */
        for (j = 0; j < res->nssb; j++)
            if (res->ssb[j].pci == ssb.pci && labs((long)res->ssb[j].start - (long)ssb.start) < dim.cp &&
                fabs(res->ssb[j].freq_offset_hz - ssb.freq_offset_hz) < scs / 2)
                dup = true;
        if (dup)
            continue;
        memcpy(grids[res->nssb], w->g, sizeof(w->g));
        res->ssb[res->nssb++] = ssb;
    }

    /* The strongest and its grid, the cells, then time order. */
    res->best  = -1;
    res->cells = 0;
    for (i = 0; i < res->nssb; i++) {
        bool seen = false;

        if (res->best < 0 || res->ssb[i].rsrp_db > res->ssb[res->best].rsrp_db)
            res->best = i;
        for (j = 0; j < i; j++)
            if (res->ssb[j].pci == res->ssb[i].pci)
                seen = true;
        if (!seen)
            res->cells++;
    }
    if (res->best >= 0) {
        const size_t best_start = res->ssb[res->best].start;
        const int best_pci = res->ssb[res->best].pci;
        int l, k;

        for (l = 0; l < M2SDR_NR_SSB_SYMBOLS; l++)
            for (k = 0; k < M2SDR_NR_SSB_CARRIERS; k++) {
                const float complex v = grids[res->best][l][k];

                res->grid_db[l][k] = (float)(10.0 * log10(crealf(v * conjf(v)) + 1e-20));
            }
        for (i = 1; i < res->nssb; i++) {
            for (j = i; j > 0 && res->ssb[j].start < res->ssb[j - 1].start; j--) {
                const struct m2sdr_nr_ssb tmp = res->ssb[j];

                res->ssb[j]     = res->ssb[j - 1];
                res->ssb[j - 1] = tmp;
            }
        }
        for (i = 0; i < res->nssb; i++)
            if (res->ssb[i].start == best_start && res->ssb[i].pci == best_pci)
                res->best = i;
        /* The offset of each SSB is from its own PSS and SSS: the mean over those of the cell
         * at the same place is the figure of the carrier. */
        {
            const double fb = res->ssb[res->best].freq_offset_hz;
            double sum = 0;
            int nf = 0;

            for (i = 0; i < res->nssb; i++)
                if (res->ssb[i].pci == best_pci && fabs(res->ssb[i].freq_offset_hz - fb) < scs / 2) {
                    sum += res->ssb[i].freq_offset_hz;
                    nf++;
                }
            res->freq_error_hz = sum / nf;
        }
    }
    res->found = res->nssb > 0;
    rc = 0;
out:
    if (w) {
        m2sdr_fft_free(w->fwd);
        m2sdr_fft_free(w->inv);
        free(w->in);
        free(w->out);
        free(w->pt);
    }
    free(w);
    free(cand);
    free(grids);
    return rc;
}

static bool fft_size_ok(double fs, double scs)
{
    const double r = fs / scs;
    const long n = lround(r);

    return fabs(r - (double)n) < 1e-6 * r && n >= MIN_FFT && n <= MAX_FFT && (n & (n - 1)) == 0;
}

int m2sdr_nr_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_nr_config *cfg,
                     struct m2sdr_nr_result *res)
{
    /* 30 kHz first: the spacing of the SSBs of the TDD bands (n77, n78), the most deployed. */
    static const double spacings[2] = { 30000.0, 15000.0 };
    struct m2sdr_nr_result *tmp = NULL;
    double power = 0;
    size_t i;
    int k, tried = 0, rc = 0;

    if (!x || !res)
        return -1;
    memset(res, 0, sizeof(*res));
    res->best = -1;
    if (cfg && cfg->scs_hz > 0 && !fft_size_ok(sample_rate, cfg->scs_hz))
        return -1;
    if (cfg && cfg->lmax && cfg->lmax != 4 && cfg->lmax != 8 && cfg->lmax != 64)
        return -1;
    for (k = 0; k < 2; k++)
        if ((!cfg || cfg->scs_hz <= 0 || cfg->scs_hz == spacings[k]) && fft_size_ok(sample_rate, spacings[k]))
            tried++;
    if (!tried)
        return -1;
    for (k = 0; k < 2; k++) {
        const int n = (int)lround(sample_rate / spacings[k]);

        if ((!cfg || cfg->scs_hz <= 0 || cfg->scs_hz == spacings[k]) && fft_size_ok(sample_rate, spacings[k]) &&
            count < (size_t)(4 * (n + n * 9 / 128) + n))
            return -1;
    }
    for (i = 0; i < count; i++)
        power += crealf(x[i] * conjf(x[i]));

    tmp = malloc(sizeof(*tmp));
    if (!tmp)
        return -1;
    for (k = 0; k < 2; k++) {
        if ((cfg && cfg->scs_hz > 0 && cfg->scs_hz != spacings[k]) || !fft_size_ok(sample_rate, spacings[k]))
            continue;
        memset(tmp, 0, sizeof(*tmp));
        tmp->best = -1;
        if (analyse_scs(x, count, sample_rate, spacings[k], cfg, tmp) != 0) {
            rc = -1;
            break;
        }
        /* The first spacing with SSBs: a capture holds the SSBs of one carrier. */
        memcpy(res, tmp, sizeof(*res));
        if (res->found)
            break;
    }
    free(tmp);
    res->power_dbfs = 10.0 * log10(power / (double)count + 1e-30);
    res->mib        = false;
    return rc;
}
