/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: spectrum and time-domain measurements.
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

/* Helpers */
/*---------*/

double m2sdr_power_db(double power)
{
    return power > 1e-20 ? 10.0 * log10(power) : -200.0;
}

static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Bins of a range of frequencies, clamped to the spectrum. Returns -1 when it holds no bin. */
static int psd_range(const struct m2sdr_psd *psd, double lo_hz, double hi_hz, size_t *k0, size_t *k1)
{
    const double half = (double)(psd->bins / 2);
    double a, b;

    if (!psd || !psd->power || !psd->bins || !(hi_hz >= lo_hz))
        return -1;
    a = ceil(lo_hz / psd->bin_hz + half - 1e-9);
    b = floor(hi_hz / psd->bin_hz + half + 1e-9);
    if (a < 0)
        a = 0;
    if (b > (double)(psd->bins - 1))
        b = (double)(psd->bins - 1);
    if (b < a)
        return -1;
    *k0 = (size_t)a;
    *k1 = (size_t)b;
    return 0;
}

/* Spectrum */
/*----------*/

static void window_fill(enum m2sdr_window kind, float *w, size_t n)
{
    static const double hann[]    = { 0.5, 0.5 };
    static const double flattop[] = { 0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368 };
    static const double bh[]      = { 0.35875, 0.48829, 0.14128, 0.01168 };
    static const double rect[]    = { 1.0 };
    const double *a = hann;
    int terms = 2, t;
    size_t i;

    switch (kind) {
    case M2SDR_WINDOW_FLATTOP:         a = flattop; terms = 5; break;
    case M2SDR_WINDOW_BLACKMAN_HARRIS: a = bh;      terms = 4; break;
    case M2SDR_WINDOW_RECTANGULAR:     a = rect;    terms = 1; break;
    default: break;
    }
    for (i = 0; i < n; i++) {
        double v = 0, sign = 1;
        for (t = 0; t < terms; t++) {
            v    += sign * a[t] * cos(2.0 * M_PI * t * (double)i / (double)n);
            sign  = -sign;
        }
        w[i] = (float)v;
    }
}

/* Half width of the main lobe of a window, in bins, with a margin. */
static size_t window_lobe_bins(int kind)
{
    switch (kind) {
    case M2SDR_WINDOW_FLATTOP:         return 6;
    case M2SDR_WINDOW_BLACKMAN_HARRIS: return 5;
    case M2SDR_WINDOW_RECTANGULAR:     return 8;
    default:                           return 3;
    }
}

void m2sdr_psd_free(struct m2sdr_psd *psd)
{
    if (!psd)
        return;
    m2sdr_fft_free(psd->fft);
    free(psd->power);
    free(psd->win);
    free(psd->work);
    memset(psd, 0, sizeof(*psd));
}

int m2sdr_psd_compute(const float complex *x, size_t count, double sample_rate, size_t fft_len,
                      enum m2sdr_window window, struct m2sdr_psd *psd)
{
    const size_t hop = fft_len / 2;
    double sum = 0, sum2 = 0, scale;
    float complex *in, *out;
    size_t i, start;
    unsigned segments = 0;

    if (!x || !psd || fft_len < 8 || (fft_len & (fft_len - 1)) || count < fft_len || !(sample_rate > 0))
        return -1;

    if (psd->bins != fft_len || !psd->fft) {
        m2sdr_psd_free(psd);
        psd->fft   = m2sdr_fft_alloc(fft_len, false);
        psd->power = malloc(fft_len * sizeof(*psd->power));
        psd->win   = malloc(fft_len * sizeof(*psd->win));
        psd->work  = malloc(2 * fft_len * sizeof(*psd->work));
        if (!psd->fft || !psd->power || !psd->win || !psd->work) {
            m2sdr_psd_free(psd);
            return -1;
        }
        psd->bins     = fft_len;
        psd->win_kind = -1;
    }
    if (psd->win_kind != (int)window) {
        window_fill(window, psd->win, fft_len);
        psd->win_kind = (int)window;
    }
    for (i = 0; i < fft_len; i++) {
        sum  += psd->win[i];
        sum2 += (double)psd->win[i] * psd->win[i];
    }
    psd->sample_rate = sample_rate;
    psd->bin_hz      = sample_rate / (double)fft_len;
    psd->enbw_bins   = (double)fft_len * sum2 / (sum * sum);
    psd->rbw_hz      = psd->enbw_bins * psd->bin_hz;

    in  = psd->work;
    out = psd->work + fft_len;
    memset(psd->power, 0, fft_len * sizeof(*psd->power));
    for (start = 0; start + fft_len <= count; start += hop) {
        for (i = 0; i < fft_len; i++)
            in[i] = x[start + i] * psd->win[i];
        m2sdr_fft_run(psd->fft, in, out);
        /* Shifted: bin 0 is -sample_rate / 2. */
        for (i = 0; i < fft_len; i++) {
            const float complex v = out[(i + fft_len / 2) & (fft_len - 1)];
            psd->power[i] += crealf(v) * crealf(v) + cimagf(v) * cimagf(v);
        }
        segments++;
    }
    scale = 1.0 / (sum * sum * (double)segments);
    for (i = 0; i < fft_len; i++)
        psd->power[i] = (float)(psd->power[i] * scale);
    psd->segments = segments;
    return 0;
}

/* Frequency and level of the line whose highest bin is k. The top of a flat-top window is
 * flat by design: the bin gives the level, and the centre of the power of the lobe gives the
 * frequency. For the other windows, the peak of the parabola through the bin and its
 * neighbours, in dB, gives both. */
static void peak_refine(const struct m2sdr_psd *psd, size_t k, struct m2sdr_psd_peak *peak)
{
    double delta = 0, level = m2sdr_power_db(psd->power[k]);

    if (psd->win_kind == M2SDR_WINDOW_FLATTOP) {
        const size_t lobe = window_lobe_bins(psd->win_kind);
        const size_t a = k > lobe ? k - lobe : 0;
        const size_t b = k + lobe < psd->bins ? k + lobe : psd->bins - 1;
        double sum = 0, moment = 0;
        size_t i;

        for (i = a; i <= b; i++) {
            sum    += psd->power[i];
            moment += psd->power[i] * ((double)i - (double)k);
        }
        if (sum > 0)
            delta = moment / sum;
    } else if (k > 0 && k + 1 < psd->bins) {
        const double a = m2sdr_power_db(psd->power[k - 1]);
        const double c = m2sdr_power_db(psd->power[k + 1]);
        const double den = a - 2.0 * level + c;
        if (den < -1e-9) {
            delta = 0.5 * (a - c) / den;
            if (delta > 0.5)
                delta = 0.5;
            if (delta < -0.5)
                delta = -0.5;
            level -= 0.25 * (a - c) * delta;
        }
    }
    peak->freq_hz  = m2sdr_psd_freq(psd, k) + delta * psd->bin_hz;
    peak->power_db = level;
}

int m2sdr_psd_peak(const struct m2sdr_psd *psd, double lo_hz, double hi_hz, struct m2sdr_psd_peak *peak)
{
    size_t k0, k1, k, best;

    if (!peak || psd_range(psd, lo_hz, hi_hz, &k0, &k1) != 0)
        return -1;
    best = k0;
    for (k = k0; k <= k1; k++) {
        if (psd->power[k] > psd->power[best])
            best = k;
    }
    peak_refine(psd, best, peak);
    return 0;
}

static int cmp_peak(const void *a, const void *b)
{
    const double x = ((const struct m2sdr_psd_peak *)a)->power_db;
    const double y = ((const struct m2sdr_psd_peak *)b)->power_db;
    return (x < y) - (x > y);
}

int m2sdr_psd_peaks(const struct m2sdr_psd *psd, double lo_hz, double hi_hz, double excursion_db,
                    struct m2sdr_psd_peak *peaks, int max)
{
    const double ratio = pow(10.0, excursion_db / 10.0);
    struct m2sdr_psd_peak *all;
    size_t k0, k1, k, cand;
    double low, high;
    bool rising = false;
    int count = 0, cap = 64;

    if (!peaks || max <= 0 || psd_range(psd, lo_hz, hi_hz, &k0, &k1) != 0)
        return 0;
    all = malloc((size_t)cap * sizeof(*all));
    if (!all)
        return 0;

    /* A candidate is confirmed once the trace has fallen by the excursion after having risen by
     * it: the usual peak excursion of a spectrum analyser. */
    low  = psd->power[k0];
    high = psd->power[k0];
    cand = k0;
    for (k = k0; k <= k1; k++) {
        const double p = psd->power[k];
        if (rising) {
            if (p > high) {
                high = p;
                cand = k;
            } else if (p * ratio < high) {
                if (count == cap) {
                    struct m2sdr_psd_peak *grown = realloc(all, (size_t)cap * 2 * sizeof(*all));
                    if (!grown)
                        break;
                    all  = grown;
                    cap *= 2;
                }
                peak_refine(psd, cand, &all[count++]);
                rising = false;
                low    = p;
            }
        } else {
            if (p < low) {
                low = p;
            } else if (p > low * ratio) {
                rising = true;
                high   = p;
                cand   = k;
            }
        }
    }
    qsort(all, (size_t)count, sizeof(*all), cmp_peak);
    if (count > max)
        count = max;
    memcpy(peaks, all, (size_t)count * sizeof(*all));
    free(all);
    return count;
}

static double band_power(const struct m2sdr_psd *psd, double lo_hz, double hi_hz)
{
    size_t k0, k1, k;
    double sum = 0;

    if (psd_range(psd, lo_hz, hi_hz, &k0, &k1) != 0)
        return 0.0;
    for (k = k0; k <= k1; k++)
        sum += psd->power[k];
    return sum / psd->enbw_bins;
}

double m2sdr_psd_band_power_db(const struct m2sdr_psd *psd, double lo_hz, double hi_hz)
{
    return m2sdr_power_db(band_power(psd, lo_hz, hi_hz));
}

double m2sdr_psd_noise_density_db(const struct m2sdr_psd *psd, double lo_hz, double hi_hz)
{
    size_t k0, k1, n;
    float *sorted;
    double median, bias;

    if (psd_range(psd, lo_hz, hi_hz, &k0, &k1) != 0)
        return -200.0;
    n      = k1 - k0 + 1;
    sorted = malloc(n * sizeof(*sorted));
    if (!sorted)
        return -200.0;
    memcpy(sorted, psd->power + k0, n * sizeof(*sorted));
    qsort(sorted, n, sizeof(*sorted), cmp_float);
    median = (n & 1) ? sorted[n / 2] : 0.5 * ((double)sorted[n / 2 - 1] + sorted[n / 2]);
    free(sorted);
    /* The median of an average of K periodograms is below its mean: by ln(2) for K = 1, by
     * (1 - 1/(9K))^3 for more (Wilson-Hilferty). */
    bias = psd->segments <= 1 ? log(2.0) : pow(1.0 - 1.0 / (9.0 * psd->segments), 3.0);
    return m2sdr_power_db(median / bias / psd->rbw_hz);
}

int m2sdr_psd_obw(const struct m2sdr_psd *psd, double lo_hz, double hi_hz, double fraction,
                  double *obw_lo_hz, double *obw_hi_hz)
{
    size_t k0, k1, k;
    double total = 0, tail, acc;

    if (!obw_lo_hz || !obw_hi_hz || !(fraction > 0.0 && fraction < 1.0) ||
        psd_range(psd, lo_hz, hi_hz, &k0, &k1) != 0)
        return -1;
    for (k = k0; k <= k1; k++)
        total += psd->power[k];
    if (!(total > 0))
        return -1;
    tail = 0.5 * (1.0 - fraction) * total;

    /* Each bin holds its power between its edges; the bounds are interpolated inside a bin. */
    acc = 0;
    for (k = k0; k <= k1; k++) {
        if (acc + psd->power[k] >= tail)
            break;
        acc += psd->power[k];
    }
    *obw_lo_hz = m2sdr_psd_freq(psd, k) + ((tail - acc) / psd->power[k] - 0.5) * psd->bin_hz;
    acc = 0;
    for (k = k1 + 1; k-- > k0; ) {
        if (acc + psd->power[k] >= tail)
            break;
        acc += psd->power[k];
    }
    *obw_hi_hz = m2sdr_psd_freq(psd, k) - ((tail - acc) / psd->power[k] - 0.5) * psd->bin_hz;
    return 0;
}

int m2sdr_psd_acpr(const struct m2sdr_psd *psd, double center_hz, double bw_hz, double spacing_hz,
                   struct m2sdr_acpr *acpr)
{
    const double half_span = psd && psd->bins ? 0.5 * psd->sample_rate : 0.0;
    double main_power;
    int n;

    if (!acpr || !psd || !psd->power || !(bw_hz > 0) || !(spacing_hz > 0))
        return -1;
    memset(acpr, 0, sizeof(*acpr));
    if (center_hz - 0.5 * bw_hz < -half_span || center_hz + 0.5 * bw_hz > half_span)
        return -1;
    main_power       = band_power(psd, center_hz - 0.5 * bw_hz, center_hz + 0.5 * bw_hz);
    acpr->channel_db = m2sdr_power_db(main_power);
    for (n = 0; n < M2SDR_ACPR_MAX_ADJACENT; n++) {
        const double lo = center_hz - (n + 1) * spacing_hz;
        const double hi = center_hz + (n + 1) * spacing_hz;
        if (lo - 0.5 * bw_hz < -half_span || hi + 0.5 * bw_hz > half_span)
            break;
        acpr->lower_dbc[n] = m2sdr_power_db(band_power(psd, lo - 0.5 * bw_hz, lo + 0.5 * bw_hz)) - acpr->channel_db;
        acpr->upper_dbc[n] = m2sdr_power_db(band_power(psd, hi - 0.5 * bw_hz, hi + 0.5 * bw_hz)) - acpr->channel_db;
        acpr->adjacent = n + 1;
    }
    return 0;
}

/* Sum of the bins within `lobe` of bin k, removed from `left` so that they count once. */
static double take_line(float *left, size_t bins, size_t k, size_t lobe)
{
    const size_t a = k > lobe ? k - lobe : 0;
    const size_t b = k + lobe < bins ? k + lobe : bins - 1;
    double sum = 0;
    size_t i;

    for (i = a; i <= b; i++) {
        sum    += left[i];
        left[i] = 0;
    }
    return sum;
}

int m2sdr_psd_tone(const struct m2sdr_psd *psd, struct m2sdr_tone *tone)
{
    const size_t bins = psd ? psd->bins : 0;
    size_t lobe, dc, k, fund = 0, spur = 0, noise_bins = 0;
    double fund_power, harm_power = 0, rest = 0, noise;
    struct m2sdr_psd_peak peak;
    float *left;
    int h;

    if (!tone || !psd || !psd->power || bins < 64)
        return -1;
    memset(tone, 0, sizeof(*tone));
    lobe = window_lobe_bins(psd->win_kind);
    dc   = bins / 2;
    left = malloc(bins * sizeof(*left));
    if (!left)
        return -1;
    memcpy(left, psd->power, bins * sizeof(*left));
    (void)take_line(left, bins, dc, lobe);

    for (k = 0; k < bins; k++) {
        if (left[k] > left[fund])
            fund = k;
    }
    peak_refine(psd, fund, &peak);
    tone->freq_hz = peak.freq_hz;
    fund_power    = take_line(left, bins, fund, lobe) / psd->enbw_bins;
    tone->level_db = m2sdr_power_db(fund_power);

    /* Strongest other line, harmonics included, before they are removed. */
    for (k = 0; k < bins; k++) {
        if (left[k] > left[spur])
            spur = k;
    }
    tone->sfdr_dbc     = tone->level_db - m2sdr_power_db(psd->power[spur]);
    tone->spur_freq_hz = m2sdr_psd_freq(psd, spur);

    for (h = 0; h < M2SDR_TONE_HARMONICS; h++) {
        /* Complex samples: a harmonic above half the sample rate comes back from the other side. */
        double f = fmod((h + 2) * tone->freq_hz + 0.5 * psd->sample_rate, psd->sample_rate);
        double p;

        if (f < 0)
            f += psd->sample_rate;
        f -= 0.5 * psd->sample_rate;
        k  = (size_t)llround(f / psd->bin_hz + (double)dc);
        if (k >= bins)
            k = bins - 1;
        p = take_line(left, bins, k, lobe) / psd->enbw_bins;
        tone->harmonic_hz[h]  = f;
        tone->harmonic_dbc[h] = m2sdr_power_db(p) - tone->level_db;
        harm_power += p;
    }
    for (k = 0; k < bins; k++) {
        if (left[k] > 0) {
            rest += left[k];
            noise_bins++;
        }
    }
    free(left);
    /* The noise under the lines removed is filled in at the level of the rest. */
    noise = noise_bins ? rest / psd->enbw_bins * (double)bins / (double)noise_bins : 0.0;
    tone->snr_db      = tone->level_db - m2sdr_power_db(noise);
    tone->sinad_db    = tone->level_db - m2sdr_power_db(noise + harm_power);
    tone->thd_dbc     = m2sdr_power_db(harm_power) - tone->level_db;
    tone->noise_db_hz = m2sdr_power_db(noise / psd->sample_rate);
    return 0;
}

/* Time domain */
/*-------------*/

int m2sdr_iq_stats(const float complex *x, size_t count, double clip, struct m2sdr_iq_stats *stats)
{
    double si = 0, sq = 0, sii = 0, sqq = 0, siq = 0, peak = 0, mean;
    size_t hist[M2SDR_CCDF_POINTS] = { 0 };
    size_t i, clipped = 0, above;
    int k;

    if (!x || !stats || !count)
        return -1;
    memset(stats, 0, sizeof(*stats));
    for (i = 0; i < count; i++) {
        const double a = crealf(x[i]), b = cimagf(x[i]);
        const double p = a * a + b * b;

        si  += a;
        sq  += b;
        sii += a * a;
        sqq += b * b;
        siq += a * b;
        if (p > peak)
            peak = p;
        if (fabs(a) >= clip || fabs(b) >= clip)
            clipped++;
    }
    mean = (sii + sqq) / (double)count;
    stats->dc_i    = si / (double)count;
    stats->dc_q    = sq / (double)count;
    stats->dc_db   = m2sdr_power_db(stats->dc_i * stats->dc_i + stats->dc_q * stats->dc_q);
    stats->mean_db = m2sdr_power_db(mean);
    stats->peak_db = m2sdr_power_db(peak);
    stats->papr_db = stats->peak_db - stats->mean_db;
    stats->clipped = (double)clipped / (double)count;
    {
        /* Rails without their mean. */
        const double vii = sii / count - stats->dc_i * stats->dc_i;
        const double vqq = sqq / count - stats->dc_q * stats->dc_q;
        const double viq = siq / count - stats->dc_i * stats->dc_q;

        if (vii > 0 && vqq > 0) {
            double s = viq / sqrt(vii * vqq);
            if (s > 1.0)
                s = 1.0;
            if (s < -1.0)
                s = -1.0;
            stats->gain_imbalance_db = 10.0 * log10(vii / vqq);
            stats->quadrature_deg    = asin(s) * 180.0 / M_PI;
        }
    }
    if (mean > 0) {
        for (i = 0; i < count; i++) {
            const double a = crealf(x[i]), b = cimagf(x[i]);
            const double db = m2sdr_power_db((a * a + b * b) / mean);
            if (db >= 0) {
                k = (int)(db / M2SDR_CCDF_STEP_DB);
                hist[k < M2SDR_CCDF_POINTS ? k : M2SDR_CCDF_POINTS - 1]++;
            }
        }
        above = 0;
        for (k = M2SDR_CCDF_POINTS - 1; k >= 0; k--) {
            above += hist[k];
            stats->ccdf[k] = (double)above / (double)count;
        }
    }
    return 0;
}
