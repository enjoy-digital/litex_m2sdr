/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: LoRa chirp spread spectrum packets.
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

#define SF_MIN             7
#define SF_MAX             12
#define BANDS              3
#define MIN_RUN            4       /* Windows of a preamble in a row: 8 up-chirps hold 7 whole ones. */
#define MAX_RUNS           256
#define DOWN_SEARCH        7       /* Windows after a preamble searched for its down-chirps. */
#define END_DROP           0.125   /* A symbol peak this much below those of the preamble: the packet ended. */
#define MAX_PREAMBLE       64
#define ALIGN_PASSES       4       /* Cuts of the band of a packet, each closer to its timing. */

static const double bandwidths[BANDS] = { 125e3, 250e3, 500e3 };

/* Peak of a window over the mean of its bins, that noise alone stays under: the largest of N
 * exponential bins exceeds t times their mean with a probability of about N exp(-t), here
 * exp(-4) / N per window, and a preamble needs MIN_RUN of them in a row on the same bin. A
 * chirp of SNR s (in the bandwidth) reads N s + 1: SF7 is found from -9.7 dB, SF12 from -23 dB,
 * under the limits of demodulation (-7.5 and -20 dB). */
static double min_ratio(int n)
{
    return 2.0 * log((double)n) + 4.0;
}

/* A bandwidth cut out of the capture, at a sample rate of the bandwidth. */
struct band {
    double         bw;
    int            r;           /* Decimation: sample_rate / bw. */
    float complex *y;
    size_t         n;
};

/* Windows of one bandwidth and SF in a row with the same peak. */
struct run {
    int    band, sf;
    size_t w0, w1;              /* Windows [w0, w1). */
    int    bin;
    double share;               /* Mean share of the power of a window in its peak bin: 1 for a
                                 * chirp alone, whatever the SF. */
    bool   dropped;
};

/* Work of one SF: plan and chirps. */
struct chirps {
    int    n;
    struct m2sdr_fft *fft;
    float complex *up, *down, *in, *out;
    float *pw;
};

static int chirps_alloc(struct chirps *c, int sf)
{
    int i;

    c->n    = 1 << sf;
    c->fft  = m2sdr_fft_alloc((size_t)c->n, false);
    c->up   = malloc((size_t)c->n * sizeof(*c->up));
    c->down = malloc((size_t)c->n * sizeof(*c->down));
    c->in   = malloc((size_t)c->n * sizeof(*c->in));
    c->out  = malloc((size_t)c->n * sizeof(*c->out));
    c->pw   = malloc((size_t)c->n * sizeof(*c->pw));
    if (!c->fft || !c->up || !c->down || !c->in || !c->out || !c->pw)
        return -1;
    /* Base up-chirp at the rate of the bandwidth: from -bw/2 to bw/2 over n samples, a phase
     * of pi n^2 / N - pi n, periodic in N. */
    for (i = 0; i < c->n; i++) {
        const double ph = M_PI * ((double)i * (double)i / c->n - (double)i);

        c->up[i]   = (float complex)cexp(I * ph);
        c->down[i] = conjf(c->up[i]);
    }
    return 0;
}

static void chirps_free(struct chirps *c)
{
    m2sdr_fft_free(c->fft);
    free(c->up);
    free(c->down);
    free(c->in);
    free(c->out);
    free(c->pw);
}

/* A window dechirped by ref and turned by -shift bins: FFT in c->out, its peak bin, the power
 * of the peak and the mean of the bins. */
struct peak {
    int    bin;
    double power, mean;
};

static void dechirp(struct chirps *c, const float complex *y, const float complex *ref, double shift, struct peak *pk)
{
    double total = 0, best = -1;
    int i, k = 0;

    if (shift != 0.0) {
        const double complex step = cexp(-I * 2.0 * M_PI * shift / c->n);
        double complex rot = 1.0;

        for (i = 0; i < c->n; i++) {
            c->in[i] = y[i] * ref[i] * (float complex)rot;
            rot *= step;
        }
    } else {
        for (i = 0; i < c->n; i++)
            c->in[i] = y[i] * ref[i];
    }
    m2sdr_fft_run(c->fft, c->in, c->out);
    for (i = 0; i < c->n; i++) {
        const double p = crealf(c->out[i]) * crealf(c->out[i]) + cimagf(c->out[i]) * cimagf(c->out[i]);

        total += p;
        if (p > best) {
            best = p;
            k    = i;
        }
    }
    pk->bin   = k;
    pk->power = best;
    pk->mean  = total / c->n;
}

/* Fractional peak of the last dechirp, from the magnitudes of the peak and its larger
 * neighbour: a tone k + d bins, rectangular window, gives |X[k + 1]| / |X[k]| = d / (1 - d).
 * Magnitudes and not the complex bins: a chirp off the window dechirps into a tone cut and
 * turned at its wrap, a tone of the window turned round, whose bins keep their magnitudes. */
static double frac_peak(const struct chirps *c, int k)
{
    const double a = cabsf(c->out[(k + c->n - 1) % c->n]), b = cabsf(c->out[k]), d = cabsf(c->out[(k + 1) % c->n]);

    if (d > a)
        return k + d / (b + d + 1e-30);
    return k - a / (a + b + 1e-30);
}

/* Noise power of a bin of the last dechirp: the median of the bins over ln 2 (that of
 * exponential bins), which the peak, its sidelobes and the chirp cut at the band edges leave
 * alone. */
static double noise_bin(struct chirps *c)
{
    int i, lo = 0, hi = c->n - 1;
    const int mid = c->n / 2;

    for (i = 0; i < c->n; i++)
        c->pw[i] = crealf(c->out[i]) * crealf(c->out[i]) + cimagf(c->out[i]) * cimagf(c->out[i]);
    /* Quickselect of the middle one. */
    while (lo < hi) {
        const float pivot = c->pw[(lo + hi) / 2];
        int a = lo, b = hi;

        while (a <= b) {
            while (c->pw[a] < pivot)
                a++;
            while (c->pw[b] > pivot)
                b--;
            if (a <= b) {
                const float t = c->pw[a];

                c->pw[a++] = c->pw[b];
                c->pw[b--] = t;
            }
        }
        if (mid <= b)
            hi = b;
        else if (mid >= a)
            lo = a;
        else
            break;
    }
    return c->pw[mid] / M_LN2;
}

/* x wrapped into [-n/2, n/2). */
static double wrap(double x, double n)
{
    return x - n * floor(x / n + 0.5);
}

static bool same_bin(int a, int b, int n)
{
    const int d = ((a - b) % n + n) % n;

    return d <= 1 || d >= n - 1;
}

/* Bandwidth Cut */
/*---------------*/

/* The bins of [-bw/2, bw/2) around bin `shift` of the spectrum X of p points back to time:
 * samples at bw, the band then centred, ahead by `advance` of them (a fraction: the band is cut
 * exactly, the delay too). bd->y is allocated on the first call. */
static int band_cut(const float complex *X, size_t p, size_t count, struct band *bd, double advance, long shift)
{
    const size_t sh = (size_t)(((shift % (long)p) + (long)p) % (long)p);
    const size_t m = p / (size_t)bd->r;
    struct m2sdr_fft *ifft = m2sdr_fft_alloc(m, true);
    float complex *in = malloc(m * sizeof(*in));
    size_t i;
    int rc = -1;

    if (!bd->y)
        bd->y = malloc(m * sizeof(*bd->y));
    if (!ifft || !in || !bd->y)
        goto out;
    for (i = 0; i < m / 2; i++) {
        /* Bin i is i / m of the new rate, bin m / 2 + i is i / m - 1/2 of it. */
        const double fp = (double)i / (double)m, fn = (double)i / (double)m - 0.5;

        in[i]         = X[(i + sh) % p] / (float)p;
        in[m / 2 + i] = X[(p - m / 2 + i + sh) % p] / (float)p;
        if (advance != 0.0) {
            in[i]         *= (float complex)cexp(I * 2.0 * M_PI * fp * advance);
            in[m / 2 + i] *= (float complex)cexp(I * 2.0 * M_PI * fn * advance);
        }
    }
    m2sdr_fft_run(ifft, in, bd->y);
    bd->n = count / (size_t)bd->r;
    rc = 0;
out:
    m2sdr_fft_free(ifft);
    free(in);
    return rc;
}

/* Preamble Search */
/*-----------------*/

static int scan(const struct band *bd, int b, int sf, struct chirps *c, struct run *runs, int nruns)
{
    const double thr = min_ratio(c->n);
    const size_t windows = bd->n / (size_t)c->n;
    size_t w, start = 0;
    int bin = -1, len = 0;
    double sum = 0;

    for (w = 0; w <= windows; w++) {
        struct peak pk = { 0, 0, 1 };
        bool strong = false;

        if (w < windows) {
            dechirp(c, bd->y + w * (size_t)c->n, c->down, 0.0, &pk);
            strong = pk.power > thr * pk.mean;
        }
        if (strong && len > 0 && same_bin(pk.bin, bin, c->n)) {
            len++;
            sum += pk.power / (pk.mean * c->n);
            continue;
        }
        if (len >= MIN_RUN) {
            struct run *prev = nruns > 0 ? &runs[nruns - 1] : NULL;

            /* A run broken by a weak window: one preamble. */
            if (prev && prev->band == b && prev->sf == sf && start <= prev->w1 + 2 && same_bin(prev->bin, bin, c->n)) {
                prev->share = (prev->share * (double)(prev->w1 - prev->w0) + sum) / (double)(w - prev->w0);
                prev->w1    = w;
            } else if (nruns < MAX_RUNS) {
                runs[nruns].band    = b;
                runs[nruns].sf      = sf;
                runs[nruns].w0      = start;
                runs[nruns].w1      = w;
                runs[nruns].bin     = bin;
                runs[nruns].share   = sum / len;
                runs[nruns].dropped = false;
                nruns++;
            }
        }
        len = 0;
        if (strong) {
            start = w;
            bin   = pk.bin;
            len   = 1;
            sum   = pk.power / (pk.mean * c->n);
        }
    }
    return nruns;
}

/* Packet */
/*--------*/

/* Symbol k of the grid of a packet: up (dechirped by the down-chirp) or down. */
struct grid {
    double tau;                 /* Start of symbol 0 on the window grid, samples. */
    size_t t0;                  /* Its whole samples. */
    double c_up, c_down;        /* Bins taken off the up and the down symbols. */
};

static bool grid_symbol(struct chirps *c, const struct band *bd, const struct grid *g, long k, bool down,
                        size_t extra, struct peak *pk)
{
    const long a = (long)g->t0 + k * c->n + (long)extra;

    if (a < 0 || (size_t)a + (size_t)c->n > bd->n)
        return false;
    dechirp(c, bd->y + a, down ? c->up : c->down, down ? g->c_down : g->c_up, pk);
    return true;
}

/* Peaks of the up-chirps of a run and of the down-chirps after it, fractional bins. Returns 0,
 * or -1 without down-chirps. */
static int run_peaks(const struct band *bd, const struct run *ru, struct chirps *c, double *b_up, double *b_down)
{
    const int n = c->n;
    const double thr = min_ratio(n);
    const size_t windows = bd->n / (size_t)n;
    struct peak pk;
    double up = 0, best = 0;
    size_t w, w0, w1;
    int nw = 0;

    /* Up-chirps: the windows inside the run (its ends may hold other symbols). */
    w0 = ru->w1 - ru->w0 > 4 ? ru->w0 + 1 : ru->w0;
    w1 = ru->w1 - ru->w0 > 4 ? ru->w1 - 1 : ru->w1;
    for (w = w0; w < w1; w++) {
        dechirp(c, bd->y + w * (size_t)n, c->down, 0.0, &pk);
        up += wrap(frac_peak(c, pk.bin) - ru->bin, n);
        nw++;
    }
    *b_up = ru->bin + up / nw;
    /* Down-chirps: the strongest window after the preamble, dechirped by the up-chirp. */
    for (w = ru->w1; w < ru->w1 + DOWN_SEARCH && w < windows; w++) {
        dechirp(c, bd->y + w * (size_t)n, c->up, 0.0, &pk);
        if (pk.power > thr * pk.mean && pk.power / pk.mean > best) {
            best    = pk.power / pk.mean;
            *b_down = frac_peak(c, pk.bin);
        }
    }
    return best > 0.0 ? 0 : -1;
}

/* A symbol starting tau samples into the window grid, a carrier f bins off: the up-chirps peak
 * at f - tau, the down-chirps at f + tau. f is taken within +-n/4 (+-bw/4). */
static void offsets(double b_up, double b_down, int n, double *f, double *tau)
{
    *f   = wrap(0.5 * (b_up + b_down), 0.5 * n);
    *tau = fmod(fmod(b_down - *f, n) + n, n);
}

/* Fractional peak of the last dechirp from the complex bins (Jacobsen, rectangular window):
 * right for a window without a wrap, a symbol on its own window. */
static double frac_peak_complex(const struct chirps *c, int k)
{
    const float complex a = c->out[(k + c->n - 1) % c->n], b = c->out[k], d = c->out[(k + 1) % c->n];
    const float complex den = 2.0f * b - a - d;

    if (cabsf(den) < 1e-30f)
        return k;
    return k + crealf((a - d) / den);
}

/* Peaks of the up-chirps of a run and of the strongest down-chirp after it, on the windows of
 * symbols starting at t0 + k n: bins from -n/2 to n/2. Returns 0, or -1 without down-chirps. */
static int grid_peaks(const struct band *bd, const struct run *ru, struct chirps *c, size_t t0, double *b_up,
                      double *b_down)
{
    const int n = c->n;
    const double thr = min_ratio(n);
    struct peak pk;
    double up = 0, best = 0;
    size_t a;
    int nw = 0;

    for (a = t0; a + (size_t)n <= bd->n && a + (size_t)n <= ru->w1 * (size_t)n; a += (size_t)n) {
        if (a < ru->w0 * (size_t)n)
            continue;
        dechirp(c, bd->y + a, c->down, 0.0, &pk);
        up += wrap(frac_peak_complex(c, pk.bin), n);
        nw++;
    }
    for (; a + (size_t)n <= bd->n && a < (ru->w1 + DOWN_SEARCH) * (size_t)n; a += (size_t)n) {
        dechirp(c, bd->y + a, c->up, 0.0, &pk);
        if (pk.power > thr * pk.mean && pk.power / pk.mean > best) {
            best    = pk.power / pk.mean;
            *b_down = wrap(frac_peak_complex(c, pk.bin), n);
        }
    }
    if (nw == 0 || best == 0.0)
        return -1;
    *b_up = up / nw;
    return 0;
}

static int analyse_run(const float complex *X, size_t p, size_t count, double sample_rate, const struct band *bd,
                       struct band *al, const struct run *ru, struct chirps *c, struct m2sdr_lora_packet *pkt)
{
    const int n = c->n;
    const double thr = min_ratio(n);
    struct peak pk;
    struct grid g;
    double b_up, b_down = 0, f, tau, advance, pre_peak = 0, pre_noise = 0, power = 0;
    long k, first, ks, shift;
    size_t w, a, end;
    int i, pre = 0;

    memset(pkt, 0, sizeof(*pkt));
    pkt->sf        = ru->sf;
    pkt->bw_hz     = bd->bw;
    pkt->sync_word = -1;

    if (run_peaks(bd, ru, c, &b_up, &b_down) != 0) {
        /* No down-chirps: a preamble cut by the end of the capture is still reported, its
         * offset and timing unknown. */
        if (ru->w1 + 1 < bd->n / (size_t)n)
            return -1;
        pkt->time_s           = (double)(ru->w0 * (size_t)n) / bd->bw;
        pkt->preamble_symbols = (int)(ru->w1 - ru->w0);
        for (w = ru->w0; w < ru->w1; w++) {
            dechirp(c, bd->y + w * (size_t)n, c->down, 0.0, &pk);
            pre_peak  += pk.power;
            pre_noise += noise_bin(c);
        }
        pkt->snr_db    = 10.0 * log10(fmax(pre_peak - pre_noise, 1e-30) / ((double)n * pre_noise + 1e-30));
        for (a = ru->w0 * (size_t)n; a < bd->n; a++)
            power += crealf(bd->y[a] * conjf(bd->y[a]));
        pkt->rssi_dbfs = m2sdr_power_db(power / (double)(bd->n - ru->w0 * (size_t)n));
        pkt->airtime_s = (double)(bd->n - ru->w0 * (size_t)n) / bd->bw;
        return 0;
    }
    /* The band is cut again around the carrier, its chirps then whole, and ahead by the
     * fraction of a sample of the timing: the symbols then start on whole samples t0 + k n.
     * Their peaks are read again on their own windows, where a chirp has no wrap and the bins
     * give the fraction left, until it is within a hundredth of a sample. */
    offsets(b_up, b_down, n, &f, &tau);
    shift   = (long)floor(f * bd->bw / n / (sample_rate / (double)p) + 0.5);
    g.t0    = (size_t)floor(tau + 0.5);
    advance = tau - (double)g.t0;
    for (i = 0; i < ALIGN_PASSES; i++) {
        if (band_cut(X, p, count, al, advance, shift) != 0 || grid_peaks(al, ru, c, g.t0, &b_up, &b_down) != 0)
            return -1;
        f       = 0.5 * (b_up + b_down);
        tau     = 0.5 * (b_down - b_up);
        if (fabs(tau) < 0.01)
            break;
        advance += tau;
    }
    bd = al;
    g.tau    = (double)g.t0 + tau;
    g.c_up   = f - tau;
    g.c_down = f + tau;
    pkt->freq_error_hz = f * bd->bw / n + (double)shift * sample_rate / (double)p;

    /* Up-chirps: from the first symbol of the run, back then forth. */
    first = (long)ceil(((double)(ru->w0 * (size_t)n) - (double)g.t0) / n);
    while (first > 0 && grid_symbol(c, bd, &g, first - 1, false, 0, &pk) &&
           pk.power > thr * pk.mean && same_bin(pk.bin, 0, n))
        first--;
    for (k = first; k < first + MAX_PREAMBLE; k++) {
        if (!grid_symbol(c, bd, &g, k, false, 0, &pk) || pk.power < thr * pk.mean || !same_bin(pk.bin, 0, n))
            break;
        pre_peak  += pk.power;
        pre_noise += noise_bin(c);
        pre++;
    }
    if (pre == 0)
        return -1;
    pkt->preamble_symbols = pre;
    pkt->time_s           = ((double)first * n + g.tau + advance) / bd->bw;
    /* SNR: the peak holds N^2 times the power of the chirp, a bin N times that of the noise. */
    pkt->snr_db = 10.0 * log10(fmax(pre_peak - pre_noise, 1e-30) / ((double)n * pre_noise + 1e-30));
    pre_peak /= pre;

    /* Sync word, then two down-chirps and a quarter. */
    ks = first + pre;
    for (i = 0; i < 2; i++) {
        if (!grid_symbol(c, bd, &g, ks + i, false, 0, &pk))
            break;
        pkt->sync_symbols[i] = pk.bin;
    }
    if (i == 2 && pkt->sync_symbols[0] % 8 == 0 && pkt->sync_symbols[1] % 8 == 0 &&
        pkt->sync_symbols[0] < 128 && pkt->sync_symbols[1] < 128)
        pkt->sync_word = (pkt->sync_symbols[0] / 8) << 4 | (pkt->sync_symbols[1] / 8);
    pkt->sync = i == 2 && grid_symbol(c, bd, &g, ks + 2, true, 0, &pk) &&
                pk.power > thr * pk.mean && same_bin(pk.bin, 0, n);
    end = g.t0 + (size_t)((ks + 2) * n);
    if (pkt->sync) {
        /* Data: from the end of the quarter down-chirp, until the peaks drop. */
        for (k = 0;; k++) {
            if (!grid_symbol(c, bd, &g, ks + 4 + k, false, (size_t)(n / 4), &pk))
                break;      /* Cut by the end of the capture. */
            /* The end: the peak far under those of the preamble, or no peak above the noise. */
            if (pk.power < END_DROP * pre_peak || pk.power < thr * pk.mean) {
                pkt->complete = true;
                break;
            }
            if (k < M2SDR_LORA_MAX_SYMBOLS)
                pkt->symbol[k] = (uint16_t)pk.bin;
            pkt->symbols++;
            end = g.t0 + (size_t)((ks + 5 + k) * n + n / 4);
        }
        if (pkt->symbols == 0)
            end = g.t0 + (size_t)((ks + 4) * n + n / 4);
    }
    a = g.t0 + (size_t)(first * n);
    if (end > bd->n)
        end = bd->n;
    for (power = 0, w = a; w < end; w++)
        power += crealf(bd->y[w] * conjf(bd->y[w]));
    pkt->rssi_dbfs = m2sdr_power_db(power / (double)(end > a ? end - a : 1));
    pkt->airtime_s = (double)(end - a) / bd->bw;
    return 0;
}

/* Analysis */
/*----------*/

static int cmp_packet(const void *a, const void *b)
{
    const double x = ((const struct m2sdr_lora_packet *)a)->time_s, y = ((const struct m2sdr_lora_packet *)b)->time_s;

    return (x > y) - (x < y);
}

int m2sdr_lora_analyse(const float complex *x, size_t count, double sample_rate,
                       const struct m2sdr_lora_config *cfg, struct m2sdr_lora_result *res)
{
    struct band bands[BANDS], aligned[BANDS];
    struct chirps ch[SF_MAX + 1];
    struct run *runs = NULL;
    struct m2sdr_fft *fft = NULL;
    float complex *X = NULL, *xp = NULL;
    double power = 0, busy_end = 0, busy = 0;
    size_t p, i;
    int b, sf, nruns = 0, nb = 0, j, rc = -1;

    memset(bands, 0, sizeof(bands));
    memset(aligned, 0, sizeof(aligned));
    memset(ch, 0, sizeof(ch));
    if (!x || !res || sample_rate <= 0.0)
        return -1;
    memset(res, 0, sizeof(*res));
    for (b = 0; b < BANDS; b++) {
        const double r = sample_rate / bandwidths[b];
        const int ri = (int)floor(r + 0.5);

        bands[b].bw = bandwidths[b];
        if (cfg && cfg->bw_hz > 0.0 && fabs(cfg->bw_hz - bandwidths[b]) > 1.0)
            continue;
        if (ri >= 1 && fabs(r - ri) < 1e-6 * r && (ri & (ri - 1)) == 0) {
            bands[b].r = aligned[b].r = ri;
            aligned[b].bw = bandwidths[b];
            nb++;
        }
    }
    if (nb == 0 || (cfg && cfg->sf != 0 && (cfg->sf < SF_MIN || cfg->sf > SF_MAX)) ||
        (double)count < 16.0 * (1 << SF_MIN) / 500e3 * sample_rate)
        return -1;
    for (i = 0; i < count; i++)
        power += crealf(x[i] * conjf(x[i]));
    res->power_dbfs = m2sdr_power_db(power / (double)count);

    /* The spectrum of the capture, once for the bandwidths. */
    for (p = 1; p < count; p <<= 1)
        ;
    fft  = m2sdr_fft_alloc(p, false);
    X    = malloc(p * sizeof(*X));
    xp   = calloc(p, sizeof(*xp));
    runs = malloc(MAX_RUNS * sizeof(*runs));
    if (!fft || !X || !xp || !runs)
        goto out;
    memcpy(xp, x, count * sizeof(*x));
    m2sdr_fft_run(fft, xp, X);
    free(xp);
    xp = NULL;
    for (b = 0; b < BANDS; b++)
        if (bands[b].r && band_cut(X, p, count, &bands[b], 0.0, 0) != 0)
            goto out;

    for (sf = SF_MIN; sf <= SF_MAX; sf++) {
        if (cfg && cfg->sf != 0 && cfg->sf != sf)
            continue;
        if (chirps_alloc(&ch[sf], sf) != 0)
            goto out;
        for (b = 0; b < BANDS; b++)
            if (bands[b].r && bands[b].n >= (size_t)(MIN_RUN << sf))
                nruns = scan(&bands[b], b, sf, &ch[sf], runs, nruns);
    }

    /* A packet seen by two bandwidths and SFs of the same chirp slope (bw^2 / 2^sf): the one
     * that puts more of the power of a window in its peak is right (the other one sees the chirp
     * in pieces, and twice the noise or more). */
    for (i = 0; i < (size_t)nruns; i++) {
        for (j = 0; j < nruns; j++) {
            const struct run *u = &runs[i], *v = &runs[j];
            const double su = bandwidths[u->band] * bandwidths[u->band] / (1 << u->sf);
            const double sv = bandwidths[v->band] * bandwidths[v->band] / (1 << v->sf);
            const double u0 = (double)(u->w0 << u->sf) / bandwidths[u->band], u1 = (double)(u->w1 << u->sf) / bandwidths[u->band];
            const double v0 = (double)(v->w0 << v->sf) / bandwidths[v->band], v1 = (double)(v->w1 << v->sf) / bandwidths[v->band];

            if ((int)i != j && fabs(su - sv) < 1e-6 * su && u0 < v1 && v0 < u1 &&
                (v->share > u->share || (v->share == u->share && j < (int)i)))
                runs[i].dropped = true;
        }
    }
    for (i = 0; i < (size_t)nruns; i++) {
        struct m2sdr_lora_packet pkt;
        const struct run *u = &runs[i];

        if (u->dropped || analyse_run(X, p, count, sample_rate, &bands[u->band], &aligned[u->band], u, &ch[u->sf], &pkt) != 0)
            continue;
        res->packets++;
        if (res->npackets < M2SDR_LORA_MAX_PACKETS)
            res->packet[res->npackets++] = pkt;
    }
    qsort(res->packet, (size_t)res->npackets, sizeof(res->packet[0]), cmp_packet);
    /* Busy time: the union of the packets. */
    for (j = 0; j < res->npackets; j++) {
        const double s = fmax(res->packet[j].time_s, busy_end), e = res->packet[j].time_s + res->packet[j].airtime_s;

        if (e > s)
            busy += e - s;
        if (e > busy_end)
            busy_end = e;
    }
    res->busy_fraction = busy / ((double)count / sample_rate);
    rc = 0;
out:
    for (sf = SF_MIN; sf <= SF_MAX; sf++)
        chirps_free(&ch[sf]);
    for (b = 0; b < BANDS; b++) {
        free(bands[b].y);
        free(aligned[b].y);
    }
    m2sdr_fft_free(fft);
    free(X);
    free(xp);
    free(runs);
    return rc;
}
