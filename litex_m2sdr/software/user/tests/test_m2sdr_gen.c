/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR generator: the samples that gen_fill_payload() of m2sdr_gen.c writes, against the plain
 * computation of each sample (cos() and sin(), a shift register bit by bit, fmod(), lroundf(),
 * QAM a sample at a time). The fast paths of the tool must give the same bits. With --streams,
 * the QAM and OFDM samples as hashes, to compare the builds with and without SSE2.
 *
 * This file is part of LiteX-M2SDR.
 */

#define main m2sdr_gen_tool_main
#include "../m2sdr_gen.c"
#undef main

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        if (failures++ < 20) { printf("FAIL: " __VA_ARGS__); printf("\n"); } \
    } \
} while (0)

/* The 8-bit conversion as it was written first. */
static int8_t ref_float_to_sc8(float sample)
{
    float scaled;

    if (!isfinite(sample))
        return 0;
    scaled = sample * 127.0f;
    if (scaled > 127.0f)
        return 127;
    if (scaled < -128.0f)
        return -128;
    return (int8_t)lroundf(scaled);
}

/* Frame j of a payload: I, Q, and the GPIO bit of the PPS (16 bits only). */
static void check_frame(const uint8_t *payload, size_t j, int use_8bit, float i_ref, float q_ref, int gpio,
                        int gpio_pin, const char *what, int *bad)
{
    if (use_8bit) {
        const int8_t *p = (const int8_t *)payload + 4 * j;
        const int8_t i8 = ref_float_to_sc8(i_ref), q8 = ref_float_to_sc8(q_ref);

        if (p[0] != i8 || p[1] != q8 || p[2] != i8 || p[3] != q8)
            (*bad)++;
    } else {
        const uint16_t *p = (const uint16_t *)payload + 4 * j;
        const uint16_t g = gpio ? (uint16_t)((1u << gpio_pin) << 12) : 0;
        const uint16_t i16 = (uint16_t)(((int16_t)(i_ref * 2047) & 0xFFF) | g);
        const uint16_t q16 = (uint16_t)(((int16_t)(q_ref * 2047) & 0xFFF) | g);

        if (p[0] != i16 || p[1] != q16 || p[2] != i16 || p[3] != q16)
            (*bad)++;
    }
    (void)what;
}

/* Tone: cos() and sin() of the phase on each sample, the phase kept within a turn; with a PPS
 * of a period that is not a whole number of samples, from fmod() on each sample. */
static void test_tone(void)
{
    /* Among them, frequencies and amplitudes that put samples on the steps of the conversion
     * (k / 2047 for 12 bits, (k + 0.5) / 127 for 8 bits, at a quarter, a sixth and an eighth of
     * the sample rate): where the last bits of the tone decide the sample. */
    static const double freqs[] = { 1e6, 1.0, 123456.789, 7.68e6, 15.36e6, -3.3e6, -1e3, 30.72e6 / 3, 14999999.5, 0.0,
                                    30.72e6 / 4, 30.72e6 / 8, -30.72e6 / 4, 30.72e6 / 6, 30.72e6 / 12, 30.72e6 / 16 };
    static const double amps[] = { 1.0, 0.7, 0.999, 0.123, 1000.0 / 2047.0, 2046.0 / 2047.0, 1447.0 / 2047.0,
                                   100.5 / 127.0, 63.5 / 127.0, 89.5 / 127.0 };
    static uint8_t buf[1 << 16];
    const double fs = 30.72e6;
    unsigned long samples = 0;

    for (unsigned f = 0; f < sizeof(freqs) / sizeof(freqs[0]); f++) {
        for (unsigned a = 0; a < sizeof(amps) / sizeof(amps[0]); a++) {
            for (int use_8bit = 0; use_8bit <= 1; use_8bit++) {
                struct gen_signal_state g;
                const double omega = 2 * M_PI * freqs[f] / fs;
                double phi = 0.0;
                uint64_t n = 0;
                int bad = 0;

                memset(&g, 0, sizeof(g));
                g.signal_type = "tone";
                g.amplitude   = amps[a];
                g.omega       = omega;
                g.use_8bit    = (uint8_t)use_8bit;
                g.gpio_pin    = 2;
                if ((f + a) & 1) {
                    g.pps_freq           = 3;
                    g.pps_period_samples = fs / 3 / 1000.5;
                    g.pps_high_samples   = g.pps_period_samples * 0.2;
                }
                for (int b = 0; b < 12; b++) {
                    const size_t bytes  = sizeof(buf) - (size_t)(b % 3) * 8;   /* Buffers of several lengths. */
                    const size_t frames = bytes / (use_8bit ? 4 : 8);

                    gen_fill_payload(buf, bytes, &g);
                    for (size_t j = 0; j < frames; j++, n++) {
                        const float i_ref = cos(phi) * amps[a];
                        const float q_ref = sin(phi) * amps[a];
                        const int gpio = g.pps_freq > 0 && fmod(n, g.pps_period_samples) < g.pps_high_samples;

                        check_frame(buf, j, use_8bit, i_ref, q_ref, gpio, g.gpio_pin, "tone", &bad);
                        phi += omega;
                        if (phi >= 2 * M_PI)
                            phi -= 2 * M_PI;
                        else if (phi < 0)
                            phi += 2 * M_PI;
                    }
                    samples += frames;
                }
                CHECK(bad == 0, "tone %g Hz, amplitude %g, %d bits: %d samples differ", freqs[f], amps[a],
                      use_8bit ? 8 : 16, bad);
                CHECK(g.sample_count == n, "tone: %llu samples counted, %llu written",
                      (unsigned long long)g.sample_count, (unsigned long long)n);
            }
        }
    }
    printf("test_m2sdr_gen: tone, %lu samples checked\n", samples);
}

/* What makes the fast tone exact: a value that gen_tone_near_step() lets through gives the same
 * sample when it moves by much more than the error of the rotations (checked below), around
 * every step of both conversions; and that error stays far under what is checked here. */
static void test_tone_guard(void)
{
    static const double offsets[] = { 0.0, 1e-13, 1e-10, 1e-7, 1e-5, 1e-4, 3e-4, 9e-4, 1.01e-3, 2e-3, 0.01, 0.3 };
    const double moved = 1e-9;     /* Of full scale: a thousand times the error allowed below. */
    int bad = 0, let_through = 0;
    double worst = 0.0;

    for (int use_8bit = 0; use_8bit <= 1; use_8bit++) {
        const double scale = use_8bit ? 127.0 : 2047.0;

        for (int k = -(int)scale; k <= (int)scale; k++) {
            for (unsigned o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
                for (int sign = -1; sign <= 1; sign += 2) {
                    const double v = ((use_8bit ? k + 0.5 : k) + sign * offsets[o]) / scale;

                    if (fabs(v) > 1.0 || gen_tone_near_step(v, use_8bit))
                        continue;
                    let_through++;
                    for (int m = -1; m <= 1; m += 2) {
                        const float a = v, b = v + m * moved;

                        if (use_8bit)
                            bad += gen_float_to_sc8(a) != gen_float_to_sc8(b);
                        else
                            bad += (int16_t)(a * 2047) != (int16_t)(b * 2047);
                    }
                }
            }
        }
    }
    CHECK(bad == 0, "tone guard: %d values outside the guard change their sample when moved by %g", bad, moved);
    CHECK(let_through > 10000, "tone guard: only %d values let through", let_through);

    /* The rotations against cos() and sin(), over the samples between two of their restarts. */
    for (double omega = -3.0; omega <= 3.0; omega += 0.0937) {
        double phi = 0.3, c = cos(phi), s = sin(phi);
        const double co = cos(omega), so = sin(omega);

        for (int n = 0; n < GEN_TONE_RESYNC; n++) {
            const double t = c * co - s * so;

            s = s * co + c * so;
            c = t;
            phi += omega;
            if (phi >= 2 * M_PI)
                phi -= 2 * M_PI;
            else if (phi < 0)
                phi += 2 * M_PI;
            if (fabs(c - cos(phi)) > worst)
                worst = fabs(c - cos(phi));
            if (fabs(s - sin(phi)) > worst)
                worst = fabs(s - sin(phi));
        }
    }
    CHECK(worst < 1e-12, "tone: the rotations are %g from cos() and sin()", worst);
    printf("test_m2sdr_gen: tone guard checked (%d values, rotations within %.1e)\n", let_through, worst);
}

/* PRBS31, x^31 + x^28 + 1: 12 bits for I then 12 for Q, from the register bit by bit. */
static void test_prbs(void)
{
    static uint8_t buf[1 << 16];
    struct gen_signal_state g;
    uint32_t lfsr = 0xFFFFFFFFu;
    int bad = 0;

    memset(&g, 0, sizeof(g));
    g.signal_type = "prbs";
    g.amplitude   = 0.9;
    prbs_lfsr     = 0xFFFFFFFFu;
    for (int b = 0; b < 20; b++) {
        gen_fill_payload(buf, sizeof(buf), &g);
        for (size_t j = 0; j < sizeof(buf) / 8; j++) {
            int32_t v[2];

            for (int k = 0; k < 2; k++) {
                v[k] = 0;
                for (int bit = 0; bit < 12; bit++) {
                    const int x = ((lfsr >> 30) ^ (lfsr >> 27)) & 1;

                    lfsr = (lfsr << 1) | (uint32_t)x;
                    v[k] = (v[k] << 1) | x;
                }
            }
            check_frame(buf, j, 0, ((float)(v[0] - 2048) / 2048.0f) * g.amplitude,
                        ((float)(v[1] - 2048) / 2048.0f) * g.amplitude, 0, 0, "prbs", &bad);
        }
    }
    CHECK(bad == 0, "prbs: %d samples differ", bad);
    CHECK(prbs_lfsr == lfsr, "prbs: register %08x, expected %08x", prbs_lfsr, lfsr);
    printf("test_m2sdr_gen: prbs checked\n");
}

/* The conversion to 8 bits: rounding ties, both ends, values that are not finite, and floats
 * drawn over all their bit patterns. */
static void test_sc8(void)
{
    uint32_t x = 1;
    int bad = 0;

    for (int k = -260; k <= 260; k++) {
        const float tie = (k + 0.5f) / 127.0f;

        bad += gen_float_to_sc8(tie) != ref_float_to_sc8(tie);
        bad += gen_float_to_sc8(nextafterf(tie, 4.0f)) != ref_float_to_sc8(nextafterf(tie, 4.0f));
        bad += gen_float_to_sc8(nextafterf(tie, -4.0f)) != ref_float_to_sc8(nextafterf(tie, -4.0f));
        bad += gen_float_to_sc8(k / 127.0f) != ref_float_to_sc8(k / 127.0f);
    }
    for (long n = 0; n < 20000000; n++) {
        float v;

        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        memcpy(&v, &x, sizeof(v));
        bad += gen_float_to_sc8(v) != ref_float_to_sc8(v);
    }
    CHECK(bad == 0, "gen_float_to_sc8: %d values differ from lroundf()", bad);
    CHECK(gen_float_to_sc8(NAN) == 0 && gen_float_to_sc8(INFINITY) == 0 && gen_float_to_sc8(-INFINITY) == 0,
          "gen_float_to_sc8: a value that is not finite is not 0");
    printf("test_m2sdr_gen: 8-bit conversion checked\n");
}

/* The clamp of the QAM samples against fminf(fmaxf()). */
static void test_clamp(void)
{
    static const float values[] = { 0.0f, -0.0f, 0.5f, 1.0f, -1.0f, 1.0000001f, -1.0000001f, 3.0f, -3.0f,
                                    INFINITY, -INFINITY, NAN };
    int bad = 0;

    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        const float want = fminf(fmaxf(values[i], -1.0f), 1.0f), got = gen_clamp1(values[i]);

        bad += memcmp(&want, &got, sizeof(want)) != 0;
    }
    CHECK(bad == 0, "gen_clamp1: %d values differ from fminf(fmaxf())", bad);
}

/* QAM as it was written first: a sample at a time, the pulse under each symbol computed for
 * each sample, one sum of complex products. */
struct ref_qam {
    float complex symbols[2 * QAM_SPAN];
    double frac;
    uint32_t lfsr;
};

static float complex ref_qam_symbol(struct ref_qam *r, const struct qam_state *st)
{
    r->lfsr ^= r->lfsr << 13;
    r->lfsr ^= r->lfsr >> 17;
    r->lfsr ^= r->lfsr << 5;
    return st->points[(r->lfsr >> 8) % (uint32_t)st->npoints];
}

static void ref_qam_sample(struct ref_qam *r, const struct qam_state *st, float *i_out, float *q_out)
{
    float complex acc = 0;
    int j;

    for (j = 0; j < 2 * QAM_SPAN; j++) {
        const double u = (r->frac + (double)(2 * QAM_SPAN - 1 - j)) * QAM_PHASES;
        const int k = (int)u;
        acc += r->symbols[j] * (st->pulse[k] + (float)(u - k) * (st->pulse[k + 1] - st->pulse[k]));
    }
    acc *= st->scale;
    *i_out = fminf(fmaxf(crealf(acc), -1.0f), 1.0f);
    *q_out = fminf(fmaxf(cimagf(acc), -1.0f), 1.0f);
    r->frac += st->step;
    while (r->frac >= 1.0) {
        memmove(r->symbols, r->symbols + 1, (2 * QAM_SPAN - 1) * sizeof(r->symbols[0]));
        r->symbols[2 * QAM_SPAN - 1] = ref_qam_symbol(r, st);
        r->frac -= 1.0;
    }
}

/* The symbol rates: ones that come back to the same positions in the symbol (the default, a
 * sixteenth of the sample rate, and others), ones that never do, the sample rate itself and
 * more (several new symbols per sample). */
static void test_qam(void)
{
    static const double fs = 30.72e6;
    static const double rates[] = { 0, 30.72e6 / 10, 1.2345e6, 30.72e6 / 20, 30.72e6 / 6, 30.72e6, 2 * 30.72e6,
                                    3.7 * 30.72e6, 30.72e6 / 4, 30.72e6 / 64, 30.72e6 / 3, 1.0, 29.9e6, 7.3e6 };
    static const enum m2sdr_modulation mods[] = { M2SDR_MOD_QPSK, M2SDR_MOD_16QAM, M2SDR_MOD_64QAM, M2SDR_MOD_BPSK };
    static struct qam_state qam;
    unsigned long samples = 0;

    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (unsigned m = 0; m < sizeof(mods) / sizeof(mods[0]); m++) {
            struct ref_qam ref;
            int bad = 0;

            qam_symbol_rate = rates[r];
            qam_modulation  = mods[m];
            qam_rolloff     = m == 2 ? 0.2 : 0.35;
            qam_init(&qam, fs, 77 + r);
            memcpy(ref.symbols, qam.symbols, sizeof(ref.symbols));
            ref.frac = qam.frac;
            ref.lfsr = qam.lfsr;
            for (int n = 0; n < 200000; n++) {
                float i_got, q_got, i_ref, q_ref;

                qam_next_sample(&qam, &i_got, &q_got);
                ref_qam_sample(&ref, &qam, &i_ref, &q_ref);
                bad += memcmp(&i_got, &i_ref, sizeof(i_got)) != 0 || memcmp(&q_got, &q_ref, sizeof(q_got)) != 0;
            }
            samples += 200000;
            CHECK(bad == 0, "qam at %g symbols/s, modulation %u: %d samples differ", rates[r], m, bad);
        }
    }
    qam_symbol_rate = 0.0;
    qam_modulation  = M2SDR_MOD_QPSK;
    qam_rolloff     = 0.35;
    printf("test_m2sdr_gen: qam, %lu samples checked\n", samples);
}

/* --streams: a hash of the QAM and OFDM samples of gen_fill_payload(), a line per case. The
 * build with SSE2 (four samples or four symbols at a time, kiss_fft in its SIMD mode) and the
 * one without (M2SDR_GEN_NO_SIMD) must print the same lines: the Makefile compares them. */
static uint64_t stream_hash(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void print_streams(void)
{
    static const int ffts[] = { 64, 100, 256, 600, 1024, 1536, 2048, 4096 };
    static const double rates[] = { 0, 30.72e6 / 10, 1.2345e6, 2 * 30.72e6 };
    static uint8_t buf[1 << 16];
    static struct qam_state qam;

    for (unsigned f = 0; f < sizeof(ffts) / sizeof(ffts[0]); f++) {
        for (int v = 0; v < 6; v++) {
            const int fft = ffts[f], use_8bit = v & 1, variant = v / 2;
            struct gen_signal_state g;
            struct ofdm_state ofdm;
            uint64_t h = 1469598103934665603ull;

            memset(&g, 0, sizeof(g));
            g.signal_type = "ofdm";
            g.amplitude   = variant == 0 ? 1.0 : 0.6;
            g.use_8bit    = (uint8_t)use_8bit;
            /* Cyclic prefixes and occupations of several sizes, both modulations. */
            ofdm_init(&ofdm, fft, variant == 2 ? fft / 4 : fft * 72 / 1024, variant == 1 ? fft - 2 : fft * 600 / 1024,
                      variant == 1 ? "16qam" : "qpsk", 5 + (uint32_t)v);
            g.ofdm = &ofdm;
            for (int n = 0; n < 60; n++) {
                gen_fill_payload(buf, sizeof(buf) - (size_t)(n % 5) * 8, &g);
                h = stream_hash(h, buf, sizeof(buf));
            }
            ofdm_cleanup(&ofdm);
            printf("ofdm %d %d: %016llx\n", fft, v, (unsigned long long)h);
        }
    }
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (int use_8bit = 0; use_8bit <= 1; use_8bit++) {
            struct gen_signal_state g;
            uint64_t h = 1469598103934665603ull;

            memset(&g, 0, sizeof(g));
            qam_symbol_rate = rates[r];
            qam_modulation  = r & 1 ? M2SDR_MOD_16QAM : M2SDR_MOD_QPSK;
            g.signal_type = "qam";
            g.amplitude   = 0.8;
            g.use_8bit    = (uint8_t)use_8bit;
            qam_init(&qam, 30.72e6, 77 + r);
            g.qam = &qam;
            for (int n = 0; n < 60; n++) {
                gen_fill_payload(buf, sizeof(buf) - (size_t)(n % 7) * 8, &g);
                h = stream_hash(h, buf, sizeof(buf));
            }
            printf("qam %g %d: %016llx\n", rates[r], use_8bit ? 8 : 16, (unsigned long long)h);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--streams") == 0) {
        print_streams();
        return 0;
    }
    test_tone();
    test_tone_guard();
    test_prbs();
    test_sc8();
    test_clamp();
    test_qam();
    if (failures) {
        printf("test_m2sdr_gen: %d FAILED\n", failures);
        return 1;
    }
    printf("test_m2sdr_gen: all tests passed\n");
    return 0;
}
