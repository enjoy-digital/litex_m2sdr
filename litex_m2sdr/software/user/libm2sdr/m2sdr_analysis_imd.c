/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis, lab instrument: two-tone intermodulation (IM3, IM5, intercepts).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_lab.h"
#include "m2sdr_analysis_lab_internal.h"

#define LOBE          5       /* Bins of the main lobe of the Blackman-Harris window, on each side. */
#define MIN_SPACING   32      /* Bins between the tones, at least. */
#define AUTO_SPACING  64      /* Bins between the tones of the transform chosen. */
#define MIN_FFT       1024
#define MAX_FFT       65536
#define TONE_SEARCH   3       /* Bins searched around a tone. */
#define MIN_TONE_DB   10.0    /* Of a tone over the noise of its band. */
#define MIN_LINE_DB   6.0     /* Of a product or a spur over the noise of its band. */
#define MAX_ORDER     9       /* Odd products left out of the spurs, up to this order. */
#define MAX_PEAKS     64

struct line {
    bool   in_span;
    bool   measured;
    double freq_hz;
    double dbfs;
};

/* Line at a known frequency: the peak within `search` bins, its lobe less the noise. Below the
 * threshold, the level is the noise of its band: a bound. */
static void measure_line(const struct m2sdr_psd *psd, double freq_hz, size_t search, double noise_bin,
                         double min_db, struct line *l)
{
    const double half = 0.5 * psd->sample_rate - (LOBE + 1) * psd->bin_hz;
    double floor_power, power;
    size_t k, k0, k1, i;

    memset(l, 0, sizeof(*l));
    l->freq_hz = freq_hz;
    if (fabs(freq_hz) > half) {
        l->dbfs = -200.0;
        return;
    }
    l->in_span = true;
    k  = m2sdr_lab_psd_bin(psd, freq_hz);
    k0 = k > search ? k - search : 0;
    k1 = k + search < psd->bins ? k + search : psd->bins - 1;
    for (i = k0; i <= k1; i++) {
        if (psd->power[i] > psd->power[k])
            k = i;
    }
    power = m2sdr_lab_line_power(psd, k, LOBE, noise_bin, &floor_power);
    l->measured = power > floor_power * pow(10.0, min_db / 10.0);
    l->dbfs     = m2sdr_power_db(l->measured ? power : floor_power);
    if (l->measured) {
        /* Centre of the power of the lobe: the frequency between the bins. */
        double sum = 0, moment = 0;
        for (i = k - (k >= 2 ? 2 : k); i <= k + 2 && i < psd->bins; i++) {
            sum    += psd->power[i];
            moment += psd->power[i] * ((double)i - (double)k);
        }
        l->freq_hz = m2sdr_psd_freq(psd, k) + (sum > 0 ? moment / sum : 0) * psd->bin_hz;
    }
}

static double stronger(const struct line *a, const struct line *b)
{
    if (a->in_span && b->in_span)
        return fmax(a->dbfs, b->dbfs);
    return a->in_span ? a->dbfs : b->dbfs;
}

int m2sdr_imd_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_imd_config *cfg,
                      struct m2sdr_imd_result *res)
{
    struct m2sdr_psd psd;
    struct m2sdr_psd_peak peaks[MAX_PEAKS];
    struct line t1, t2, im3l, im3h, im5l, im5h;
    double spacing, noise, tone_db, excursion;
    size_t fft_len;
    int n, i, rc = -1;

    if (!x || !cfg || !res || !(sample_rate > 0) || !(cfg->f2_hz > cfg->f1_hz) ||
        !(cfg->f1_hz > -0.5 * sample_rate) || !(cfg->f2_hz < 0.5 * sample_rate))
        return -1;
    spacing = cfg->f2_hz - cfg->f1_hz;
    if (cfg->fft_len) {
        fft_len = cfg->fft_len;
    } else {
        fft_len = MIN_FFT;
        while (fft_len < MAX_FFT && spacing < AUTO_SPACING * sample_rate / (double)fft_len)
            fft_len *= 2;
        while (fft_len > MIN_FFT && fft_len > count)
            fft_len /= 2;
    }
    if (!m2sdr_lab_pow2(fft_len) || fft_len > count || spacing < MIN_SPACING * sample_rate / (double)fft_len)
        return -1;
    memset(res, 0, sizeof(*res));
    res->input_known = cfg->input_known;

    memset(&psd, 0, sizeof(psd));
    if (m2sdr_psd_compute(x, count, sample_rate, fft_len, M2SDR_WINDOW_BLACKMAN_HARRIS, &psd) != 0)
        goto out;
    noise = m2sdr_lab_noise_bin(&psd);
    res->floor_dbfs = m2sdr_power_db(noise * (2 * LOBE + 1) / psd.enbw_bins);

    /* Tones, then the products from their measured frequencies. */
    measure_line(&psd, cfg->f1_hz, TONE_SEARCH, noise, MIN_TONE_DB, &t1);
    measure_line(&psd, cfg->f2_hz, TONE_SEARCH, noise, MIN_TONE_DB, &t2);
    rc = 0;
    if (!t1.measured || !t2.measured)
        goto out;
    res->found      = true;
    res->f1_hz      = t1.freq_hz;
    res->f2_hz      = t2.freq_hz;
    res->tone1_dbfs = t1.dbfs;
    res->tone2_dbfs = t2.dbfs;
    measure_line(&psd, 2.0 * t1.freq_hz - t2.freq_hz, 1, noise, MIN_LINE_DB, &im3l);
    measure_line(&psd, 2.0 * t2.freq_hz - t1.freq_hz, 1, noise, MIN_LINE_DB, &im3h);
    measure_line(&psd, 3.0 * t1.freq_hz - 2.0 * t2.freq_hz, 1, noise, MIN_LINE_DB, &im5l);
    measure_line(&psd, 3.0 * t2.freq_hz - 2.0 * t1.freq_hz, 1, noise, MIN_LINE_DB, &im5h);
    res->im3_low_dbfs  = im3l.dbfs;
    res->im3_high_dbfs = im3h.dbfs;
    res->im5_low_dbfs  = im5l.dbfs;
    res->im5_high_dbfs = im5h.dbfs;
    /* Measured when the stronger product of the order is (the weaker may be under the noise). */
    res->im3_measured = (im3l.in_span && im3l.measured) || (im3h.in_span && im3h.measured);
    res->im5_measured = (im5l.in_span && im5l.measured) || (im5h.in_span && im5h.measured);

    /* Intercepts: the products of order N rise N dB per dB of the tones, the tones by one. */
    tone_db        = 0.5 * (t1.dbfs + t2.dbfs);
    res->im3_dbc   = stronger(&im3l, &im3h) - tone_db;
    res->im5_dbc   = stronger(&im5l, &im5h) - tone_db;
    res->oip3_dbfs = tone_db - res->im3_dbc / 2.0;
    res->oip5_dbfs = tone_db - res->im5_dbc / 4.0;
    if (cfg->input_known) {
        res->gain_db   = tone_db - cfg->input_dbfs;
        res->iip3_dbfs = res->oip3_dbfs - res->gain_db;
        res->iip5_dbfs = res->oip5_dbfs - res->gain_db;
    }

    /* Spurs: the other lines, without DC (the LO leakage of the receiver) and the odd products.
     * A line within two lobes of one of those is not told from its skirt (and the sidelobes of the
     * window stand out of their nulls): it is left out too. The peak bin has to stand out of the
     * noise of a bin, its lobe gives the level. */
    excursion = cfg->spur_excursion_db > 0 ? cfg->spur_excursion_db : 10.0;
    n = m2sdr_psd_peaks(&psd, -0.5 * sample_rate, 0.5 * sample_rate, excursion, peaks, MAX_PEAKS);
    for (i = 0; i < n && res->nspurs < M2SDR_IMD_MAX_SPURS; i++) {
        const double guard = (2 * LOBE + 1) * psd.bin_hz, f = peaks[i].freq_hz;
        struct line l;
        bool known = fabs(f) <= guard;
        int order;
        for (order = 1; order <= MAX_ORDER && !known; order += 2) {
            const int a = (order + 1) / 2, b = (order - 1) / 2;
            known = fabs(f - (a * t1.freq_hz - b * t2.freq_hz)) <= guard ||
                    fabs(f - (a * t2.freq_hz - b * t1.freq_hz)) <= guard;
        }
        if (known || pow(10.0, peaks[i].power_db / 10.0) < noise * pow(10.0, MIN_LINE_DB / 10.0))
            continue;
        measure_line(&psd, f, 0, noise, MIN_LINE_DB, &l);
        if (!l.measured)
            continue;
        res->spur_hz[res->nspurs]   = l.freq_hz;
        res->spur_dbfs[res->nspurs] = l.dbfs;
        res->nspurs++;
    }
out:
    m2sdr_psd_free(&psd);
    return rc;
}
