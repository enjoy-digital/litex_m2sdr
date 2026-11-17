/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the FM modulator and demodulator: tones through the modulator, checked on the I/Q
 * samples with a discriminator of the test, then through the demodulator. RDS: blocks against
 * the tables of EN 50067, the subcarrier on the multiplex, the decoder on a reference signal
 * written from the standard, then encoder to decoder. With --tools, the same round trips
 * through m2sdr_fm_tx and m2sdr_fm_rx, with files and with pipes.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../libm2sdr/m2sdr_fm.h"

static int failures;
static int verbose;      /* -v: print the measurements. */

#define NOTE(...) do { if (verbose) { printf(__VA_ARGS__); printf("\n"); } } while (0)

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static double db(double ratio)
{
    return 20.0 * log10(fabs(ratio) + 1e-30);
}

/* Measurements */
/*--------------*/

/* Tone of a signal, through a Hann window: amplitude, and phase of its sine at the first
 * sample. `stride` is the distance between two samples of the signal in x. */
static double tone(const float *x, size_t n, unsigned stride, double rate, double hz, double *phase)
{
    double complex acc = 0.0;
    double wsum = 0.0;
    size_t i;

    for (i = 0; i < n; i++) {
        const double w = 0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / n);

        acc  += w * x[i * stride] * cexp(-I * 2.0 * M_PI * hz * (double)i / rate);
        wsum += w;
    }
    if (phase)
        *phase = carg(acc) + M_PI / 2.0;
    return 2.0 * cabs(acc) / wsum;
}

/* Signal over noise and distortion of a tone of the given amplitude, in dB. */
static double sinad(const float *x, size_t n, unsigned stride, double amplitude)
{
    double power = 0.0, wsum = 0.0, mean = 0.0, signal, rest;
    size_t i;

    for (i = 0; i < n; i++)
        mean += x[i * stride];
    mean /= n;
    for (i = 0; i < n; i++) {
        const double w = 0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / n);

        power += w * w * (x[i * stride] - mean) * (x[i * stride] - mean);
        wsum  += w * w;
    }
    signal = 0.5 * amplitude * amplitude;
    rest   = power / wsum - signal;
    return 10.0 * log10(signal / (rest > 1e-30 ? rest : 1e-30));
}

/* Signals */
/*---------*/

struct audio {
    double l_hz, l_amp;   /* Left channel, or the only one in mono. */
    double r_hz, r_amp;
    double dc;            /* Added to both channels. */
};

struct channel {
    double offset_hz;     /* Tuning offset. */
    double noise;         /* Standard deviation of the noise on I and on Q, in LSB. */
    double gain;          /* 0: unchanged. */
    size_t tx_block;      /* Audio frames per modulator call (0: default). */
    size_t rx_block;      /* I/Q samples per demodulator call (0: default). */
};

static unsigned rng_state = 2463534242u;
static double rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return ((rng_state >> 8) + 0.5) / 16777216.0;
}

/* Modulates `seconds` of tones. Returns the I/Q samples; *count is their number. */
static int16_t *modulate(const struct m2sdr_fm_config *cfg, const struct audio *a, double seconds,
                         size_t tx_block, size_t *count, unsigned long long *clipped)
{
    struct m2sdr_fm_tx *tx = m2sdr_fm_tx_new(cfg);
    const unsigned channels = cfg->stereo ? 2 : 1;
    size_t frames, done = 0, i;
    unsigned interp;
    double rate;
    float *audio;
    int16_t *iq;

    if (!tx)
        return NULL;
    rate   = m2sdr_fm_tx_audio_rate(tx);
    interp = m2sdr_fm_tx_interpolation(tx);
    frames = (size_t)(seconds * rate);
    audio  = malloc(frames * channels * sizeof(*audio));
    iq     = malloc(frames * interp * 2 * sizeof(*iq));
    for (i = 0; i < frames; i++) {
        const double t = (double)i / rate;

        audio[i * channels] = (float)(a->dc + a->l_amp * sin(2.0 * M_PI * a->l_hz * t));
        if (channels == 2)
            audio[i * 2 + 1] = (float)(a->dc + a->r_amp * sin(2.0 * M_PI * a->r_hz * t));
    }
    if (!tx_block)
        tx_block = 1000;
    while (done < frames) {
        const size_t n = (frames - done < tx_block) ? frames - done : tx_block;

        m2sdr_fm_tx_process(tx, audio + done * channels, n, iq + done * interp * 2);
        done += n;
    }
    if (clipped)
        *clipped = m2sdr_fm_tx_clipped(tx);
    *count = frames * interp;
    free(audio);
    m2sdr_fm_tx_free(tx);
    return iq;
}

/* Tuning offset, gain and noise, in place. */
static void impair(int16_t *iq, size_t count, double rate, const struct channel *ch)
{
    const double gain = (ch->gain > 0.0) ? ch->gain : 1.0;
    size_t i;

    if (ch->offset_hz == 0.0 && ch->noise == 0.0 && gain == 1.0)
        return;
    for (i = 0; i < count; i++) {
        const double r = ch->noise * sqrt(-2.0 * log(rnd())), p = 2.0 * M_PI * rnd();
        const double complex z = gain * (iq[2 * i] + I * iq[2 * i + 1]) *
                                 cexp(I * 2.0 * M_PI * ch->offset_hz * (double)i / rate) +
                                 r * cos(p) + I * r * sin(p);

        iq[2 * i + 0] = (int16_t)lrint(creal(z));
        iq[2 * i + 1] = (int16_t)lrint(cimag(z));
    }
}

/* RDS of the last demodulate(). */
static struct m2sdr_fm_rds_info last_rds;

/* Demodulates. Returns the audio; *frames is its length, *rate its rate. */
static float *demodulate(const struct m2sdr_fm_config *cfg, const int16_t *iq, size_t count,
                         size_t rx_block, size_t *frames, double *rate, bool *locked)
{
    struct m2sdr_fm_rx *rx = m2sdr_fm_rx_new(cfg);
    const unsigned channels = cfg->stereo ? 2 : 1;
    size_t done = 0;
    float *audio;

    if (!rx)
        return NULL;
    audio   = malloc((count / m2sdr_fm_rx_decimation(rx) + 16) * channels * sizeof(*audio));
    *frames = 0;
    *rate   = m2sdr_fm_rx_audio_rate(rx);
    if (!rx_block)
        rx_block = 4099;
    while (done < count) {
        const size_t n = (count - done < rx_block) ? count - done : rx_block;

        *frames += m2sdr_fm_rx_process(rx, iq + 2 * done, n, audio + *frames * channels);
        done    += n;
    }
    if (locked)
        *locked = m2sdr_fm_rx_pilot_locked(rx);
    m2sdr_fm_rx_rds(rx, &last_rds);
    m2sdr_fm_rx_free(rx);
    return audio;
}

/* Modulator, channel, demodulator. `rx_cfg` is the configuration of the modulator when NULL. */
static float *roundtrip(const struct m2sdr_fm_config *cfg, const struct m2sdr_fm_config *rx_cfg,
                        const struct audio *a, const struct channel *ch, double seconds,
                        size_t *frames, double *rate, bool *locked)
{
    static const struct channel clean;
    size_t count;
    int16_t *iq;
    float *audio;

    if (!ch)
        ch = &clean;
    iq = modulate(cfg, a, seconds, ch->tx_block, &count, NULL);
    if (!iq)
        return NULL;
    impair(iq, count, cfg->sample_rate, ch);
    audio = demodulate(rx_cfg ? rx_cfg : cfg, iq, count, ch->rx_block, frames, rate, locked);
    free(iq);
    return audio;
}

/* Discriminator of the test: instantaneous frequency of the I/Q samples, in Hz. */
static float *discriminate(const int16_t *iq, size_t count, double rate)
{
    float *f = malloc(count * sizeof(*f));
    size_t i;

    f[0] = 0.0f;
    for (i = 1; i < count; i++) {
        const double complex z = (iq[2 * i] + I * iq[2 * i + 1]) *
                                 (iq[2 * i - 2] - I * iq[2 * i - 1]);

        f[i] = (float)(carg(z) * rate / (2.0 * M_PI));
    }
    return f;
}

/* The settling of the filters and of the pilot PLL is left out of the measurements. */
#define SETTLE_S  0.15
#define MEASURE_S 0.15
#define TOTAL_S   (SETTLE_S + MEASURE_S + 0.01)

/* Modulator */
/*-----------*/

/* Deviation, amplitude of the I/Q samples, pre-emphasis. */
static void test_modulator(void)
{
    static const double taus[]  = { M2SDR_FM_TAU_EU, M2SDR_FM_TAU_US };
    static const double freqs[] = { 400.0, 1000.0, 5000.0, 10000.0, 15000.0 };
    struct m2sdr_fm_config cfg;
    size_t count, skip, n, i, t, k;
    double amp, power = 0.0;
    int16_t *iq;
    float *f;
    int peak = 0;

    /* Full-scale tone, no emphasis: 75 kHz of deviation, 12-bit amplitude. */
    m2sdr_fm_config_init(&cfg);
    cfg.tau = 0.0;
    {
        const struct audio a = { .l_hz = 1000.0, .l_amp = 1.0 };

        iq = modulate(&cfg, &a, TOTAL_S, 0, &count, NULL);
    }
    CHECK(iq != NULL, "modulator");
    if (!iq)
        return;
    skip = (size_t)(SETTLE_S * cfg.sample_rate);
    n    = (size_t)(MEASURE_S * cfg.sample_rate);
    f    = discriminate(iq, count, cfg.sample_rate);
    amp  = tone(f + skip, n, 1, cfg.sample_rate, 1000.0, NULL);
    CHECK(fabs(amp / 75000.0 - 1.0) < 0.002, "deviation %.1f Hz, expected 75000", amp);
    for (i = 0; i < count; i++) {
        if (abs(iq[2 * i]) > peak)     peak = abs(iq[2 * i]);
        if (abs(iq[2 * i + 1]) > peak) peak = abs(iq[2 * i + 1]);
        power += (double)iq[2 * i] * iq[2 * i] + (double)iq[2 * i + 1] * iq[2 * i + 1];
    }
    CHECK(peak == 2047, "peak of the I/Q samples %d, expected 2047", peak);
    CHECK(fabs(sqrt(power / count) / 2047.0 - 1.0) < 0.001, "I/Q amplitude %.2f", sqrt(power / count));
    free(f);
    free(iq);

    /* The other sample widths. */
    for (k = 2; k <= 16; k += 7) {
        const struct audio a = { .l_hz = 1000.0, .l_amp = 0.5 };

        cfg.bits = (unsigned)k;
        iq = modulate(&cfg, &a, 0.01, 0, &count, NULL);
        for (peak = 0, i = 0; i < 2 * count; i++)
            if (abs(iq[i]) > peak) peak = abs(iq[i]);
        CHECK(peak == (1 << (k - 1)) - 1, "%zu bits: peak %d", k, peak);
        free(iq);
    }
    cfg.bits = 12;

    /* Pre-emphasis: 1 + j * 2 * pi * f * tau. */
    for (t = 0; t < 2; t++) {
        cfg.tau = taus[t];
        for (k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
            const struct audio a = { .l_hz = freqs[k], .l_amp = 0.1 };
            const double w = 2.0 * M_PI * freqs[k] * taus[t];
            const double expected = 0.1 * 75000.0 * sqrt(1.0 + w * w);

            iq  = modulate(&cfg, &a, TOTAL_S, 0, &count, NULL);
            f   = discriminate(iq, count, cfg.sample_rate);
            amp = tone(f + skip, n, 1, cfg.sample_rate, freqs[k], NULL);
            CHECK(fabs(db(amp / expected)) < 0.15, "pre-emphasis %.0f us at %.0f Hz: %+.2f dB",
                  taus[t] * 1e6, freqs[k], db(amp / expected));
            NOTE("pre-emphasis %.0f us at %.0f Hz: %+.2f dB", taus[t] * 1e6, freqs[k], db(amp / expected));
            free(f);
            free(iq);
        }
    }
}

/* A step at the first sample, a file that does not start at zero, gives its deviation and
 * nothing else: the pre-emphasis settles. */
static void test_modulator_step(void)
{
    const struct audio a = { .dc = 0.2 };
    struct m2sdr_fm_config cfg;
    size_t count, i;
    double worst = 0.0;
    int16_t *iq;
    float *f;

    m2sdr_fm_config_init(&cfg);
    iq = modulate(&cfg, &a, 0.1, 0, &count, NULL);
    f  = discriminate(iq, count, cfg.sample_rate);
    for (i = count / 2; i < count; i++)
        if (fabs(f[i] - 15000.0) > worst)
            worst = fabs(f[i] - 15000.0);
    /* The rounding of the 12-bit samples is about 100 Hz of frequency noise per sample. */
    CHECK(worst < 300.0, "frequency after a step: %.0f Hz away from 15000", worst);
    for (worst = 0.0, i = 1; i < count; i++)
        if (fabs(f[i]) > worst)
            worst = fabs(f[i]);
    CHECK(worst < 1.2 * 75000.0, "peak frequency after a step: %.0f Hz", worst);
    free(f);
    free(iq);
}

/* Audio that is too loud after the pre-emphasis is limited, and counted. */
static void test_modulator_limiter(void)
{
    const struct audio loud = { .l_hz = 10000.0, .l_amp = 1.0 };
    const struct audio fine = { .l_hz = 1000.0,  .l_amp = 0.7 };
    struct m2sdr_fm_config cfg;
    unsigned long long clipped = 0;
    size_t count, i;
    double worst = 0.0;
    int16_t *iq;
    float *f;

    m2sdr_fm_config_init(&cfg);
    iq = modulate(&cfg, &loud, 0.1, 0, &count, &clipped);
    f  = discriminate(iq, count, cfg.sample_rate);
    for (i = 1; i < count; i++)
        if (fabs(f[i]) > worst)
            worst = fabs(f[i]);
    CHECK(clipped > 0, "limiter not reported");
    CHECK(worst < 1.05 * 75000.0, "deviation of limited audio: %.0f Hz", worst);
    free(f);
    free(iq);

    iq = modulate(&cfg, &fine, 0.1, 0, &count, &clipped);
    CHECK(clipped == 0, "limiter on audio within full scale: %llu samples", clipped);
    free(iq);
}

/* Stereo multiplex, decoded here from the definition: pilot of 10 % at 19 kHz, L+R in the
 * audio band, L-R on a suppressed subcarrier at 38 kHz that crosses zero upwards with the
 * pilot. */
static void test_multiplex(void)
{
    const struct audio a = { .l_hz = 1000.0, .l_amp = 0.5 };
    struct m2sdr_fm_config cfg;
    size_t count, skip, n, i;
    double pilot, pilot_phase, sum, sum_phase, diff, diff_phase, beside, delta;
    int16_t *iq;
    float *f, *s;

    m2sdr_fm_config_init(&cfg);
    cfg.stereo = true;
    cfg.tau    = 0.0;
    iq = modulate(&cfg, &a, TOTAL_S, 0, &count, NULL);
    CHECK(iq != NULL, "stereo modulator");
    if (!iq)
        return;
    skip = (size_t)(SETTLE_S * cfg.sample_rate);
    n    = (size_t)(MEASURE_S * cfg.sample_rate);
    f    = discriminate(iq, count, cfg.sample_rate);
    for (i = 0; i < count; i++)
        f[i] /= 75000.0f;

    pilot  = tone(f + skip, n, 1, cfg.sample_rate, 19000.0, &pilot_phase);
    beside = tone(f + skip, n, 1, cfg.sample_rate, 19050.0, NULL);
    CHECK(fabs(pilot / M2SDR_FM_PILOT_LEVEL - 1.0) < 0.01, "pilot level %.4f", pilot);
    CHECK(beside < 0.001 * pilot, "pilot frequency: %.5f at 19050 Hz", beside);

    /* L+R: half of the left tone, in the 90 % left by the pilot. */
    sum = tone(f + skip, n, 1, cfg.sample_rate, 1000.0, &sum_phase);
    CHECK(fabs(sum / (0.9 * 0.25) - 1.0) < 0.01, "L+R level %.4f, expected %.4f", sum, 0.9 * 0.25);

    /* L-R, with the subcarrier rebuilt from the phase of the pilot: the same tone, in phase
     * with L+R since only the left channel is driven. */
    s = malloc(n * sizeof(*s));
    for (i = 0; i < n; i++) {
        const double p = 2.0 * M_PI * 19000.0 * (double)i / cfg.sample_rate + pilot_phase;

        s[i] = (float)(2.0 * f[skip + i] * sin(2.0 * p));
    }
    diff  = tone(s, n, 1, cfg.sample_rate, 1000.0, &diff_phase);
    delta = remainder(diff_phase - sum_phase, 2.0 * M_PI);
    CHECK(fabs(diff / (0.9 * 0.25) - 1.0) < 0.01, "L-R level %.4f, expected %.4f", diff, 0.9 * 0.25);
    CHECK(fabs(delta) < 0.02, "L-R against L+R: %.1f degrees", delta * 180.0 / M_PI);

    /* Nothing else: no 38 kHz carrier, no audio left around the pilot. */
    CHECK(tone(f + skip, n, 1, cfg.sample_rate, 38000.0, NULL) < 0.001, "38 kHz carrier");
    free(s);
    free(f);
    free(iq);
}

/* Round trip */
/*------------*/

/* Mono: level over the audio band with both emphasis, distortion, small signals. */
static void test_mono(void)
{
    static const double taus[]  = { 0.0, M2SDR_FM_TAU_EU, M2SDR_FM_TAU_US };
    static const double freqs[] = { 100.0, 1000.0, 5000.0, 10000.0, 15000.0 };
    struct m2sdr_fm_config cfg;
    size_t frames, skip, n, t, k;
    double rate, amp;
    float *audio;

    m2sdr_fm_config_init(&cfg);
    for (t = 0; t < sizeof(taus) / sizeof(taus[0]); t++) {
        cfg.tau = taus[t];
        for (k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
            const struct audio a = { .l_hz = freqs[k], .l_amp = 0.1 };

            audio = roundtrip(&cfg, NULL, &a, NULL, TOTAL_S, &frames, &rate, NULL);
            CHECK(audio != NULL, "round trip");
            if (!audio)
                return;
            skip = (size_t)(SETTLE_S * rate);
            n    = (size_t)(MEASURE_S * rate);
            amp  = tone(audio + skip, n, 1, rate, freqs[k], NULL);
            CHECK(fabs(db(amp / 0.1)) < 0.2, "mono, %.0f us, %.0f Hz: %+.2f dB",
                  taus[t] * 1e6, freqs[k], db(amp / 0.1));
            NOTE("mono, %.0f us, %.0f Hz: %+.2f dB", taus[t] * 1e6, freqs[k], db(amp / 0.1));
            free(audio);
        }
    }

    /* Distortion and noise of a loud tone, then of a tone 50 dB below full scale. */
    cfg.tau = M2SDR_FM_TAU_EU;
    {
        const struct audio a = { .l_hz = 1000.0, .l_amp = 0.9 };

        audio = roundtrip(&cfg, NULL, &a, NULL, TOTAL_S, &frames, &rate, NULL);
        skip  = (size_t)(SETTLE_S * rate);
        n     = (size_t)(MEASURE_S * rate);
        amp   = tone(audio + skip, n, 1, rate, 1000.0, NULL);
        CHECK(fabs(db(amp / 0.9)) < 0.1, "mono, loud tone: %+.2f dB", db(amp / 0.9));
        CHECK(sinad(audio + skip, n, 1, amp) > 60.0, "mono, loud tone: SINAD %.1f dB",
              sinad(audio + skip, n, 1, amp));
        NOTE("mono, loud tone: %+.2f dB, SINAD %.1f dB", db(amp / 0.9), sinad(audio + skip, n, 1, amp));
        free(audio);
    }
    {
        const struct audio a = { .l_hz = 1000.0, .l_amp = 0.003 };

        audio = roundtrip(&cfg, NULL, &a, NULL, TOTAL_S, &frames, &rate, NULL);
        amp   = tone(audio + skip, n, 1, rate, 1000.0, NULL);
        CHECK(fabs(db(amp / 0.003)) < 0.2, "mono, quiet tone: %+.2f dB", db(amp / 0.003));
        CHECK(sinad(audio + skip, n, 1, amp) > 30.0, "mono, quiet tone: SINAD %.1f dB",
              sinad(audio + skip, n, 1, amp));
        NOTE("mono, quiet tone: %+.2f dB, SINAD %.1f dB", db(amp / 0.003), sinad(audio + skip, n, 1, amp));
        free(audio);
    }
}

/* Stereo with one channel driven: level, separation, pilot residue. Returns the separation. */
static double stereo_case(const struct m2sdr_fm_config *cfg, const struct channel *ch,
                          double hz, bool right, double level_db, double separation_db,
                          const char *name)
{
    /* Less level at the frequencies that the pre-emphasis raises. */
    const double level = (hz <= 1000.0) ? 0.5 : 0.15;
    const struct audio a = {
        .l_hz = hz, .l_amp = right ? 0.0 : level,
        .r_hz = hz, .r_amp = right ? level : 0.0,
    };
    size_t frames, skip, n;
    double rate, on, off, pilot;
    bool locked = false;
    float *audio;

    audio = roundtrip(cfg, NULL, &a, ch, TOTAL_S, &frames, &rate, &locked);
    CHECK(audio != NULL, "%s: round trip", name);
    if (!audio)
        return 0.0;
    skip  = (size_t)(SETTLE_S * rate);
    n     = (size_t)(MEASURE_S * rate);
    on    = tone(audio + 2 * skip + (right ? 1 : 0), n, 2, rate, hz, NULL);
    off   = tone(audio + 2 * skip + (right ? 0 : 1), n, 2, rate, hz, NULL);
    pilot = tone(audio + 2 * skip, n, 2, rate, 19000.0, NULL);
    CHECK(locked, "%s: pilot not locked", name);
    CHECK(fabs(db(on / level)) < level_db, "%s, %.0f Hz, %s: %+.2f dB", name, hz,
          right ? "right" : "left", db(on / level));
    CHECK(db(on / off) > separation_db, "%s, %.0f Hz, %s: separation %.1f dB", name, hz,
          right ? "right" : "left", db(on / off));
    CHECK(db(pilot) < -60.0, "%s: pilot in the audio %.1f dB", name, db(pilot));
    NOTE("%s, %.0f Hz, %s: level %+.2f dB, separation %.1f dB, pilot %.1f dB", name, hz,
         right ? "right" : "left", db(on / level), db(on / off), db(pilot));
    free(audio);
    return db(on / off);
}

static void test_stereo(void)
{
    static const double freqs[] = { 100.0, 1000.0, 5000.0, 10000.0, 15000.0 };
    struct m2sdr_fm_config cfg;
    size_t frames, skip, n, k;
    double rate, l, r;
    bool locked;
    float *audio;

    m2sdr_fm_config_init(&cfg);
    cfg.stereo = true;
    for (k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
        stereo_case(&cfg, NULL, freqs[k], false, 0.2, 40.0, "stereo");
        stereo_case(&cfg, NULL, freqs[k], true,  0.2, 40.0, "stereo");
    }
    cfg.tau = M2SDR_FM_TAU_US;
    stereo_case(&cfg, NULL, 1000.0, false, 0.2, 40.0, "stereo 75 us");
    cfg.tau = M2SDR_FM_TAU_EU;

    /* Two different tones: each one in its channel. */
    {
        const struct audio a = { .l_hz = 1000.0, .l_amp = 0.5, .r_hz = 3000.0, .r_amp = 0.3 };

        audio = roundtrip(&cfg, NULL, &a, NULL, TOTAL_S, &frames, &rate, &locked);
        skip  = (size_t)(SETTLE_S * rate);
        n     = (size_t)(MEASURE_S * rate);
        l     = tone(audio + 2 * skip, n, 2, rate, 1000.0, NULL);
        r     = tone(audio + 2 * skip + 1, n, 2, rate, 3000.0, NULL);
        CHECK(fabs(db(l / 0.5)) < 0.2 && fabs(db(r / 0.3)) < 0.2, "two tones: %+.2f and %+.2f dB",
              db(l / 0.5), db(r / 0.3));
        CHECK(sinad(audio + 2 * skip, n, 2, l) > 40.0, "two tones: SINAD %.1f dB",
              sinad(audio + 2 * skip, n, 2, l));
        NOTE("two tones: %+.2f and %+.2f dB, SINAD %.1f dB", db(l / 0.5), db(r / 0.3),
             sinad(audio + 2 * skip, n, 2, l));
        free(audio);
    }
}

/* A mono receiver on a stereo signal gets L+R without the pilot; a stereo receiver on a mono
 * signal does not lock and gives the same signal on both channels. */
static void test_compatibility(void)
{
    const struct audio a = { .l_hz = 1000.0, .l_amp = 0.5, .r_hz = 1000.0, .r_amp = 0.0 };
    struct m2sdr_fm_config stereo, mono;
    size_t frames, skip, n, i;
    double rate, amp, pilot, worst = 0.0;
    bool locked = true;
    float *audio;

    m2sdr_fm_config_init(&mono);
    m2sdr_fm_config_init(&stereo);
    stereo.stereo = true;

    audio = roundtrip(&stereo, &mono, &a, NULL, TOTAL_S, &frames, &rate, NULL);
    skip  = (size_t)(SETTLE_S * rate);
    n     = (size_t)(MEASURE_S * rate);
    amp   = tone(audio + skip, n, 1, rate, 1000.0, NULL);
    pilot = tone(audio + skip, n, 1, rate, 19000.0, NULL);
    CHECK(fabs(db(amp / (0.9 * 0.25))) < 0.2, "mono receiver, stereo signal: %+.2f dB",
          db(amp / (0.9 * 0.25)));
    CHECK(db(pilot) < -60.0, "mono receiver: pilot in the audio %.1f dB", db(pilot));
    free(audio);

    audio = roundtrip(&mono, &stereo, &a, NULL, TOTAL_S, &frames, &rate, &locked);
    amp   = tone(audio + 2 * skip, n, 2, rate, 1000.0, NULL);
    for (i = 0; i < frames; i++)
        if (fabs(audio[2 * i] - audio[2 * i + 1]) > worst)
            worst = fabs(audio[2 * i] - audio[2 * i + 1]);
    CHECK(!locked, "stereo receiver, mono signal: pilot locked");
    CHECK(fabs(db(amp / 0.5)) < 0.2, "stereo receiver, mono signal: %+.2f dB", db(amp / 0.5));
    CHECK(worst == 0.0, "stereo receiver, mono signal: channels differ by %g", worst);
    free(audio);
}

/* Tuning offset, noise, and the small amplitude of 8-bit samples. */
static void test_channel(void)
{
    /* 30 kHz off tune, carrier 30 dB over the noise of the whole 1 MHz. */
    const struct channel offset = { .offset_hz = 30000.0 };
    const struct channel noisy  = { .offset_hz = -20000.0, .noise = 2047.0 / sqrt(2.0) / 31.6 };
    const struct channel small  = { .gain = 127.0 / 2047.0 };
    const struct audio a = { .l_hz = 1000.0, .l_amp = 0.5 };
    struct m2sdr_fm_config cfg;
    size_t frames, skip, n, i;
    double rate, amp, mean = 0.0;
    float *audio;

    m2sdr_fm_config_init(&cfg);
    cfg.stereo = true;
    stereo_case(&cfg, &offset, 1000.0, false, 0.2, 40.0, "30 kHz off tune");
    stereo_case(&cfg, &noisy,  1000.0, true,  0.5, 25.0, "noise");
    stereo_case(&cfg, &small,  1000.0, false, 0.3, 30.0, "8-bit amplitude");

    /* The tuning offset is not in the audio. */
    cfg.stereo = false;
    audio = roundtrip(&cfg, NULL, &a, &offset, 2 * TOTAL_S, &frames, &rate, NULL);
    skip  = (size_t)(2 * SETTLE_S * rate);
    n     = (size_t)(MEASURE_S * rate);
    amp   = tone(audio + skip, n, 1, rate, 1000.0, NULL);
    for (i = 0; i < n; i++)
        mean += audio[skip + i];
    CHECK(fabs(db(amp / 0.5)) < 0.2, "mono, off tune: %+.2f dB", db(amp / 0.5));
    CHECK(fabs(mean / n) < 0.01, "mono, off tune: DC level %.3f", mean / n);
    free(audio);
}

/* Other I/Q sample rates, with and without interpolation and channel filter. */
static void test_rates(void)
{
    static const double rates[] = { 250000.0, 500000.0, 768000.0, 1920000.0, 2000000.0, 4000000.0 };
    const struct audio a = { .l_hz = 1000.0, .l_amp = 0.5 };
    struct m2sdr_fm_config cfg;
    size_t frames, skip, n, k;
    double rate, amp;
    char name[64];
    float *audio;

    m2sdr_fm_config_init(&cfg);
    cfg.stereo = true;
    for (k = 0; k < sizeof(rates) / sizeof(rates[0]); k++) {
        cfg.sample_rate = rates[k];
        snprintf(name, sizeof(name), "%.0f SPS", rates[k]);
        stereo_case(&cfg, NULL, 1000.0, false, 0.2, 40.0, name);
        stereo_case(&cfg, NULL, 10000.0, true, 0.3, 35.0, name);
    }

    /* Narrow FM at an audio-card rate, mono. */
    cfg.stereo      = false;
    cfg.sample_rate = 96000.0;
    cfg.deviation   = 5000.0;
    cfg.tau         = 0.0;
    audio = roundtrip(&cfg, NULL, &a, NULL, TOTAL_S, &frames, &rate, NULL);
    CHECK(audio != NULL, "96 kSPS");
    if (audio) {
        skip = (size_t)(SETTLE_S * rate);
        n    = (size_t)(MEASURE_S * rate);
        amp  = tone(audio + skip, n, 1, rate, 1000.0, NULL);
        CHECK(fabs(db(amp / 0.5)) < 0.2, "96 kSPS, 5 kHz deviation: %+.2f dB", db(amp / 0.5));
        free(audio);
    }
}

/* The samples do not depend on how the stream is cut into calls. */
static void test_blocks(void)
{
    const struct channel odd = { .tx_block = 7, .rx_block = 13 };
    const struct audio a = { .l_hz = 1000.0, .l_amp = 0.5, .r_hz = 3000.0, .r_amp = 0.5 };
    struct m2sdr_fm_config cfg;
    size_t frames_a, frames_b;
    double rate;
    float *x, *y;

    m2sdr_fm_config_init(&cfg);
    cfg.stereo = true;
    x = roundtrip(&cfg, NULL, &a, NULL, 0.05, &frames_a, &rate, NULL);
    y = roundtrip(&cfg, NULL, &a, &odd, 0.05, &frames_b, &rate, NULL);
    CHECK(frames_a == frames_b && frames_a > 0, "block sizes: %zu and %zu frames", frames_a, frames_b);
    if (frames_a == frames_b)
        CHECK(memcmp(x, y, frames_a * 2 * sizeof(*x)) == 0, "block sizes: different audio");
    free(x);
    free(y);
}

static void test_config(void)
{
    struct m2sdr_fm_config cfg;

    m2sdr_fm_config_init(&cfg);
    CHECK(cfg.sample_rate == 1e6 && cfg.deviation == 75000.0 && cfg.bits == 12 && !cfg.stereo,
          "default configuration");
    cfg.sample_rate = 100000.0;
    cfg.stereo      = true;
    CHECK(m2sdr_fm_tx_new(&cfg) == NULL && m2sdr_fm_rx_new(&cfg) == NULL, "stereo at 100 kSPS");
    m2sdr_fm_config_init(&cfg);
    cfg.deviation = 500000.0;
    CHECK(m2sdr_fm_tx_new(&cfg) == NULL && m2sdr_fm_rx_new(&cfg) == NULL, "deviation over the rate");
    m2sdr_fm_config_init(&cfg);
    cfg.bits = 17;
    CHECK(m2sdr_fm_tx_new(&cfg) == NULL, "17 bits");
    CHECK(m2sdr_fm_tx_new(NULL) == NULL && m2sdr_fm_rx_new(NULL) == NULL, "no configuration");
    m2sdr_fm_tx_free(NULL);
    m2sdr_fm_rx_free(NULL);
}

/* RDS */
/*-----*/

/* From EN 50067. Annex A, table A.1: offset words A, B, C, C', D. */
static const char *const std_offsets[5] = {
    "0011111100", "0110011000", "0101101000", "1101010000", "0110110100",
};
/* Annex B, table B.1: their syndromes with the parity-check matrix of figure B.3. */
static const char *const std_syndromes[5] = {
    "1111011000", "1111010100", "1001011100", "1111001100", "1001011000",
};
/* Annex B, figure B.1: check bits of the generator matrix, one row per information bit,
 * m15 first. */
static const char *const std_generator[16] = {
    "0001110111", "1011100111", "1110101111", "1100001011", "1101011001", "1101110000",
    "0110111000", "0011011100", "0001101110", "0000110111", "1011000111", "1110111111",
    "1100000011", "1101011101", "1101110010", "0110111001",
};
/* Annex B, figure B.3: parity-check matrix, one row per received bit, first bit first. */
static const char *const std_parity[26] = {
    "1000000000", "0100000000", "0010000000", "0001000000", "0000100000", "0000010000",
    "0000001000", "0000000100", "0000000010", "0000000001", "1011011100", "0101101110",
    "0010110111", "1010000111", "1110011111", "1100010011", "1101010101", "1101110110",
    "0110111011", "1000000001", "1111011100", "0111101110", "0011110111", "1010100111",
    "1110001111", "1100011011",
};

static unsigned bin(const char *digits)
{
    unsigned v = 0;

    while (*digits)
        v = (v << 1) | (unsigned)(*digits++ - '0');
    return v;
}

/* Block of the standard: check bits from the generator matrix, plus the offset word. */
static uint32_t std_block(uint16_t data, int offset)
{
    unsigned check = 0;
    int i;

    for (i = 0; i < 16; i++)
        if (data & (0x8000 >> i))
            check ^= bin(std_generator[i]);
    return ((uint32_t)data << 10) | (check ^ bin(std_offsets[offset]));
}

/* Syndrome of the standard: sum of the rows of the parity-check matrix of the bits set. */
static unsigned std_syndrome(uint32_t block)
{
    unsigned syndrome = 0;
    int i;

    for (i = 0; i < 26; i++)
        if (block & (1u << (25 - i)))
            syndrome ^= bin(std_parity[i]);
    return syndrome;
}

/* Blocks against the standard: its two worked examples, its generator matrix, its offset
 * words and their syndromes. */
static void test_rds_blocks(void)
{
    static const uint16_t words[] = { 0x0000, 0x0001, 0x8000, 0xffff, 0x1234, 0xe0cd, 0x4d32, 0xa5a5 };
    uint16_t data;
    unsigned w, bit;
    int o;

    /* Annex B.1.1: 0000000000000001 and 1111111111111111 with the offset word B. */
    CHECK(m2sdr_fm_rds_encode_block(0x0001, M2SDR_FM_RDS_OFFSET_B) == bin("00000000000000010000100001"),
          "block of 0x0001: 0x%07x", m2sdr_fm_rds_encode_block(0x0001, M2SDR_FM_RDS_OFFSET_B));
    CHECK(m2sdr_fm_rds_encode_block(0xffff, M2SDR_FM_RDS_OFFSET_B) == bin("11111111111111110101010101"),
          "block of 0xffff: 0x%07x", m2sdr_fm_rds_encode_block(0xffff, M2SDR_FM_RDS_OFFSET_B));

    for (o = 0; o < 5; o++) {
        /* An offset word alone is the block of the information word zero. */
        CHECK(m2sdr_fm_rds_encode_block(0, (enum m2sdr_fm_rds_offset)o) == bin(std_offsets[o]),
              "offset word %d", o);
        CHECK(std_syndrome(bin(std_offsets[o])) == bin(std_syndromes[o]), "syndrome of offset %d", o);
        for (w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
            const uint32_t block = m2sdr_fm_rds_encode_block(words[w], (enum m2sdr_fm_rds_offset)o);

            CHECK(block == std_block(words[w], o), "block of 0x%04x, offset %d: 0x%07x, standard 0x%07x",
                  words[w], o, block, std_block(words[w], o));
            /* The decoder of the standard finds the syndrome of the offset word, whatever
             * the information word. */
            CHECK(std_syndrome(block) == bin(std_syndromes[o]), "syndrome of 0x%04x, offset %d", words[w], o);
            data = 0;
            CHECK(m2sdr_fm_rds_decode_block(block, &data) == o && data == words[w],
                  "decode of 0x%04x, offset %d", words[w], o);
            /* One wrong bit is never taken for the same block. */
            for (bit = 0; bit < 26; bit++)
                CHECK(m2sdr_fm_rds_decode_block(block ^ (1u << bit), &data) != o,
                      "0x%04x, offset %d: error on bit %u not detected", words[w], o, bit);
        }
    }
    CHECK(strcmp(m2sdr_fm_rds_pty_name(1), "News") == 0 && strcmp(m2sdr_fm_rds_pty_name(31), "Alarm") == 0 &&
          m2sdr_fm_rds_pty_name(32)[0] == '\0', "PTY names");
}

/* Modified Julian Day of a date (EN 50067, annex G), year from 1900. */
static long std_mjd(int year, int month, int day)
{
    const int l = (month == 1 || month == 2) ? 1 : 0;

    return 14956 + day + (long)((year - l) * 365.25) + (long)((month + 1 + l * 12) * 30.6001);
}

/* 2026-11-17 12:35 UTC, the minute that the test signals announce. */
#define CT_MJD   std_mjd(126, 11, 17)
#define CT_UNIX  ((CT_MJD - 40587) * 86400LL + 12 * 3600 + 35 * 60)

static void rds_test_config(struct m2sdr_fm_config *cfg)
{
    m2sdr_fm_config_init(cfg);
    cfg->rds.enable = true;
    cfg->rds.pi     = 0x1234;
    cfg->rds.pty    = 10;
    cfg->rds.music  = true;
    cfg->rds.ct     = true;
    cfg->rds.ct_unix_time = CT_UNIX - 2;   /* The minute comes two seconds into the signal. */
    cfg->rds.ct_offset    = 2;
    snprintf(cfg->rds.ps, sizeof(cfg->rds.ps), "M2SDR FM");
    snprintf(cfg->rds.rt, sizeof(cfg->rds.rt), "LiteX-M2SDR RDS test, 0123456789");
}

/* What the decoder must have found in a signal of rds_test_config(). */
static void rds_check(const struct m2sdr_fm_rds_info *info, const struct m2sdr_fm_config *cfg,
                      double max_error_rate, const char *name)
{
    const double rate = info->blocks ? (double)info->block_errors / (double)info->blocks : 1.0;

    CHECK(info->synced && info->groups > 20, "%s: sync %d, %llu groups", name, info->synced,
          (unsigned long long)info->groups);
    CHECK(rate <= max_error_rate, "%s: %llu wrong blocks of %llu", name,
          (unsigned long long)info->block_errors, (unsigned long long)info->blocks);
    CHECK(info->pi_valid && info->pi == cfg->rds.pi, "%s: PI 0x%04x", name, info->pi);
    CHECK(info->pty == cfg->rds.pty && info->music == cfg->rds.music && info->ta == cfg->rds.ta &&
          info->tp == cfg->rds.tp, "%s: PTY %u, flags %d %d %d", name, info->pty, info->music,
          info->ta, info->tp);
    CHECK(info->ps_valid && strcmp(info->ps, cfg->rds.ps) == 0, "%s: PS \"%s\"", name, info->ps);
    CHECK(info->rt_valid && strcmp(info->rt, cfg->rds.rt) == 0, "%s: RT \"%s\"", name, info->rt);
    CHECK(info->ct_valid && info->ct_unix_time == CT_UNIX && info->ct_offset == cfg->rds.ct_offset,
          "%s: CT %lld (expected %lld), offset %d", name, (long long)info->ct_unix_time,
          (long long)CT_UNIX, info->ct_offset);
    NOTE("%s: %llu groups, %llu wrong blocks of %llu, PS \"%s\", RT \"%s\"", name,
         (unsigned long long)info->groups, (unsigned long long)info->block_errors,
         (unsigned long long)info->blocks, info->ps, info->rt);
}

/* RDS subcarrier of the modulator, measured on the multiplex with the pilot as reference:
 * level, spectrum, phase against the third harmonic of the pilot, bit rate. */
static void test_rds_signal(void)
{
    const struct audio silence = { 0 };
    const double seconds = 1.0, block_rate = 1e6 / 128.0;
    struct m2sdr_fm_config cfg;
    size_t count, skip, n, blocks, i, k;
    double pilot, pilot_phase, peak = 0.0, in_band, out_band = 0.0, angle, line, beside;
    double complex axis = 0.0;
    float *f, *power;
    int16_t *iq;

    rds_test_config(&cfg);
    cfg.stereo = true;
    cfg.tau    = 0.0;
    iq = modulate(&cfg, &silence, seconds, 0, &count, NULL);
    CHECK(iq != NULL, "modulator with RDS");
    if (!iq)
        return;
    skip = (size_t)(0.1 * cfg.sample_rate);
    n    = count - skip;
    f    = discriminate(iq, count, cfg.sample_rate);
    for (i = 0; i < count; i++)
        f[i] /= 75000.0f;

    /* The pilot keeps its level; with the subcarrier, the multiplex stays within the
     * deviation. */
    pilot = tone(f + skip, n, 1, cfg.sample_rate, 19000.0, &pilot_phase);
    CHECK(fabs(pilot / M2SDR_FM_PILOT_LEVEL - 1.0) < 0.01, "pilot level with RDS %.4f", pilot);
    for (i = skip; i < count; i++)
        if (fabs(f[i]) > peak)
            peak = fabs(f[i]);
    peak -= M2SDR_FM_PILOT_LEVEL;
    CHECK(peak > 0.9 * M2SDR_FM_RDS_LEVEL && peak < 1.8 * M2SDR_FM_RDS_LEVEL,
          "RDS peak level %.4f for a nominal %.4f", peak, M2SDR_FM_RDS_LEVEL);
    NOTE("RDS: peak %.4f of the deviation (nominal %.4f)", peak, M2SDR_FM_RDS_LEVEL);

    /* Spectrum: data within 2.4 kHz of a suppressed 57 kHz carrier. */
    in_band = tone(f + skip, n, 1, cfg.sample_rate, 57000.0 + 600.0, NULL) +
              tone(f + skip, n, 1, cfg.sample_rate, 57000.0 - 900.0, NULL);
    for (k = 0; k < 6; k++) {
        static const double off[6] = { -8000.0, -4000.0, -2800.0, 2800.0, 4000.0, 8000.0 };
        const double a = tone(f + skip, n, 1, cfg.sample_rate, 57000.0 + off[k], NULL);

        if (a > out_band)
            out_band = a;
    }
    CHECK(db(out_band / in_band) < -40.0, "RDS spectrum: %.1f dB out of band", db(out_band / in_band));
    NOTE("RDS: %.1f dB beyond 2.8 kHz from the subcarrier", db(out_band / in_band));

    /* The subcarrier removed with the third harmonic of the pilot, in blocks of 256 samples
     * through a Hann window: complex data samples. Their square does not depend on the data
     * and gives the axis they are on: sin(3 * phase) is the imaginary axis here. */
    blocks = n / 128 - 2;
    power  = malloc(blocks * sizeof(*power));
    for (k = 0; k < blocks; k++) {
        double complex z = 0.0;

        for (i = 0; i < 256; i++) {
            const size_t at = k * 128 + i;
            const double p  = 3.0 * (2.0 * M_PI * 19000.0 * (double)at / cfg.sample_rate + pilot_phase);
            const double w  = 0.5 - 0.5 * cos(2.0 * M_PI * (i + 0.5) / 256.0);

            z += w * f[skip + at] * cexp(-I * p);
        }
        axis    += z * z;
        power[k] = (float)(creal(z) * creal(z) + cimag(z) * cimag(z));
    }
    angle = remainder(carg(axis) - M_PI, 2.0 * M_PI) / 2.0;
    CHECK(fabs(angle) < 2.0 * M_PI / 180.0, "RDS subcarrier %.1f degrees from the third harmonic of the pilot",
          angle * 180.0 / M_PI);
    NOTE("RDS: subcarrier %.2f degrees from the third harmonic of the pilot", angle * 180.0 / M_PI);

    /* Bit rate: the power of a biphase signal has lines at the bit rate, 57000 / 48, and at
     * twice the bit rate. */
    line   = tone(power, blocks, 1, block_rate, 2.0 * 57000.0 / 48.0, NULL);
    beside = tone(power, blocks, 1, block_rate, 2.0 * 57000.0 / 48.0 + 25.0, NULL) +
             tone(power, blocks, 1, block_rate, 2.0 * 57000.0 / 48.0 - 25.0, NULL);
    CHECK(line > 5.0 * beside, "RDS bit clock: line %.3g, beside %.3g", line, beside);
    NOTE("RDS: line at twice the bit rate %.1f dB over its sides", db(line / beside));
    free(power);
    free(f);
    free(iq);
}

/* Reference signal written from the standard, without the encoder of the library: groups as
 * information words, check bits from the generator matrix, differential coding, square
 * biphase symbols on a 57 kHz subcarrier, FM. */
static int16_t *rds_reference(const uint16_t (*groups)[4], const unsigned char *version_b,
                              size_t group_count, double seconds, double rate, size_t *count)
{
    const size_t n = (size_t)(seconds * rate);
    const size_t bits = (size_t)(seconds * 1187.5) + 2;
    unsigned char *coded = malloc(bits);
    int16_t *iq = malloc(n * 2 * sizeof(*iq));
    double phase = 0.0;
    unsigned prev = 0;
    size_t i;

    for (i = 0; i < bits; i++) {
        const size_t g = (i / 104) % group_count, block = (i % 104) / 26, bit = i % 26;
        const int offset = (block == 3) ? 4 : (block == 2 && version_b[g]) ? 3 : (int)block;
        const uint32_t word = std_block(groups[g][block], offset);

        /* Table 1 of the standard: the output changes when the input is 1. */
        prev    ^= (word >> (25 - bit)) & 1;
        coded[i] = (unsigned char)prev;
    }
    for (i = 0; i < n; i++) {
        const double t    = (double)i / rate;
        const double pos  = t * 1187.5;
        const size_t b    = (size_t)pos;
        /* A 1 is a positive impulse then a negative one half a bit later. */
        const double half = (pos - (double)b < 0.5) ? 1.0 : -1.0;
        const double mpx  = 0.04 * (coded[b] ? half : -half) * cos(2.0 * M_PI * 57000.0 * t);

        phase += 2.0 * M_PI * 75000.0 * mpx / rate;
        iq[2 * i + 0] = (int16_t)lrint(2047.0 * cos(phase));
        iq[2 * i + 1] = (int16_t)lrint(2047.0 * sin(phase));
    }
    free(coded);
    *count = n;
    return iq;
}

/* The decoder on the reference signal: groups 0A, 0B, 2A and 4A laid out as in the figures
 * of the standard. */
static void test_rds_reference(void)
{
    /* Block 2: group type (4 bits), version, TP, PTY (5 bits), then 5 bits of the group.
     * PTY 5 = 00101, TP = 1. */
    static const uint16_t groups[][4] = {
        /* 0A, TA = 1, M/S = 0, DI, address 0 and 1: no AF (224, filler 205), "RE", "F ". */
        { 0xc0de, 0x0000 | 0x0400 | (5 << 5) | 0x10 | 0, 0xe0cd, ('R' << 8) | 'E' },
        { 0xc0de, 0x0000 | 0x0400 | (5 << 5) | 0x10 | 1, 0xe0cd, ('F' << 8) | ' ' },
        /* 0B, address 2 and 3: the third block repeats the PI, offset word C'. */
        { 0xc0de, 0x0800 | 0x0400 | (5 << 5) | 0x10 | 2, 0xc0de, ('T' << 8) | 'E' },
        { 0xc0de, 0x0800 | 0x0400 | (5 << 5) | 0x10 | 3, 0xc0de, ('S' << 8) | 'T' },
        /* 2A, text flag 1, segments 0 to 2: "Standard", then the carriage return. */
        { 0xc0de, 0x2000 | 0x0400 | (5 << 5) | 0x10 | 0, ('S' << 8) | 't', ('a' << 8) | 'n' },
        { 0xc0de, 0x2000 | 0x0400 | (5 << 5) | 0x10 | 1, ('d' << 8) | 'a', ('r' << 8) | 'd' },
        { 0xc0de, 0x2000 | 0x0400 | (5 << 5) | 0x10 | 2, ('\r' << 8) | ' ', (' ' << 8) | ' ' },
        /* 4A: filled below. */
        { 0xc0de, 0x4000 | 0x0400 | (5 << 5), 0, 0 },
    };
    static const unsigned char version_b[] = { 0, 0, 1, 1, 0, 0, 0, 0 };
    uint16_t g[8][4];
    const unsigned long mjd = (unsigned long)CT_MJD;
    struct m2sdr_fm_config cfg;
    struct m2sdr_fm_rds_info info;
    size_t count, frames;
    double rate;
    int16_t *iq;
    float *audio;

    /* 4A: the two upper bits of the 17-bit day in block 2, its 15 lower bits then the upper
     * bit of the hour in block 3; hour (4 bits), minute (6 bits), sense and offset (5 bits)
     * in block 4. 12:35, local time 1 h 30 behind. */
    memcpy(g, groups, sizeof(g));
    g[7][1] |= (uint16_t)(mjd >> 15);
    g[7][2]  = (uint16_t)(((mjd & 0x7fff) << 1) | (12 >> 4));
    g[7][3]  = (uint16_t)(((12 & 15) << 12) | (35 << 6) | 0x20 | 3);

    m2sdr_fm_config_init(&cfg);
    cfg.rds.enable = true;
    iq    = rds_reference((const uint16_t (*)[4])g, version_b, 8, 2.5, cfg.sample_rate, &count);
    audio = demodulate(&cfg, iq, count, 0, &frames, &rate, NULL);
    info  = last_rds;
    CHECK(audio != NULL, "demodulator with RDS");
    CHECK(info.synced && info.block_errors == 0 && info.groups > 20,
          "reference: sync %d, %llu groups, %llu wrong blocks", info.synced,
          (unsigned long long)info.groups, (unsigned long long)info.block_errors);
    CHECK(info.pi_valid && info.pi == 0xc0de && info.pty == 5 && info.tp && info.ta && !info.music,
          "reference: PI 0x%04x, PTY %u, TP %d, TA %d, M/S %d", info.pi, info.pty, info.tp, info.ta, info.music);
    CHECK(info.ps_valid && strcmp(info.ps, "REF TEST") == 0, "reference: PS \"%s\"", info.ps);
    CHECK(info.rt_valid && strcmp(info.rt, "Standard") == 0, "reference: RT \"%s\"", info.rt);
    CHECK(info.ct_valid && info.ct_unix_time == CT_UNIX && info.ct_offset == -3,
          "reference: CT %lld (expected %lld), offset %d", (long long)info.ct_unix_time,
          (long long)CT_UNIX, info.ct_offset);
    NOTE("reference: %llu groups, PS \"%s\", RT \"%s\", CT %lld", (unsigned long long)info.groups,
         info.ps, info.rt, (long long)info.ct_unix_time);
    free(audio);
    free(iq);
}

/* Encoder to decoder: clean, with audio, with noise, off tune, with a sample clock error. */
static void test_rds_roundtrip(void)
{
    const struct audio silence = { 0 };
    const struct audio music   = { .l_hz = 1000.0, .l_amp = 0.5, .r_hz = 3000.0, .r_amp = 0.3 };
    /* Carrier 30 dB, 15 dB, then 9 dB over the noise of the whole 1 MHz: 16 dB in the
     * 200 kHz of the channel for the last one, where blocks start to be wrong. */
    const struct channel noise30 = { .noise = 2047.0 / sqrt(2.0) / 31.6 };
    const struct channel noise15 = { .noise = 2047.0 / sqrt(2.0) / 5.62, .offset_hz = 15000.0 };
    const struct channel noise9  = { .noise = 2047.0 / sqrt(2.0) / 2.82 };
    const struct channel offset  = { .offset_hz = -25000.0 };
    const double seconds = 3.0;
    struct m2sdr_fm_config cfg, rx_cfg, plain;
    size_t frames, skip, n;
    double rate, l, r, leak;
    bool locked;
    float *audio;

    /* Mono, no audio. */
    rds_test_config(&cfg);
    audio = roundtrip(&cfg, NULL, &silence, NULL, seconds, &frames, &rate, NULL);
    CHECK(audio != NULL, "RDS round trip");
    if (!audio)
        return;
    rds_check(&last_rds, &cfg, 0.0, "RDS, mono");
    free(audio);

    /* Stereo with audio: the data are right and the audio is unchanged. */
    cfg.stereo = true;
    audio = roundtrip(&cfg, NULL, &music, NULL, seconds, &frames, &rate, &locked);
    rds_check(&last_rds, &cfg, 0.0, "RDS, stereo and audio");
    skip = (size_t)(SETTLE_S * rate);
    n    = (size_t)(MEASURE_S * rate);
    l    = tone(audio + 2 * skip, n, 2, rate, 1000.0, NULL);
    r    = tone(audio + 2 * skip + 1, n, 2, rate, 3000.0, NULL);
    leak = tone(audio + 2 * skip + 1, n, 2, rate, 1000.0, NULL);
    CHECK(locked && fabs(db(l / 0.5)) < 0.2 && fabs(db(r / 0.3)) < 0.2 && db(l / leak) > 40.0,
          "audio with RDS: %+.2f and %+.2f dB, separation %.1f dB", db(l / 0.5), db(r / 0.3), db(l / leak));
    CHECK(sinad(audio + 2 * skip, n, 2, l) > 40.0, "audio with RDS: SINAD %.1f dB",
          sinad(audio + 2 * skip, n, 2, l));
    NOTE("audio with RDS: %+.2f and %+.2f dB, separation %.1f dB, SINAD %.1f dB", db(l / 0.5),
         db(r / 0.3), db(l / leak), sinad(audio + 2 * skip, n, 2, l));
    free(audio);

    /* Noise, tuning offset. */
    audio = roundtrip(&cfg, NULL, &music, &noise30, seconds, &frames, &rate, NULL);
    rds_check(&last_rds, &cfg, 0.0, "RDS, 30 dB over the noise");
    free(audio);
    audio = roundtrip(&cfg, NULL, &music, &noise15, seconds, &frames, &rate, NULL);
    rds_check(&last_rds, &cfg, 0.02, "RDS, 15 dB over the noise, 15 kHz off tune");
    free(audio);

    /* At the limit: blocks are lost, the checkwords keep what is decoded right. */
    audio = roundtrip(&cfg, NULL, &music, &noise9, 6.0, &frames, &rate, NULL);
    CHECK(last_rds.groups > 10 && last_rds.block_errors > 0 &&
          last_rds.block_errors < last_rds.blocks / 2, "RDS, 9 dB over the noise: %llu groups, %llu wrong blocks of %llu",
          (unsigned long long)last_rds.groups, (unsigned long long)last_rds.block_errors,
          (unsigned long long)last_rds.blocks);
    CHECK(last_rds.pi_valid && last_rds.pi == cfg.rds.pi && last_rds.pty == cfg.rds.pty,
          "RDS, 9 dB over the noise: PI 0x%04x, PTY %u", last_rds.pi, last_rds.pty);
    CHECK(!last_rds.ps_valid || strcmp(last_rds.ps, cfg.rds.ps) == 0, "RDS, 9 dB over the noise: PS \"%s\"",
          last_rds.ps);
    CHECK(!last_rds.rt_valid || strcmp(last_rds.rt, cfg.rds.rt) == 0, "RDS, 9 dB over the noise: RT \"%s\"",
          last_rds.rt);
    CHECK(!last_rds.ct_valid || last_rds.ct_unix_time == CT_UNIX, "RDS, 9 dB over the noise: CT %lld",
          (long long)last_rds.ct_unix_time);
    NOTE("RDS, 9 dB over the noise: %llu groups, %llu wrong blocks of %llu, PS %d, RT %d, CT %d",
         (unsigned long long)last_rds.groups, (unsigned long long)last_rds.block_errors,
         (unsigned long long)last_rds.blocks, last_rds.ps_valid, last_rds.rt_valid, last_rds.ct_valid);
    free(audio);
    audio = roundtrip(&cfg, NULL, &music, &offset, seconds, &frames, &rate, NULL);
    rds_check(&last_rds, &cfg, 0.0, "RDS, 25 kHz off tune");
    free(audio);

    /* Sample clocks 100 ppm apart: the receiver sees the subcarrier 5.7 Hz away. */
    rx_cfg = cfg;
    cfg.sample_rate *= 1.0 + 100e-6;
    cfg.deviation   *= 1.0 + 100e-6;
    audio = roundtrip(&cfg, &rx_cfg, &music, &noise30, seconds, &frames, &rate, NULL);
    rds_check(&last_rds, &cfg, 0.0, "RDS, clocks 100 ppm apart");
    free(audio);
    cfg.sample_rate = rx_cfg.sample_rate * (1.0 - 100e-6);
    cfg.deviation   = rx_cfg.deviation * (1.0 - 100e-6);
    audio = roundtrip(&cfg, &rx_cfg, &music, NULL, seconds, &frames, &rate, NULL);
    rds_check(&last_rds, &cfg, 0.0, "RDS, clocks -100 ppm apart");
    free(audio);

    /* Other I/Q rates. */
    rds_test_config(&cfg);
    cfg.stereo      = true;
    cfg.sample_rate = 250000.0;
    audio = roundtrip(&cfg, NULL, &music, NULL, seconds, &frames, &rate, NULL);
    rds_check(&last_rds, &cfg, 0.0, "RDS, 250 kSPS");
    free(audio);
    cfg.sample_rate = 2000000.0;
    audio = roundtrip(&cfg, NULL, &music, NULL, seconds, &frames, &rate, NULL);
    rds_check(&last_rds, &cfg, 0.0, "RDS, 2 MSPS");
    free(audio);

    /* A signal without RDS: nothing is decoded. */
    m2sdr_fm_config_init(&plain);
    plain.stereo = true;
    rds_test_config(&rx_cfg);
    rx_cfg.stereo = true;
    audio = roundtrip(&plain, &rx_cfg, &music, &noise30, 2.0, &frames, &rate, NULL);
    CHECK(last_rds.groups == 0 && !last_rds.pi_valid && !last_rds.ps_valid, "RDS found in a signal without it");
    free(audio);

    /* Not asked: nothing is reported. */
    rds_test_config(&cfg);
    audio = roundtrip(&cfg, &plain, &music, NULL, 0.5, &frames, &rate, NULL);
    CHECK(last_rds.groups == 0 && !last_rds.synced, "RDS reported without being enabled");
    free(audio);

    /* Rates and fields that RDS does not accept. */
    rds_test_config(&cfg);
    cfg.sample_rate = 100000.0;
    CHECK(m2sdr_fm_tx_new(&cfg) == NULL && m2sdr_fm_rx_new(&cfg) == NULL, "RDS at 100 kSPS");
    rds_test_config(&cfg);
    cfg.rds.pty = 32;
    CHECK(m2sdr_fm_tx_new(&cfg) == NULL, "PTY 32");
}

/* Tools */
/*-------*/

#define WAV_RATE 44100

static int write_wav(const char *path, double seconds, const struct audio *a)
{
    const unsigned frames = (unsigned)(seconds * WAV_RATE);
    const unsigned bytes  = frames * 4;
    unsigned char h[44] = "RIFF....WAVEfmt ";
    FILE *f = fopen(path, "wb");
    unsigned i;

    if (!f)
        return -1;
    memcpy(h + 4,  (unsigned[]){ 36 + bytes }, 4);
    memcpy(h + 16, (unsigned[]){ 16 }, 4);
    memcpy(h + 20, (unsigned short[]){ 1, 2 }, 4);
    memcpy(h + 24, (unsigned[]){ WAV_RATE, WAV_RATE * 4 }, 8);
    memcpy(h + 32, (unsigned short[]){ 4, 16 }, 4);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fwrite(h, 1, sizeof(h), f);
    for (i = 0; i < frames; i++) {
        const double t = (double)i / WAV_RATE;
        const short s[2] = {
            (short)lrint(32767.0 * a->l_amp * sin(2.0 * M_PI * a->l_hz * t)),
            (short)lrint(32767.0 * a->r_amp * sin(2.0 * M_PI * a->r_hz * t)),
        };

        fwrite(s, 2, 2, f);
    }
    return fclose(f);
}

/* 16-bit stereo PCM of a file, after `skip` bytes of header. Returns the left and right
 * samples interleaved, as floats. */
static float *read_pcm(const char *path, size_t skip, size_t *frames, unsigned char *header)
{
    FILE *f = fopen(path, "rb");
    float *x = NULL;
    short *raw;
    long size;
    size_t i;

    *frames = 0;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < (long)skip || (header && fread(header, 1, skip, f) != skip)) {
        fclose(f);
        return NULL;
    }
    fseek(f, (long)skip, SEEK_SET);
    *frames = ((size_t)size - skip) / 4;
    raw = malloc(*frames * 4 + 4);
    x   = malloc((*frames * 2 + 2) * sizeof(*x));
    if (fread(raw, 4, *frames, f) != *frames)
        *frames = 0;
    for (i = 0; i < *frames * 2; i++)
        x[i] = raw[i] / 32768.0f;
    free(raw);
    fclose(f);
    return x;
}

/* The two tones of the tools test, each in its channel. */
static void check_tool_audio(const float *x, size_t frames, const struct audio *a,
                             double seconds, const char *name)
{
    const size_t skip = (size_t)(0.3 * WAV_RATE), n = (size_t)(0.4 * WAV_RATE);
    double l, r, l_leak, r_leak;

    CHECK(x != NULL && fabs((double)frames / WAV_RATE - seconds) < 0.02, "%s: %zu frames for %.1f s",
          name, frames, seconds);
    if (!x || frames < skip + n)
        return;
    l      = tone(x + 2 * skip,     n, 2, WAV_RATE, a->l_hz, NULL);
    r      = tone(x + 2 * skip + 1, n, 2, WAV_RATE, a->r_hz, NULL);
    l_leak = tone(x + 2 * skip + 1, n, 2, WAV_RATE, a->l_hz, NULL);
    r_leak = tone(x + 2 * skip,     n, 2, WAV_RATE, a->r_hz, NULL);
    CHECK(fabs(db(l / a->l_amp)) < 0.3 && fabs(db(r / a->r_amp)) < 0.3, "%s: levels %+.2f and %+.2f dB",
          name, db(l / a->l_amp), db(r / a->r_amp));
    CHECK(db(l / l_leak) > 35.0 && db(r / r_leak) > 35.0, "%s: separation %.1f and %.1f dB",
          name, db(l / l_leak), db(r / r_leak));
    CHECK(sinad(x + 2 * skip, n, 2, l) > 35.0, "%s: SINAD %.1f dB", name, sinad(x + 2 * skip, n, 2, l));
    NOTE("%s: levels %+.2f and %+.2f dB, separation %.1f and %.1f dB, SINAD %.1f dB", name,
         db(l / a->l_amp), db(r / a->r_amp), db(l / l_leak), db(r / r_leak), sinad(x + 2 * skip, n, 2, l));
}

static int run(const char *dir, const char *fmt)
{
    char cmd[2048];

    /* The format takes the directory up to six times. */
    snprintf(cmd, sizeof(cmd), fmt, dir, dir, dir, dir, dir, dir);
    fflush(stdout);
    return system(cmd);
}

static void test_tools(void)
{
    const struct audio a = { .l_hz = 1000.0, .l_amp = 0.5, .r_hz = 3000.0, .r_amp = 0.3 };
    const double seconds = 1.0;
    char template[] = "/tmp/m2sdr_fmXXXXXX";
    char path[512];
    unsigned char h[44];
    const char *dir = mkdtemp(template);
    size_t frames;
    unsigned bytes = 0;
    float *x;

    CHECK(dir != NULL, "temporary directory");
    if (!dir)
        return;
    snprintf(path, sizeof(path), "%s/in.wav", dir);
    CHECK(write_wav(path, seconds, &a) == 0, "write %s", path);

    /* Through files: WAV to I/Q to WAV. */
    CHECK(run(dir, "./m2sdr_fm_tx -s 1000000 -m stereo %s/in.wav %s/a.iq 2>/dev/null >%s/tx.out") == 0,
          "m2sdr_fm_tx to a file");
    CHECK(run(dir, "./m2sdr_fm_rx -s 1000000 -m stereo %s/a.iq %s/a.wav 2>/dev/null >%s/rx.out") == 0,
          "m2sdr_fm_rx to a file");
    snprintf(path, sizeof(path), "%s/a.wav", dir);
    x = read_pcm(path, 44, &frames, h);
    check_tool_audio(x, frames, &a, seconds, "files");
    memcpy(&bytes, h + 40, 4);
    CHECK(x && memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVEfmt ", 8) == 0 && h[22] == 2 &&
          bytes == frames * 4, "WAV header: %u bytes of data for %zu frames", bytes, frames);
    free(x);
    snprintf(path, sizeof(path), "%s/tx.out", dir);
    x = read_pcm(path, 0, &frames, NULL);
    CHECK(frames == 0, "m2sdr_fm_tx writes text on stdout");
    free(x);

    /* Through pipes: stdout carries the samples and nothing else. */
    CHECK(run(dir, "./m2sdr_fm_tx -s 1000000 -m stereo %s/in.wav - 2>/dev/null"
                   " | ./m2sdr_fm_rx -s 1000000 -m stereo - - 2>/dev/null >%s/b.pcm") == 0, "pipes");
    snprintf(path, sizeof(path), "%s/b.pcm", dir);
    x = read_pcm(path, 0, &frames, NULL);
    check_tool_audio(x, frames, &a, seconds, "pipes");
    free(x);

    /* Raw PCM on stdin, 8-bit I/Q samples, 500 kSPS, 75 us. */
    CHECK(run(dir, "tail -c +45 %s/in.wav"
                   " | ./m2sdr_fm_tx -s 500000 -m stereo -e us --sc8 -i 2 -f 44100 - - 2>/dev/null"
                   " | ./m2sdr_fm_rx -s 500000 -m stereo -e us --sc8 - %s/c.wav 2>/dev/null") == 0,
          "raw PCM and SC8");
    snprintf(path, sizeof(path), "%s/c.wav", dir);
    x = read_pcm(path, 44, &frames, h);
    check_tool_audio(x, frames, &a, seconds, "raw PCM and SC8");
    free(x);

    /* Mono of a stereo file: half the sum of the channels. */
    CHECK(run(dir, "./m2sdr_fm_tx %s/in.wav %s/d.iq 2>/dev/null"
                   " && ./m2sdr_fm_rx %s/d.iq %s/d.wav 2>/dev/null") == 0, "mono");
    snprintf(path, sizeof(path), "%s/d.wav", dir);
    x = read_pcm(path, 44, &frames, h);
    CHECK(x && h[22] == 1, "mono WAV");
    if (x && frames > WAV_RATE / 2) {
        /* One channel: the samples of read_pcm() are consecutive. */
        const double l = tone(x + WAV_RATE / 4, WAV_RATE / 2, 1, WAV_RATE, a.l_hz, NULL);

        CHECK(fabs(db(l / (0.5 * a.l_amp))) < 0.3, "mono: %+.2f dB", db(l / (0.5 * a.l_amp)));
    }
    free(x);

    /* RDS: what m2sdr_fm_tx is given comes out of m2sdr_fm_rx, next to the audio. The clock
     * time is 2026-11-17 12:35 UTC, sent for a host one hour ahead of UTC. */
    snprintf(path, sizeof(path), "%s/long.wav", dir);
    CHECK(write_wav(path, 3.5, &a) == 0, "write %s", path);
    CHECK(run(dir, "TZ=UTC-1 ./m2sdr_fm_tx -m stereo --rds-pi F212 --rds-ps 'M2SDR FM' --rds-pty 10"
                   " --rds-rt 'RDS through the utilities' --rds-ct 1794918898 %s/long.wav - 2>/dev/null"
                   " | ./m2sdr_fm_rx -m stereo --rds - %s/r.wav 2>%s/rds.txt") == 0, "RDS through the tools");
    CHECK(run(dir, "grep -q 'PI=F212 PTY=10 (Pop music) PS=\"M2SDR FM\" RT=\"RDS through the utilities\""
                   " CT=2026-11-17 13:35 +01:00' %s/rds.txt") == 0, "RDS line of m2sdr_fm_rx");
    CHECK(run(dir, "grep -q 'RDS: [0-9]* groups, 0 wrong blocks' %s/rds.txt") == 0, "RDS summary of m2sdr_fm_rx");
    snprintf(path, sizeof(path), "%s/r.wav", dir);
    x = read_pcm(path, 44, &frames, h);
    check_tool_audio(x, frames, &a, 3.5, "RDS and audio");
    free(x);
    if (verbose)
        run(dir, "grep RDS %s/rds.txt | tail -n 2");
    CHECK(run(dir, "./m2sdr_fm_tx --rds-ps 'NINE CHAR' %s/in.wav %s/e.iq 2>/dev/null") != 0, "long PS accepted");
    CHECK(run(dir, "./m2sdr_fm_tx --rds-pi 12345 %s/in.wav %s/e.iq 2>/dev/null") != 0, "long PI accepted");
    CHECK(run(dir, "./m2sdr_fm_tx -s 100000 --rds %s/in.wav %s/e.iq 2>/dev/null") != 0, "RDS at 100 kSPS accepted");

    /* Bad arguments end with an error. */
    CHECK(run(dir, "./m2sdr_fm_tx -m quad %s/in.wav %s/e.iq 2>/dev/null") != 0, "bad mode accepted");
    CHECK(run(dir, "./m2sdr_fm_tx -s 100000 -m stereo %s/in.wav %s/e.iq 2>/dev/null") != 0,
          "stereo at 100 kSPS accepted");
    CHECK(run(dir, "./m2sdr_fm_rx -e jp %s/a.iq %s/e.wav 2>/dev/null") != 0, "bad emphasis accepted");
    CHECK(run(dir, "./m2sdr_fm_tx %s/missing.wav %s/e.iq 2>/dev/null") != 0, "missing input accepted");

    run(dir, "rm -rf %s");
}

int main(int argc, char **argv)
{
    int i, tools = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else if (strcmp(argv[i], "--tools") == 0)
            tools = 1;
    }
    test_config();
    test_modulator();
    test_modulator_step();
    test_modulator_limiter();
    test_multiplex();
    test_mono();
    test_stereo();
    test_compatibility();
    test_channel();
    test_rates();
    test_blocks();
    test_rds_blocks();
    test_rds_signal();
    test_rds_reference();
    test_rds_roundtrip();
    if (tools)
        test_tools();

    if (failures) {
        printf("test_m2sdr_fm: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_m2sdr_fm: ok\n");
    return 0;
}
