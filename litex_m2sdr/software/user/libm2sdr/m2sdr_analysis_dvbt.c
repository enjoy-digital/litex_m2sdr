/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: DVB-T reception (EN 300 744), 2k and 8k.
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

#define MIN_SYNC_QUALITY   0.08    /* Correlation of the guard intervals of a multiplex; noise reads about 0.01. */
#define MIN_PILOT_QUALITY  0.15    /* Coherence of the continual pilots; noise reads about 0.05. */
#define MAX_SYMBOLS        160
#define SYNC_SYMBOLS       40      /* 8k symbols of the longest guard the guard search looks at. */     /* Analysed in a capture: a TPS frame (68) and its sync word wherever it starts. */
#define SP_SPACING         12      /* Scattered pilots of a symbol. */
#define SP_GRID            3       /* Scattered pilots of four symbols together. */
#define ECHO_FFT           4096    /* Points of the impulse response, a power of two over 6817 / 3. */
#define ECHO_MAIN_LOBE     12      /* Points of ECHO_FFT around the strongest path left out of the search of an echo. */
#define TPS_SYNC           0x35eeu /* s1 to s16 of odd frames; even ones carry its inverse. */
#define TPS_LAST_BIT       39      /* s39: the last of the parameters read. */
#define MOD_TRIED          3       /* QPSK, 16QAM, 64QAM. */
#define MOD_PREFER_DB      3.0     /* Without a TPS, the lowest order within this of the best MER. */

/* Carriers of the 2k mode; the 8k mode repeats them every 1704 carriers. */
static const int cp_2k[] = {
       0,   48,   54,   87,  141,  156,  192,  201,  255,  279,  282,  333,  432,  450,  483,
     525,  531,  618,  636,  714,  759,  765,  780,  804,  873,  888,  918,  939,  942,  969,
     984, 1050, 1101, 1107, 1110, 1137, 1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704,
};
static const int tps_2k[] = {
      34,   50,  209,  346,  413,  569,  595,  688,  790,  901, 1073, 1219, 1262, 1286, 1469,
    1594, 1687,
};

#define KIND_DATA   0
#define KIND_CP     1
#define KIND_TPS    2

static const enum m2sdr_modulation mod_tried[MOD_TRIED] = { M2SDR_MOD_QPSK, M2SDR_MOD_16QAM, M2SDR_MOD_64QAM };
static const char *const rate_names[5] = { "1/2", "2/3", "3/4", "5/6", "7/8" };

struct dvbt_dims {
    int    n;                     /* FFT length. */
    int    k;                     /* Carriers. */
    int    g;                     /* Guard interval: 1/g of n. */
    int    gl;                    /* Its length in samples. */
    int    l;                     /* Symbol: n + gl. */
    double df;                    /* Carrier spacing, Hz. */
};

/* Sequence of the pilots: x^11 + x^2 + 1 from all ones, one bit per carrier. Pilot k is
 * 4/3 * (1 - 2 w[k]). */
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

/* Synchronisation */
/*-----------------*/

/* Guard intervals: x against itself n samples later, folded on the symbol period and summed
 * over a guard interval. Returns its peak over the power, 0 to 1, with the start of a guard
 * interval and the fractional carrier offset, in carriers, of the peak. */
static double guard_search(const float complex *x, size_t count, int n, int g, size_t *t0, double *eps)
{
    const int gl = n / g, l = n + gl;
    double complex *fold, m = 0, best_m = 0;
    double pw = 0, best = -1;
    size_t nsym, s;
    int i, t, best_t = 0;

    if (count < (size_t)n + 4 * (size_t)l)
        return 0;
    nsym = (count - (size_t)n) / (size_t)l;
    fold = calloc((size_t)l, sizeof(*fold));
    if (!fold)
        return -1;
    for (s = 0; s < nsym; s++) {
        const float complex *a = x + s * (size_t)l;

        for (i = 0; i < l; i++) {
            fold[i] += a[i] * conjf(a[i + n]);
            pw += 0.5 * (crealf(a[i] * conjf(a[i])) + crealf(a[i + n] * conjf(a[i + n])));
        }
    }
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
    free(fold);
    *t0  = (size_t)best_t;
    *eps = carg(best_m) / (2.0 * M_PI);
    return pw > 0 ? best / (pw / l * gl) : 0;
}

/* Symbols in frequency: the FFT window starts an eighth of the guard interval before the end of
 * it, so that a path a little early stays in the window. The fractional offset is removed. */
static float complex *symbols_fft(const float complex *x, size_t count, const struct dvbt_dims *d, size_t t0, double eps,
                                  int *nsym)
{
    const size_t first = t0 + (size_t)(d->gl - d->gl / 8);
    struct m2sdr_fft *fft = m2sdr_fft_alloc((size_t)d->n, false);
    float complex *in = malloc((size_t)d->n * sizeof(*in));
    float complex *y;
    int s, i, ns;

    if (count < first + (size_t)d->n)
        ns = 0;
    else
        ns = (int)((count - first - (size_t)d->n) / (size_t)d->l) + 1;
    if (ns > MAX_SYMBOLS)
        ns = MAX_SYMBOLS;
    y = malloc((size_t)(ns ? ns : 1) * (size_t)d->n * sizeof(*y));
    if (!fft || !in || !y) {
        m2sdr_fft_free(fft);
        free(in);
        free(y);
        return NULL;
    }
    for (s = 0; s < ns; s++) {
        const size_t a = first + (size_t)s * (size_t)d->l;
        double complex rot = cexp(I * 2.0 * M_PI * eps * (double)a / d->n);
        const double complex step = cexp(I * 2.0 * M_PI * eps / d->n);

        for (i = 0; i < d->n; i++) {
            in[i] = x[a + (size_t)i] * (float complex)rot;
            rot *= step;
        }
        m2sdr_fft_run(fft, in, y + (size_t)s * (size_t)d->n);
    }
    m2sdr_fft_free(fft);
    free(in);
    *nsym = ns;
    return y;
}

/* FFT bin of carrier c, the multiplex off by `off` carriers. */
static int carrier_bin(const struct dvbt_dims *d, int c, int off)
{
    return ((c - (d->k - 1) / 2 + off) % d->n + d->n) % d->n;
}

/* Integer carrier offset: the continual pilots are the same from symbol to symbol. */
static int pilot_offset(const float complex *y, int nsym, const struct dvbt_dims *d, const uint8_t *kind, double *quality)
{
    double complex *dsum = calloc((size_t)d->n, sizeof(*dsum));
    double *asum = calloc((size_t)d->n, sizeof(*asum));
    const int max_off = (d->n - d->k) / 2 - 1;
    double best = -1;
    int s, b, c, off, ncp = 0, best_off = 0;

    if (!dsum || !asum) {
        free(dsum);
        free(asum);
        *quality = 0;
        return 0;
    }
    for (s = 1; s < nsym; s++) {
        const float complex *a = y + (size_t)s * (size_t)d->n, *p = a - d->n;

        for (b = 0; b < d->n; b++) {
            const float complex v = a[b] * conjf(p[b]);

            dsum[b] += v;
            asum[b] += cabsf(v);
        }
    }
    for (c = 0; c < d->k; c++)
        ncp += kind[c] == KIND_CP;
    for (off = -max_off; off <= max_off; off++) {
        double q = 0;

        for (c = 0; c < d->k; c++) {
            if (kind[c] == KIND_CP) {
                b = carrier_bin(d, c, off);
                q += asum[b] > 0 ? cabs(dsum[b]) / asum[b] : 0;
            }
        }
        q /= ncp;
        if (q > best) {
            best     = q;
            best_off = off;
        }
    }
    free(dsum);
    free(asum);
    *quality = best;
    return best_off;
}

/* Scattered pilots: carriers 3 (l mod 4) + 12 p of symbol l of a frame. The phase of the first
 * symbol is the one most symbols agree with. */
static int pilot_phase(const float complex *cs, int nsym, int k, const uint8_t *w)
{
    int votes[4] = { 0, 0, 0, 0 };
    int s, ph, c, best;

    for (s = 0; s < nsym; s++) {
        const float complex *a = cs + (size_t)s * (size_t)k;
        double score_best = -1;
        int ph_best = 0;

        for (ph = 0; ph < 4; ph++) {
            double complex num = 0;
            double den = 0;

            for (c = 3 * ph; c + SP_SPACING < k; c += SP_SPACING) {
                const float complex h0 = a[c] * (w[c] ? -1.0f : 1.0f);
                const float complex h1 = a[c + SP_SPACING] * (w[c + SP_SPACING] ? -1.0f : 1.0f);

                num += h1 * conjf(h0);
                den += cabsf(h1 * conjf(h0));
            }
            if (den > 0 && cabs(num) / den > score_best) {
                score_best = cabs(num) / den;
                ph_best    = ph;
            }
        }
        votes[(ph_best - s % 4 + 4) % 4]++;
    }
    for (best = 0, ph = 1; ph < 4; ph++)
        if (votes[ph] > votes[best])
            best = ph;
    return best;
}

/* TPS */
/*-----*/

/* Bit of symbol s, from s - 1: differential BPSK on the TPS carriers. */
static int tps_bit(const float complex *cs, int s, int k, const uint8_t *kind)
{
    const float complex *a = cs + (size_t)s * (size_t)k, *p = a - k;
    double sum = 0;
    int c;

    for (c = 0; c < k; c++)
        if (kind[c] == KIND_TPS)
            sum += crealf(a[c] * conjf(p[c]));
    return sum < 0;
}

static unsigned tps_field(const uint8_t *bits, int frame, int first, int last)
{
    unsigned v = 0;
    int i;

    for (i = first; i <= last; i++)
        v = (v << 1) | bits[frame + i];
    return v;
}

/* First frame of the capture with its sync word and valid parameters, its first symbol in
 * *frame. bits[s] is the bit of symbol s, from 1. */
static bool tps_read(const uint8_t *bits, int nsym, const struct dvbt_dims *d, struct m2sdr_dvbt_result *res, int *frame)
{
    int f;

    for (f = 0; f + TPS_LAST_BIT < nsym; f++) {
        const unsigned sync = tps_field(bits, f, 1, 16);
        unsigned con, hier, hp, lp, guard, mode, len, num;

        if (sync != TPS_SYNC && sync != (~TPS_SYNC & 0xffffu))
            continue;
        len   = tps_field(bits, f, 17, 22);
        num   = tps_field(bits, f, 23, 24);
        con   = tps_field(bits, f, 25, 26);
        hier  = tps_field(bits, f, 27, 29);
        hp    = tps_field(bits, f, 30, 32);
        lp    = tps_field(bits, f, 33, 35);
        guard = tps_field(bits, f, 36, 37);
        mode  = tps_field(bits, f, 38, 39);
        if (con > 2 || hier > 3 || hp > 4 || lp > 4 || mode > 1)
            continue;
        if ((mode ? 8192 : 2048) != d->n || (32 >> guard) != d->g)
            continue;
        res->tps            = true;
        res->tps_fft_len    = mode ? 8192 : 2048;
        res->tps_guard      = 32 >> guard;
        res->tps_modulation = mod_tried[con];
        res->tps_hierarchy  = hier ? 1 << (hier - 1) : 0;
        strcpy(res->code_rate_hp, rate_names[hp]);
        strcpy(res->code_rate_lp, rate_names[lp]);
        /* The cell identity takes two frames: its high byte in frames 1 and 3 (numbers 0 and 2),
         * its low byte in the others. */
        res->cell_id = -1;
        if (len == 31 && f + 68 + 47 < nsym && tps_field(bits, f + 68, 1, 16) == (~sync & 0xffffu)) {
            const unsigned a = tps_field(bits, f, 40, 47), b = tps_field(bits, f + 68, 40, 47);

            res->cell_id = (int)(num % 2 == 0 ? (a << 8) | b : (b << 8) | a);
        }
        *frame = f;
        return true;
    }
    return false;
}

/* MER */
/*-----*/

/* Nearest point of a square constellation of side m (2, 4 or 8), mean power 1. */
static float complex qam_slice(float complex z, int m, float scale)
{
    float v[2] = { crealf(z) / scale, cimagf(z) / scale };
    int i;

    for (i = 0; i < 2; i++) {
        float q = 2.0f * floorf(0.5f * v[i]) + 1.0f;

        v[i] = q > m - 1 ? (float)(m - 1) : q < 1 - m ? (float)(1 - m) : q;
    }
    return (v[0] + I * v[1]) * scale;
}

static int qam_side(enum m2sdr_modulation mod)
{
    return mod == M2SDR_MOD_QPSK ? 2 : mod == M2SDR_MOD_16QAM ? 4 : 8;
}

static float qam_scale(int m)
{
    /* Mean power of the odd levels of a side of m points: (m^2 - 1) / 3, per axis. */
    return 1.0f / sqrtf(2.0f * (m * m - 1) / 3.0f);
}

/* Analysis */
/*----------*/

int m2sdr_dvbt_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_dvbt_config *cfg,
                       struct m2sdr_dvbt_result *res)
{
    static const double rates[3] = { 64e6 / 7.0, 8e6, 48e6 / 7.0 };
    struct dvbt_dims d;
    float complex *y = NULL, *cs = NULL, *h3 = NULL, *ein = NULL, *eout = NULL;
    double *err = NULL, *ref = NULL, *hpow = NULL, *echo = NULL;
    uint8_t *kind = NULL, *w = NULL, *bits = NULL;
    struct m2sdr_fft *efft = NULL;
    size_t t0 = 0, best_t0 = 0, j;
    double eps = 0, best_eps = 0, best_q = 0, power = 0;
    int i, s, c, nsym = 0, off, ph0, frame = -1, m, ngrid, rc = -1, best_n = 8192, best_g = 4;
    bool rate_ok = false;

    if (!x || !res)
        return -1;
    memset(res, 0, sizeof(*res));
    res->cell_id = -1;
    for (i = 0; i < 3; i++)
        rate_ok |= fabs(sample_rate - rates[i]) < 2e-3 * rates[i];
    if (!rate_ok || count < 8 * (size_t)(8192 + 2048))
        return -1;
    for (j = 0; j < count; j++)
        power += crealf(x[j] * conjf(x[j]));
    res->power_dbfs = 10.0 * log10(power / (double)count + 1e-30);

    /* Mode and guard interval: the best correlation of the eight, or the one given. */
    for (i = 0; i < 8; i++) {
        const int n = i < 4 ? 8192 : 2048, g = 4 << (i % 4);
        const size_t span = (size_t)SYNC_SYMBOLS * (8192 + 2048) + 8192;
        double q;

        if (cfg && cfg->fft_len && cfg->guard && (cfg->fft_len != n || cfg->guard != g))
            continue;
        q = guard_search(x, count < span ? count : span, n, g, &t0, &eps);
        if (q < 0)
            return -1;
        if (q > best_q) {
            best_q  = q;
            best_n  = n;
            best_g  = g;
            best_t0 = t0;
            best_eps = eps;
        }
    }
    res->sync_quality = best_q;
    res->sync_snr_db  = best_q < 1 ? 10.0 * log10(best_q / (1.0 - best_q) + 1e-12) : 99.0;
    if (best_q < MIN_SYNC_QUALITY)
        return 0;
    d.n  = best_n;
    d.k  = best_n == 8192 ? 6817 : 1705;
    d.g  = best_g;
    d.gl = d.n / d.g;
    d.l  = d.n + d.gl;
    d.df = sample_rate / d.n;
    res->found    = true;
    res->fft_len  = d.n;
    res->carriers = d.k;
    res->guard    = d.g;
    res->guard_us = d.gl / sample_rate * 1e6;

    /* Carriers: continual pilots and TPS, the rest data or scattered pilots. */
    kind = calloc((size_t)d.k, 1);
    w    = malloc((size_t)d.k);
    if (!kind || !w)
        goto out;
    pilot_prbs(w, d.k);
    for (i = 0; i < d.k; i += 1704) {
        for (c = 0; c < (int)(sizeof(cp_2k) / sizeof(cp_2k[0])); c++)
            if (i + cp_2k[c] < d.k)
                kind[i + cp_2k[c]] = KIND_CP;
        for (c = 0; c < (int)(sizeof(tps_2k) / sizeof(tps_2k[0])); c++)
            if (i + tps_2k[c] < d.k)
                kind[i + tps_2k[c]] = KIND_TPS;
    }

    y = symbols_fft(x, count, &d, best_t0, best_eps, &nsym);
    if (!y)
        goto out;
    if (nsym < 8) {
        res->found = false;
        rc = 0;
        goto out;
    }
    off = pilot_offset(y, nsym, &d, kind, &res->pilot_quality);
    res->freq_error_hz = (off - best_eps) * d.df;
    res->symbols = nsym;
    if (res->pilot_quality < MIN_PILOT_QUALITY) {
        rc = 0;
        goto out;
    }
    res->pilots = true;

    /* The carriers of the multiplex, symbol after symbol. */
    cs = malloc((size_t)nsym * (size_t)d.k * sizeof(*cs));
    if (!cs)
        goto out;
    /* The integer offset turns each symbol by its start: undone, so symbols compare. */
    for (s = 0; s < nsym; s++) {
        const size_t a = best_t0 + (size_t)(d.gl - d.gl / 8) + (size_t)s * (size_t)d.l;
        const float complex turn = (float complex)cexp(-I * 2.0 * M_PI * (double)off * (double)(a % (size_t)d.n) / d.n);

        for (c = 0; c < d.k; c++)
            cs[(size_t)s * (size_t)d.k + (size_t)c] = y[(size_t)s * (size_t)d.n + (size_t)carrier_bin(&d, c, off)] * turn;
    }
    free(y);
    y = NULL;
    /* Common phase: what is left of the carrier offset and the phase noise turn every carrier of a
     * symbol alike. The continual pilots measure it against the symbol before. */
    for (s = 1; s < nsym; s++) {
        float complex *a = cs + (size_t)s * (size_t)d.k;
        const float complex *p = a - d.k;
        double complex sum = 0;
        float complex turn;

        for (c = 0; c < d.k; c++)
            if (kind[c] == KIND_CP)
                sum += a[c] * conjf(p[c]);
        turn = (float complex)cexp(-I * carg(sum));
        for (c = 0; c < d.k; c++)
            a[c] *= turn;
    }

    /* TPS: it gives the frame, so the phase of the scattered pilots too. */
    bits = calloc((size_t)nsym, 1);
    if (!bits)
        goto out;
    for (s = 1; s < nsym; s++)
        bits[s] = (uint8_t)tps_bit(cs, s, d.k, kind);
    if (tps_read(bits, nsym, &d, res, &frame))
        ph0 = (4 - frame % 4) % 4;
    else
        ph0 = pilot_phase(cs, nsym, d.k, w);

    /* Channel on the carriers 3 m from the scattered pilots of the last four symbols, then
     * equalisation, MER, the channel and its impulse response. */
    ngrid = (d.k - 1) / SP_GRID + 1;
    h3   = calloc((size_t)ngrid, sizeof(*h3));
    err  = calloc((size_t)d.k * 2, sizeof(*err));
    hpow = calloc((size_t)d.k, sizeof(*hpow));
    echo = calloc(ECHO_FFT, sizeof(*echo));
    ein  = malloc(ECHO_FFT * sizeof(*ein));
    eout = malloc(ECHO_FFT * sizeof(*eout));
    efft = m2sdr_fft_alloc(ECHO_FFT, true);
    if (!h3 || !err || !hpow || !echo || !ein || !eout || !efft)
        goto out;
    ref = err + d.k;
    {
        double mer_try[MOD_TRIED] = { 0, 0, 0 };
        double try_err[MOD_TRIED] = { 0, 0, 0 }, try_ref[MOD_TRIED] = { 0, 0, 0 };
        enum m2sdr_modulation mod = res->tps ? res->tps_modulation : cfg ? cfg->modulation : M2SDR_MOD_AUTO;
        int side = 0, pass, stride = 1, taken = 0, nused = 0;
        float scale = 0;
        size_t ndata = 0;

        /* Without a TPS, two passes: the constellation first, then the figures. */
        for (pass = mod == M2SDR_MOD_AUTO ? 0 : 1; pass < 2; pass++) {
            if (pass == 1) {
                if (mod == M2SDR_MOD_AUTO) {
                    int best = MOD_TRIED - 1;

                    for (i = 0; i < MOD_TRIED; i++)
                        mer_try[i] = try_err[i] > 0 ? 10.0 * log10(try_ref[i] / try_err[i]) : 99.0;
                    for (i = 0; i < MOD_TRIED; i++)
                        if (mer_try[i] >= mer_try[MOD_TRIED - 1] - MOD_PREFER_DB) {
                            best = i;
                            break;
                        }
                    mod = mod_tried[best];
                }
                side  = qam_side(mod);
                scale = qam_scale(side);
                stride = (int)(((size_t)(nsym - 3) * (size_t)d.k * 9 / 10) / M2SDR_DVBT_MAX_POINTS) + 1;
            }
            memset(h3, 0, (size_t)ngrid * sizeof(*h3));
            for (s = 0; s < nsym; s++) {
                const float complex *a = cs + (size_t)s * (size_t)d.k;
                const int ph = (ph0 + s) % 4;

                for (c = 3 * ph; c < d.k; c += SP_SPACING)
                    h3[c / SP_GRID] = a[c] * (w[c] ? -0.75f : 0.75f);
                if (s < 3)
                    continue;
                for (c = 0; c < d.k; c++) {
                    const int g = c / SP_GRID, r = c % SP_GRID;
                    const float complex h = r ? h3[g] + (h3[g + 1] - h3[g]) * (r / (float)SP_GRID) : h3[g];
                    float complex z;

                    if (pass == 1)
                        hpow[c] += crealf(h * conjf(h));
                    if (kind[c] != KIND_DATA || (c >= 3 * ph && (c - 3 * ph) % SP_SPACING == 0))
                        continue;
                    if (crealf(h * conjf(h)) == 0)
                        continue;
                    z = a[c] * conjf(h) / crealf(h * conjf(h));
                    if (pass == 0) {
                        for (i = 0; i < MOD_TRIED; i++) {
                            const int mi = qam_side(mod_tried[i]);
                            const float complex q = qam_slice(z, mi, qam_scale(mi));

                            try_err[i] += crealf((z - q) * conjf(z - q));
                            try_ref[i] += crealf(q * conjf(q));
                        }
                        continue;
                    }
                    {
                        const float complex q = qam_slice(z, side, scale);

                        err[c] += crealf((z - q) * conjf(z - q));
                        ref[c] += crealf(q * conjf(q));
                    }
                    if (taken < M2SDR_DVBT_MAX_POINTS && ndata++ % (size_t)stride == 0)
                        res->points[taken++] = z;
                }
                if (pass == 0)
                    continue;
                /* Impulse response: the channel on the grid under a Hann window. */
                memset(ein, 0, ECHO_FFT * sizeof(*ein));
                for (i = 0; i < ngrid; i++)
                    ein[i] = h3[i] * (float)(0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / ngrid));
                m2sdr_fft_run(efft, ein, eout);
                for (i = 0; i < ECHO_FFT; i++)
                    echo[i] += crealf(eout[i] * conjf(eout[i]));
                nused++;
            }
        }
        res->modulation = mod;
        res->npoints    = taken;

        /* MER, over everything and per group of carriers; the channel per group. */
        {
            double te = 0, tr = 0, hmean = 0;

            for (c = 0; c < d.k; c++) {
                te += err[c];
                tr += ref[c];
                hmean += hpow[c];
            }
            res->mer_db = te > 0 ? 10.0 * log10(tr / te) : 99.0;
            hmean /= d.k;
            res->bins = M2SDR_DVBT_BINS;
            res->carriers_per_bin = (double)d.k / M2SDR_DVBT_BINS;
            for (m = 0; m < M2SDR_DVBT_BINS; m++) {
                const int c0 = (int)(m * res->carriers_per_bin), c1 = (int)((m + 1) * res->carriers_per_bin);
                double be = 0, br = 0, bh = 0;

                for (c = c0; c < c1 && c < d.k; c++) {
                    be += err[c];
                    br += ref[c];
                    bh += hpow[c];
                }
                res->mer_bin_db[m]     = (float)(be > 0 ? 10.0 * log10(br / be) : 99.0);
                res->channel_bin_db[m] = (float)(hmean > 0 && bh > 0 ? 10.0 * log10(bh / (c1 - c0) / hmean) : -99.0);
            }
        }

        /* Echo profile: an eighth of the range before the strongest path, the rest after it. */
        if (nused > 0) {
            const double step_s = 1.0 / (ECHO_FFT * SP_GRID * d.df);
            const int per = ECHO_FFT / M2SDR_DVBT_ECHO_BINS;
            double peak = 0, second = 0;
            int bp = 0, bs = -1;

            for (i = 0; i < ECHO_FFT; i++)
                if (echo[i] > peak) {
                    peak = echo[i];
                    bp   = i;
                }
            for (i = 0; i < ECHO_FFT; i++) {
                const int dist = abs(((i - bp) % ECHO_FFT + ECHO_FFT + ECHO_FFT / 2) % ECHO_FFT - ECHO_FFT / 2);

                if (dist > ECHO_MAIN_LOBE && echo[i] > second) {
                    second = echo[i];
                    bs     = i;
                }
            }
            for (m = 0; m < M2SDR_DVBT_ECHO_BINS; m++) {
                double v = 0;

                for (i = 0; i < per; i++) {
                    const int b = ((bp - ECHO_FFT / 8 + m * per + i) % ECHO_FFT + ECHO_FFT) % ECHO_FFT;

                    if (echo[b] > v)
                        v = echo[b];
                }
                res->echo_db[m] = (float)(10.0 * log10(v / peak + 1e-12));
            }
            res->echo_start_us = -(ECHO_FFT / 8) * step_s * 1e6;
            res->echo_step_us  = per * step_s * 1e6;
            if (bs >= 0) {
                int rel = ((bs - bp) % ECHO_FFT + ECHO_FFT) % ECHO_FFT;

                if (rel >= ECHO_FFT * 7 / 8)
                    rel -= ECHO_FFT;
                res->echo_delay_us = rel * step_s * 1e6;
                res->echo_level_db = 10.0 * log10(second / peak + 1e-12);
            } else {
                res->echo_level_db = -99.0;
            }
        }
    }
    rc = 0;
out:
    free(y);
    free(cs);
    free(kind);
    free(w);
    free(bits);
    free(h3);
    free(err);
    free(hpow);
    free(echo);
    free(ein);
    free(eout);
    m2sdr_fft_free(efft);
    return rc;
}
