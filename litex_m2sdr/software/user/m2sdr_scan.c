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
#include "simple_fft.h"

#define igGetIO igGetIO_Nil

#define SCAN_SAMPLERATE_HZ 61440000U
#define SCAN_BANDWIDTH_HZ  56000000
#define SCAN_TX_GAIN_DB    -30
#define DEFAULT_START_FREQ_HZ 2300000000LL
#define DEFAULT_STOP_FREQ_HZ  2500000000LL
#define DEFAULT_RX_GAIN_DB    35
#define DEFAULT_FFT_LEN       1024
#define DEFAULT_LINES         300
#define DEFAULT_DB_MIN        -120.0f
#define DEFAULT_DB_MAX        -20.0f

#define RX_SETTLE_US 2000
#define MAX_WATERFALL_WIDTH 262144
#define MAX_PLOT_POINTS 4096

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
    int fft_len;
    int lines;
    int rx_gain;
    int64_t refclk_hz;
    int64_t scan_start_hz;
    int64_t scan_stop_hz;
    float db_min;
    float db_max;
    bool run;
    int display_rows;

    int segments;
    int waterfall_width;

    struct litepcie_dma_ctrl dma;
    struct simple_fft_plan fft;

    float *in_re;
    float *in_im;
    float *out_re;
    float *out_im;
    float *window;
    float *line_db;
    float *plot_db;
    uint32_t *waterfall_rgba;

    GLuint waterfall_tex;
};

static void make_window(float *dst, int n)
{
    int i;
    if (n <= 1)
        return;
    for (i = 0; i < n; i++)
        dst[i] = 0.5f - 0.5f * cosf((2.0f * (float)M_PI * i) / (float)(n - 1));
}

static uint32_t colormap_jet(float x)
{
    float r, g, b;
    float four_x = 4.0f * x;

    if (four_x <= 1.0f) {
        r = 0.0f;
        g = four_x;
        b = 1.0f;
    } else if (four_x <= 2.0f) {
        r = 0.0f;
        g = 1.0f;
        b = 2.0f - four_x;
    } else if (four_x <= 3.0f) {
        r = four_x - 2.0f;
        g = 1.0f;
        b = 0.0f;
    } else {
        r = 1.0f;
        g = 4.0f - four_x;
        b = 0.0f;
    }

    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f;
    if (b > 1.0f) b = 1.0f;

    return (0xFFu << 24) |
           ((uint32_t)(b * 255.0f) << 16) |
           ((uint32_t)(g * 255.0f) << 8) |
           ((uint32_t)(r * 255.0f));
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

static bool scan_segment(struct scan_state *s, int64_t center_hz, int bin_offset)
{
    int i;

    if (!retune_rx(center_hz)) {
        fprintf(stderr, "Failed to tune RX LO to %" PRId64 " Hz\n", center_hz);
        return false;
    }
    usleep(RX_SETTLE_US);

    if (!capture_iq_block(s))
        return false;

    for (i = 0; i < s->fft_len; i++) {
        s->in_re[i] *= s->window[i];
        s->in_im[i] *= s->window[i];
    }

    simple_fft_run(&s->fft, s->in_re, s->in_im, s->out_re, s->out_im);

    for (i = 0; i < s->fft_len; i++) {
        int shifted = (i + s->fft_len / 2) % s->fft_len;
        float re = s->out_re[shifted];
        float im = s->out_im[shifted];
        float pwr = 10.0f * log10f(re * re + im * im + 1e-20f);
        s->line_db[bin_offset + i] = pwr;
    }

    return true;
}

static void waterfall_push_line(struct scan_state *s)
{
    int x;
    uint32_t *dst_last;

    if (s->lines > 1) {
        memmove(s->waterfall_rgba,
                s->waterfall_rgba + s->waterfall_width,
                (size_t)(s->lines - 1) * (size_t)s->waterfall_width * sizeof(uint32_t));
    }

    dst_last = s->waterfall_rgba + (size_t)(s->lines - 1) * (size_t)s->waterfall_width;
    for (x = 0; x < s->waterfall_width; x++) {
        float t = (s->line_db[x] - s->db_min) / (s->db_max - s->db_min + 1e-6f);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        dst_last[x] = colormap_jet(t);
    }

    glBindTexture(GL_TEXTURE_2D, s->waterfall_tex);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    s->waterfall_width,
                    s->lines,
                    GL_RGBA,
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

static bool scan_line(struct scan_state *s)
{
    double fs = (double)SCAN_SAMPLERATE_HZ;
    double start = (double)s->scan_start_hz;
    int seg;

    for (seg = 0; seg < s->segments; seg++) {
        double seg_start = start + (double)seg * fs;
        int64_t center_hz = (int64_t)(seg_start + fs * 0.5);
        if (!scan_segment(s, center_hz, seg * s->fft_len))
            return false;
    }

    waterfall_push_line(s);
    return true;
}

static bool resize_buffers(struct scan_state *s)
{
    double range_hz;
    int new_segments;
    int new_width;

    simple_fft_destroy(&s->fft);

    free(s->in_re);
    free(s->in_im);
    free(s->out_re);
    free(s->out_im);
    free(s->window);
    free(s->line_db);
    free(s->plot_db);
    free(s->waterfall_rgba);

    s->in_re = NULL;
    s->in_im = NULL;
    s->out_re = NULL;
    s->out_im = NULL;
    s->window = NULL;
    s->line_db = NULL;
    s->plot_db = NULL;
    s->waterfall_rgba = NULL;

    if (s->scan_stop_hz <= s->scan_start_hz)
        s->scan_stop_hz = s->scan_start_hz + SCAN_SAMPLERATE_HZ;

    range_hz = (double)(s->scan_stop_hz - s->scan_start_hz);
    new_segments = (int)ceil(range_hz / (double)SCAN_SAMPLERATE_HZ);
    if (new_segments < 1)
        new_segments = 1;

    new_width = new_segments * s->fft_len;
    if (new_width <= 0 || new_width > MAX_WATERFALL_WIDTH) {
        fprintf(stderr,
            "Waterfall width %d is invalid (range too wide or FFT too large).\n",
            new_width);
        return false;
    }

    s->in_re = (float *)calloc((size_t)s->fft_len, sizeof(float));
    s->in_im = (float *)calloc((size_t)s->fft_len, sizeof(float));
    s->out_re = (float *)calloc((size_t)s->fft_len, sizeof(float));
    s->out_im = (float *)calloc((size_t)s->fft_len, sizeof(float));
    s->window = (float *)calloc((size_t)s->fft_len, sizeof(float));
    s->line_db = (float *)calloc((size_t)new_width, sizeof(float));
    s->plot_db = (float *)calloc((size_t)MAX_PLOT_POINTS, sizeof(float));
    s->waterfall_rgba = (uint32_t *)calloc((size_t)new_width * (size_t)s->lines, sizeof(uint32_t));

    if (!s->in_re || !s->in_im || !s->out_re || !s->out_im ||
        !s->window || !s->line_db || !s->plot_db || !s->waterfall_rgba) {
        fprintf(stderr, "Out of memory allocating scan buffers.\n");
        return false;
    }

    if (simple_fft_init(&s->fft, s->fft_len) < 0) {
        fprintf(stderr, "Failed to initialize FFT plan for length %d.\n", s->fft_len);
        return false;
    }

    make_window(s->window, s->fft_len);

    s->segments = new_segments;
    s->waterfall_width = new_width;

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
                 s->waterfall_width,
                 s->lines,
                 0,
                 GL_RGBA,
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

    ad9361_set_tx_sampling_freq(ad9361_phy, SCAN_SAMPLERATE_HZ);
    ad9361_set_rx_sampling_freq(ad9361_phy, SCAN_SAMPLERATE_HZ);
    ad9361_set_rx_rf_bandwidth(ad9361_phy, SCAN_BANDWIDTH_HZ);
    ad9361_set_tx_rf_bandwidth(ad9361_phy, SCAN_BANDWIDTH_HZ);

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
           "  -fft_len n             FFT length (power of two, default: %d).\n"
           "  -lines n               Waterfall lines (default: %d).\n"
           "\n"
           "Runtime controls in UI:\n"
           "  - Scan start/stop (MHz), FFT length, line count, RX gain and dB scale.\n"
           "  - Parameters are applied live while moving sliders.\n"
           "\n"
           "Notes:\n"
           "  - Scan sampling rate is fixed to 61.44 MSPS.\n"
           "  - Full-spectrum scans are possible but slower because they retune LO per segment.\n",
           DEFAULT_REFCLK_FREQ,
           (int64_t)DEFAULT_START_FREQ_HZ,
           (int64_t)DEFAULT_STOP_FREQ_HZ,
           DEFAULT_RX_GAIN_DB,
           DEFAULT_FFT_LEN,
           DEFAULT_LINES);
}

static bool is_power_of_two_int(int n)
{
    return (n > 0) && ((n & (n - 1)) == 0);
}

static int ilog2_int(int n)
{
    int e = 0;
    while ((1 << e) < n)
        e++;
    return e;
}

static bool apply_runtime_config(struct scan_state *s,
                                 int64_t start_hz,
                                 int64_t stop_hz,
                                 int fft_len,
                                 int lines,
                                 int rx_gain)
{
    if (!is_power_of_two_int(fft_len) || fft_len < 128 || fft_len > 16384) {
        fprintf(stderr, "Invalid FFT length %d (must be power of two between 128 and 16384).\n", fft_len);
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

    s->scan_start_hz = start_hz;
    s->scan_stop_hz = stop_hz;
    s->fft_len = fft_len;
    s->lines = lines;
    s->rx_gain = rx_gain;

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
    int ui_fft_exp;
    int ui_lines;
    int ui_rx_gain;

    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "refclk_freq", required_argument, NULL, 1 },
        { "start_freq", required_argument, NULL, 2 },
        { "stop_freq", required_argument, NULL, 3 },
        { "rx_gain", required_argument, NULL, 4 },
        { "fft_len", required_argument, NULL, 5 },
        { "lines", required_argument, NULL, 6 },
        { NULL, 0, NULL, 0 }
    };

    memset(&s, 0, sizeof(s));
    s.refclk_hz = DEFAULT_REFCLK_FREQ;
    s.scan_start_hz = DEFAULT_START_FREQ_HZ;
    s.scan_stop_hz = DEFAULT_STOP_FREQ_HZ;
    s.rx_gain = DEFAULT_RX_GAIN_DB;
    s.fft_len = DEFAULT_FFT_LEN;
    s.lines = DEFAULT_LINES;
    s.db_min = DEFAULT_DB_MIN;
    s.db_max = DEFAULT_DB_MAX;
    s.run = true;
    s.display_rows = 1;

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
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
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
    ui_fft_exp = ilog2_int(s.fft_len);
    ui_lines = s.lines;
    ui_rx_gain = s.rx_gain;

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

        igSetNextWindowPos((ImVec2){10, 10}, ImGuiCond_FirstUseEver, (ImVec2){0, 0});
        igSetNextWindowSize((ImVec2){1370, 830}, ImGuiCond_FirstUseEver);

        igBegin("RF Scan", NULL, 0);
        igText("Device: %s", m2sdr_device);
        igText("Samplerate: %.2f MSPS", SCAN_SAMPLERATE_HZ / 1e6);
        igText("Segments: %d | Width: %d bins", s.segments, s.waterfall_width);

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

            changed |= igSliderInt("FFT log2", &ui_fft_exp, 7, 14, "%d", 0);
            igText("FFT Length: %d", 1 << ui_fft_exp);
            changed |= igSliderInt("Lines", &ui_lines, 32, 2048, "%d", 0);
            changed |= igSliderInt("Display Rows", &s.display_rows, 1, 8, "%d", 0);
            changed |= igSliderInt("RX Gain (dB)", &ui_rx_gain, 0, 73, "%d", 0);
            changed |= igSliderFloat("Min dB", &s.db_min, -160.0f, 20.0f, "%.1f", 0);
            changed |= igSliderFloat("Max dB", &s.db_max, -160.0f, 40.0f, "%.1f", 0);

            if (s.db_max <= s.db_min + 1.0f) {
                s.db_max = s.db_min + 1.0f;
                changed = true;
            }

            if (changed) {
                int64_t new_start = (int64_t)(ui_start_mhz * 1e6f);
                int64_t new_stop = (int64_t)(ui_stop_mhz * 1e6f);
                int new_fft_len = 1 << ui_fft_exp;
                if (apply_runtime_config(&s, new_start, new_stop, new_fft_len, ui_lines, ui_rx_gain)) {
                    ui_start_mhz = (float)s.scan_start_hz / 1e6f;
                    ui_stop_mhz = (float)s.scan_stop_hz / 1e6f;
                    ui_fft_exp = ilog2_int(s.fft_len);
                    ui_lines = s.lines;
                    ui_rx_gain = s.rx_gain;
                } else {
                    ui_start_mhz = (float)s.scan_start_hz / 1e6f;
                    ui_stop_mhz = (float)s.scan_stop_hz / 1e6f;
                    ui_fft_exp = ilog2_int(s.fft_len);
                    ui_lines = s.lines;
                    ui_rx_gain = s.rx_gain;
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

                igText("Spectrum Row %d: %.3f MHz -> %.3f MHz", row + 1, f0_hz / 1e6, f1_hz / 1e6);
                snprintf(overlay, sizeof(overlay), "Row %d", row + 1);
                snprintf(plot_id, sizeof(plot_id), "##spectrum_row_%d", row);
                igPlotLines_FloatPtr(plot_id,
                                     s.plot_db,
                                     plot_count,
                                     0,
                                     overlay,
                                     s.db_min,
                                     s.db_max,
                                     (ImVec2){avail.x, spectrum_row_h},
                                     sizeof(float));

                igText("Waterfall Row %d: %.3f MHz -> %.3f MHz", row + 1, f0_hz / 1e6, f1_hz / 1e6);
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
    free(s.plot_db);
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
    free(s.plot_db);
    free(s.waterfall_rgba);

    m2sdr_close_global();
    return 1;
}
