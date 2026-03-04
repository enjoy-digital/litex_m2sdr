/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR RF Spectrum Scan Utility (Dear ImGui + SDL2/OpenGL3).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <GL/gl.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui/cimgui.h"
#include "imgui_sdl_gl3_bridge.h"

#include "ad9361/platform.h"
#include "ad9361/ad9361.h"
#include "ad9361/ad9361_api.h"

#include "liblitepcie.h"
#include "libm2sdr.h"
#include "m2sdr_config.h"
#include "m2sdr_colormaps_gqrx.h"
#include "simple_fft.h"

#define igGetIO igGetIO_Nil

#define DEFAULT_SCAN_SAMPLERATE_HZ 61440000U
#define DEFAULT_SCAN_BANDWIDTH_HZ  56000000U
#define SCAN_TX_GAIN_DB    -30
#define DEFAULT_START_FREQ_HZ 2300000000LL
#define DEFAULT_STOP_FREQ_HZ  2500000000LL
#define DEFAULT_RX_GAIN_DB    35
#define DEFAULT_FFT_LEN       1024
#define DEFAULT_LINES         300
#define DEFAULT_DB_MIN        -120.0f
#define DEFAULT_DB_MAX        10.0f

#define DEFAULT_RX_SETTLE_US 2000
#define MAX_WATERFALL_WIDTH 262144
#define MAX_PLOT_POINTS 4096

static const uint32_t k_scan_samplerates_hz[] = {
    61440000U,
    30720000U,
    15360000U,
    7680000U
};

static const char *k_scan_samplerate_labels[] = {
    "61.44 MSPS",
    "30.72 MSPS",
    "15.36 MSPS",
    "7.68 MSPS"
};

static const int k_fft_lengths[] = { 128, 256, 512, 1024, 2048 };
static const char *k_fft_length_labels[] = { "128", "256", "512", "1024", "2048" };

static volatile sig_atomic_t g_keep_running = 1;
static struct ad9361_rf_phy *ad9361_phy;

static char m2sdr_device[1024];
static int m2sdr_device_num = 0;
static void *g_conn;

static void int_handler(int dummy)
{
    (void)dummy;
    g_keep_running = 0;
}

static void *m2sdr_open(void)
{
    if (g_conn)
        return g_conn;

    int fd = open(m2sdr_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not open %s: %s\n", m2sdr_device, strerror(errno));
        exit(1);
    }

    g_conn = (void *)(intptr_t)fd;
    return g_conn;
}

static void m2sdr_close_global(void)
{
    if (g_conn)
        close((int)(intptr_t)g_conn);
    g_conn = NULL;
}

int spi_write_then_read(struct spi_device *spi,
    const unsigned char *txbuf, unsigned n_tx,
    unsigned char *rxbuf, unsigned n_rx)
{
    (void)spi;
    void *conn = m2sdr_open();

    if (n_tx == 2 && n_rx == 1) {
        rxbuf[0] = m2sdr_ad9361_spi_read(conn, txbuf[0] << 8 | txbuf[1]);
    } else if (n_tx == 3 && n_rx == 0) {
        m2sdr_ad9361_spi_write(conn, txbuf[0] << 8 | txbuf[1], txbuf[2]);
    } else {
        fprintf(stderr, "Unsupported SPI transfer n_tx=%u n_rx=%u\n", n_tx, n_rx);
        return -1;
    }

    return 0;
}

void udelay(unsigned long usecs)
{
    usleep(usecs);
}

void mdelay(unsigned long msecs)
{
    usleep(msecs * 1000);
}

unsigned long msleep_interruptible(unsigned int msecs)
{
    usleep(msecs * 1000);
    return 0;
}

#define AD9361_GPIO_RESET_PIN 0

bool gpio_is_valid(int number)
{
    return number == AD9361_GPIO_RESET_PIN;
}

void gpio_set_value(unsigned gpio, int value)
{
    (void)gpio;
    (void)value;
}

struct scan_state {
    uint32_t sample_rate_hz;
    uint32_t rf_bandwidth_hz;
    int fft_len;
    int lines;
    int rx_gain;
    int64_t refclk_hz;
    int64_t scan_start_hz;
    int64_t scan_stop_hz;
    float db_min;
    float db_max;
    int rx_settle_us;
    int stitch_mode;
    bool run;
    int display_rows;
    int waterfall_palette;

    int segments;
    int stride_bins;
    int overlap_bins;
    double step_hz;
    int waterfall_width;
    int waterfall_tex_width;
    int gl_max_texture_size;

    struct litepcie_dma_ctrl dma;
    struct simple_fft_plan fft;

    float *in_re;
    float *in_im;
    float *out_re;
    float *out_im;
    float *window;
    float *line_db;
    float *line_pow_accum;
    float *line_w_accum;
    float *plot_db;
    ImVec2 *plot_points;
    uint32_t *waterfall_rgba;

    GLuint waterfall_tex;

    struct {
        double last_line_ms, ema_line_ms;
        double last_tune_ms, ema_tune_ms;
        double last_capture_ms, ema_capture_ms;
        double last_fft_ms, ema_fft_ms;
        double last_waterfall_ms, ema_waterfall_ms;
        double lines_per_sec, captures_per_sec, retunes_per_sec;
        double iq_msps, sweep_mhz_per_sec;
        uint64_t lines_total, captures_total, retunes_total;
        uint64_t lines_at_prev_rate, captures_at_prev_rate, retunes_at_prev_rate;
        double prev_rate_t;
    } perf;
};

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double ema_update(double prev, double sample, double alpha)
{
    if (prev <= 0.0)
        return sample;
    return (1.0 - alpha) * prev + alpha * sample;
}

static uint32_t scan_bandwidth_from_samplerate(uint32_t sample_rate_hz)
{
    uint64_t bw = (uint64_t)sample_rate_hz * (uint64_t)DEFAULT_SCAN_BANDWIDTH_HZ;
    bw /= (uint64_t)DEFAULT_SCAN_SAMPLERATE_HZ;
    if (bw < 2000000ULL)
        bw = 2000000ULL;
    if (bw > sample_rate_hz)
        bw = sample_rate_hz;
    return (uint32_t)bw;
}

static int samplerate_index_from_hz(uint32_t sample_rate_hz)
{
    int i;
    for (i = 0; i < (int)(sizeof(k_scan_samplerates_hz) / sizeof(k_scan_samplerates_hz[0])); i++) {
        if (k_scan_samplerates_hz[i] == sample_rate_hz)
            return i;
    }
    return 0;
}

static bool is_supported_samplerate(uint32_t sample_rate_hz)
{
    int i;
    for (i = 0; i < (int)(sizeof(k_scan_samplerates_hz) / sizeof(k_scan_samplerates_hz[0])); i++) {
        if (k_scan_samplerates_hz[i] == sample_rate_hz)
            return true;
    }
    return false;
}

static int fft_len_index_from_value(int fft_len)
{
    int i;
    for (i = 0; i < (int)(sizeof(k_fft_lengths) / sizeof(k_fft_lengths[0])); i++) {
        if (k_fft_lengths[i] == fft_len)
            return i;
    }
    return 3; /* 1024 default */
}

static void make_window(float *dst, int n)
{
    int i;
    if (n <= 1)
        return;
    for (i = 0; i < n; i++)
        dst[i] = 0.5f - 0.5f * cosf((2.0f * (float)M_PI * i) / (float)(n - 1));
}

static uint32_t pack_rgba_u8(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (0xFFu << 24) |
           ((uint32_t)b << 16) |
           ((uint32_t)g << 8) |
           (uint32_t)r;
}

static uint32_t colormap_turbo_idx(int i)
{
    return pack_rgba_u8(gqrx_turbo[i][0], gqrx_turbo[i][1], gqrx_turbo[i][2]);
}

static uint32_t colormap_plasma_idx(int i)
{
    return pack_rgba_u8(gqrx_plasma[i][0], gqrx_plasma[i][1], gqrx_plasma[i][2]);
}

static uint32_t colormap_viridis_idx(int i)
{
    return pack_rgba_u8((int)(gqrx_viridis[i][0] * 256.0f),
                        (int)(gqrx_viridis[i][1] * 256.0f),
                        (int)(gqrx_viridis[i][2] * 256.0f));
}

static uint32_t colormap_white_hot_compressed_idx(int i)
{
    if (i < 64)
        return pack_rgba_u8(i * 4, i * 4, i * 4);
    return pack_rgba_u8(255, 255, 255);
}

static uint32_t colormap_white_hot_idx(int i)
{
    return pack_rgba_u8(i, i, i);
}

static uint32_t colormap_black_hot_idx(int i)
{
    return pack_rgba_u8(255 - i, 255 - i, 255 - i);
}

static uint32_t colormap_apply(int palette, float x)
{
    int idx;
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    idx = (int)(x * 255.0f + 0.5f);
    if (idx < 0) idx = 0;
    if (idx > 255) idx = 255;

    switch (palette) {
    case 0: return colormap_turbo_idx(idx);
    case 1: return colormap_plasma_idx(idx);
    case 2: return colormap_viridis_idx(idx);
    case 3: return colormap_white_hot_compressed_idx(idx);
    case 4: return colormap_white_hot_idx(idx);
    case 5: return colormap_black_hot_idx(idx);
    default:
        return colormap_turbo_idx(idx);
    }
}

static bool scan_dma_init(struct scan_state *s)
{
    memset(&s->dma, 0, sizeof(s->dma));
    s->dma.use_writer = 1;
    s->dma.zero_copy = 0;

    if (litepcie_dma_init(&s->dma, m2sdr_device, 0) < 0)
        return false;

    s->dma.writer_enable = 1;

    m2sdr_writel(s->dma.fds.fd, CSR_HEADER_RX_CONTROL_ADDR,
        (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
        (0 << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET));
    m2sdr_writel(s->dma.fds.fd, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);

    return true;
}

static void scan_dma_cleanup(struct scan_state *s)
{
    litepcie_dma_cleanup(&s->dma);
}

static bool capture_iq_block(struct scan_state *s)
{
    int got = 0;

    while (g_keep_running && got < s->fft_len) {
        litepcie_dma_process(&s->dma);

        while (got < s->fft_len) {
            char *buf = litepcie_dma_next_read_buffer(&s->dma);
            if (!buf)
                break;

            const int16_t *iq = (const int16_t *)buf;
            int iq_count = (int)(DMA_BUFFER_SIZE / (int)sizeof(int16_t));
            int pairs = iq_count / 2;
            int p;

            for (p = 0; p < pairs && got < s->fft_len; p++) {
                s->in_re[got] = (float)iq[p * 2 + 0] / 32768.0f;
                s->in_im[got] = (float)iq[p * 2 + 1] / 32768.0f;
                got++;
            }
        }
    }

    return got == s->fft_len;
}

static bool retune_rx(int64_t freq_hz)
{
    int ret = ad9361_set_rx_lo_freq(ad9361_phy, freq_hz);
    return ret == 0;
}

static bool scan_segment(struct scan_state *s, int64_t center_hz, int seg_index,
                         double *t_tune_s, double *t_capture_s, double *t_fft_s)
{
    int i;
    double t0, t1;

    t0 = now_s();
    if (!retune_rx(center_hz)) {
        fprintf(stderr, "Failed to tune RX LO to %" PRId64 " Hz\n", center_hz);
        return false;
    }
    usleep((useconds_t)s->rx_settle_us);
    t1 = now_s();
    *t_tune_s += (t1 - t0);

    t0 = now_s();
    if (!capture_iq_block(s))
        return false;
    t1 = now_s();
    *t_capture_s += (t1 - t0);

    t0 = now_s();
    for (i = 0; i < s->fft_len; i++) {
        s->in_re[i] *= s->window[i];
        s->in_im[i] *= s->window[i];
    }

    simple_fft_run(&s->fft, s->in_re, s->in_im, s->out_re, s->out_im);

    for (i = 0; i < s->fft_len; i++) {
        int shifted = (i + s->fft_len / 2) % s->fft_len;
        float re = s->out_re[shifted];
        float im = s->out_im[shifted];
        float pwr_lin = re * re + im * im + 1e-20f;
        float w = 1.0f;
        int dst = seg_index * s->stride_bins + i;

        if (s->overlap_bins > 0) {
            if (seg_index > 0 && i < s->overlap_bins) {
                float t = (float)(i + 1) / (float)(s->overlap_bins + 1);
                w *= t * t * (3.0f - 2.0f * t); /* smoothstep fade-in */
            }
            if (seg_index < s->segments - 1 && i >= s->fft_len - s->overlap_bins) {
                float t = (float)(s->fft_len - i) / (float)(s->overlap_bins + 1);
                w *= t * t * (3.0f - 2.0f * t); /* smoothstep fade-out */
            }
        }

        if (dst >= 0 && dst < s->waterfall_width) {
            s->line_pow_accum[dst] += pwr_lin * w;
            s->line_w_accum[dst] += w;
        }
    }
    t1 = now_s();
    *t_fft_s += (t1 - t0);

    return true;
}

static void waterfall_push_line(struct scan_state *s)
{
    int x;
    uint32_t *dst_last;

    if (s->lines > 1) {
        memmove(s->waterfall_rgba,
                s->waterfall_rgba + s->waterfall_tex_width,
                (size_t)(s->lines - 1) * (size_t)s->waterfall_tex_width * sizeof(uint32_t));
    }

    dst_last = s->waterfall_rgba + (size_t)(s->lines - 1) * (size_t)s->waterfall_tex_width;
    for (x = 0; x < s->waterfall_tex_width; x++) {
        int start = (int)((int64_t)x * s->waterfall_width / s->waterfall_tex_width);
        int stop  = (int)((int64_t)(x + 1) * s->waterfall_width / s->waterfall_tex_width);
        int j;
        float pwr = 0.0f;
        int n = stop - start;
        if (n < 1)
            n = 1;
        for (j = start; j < stop; j++)
            pwr += s->line_db[j];
        pwr /= (float)n;

        float t = (pwr - s->db_min) / (s->db_max - s->db_min + 1e-6f);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        dst_last[x] = colormap_apply(s->waterfall_palette, t);
    }

    glBindTexture(GL_TEXTURE_2D, s->waterfall_tex);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    s->waterfall_tex_width,
                    s->lines,
                    GL_BGRA,
                    GL_UNSIGNED_BYTE,
                    s->waterfall_rgba);
}

static int build_plot_slice(struct scan_state *s, int bin0, int bin1)
{
    int i;
    int bins = bin1 - bin0;

    if (bins <= 0)
        return 0;

    if (bins <= MAX_PLOT_POINTS) {
        for (i = 0; i < bins; i++)
            s->plot_db[i] = s->line_db[bin0 + i];
        return bins;
    }

    for (i = 0; i < MAX_PLOT_POINTS; i++) {
        int start = bin0 + (int)((int64_t)i * bins / MAX_PLOT_POINTS);
        int stop  = bin0 + (int)((int64_t)(i + 1) * bins / MAX_PLOT_POINTS);
        int j;
        float acc = 0.0f;
        int n = stop - start;
        if (n < 1)
            n = 1;
        for (j = start; j < stop; j++)
            acc += s->line_db[j];
        s->plot_db[i] = acc / (float)n;
    }

    return MAX_PLOT_POINTS;
}

static void format_freq_label(double hz, char *buf, size_t buflen)
{
    if (hz >= 1e9)
        snprintf(buf, buflen, "%.3fG", hz / 1e9);
    else
        snprintf(buf, buflen, "%.1fM", hz / 1e6);
}

static void draw_spectrum_with_grid(struct scan_state *s,
                                    const char *id,
                                    float width,
                                    float height,
                                    int plot_count,
                                    double f0_hz,
                                    double f1_hz)
{
    int i;
    int v_ticks = 10;
    int h_ticks = 4;
    float y_min = s->db_min;
    float y_max = s->db_max;
    ImVec2 pmin, pmax;
    ImDrawList *dl;
    ImU32 col_bg = 0xFF131415u;
    ImU32 col_border = 0xFF3A3A3Au;
    ImU32 col_grid_v = 0xFF4A4A4Au;
    ImU32 col_grid_h = 0xFF2A2A2Au;
    ImU32 col_trace = 0xFF66D9FFu;
    ImU32 col_text = 0xFFAFAFAFu;

    if (plot_count <= 1 || width <= 4.0f || height <= 4.0f)
        return;

    igInvisibleButton(id, (ImVec2){width, height}, 0);
    igGetItemRectMin(&pmin);
    igGetItemRectMax(&pmax);
    dl = igGetWindowDrawList();

    ImDrawList_AddRectFilled(dl, pmin, pmax, col_bg, 2.0f, 0);
    ImDrawList_AddRect(dl, pmin, pmax, col_border, 2.0f, 0, 1.0f);

    for (i = 0; i <= v_ticks; i++) {
        float x = pmin.x + (pmax.x - pmin.x) * (float)i / (float)v_ticks;
        ImDrawList_AddLine(dl, (ImVec2){x, pmin.y}, (ImVec2){x, pmax.y}, col_grid_v, 1.2f);
    }
    for (i = 0; i <= h_ticks; i++) {
        float y = pmin.y + (pmax.y - pmin.y) * (float)i / (float)h_ticks;
        ImDrawList_AddLine(dl, (ImVec2){pmin.x, y}, (ImVec2){pmax.x, y}, col_grid_h, 1.0f);
    }

    for (i = 0; i < plot_count; i++) {
        float t = (float)i / (float)(plot_count - 1);
        float x = pmin.x + t * (pmax.x - pmin.x);
        float yn = (s->plot_db[i] - y_min) / (y_max - y_min + 1e-6f);
        float y = pmax.y - yn * (pmax.y - pmin.y);
        if (y < pmin.y) y = pmin.y;
        if (y > pmax.y) y = pmax.y;
        s->plot_points[i] = (ImVec2){x, y};
    }
    ImDrawList_AddPolyline(dl, s->plot_points, plot_count, col_trace, 0, 1.6f);

    for (i = 0; i <= v_ticks; i++) {
        char txt[32];
        double f = f0_hz + (f1_hz - f0_hz) * (double)i / (double)v_ticks;
        format_freq_label(f, txt, sizeof(txt));
        ImDrawList_AddText_Vec2(dl, (ImVec2){pmin.x + (pmax.x - pmin.x) * (float)i / (float)v_ticks + 2.0f, pmax.y - 16.0f},
                                col_text, txt, NULL);
    }
}

static bool scan_line(struct scan_state *s)
{
    double fs = (double)s->sample_rate_hz;
    double start = (double)s->scan_start_hz;
    double t_line0, t_line1, t_wf0, t_wf1;
    double t_tune_s = 0.0, t_capture_s = 0.0, t_fft_s = 0.0;
    double dt;
    int seg;

    memset(s->line_pow_accum, 0, (size_t)s->waterfall_width * sizeof(float));
    memset(s->line_w_accum, 0, (size_t)s->waterfall_width * sizeof(float));

    t_line0 = now_s();
    for (seg = 0; seg < s->segments; seg++) {
        double seg_start = start + (double)seg * s->step_hz;
        int64_t center_hz = (int64_t)(seg_start + fs * 0.5);
        if (!scan_segment(s, center_hz, seg, &t_tune_s, &t_capture_s, &t_fft_s))
            return false;
    }

    for (seg = 0; seg < s->waterfall_width; seg++) {
        float w = s->line_w_accum[seg];
        if (w > 1e-12f)
            s->line_db[seg] = 10.0f * log10f(s->line_pow_accum[seg] / w + 1e-20f);
        else
            s->line_db[seg] = s->db_min;
    }

    t_wf0 = now_s();
    waterfall_push_line(s);
    t_wf1 = now_s();
    t_line1 = now_s();

    s->perf.lines_total++;
    s->perf.captures_total += (uint64_t)s->segments;
    s->perf.retunes_total += (uint64_t)s->segments;

    s->perf.last_tune_ms = t_tune_s * 1e3;
    s->perf.last_capture_ms = t_capture_s * 1e3;
    s->perf.last_fft_ms = t_fft_s * 1e3;
    s->perf.last_waterfall_ms = (t_wf1 - t_wf0) * 1e3;
    s->perf.last_line_ms = (t_line1 - t_line0) * 1e3;

    s->perf.ema_tune_ms = ema_update(s->perf.ema_tune_ms, s->perf.last_tune_ms, 0.2);
    s->perf.ema_capture_ms = ema_update(s->perf.ema_capture_ms, s->perf.last_capture_ms, 0.2);
    s->perf.ema_fft_ms = ema_update(s->perf.ema_fft_ms, s->perf.last_fft_ms, 0.2);
    s->perf.ema_waterfall_ms = ema_update(s->perf.ema_waterfall_ms, s->perf.last_waterfall_ms, 0.2);
    s->perf.ema_line_ms = ema_update(s->perf.ema_line_ms, s->perf.last_line_ms, 0.2);

    if (s->perf.prev_rate_t <= 0.0)
        s->perf.prev_rate_t = t_line1;

    dt = t_line1 - s->perf.prev_rate_t;
    if (dt >= 0.25) {
        double d_lines = (double)(s->perf.lines_total - s->perf.lines_at_prev_rate);
        double d_caps = (double)(s->perf.captures_total - s->perf.captures_at_prev_rate);
        double d_retunes = (double)(s->perf.retunes_total - s->perf.retunes_at_prev_rate);

        s->perf.lines_per_sec = d_lines / dt;
        s->perf.captures_per_sec = d_caps / dt;
        s->perf.retunes_per_sec = d_retunes / dt;
        s->perf.iq_msps = (s->perf.captures_per_sec * (double)s->fft_len) / 1e6;
        s->perf.sweep_mhz_per_sec = s->perf.lines_per_sec *
            ((double)(s->scan_stop_hz - s->scan_start_hz) / 1e6);

        s->perf.lines_at_prev_rate = s->perf.lines_total;
        s->perf.captures_at_prev_rate = s->perf.captures_total;
        s->perf.retunes_at_prev_rate = s->perf.retunes_total;
        s->perf.prev_rate_t = t_line1;
    }

    return true;
}

static bool resize_buffers(struct scan_state *s)
{
    double range_hz;
    double stride_ratio;
    double step_hz;
    int new_segments;
    int new_width;
    int stride_bins;
    int overlap_bins;
    int tex_width;

    simple_fft_destroy(&s->fft);

    free(s->in_re);
    free(s->in_im);
    free(s->out_re);
    free(s->out_im);
    free(s->window);
    free(s->line_db);
    free(s->line_pow_accum);
    free(s->line_w_accum);
    free(s->plot_db);
    free(s->plot_points);
    free(s->waterfall_rgba);

    s->in_re = NULL;
    s->in_im = NULL;
    s->out_re = NULL;
    s->out_im = NULL;
    s->window = NULL;
    s->line_db = NULL;
    s->line_pow_accum = NULL;
    s->line_w_accum = NULL;
    s->plot_db = NULL;
    s->plot_points = NULL;
    s->waterfall_rgba = NULL;

    if (s->scan_stop_hz <= s->scan_start_hz)
        s->scan_stop_hz = s->scan_start_hz + s->sample_rate_hz;

    range_hz = (double)(s->scan_stop_hz - s->scan_start_hz);
    if (s->stitch_mode == 1) {
        stride_ratio = 1.0; /* Fast mode: no overlap between adjacent captures. */
    } else {
        stride_ratio = (double)s->rf_bandwidth_hz / (double)s->sample_rate_hz;
        if (stride_ratio < 0.5)
            stride_ratio = 0.5;
        if (stride_ratio > 1.0)
            stride_ratio = 1.0;
    }

    stride_bins = (int)llround((double)s->fft_len * stride_ratio);
    if (stride_bins < 1)
        stride_bins = 1;
    if (stride_bins > s->fft_len)
        stride_bins = s->fft_len;
    overlap_bins = s->fft_len - stride_bins;
    step_hz = (double)s->sample_rate_hz * (double)stride_bins / (double)s->fft_len;

    if (range_hz <= (double)s->sample_rate_hz) {
        new_segments = 1;
    } else {
        new_segments = 1 + (int)ceil((range_hz - (double)s->sample_rate_hz) / step_hz);
    }
    if (new_segments < 1)
        new_segments = 1;

    new_width = (new_segments - 1) * stride_bins + s->fft_len;
    if (new_width <= 0 || new_width > MAX_WATERFALL_WIDTH) {
        fprintf(stderr,
            "Waterfall width %d is invalid (range too wide or FFT too large).\n",
            new_width);
        return false;
    }

    if (s->gl_max_texture_size <= 0) {
        GLint max_tex = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
        if (max_tex <= 0)
            max_tex = 4096;
        s->gl_max_texture_size = (int)max_tex;
    }

    tex_width = new_width;
    if (tex_width > s->gl_max_texture_size)
        tex_width = s->gl_max_texture_size;

    s->in_re = (float *)calloc((size_t)s->fft_len, sizeof(float));
    s->in_im = (float *)calloc((size_t)s->fft_len, sizeof(float));
    s->out_re = (float *)calloc((size_t)s->fft_len, sizeof(float));
    s->out_im = (float *)calloc((size_t)s->fft_len, sizeof(float));
    s->window = (float *)calloc((size_t)s->fft_len, sizeof(float));
    s->line_db = (float *)calloc((size_t)new_width, sizeof(float));
    s->line_pow_accum = (float *)calloc((size_t)new_width, sizeof(float));
    s->line_w_accum = (float *)calloc((size_t)new_width, sizeof(float));
    s->plot_db = (float *)calloc((size_t)MAX_PLOT_POINTS, sizeof(float));
    s->plot_points = (ImVec2 *)calloc((size_t)MAX_PLOT_POINTS, sizeof(ImVec2));
    s->waterfall_rgba = (uint32_t *)calloc((size_t)tex_width * (size_t)s->lines, sizeof(uint32_t));

    if (!s->in_re || !s->in_im || !s->out_re || !s->out_im ||
        !s->window || !s->line_db || !s->line_pow_accum || !s->line_w_accum ||
        !s->plot_db || !s->plot_points || !s->waterfall_rgba) {
        fprintf(stderr, "Out of memory allocating scan buffers.\n");
        return false;
    }

    if (simple_fft_init(&s->fft, s->fft_len) < 0) {
        fprintf(stderr, "Failed to initialize FFT plan for length %d.\n", s->fft_len);
        return false;
    }

    make_window(s->window, s->fft_len);

    s->segments = new_segments;
    s->stride_bins = stride_bins;
    s->overlap_bins = overlap_bins;
    s->step_hz = step_hz;
    s->waterfall_width = new_width;
    s->waterfall_tex_width = tex_width;

    if (!s->waterfall_tex)
        glGenTextures(1, &s->waterfall_tex);

    glBindTexture(GL_TEXTURE_2D, s->waterfall_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA8,
                 s->waterfall_tex_width,
                 s->lines,
                 0,
                 GL_BGRA,
                 GL_UNSIGNED_BYTE,
                 s->waterfall_rgba);

    return true;
}

static bool m2sdr_rfic_init(struct scan_state *s)
{
    void *conn = m2sdr_open();

#ifdef CSR_SI5351_BASE
    m2sdr_writel(conn, CSR_SI5351_CONTROL_ADDR,
                 SI5351B_VERSION * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET));

    if (s->refclk_hz == 40000000) {
        m2sdr_si5351_i2c_config(conn,
            SI5351_I2C_ADDR,
            si5351_xo_40m_config,
            sizeof(si5351_xo_40m_config) / sizeof(si5351_xo_40m_config[0]));
    } else {
        m2sdr_si5351_i2c_config(conn,
            SI5351_I2C_ADDR,
            si5351_xo_38p4m_config,
            sizeof(si5351_xo_38p4m_config) / sizeof(si5351_xo_38p4m_config[0]));
    }
#endif

    m2sdr_ad9361_spi_init(conn, 1);

    default_init_param.reference_clk_rate = s->refclk_hz;
    default_init_param.gpio_resetb = AD9361_GPIO_RESET_PIN;
    default_init_param.gpio_sync = -1;
    default_init_param.gpio_cal_sw1 = -1;
    default_init_param.gpio_cal_sw2 = -1;

    default_init_param.two_rx_two_tx_mode_enable = 0;
    default_init_param.one_rx_one_tx_mode_use_rx_num = 0;
    default_init_param.one_rx_one_tx_mode_use_tx_num = 0;
    default_init_param.two_t_two_r_timing_enable = 0;
    m2sdr_writel(conn, CSR_AD9361_PHY_CONTROL_ADDR, 1);

    if (ad9361_init(&ad9361_phy, &default_init_param, 1) < 0) {
        fprintf(stderr, "Failed to initialize AD9361.\n");
        return false;
    }

    ad9361_set_tx_sampling_freq(ad9361_phy, s->sample_rate_hz);
    ad9361_set_rx_sampling_freq(ad9361_phy, s->sample_rate_hz);
    ad9361_set_rx_rf_bandwidth(ad9361_phy, s->rf_bandwidth_hz);
    ad9361_set_tx_rf_bandwidth(ad9361_phy, s->rf_bandwidth_hz);

    ad9361_set_tx_atten(ad9361_phy, -SCAN_TX_GAIN_DB * 1000, 1, 1, 1);
    ad9361_set_rx_rf_gain(ad9361_phy, 0, s->rx_gain);
    ad9361_set_rx_rf_gain(ad9361_phy, 1, s->rx_gain);

    ad9361_set_tx_lo_freq(ad9361_phy, (s->scan_start_hz + s->scan_stop_hz) / 2);
    ad9361_set_rx_lo_freq(ad9361_phy, (s->scan_start_hz + s->scan_stop_hz) / 2);

    m2sdr_writel(conn, CSR_AD9361_BITMODE_ADDR, 0);

    return true;
}

static void help(void)
{
    printf("M2SDR RF Scan Utility (Dear ImGui)\n"
           "usage: m2sdr_scan [options]\n"
           "\n"
           "Options:\n"
           "  -h                     Show this help message and exit.\n"
           "  -c device_num          Select device number (default: 0).\n"
           "  -refclk_freq hz        AD9361 reference clock in Hz (default: %" PRId64 ").\n"
           "  -start_freq hz         Scan start frequency in Hz (default: %" PRId64 ").\n"
           "  -stop_freq hz          Scan stop frequency in Hz (default: %" PRId64 ").\n"
           "  -rx_gain db            RX gain in dB [0..73] (default: %d).\n"
           "  -sample_rate hz        Scan sample rate in Hz (default: %u).\n"
           "  -fft_len n             FFT length (power of two, default: %d).\n"
           "  -lines n               Waterfall lines (default: %d).\n"
           "\n"
           "Runtime controls in UI:\n"
           "  - Scan start/stop (MHz), sample rate, stitch mode, settle time, FFT length, line count,\n"
           "    RX gain and dB scale.\n"
           "  - Parameters are applied live while moving sliders.\n"
           "\n"
           "Notes:\n"
           "  - Supported samplerates are submultiples of 61.44 MSPS.\n"
           "  - Full-spectrum scans are possible but slower because they retune LO per segment.\n",
           DEFAULT_REFCLK_FREQ,
           (int64_t)DEFAULT_START_FREQ_HZ,
           (int64_t)DEFAULT_STOP_FREQ_HZ,
           DEFAULT_RX_GAIN_DB,
           DEFAULT_SCAN_SAMPLERATE_HZ,
           DEFAULT_FFT_LEN,
           DEFAULT_LINES);
}

static bool is_power_of_two_int(int n)
{
    return (n > 0) && ((n & (n - 1)) == 0);
}

static bool apply_runtime_config(struct scan_state *s,
                                 int64_t start_hz,
                                 int64_t stop_hz,
                                 uint32_t sample_rate_hz,
                                 int fft_len,
                                 int lines,
                                 int rx_gain,
                                 int rx_settle_us,
                                 int stitch_mode)
{
    if (!is_power_of_two_int(fft_len) || fft_len < 128 || fft_len > 2048) {
        fprintf(stderr, "Invalid FFT length %d (must be power of two between 128 and 2048).\n", fft_len);
        return false;
    }
    if (lines < 32 || lines > 2048) {
        fprintf(stderr, "Invalid line count %d (must be 32..2048).\n", lines);
        return false;
    }
    if (start_hz < RX_FREQ_MIN || start_hz > RX_FREQ_MAX ||
        stop_hz < RX_FREQ_MIN || stop_hz > RX_FREQ_MAX) {
        fprintf(stderr, "Invalid scan range: %" PRId64 "..%" PRId64 " Hz\n", start_hz, stop_hz);
        return false;
    }
    if (stop_hz <= start_hz) {
        fprintf(stderr, "Invalid scan range: stop must be greater than start.\n");
        return false;
    }
    if (rx_gain < RX_GAIN_MIN || rx_gain > RX_GAIN_MAX) {
        fprintf(stderr, "Invalid rx_gain %d (must be %d..%d).\n", rx_gain, RX_GAIN_MIN, RX_GAIN_MAX);
        return false;
    }
    if (!is_supported_samplerate(sample_rate_hz)) {
        fprintf(stderr, "Invalid sample rate %u Hz (supported: 61440000, 30720000, 15360000, 7680000).\n",
                sample_rate_hz);
        return false;
    }
    if (rx_settle_us < 0 || rx_settle_us > 100000) {
        fprintf(stderr, "Invalid settle time %d us (must be 0..100000).\n", rx_settle_us);
        return false;
    }
    if (stitch_mode < 0 || stitch_mode > 1) {
        fprintf(stderr, "Invalid stitch mode %d.\n", stitch_mode);
        return false;
    }

    s->scan_start_hz = start_hz;
    s->scan_stop_hz = stop_hz;
    s->sample_rate_hz = sample_rate_hz;
    s->rf_bandwidth_hz = scan_bandwidth_from_samplerate(sample_rate_hz);
    s->fft_len = fft_len;
    s->lines = lines;
    s->rx_gain = rx_gain;
    s->rx_settle_us = rx_settle_us;
    s->stitch_mode = stitch_mode;

    s->perf.prev_rate_t = now_s();
    s->perf.lines_at_prev_rate = s->perf.lines_total;
    s->perf.captures_at_prev_rate = s->perf.captures_total;
    s->perf.retunes_at_prev_rate = s->perf.retunes_total;

    ad9361_set_tx_sampling_freq(ad9361_phy, s->sample_rate_hz);
    ad9361_set_rx_sampling_freq(ad9361_phy, s->sample_rate_hz);
    ad9361_set_rx_rf_bandwidth(ad9361_phy, s->rf_bandwidth_hz);
    ad9361_set_tx_rf_bandwidth(ad9361_phy, s->rf_bandwidth_hz);
    ad9361_set_rx_rf_gain(ad9361_phy, 0, s->rx_gain);
    ad9361_set_rx_rf_gain(ad9361_phy, 1, s->rx_gain);

    return resize_buffers(s);
}

int main(int argc, char **argv)
{
    int c;
    int option_index = 0;
    struct scan_state s;
    SDL_Window *window = NULL;
    SDL_GLContext gl_context = NULL;
    bool quit = false;
    const char *glsl_version = "#version 130";
    float ui_start_mhz;
    float ui_stop_mhz;
    int ui_samplerate_idx;
    int ui_fft_idx;
    int ui_lines;
    int ui_rx_gain;
    int ui_settle_us;
    int ui_stitch_mode;

    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "refclk_freq", required_argument, NULL, 1 },
        { "start_freq", required_argument, NULL, 2 },
        { "stop_freq", required_argument, NULL, 3 },
        { "rx_gain", required_argument, NULL, 4 },
        { "fft_len", required_argument, NULL, 5 },
        { "lines", required_argument, NULL, 6 },
        { "sample_rate", required_argument, NULL, 7 },
        { NULL, 0, NULL, 0 }
    };

    memset(&s, 0, sizeof(s));
    s.refclk_hz = DEFAULT_REFCLK_FREQ;
    s.sample_rate_hz = DEFAULT_SCAN_SAMPLERATE_HZ;
    s.rf_bandwidth_hz = scan_bandwidth_from_samplerate(s.sample_rate_hz);
    s.scan_start_hz = DEFAULT_START_FREQ_HZ;
    s.scan_stop_hz = DEFAULT_STOP_FREQ_HZ;
    s.rx_gain = DEFAULT_RX_GAIN_DB;
    s.fft_len = DEFAULT_FFT_LEN;
    s.lines = DEFAULT_LINES;
    s.db_min = DEFAULT_DB_MIN;
    s.db_max = DEFAULT_DB_MAX;
    s.rx_settle_us = DEFAULT_RX_SETTLE_US;
    s.stitch_mode = 0;
    s.run = true;
    s.display_rows = 1;
    s.waterfall_palette = 0;

    for (;;) {
        c = getopt_long_only(argc, argv, "hc:", options, &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'h':
            help();
            return 0;
        case 'c':
            m2sdr_device_num = atoi(optarg);
            break;
        case 0:
            break;
        case 1:
            s.refclk_hz = (int64_t)strtod(optarg, NULL);
            break;
        case 2:
            s.scan_start_hz = (int64_t)strtod(optarg, NULL);
            break;
        case 3:
            s.scan_stop_hz = (int64_t)strtod(optarg, NULL);
            break;
        case 4:
            s.rx_gain = atoi(optarg);
            break;
        case 5:
            s.fft_len = atoi(optarg);
            break;
        case 6:
            s.lines = atoi(optarg);
            break;
        case 7:
            s.sample_rate_hz = (uint32_t)strtoul(optarg, NULL, 10);
            if (!is_supported_samplerate(s.sample_rate_hz)) {
                fprintf(stderr, "Unsupported sample rate %u Hz.\n", s.sample_rate_hz);
                return 1;
            }
            s.rf_bandwidth_hz = scan_bandwidth_from_samplerate(s.sample_rate_hz);
            break;
        default:
            return 1;
        }
    }

    snprintf(m2sdr_device, sizeof(m2sdr_device), "/dev/m2sdr%d", m2sdr_device_num);

    signal(SIGINT, int_handler);

    if (!m2sdr_rfic_init(&s))
        goto fail;

    if (!scan_dma_init(&s))
        goto fail;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        goto fail;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    window = SDL_CreateWindow("M2SDR RF Scan",
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              1400,
                              850,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                              SDL_WINDOW_MAXIMIZED);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        goto fail;
    }

    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        goto fail;
    }

    SDL_GL_SetSwapInterval(1);

    igCreateContext(NULL);
    igStyleColorsDark(NULL);

    ImGuiIO *io = igGetIO();
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    if (!m2sdr_imgui_sdl2_init_for_opengl(window, gl_context)) {
        fprintf(stderr, "ImGui SDL2 backend init failed.\n");
        goto fail;
    }
    if (!m2sdr_imgui_opengl3_init(glsl_version)) {
        fprintf(stderr, "ImGui OpenGL3 backend init failed.\n");
        goto fail;
    }

    if (!resize_buffers(&s))
        goto fail;

    ui_start_mhz = (float)s.scan_start_hz / 1e6f;
    ui_stop_mhz = (float)s.scan_stop_hz / 1e6f;
    ui_samplerate_idx = samplerate_index_from_hz(s.sample_rate_hz);
    ui_fft_idx = fft_len_index_from_value(s.fft_len);
    ui_lines = s.lines;
    ui_rx_gain = s.rx_gain;
    ui_settle_us = s.rx_settle_us;
    ui_stitch_mode = s.stitch_mode;

    while (!quit && g_keep_running) {
        SDL_Event e;

        while (SDL_PollEvent(&e)) {
            m2sdr_imgui_sdl2_process_event(&e);
            if (e.type == SDL_QUIT)
                quit = true;
            if (e.type == SDL_WINDOWEVENT &&
                e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window))
                quit = true;
        }

        if (s.run) {
            if (!scan_line(&s)) {
                fprintf(stderr, "Scan failed, stopping.\n");
                quit = true;
            }
        }

        m2sdr_imgui_opengl3_new_frame();
        m2sdr_imgui_sdl2_new_frame();
        igNewFrame();

        igSetNextWindowPos((ImVec2){0, 0}, ImGuiCond_Always, (ImVec2){0, 0});
        igSetNextWindowSize(io->DisplaySize, ImGuiCond_Always);

        igBegin("RF Scan", NULL,
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse);
        igText("Device: %s", m2sdr_device);
        igText("Samplerate: %.2f MSPS | RF BW: %.2f MHz",
               (double)s.sample_rate_hz / 1e6,
               (double)s.rf_bandwidth_hz / 1e6);
        igText("Segments: %d | FFT Width: %d bins | Waterfall Tex Width: %d px (GL max: %d)",
               s.segments, s.waterfall_width, s.waterfall_tex_width, s.gl_max_texture_size);
        igText("Stitch: %s | settle %d us | step %.3f MHz | stride %d bins | overlap %d bins",
               s.stitch_mode == 1 ? "Fast" : "Quality",
               s.rx_settle_us, s.step_hz / 1e6, s.stride_bins, s.overlap_bins);

        igSeparator();
        igText("Performance (Benchmark)");
        igText("Throughput: %.2f lines/s | line avg %.3f ms",
               s.perf.lines_per_sec,
               s.perf.ema_line_ms);
        igText("Split avg (ms): tune %.3f | capture %.3f | FFT %.3f | waterfall %.3f",
               s.perf.ema_tune_ms,
               s.perf.ema_capture_ms,
               s.perf.ema_fft_ms,
               s.perf.ema_waterfall_ms);
        if (s.perf.ema_line_ms > 0.0) {
            igText("Split avg (%%): tune %.1f | capture %.1f | FFT %.1f | waterfall %.1f",
                   100.0 * s.perf.ema_tune_ms / s.perf.ema_line_ms,
                   100.0 * s.perf.ema_capture_ms / s.perf.ema_line_ms,
                   100.0 * s.perf.ema_fft_ms / s.perf.ema_line_ms,
                   100.0 * s.perf.ema_waterfall_ms / s.perf.ema_line_ms);
        }

        igSeparator();

        {
            bool changed = false;
            bool changed_start = false;
            bool changed_stop = false;
            const float min_span_mhz = 1.0f;

            changed_start = igSliderFloat("Scan Start (MHz)", &ui_start_mhz, 70.0f, 6000.0f, "%.3f", 0);
            changed_stop  = igSliderFloat("Scan Stop (MHz)",  &ui_stop_mhz,  70.0f, 6000.0f, "%.3f", 0);
            changed = changed_start || changed_stop;

            if (changed_start && ui_start_mhz > ui_stop_mhz - min_span_mhz)
                ui_stop_mhz = ui_start_mhz + min_span_mhz;
            if (changed_stop && ui_stop_mhz < ui_start_mhz + min_span_mhz)
                ui_start_mhz = ui_stop_mhz - min_span_mhz;

            changed |= igCombo_Str_arr("Sample Rate", &ui_samplerate_idx,
                                        k_scan_samplerate_labels,
                                        (int)(sizeof(k_scan_samplerate_labels) / sizeof(k_scan_samplerate_labels[0])),
                                        4);
            {
                static const char *stitch_mode_items[] = {
                    "Quality (Overlap)",
                    "Fast (No Overlap)"
                };
                if (ui_stitch_mode < 0 || ui_stitch_mode > 1)
                    ui_stitch_mode = 0;
                changed |= igCombo_Str_arr("Stitch Mode", &ui_stitch_mode, stitch_mode_items, 2, 2);
            }
            changed |= igCombo_Str_arr("FFT Points", &ui_fft_idx,
                                       k_fft_length_labels,
                                       (int)(sizeof(k_fft_length_labels) / sizeof(k_fft_length_labels[0])),
                                       5);
            changed |= igSliderInt("Lines", &ui_lines, 32, 2048, "%d", 0);
            changed |= igSliderInt("Display Rows", &s.display_rows, 1, 8, "%d", 0);
            changed |= igSliderInt("RX Gain (dB)", &ui_rx_gain, 0, 73, "%d", 0);
            changed |= igSliderInt("Settle (us)", &ui_settle_us, 0, 5000, "%d", 0);
            changed |= igSliderFloat("Min dB", &s.db_min, -160.0f, 20.0f, "%.1f", 0);
            changed |= igSliderFloat("Max dB", &s.db_max, -160.0f, 40.0f, "%.1f", 0);
            {
                static const char *palette_items[] = {
                    "Google Turbo",
                    "Plasma",
                    "Viridis",
                    "White Hot Compressed",
                    "White Hot",
                    "Black Hot"
                };
                int p = s.waterfall_palette;
                if (p < 0 || p > 5)
                    p = 0;
                if (igCombo_Str_arr("Waterfall Palette", &p, palette_items, 6, 6)) {
                    s.waterfall_palette = p;
                    changed = true;
                }
            }
            if (s.db_max <= s.db_min + 1.0f) {
                s.db_max = s.db_min + 1.0f;
                changed = true;
            }

            if (changed) {
                int64_t new_start = (int64_t)(ui_start_mhz * 1e6f);
                int64_t new_stop = (int64_t)(ui_stop_mhz * 1e6f);
                if (ui_samplerate_idx < 0 ||
                    ui_samplerate_idx >= (int)(sizeof(k_scan_samplerates_hz) / sizeof(k_scan_samplerates_hz[0])))
                    ui_samplerate_idx = 0;
                if (ui_fft_idx < 0 ||
                    ui_fft_idx >= (int)(sizeof(k_fft_lengths) / sizeof(k_fft_lengths[0])))
                    ui_fft_idx = fft_len_index_from_value(s.fft_len);
                uint32_t new_samplerate = k_scan_samplerates_hz[ui_samplerate_idx];
                int new_fft_len = k_fft_lengths[ui_fft_idx];
                if (apply_runtime_config(&s, new_start, new_stop, new_samplerate, new_fft_len, ui_lines, ui_rx_gain,
                                         ui_settle_us, ui_stitch_mode)) {
                    ui_start_mhz = (float)s.scan_start_hz / 1e6f;
                    ui_stop_mhz = (float)s.scan_stop_hz / 1e6f;
                    ui_samplerate_idx = samplerate_index_from_hz(s.sample_rate_hz);
                    ui_fft_idx = fft_len_index_from_value(s.fft_len);
                    ui_lines = s.lines;
                    ui_rx_gain = s.rx_gain;
                    ui_settle_us = s.rx_settle_us;
                    ui_stitch_mode = s.stitch_mode;
                } else {
                    ui_start_mhz = (float)s.scan_start_hz / 1e6f;
                    ui_stop_mhz = (float)s.scan_stop_hz / 1e6f;
                    ui_samplerate_idx = samplerate_index_from_hz(s.sample_rate_hz);
                    ui_fft_idx = fft_len_index_from_value(s.fft_len);
                    ui_lines = s.lines;
                    ui_rx_gain = s.rx_gain;
                    ui_settle_us = s.rx_settle_us;
                    ui_stitch_mode = s.stitch_mode;
                }
            }
        }

        igSeparator();

        ImVec2 avail;
        igGetContentRegionAvail(&avail);
        if (avail.x > 10.0f && avail.y > 10.0f) {
            int row;
            int rows = s.display_rows;
            const float row_spacing = 4.0f;
            const float label_h = igGetTextLineHeightWithSpacing();
            const float spectrum_row_h = 56.0f;
            float waterfall_avail_h;
            float waterfall_row_h;
            ImTextureRef tex_ref;

            if (rows < 1)
                rows = 1;

            waterfall_avail_h = avail.y - rows * (label_h + spectrum_row_h + row_spacing) - row_spacing;
            if (waterfall_avail_h < rows * (label_h + 8.0f))
                waterfall_avail_h = rows * (label_h + 8.0f);
            waterfall_row_h = (waterfall_avail_h - rows * (label_h + row_spacing)) / rows;
            if (waterfall_row_h < 8.0f)
                waterfall_row_h = 8.0f;

            tex_ref._TexData = NULL;
            tex_ref._TexID = (ImTextureID)(uintptr_t)s.waterfall_tex;

            /* Alternate Spectrum/Waterfall per row for direct temporal continuity. */
            for (row = 0; row < rows; row++) {
                double total_hz = (double)(s.scan_stop_hz - s.scan_start_hz);
                double f0_hz = (double)s.scan_start_hz + total_hz * (double)row / (double)rows;
                double f1_hz = (double)s.scan_start_hz + total_hz * (double)(row + 1) / (double)rows;
                int bin0 = (int)((int64_t)row * s.waterfall_width / rows);
                int bin1 = (int)((int64_t)(row + 1) * s.waterfall_width / rows);
                int plot_count = build_plot_slice(&s, bin0, bin1);
                char overlay[128];
                char plot_id[32];
                float u0 = (float)row / (float)rows;
                float u1 = (float)(row + 1) / (float)rows;

                snprintf(overlay, sizeof(overlay), "Row %d", row + 1);
                snprintf(plot_id, sizeof(plot_id), "##spectrum_row_%d", row);
                (void)overlay;
                draw_spectrum_with_grid(&s, plot_id, avail.x, spectrum_row_h, plot_count, f0_hz, f1_hz);

                igImage(tex_ref, (ImVec2){avail.x, waterfall_row_h}, (ImVec2){u0, 1}, (ImVec2){u1, 0});

                if (row != rows - 1)
                    igSeparator();
            }
        }

        igEnd();

        igRender();

        glViewport(0, 0, (int)io->DisplaySize.x, (int)io->DisplaySize.y);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        m2sdr_imgui_opengl3_render_draw_data(igGetDrawData());

        SDL_GL_SwapWindow(window);
    }

    m2sdr_imgui_opengl3_shutdown();
    m2sdr_imgui_sdl2_shutdown();
    igDestroyContext(NULL);

    if (s.waterfall_tex)
        glDeleteTextures(1, &s.waterfall_tex);
    scan_dma_cleanup(&s);
    simple_fft_destroy(&s.fft);
    free(s.in_re);
    free(s.in_im);
    free(s.out_re);
    free(s.out_im);
    free(s.window);
    free(s.line_db);
    free(s.line_pow_accum);
    free(s.line_w_accum);
    free(s.plot_db);
    free(s.plot_points);
    free(s.waterfall_rgba);

    if (gl_context)
        SDL_GL_DeleteContext(gl_context);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();

    m2sdr_close_global();
    return 0;

fail:
    if (gl_context)
        SDL_GL_DeleteContext(gl_context);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();

    if (s.waterfall_tex)
        glDeleteTextures(1, &s.waterfall_tex);

    if (s.dma.fds.fd > 0)
        scan_dma_cleanup(&s);

    simple_fft_destroy(&s.fft);
    free(s.in_re);
    free(s.in_im);
    free(s.out_re);
    free(s.out_im);
    free(s.window);
    free(s.line_db);
    free(s.line_pow_accum);
    free(s.line_w_accum);
    free(s.plot_db);
    free(s.plot_points);
    free(s.waterfall_rgba);

    m2sdr_close_global();
    return 1;
}
