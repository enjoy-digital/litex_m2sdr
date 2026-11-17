/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests of the FM modulator and demodulator: tones through the modulator, checked on the I/Q
 * samples with a discriminator of the test, then through the demodulator. With --tools, the
 * same round trip through m2sdr_fm_tx and m2sdr_fm_rx, with files and with pipes.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <complex.h>
#include <math.h>
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
    if (tools)
        test_tools();

    if (failures) {
        printf("test_m2sdr_fm: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_m2sdr_fm: ok\n");
    return 0;
}
