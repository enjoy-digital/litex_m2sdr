/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: demodulation of a single-carrier PSK or QAM signal.
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

#define SPS               4          /* Samples per symbol after the resampler. */
#define RRC_SPAN          8          /* Symbols of the receive filter on each side. */
#define EDGE_SYMBOLS      (RRC_SPAN + 2)   /* Left out at each end: the filter is not full there. */
#define MIN_SYMBOLS       256
#define TIMING_BLOCK      256        /* Symbols per timing estimate. */
#define TIMING_MIN_BLOCKS 8          /* Blocks for a line through the estimates; fewer: their mean. */
#define TRIAL_SYMBOLS     2048       /* For the choice of the modulation and of the roll-off. */
#define KERNEL_ZC         6          /* Zero crossings of the resampler kernel on each side. */
#define KERNEL_PHASES     64         /* Table points per zero crossing. */
#define MIN_SAMPLES       4096
#define SEARCH_FFT        4096
#define SIGNAL_MIN_RATIO  5.0        /* Signal plateau over the quiet part of the capture. */
#define RATE_LINE_RATIO   8.0        /* Symbol rate line over the spectrum around it. */
#define LOOP_KP           0.0267     /* Carrier loop: a hundredth of the symbol rate wide. */
#define LOOP_KI           3.6e-4

static const char *const mod_names[M2SDR_MOD_COUNT] = {
    "auto", "BPSK", "QPSK", "8PSK", "16QAM", "32QAM", "64QAM", "128QAM", "256QAM",
};

/* Constellations */
/*----------------*/

const char *m2sdr_modulation_name(enum m2sdr_modulation modulation)
{
    return (int)modulation >= 0 && modulation < M2SDR_MOD_COUNT ? mod_names[modulation] : "?";
}

/* Square or cross grid of `side` levels a side, the corners of `cut` levels removed. */
static int qam_points(int side, int cut, float complex *points)
{
    int i, q, n = 0;

    for (i = 0; i < side; i++) {
        for (q = 0; q < side; q++) {
            const bool edge_i = i < cut || i >= side - cut;
            const bool edge_q = q < cut || q >= side - cut;
            if (edge_i && edge_q)
                continue;
            points[n++] = (float)(2 * i - side + 1) + I * (float)(2 * q - side + 1);
        }
    }
    return n;
}

int m2sdr_constellation(enum m2sdr_modulation modulation, float complex *points)
{
    double power = 0;
    int n = 0, i;

    if (!points)
        return 0;
    switch (modulation) {
    case M2SDR_MOD_BPSK:
        points[0] = 1.0f;
        points[1] = -1.0f;
        n = 2;
        break;
    case M2SDR_MOD_QPSK:   n = qam_points(2, 0, points);  break;
    case M2SDR_MOD_8PSK:
        for (n = 0; n < 8; n++)
            points[n] = (float complex)cexp(I * M_PI * n / 4.0);
        break;
    case M2SDR_MOD_16QAM:  n = qam_points(4, 0, points);  break;
    case M2SDR_MOD_32QAM:  n = qam_points(6, 1, points);  break;
    case M2SDR_MOD_64QAM:  n = qam_points(8, 0, points);  break;
    case M2SDR_MOD_128QAM: n = qam_points(12, 2, points); break;
    case M2SDR_MOD_256QAM: n = qam_points(16, 0, points); break;
    default: return 0;
    }
    for (i = 0; i < n; i++)
        power += crealf(points[i]) * crealf(points[i]) + cimagf(points[i]) * cimagf(points[i]);
    power = sqrt(power / n);
    for (i = 0; i < n; i++)
        points[i] /= (float)power;
    return n;
}

struct constellation {
    float complex points[256];
    int    count;
    double dmin;                  /* Smallest distance between two points. */
    int    power;                 /* Power that turns the points into a constant: 2, 4 or 8. */
    double power_phase;           /* Phase of the mean of the points at that power. */
};

static void constellation_init(struct constellation *c, enum m2sdr_modulation modulation)
{
    double complex sum = 0;
    int i, j, k;

    c->count = m2sdr_constellation(modulation, c->points);
    c->power = modulation == M2SDR_MOD_BPSK ? 2 : modulation == M2SDR_MOD_8PSK ? 8 : 4;
    c->dmin  = 1e9;
    for (i = 0; i < c->count; i++) {
        double complex p = 1.0;
        for (k = 0; k < c->power; k++)
            p *= c->points[i];
        sum += p;
        for (j = i + 1; j < c->count; j++) {
            const double d = cabsf(c->points[i] - c->points[j]);
            if (d < c->dmin)
                c->dmin = d;
        }
    }
    c->power_phase = carg(sum);
}

static float complex slice(const struct constellation *c, float complex y)
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

/* Front end */
/*-----------*/

/* Signal band from a smoothed spectrum: the plateau of the strongest signal, and the points
 * where it is 3 dB down. A raised cosine spectrum is 3 dB down half the symbol rate from its
 * centre, whatever its roll-off: their distance is a first value of the symbol rate. */
static int find_signal(const float complex *x, size_t count, double sample_rate,
                       double *center_hz, double *width_hz, double *power_db)
{
    struct m2sdr_psd psd;
    size_t fft_len = SEARCH_FFT, k, peak = 0, lo, hi, lo3, hi3, n;
    float *smooth = NULL, *sorted = NULL;
    double plateau, quiet;
    int half, rc = -1;

    memset(&psd, 0, sizeof(psd));
    while (fft_len * 2 > count)
        fft_len >>= 1;
    if (m2sdr_psd_compute(x, count, sample_rate, fft_len, M2SDR_WINDOW_HANN, &psd) != 0)
        return -1;
    smooth = malloc(fft_len * sizeof(*smooth));
    sorted = malloc(fft_len * sizeof(*sorted));
    if (!smooth || !sorted)
        goto out;

    /* Mean over about a hundredth of the span. */
    half = (int)(fft_len / 256);
    if (half < 2)
        half = 2;
    for (k = 0; k < fft_len; k++) {
        const size_t a = k > (size_t)half ? k - (size_t)half : 0;
        const size_t b = k + (size_t)half < fft_len ? k + (size_t)half : fft_len - 1;
        double sum = 0;
        size_t i;

        for (i = a; i <= b; i++)
            sum += psd.power[i];
        smooth[k] = (float)(sum / (double)(b - a + 1));
        if (smooth[k] > smooth[peak])
            peak = k;
    }
    /* Quiet level: the tenth of the bins that is the lowest. */
    memcpy(sorted, smooth, fft_len * sizeof(*sorted));
    {
        size_t i, j;
        /* Partial selection is not worth it here: a few thousand floats. */
        for (i = 1; i < fft_len; i++) {
            const float v = sorted[i];
            for (j = i; j > 0 && sorted[j - 1] > v; j--)
                sorted[j] = sorted[j - 1];
            sorted[j] = v;
        }
    }
    quiet = sorted[fft_len / 10];

    /* Band at 10 dB under the highest point, then the plateau as the median of its middle. */
    for (lo = peak; lo > 0 && smooth[lo - 1] > 0.1f * smooth[peak]; lo--)
        ;
    for (hi = peak; hi + 1 < fft_len && smooth[hi + 1] > 0.1f * smooth[peak]; hi++)
        ;
    n = hi - lo + 1;
    if (n < 8)
        goto out;
    memcpy(sorted, smooth + lo + n / 4, (n / 2) * sizeof(*sorted));
    {
        size_t i, j;
        for (i = 1; i < n / 2; i++) {
            const float v = sorted[i];
            for (j = i; j > 0 && sorted[j - 1] > v; j--)
                sorted[j] = sorted[j - 1];
            sorted[j] = v;
        }
    }
    plateau = sorted[n / 4];
    if (!(plateau > SIGNAL_MIN_RATIO * quiet) && !(quiet <= 0)) {
        rc = 1;                   /* Nothing stands out. */
        goto out;
    }
    for (lo3 = lo; lo3 < peak && smooth[lo3] < 0.5 * plateau; lo3++)
        ;
    for (hi3 = hi; hi3 > peak && smooth[hi3] < 0.5 * plateau; hi3--)
        ;
    if (hi3 <= lo3 + 2) {
        rc = 1;
        goto out;
    }
    *center_hz = 0.5 * (m2sdr_psd_freq(&psd, lo3) + m2sdr_psd_freq(&psd, hi3));
    *width_hz  = m2sdr_psd_freq(&psd, hi3) - m2sdr_psd_freq(&psd, lo3);
    *power_db  = m2sdr_psd_band_power_db(&psd, m2sdr_psd_freq(&psd, lo), m2sdr_psd_freq(&psd, hi));
    rc = 0;

out:
    free(smooth);
    free(sorted);
    m2sdr_psd_free(&psd);
    return rc;
}

/* Kernel of the resampler: a sinc under a Hann window, as a table over its half. */
static void kernel_init(float *table)
{
    int i;

    for (i = 0; i <= KERNEL_ZC * KERNEL_PHASES; i++) {
        const double u = (double)i / KERNEL_PHASES;
        const double sinc = i ? sin(M_PI * u) / (M_PI * u) : 1.0;
        table[i] = (float)(sinc * (0.5 + 0.5 * cos(M_PI * u / KERNEL_ZC)));
    }
    table[KERNEL_ZC * KERNEL_PHASES + 1] = 0.0f;
}

/* x, already mixed to the carrier, at out_rate. The cutoff is half the lower of the two rates.
 * Returns the number of samples written, out_max at most. */
static size_t resample(const float complex *x, size_t count, double sample_rate, double out_rate,
                       float complex *y, size_t out_max)
{
    static float table[KERNEL_ZC * KERNEL_PHASES + 2];
    static bool ready;
    const double ratio = sample_rate / out_rate;          /* Input samples per output sample. */
    const double zc = ratio > 1.0 ? ratio : 1.0;          /* Input samples per zero crossing. */
    const double width = KERNEL_ZC * zc;
    size_t j;

    if (!ready) {
        kernel_init(table);
        ready = true;
    }
    for (j = 0; j < out_max; j++) {
        const double c = (double)j * ratio;
        long n0 = (long)ceil(c - width), n1 = (long)floor(c + width), n;
        float complex acc = 0;
        float norm = 0;

        if (n1 >= (long)count)
            break;
        if (n0 < 0)
            n0 = 0;
        for (n = n0; n <= n1; n++) {
            const double u = fabs((double)n - c) / zc * KERNEL_PHASES;
            const int i = (int)u;
            const float h = table[i] + (float)(u - i) * (table[i + 1] - table[i]);

            acc  += x[n] * h;
            norm += h;
        }
        y[j] = norm != 0 ? acc / norm : 0;
    }
    return j;
}

/* Root raised cosine, time in symbols. */
static double rrc(double t, double alpha)
{
    if (fabs(t) < 1e-9)
        return 1.0 - alpha + 4.0 * alpha / M_PI;
    if (fabs(fabs(t) - 1.0 / (4.0 * alpha)) < 1e-9)
        return alpha / sqrt(2.0) * ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * alpha)) +
                                    (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * alpha)));
    return (sin(M_PI * t * (1.0 - alpha)) + 4.0 * alpha * t * cos(M_PI * t * (1.0 + alpha))) /
           (M_PI * t * (1.0 - 16.0 * alpha * alpha * t * t));
}

/* Receive filter for a pulse, at SPS samples per symbol: y to out, of the same length. */
static void receive_filter(const float complex *y, size_t n, enum m2sdr_pulse pulse, double alpha,
                           float complex *out)
{
    float taps[2 * RRC_SPAN * SPS + 1];
    int ntaps = 2 * RRC_SPAN * SPS + 1, i;
    double sum = 0;
    size_t k;

    if (pulse == M2SDR_PULSE_RC) {
        memcpy(out, y, n * sizeof(*y));
        return;
    }
    if (pulse == M2SDR_PULSE_RECT) {
        ntaps = SPS;
        for (i = 0; i < ntaps; i++)
            taps[i] = 1.0f / SPS;
    } else {
        for (i = 0; i < ntaps; i++) {
            taps[i] = (float)rrc((double)(i - RRC_SPAN * SPS) / SPS, alpha);
            sum    += taps[i];
        }
        for (i = 0; i < ntaps; i++)
            taps[i] = (float)(taps[i] / sum);
    }
    /* Centred: the output is aligned with the input. */
    for (k = 0; k < n; k++) {
        float complex acc = 0;
        for (i = 0; i < ntaps; i++) {
            const long at = (long)k + i - ntaps / 2;
            if (at >= 0 && at < (long)n)
                acc += y[at] * taps[i];
        }
        out[k] = acc;
    }
}

/* Value of y between its samples: cubic through the four around. */
static float complex sample_at(const float complex *y, size_t n, double pos)
{
    const long i = (long)floor(pos);
    const float t = (float)(pos - (double)i);

    if (i < 1 || i + 2 >= (long)n)
        return 0;
    return y[i - 1] * (-t * (t - 1) * (t - 2) / 6) + y[i] * ((t + 1) * (t - 1) * (t - 2) / 2) +
           y[i + 1] * (-(t + 1) * t * (t - 2) / 2) + y[i + 2] * ((t + 1) * t * (t - 1) / 6);
}

/* Symbol timing: the envelope of the filtered signal has a line at the symbol rate, whose
 * phase gives the symbol instants. It is read per block, and a line through the blocks takes
 * the error left in the symbol rate. Position of symbol k: pos0 + k * step, in samples. */
static void symbol_timing(const float complex *y, size_t n, double *pos0, double *step)
{
    const size_t block = (size_t)TIMING_BLOCK * SPS;
    const size_t blocks = n / block;
    double prev = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    double complex all = 0;
    size_t b, i;

    *pos0 = 0;
    *step = SPS;
    for (b = 0; b < blocks; b++) {
        double complex acc = 0;
        double tau, centre = ((double)b + 0.5) * (double)block;

        for (i = 0; i < block; i++) {
            const float complex v = y[b * block + i];
            const double p = crealf(v) * crealf(v) + cimagf(v) * cimagf(v);
            /* exp(-j 2 pi n / SPS) with SPS = 4: 1, -j, -1, j. */
            switch (i & 3) {
            case 0:  acc += p;      break;
            case 1:  acc += -I * p; break;
            case 2:  acc += -p;     break;
            default: acc += I * p;  break;
            }
        }
        all += acc;
        tau  = -carg(acc) * SPS / (2.0 * M_PI);
        /* Unwrapped against the block before: a symbol is SPS samples. */
        if (b) {
            while (tau - prev > SPS / 2.0)
                tau -= SPS;
            while (tau - prev < -SPS / 2.0)
                tau += SPS;
        }
        prev = tau;
        sx  += centre;
        sy  += tau;
        sxx += centre * centre;
        sxy += centre * tau;
    }
    if (blocks >= TIMING_MIN_BLOCKS) {
        const double den = (double)blocks * sxx - sx * sx;
        const double slope = den != 0 ? ((double)blocks * sxy - sx * sy) / den : 0.0;
        const double offset = (sy - slope * sx) / (double)blocks;
        /* tau(n) = offset + slope * n, and symbol k sits at n = SPS * k + tau(n). */
        *step = SPS / (1.0 - slope);
        *pos0 = offset / (1.0 - slope);
    } else if (blocks >= 1) {
        /* A short capture: one estimate over all of it is steadier than a slope. */
        *pos0 = -carg(all) * SPS / (2.0 * M_PI);
    }
    /* First symbol after the edge of the filter. */
    while (*pos0 < EDGE_SYMBOLS * SPS)
        *pos0 += *step;
}

/* Carrier and symbols */
/*---------------------*/

/* Strength and frequency of the line of z^power: 1 for a clean PSK, in cycles per symbol. */
static double power_line(const float complex *z, size_t n, int power, double *freq, double complex *value)
{
    size_t len = 1, k, at = 0;
    struct m2sdr_fft *fft;
    float complex *in, *out;
    double total = 0, best = 0, delta = 0;
    int p;

    while (len < 4 * n)
        len <<= 1;
    fft = m2sdr_fft_alloc(len, false);
    in  = calloc(len, sizeof(*in));
    out = malloc(len * sizeof(*out));
    if (!fft || !in || !out) {
        m2sdr_fft_free(fft);
        free(in);
        free(out);
        return -1.0;
    }
    for (k = 0; k < n; k++) {
        float complex v = z[k];
        for (p = 1; p < power; p++)
            v *= z[k];
        in[k]  = v;
        total += cabsf(v);
    }
    m2sdr_fft_run(fft, in, out);
    for (k = 0; k < len; k++) {
        const double m = cabsf(out[k]);
        if (m > best) {
            best = m;
            at   = k;
        }
    }
    {
        const double a = cabsf(out[(at + len - 1) % len]), c = cabsf(out[(at + 1) % len]);
        const double den = a - 2.0 * best + c;
        if (den < 0)
            delta = 0.5 * (a - c) / den;
    }
    if (freq) {
        double f = ((double)at + delta) / (double)len;
        if (f > 0.5)
            f -= 1.0;
        *freq = f / power;
    }
    if (value)
        *value = out[at];
    m2sdr_fft_free(fft);
    free(in);
    free(out);
    return total > 0 ? best / total : 0.0;
}

struct symbols_result {
    double evm_rms, evm_peak, mag_err_rms, phase_err_rad;
    double freq;                  /* Carrier left in the symbols, cycles per symbol. */
    double complex offset;        /* Mean of the error. */
    double gain_imbalance_db, quadrature_deg;
};

/* Carrier recovery and decisions on n symbols of unit mean power. sym and ref (optional)
 * receive the corrected symbols and the points chosen; rot receives, per symbol, the factor
 * that took it from z to sym. */
static int demod_symbols(const float complex *z, size_t n, const struct constellation *c,
                         struct symbols_result *r, float complex *sym, float complex *ref, float complex *rot)
{
    float complex *y = malloc(n * sizeof(*y));
    float complex *d = malloc(n * sizeof(*d));
    float complex *w = rot ? rot : malloc(n * sizeof(*w));
    double complex line = 0, gain_num = 0, off = 0;
    double freq = 0, theta, phase, rate = 0, gain_den = 0;
    double se = 0, sd = 0, sm = 0, sp = 0, peak = 0;
    double sii = 0, sqq = 0, siq = 0, sxi = 0, sxq = 0, syi = 0, syq = 0, a11, a12, a21, a22;
    size_t k, np = 0;
    int rc = -1;

    if (!y || !d || !w)
        goto out;
    memset(r, 0, sizeof(*r));
    if (power_line(z, n, c->power, &freq, &line) < 0)
        goto out;
    /* The phase of the line is the one of the points at that power, plus power times the
     * carrier phase at the first symbol. */
    theta = (carg(line) - c->power_phase) / c->power;
    phase = theta;

    /* Decision-directed loop over the symbols. */
    for (k = 0; k < n; k++) {
        const float complex e = (float complex)cexp(-I * (phase + 2.0 * M_PI * freq * (double)k));
        double err;

        w[k] = e;
        y[k] = z[k] * e;
        d[k] = slice(c, y[k]);
        err  = cimagf(y[k] * conjf(d[k])) / (cabsf(y[k]) * cabsf(d[k]) + 1e-12);
        rate  += LOOP_KI * err;
        phase += LOOP_KP * err + rate;
        gain_num += y[k] * conjf(d[k]);
        gain_den += crealf(d[k]) * crealf(d[k]) + cimagf(d[k]) * cimagf(d[k]);
    }
    r->freq = freq + rate / (2.0 * M_PI);
    if (!(gain_den > 0) || cabs(gain_num) == 0)
        goto out;

    /* Scale and mean phase by least squares against the decisions, then the figures. */
    {
        const float complex g = (float complex)(gain_den / gain_num);

        for (k = 0; k < n; k++) {
            float complex e;
            double m, a, dm;

            w[k] *= g;
            y[k] *= g;
            d[k]  = slice(c, y[k]);
            e     = y[k] - d[k];
            m     = crealf(e) * crealf(e) + cimagf(e) * cimagf(e);
            dm    = cabsf(d[k]);
            se   += m;
            sd   += dm * dm;
            off  += e;
            if (m > peak)
                peak = m;
            a   = cabsf(y[k]) - dm;
            sm += a * a;
            if (dm > 1e-6) {
                const double ph = carg(y[k] * conjf(d[k]));
                sp += ph * ph;
                np++;
            }
            /* Linear fit of the rails: [yi, yq] = A [di, dq]. */
            sii += crealf(d[k]) * crealf(d[k]);
            sqq += cimagf(d[k]) * cimagf(d[k]);
            siq += crealf(d[k]) * cimagf(d[k]);
            sxi += crealf(y[k]) * crealf(d[k]);
            sxq += crealf(y[k]) * cimagf(d[k]);
            syi += cimagf(y[k]) * crealf(d[k]);
            syq += cimagf(y[k]) * cimagf(d[k]);
        }
    }
    r->evm_rms       = sqrt(se / sd);
    r->evm_peak      = sqrt(peak / (sd / (double)n));
    r->mag_err_rms   = sqrt(sm / sd);
    r->phase_err_rad = np ? sqrt(sp / (double)np) : 0.0;
    r->offset        = off / (double)n;
    {
        const double det = sii * sqq - siq * siq;
        if (fabs(det) > 1e-12) {
            double gi, gq, dot;

            a11 = (sxi * sqq - sxq * siq) / det;
            a12 = (sxq * sii - sxi * siq) / det;
            a21 = (syi * sqq - syq * siq) / det;
            a22 = (syq * sii - syi * siq) / det;
            /* The columns of A are the images of the I and of the Q rails. */
            gi  = hypot(a11, a21);
            gq  = hypot(a12, a22);
            dot = (a11 * a12 + a21 * a22) / (gi * gq);
            if (dot > 1.0)
                dot = 1.0;
            if (dot < -1.0)
                dot = -1.0;
            r->gain_imbalance_db = 20.0 * log10(gi / gq);
            r->quadrature_deg    = asin(dot) * 180.0 / M_PI;
        }
    }
    if (sym)
        memcpy(sym, y, n * sizeof(*y));
    if (ref)
        memcpy(ref, d, n * sizeof(*d));
    rc = 0;

out:
    free(y);
    free(d);
    if (!rot)
        free(w);
    return rc;
}

/* Symbols of the filtered signal at their instants, scaled to a mean power of 1; *scale is
 * the factor applied. */
static void take_symbols(const float complex *yf, size_t ny, double pos0, double step, float complex *z,
                         size_t n, double *scale)
{
    double power = 0, g;
    size_t k;

    for (k = 0; k < n; k++) {
        z[k]   = sample_at(yf, ny, pos0 + step * (double)k);
        power += crealf(z[k]) * crealf(z[k]) + cimagf(z[k]) * cimagf(z[k]);
    }
    g = power > 0 ? sqrt((double)n / power) : 1.0;
    for (k = 0; k < n; k++)
        z[k] *= (float)g;
    if (scale)
        *scale = g;
}

/* Modulation of symbols, from the lines of their powers, then from the EVM against each
 * candidate: the smallest constellation that the symbols fit. */
static enum m2sdr_modulation find_modulation(const float complex *z, size_t n)
{
    static const enum m2sdr_modulation family[] = {
        M2SDR_MOD_QPSK, M2SDR_MOD_16QAM, M2SDR_MOD_32QAM, M2SDR_MOD_64QAM, M2SDR_MOD_128QAM, M2SDR_MOD_256QAM,
    };
    const double c2 = power_line(z, n, 2, NULL, NULL);
    const double c4 = power_line(z, n, 4, NULL, NULL);
    enum m2sdr_modulation best = M2SDR_MOD_QPSK;
    double best_ratio = 1e9;
    unsigned i;

    if (c2 > 0.5)
        return M2SDR_MOD_BPSK;
    if (c4 < 0.08) {
        /* No line at the fourth power: 8PSK has its own at the eighth. */
        return power_line(z, n, 8, NULL, NULL) > 0.3 ? M2SDR_MOD_8PSK : M2SDR_MOD_QPSK;
    }
    for (i = 0; i < sizeof(family) / sizeof(family[0]); i++) {
        struct constellation c;
        struct symbols_result r;
        double ratio;

        constellation_init(&c, family[i]);
        if (demod_symbols(z, n, &c, &r, NULL, NULL, NULL) != 0)
            continue;
        ratio = r.evm_rms / c.dmin;
        if (ratio < 0.2)
            return family[i];
        if (ratio < best_ratio) {
            best_ratio = ratio;
            best       = family[i];
        }
    }
    return best;
}

/* Demodulation */
/*--------------*/

int m2sdr_demod(const float complex *x, size_t count, double sample_rate,
                const struct m2sdr_demod_config *cfg, struct m2sdr_demod_result *res)
{
    static const double rolloffs[] = { 0.15, 0.22, 0.35, 0.5 };
    float complex *xm = NULL, *y = NULL, *yf = NULL, *z = NULL, *rot = NULL;
    double center = 0, width = 0, power_db = -200.0, symbol_rate, alpha, pos0, step, scale;
    struct constellation c;
    struct symbols_result r;
    enum m2sdr_modulation modulation;
    size_t nsym, ny, got, k, trial;
    int rc = -1, found, i;

    if (!x || !cfg || !res || count < MIN_SAMPLES || !(sample_rate > 0) ||
        (cfg->symbol_rate != 0 && !(cfg->symbol_rate > 0 && cfg->symbol_rate < sample_rate)) ||
        (cfg->rolloff != 0 && !(cfg->rolloff >= 0.05 && cfg->rolloff <= 1.0)) ||
        (int)cfg->modulation < 0 || cfg->modulation >= M2SDR_MOD_COUNT)
        return -1;
    memset(res, 0, sizeof(*res));

    found = find_signal(x, count, sample_rate, &center, &width, &power_db);
    if (found < 0)
        return -1;
    if (found > 0)
        return 0;
    res->power_db       = power_db;
    res->freq_offset_hz = center;

    /* Signal to its centre. */
    xm = malloc(count * sizeof(*xm));
    if (!xm)
        goto out;
    {
        const float complex stepv = (float complex)cexp(-2.0 * M_PI * I * center / sample_rate);
        float complex osc = 1.0f;

        for (k = 0; k < count; k++) {
            xm[k] = x[k] * osc;
            osc  *= stepv;
            if ((k & 1023) == 1023)
                osc /= cabsf(osc);
        }
    }

    /* Symbol rate: the line of the envelope, searched around the width of the spectrum. */
    symbol_rate = cfg->symbol_rate;
    if (symbol_rate == 0) {
        const double rate = 8.0 * width;
        const size_t np = (size_t)((double)count * rate / sample_rate) - 16;
        float complex *e = malloc(np * sizeof(*e));
        struct m2sdr_psd psd;
        struct m2sdr_psd_peak line;
        size_t fft_len = 65536;
        double mean = 0, around;

        if (!e)
            goto out;
        got = resample(xm, count, sample_rate, rate, e, np);
        for (k = 0; k < got; k++) {
            e[k]  = crealf(e[k]) * crealf(e[k]) + cimagf(e[k]) * cimagf(e[k]);
            mean += crealf(e[k]);
        }
        mean /= (double)(got ? got : 1);
        for (k = 0; k < got; k++)
            e[k] -= (float)mean;
        while (fft_len * 2 > got && fft_len > 64)
            fft_len >>= 1;
        memset(&psd, 0, sizeof(psd));
        if (got < fft_len || m2sdr_psd_compute(e, got, rate, fft_len, M2SDR_WINDOW_HANN, &psd) != 0) {
            free(e);
            goto out;
        }
        free(e);
        if (m2sdr_psd_peak(&psd, 0.7 * width, 1.4 * width, &line) != 0) {
            m2sdr_psd_free(&psd);
            goto out;
        }
        around = pow(10.0, (m2sdr_psd_noise_density_db(&psd, 0.7 * width, 1.4 * width) + 10.0 * log10(psd.rbw_hz)) / 10.0);
        m2sdr_psd_free(&psd);
        if (pow(10.0, line.power_db / 10.0) < RATE_LINE_RATIO * around) {
            /* No line: a roll-off near zero, or not a linear modulation. */
            rc = 0;
            goto out;
        }
        symbol_rate = line.freq_hz;
    }
    res->symbol_rate = symbol_rate;
    res->signal      = true;

    /* Symbols that fit in the capture. */
    nsym = cfg->max_symbols ? cfg->max_symbols : M2SDR_DEMOD_MAX_SYMBOLS;
    if (nsym > M2SDR_DEMOD_MAX_SYMBOLS)
        nsym = M2SDR_DEMOD_MAX_SYMBOLS;
    {
        const double fit = (double)count * symbol_rate / sample_rate - 2.0 * EDGE_SYMBOLS - 2.0 * KERNEL_ZC - 4.0;
        if (fit < MIN_SYMBOLS) {
            rc = 0;
            goto out;
        }
        if ((double)nsym > fit)
            nsym = (size_t)fit;
    }
    ny  = (nsym + 2 * EDGE_SYMBOLS + 2) * SPS;
    y   = malloc(ny * sizeof(*y));
    yf  = malloc(ny * sizeof(*yf));
    z   = malloc(nsym * sizeof(*z));
    rot = malloc(nsym * sizeof(*rot));
    if (!y || !yf || !z || !rot)
        goto out;
    got = resample(xm, count, sample_rate, SPS * symbol_rate, y, ny);
    if (got < (size_t)(MIN_SYMBOLS + 2 * EDGE_SYMBOLS) * SPS) {
        rc = 0;
        goto out;
    }
    ny   = got;
    if (nsym > ny / SPS - 2 * EDGE_SYMBOLS - 2)
        nsym = ny / SPS - 2 * EDGE_SYMBOLS - 2;
    trial = nsym < TRIAL_SYMBOLS ? nsym : TRIAL_SYMBOLS;

    /* Modulation, with a usual roll-off when it is not given. */
    alpha = cfg->rolloff > 0 ? cfg->rolloff : 0.35;
    receive_filter(y, ny, cfg->pulse, alpha, yf);
    symbol_timing(yf, ny, &pos0, &step);
    take_symbols(yf, ny, pos0, step, z, trial, NULL);
    modulation = cfg->modulation != M2SDR_MOD_AUTO ? cfg->modulation : find_modulation(z, trial);
    constellation_init(&c, modulation);

    /* Roll-off: the one that gives the lowest EVM. */
    if (cfg->rolloff == 0 && cfg->pulse == M2SDR_PULSE_RRC) {
        double best = 1e9;

        for (i = 0; i < (int)(sizeof(rolloffs) / sizeof(rolloffs[0])); i++) {
            receive_filter(y, ny, cfg->pulse, rolloffs[i], yf);
            symbol_timing(yf, ny, &pos0, &step);
            take_symbols(yf, ny, pos0, step, z, trial, NULL);
            if (demod_symbols(z, trial, &c, &r, NULL, NULL, NULL) == 0 && r.evm_rms < best) {
                best  = r.evm_rms;
                alpha = rolloffs[i];
            }
        }
        receive_filter(y, ny, cfg->pulse, alpha, yf);
        symbol_timing(yf, ny, &pos0, &step);
    }
    res->rolloff    = cfg->pulse == M2SDR_PULSE_RRC || cfg->pulse == M2SDR_PULSE_RC ? alpha : 0.0;
    res->modulation = modulation;

    /* All the symbols. */
    take_symbols(yf, ny, pos0, step, z, nsym, &scale);
    if (demod_symbols(z, nsym, &c, &r, res->sym, res->ref, rot) != 0)
        goto out;
    res->symbols           = nsym;
    res->evm_rms           = r.evm_rms;
    res->evm_peak          = r.evm_peak;
    res->mer_db            = r.evm_rms > 0 ? -20.0 * log10(r.evm_rms) : 200.0;
    res->mag_err_rms       = r.mag_err_rms;
    res->phase_err_deg     = r.phase_err_rad * 180.0 / M_PI;
    res->iq_offset_db      = m2sdr_power_db(creal(r.offset) * creal(r.offset) + cimag(r.offset) * cimag(r.offset));
    res->gain_imbalance_db = r.gain_imbalance_db;
    res->quadrature_deg    = r.quadrature_deg;
    /* The timing line took the error of the symbol rate, the loop the one of the carrier. */
    res->symbol_rate      *= SPS / step;
    res->freq_offset_hz   += r.freq * res->symbol_rate;
    res->locked            = r.evm_rms < 0.25 * c.dmin;

    /* Eye: two symbols around symbols spread over the capture, with their carrier removed. */
    res->eye_traces = 0;
    for (i = 0; i < M2SDR_DEMOD_EYE_TRACES; i++) {
        const size_t at = (size_t)i * (nsym - 2) / M2SDR_DEMOD_EYE_TRACES;
        int m;

        for (m = 0; m < M2SDR_DEMOD_EYE_POINTS; m++)
            res->eye[i][m] = sample_at(yf, ny, pos0 + step * ((double)at + (m - 4) / 8.0)) * (float)scale * rot[at];
        res->eye_traces++;
    }
    rc = 0;

out:
    free(xm);
    free(y);
    free(yf);
    free(z);
    free(rot);
    return rc;
}
