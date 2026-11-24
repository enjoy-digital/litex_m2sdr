/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR broadcast FM: modulator and demodulator between audio and I/Q samples, with RDS.
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

/* RDS: blocks */
/*-------------*/

#define FM_RDS_POLY  0x5b9u  /* x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1 */

/* Offset words A, B, C, C', D (EN 50067, annex A). */
static const uint16_t fm_rds_offsets[5] = { 0x0fc, 0x198, 0x168, 0x350, 0x1b4 };

/* Remainder of a 26-bit word divided by the generator polynomial. */
static uint32_t fm_rds_remainder(uint32_t word)
{
    int i;

    for (i = 25; i >= 10; i--)
        if (word & (1u << i))
            word ^= FM_RDS_POLY << (i - 10);
    return word & 0x3ff;
}

uint32_t m2sdr_fm_rds_encode_block(uint16_t data, enum m2sdr_fm_rds_offset offset)
{
    const uint32_t word = (uint32_t)data << 10;

    return word | (fm_rds_remainder(word) ^ fm_rds_offsets[offset]);
}

/* The remainder of a block without error is its offset word: the checkword cancels the one
 * of the information bits. */
int m2sdr_fm_rds_decode_block(uint32_t block, uint16_t *data)
{
    const uint32_t rem = fm_rds_remainder(block & 0x3ffffff);
    int i;

    for (i = 0; i < 5; i++) {
        if (rem == fm_rds_offsets[i]) {
            if (data)
                *data = (uint16_t)(block >> 10);
            return i;
        }
    }
    return -1;
}

const char *m2sdr_fm_rds_pty_name(unsigned pty)
{
    static const char *const names[32] = {
        "None", "News", "Current affairs", "Information", "Sport", "Education", "Drama",
        "Culture", "Science", "Varied", "Pop music", "Rock music", "Easy listening",
        "Light classical", "Serious classical", "Other music", "Weather", "Finance",
        "Children's programmes", "Social affairs", "Religion", "Phone-in", "Travel", "Leisure",
        "Jazz music", "Country music", "National music", "Oldies music", "Folk music",
        "Documentary", "Alarm test", "Alarm",
    };

    return (pty < 32) ? names[pty] : "";
}

/* RDS: shaped biphase symbol */
/*----------------------------*/

/* Impulse response of the shaping filter cos(pi * f * td / 4) up to f = 2 / td, where td is
 * the bit period and u the time in bit periods. */
static double fm_rds_shape(double u)
{
    const double d = 1.0 - 64.0 * u * u;

    if (fabs(d) < 1e-9)
        return 1.0;
    return (4.0 / M_PI) * cos(4.0 * M_PI * u) / d;
}

/* A bit is a pair of opposite impulses half a bit apart through this filter, centred here on
 * the middle of the bit. */
static double fm_rds_pulse(double u)
{
    return fm_rds_shape(u + 0.25) - fm_rds_shape(u - 0.25);
}

/* RDS: encoder */
/*--------------*/

#define FM_RDS_SPAN   3    /* Bits kept on each side of the symbol: it is 66 dB down beyond. */
#define FM_RDS_RES    64   /* Points per bit in the table of the symbol. */
#define FM_RDS_TABLE  ((2 * FM_RDS_SPAN + 2) * FM_RDS_RES + 1)
#define FM_RDS_UNIX_MJD 40587  /* Modified Julian Day of 1970-01-01. */

struct fm_rds_tx {
    struct m2sdr_fm_rds_config cfg;
    bool     stereo;                     /* For the decoder identification. */
    char     ps[M2SDR_FM_RDS_PS_LEN];
    char     rt[M2SDR_FM_RDS_RT_LEN];
    unsigned rt_segments;                /* Groups 2A of one pass over the text. */
    float    pulse[FM_RDS_TABLE];
    double   phase, step;                /* Position in the bit, 0 to 1, and its step per sample. */
    float    sym[2 * FM_RDS_SPAN + 1];   /* Symbols around the current bit, +-1. */
    uint32_t blocks[4];                  /* Group being sent. */
    unsigned bit;                        /* Its next bit, 0 to 103. */
    unsigned prev;                       /* Last bit after the differential coding. */
    uint64_t bits;                       /* Bits sent. */
    unsigned groups, ps_seg, rt_seg;
};

static void fm_rds_tx_init(struct fm_rds_tx *e, const struct m2sdr_fm_rds_config *cfg, double rate,
                           bool stereo)
{
    size_t len;
    double peak = 0.0;
    int i, k;

    memset(e, 0, sizeof(*e));
    e->cfg    = *cfg;
    e->stereo = stereo;
    e->step   = M2SDR_FM_RDS_BITRATE / rate;

    /* Name padded with spaces. Text ended by a carriage return when shorter than its 64
     * characters, then padded to whole groups of four characters. */
    memset(e->ps, ' ', sizeof(e->ps));
    memcpy(e->ps, cfg->ps, strnlen(cfg->ps, sizeof(e->ps)));
    memset(e->rt, ' ', sizeof(e->rt));
    len = strnlen(cfg->rt, sizeof(e->rt));
    memcpy(e->rt, cfg->rt, len);
    if (len > 0 && len < sizeof(e->rt))
        e->rt[len++] = '\r';
    e->rt_segments = (unsigned)((len + 3) / 4);

    /* Table of the symbol, scaled so that a stream of identical bits, a sine at the bit rate,
     * has an amplitude of 1. */
    for (i = 0; i < FM_RDS_TABLE; i++)
        e->pulse[i] = (float)fm_rds_pulse((double)i / FM_RDS_RES - (FM_RDS_SPAN + 1));
    for (i = 0; i < FM_RDS_RES; i++) {
        double sum = 0.0;

        for (k = 0; k < 2 * FM_RDS_SPAN + 2; k++)
            sum += e->pulse[i + k * FM_RDS_RES];
        if (fabs(sum) > peak)
            peak = fabs(sum);
    }
    for (i = 0; i < FM_RDS_TABLE; i++)
        e->pulse[i] = (float)(e->pulse[i] / peak);
}

/* Next group: clock time when the group ends on a minute, else name and text in turn. */
static void fm_rds_tx_group(struct fm_rds_tx *e)
{
    const struct m2sdr_fm_rds_config *c = &e->cfg;
    const double start = (double)c->ct_unix_time + (double)e->bits / M2SDR_FM_RDS_BITRATE;
    const double end   = start + 104.0 / M2SDR_FM_RDS_BITRATE;
    uint16_t b[4];
    int i;

    b[0] = c->pi;
    b[1] = (uint16_t)((c->tp ? 0x0400 : 0) | ((c->pty & 31u) << 5));
    if (c->ct && floor(end / 60.0) > floor(start / 60.0)) {
        /* 4A: Modified Julian Day, hour and minute in UTC, local offset in half hours. */
        const int64_t  minutes = (int64_t)floor(end / 60.0);
        const int64_t  days    = (minutes >= 0) ? minutes / 1440 : -((-minutes + 1439) / 1440);
        const unsigned in_day  = (unsigned)(minutes - days * 1440);
        const uint32_t mjd     = (uint32_t)(days + FM_RDS_UNIX_MJD) & 0x1ffff;
        const unsigned hour    = in_day / 60, minute = in_day % 60;
        const unsigned offset  = (unsigned)abs(c->ct_offset) & 31u;

        b[1] |= (4u << 12) | (mjd >> 15);
        b[2]  = (uint16_t)(((mjd & 0x7fff) << 1) | (hour >> 4));
        b[3]  = (uint16_t)(((hour & 15u) << 12) | (minute << 6) |
                           (c->ct_offset < 0 ? 0x20 : 0) | offset);
    } else if (e->rt_segments && (e->groups & 1)) {
        /* 2A: four characters of the text, at their segment address. */
        const unsigned char *t = (const unsigned char *)&e->rt[4 * e->rt_seg];

        b[1] |= (2u << 12) | e->rt_seg;
        b[2]  = (uint16_t)((t[0] << 8) | t[1]);
        b[3]  = (uint16_t)((t[2] << 8) | t[3]);
        if (++e->rt_seg == e->rt_segments)
            e->rt_seg = 0;
    } else {
        /* 0A: flags, no alternative frequency, two characters of the name. The decoder
         * identification is sent one bit per group: its last bit, stereo, with address 3. */
        const unsigned char *t = (const unsigned char *)&e->ps[2 * e->ps_seg];

        b[1] |= (c->ta ? 0x10 : 0) | (c->music ? 0x08 : 0) | e->ps_seg;
        if (e->stereo && e->ps_seg == 3)
            b[1] |= 0x04;
        b[2]  = 0xe0cd;
        b[3]  = (uint16_t)((t[0] << 8) | t[1]);
        e->ps_seg = (e->ps_seg + 1) & 3;
    }
    e->groups++;

    e->blocks[0] = m2sdr_fm_rds_encode_block(b[0], M2SDR_FM_RDS_OFFSET_A);
    e->blocks[1] = m2sdr_fm_rds_encode_block(b[1], M2SDR_FM_RDS_OFFSET_B);
    e->blocks[2] = m2sdr_fm_rds_encode_block(b[2], M2SDR_FM_RDS_OFFSET_C);
    e->blocks[3] = m2sdr_fm_rds_encode_block(b[3], M2SDR_FM_RDS_OFFSET_D);
    for (i = 0; i < 4; i++)
        e->blocks[i] &= 0x3ffffff;
}

/* Next bit of the stream, differentially coded: a 1 inverts the previous output. */
static float fm_rds_tx_symbol(struct fm_rds_tx *e)
{
    unsigned bit;

    if (e->bit == 0)
        fm_rds_tx_group(e);
    bit = (e->blocks[e->bit / 26] >> (25 - e->bit % 26)) & 1;
    if (++e->bit == 104)
        e->bit = 0;
    e->bits++;
    e->prev ^= bit;
    return e->prev ? 1.0f : -1.0f;
}

/* One sample of the data signal, before the subcarrier: the symbols of the bits around the
 * current one, each at its place in the table. */
static inline float fm_rds_tx_sample(struct fm_rds_tx *e)
{
    const float  x = ((float)e->phase + 0.5f) * FM_RDS_RES;
    const int    k = (int)x;
    const float  f = x - (float)k;
    const float *p = e->pulse + k;
    float acc = 0.0f;
    int i;

    for (i = 0; i <= 2 * FM_RDS_SPAN; i++) {
        const float *q = p + (2 * FM_RDS_SPAN - i) * FM_RDS_RES;

        acc += e->sym[i] * (q[0] + f * (q[1] - q[0]));
    }
    e->phase += e->step;
    if (e->phase >= 1.0) {
        e->phase -= 1.0;
        memmove(e->sym, e->sym + 1, 2 * FM_RDS_SPAN * sizeof(e->sym[0]));
        e->sym[2 * FM_RDS_SPAN] = fm_rds_tx_symbol(e);
    }
    return acc;
}

/* RDS: decoder */
/*--------------*/

#define FM_RDS_RX_RATE_MIN  19000.0  /* Lowest rate of the data signal: 16 samples per bit. */
#define FM_RDS_PASS_HZ       2400.0  /* Data signal after the subcarrier is removed... */
#define FM_RDS_STOP_HZ       8000.0  /* ...and stop band of its decimation filter. */
#define FM_RDS_MF_SPAN            2  /* Bits on each side in the matched filter. */
#define FM_RDS_COSTAS_HZ       20.0  /* Natural frequency of the subcarrier loop. */
#define FM_RDS_POWER_BITS      50.0  /* Time constant of the level estimate. */
#define FM_RDS_TIMING_GAIN     0.05  /* Bit clock correction, in bits per unit of error. */
#define FM_RDS_SYNC_SPAN          3  /* Blocks between the two offset words that give the sync. */
#define FM_RDS_SYNC_LOSS         12  /* Wrong blocks in a row that end it. */
#define FM_RDS_HIST              32  /* Samples kept for the bit clock: over half a bit. */

struct m2sdr_fm_rds_rx {
    /* Subcarrier removed with a free 57 kHz phasor, then decimation. */
    double   nco_c, nco_s, step_c, step_s;
    float   *dec_taps;
    unsigned dec_count, decim, dec_phase;
    struct fm_delay dec_i, dec_q;

    /* Matched filter: the shaped biphase symbol. */
    float   *mf_taps;
    unsigned mf_count;
    struct fm_delay mf_i, mf_q;

    /* Subcarrier phase (Costas loop) and level. */
    double   psi, freq, kp, ki;
    float    power, power_alpha;

    /* Bit clock. */
    double   tau, tau_step;
    unsigned quarter;                    /* Samples in a quarter of a bit. */
    float    hist_re[FM_RDS_HIST], hist_im[FM_RDS_HIST], hist_mag[FM_RDS_HIST];
    unsigned hist_pos;

    /* Optional record of the bit decisions, see m2sdr_fm_rds_rx_points(). */
    float   *points;
    size_t   points_max, points_count;

    /* Bits, blocks and groups. */
    unsigned prev_bit;
    uint32_t reg;                        /* Last 26 bits. */
    uint64_t bit_count;
    bool     have_prev;                  /* An offset word was seen at prev_at, in block prev_block. */
    uint64_t prev_at;
    unsigned prev_block;
    unsigned block, bits_in_block, bad_run;
    bool     group_good, version_b;
    uint16_t data[4];

    /* Name and text being received. */
    char     ps[M2SDR_FM_RDS_PS_LEN];
    unsigned ps_mask;
    char     rt[M2SDR_FM_RDS_RT_LEN];
    unsigned rt_mask;
    int      rt_flag;

    struct m2sdr_fm_rds_info info;
};

static void fm_rds_rx_free(struct m2sdr_fm_rds_rx *d)
{
    if (!d)
        return;
    free(d->dec_taps);
    free(d->dec_i.hist);
    free(d->dec_q.hist);
    free(d->mf_taps);
    free(d->mf_i.hist);
    free(d->mf_q.hist);
    free(d);
}

static struct m2sdr_fm_rds_rx *fm_rds_rx_new(double mpx_rate)
{
    struct m2sdr_fm_rds_rx *d = calloc(1, sizeof(*d));
    double rate, spb, wn, energy = 0.0;
    unsigned i;

    if (!d)
        return NULL;
    d->nco_c  = 1.0;
    d->step_c = cos(2.0 * M_PI * M2SDR_FM_RDS_HZ / mpx_rate);
    d->step_s = sin(2.0 * M_PI * M2SDR_FM_RDS_HZ / mpx_rate);
    d->decim  = (unsigned)(mpx_rate / FM_RDS_RX_RATE_MIN);
    if (d->decim < 1)
        d->decim = 1;
    rate = mpx_rate / d->decim;
    spb  = rate / M2SDR_FM_RDS_BITRATE;

    d->dec_taps = fm_lowpass_design(mpx_rate, FM_RDS_PASS_HZ, FM_RDS_STOP_HZ, 1, &d->dec_count);
    if (!d->dec_taps || fm_delay_init(&d->dec_i, d->dec_count) != 0 ||
        fm_delay_init(&d->dec_q, d->dec_count) != 0)
        goto fail;

    d->mf_count = 2 * (unsigned)lrint(FM_RDS_MF_SPAN * spb) + 1;
    d->mf_taps  = malloc(d->mf_count * sizeof(*d->mf_taps));
    if (!d->mf_taps || fm_delay_init(&d->mf_i, d->mf_count) != 0 ||
        fm_delay_init(&d->mf_q, d->mf_count) != 0)
        goto fail;
    for (i = 0; i < d->mf_count; i++) {
        const double p = fm_rds_pulse(((double)i - (d->mf_count - 1) / 2.0) / spb);

        d->mf_taps[i] = (float)p;
        energy       += p * p;
    }
    for (i = 0; i < d->mf_count; i++)
        d->mf_taps[i] = (float)(d->mf_taps[i] / sqrt(energy));

    /* Second-order loop, damping 0.707; its phase detector has a unit slope. */
    wn     = 2.0 * M_PI * FM_RDS_COSTAS_HZ / rate;
    d->kp  = 2.0 * M_SQRT1_2 * wn;
    d->ki  = wn * wn;
    d->power_alpha = (float)(1.0 / (FM_RDS_POWER_BITS * spb));

    d->tau_step = 1.0 / spb;
    d->quarter  = (unsigned)lrint(spb / 4.0);
    if (d->quarter < 1 || 2 * d->quarter >= FM_RDS_HIST)
        goto fail;
    d->rt_flag = -1;
    return d;

fail:
    fm_rds_rx_free(d);
    return NULL;
}

/* Characters as received, the control codes as spaces. */
static void fm_rds_rx_chars(char *dst, const uint16_t *words, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        const unsigned c = (words[i / 2] >> ((i & 1) ? 0 : 8)) & 0xff;

        dst[i] = (char)((c < 0x20 && c != '\r') || c == 0x7f ? ' ' : c);
    }
}

/* Text: `width` characters per segment. It is complete with all its segments, or with those
 * up to the one that holds the carriage return. */
static void fm_rds_rx_text(struct m2sdr_fm_rds_rx *d, unsigned seg, unsigned width, int flag,
                           const uint16_t *words)
{
    const unsigned len = 16 * width;
    unsigned need = 16, end = len, i;
    char text[M2SDR_FM_RDS_RT_LEN + 1];

    /* The A/B flag changes with a new text. */
    if (flag != d->rt_flag) {
        memset(d->rt, ' ', sizeof(d->rt));
        d->rt_mask = 0;
        d->rt_flag = flag;
    }
    fm_rds_rx_chars(&d->rt[seg * width], words, width);
    d->rt_mask |= 1u << seg;

    for (i = 0; i < len; i++) {
        if (d->rt[i] == '\r') {
            end  = i;
            need = i / width + 1;
            break;
        }
    }
    if ((d->rt_mask & ((1u << need) - 1)) != (1u << need) - 1)
        return;
    while (end > 0 && d->rt[end - 1] == ' ')
        end--;
    memcpy(text, d->rt, end);
    text[end] = '\0';
    if (!d->info.rt_valid || strcmp(text, d->info.rt) != 0) {
        memcpy(d->info.rt, text, end + 1);
        d->info.rt_valid = true;
        d->info.updates++;
    }
}

/* A group with its four blocks right. */
static void fm_rds_rx_group(struct m2sdr_fm_rds_rx *d)
{
    struct m2sdr_fm_rds_info *info = &d->info;
    const uint16_t *b = d->data;
    const unsigned type = b[1] >> 12;
    const unsigned pty  = (b[1] >> 5) & 31;
    const bool     tp   = (b[1] >> 10) & 1;

    info->groups++;
    info->type_groups[2 * type + (d->version_b ? 1 : 0)]++;
    if (!info->pi_valid || info->pi != b[0]) {
        /* Another station: what was received so far is not its name nor its text. */
        info->pi       = b[0];
        info->pi_valid = true;
        info->ps_valid = info->rt_valid = info->ct_valid = false;
        d->ps_mask = d->rt_mask = 0;
        d->rt_flag = -1;
        info->updates++;
    }
    if (info->pty != pty || info->tp != tp) {
        info->pty = (uint8_t)pty;
        info->tp  = tp;
        info->updates++;
    }

    if (type == 0) {
        /* 0A, 0B: flags and two characters of the name. */
        const unsigned seg = b[1] & 3;
        const bool ta = (b[1] >> 4) & 1, music = (b[1] >> 3) & 1;

        if (info->ta != ta || info->music != music) {
            info->ta    = ta;
            info->music = music;
            info->updates++;
        }
        fm_rds_rx_chars(&d->ps[2 * seg], &b[3], 2);
        d->ps_mask |= 1u << seg;
        if (d->ps_mask == 0xf && (!info->ps_valid || memcmp(info->ps, d->ps, sizeof(d->ps)) != 0)) {
            memcpy(info->ps, d->ps, sizeof(d->ps));
            info->ps[sizeof(d->ps)] = '\0';
            info->ps_valid = true;
            info->updates++;
        }
    } else if (type == 2) {
        /* 2A: four characters of the text. 2B: two, the third block repeats the PI. */
        if (d->version_b)
            fm_rds_rx_text(d, b[1] & 15, 2, (b[1] >> 4) & 1, &b[3]);
        else
            fm_rds_rx_text(d, b[1] & 15, 4, (b[1] >> 4) & 1, &b[2]);
    } else if (type == 4 && !d->version_b) {
        /* 4A: clock time. */
        const uint32_t mjd    = ((uint32_t)(b[1] & 3) << 15) | (b[2] >> 1);
        const unsigned hour   = ((b[2] & 1u) << 4) | (b[3] >> 12);
        const unsigned minute = (b[3] >> 6) & 63;
        const int      offset = (b[3] & 0x20) ? -(int)(b[3] & 31) : (int)(b[3] & 31);

        if (hour < 24 && minute < 60) {
            info->ct_unix_time = ((int64_t)mjd - FM_RDS_UNIX_MJD) * 86400 + hour * 3600 + minute * 60;
            info->ct_offset    = offset;
            info->ct_valid     = true;
            info->updates++;
        }
    }
}

/* One received bit. Without sync, every bit is tried as the end of a block: two offset words
 * at the distance and in the order of the blocks give the sync. With it, a block ends every
 * 26 bits, and a run of wrong checkwords ends the sync. */
static void fm_rds_rx_bit(struct m2sdr_fm_rds_rx *d, unsigned bit)
{
    static const unsigned char block_of[5] = { 0, 1, 2, 2, 3 };
    uint16_t data = 0;
    int offset;

    d->reg = ((d->reg << 1) | bit) & 0x3ffffff;
    d->bit_count++;

    if (!d->info.synced) {
        unsigned block;

        offset = m2sdr_fm_rds_decode_block(d->reg, &data);
        if (offset < 0)
            return;
        block = block_of[offset];
        if (d->have_prev) {
            const uint64_t dist = d->bit_count - d->prev_at;

            if (dist % 26 == 0 && dist / 26 <= FM_RDS_SYNC_SPAN &&
                (d->prev_block + dist / 26) % 4 == block) {
                d->info.synced   = true;
                d->block         = (block + 1) & 3;
                d->bits_in_block = 0;
                d->bad_run       = 0;
                d->group_good    = false;   /* Until a first block of a group. */
            }
        }
        d->have_prev  = true;
        d->prev_at    = d->bit_count;
        d->prev_block = block;
        return;
    }

    if (++d->bits_in_block < 26)
        return;
    d->bits_in_block = 0;
    offset = m2sdr_fm_rds_decode_block(d->reg, &data);
    d->info.blocks++;
    if (offset >= 0 && block_of[offset] == d->block) {
        d->bad_run = 0;
        if (d->block == 0)
            d->group_good = true;
        if (d->block == 2)
            d->version_b = (offset == M2SDR_FM_RDS_OFFSET_CP);
        d->data[d->block] = data;
        /* The version bit of the second block and the offset word of the third agree. */
        if (d->block == 3 && d->group_good && d->version_b == (((d->data[1] >> 11) & 1) != 0))
            fm_rds_rx_group(d);
    } else {
        d->info.block_errors++;
        d->group_good = false;
        if (++d->bad_run >= FM_RDS_SYNC_LOSS) {
            d->info.synced = false;
            d->have_prev   = false;
        }
    }
    d->block = (d->block + 1) & 3;
}

/* One sample of the multiplex. */
static inline void fm_rds_rx_sample(struct m2sdr_fm_rds_rx *d, float mpx)
{
    const double c = d->nco_c, s = d->nco_s;
    const double norm = 1.5 - 0.5 * (c * c + s * s);
    const unsigned mask = FM_RDS_HIST - 1;
    float bi, bq, yi, yq, vr, vi, pc, ps, err, level;

    /* Subcarrier down to zero, low-pass, decimation. */
    fm_delay_push(&d->dec_i, mpx * (float)c);
    fm_delay_push(&d->dec_q, -mpx * (float)s);
    d->nco_c = (c * d->step_c - s * d->step_s) * norm;
    d->nco_s = (s * d->step_c + c * d->step_s) * norm;
    if (++d->dec_phase < d->decim)
        return;
    d->dec_phase = 0;
    bi = fm_dot(d->dec_taps, fm_delay_window(&d->dec_i), d->dec_count);
    bq = fm_dot(d->dec_taps, fm_delay_window(&d->dec_q), d->dec_count);

    /* Matched filter. */
    fm_delay_push(&d->mf_i, bi);
    fm_delay_push(&d->mf_q, bq);
    yi = fm_dot(d->mf_taps, fm_delay_window(&d->mf_i), d->mf_count);
    yq = fm_dot(d->mf_taps, fm_delay_window(&d->mf_q), d->mf_count);

    /* Subcarrier phase: the data are on the real axis when the product of the two axes
     * averages to zero. The inversion that remains does not matter to the differential
     * coding. */
    pc = cosf((float)d->psi);
    ps = sinf((float)d->psi);
    vr = yi * pc + yq * ps;
    vi = yq * pc - yi * ps;
    d->power += d->power_alpha * (vr * vr + vi * vi - d->power);
    level = d->power + 1e-20f;
    err   = vr * vi / level;
    if (err > 1.0f)
        err = 1.0f;
    else if (err < -1.0f)
        err = -1.0f;
    d->freq += d->ki * err;
    d->psi  += d->freq + d->kp * err;
    if (d->psi > M_PI)
        d->psi -= 2.0 * M_PI;
    else if (d->psi < -M_PI)
        d->psi += 2.0 * M_PI;

    /* Bit clock: the matched filter output peaks in the middle of a bit. The sample taken is
     * a quarter of a bit old, and the levels a quarter of a bit before and after it steer
     * the clock. */
    d->hist_pos = (d->hist_pos + 1) & mask;
    d->hist_re[d->hist_pos]  = vr;
    d->hist_im[d->hist_pos]  = vi;
    d->hist_mag[d->hist_pos] = sqrtf(vr * vr + vi * vi);
    d->tau += d->tau_step;
    if (d->tau >= 1.0) {
        const float late   = d->hist_mag[d->hist_pos];
        const float early  = d->hist_mag[(d->hist_pos - 2 * d->quarter) & mask];
        const unsigned at  = (d->hist_pos - d->quarter) & mask;
        const unsigned bit = d->hist_re[at] > 0.0f;

        err = (late - early) / sqrtf(level);
        if (err > 1.0f)
            err = 1.0f;
        else if (err < -1.0f)
            err = -1.0f;
        d->tau -= 1.0 + FM_RDS_TIMING_GAIN * err;
        if (d->points && d->info.synced && d->points_count < d->points_max) {
            const float scale = 1.0f / sqrtf(level);

            d->points[2 * d->points_count + 0] = d->hist_re[at] * scale;
            d->points[2 * d->points_count + 1] = d->hist_im[at] * scale;
            d->points_count++;
        }
        fm_rds_rx_bit(d, bit ^ d->prev_bit);
        d->prev_bit = bit;
    }
}

struct m2sdr_fm_rds_rx *m2sdr_fm_rds_rx_new(double mpx_rate)
{
    if (!(mpx_rate >= M2SDR_FM_MIN_RATE_RDS && mpx_rate <= 1e9))
        return NULL;
    return fm_rds_rx_new(mpx_rate);
}

void m2sdr_fm_rds_rx_free(struct m2sdr_fm_rds_rx *d)
{
    fm_rds_rx_free(d);
}

void m2sdr_fm_rds_rx_process(struct m2sdr_fm_rds_rx *d, const float *mpx, size_t count)
{
    size_t n;

    for (n = 0; n < count; n++)
        fm_rds_rx_sample(d, mpx[n]);
}

void m2sdr_fm_rds_rx_info(const struct m2sdr_fm_rds_rx *d, struct m2sdr_fm_rds_info *info)
{
    *info = d->info;
}

void m2sdr_fm_rds_rx_restart(struct m2sdr_fm_rds_rx *d)
{
    /* Filters, loops and bit clock from zero, as for a new decoder; the decoded fields, the
     * counts and the name and text being received stay. */
    memset(d->dec_i.hist, 0, 2 * d->dec_count * sizeof(*d->dec_i.hist));
    memset(d->dec_q.hist, 0, 2 * d->dec_count * sizeof(*d->dec_q.hist));
    memset(d->mf_i.hist, 0, 2 * d->mf_count * sizeof(*d->mf_i.hist));
    memset(d->mf_q.hist, 0, 2 * d->mf_count * sizeof(*d->mf_q.hist));
    d->dec_i.pos  = d->dec_q.pos = d->mf_i.pos = d->mf_q.pos = 0;
    d->nco_c      = 1.0;
    d->nco_s      = 0.0;
    d->dec_phase  = 0;
    d->psi        = d->freq = 0.0;
    d->power      = 0.0f;
    d->tau        = 0.0;
    memset(d->hist_re, 0, sizeof(d->hist_re));
    memset(d->hist_im, 0, sizeof(d->hist_im));
    memset(d->hist_mag, 0, sizeof(d->hist_mag));
    d->hist_pos   = 0;
    d->prev_bit   = 0;
    d->reg        = 0;
    d->have_prev  = false;
    d->group_good = false;
    d->info.synced = false;
}

void m2sdr_fm_rds_rx_points(struct m2sdr_fm_rds_rx *d, float *points, size_t max)
{
    d->points       = points;
    d->points_max   = points ? max : 0;
    d->points_count = 0;
}

size_t m2sdr_fm_rds_rx_point_count(const struct m2sdr_fm_rds_rx *d)
{
    return d->points_count;
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

    /* Pilot: a unit phasor turned by a fixed step. The RDS subcarrier is its third harmonic. */
    double   pilot_c, pilot_s;
    double   step_c, step_s;
    float    audio_level;        /* Share of the deviation left to the audio. */
    struct fm_rds_tx *rds;

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
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate = 1e6;
    cfg->deviation   = M2SDR_FM_DEVIATION_HZ;
    cfg->tau         = M2SDR_FM_TAU_EU;
    cfg->stereo      = false;
    cfg->bits        = 12;
}

static bool fm_config_valid(const struct m2sdr_fm_config *cfg)
{
    double min_rate = cfg->stereo ? M2SDR_FM_MIN_RATE_STEREO : M2SDR_FM_MIN_RATE_MONO;

    if (cfg->rds.enable) {
        if (min_rate < M2SDR_FM_MIN_RATE_RDS)
            min_rate = M2SDR_FM_MIN_RATE_RDS;
        if (cfg->rds.pty > 31 || abs(cfg->rds.ct_offset) > 31)
            return false;
    }
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
    free(tx->rds);
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

    /* The pilot and the RDS subcarrier take their share of the deviation from the audio. */
    tx->audio_level = 1.0f;
    if (cfg->stereo)
        tx->audio_level -= (float)M2SDR_FM_PILOT_LEVEL;
    if (cfg->rds.enable) {
        tx->audio_level -= (float)M2SDR_FM_RDS_LEVEL;
        tx->rds = malloc(sizeof(*tx->rds));
        if (!tx->rds)
            goto fail;
        fm_rds_tx_init(tx->rds, &cfg->rds, tx->audio_rate, cfg->stereo);
    }

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
    const float audio_level = tx->audio_level;
    size_t n;
    unsigned m;

    for (n = 0; n < frames; n++) {
        const double c = tx->pilot_c, s = tx->pilot_s;
        float mpx;

        if (tx->channels == 2) {
            const float l = fm_tx_audio(tx, 0, audio[2 * n + 0]);
            const float r = fm_tx_audio(tx, 1, audio[2 * n + 1]);

            /* The L-R subcarrier is sin(2 * phase) for a pilot sin(phase): both cross zero
             * upwards together. */
            mpx = audio_level * (0.5f * (l + r) + 0.5f * (l - r) * (float)(2.0 * s * c)) +
                  (float)M2SDR_FM_PILOT_LEVEL * (float)s;
        } else {
            mpx = audio_level * fm_tx_audio(tx, 0, audio[n]);
        }
        /* RDS: the data signal on sin(3 * phase), in phase with the third harmonic of the
         * pilot. In mono the pilot is not sent, and the subcarrier is a plain 57 kHz. */
        if (tx->rds)
            mpx += (float)M2SDR_FM_RDS_LEVEL * fm_rds_tx_sample(tx->rds) *
                   (float)(s * (3.0 - 4.0 * s * s));
        if (tx->channels == 2 || tx->rds) {
            const double norm = 1.5 - 0.5 * (c * c + s * s);

            tx->pilot_c = (c * tx->step_c - s * tx->step_s) * norm;
            tx->pilot_s = (s * tx->step_c + c * tx->step_s) * norm;
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
    float    audio_gain;         /* Back to full scale: the audio shares the deviation. */

    struct m2sdr_fm_rds_rx *rds;
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
    fm_rds_rx_free(rx->rds);
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

    /* A signal with RDS leaves its share less to the audio; the pilot is counted once
     * locked. */
    rx->audio_gain = 1.0f;
    if (cfg->rds.enable) {
        rx->audio_gain = 1.0f / (1.0f - (float)M2SDR_FM_RDS_LEVEL);
        rx->rds = fm_rds_rx_new(rx->mpx_rate);
        if (!rx->rds)
            goto fail;
    }
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

void m2sdr_fm_rx_rds(const struct m2sdr_fm_rx *rx, struct m2sdr_fm_rds_info *info)
{
    if (rx->rds)
        *info = rx->rds->info;
    else
        memset(info, 0, sizeof(*info));
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

    if (rx->rds)
        fm_rds_rx_sample(rx->rds, mpx);

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
    y *= rx->audio_gain;
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
        const float level = 1.0f - (float)M2SDR_FM_PILOT_LEVEL -
                            (rx->rds ? (float)M2SDR_FM_RDS_LEVEL : 0.0f);

        y /= rx->audio_gain;
        audio[0] = (y + m) / level;
        audio[1] = (y - m) / level;
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
