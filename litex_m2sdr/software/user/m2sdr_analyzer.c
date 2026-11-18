/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility.
 *
 * Spectrum, markers, channel power and time-domain measurements on the RX of a board or on a
 * capture file. With SDL2 and cimgui it opens a Dear ImGui window; --report prints the
 * measurements of one capture without a display.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <complex.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef M2SDR_ANALYZER_GUI
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include "scan_ui/m2sdr_plot.h"
#include "scan_ui/imgui_sdl_gl3_bridge.h"
#include "scan_ui/m2sdr_colormaps_gqrx.h"
#define igGetIO igGetIO_Nil
#endif

#include "m2sdr.h"
#include "m2sdr_analysis.h"
#include "m2sdr_cal.h"
#include "m2sdr_cli.h"
#include "m2sdr_sigmf.h"

#define AN_DEFAULT_FREQ_HZ     2400e6
#define AN_DEFAULT_SAMPLE_RATE 7680000.0
#define AN_DEFAULT_FFT_LEN     16384
#define AN_DEFAULT_AVERAGES    16
#define AN_MIN_FFT_LEN         256
#define AN_MAX_FFT_LEN         (1 << 20)
#define AN_MAX_AVERAGES        1024
#define AN_TIME_POINTS         8192          /* Samples kept for the time views. */
#define AN_MAX_PEAKS           8
#define AN_PEAK_EXCURSION_DB   6.0
#define AN_PEAK_MIN_ABOVE_DB    10.0          /* A peak or a tone stands this far above the noise in a bin. */
#define AN_SETTLE_US           30000         /* After a change of the RFIC. */
#define AN_CAPTURE_RETRIES     3             /* Captures redone when samples were lost. */
#define AN_FRAME_HEADER_BYTES  16
#define AN_PN_DEFAULT_LOG2      20            /* Samples of a phase noise capture: 2^20. */
#define AN_PN_MIN_LOG2          16
#define AN_PN_MAX_LOG2          23
#define AN_PN_FLOOR_MARGIN_DB   3.0           /* A reading this close to the floor is the floor. */
#define AN_DEMOD_DEFAULT_LOG2   18            /* Samples of a demodulation capture: 2^18. */
#define AN_DEMOD_MIN_LOG2       14
#define AN_DEMOD_MAX_LOG2       22
#define AN_LTE_SECONDS          0.045         /* Capture of an LTE analysis: a whole frame wherever it starts. */
#define AN_TX_TONE_ATT_DB       60            /* Default attenuation of the test tone. */
#define AN_MIN_PERIOD_MS        40.0          /* Between two results shown. */

/* Settings */
/*----------*/

/* What a capture and its analysis depend on. The UI edits a copy, the worker applies it. */
struct an_settings {
    double freq_hz;                /* Centre of the capture. */
    double sample_rate;
    double bandwidth;              /* RF bandwidth; 0: from the sample rate. */
    bool   agc;
    int    rx_gain;                /* Manual gain, dB. */
    int    channel;                /* 0: RX1, 1: RX2. */
    int    fft_len;
    int    window;                 /* enum m2sdr_window. */
    int    averages;               /* Segments averaged in one capture. */
    double chan_offset_hz;         /* Channel measured: its centre from the centre of the capture. */
    double chan_bw_hz;             /* 0: no channel measurement. */
    double chan_spacing_hz;        /* 0: the bandwidth. */
    double ref_offset_db;          /* Added to the levels: dBFS to dBm at the connector. */
    bool   pn_on;                  /* Phase noise of the strongest carrier. */
    int    pn_log2;                /* Its capture: 2^pn_log2 samples. */
    double pn_lo_hz, pn_hi_hz;     /* Integration limits; 0: the offsets measured. */
    bool   demod_on;               /* Demodulation of the strongest signal. */
    int    demod_log2;             /* Its capture: 2^demod_log2 samples. */
    struct m2sdr_demod_config demod;
    bool   lte_on;                 /* LTE downlink carrier. */
};

static const char *const window_names[] = { "Hann", "Flat-top", "Blackman-Harris", "Rectangular" };

/* Source */
/*--------*/

struct an_source {
    bool   live;
    char   name[1024];
    /* Board. */
    struct m2sdr_dev *dev;
    double trim_ppm;
    const char *sync;
    int64_t refclk_freq;
    bool   no_rf_init;             /* The RFIC is left as another utility set it. */
    bool   tx_tone;                /* Test tone on TX, from the RFIC itself. */
    int    tx_att;
    uint64_t dna;
    struct an_settings applied;    /* What the RFIC is set to. */
    bool   configured;
    /* File. */
    FILE  *file;
    int    sample_bytes;           /* 1: SC8, 2: SC16. */
    int    nbits;
    int    nchannels;
    bool   frame_header;
    size_t frame_size;
    uint64_t data_start;           /* Bytes. */
    uint64_t total_sets;           /* Sample periods in the file. */
    uint64_t position;             /* Next sample period read. */
    bool   loop;
    double file_rate;
    double file_freq;
    /* Buffer of raw samples. */
    void  *raw;
    size_t raw_bytes;
    unsigned lost;                 /* Captures with samples lost, since the start. */
};

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
        if (rc == M2SDR_ERR_OK && src->tx_tone)
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

static int live_capture(struct an_source *src, const struct an_settings *set, float complex *x,
                        size_t count, char *err, size_t err_len)
{
    const unsigned per_buf = m2sdr_bytes_to_samples(M2SDR_FORMAT_SC16_Q11, M2SDR_BUFFER_BYTES);
    const size_t bufs = (count * 2 + per_buf - 1) / per_buf;   /* 2T2R: two channels per period. */
    int16_t *raw;
    int attempt, rc;
    size_t b;

    if (live_apply(src, set, err, err_len) != 0)
        return -1;
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
            rc = m2sdr_sync_rx(src->dev, raw + b * per_buf * 2, per_buf, NULL, 2000);
            if (rc != M2SDR_ERR_OK) {
                snprintf(err, err_len, "RX failed: %s", m2sdr_strerror(rc));
                return -1;
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
            return 1;
        }
    }
    raw_to_complex(raw, count, 2, 12, 4, set->channel, x);
    return 0;
}

/* File */

static int file_open(struct an_source *src, const char *path, int capture_index, char *err, size_t err_len)
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

static int file_capture(struct an_source *src, const struct an_settings *set, float complex *x,
                        size_t count, char *err, size_t err_len)
{
    const size_t set_bytes = (size_t)src->nchannels * 2u * (size_t)src->sample_bytes;

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
    src->position += count;
    return 0;
}

/* One capture of `count` samples with the settings given. Returns 0, 1 when samples were lost
 * in it (err says so, x is filled), -1 on an error. */
static int source_capture(struct an_source *src, const struct an_settings *set, float complex *x,
                          size_t count, char *err, size_t err_len)
{
    err[0] = '\0';
    return src->live ? live_capture(src, set, x, count, err, err_len)
                     : file_capture(src, set, x, count, err, err_len);
}

static void source_close(struct an_source *src)
{
    if (src->dev) {
        (void)m2sdr_stream_deactivate(src->dev, M2SDR_RX);
        /* The test tone is not left on the air. */
        if (src->tx_tone && src->configured)
            (void)m2sdr_set_tx_att(src->dev, 89);
        m2sdr_close(src->dev);
    }
    if (src->file)
        fclose(src->file);
    free(src->raw);
}

/* Analysis */
/*----------*/

struct an_result {
    struct an_settings set;        /* Settings of the capture. */
    struct m2sdr_psd psd;
    struct m2sdr_psd tone_psd;     /* Worker only: spectrum behind the tone figures. */
    struct m2sdr_iq_stats stats;
    struct m2sdr_tone tone;
    bool   tone_valid;
    struct m2sdr_psd_peak peaks[AN_MAX_PEAKS];
    int    npeaks;
    bool   chan_valid;
    double chan_power_db;
    struct m2sdr_acpr acpr;
    bool   acpr_valid;
    double obw_lo_hz, obw_hi_hz;
    bool   obw_valid;
    double noise_db_hz;
    struct m2sdr_phase_noise pn;
    bool   pn_valid;               /* Measured, with a carrier. */
    struct m2sdr_demod_result demod;
    bool   demod_valid;            /* Measured; demod.signal and demod.locked say what was found. */
    struct m2sdr_lte_result lte;
    bool   lte_valid;              /* Measured: the sample rate was one of LTE; lte.found says the rest. */
    float complex time[AN_TIME_POINTS];
    size_t time_count;
    bool   lost;                   /* Samples were lost in the capture. */
};

static size_t capture_samples(const struct an_settings *set)
{
    size_t count = (size_t)set->fft_len * ((size_t)set->averages + 1) / 2;

    if (count < (size_t)set->fft_len)
        count = (size_t)set->fft_len;
    if (set->pn_on && count < ((size_t)1 << set->pn_log2))
        count = (size_t)1 << set->pn_log2;
    if (set->demod_on && count < ((size_t)1 << set->demod_log2))
        count = (size_t)1 << set->demod_log2;
    if (set->lte_on && count < (size_t)(AN_LTE_SECONDS * set->sample_rate))
        count = (size_t)(AN_LTE_SECONDS * set->sample_rate);
    return count < AN_TIME_POINTS ? AN_TIME_POINTS : count;
}

static int analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const double half = 0.5 * set->sample_rate;

    res->set = *set;
    if (m2sdr_psd_compute(x, count, set->sample_rate, (size_t)set->fft_len, (enum m2sdr_window)set->window, &res->psd) != 0)
        return -1;
    (void)m2sdr_iq_stats(x, count, 0.999, &res->stats);
    res->noise_db_hz = m2sdr_psd_noise_density_db(&res->psd, -half, half);
    /* Peaks and tone: only what stands out of the noise. On a wide signal, the highest bins are
     * its own ripple. */
    {
        const double floor_db = res->noise_db_hz + 10.0 * log10(res->psd.rbw_hz) + AN_PEAK_MIN_ABOVE_DB;
        int n = m2sdr_psd_peaks(&res->psd, -half, half, AN_PEAK_EXCURSION_DB, res->peaks, AN_MAX_PEAKS);

        while (n > 0 && res->peaks[n - 1].power_db < floor_db)
            n--;
        res->npeaks     = n;
        struct m2sdr_psd_peak top;

        res->tone_valid = m2sdr_psd_peak(&res->psd, -half, half, &top) == 0 && top.power_db >= floor_db;
        if (res->tone_valid) {
            /* The skirts of a Hann or flat-top window would count as spurs and noise next to a
             * strong tone: its figures are taken with a Blackman-Harris window, whatever the
             * one of the trace. */
            const struct m2sdr_psd *psd = &res->psd;

            if (set->window != M2SDR_WINDOW_BLACKMAN_HARRIS) {
                psd = &res->tone_psd;
                if (m2sdr_psd_compute(x, count, set->sample_rate, (size_t)set->fft_len,
                                      M2SDR_WINDOW_BLACKMAN_HARRIS, &res->tone_psd) != 0)
                    psd = NULL;
            }
            res->tone_valid = psd && m2sdr_psd_tone(psd, &res->tone) == 0;
        }
    }

    res->chan_valid = res->acpr_valid = res->obw_valid = false;
    if (set->chan_bw_hz > 0) {
        const double lo = set->chan_offset_hz - 0.5 * set->chan_bw_hz;
        const double hi = set->chan_offset_hz + 0.5 * set->chan_bw_hz;
        const double spacing = set->chan_spacing_hz > 0 ? set->chan_spacing_hz : set->chan_bw_hz;

        if (lo >= -half && hi <= half) {
            res->chan_valid    = true;
            res->chan_power_db = m2sdr_psd_band_power_db(&res->psd, lo, hi);
            res->acpr_valid    = m2sdr_psd_acpr(&res->psd, set->chan_offset_hz, set->chan_bw_hz, spacing, &res->acpr) == 0;
            /* The occupied bandwidth is searched in the channel and half of it on each side. */
            res->obw_valid     = m2sdr_psd_obw(&res->psd, lo - 0.5 * set->chan_bw_hz, hi + 0.5 * set->chan_bw_hz,
                                               0.99, &res->obw_lo_hz, &res->obw_hi_hz) == 0;
        }
    } else {
        res->obw_valid = m2sdr_psd_obw(&res->psd, -half, half, 0.99, &res->obw_lo_hz, &res->obw_hi_hz) == 0;
    }
    res->pn_valid = false;
    if (set->pn_on) {
        const size_t n = count < ((size_t)1 << set->pn_log2) ? count : (size_t)1 << set->pn_log2;

        res->pn_valid = m2sdr_phase_noise(x, n, set->sample_rate, set->freq_hz, set->pn_lo_hz, set->pn_hi_hz,
                                          &res->pn) == 0 && res->pn.found;
        /* The jitter needs the RF frequency of the carrier, known once it is found. */
        if (res->pn_valid && set->freq_hz + res->pn.carrier_hz > 0)
            res->pn.jitter_s = res->pn.rms_rad / (2.0 * M_PI * (set->freq_hz + res->pn.carrier_hz));
    }
    res->demod_valid = false;
    if (set->demod_on) {
        const size_t n = count < ((size_t)1 << set->demod_log2) ? count : (size_t)1 << set->demod_log2;

        res->demod_valid = m2sdr_demod(x, n, set->sample_rate, &set->demod, &res->demod) == 0;
    }
    res->lte_valid = false;
    if (set->lte_on) {
        const size_t n = (size_t)(AN_LTE_SECONDS * set->sample_rate);

        res->lte_valid = m2sdr_lte_analyse(x, count < n ? count : n, set->sample_rate, &res->lte) == 0;
    }
    res->time_count = count < AN_TIME_POINTS ? count : AN_TIME_POINTS;
    memcpy(res->time, x, res->time_count * sizeof(*x));
    return 0;
}

/* Phase noise floor */
/*-------------------*/

/* Phase noise read with a source cleaner than the board, or with its own test tone: what the
 * board adds. Kept per board, in a text file. */
struct an_floor {
    int    points;
    double offset_hz[M2SDR_PN_MAX_POINTS];
    double l_dbc_hz[M2SDR_PN_MAX_POINTS];
    double freq_hz, sample_rate;   /* Of the capture it comes from. */
    char   path[1024];
};

static int floor_default_path(uint64_t dna, char *path, size_t len)
{
    char cal[1024], *slash;

    if (m2sdr_cal_default_path(dna, cal, sizeof(cal)) != 0)
        return -1;
    slash = strrchr(cal, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    return snprintf(path, len, "%.900s/pn_floor_%014llx.csv", cal, (unsigned long long)dna) < (int)len ? 0 : -1;
}

static int floor_load(struct an_floor *fl, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];

    fl->points = 0;
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        double a, b;

        if (sscanf(line, "# freq_hz=%lf sample_rate=%lf", &a, &b) == 2) {
            fl->freq_hz     = a;
            fl->sample_rate = b;
        } else if (sscanf(line, "%lf,%lf", &a, &b) == 2 && a > 0 && fl->points < M2SDR_PN_MAX_POINTS) {
            fl->offset_hz[fl->points] = a;
            fl->l_dbc_hz[fl->points]  = b;
            fl->points++;
        }
    }
    fclose(f);
    snprintf(fl->path, sizeof(fl->path), "%.1023s", path);
    return fl->points >= 2 ? 0 : -1;
}

static int floor_save(struct an_floor *fl, const char *path, const struct an_result *res)
{
    char dir[1024], *p;
    FILE *f;
    int i;

    /* Directories of the path. */
    if (snprintf(dir, sizeof(dir), "%s", path) < (int)sizeof(dir)) {
        for (p = dir + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                (void)mkdir(dir, 0755);
                *p = '/';
            }
        }
    }
    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "# M2SDR phase noise floor: offset_hz,l_dbc_hz\n");
    fprintf(f, "# freq_hz=%.0f sample_rate=%.0f\n", res->set.freq_hz + res->pn.carrier_hz, res->set.sample_rate);
    for (i = 0; i < res->pn.points; i++)
        fprintf(f, "%.3f,%.3f\n", res->pn.offset_hz[i], res->pn.l_dbc_hz[i]);
    if (fclose(f) != 0)
        return -1;
    return floor_load(fl, path);
}

/* Value of a trace of phase noise at an offset, between its points; NAN out of them. */
static double pn_interp(const double *offset_hz, const double *value, int points, double at_hz)
{
    int i;

    if (points < 2 || at_hz < offset_hz[0] || at_hz > offset_hz[points - 1])
        return NAN;
    for (i = 1; i < points; i++) {
        if (offset_hz[i] >= at_hz) {
            const double t = log(at_hz / offset_hz[i - 1]) / log(offset_hz[i] / offset_hz[i - 1]);
            return value[i - 1] + t * (value[i] - value[i - 1]);
        }
    }
    return NAN;
}

static const double pn_spots[] = { 100.0, 1e3, 1e4, 1e5, 1e6, 1e7 };
#define PN_SPOTS ((int)(sizeof(pn_spots) / sizeof(pn_spots[0])))

/* Report */
/*--------*/

static void print_json_number(const char *name, double value, bool last)
{
    if (isfinite(value))
        printf("    \"%s\": %.6f%s\n", name, value, last ? "" : ",");
    else
        printf("    \"%s\": null%s\n", name, last ? "" : ",");
}

static void report_json(const struct an_source *src, const struct an_result *res, const struct an_floor *fl)
{
    const struct an_settings *set = &res->set;
    const double off = set->ref_offset_db;
    int i;

    printf("{\n");
    printf("  \"source\": {\n");
    printf("    \"kind\": \"%s\",\n", src->live ? "board" : "file");
    print_json_number("center_hz", set->freq_hz, false);
    print_json_number("sample_rate", set->sample_rate, false);
    printf("    \"channel\": %d,\n", set->channel + 1);
    printf("    \"samples_lost\": %s\n", res->lost ? "true" : "false");
    printf("  },\n");
    printf("  \"spectrum\": {\n");
    printf("    \"fft_len\": %d,\n", set->fft_len);
    printf("    \"window\": \"%s\",\n", window_names[set->window]);
    printf("    \"segments\": %u,\n", res->psd.segments);
    print_json_number("rbw_hz", res->psd.rbw_hz, false);
    print_json_number("noise_db_hz", res->noise_db_hz + off, false);
    printf("    \"peaks\": [");
    for (i = 0; i < res->npeaks; i++)
        printf("%s{\"freq_hz\": %.3f, \"offset_hz\": %.3f, \"level_db\": %.3f}", i ? ", " : "",
               set->freq_hz + res->peaks[i].freq_hz, res->peaks[i].freq_hz, res->peaks[i].power_db + off);
    printf("]\n");
    printf("  },\n");
    if (res->tone_valid) {
        printf("  \"tone\": {\n");
        print_json_number("freq_hz", set->freq_hz + res->tone.freq_hz, false);
        print_json_number("offset_hz", res->tone.freq_hz, false);
        print_json_number("level_db", res->tone.level_db + off, false);
        print_json_number("snr_db", res->tone.snr_db, false);
        print_json_number("sinad_db", res->tone.sinad_db, false);
        print_json_number("thd_dbc", res->tone.thd_dbc, false);
        print_json_number("sfdr_dbc", res->tone.sfdr_dbc, false);
        print_json_number("spur_offset_hz", res->tone.spur_freq_hz, false);
        printf("    \"harmonics_dbc\": [");
        for (i = 0; i < M2SDR_TONE_HARMONICS; i++)
            printf("%s%.3f", i ? ", " : "", res->tone.harmonic_dbc[i]);
        printf("]\n");
        printf("  },\n");
    }
    printf("  \"channel\": {\n");
    if (res->chan_valid) {
        print_json_number("offset_hz", set->chan_offset_hz, false);
        print_json_number("bandwidth_hz", set->chan_bw_hz, false);
        print_json_number("power_db", res->chan_power_db + off, false);
        if (res->acpr_valid) {
            printf("    \"acpr_lower_dbc\": [");
            for (i = 0; i < res->acpr.adjacent; i++)
                printf("%s%.3f", i ? ", " : "", res->acpr.lower_dbc[i]);
            printf("],\n    \"acpr_upper_dbc\": [");
            for (i = 0; i < res->acpr.adjacent; i++)
                printf("%s%.3f", i ? ", " : "", res->acpr.upper_dbc[i]);
            printf("],\n");
        }
    }
    if (res->obw_valid) {
        print_json_number("obw_hz", res->obw_hi_hz - res->obw_lo_hz, false);
        print_json_number("obw_center_offset_hz", 0.5 * (res->obw_lo_hz + res->obw_hi_hz), true);
    } else {
        printf("    \"obw_hz\": null\n");
    }
    printf("  },\n");
    printf("  \"time\": {\n");
    print_json_number("mean_db", res->stats.mean_db + off, false);
    print_json_number("peak_db", res->stats.peak_db + off, false);
    print_json_number("papr_db", res->stats.papr_db, false);
    print_json_number("dc_db", res->stats.dc_db + off, false);
    print_json_number("gain_imbalance_db", res->stats.gain_imbalance_db, false);
    print_json_number("quadrature_deg", res->stats.quadrature_deg, false);
    print_json_number("clipped", res->stats.clipped, true);
    printf("  }%s\n", set->pn_on || set->demod_on || set->lte_on ? "," : "");
    if (set->lte_on) {
        const struct m2sdr_lte_result *l = &res->lte;
        static const char *const phich[4] = { "1/6", "1/2", "1", "2" };

        printf("  \"lte\": {\n");
        printf("    \"found\": %s%s\n", res->lte_valid && l->found ? "true" : "false", res->lte_valid && l->found ? "," : "");
        if (res->lte_valid && l->found) {
            printf("    \"cell_id\": %d,\n", l->cell_id);
            print_json_number("carrier_hz", set->freq_hz + l->freq_error_hz, false);
            print_json_number("carrier_offset_hz", l->freq_error_hz, false);
            printf("    \"mib\": %s,\n", l->mib ? "true" : "false");
            if (l->mib) {
                printf("    \"ports\": %d,\n", l->ports);
                printf("    \"n_rb\": %d,\n", l->n_rb);
                printf("    \"sfn\": %d,\n", l->sfn);
                printf("    \"phich_duration\": \"%s\",\n", l->phich_duration ? "extended" : "normal");
                printf("    \"phich_resource\": \"%s\",\n", phich[l->phich_resource & 3]);
            }
            printf("    \"analysed_rb\": %d,\n", l->analysed_rb);
            print_json_number("rsrp_db", l->rsrp_db + off, false);
            print_json_number("rssi_db", l->rssi_db + off, false);
            print_json_number("pss_db", l->pss_db + off, false);
            print_json_number("sss_db", l->sss_db + off, false);
            print_json_number("rs_evm_percent", 100.0 * l->rs_evm, false);
            if (l->mib)
                print_json_number("pbch_evm_percent", 100.0 * l->pbch_evm, false);
            printf("    \"data_analysed\": %s", l->data ? "true" : "false");
            if (l->data) {
                printf(",\n    \"data_blocks\": %d,\n", l->data_blocks);
                if (l->ports > 1) {
                    printf("    \"data_blocks_diversity\": %d,\n", l->data_diversity);
                    printf("    \"data_blocks_precoded\": %d,\n", l->data_precoded);
                    printf("    \"data_blocks_not_read\": %d,\n", l->data_unknown);
                }
                printf("    \"data_modulation\": \"%s\",\n", m2sdr_modulation_name(l->data_modulation));
                print_json_number("data_evm_percent", 100.0 * l->data_evm, true);
            } else {
                printf("\n");
            }
        }
        printf("  }%s\n", set->pn_on || set->demod_on ? "," : "");
    }
    if (set->demod_on) {
        const struct m2sdr_demod_result *d = &res->demod;

        printf("  \"modulation\": {\n");
        printf("    \"signal\": %s,\n", res->demod_valid && d->signal ? "true" : "false");
        printf("    \"locked\": %s%s\n", res->demod_valid && d->locked ? "true" : "false",
               res->demod_valid && d->signal ? "," : "");
        if (res->demod_valid && d->signal) {
            printf("    \"type\": \"%s\",\n", m2sdr_modulation_name(d->modulation));
            print_json_number("symbol_rate", d->symbol_rate, false);
            print_json_number("rolloff", d->rolloff, false);
            print_json_number("carrier_hz", set->freq_hz + d->freq_offset_hz, false);
            print_json_number("carrier_offset_hz", d->freq_offset_hz, false);
            print_json_number("power_db", d->power_db + off, false);
            printf("    \"symbols\": %zu,\n", d->symbols);
            print_json_number("evm_rms_percent", 100.0 * d->evm_rms, false);
            print_json_number("evm_peak_percent", 100.0 * d->evm_peak, false);
            print_json_number("mer_db", d->mer_db, false);
            print_json_number("magnitude_error_percent", 100.0 * d->mag_err_rms, false);
            print_json_number("phase_error_deg", d->phase_err_deg, false);
            print_json_number("origin_offset_db", d->iq_offset_db, false);
            print_json_number("gain_imbalance_db", d->gain_imbalance_db, false);
            print_json_number("quadrature_deg", d->quadrature_deg, true);
        }
        printf("  }%s\n", set->pn_on ? "," : "");
    }
    if (set->pn_on) {
        const struct m2sdr_phase_noise *pn = &res->pn;

        printf("  \"phase_noise\": {\n");
        printf("    \"found\": %s,\n", res->pn_valid ? "true" : "false");
        if (res->pn_valid) {
            print_json_number("carrier_hz", set->freq_hz + pn->carrier_hz, false);
            print_json_number("carrier_db", pn->carrier_db + off, false);
            print_json_number("integrated_from_hz", pn->lo_hz, false);
            print_json_number("integrated_to_hz", pn->hi_hz, false);
            print_json_number("rms_deg", pn->rms_deg, false);
            print_json_number("jitter_s", pn->jitter_s, false);
            print_json_number("residual_fm_hz", pn->residual_fm_hz, false);
            printf("    \"offsets_hz\": [");
            for (i = 0; i < pn->points; i++)
                printf("%s%.3f", i ? ", " : "", pn->offset_hz[i]);
            printf("],\n    \"l_dbc_hz\": [");
            for (i = 0; i < pn->points; i++)
                printf("%s%.3f", i ? ", " : "", pn->l_dbc_hz[i]);
            printf("],\n    \"am_dbc_hz\": [");
            for (i = 0; i < pn->points; i++)
                printf("%s%.3f", i ? ", " : "", pn->am_dbc_hz[i]);
            printf("],\n    \"floor_dbc_hz\": [");
            for (i = 0; fl && fl->points && i < pn->points; i++) {
                const double v = pn_interp(fl->offset_hz, fl->l_dbc_hz, fl->points, pn->offset_hz[i]);
                if (isfinite(v))
                    printf("%s%.3f", i ? ", " : "", v);
                else
                    printf("%snull", i ? ", " : "");
            }
            printf("],\n    \"spurs\": [");
            for (i = 0; i < pn->nspurs; i++)
                printf("%s{\"offset_hz\": %.3f, \"level_dbc\": %.3f}", i ? ", " : "", pn->spur_hz[i], pn->spur_dbc[i]);
            printf("]\n");
        } else {
            printf("    \"carrier_hz\": null\n");
        }
        printf("  }\n");
    }
    printf("}\n");
}

static void report_text(const struct an_source *src, const struct an_result *res, const struct an_floor *fl)
{
    const struct an_settings *set = &res->set;
    const double off = set->ref_offset_db;
    const char *unit = off != 0.0 ? "dBm" : "dBFS";
    int i;

    printf("Source     : %s, centre %.6f MHz, %.3f MSPS, RX%d\n", src->live ? "board" : src->name,
           set->freq_hz / 1e6, set->sample_rate / 1e6, set->channel + 1);
    printf("Spectrum   : FFT %d, %s window, RBW %.1f Hz, %u segments averaged\n",
           set->fft_len, window_names[set->window], res->psd.rbw_hz, res->psd.segments);
    if (res->lost)
        printf("Warning    : samples were lost in the capture; the figures are not reliable.\n");
    printf("Noise      : %.1f %s/Hz (median of the span)\n", res->noise_db_hz + off, unit);

    printf("\nPeaks (%.0f dB above the noise of a bin):\n", AN_PEAK_MIN_ABOVE_DB);
    for (i = 0; i < res->npeaks; i++)
        printf("  %14.6f MHz  %+12.1f Hz  %8.2f %s\n", (set->freq_hz + res->peaks[i].freq_hz) / 1e6,
               res->peaks[i].freq_hz, res->peaks[i].power_db + off, unit);
    if (!res->npeaks)
        printf("  none\n");

    if (res->tone_valid) {
        printf("\nStrongest line, as a tone:\n");
        printf("  Frequency  : %.6f MHz (%+.1f Hz from the centre)\n", (set->freq_hz + res->tone.freq_hz) / 1e6, res->tone.freq_hz);
        printf("  Level      : %.2f %s\n", res->tone.level_db + off, unit);
        printf("  SNR        : %.1f dB, SINAD %.1f dB\n", res->tone.snr_db, res->tone.sinad_db);
        printf("  SFDR       : %.1f dBc (other line at %+.1f Hz)\n", res->tone.sfdr_dbc, res->tone.spur_freq_hz);
        printf("  THD        : %.1f dBc; harmonics 2 to %d:", res->tone.thd_dbc, M2SDR_TONE_HARMONICS + 1);
        for (i = 0; i < M2SDR_TONE_HARMONICS; i++)
            printf(" %.1f", res->tone.harmonic_dbc[i]);
        printf(" dBc\n");
    } else {
        printf("\nNo line stands out of the noise: no tone figures.\n");
    }

    printf("\nChannel:\n");
    if (res->chan_valid) {
        printf("  Channel    : %.3f kHz wide at %+.3f kHz\n", set->chan_bw_hz / 1e3, set->chan_offset_hz / 1e3);
        printf("  Power      : %.2f %s\n", res->chan_power_db + off, unit);
        for (i = 0; res->acpr_valid && i < res->acpr.adjacent; i++)
            printf("  ACPR %d     : lower %.1f dBc, upper %.1f dBc\n", i + 1, res->acpr.lower_dbc[i], res->acpr.upper_dbc[i]);
    } else if (set->chan_bw_hz > 0) {
        printf("  The channel does not fit in the span.\n");
    }
    if (res->obw_valid)
        printf("  OBW (99 %%)  : %.3f kHz, centred at %+.3f kHz\n", (res->obw_hi_hz - res->obw_lo_hz) / 1e3,
               0.5 * (res->obw_lo_hz + res->obw_hi_hz) / 1e3);

    printf("\nTime domain:\n");
    printf("  Mean power : %.2f %s, peak %.2f %s, PAPR %.2f dB\n", res->stats.mean_db + off, unit,
           res->stats.peak_db + off, unit, res->stats.papr_db);
    printf("  DC offset  : %.1f %s (I %+.5f, Q %+.5f)\n", res->stats.dc_db + off, unit, res->stats.dc_i, res->stats.dc_q);
    printf("  I/Q        : gain imbalance %+.3f dB, quadrature error %+.3f degrees\n",
           res->stats.gain_imbalance_db, res->stats.quadrature_deg);
    printf("  Clipping   : %.4f %% of the samples\n", 100.0 * res->stats.clipped);

    if (set->lte_on) {
        const struct m2sdr_lte_result *l = &res->lte;
        static const char *const phich[4] = { "1/6", "1/2", "1", "2" };

        printf("\nLTE downlink:\n");
        if (!res->lte_valid) {
            printf("  The sample rate must be 1.92, 3.84, 7.68, 15.36 or 30.72 MSPS.\n");
        } else if (!l->found) {
            printf("  No cell found (PSS quality %.1f): the capture is tuned on the centre of an FDD carrier\n"
                   "  with a normal cyclic prefix.\n", l->sync_quality);
        } else {
            printf("  Cell       : identity %d (group %d, sector %d)\n", l->cell_id, l->n_id_1, l->n_id_2);
            printf("  Carrier    : %.6f MHz (%+.1f Hz from the centre)\n", (set->freq_hz + l->freq_error_hz) / 1e6, l->freq_error_hz);
            if (l->mib)
                printf("  MIB        : %d resource blocks, %d antenna port%s, frame %d, PHICH %s, %s\n", l->n_rb, l->ports,
                       l->ports == 1 ? "" : "s", l->sfn, l->phich_duration ? "extended" : "normal", phich[l->phich_resource & 3]);
            else
                printf("  MIB        : not decoded (the PBCH of one and of two antenna ports was tried)\n");
            printf("  Analysed   : %d resource blocks around the centre\n", l->analysed_rb);
            printf("  Levels     : RSRP %.1f %s, RSSI %.1f %s, PSS %.1f, SSS %.1f %s per element\n", l->rsrp_db + off, unit,
                   l->rssi_db + off, unit, l->pss_db + off, l->sss_db + off, unit);
            printf("  EVM        : reference signals %.2f %%", 100.0 * l->rs_evm);
            if (l->mib)
                printf(", PBCH %.2f %%", 100.0 * l->pbch_evm);
            printf("\n");
            if (l->data) {
                int used[M2SDR_MOD_COUNT] = { 0 }, ns, rb, m;

                printf("  Data       : %d blocks read, mostly %s, EVM %.2f %% against the nearest points\n", l->data_blocks,
                       l->data_blocks ? m2sdr_modulation_name(l->data_modulation) : "none", 100.0 * l->data_evm);
                for (ns = 0; ns < M2SDR_LTE_FRAME_SLOTS; ns++) {
                    for (rb = 0; rb < M2SDR_LTE_MAX_ANALYSED_RB; rb++)
                        used[l->rb_modulation[ns][rb] % M2SDR_MOD_COUNT]++;
                }
                printf("  Blocks     :");
                for (m = 0; m < M2SDR_MOD_COUNT; m++) {
                    if (m != M2SDR_MOD_AUTO && used[m])
                        printf(" %s %d", m2sdr_modulation_name((enum m2sdr_modulation)m), used[m]);
                }
                printf("\n");
                if (l->ports > 1)
                    printf("  Two ports  : %d blocks in transmit diversity, %d as one precoded layer, %d in use and not read\n"
                           "               (two layers, that one receiver does not separate)\n",
                           l->data_diversity, l->data_precoded, l->data_unknown);
            } else if (l->mib)
                printf("  Data       : not analysed, the cell has %d antenna ports\n", l->ports);
        }
    }

    if (set->demod_on) {
        const struct m2sdr_demod_result *d = &res->demod;

        printf("\nModulation:\n");
        if (!res->demod_valid || !d->signal) {
            printf("  No signal with a symbol rate was found: tune on it, with a sample rate of a few\n"
                   "  times its bandwidth, or give --symbol-rate.\n");
        } else {
            printf("  Type       : %s%s\n", m2sdr_modulation_name(d->modulation),
                   d->locked ? "" : " (NOT LOCKED: the figures below are not valid)");
            printf("  Symbol rate: %.3f symbols/s, roll-off %.2f\n", d->symbol_rate, d->rolloff);
            printf("  Carrier    : %.6f MHz (%+.1f Hz from the centre), %.2f %s\n",
                   (set->freq_hz + d->freq_offset_hz) / 1e6, d->freq_offset_hz, d->power_db + off, unit);
            printf("  EVM        : %.2f %% RMS, %.2f %% peak, over %zu symbols\n", 100.0 * d->evm_rms, 100.0 * d->evm_peak, d->symbols);
            printf("  MER        : %.1f dB\n", d->mer_db);
            printf("  Errors     : magnitude %.2f %% RMS, phase %.2f degrees RMS\n", 100.0 * d->mag_err_rms, d->phase_err_deg);
            printf("  I/Q        : origin offset %.1f dB, gain imbalance %+.3f dB, quadrature error %+.3f degrees\n",
                   d->iq_offset_db, d->gain_imbalance_db, d->quadrature_deg);
        }
    }

    if (set->pn_on) {
        const struct m2sdr_phase_noise *pn = &res->pn;

        printf("\nPhase noise:\n");
        if (!res->pn_valid) {
            printf("  No carrier stands %.0f dB above the rest of the capture.\n", 10.0);
            return;
        }
        printf("  Carrier    : %.6f MHz, %.2f %s, %zu samples\n", (set->freq_hz + pn->carrier_hz) / 1e6,
               pn->carrier_db + off, unit, (size_t)1 << set->pn_log2);
        for (i = 0; i < PN_SPOTS; i++) {
            const double l = pn_interp(pn->offset_hz, pn->l_dbc_hz, pn->points, pn_spots[i]);
            const double am = pn_interp(pn->offset_hz, pn->am_dbc_hz, pn->points, pn_spots[i]);
            const double floor = fl && fl->points ? pn_interp(fl->offset_hz, fl->l_dbc_hz, fl->points, pn_spots[i]) : NAN;
            char freq[32];

            if (!isfinite(l))
                continue;
            if (pn_spots[i] >= 1e6)
                snprintf(freq, sizeof(freq), "%.0f MHz", pn_spots[i] / 1e6);
            else if (pn_spots[i] >= 1e3)
                snprintf(freq, sizeof(freq), "%.0f kHz", pn_spots[i] / 1e3);
            else
                snprintf(freq, sizeof(freq), "%.0f Hz", pn_spots[i]);
            printf("  %-10s : %7.1f dBc/Hz (AM %7.1f)", freq, l, am);
            if (isfinite(floor))
                printf(", floor %7.1f%s", floor, l - floor < AN_PN_FLOOR_MARGIN_DB ? ": the reading is the floor of the board" : "");
            printf("\n");
        }
        printf("  Integrated : %.4f degrees RMS from %.0f Hz to %.0f Hz\n", pn->rms_deg, pn->lo_hz, pn->hi_hz);
        printf("  Jitter     : %.3f ps RMS\n", pn->jitter_s * 1e12);
        printf("  Residual FM: %.2f Hz RMS\n", pn->residual_fm_hz);
        for (i = 0; i < pn->nspurs && i < 8; i++)
            printf("  Spur       : %12.1f Hz  %7.1f dBc\n", pn->spur_hz[i], pn->spur_dbc[i]);
        if (!fl || !fl->points)
            printf("  No floor of the board is stored: the reading includes its own phase noise.\n");
    }
}

static int export_csv(const char *path, const struct an_result *res)
{
    FILE *f = fopen(path, "w");
    size_t k;

    if (!f)
        return -1;
    fprintf(f, "frequency_hz,offset_hz,level_db\n");
    for (k = 0; k < res->psd.bins; k++)
        fprintf(f, "%.3f,%.3f,%.3f\n", res->set.freq_hz + m2sdr_psd_freq(&res->psd, k), m2sdr_psd_freq(&res->psd, k),
                m2sdr_power_db(res->psd.power[k]) + res->set.ref_offset_db);
    return fclose(f) == 0 ? 0 : -1;
}

static int run_report(struct an_source *src, const struct an_settings *set, bool json, const char *csv_path,
                      struct an_floor *fl, const char *floor_save_path)
{
    const size_t count = capture_samples(set);
    float complex *x = malloc(count * sizeof(*x));
    struct an_result *res = calloc(1, sizeof(*res));
    char err[256];
    int rc, status = 1;

    if (!x || !res) {
        fprintf(stderr, "Out of memory\n");
        goto out;
    }
    rc = source_capture(src, set, x, count, err, sizeof(err));
    if (rc < 0) {
        fprintf(stderr, "Capture failed: %s\n", err);
        goto out;
    }
    res->lost = rc > 0;
    if (analyse(x, count, set, res) != 0) {
        fprintf(stderr, "Analysis failed\n");
        goto out;
    }
    if (json)
        report_json(src, res, fl);
    else
        report_text(src, res, fl);
    if (floor_save_path) {
        if (!res->pn_valid) {
            fprintf(stderr, "No carrier: no floor stored.\n");
            goto out;
        }
        if (floor_save(fl, floor_save_path, res) != 0) {
            fprintf(stderr, "Could not write %s: %s\n", floor_save_path, strerror(errno));
            goto out;
        }
        fprintf(stderr, "Phase noise stored as the floor of the board in %s.\n", floor_save_path);
    }
    if (csv_path && export_csv(csv_path, res) != 0) {
        fprintf(stderr, "Could not write %s: %s\n", csv_path, strerror(errno));
        goto out;
    }
    status = 0;

out:
    if (res) {
        m2sdr_psd_free(&res->psd);
        m2sdr_psd_free(&res->tone_psd);
    }
    free(res);
    free(x);
    return status;
}

/* GUI */
/*-----*/

#ifdef M2SDR_ANALYZER_GUI

#define UI_MARKERS       4
#define UI_WF_COLS       1024
#define UI_WF_ROWS       256
#define UI_TRACES        4

enum { TRACE_LIVE = 0, TRACE_AVG = 1, TRACE_MAX = 2, TRACE_MIN = 3 };

static const char *const trace_names[UI_TRACES] = { "Live", "Average", "Max hold", "Min hold" };
static const ImU32 trace_colors[UI_TRACES] = { 0xff40d0ffu, 0xff60ff60u, 0xff6060ffu, 0xffffa040u };
static const ImU32 marker_colors[UI_MARKERS] = { 0xffffffffu, 0xff80ffffu, 0xffff80ffu, 0xffffff80u };

/* LTE: colours of the blocks of data and of their symbols by modulation (the first: empty), and
 * the scale of the resource grid, in dB over a reference element. */
static const ImU32 lte_mod_colors[M2SDR_MOD_COUNT] = {
    0xff282828u, 0xff808080u, 0xffff9040u, 0xff808080u, 0xff60ff60u, 0xff808080u, 0xff40d0ffu, 0xff808080u, 0xff4040ffu,
};
#define LTE_COL_UNREAD     0xff808080u
#define LTE_COL_PBCH       0xffff60ffu
#define LTE_COL_RS         0xffffffffu
#define LTE_GRID_TOP_DB    10.0
#define LTE_GRID_BOTTOM_DB (-40.0)
#define LTE_SCALE_W        52.0f     /* Room of the scale, right of the grid. */
#define AN_LTE_HOLD        10        /* Captures a frame stays shown without a cell, or its symbols without data. */

/* Shared between the worker, which captures and analyses, and the UI, which draws. */
struct an_shared {
    pthread_mutex_t lock;
    struct an_settings set;        /* Wanted by the UI. */
    bool   run;
    bool   single;                 /* One capture, then stop. */
    bool   quit;
    struct an_result res;          /* Last result; its psd.power is a copy owned here. */
    unsigned seq;                  /* Counts the results. */
    char   status[256];
    double capture_ms, analysis_ms;
    struct an_source *src;
};

struct an_marker {
    bool   on;
    double offset_hz;
};

struct an_ui {
    struct an_settings set;        /* Edited by the controls. */
    struct an_result res;          /* Result being drawn. */
    unsigned seq;
    /* Traces, in dB, of res.psd.bins points. */
    float *trace[UI_TRACES];
    double *avg_lin;
    size_t bins;
    bool   trace_on[UI_TRACES];
    int    avg_count, avg_target;
    /* View. */
    double view_lo, view_hi;       /* Offsets from the centre, Hz. */
    double ref_db, range_db;
    struct an_marker markers[UI_MARKERS];
    int    marker_sel;
    /* Waterfall. */
    GLuint wf_tex;
    uint32_t *wf_rgba;
    int    wf_palette;
    bool   wf_on;
    bool   wf_auto;                /* Levels to be set from the next result. */
    /* Phase noise. */
    int    tab;                    /* View shown: 0 spectrum, 1 time, 2 statistics, 3 phase noise, 4 modulation, 5 LTE. */
    bool   pn_am;
    struct an_floor *fl;
    const char *floor_path;        /* Where "Store as floor" writes; NULL: nowhere. */
    /* LTE: resource grid and blocks as textures, redrawn with each result that has a cell. */
    GLuint lte_grid_tex, lte_block_tex;
    uint32_t *lte_rgba;
    /* Last frame with a cell, and symbols of the last one with data read: held, see lte_keep(). */
    struct m2sdr_lte_result *lte_last;
    unsigned lte_last_seq;
    bool   lte_last_ok;
    double lte_freq_hz, lte_rate;  /* What they were read on: nothing is held for another signal. */
    int    lte_channel;
    float complex lte_points[M2SDR_LTE_MAX_POINTS];
    uint8_t lte_points_mod[M2SDR_LTE_MAX_POINTS];
    int    lte_npoints, lte_points_sfn;
    unsigned lte_points_seq;
    float  wf_floor_db, wf_ceil_db;
    char   message[256];
};

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return 1e3 * (double)ts.tv_sec + 1e-6 * (double)ts.tv_nsec;
}

/* Copy of a result, with its own spectrum. */
static int result_copy(struct an_result *dst, const struct an_result *src)
{
    float *power = dst->psd.power;

    if (dst->psd.bins != src->psd.bins) {
        power = realloc(power, src->psd.bins * sizeof(*power));
        if (!power)
            return -1;
    }
    *dst = *src;
    dst->psd.power = power;
    dst->psd.fft   = NULL;
    dst->psd.win   = NULL;
    dst->psd.work  = NULL;
    memset(&dst->tone_psd, 0, sizeof(dst->tone_psd));
    memcpy(power, src->psd.power, src->psd.bins * sizeof(*power));
    return 0;
}

static void *worker_main(void *arg)
{
    struct an_shared *sh = arg;
    struct an_result *res = calloc(1, sizeof(*res));
    float complex *x = NULL;
    size_t x_count = 0;

    while (res) {
        struct an_settings set;
        size_t count;
        char err[256];
        double t0, t1, t2;
        bool run, quit;
        int rc;

        pthread_mutex_lock(&sh->lock);
        set  = sh->set;
        run  = sh->run || sh->single;
        quit = sh->quit;
        pthread_mutex_unlock(&sh->lock);
        if (quit)
            break;
        if (!run) {
            usleep(20000);
            continue;
        }

        count = capture_samples(&set);
        if (count > x_count) {
            float complex *grown = realloc(x, count * sizeof(*x));
            if (!grown) {
                pthread_mutex_lock(&sh->lock);
                snprintf(sh->status, sizeof(sh->status), "out of memory for %zu samples", count);
                sh->run = sh->single = false;
                pthread_mutex_unlock(&sh->lock);
                continue;
            }
            x       = grown;
            x_count = count;
        }
        t0 = now_ms();
        rc = source_capture(sh->src, &set, x, count, err, sizeof(err));
        t1 = now_ms();
        if (rc < 0 || analyse(x, count, &set, res) != 0) {
            pthread_mutex_lock(&sh->lock);
            snprintf(sh->status, sizeof(sh->status), "%s", rc < 0 ? err : "analysis failed");
            sh->run = sh->single = false;
            pthread_mutex_unlock(&sh->lock);
            continue;
        }
        res->lost = rc > 0;
        t2 = now_ms();

        /* A file is read faster than it can be looked at. */
        if (t2 - t0 < AN_MIN_PERIOD_MS)
            usleep((useconds_t)(1e3 * (AN_MIN_PERIOD_MS - (t2 - t0))));

        pthread_mutex_lock(&sh->lock);
        if (result_copy(&sh->res, res) == 0)
            sh->seq++;
        snprintf(sh->status, sizeof(sh->status), "%s", rc > 0 ? err : "");
        sh->capture_ms  = t1 - t0;
        sh->analysis_ms = t2 - t1;
        sh->single      = false;
        pthread_mutex_unlock(&sh->lock);
    }
    if (res) {
        m2sdr_psd_free(&res->psd);
        m2sdr_psd_free(&res->tone_psd);
    }
    free(res);
    free(x);
    return NULL;
}

/* Traces */

static void traces_reset(struct an_ui *ui)
{
    ui->avg_count = 0;
}

static int traces_update(struct an_ui *ui)
{
    const struct m2sdr_psd *psd = &ui->res.psd;
    size_t k;
    int t;

    if (ui->bins != psd->bins) {
        for (t = 0; t < UI_TRACES; t++) {
            float *grown = realloc(ui->trace[t], psd->bins * sizeof(float));
            if (!grown)
                return -1;
            ui->trace[t] = grown;
        }
        {
            double *grown = realloc(ui->avg_lin, psd->bins * sizeof(double));
            if (!grown)
                return -1;
            ui->avg_lin = grown;
        }
        ui->bins      = psd->bins;
        ui->avg_count = 0;
    }
    for (k = 0; k < psd->bins; k++) {
        const float db = (float)(m2sdr_power_db(psd->power[k]) + ui->res.set.ref_offset_db);

        ui->trace[TRACE_LIVE][k] = db;
        if (ui->avg_count == 0) {
            ui->avg_lin[k]          = psd->power[k];
            ui->trace[TRACE_MAX][k] = db;
            ui->trace[TRACE_MIN][k] = db;
        } else {
            const double n = ui->avg_count < ui->avg_target ? ui->avg_count + 1 : ui->avg_target;
            ui->avg_lin[k] += (psd->power[k] - ui->avg_lin[k]) / n;
            if (db > ui->trace[TRACE_MAX][k])
                ui->trace[TRACE_MAX][k] = db;
            if (db < ui->trace[TRACE_MIN][k])
                ui->trace[TRACE_MIN][k] = db;
        }
        ui->trace[TRACE_AVG][k] = (float)(m2sdr_power_db(ui->avg_lin[k]) + ui->res.set.ref_offset_db);
    }
    if (ui->avg_count < ui->avg_target)
        ui->avg_count++;
    return 0;
}

/* Trace read by the markers and the peak search: the first one shown. */
static const float *marker_trace(const struct an_ui *ui)
{
    int t;

    for (t = 0; t < UI_TRACES; t++) {
        if (ui->trace_on[t] && ui->trace[t])
            return ui->trace[t];
    }
    return ui->trace[TRACE_LIVE];
}

static double trace_at(const struct an_ui *ui, const float *trace, double offset_hz)
{
    const struct m2sdr_psd *psd = &ui->res.psd;
    double k = offset_hz / psd->bin_hz + (double)(psd->bins / 2);

    if (!trace || !psd->bins)
        return -200.0;
    if (k < 0)
        k = 0;
    if (k > (double)(psd->bins - 1))
        k = (double)(psd->bins - 1);
    return trace[(size_t)llround(k)];
}

/* Waterfall */

static uint32_t palette_color(int palette, float t)
{
    int idx = (int)(t * 255.0f + 0.5f);
    unsigned r, g, b;

    if (idx < 0)
        idx = 0;
    if (idx > 255)
        idx = 255;
    switch (palette) {
    case 1:
        r = gqrx_plasma[idx][0]; g = gqrx_plasma[idx][1]; b = gqrx_plasma[idx][2];
        break;
    case 2:
        r = (unsigned)(gqrx_viridis[idx][0] * 255.0f);
        g = (unsigned)(gqrx_viridis[idx][1] * 255.0f);
        b = (unsigned)(gqrx_viridis[idx][2] * 255.0f);
        break;
    default:
        r = gqrx_turbo[idx][0]; g = gqrx_turbo[idx][1]; b = gqrx_turbo[idx][2];
        break;
    }
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

static void waterfall_init(struct an_ui *ui)
{
    ui->wf_rgba = calloc((size_t)UI_WF_COLS * UI_WF_ROWS, sizeof(uint32_t));
    glGenTextures(1, &ui->wf_tex);
    glBindTexture(GL_TEXTURE_2D, ui->wf_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, UI_WF_COLS, UI_WF_ROWS, 0, GL_RGBA, GL_UNSIGNED_BYTE, ui->wf_rgba);
}

/* New line at the top, the others move down. A column shows the highest of its bins. */
static void waterfall_push(struct an_ui *ui)
{
    const float *live = ui->trace[TRACE_LIVE];
    float span;
    int col;

    if (!ui->wf_rgba || !live || !ui->bins)
        return;
    if (ui->wf_auto) {
        /* From a little under the noise of a bin to 70 dB above. */
        ui->wf_floor_db = (float)(ui->res.noise_db_hz + 10.0 * log10(ui->res.psd.rbw_hz) + ui->res.set.ref_offset_db - 5.0);
        ui->wf_ceil_db  = ui->wf_floor_db + 70.0f;
        ui->wf_auto     = false;
    }
    span = ui->wf_ceil_db - ui->wf_floor_db;
    memmove(ui->wf_rgba + UI_WF_COLS, ui->wf_rgba, (size_t)UI_WF_COLS * (UI_WF_ROWS - 1) * sizeof(uint32_t));
    for (col = 0; col < UI_WF_COLS; col++) {
        size_t a = (size_t)col * ui->bins / UI_WF_COLS;
        size_t b = (size_t)(col + 1) * ui->bins / UI_WF_COLS;
        float v;

        if (b <= a)
            b = a + 1;
        v = live[a];
        for (a++; a < b && a < ui->bins; a++) {
            if (live[a] > v)
                v = live[a];
        }
        ui->wf_rgba[col] = palette_color(ui->wf_palette, span > 0 ? (v - ui->wf_floor_db) / span : 0.0f);
    }
    glBindTexture(GL_TEXTURE_2D, ui->wf_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, UI_WF_COLS, UI_WF_ROWS, GL_RGBA, GL_UNSIGNED_BYTE, ui->wf_rgba);
}

/* Widgets */

static void label_value(const char *label, const char *fmt, ...)
{
    char text[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    igTableNextRow(0, 0.0f);
    igTableNextColumn();
    igTextUnformatted(label, NULL);
    igTableNextColumn();
    igTextUnformatted(text, NULL);
}

/* Number edited as text, in units of `scale`. The value is taken when the edit ends, not at
 * every key: half-typed numbers do not reach the board. */
static bool input_scaled(const char *label, double *value, double scale, const char *fmt)
{
    static ImGuiID editing;
    static double edit;
    const ImGuiID id = igGetID_Str(label);
    double v = editing == id ? edit : *value / scale;
    bool done = false;

    igSetNextItemWidth(150.0f);
    igInputDouble(label, &v, 0.0, 0.0, fmt, 0);
    if (igIsItemActive()) {
        editing = id;
        edit    = v;
    }
    if (igIsItemDeactivatedAfterEdit()) {
        *value = (editing == id ? edit : v) * scale;
        done   = true;
    }
    if (!igIsItemActive() && editing == id)
        editing = 0;
    return done;
}

static bool input_mhz(const char *label, double *hz)
{
    return input_scaled(label, hz, 1e6, "%.6f");
}

static bool input_khz(const char *label, double *hz)
{
    return input_scaled(label, hz, 1e3, "%.3f");
}

static void view_full(struct an_ui *ui)
{
    ui->view_lo = -0.5 * ui->set.sample_rate;
    ui->view_hi =  0.5 * ui->set.sample_rate;
}

static void view_clamp(struct an_ui *ui)
{
    const double half = 0.5 * ui->set.sample_rate;
    const double min_span = 16.0 * ui->set.sample_rate / (double)ui->set.fft_len;

    if (ui->view_hi - ui->view_lo < min_span) {
        const double mid = 0.5 * (ui->view_lo + ui->view_hi);
        ui->view_lo = mid - 0.5 * min_span;
        ui->view_hi = mid + 0.5 * min_span;
    }
    if (ui->view_hi - ui->view_lo > 2.0 * half) {
        ui->view_lo = -half;
        ui->view_hi = half;
    }
    if (ui->view_lo < -half) {
        ui->view_hi += -half - ui->view_lo;
        ui->view_lo  = -half;
    }
    if (ui->view_hi > half) {
        ui->view_lo -= ui->view_hi - half;
        ui->view_hi  = half;
    }
}

/* Panels */

static void draw_controls(struct an_ui *ui, struct an_shared *sh, bool *changed)
{
    static const char *const fft_names[] = { "256", "1024", "4096", "16384", "65536", "262144", "1048576" };
    static const int fft_values[] = { 256, 1024, 4096, 16384, 65536, 262144, 1048576 };
    static const char *const palettes[] = { "Turbo", "Plasma", "Viridis" };
    const struct an_source *src = sh->src;
    bool run;
    int i, fft_idx = 3;

    pthread_mutex_lock(&sh->lock);
    run = sh->run;
    pthread_mutex_unlock(&sh->lock);

    igSeparatorText(run ? "Acquisition: running" : "Acquisition: stopped");
    if (igButton(run ? "Stop" : "Run", (ImVec2){ 70.0f, 0.0f })) {
        pthread_mutex_lock(&sh->lock);
        sh->run = !run;
        pthread_mutex_unlock(&sh->lock);
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Single", (ImVec2){ 70.0f, 0.0f })) {
        pthread_mutex_lock(&sh->lock);
        sh->run    = false;
        sh->single = true;
        pthread_mutex_unlock(&sh->lock);
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Restart", (ImVec2){ 70.0f, 0.0f }))
        traces_reset(ui);

    igSeparatorText(src->live ? "Board" : "File");
    if (src->live && src->no_rf_init) {
        /* The RFIC is the one of another utility: nothing here would reach it. */
        igText("%.6f MHz, %.3f MSPS", ui->set.freq_hz / 1e6, ui->set.sample_rate / 1e6);
        igPushTextWrapPos(0.0f);
        igTextUnformatted("RFIC left as it is (--no-rf-init): its frequency, rate and gain are set with m2sdr_rf.", NULL);
        igPopTextWrapPos();
        if (igRadioButton_IntPtr("RX1", &ui->set.channel, 0))
            *changed = true;
        igSameLine(0.0f, -1.0f);
        if (igRadioButton_IntPtr("RX2", &ui->set.channel, 1))
            *changed = true;
    } else if (src->live) {
        if (input_mhz("Centre (MHz)", &ui->set.freq_hz)) {
            *changed = true;
            traces_reset(ui);
        }
        {
            double rate = ui->set.sample_rate;
            if (input_scaled("Rate (MSPS)", &rate, 1e6, "%.6f") && rate >= 0.5e6 && rate <= 61.44e6) {
                ui->set.sample_rate = rate;
                ui->set.bandwidth   = 0;
                view_full(ui);
                traces_reset(ui);
                *changed = true;
            }
        }
        if (igCheckbox("AGC", &ui->set.agc))
            *changed = true;
        if (!ui->set.agc) {
            igSetNextItemWidth(150.0f);
            if (igSliderInt("Gain (dB)", &ui->set.rx_gain, 0, 73, "%d", 0))
                *changed = true;
        }
        if (igRadioButton_IntPtr("RX1", &ui->set.channel, 0))
            *changed = true;
        igSameLine(0.0f, -1.0f);
        if (igRadioButton_IntPtr("RX2", &ui->set.channel, 1))
            *changed = true;
    } else {
        igText("%.3f MSPS, %d channel%s", ui->set.sample_rate / 1e6, src->nchannels, src->nchannels == 1 ? "" : "s");
        igText("%.1f %% of %.3f s", src->total_sets ? 100.0 * (double)src->position / (double)src->total_sets : 0.0,
               (double)src->total_sets / ui->set.sample_rate);
        for (i = 0; i < src->nchannels && i < 2; i++) {
            char name[8];
            snprintf(name, sizeof(name), "RX%d", i + 1);
            if (i)
                igSameLine(0.0f, -1.0f);
            if (igRadioButton_IntPtr(name, &ui->set.channel, i))
                *changed = true;
        }
    }

    igSeparatorText("Spectrum");
    for (i = 0; i < (int)(sizeof(fft_values) / sizeof(fft_values[0])); i++) {
        if (fft_values[i] == ui->set.fft_len)
            fft_idx = i;
    }
    igSetNextItemWidth(150.0f);
    if (igCombo_Str_arr("FFT length", &fft_idx, fft_names, (int)(sizeof(fft_values) / sizeof(fft_values[0])), -1)) {
        ui->set.fft_len = fft_values[fft_idx];
        traces_reset(ui);
        *changed = true;
    }
    igSetNextItemWidth(150.0f);
    if (igCombo_Str_arr("Window", &ui->set.window, window_names, 4, -1)) {
        traces_reset(ui);
        *changed = true;
    }
    igSetNextItemWidth(150.0f);
    if (igInputInt("Averages", &ui->set.averages, 1, 8, 0)) {
        if (ui->set.averages < 1)
            ui->set.averages = 1;
        if (ui->set.averages > AN_MAX_AVERAGES)
            ui->set.averages = AN_MAX_AVERAGES;
        *changed = true;
    }
    if (ui->res.psd.bins)
        igText("RBW %.1f Hz", ui->res.psd.rbw_hz);

    igSeparatorText("Traces");
    for (i = 0; i < UI_TRACES; i++) {
        if (i != 0 && i != 2)
            igSameLine(0.0f, -1.0f);
        /* The name of a trace in its colour. */
        igPushStyleColor_U32(ImGuiCol_Text, trace_colors[i]);
        igCheckbox(trace_names[i], &ui->trace_on[i]);
        igPopStyleColor(1);
    }
    igSetNextItemWidth(150.0f);
    if (igInputInt("Trace average", &ui->avg_target, 1, 8, 0)) {
        if (ui->avg_target < 1)
            ui->avg_target = 1;
    }

    igSeparatorText("Scale");
    {
        float ref = (float)ui->ref_db, range = (float)ui->range_db;
        igSetNextItemWidth(150.0f);
        if (igDragFloat("Top (dB)", &ref, 0.5f, -200.0f, 100.0f, "%.0f", 0))
            ui->ref_db = ref;
        igSetNextItemWidth(150.0f);
        if (igDragFloat("Range (dB)", &range, 0.5f, 10.0f, 200.0f, "%.0f", 0))
            ui->range_db = range;
    }
    {
        double off = ui->set.ref_offset_db;
        if (input_scaled("Offset (dB)", &off, 1.0, "%.2f")) {
            ui->set.ref_offset_db = off;
            traces_reset(ui);
            *changed = true;
        }
    }
    if (igButton("Full span", (ImVec2){ 0.0f, 0.0f }))
        view_full(ui);

    igSeparatorText("Channel");
    if (input_khz("Width (kHz)", &ui->set.chan_bw_hz))
        *changed = true;
    if (input_khz("Offset (kHz)", &ui->set.chan_offset_hz))
        *changed = true;
    if (input_khz("Spacing (kHz)", &ui->set.chan_spacing_hz))
        *changed = true;

    igSeparatorText("Waterfall");
    igCheckbox("Show", &ui->wf_on);
    igSameLine(0.0f, -1.0f);
    if (igButton("Auto levels", (ImVec2){ 0.0f, 0.0f }))
        ui->wf_auto = true;
    igSetNextItemWidth(150.0f);
    igCombo_Str_arr("Colours", &ui->wf_palette, palettes, 3, -1);
    igSetNextItemWidth(150.0f);
    igDragFloat("Floor (dB)", &ui->wf_floor_db, 0.5f, -200.0f, 100.0f, "%.0f", 0);
    igSetNextItemWidth(150.0f);
    igDragFloat("Ceiling (dB)", &ui->wf_ceil_db, 0.5f, -200.0f, 100.0f, "%.0f", 0);
}

static void draw_spectrum(struct an_ui *ui, bool *changed)
{
    const struct m2sdr_psd *psd = &ui->res.psd;
    const double half = 0.5 * ui->res.set.sample_rate;
    const double centre = ui->res.set.freq_hz;
    const char *unit = ui->res.set.ref_offset_db != 0.0 ? "dBm" : "dBFS";
    struct m2sdr_plot plot;
    ImVec2 avail;
    float plot_h;
    char text[96];
    int t, m;

    igGetContentRegionAvail(&avail);
    plot_h = ui->wf_on ? 0.62f * avail.y : avail.y;
    if (!m2sdr_plot_begin(&plot, "##spectrum", (ImVec2){ avail.x, plot_h }, centre + ui->view_lo, centre + ui->view_hi,
                          M2SDR_PLOT_FREQ, ui->ref_db - ui->range_db, ui->ref_db, unit))
        return;

    if (ui->res.set.chan_bw_hz > 0) {
        const double c = centre + ui->res.set.chan_offset_hz, bw = ui->res.set.chan_bw_hz;
        const double spacing = ui->res.set.chan_spacing_hz > 0 ? ui->res.set.chan_spacing_hz : bw;
        int n;

        m2sdr_plot_band(&plot, c - 0.5 * bw, c + 0.5 * bw, 0x3060ff60u);
        for (n = 1; ui->res.acpr_valid && n <= ui->res.acpr.adjacent; n++) {
            m2sdr_plot_band(&plot, c - n * spacing - 0.5 * bw, c - n * spacing + 0.5 * bw, 0x20ffa040u);
            m2sdr_plot_band(&plot, c + n * spacing - 0.5 * bw, c + n * spacing + 0.5 * bw, 0x20ffa040u);
        }
    }
    if (ui->res.obw_valid) {
        m2sdr_plot_vline(&plot, centre + ui->res.obw_lo_hz, 0x80ffff60u);
        m2sdr_plot_vline(&plot, centre + ui->res.obw_hi_hz, 0x80ffff60u);
    }
    if (psd->bins && ui->bins == psd->bins) {
        for (t = UI_TRACES - 1; t >= 0; t--) {
            if (ui->trace_on[t])
                m2sdr_plot_trace(&plot, ui->trace[t], ui->bins, centre - half, centre + half - psd->bin_hz, trace_colors[t]);
        }
        for (m = 0; m < UI_MARKERS; m++) {
            if (!ui->markers[m].on)
                continue;
            snprintf(text, sizeof(text), "M%d", m + 1);
            m2sdr_plot_marker(&plot, centre + ui->markers[m].offset_hz,
                              trace_at(ui, marker_trace(ui), ui->markers[m].offset_hz), text, marker_colors[m]);
        }
    }
    if (plot.hovered) {
        ImGuiIO *io = igGetIO();
        char freq[48];

        m2sdr_plot_format(freq, sizeof(freq), plot.mouse_x, psd->bins ? psd->bin_hz : 1.0, M2SDR_PLOT_FREQ);
        snprintf(text, sizeof(text), "%s  %.1f %s", freq, trace_at(ui, marker_trace(ui), plot.mouse_x - centre), unit);
        m2sdr_plot_text(&plot, -260.0f, 4.0f, text, 0xffffffffu);
        m2sdr_plot_vline(&plot, plot.mouse_x, 0x40ffffffu);

        /* Left click: the selected marker. Wheel: zoom around the mouse. Right drag: pan.
         * Double click: back to the full span. */
        if (plot.clicked) {
            ui->markers[ui->marker_sel].on        = true;
            ui->markers[ui->marker_sel].offset_hz = plot.mouse_x - centre;
        }
        if (io->MouseWheel != 0.0f) {
            const double factor = pow(0.8, io->MouseWheel);
            const double at = plot.mouse_x - centre;
            ui->view_lo = at - (at - ui->view_lo) * factor;
            ui->view_hi = at + (ui->view_hi - at) * factor;
            view_clamp(ui);
        }
        if (igIsMouseDragging(1, 2.0f)) {
            const double per_px = (ui->view_hi - ui->view_lo) / (plot.p1.x - plot.p0.x);
            ui->view_lo -= io->MouseDelta.x * per_px;
            ui->view_hi -= io->MouseDelta.x * per_px;
            view_clamp(ui);
        }
        if (igIsMouseDoubleClicked_Nil(0))
            view_full(ui);
    }
    if (ui->res.lost)
        m2sdr_plot_text(&plot, 8.0f, 22.0f, "Samples lost in the capture", 0xff4040ffu);
    m2sdr_plot_end(&plot);

    if (ui->wf_on) {
        ImTextureRef tex;
        ImVec2 pos;
        const float u0 = (float)((ui->view_lo + half) / (2.0 * half));
        const float u1 = (float)((ui->view_hi + half) / (2.0 * half));

        igGetCursorScreenPos(&pos);
        igGetContentRegionAvail(&avail);
        tex._TexData = NULL;
        tex._TexID   = (ImTextureID)(uintptr_t)ui->wf_tex;
        ImDrawList_AddImage(plot.dl, tex, (ImVec2){ plot.p0.x, pos.y }, (ImVec2){ plot.p1.x, pos.y + avail.y - 4.0f },
                            (ImVec2){ u0, 0.0f }, (ImVec2){ u1, 1.0f }, 0xffffffffu);
        igDummy((ImVec2){ avail.x, avail.y - 4.0f });
    }
    (void)changed;
}

static void draw_time(struct an_ui *ui)
{
    static float re[AN_TIME_POINTS], im[AN_TIME_POINTS], mag[AN_TIME_POINTS];
    static int points = 1024;
    const size_t n = ui->res.time_count < (size_t)points ? ui->res.time_count : (size_t)points;
    const double dt = ui->res.set.sample_rate > 0 ? 1.0 / ui->res.set.sample_rate : 1.0;
    struct m2sdr_plot plot;
    ImVec2 avail;
    float peak = 1e-6f;
    size_t i;

    igSetNextItemWidth(200.0f);
    igSliderInt("Samples shown", &points, 64, AN_TIME_POINTS, "%d", ImGuiSliderFlags_Logarithmic);
    if (n < 2)
        return;
    for (i = 0; i < n; i++) {
        re[i]  = crealf(ui->res.time[i]);
        im[i]  = cimagf(ui->res.time[i]);
        mag[i] = (float)(m2sdr_power_db(re[i] * re[i] + im[i] * im[i]) + ui->res.set.ref_offset_db);
        if (fabsf(re[i]) > peak)
            peak = fabsf(re[i]);
        if (fabsf(im[i]) > peak)
            peak = fabsf(im[i]);
    }
    peak *= 1.1f;
    igGetContentRegionAvail(&avail);
    if (m2sdr_plot_begin(&plot, "##iq", (ImVec2){ avail.x, 0.5f * avail.y }, 0.0, dt * (double)(n - 1), M2SDR_PLOT_TIME,
                         -peak, peak, "I (blue), Q (orange), full scale 1")) {
        m2sdr_plot_trace(&plot, re, n, 0.0, dt * (double)(n - 1), 0xffffa040u);
        m2sdr_plot_trace(&plot, im, n, 0.0, dt * (double)(n - 1), 0xff40a0ffu);
        m2sdr_plot_end(&plot);
    }
    if (m2sdr_plot_begin(&plot, "##mag", (ImVec2){ avail.x, 0.0f }, 0.0, dt * (double)(n - 1), M2SDR_PLOT_TIME,
                         ui->res.stats.mean_db + ui->res.set.ref_offset_db - 40.0,
                         ui->res.stats.peak_db + ui->res.set.ref_offset_db + 3.0,
                         ui->res.set.ref_offset_db != 0.0 ? "Power, dBm; line: the mean" : "Power, dBFS; line: the mean")) {
        m2sdr_plot_trace(&plot, mag, n, 0.0, dt * (double)(n - 1), 0xff60ff60u);
        m2sdr_plot_hline(&plot, ui->res.stats.mean_db + ui->res.set.ref_offset_db, 0x80ffffffu);
        m2sdr_plot_end(&plot);
    }
}

static void draw_statistics(struct an_ui *ui)
{
    static float x[M2SDR_CCDF_POINTS], y[M2SDR_CCDF_POINTS], g[M2SDR_CCDF_POINTS];
    static float cx[AN_TIME_POINTS], cy[AN_TIME_POINTS];
    struct m2sdr_plot plot;
    ImVec2 avail, sq;
    float side, peak = 1e-6f;
    size_t i;
    int k;

    /* CCDF on a log scale, with the one of Gaussian noise as a reference. */
    for (k = 0; k < M2SDR_CCDF_POINTS; k++) {
        const double db = k * M2SDR_CCDF_STEP_DB;
        x[k] = (float)db;
        y[k] = (float)log10(ui->res.stats.ccdf[k] > 1e-7 ? ui->res.stats.ccdf[k] : 1e-7);
        g[k] = (float)(-pow(10.0, db / 10.0) / log(10.0));
    }
    igGetContentRegionAvail(&avail);
    side = avail.y < 0.5f * avail.x ? avail.y : 0.5f * avail.x;
    sq   = m2sdr_plot_square(side);
    if (m2sdr_plot_begin(&plot, "##ccdf", (ImVec2){ avail.x - sq.x - 8.0f, side },
                         0.0, (M2SDR_CCDF_POINTS - 1) * M2SDR_CCDF_STEP_DB, M2SDR_PLOT_PLAIN,
                         -6.0, 0.0, "CCDF, log10, against dB above the mean power")) {
        m2sdr_plot_trace(&plot, g, M2SDR_CCDF_POINTS, x[0], x[M2SDR_CCDF_POINTS - 1], 0x80ffffffu);
        m2sdr_plot_trace(&plot, y, M2SDR_CCDF_POINTS, x[0], x[M2SDR_CCDF_POINTS - 1], 0xff40d0ffu);
        m2sdr_plot_text(&plot, 8.0f, -20.0f, "grey: Gaussian noise", 0xffc8c8c8u);
        m2sdr_plot_end(&plot);
    }
    igSameLine(0.0f, 8.0f);
    for (i = 0; i < ui->res.time_count; i++) {
        cx[i] = crealf(ui->res.time[i]);
        cy[i] = cimagf(ui->res.time[i]);
        if (fabsf(cx[i]) > peak)
            peak = fabsf(cx[i]);
        if (fabsf(cy[i]) > peak)
            peak = fabsf(cy[i]);
    }
    peak *= 1.1f;
    if (m2sdr_plot_begin(&plot, "##iqplane", sq, -peak, peak, M2SDR_PLOT_PLAIN, -peak, peak, "Q against I, full scale 1")) {
        m2sdr_plot_points(&plot, cx, cy, ui->res.time_count, 1.0f, 0xc040d0ffu);
        m2sdr_plot_end(&plot);
    }
}

static void draw_phase_noise(struct an_ui *ui, bool *changed)
{
    static const char *const sizes[] = { "65536", "131072", "262144", "524288", "1048576", "2097152", "4194304", "8388608" };
    static float lx[M2SDR_PN_MAX_POINTS], l[M2SDR_PN_MAX_POINTS], am[M2SDR_PN_MAX_POINTS];
    static float fx[M2SDR_PN_MAX_POINTS], fy[M2SDR_PN_MAX_POINTS];
    const struct m2sdr_phase_noise *pn = &ui->res.pn;
    const struct an_floor *fl = ui->fl;
    struct m2sdr_plot plot;
    double top = -200.0, bottom = 0.0;
    int idx = ui->set.pn_log2 - AN_PN_MIN_LOG2, i, nf = 0;
    char text[160];

    igSetNextItemWidth(110.0f);
    if (igCombo_Str_arr("Samples", &idx, sizes, AN_PN_MAX_LOG2 - AN_PN_MIN_LOG2 + 1, -1)) {
        ui->set.pn_log2 = idx + AN_PN_MIN_LOG2;
        *changed = true;
    }
    igSameLine(0.0f, 16.0f);
    if (input_scaled("From (Hz)", &ui->set.pn_lo_hz, 1.0, "%.0f"))
        *changed = true;
    igSameLine(0.0f, 16.0f);
    if (input_scaled("To (Hz)", &ui->set.pn_hi_hz, 1.0, "%.0f"))
        *changed = true;
    igSameLine(0.0f, 16.0f);
    igCheckbox("AM noise", &ui->pn_am);
    if (ui->floor_path) {
        igSameLine(0.0f, 16.0f);
        if (igButton("Store as floor", (ImVec2){ 0.0f, 0.0f }) && ui->res.pn_valid) {
            if (floor_save(ui->fl, ui->floor_path, &ui->res) == 0)
                snprintf(ui->message, sizeof(ui->message), "Floor written to %.200s", ui->floor_path);
            else
                snprintf(ui->message, sizeof(ui->message), "Could not write %.200s", ui->floor_path);
        }
    }

    if (!ui->res.set.pn_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!ui->res.pn_valid) {
        igTextUnformatted("No carrier stands 10 dB above the rest of the capture: lower the sample rate around it, "
                          "or tune on it.", NULL);
        return;
    }
    for (i = 0; i < pn->points; i++) {
        lx[i] = (float)log10(pn->offset_hz[i]);
        l[i]  = (float)pn->l_dbc_hz[i];
        am[i] = (float)pn->am_dbc_hz[i];
        if (l[i] > top)
            top = l[i];
        if (l[i] < bottom)
            bottom = l[i];
        if (ui->pn_am && am[i] < bottom)
            bottom = am[i];
    }
    for (i = 0; fl && i < fl->points; i++) {
        if (fl->offset_hz[i] < pn->offset_hz[0] || fl->offset_hz[i] > pn->offset_hz[pn->points - 1])
            continue;
        fx[nf] = (float)log10(fl->offset_hz[i]);
        fy[nf] = (float)fl->l_dbc_hz[i];
        if (fy[nf] < bottom)
            bottom = fy[nf];
        nf++;
    }
    top    = 10.0 * ceil(top / 10.0) + 10.0;
    bottom = 10.0 * floor(bottom / 10.0) - 10.0;
    if (!m2sdr_plot_begin(&plot, "##pn", (ImVec2){ 0.0f, 0.0f }, lx[0], lx[pn->points - 1], M2SDR_PLOT_LOG_FREQ,
                          bottom, top, "L(f), dBc/Hz, against the offset from the carrier"))
        return;
    /* Integration range. */
    m2sdr_plot_band(&plot, log10(pn->lo_hz), log10(pn->hi_hz), 0x18ffffffu);
    if (nf >= 2)
        m2sdr_plot_xy(&plot, fx, fy, (size_t)nf, 0xffa0a0a0u);
    if (ui->pn_am)
        m2sdr_plot_xy(&plot, lx, am, (size_t)pn->points, 0xffffa040u);
    m2sdr_plot_xy(&plot, lx, l, (size_t)pn->points, 0xff40d0ffu);
    /* Points that are the floor of the board, not the source. */
    for (i = 0; i < pn->points && nf >= 2; i++) {
        const double floor = pn_interp(fl->offset_hz, fl->l_dbc_hz, fl->points, pn->offset_hz[i]);
        if (isfinite(floor) && pn->l_dbc_hz[i] - floor < AN_PN_FLOOR_MARGIN_DB)
            m2sdr_plot_points(&plot, &lx[i], &l[i], 1, 2.5f, 0xff4040ffu);
    }
    for (i = 0; i < pn->nspurs && i < 8; i++) {
        snprintf(text, sizeof(text), "%.0f dBc", pn->spur_dbc[i]);
        m2sdr_plot_marker(&plot, log10(pn->spur_hz[i]), pn_interp(pn->offset_hz, pn->l_dbc_hz, pn->points, pn->spur_hz[i]),
                          text, 0xffc0c0ffu);
    }
    snprintf(text, sizeof(text), "yellow: phase%s%s", ui->pn_am ? ", blue: amplitude" : "",
             nf >= 2 ? ", grey: floor of the board, red: reading at the floor" : ", no floor stored");
    m2sdr_plot_text(&plot, 8.0f, -20.0f, text, 0xffc8c8c8u);
    if (plot.hovered) {
        const double f = pow(10.0, plot.mouse_x);
        snprintf(text, sizeof(text), "%.0f Hz  %.1f dBc/Hz", f, pn_interp(pn->offset_hz, pn->l_dbc_hz, pn->points, f));
        m2sdr_plot_text(&plot, -220.0f, 4.0f, text, 0xffffffffu);
        m2sdr_plot_vline(&plot, plot.mouse_x, 0x40ffffffu);
    }
    m2sdr_plot_end(&plot);
}

static void draw_modulation(struct an_ui *ui, bool *changed)
{
    static const char *const mods[] = { "Auto", "BPSK", "QPSK", "8PSK", "16QAM", "32QAM", "64QAM", "128QAM", "256QAM" };
    static const char *const pulses[] = { "Root raised cosine", "Raised cosine", "Rectangular" };
    static const char *const sizes[] = { "16384", "32768", "65536", "131072", "262144", "524288", "1048576", "2097152", "4194304" };
    static float sx[M2SDR_DEMOD_MAX_SYMBOLS], sy[M2SDR_DEMOD_MAX_SYMBOLS], evm[M2SDR_DEMOD_MAX_SYMBOLS];
    static float complex points[256];
    static float px[256], py[256];
    const struct m2sdr_demod_result *d = &ui->res.demod;
    struct m2sdr_plot plot;
    ImVec2 avail, sq;
    float side, lim = 1.0f, far = 0.0f, eye_h;
    int mod = (int)ui->set.demod.modulation, pulse = (int)ui->set.demod.pulse;
    int idx = ui->set.demod_log2 - AN_DEMOD_MIN_LOG2, npoints, i, m, rail;
    float evm_top = 1.0f;
    size_t k;

    igSetNextItemWidth(90.0f);
    if (igCombo_Str_arr("Type", &mod, mods, M2SDR_MOD_COUNT, -1)) {
        ui->set.demod.modulation = (enum m2sdr_modulation)mod;
        *changed = true;
    }
    igSameLine(0.0f, 12.0f);
    igSetNextItemWidth(170.0f);
    if (igCombo_Str_arr("Pulse", &pulse, pulses, 3, -1)) {
        ui->set.demod.pulse = (enum m2sdr_pulse)pulse;
        *changed = true;
    }
    igSameLine(0.0f, 12.0f);
    if (input_scaled("Symbols/s (0: auto)", &ui->set.demod.symbol_rate, 1.0, "%.1f"))
        *changed = true;
    igSameLine(0.0f, 12.0f);
    {
        double a = ui->set.demod.rolloff;
        if (input_scaled("Roll-off (0: auto)", &a, 1.0, "%.2f") && (a == 0 || (a >= 0.05 && a <= 1.0))) {
            ui->set.demod.rolloff = a;
            *changed = true;
        }
    }
    igSetNextItemWidth(110.0f);
    if (igCombo_Str_arr("Samples", &idx, sizes, AN_DEMOD_MAX_LOG2 - AN_DEMOD_MIN_LOG2 + 1, -1)) {
        ui->set.demod_log2 = idx + AN_DEMOD_MIN_LOG2;
        *changed = true;
    }

    if (!ui->res.set.demod_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!ui->res.demod_valid || !d->signal) {
        igTextUnformatted("No signal with a symbol rate was found: tune on it, with a sample rate of a few times its "
                          "bandwidth, or give the symbol rate.", NULL);
        return;
    }
    igSameLine(0.0f, 16.0f);
    if (d->locked)
        igTextColored((ImVec4){ 0.4f, 1.0f, 0.4f, 1.0f }, "%s locked", m2sdr_modulation_name(d->modulation));
    else
        igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "%s NOT LOCKED: the figures are not valid", m2sdr_modulation_name(d->modulation));

    npoints = m2sdr_constellation(d->modulation, points);
    for (i = 0; i < npoints; i++) {
        px[i] = crealf(points[i]);
        py[i] = cimagf(points[i]);
        if (fabsf(px[i]) > lim)
            lim = fabsf(px[i]);
        if (fabsf(py[i]) > lim)
            lim = fabsf(py[i]);
    }
    lim *= 1.25f;
    for (k = 0; k < d->symbols; k++) {
        sx[k]  = crealf(d->sym[k]);
        sy[k]  = cimagf(d->sym[k]);
        evm[k] = 100.0f * cabsf(d->sym[k] - d->ref[k]);
        if (evm[k] > evm_top)
            evm_top = evm[k];
        if (fabsf(sx[k]) > far)
            far = fabsf(sx[k]);
        if (fabsf(sy[k]) > far)
            far = fabsf(sy[k]);
    }
    /* Symbols that are not on the points (another modulation, no lock) are shown too, up to
     * twice the size of the constellation. */
    if (1.05f * far > lim)
        lim = 1.05f * far < 2.0f * lim ? 1.05f * far : 2.0f * lim;

    igGetContentRegionAvail(&avail);
    side = avail.y - 170.0f < 0.5f * avail.x ? avail.y - 170.0f : 0.5f * avail.x;
    if (side < 120.0f)
        side = 120.0f;
    eye_h = 0.5f * side;
    sq    = m2sdr_plot_square(side);

    /* Constellation: the symbols, and the points they are compared with. */
    if (m2sdr_plot_begin(&plot, "##const", sq, -lim, lim, M2SDR_PLOT_PLAIN, -lim, lim,
                         "Q against I; crosses: the points of the constellation")) {
        m2sdr_plot_points(&plot, sx, sy, d->symbols, 1.0f, 0xc040d0ffu);
        for (i = 0; i < npoints; i++) {
            const float x = m2sdr_plot_x(&plot, px[i]), y = m2sdr_plot_y(&plot, py[i]);
            ImDrawList_AddLine(plot.dl, (ImVec2){ x - 5.0f, y }, (ImVec2){ x + 5.0f, y }, 0xffffffffu, 1.0f);
            ImDrawList_AddLine(plot.dl, (ImVec2){ x, y - 5.0f }, (ImVec2){ x, y + 5.0f }, 0xffffffffu, 1.0f);
        }
        m2sdr_plot_end(&plot);
    }
    igSameLine(0.0f, 8.0f);

    /* Eye diagrams of I and of Q: two symbols, the decisions at the two vertical lines. */
    igBeginGroup();
    for (rail = 0; rail < 2; rail++) {
        if (m2sdr_plot_begin(&plot, rail ? "##eyeq" : "##eyei", (ImVec2){ avail.x - sq.x - 8.0f, eye_h },
                             -0.5, 1.5, M2SDR_PLOT_PLAIN, -lim, lim,
                             rail ? "Eye of Q, in symbols; lines: the decisions" :
                                    "Eye of I, in symbols; lines: the decisions")) {
            static float ex[M2SDR_DEMOD_EYE_POINTS], ey[M2SDR_DEMOD_EYE_POINTS];

            m2sdr_plot_vline(&plot, 0.0, 0x60ffffffu);
            m2sdr_plot_vline(&plot, 1.0, 0x60ffffffu);
            for (i = 0; i < d->eye_traces; i++) {
                for (m = 0; m < M2SDR_DEMOD_EYE_POINTS; m++) {
                    ex[m] = (float)(m - 4) / 8.0f;
                    ey[m] = rail ? cimagf(d->eye[i][m]) : crealf(d->eye[i][m]);
                }
                m2sdr_plot_xy(&plot, ex, ey, M2SDR_DEMOD_EYE_POINTS, rail ? 0x6040a0ffu : 0x60ffa040u);
            }
            m2sdr_plot_end(&plot);
        }
    }
    igEndGroup();

    /* Error of each symbol. */
    if (m2sdr_plot_begin(&plot, "##evm", (ImVec2){ 0.0f, 0.0f }, 0.0, (double)(d->symbols - 1), M2SDR_PLOT_PLAIN,
                         0.0, 1.1 * evm_top, "Error vector of each symbol, % of the RMS of the constellation, against its number; line: RMS")) {
        m2sdr_plot_trace(&plot, evm, d->symbols, 0.0, (double)(d->symbols - 1), 0xff60ff60u);
        m2sdr_plot_hline(&plot, 100.0 * d->evm_rms, 0xc0ffffffu);
        m2sdr_plot_end(&plot);
    }
}

static void draw_modulation_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_demod_result *d = &res->demod;

    igSeparatorText("Modulation");
    if (!res->demod_valid || !d->signal) {
        igTextUnformatted("No signal.", NULL);
        return;
    }
    if (igBeginTable("##demod", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Type", "%s%s", m2sdr_modulation_name(d->modulation), d->locked ? "" : " (not locked)");
        label_value("Symbol rate", "%.3f /s", d->symbol_rate);
        label_value("Roll-off", "%.2f", d->rolloff);
        label_value("Carrier", "%.6f MHz", (res->set.freq_hz + d->freq_offset_hz) / 1e6);
        label_value("Carrier offset", "%+.1f Hz", d->freq_offset_hz);
        label_value("Power", "%.2f dBFS", d->power_db);
        label_value("EVM", "%.2f %% RMS", 100.0 * d->evm_rms);
        label_value("EVM peak", "%.2f %%", 100.0 * d->evm_peak);
        label_value("MER", "%.1f dB", d->mer_db);
        label_value("Magnitude error", "%.2f %% RMS", 100.0 * d->mag_err_rms);
        label_value("Phase error", "%.2f deg RMS", d->phase_err_deg);
        label_value("Origin offset", "%.1f dB", d->iq_offset_db);
        label_value("I/Q gain", "%+.3f dB", d->gain_imbalance_db);
        label_value("Quadrature", "%+.3f deg", d->quadrature_deg);
        label_value("Symbols", "%zu", d->symbols);
        igEndTable();
    }
}

static void lte_texture(GLuint *tex, const uint32_t *rgba, int width, int height)
{
    if (!*tex) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

/* Textures of the frame: a column per symbol or per slot, the highest subcarrier at the top. */
static void lte_textures(struct an_ui *ui)
{
    const struct m2sdr_lte_result *l = &ui->res.lte;
    const int k = l->subcarriers, rb = l->data_rb;
    int q, a, ns, b;

    if (!ui->lte_rgba)
        ui->lte_rgba = malloc((size_t)M2SDR_LTE_FRAME_SYMBOLS * M2SDR_LTE_MAX_SUBCARRIERS * sizeof(uint32_t));
    if (!ui->lte_rgba || k <= 0)
        return;
    for (a = 0; a < k; a++) {
        for (q = 0; q < M2SDR_LTE_FRAME_SYMBOLS; q++)
            ui->lte_rgba[(size_t)(k - 1 - a) * M2SDR_LTE_FRAME_SYMBOLS + (size_t)q] =
                palette_color(ui->wf_palette, (float)((l->grid_db[q][a] - (l->rsrp_db + LTE_GRID_BOTTOM_DB)) /
                                                      (LTE_GRID_TOP_DB - LTE_GRID_BOTTOM_DB)));
    }
    lte_texture(&ui->lte_grid_tex, ui->lte_rgba, M2SDR_LTE_FRAME_SYMBOLS, k);
    if (rb <= 0)
        return;
    for (b = 0; b < rb; b++) {
        for (ns = 0; ns < M2SDR_LTE_FRAME_SLOTS; ns++)
            ui->lte_rgba[(size_t)(rb - 1 - b) * M2SDR_LTE_FRAME_SLOTS + (size_t)ns] =
                l->rb_unknown[ns][b] ? LTE_COL_UNREAD : lte_mod_colors[l->rb_modulation[ns][b] % M2SDR_MOD_COUNT];
    }
    lte_texture(&ui->lte_block_tex, ui->lte_rgba, M2SDR_LTE_FRAME_SLOTS, rb);
}

/* A new result: its frame and its symbols are kept, to be shown while the next captures have no
 * cell, or no block of data that is read. */
static void lte_keep(struct an_ui *ui)
{
    const struct m2sdr_lte_result *l = &ui->res.lte;

    if (!ui->res.set.lte_on || !ui->res.lte_valid)
        return;
    if (ui->res.set.freq_hz != ui->lte_freq_hz || ui->res.set.sample_rate != ui->lte_rate || ui->res.set.channel != ui->lte_channel) {
        ui->lte_freq_hz = ui->res.set.freq_hz;
        ui->lte_rate    = ui->res.set.sample_rate;
        ui->lte_channel = ui->res.set.channel;
        ui->lte_last_ok = false;
        ui->lte_npoints = 0;
    }
    if (!l->found)
        return;
    lte_textures(ui);
    if (!ui->lte_last)
        ui->lte_last = malloc(sizeof(*ui->lte_last));
    if (ui->lte_last) {
        *ui->lte_last    = *l;
        ui->lte_last_seq = ui->seq;
        ui->lte_last_ok  = true;
    }
    if (l->ndata > 0) {
        memcpy(ui->lte_points, l->data_points, (size_t)l->ndata * sizeof(l->data_points[0]));
        memcpy(ui->lte_points_mod, l->data_points_modulation, (size_t)l->ndata);
        ui->lte_npoints    = l->ndata;
        ui->lte_points_sfn = l->sfn;
        ui->lte_points_seq = ui->seq;
    }
}

/* The frame shown: the one of the last capture, or the last one with a cell for a few captures. */
static const struct m2sdr_lte_result *lte_shown(const struct an_ui *ui, bool *held)
{
    *held = false;
    if (!ui->res.set.lte_on || !ui->res.lte_valid)
        return NULL;
    if (ui->res.lte.found)
        return &ui->res.lte;
    if (ui->lte_last_ok && ui->seq - ui->lte_last_seq <= AN_LTE_HOLD) {
        *held = true;
        return ui->lte_last;
    }
    return NULL;
}

/* Entry of a legend: a square of the colour and its name, on the line while it fits before `right`. */
static void legend_item(ImU32 color, const char *name, float right)
{
    const float h = igGetTextLineHeight();
    ImVec2 pos, ts;

    igCalcTextSize(&ts, name, NULL, false, -1.0f);
    igGetCursorScreenPos(&pos);
    if (pos.x + h + ts.x > right) {
        igNewLine();
        igGetCursorScreenPos(&pos);
    }
    ImDrawList_AddRectFilled(igGetWindowDrawList(), (ImVec2){ pos.x, pos.y + 2.0f }, (ImVec2){ pos.x + h - 4.0f, pos.y + h - 2.0f },
                             color, 0.0f, 0);
    igDummy((ImVec2){ h - 2.0f, h });
    igSameLine(0.0f, 2.0f);
    igTextUnformatted(name, NULL);
    igSameLine(0.0f, 12.0f);
}

/* Symbols of the blocks of data, in the colour of the modulation of their block. */
static void lte_data_points(struct m2sdr_plot *plot, const float complex *points, const uint8_t *mod, int n, ImU32 alpha)
{
    static const enum m2sdr_modulation order[] = { M2SDR_MOD_256QAM, M2SDR_MOD_64QAM, M2SDR_MOD_16QAM, M2SDR_MOD_QPSK };
    static float px[M2SDR_LTE_MAX_POINTS], py[M2SDR_LTE_MAX_POINTS];
    unsigned m;
    int i;

    for (m = 0; m < sizeof(order) / sizeof(order[0]); m++) {
        size_t count = 0;

        for (i = 0; i < n; i++) {
            if (mod[i] != order[m])
                continue;
            px[count]   = crealf(points[i]);
            py[count++] = cimagf(points[i]);
        }
        m2sdr_plot_points(plot, px, py, count, 1.0f, (lte_mod_colors[order[m]] & 0x00ffffffu) | alpha);
    }
}

static void draw_lte(struct an_ui *ui, bool live, bool *changed)
{
    static const double rates[5] = { 1.92e6, 3.84e6, 7.68e6, 15.36e6, 30.72e6 };
    static float px[M2SDR_LTE_MAX_POINTS], py[M2SDR_LTE_MAX_POINTS];
    static float complex ref[256];
    const struct m2sdr_lte_result *l;
    struct m2sdr_plot plot;
    ImTextureRef tex;
    ImVec2 avail, pos, sq;
    float side, left_w, line, right;
    double half_mhz;
    char text[200];
    bool held;
    int i;

    if (live) {
        igTextUnformatted("Sample rate:", NULL);
        for (i = 0; i < 5; i++) {
            snprintf(text, sizeof(text), "%.2f MSPS", rates[i] / 1e6);
            igSameLine(0.0f, 8.0f);
            if (igSmallButton(text)) {
                ui->set.sample_rate = rates[i];
                ui->set.bandwidth   = 0;
                view_full(ui);
                traces_reset(ui);
                *changed = true;
            }
        }
    }
    if (!ui->res.set.lte_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!ui->res.lte_valid) {
        igTextUnformatted("The sample rate must be 1.92, 3.84, 7.68, 15.36 or 30.72 MSPS: it sets how many resource "
                          "blocks around the centre are analysed.", NULL);
        return;
    }
    l = lte_shown(ui, &held);
    if (!l) {
        igPushTextWrapPos(0.0f);
        igText("No cell found (PSS quality %.1f): the capture must be tuned on the centre of an FDD carrier with a "
               "normal cyclic prefix.", ui->res.lte.sync_quality);
        igPopTextWrapPos();
        return;
    }
    tex._TexData = NULL;
    half_mhz     = 0.5 * 0.015 * (double)l->subcarriers;

    igGetContentRegionAvail(&avail);
    line   = igGetTextLineHeightWithSpacing();
    side   = avail.y - 2.0f * line < 0.45f * avail.x ? avail.y - 2.0f * line : 0.45f * avail.x;
    sq     = m2sdr_plot_square(side);
    left_w = avail.x - sq.x - 8.0f;

    /* Resource grid, and the blocks of data under it on the same time axis. */
    igBeginGroup();
    igGetCursorScreenPos(&pos);
    right = pos.x + left_w;
    igPushTextWrapPos(igGetCursorPosX() + left_w);
    if (held)
        igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "No cell in the last capture (PSS quality %.1f): the last frame read "
                      "is held, with its figures.", ui->res.lte.sync_quality);
    else if (l->mib)
        igText("Resource grid of frame %d, MHz from the centre against the time. Colours: dB over the reference signals.", l->sfn);
    else
        igTextUnformatted("Resource grid of a frame, MHz from the centre against the time. Colours: dB over the reference signals.", NULL);
    igPopTextWrapPos();
    if (m2sdr_plot_begin(&plot, "##ltegrid", (ImVec2){ left_w - LTE_SCALE_W, 0.62f * avail.y }, 0.0, 0.010, M2SDR_PLOT_TIME,
                         -half_mhz, half_mhz, "")) {
        static const double marks[] = { LTE_GRID_TOP_DB, 0.0, -20.0, LTE_GRID_BOTTOM_DB };
        const float x = plot.p1.x + 14.0f, h = plot.p1.y - plot.p0.y;
        unsigned n;

        tex._TexID = (ImTextureID)(uintptr_t)ui->lte_grid_tex;
        ImDrawList_AddImage(plot.dl, tex, plot.p0, plot.p1, (ImVec2){ 0.0f, 0.0f }, (ImVec2){ 1.0f, 1.0f }, 0xffffffffu);
        if (plot.hovered) {
            int q = (int)(plot.mouse_x / 0.010 * M2SDR_LTE_FRAME_SYMBOLS);
            int a = (int)((plot.mouse_y + half_mhz) / 0.015);

            q = q < 0 ? 0 : q >= M2SDR_LTE_FRAME_SYMBOLS ? M2SDR_LTE_FRAME_SYMBOLS - 1 : q;
            a = a < 0 ? 0 : a >= l->subcarriers ? l->subcarriers - 1 : a;
            snprintf(text, sizeof(text), "%.2f ms (slot %d, symbol %d), %+.3f MHz: %.1f dBFS, %+.1f dB", 1e3 * plot.mouse_x,
                     q / 7, q % 7, plot.mouse_y, l->grid_db[q][a], l->grid_db[q][a] - l->rsrp_db);
            m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
        }
        m2sdr_plot_end(&plot);
        /* Scale of the colours, in dB over a reference element. */
        for (i = 0; i < (int)h; i++)
            ImDrawList_AddRectFilled(plot.dl, (ImVec2){ x, plot.p0.y + (float)i }, (ImVec2){ x + 12.0f, plot.p0.y + (float)i + 1.0f },
                                     palette_color(ui->wf_palette, 1.0f - (float)i / h), 0.0f, 0);
        for (n = 0; n < sizeof(marks) / sizeof(marks[0]); n++) {
            const float y = plot.p0.y + h * (float)((LTE_GRID_TOP_DB - marks[n]) / (LTE_GRID_TOP_DB - LTE_GRID_BOTTOM_DB));
            ImVec2 ts;

            snprintf(text, sizeof(text), marks[n] != 0.0 ? "%+.0f" : "%.0f", marks[n]);
            igCalcTextSize(&ts, text, NULL, false, -1.0f);
            ImDrawList_AddText_Vec2(plot.dl, (ImVec2){ x + 16.0f, y - (n == 0 ? 0.0f : marks[n] == LTE_GRID_BOTTOM_DB ? ts.y : 0.5f * ts.y) },
                                    0xffc8c8c8u, text, NULL);
        }
    }
    if (!l->data) {
        igPushTextWrapPos(igGetCursorPosX() + left_w);
        igTextUnformatted(l->mib ? "Blocks of data: not analysed with four antenna ports." :
                          "Blocks of data: not analysed, the MIB of the cell is not decoded.", NULL);
        igPopTextWrapPos();
    } else {
        const double y0 = (double)l->data_first_rb - (double)l->data_offset / 12.0;

        igTextUnformatted("Blocks of data, by slot and resource block of the cell:", NULL);
        legend_item(lte_mod_colors[M2SDR_MOD_QPSK], "QPSK", right);
        legend_item(lte_mod_colors[M2SDR_MOD_16QAM], "16QAM", right);
        legend_item(lte_mod_colors[M2SDR_MOD_64QAM], "64QAM", right);
        legend_item(lte_mod_colors[M2SDR_MOD_256QAM], "256QAM", right);
        if (l->ports > 1)
            legend_item(LTE_COL_UNREAD, "in use, not read", right);
        legend_item(lte_mod_colors[M2SDR_MOD_AUTO], "empty", right);
        igNewLine();
        if (m2sdr_plot_begin(&plot, "##lteblocks", (ImVec2){ left_w - LTE_SCALE_W, 0.0f }, 0.0, 0.010, M2SDR_PLOT_TIME,
                             y0, y0 + (double)l->subcarriers / 12.0, "")) {
            tex._TexID = (ImTextureID)(uintptr_t)ui->lte_block_tex;
            ImDrawList_AddImage(plot.dl, tex, (ImVec2){ plot.p0.x, m2sdr_plot_y(&plot, l->data_first_rb + l->data_rb) },
                                (ImVec2){ plot.p1.x, m2sdr_plot_y(&plot, l->data_first_rb) },
                                (ImVec2){ 0.0f, 0.0f }, (ImVec2){ 1.0f, 1.0f }, 0xffffffffu);
            /* Subframes. */
            for (i = 1; i < 10; i++)
                m2sdr_plot_vline(&plot, 0.001 * i, 0x38ffffffu);
            if (plot.hovered) {
                const int ns = (int)(plot.mouse_x / 0.0005), b = (int)floor(plot.mouse_y) - l->data_first_rb;

                if (ns >= 0 && ns < M2SDR_LTE_FRAME_SLOTS && b >= 0 && b < l->data_rb) {
                    if (l->rb_unknown[ns][b])
                        snprintf(text, sizeof(text), "Slot %d, block %d: in use, not read", ns, b + l->data_first_rb);
                    else if (l->rb_modulation[ns][b] == M2SDR_MOD_AUTO)
                        snprintf(text, sizeof(text), "Slot %d, block %d: empty", ns, b + l->data_first_rb);
                    else
                        snprintf(text, sizeof(text), "Slot %d, block %d: %s, EVM %.2f %%", ns, b + l->data_first_rb,
                                 m2sdr_modulation_name((enum m2sdr_modulation)l->rb_modulation[ns][b]), 100.0 * l->rb_evm[ns][b]);
                    m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
                }
            }
            m2sdr_plot_end(&plot);
        }
    }
    igEndGroup();
    igSameLine(0.0f, 8.0f);

    /* Equalised symbols: data, PBCH and reference signals. */
    igBeginGroup();
    igGetCursorScreenPos(&pos);
    right = pos.x + sq.x;
    legend_item(lte_mod_colors[M2SDR_MOD_QPSK], "QPSK", right);
    legend_item(lte_mod_colors[M2SDR_MOD_16QAM], "16QAM", right);
    legend_item(lte_mod_colors[M2SDR_MOD_64QAM], "64QAM", right);
    legend_item(lte_mod_colors[M2SDR_MOD_256QAM], "256QAM", right);
    legend_item(LTE_COL_PBCH, "PBCH", right);
    legend_item(LTE_COL_RS, "reference signals", right);
    igNewLine();
    {
        /* The frames without a block that is read keep the symbols of the last one that had some. */
        const bool own = l->ndata > 0;
        const bool kept = !own && l->data && ui->lte_npoints > 0 && ui->seq - ui->lte_points_seq <= AN_LTE_HOLD;
        const enum m2sdr_modulation most = own && l->data_blocks ? l->data_modulation : M2SDR_MOD_QPSK;
        const int nref = m2sdr_constellation(most, ref);
        char label[96];

        snprintf(label, sizeof(label), "Q against I, equalised; crosses: the points of %s", m2sdr_modulation_name(most));
        if (m2sdr_plot_begin(&plot, "##lteconst", sq, -1.6, 1.6, M2SDR_PLOT_PLAIN, -1.6, 1.6, label)) {
            if (own)
                lte_data_points(&plot, l->data_points, l->data_points_modulation, l->ndata, 0xc0000000u);
            else if (kept)
                lte_data_points(&plot, ui->lte_points, ui->lte_points_mod, ui->lte_npoints, 0x50000000u);
            for (i = 0; i < l->npbch; i++) {
                px[i] = crealf(l->pbch_points[i]);
                py[i] = cimagf(l->pbch_points[i]);
            }
            m2sdr_plot_points(&plot, px, py, (size_t)l->npbch, 1.5f, LTE_COL_PBCH);
            for (i = 0; i < l->nrs; i++) {
                px[i] = crealf(l->rs_points[i]);
                py[i] = cimagf(l->rs_points[i]);
            }
            m2sdr_plot_points(&plot, px, py, (size_t)l->nrs, 1.0f, (LTE_COL_RS & 0x00ffffffu) | 0xc0000000u);
            for (i = 0; i < nref; i++) {
                const float x = m2sdr_plot_x(&plot, crealf(ref[i])), y = m2sdr_plot_y(&plot, cimagf(ref[i]));
                ImDrawList_AddLine(plot.dl, (ImVec2){ x - 4.0f, y }, (ImVec2){ x + 5.0f, y }, 0xa0ffffffu, 1.0f);
                ImDrawList_AddLine(plot.dl, (ImVec2){ x, y - 4.0f }, (ImVec2){ x, y + 5.0f }, 0xa0ffffffu, 1.0f);
            }
            if (kept) {
                snprintf(text, sizeof(text), "No data read in this frame: dimmed symbols of frame %d", ui->lte_points_sfn);
                m2sdr_plot_text(&plot, 5.0f, -20.0f, text, 0xff60a0ffu);
            } else if (!own && l->data) {
                m2sdr_plot_text(&plot, 5.0f, -20.0f, "No data read in this frame", 0xff60a0ffu);
            }
            m2sdr_plot_end(&plot);
        }
    }
    igEndGroup();
}

static void draw_lte_results(struct an_ui *ui)
{
    static const char *const phich[4] = { "1/6", "1/2", "1", "2" };
    const struct an_result *res = &ui->res;
    const struct m2sdr_lte_result *l;
    bool held;

    igSeparatorText("LTE downlink");
    l = lte_shown(ui, &held);
    if (!l) {
        igTextUnformatted("No cell.", NULL);
        return;
    }
    if (held) {
        igPushTextWrapPos(0.0f);
        igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "No cell in the last capture: these are the figures of the last frame read.");
        igPopTextWrapPos();
    }
    if (igBeginTable("##lte", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Cell identity", "%d (%d, %d)", l->cell_id, l->n_id_1, l->n_id_2);
        label_value("Carrier", "%.6f MHz", (res->set.freq_hz + l->freq_error_hz) / 1e6);
        label_value("Carrier offset", "%+.1f Hz", l->freq_error_hz);
        if (l->mib) {
            label_value("Resource blocks", "%d", l->n_rb);
            label_value("Antenna ports", "%d", l->ports);
            label_value("Frame number", "%d", l->sfn);
            label_value("PHICH", "%s, %s", l->phich_duration ? "extended" : "normal", phich[l->phich_resource & 3]);
        } else {
            label_value("MIB", "not decoded");
        }
        if (l->data && l->data_rb < l->n_rb)
            label_value("Analysed", "%d blocks, %d to %d", l->analysed_rb, l->data_first_rb, l->data_first_rb + l->data_rb - 1);
        else
            label_value("Analysed", "%d blocks", l->analysed_rb);
        label_value("RSRP", "%.1f dBFS", l->rsrp_db);
        label_value("RSSI", "%.1f dBFS", l->rssi_db);
        label_value("PSS, SSS", "%.1f, %.1f dBFS", l->pss_db, l->sss_db);
        label_value("RS EVM", "%.2f %%", 100.0 * l->rs_evm);
        if (l->mib)
            label_value("PBCH EVM", "%.2f %%", 100.0 * l->pbch_evm);
        if (l->data) {
            int used[M2SDR_MOD_COUNT] = { 0 }, ns, rb, m;
            char text[96] = "";

            for (ns = 0; ns < M2SDR_LTE_FRAME_SLOTS; ns++) {
                for (rb = 0; rb < M2SDR_LTE_MAX_ANALYSED_RB; rb++)
                    used[l->rb_modulation[ns][rb] % M2SDR_MOD_COUNT]++;
            }
            for (m = 0; m < M2SDR_MOD_COUNT; m++) {
                if (m != M2SDR_MOD_AUTO && used[m])
                    snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%s %d", text[0] ? ", " : "",
                             m2sdr_modulation_name((enum m2sdr_modulation)m), used[m]);
            }
            label_value("Data blocks", "%d", l->data_blocks);
            if (l->data_blocks)
                label_value("", "%s", text);
            label_value("Data, mostly", "%s", l->data_blocks ? m2sdr_modulation_name(l->data_modulation) : "none");
            if (l->data_blocks)
                label_value("Data EVM", "%.2f %%", 100.0 * l->data_evm);
            if (l->ports > 1) {
                label_value("Transmit diversity", "%d blocks", l->data_diversity);
                label_value("One precoded layer", "%d blocks", l->data_precoded);
                label_value("In use, not read", "%d blocks", l->data_unknown);
            }
        }
        igEndTable();
    }
    igPushTextWrapPos(0.0f);
    if (l->rs_evm > 1.0)
        igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "The reference signals are in the noise (EVM over 100 %%): the "
                      "levels and the blocks of data are not to be trusted.");
    if (l->data)
        igTextUnformatted("The control channels are not decoded: a block is taken as the constellation it fits best, "
                          "and its EVM is against the nearest points.", NULL);
    if (l->data && l->ports > 1)
        igTextUnformatted("Two antenna ports: a block is read when it is in transmit diversity or holds one layer. "
                          "One receiver does not separate two layers.", NULL);
    else if (l->mib && !l->data)
        igTextUnformatted("Data are not analysed with four antenna ports.", NULL);
    else if (!l->mib)
        igTextUnformatted("The MIB is not decoded: the width of the carrier and its antenna ports are not known, and the "
                          "data are not analysed.", NULL);
    igPopTextWrapPos();
}

static void draw_pn_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_phase_noise *pn = &res->pn;
    const struct an_floor *fl = ui->fl;
    char name[32];
    int i;

    igSeparatorText("Phase noise");
    if (!res->pn_valid) {
        igTextUnformatted("No carrier.", NULL);
        return;
    }
    if (igBeginTable("##pn", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Carrier", "%.6f MHz", (res->set.freq_hz + pn->carrier_hz) / 1e6);
        label_value("Level", "%.2f dBFS", pn->carrier_db);
        for (i = 0; i < PN_SPOTS; i++) {
            const double l = pn_interp(pn->offset_hz, pn->l_dbc_hz, pn->points, pn_spots[i]);
            const double floor = fl && fl->points ? pn_interp(fl->offset_hz, fl->l_dbc_hz, fl->points, pn_spots[i]) : NAN;

            if (!isfinite(l))
                continue;
            m2sdr_plot_format(name, sizeof(name), pn_spots[i], pn_spots[i], M2SDR_PLOT_FREQ);
            if (isfinite(floor))
                label_value(name, "%.1f dBc/Hz (floor %.1f)", l, floor);
            else
                label_value(name, "%.1f dBc/Hz", l);
        }
        label_value("Integrated", "%.4f deg RMS", pn->rms_deg);
        label_value("Jitter", "%.3f ps RMS", pn->jitter_s * 1e12);
        label_value("Residual FM", "%.2f Hz RMS", pn->residual_fm_hz);
        label_value("Range", "%.0f Hz to %.0f Hz", pn->lo_hz, pn->hi_hz);
        for (i = 0; i < pn->nspurs && i < 5; i++) {
            snprintf(name, sizeof(name), "Spur %.0f Hz", pn->spur_hz[i]);
            label_value(name, "%.1f dBc", pn->spur_dbc[i]);
        }
        igEndTable();
    }
    if (!fl || !fl->points) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted("No floor stored: the reading includes the phase noise of the board.", NULL);
        igPopTextWrapPos();
    }
}

static void draw_results(struct an_ui *ui, struct an_shared *sh, bool *changed)
{
    const struct an_result *res = &ui->res;
    const double off = res->set.ref_offset_db;
    const char *unit = off != 0.0 ? "dBm" : "dBFS";
    const float *trace = marker_trace(ui);
    char text[64];
    int m, i;

    if (ui->tab == 3) {
        draw_pn_results(ui);
        goto status;
    }
    if (ui->tab == 4) {
        draw_modulation_results(ui);
        goto status;
    }
    if (ui->tab == 5) {
        draw_lte_results(ui);
        goto status;
    }

    igSeparatorText("Markers");
    for (m = 0; m < UI_MARKERS; m++) {
        snprintf(text, sizeof(text), "M%d", m + 1);
        if (m)
            igSameLine(0.0f, -1.0f);
        igRadioButton_IntPtr(text, &ui->marker_sel, m);
    }
    if (igButton("Peak", (ImVec2){ 0.0f, 0.0f }) && res->npeaks) {
        ui->markers[ui->marker_sel].on        = true;
        ui->markers[ui->marker_sel].offset_hz = res->peaks[0].freq_hz;
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Next", (ImVec2){ 0.0f, 0.0f }) && res->npeaks) {
        /* The peak after the one the marker is on, by level. */
        int at = -1;
        for (i = 0; i < res->npeaks; i++) {
            if (fabs(res->peaks[i].freq_hz - ui->markers[ui->marker_sel].offset_hz) < 2.0 * res->psd.bin_hz)
                at = i;
        }
        ui->markers[ui->marker_sel].on        = true;
        ui->markers[ui->marker_sel].offset_hz = res->peaks[(at + 1) % res->npeaks].freq_hz;
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Off", (ImVec2){ 0.0f, 0.0f }))
        ui->markers[ui->marker_sel].on = false;
    if (sh->src->live && !sh->src->no_rf_init) {
        igSameLine(0.0f, -1.0f);
        if (igButton("To centre", (ImVec2){ 0.0f, 0.0f }) && ui->markers[ui->marker_sel].on) {
            ui->set.freq_hz += ui->markers[ui->marker_sel].offset_hz;
            ui->markers[ui->marker_sel].offset_hz = 0.0;
            traces_reset(ui);
            *changed = true;
        }
    }
    if (igBeginTable("##markers", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        for (m = 0; m < UI_MARKERS; m++) {
            const double level = trace_at(ui, trace, ui->markers[m].offset_hz);
            char name[16];

            if (!ui->markers[m].on)
                continue;
            snprintf(name, sizeof(name), "M%d", m + 1);
            label_value(name, "%.6f MHz", (res->set.freq_hz + ui->markers[m].offset_hz) / 1e6);
            label_value("", "%.2f %s, %.1f %s/Hz", level, unit, level - 10.0 * log10(res->psd.rbw_hz > 0 ? res->psd.rbw_hz : 1.0), unit);
            if (m > 0 && ui->markers[0].on)
                label_value("", "M1 %+.3f kHz, %+.2f dB", (ui->markers[m].offset_hz - ui->markers[0].offset_hz) / 1e3,
                            level - trace_at(ui, trace, ui->markers[0].offset_hz));
        }
        igEndTable();
    }

    igSeparatorText("Peaks");
    if (igBeginTable("##peaks", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        for (i = 0; i < res->npeaks; i++) {
            snprintf(text, sizeof(text), "%.6f MHz", (res->set.freq_hz + res->peaks[i].freq_hz) / 1e6);
            label_value(text, "%.2f %s", res->peaks[i].power_db + off, unit);
        }
        igEndTable();
    }

    igSeparatorText("Strongest line, as a tone");
    if (res->tone_valid && igBeginTable("##tone", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Frequency", "%.6f MHz", (res->set.freq_hz + res->tone.freq_hz) / 1e6);
        label_value("Level", "%.2f %s", res->tone.level_db + off, unit);
        label_value("SNR", "%.1f dB", res->tone.snr_db);
        label_value("SINAD", "%.1f dB", res->tone.sinad_db);
        label_value("SFDR", "%.1f dBc", res->tone.sfdr_dbc);
        label_value("THD", "%.1f dBc", res->tone.thd_dbc);
        label_value("Harmonics 2, 3", "%.1f, %.1f dBc", res->tone.harmonic_dbc[0], res->tone.harmonic_dbc[1]);
        igEndTable();
    }

    igSeparatorText("Channel");
    if (igBeginTable("##chan", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        if (res->chan_valid) {
            label_value("Power", "%.2f %s", res->chan_power_db + off, unit);
            for (i = 0; res->acpr_valid && i < res->acpr.adjacent; i++) {
                snprintf(text, sizeof(text), "ACPR %d", i + 1);
                label_value(text, "%.1f / %.1f dBc", res->acpr.lower_dbc[i], res->acpr.upper_dbc[i]);
            }
        } else if (res->set.chan_bw_hz > 0) {
            label_value("Power", "out of the span");
        }
        if (res->obw_valid) {
            label_value("OBW (99 %)", "%.3f kHz", (res->obw_hi_hz - res->obw_lo_hz) / 1e3);
            label_value("OBW centre", "%+.3f kHz", 0.5 * (res->obw_lo_hz + res->obw_hi_hz) / 1e3);
        }
        label_value("Noise", "%.1f %s/Hz", res->noise_db_hz + off, unit);
        igEndTable();
    }

    igSeparatorText("Time domain");
    if (igBeginTable("##time", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Mean power", "%.2f %s", res->stats.mean_db + off, unit);
        label_value("Peak power", "%.2f %s", res->stats.peak_db + off, unit);
        label_value("PAPR", "%.2f dB", res->stats.papr_db);
        label_value("DC offset", "%.1f %s", res->stats.dc_db + off, unit);
        label_value("I/Q gain", "%+.3f dB", res->stats.gain_imbalance_db);
        label_value("Quadrature", "%+.3f deg", res->stats.quadrature_deg);
        label_value("Clipped", "%.4f %%", 100.0 * res->stats.clipped);
        igEndTable();
    }

status:
    igSeparatorText("Status");
    {
        double capture_ms, analysis_ms;
        char status[256];

        pthread_mutex_lock(&sh->lock);
        capture_ms  = sh->capture_ms;
        analysis_ms = sh->analysis_ms;
        snprintf(status, sizeof(status), "%s", sh->status);
        pthread_mutex_unlock(&sh->lock);
        igText("Capture %.0f ms, analysis %.0f ms", capture_ms, analysis_ms);
        igText("%zu samples, %u segments", capture_samples(&res->set), res->psd.segments);
        if (status[0]) {
            igPushTextWrapPos(0.0f);
            igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "%s", status);
            igPopTextWrapPos();
        }
        if (ui->message[0])
            igTextUnformatted(ui->message, NULL);
    }
    if (igButton("Export CSV", (ImVec2){ 0.0f, 0.0f }) && res->psd.bins) {
        char path[64];
        const time_t now = time(NULL);
        struct tm tm;

        localtime_r(&now, &tm);
        strftime(path, sizeof(path), "m2sdr_analyzer_%Y%m%d_%H%M%S.csv", &tm);
        if (export_csv(path, res) == 0)
            snprintf(ui->message, sizeof(ui->message), "Trace written to %s", path);
        else
            snprintf(ui->message, sizeof(ui->message), "Could not write %s", path);
    }
}

static int run_gui(struct an_source *src, const struct an_settings *set, int first_view, struct an_floor *fl,
                   const char *floor_path)
{
    const char *glsl_version = "#version 130";
    struct an_shared sh;
    struct an_ui ui;
    SDL_Window *window;
    SDL_GLContext gl_context;
    pthread_t worker;
    bool quit = false;
    char title[1100];
    int t;

    memset(&sh, 0, sizeof(sh));
    memset(&ui, 0, sizeof(ui));
    pthread_mutex_init(&sh.lock, NULL);
    sh.set = *set;
    sh.src = src;
    sh.run = true;
    ui.set = *set;
    ui.trace_on[TRACE_LIVE] = true;
    ui.avg_target  = 16;
    ui.ref_db      = 0.0 + set->ref_offset_db;
    ui.range_db    = 130.0;
    ui.wf_on       = true;
    ui.wf_auto     = true;
    ui.fl          = fl;
    ui.floor_path  = floor_path;
    ui.tab         = first_view;
    view_full(&ui);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s (use --report without a display)\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    snprintf(title, sizeof(title), "M2SDR Analyzer - %s", src->name);
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 960,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    igCreateContext(NULL);
    igStyleColorsDark(NULL);
    igGetIO()->IniFilename = NULL;
    if (!m2sdr_imgui_sdl2_init_for_opengl(window, gl_context) || !m2sdr_imgui_opengl3_init(glsl_version)) {
        fprintf(stderr, "ImGui backend init failed\n");
        igDestroyContext(NULL);
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    waterfall_init(&ui);
    if (pthread_create(&worker, NULL, worker_main, &sh) != 0) {
        fprintf(stderr, "Could not start the worker thread\n");
        quit = true;
    }

    while (!quit) {
        bool changed = false;
        SDL_Event e;

        while (SDL_PollEvent(&e)) {
            m2sdr_imgui_sdl2_process_event(&e);
            if (e.type == SDL_QUIT)
                quit = true;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window))
                quit = true;
            if (e.type == SDL_KEYDOWN && e.key.repeat == 0 && !igGetIO()->WantTextInput) {
                if (e.key.keysym.sym == SDLK_SPACE) {
                    pthread_mutex_lock(&sh.lock);
                    sh.run = !sh.run;
                    pthread_mutex_unlock(&sh.lock);
                } else if (e.key.keysym.sym == SDLK_p && ui.res.npeaks) {
                    ui.markers[ui.marker_sel].on        = true;
                    ui.markers[ui.marker_sel].offset_hz = ui.res.peaks[0].freq_hz;
                } else if (e.key.keysym.sym == SDLK_f) {
                    view_full(&ui);
                } else if (e.key.keysym.sym == SDLK_r) {
                    traces_reset(&ui);
                }
            }
        }

        /* New result of the worker. */
        pthread_mutex_lock(&sh.lock);
        if (sh.seq != ui.seq && result_copy(&ui.res, &sh.res) == 0) {
            ui.seq = sh.seq;
            pthread_mutex_unlock(&sh.lock);
            if (traces_update(&ui) == 0)
                waterfall_push(&ui);
            lte_keep(&ui);
        } else {
            pthread_mutex_unlock(&sh.lock);
        }

        m2sdr_imgui_opengl3_new_frame();
        m2sdr_imgui_sdl2_new_frame();
        igNewFrame();
        {
            ImGuiIO *io = igGetIO();
            igSetNextWindowPos((ImVec2){ 0, 0 }, ImGuiCond_Always, (ImVec2){ 0, 0 });
            igSetNextWindowSize(io->DisplaySize, ImGuiCond_Always);
        }
        igBegin("##M2SDRAnalyzerMain", NULL,
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
        if (igBeginChild_Str("##controls", (ImVec2){ 280.0f, 0.0f }, 0, 0))
            draw_controls(&ui, &sh, &changed);
        igEndChild();
        igSameLine(0.0f, -1.0f);
        {
            ImVec2 avail;
            igGetContentRegionAvail(&avail);
            if (igBeginChild_Str("##views", (ImVec2){ avail.x - 330.0f, 0.0f }, 0, 0)) {
                if (igBeginTabBar("##tabs", 0)) {
                    /* The view asked on the command line is selected once. */
                    const int select = first_view;

                    first_view = -1;
                    int tab = ui.tab;

                    if (igBeginTabItem("Spectrum", NULL, select == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                        tab = 0;
                        draw_spectrum(&ui, &changed);
                        igEndTabItem();
                    }
                    if (igBeginTabItem("Time", NULL, select == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                        tab = 1;
                        draw_time(&ui);
                        igEndTabItem();
                    }
                    if (igBeginTabItem("Statistics", NULL, select == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                        tab = 2;
                        draw_statistics(&ui);
                        igEndTabItem();
                    }
                    if (igBeginTabItem("Phase noise", NULL, select == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
                        tab = 3;
                        draw_phase_noise(&ui, &changed);
                        igEndTabItem();
                    }
                    if (igBeginTabItem("Modulation", NULL, select == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
                        tab = 4;
                        draw_modulation(&ui, &changed);
                        igEndTabItem();
                    }
                    if (igBeginTabItem("LTE", NULL, select == 5 ? ImGuiTabItemFlags_SetSelected : 0)) {
                        tab = 5;
                        draw_lte(&ui, src->live && !src->no_rf_init, &changed);
                        igEndTabItem();
                    }
                    if (select < 0 && (tab == 5) != ui.set.lte_on) {
                        ui.set.lte_on = tab == 5;
                        changed = true;
                    }
                    /* The phase noise and the demodulation need their own captures: they are
                     * measured while their view is shown. */
                    if (select < 0 && (tab == 3) != ui.set.pn_on) {
                        ui.set.pn_on = tab == 3;
                        changed = true;
                    }
                    if (select < 0 && (tab == 4) != ui.set.demod_on) {
                        ui.set.demod_on = tab == 4;
                        changed = true;
                    }
                    if (select < 0)
                        ui.tab = tab;
                    igEndTabBar();
                }
            }
            igEndChild();
        }
        igSameLine(0.0f, -1.0f);
        if (igBeginChild_Str("##results", (ImVec2){ 0.0f, 0.0f }, 0, 0))
            draw_results(&ui, &sh, &changed);
        igEndChild();
        igEnd();

        if (changed) {
            pthread_mutex_lock(&sh.lock);
            sh.set = ui.set;
            pthread_mutex_unlock(&sh.lock);
        }

        igRender();
        {
            ImGuiIO *io = igGetIO();
            glViewport(0, 0, (int)io->DisplaySize.x, (int)io->DisplaySize.y);
            glClearColor(0.07f, 0.08f, 0.09f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            m2sdr_imgui_opengl3_render_draw_data(igGetDrawData());
        }
        SDL_GL_SwapWindow(window);
    }

    pthread_mutex_lock(&sh.lock);
    sh.quit = true;
    pthread_mutex_unlock(&sh.lock);
    pthread_join(worker, NULL);

    m2sdr_imgui_opengl3_shutdown();
    m2sdr_imgui_sdl2_shutdown();
    igDestroyContext(NULL);
    glDeleteTextures(1, &ui.wf_tex);
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    for (t = 0; t < UI_TRACES; t++)
        free(ui.trace[t]);
    free(ui.avg_lin);
    free(ui.wf_rgba);
    free(ui.lte_rgba);
    free(ui.lte_last);
    free(ui.res.psd.power);
    free(sh.res.psd.power);
    pthread_mutex_destroy(&sh.lock);
    return 0;
}

#endif /* M2SDR_ANALYZER_GUI */

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR Signal Analyzer Utility.\n"
           "usage: m2sdr_analyzer [options]                  Board RX\n"
           "       m2sdr_analyzer [options] --file PATH      SigMF or raw capture\n"
           "\n"
           "Spectrum with markers, channel power, occupied bandwidth and ACPR, the figures of a\n"
           "tone, time-domain statistics and phase noise. It only receives, unless --tx-tone is given.\n"
           "\n");
    m2sdr_cli_print_device_help();
    printf("\n"
           "Board options:\n"
           "      --freq HZ          Centre frequency (default: %.0f).\n"
           "      --sample-rate HZ   Sample rate (default: %.0f).\n"
           "      --bandwidth HZ     RF bandwidth (default: 0.9 x the sample rate).\n"
           "      --rx-gain DB       Manual RX gain (default: slow AGC).\n"
           "      --channel N        RX channel, 1 or 2 (default: 1).\n"
           "      --refclk-ppm V     Reference trim in ppm, 'stored' or 'none' (default: the\n"
           "                         calibration record of the board when it has one, see m2sdr_cal).\n"
           "      --sync MODE        Clock source: internal, external or fpga (default: internal).\n"
           "      --refclk-freq HZ   Reference frequency in Hz (default: 38400000).\n"
           "\n"
           "File options:\n"
           "      --file PATH        SigMF metadata or data file, or raw interleaved samples.\n"
           "      --format FMT       Raw file: sc16 or sc8 (default: sc16).\n"
           "      --nbits N          Raw file: significant bits of a sample (default: 12, 8 for sc8).\n"
           "      --nchannels N      Raw file: channels interleaved (default: 2).\n"
           "      --frame-header     Raw file: a 16-byte header starts every frame.\n"
           "      --frame-size N     Raw file: bytes of a frame with its header (default: %u).\n"
           "      --capture-index N  SigMF: capture to open (default: 0).\n"
           "      --offset S         Start, in seconds from the start of the file (default: 0).\n"
           "      --freq HZ, --sample-rate HZ   When the file does not give them.\n"
           "\n"
           "Measurement options:\n"
           "      --fft-len N        FFT length, a power of two (default: %d).\n"
           "      --window NAME      hann, flattop, blackman-harris or rect (default: hann).\n"
           "      --averages N       Segments averaged in a capture (default: %d).\n"
           "      --channel-bw HZ    Channel measured: power, ACPR, occupied bandwidth.\n"
           "      --channel-offset HZ   Its centre, from the centre of the capture (default: 0).\n"
           "      --channel-spacing HZ  Spacing of the adjacent channels (default: the bandwidth).\n"
           "      --ref-offset DB    Added to the levels, to read dBm at the connector: the board\n"
           "                         has no absolute level calibration.\n"
           "\n"
           "Modulation options:\n"
           "      --demod            With --report: demodulate the strongest signal, a single carrier\n"
           "                         with PSK or QAM: constellation, EVM, MER, I/Q errors.\n"
           "      --modulation NAME  auto, bpsk, qpsk, 8psk, 16qam, 32qam, 64qam, 128qam or 256qam\n"
           "                         (default: auto).\n"
           "      --symbol-rate HZ   Symbols per second (default: found from the signal).\n"
           "      --rolloff A        Roll-off of the pulse (default: found from the signal).\n"
           "      --pulse NAME       Transmit pulse: rrc, rc or rect (default: rrc).\n"
           "      --demod-samples N  Samples of its capture (default: %d).\n"
           "      --no-rf-init       Board: leave the RFIC as it is, set by m2sdr_rf, and only receive;\n"
           "                         --freq and --sample-rate then only label the results.\n"
           "\n"
           "LTE options:\n"
           "      --lte              With --report: analyse an LTE downlink carrier, FDD with a normal\n"
           "                         cyclic prefix, tuned on its centre: cell, MIB, EVM, data blocks.\n"
           "                         The sample rate is 1.92, 3.84, 7.68, 15.36 or 30.72 MSPS, for\n"
           "                         6, 15, 25, 50 or 100 resource blocks around the centre.\n"
           "\n"
           "Phase noise options:\n"
           "      --phase-noise      With --report: phase noise of the strongest carrier.\n"
           "      --pn-samples N     Samples of its capture (default: %d): more reach lower offsets.\n"
           "      --pn-from HZ, --pn-to HZ   Integration limits (default: the offsets measured).\n"
           "      --pn-floor PATH    File of the floor of the board (default: one per board,\n"
           "                         ~/.config/litex_m2sdr/pn_floor_<dna>.csv).\n"
           "      --pn-floor-save    With --report: store the phase noise read as this floor.\n"
           "      --tx-tone          Board: send the test tone of the RFIC on TX, at the centre plus a\n"
           "                         thirty-second of the sample rate. For a cable from TX to RX.\n"
           "      --tx-att DB        Attenuation of the test tone (default: %d).\n"
           "\n"
           "Output options:\n"
           "      --view NAME        View shown first: spectrum, time, statistics, phase-noise,\n"
           "                         modulation or lte.\n"
           "      --report           Measure one capture and print the result, without a display.\n"
           "      --json             With --report: JSON.\n"
           "      --export-csv PATH  With --report: write the spectrum to a CSV file.\n"
           "  -h, --help             Show this help message.\n"
           "\n"
           "Window: left click places the selected marker, the wheel zooms, a right drag moves the\n"
           "span, a double click shows it all. Space: run or stop. P: marker to the peak. F: full\n"
           "span. R: restart the traces.\n",
           AN_DEFAULT_FREQ_HZ, AN_DEFAULT_SAMPLE_RATE, M2SDR_BUFFER_BYTES, AN_DEFAULT_FFT_LEN, AN_DEFAULT_AVERAGES,
           1 << AN_DEMOD_DEFAULT_LOG2, 1 << AN_PN_DEFAULT_LOG2, AN_TX_TONE_ATT_DB);
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    static struct option options[] = {
        { "help",            no_argument,       NULL, 'h' },
        { "device",          required_argument, NULL, 'd' },
        { "freq",            required_argument, NULL, 1 },
        { "sample-rate",     required_argument, NULL, 2 },
        { "bandwidth",       required_argument, NULL, 3 },
        { "rx-gain",         required_argument, NULL, 4 },
        { "channel",         required_argument, NULL, 5 },
        { "refclk-ppm",      required_argument, NULL, 6 },
        { "sync",            required_argument, NULL, 7 },
        { "refclk-freq",     required_argument, NULL, 8 },
        { "file",            required_argument, NULL, 9 },
        { "format",          required_argument, NULL, 10 },
        { "nbits",           required_argument, NULL, 11 },
        { "nchannels",       required_argument, NULL, 12 },
        { "frame-header",    no_argument,       NULL, 13 },
        { "frame-size",      required_argument, NULL, 14 },
        { "capture-index",   required_argument, NULL, 15 },
        { "offset",          required_argument, NULL, 16 },
        { "fft-len",         required_argument, NULL, 17 },
        { "window",          required_argument, NULL, 18 },
        { "averages",        required_argument, NULL, 19 },
        { "channel-bw",      required_argument, NULL, 20 },
        { "channel-offset",  required_argument, NULL, 21 },
        { "channel-spacing", required_argument, NULL, 22 },
        { "ref-offset",      required_argument, NULL, 23 },
        { "report",          no_argument,       NULL, 24 },
        { "json",            no_argument,       NULL, 25 },
        { "export-csv",      required_argument, NULL, 26 },
        { "view",            required_argument, NULL, 27 },
        { "phase-noise",     no_argument,       NULL, 28 },
        { "pn-samples",      required_argument, NULL, 29 },
        { "pn-from",         required_argument, NULL, 30 },
        { "pn-to",           required_argument, NULL, 31 },
        { "pn-floor",        required_argument, NULL, 32 },
        { "pn-floor-save",   no_argument,       NULL, 33 },
        { "tx-tone",         no_argument,       NULL, 34 },
        { "tx-att",          required_argument, NULL, 35 },
        { "demod",           no_argument,       NULL, 36 },
        { "modulation",      required_argument, NULL, 37 },
        { "symbol-rate",     required_argument, NULL, 38 },
        { "rolloff",         required_argument, NULL, 39 },
        { "pulse",           required_argument, NULL, 40 },
        { "demod-samples",   required_argument, NULL, 41 },
        { "no-rf-init",      no_argument,       NULL, 42 },
        { "lte",             no_argument,       NULL, 43 },
        { NULL, 0, NULL, 0 }
    };
    struct m2sdr_cli_device cli_dev;
    struct an_settings set;
    struct an_source src;
    const char *file = NULL, *csv_path = NULL, *trim = NULL, *floor_arg = NULL, *floor_path = NULL;
    static struct an_floor fl;
    static char floor_default[1024];
    bool floor_store = false;
    bool report = false, json = false, freq_given = false, rate_given = false, nbits_given = false;
    double offset_s = 0.0;
    int capture_index = 0, view = 0;
    char err[256];
    int c, status;

    m2sdr_cli_handle_version(argc, argv);
    m2sdr_cli_device_init(&cli_dev);
    memset(&set, 0, sizeof(set));
    memset(&src, 0, sizeof(src));
    set.freq_hz      = AN_DEFAULT_FREQ_HZ;
    set.sample_rate  = AN_DEFAULT_SAMPLE_RATE;
    set.agc          = true;
    set.rx_gain      = 40;
    set.fft_len      = AN_DEFAULT_FFT_LEN;
    set.window       = M2SDR_WINDOW_HANN;
    set.averages     = AN_DEFAULT_AVERAGES;
    src.sample_bytes = 2;
    src.nbits        = 12;
    src.nchannels    = 2;
    src.frame_size   = M2SDR_BUFFER_BYTES;
    src.loop         = true;
    src.tx_att       = AN_TX_TONE_ATT_DB;
    set.pn_log2      = AN_PN_DEFAULT_LOG2;
    set.demod_log2   = AN_DEMOD_DEFAULT_LOG2;

    while ((c = getopt_long(argc, argv, "hd:c:i:p:", options, NULL)) != -1) {
        double v;
        int n;

        switch (c) {
        case 'h':
            help();
            return 0;
        case 'd': case 'c': case 'i': case 'p':
            if (m2sdr_cli_handle_device_option(&cli_dev, c, optarg) != 0)
                return 1;
            break;
        case 1:
            if (m2sdr_cli_parse_double(optarg, &set.freq_hz) != 0 || set.freq_hz < 0) {
                m2sdr_cli_error("invalid frequency '%s'", optarg);
                return 1;
            }
            freq_given = true;
            break;
        case 2:
            if (m2sdr_cli_parse_double(optarg, &set.sample_rate) != 0 || !(set.sample_rate > 0)) {
                m2sdr_cli_error("invalid sample rate '%s'", optarg);
                return 1;
            }
            rate_given = true;
            break;
        case 3:
            if (m2sdr_cli_parse_double(optarg, &set.bandwidth) != 0 || set.bandwidth < 0) {
                m2sdr_cli_error("invalid bandwidth '%s'", optarg);
                return 1;
            }
            break;
        case 4:
            if (m2sdr_cli_parse_int_range(optarg, -10, 77, &set.rx_gain) != 0) {
                m2sdr_cli_error("invalid RX gain '%s'", optarg);
                return 1;
            }
            set.agc = false;
            break;
        case 5:
            if (m2sdr_cli_parse_int_range(optarg, 1, 2, &n) != 0) {
                m2sdr_cli_error("invalid channel '%s' (expected 1 or 2)", optarg);
                return 1;
            }
            set.channel = n - 1;
            break;
        case 6:  trim = optarg; break;
        case 7:  src.sync = optarg; break;
        case 8:
            if (m2sdr_cli_parse_double(optarg, &v) != 0 || !(v > 0)) {
                m2sdr_cli_error("invalid refclk frequency '%s'", optarg);
                return 1;
            }
            src.refclk_freq = (int64_t)v;
            break;
        case 9:  file = optarg; break;
        case 10:
            if (strcmp(optarg, "sc16") == 0) {
                src.sample_bytes = 2;
            } else if (strcmp(optarg, "sc8") == 0) {
                src.sample_bytes = 1;
                if (!nbits_given)
                    src.nbits = 8;
            } else {
                m2sdr_cli_invalid_choice("format", optarg, "sc16 or sc8");
                return 1;
            }
            break;
        case 11:
            if (m2sdr_cli_parse_int_range(optarg, 2, 16, &src.nbits) != 0) {
                m2sdr_cli_error("invalid nbits '%s' (expected 2 to 16)", optarg);
                return 1;
            }
            nbits_given = true;
            break;
        case 12:
            if (m2sdr_cli_parse_int_range(optarg, 1, 8, &src.nchannels) != 0) {
                m2sdr_cli_error("invalid nchannels '%s' (expected 1 to 8)", optarg);
                return 1;
            }
            break;
        case 13: src.frame_header = true; break;
        case 14:
            if (m2sdr_cli_parse_int_range(optarg, 32, 1 << 26, &n) != 0) {
                m2sdr_cli_error("invalid frame size '%s'", optarg);
                return 1;
            }
            src.frame_size = (size_t)n;
            break;
        case 15:
            if (m2sdr_cli_parse_int_range(optarg, 0, 1 << 20, &capture_index) != 0) {
                m2sdr_cli_error("invalid capture index '%s'", optarg);
                return 1;
            }
            break;
        case 16:
            if (m2sdr_cli_parse_double(optarg, &offset_s) != 0 || offset_s < 0) {
                m2sdr_cli_error("invalid offset '%s'", optarg);
                return 1;
            }
            break;
        case 17:
            if (m2sdr_cli_parse_int_range(optarg, AN_MIN_FFT_LEN, AN_MAX_FFT_LEN, &set.fft_len) != 0 ||
                (set.fft_len & (set.fft_len - 1))) {
                m2sdr_cli_error("invalid FFT length '%s' (expected a power of two from %d to %d)", optarg,
                                AN_MIN_FFT_LEN, AN_MAX_FFT_LEN);
                return 1;
            }
            break;
        case 18:
            if (strcmp(optarg, "hann") == 0) {
                set.window = M2SDR_WINDOW_HANN;
            } else if (strcmp(optarg, "flattop") == 0) {
                set.window = M2SDR_WINDOW_FLATTOP;
            } else if (strcmp(optarg, "blackman-harris") == 0) {
                set.window = M2SDR_WINDOW_BLACKMAN_HARRIS;
            } else if (strcmp(optarg, "rect") == 0) {
                set.window = M2SDR_WINDOW_RECTANGULAR;
            } else {
                m2sdr_cli_invalid_choice("window", optarg, "hann, flattop, blackman-harris or rect");
                return 1;
            }
            break;
        case 19:
            if (m2sdr_cli_parse_int_range(optarg, 1, AN_MAX_AVERAGES, &set.averages) != 0) {
                m2sdr_cli_error("invalid averages '%s' (expected 1 to %d)", optarg, AN_MAX_AVERAGES);
                return 1;
            }
            break;
        case 20:
            if (m2sdr_cli_parse_double(optarg, &set.chan_bw_hz) != 0 || set.chan_bw_hz < 0) {
                m2sdr_cli_error("invalid channel bandwidth '%s'", optarg);
                return 1;
            }
            break;
        case 21:
            if (m2sdr_cli_parse_double(optarg, &set.chan_offset_hz) != 0) {
                m2sdr_cli_error("invalid channel offset '%s'", optarg);
                return 1;
            }
            break;
        case 22:
            if (m2sdr_cli_parse_double(optarg, &set.chan_spacing_hz) != 0 || set.chan_spacing_hz < 0) {
                m2sdr_cli_error("invalid channel spacing '%s'", optarg);
                return 1;
            }
            break;
        case 23:
            if (m2sdr_cli_parse_double(optarg, &set.ref_offset_db) != 0) {
                m2sdr_cli_error("invalid reference offset '%s'", optarg);
                return 1;
            }
            break;
        case 24: report = true; break;
        case 25: json = true; break;
        case 26: csv_path = optarg; break;
        case 27:
            if (strcmp(optarg, "spectrum") == 0) {
                view = 0;
            } else if (strcmp(optarg, "time") == 0) {
                view = 1;
            } else if (strcmp(optarg, "statistics") == 0) {
                view = 2;
            } else if (strcmp(optarg, "phase-noise") == 0) {
                view = 3;
            } else if (strcmp(optarg, "modulation") == 0) {
                view = 4;
            } else if (strcmp(optarg, "lte") == 0) {
                view = 5;
            } else {
                m2sdr_cli_invalid_choice("view", optarg, "spectrum, time, statistics, phase-noise, modulation or lte");
                return 1;
            }
            break;
        case 28: set.pn_on = true; break;
        case 29:
            if (m2sdr_cli_parse_double(optarg, &v) != 0 || v < (double)(1 << AN_PN_MIN_LOG2) || v > (double)(1 << AN_PN_MAX_LOG2)) {
                m2sdr_cli_error("invalid phase noise samples '%s' (expected %d to %d)", optarg,
                                1 << AN_PN_MIN_LOG2, 1 << AN_PN_MAX_LOG2);
                return 1;
            }
            /* The power of two at or below. */
            for (set.pn_log2 = AN_PN_MIN_LOG2; (double)((size_t)2 << set.pn_log2) <= v; set.pn_log2++)
                ;
            break;
        case 30:
            if (m2sdr_cli_parse_double(optarg, &set.pn_lo_hz) != 0 || set.pn_lo_hz < 0) {
                m2sdr_cli_error("invalid offset '%s'", optarg);
                return 1;
            }
            break;
        case 31:
            if (m2sdr_cli_parse_double(optarg, &set.pn_hi_hz) != 0 || set.pn_hi_hz < 0) {
                m2sdr_cli_error("invalid offset '%s'", optarg);
                return 1;
            }
            break;
        case 32: floor_arg = optarg; break;
        case 33: floor_store = true; break;
        case 36: set.demod_on = true; break;
        case 37:
            for (n = M2SDR_MOD_AUTO; n < M2SDR_MOD_COUNT; n++) {
                if (strcasecmp(optarg, m2sdr_modulation_name((enum m2sdr_modulation)n)) == 0)
                    break;
            }
            if (n == M2SDR_MOD_COUNT) {
                m2sdr_cli_invalid_choice("modulation", optarg, "auto, bpsk, qpsk, 8psk, 16qam, 32qam, 64qam, 128qam or 256qam");
                return 1;
            }
            set.demod.modulation = (enum m2sdr_modulation)n;
            break;
        case 38:
            if (m2sdr_cli_parse_double(optarg, &set.demod.symbol_rate) != 0 || set.demod.symbol_rate < 0) {
                m2sdr_cli_error("invalid symbol rate '%s'", optarg);
                return 1;
            }
            break;
        case 39:
            if (m2sdr_cli_parse_double(optarg, &set.demod.rolloff) != 0 ||
                (set.demod.rolloff != 0 && (set.demod.rolloff < 0.05 || set.demod.rolloff > 1.0))) {
                m2sdr_cli_error("invalid roll-off '%s' (expected 0.05 to 1, or 0 to find it)", optarg);
                return 1;
            }
            break;
        case 40:
            if (strcmp(optarg, "rrc") == 0) {
                set.demod.pulse = M2SDR_PULSE_RRC;
            } else if (strcmp(optarg, "rc") == 0) {
                set.demod.pulse = M2SDR_PULSE_RC;
            } else if (strcmp(optarg, "rect") == 0) {
                set.demod.pulse = M2SDR_PULSE_RECT;
            } else {
                m2sdr_cli_invalid_choice("pulse", optarg, "rrc, rc or rect");
                return 1;
            }
            break;
        case 41:
            if (m2sdr_cli_parse_double(optarg, &v) != 0 || v < (double)(1 << AN_DEMOD_MIN_LOG2) || v > (double)(1 << AN_DEMOD_MAX_LOG2)) {
                m2sdr_cli_error("invalid demodulation samples '%s' (expected %d to %d)", optarg,
                                1 << AN_DEMOD_MIN_LOG2, 1 << AN_DEMOD_MAX_LOG2);
                return 1;
            }
            for (set.demod_log2 = AN_DEMOD_MIN_LOG2; (double)((size_t)2 << set.demod_log2) <= v; set.demod_log2++)
                ;
            break;
        case 42: src.no_rf_init = true; break;
        case 43: set.lte_on = true; break;
        case 34: src.tx_tone = true; break;
        case 35:
            if (m2sdr_cli_parse_int_range(optarg, 0, 89, &src.tx_att) != 0) {
                m2sdr_cli_error("invalid TX attenuation '%s' (expected 0 to 89)", optarg);
                return 1;
            }
            break;
        default:
            return 1;
        }
    }
    if (optind < argc) {
        m2sdr_cli_error("unexpected argument '%s'", argv[optind]);
        return 1;
    }
    if ((json || csv_path || floor_store) && !report) {
        m2sdr_cli_error("--json, --export-csv and --pn-floor-save go with --report");
        return 1;
    }
    if (floor_store)
        set.pn_on = true;
    if (view == 3)
        set.pn_on = true;
    if (view == 4)
        set.demod_on = true;
    if (view == 5)
        set.lte_on = true;
    if (src.no_rf_init && (file || src.tx_tone)) {
        m2sdr_cli_error("--no-rf-init is for a board, without --tx-tone");
        return 1;
    }
    if (src.tx_tone && file) {
        m2sdr_cli_error("--tx-tone is for a board");
        return 1;
    }

    if (file) {
        if (src.frame_header && src.frame_size <= AN_FRAME_HEADER_BYTES) {
            m2sdr_cli_error("the frame size must be above %d bytes", AN_FRAME_HEADER_BYTES);
            return 1;
        }
        if (file_open(&src, file, capture_index, err, sizeof(err)) != 0) {
            fprintf(stderr, "Could not open the capture: %s\n", err);
            source_close(&src);
            return 1;
        }
        if (!rate_given && src.file_rate > 0)
            set.sample_rate = src.file_rate;
        if (!freq_given)
            set.freq_hz = src.file_freq;
        if (!rate_given && !(src.file_rate > 0))
            fprintf(stderr, "The file does not give its sample rate: %.0f assumed (--sample-rate).\n", set.sample_rate);
        if (set.channel >= src.nchannels) {
            fprintf(stderr, "The file has %d channel%s.\n", src.nchannels, src.nchannels == 1 ? "" : "s");
            source_close(&src);
            return 1;
        }
        src.position = (uint64_t)(offset_s * set.sample_rate);
        if (src.position >= src.total_sets)
            src.position = 0;
        if (report)
            src.loop = false;
    } else {
        struct m2sdr_cal_record rec;

        if (!m2sdr_cli_finalize_device(&cli_dev))
            return 1;
        if (m2sdr_open(&src.dev, m2sdr_cli_device_id(&cli_dev)) != 0) {
            fprintf(stderr, "Could not open device: %s\n", m2sdr_cli_device_id(&cli_dev));
            return 1;
        }
        src.live = true;
        if (json)
            (void)m2sdr_set_log_enabled(false);     /* The JSON is alone on the output. */
        snprintf(src.name, sizeof(src.name), "%s", m2sdr_cli_device_id(&cli_dev));

        /* Reference trim: the calibration record of the board unless told otherwise. */
        if (trim && strcmp(trim, "none") == 0) {
            src.trim_ppm = 0.0;
        } else if (trim && strcmp(trim, "stored") != 0) {
            if (m2sdr_cli_parse_double(trim, &src.trim_ppm) != 0 || fabs(src.trim_ppm) > M2SDR_REFCLK_PPM_MAX) {
                m2sdr_cli_error("invalid refclk ppm '%s' (expected 'stored', 'none' or -%g to %g)", trim,
                                (double)M2SDR_REFCLK_PPM_MAX, (double)M2SDR_REFCLK_PPM_MAX);
                source_close(&src);
                return 1;
            }
        } else if (m2sdr_cal_load(src.dev, M2SDR_CAL_FLASH_OFFSET, &rec) != M2SDR_CAL_SOURCE_NONE) {
            src.trim_ppm = rec.refclk_ppm;
            fprintf(stderr, "RefClk trim %+.3f ppm, from the calibration record of the board.\n", rec.refclk_ppm);
        } else if (trim) {
            fprintf(stderr, "No calibration record for this board: run m2sdr_cal measure --store.\n");
            source_close(&src);
            return 1;
        }
    }

    /* Floor of the phase noise: the file given, or the one of the board. */
    if (floor_arg) {
        floor_path = floor_arg;
    } else if (src.live && m2sdr_get_fpga_dna(src.dev, &src.dna) == M2SDR_ERR_OK &&
               floor_default_path(src.dna, floor_default, sizeof(floor_default)) == 0) {
        floor_path = floor_default;
    }
    if (floor_path && floor_load(&fl, floor_path) != 0 && floor_arg && !floor_store)
        fprintf(stderr, "No phase noise floor in %s.\n", floor_path);
    if (floor_store && !floor_path) {
        m2sdr_cli_error("--pn-floor-save needs a board or --pn-floor PATH");
        source_close(&src);
        return 1;
    }

    (void)view;
    if (report) {
        status = run_report(&src, &set, json, csv_path, &fl, floor_store ? floor_path : NULL);
    } else {
#ifdef M2SDR_ANALYZER_GUI
        status = run_gui(&src, &set, view, &fl, floor_path);
#else
        fprintf(stderr, "This build has no window (SDL2 or cimgui was missing): use --report.\n");
        status = 1;
#endif
    }
    source_close(&src);
    return status;
}
