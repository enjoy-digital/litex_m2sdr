/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: FM broadcast, stereo multiplex and RDS.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_fm.h"

#define CHAN_PASS_HZ     100000.0  /* Channel filter: flat to here... */
#define CHAN_STOP_HZ     140000.0  /* ...60 dB down from here. */
#define MPX_PASS_HZ       60000.0  /* Multiplex low-pass of the deviation figures: RDS ends at 59.4 kHz. */
#define MPX_STOP_HZ       80000.0
#define STOPBAND_DB          60.0
#define MAX_OFFSET_HZ     25000.0  /* Station searched no further from the centre (or cfg->offset_hz). */

#define DROOP_FIT_HZ      38000.0  /* Discriminator response compensated exactly here. */
#define PSD_LEN            4096    /* Spectrum of the multiplex: 59 Hz bins at 240 kHz. */
#define PILOT_PIECE_S      0.01    /* Pieces of the pilot phase. */
#define PILOT_MIN_HZ     1000.0
#define PILOT_MIN_COHERENCE 0.9
#define MIN_CNR_DB            6.0

#define MONO_LO_HZ           30.0  /* L+R. */
#define MONO_HI_HZ        15000.0
#define STEREO_LO_HZ      23000.0  /* Sidebands of L-R. */
#define STEREO_HI_HZ      53000.0
#define RDS_LO_HZ         54600.0  /* RDS: 57 kHz +-2.4 kHz. */
#define RDS_HI_HZ         59400.0
#define RDS_NOISE_LO_HZ   61000.0  /* Noise next to it, where the multiplex has nothing. */
#define RDS_NOISE_HI_HZ   66000.0
#define GUARD_LO_HZ       16000.0  /* Between the audio and the pilot. */
#define GUARD_HI_HZ       18500.0
#define SNR_REF_HZ        75000.0  /* Audio SNR: from a sine of this deviation. */

#define FINE_BIN_HZ          50.0  /* Histogram of the percentiles. */
#define FINE_BINS          4000

/* Filters */
/*---------*/

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    int k;

    for (k = 1; k < 64; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum  += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}

/* Low-pass with a Kaiser window, unity gain at DC, odd length. */
static float *lowpass_design(double rate, double pass_hz, double stop_hz, unsigned *count)
{
    const double beta   = 0.1102 * (STOPBAND_DB - 8.7);
    const double width  = 2.0 * M_PI * (stop_hz - pass_hz) / rate;
    const double cutoff = (pass_hz + stop_hz) / rate;
    const unsigned n    = ((unsigned)ceil((STOPBAND_DB - 7.95) / (2.285 * width)) + 1) | 1;
    double sum = 0.0;
    float *taps;
    unsigned i;

    taps = malloc(n * sizeof(*taps));
    if (!taps)
        return NULL;
    for (i = 0; i < n; i++) {
        const double t = i - (n - 1) / 2.0;
        const double r = 2.0 * t / (n - 1);
        const double w = bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
        const double h = (t == 0.0) ? cutoff : sin(M_PI * cutoff * t) / (M_PI * t);

        taps[i] = (float)(h * w);
        sum    += h * w;
    }
    for (i = 0; i < n; i++)
        taps[i] = (float)(taps[i] / sum);
    *count = n;
    return taps;
}

/* Four partial sums: the compiler turns them into one vector. */
static inline float dot(const float *a, const float *b, unsigned n)
{
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    unsigned i;

    for (i = 0; i + 4 <= n; i += 4) {
        s0 += a[i + 0] * b[i + 0];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    for (; i < n; i++)
        s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}

/* Discriminator */
/*---------------*/

/* The phase step between two samples is the frequency averaged over one sample: a response
 * sin(x) / x, x = pi * f / rate, 0.8 dB down at 57 kHz for 240 kHz. Three taps (k, 1 - 2k, k)
 * after it bring it back to 1 at DROOP_FIT_HZ; what remains is droop_response(). */
struct droop {
    double rate, k;
};

static void droop_init(struct droop *d, double rate)
{
    const double x = M_PI * DROOP_FIT_HZ / rate;

    d->rate = rate;
    d->k    = (1.0 - x / sin(x)) / (2.0 * (1.0 - cos(2.0 * x)));
}

static double droop_response(const struct droop *d, double hz)
{
    const double x = M_PI * hz / d->rate;
    const double s = (x == 0.0) ? 1.0 : sin(x) / x;

    return s * (1.0 - 2.0 * d->k * (1.0 - cos(2.0 * x)));
}

/* Spectrum of the multiplex */
/*---------------------------*/

/* One-sided power of a band of the real multiplex: its positive half, twice. */
static double band_power(const struct m2sdr_psd *psd, double lo_hz, double hi_hz)
{
    return 2.0 * pow(10.0, m2sdr_psd_band_power_db(psd, lo_hz, hi_hz) / 10.0);
}

/* One-sided noise density, Hz^2 per Hz. */
static double noise_density(const struct m2sdr_psd *psd, double lo_hz, double hi_hz)
{
    return 2.0 * pow(10.0, m2sdr_psd_noise_density_db(psd, lo_hz, hi_hz) / 10.0);
}

/* Integral of f^2 / (1 + (2 pi tau f)^2) from lo to hi: the noise of the discriminator (f^2)
 * through the de-emphasis. */
static double deemph_f2_integral(double tau, double lo, double hi)
{
    const double a = 2.0 * M_PI * tau;

    if (a == 0.0)
        return (hi * hi * hi - lo * lo * lo) / 3.0;
    return (hi - lo) / (a * a) - (atan(a * hi) - atan(a * lo)) / (a * a * a);
}

/* Pilot */
/*-------*/

/* The pilot in pieces of `len` samples (Hann window): their phase gives its frequency, the
 * sum of the pieces turned by it gives its amplitude and its phase at sample 0, the
 * multiplex then holds amp * cos(omega * n + phi0). */
struct pilot_fit {
    double amp, omega, phi0, coherence;
};

static int pilot_measure(const float *mpx, size_t n, double rate, struct pilot_fit *fit)
{
    const double omega0 = 2.0 * M_PI * M2SDR_FM_PILOT_HZ / rate;
    const size_t len    = (size_t)(PILOT_PIECE_S * rate);
    const size_t pieces = len ? n / len : 0;
    float  *wr, *wi;
    double complex *z, sum = 0.0, step = 0.0;
    double wsum = 0.0, b, mag = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, phase, prev = 0.0;
    size_t i, c;

    memset(fit, 0, sizeof(*fit));
    if (pieces < 2)
        return -1;
    wr = malloc(len * sizeof(*wr));
    wi = malloc(len * sizeof(*wi));
    z  = malloc(pieces * sizeof(*z));
    if (!wr || !wi || !z) {
        free(wr);
        free(wi);
        free(z);
        return -1;
    }
    for (i = 0; i < len; i++) {
        const double w = 0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / len);

        wr[i] = (float)(w * cos(omega0 * i));
        wi[i] = (float)(-w * sin(omega0 * i));
        wsum += w;
    }
    for (c = 0; c < pieces; c++) {
        const float *p = mpx + c * len;

        z[c] = (dot(wr, p, (unsigned)len) + I * dot(wi, p, (unsigned)len)) *
               cexp(-I * omega0 * (double)(c * len));
    }

    /* Frequency: coarse from the mean turn between pieces (up to half a turn: +-50 Hz), then
     * the least squares line through the phases unwrapped around it. */
    for (c = 1; c < pieces; c++)
        step += z[c] * conj(z[c - 1]);
    b = carg(step) / (double)len;
    for (c = 0; c < pieces; c++) {
        const double t = (double)(c * len) + 0.5 * (double)(len - 1);

        phase = carg(z[c]);
        if (c > 0) {
            const double expect = prev + b * (double)len;

            phase += 2.0 * M_PI * round((expect - phase) / (2.0 * M_PI));
        }
        prev = phase;
        sx  += t;
        sy  += phase;
        sxx += t * t;
        sxy += t * phase;
    }
    b = (pieces * sxy - sx * sy) / (pieces * sxx - sx * sx);

    for (c = 0; c < pieces; c++) {
        const double t = (double)(c * len) + 0.5 * (double)(len - 1);

        sum += z[c] * cexp(-I * b * t);
        mag += cabs(z[c]);
    }
    fit->amp       = 2.0 * cabs(sum) / ((double)pieces * wsum);
    fit->omega     = omega0 + b;
    fit->phi0      = carg(sum);
    fit->coherence = mag > 0.0 ? cabs(sum) / mag : 0.0;
    free(wr);
    free(wi);
    free(z);
    return 0;
}

/* Test tone */
/*-----------*/

/* Amplitude of the tone at omega in L+R and in L-R, complex, over the whole multiplex with a
 * Hann window. L-R is demodulated with the subcarrier of the pilot: the pilot is sin(theta),
 * amp * cos(omega * n + phi0) here, and L-R rides on sin(2 theta) = -sin(2 (omega * n + phi0)). */
static void tone_measure(const float *mpx, size_t n, double omega, const struct pilot_fit *p,
                         double complex *mono, double complex *stereo)
{
    const double complex step_t = cexp(-I * omega);
    const double complex step_s = cexp(I * 2.0 * p->omega);
    double complex rot_t = 1.0, rot_s = cexp(I * 2.0 * p->phi0), am = 0.0, as = 0.0;
    double wsum = 0.0;
    size_t i;

    for (i = 0; i < n; i++) {
        const double w = 0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / n);
        const double s = -2.0 * mpx[i] * cimag(rot_s);

        am   += w * mpx[i] * rot_t;
        as   += w * s * rot_t;
        wsum += w;
        rot_t *= step_t;
        rot_s *= step_s;
        if ((i & 1023) == 1023) {
            rot_t /= cabs(rot_t);
            rot_s /= cabs(rot_s);
        }
    }
    *mono   = 2.0 * am / wsum;
    *stereo = 2.0 * as / wsum;
}

/* Deviation */
/*-----------*/

/* Level exceeded by `share` of the samples, from the fine histogram. */
static double percentile(const uint32_t *hist, size_t total, double share)
{
    const double target = share * (double)total;
    double above = hist[FINE_BINS];   /* Beyond the histogram. */
    int k;

    if (above >= target)
        return FINE_BINS * FINE_BIN_HZ;
    for (k = FINE_BINS - 1; k >= 0; k--) {
        if (above + hist[k] >= target)
            return (k + (above + hist[k] - target) / (double)hist[k]) * FINE_BIN_HZ;
        above += hist[k];
    }
    return 0.0;
}

/* Analysis */
/*----------*/

double m2sdr_fm_analysis_mpx_rate(double sample_rate)
{
    unsigned decim = (unsigned)(sample_rate / M2SDR_FM_AN_MIN_RATE);

    if (decim < 1)
        decim = 1;
    return sample_rate / decim;
}

int m2sdr_fm_analyse(const float complex *x, size_t count, double sample_rate,
                     const struct m2sdr_fm_analysis_config *cfg, struct m2sdr_fm_result *res)
{
    static const struct m2sdr_fm_analysis_config defaults = { .tau = 50e-6 };
    struct m2sdr_psd psd;
    struct droop droop;
    struct pilot_fit pilot;
    struct m2sdr_fm_rds_rx *rds = NULL;
    struct m2sdr_fm_rds_info before;
    float *xr = NULL, *xi = NULL, *chan = NULL, *disc = NULL, *mpx = NULL, *lp = NULL, *lp_taps = NULL;
    uint32_t *fine = NULL;
    float complex *zc = NULL;
    double complex *y = NULL;
    unsigned decim, chan_count = 1, lp_count;
    size_t ny, nd, nm, nl, i;
    double rate, m2 = 0.0, m4 = 0.0, c, nz, mean, wsum, ms, peak, ref;
    int ret = -1;

    if (!x || !res || !(sample_rate >= M2SDR_FM_AN_MIN_RATE && sample_rate <= 1e9) ||
        (double)count < M2SDR_FM_AN_MIN_SECONDS * sample_rate)
        return -1;
    if (!cfg)
        cfg = &defaults;
    if (!(fabs(cfg->offset_hz) < 0.5 * sample_rate - CHAN_PASS_HZ) || !(cfg->tau >= 0.0 && cfg->tau < 1e-3) ||
        !(cfg->tone_hz >= 0.0 && cfg->tone_hz < MONO_HI_HZ))
        return -1;
    memset(res, 0, sizeof(*res));
    memset(&psd, 0, sizeof(psd));

    decim = (unsigned)(sample_rate / M2SDR_FM_AN_MIN_RATE);
    if (decim < 1)
        decim = 1;
    rate          = sample_rate / decim;
    res->mpx_rate = rate;

    /* Power of the capture; the samples apart, brought to the station. */
    xr = malloc(count * sizeof(*xr));
    xi = malloc(count * sizeof(*xi));
    if (!xr || !xi)
        goto out;
    {
        const double complex step = cexp(-I * 2.0 * M_PI * cfg->offset_hz / sample_rate);
        double complex rot = 1.0;
        double p = 0.0;

        for (i = 0; i < count; i++) {
            float complex v = x[i];

            p += crealf(v) * crealf(v) + cimagf(v) * cimagf(v);
            if (cfg->offset_hz != 0.0) {
                v   *= (float complex)rot;
                rot *= step;
                if ((i & 1023) == 1023)
                    rot /= cabs(rot);
            }
            xr[i] = crealf(v);
            xi[i] = cimagf(v);
        }
        res->power_dbfs = m2sdr_power_db(p / (double)count);
    }

    /* Channel filter and decimation. Without room for its transition band (I/Q rates up to
     * 280 kHz) the capture is the channel. */
    {
        double stop_hz = rate - CHAN_PASS_HZ, e = 0.0;

        if (stop_hz > CHAN_STOP_HZ)
            stop_hz = CHAN_STOP_HZ;
        if (stop_hz > CHAN_PASS_HZ + 10000.0) {
            chan = lowpass_design(sample_rate, CHAN_PASS_HZ, stop_hz, &chan_count);
            if (!chan)
                goto out;
            for (i = 0; i < chan_count; i++)
                e += (double)chan[i] * chan[i];
            res->noise_bw_hz = sample_rate * e;
        } else {
            res->noise_bw_hz = sample_rate;
        }
    }
    if (count < chan_count + 4 * (size_t)decim)
        goto out;
    ny = (count - chan_count) / decim + 1;
    y  = malloc(ny * sizeof(*y));
    if (!y)
        goto out;
    for (i = 0; i < ny; i++) {
        const size_t at = i * decim;
        double p;

        if (chan)
            y[i] = dot(chan, xr + at, chan_count) + I * dot(chan, xi + at, chan_count);
        else
            y[i] = xr[at] + I * xi[at];
        p   = creal(y[i]) * creal(y[i]) + cimag(y[i]) * cimag(y[i]);
        m2 += p;
        m4 += p * p;
    }
    free(xr);
    free(xi);
    xr = xi = NULL;

    /* Carrier and noise from the moments: a constant envelope C plus Gaussian noise N gives
     * M2 = C + N and M4 = C^2 + 4 C N + 2 N^2. */
    m2 /= (double)ny;
    m4 /= (double)ny;
    c   = 2.0 * m2 * m2 - m4;
    c   = c > 0.0 ? sqrt(c) : 0.0;
    if (c > m2)
        c = m2;
    nz  = m2 - c;
    if (nz < 1e-9 * m2)
        nz = 1e-9 * m2;
    res->carrier_dbfs = m2sdr_power_db(c);
    res->cnr_db       = (c > 0.0) ? 10.0 * log10(c / nz) : -200.0;
    res->cn0_dbhz     = res->cnr_db + 10.0 * log10(res->noise_bw_hz);
    res->found        = res->cnr_db >= MIN_CNR_DB;

    /* Discriminator, in Hz, and its compensation. */
    nd   = ny - 1;
    disc = malloc(nd * sizeof(*disc));
    if (!disc || nd < 3)
        goto out;
    for (i = 0; i < nd; i++) {
        const double complex d = y[i + 1] * conj(y[i]);

        disc[i] = (float)(atan2(cimag(d), creal(d)) * rate / (2.0 * M_PI));
    }
    free(y);
    y = NULL;
    droop_init(&droop, rate);
    nm  = nd - 2;
    mpx = malloc(nm * sizeof(*mpx));
    if (!mpx)
        goto out;
    {
        const float k = (float)droop.k;

        for (i = 0; i < nm; i++)
            mpx[i] = disc[i + 1] + k * (disc[i + 2] - 2.0f * disc[i + 1] + disc[i]);
    }
    free(disc);
    disc = NULL;

    /* Carrier offset: the mean frequency. The Hann window keeps the low audio out of it. */
    mean = 0.0;
    wsum = 0.0;
    for (i = 0; i < nm; i++) {
        const double w = 0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / nm);

        mean += w * mpx[i];
        wsum += w;
    }
    mean /= wsum;
    res->carrier_offset_hz = cfg->offset_hz + mean;
    res->duration_s        = (double)nm / rate;
    if (!res->found || fabs(mean) > MAX_OFFSET_HZ) {
        res->found = false;
        ret = 0;
        goto out;
    }
    for (i = 0; i < nm; i++)
        mpx[i] -= (float)mean;

    /* Spectrum of the multiplex, and its trace. */
    zc = malloc(nm * sizeof(*zc));
    if (!zc)
        goto out;
    for (i = 0; i < nm; i++)
        zc[i] = mpx[i];
    if (m2sdr_psd_compute(zc, nm, rate, PSD_LEN, M2SDR_WINDOW_FLATTOP, &psd) != 0)
        goto out;
    free(zc);
    zc  = NULL;
    ref = M2SDR_FM_BS412_REF_HZ * M2SDR_FM_BS412_REF_HZ / 2.0;
    res->mpx_rbw_hz = psd.rbw_hz;
    for (i = 0; i < M2SDR_FM_MPX_BINS; i++) {
        const double lo = ((double)i - 0.5) * M2SDR_FM_MPX_BIN_HZ;
        const double hi = ((double)i + 0.5) * M2SDR_FM_MPX_BIN_HZ;
        double best = 0.0;
        size_t k;

        for (k = psd.bins / 2; k < psd.bins; k++) {
            const double f = m2sdr_psd_freq(&psd, k);

            if (f >= lo && f < hi && psd.power[k] > best)
                best = psd.power[k];
        }
        res->mpx_db[i] = (float)m2sdr_power_db(2.0 * best / ref);
    }

    /* Multiplex low-passed at 60 kHz: the deviation and the power, without the receiver noise
     * above. */
    lp_taps = lowpass_design(rate, MPX_PASS_HZ, MPX_STOP_HZ, &lp_count);
    if (!lp_taps || nm < lp_count + 16)
        goto out;
    nl = nm - lp_count + 1;
    lp = malloc(nl * sizeof(*lp));
    fine = calloc(FINE_BINS + 1, sizeof(*fine));
    if (!lp || !fine)
        goto out;
    ms   = 0.0;
    peak = 0.0;
    for (i = 0; i < nl; i++) {
        const float v = dot(lp_taps, mpx + i, lp_count);
        const double a = fabs(v);
        const size_t k = (size_t)(a / FINE_BIN_HZ);

        lp[i] = v;
        ms   += (double)v * v;
        if (a > peak)
            peak = a;
        fine[k < FINE_BINS ? k : FINE_BINS]++;
    }
    res->mpx_mean_square_hz2 = ms / (double)nl;
    res->mpx_power_dbr       = 10.0 * log10(res->mpx_mean_square_hz2 / ref + 1e-30);
    res->peak_dev_hz         = peak;
    res->dev_p99_hz          = percentile(fine, nl, 0.01);
    res->dev_p999_hz         = percentile(fine, nl, 0.001);
    for (i = 0; i <= FINE_BINS; i++) {
        size_t b = (size_t)(i * FINE_BIN_HZ / M2SDR_FM_DEV_BIN_HZ);

        if (b >= M2SDR_FM_DEV_BINS)
            b = M2SDR_FM_DEV_BINS - 1;
        res->dev_hist[b] += (float)((double)fine[i] / (double)nl);
    }

    /* Pilot and stereo. */
    if (pilot_measure(lp, nl, rate, &pilot) != 0)
        goto out;
    res->pilot_dev_hz        = pilot.amp / droop_response(&droop, M2SDR_FM_PILOT_HZ);
    res->pilot_freq_hz       = pilot.omega * rate / (2.0 * M_PI);
    res->pilot_freq_error_hz = res->pilot_freq_hz - M2SDR_FM_PILOT_HZ;
    res->pilot_coherence     = pilot.coherence;
    res->pilot  = res->pilot_dev_hz >= PILOT_MIN_HZ && pilot.coherence >= PILOT_MIN_COHERENCE;
    res->stereo = res->pilot;
    res->mono_rms_hz   = sqrt(band_power(&psd, MONO_LO_HZ, MONO_HI_HZ));
    res->stereo_rms_hz = sqrt(2.0 * band_power(&psd, STEREO_LO_HZ, STEREO_HI_HZ));
    res->stereo_ratio_db = 20.0 * log10((res->stereo_rms_hz + 1e-9) / (res->mono_rms_hz + 1e-9));
    if (res->pilot && cfg->tone_hz > 0.0) {
        double complex m, s;

        tone_measure(lp, nl, 2.0 * M_PI * cfg->tone_hz / rate, &pilot, &m, &s);
        m /= droop_response(&droop, cfg->tone_hz);
        s /= 0.5 * (droop_response(&droop, 2.0 * M2SDR_FM_PILOT_HZ - cfg->tone_hz) +
                    droop_response(&droop, 2.0 * M2SDR_FM_PILOT_HZ + cfg->tone_hz));
        res->tone           = true;
        res->tone_mono_hz   = cabs(m);
        res->tone_stereo_hz = cabs(s);
        res->tone_left_hz   = cabs(m + s);
        res->tone_right_hz  = cabs(m - s);
        res->separation_db  = 20.0 * log10((fmax(res->tone_left_hz, res->tone_right_hz) + 1e-9) /
                                           (fmin(res->tone_left_hz, res->tone_right_hz) + 1e-9));
    }

    /* Audio SNR from the noise between the audio and the pilot: the mean of its density over
     * f^2 there (the band holds no tone, the mean has less variance than the median). */
    {
        const double fc = 0.5 * (GUARD_LO_HZ + GUARD_HI_HZ);
        double sum = 0.0, d, noise;
        size_t k, bins = 0;

        for (k = psd.bins / 2; k < psd.bins; k++) {
            const double f = m2sdr_psd_freq(&psd, k);

            if (f >= GUARD_LO_HZ && f <= GUARD_HI_HZ) {
                sum += psd.power[k] / (f * f * pow(droop_response(&droop, f), 2.0));
                bins++;
            }
        }
        d     = bins ? 2.0 * sum / (double)bins / psd.rbw_hz * fc * fc : 0.0;
        noise = d / (fc * fc) * deemph_f2_integral(cfg->tau, MONO_LO_HZ, MONO_HI_HZ);

        res->guard_noise_dbr_hz = m2sdr_power_db(d / ref);
        res->audio_snr_db       = m2sdr_power_db(SNR_REF_HZ * SNR_REF_HZ / 2.0 / (noise + 1e-30));
    }

    /* RDS: level from its band less the noise next to it (scaled by f^2), then the decoder. */
    {
        const double fn = 0.5 * (RDS_NOISE_LO_HZ + RDS_NOISE_HI_HZ);
        const double f0 = 0.5 * (RDS_LO_HZ + RDS_HI_HZ);
        double p = band_power(&psd, RDS_LO_HZ, RDS_HI_HZ);
        struct m2sdr_fm_rds_info after;

        p -= noise_density(&psd, RDS_NOISE_LO_HZ, RDS_NOISE_HI_HZ) * (f0 * f0) / (fn * fn) *
             (RDS_HI_HZ - RDS_LO_HZ);
        res->rds_dev_hz = (p > 0.0 ? 2.0 * sqrt(p) : 0.0) / droop_response(&droop, M2SDR_FM_RDS_HZ);

        if (cfg->rds) {
            rds = cfg->rds;
            m2sdr_fm_rds_rx_restart(rds);
        } else {
            rds = m2sdr_fm_rds_rx_new(rate);
            if (!rds)
                goto out;
        }
        m2sdr_fm_rds_rx_info(rds, &before);
        m2sdr_fm_rds_rx_points(rds, (float *)res->points, M2SDR_FM_MAX_POINTS);
        m2sdr_fm_rds_rx_process(rds, lp, nl);
        res->npoints = (int)m2sdr_fm_rds_rx_point_count(rds);
        m2sdr_fm_rds_rx_points(rds, NULL, 0);
        if (res->npoints > 0) {
            double a = 0.0, e = 0.0;
            int k;

            for (k = 0; k < res->npoints; k++)
                a += fabsf(crealf(res->points[k]));
            a /= res->npoints;
            for (k = 0; k < res->npoints; k++) {
                const float complex q = res->points[k] / (float)a;
                const float re = fabsf(crealf(q)) - 1.0f, im = cimagf(q);

                res->points[k] = q;
                e += re * re + im * im;
            }
            res->rds_mer_db = m2sdr_power_db(res->npoints / (e + 1e-30));
        }
        m2sdr_fm_rds_rx_info(rds, &after);
        res->rds_blocks       = after.blocks - before.blocks;
        res->rds_block_errors = after.block_errors - before.block_errors;
        res->rds_groups       = after.groups - before.groups;
        for (i = 0; i < 32; i++)
            res->rds_type_groups[i] = after.type_groups[i] - before.type_groups[i];
        res->rds_bler = res->rds_blocks ? (double)res->rds_block_errors / (double)res->rds_blocks : 0.0;
        res->rds      = res->rds_groups > 0;
        res->rds_info = after;
    }
    ret = 0;

out:
    if (rds && rds != cfg->rds)
        m2sdr_fm_rds_rx_free(rds);
    m2sdr_psd_free(&psd);
    free(xr);
    free(xi);
    free(chan);
    free(y);
    free(disc);
    free(mpx);
    free(zc);
    free(lp_taps);
    free(lp);
    free(fine);
    return ret;
}
