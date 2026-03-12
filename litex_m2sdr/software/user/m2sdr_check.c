/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR File Inspection Utility (Dear ImGui + SDL2/OpenGL3).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 */

#include <getopt.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <GL/gl.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui/cimgui.h"
#include "scan_ui/imgui_sdl_gl3_bridge.h"
#include "kissfft/kiss_fft.h"
#include "m2sdr_sigmf.h"

#define igGetIO igGetIO_Nil

#define CHECK_MAX_CHANNELS 8
#define CHECK_DEFAULT_CHANNELS 2
#define CHECK_DEFAULT_NBITS 12
#define CHECK_DEFAULT_SAMPLERATE 30720000.0
#define CHECK_DEFAULT_FRAME_SIZE 245760
#define CHECK_DEFAULT_MAX_SAMPLES 1048576
#define CHECK_DEFAULT_VIEW_SAMPLES 4096
#define CHECK_DEFAULT_CONSTELLATION_POINTS 4096
#define CHECK_DEFAULT_FFT_LEN 2048
#define CHECK_HIST_BINS 128

struct channel_data {
    float *i;
    float *q;
};

struct channel_stats {
    double mean_i;
    double mean_q;
    double rms_i;
    double rms_q;
    double rms_mag;
    double dc_dbfs;
    double rms_dbfs;
    double crest_db;
    double iq_gain_mismatch_db;
    double iq_quad_error_deg;
    float min_i;
    float max_i;
    float min_q;
    float max_q;
    float peak_abs;
    size_t clip_i;
    size_t clip_q;
};

struct check_data {
    struct channel_data channels[CHECK_MAX_CHANNELS];
    struct channel_stats stats[CHECK_MAX_CHANNELS];
    size_t samples;
    int nchannels;
    int nbits;
    int sample_bytes;
    double sample_rate;
    bool frame_header;
    size_t frame_size;
    size_t headers_seen;
    size_t headers_bad_magic;
    uint64_t first_timestamp;
    uint64_t last_timestamp;
    double avg_timestamp_step_ns;
    double rms_timestamp_jitter_ns;
    bool sigmf_loaded;
    struct m2sdr_sigmf_meta sigmf_meta;
    char source_path[1024];
};

struct plot_cache {
    float *time_i;
    float *time_q;
    float *time_mag;
    float *hist_i;
    float *hist_q;
    float *fft_db;
    int capacity;
    int fft_len;
    kiss_fft_cfg fft_cfg;
    kiss_fft_cpx *fft_in;
    kiss_fft_cpx *fft_out;
};

struct ui_state {
    int selected_channel;
    int start_sample;
    int view_samples;
    int constellation_points;
    int fft_len_idx;
    bool autoscale_time;
    bool autoscale_constellation;
    bool show_i;
    bool show_q;
    bool show_mag;
};

static const int k_fft_lengths[] = {128, 256, 512, 1024, 2048, 4096, 8192, 16384};
static const char *k_fft_labels[] = {"128", "256", "512", "1024", "2048", "4096", "8192", "16384"};

static double safe_db(double num, double den)
{
    if (num <= 0.0 || den <= 0.0)
        return -200.0;
    return 20.0 * log10(num / den);
}

static void help(void)
{
    printf("M2SDR File Inspection Utility.\n"
           "usage: m2sdr_check [options] <filename>\n"
           "\n"
           "Options:\n"
           "  -h, --help              Show this help message.\n"
           "      --nchannels N       Number of interleaved RF channels (default: %d).\n"
           "      --nbits N           Effective ADC/DAC bit width for stats (default: %d).\n"
           "      --sample-rate HZ    Sample rate in Hz (default: %.0f).\n"
           "      --format FMT        Sample format: sc16 or sc8 (default: sc16).\n"
           "      --frame-header      Expect a 16-byte DMA header per frame.\n"
           "      --frame-size BYTES  Frame size in bytes including header (default: %d).\n"
           "      --max-samples N     Maximum samples to load per channel (default: %d).\n"
           "\n"
           "The GUI shows:\n"
           "  - time-domain I/Q/magnitude\n"
           "  - constellation\n"
           "  - FFT spectrum\n"
           "  - I/Q histograms\n"
           "  - DC/RMS/clipping/timestamp summary\n",
           CHECK_DEFAULT_CHANNELS,
           CHECK_DEFAULT_NBITS,
           CHECK_DEFAULT_SAMPLERATE,
           CHECK_DEFAULT_FRAME_SIZE,
           CHECK_DEFAULT_MAX_SAMPLES);
}

static void *xcalloc(size_t nmemb, size_t size)
{
    void *ptr = calloc(nmemb, size);
    if (!ptr) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return ptr;
}

static float decode_sc16_component(const uint8_t *p)
{
    int16_t v = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return (float)v;
}

static void free_check_data(struct check_data *data)
{
    int ch;

    if (!data)
        return;
    for (ch = 0; ch < CHECK_MAX_CHANNELS; ch++) {
        free(data->channels[ch].i);
        free(data->channels[ch].q);
        data->channels[ch].i = NULL;
        data->channels[ch].q = NULL;
    }
    memset(data, 0, sizeof(*data));
}

static void free_plot_cache(struct plot_cache *cache)
{
    if (!cache)
        return;
    free(cache->time_i);
    free(cache->time_q);
    free(cache->time_mag);
    free(cache->hist_i);
    free(cache->hist_q);
    free(cache->fft_db);
    kiss_fft_free(cache->fft_cfg);
    free(cache->fft_in);
    free(cache->fft_out);
    memset(cache, 0, sizeof(*cache));
}

static bool ensure_plot_capacity(struct plot_cache *cache, int view_samples)
{
    if (cache->capacity >= view_samples)
        return true;

    free(cache->time_i);
    free(cache->time_q);
    free(cache->time_mag);

    cache->time_i = (float *)xcalloc((size_t)view_samples, sizeof(float));
    cache->time_q = (float *)xcalloc((size_t)view_samples, sizeof(float));
    cache->time_mag = (float *)xcalloc((size_t)view_samples, sizeof(float));
    if (!cache->hist_i)
        cache->hist_i = (float *)xcalloc(CHECK_HIST_BINS, sizeof(float));
    if (!cache->hist_q)
        cache->hist_q = (float *)xcalloc(CHECK_HIST_BINS, sizeof(float));
    cache->capacity = view_samples;
    return true;
}

static bool ensure_fft_capacity(struct plot_cache *cache, int fft_len)
{
    size_t cfg_sz = 0;

    if (cache->fft_len == fft_len && cache->fft_cfg && cache->fft_db && cache->fft_in && cache->fft_out)
        return true;

    kiss_fft_free(cache->fft_cfg);
    free(cache->fft_db);
    free(cache->fft_in);
    free(cache->fft_out);
    cache->fft_cfg = NULL;
    cache->fft_db = NULL;
    cache->fft_in = NULL;
    cache->fft_out = NULL;
    cache->fft_len = 0;

    kiss_fft_alloc(fft_len, 0, NULL, &cfg_sz);
    cache->fft_cfg = kiss_fft_alloc(fft_len, 0, malloc(cfg_sz), &cfg_sz);
    if (!cache->fft_cfg)
        return false;

    cache->fft_db = (float *)xcalloc((size_t)fft_len, sizeof(float));
    cache->fft_in = (kiss_fft_cpx *)xcalloc((size_t)fft_len, sizeof(kiss_fft_cpx));
    cache->fft_out = (kiss_fft_cpx *)xcalloc((size_t)fft_len, sizeof(kiss_fft_cpx));
    cache->fft_len = fft_len;
    return true;
}

static bool load_file(const char *filename,
                      int nchannels,
                      int nbits,
                      int sample_bytes,
                      double sample_rate,
                      bool frame_header,
                      size_t frame_size,
                      size_t max_samples,
                      struct check_data *out)
{
    FILE *f = NULL;
    size_t sample_idx = 0;
    size_t headers_seen = 0;
    size_t headers_bad = 0;
    uint64_t first_ts = 0;
    uint64_t last_ts = 0;
    double accum_dt = 0.0;
    double accum_dt2 = 0.0;
    uint64_t prev_ts = 0;
    bool first_header = true;
    size_t bytes_per_sample_set = (size_t)nchannels * (size_t)sample_bytes * 2u;
    size_t frame_payload_bytes = frame_header ? frame_size - 16u : 0;
    size_t frame_payload_used = frame_payload_bytes;
    int ch;

    if (nchannels <= 0 || nchannels > CHECK_MAX_CHANNELS) {
        fprintf(stderr, "Invalid channel count: %d\n", nchannels);
        return false;
    }
    if (sample_bytes != 1 && sample_bytes != 2) {
        fprintf(stderr, "Unsupported sample byte width: %d\n", sample_bytes);
        return false;
    }
    if (frame_header && frame_size <= 16) {
        fprintf(stderr, "Frame size must be > 16 when --frame-header is used\n");
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->nchannels = nchannels;
    out->nbits = nbits;
    out->sample_bytes = sample_bytes;
    out->sample_rate = sample_rate;
    out->frame_header = frame_header;
    out->frame_size = frame_size;
    snprintf(out->source_path, sizeof(out->source_path), "%s", filename);

    for (ch = 0; ch < nchannels; ch++) {
        out->channels[ch].i = (float *)xcalloc(max_samples ? max_samples : 1, sizeof(float));
        out->channels[ch].q = (float *)xcalloc(max_samples ? max_samples : 1, sizeof(float));
    }

    f = fopen(filename, "rb");
    if (!f) {
        perror(filename);
        free_check_data(out);
        return false;
    }

    while (max_samples == 0 || sample_idx < max_samples) {
        bool done = false;

        if (frame_header && frame_payload_used >= frame_payload_bytes) {
            uint8_t header[16];
            uint64_t magic;
            uint64_t ts;

            if (fread(header, 1, sizeof(header), f) != sizeof(header))
                break;

            magic = ((uint64_t)header[0] << 0) | ((uint64_t)header[1] << 8) |
                    ((uint64_t)header[2] << 16) | ((uint64_t)header[3] << 24) |
                    ((uint64_t)header[4] << 32) | ((uint64_t)header[5] << 40) |
                    ((uint64_t)header[6] << 48) | ((uint64_t)header[7] << 56);
            ts = ((uint64_t)header[8] << 0) | ((uint64_t)header[9] << 8) |
                 ((uint64_t)header[10] << 16) | ((uint64_t)header[11] << 24) |
                 ((uint64_t)header[12] << 32) | ((uint64_t)header[13] << 40) |
                 ((uint64_t)header[14] << 48) | ((uint64_t)header[15] << 56);

            headers_seen++;
            if (magic != 0x5aa55aa55aa55aa5ULL)
                headers_bad++;

            if (first_header) {
                first_ts = ts;
                first_header = false;
            } else {
                double dt = (double)(ts - prev_ts);
                accum_dt += dt;
                accum_dt2 += dt * dt;
            }
            prev_ts = ts;
            last_ts = ts;
            frame_payload_used = 0;
        }

        for (ch = 0; ch < nchannels; ch++) {
            uint8_t raw[4];

            if (fread(raw, 1, (size_t)sample_bytes * 2u, f) != (size_t)sample_bytes * 2u) {
                done = true;
                break;
            }

            if (sample_bytes == 2) {
                out->channels[ch].i[sample_idx] = decode_sc16_component(&raw[0]);
                out->channels[ch].q[sample_idx] = decode_sc16_component(&raw[2]);
            } else {
                out->channels[ch].i[sample_idx] = (float)(int8_t)raw[0];
                out->channels[ch].q[sample_idx] = (float)(int8_t)raw[1];
            }
        }

        if (done)
            break;

        sample_idx++;
        if (frame_header)
            frame_payload_used += bytes_per_sample_set;
    }

    fclose(f);
    out->samples = sample_idx;
    out->headers_seen = headers_seen;
    out->headers_bad_magic = headers_bad;
    out->first_timestamp = first_ts;
    out->last_timestamp = last_ts;
    if (headers_seen > 1)
        out->avg_timestamp_step_ns = accum_dt / (double)(headers_seen - 1);
    if (headers_seen > 2) {
        double mean = out->avg_timestamp_step_ns;
        double mean2 = accum_dt2 / (double)(headers_seen - 1);
        double var = mean2 - mean * mean;
        out->rms_timestamp_jitter_ns = sqrt(var > 0.0 ? var : 0.0);
    }

    if (sample_idx == 0) {
        fprintf(stderr, "No samples loaded from %s\n", filename);
        free_check_data(out);
        return false;
    }

    return true;
}

static float full_scale_value(const struct check_data *data)
{
    if (data->sample_bytes == 1)
        return 127.0f;
    if (data->nbits >= 2 && data->nbits < 16)
        return (float)((1u << (data->nbits - 1)) - 1u);
    return 32767.0f;
}

static void compute_stats(struct check_data *data)
{
    int ch;
    float clip_level = full_scale_value(data);
    double fs = clip_level > 0.0f ? (double)clip_level : 1.0;

    for (ch = 0; ch < data->nchannels; ch++) {
        struct channel_stats *st = &data->stats[ch];
        const float *pi = data->channels[ch].i;
        const float *pq = data->channels[ch].q;
        double sum_i = 0.0;
        double sum_q = 0.0;
        double sum_i2 = 0.0;
        double sum_q2 = 0.0;
        double sum_mag2 = 0.0;
        double sum_iq = 0.0;
        size_t n;

        memset(st, 0, sizeof(*st));
        st->min_i = st->max_i = pi[0];
        st->min_q = st->max_q = pq[0];

        for (n = 0; n < data->samples; n++) {
            float vi = pi[n];
            float vq = pq[n];
            float abs_i = fabsf(vi);
            float abs_q = fabsf(vq);
            float peak = abs_i > abs_q ? abs_i : abs_q;

            sum_i += vi;
            sum_q += vq;
            sum_i2 += (double)vi * (double)vi;
            sum_q2 += (double)vq * (double)vq;
            sum_mag2 += (double)vi * (double)vi + (double)vq * (double)vq;
            sum_iq += (double)vi * (double)vq;

            if (vi < st->min_i) st->min_i = vi;
            if (vi > st->max_i) st->max_i = vi;
            if (vq < st->min_q) st->min_q = vq;
            if (vq > st->max_q) st->max_q = vq;
            if (peak > st->peak_abs) st->peak_abs = peak;
            if (abs_i >= clip_level) st->clip_i++;
            if (abs_q >= clip_level) st->clip_q++;
        }

        st->mean_i = sum_i / (double)data->samples;
        st->mean_q = sum_q / (double)data->samples;
        st->rms_i = sqrt(sum_i2 / (double)data->samples);
        st->rms_q = sqrt(sum_q2 / (double)data->samples);
        st->rms_mag = sqrt(sum_mag2 / (double)data->samples);
        st->dc_dbfs = safe_db(hypot(st->mean_i, st->mean_q), fs);
        st->rms_dbfs = safe_db(st->rms_mag, fs);
        st->crest_db = safe_db(st->peak_abs, st->rms_mag);
        st->iq_gain_mismatch_db = safe_db(st->rms_i, st->rms_q);
        if (st->rms_i > 0.0 && st->rms_q > 0.0) {
            double corr = sum_iq / ((double)data->samples * st->rms_i * st->rms_q);
            if (corr > 1.0) corr = 1.0;
            if (corr < -1.0) corr = -1.0;
            st->iq_quad_error_deg = asin(corr) * (180.0 / M_PI);
        }
    }
}

static void draw_metric_card(const char *id, const char *title, const char *value, ImU32 accent)
{
    ImVec2 pmin, pmax, avail;
    ImDrawList *dl;

    igGetContentRegionAvail(&avail);
    if (avail.x < 40.0f)
        avail.x = 40.0f;
    avail.y = 68.0f;
    igInvisibleButton(id, avail, 0);
    igGetItemRectMin(&pmin);
    igGetItemRectMax(&pmax);
    dl = igGetWindowDrawList();

    ImDrawList_AddRectFilled(dl, pmin, pmax, 0xFF171A1Eu, 8.0f, 0);
    ImDrawList_AddRectFilled(dl, pmin, (ImVec2){pmin.x + 5.0f, pmax.y}, accent, 8.0f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2){pmin.x + 14.0f, pmin.y + 12.0f}, 0xA0FFFFFFu, title, NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){pmin.x + 14.0f, pmin.y + 34.0f}, 0xFFFFFFFFu, value, NULL);
}

static void draw_overview_panel(const struct check_data *data, const struct channel_stats *st, int start_sample, int view_samples)
{
    char buf[128];
    double dur_ms = ((double)data->samples / data->sample_rate) * 1e3;
    double view_us = ((double)view_samples / data->sample_rate) * 1e6;

    if (!igBeginChild_Str("##overview_panel", (ImVec2){0.0f, 86.0f}, 0, 0)) {
        igEndChild();
        return;
    }

    igColumns(4, "##overview_cols", false);
    snprintf(buf, sizeof(buf), "%zu samples", data->samples);
    draw_metric_card("##card_samples", "Capture", buf, 0xFF36C6D3u);
    igNextColumn();

    snprintf(buf, sizeof(buf), "%.3f ms", dur_ms);
    draw_metric_card("##card_duration", "Duration", buf, 0xFF5BC27Eu);
    igNextColumn();

    snprintf(buf, sizeof(buf), "%.2f dBFS", st->rms_dbfs);
    draw_metric_card("##card_level", "RMS Level", buf, 0xFFE0B84Eu);
    igNextColumn();

    snprintf(buf, sizeof(buf), "start %d  %.2f us", start_sample, view_us);
    draw_metric_card("##card_window", "View Window", buf, 0xFFB86ADEu);
    igColumns(1, NULL, false);
    igEndChild();
}

static void fill_time_plots(const struct check_data *data,
                            struct plot_cache *cache,
                            int channel,
                            int start,
                            int count)
{
    int i;
    const float *pi = data->channels[channel].i;
    const float *pq = data->channels[channel].q;

    ensure_plot_capacity(cache, count);
    for (i = 0; i < count; i++) {
        float vi = pi[start + i];
        float vq = pq[start + i];
        cache->time_i[i] = vi;
        cache->time_q[i] = vq;
        cache->time_mag[i] = sqrtf(vi * vi + vq * vq);
    }
}

static void fill_histograms(const struct check_data *data,
                            struct plot_cache *cache,
                            int channel,
                            int start,
                            int count)
{
    float fs = full_scale_value(data);
    int i;

    memset(cache->hist_i, 0, CHECK_HIST_BINS * sizeof(float));
    memset(cache->hist_q, 0, CHECK_HIST_BINS * sizeof(float));

    for (i = 0; i < count; i++) {
        float vi = data->channels[channel].i[start + i];
        float vq = data->channels[channel].q[start + i];
        int bin_i = (int)(((vi + fs) / (2.0f * fs)) * (float)(CHECK_HIST_BINS - 1));
        int bin_q = (int)(((vq + fs) / (2.0f * fs)) * (float)(CHECK_HIST_BINS - 1));

        if (bin_i < 0) bin_i = 0;
        if (bin_i >= CHECK_HIST_BINS) bin_i = CHECK_HIST_BINS - 1;
        if (bin_q < 0) bin_q = 0;
        if (bin_q >= CHECK_HIST_BINS) bin_q = CHECK_HIST_BINS - 1;

        cache->hist_i[bin_i] += 1.0f;
        cache->hist_q[bin_q] += 1.0f;
    }
}

static void fill_fft_plot(const struct check_data *data,
                          struct plot_cache *cache,
                          int channel,
                          int start,
                          int fft_len)
{
    int i;
    int half = fft_len / 2;
    float window_sum = 0.0f;
    float fs = full_scale_value(data);
    const float *pi = data->channels[channel].i;
    const float *pq = data->channels[channel].q;

    ensure_fft_capacity(cache, fft_len);

    for (i = 0; i < fft_len; i++) {
        float w = 0.5f - 0.5f * cosf((2.0f * (float)M_PI * (float)i) / (float)(fft_len - 1));
        int idx = start + i;
        float vi = 0.0f;
        float vq = 0.0f;

        if (idx < (int)data->samples) {
            vi = pi[idx];
            vq = pq[idx];
        }
        cache->fft_in[i].r = vi * w;
        cache->fft_in[i].i = vq * w;
        window_sum += w;
    }

    kiss_fft(cache->fft_cfg, cache->fft_in, cache->fft_out);

    for (i = 0; i < fft_len; i++) {
        int src = (i + half) % fft_len;
        float re = cache->fft_out[src].r;
        float im = cache->fft_out[src].i;
        float mag = sqrtf(re * re + im * im);
        float norm = mag / ((window_sum > 0.0f ? window_sum : 1.0f) * fs);
        cache->fft_db[i] = 20.0f * log10f(norm + 1e-12f);
    }
}

static void draw_constellation(const struct check_data *data,
                               const struct plot_cache *cache,
                               const struct ui_state *ui,
                               int start,
                               int count)
{
    ImVec2 avail;
    ImVec2 pmin, pmax;
    ImDrawList *dl;
    float fs = full_scale_value(data);
    float scale = fs;
    float xmid, ymid, radius;
    int step;
    int i;

    igGetContentRegionAvail(&avail);
    if (avail.y > 260.0f)
        avail.y = 260.0f;
    if (avail.y < 180.0f)
        avail.y = 180.0f;

    igInvisibleButton("##constellation", avail, 0);
    igGetItemRectMin(&pmin);
    igGetItemRectMax(&pmax);
    dl = igGetWindowDrawList();

    ImDrawList_AddRectFilled(dl, pmin, pmax, 0xFF121417u, 6.0f, 0);
    xmid = 0.5f * (pmin.x + pmax.x);
    ymid = 0.5f * (pmin.y + pmax.y);
    radius = fminf((pmax.x - pmin.x), (pmax.y - pmin.y)) * 0.42f;

    if (ui->autoscale_constellation) {
        scale = 1.0f;
        for (i = 0; i < count; i++) {
            float vi = cache->time_i[i];
            float vq = cache->time_q[i];
            float peak = fmaxf(fabsf(vi), fabsf(vq));
            if (peak > scale)
                scale = peak;
        }
        scale *= 1.1f;
    }

    ImDrawList_AddLine(dl, (ImVec2){pmin.x + 8.0f, ymid}, (ImVec2){pmax.x - 8.0f, ymid}, 0x55FFFFFFu, 1.0f);
    ImDrawList_AddLine(dl, (ImVec2){xmid, pmin.y + 8.0f}, (ImVec2){xmid, pmax.y - 8.0f}, 0x55FFFFFFu, 1.0f);
    ImDrawList_AddRectFilled(dl,
                             (ImVec2){xmid - radius, ymid - radius},
                             (ImVec2){xmid + radius, ymid + radius},
                             0x0914A3FFu, 0.0f, 0);
    step = count > ui->constellation_points ? (count / ui->constellation_points) : 1;
    for (i = 0; i < count; i += step) {
        float x = xmid + (cache->time_i[i] / scale) * radius;
        float y = ymid - (cache->time_q[i] / scale) * radius;
        if (x < pmin.x + 2.0f || x > pmax.x - 2.0f || y < pmin.y + 2.0f || y > pmax.y - 2.0f)
            continue;
        ImDrawList_AddCircleFilled(dl, (ImVec2){x, y}, 1.6f, 0xC0FFD060u, 12);
    }
    ImDrawList_AddText_Vec2(dl, (ImVec2){pmin.x + 8.0f, pmin.y + 6.0f}, 0xFFFFFFFFu, "Constellation", NULL);
    {
        char txt[96];
        snprintf(txt, sizeof(txt), "sample %d..%d  scale %.1f", start, start + count - 1, scale);
        ImDrawList_AddText_Vec2(dl, (ImVec2){pmin.x + 8.0f, pmax.y - 18.0f}, 0xB0FFFFFFu, txt, NULL);
    }
}

static void draw_controls(const struct check_data *data, struct ui_state *ui)
{
    int max_start = 0;

    if (data->samples > (size_t)ui->view_samples)
        max_start = (int)data->samples - ui->view_samples;
    if (ui->start_sample > max_start)
        ui->start_sample = max_start;
    if (ui->start_sample < 0)
        ui->start_sample = 0;

    igText("Source");
    igTextWrapped("%s", data->source_path);
    igText("Samples/channel: %zu", data->samples);
    igText("Rate: %.3f MSPS", data->sample_rate / 1e6);
    igText("Keyboard: Left/Right pan, Home/End jump");
    igSeparator();

    if (data->nchannels > 1) {
        const char *labels[CHECK_MAX_CHANNELS];
        int ch;
        for (ch = 0; ch < data->nchannels; ch++) {
            static char storage[CHECK_MAX_CHANNELS][16];
            snprintf(storage[ch], sizeof(storage[ch]), "CH%d", ch);
            labels[ch] = storage[ch];
        }
        igCombo_Str_arr("Channel", &ui->selected_channel, labels, data->nchannels, data->nchannels);
    } else {
        ui->selected_channel = 0;
    }

    igSliderInt("Start", &ui->start_sample, 0, max_start > 0 ? max_start : 0, "%d", 0);
    igSliderInt("View", &ui->view_samples, 256, (int)((data->samples < 65536) ? data->samples : 65536), "%d", 0);
    igSliderInt("Const Pts", &ui->constellation_points, 256, 16384, "%d", 0);
    igCombo_Str_arr("FFT", &ui->fft_len_idx, k_fft_labels, (int)(sizeof(k_fft_lengths) / sizeof(k_fft_lengths[0])), 8);
    igCheckbox("Autoscale time", &ui->autoscale_time);
    igSameLine(0.0f, -1.0f);
    igCheckbox("Autoscale constellation", &ui->autoscale_constellation);
    igCheckbox("Show I", &ui->show_i);
    igSameLine(0.0f, -1.0f);
    igCheckbox("Show Q", &ui->show_q);
    igSameLine(0.0f, -1.0f);
    igCheckbox("Show |IQ|", &ui->show_mag);
}

static void draw_stats_panel(const struct check_data *data, int channel)
{
    const struct channel_stats *st = &data->stats[channel];

    igText("Channel %d stats", channel);
    igSeparator();
    igText("Mean I / Q        : %8.3f / %8.3f", st->mean_i, st->mean_q);
    igText("RMS I / Q         : %8.3f / %8.3f", st->rms_i, st->rms_q);
    igText("RMS |IQ|          : %8.3f", st->rms_mag);
    igText("RMS dBFS          : %8.2f dBFS", st->rms_dbfs);
    igText("DC dBFS           : %8.2f dBFS", st->dc_dbfs);
    igText("Crest factor      : %8.2f dB", st->crest_db);
    igText("Min/Max I         : %8.1f / %8.1f", st->min_i, st->max_i);
    igText("Min/Max Q         : %8.1f / %8.1f", st->min_q, st->max_q);
    igText("Peak abs          : %8.1f", st->peak_abs);
    igText("IQ gain mismatch  : %8.3f dB", st->iq_gain_mismatch_db);
    igText("Quadrature error  : %8.3f deg", st->iq_quad_error_deg);
    igText("Clip I / Q        : %8.4f%% / %8.4f%%",
           100.0 * (double)st->clip_i / (double)data->samples,
           100.0 * (double)st->clip_q / (double)data->samples);

    igSeparator();
    igText("Format            : %s (%d-bit effective)", data->sample_bytes == 1 ? "sc8" : "sc16", data->nbits);
    if (data->sigmf_loaded) {
        igText("SigMF datatype    : %s", data->sigmf_meta.datatype);
        if (data->sigmf_meta.has_center_freq)
            igText("SigMF center freq : %.6f MHz", data->sigmf_meta.center_freq / 1e6);
        if (data->sigmf_meta.description[0])
            igTextWrapped("SigMF description : %s", data->sigmf_meta.description);
        if (data->sigmf_meta.author[0])
            igText("SigMF author      : %s", data->sigmf_meta.author);
        if (data->sigmf_meta.hw[0])
            igText("SigMF hw          : %s", data->sigmf_meta.hw);
    }
    if (data->frame_header) {
        igText("Headers seen      : %zu", data->headers_seen);
        igText("Bad header magic  : %zu", data->headers_bad_magic);
        if (data->headers_seen > 0) {
            igText("First timestamp   : %" PRIu64, data->first_timestamp);
            igText("Last timestamp    : %" PRIu64, data->last_timestamp);
        }
        if (data->headers_seen > 1)
            igText("Avg ts step       : %.3f us", data->avg_timestamp_step_ns / 1000.0);
        if (data->headers_seen > 2)
            igText("RMS ts jitter     : %.3f ns", data->rms_timestamp_jitter_ns);
    }
}

static void draw_time_domain(const struct ui_state *ui, const struct plot_cache *cache, int count)
{
    float min_v = -1.0f;
    float max_v = 1.0f;
    int i;

    if (ui->autoscale_time && count > 0) {
        min_v = max_v = cache->time_i[0];
        for (i = 0; i < count; i++) {
            if (cache->time_i[i] < min_v) min_v = cache->time_i[i];
            if (cache->time_i[i] > max_v) max_v = cache->time_i[i];
            if (cache->time_q[i] < min_v) min_v = cache->time_q[i];
            if (cache->time_q[i] > max_v) max_v = cache->time_q[i];
            if (cache->time_mag[i] < min_v) min_v = cache->time_mag[i];
            if (cache->time_mag[i] > max_v) max_v = cache->time_mag[i];
        }
        if (fabsf(max_v - min_v) < 1e-6f) {
            min_v -= 1.0f;
            max_v += 1.0f;
        }
    }

    igText("Time-domain");
    if (ui->show_i)
        igPlotLines_FloatPtr("I", cache->time_i, count, 0, NULL, min_v, max_v, (ImVec2){0.0f, 110.0f}, sizeof(float));
    if (ui->show_q)
        igPlotLines_FloatPtr("Q", cache->time_q, count, 0, NULL, min_v, max_v, (ImVec2){0.0f, 110.0f}, sizeof(float));
    if (ui->show_mag)
        igPlotLines_FloatPtr("|IQ|", cache->time_mag, count, 0, NULL, min_v, max_v, (ImVec2){0.0f, 110.0f}, sizeof(float));
}

static void draw_histograms(const struct plot_cache *cache)
{
    igText("I/Q histograms");
    igPlotHistogram_FloatPtr("I hist", cache->hist_i, CHECK_HIST_BINS, 0, NULL, 0.0f, FLT_MAX, (ImVec2){0.0f, 100.0f}, sizeof(float));
    igPlotHistogram_FloatPtr("Q hist", cache->hist_q, CHECK_HIST_BINS, 0, NULL, 0.0f, FLT_MAX, (ImVec2){0.0f, 100.0f}, sizeof(float));
}

static void draw_spectrum(const struct check_data *data, const struct plot_cache *cache, int fft_len)
{
    int i;
    float peak_db = cache->fft_db[0];
    int peak_bin = 0;
    double peak_hz;

    for (i = 1; i < fft_len; i++) {
        if (cache->fft_db[i] > peak_db) {
            peak_db = cache->fft_db[i];
            peak_bin = i;
        }
    }
    peak_hz = ((double)peak_bin - (double)(fft_len / 2)) * data->sample_rate / (double)fft_len;

    igText("Spectrum");
    igPlotLines_FloatPtr("FFT (dBFS)", cache->fft_db, fft_len, 0, NULL, -140.0f, 10.0f, (ImVec2){0.0f, 140.0f}, sizeof(float));
    igText("Peak bin: %d, peak freq: %.3f MHz, peak level: %.2f dBFS, RBW: %.1f kHz",
           peak_bin, peak_hz / 1e6, peak_db, (data->sample_rate / (double)fft_len) / 1e3);
}

int main(int argc, char **argv)
{
    const char *filename = NULL;
    char resolved_filename[1024] = {0};
    struct check_data data;
    struct plot_cache cache;
    struct ui_state ui;
    struct m2sdr_sigmf_meta sigmf_meta;
    SDL_Window *window = NULL;
    SDL_GLContext gl_context = NULL;
    const char *glsl_version = "#version 130";
    int nchannels = CHECK_DEFAULT_CHANNELS;
    int nbits = CHECK_DEFAULT_NBITS;
    double sample_rate = CHECK_DEFAULT_SAMPLERATE;
    int sample_bytes = 2;
    bool frame_header = false;
    size_t frame_size = CHECK_DEFAULT_FRAME_SIZE;
    size_t max_samples = CHECK_DEFAULT_MAX_SAMPLES;
    bool quit = false;
    int option_index = 0;
    int c;
    static struct option options[] = {
        {"help", no_argument, NULL, 'h'},
        {"nchannels", required_argument, NULL, 1},
        {"nbits", required_argument, NULL, 2},
        {"sample-rate", required_argument, NULL, 3},
        {"format", required_argument, NULL, 4},
        {"frame-header", no_argument, NULL, 5},
        {"frame-size", required_argument, NULL, 6},
        {"max-samples", required_argument, NULL, 7},
        {NULL, 0, NULL, 0}
    };

    memset(&data, 0, sizeof(data));
    memset(&cache, 0, sizeof(cache));
    memset(&sigmf_meta, 0, sizeof(sigmf_meta));
    ui.selected_channel = 0;
    ui.start_sample = 0;
    ui.view_samples = CHECK_DEFAULT_VIEW_SAMPLES;
    ui.constellation_points = CHECK_DEFAULT_CONSTELLATION_POINTS;
    ui.fft_len_idx = 4;
    ui.autoscale_time = true;
    ui.autoscale_constellation = true;
    ui.show_i = true;
    ui.show_q = true;
    ui.show_mag = false;

    for (;;) {
        c = getopt_long(argc, argv, "h", options, &option_index);
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            help();
            return 0;
        case 1:
            nchannels = atoi(optarg);
            break;
        case 2:
            nbits = atoi(optarg);
            break;
        case 3:
            sample_rate = atof(optarg);
            break;
        case 4:
            if (strcmp(optarg, "sc16") == 0)
                sample_bytes = 2;
            else if (strcmp(optarg, "sc8") == 0)
                sample_bytes = 1;
            else {
                fprintf(stderr, "Unsupported format: %s\n", optarg);
                return 1;
            }
            break;
        case 5:
            frame_header = true;
            break;
        case 6:
            frame_size = (size_t)strtoull(optarg, NULL, 0);
            break;
        case 7:
            max_samples = (size_t)strtoull(optarg, NULL, 0);
            break;
        default:
            return 1;
        }
    }

    if (optind >= argc) {
        help();
        return 1;
    }
    filename = argv[optind];

    if (m2sdr_sigmf_read(filename, &sigmf_meta) == 0) {
        enum m2sdr_format sigmf_format = m2sdr_sigmf_format_from_datatype(sigmf_meta.datatype);

        if (sigmf_format == M2SDR_FORMAT_SC16_Q11)
            sample_bytes = 2;
        else if (sigmf_format == M2SDR_FORMAT_SC8_Q7)
            sample_bytes = 1;
        else {
            fprintf(stderr, "Unsupported SigMF datatype: %s\n", sigmf_meta.datatype);
            return 1;
        }

        if (sigmf_meta.has_sample_rate)
            sample_rate = sigmf_meta.sample_rate;
        if (sigmf_meta.has_num_channels)
            nchannels = (int)sigmf_meta.num_channels;
        if (sigmf_meta.has_header_bytes) {
            if (sigmf_meta.header_bytes == 16) {
                frame_header = true;
            } else {
                fprintf(stderr, "Unsupported SigMF header_bytes=%u in m2sdr_check\n", sigmf_meta.header_bytes);
                return 1;
            }
        }

        snprintf(resolved_filename, sizeof(resolved_filename), "%s", sigmf_meta.data_path);
        filename = resolved_filename;
        data.sigmf_loaded = true;
        memcpy(&data.sigmf_meta, &sigmf_meta, sizeof(sigmf_meta));
    }

    if (!load_file(filename, nchannels, nbits, sample_bytes, sample_rate, frame_header, frame_size, max_samples, &data))
        return 1;

    compute_stats(&data);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        free_check_data(&data);
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    window = SDL_CreateWindow("M2SDR Check - RX stream inspector",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1520, 960, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        free_check_data(&data);
        SDL_Quit();
        return 1;
    }

    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        free_check_data(&data);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    igCreateContext(NULL);
    igStyleColorsDark(NULL);
    {
        ImGuiIO *io = igGetIO();
        io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    }

    if (!m2sdr_imgui_sdl2_init_for_opengl(window, gl_context) ||
        !m2sdr_imgui_opengl3_init(glsl_version)) {
        fprintf(stderr, "ImGui backend init failed\n");
        igDestroyContext(NULL);
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        free_check_data(&data);
        SDL_Quit();
        return 1;
    }

    while (!quit) {
        SDL_Event e;
        int count;
        int fft_len;

        while (SDL_PollEvent(&e)) {
            m2sdr_imgui_sdl2_process_event(&e);
            if (e.type == SDL_QUIT)
                quit = true;
            if (e.type == SDL_WINDOWEVENT &&
                e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window))
                quit = true;
            if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
                if (e.key.keysym.sym == SDLK_LEFT)
                    ui.start_sample -= ui.view_samples / 8;
                else if (e.key.keysym.sym == SDLK_RIGHT)
                    ui.start_sample += ui.view_samples / 8;
                else if (e.key.keysym.sym == SDLK_HOME)
                    ui.start_sample = 0;
                else if (e.key.keysym.sym == SDLK_END)
                    ui.start_sample = (int)data.samples - ui.view_samples;
            }
        }

        if (ui.view_samples < 16)
            ui.view_samples = 16;
        if ((size_t)ui.view_samples > data.samples)
            ui.view_samples = (int)data.samples;
        if (ui.start_sample < 0)
            ui.start_sample = 0;
        if ((size_t)(ui.start_sample + ui.view_samples) > data.samples)
            ui.start_sample = (int)data.samples - ui.view_samples;
        if (ui.start_sample < 0)
            ui.start_sample = 0;

        count = ui.view_samples;
        fft_len = k_fft_lengths[ui.fft_len_idx];
        if (fft_len > count)
            fft_len = count;
        if (fft_len < 128)
            fft_len = 128;

        fill_time_plots(&data, &cache, ui.selected_channel, ui.start_sample, count);
        fill_histograms(&data, &cache, ui.selected_channel, ui.start_sample, count);
        fill_fft_plot(&data, &cache, ui.selected_channel, ui.start_sample, fft_len);

        m2sdr_imgui_opengl3_new_frame();
        m2sdr_imgui_sdl2_new_frame();
        igNewFrame();

        {
            ImGuiIO *io = igGetIO();
            igSetNextWindowPos((ImVec2){0, 0}, ImGuiCond_Always, (ImVec2){0, 0});
            igSetNextWindowSize(io->DisplaySize, ImGuiCond_Always);
        }

        igBegin("##M2SDRCheckMain", NULL,
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoTitleBar);
        {
            draw_overview_panel(&data, &data.stats[ui.selected_channel], ui.start_sample, count);
            if (igBeginChild_Str("##left", (ImVec2){340.0f, 0.0f}, 0, 0)) {
                draw_controls(&data, &ui);
                igSeparator();
                draw_stats_panel(&data, ui.selected_channel);
            }
            igEndChild();

            igSameLine(0.0f, -1.0f);

            if (igBeginChild_Str("##right", (ImVec2){0.0f, 0.0f}, 0, 0)) {
                draw_time_domain(&ui, &cache, count);
                draw_constellation(&data, &cache, &ui, ui.start_sample, count);
                draw_spectrum(&data, &cache, fft_len);
                draw_histograms(&cache);
            }
            igEndChild();
        }
        igEnd();

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

    m2sdr_imgui_opengl3_shutdown();
    m2sdr_imgui_sdl2_shutdown();
    igDestroyContext(NULL);
    free_plot_cache(&cache);
    free_check_data(&data);
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
