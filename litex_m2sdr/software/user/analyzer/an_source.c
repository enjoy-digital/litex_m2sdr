/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: sources of the samples, the RX of a board or a capture file.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <complex.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "m2sdr.h"
#include "m2sdr_sigmf.h"

#include "analyzer.h"

/* Source */
/*--------*/

static int source_raw_reserve(struct an_source *src, size_t bytes)
{
    void *grown;

    if (bytes <= src->raw_bytes)
        return 0;
    grown = realloc(src->raw, bytes);
    if (!grown)
        return -1;
    src->raw       = grown;
    src->raw_bytes = bytes;
    return 0;
}

/* Interleaved I/Q to complex, full scale 1.0. Unlike m2sdr_cal_iq_to_complex(), the mean is
 * kept: the DC offset is one of the things measured. */
static void raw_to_complex(const void *raw, size_t count, int sample_bytes, int nbits, int stride,
                           int channel, float complex *x)
{
    const float scale = 1.0f / (float)(1 << (nbits - 1));
    size_t i;

    if (sample_bytes == 2) {
        const int16_t *p = (const int16_t *)raw + 2 * channel;
        for (i = 0; i < count; i++, p += stride)
            x[i] = (p[0] + I * p[1]) * scale;
    } else {
        const int8_t *p = (const int8_t *)raw + 2 * channel;
        for (i = 0; i < count; i++, p += stride)
            x[i] = (p[0] + I * p[1]) * scale;
    }
}

/* TX of a stimulus */

/* A thread sends the period again and again; m2sdr_sync_tx() blocks on the DMA (or on the flow
 * control of Ethernet), so it runs at the sample rate. The RX of the captures goes on beside it,
 * from the worker. */
struct an_tx {
    pthread_t thread;
    volatile int stop;
    int16_t  *iq;              /* The period, 2T2R: both channels, I and Q, SC16 Q11. */
    size_t    period;
    uint64_t  hash;            /* Of the stimulus: a new one restarts the thread. */
    int       att;
    bool      configured;      /* TX stream set up. */
    bool      running;
    char      err[128];
};

static uint64_t stimulus_hash(const float complex *x, size_t period)
{
    const unsigned char *p = (const unsigned char *)x;
    uint64_t h = 1469598103934665603ull ^ period;
    size_t i;

    for (i = 0; i < period * sizeof(*x); i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void *tx_main(void *arg)
{
    struct an_source *src = arg;
    struct an_tx *tx = src->tx;
    const unsigned per_buf = m2sdr_bytes_to_samples(M2SDR_FORMAT_SC16_Q11, M2SDR_BUFFER_BYTES);
    const size_t pairs = 2 * tx->period;          /* I/Q pairs of a period, both channels. */
    int16_t *chunk = malloc((size_t)per_buf * 2 * sizeof(*chunk));
    size_t pos = 0;

    if (!chunk) {
        snprintf(tx->err, sizeof(tx->err), "out of memory");
        return NULL;
    }
    while (!tx->stop) {
        unsigned i;
        int rc;

        for (i = 0; i < per_buf; i++) {
            chunk[2 * i]     = tx->iq[2 * pos];
            chunk[2 * i + 1] = tx->iq[2 * pos + 1];
            if (++pos == pairs)
                pos = 0;
        }
        rc = m2sdr_sync_tx(src->dev, chunk, per_buf, NULL, 1000);
        if (rc != M2SDR_ERR_OK && rc != M2SDR_ERR_TIMEOUT) {
            snprintf(tx->err, sizeof(tx->err), "TX failed: %s", m2sdr_strerror(rc));
            break;
        }
    }
    free(chunk);
    return NULL;
}

bool an_source_tx_running(const struct an_source *src)
{
    return src->tx && src->tx->running;
}

void an_source_tx_stop(struct an_source *src)
{
    struct an_tx *tx = src->tx;

    if (!tx || !tx->running)
        return;
    tx->stop = 1;
    pthread_join(tx->thread, NULL);
    tx->running = false;
    /* Nothing is left on the air: the attenuation at its highest, then the stream off. */
    (void)m2sdr_set_tx_att(src->dev, 89);
    (void)m2sdr_stream_deactivate(src->dev, M2SDR_TX);
    tx->configured = false;
}

int an_source_tx_start(struct an_source *src, const float complex *x, size_t period, int att, char *err, size_t err_len)
{
    const unsigned per_buf = m2sdr_bytes_to_samples(M2SDR_FORMAT_SC16_Q11, M2SDR_BUFFER_BYTES);
    const uint64_t hash = x ? stimulus_hash(x, period) : 0;
    struct an_tx *tx;
    size_t i;
    int rc;

    if (!src->live || !src->dev || !x || period < 2 || att < 0 || att > 89) {
        snprintf(err, err_len, "no TX on this source");
        return -1;
    }
    if (src->no_rf_init || src->tx_tone) {
        snprintf(err, err_len, "the lab modes transmit: not with --no-rf-init or --tx-tone");
        return -1;
    }
    if (!src->tx && !(src->tx = calloc(1, sizeof(*src->tx)))) {
        snprintf(err, err_len, "out of memory");
        return -1;
    }
    tx = src->tx;
    if (tx->running && tx->hash == hash && tx->period == period) {
        if (att != tx->att) {
            rc = m2sdr_set_tx_att(src->dev, att);
            if (rc != M2SDR_ERR_OK) {
                snprintf(err, err_len, "TX attenuation refused: %s", m2sdr_strerror(rc));
                return -1;
            }
            tx->att = att;
            usleep(AN_SETTLE_US);
        }
        return 0;
    }
    an_source_tx_stop(src);

    /* SC16 Q11, the same samples on TX1 and TX2: whichever the cable is on. */
    free(tx->iq);
    tx->iq = malloc(period * 4 * sizeof(*tx->iq));
    if (!tx->iq) {
        snprintf(err, err_len, "out of memory");
        return -1;
    }
    for (i = 0; i < period; i++) {
        const long re = lrintf(fminf(fmaxf(crealf(x[i]), -1.0f), 1.0f) * 2047.0f);
        const long im = lrintf(fminf(fmaxf(cimagf(x[i]), -1.0f), 1.0f) * 2047.0f);
        tx->iq[4 * i]     = tx->iq[4 * i + 2] = (int16_t)re;
        tx->iq[4 * i + 1] = tx->iq[4 * i + 3] = (int16_t)im;
    }
    tx->period = period;
    tx->hash   = hash;
    tx->err[0] = '\0';

    rc = m2sdr_set_tx_frequency(src->dev, (uint64_t)llround(src->applied.freq_hz));
    if (rc == M2SDR_ERR_OK)
        rc = m2sdr_set_tx_att(src->dev, att);
    if (rc != M2SDR_ERR_OK) {
        snprintf(err, err_len, "TX setup refused: %s", m2sdr_strerror(rc));
        return -1;
    }
    tx->att = att;
    if (!tx->configured) {
        rc = m2sdr_sync_config(src->dev, M2SDR_TX, M2SDR_FORMAT_SC16_Q11, 0, per_buf, 0, 1000);
        if (rc != M2SDR_ERR_OK) {
            snprintf(err, err_len, "TX stream configuration failed: %s", m2sdr_strerror(rc));
            return -1;
        }
        tx->configured = true;
    }
    tx->stop = 0;
    if (pthread_create(&tx->thread, NULL, tx_main, src) != 0) {
        snprintf(err, err_len, "no TX thread");
        return -1;
    }
    tx->running = true;
    /* The stimulus through the TX pipeline, and the RX filters settled on it. */
    usleep(AN_SETTLE_US);
    return 0;
}

/* The stimulus of the modes on (the first one that has one), started, changed or stopped. */
static int live_tx_update(struct an_source *src, const struct an_settings *set, char *err, size_t err_len)
{
    int m;

    for (m = 0; an_modes[m]; m++) {
        const struct an_mode *mode = an_modes[m];
        float complex *x;
        size_t period = 0;
        int rc;

        if (!mode->stimulus || !an_mode_on(mode, set))
            continue;
        x = mode->stimulus(set, &period);
        if (!x) {
            snprintf(err, err_len, "%s: no stimulus for these settings", mode->title);
            return -1;
        }
        rc = an_source_tx_start(src, x, period, set->lab.tx_att, err, err_len);
        free(x);
        return rc;
    }
    an_source_tx_stop(src);
    return 0;
}

/* Board */

static int64_t default_bandwidth(double sample_rate)
{
    /* The analog filter of the AD9361 goes from 200 kHz to 56 MHz. */
    double bw = 0.9 * sample_rate;

    if (bw < 200e3)
        bw = 200e3;
    if (bw > 56e6)
        bw = 56e6;
    return (int64_t)bw;
}

/* Samples lost by the stream so far: a capture during which this moves has a hole. */
static uint64_t live_loss_count(struct m2sdr_dev *dev)
{
    struct m2sdr_stream_stats stats;
    struct m2sdr_liteeth_udp_stats udp;
    uint64_t lost = 0;

    memset(&stats, 0, sizeof(stats));
    if (m2sdr_get_stream_stats(dev, M2SDR_RX, &stats) == M2SDR_ERR_OK)
        lost += stats.overflow_events;
    memset(&udp, 0, sizeof(udp));
    if (m2sdr_liteeth_get_udp_stats(dev, &udp) == M2SDR_ERR_OK)
        lost += udp.rx_kernel_drops + udp.rx_source_drops + udp.rx_frame_resyncs + udp.rx_ring_full_events;
    return lost;
}

static int live_apply(struct an_source *src, const struct an_settings *set, char *err, size_t err_len)
{
    const unsigned per_buf = m2sdr_bytes_to_samples(M2SDR_FORMAT_SC16_Q11, M2SDR_BUFFER_BYTES);
    const int64_t bandwidth = set->bandwidth > 0 ? (int64_t)set->bandwidth : default_bandwidth(set->sample_rate);
    struct an_settings *now = &src->applied;
    bool changed = false;
    int rc;

    if (src->no_rf_init) {
        /* Only the stream: frequency, rate and gain are the ones of the utility that set the
         * RFIC, and the settings here only label the results. */
        if (!src->configured) {
            rc = m2sdr_sync_config(src->dev, M2SDR_RX, M2SDR_FORMAT_SC16_Q11, 0, per_buf, 0, 2000);
            if (rc != M2SDR_ERR_OK) {
                snprintf(err, err_len, "RX stream configuration failed: %s", m2sdr_strerror(rc));
                return -1;
            }
            src->configured = true;
        }
        *now = *set;
        return 0;
    }
    if (!src->configured) {
        struct m2sdr_config cfg;

        m2sdr_config_init(&cfg);
        cfg.sample_rate    = (int64_t)set->sample_rate;
        cfg.bandwidth      = bandwidth;
        cfg.rx_freq        = (int64_t)llround(set->freq_hz);
        cfg.tx_att         = 89;              /* The analyser only receives, */
        if (src->tx_tone) {
            /* unless asked for its test tone: the built-in tone of the RFIC, a thirty-second of
             * the sample rate above the centre, the TX synthesizer on the RX frequency. */
            cfg.tx_att         = src->tx_att;
            cfg.tx_freq        = (int64_t)llround(set->freq_hz);
            cfg.bist_tx_tone   = true;
            cfg.bist_tone_freq = (int32_t)(set->sample_rate / 32.0);
        }
        cfg.refclk_ppm     = src->trim_ppm;
        cfg.channel_layout = M2SDR_CHANNEL_LAYOUT_2T2R;
        if (src->refclk_freq)
            cfg.refclk_freq = src->refclk_freq;
        if (src->sync) {
            if (strcmp(src->sync, "internal") == 0) {
                cfg.clock_source = M2SDR_CLOCK_SOURCE_INTERNAL;
            } else if (strcmp(src->sync, "external") == 0) {
                cfg.clock_source = M2SDR_CLOCK_SOURCE_EXTERNAL;
            } else if (strcmp(src->sync, "fpga") == 0) {
                cfg.clock_source = M2SDR_CLOCK_SOURCE_SI5351C_FPGA;
            } else {
                snprintf(err, err_len, "invalid sync '%s' (expected internal, external or fpga)", src->sync);
                return -1;
            }
        }
        if (set->agc) {
            cfg.program_rx_gains      = false;
            cfg.program_rx_gain_modes = true;
            cfg.rx_gain_mode1 = cfg.rx_gain_mode2 = M2SDR_RX_GAIN_MODE_SLOW_ATTACK_AGC;
        } else {
            cfg.rx_gain1 = cfg.rx_gain2 = set->rx_gain;
        }
        rc = m2sdr_apply_config(src->dev, &cfg);
        if (rc != M2SDR_ERR_OK) {
            snprintf(err, err_len, "RF configuration failed: %s", m2sdr_strerror(rc));
            return -1;
        }
        rc = m2sdr_sync_config(src->dev, M2SDR_RX, M2SDR_FORMAT_SC16_Q11, 0, per_buf, 0, 2000);
        if (rc != M2SDR_ERR_OK) {
            snprintf(err, err_len, "RX stream configuration failed: %s", m2sdr_strerror(rc));
            return -1;
        }
        *now            = *set;
        now->bandwidth  = (double)bandwidth;
        src->configured = true;
        usleep(AN_SETTLE_US);
        return 0;
    }

    if (set->sample_rate != now->sample_rate) {
        rc = m2sdr_set_sample_rate(src->dev, (int64_t)set->sample_rate);
        if (rc != M2SDR_ERR_OK) {
            snprintf(err, err_len, "sample rate %.0f refused: %s", set->sample_rate, m2sdr_strerror(rc));
            return -1;
        }
        now->sample_rate = set->sample_rate;
        changed = true;
    }
    if ((double)bandwidth != now->bandwidth) {
        rc = m2sdr_set_bandwidth(src->dev, bandwidth);
        if (rc != M2SDR_ERR_OK) {
            snprintf(err, err_len, "bandwidth %.0f refused: %s", (double)bandwidth, m2sdr_strerror(rc));
            return -1;
        }
        now->bandwidth = (double)bandwidth;
        changed = true;
    }
    if (set->freq_hz != now->freq_hz) {
        rc = m2sdr_set_rx_frequency(src->dev, (uint64_t)llround(set->freq_hz));
        if (rc == M2SDR_ERR_OK && (src->tx_tone || an_source_tx_running(src)))
            rc = m2sdr_set_tx_frequency(src->dev, (uint64_t)llround(set->freq_hz));
        if (rc != M2SDR_ERR_OK) {
            snprintf(err, err_len, "frequency %.0f refused: %s", set->freq_hz, m2sdr_strerror(rc));
            return -1;
        }
        now->freq_hz = set->freq_hz;
        changed = true;
    }
    if (set->agc != now->agc || (!set->agc && set->rx_gain != now->rx_gain)) {
        if (set->agc)
            rc = m2sdr_set_rx_gain_mode_all(src->dev, M2SDR_RX_GAIN_MODE_SLOW_ATTACK_AGC);
        else
            rc = m2sdr_set_rx_gain_mode_all(src->dev, M2SDR_RX_GAIN_MODE_MANUAL);
        if (rc == M2SDR_ERR_OK && !set->agc)
            rc = m2sdr_set_rx_gain(src->dev, set->rx_gain);
        if (rc != M2SDR_ERR_OK) {
            snprintf(err, err_len, "RX gain refused: %s", m2sdr_strerror(rc));
            return -1;
        }
        now->agc     = set->agc;
        now->rx_gain = set->rx_gain;
        changed = true;
    }
    if (changed)
        usleep(AN_SETTLE_US);
    return 0;
}

static int live_capture(struct an_source *src, const struct an_settings *set, float complex *x, float complex *x2,
                        size_t count, char *err, size_t err_len)
{
    const unsigned per_buf = m2sdr_bytes_to_samples(M2SDR_FORMAT_SC16_Q11, M2SDR_BUFFER_BYTES);
    const size_t bufs = (count * 2 + per_buf - 1) / per_buf;   /* 2T2R: two channels per period. */
    int16_t *raw;
    int attempt, rc;
    size_t b;

    if (live_apply(src, set, err, err_len) != 0)
        return -1;
    if (live_tx_update(src, set, err, err_len) != 0)
        return -1;
    if (src->tx && src->tx->running && src->tx->err[0]) {
        snprintf(err, err_len, "%s", src->tx->err);
        return -1;
    }
    if (source_raw_reserve(src, bufs * per_buf * 2 * sizeof(int16_t)) != 0) {
        snprintf(err, err_len, "out of memory");
        return -1;
    }
    raw = src->raw;
    for (attempt = 0; ; attempt++) {
        uint64_t lost;

        (void)m2sdr_sync_rx_flush(src->dev);
        lost = live_loss_count(src->dev);
        for (b = 0; b < bufs; b++) {
            struct m2sdr_metadata meta = { 0 };

            rc = m2sdr_sync_rx(src->dev, raw + b * per_buf * 2, per_buf, b == 0 ? &meta : NULL, 2000);
            if (rc != M2SDR_ERR_OK) {
                snprintf(err, err_len, "RX failed: %s", m2sdr_strerror(rc));
                return -1;
            }
            /* Time of the capture: the timestamp of its first buffer, else the host clock at its
             * arrival less its duration (two channels per sample period). */
            if (b == 0 && (meta.flags & M2SDR_META_FLAG_HAS_TIME)) {
                src->capture.t_s   = 1e-9 * (double)meta.timestamp;
                src->capture.clock = AN_CLOCK_BOARD;
            } else if (b == 0) {
                src->capture.t_s   = 1e-3 * now_ms() - 0.5 * per_buf / set->sample_rate;
                src->capture.clock = AN_CLOCK_HOST;
            }
        }
        if (live_loss_count(src->dev) == lost)
            break;
        if (attempt + 1 >= AN_CAPTURE_RETRIES) {
            /* The capture is analysed all the same, and reported as having a hole. */
            src->lost++;
            snprintf(err, err_len, "samples lost in the capture: the host or the link does not follow %.2f MSPS",
                     set->sample_rate / 1e6);
            raw_to_complex(raw, count, 2, 12, 4, set->channel, x);
            if (x2)
                raw_to_complex(raw, count, 2, 12, 4, 1 - set->channel, x2);
            return 1;
        }
    }
    raw_to_complex(raw, count, 2, 12, 4, set->channel, x);
    /* The other channel of the same sample periods: 2T2R interleaves them. */
    if (x2)
        raw_to_complex(raw, count, 2, 12, 4, 1 - set->channel, x2);
    return 0;
}

/* The stimulus of the lab mode on, one period, to a file: SC16 Q11, the same samples on two
 * channels, as m2sdr_play sends them; and what the tests feed their devices with. */
int lab_stimulus_write(const struct an_settings *set, const char *path)
{
    int m;

    for (m = 0; an_modes[m]; m++) {
        const struct an_mode *mode = an_modes[m];
        float complex *x;
        size_t period = 0, i;
        FILE *f;
        int rc = 0;

        if (!mode->stimulus || !an_mode_on(mode, set))
            continue;
        x = mode->stimulus(set, &period);
        if (!x)
            return -1;
        f = fopen(path, "wb");
        if (!f) {
            free(x);
            return -1;
        }
        for (i = 0; i < period && rc == 0; i++) {
            const int16_t re = (int16_t)lrintf(fminf(fmaxf(crealf(x[i]), -1.0f), 1.0f) * 2047.0f);
            const int16_t im = (int16_t)lrintf(fminf(fmaxf(cimagf(x[i]), -1.0f), 1.0f) * 2047.0f);
            const int16_t v[4] = { re, im, re, im };
            if (fwrite(v, sizeof(v), 1, f) != 1)
                rc = -1;
        }
        free(x);
        if (fclose(f) != 0)
            rc = -1;
        return rc;
    }
    return -1;
}

/* File */

int file_open(struct an_source *src, const char *path, int capture_index, char *err, size_t err_len)
{
    struct m2sdr_sigmf_meta *meta = calloc(1, sizeof(*meta));
    const char *data_path = path;
    uint64_t size, start = 0, end;
    int rc = -1;

    if (!meta) {
        snprintf(err, err_len, "out of memory");
        return -1;
    }
    if (m2sdr_sigmf_read(path, meta) == 0) {
        const enum m2sdr_format format = m2sdr_sigmf_format_from_datatype(meta->datatype);
        unsigned header_bytes = meta->has_header_bytes ? meta->header_bytes : 0;

        if (format == M2SDR_FORMAT_SC16_Q11) {
            src->sample_bytes = 2;
        } else if (format == M2SDR_FORMAT_SC8_Q7) {
            src->sample_bytes = 1;
            src->nbits        = 8;
        } else {
            snprintf(err, err_len, "unsupported SigMF datatype '%s'", meta->datatype);
            goto out;
        }
        if (meta->has_sample_rate)
            src->file_rate = meta->sample_rate;
        if (meta->has_center_freq)
            src->file_freq = meta->center_freq;
        if (meta->has_num_channels)
            src->nchannels = (int)meta->num_channels;
        if (meta->capture_count > 0) {
            const struct m2sdr_sigmf_capture *cap;

            if (capture_index < 0 || (unsigned)capture_index >= meta->capture_count) {
                snprintf(err, err_len, "capture %d out of range (the file has %u)", capture_index, meta->capture_count);
                goto out;
            }
            cap = &meta->captures[capture_index];
            if (cap->has_center_freq)
                src->file_freq = cap->center_freq;
            if (cap->has_header_bytes)
                header_bytes = cap->header_bytes;
            if (m2sdr_sigmf_capture_byte_range(meta, (unsigned)capture_index, format, header_bytes,
                                               (unsigned)src->frame_size, &start, &end) != 0)
                start = 0;
        }
        if (header_bytes == AN_FRAME_HEADER_BYTES) {
            src->frame_header = true;
        } else if (header_bytes != 0) {
            snprintf(err, err_len, "unsupported SigMF header of %u bytes", header_bytes);
            goto out;
        }
        data_path = meta->data_path;
    }

    src->file = fopen(data_path, "rb");
    if (!src->file) {
        snprintf(err, err_len, "%.180s: %s", data_path, strerror(errno));
        goto out;
    }
    if (fseeko(src->file, 0, SEEK_END) != 0) {
        snprintf(err, err_len, "%.180s: %s", data_path, strerror(errno));
        goto out;
    }
    size = (uint64_t)ftello(src->file);
    if (start > size)
        start = 0;
    src->data_start = start;
    size -= start;
    if (src->frame_header) {
        const uint64_t payload = src->frame_size - AN_FRAME_HEADER_BYTES;
        const uint64_t frames  = size / src->frame_size;
        const uint64_t tail    = size % src->frame_size;
        size = frames * payload + (tail > AN_FRAME_HEADER_BYTES ? tail - AN_FRAME_HEADER_BYTES : 0);
    }
    src->total_sets = size / ((uint64_t)src->nchannels * 2u * (uint64_t)src->sample_bytes);
    snprintf(src->name, sizeof(src->name), "%.1023s", data_path);
    rc = 0;

out:
    free(meta);
    return rc;
}

/* `bytes` of samples from sample byte `offset`, the frame headers skipped. */
static int file_read(struct an_source *src, uint64_t offset, uint8_t *dst, size_t bytes)
{
    if (!src->frame_header) {
        if (fseeko(src->file, (off_t)(src->data_start + offset), SEEK_SET) != 0)
            return -1;
        return fread(dst, 1, bytes, src->file) == bytes ? 0 : -1;
    }
    while (bytes) {
        const uint64_t payload = src->frame_size - AN_FRAME_HEADER_BYTES;
        const uint64_t in_frame = offset % payload;
        size_t n = (size_t)(payload - in_frame);

        if (n > bytes)
            n = bytes;
        if (fseeko(src->file, (off_t)(src->data_start + (offset / payload) * src->frame_size +
                                      AN_FRAME_HEADER_BYTES + in_frame), SEEK_SET) != 0)
            return -1;
        if (fread(dst, 1, n, src->file) != n)
            return -1;
        dst    += n;
        offset += n;
        bytes  -= n;
    }
    return 0;
}

static int file_capture(struct an_source *src, const struct an_settings *set, float complex *x, float complex *x2,
                        size_t count, char *err, size_t err_len)
{
    const size_t set_bytes = (size_t)src->nchannels * 2u * (size_t)src->sample_bytes;

    if (x2 && src->nchannels < 2) {
        snprintf(err, err_len, "the file has one channel: the modes of two channels need RX1 and RX2 (--nchannels 2)");
        return -1;
    }

    if (count > src->total_sets) {
        snprintf(err, err_len, "the file holds %llu samples, the capture needs %zu: lower the FFT length, the averages or the samples of the measurement",
                 (unsigned long long)src->total_sets, count);
        return -1;
    }
    if (src->position + count > src->total_sets) {
        if (!src->loop) {
            snprintf(err, err_len, "end of the file");
            return -1;
        }
        src->position = 0;
    }
    if (source_raw_reserve(src, count * set_bytes) != 0) {
        snprintf(err, err_len, "out of memory");
        return -1;
    }
    if (file_read(src, src->position * set_bytes, src->raw, count * set_bytes) != 0) {
        snprintf(err, err_len, "read error in %.200s", src->name);
        return -1;
    }
    raw_to_complex(src->raw, count, src->sample_bytes, src->nbits, 2 * src->nchannels,
                   set->channel < src->nchannels ? set->channel : 0, x);
    src->capture.t_s   = (double)src->position / set->sample_rate;
    src->capture.clock = AN_CLOCK_SAMPLES;
    if (x2)
        raw_to_complex(src->raw, count, src->sample_bytes, src->nbits, 2 * src->nchannels, set->channel ? 0 : 1, x2);
    src->position += count;
    return 0;
}

/* One capture of `count` samples with the settings given. Returns 0, 1 when samples were lost
 * in it (err says so, x is filled), -1 on an error. */
int source_capture(struct an_source *src, const struct an_settings *set, float complex *x,
                   size_t count, char *err, size_t err_len)
{
    return source_capture_two(src, set, x, NULL, count, err, err_len);
}

int source_capture_two(struct an_source *src, const struct an_settings *set, float complex *x, float complex *x2,
                       size_t count, char *err, size_t err_len)
{
    err[0] = '\0';
    return src->live ? live_capture(src, set, x, x2, count, err, err_len)
                     : file_capture(src, set, x, x2, count, err, err_len);
}

void source_close(struct an_source *src)
{
    if (src->dev) {
        an_source_tx_stop(src);
        (void)m2sdr_stream_deactivate(src->dev, M2SDR_RX);
        /* The test tone is not left on the air. */
        if (src->tx_tone && src->configured)
            (void)m2sdr_set_tx_att(src->dev, 89);
        m2sdr_close(src->dev);
    }
    if (src->file)
        fclose(src->file);
    if (src->tx)
        free(src->tx->iq);
    free(src->tx);
    free(src->raw);
}
