/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: phase noise of a carrier.
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

#define PN_SEARCH_FFT       65536      /* Longest transform of the carrier search. */
#define PN_MIN_SAMPLES      4096
#define PN_DC_BINS          8.0        /* Bins of the search left out on each side of the centre. */
#define PN_MIN_CARRIER_DB   10.0       /* Carrier over the rest of the capture. */
#define PN_STAGES           6          /* Transform lengths, each PN_STAGE_RATIO shorter. */
#define PN_STAGE_RATIO      8
#define PN_MIN_FFT          256
#define PN_MIN_BIN          4.0        /* Lowest bin used: the window spreads the carrier below. */
#define PN_BINS_PER_POINT   8.0        /* A shorter transform is used once it has this many bins in a point. */
#define PN_SPUR_DB          10.0       /* A line stands this far above the noise around it, */
#define PN_SPUR_SINGLE_DB   17.0       /* or this far in a spectrum that is not averaged. */
#define PN_SPUR_WINDOW      32         /* Bins on each side that give this noise. */

/* One length of transform: densities of the phase and of the amplitude, per Hz, both sidebands
 * folded on the positive offsets, and the same normalised as lines for the spurs. */
struct pn_stage {
    size_t len;
    double bin_hz;
    double enbw_bins;
    float *phase;                      /* len / 2 bins: density, rad^2/Hz, two-sided. */
    float *am;
    unsigned segments;
};

static void stage_free(struct pn_stage *st)
{
    free(st->phase);
    free(st->am);
    memset(st, 0, sizeof(*st));
}

/* Welch density of two real signals, the phase and the amplitude, with one complex transform:
 * the phase goes on the real part and the amplitude on the imaginary one, and their spectra
 * come apart from the two halves of the result. */
static int stage_compute(struct pn_stage *st, const float *phase, const float *am, size_t count,
                         size_t len, double sample_rate)
{
    struct m2sdr_fft *fft = m2sdr_fft_alloc(len, false);
    float complex *in = malloc(len * sizeof(*in));
    float complex *out = malloc(len * sizeof(*out));
    float *win = malloc(len * sizeof(*win));
    const size_t hop = len / 2;
    double sum2 = 0, sum = 0, scale;
    size_t i, k, start;
    int rc = -1;

    memset(st, 0, sizeof(*st));
    st->phase = calloc(len / 2, sizeof(*st->phase));
    st->am    = calloc(len / 2, sizeof(*st->am));
    if (!fft || !in || !out || !win || !st->phase || !st->am)
        goto out;
    for (i = 0; i < len; i++) {
        win[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)len));
        sum   += win[i];
        sum2  += (double)win[i] * win[i];
    }
    for (start = 0; start + len <= count; start += hop) {
        double mp = 0, ma = 0;

        /* The mean of each segment is removed: it is the carrier, not its noise. */
        for (i = 0; i < len; i++) {
            mp += phase[start + i];
            ma += am[start + i];
        }
        mp /= (double)len;
        ma /= (double)len;
        for (i = 0; i < len; i++)
            in[i] = ((phase[start + i] - (float)mp) + I * (am[start + i] - (float)ma)) * win[i];
        m2sdr_fft_run(fft, in, out);
        for (k = 1; k < len / 2; k++) {
            /* For a real p and a real a, X = P + jA with P and A Hermitian:
             * P(k) = (X(k) + conj(X(-k))) / 2, A(k) = (X(k) - conj(X(-k))) / (2j). */
            const float complex xp = out[k], xn = conjf(out[len - k]);
            const float complex p = 0.5f * (xp + xn), a = -0.5f * I * (xp - xn);
            st->phase[k] += crealf(p) * crealf(p) + cimagf(p) * cimagf(p);
            st->am[k]    += crealf(a) * crealf(a) + cimagf(a) * cimagf(a);
        }
        st->segments++;
    }
    if (!st->segments)
        goto out;
    /* Two-sided density: |X|^2 / (fs * sum(w^2)). */
    scale = 1.0 / (sample_rate * sum2 * (double)st->segments);
    for (k = 0; k < len / 2; k++) {
        st->phase[k] = (float)(st->phase[k] * scale);
        st->am[k]    = (float)(st->am[k] * scale);
    }
    st->len       = len;
    st->bin_hz    = sample_rate / (double)len;
    st->enbw_bins = (double)len * sum2 / (sum * sum);
    rc = 0;

out:
    m2sdr_fft_free(fft);
    free(in);
    free(out);
    free(win);
    if (rc != 0)
        stage_free(st);
    return rc;
}

static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

struct pn_spur {
    double hz;
    double power;
};

static int cmp_spur(const void *a, const void *b)
{
    const double x = ((const struct pn_spur *)a)->power, y = ((const struct pn_spur *)b)->power;
    return (x < y) - (x > y);
}

/* Lines of the phase in a stage: bins that stand above the median of their neighbourhood. A
 * line of a sinusoidal phase of peak b reads (b/2)^2, its level as a sideband. The stage must
 * be averaged: the bins of a single transform of noise vary too much to tell a line. */
static void find_spurs(const struct pn_stage *st, struct m2sdr_phase_noise *pn)
{
    const size_t n = st->len / 2;
    const double ratio = pow(10.0, (st->segments >= 8 ? PN_SPUR_DB : PN_SPUR_SINGLE_DB) / 10.0);
    /* The median of an averaged periodogram is below its mean. */
    const double bias = st->segments <= 1 ? log(2.0) : pow(1.0 - 1.0 / (9.0 * st->segments), 3.0);
    struct pn_spur *found = malloc(n * sizeof(struct pn_spur));
    float local[2 * PN_SPUR_WINDOW + 1];
    size_t k, count = 0;

    pn->nspurs = 0;
    if (!found)
        return;
    for (k = (size_t)PN_MIN_BIN; k + 1 < n; k++) {
        const size_t a = k > PN_SPUR_WINDOW ? k - PN_SPUR_WINDOW : 1;
        const size_t b = k + PN_SPUR_WINDOW < n ? k + PN_SPUR_WINDOW : n - 1;
        const size_t m = b - a + 1;
        double median;

        if (st->phase[k] < st->phase[k - 1] || st->phase[k] < st->phase[k + 1])
            continue;
        memcpy(local, st->phase + a, m * sizeof(*local));
        qsort(local, m, sizeof(*local), cmp_float);
        median = local[m / 2] / bias;
        if (st->phase[k] > ratio * median && k >= 2 && k + 2 < n) {
            /* Power of the line: the densities of the bins of its lobe, less the noise under
             * them, times the bin width. */
            const double line = ((double)st->phase[k - 2] + st->phase[k - 1] + st->phase[k] + st->phase[k + 1] +
                                 st->phase[k + 2] - 5.0 * median) * st->bin_hz;
            /* Its frequency: the centre of the power of the lobe. */
            const double moment = 2.0 * ((double)st->phase[k + 2] - st->phase[k - 2]) +
                                  ((double)st->phase[k + 1] - st->phase[k - 1]);
            const double sum = (double)st->phase[k - 2] + st->phase[k - 1] + st->phase[k] + st->phase[k + 1] + st->phase[k + 2];

            if (!(line > 0))
                continue;
            found[count].hz    = ((double)k + moment / sum) * st->bin_hz;
            found[count].power = line;
            count++;
        }
    }
    qsort(found, count, sizeof(*found), cmp_spur);
    for (k = 0; k < count && k < M2SDR_PN_MAX_SPURS; k++) {
        pn->spur_hz[k]  = found[k].hz;
        pn->spur_dbc[k] = m2sdr_power_db(found[k].power);
        pn->nspurs++;
    }
    free(found);
}

int m2sdr_phase_noise(const float complex *x, size_t count, double sample_rate, double carrier_freq_hz,
                      double lo_hz, double hi_hz, struct m2sdr_phase_noise *pn)
{
    struct pn_stage stages[PN_STAGES];
    struct m2sdr_psd psd;
    struct m2sdr_psd_peak peak;
    float *phase = NULL, *am = NULL;
    double total_db, mean_amp = 0, prev, turns, f, step;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, slope, intercept;
    double int_phase = 0, int_fm = 0;
    size_t search_len = PN_SEARCH_FFT, len, i;
    int nstages = 0, s, rc = -1;

    if (!x || !pn || count < PN_MIN_SAMPLES || !(sample_rate > 0))
        return -1;
    memset(pn, 0, sizeof(*pn));
    memset(stages, 0, sizeof(stages));
    memset(&psd, 0, sizeof(psd));

    /* Carrier: the strongest line, and what it weighs in the capture. */
    while (search_len > count)
        search_len >>= 1;
    if (m2sdr_psd_compute(x, count, sample_rate, search_len, M2SDR_WINDOW_BLACKMAN_HARRIS, &psd) != 0)
        return -1;
    /* The line at the centre is the DC offset and the LO leakage of the receiver, not a
     * carrier: it is left out of the search and of the rest. */
    {
        const double dc = PN_DC_BINS * psd.bin_hz, half = 0.5 * sample_rate;
        struct m2sdr_psd_peak below, above;
        const bool has_below = m2sdr_psd_peak(&psd, -half, -dc, &below) == 0;
        const bool has_above = m2sdr_psd_peak(&psd, dc, half, &above) == 0;

        if (!has_below && !has_above) {
            m2sdr_psd_free(&psd);
            return -1;
        }
        peak = (has_above && (!has_below || above.power_db >= below.power_db)) ? above : below;
        total_db = m2sdr_power_db(pow(10.0, m2sdr_psd_band_power_db(&psd, -half, -dc) / 10.0) +
                                  pow(10.0, m2sdr_psd_band_power_db(&psd, dc, half) / 10.0));
    }
    pn->carrier_hz = peak.freq_hz;
    pn->carrier_db = m2sdr_psd_band_power_db(&psd, peak.freq_hz - 6.0 * psd.bin_hz, peak.freq_hz + 6.0 * psd.bin_hz);
    m2sdr_psd_free(&psd);
    /* The phase can only be followed when the carrier is well above all the rest: the rest is
     * total - carrier. */
    {
        const double rest = pow(10.0, total_db / 10.0) - pow(10.0, pn->carrier_db / 10.0);
        if (rest > 0 && pn->carrier_db - m2sdr_power_db(rest) < PN_MIN_CARRIER_DB)
            return 0;
    }

    phase = malloc(count * sizeof(*phase));
    am    = malloc(count * sizeof(*am));
    if (!phase || !am)
        goto out;

    /* Carrier to zero frequency, then its phase, unwrapped. The frequency of the search is
     * within a fraction of a bin: what is left is a slope of the phase, removed below. */
    {
        const float complex rot = (float complex)cexp(-2.0 * M_PI * I * pn->carrier_hz / sample_rate);
        float complex osc = 1.0f;

        prev  = 0;
        turns = 0;
        for (i = 0; i < count; i++) {
            const float complex y = x[i] * osc;
            const double ph = atan2(cimagf(y), crealf(y));

            if (i) {
                if (ph - prev > M_PI)
                    turns -= 2.0 * M_PI;
                else if (ph - prev < -M_PI)
                    turns += 2.0 * M_PI;
            }
            prev     = ph;
            phase[i] = (float)(ph + turns);
            am[i]    = cabsf(y);
            mean_amp += am[i];
            osc *= rot;
            if ((i & 1023) == 1023)
                osc /= cabsf(osc);
        }
        mean_amp /= (double)count;
    }
    if (!(mean_amp > 0))
        goto out;

    /* Line through the phase: its slope is the rest of the frequency offset. */
    for (i = 0; i < count; i++) {
        const double t = (double)i - 0.5 * (double)(count - 1);
        sx  += t;
        sy  += phase[i];
        sxx += t * t;
        sxy += t * phase[i];
    }
    slope     = sxy / sxx;
    intercept = sy / (double)count;
    pn->carrier_hz += slope * sample_rate / (2.0 * M_PI);
    for (i = 0; i < count; i++) {
        const double t = (double)i - 0.5 * (double)(count - 1);
        phase[i] = (float)(phase[i] - intercept - slope * t);
        am[i]    = (float)(am[i] / mean_amp - 1.0);
    }

    /* Stages, the longest first. */
    for (len = 1; len * 2 <= count; len <<= 1)
        ;
    for (s = 0; s < PN_STAGES && len >= PN_MIN_FFT; s++, len /= PN_STAGE_RATIO) {
        if (stage_compute(&stages[s], phase, am, count, len, sample_rate) != 0)
            goto out;
        nstages++;
    }
    if (!nstages)
        goto out;

    /* Points: for each, the shortest stage that still resolves it, since it is the one with
     * the most averages, and the mean of its bins over the width of the point. */
    step = pow(10.0, 1.0 / M2SDR_PN_POINTS_PER_DECADE);
    for (f = PN_MIN_BIN * stages[0].bin_hz * sqrt(step); f * sqrt(step) < 0.5 * sample_rate && pn->points < M2SDR_PN_MAX_POINTS; f *= step) {
        const double f0 = f / sqrt(step), f1 = f * sqrt(step);
        const struct pn_stage *st = &stages[0];
        double sp = 0, sa = 0;
        size_t k0, k1, k;

        for (s = nstages - 1; s > 0; s--) {
            if ((f1 - f0) >= PN_BINS_PER_POINT * stages[s].bin_hz && f0 >= PN_MIN_BIN * stages[s].bin_hz) {
                st = &stages[s];
                break;
            }
        }
        k0 = (size_t)ceil(f0 / st->bin_hz);
        k1 = (size_t)floor(f1 / st->bin_hz);
        if (k1 < k0)
            k0 = k1 = (size_t)llround(f / st->bin_hz);
        if (k1 >= st->len / 2)
            k1 = st->len / 2 - 1;
        if (k0 < 1 || k0 > k1)
            continue;
        for (k = k0; k <= k1; k++) {
            sp += st->phase[k];
            sa += st->am[k];
        }
        sp /= (double)(k1 - k0 + 1);
        sa /= (double)(k1 - k0 + 1);
        /* L(f) is half of the one-sided density of the phase: the two-sided one. */
        pn->offset_hz[pn->points] = f;
        pn->l_dbc_hz[pn->points]  = m2sdr_power_db(sp);
        pn->am_dbc_hz[pn->points] = m2sdr_power_db(sa);
        pn->points++;
    }
    if (pn->points < 2)
        goto out;

    /* Integrals over the points, on their widths. */
    pn->lo_hz = lo_hz > 0 ? lo_hz : pn->offset_hz[0] / sqrt(step);
    pn->hi_hz = hi_hz > 0 ? hi_hz : pn->offset_hz[pn->points - 1] * sqrt(step);
    for (s = 0; s < pn->points; s++) {
        double f0 = pn->offset_hz[s] / sqrt(step), f1 = pn->offset_hz[s] * sqrt(step);
        const double l = pow(10.0, pn->l_dbc_hz[s] / 10.0);

        if (f0 < pn->lo_hz)
            f0 = pn->lo_hz;
        if (f1 > pn->hi_hz)
            f1 = pn->hi_hz;
        if (f1 <= f0)
            continue;
        int_phase += l * (f1 - f0);
        int_fm    += l * (f1 * f1 * f1 - f0 * f0 * f0) / 3.0;
    }
    pn->rms_rad        = sqrt(2.0 * int_phase);
    pn->rms_deg        = pn->rms_rad * 180.0 / M_PI;
    pn->residual_fm_hz = sqrt(2.0 * int_fm);
    if (carrier_freq_hz > 0)
        pn->jitter_s = pn->rms_rad / (2.0 * M_PI * carrier_freq_hz);

    find_spurs(&stages[nstages > 1 ? 1 : 0], pn);
    pn->found = true;
    rc = 0;

out:
    for (s = 0; s < PN_STAGES; s++)
        stage_free(&stages[s]);
    free(phase);
    free(am);
    return pn->found || rc == 0 ? 0 : -1;
}
