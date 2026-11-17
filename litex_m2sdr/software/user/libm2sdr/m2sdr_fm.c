/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR broadcast FM: modulator and demodulator between audio and I/Q samples.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_fm.h"

/* Rates */
/*-------*/

/* The multiplex and the audio are processed at an integer fraction of the I/Q rate, so the
 * long filters run at a few hundred kHz whatever the I/Q rate is. */

#define FM_TX_MPX_RATE_MIN   160000.0  /* Modulator: lowest multiplex rate when interpolating. */
#define FM_RX_MPX_RATE_MIN   240000.0  /* Demodulator: lowest rate at the discriminator. */
#define FM_RX_AUDIO_RATE_MIN  48000.0  /* Demodulator: lowest audio rate when decimating. */

#define FM_MPX_EDGE_HZ        60000.0  /* Upper edge of the multiplex kept by the interpolator. */
#define FM_AUDIO_STOP_HZ      19000.0  /* The audio low-pass rejects from the pilot up. */
#define FM_CHANNEL_PASS_HZ   100000.0  /* Channel filter of the demodulator. */
#define FM_CHANNEL_STOP_HZ   150000.0

#define FM_STOPBAND_DB           60.0

#define FM_SINE_BITS               14  /* Sine table of the modulator: 2^14 points. */

/* FIR filters */
/*-------------*/

static double fm_bessel_i0(double x)
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

/* Low-pass with a Kaiser window, unity gain at DC. The length is rounded up to a multiple of
 * `multiple`, then to an odd number when `multiple` is 1. */
static float *fm_lowpass_design(double rate, double pass_hz, double stop_hz, unsigned multiple,
                                unsigned *count)
{
    const double beta   = 0.1102 * (FM_STOPBAND_DB - 8.7);
    const double width  = 2.0 * M_PI * (stop_hz - pass_hz) / rate;
    const double cutoff = (pass_hz + stop_hz) / rate;      /* In cycles per sample, times two. */
    unsigned n = (unsigned)ceil((FM_STOPBAND_DB - 7.95) / (2.285 * width)) + 1;
    double sum = 0.0;
    float *taps;
    unsigned i;

    if (multiple > 1)
        n = ((n + multiple - 1) / multiple) * multiple;
    else
        n |= 1;
    taps = malloc(n * sizeof(*taps));
    if (!taps)
        return NULL;
    for (i = 0; i < n; i++) {
        const double t = i - (n - 1) / 2.0;
        const double r = 2.0 * t / (n - 1);
        const double w = fm_bessel_i0(beta * sqrt(1.0 - r * r)) / fm_bessel_i0(beta);
        const double h = (t == 0.0) ? cutoff : sin(M_PI * cutoff * t) / (M_PI * t);

        taps[i] = (float)(h * w);
        sum    += h * w;
    }
    for (i = 0; i < n; i++)
        taps[i] = (float)(taps[i] / sum);
    *count = n;
    return taps;
}

/* Sampled FM and the discriminator */
/*-----------------------------------*/

/* The phase step between two samples of an FM signal is the modulating signal averaged over
 * one sample period: its response is sin(x) / x with x = pi * f / rate, which is 0.3 dB down at
 * the L-R subcarrier for 250 kSPS. The modulator applies this droop to its phase steps and
 * the demodulator removes it from the discriminator output, each with three taps
 * (k, 1 - 2 * k, k) fitted at the L-R subcarrier in stereo, and in the audio band in mono. */
static double fm_droop(double rate, bool stereo)
{
    const double x = M_PI * (stereo ? 2.0 * M2SDR_FM_PILOT_HZ : 10000.0) / rate;

    return sin(x) / x;
}

static double fm_droop_tap(double rate, bool stereo, double gain)
{
    const double x = M_PI * (stereo ? 2.0 * M2SDR_FM_PILOT_HZ : 10000.0) / rate;

    return (1.0 - gain) / (2.0 * (1.0 - cos(2.0 * x)));
}

/* Four partial sums: the compiler turns them into one vector. */
static inline float fm_dot(const float *a, const float *b, unsigned n)
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

/* History of the last n samples. Every sample is written twice, n apart, so the window
 * hist[pos] .. hist[pos + n - 1] is contiguous, oldest sample first. */
struct fm_delay {
    unsigned n;
    unsigned pos;
    float   *hist;
};

static int fm_delay_init(struct fm_delay *d, unsigned n)
{
    d->n    = n;
    d->pos  = 0;
    d->hist = calloc(2 * (size_t)n, sizeof(*d->hist));
    return d->hist ? 0 : -1;
}

static inline void fm_delay_push(struct fm_delay *d, float x)
{
    d->hist[d->pos]        = x;
    d->hist[d->pos + d->n] = x;
    if (++d->pos == d->n)
        d->pos = 0;
}

static inline const float *fm_delay_window(const struct fm_delay *d)
{
    return d->hist + d->pos;
}

/* Modulator */
/*-----------*/

struct m2sdr_fm_tx {
    unsigned channels;
    unsigned interp;             /* I/Q samples per multiplex sample. */
    double   audio_rate;

    /* Pre-emphasis: y = (x - a * x[-1]) / (1 - a), the inverse of the de-emphasis. */
    float    pre_a, pre_gain;
    float    pre_x1[2];
    uint64_t clipped;

    /* Audio low-pass. */
    float   *lpf_taps;
    unsigned lpf_count;
    struct fm_delay lpf[2];

    /* Pilot: a unit phasor turned by a fixed step. */
    double   pilot_c, pilot_s;
    double   step_c, step_s;

    /* Interpolator: one set of taps per output phase, in the order of the window. */
    float   *interp_taps;
    unsigned interp_count;       /* Taps per phase. */
    struct fm_delay mpx;

    /* Frequency modulator. */
    uint32_t phase;
    double   phase_gain;         /* Multiplex value to phase step, 2^32 per turn. */
    float    droop_tap;          /* Response of a phase step, see fm_droop(). */
    float    mod_x1, mod_x2;
    int16_t *sine;
};

void m2sdr_fm_config_init(struct m2sdr_fm_config *cfg)
{
    cfg->sample_rate = 1e6;
    cfg->deviation   = M2SDR_FM_DEVIATION_HZ;
    cfg->tau         = M2SDR_FM_TAU_EU;
    cfg->stereo      = false;
    cfg->bits        = 12;
}

static bool fm_config_valid(const struct m2sdr_fm_config *cfg)
{
    const double min_rate = cfg->stereo ? M2SDR_FM_MIN_RATE_STEREO : M2SDR_FM_MIN_RATE_MONO;

    return cfg->sample_rate >= min_rate && cfg->sample_rate <= 1e9 &&
           cfg->deviation > 0.0 && cfg->deviation <= 0.4 * cfg->sample_rate &&
           cfg->tau >= 0.0 && cfg->tau < 1e-3;
}

void m2sdr_fm_tx_free(struct m2sdr_fm_tx *tx)
{
    if (!tx)
        return;
    free(tx->lpf_taps);
    free(tx->lpf[0].hist);
    free(tx->lpf[1].hist);
    free(tx->interp_taps);
    free(tx->mpx.hist);
    free(tx->sine);
    free(tx);
}

struct m2sdr_fm_tx *m2sdr_fm_tx_new(const struct m2sdr_fm_config *cfg)
{
    struct m2sdr_fm_tx *tx;
    double amplitude;
    unsigned i, ch;

    if (!cfg || !fm_config_valid(cfg) || cfg->bits < 2 || cfg->bits > 16)
        return NULL;
    tx = calloc(1, sizeof(*tx));
    if (!tx)
        return NULL;
    tx->channels   = cfg->stereo ? 2 : 1;
    tx->interp     = (unsigned)(cfg->sample_rate / FM_TX_MPX_RATE_MIN);
    if (tx->interp < 1)
        tx->interp = 1;
    tx->audio_rate = cfg->sample_rate / tx->interp;

    /* Pre-emphasis. */
    tx->pre_a    = (cfg->tau > 0.0) ? (float)exp(-1.0 / (cfg->tau * tx->audio_rate)) : 0.0f;
    tx->pre_gain = 1.0f / (1.0f - tx->pre_a);

    /* Audio low-pass: keeps the audio out of the pilot and of the L-R subcarrier. */
    tx->lpf_taps = fm_lowpass_design(tx->audio_rate, M2SDR_FM_AUDIO_HZ, FM_AUDIO_STOP_HZ, 1,
                                     &tx->lpf_count);
    if (!tx->lpf_taps)
        goto fail;
    for (ch = 0; ch < tx->channels; ch++)
        if (fm_delay_init(&tx->lpf[ch], tx->lpf_count) != 0)
            goto fail;

    /* Pilot. */
    tx->pilot_c = 1.0;
    tx->pilot_s = 0.0;
    tx->step_c  = cos(2.0 * M_PI * M2SDR_FM_PILOT_HZ / tx->audio_rate);
    tx->step_s  = sin(2.0 * M_PI * M2SDR_FM_PILOT_HZ / tx->audio_rate);

    /* Interpolator: removes the images of the multiplex around the multiples of its rate. */
    if (tx->interp > 1) {
        unsigned count, m, k;
        float *proto = fm_lowpass_design(cfg->sample_rate, FM_MPX_EDGE_HZ,
                                         tx->audio_rate - FM_MPX_EDGE_HZ, tx->interp, &count);

        if (!proto)
            goto fail;
        tx->interp_count = count / tx->interp;
        tx->interp_taps  = malloc(count * sizeof(*tx->interp_taps));
        if (!tx->interp_taps || fm_delay_init(&tx->mpx, tx->interp_count) != 0) {
            free(proto);
            goto fail;
        }
        for (m = 0; m < tx->interp; m++)
            for (k = 0; k < tx->interp_count; k++)
                tx->interp_taps[m * tx->interp_count + k] =
                    tx->interp * proto[m + (tx->interp_count - 1 - k) * tx->interp];
        free(proto);
    }

    /* Frequency modulator. */
    tx->phase_gain = cfg->deviation / cfg->sample_rate * 4294967296.0;
    tx->droop_tap  = (float)fm_droop_tap(cfg->sample_rate, cfg->stereo,
                                         fm_droop(cfg->sample_rate, cfg->stereo));
    tx->sine       = malloc((1u << FM_SINE_BITS) * sizeof(*tx->sine));
    if (!tx->sine)
        goto fail;
    amplitude = (double)((1u << (cfg->bits - 1)) - 1);
    for (i = 0; i < (1u << FM_SINE_BITS); i++)
        tx->sine[i] = (int16_t)lrint(amplitude * sin(2.0 * M_PI * i / (1u << FM_SINE_BITS)));
    return tx;

fail:
    m2sdr_fm_tx_free(tx);
    return NULL;
}

double m2sdr_fm_tx_audio_rate(const struct m2sdr_fm_tx *tx)
{
    return tx->audio_rate;
}

unsigned m2sdr_fm_tx_interpolation(const struct m2sdr_fm_tx *tx)
{
    return tx->interp;
}

uint64_t m2sdr_fm_tx_clipped(const struct m2sdr_fm_tx *tx)
{
    return tx->clipped;
}

/* Pre-emphasis, limiter and low-pass of one audio sample. The low-pass turns limited audio
 * into overshoots, up to 27 % for a square wave: a second limiter keeps the deviation, and
 * leaves out of the audio band only what it removes from these overshoots. */
static inline float fm_tx_audio(struct m2sdr_fm_tx *tx, unsigned ch, float x)
{
    float y = (x - tx->pre_a * tx->pre_x1[ch]) * tx->pre_gain;

    tx->pre_x1[ch] = x;
    if (y > 1.0f) {
        y = 1.0f;
        tx->clipped++;
    } else if (y < -1.0f) {
        y = -1.0f;
        tx->clipped++;
    } else if (isnan(y)) {
        y = 0.0f;
    }
    fm_delay_push(&tx->lpf[ch], y);
    y = fm_dot(tx->lpf_taps, fm_delay_window(&tx->lpf[ch]), tx->lpf_count);
    if (y > 1.0f)
        y = 1.0f;
    else if (y < -1.0f)
        y = -1.0f;
    return y;
}

/* One I/Q sample for one multiplex value: the phase advances by the instantaneous frequency. */
static inline void fm_tx_modulate(struct m2sdr_fm_tx *tx, float mpx, int16_t *iq)
{
    const uint32_t mask = (1u << FM_SINE_BITS) - 1;
    const float step = tx->mod_x1 + tx->droop_tap * (mpx - 2.0f * tx->mod_x1 + tx->mod_x2);
    uint32_t index;

    tx->mod_x2 = tx->mod_x1;
    tx->mod_x1 = mpx;
    tx->phase += (uint32_t)(int64_t)llrint((double)step * tx->phase_gain);
    index = (tx->phase + (1u << (31 - FM_SINE_BITS))) >> (32 - FM_SINE_BITS);
    iq[0] = tx->sine[(index + (1u << (FM_SINE_BITS - 2))) & mask];
    iq[1] = tx->sine[index & mask];
}

size_t m2sdr_fm_tx_process(struct m2sdr_fm_tx *tx, const float *audio, size_t frames, int16_t *iq)
{
    const float audio_level = 1.0f - (float)M2SDR_FM_PILOT_LEVEL;
    size_t n;
    unsigned m;

    for (n = 0; n < frames; n++) {
        float mpx;

        if (tx->channels == 2) {
            const float l = fm_tx_audio(tx, 0, audio[2 * n + 0]);
            const float r = fm_tx_audio(tx, 1, audio[2 * n + 1]);
            const double c = tx->pilot_c, s = tx->pilot_s;
            const double norm = 1.5 - 0.5 * (c * c + s * s);

            /* The L-R subcarrier is sin(2 * phase) for a pilot sin(phase): both cross zero
             * upwards together. */
            mpx = audio_level * (0.5f * (l + r) + 0.5f * (l - r) * (float)(2.0 * s * c)) +
                  (float)M2SDR_FM_PILOT_LEVEL * (float)s;
            tx->pilot_c = (c * tx->step_c - s * tx->step_s) * norm;
            tx->pilot_s = (s * tx->step_c + c * tx->step_s) * norm;
        } else {
            mpx = fm_tx_audio(tx, 0, audio[n]);
        }

        if (tx->interp == 1) {
            fm_tx_modulate(tx, mpx, iq);
            iq += 2;
        } else {
            const float *taps = tx->interp_taps;

            fm_delay_push(&tx->mpx, mpx);
            for (m = 0; m < tx->interp; m++) {
                fm_tx_modulate(tx, fm_dot(taps, fm_delay_window(&tx->mpx), tx->interp_count), iq);
                taps += tx->interp_count;
                iq   += 2;
            }
        }
    }
    return frames * tx->interp;
}

/* Demodulator */
/*-------------*/

#define FM_PLL_ACQ_HZ       100.0  /* Natural frequency of the pilot PLL while it acquires. */
#define FM_PLL_TRACK_HZ      15.0  /* The same once locked. */
#define FM_PLL_FILTER_HZ    600.0  /* Low-pass of the phase detector: the programme is 4 kHz away. */
#define FM_PILOT_DETECT_HZ    8.0  /* Low-pass of the pilot level. */
#define FM_PILOT_LOCK        0.03  /* Pilot level that enables the stereo decoder... */
#define FM_PILOT_UNLOCK      0.02  /* ...and the one that disables it. */
#define FM_DC_BLOCK_HZ        5.0  /* High-pass of the audio: a tuning offset is a DC level. */

struct m2sdr_fm_rx {
    unsigned channels;
    unsigned decim;              /* I/Q samples per multiplex sample. */
    unsigned adecim;             /* Multiplex samples per audio frame. */
    double   mpx_rate;

    /* Channel filter. */
    float   *chan_taps;
    unsigned chan_count;
    struct fm_delay chan_i, chan_q;
    unsigned chan_phase;

    /* Discriminator. */
    float    prev_i, prev_q;
    float    demod_gain;
    float    droop_tap;          /* Inverse of the response of a phase step, see fm_droop(). */
    float    demod_x1, demod_x2;

    /* Pilot PLL. */
    double   pll_phase;          /* In radians. */
    double   pll_freq;           /* Offset from the nominal step, in radians per sample. */
    double   pll_step;
    double   pll_kp[2], pll_ki[2];   /* Acquisition, tracking. */
    float    pll_err[2];         /* Two one-pole low-passes of the phase detector. */
    float    pll_alpha;
    float    pilot_level;        /* Half the pilot amplitude, in parts of the deviation. */
    float    pilot_alpha;
    bool     locked;

    /* De-emphasis of L+R and L-R. */
    float    de_a;
    float    de_m, de_s;

    /* Audio low-pass and decimation. */
    float   *lpf_taps;
    unsigned lpf_count;
    struct fm_delay lpf_m, lpf_s;
    unsigned lpf_phase;

    /* DC block of L+R, and of L-R to keep them alike. */
    float    dc_r, dc_x1[2], dc_y1[2];
};

void m2sdr_fm_rx_free(struct m2sdr_fm_rx *rx)
{
    if (!rx)
        return;
    free(rx->chan_taps);
    free(rx->chan_i.hist);
    free(rx->chan_q.hist);
    free(rx->lpf_taps);
    free(rx->lpf_m.hist);
    free(rx->lpf_s.hist);
    free(rx);
}

struct m2sdr_fm_rx *m2sdr_fm_rx_new(const struct m2sdr_fm_config *cfg)
{
    /* Gain of the phase detector for the nominal pilot: half its amplitude. */
    const double kd = 0.5 * M2SDR_FM_PILOT_LEVEL;
    static const double pll_hz[2] = { FM_PLL_ACQ_HZ, FM_PLL_TRACK_HZ };
    struct m2sdr_fm_rx *rx;
    double audio_rate;
    unsigned i;

    if (!cfg || !fm_config_valid(cfg))
        return NULL;
    rx = calloc(1, sizeof(*rx));
    if (!rx)
        return NULL;
    rx->channels = cfg->stereo ? 2 : 1;
    rx->decim    = (unsigned)(cfg->sample_rate / FM_RX_MPX_RATE_MIN);
    if (rx->decim < 1)
        rx->decim = 1;
    rx->mpx_rate = cfg->sample_rate / rx->decim;
    rx->adecim   = (unsigned)(rx->mpx_rate / FM_RX_AUDIO_RATE_MIN);
    if (rx->adecim < 1)
        rx->adecim = 1;
    audio_rate   = rx->mpx_rate / rx->adecim;

    /* Channel filter: selects the station and decimates. Skipped when the I/Q rate leaves no
     * room for its transition band. */
    if (rx->mpx_rate >= FM_RX_MPX_RATE_MIN) {
        double stop_hz = rx->mpx_rate - FM_CHANNEL_PASS_HZ;

        if (stop_hz > FM_CHANNEL_STOP_HZ)
            stop_hz = FM_CHANNEL_STOP_HZ;
        rx->chan_taps = fm_lowpass_design(cfg->sample_rate, FM_CHANNEL_PASS_HZ, stop_hz, 1,
                                          &rx->chan_count);
        if (!rx->chan_taps || fm_delay_init(&rx->chan_i, rx->chan_count) != 0 ||
            fm_delay_init(&rx->chan_q, rx->chan_count) != 0)
            goto fail;
    }

    /* Discriminator: phase step per sample to parts of the deviation. */
    rx->demod_gain = (float)(rx->mpx_rate / (2.0 * M_PI * cfg->deviation));
    rx->droop_tap  = (float)fm_droop_tap(rx->mpx_rate, cfg->stereo,
                                         1.0 / fm_droop(rx->mpx_rate, cfg->stereo));

    /* Pilot PLL, second order, damping 0.707. */
    rx->pll_step = 2.0 * M_PI * M2SDR_FM_PILOT_HZ / rx->mpx_rate;
    for (i = 0; i < 2; i++) {
        const double wn = 2.0 * M_PI * pll_hz[i] / rx->mpx_rate;

        rx->pll_kp[i] = 2.0 * M_SQRT1_2 * wn / kd;
        rx->pll_ki[i] = wn * wn / kd;
    }
    rx->pll_alpha   = (float)(1.0 - exp(-2.0 * M_PI * FM_PLL_FILTER_HZ / rx->mpx_rate));
    rx->pilot_alpha = (float)(1.0 - exp(-2.0 * M_PI * FM_PILOT_DETECT_HZ / rx->mpx_rate));

    /* De-emphasis: y = (1 - a) * x + a * y[-1]. */
    rx->de_a = (cfg->tau > 0.0) ? (float)exp(-1.0 / (cfg->tau * rx->mpx_rate)) : 0.0f;

    /* Audio low-pass: rejects the pilot and what is above, and decimates. */
    rx->lpf_taps = fm_lowpass_design(rx->mpx_rate, M2SDR_FM_AUDIO_HZ, FM_AUDIO_STOP_HZ, 1,
                                     &rx->lpf_count);
    if (!rx->lpf_taps || fm_delay_init(&rx->lpf_m, rx->lpf_count) != 0 ||
        (cfg->stereo && fm_delay_init(&rx->lpf_s, rx->lpf_count) != 0))
        goto fail;

    rx->dc_r = (float)exp(-2.0 * M_PI * FM_DC_BLOCK_HZ / audio_rate);
    return rx;

fail:
    m2sdr_fm_rx_free(rx);
    return NULL;
}

double m2sdr_fm_rx_audio_rate(const struct m2sdr_fm_rx *rx)
{
    return rx->mpx_rate / rx->adecim;
}

unsigned m2sdr_fm_rx_decimation(const struct m2sdr_fm_rx *rx)
{
    return rx->decim * rx->adecim;
}

bool m2sdr_fm_rx_pilot_locked(const struct m2sdr_fm_rx *rx)
{
    return rx->locked;
}

/* Pilot PLL on one multiplex sample. Returns sin(2 * phase), the L-R subcarrier. */
static inline float fm_rx_pilot(struct m2sdr_fm_rx *rx, float mpx)
{
    const float s = sinf((float)rx->pll_phase);
    const float c = cosf((float)rx->pll_phase);
    const int   k = rx->locked ? 1 : 0;
    float err;

    /* Phase detector: the pilot is sin(phase) when locked, so its product with cos(phase)
     * averages to zero. The low-passes remove the programme around the pilot. */
    rx->pll_err[0] += rx->pll_alpha * (mpx * c - rx->pll_err[0]);
    rx->pll_err[1] += rx->pll_alpha * (rx->pll_err[0] - rx->pll_err[1]);
    err = rx->pll_err[1];

    rx->pll_freq  += rx->pll_ki[k] * err;
    rx->pll_phase += rx->pll_step + rx->pll_freq + rx->pll_kp[k] * err;
    if (rx->pll_phase > M_PI)
        rx->pll_phase -= 2.0 * M_PI;

    /* Pilot level: its product with sin(phase) averages to half its amplitude. */
    rx->pilot_level += rx->pilot_alpha * (mpx * s - rx->pilot_level);
    if (rx->locked) {
        if (2.0f * rx->pilot_level < (float)FM_PILOT_UNLOCK)
            rx->locked = false;
    } else if (2.0f * rx->pilot_level > (float)FM_PILOT_LOCK) {
        rx->locked = true;
    }
    return 2.0f * s * c;
}

/* One multiplex sample from the discriminator. Returns 1 when an audio frame was written. */
static inline int fm_rx_mpx(struct m2sdr_fm_rx *rx, float mpx, float *audio)
{
    float m, s, y;

    /* L+R, and L-R brought back from its subcarrier, with their de-emphasis. */
    rx->de_m += (1.0f - rx->de_a) * (mpx - rx->de_m);
    fm_delay_push(&rx->lpf_m, rx->de_m);
    if (rx->channels == 2) {
        const float sub = fm_rx_pilot(rx, mpx);

        rx->de_s += (1.0f - rx->de_a) * (2.0f * mpx * sub - rx->de_s);
        fm_delay_push(&rx->lpf_s, rx->de_s);
    }
    if (++rx->lpf_phase < rx->adecim)
        return 0;
    rx->lpf_phase = 0;

    m = fm_dot(rx->lpf_taps, fm_delay_window(&rx->lpf_m), rx->lpf_count);
    y = m - rx->dc_x1[0] + rx->dc_r * rx->dc_y1[0];
    rx->dc_x1[0] = m;
    rx->dc_y1[0] = y;
    if (rx->channels == 1) {
        audio[0] = y;
        return 1;
    }

    /* Without a pilot, both channels carry L+R at its mono level. */
    s = fm_dot(rx->lpf_taps, fm_delay_window(&rx->lpf_s), rx->lpf_count);
    m = s - rx->dc_x1[1] + rx->dc_r * rx->dc_y1[1];
    rx->dc_x1[1] = s;
    rx->dc_y1[1] = m;
    if (rx->locked) {
        const float gain = 1.0f / (1.0f - (float)M2SDR_FM_PILOT_LEVEL);

        audio[0] = (y + m) * gain;
        audio[1] = (y - m) * gain;
    } else {
        audio[0] = y;
        audio[1] = y;
    }
    return 1;
}

size_t m2sdr_fm_rx_process(struct m2sdr_fm_rx *rx, const int16_t *iq, size_t count, float *audio)
{
    size_t n, frames = 0;

    for (n = 0; n < count; n++) {
        float i = iq[2 * n + 0];
        float q = iq[2 * n + 1];
        float re, im, x, mpx;

        if (rx->chan_taps) {
            fm_delay_push(&rx->chan_i, i);
            fm_delay_push(&rx->chan_q, q);
            if (++rx->chan_phase < rx->decim)
                continue;
            rx->chan_phase = 0;
            i = fm_dot(rx->chan_taps, fm_delay_window(&rx->chan_i), rx->chan_count);
            q = fm_dot(rx->chan_taps, fm_delay_window(&rx->chan_q), rx->chan_count);
        }

        /* Discriminator: phase of this sample against the previous one. */
        re = i * rx->prev_i + q * rx->prev_q;
        im = q * rx->prev_i - i * rx->prev_q;
        rx->prev_i = i;
        rx->prev_q = q;
        x   = atan2f(im, re) * rx->demod_gain;
        mpx = rx->demod_x1 + rx->droop_tap * (x - 2.0f * rx->demod_x1 + rx->demod_x2);
        rx->demod_x2 = rx->demod_x1;
        rx->demod_x1 = x;
        frames += (size_t)fm_rx_mpx(rx, mpx, audio + frames * rx->channels);
    }
    return frames;
}
