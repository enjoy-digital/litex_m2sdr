/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: DVB-T2 reception (EN 302 755), 1k to 32k.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_dvbt2.h"
#include "m2sdr_fft.h"

#define P1_LEN              2048    /* P1: C (542 samples), A (1024), B (482), at any FFT size. */
#define P1_A                1024
#define P1_C                542
#define P1_B                482
#define MIN_P1_QUALITY      0.4     /* Correlation of the C-A-B parts; noise and data read about 0.05. */
#define MIN_SYNC_QUALITY    0.08    /* Correlation of the guard intervals; noise reads about 0.01. */
#define MIN_PATTERN_QUALITY 0.3     /* Coherence of the scattered pilots Dy symbols apart. */
#define MIN_PILOT_QUALITY   0.3     /* Coherence of neighbour pilots with their PRBS; noise about 0.05. */
#define MIN_COUNT           10240   /* Eight 1k symbols of guard 1/4. */
#define MAX_SYMBOLS         160
#define MAX_CELLS           (32 * 32768)   /* Symbols times FFT points analysed: the time and the memory of an analysis. */
#define SYNC_SPAN           300000  /* Samples the guard search looks at: eight 32k symbols. */
#define SEARCH_SYMBOLS      32      /* Symbols of the pattern and offset searches. */
#define SEARCH_CARRIERS     6144    /* Carriers of the pattern search: 64 pilots of a symbol at least. */
#define OFFSET_SEARCH       32      /* Carriers searched on each side of the edges of the spectrum. */
#define CP_MIN_SYMBOLS      8       /* Symbols to tell a continual pilot from a data carrier. */
#define CP_COHERENCE        0.8     /* Of a continual pilot from symbol to symbol; data reads 1 / symbols. */
#define CP_NOISE_REJECT     10.0    /* A continual pilot this far over the median noise is a data carrier. */
#define DENSE_RATIO         1.4     /* Frame closing symbol: pilots on every Dx-th carrier, 1.8 to 5.4 times the data power. */
#define FIT_CELLS           16384   /* Data cells the constellation is found on: its fit to 1 %. */
#define MIN_FIT             1.6     /* A constellation fits: its slicing error this far under that of an even spread
                                     * of values; a wrong one reads 0.8 to 1.3. */
#define ROTATED_PREFER      1.3     /* A rotated one fits this much better than the best square one: rotated QPSK
                                     * and 16QAM have levels 0.04 apart. */
#define ROTATED_FIT         3.0     /* No grid where a square 256QAM would fit this well: rotated. */
#define MODELS              8       /* QPSK to 256QAM, square then rotated. */
#define LEVEL_LUT           2048
#define LEVEL_RANGE         1.6f    /* Of the levels of a constellation of mean power 1, with margin. */
#define ECHO_FFT_MIN        4096
#define ECHO_MAX_FFTS       32

static const int fft_sizes[6]       = { 1024, 2048, 4096, 8192, 16384, 32768 };
static const int carriers_normal[6] = { 853, 1705, 3409, 6817, 13633, 27265 };
static const int carriers_ext[6]    = { 0, 0, 0, 6913, 13921, 27841 };
static const int p2_symbols[6]      = { 16, 8, 4, 2, 1, 1 };
static const int guards[7][2]       = { { 1, 128 }, { 1, 32 }, { 1, 16 }, { 19, 256 }, { 1, 8 }, { 19, 128 }, { 1, 4 } };
/* Guard intervals of each FFT size, bit g for guards[g]: the combinations of the standard as
 * remembered (the 1k row, 1/16 to 1/4, the least sure). */
static const unsigned guard_allowed[6] = { 0x54, 0x56, 0x56, 0x7f, 0x7f, 0x3f };

/* Scattered pilot patterns: carrier k of symbol l is a pilot when k mod (Dx Dy) = Dx (l mod Dy),
 * of amplitude a. */
struct t2_pattern {
    int   dx, dy;
    float a;
};

static const struct t2_pattern patterns[8] = {
    {  3,  4, 4.0f / 3.0f }, {  6,  2, 4.0f / 3.0f }, {  6, 4, 7.0f / 4.0f }, { 12, 2, 7.0f / 4.0f },
    { 12,  4, 7.0f / 3.0f }, { 24,  2, 7.0f / 3.0f }, { 24, 4, 7.0f / 3.0f }, {  6, 16, 7.0f / 3.0f },
};

static const enum m2sdr_modulation model_mod[4] = { M2SDR_MOD_QPSK, M2SDR_MOD_16QAM, M2SDR_MOD_64QAM, M2SDR_MOD_256QAM };
/* Rotation of the rotated constellations, degrees: 256QAM is atan(1/16). */
static const double model_angle[4] = { 29.0, 16.8, 8.6, 3.576334 };

struct t2_dims {
    int    n;                     /* FFT length. */
    int    k;                     /* Carriers. */
    int    gl;                    /* Guard interval, samples. */
    int    l;                     /* Symbol: n + gl. */
    double df;                    /* Carrier spacing, Hz. */
};

/* Sequence of the pilots: x^11 + x^2 + 1 from all ones, one bit per carrier from the first one
 * (in extended mode too: the first carrier of the extension, as understood). Pilot k of symbol
 * l is a (1 - 2 (w[k] xor p[l])); p[l], a sequence per symbol, is not needed: a symbol turned by
 * pi is found as any other turn. */
static void pilot_prbs(uint8_t *w, int k)
{
    unsigned reg = 0x7ff;
    int i;

    for (i = 0; i < k; i++) {
        const unsigned out = (reg >> 10) & 1u;

        w[i] = (uint8_t)out;
        reg = ((reg << 1) | (out ^ ((reg >> 8) & 1u))) & 0x7ffu;
    }
}

static float pw2(float complex v)
{
    return crealf(v) * crealf(v) + cimagf(v) * cimagf(v);
}

/* a b and a b*. The samples and the products here are finite: the products of the loops written
 * out, without the NaN and infinity handling of C99 (__mulsc3) that GCC calls for a * b. */
static inline float complex cmul(float complex a, float complex b)
{
    return (crealf(a) * crealf(b) - cimagf(a) * cimagf(b)) + I * (crealf(a) * cimagf(b) + cimagf(a) * crealf(b));
}

static inline float complex cmulc(float complex a, float complex b)
{
    return (crealf(a) * crealf(b) + cimagf(a) * cimagf(b)) + I * (cimagf(a) * crealf(b) - crealf(a) * cimagf(b));
}

/* P1 */
/*----*/

/* P1 symbols: C is the first 542 samples of A and B its last 482, both shifted up by one
 * carrier of the 1k FFT (fs / 1024), C before A and B after it. A sample of C against the one
 * 542 later, and of B against the one 482 earlier, both turned back by the shift, add up at the
 * start of a P1. Returns the number found, their first samples in pos; the quality and the
 * frequency (cycles per sample, modulo 1 / 1024) of the best. */
#define P1_RING 1024u

static int p1_search(const float complex *x, size_t count, size_t *pos, int max, double *quality, double *freq)
{
    float complex rot[P1_A], ur[P1_RING], vr[P1_RING];
    double complex sc = 0, sb = 0, cur_v = 0;
    double ec = 0, eb = 0, cur_q = 0;
    size_t n, m, cur_n = 0, skip = 0;
    int i, found = 0;
    bool have = false;

    *quality = 0;
    *freq    = 0;
    if (count <= P1_LEN)
        return 0;
    /* x[j] turned back by the shift, conjugated, against the sample it repeats: u for C, v for B,
     * kept in rings until they leave the sums. */
    for (i = 0; i < P1_A; i++)
        rot[i] = (float complex)cexp(-I * 2.0 * M_PI * i / P1_A);
#define P1_U(j) cmulc(x[(j) + P1_C], cmul(x[j], rot[(j) % P1_A]))
#define P1_V(j) cmulc(x[(j) - P1_B], cmul(x[j], rot[(j) % P1_A]))
    for (m = 0; m < P1_C; m++) {
        ur[m % P1_RING] = P1_U(m);
        sc += ur[m % P1_RING];
        ec += 0.5 * (pw2(x[m]) + pw2(x[m + P1_C]));
    }
    for (m = P1_C + P1_A; m < P1_LEN; m++) {
        vr[m % P1_RING] = P1_V(m);
        sb += vr[m % P1_RING];
        eb += 0.5 * (pw2(x[m]) + pw2(x[m - P1_B]));
    }
    for (n = 0; n + P1_LEN <= count; n++) {
        const double e = ec + eb, sc2 = creal(sc) * creal(sc) + cimag(sc) * cimag(sc);
        const double sb2 = creal(sb) * creal(sb) + cimag(sb) * cimag(sb);

        /* (|sc| + |sb|)^2 <= 2 (|sc|^2 + |sb|^2): the square roots only near a P1. */
        if (e > 0 && n >= skip && 2.0 * (sc2 + sb2) > MIN_P1_QUALITY * MIN_P1_QUALITY * e * e) {
            const double q = (sqrt(sc2) + sqrt(sb2)) / e;

            if (q > MIN_P1_QUALITY && (!have || q > cur_q)) {
                have  = true;
                cur_q = q;
                cur_n = n;
                cur_v = sc * conj(sb);
            }
        }
        if (have && (n >= cur_n + P1_LEN / 2 || n + P1_LEN == count)) {
            if (found < max)
                pos[found] = cur_n;
            found++;
            if (cur_q > *quality) {
                *quality = cur_q;
                *freq    = carg(cur_v) / (2.0 * M_PI * P1_A);
            }
            have = false;
            skip = cur_n + P1_LEN;
        }
        if (n + P1_LEN == count)
            break;
        {
            const size_t mu = n + P1_C, mv = n + P1_LEN;
            const float complex u = P1_U(mu), v = P1_V(mv);

            sc += u - ur[n % P1_RING];
            sb += v - vr[(n + P1_C + P1_A) % P1_RING];
            ur[mu % P1_RING] = u;
            vr[mv % P1_RING] = v;
        }
        ec += 0.5 * (pw2(x[n + 2 * P1_C]) - pw2(x[n]));
        eb += 0.5 * (pw2(x[n + P1_LEN]) - pw2(x[n + P1_C + P1_A - P1_B]));
    }
#undef P1_U
#undef P1_V
    return found < max ? found : max;
}

/* Synchronisation */
/*-----------------*/

/* Guard intervals: prod[i] = x[i] x*[i + n], folded on the symbol period l and summed over a
 * guard interval. cum[i] is the power of the first i samples. Returns its peak over the power,
 * 0 to 1, with the start of a guard interval and the fractional carrier offset of the peak. */
static double guard_fold(const float complex *prod, const double *cum, size_t len, int n, int gl,
                         float complex *fold, size_t *t0, double *eps)
{
    const int l = n + gl;
    const size_t nsym = (len - (size_t)n) / (size_t)l;
    double complex m = 0, best_m = 0;
    double pw, best = -1;
    size_t s;
    int i, t, best_t = 0;

    memset(fold, 0, (size_t)l * sizeof(*fold));
    for (s = 0; s < nsym; s++) {
        const float complex *a = prod + s * (size_t)l;

        for (i = 0; i < l; i++)
            fold[i] += a[i];
    }
    pw = 0.5 * (cum[nsym * (size_t)l] - cum[0] + cum[nsym * (size_t)l + (size_t)n] - cum[n]);
    for (i = 0; i < gl; i++)
        m += fold[i];
    for (t = 0; t < l; t++) {
        if (cabs(m) > best) {
            best   = cabs(m);
            best_m = m;
            best_t = t;
        }
        m += fold[(t + gl) % l] - fold[t];
    }
    *t0  = (size_t)best_t;
    *eps = carg(best_m) / (2.0 * M_PI);
    return pw > 0 ? best / (pw / l * gl) : 0;
}

/* FFT bin of carrier c, the multiplex off by `off` carriers. */
static int carrier_bin(const struct t2_dims *d, int c, int off)
{
    return ((c - (d->k - 1) / 2 + off) % d->n + d->n) % d->n;
}

/* The carriers of the symbols y, the multiplex off by `off` carriers. */
static void carriers_take(const float complex *y, float complex *cs, int nsym, const struct t2_dims *d, int off)
{
    int s, c;

    for (s = 0; s < nsym; s++) {
        const float complex *a = y + (size_t)s * (size_t)d->n;
        float complex *o = cs + (size_t)s * (size_t)d->k;
        int b = carrier_bin(d, 0, off);

        for (c = 0; c < d->k; c++) {
            o[c] = a[b];
            b = b + 1 == d->n ? 0 : b + 1;
        }
    }
}

/* Window of k bins of the most power in p (bins from the lowest frequency): its first bin. */
static int band_window(const float *p, int n, int k)
{
    double sum = 0, best;
    int i, best_i = 0;

    for (i = 0; i < k; i++)
        sum += p[i];
    best = sum;
    for (i = 1; i + k <= n; i++) {
        sum += p[i + k - 1] - p[i - 1];
        if (sum > best) {
            best   = sum;
            best_i = i;
        }
    }
    return best_i;
}

static double band_mean(const float *p, int a, int b)
{
    double sum = 0;
    int i;

    for (i = a; i < b; i++)
        sum += p[i];
    return b > a ? sum / (b - a) : 0;
}

/* Pilots */
/*--------*/

/* Scattered pilot pattern: the pilots of symbol l are at the same carriers in symbol l - Dy, so
 * they correlate from one to the other whatever their sequence, data carriers do not. For each
 * pattern and shift sigma (pilots of symbol l at sigma + Dx (l mod Dy), modulo Dx Dy, in the
 * carriers of cs), num / den is that coherence and num^2 / den the coherent pilot power: a
 * pattern with fewer pilots than the right one (a subset of it) has the same coherence but less
 * power, one with more has the same power but less coherence. Returns the pattern of the most,
 * or -1. */
static int pattern_search(const float complex *cs, const float *pw, int nsym, int k, int only, int *sigma,
                          double *quality)
{
    const int ns = nsym < SEARCH_SYMBOLS ? nsym : SEARCH_SYMBOLS, kc = k < SEARCH_CARRIERS ? k : SEARCH_CARRIERS;
    double best = 0;
    int p, best_p = -1;

    *quality = 0;
    *sigma   = 0;
    for (p = 0; p < 8; p++) {
        const int dx = patterns[p].dx, dy = patterns[p].dy, dd = dx * dy;
        int sg;

        if ((only && only != p + 1) || ns < dy + 2)
            continue;
        for (sg = 0; sg < dd; sg++) {
            double num = 0, den = 0, score;
            int l, c;

            for (l = dy; l < ns; l++) {
                const float complex *a = cs + (size_t)l * (size_t)k, *b = a - (size_t)dy * (size_t)k;
                const float *pa = pw + (size_t)l * (size_t)k, *pb = pa - (size_t)dy * (size_t)k;
                float complex sum = 0;
                float ea = 0, eb = 0;

                for (c = (sg + dx * (l % dy)) % dd; c < kc; c += dd) {
                    sum += cmulc(a[c], b[c]);
                    ea  += pa[c];
                    eb  += pb[c];
                }
                num += cabsf(sum);
                den += sqrt((double)ea * eb);
            }
            if (den <= 0)
                continue;
            score = num * num / (den * (ns - dy));
            if (score > best) {
                best     = score;
                best_p   = p;
                *sigma   = sg;
                *quality = num / den;
            }
        }
    }
    return best_p;
}

/* Integer carrier offset: the carriers of cs are those of the multiplex shifted by d, the
 * pilots then at multiples of Dx; neighbour pilots of a symbol correlate once their PRBS is
 * taken off, at the right d only. d in [lo, hi]; returns the best, its coherence in quality. */
static int prbs_offset(const float complex *cs, int nsym, int k, const struct t2_pattern *pt, int sigma,
                       const uint8_t *w, int lo, int hi, double *quality)
{
    const int dd = pt->dx * pt->dy, ns = nsym < SEARCH_SYMBOLS ? nsym : SEARCH_SYMBOLS;
    double best = -1;
    int d, best_d = 0;

    for (d = lo; d <= hi; d++) {
        double num = 0, den = 0;
        int l, c;

        if (((sigma + d) % pt->dx + pt->dx) % pt->dx)
            continue;
        for (l = 0; l < ns; l++) {
            const float complex *a = cs + (size_t)l * (size_t)k;
            float complex sum = 0;

            for (c = (sigma + pt->dx * (l % pt->dy)) % dd; c + dd < k; c += dd) {
                float complex v;

                if (c + d < 0 || c + dd + d >= k)
                    continue;
                v = cmulc(a[c + dd], a[c]);
                if (w[c + d] != w[c + dd + d])
                    v = -v;
                sum += v;
                den += cabsf(v);
            }
            num += cabsf(sum);
        }
        if (den > 0 && num / den > best) {
            best   = num / den;
            best_d = d;
        }
    }
    *quality = best > 0 ? best : 0;
    return best_d;
}

/* The channel on a grid of carriers 0, Dx, 2 Dx... and the last carrier: the pilots of the last
 * Dy symbols. */
struct t2_grid {
    int            dx, dy, g0, k, ngrid, gn;   /* gn: ngrid, and one more for the last carrier off the grid. */
    float          a;
    float complex *h;
    uint8_t       *valid;
    int           *list;
};

static int grid_carrier(const struct t2_grid *g, int i)
{
    return i < g->ngrid ? i * g->dx : g->k - 1;
}

/* Points of the grid with a pilot in symbol s: its scattered pilots and the edge pilots. */
static int grid_pilots(const struct t2_grid *g, int s)
{
    int i, n = 0;

    g->list[n++] = 0;
    for (i = (g->g0 + s) % g->dy; i < g->ngrid; i += g->dy)
        if (i != 0)
            g->list[n++] = i;
    if (g->gn > g->ngrid || (g->list[n - 1] != g->ngrid - 1))
        g->list[n++] = g->gn - 1;
    return n;
}

/* Pilots of symbol s: its scattered pilots and the edge pilots, 1 in mask. */
static void grid_mask(const struct t2_grid *g, int s, uint8_t *mask)
{
    int i;

    memset(mask, 0, (size_t)g->k);
    for (i = (g->g0 + s) % g->dy; i < g->ngrid; i += g->dy)
        mask[i * g->dx] = 1;
    mask[0] = mask[g->k - 1] = 1;
}

/* Channel at point i of the grid from the nearest valid points around it; 0 with none. */
static float complex grid_interp(const struct t2_grid *g, int i)
{
    int lo = i - 1, hi = i + 1;

    while (lo >= 0 && !g->valid[lo])
        lo--;
    while (hi < g->gn && !g->valid[hi])
        hi++;
    if (lo >= 0 && hi < g->gn) {
        const float r = (float)(grid_carrier(g, i) - grid_carrier(g, lo)) /
                        (float)(grid_carrier(g, hi) - grid_carrier(g, lo));

        return g->h[lo] + (g->h[hi] - g->h[lo]) * r;
    }
    return lo >= 0 ? g->h[lo] : hi < g->gn ? g->h[hi] : 0;
}

/* Channel on every carrier, linear between the points of the grid. */
static void grid_fill(const struct t2_grid *g, float complex *hc)
{
    int i, c;

    for (i = 0; i + 1 < g->gn; i++) {
        const int c0 = grid_carrier(g, i), c1 = grid_carrier(g, i + 1);
        const float complex step = (g->h[i + 1] - g->h[i]) / (float)(c1 - c0);
        float complex h = g->h[i];

        for (c = c0; c < c1; c++) {
            hc[c] = h;
            h += step;
        }
    }
    hc[g->k - 1] = g->h[g->gn - 1];
}

/* Constellations */
/*----------------*/

/* Nearest point of a square constellation of side m (2 to 16), mean power 1. */
static float complex qam_slice(float complex z, int m, float scale)
{
    float v[2] = { crealf(z) / scale, cimagf(z) / scale };
    int i;

    for (i = 0; i < 2; i++) {
        /* Nearest odd level: floor without a call, for |v| < 64. */
        const int f = (int)(0.5f * v[i] + 64.0f) - 64;
        const float q = (float)(2 * f + 1);

        v[i] = q > m - 1 ? (float)(m - 1) : q < 1 - m ? (float)(1 - m) : q;
    }
    return (v[0] + I * v[1]) * scale;
}

static float qam_scale(int m)
{
    /* Mean power of the odd levels of a side of m points: (m^2 - 1) / 3, per axis. */
    return 1.0f / sqrtf(2.0f * (m * m - 1) / 3.0f);
}

/* A rotated constellation once the Q of each cell is that of another cell: on each axis, the
 * m^2 levels a cos(t) - b sin(t) of the side levels a and b, sorted, with a table of the nearest
 * to a value. u is the mean square distance to the nearest level of a value spread evenly over
 * them. */
struct t2_levels {
    int      count;
    float    v[256];
    uint16_t lut[LEVEL_LUT];
    double   u;
};

static int float_cmp(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;

    return x < y ? -1 : x > y;
}

static void levels_init(struct t2_levels *lv, int m, double angle_deg)
{
    const float scale = qam_scale(m), c = (float)cos(angle_deg * M_PI / 180.0), s = (float)sin(angle_deg * M_PI / 180.0);
    double g3 = 0, g1 = 0;
    int a, b, i, j = 0;

    lv->count = 0;
    for (a = 1 - m; a < m; a += 2)
        for (b = 1 - m; b < m; b += 2)
            lv->v[lv->count++] = (a * c - b * s) * scale;
    qsort(lv->v, (size_t)lv->count, sizeof(lv->v[0]), float_cmp);
    for (i = 1; i < lv->count; i++) {
        const double gap = lv->v[i] - lv->v[i - 1];

        g3 += gap * gap * gap;
        g1 += gap;
    }
    lv->u = 2.0 * g3 / 12.0 / g1;
    for (i = 0; i < LEVEL_LUT; i++) {
        const float x = -LEVEL_RANGE + (i + 0.5f) * (2.0f * LEVEL_RANGE / LEVEL_LUT);

        while (j + 1 < lv->count && fabsf(lv->v[j + 1] - x) <= fabsf(lv->v[j] - x))
            j++;
        lv->lut[i] = (uint16_t)j;
    }
}

static float levels_slice(const struct t2_levels *lv, float x)
{
    int i = (int)((x + LEVEL_RANGE) * (LEVEL_LUT / (2.0f * LEVEL_RANGE)));
    int j;
    float best;

    i = i < 0 ? 0 : i >= LEVEL_LUT ? LEVEL_LUT - 1 : i;
    j = lv->lut[i];
    best = lv->v[j];
    if (j > 0 && fabsf(lv->v[j - 1] - x) < fabsf(best - x))
        best = lv->v[j - 1];
    if (j + 1 < lv->count && fabsf(lv->v[j + 1] - x) < fabsf(best - x))
        best = lv->v[j + 1];
    return best;
}

/* Error of z against model i: 0 to 3 square QPSK to 256QAM, 4 to 7 the same rotated. */
static float model_error(const struct t2_levels *lv, int i, float complex z)
{
    if (i < 4) {
        static const float scale[4] = { 0.70710678f, 0.31622777f, 0.15430335f, 0.07669650f };   /* qam_scale(). */
        const float complex q = qam_slice(z, 2 << i, scale[i]);

        return pw2(z - q);
    } else {
        const float a = crealf(z) - levels_slice(&lv[i - 4], crealf(z));
        const float b = cimagf(z) - levels_slice(&lv[i - 4], cimagf(z));

        return a * a + b * b;
    }
}

static double model_u(const struct t2_levels *lv, int i)
{
    if (i < 4) {
        const double step = 2.0 * qam_scale(2 << i);

        return 2.0 * step * step / 12.0;
    }
    return lv[i - 4].u;
}

static int double_cmp(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : x > y;
}

/* Analysis */
/*----------*/

int m2sdr_dvbt2_analyse(const float complex *x, size_t count, double sample_rate,
                        const struct m2sdr_dvbt2_config *cfg, struct m2sdr_dvbt2_result *res)
{
    static const double rates[3] = { 64e6 / 7.0, 8e6, 48e6 / 7.0 };
    struct t2_dims d;
    struct t2_grid grid;
    struct t2_levels *lv = NULL;
    struct m2sdr_fft *fft = NULL, *efft = NULL;
    float complex *y = NULL, *cs = NULL, *hc = NULL, *prod = NULL, *fold = NULL, *ein = NULL, *eout = NULL, *zsum = NULL;
    double *cum = NULL, *err = NULL, *ref = NULL, *hpow = NULL, *echo = NULL, *csum2 = NULL, *zpow = NULL, *noise = NULL;
    double complex *csum = NULL;
    float *pw = NULL;
    uint8_t *w = NULL, *use = NULL, *cp = NULL, *mask = NULL;
    int *zn = NULL;
    size_t seg0 = 0, seg1 = count, t0 = 0, best_t0 = 0, j;
    double eps = 0, best_eps = 0, best_q = 0, power = 0, p1f = 0;
    int i, s, c, m, nsym = 0, rc = -1, best_size = -1, best_g = 0, after = -1, skip = 0;
    int pat, sigma = 0, off_c, off, dd, b0, nused = 0, necho = 0, efft_n = ECHO_FFT_MIN;
    bool rate_ok = false;

    memset(&grid, 0, sizeof(grid));
    if (!x || !res)
        return -1;
    memset(res, 0, sizeof(*res));
    res->modulation = M2SDR_MOD_AUTO;
    for (i = 0; i < 3; i++)
        rate_ok |= fabs(sample_rate - rates[i]) < 2e-3 * rates[i];
    if (!rate_ok || count < MIN_COUNT)
        return -1;
    for (j = 0; j < count; j++)
        power += pw2(x[j]);
    res->power_dbfs = 10.0 * log10(power / (double)count + 1e-30);

    /* P1 symbols, and the longest stretch of the capture without one: the symbols there follow
     * each other with no break. */
    res->p1_count = p1_search(x, count, res->p1_sample, M2SDR_DVBT2_MAX_P1, &res->p1_quality, &p1f);
    res->p1_freq_hz = p1f * sample_rate;
    if (res->p1_count >= 2)
        res->frame_ms = (double)(res->p1_sample[res->p1_count - 1] - res->p1_sample[0]) /
                        (res->p1_count - 1) / sample_rate * 1e3;
    if (res->p1_count)
        seg1 = res->p1_sample[0];
    for (i = 0; i < res->p1_count; i++) {
        const size_t a = res->p1_sample[i] + P1_LEN, b = i + 1 < res->p1_count ? res->p1_sample[i + 1] : count;

        if (b > a && b - a > seg1 - seg0) {
            seg0  = a;
            seg1  = b;
            after = i;
        }
    }

    /* FFT size and guard interval: the best correlation of the guard intervals, or the ones given. */
    {
        const size_t span = seg1 - seg0 < SYNC_SPAN ? seg1 - seg0 : SYNC_SPAN;
        const float complex *a = x + seg0;

        cum  = malloc((span + 1) * sizeof(*cum));
        prod = malloc((span ? span : 1) * sizeof(*prod));
        fold = malloc((32768 + 32768 / 4) * sizeof(*fold));
        if (!cum || !prod || !fold)
            goto out;
        cum[0] = 0;
        for (j = 0; j < span; j++)
            cum[j + 1] = cum[j] + pw2(a[j]);
        for (i = 0; i < 6; i++) {
            const int n = fft_sizes[i];
            int g;

            if (cfg && cfg->fft_len && cfg->fft_len != n)
                continue;
            if (span < (size_t)n + 4 * (size_t)(n + n / 128))
                continue;
            for (j = 0; j + (size_t)n < span; j++)
                prod[j] = cmulc(a[j], a[j + (size_t)n]);
            for (g = 0; g < 7; g++) {
                const int gl = n / guards[g][1] * guards[g][0];
                double q;

                if (!(guard_allowed[i] & (1u << g)))
                    continue;
                if (cfg && cfg->fft_len && cfg->guard_den &&
                    (cfg->guard_num != guards[g][0] || cfg->guard_den != guards[g][1]))
                    continue;
                if (span < (size_t)n + 4 * (size_t)(n + gl))
                    continue;
                q = guard_fold(prod, cum, span, n, gl, fold, &t0, &eps);
                if (q > best_q) {
                    best_q    = q;
                    best_size = i;
                    best_g    = g;
                    best_t0   = t0;
                    best_eps  = eps;
                }
            }
        }
        free(cum);
        free(prod);
        free(fold);
        cum  = NULL;
        prod = NULL;
        fold = NULL;
    }
    res->sync_quality = best_q;
    res->sync_snr_db  = best_q < 1 ? 10.0 * log10(best_q / (1.0 - best_q) + 1e-12) : 99.0;
    if (best_size < 0 || best_q < MIN_SYNC_QUALITY) {
        rc = 0;
        goto out;
    }
    d.n  = fft_sizes[best_size];
    d.k  = carriers_normal[best_size];
    d.gl = d.n / guards[best_g][1] * guards[best_g][0];
    d.l  = d.n + d.gl;
    d.df = sample_rate / d.n;
    res->found     = true;
    res->fft_len   = d.n;
    res->guard_num = guards[best_g][0];
    res->guard_den = guards[best_g][1];
    res->guard_us  = d.gl / sample_rate * 1e6;

    /* Symbols in frequency, the P2 symbols after a P1 left out. The FFT window starts an eighth
     * of the guard interval before its end, so that a path a little early stays in it, the
     * fractional offset removed; the slope of phase that start puts on the carriers is taken off
     * too, or the channel would turn between pilots Dx carriers apart (by 0.6 rad at 32k 1/16
     * with Dx = 12: an error of -27 dB once interpolated). */
    if (after >= 0) {
        const int m0 = (int)lround((double)best_t0 / d.l);

        skip = p2_symbols[best_size] - m0 > 0 ? p2_symbols[best_size] - m0 : 0;
    }
    {
        const size_t first = seg0 + best_t0 + (size_t)skip * (size_t)d.l + (size_t)(d.gl - d.gl / 8);
        float complex *in, *ramp;

        nsym = first + (size_t)d.n <= seg1 ? (int)((seg1 - first - (size_t)d.n) / (size_t)d.l) + 1 : 0;
        if (nsym > MAX_SYMBOLS)
            nsym = MAX_SYMBOLS;
        if ((size_t)nsym * (size_t)d.n > MAX_CELLS)
            nsym = MAX_CELLS / d.n;
        if (nsym < 4) {
            rc = 0;
            goto out;
        }
        fft = m2sdr_fft_alloc((size_t)d.n, false);
        y   = malloc((size_t)nsym * (size_t)d.n * sizeof(*y));
        in   = malloc((size_t)d.n * sizeof(*in));
        ramp = malloc((size_t)d.n * sizeof(*ramp));
        if (!fft || !y || !in || !ramp) {
            free(in);
            free(ramp);
            goto out;
        }
        for (i = 0; i < d.n; i++)
            ramp[i] = (float complex)cexp(I * 2.0 * M_PI * (double)(i < d.n / 2 ? i : i - d.n) * (d.gl / 8) / d.n);
        for (s = 0; s < nsym; s++) {
            const size_t a = first + (size_t)s * (size_t)d.l;
            const float complex step = (float complex)cexp(I * 2.0 * M_PI * best_eps / d.n);

            /* A turn of float steps, set again every 512 samples. */
            for (i = 0; i < d.n; i += 512) {
                float complex rot = (float complex)cexp(I * 2.0 * M_PI * best_eps * (double)(a + (size_t)i) / d.n);
                const int e = i + 512 < d.n ? i + 512 : d.n;
                int t;

                for (t = i; t < e; t++) {
                    in[t] = cmul(x[a + (size_t)t], rot);
                    rot    = cmul(rot, step);
                }
            }
            m2sdr_fft_run(fft, in, y + (size_t)s * (size_t)d.n);
            for (i = 0; i < d.n; i++)
                y[(size_t)s * (size_t)d.n + (size_t)i] = cmul(y[(size_t)s * (size_t)d.n + (size_t)i], ramp[i]);
        }
        free(in);
        free(ramp);
        m2sdr_fft_free(fft);
        fft = NULL;
    }

    /* Normal or extended carriers, and the offset to a few carriers, from the width and the
     * place of the spectrum. */
    pw = calloc((size_t)d.n, sizeof(*pw));
    if (!pw)
        goto out;
    for (s = 0; s < nsym; s++)
        for (i = 0; i < d.n; i++)
            pw[i < d.n / 2 ? i + d.n / 2 : i - d.n / 2] += pw2(y[(size_t)s * (size_t)d.n + (size_t)i]);
    if (carriers_ext[best_size]) {
        const int ke = carriers_ext[best_size], kext = (ke - d.k) / 2, u = band_window(pw, d.n, ke);
        const double inner = band_mean(pw, u + kext, u + ke - kext);
        const double lo = band_mean(pw, u, u + kext), hi = band_mean(pw, u + ke - kext, u + ke);

        if ((lo < hi ? lo : hi) >= 0.5 * inner) {
            res->extended = true;
            d.k = ke;
        }
    }
    res->carriers = d.k;
    off_c = band_window(pw, d.n, d.k) + (d.k - 1) / 2 - d.n / 2;
    free(pw);
    pw = NULL;

    /* The carriers there, and their power, for the pilot searches. */
    cs = malloc((size_t)nsym * (size_t)d.k * sizeof(*cs));
    pw = malloc((size_t)nsym * (size_t)d.k * sizeof(*pw));
    w  = malloc((size_t)d.k);
    if (!cs || !pw || !w)
        goto out;
    pilot_prbs(w, d.k);
    carriers_take(y, cs, nsym, &d, off_c);
    for (j = 0; j < (size_t)nsym * (size_t)d.k; j++)
        pw[j] = pw2(cs[j]);
    pat = pattern_search(cs, pw, nsym, d.k, cfg ? cfg->pattern : 0, &sigma, &res->pattern_quality);
    res->symbols = nsym;
    if (pat < 0 || res->pattern_quality < MIN_PATTERN_QUALITY) {
        rc = 0;
        goto out;
    }
    res->pattern    = pat + 1;
    res->pattern_dx = patterns[pat].dx;
    res->pattern_dy = patterns[pat].dy;
    dd = patterns[pat].dx * patterns[pat].dy;

    /* Integer offset: the PRBS of the pilots, around the place of the spectrum or a known offset. */
    {
        int d0 = cfg && cfg->offset_known ? off_c - cfg->carrier_offset : 0, dk;

        if (abs(d0) > d.n / 2)
            d0 = 0;
        dk = prbs_offset(cs, nsym, d.k, &patterns[pat], sigma, w, d0 - OFFSET_SEARCH, d0 + OFFSET_SEARCH, &res->pilot_quality);
        if (d0 && res->pilot_quality < 2.0 * MIN_PILOT_QUALITY)
            dk = prbs_offset(cs, nsym, d.k, &patterns[pat], sigma, w, -OFFSET_SEARCH, OFFSET_SEARCH, &res->pilot_quality);
        off = off_c - dk;
        b0  = ((sigma + dk) % dd + dd) % dd;
    }
    res->carrier_offset = off;
    res->freq_error_hz  = (off - best_eps) * d.df;
    if (res->pilot_quality < MIN_PILOT_QUALITY) {
        rc = 0;
        goto out;
    }
    res->pilots = true;
    carriers_take(y, cs, nsym, &d, off);
    free(y);
    y = NULL;

    /* The grid of the channel, and the symbols left out: a frame closing symbol has pilots on
     * every Dx-th carrier. */
    grid.dx    = patterns[pat].dx;
    grid.dy    = patterns[pat].dy;
    grid.a     = patterns[pat].a;
    grid.g0    = b0 / grid.dx;
    grid.k     = d.k;
    grid.ngrid = (d.k - 1) / grid.dx + 1;
    grid.gn    = grid.ngrid + (((d.k - 1) % grid.dx) ? 1 : 0);
    grid.h     = calloc((size_t)grid.gn, sizeof(*grid.h));
    grid.valid = calloc((size_t)grid.gn, 1);
    grid.list  = malloc(((size_t)grid.ngrid / (size_t)grid.dy + 4) * sizeof(*grid.list));
    use        = calloc((size_t)nsym, 1);
    mask       = malloc((size_t)d.k);
    hc         = malloc((size_t)d.k * sizeof(*hc));
    if (!grid.h || !grid.valid || !grid.list || !use || !mask || !hc)
        goto out;
    for (s = 0; s < nsym; s++) {
        const float complex *a = cs + (size_t)s * (size_t)d.k;
        double on = 0, offg = 0;
        int non = 0, noff = 0;

        grid_mask(&grid, s, mask);
        for (c = 1; c < d.k - 1; c++) {
            if (c % grid.dx) {
                offg += pw2(a[c]);
                noff++;
            } else if (!mask[c]) {
                on += pw2(a[c]);
                non++;
            }
        }
        use[s] = !(non && noff && on / non > DENSE_RATIO * (offg / noff));
    }

    /* Common turn of each symbol: what is left of the carrier offset, the phase noise, and the
     * sign of the pilots of the symbol. Its pilots against the grid: the same carriers Dy
     * symbols before, or the grid interpolated for the first symbols. */
    {
        bool first = true;

        for (s = 0; s < nsym; s++) {
            float complex *a = cs + (size_t)s * (size_t)d.k;
            const int np = grid_pilots(&grid, s);
            float complex sum = 0, turn = 1;

            if (!use[s])
                continue;
            for (i = 0; i < np; i++) {
                const int g = grid.list[i], cc = grid_carrier(&grid, g);
                const float complex r = a[cc] * (w[cc] ? -1.0f : 1.0f) / grid.a;
                const float complex h = grid.valid[g] ? grid.h[g] : grid_interp(&grid, g);

                sum += cmulc(r, h);
            }
            if (!first && cabsf(sum) > 0)
                turn = conjf(sum) / cabsf(sum);
            for (c = 0; c < d.k; c++)
                a[c] = cmul(a[c], turn);
            for (i = 0; i < np; i++) {
                const int g = grid.list[i], cc = grid_carrier(&grid, g);

                grid.h[g]     = a[cc] * (w[cc] ? -1.0f : 1.0f) / grid.a;
                grid.valid[g] = 1;
            }
            first = false;
        }
    }

    /* Equalisation: the channel from the grid of the last Dy symbols, linear in frequency. On
     * the carriers without a scattered pilot: the sums that find the continual pilots and their
     * noise. The impulse response from the grid. */
    zsum  = calloc((size_t)d.k, sizeof(*zsum));
    csum  = calloc((size_t)d.k, sizeof(*csum));
    zpow  = calloc((size_t)d.k * 5, sizeof(*zpow));
    zn    = calloc((size_t)d.k, sizeof(*zn));
    while (efft_n < grid.ngrid)
        efft_n *= 2;
    echo = calloc((size_t)efft_n, sizeof(*echo));
    ein  = malloc((size_t)efft_n * sizeof(*ein));
    eout = malloc((size_t)efft_n * sizeof(*eout));
    efft = m2sdr_fft_alloc((size_t)efft_n, true);
    if (!zsum || !csum || !zpow || !zn || !echo || !ein || !eout || !efft)
        goto out;
    csum2 = zpow + d.k;
    hpow  = zpow + 2 * (size_t)d.k;
    err   = zpow + 3 * (size_t)d.k;
    ref   = zpow + 4 * (size_t)d.k;
    memset(grid.valid, 0, (size_t)grid.gn);
    {
        int nvalid = 0;

        for (s = 0; s < nsym; s++) {
            float complex *a = cs + (size_t)s * (size_t)d.k;
            const int np = grid_pilots(&grid, s);

            if (!use[s])
                continue;
            for (i = 0; i < np; i++) {
                const int g = grid.list[i], cc = grid_carrier(&grid, g);

                grid.h[g] = a[cc] * (w[cc] ? -1.0f : 1.0f) / grid.a;
                nvalid += !grid.valid[g];
                grid.valid[g] = 1;
            }
            if (nvalid < grid.gn) {
                use[s] = 0;
                continue;
            }
            nused++;
            grid_fill(&grid, hc);
            grid_mask(&grid, s, mask);
            mask[0] = mask[d.k - 1] = 0;
            for (c = 0; c < d.k; c++) {
                const float complex h = hc[c], v = a[c];
                const float hp = pw2(h);
                const float complex z = hp > 0 ? cmulc(v, h) / hp : 0;

                hpow[c] += hp;
                if (!mask[c]) {
                    zsum[c]  += z;
                    zpow[c]  += pw2(z);
                    csum[c]  += v;
                    csum2[c] += pw2(v);
                    zn[c]++;
                }
                a[c] = z;
            }
            /* Impulse response: the grid under a Hann window. */
            if (necho < ECHO_MAX_FFTS && nused % grid.dy == 0) {
                memset(ein, 0, (size_t)efft_n * sizeof(*ein));
                for (i = 0; i < grid.ngrid; i++)
                    ein[i] = grid.h[i] * (float)(0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / grid.ngrid));
                m2sdr_fft_run(efft, ein, eout);
                for (i = 0; i < efft_n; i++)
                    echo[i] += pw2(eout[i]);
                necho++;
            }
        }
    }
    res->symbols = nused;
    if (nused < 1) {
        rc = 0;
        goto out;
    }

    /* Continual and edge pilots: carriers that stay the same, real, from symbol to symbol. Their
     * noise (the spread of the received values, not equalised: the estimate of the channel is not
     * in it) over the power of the channel there is the noise against data of power 1. */
    cp    = calloc((size_t)d.k, 1);
    noise = malloc((size_t)d.k * sizeof(*noise));
    if (!cp || !noise)
        goto out;
    {
        int ncp = 0, nacc = 0;
        double q = 0;

        for (c = 0; c < d.k; c++) {
            const double t = zn[c] && zpow[c] > 0 ? pw2(zsum[c]) / (zn[c] * zpow[c]) : 0;

            noise[c] = 0;
            if (zn[c] >= CP_MIN_SYMBOLS && t > CP_COHERENCE && fabsf(cimagf(zsum[c])) < 0.3f * cabsf(zsum[c])) {
                cp[c] = 1;
                noise[c] = (csum2[c] - pw2((float complex)csum[c]) / zn[c]) / (zn[c] - 1);
                ncp++;
            }
        }
        if (ncp >= 2) {
            double *sorted = malloc((size_t)ncp * sizeof(*sorted)), med;

            if (!sorted)
                goto out;
            for (c = 0, i = 0; c < d.k; c++)
                if (cp[c])
                    sorted[i++] = noise[c];
            qsort(sorted, (size_t)ncp, sizeof(*sorted), double_cmp);
            med = sorted[ncp / 2];
            free(sorted);
            for (c = 0; c < d.k; c++)
                if (cp[c] && noise[c] <= CP_NOISE_REJECT * med && hpow[c] > 0) {
                    q += noise[c] / (hpow[c] / nused);
                    nacc++;
                }
        }
        cp[0] = cp[d.k - 1] = 1;
        res->continual_pilots = nacc;
        if (nacc >= 2) {
            res->pilot_mer_valid = true;
            res->pilot_mer_db    = q > 0 ? -10.0 * log10(q / nacc) : 99.0;
        }
    }

    /* Constellation: the model the data cells fit best, then the MER against it. */
    lv = malloc(4 * sizeof(*lv));
    if (!lv)
        goto out;
    for (i = 0; i < 4; i++)
        levels_init(&lv[i], 2 << i, model_angle[i]);
    {
        double e[MODELS] = { 0 }, best_fit = 0;
        size_t ndata = 0, taken = 0, stride, pstride;
        int best_m, chosen;

        for (c = 0; c < d.k; c++)
            ndata += !cp[c];
        ndata = (ndata - (size_t)(d.k / dd)) * (size_t)nused;
        stride  = ndata / FIT_CELLS + 1;
        pstride = ndata / M2SDR_DVBT2_MAX_POINTS + 1;
        for (s = 0, j = 1; s < nsym; s++) {
            const float complex *a = cs + (size_t)s * (size_t)d.k;

            if (!use[s])
                continue;
            grid_mask(&grid, s, mask);
            for (c = 1; c < d.k - 1; c++) {
                if (cp[c] || mask[c])
                    continue;
                if (--j == 0) {
                    j = stride;
                    for (m = 0; m < MODELS; m++)
                        e[m] += model_error(lv, m, a[c]);
                    taken++;
                }
            }
        }
        /* The fit of a model: the slicing error a spread of values would have over the one read. */
        {
            double fit[MODELS];
            int bs = 0, br = 4;

            for (m = 0; m < MODELS; m++)
                fit[m] = !taken ? 0 : e[m] > 0 ? model_u(lv, m) / (e[m] / taken) : 99.0;
            for (m = 1; m < 4; m++) {
                bs = fit[m] > fit[bs] ? m : bs;
                br = fit[m + 4] > fit[br] ? m + 4 : br;
            }
            best_m   = fit[br] > ROTATED_PREFER * fit[bs] ? br : bs;
            best_fit = fit[best_m];
        }
        res->fit = best_fit;
        if (cfg && cfg->modulation != M2SDR_MOD_AUTO) {
            for (chosen = 0; chosen < 4 && model_mod[chosen] != cfg->modulation; chosen++)
                ;
            chosen = chosen < 4 ? chosen + (cfg->rotated ? 4 : 0) : -1;
        } else if (best_fit >= MIN_FIT) {
            chosen = best_m;
        } else {
            chosen = -1;
            res->rotated = res->pilot_mer_valid &&
                           model_u(lv, 3) * pow(10.0, res->pilot_mer_db / 10.0) >= ROTATED_FIT;
        }
        if (chosen >= 0) {
            res->modulation = model_mod[chosen % 4];
            res->rotated    = chosen >= 4;
            res->mer_valid  = chosen < 4;
        }

        for (s = 0, j = 1; s < nsym; s++) {
            const float complex *a = cs + (size_t)s * (size_t)d.k;
            const int side = 2 << (chosen >= 0 ? chosen % 4 : 0);
            const float scale = qam_scale(side);

            if (!use[s])
                continue;
            grid_mask(&grid, s, mask);
            for (c = 1; c < d.k - 1; c++) {
                if (cp[c] || mask[c])
                    continue;
                if (res->mer_valid) {
                    err[c] += pw2(a[c] - qam_slice(a[c], side, scale));
                    ref[c] += 1.0;
                }
                if (--j == 0) {
                    j = pstride;
                    if (res->npoints < M2SDR_DVBT2_MAX_POINTS)
                        res->points[res->npoints++] = a[c];
                }
            }
        }
    }

    /* MER, over everything and per group of carriers; the channel per group. The reference of
     * a cell is the mean power of the constellation, 1. */
    {
        double te = 0, tr = 0, hmean = 0;

        for (c = 0; c < d.k; c++) {
            te += err[c];
            tr += ref[c];
            hmean += hpow[c];
        }
        res->mer_db = res->mer_valid ? te > 0 ? 10.0 * log10(tr / te) : 99.0 : 0.0;
        hmean /= d.k;
        res->bins = M2SDR_DVBT2_BINS;
        res->carriers_per_bin = (double)d.k / M2SDR_DVBT2_BINS;
        for (m = 0; m < M2SDR_DVBT2_BINS; m++) {
            const int c0 = (int)(m * res->carriers_per_bin), c1 = (int)((m + 1) * res->carriers_per_bin);
            double be = 0, br = 0, bh = 0;

            for (c = c0; c < c1 && c < d.k; c++) {
                be += err[c];
                br += ref[c];
                bh += hpow[c];
            }
            res->mer_bin_db[m]     = (float)(res->mer_valid && be > 0 ? 10.0 * log10(br / be) : res->mer_valid ? 99.0 : 0.0);
            res->channel_bin_db[m] = (float)(hmean > 0 && bh > 0 && c1 > c0 ? 10.0 * log10(bh / (c1 - c0) / hmean) : -99.0);
        }
    }

    /* Echo profile: an eighth of the range before the strongest path, the rest after it. */
    if (necho > 0) {
        const double step_s = 1.0 / (efft_n * grid.dx * d.df);
        const int per = efft_n / M2SDR_DVBT2_ECHO_BINS, lobe = 6 * efft_n / grid.ngrid + 2;
        double peak = 0, second = 0;
        int bp = 0, bs = -1;

        for (i = 0; i < efft_n; i++)
            if (echo[i] > peak) {
                peak = echo[i];
                bp   = i;
            }
        for (i = 0; i < efft_n; i++) {
            const int dist = abs(((i - bp) % efft_n + efft_n + efft_n / 2) % efft_n - efft_n / 2);

            if (dist > lobe && echo[i] > second) {
                second = echo[i];
                bs     = i;
            }
        }
        for (m = 0; m < M2SDR_DVBT2_ECHO_BINS; m++) {
            double v = 0;

            for (i = 0; i < per; i++) {
                const int b = ((bp - efft_n / 8 + m * per + i) % efft_n + efft_n) % efft_n;

                if (echo[b] > v)
                    v = echo[b];
            }
            res->echo_db[m] = (float)(10.0 * log10(v / peak + 1e-12));
        }
        res->echo_start_us = -(efft_n / 8) * step_s * 1e6;
        res->echo_step_us  = per * step_s * 1e6;
        if (bs >= 0) {
            int rel = ((bs - bp) % efft_n + efft_n) % efft_n;

            if (rel >= efft_n * 7 / 8)
                rel -= efft_n;
            res->echo_delay_us = rel * step_s * 1e6;
            res->echo_level_db = 10.0 * log10(second / peak + 1e-12);
        } else {
            res->echo_level_db = -99.0;
        }
    }
    rc = 0;
out:
    m2sdr_fft_free(fft);
    m2sdr_fft_free(efft);
    free(cum);
    free(prod);
    free(fold);
    free(y);
    free(cs);
    free(pw);
    free(w);
    free(use);
    free(mask);
    free(hc);
    free(cp);
    free(noise);
    free(zsum);
    free(csum);
    free(zpow);
    free(zn);
    free(echo);
    free(ein);
    free(eout);
    free(lv);
    free(grid.h);
    free(grid.valid);
    free(grid.list);
    return rc;
}
