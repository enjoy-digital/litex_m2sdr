/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: GPS L1 C/A acquisition (IS-GPS-200) and jamming indicator.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_timing.h"
#include "m2sdr_fft.h"

/* The samples and the products here are finite: complex products without the NaN and infinity
 * handling of C99 (__mulsc3). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("cx-limited-range")
#endif

#define MAX_MS            200
#define SEARCH_RATE       2.048e6 /* Of the code phase search. */
#define SEARCH_N          2048    /* Its millisecond. */
#define JAM_FFT           1024
#define JAM_LINE_DB       10.0    /* Narrowband line: over the floor by this much. */
#define JAM_JN_DB         3.0
#define JAM_LINE_BINS     3       /* Bins each side of a line summed into its power. */

/* C/A code */
/*----------*/

/* G2 taps of each PRN, stages 1 to 10 (IS-GPS-200, table 3-Ia). */
static const uint8_t ca_taps[M2SDR_GNSS_PRNS][2] = {
    { 2, 6 }, { 3, 7 }, { 4, 8 }, { 5, 9 }, { 1, 9 }, { 2, 10 }, { 1, 8 }, { 2, 9 },
    { 3, 10 }, { 2, 3 }, { 3, 4 }, { 5, 6 }, { 6, 7 }, { 7, 8 }, { 8, 9 }, { 9, 10 },
    { 1, 4 }, { 2, 5 }, { 3, 6 }, { 4, 7 }, { 5, 8 }, { 6, 9 }, { 1, 3 }, { 4, 6 },
    { 5, 7 }, { 6, 8 }, { 7, 9 }, { 8, 10 }, { 1, 6 }, { 2, 7 }, { 3, 8 }, { 4, 9 },
};

/* G1 = 1 + x^3 + x^10, G2 = 1 + x^2 + x^3 + x^6 + x^8 + x^9 + x^10, both starting all ones;
 * g[i] is stage i + 1, the output of G1 is stage 10. */
int m2sdr_gnss_ca_code(int prn, uint8_t chips[M2SDR_GNSS_CA_CHIPS])
{
    uint8_t g1[10], g2[10];
    int i, k;

    if (prn < 1 || prn > M2SDR_GNSS_PRNS || !chips)
        return -1;
    memset(g1, 1, sizeof(g1));
    memset(g2, 1, sizeof(g2));
    for (i = 0; i < M2SDR_GNSS_CA_CHIPS; i++) {
        const uint8_t f1 = g1[2] ^ g1[9];
        const uint8_t f2 = g2[1] ^ g2[2] ^ g2[5] ^ g2[7] ^ g2[8] ^ g2[9];

        chips[i] = g1[9] ^ g2[ca_taps[prn - 1][0] - 1] ^ g2[ca_taps[prn - 1][1] - 1];
        for (k = 9; k > 0; k--) {
            g1[k] = g1[k - 1];
            g2[k] = g2[k - 1];
        }
        g1[0] = f1;
        g2[0] = f2;
    }
    return 0;
}

/* Jamming */
/*---------*/

int m2sdr_gnss_jamming(const float complex *x, size_t count, double sample_rate, struct m2sdr_gnss_jamming *jam)
{
    struct m2sdr_psd psd;
    struct m2sdr_psd_peak peaks[M2SDR_GNSS_MAX_JAMMERS];
    const size_t nfft = JAM_FFT;
    double half, n0, floor_bin, lsum = 0.0, psum = 0.0, power = 0.0, band_power, noise_power, ratio;
    size_t k, k0, k1, nb = 0;
    int np, i;

    if (!x || !jam || count < 4096 || !(sample_rate > 0.0))
        return -1;
    memset(jam, 0, sizeof(*jam));
    memset(&psd, 0, sizeof(psd));
    if (m2sdr_psd_compute(x, count, sample_rate, nfft, M2SDR_WINDOW_HANN, &psd) != 0) {
        m2sdr_psd_free(&psd);
        return -1;
    }
    for (k = 0; k < count; k++)
        power += (double)(crealf(x[k]) * crealf(x[k]) + cimagf(x[k]) * cimagf(x[k]));
    jam->power_dbfs = m2sdr_power_db(power / (double)count);

    /* Band: 80 % of the sample rate, the roll-off of the receiver filters left out. */
    half          = 0.4 * sample_rate;
    jam->band_hz  = 2.0 * half;
    jam->noise_dbfs_hz = m2sdr_psd_noise_density_db(&psd, -half, half);
    n0            = pow(10.0, jam->noise_dbfs_hz / 10.0);
    floor_bin     = n0 * psd.rbw_hz;
    k0            = (size_t)ceil(-half / psd.bin_hz) + nfft / 2;
    k1            = (size_t)floor(half / psd.bin_hz) + nfft / 2;
    for (k = k0; k <= k1; k++) {
        const double p = psd.power[k] > 1e-30f ? psd.power[k] : 1e-30;

        if (k + 1 >= nfft / 2 && k <= nfft / 2 + 1)
            continue;   /* DC. */
        lsum += log(p);
        psum += p;
        nb++;
    }
    jam->flatness_db = 10.0 * log10(exp(lsum / (double)nb) / (psum / (double)nb));
    /* Bins sum to the power times the noise bandwidth of the window. */
    band_power  = psum / psd.enbw_bins;
    noise_power = floor_bin * (double)nb / psd.enbw_bins;
    ratio       = (band_power - noise_power) / noise_power;
    jam->jn_db  = 10.0 * log10(ratio > 1e-3 ? ratio : 1e-3);

    /* Narrowband lines, DC left out. */
    np = m2sdr_psd_peaks(&psd, -half, half, JAM_LINE_DB, peaks, M2SDR_GNSS_MAX_JAMMERS);
    for (i = 0; i < np; i++) {
        const long kc = lround(peaks[i].freq_hz / psd.bin_hz) + (long)(nfft / 2);
        double line = 0.0;
        long j;

        if (labs(kc - (long)(nfft / 2)) <= 1)
            continue;
        if (pow(10.0, peaks[i].power_db / 10.0) < floor_bin * pow(10.0, JAM_LINE_DB / 10.0))
            continue;
        for (j = kc - JAM_LINE_BINS; j <= kc + JAM_LINE_BINS; j++)
            if (j >= 0 && j < (long)nfft)
                line += psd.power[j] - floor_bin;
        line /= psd.enbw_bins;
        if (line <= 0.0)
            continue;
        jam->jammer_hz[jam->njammers]    = peaks[i].freq_hz;
        jam->jammer_dbfs[jam->njammers]  = m2sdr_power_db(line);
        jam->jammer_jn_db[jam->njammers] = 10.0 * log10(line / noise_power);
        jam->njammers++;
    }
    jam->jammed = jam->njammers > 0 || jam->jn_db >= JAM_JN_DB;

    /* Spectrum trace over the whole capture, bins grouped. */
    {
        const size_t group = nfft / M2SDR_GNSS_SPECTRUM;

        jam->spectrum_points   = M2SDR_GNSS_SPECTRUM;
        jam->spectrum_step_hz  = psd.bin_hz * (double)group;
        jam->spectrum_start_hz = m2sdr_psd_freq(&psd, 0) + 0.5 * (double)(group - 1) * psd.bin_hz;
        for (i = 0; i < M2SDR_GNSS_SPECTRUM; i++) {
            double s = 0.0;

            for (k = 0; k < group; k++)
                s += psd.power[(size_t)i * group + k];
            jam->spectrum_db[i] = (float)(10.0 * log10(s / (double)group / floor_bin + 1e-30));
        }
    }
    m2sdr_psd_free(&psd);
    return 0;
}

/* Acquisition */
/*-------------*/

/* Upper tail of Gamma(k, 1), k whole: exp(-s) sum_{i<k} s^i / i!. */
static double gamma_tail(int k, double s)
{
    double term = 1.0, sum = 1.0;
    int i;

    for (i = 1; i < k; i++) {
        term *= s / i;
        sum  += term;
    }
    return exp(-s + log(sum));
}

/* Threshold of a sum of k exponential cells of mean 1 for a false alarm probability p. */
static double gamma_threshold(int k, double p)
{
    double lo = 0.0, hi = 10.0 * k + 100.0;
    int it;

    for (it = 0; it < 100; it++) {
        const double mid = 0.5 * (lo + hi);

        if (gamma_tail(k, mid) > p)
            lo = mid;
        else
            hi = mid;
    }
    return hi;
}

/* Code of a PRN sampled at sample_rate from its epoch at tau samples, with a chip rate rate:
 * +1 for chip 0, -1 for chip 1. */
static void sample_code(const uint8_t *chips, double sample_rate, double rate, double tau, size_t n0, size_t n,
                        float *out)
{
    size_t i;

    for (i = 0; i < n; i++) {
        const double ph = ((double)(n0 + i) - tau) * rate / sample_rate;
        long c = (long)floor(ph) % M2SDR_GNSS_CA_CHIPS;

        if (c < 0)
            c += M2SDR_GNSS_CA_CHIPS;
        out[i] = chips[c] ? -1.0f : 1.0f;
    }
}

/* Prompt correlations of `ms` milliseconds at a Doppler and a code delay (samples). */
static void prompt(const float complex *x, size_t n, int ms, double sample_rate, const uint8_t *chips,
                   double doppler_hz, double tau, float *code, double complex *p)
{
    const double rate = M2SDR_GNSS_CA_RATE * (1.0 + doppler_hz / M2SDR_GNSS_L1_HZ);
    const double w = -2.0 * M_PI * doppler_hz / sample_rate;
    const double complex step = cexp(I * w);
    int j;

    for (j = 0; j < ms; j++) {
        double complex rot = cexp(I * w * (double)((size_t)j * n));
        double complex s = 0.0;
        size_t i;

        sample_code(chips, sample_rate, rate, tau, (size_t)j * n, n, code);
        for (i = 0; i < n; i++) {
            s   += (double complex)x[(size_t)j * n + i] * rot * code[i];
            rot *= step;
        }
        p[j] = s;
    }
}

/* Mean power of the prompt correlations. */
static double prompt_power(const float complex *x, size_t n, int ms, double sample_rate, const uint8_t *chips,
                           double doppler_hz, double tau, float *code, double complex *p)
{
    double s = 0.0;
    int j;

    prompt(x, n, ms, sample_rate, chips, doppler_hz, tau, code, p);
    for (j = 0; j < ms; j++)
        s += creal(p[j] * conj(p[j]));
    return s / ms;
}

int m2sdr_gnss_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_gnss_config *cfg,
                       struct m2sdr_gnss_result *res)
{
    const uint32_t mask = cfg && cfg->prn_mask ? cfg->prn_mask : 0xffffffffu;
    const double dmax   = cfg && cfg->doppler_hz > 0.0 ? cfg->doppler_hz : 5000.0;
    const double dstep  = cfg && cfg->doppler_step_hz > 0.0 ? cfg->doppler_step_hz : 500.0;
    const int ms        = cfg && cfg->ms > 0 ? cfg->ms : 10;
    const double pfa    = cfg && cfg->pfa > 0.0 ? cfg->pfa : 1e-3;
    const size_t m = SEARCH_N;              /* Samples of a millisecond in the search. */
    const double cps = M2SDR_GNSS_CA_RATE / SEARCH_RATE;   /* Chips per search sample. */
    size_t n, decim, k;
    int nd, nr, p, d, j, prns[M2SDR_GNSS_PRNS], np = 0;
    double frac[M2SDR_GNSS_MAX_DOPPLERS];
    int dfrac[M2SDR_GNSS_MAX_DOPPLERS], dint[M2SDR_GNSS_MAX_DOPPLERS];
    struct m2sdr_fft *fwd = NULL, *inv = NULL;
    float complex *codef = NULL, *spec = NULL, *buf = NULL, *prod = NULL, *corr = NULL;
    float *grid = NULL, *code = NULL;
    uint8_t (*chips)[M2SDR_GNSS_CA_CHIPS] = NULL;
    double complex *pr = NULL;
    double best_metric = 0.0, thr;
    int ret = -1;

    if (!x || !res)
        return -1;
    if (fabs(sample_rate - 2.048e6) < 1.0)
        n = 2048;
    else if (fabs(sample_rate - 4.096e6) < 1.0)
        n = 4096;
    else
        return -1;
    decim = n / m;
    if (ms > MAX_MS || count < (size_t)ms * n || !(dstep > 0.0) || dmax < 0.0)
        return -1;
    nd = 2 * (int)floor(dmax / dstep + 1e-9) + 1;
    if (nd > M2SDR_GNSS_MAX_DOPPLERS)
        return -1;
    memset(res, 0, sizeof(*res));
    res->ms   = ms;
    res->best = -1;
    if (m2sdr_gnss_jamming(x, count, sample_rate, &res->jamming) != 0)
        return -1;

    /* Doppler bins: whole kHz (rotations of the spectrum of a millisecond) plus one of the nr
     * fractional parts. */
    nr = 0;
    for (d = 0; d < nd; d++) {
        const double f  = -(double)(nd / 2) * dstep + d * dstep;
        const double q  = floor(f / 1000.0 + 1e-9);
        const double fr = f - 1000.0 * q;
        int r;

        for (r = 0; r < nr; r++)
            if (fabs(frac[r] - fr) < 1e-6)
                break;
        if (r == nr)
            frac[nr++] = fr;
        dfrac[d] = r;
        dint[d]  = (int)q;
    }
    for (p = 1; p <= M2SDR_GNSS_PRNS; p++)
        if (mask & (1u << (p - 1)))
            prns[np++] = p;
    res->nprn             = np;
    res->ndopplers        = nd;
    res->doppler_start_hz = -(double)(nd / 2) * dstep;
    res->doppler_step_hz  = dstep;

    fwd   = m2sdr_fft_alloc(m, false);
    inv   = m2sdr_fft_alloc(m, true);
    codef = malloc((size_t)np * m * sizeof(*codef));
    spec  = malloc((size_t)nr * m * sizeof(*spec));
    buf   = malloc(m * sizeof(*buf));
    prod  = malloc(m * sizeof(*prod));
    corr  = malloc(m * sizeof(*corr));
    grid  = calloc((size_t)np * nd * m, sizeof(*grid));
    code  = malloc(n * sizeof(*code));
    chips = malloc((size_t)np * sizeof(*chips));
    pr    = malloc((size_t)ms * sizeof(*pr));
    if (!fwd || !inv || !codef || !spec || !buf || !prod || !corr || !grid || !code || !chips || !pr)
        goto out;

    /* Conjugate spectra of the codes, one millisecond from their epoch. */
    for (p = 0; p < np; p++) {
        m2sdr_gnss_ca_code(prns[p], chips[p]);
        sample_code(chips[p], SEARCH_RATE, M2SDR_GNSS_CA_RATE, 0.0, 0, m, code);
        for (k = 0; k < m; k++)
            buf[k] = code[k];
        m2sdr_fft_run(fwd, buf, codef + (size_t)p * m);
        for (k = 0; k < m; k++)
            codef[(size_t)p * m + k] = conjf(codef[(size_t)p * m + k]);
    }

    /* Search at 2.048 MSPS (4.096 MSPS summed by pairs: the main lobe of the code is kept, the
     * noise of a correlation is unchanged): |IFFT(X shifted * conj(C))|^2 summed over the
     * milliseconds. */
    for (j = 0; j < ms; j++) {
        int r;

        for (r = 0; r < nr; r++) {
            const double w = -2.0 * M_PI * frac[r] / sample_rate;
            const double complex step = cexp(I * w);
            double complex rot = cexp(I * w * (double)((size_t)j * n));

            for (k = 0; k < m; k++) {
                double complex s = 0.0;
                size_t i;

                for (i = 0; i < decim; i++) {
                    s   += (double complex)x[(size_t)j * n + k * decim + i] * rot;
                    rot *= step;
                }
                buf[k] = (float complex)s;
            }
            m2sdr_fft_run(fwd, buf, spec + (size_t)r * m);
        }
        for (p = 0; p < np; p++) {
            const float complex *cf = codef + (size_t)p * m;

            for (d = 0; d < nd; d++) {
                const float complex *s = spec + (size_t)dfrac[d] * m;
                float *g = grid + ((size_t)p * nd + d) * m;
                /* Removing q kHz more moves bin k + q to bin k. */
                const size_t shift = (size_t)(((long)dint[d] % (long)m + (long)m) % (long)m);

                for (k = 0; k < m - shift; k++)
                    prod[k] = s[k + shift] * cf[k];
                for (; k < m; k++)
                    prod[k] = s[k + shift - m] * cf[k];
                m2sdr_fft_run(inv, prod, corr);
                for (k = 0; k < m; k++)
                    g[k] += crealf(corr[k]) * crealf(corr[k]) + cimagf(corr[k]) * cimagf(corr[k]);
            }
        }
    }

    /* Detection: the floor of a PRN is the mean of its cells away from its peak; the cells
     * over it are sums of ms exponential cells, Gamma(ms). */
    thr = gamma_threshold(ms, pfa / ((double)np * nd * (double)m)) / ms;
    res->threshold_db = 10.0 * log10(thr);
    for (p = 0; p < np; p++) {
        struct m2sdr_gnss_prn *o = &res->prn[p];
        const float *g = grid + (size_t)p * nd * m;
        const long guard = (long)(2.0 / cps) + 1;
        double peak = -1.0, floor_sum = 0.0, floor_mean, noise_c;
        size_t floor_n = 0, kb = 0;
        int db = 0;

        o->prn = prns[p];
        for (d = 0; d < nd; d++)
            for (k = 0; k < m; k++)
                if (g[(size_t)d * m + k] > peak) {
                    peak = g[(size_t)d * m + k];
                    kb   = k;
                    db   = d;
                }
        for (d = 0; d < nd; d++)
            for (k = 0; k < m; k++) {
                long dk = labs((long)k - (long)kb);

                if (dk > (long)m / 2)
                    dk = (long)m - dk;
                if (dk <= guard)
                    continue;
                floor_sum += g[(size_t)d * m + k];
                floor_n++;
            }
        floor_mean   = floor_n ? floor_sum / (double)floor_n : 1e-30;
        o->metric_db = 10.0 * log10(peak / floor_mean);
        o->detected  = peak / floor_mean >= thr;
        if (!o->detected)
            continue;
        res->detected++;
        /* Noise of a prompt correlation: a cell of the floor is ms of them times m^2 (FFTs
         * unscaled); a correlation over n samples at the full rate has the same noise. */
        noise_c = floor_mean / ms / ((double)m * (double)m);

        {
            const double spc = sample_rate / M2SDR_GNSS_CA_RATE;   /* Samples per chip, full rate. */
            const float *row = g + (size_t)db * m;
            double tau = (double)kb * (double)decim, fd, sig, a, b, c, den, delta;
            double complex z = 0.0;
            int it;

            /* Doppler: the bin, then the turn of the prompt from one millisecond to the next
             * (unambiguous within +-500 Hz; a data bit edge flips one of the ms - 1 terms). */
            fd = res->doppler_start_hz + db * dstep;
            for (it = 0; it < 2 && ms >= 2; it++) {
                double complex dz = 0.0;

                prompt(x, n, ms, sample_rate, chips[p], fd, tau, code, pr);
                for (j = 1; j < ms; j++)
                    dz += pr[j] * conj(pr[j - 1]);
                fd += carg(dz) / (2.0 * M_PI * (double)n / sample_rate);
            }
            o->doppler_hz = fd;

            /* Code phase between the samples: the correlation is a triangle of slope one per
             * chip. Early, prompt and late half a chip apart at the full rate; their
             * amplitudes are the square roots of their powers above the noise. */
            a   = sqrt(fmax(prompt_power(x, n, ms, sample_rate, chips[p], fd, tau - 0.5 * spc, code, pr) - noise_c, 0.0));
            c   = sqrt(fmax(prompt_power(x, n, ms, sample_rate, chips[p], fd, tau + 0.5 * spc, code, pr) - noise_c, 0.0));
            b   = sqrt(fmax(prompt_power(x, n, ms, sample_rate, chips[p], fd, tau, code, pr) - noise_c, 0.0));
            den = 2.0 * (b - fmin(a, c));
            delta = den > 0.0 ? 0.5 * (c - a) / den : 0.0;   /* Chips. */
            if (delta > 0.5)
                delta = 0.5;
            if (delta < -0.5)
                delta = -0.5;
            tau += delta * spc;
            tau  = fmod(tau + (double)n, (double)n);
            o->code_phase_chips   = fmod(tau / spc, M2SDR_GNSS_CA_CHIPS);
            o->code_phase_samples = (size_t)lround(tau) % n;

            /* C/N0: prompt power over the noise of a correlation, coherent time n / sample_rate. */
            sig = prompt_power(x, n, ms, sample_rate, chips[p], fd, tau, code, pr);
            for (j = 0; j < ms; j++)
                z += pr[j] * pr[j];
            o->cn0_dbhz = 10.0 * log10(fmax(sig - noise_c, noise_c * 1e-3) / noise_c * sample_rate / (double)n);

            if (res->best < 0 || o->metric_db > best_metric) {
                const double complex turn = cexp(-0.5 * I * carg(z));
                const double scale = 1.0 / sqrt(sig > 0.0 ? sig : 1.0);
                size_t c0;

                res->best    = p;
                best_metric  = o->metric_db;
                res->npoints = ms < M2SDR_GNSS_MAX_POINTS ? ms : M2SDR_GNSS_MAX_POINTS;
                for (j = 0; j < res->npoints; j++)
                    res->points[j] = (float complex)(pr[j] * turn * scale);
                /* Traces: the best sample of each chip at the Doppler of the peak, and each
                 * Doppler bin at the code phase of the peak, over the floor. */
                for (c0 = 0; c0 < M2SDR_GNSS_CA_CHIPS; c0++)
                    res->code_db[c0] = -200.0f;
                for (k = 0; k < m; k++) {
                    const size_t chip = (size_t)((double)k * cps) % M2SDR_GNSS_CA_CHIPS;
                    const float v = (float)(10.0 * log10(row[k] / floor_mean + 1e-30));

                    if (v > res->code_db[chip])
                        res->code_db[chip] = v;
                }
                for (d = 0; d < nd; d++)
                    res->doppler_db[d] = (float)(10.0 * log10(g[(size_t)d * m + kb] / floor_mean + 1e-30));
            }
        }
    }
    res->found = res->detected > 0;
    ret = 0;
out:
    m2sdr_fft_free(fwd);
    m2sdr_fft_free(inv);
    free(codef);
    free(spec);
    free(buf);
    free(prod);
    free(corr);
    free(grid);
    free(code);
    free(chips);
    free(pr);
    return ret;
}
