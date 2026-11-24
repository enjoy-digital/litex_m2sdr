/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: EMC pre-compliance, the measuring receiver of CISPR 16-1-1.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_monitoring.h"
#include "m2sdr_fft.h"

/* The samples and the products here are finite: complex products without the NaN and infinity
 * handling of C99 (__mulsc3). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("cx-limited-range")
#endif

#define MIN_COUNT          4096
#define FILTER_SPAN        3.0     /* Envelope rate over the RBW: the Gaussian is 54 dB down at
                                    * 1.5 RBW from its centre, where the rate folds it. */
#define MIN_RBW_BINS       8.0     /* Bins of the FFT of the capture in the RBW. */
#define METER_STEPS        50      /* Steps of the meter in its time constant, at least. */
#define SECANT_ITER        60
#define METER_SETTLE       12.0    /* Meter time constants to its steady state. */
#define NO_LIMIT           999.0f

/* Bands */
/*-------*/

/* CISPR 16-1-1 (measuring receivers, quasi-peak: table of the band characteristics):
 * bandwidth at 6 dB, electrical charge and discharge time constants, mechanical time constant
 * of the critically damped meter. The meter of the CISPR-AV detector: 160 ms in bands A and B,
 * 100 ms in bands C and D. */
int m2sdr_emc_band_params(enum m2sdr_emc_band band, double *rbw_hz, double *charge_s, double *discharge_s,
                          double *meter_s, double *av_meter_s)
{
    static const double params[3][5] = {
        {   200.0, 45e-3, 500e-3, 160e-3, 160e-3 },   /* A: 9 to 150 kHz. */
        {   9.0e3,  1e-3, 160e-3, 160e-3, 160e-3 },   /* B: 0.15 to 30 MHz. */
        { 120.0e3,  1e-3, 550e-3, 100e-3, 100e-3 },   /* C, D: 30 to 1000 MHz. */
    };
    const double *p;

    if (band < M2SDR_EMC_BAND_A || band > M2SDR_EMC_BAND_CD)
        return -1;
    p = params[band - M2SDR_EMC_BAND_A];
    if (rbw_hz)
        *rbw_hz = p[0];
    if (charge_s)
        *charge_s = p[1];
    if (discharge_s)
        *discharge_s = p[2];
    if (meter_s)
        *meter_s = p[3];
    if (av_meter_s)
        *av_meter_s = p[4];
    return 0;
}

/* Lines */
/*-------*/

double m2sdr_emc_line_db(const struct m2sdr_emc_point *line, int n, double freq_hz)
{
    int i;
    double u;

    if (!line || n <= 0)
        return NAN;
    if (freq_hz <= line[0].freq_hz)
        return line[0].level_db;
    if (freq_hz >= line[n - 1].freq_hz)
        return line[n - 1].level_db;
    /* The segment that holds freq_hz; at a step, the lower part up to and at its frequency. */
    for (i = 0; i + 1 < n && line[i + 1].freq_hz < freq_hz; i++)
        ;
    if (line[i + 1].freq_hz <= line[i].freq_hz)
        return line[i + 1].level_db;
    u = log(freq_hz / line[i].freq_hz) / log(line[i + 1].freq_hz / line[i].freq_hz);
    return line[i].level_db + u * (line[i + 1].level_db - line[i].level_db);
}

static bool line_valid(const struct m2sdr_emc_point *line, int n)
{
    int i;

    if (n < 0 || n > M2SDR_EMC_MAX_LIMIT)
        return false;
    for (i = 0; i < n; i++)
        if (!(line[i].freq_hz > 0) || !isfinite(line[i].level_db) || (i > 0 && line[i].freq_hz < line[i - 1].freq_hz))
            return false;
    return true;
}

/* Detectors */
/*-----------*/

/* Quasi-peak detector on the envelope e[0..n-1], time step dt. The diode rectifies the IF
 * carrier: it conducts on its crests, over +-theta with cos(theta) = v / e, and its mean current
 * over a cycle through the charge resistor is e phi(v / e) / Rc, phi(u) = (sqrt(1 - u^2)
 * - u acos(u)) / pi. With the discharge resistor across:
 *   dv/dt = e phi(v / e) / (Rc C) - v / (Rd C).
 * CISPR gives the discharge time constant, Rd C, and the charge one as the time to 63 % of the
 * final output on a sine applied at once: Rc C follows from it (qp_calibrate()), and the output
 * is scaled by the final one so that a CW reads its level. A linear charge on the envelope,
 * (e - v) / (Rc C), reads isolated and slow pulses 2 to 3 dB under the pulse response curves of
 * bands B and C/D; this one fits them, and band A, within about 1 dB.
 * phi is tabulated; the charging steps are of second order (midpoint), the discharge exact. */
#define QP_TABLE  4096

struct qp {
    double kc;                  /* dt / (Rc C). */
    double kd;                  /* dt / (Rd C). */
    double dd;                  /* exp(-dt / (Rd C)). */
    double final;               /* Output for a constant input of 1. */
    float  phi[QP_TABLE + 2];
};

static double phi_exact(double u)
{
    return (sqrt(fmax(1.0 - u * u, 0.0)) - u * acos(fmin(u, 1.0))) / M_PI;
}

static inline double phi_table(const struct qp *q, double u)
{
    const double x = u * QP_TABLE;
    const int i = (int)x;

    if (i >= QP_TABLE)
        return 0.0;
    return q->phi[i] + (x - i) * (q->phi[i + 1] - q->phi[i]);
}

/* Final output for a constant input of 1 with rc = Rc C / Rd C: phi(v) = rc v. */
static double qp_final(double rc)
{
    double lo = 0.0, hi = 1.0;
    int i;

    for (i = 0; i < 60; i++) {
        const double v = 0.5 * (lo + hi);

        if (phi_exact(v) > rc * v)
            lo = v;
        else
            hi = v;
    }
    return 0.5 * (lo + hi);
}

/* Rc C for a rise to 63 % in tc: t63 = Rc C integral from 0 to 0.63 final of
 * dv / (phi(v) - rc v), by Simpson's rule (the integrand stays finite there); t63 grows with
 * Rc C, by bisection. */
static double qp_calibrate(double tc, double td)
{
    double lo = tc / 100.0, hi = tc * 10.0, rcc = tc;
    int it, k;

    for (it = 0; it < 60; it++) {
        const int nk = 256;
        const double vf = qp_final((rcc = sqrt(lo * hi)) / td), h = 0.63 * vf / nk;
        double sum = 0;

        for (k = 0; k <= nk; k++) {
            const double v = k * h;
            const double w = (k == 0 || k == nk) ? 1.0 : (k & 1) ? 4.0 : 2.0;

            sum += w / (phi_exact(v) - rcc / td * v);
        }
        if (rcc * sum * h / 3.0 > tc)
            hi = rcc;
        else
            lo = rcc;
    }
    return rcc;
}

static void qp_init(struct qp *q, double tc, double td, double dt)
{
    const double rcc = qp_calibrate(tc, td);
    int i;

    for (i = 0; i <= QP_TABLE + 1; i++)
        q->phi[i] = (float)phi_exact(fmin((double)i / QP_TABLE, 1.0));
    q->kc    = dt / rcc;
    q->kd    = dt / td;
    q->dd    = exp(-dt / td);
    q->final = qp_final(rcc / td);
}

/* One step of a charging sample: e > v. */
static inline double qp_charge(const struct qp *q, double e, double v)
{
    const double k1 = q->kc * e * phi_table(q, v / e) - q->kd * v;
    const double vm = fmax(v + 0.5 * k1, 0.0);
    const double k2 = vm < e ? q->kc * e * phi_table(q, vm / e) - q->kd * vm : -q->kd * vm;

    return v + k2;
}

/* One pass from v0 over e. Charging samples get appended to list (when given): a pass from zero
 * is the lowest output there can be, so its charging samples hold those of any other pass. */
static double qp_pass(const struct qp *q, const float *e, size_t n, double v0, size_t *list, size_t *nlist)
{
    double v = v0;
    size_t i;

    for (i = 0; i < n; i++) {
        if (e[i] > v) {
            v = qp_charge(q, e[i], v);
            if (list)
                list[(*nlist)++] = i;
        } else {
            v *= q->dd;
        }
    }
    return v;
}

/* The same over the charging samples only, the discharge between them in one step each. */
static double qp_pass_list(const struct qp *q, const float *e, double v0, const size_t *list, const float *decay,
                           size_t nlist)
{
    double v = v0;
    size_t k;

    for (k = 0; k < nlist; k++) {
        const float ek = e[list[k]];

        v *= decay[k];
        if (ek > v)
            v = qp_charge(q, ek, v);
        else
            v *= q->dd;
    }
    return v * decay[nlist];
}

/* Critically damped meter, 1 / (1 + s T)^2: two first order sections at the rate of the blocks
 * u[0..nb-1] of the input. Periodic: the blocks repeat until the meter settles; single: they
 * are followed by `tail` (a function of the block index past the end). Returns the highest
 * deflection over the last period (periodic) or over the whole response (single). */
static double meter_reading(const double *u, size_t nb, double dtb, double tm, bool single, const double *tail,
                            size_t ntail)
{
    const double a = 1.0 - exp(-dtb / tm);
    double s1 = 0, s2 = 0, best = 0;
    size_t b, pass, passes;

    if (single) {
        for (b = 0; b < nb + ntail; b++) {
            s1 += ((b < nb ? u[b] : tail[b - nb]) - s1) * a;
            s2 += (s1 - s2) * a;
            if (s2 > best)
                best = s2;
        }
        return best;
    }
    /* Steady state from the mean, then passes for METER_SETTLE time constants. */
    for (b = 0; b < nb; b++)
        s1 += u[b];
    s1 /= (double)nb;
    s2 = s1;
    passes = (size_t)ceil(METER_SETTLE * tm / (dtb * (double)nb)) + 1;
    for (pass = 0; pass < passes; pass++) {
        best = 0;
        for (b = 0; b < nb; b++) {
            s1 += (u[b] - s1) * a;
            s2 += (s1 - s2) * a;
            if (s2 > best)
                best = s2;
        }
    }
    return best;
}

/* Analysis */
/*----------*/

int m2sdr_emc_analyse(const float complex *x, size_t count, double sample_rate,
                      const struct m2sdr_emc_config *cfg, struct m2sdr_emc_result *res)
{
    struct m2sdr_fft *fft = NULL, *ifft = NULL;
    float complex *spec = NULL, *in = NULL, *y = NULL;
    float *gauss = NULL, *env = NULL, *decay = NULL;
    size_t *list = NULL;
    double *ublk = NULL, *tail = NULL;
    enum m2sdr_emc_band band = M2SDR_EMC_BAND_B;
    double rbw, tc, td, tm, tam, df, start, stop, step, env_rate, dt, dtb, offset = 0;
    size_t n = 1, m = 1, nblk_len = 1, nb, ntail, i;
    int c, nch, half, rc = -1;
    bool single = cfg && cfg->single;
    struct qp *q = NULL;

    if (!x || !res || !(sample_rate > 0) || count < MIN_COUNT)
        return -1;
    memset(res, 0, sizeof(*res));
    if (cfg && cfg->band != M2SDR_EMC_BAND_AUTO)
        band = cfg->band;
    else if (cfg && cfg->centre_hz > 0)
        band = cfg->centre_hz < 150e3 ? M2SDR_EMC_BAND_A : cfg->centre_hz < 30e6 ? M2SDR_EMC_BAND_B : M2SDR_EMC_BAND_CD;
    if (m2sdr_emc_band_params(band, &rbw, &tc, &td, &tm, &tam) != 0)
        return -1;
    if (cfg && (!line_valid(cfg->factor, cfg->nfactor) || !line_valid(cfg->qp_limit, cfg->nqp_limit) ||
                !line_valid(cfg->av_limit, cfg->nav_limit) || !isfinite(cfg->offset_db)))
        return -1;
    while (n * 2 <= count)
        n *= 2;
    df = sample_rate / (double)n;
    if (rbw < MIN_RBW_BINS * df || FILTER_SPAN * rbw > sample_rate)
        return -1;
    while ((double)m * df < FILTER_SPAN * rbw)
        m *= 2;
    env_rate = (double)m * df;
    dt       = 1.0 / env_rate;
    half     = (int)(m / 2);

    /* Channels. */
    start = -0.5 * sample_rate + 0.5 * FILTER_SPAN * rbw;
    stop  = -start;
    step  = 0.5 * rbw;
    if (cfg && (cfg->start_hz != 0 || cfg->stop_hz != 0 || cfg->step_hz != 0)) {
        start = cfg->start_hz;
        stop  = cfg->stop_hz;
        step  = cfg->step_hz > 0 ? cfg->step_hz : step;
        if (stop < start || fabs(start) + 0.5 * FILTER_SPAN * rbw > 0.5 * sample_rate + 1e-9 ||
            fabs(stop) + 0.5 * FILTER_SPAN * rbw > 0.5 * sample_rate + 1e-9)
            return -1;
    } else if ((stop - start) / step + 1 > M2SDR_EMC_MAX_CHANNELS) {
        step = (stop - start) / (M2SDR_EMC_MAX_CHANNELS - 1);
    }
    nch = (int)floor((stop - start) / step + 1e-9) + 1;
    if (nch > M2SDR_EMC_MAX_CHANNELS)
        return -1;

    res->band          = band;
    res->rbw_hz        = rbw;
    res->charge_s      = tc;
    res->discharge_s   = td;
    res->meter_s       = tm;
    res->av_meter_s    = tam;
    res->capture_s     = (double)n / sample_rate;
    res->env_rate_hz   = env_rate;
    res->nchannels     = nch;

    /* Meter blocks: a power of two of envelope samples, METER_STEPS or more per time constant
     * (of the faster meter). */
    while (nblk_len * 2 <= m && (double)(nblk_len * 2) * dt <= fmin(tm, tam) / METER_STEPS)
        nblk_len *= 2;
    nb    = m / nblk_len;
    dtb   = (double)nblk_len * dt;
    ntail = single ? (size_t)ceil((5.0 * td + METER_SETTLE * fmax(tm, tam)) / dtb) : 0;

    fft   = m2sdr_fft_alloc(n, false);
    ifft  = m2sdr_fft_alloc(m, true);
    spec  = malloc(n * sizeof(*spec));
    in    = malloc(m * sizeof(*in));
    y     = malloc(m * sizeof(*y));
    gauss = malloc(m * sizeof(*gauss));
    env   = malloc(m * sizeof(*env));
    decay = malloc((m + 1) * sizeof(*decay));
    list  = malloc(m * sizeof(*list));
    ublk  = malloc(nb * sizeof(*ublk));
    tail  = malloc((ntail + 1) * sizeof(*tail));
    q     = malloc(sizeof(*q));
    if (!q || !fft || !ifft || !spec || !in || !y || !gauss || !env || !decay || !list || !ublk || !tail)
        goto out;
    m2sdr_fft_run(fft, x, spec);

    /* Gaussian RBW filter, 6 dB down at +-rbw/2: |H(f)| = 2^-(2 f / rbw)^2. Its impulse bandwidth
     * (integral over peak) is rbw sqrt(pi / ln 2) / 2 = 1.064 rbw. Scaled by 1/n for the
     * unscaled transforms. */
    res->impulse_bw_hz = 0;
    for (i = 0; i < m; i++) {
        const int j = (int)i < half ? (int)i : (int)i - (int)m;
        const double u = 2.0 * j * df / rbw;
        const double h = exp(-M_LN2 * u * u);

        gauss[i] = (float)(h / (double)n);
        res->impulse_bw_hz += h * df;
    }

    qp_init(q, tc, td, dt);

    for (c = 0; c < nch; c++) {
        const double fc = start + c * step;
        const long kc = lround(fc / df);
        double peak = 0, v0, vq, qp, av;
        size_t nlist = 0, b;
        int it;

        /* Channel at baseband: the bins around kc through the filter, at env_rate. */
        for (i = 0; i < m; i++) {
            const long j = (long)i < half ? (long)i : (long)i - (long)m;
            const long k = kc + j;

            if (k < -(long)n / 2 || k >= (long)n / 2)
                in[i] = 0;
            else
                in[i] = spec[(size_t)((k + (long)n) % (long)n)] * gauss[i];
        }
        m2sdr_fft_run(ifft, in, y);
        for (i = 0; i < m; i++) {
            env[i] = cabsf(y[i]);
            if (env[i] > peak)
                peak = env[i];
        }

        /* Quasi-peak: the first pass from zero gives the charging samples; in the steady state
         * the capacitor ends a period where it started: the fixed point of v0 -> v(end), by the
         * secant on the charging samples. */
        v0 = qp_pass(q, env, m, 0.0, list, &nlist);
        if (!single) {
            size_t prev = 0;

            for (i = 0; i < nlist; i++) {
                const size_t gap = list[i] - (i ? prev + 1 : 0);

                decay[i] = gap ? (float)pow(q->dd, (double)gap) : 1.0f;
                prev = list[i];
            }
            decay[nlist] = (float)pow(q->dd, nlist ? (double)(m - 1 - prev) : (double)m);
            /* Secant on G(v) = F(v) - v from the points 0 and F(0). */
            {
                double va = 0.0, ga = v0, vb = v0, gb = qp_pass_list(q, env, v0, list, decay, nlist) - v0;

                for (it = 0; it < SECANT_ITER && fabs(gb) > 1e-7 * fmax(vb, 1e-30); it++) {
                    double vn = gb != ga ? vb - gb * (vb - va) / (gb - ga) : vb + gb;

                    if (!(vn >= 0))
                        vn = vb + gb;
                    va = vb;
                    ga = gb;
                    vb = vn;
                    gb = qp_pass_list(q, env, vb, list, decay, nlist) - vb;
                }
                v0 = vb;
            }
        } else {
            v0 = 0.0;
        }

        /* Final pass from v0: the output per block for the meter. */
        vq = v0;
        for (b = 0; b < nb; b++) {
            double s = 0;

            for (i = b * nblk_len; i < (b + 1) * nblk_len; i++) {
                if (env[i] > vq)
                    vq = qp_charge(q, env[i], vq);
                else
                    vq *= q->dd;
                s += vq;
            }
            ublk[b] = s / (double)nblk_len / q->final;
        }
        if (single) {
            /* Discharge after the capture: block means of the exponential. */
            const double db = pow(q->dd, (double)nblk_len);
            const double mean = q->dd * (1.0 - db) / (1.0 - q->dd) / (double)nblk_len;

            for (b = 0; b < ntail; b++) {
                tail[b] = vq * mean / q->final;
                vq *= db;
            }
        }
        qp = meter_reading(ublk, nb, dtb, tm, single, tail, ntail);

        /* Average: the envelope through its meter, nothing after a single capture. */
        for (b = 0; b < nb; b++) {
            double s = 0;

            for (i = b * nblk_len; i < (b + 1) * nblk_len; i++)
                s += env[i];
            ublk[b] = s / (double)nblk_len;
        }
        for (b = 0; b < ntail; b++)
            tail[b] = 0;
        av = meter_reading(ublk, nb, dtb, tam, single, tail, ntail);

        res->freq_hz[c]   = (cfg ? cfg->centre_hz : 0.0) + (double)kc * df;
        res->peak_dbfs[c] = (float)(20.0 * log10(peak + 1e-15));
        res->qp_dbfs[c]   = (float)(20.0 * log10(qp + 1e-15));
        res->av_dbfs[c]   = (float)(20.0 * log10(av + 1e-15));
        if (c == 0 || res->peak_dbfs[c] > res->max_peak_dbfs) {
            res->max_peak_dbfs    = res->peak_dbfs[c];
            res->max_peak_freq_hz = res->freq_hz[c];
        }
    }

    /* Limits. */
    if (cfg)
        offset = cfg->offset_db;
    res->limits          = cfg && (cfg->nqp_limit > 0 || cfg->nav_limit > 0);
    res->pass            = true;
    res->worst_margin_db = NO_LIMIT;
    for (c = 0; c < nch; c++) {
        const double f = res->freq_hz[c];
        double k = offset, lq, la;

        res->margin_peak_db[c] = res->margin_qp_db[c] = res->margin_av_db[c] = NO_LIMIT;
        if (!res->limits)
            continue;
        if (cfg->nfactor > 0)
            k += m2sdr_emc_line_db(cfg->factor, cfg->nfactor, f);
        if (cfg->nqp_limit > 0) {
            lq = m2sdr_emc_line_db(cfg->qp_limit, cfg->nqp_limit, f);
            res->margin_peak_db[c] = (float)(lq - (res->peak_dbfs[c] + k));
            res->margin_qp_db[c]   = (float)(lq - (res->qp_dbfs[c] + k));
            if (res->margin_qp_db[c] < res->worst_margin_db) {
                res->worst_margin_db = res->margin_qp_db[c];
                res->worst_freq_hz   = f;
                res->worst_detector  = 1;
            }
        }
        if (cfg->nav_limit > 0) {
            la = m2sdr_emc_line_db(cfg->av_limit, cfg->nav_limit, f);
            res->margin_av_db[c] = (float)(la - (res->av_dbfs[c] + k));
            if (res->margin_av_db[c] < res->worst_margin_db) {
                res->worst_margin_db = res->margin_av_db[c];
                res->worst_freq_hz   = f;
                res->worst_detector  = 2;
            }
        }
    }
    if (res->limits)
        res->pass = res->worst_margin_db >= 0;
    rc = 0;
out:
    m2sdr_fft_free(fft);
    m2sdr_fft_free(ifft);
    free(spec);
    free(in);
    free(y);
    free(gauss);
    free(env);
    free(decay);
    free(list);
    free(ublk);
    free(tail);
    free(q);
    return rc;
}
