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
#include <signal.h>
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
#endif

#include "m2sdr.h"
#include "m2sdr_analysis.h"
#include "m2sdr_cal.h"
#include "m2sdr_cli.h"
#include "m2sdr_sigmf.h"

#include "analyzer/analyzer.h"

/* GUI */
/*-----*/

#ifdef M2SDR_ANALYZER_GUI

static const char *const trace_names[UI_TRACES] = { "Live", "Average", "Max hold", "Min hold" };
static const ImU32 trace_colors[UI_TRACES] = { 0xff40d0ffu, 0xff60ff60u, 0xff6060ffu, 0xffffa040u };
static const ImU32 marker_colors[UI_MARKERS] = { 0xffffffffu, 0xff80ffffu, 0xffff80ffu, 0xffffff80u };

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

/* A capture and its analysis. The worker captures into one job while the other is analysed in
 * its own thread: a result every max(capture, analysis) instead of their sum. */
struct an_job {
    pthread_t thread;
    bool   busy;
    float complex *x;
    size_t x_count;                /* Room in x. */
    size_t count;
    struct an_settings set;
    struct an_result *res;
    int    capture_rc;             /* > 0: samples lost. */
    char   err[256];
    double capture_ms, analysis_ms;
    int    rc;
};

static void *job_main(void *arg)
{
    struct an_job *j = arg;
    const double t = now_ms();

    j->rc = analyse(j->x, j->count, &j->set, j->res);
    j->res->lost   = j->capture_rc > 0;
    j->analysis_ms = now_ms() - t;
    return NULL;
}

/* The result of an analysed job to the modes, then to the UI. */
static void job_publish(struct an_shared *sh, struct an_job *j)
{
    int m;

    if (j->rc != 0) {
        pthread_mutex_lock(&sh->lock);
        snprintf(sh->status, sizeof(sh->status), "analysis failed");
        sh->run = sh->single = false;
        pthread_mutex_unlock(&sh->lock);
        return;
    }
    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->worker_result)
            an_modes[m]->worker_result(&j->set, j->res);
    }
    pthread_mutex_lock(&sh->lock);
    if (result_copy(&sh->res, j->res) == 0)
        sh->seq++;
    snprintf(sh->status, sizeof(sh->status), "%s", j->capture_rc > 0 ? j->err : "");
    sh->capture_ms  = j->capture_ms;
    sh->analysis_ms = j->analysis_ms;
    pthread_mutex_unlock(&sh->lock);
}

/* Wait for the analysis of a job, if one runs, and publish it. */
static void job_finish(struct an_shared *sh, struct an_job *j)
{
    if (!j->busy)
        return;
    pthread_join(j->thread, NULL);
    j->busy = false;
    job_publish(sh, j);
}

static void job_free(struct an_job *j)
{
    if (j->res) {
        m2sdr_psd_free(&j->res->psd);
        m2sdr_psd_free(&j->res->tone_psd);
    }
    free(j->res);
    free(j->x);
}

static void *worker_main(void *arg)
{
    struct an_shared *sh = arg;
    struct an_job jobs[2];
    double last = 0;
    int cur = 0, m;

    memset(jobs, 0, sizeof(jobs));
    jobs[0].res = calloc(1, sizeof(*jobs[0].res));
    jobs[1].res = calloc(1, sizeof(*jobs[1].res));
    while (jobs[0].res && jobs[1].res) {
        struct an_job *j = &jobs[cur];
        struct an_settings set;
        double t0;
        bool run, quit;

        pthread_mutex_lock(&sh->lock);
        set  = sh->set;
        run  = sh->run || sh->single;
        quit = sh->quit;
        /* A single capture is taken once; its result arrives when it is analysed. */
        sh->single = false;
        pthread_mutex_unlock(&sh->lock);
        if (quit || !run) {
            job_finish(sh, &jobs[cur ^ 1]);
            if (quit)
                break;
            usleep(20000);
            continue;
        }

        /* What the modes know from the results before (DVB-T: a known multiplex). */
        for (m = 0; an_modes[m]; m++) {
            if (an_modes[m]->worker_prepare)
                an_modes[m]->worker_prepare(&set, sh->src->live);
        }
        j->set   = set;
        j->count = capture_samples(&set);
        if (j->count > j->x_count) {
            float complex *grown = realloc(j->x, j->count * sizeof(*j->x));

            if (!grown) {
                job_finish(sh, &jobs[cur ^ 1]);
                pthread_mutex_lock(&sh->lock);
                snprintf(sh->status, sizeof(sh->status), "out of memory for %zu samples", j->count);
                sh->run = false;
                pthread_mutex_unlock(&sh->lock);
                continue;
            }
            j->x       = grown;
            j->x_count = j->count;
        }

        /* A file is read faster than it can be looked at. */
        if (now_ms() - last < AN_MIN_PERIOD_MS)
            usleep((useconds_t)(1e3 * (AN_MIN_PERIOD_MS - (now_ms() - last))));
        last = now_ms();

        t0 = now_ms();
        j->capture_rc = source_capture(sh->src, &set, j->x, j->count, j->err, sizeof(j->err));
        j->capture_ms = now_ms() - t0;
        /* The analysis of the capture before, then this one in the background. */
        job_finish(sh, &jobs[cur ^ 1]);
        if (j->capture_rc < 0) {
            pthread_mutex_lock(&sh->lock);
            snprintf(sh->status, sizeof(sh->status), "%s", j->err);
            sh->run = false;
            pthread_mutex_unlock(&sh->lock);
            continue;
        }
        if (pthread_create(&j->thread, NULL, job_main, j) != 0) {
            job_main(j);           /* No thread: analysed here, in turn. */
            job_publish(sh, j);
            continue;
        }
        j->busy = true;
        cur ^= 1;
    }
    job_finish(sh, &jobs[0]);
    job_finish(sh, &jobs[1]);
    job_free(&jobs[0]);
    job_free(&jobs[1]);
    return NULL;
}

/* Traces */

void traces_reset(struct an_ui *ui)
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

uint32_t palette_color(int palette, float t)
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

void label_value(const char *label, const char *fmt, ...)
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
bool input_scaled(const char *label, double *value, double scale, const char *fmt)
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

/* The next control on the same line when `width` pixels are left on it, on a new line if not. */
void same_line_or_wrap(float spacing, float width)
{
    ImVec2 avail;

    igSameLine(0.0f, spacing);
    igGetContentRegionAvail(&avail);
    if (avail.x < width)
        igNewLine();
}

static bool input_mhz(const char *label, double *hz)
{
    return input_scaled(label, hz, 1e6, "%.6f");
}

static bool input_khz(const char *label, double *hz)
{
    return input_scaled(label, hz, 1e3, "%.3f");
}

void view_full(struct an_ui *ui)
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

static void draw_spectrum(struct an_ui *ui, bool live, bool *changed)
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
    (void)live;
    (void)changed;
}

static void draw_time(struct an_ui *ui, bool live, bool *changed)
{
    static float re[AN_TIME_POINTS], im[AN_TIME_POINTS], mag[AN_TIME_POINTS];
    static int points = 1024;
    const size_t n = ui->res.time_count < (size_t)points ? ui->res.time_count : (size_t)points;
    const double dt = ui->res.set.sample_rate > 0 ? 1.0 / ui->res.set.sample_rate : 1.0;
    struct m2sdr_plot plot;
    ImVec2 avail;
    float peak = 1e-6f;
    size_t i;

    (void)live;
    (void)changed;
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

static void draw_statistics(struct an_ui *ui, bool live, bool *changed)
{
    static float x[M2SDR_CCDF_POINTS], y[M2SDR_CCDF_POINTS], g[M2SDR_CCDF_POINTS];
    static float cx[AN_TIME_POINTS], cy[AN_TIME_POINTS];
    struct m2sdr_plot plot;
    ImVec2 avail, sq;
    float side, peak = 1e-6f;
    size_t i;
    int k;

    (void)live;
    (void)changed;
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

/* Entry of a legend: a square of the colour and its name, on the line while it fits before `right`. */
void legend_item(ImU32 color, const char *name, float right)
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

void view_retune(struct an_ui *ui, double freq_hz, bool *changed)
{
    ui->set.freq_hz = freq_hz;
    traces_reset(ui);
    *changed = true;
}

void view_rate(struct an_ui *ui, double rate, bool *changed)
{
    ui->set.sample_rate = rate;
    ui->set.bandwidth   = 0;
    view_full(ui);
    traces_reset(ui);
    *changed = true;
}

static void draw_results(struct an_ui *ui, struct an_shared *sh, bool *changed)
{
    const struct an_result *res = &ui->res;
    const double off = res->set.ref_offset_db;
    const char *unit = off != 0.0 ? "dBm" : "dBFS";
    const float *trace = marker_trace(ui);
    char text[64];
    int m, i;

    /* The figures of the mode shown, or the ones of the spectrum. */
    if (an_modes[ui->tab]->results) {
        an_modes[ui->tab]->results(ui);
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
        if (ui->message[0]) {
            igPushTextWrapPos(0.0f);
            igTextUnformatted(ui->message, NULL);
            igPopTextWrapPos();
        }
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
    int t, m;

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
            for (m = 0; an_modes[m]; m++) {
                if (an_modes[m]->keep)
                    an_modes[m]->keep(&ui);
            }
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

                    for (m = 0; an_modes[m]; m++) {
                        if (igBeginTabItem(an_modes[m]->title, NULL, select == m ? ImGuiTabItemFlags_SetSelected : 0)) {
                            tab = m;
                            an_modes[m]->draw(&ui, src->live && !src->no_rf_init, &changed);
                            igEndTabItem();
                        }
                    }
                    /* A mode is measured while its view is shown: some need their own
                     * captures. */
                    for (m = 0; select < 0 && an_modes[m]; m++) {
                        bool *on = an_mode_flag(an_modes[m], &ui.set);

                        if (on && *on != (tab == m)) {
                            *on     = tab == m;
                            changed = true;
                        }
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
    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->close)
            an_modes[m]->close(&ui);
    }
    free(ui.res.psd.power);
    free(sh.res.psd.power);
    pthread_mutex_destroy(&sh.lock);
    return 0;
}

#endif /* M2SDR_ANALYZER_GUI */

/* Modes */
/*-------*/

/* Views of the spectrum: measured with every capture. */
const struct an_mode an_mode_spectrum = {
    .key      = "spectrum",
    .title    = "Spectrum",
    .category = AN_CAT_SPECTRUM,
    .on       = AN_NONE,
    .valid    = AN_NONE,
#ifdef M2SDR_ANALYZER_GUI
    .draw     = draw_spectrum,
#endif
};

const struct an_mode an_mode_time = {
    .key      = "time",
    .title    = "Time",
    .category = AN_CAT_SPECTRUM,
    .on       = AN_NONE,
    .valid    = AN_NONE,
#ifdef M2SDR_ANALYZER_GUI
    .draw     = draw_time,
#endif
};

const struct an_mode an_mode_statistics = {
    .key      = "statistics",
    .title    = "Statistics",
    .category = AN_CAT_SPECTRUM,
    .on       = AN_NONE,
    .valid    = AN_NONE,
#ifdef M2SDR_ANALYZER_GUI
    .draw     = draw_statistics,
#endif
};

/* Registry: the tabs, in their order. */
const struct an_mode *const an_modes[] = {
    &an_mode_spectrum,
    &an_mode_time,
    &an_mode_statistics,
    &an_mode_pn,
    &an_mode_demod,
    &an_mode_lte,
    &an_mode_dvbt,
    &an_mode_dab,
    NULL
};

/* Help */
/*------*/

#define AN_HELP_COLUMNS 94         /* The longest line of the help. */

/* The names of the views, as "a, b or c". */
static char *view_names(char *text, size_t len)
{
    size_t n = 0;
    int m;

    text[0] = '\0';
    for (m = 0; an_modes[m] && n < len; m++)
        n += (size_t)snprintf(text + n, len - n, "%s%s", m == 0 ? "" : an_modes[m + 1] ? ", " : " or ", an_modes[m]->key);
    return text;
}

/* --view: the names of the views, on lines as long as the other ones of the help. */
static void help_views(void)
{
    char names[512], *word;
    int col;

    view_names(names, sizeof(names) - 1);
    strcat(names, ".");
    col = printf("      --view NAME        View shown first:");
    for (word = strtok(names, " "); word; word = strtok(NULL, " ")) {
        if (col + 1 + (int)strlen(word) > AN_HELP_COLUMNS)
            col = printf("\n%25s%s", "", word) - 1;
        else
            col += printf(" %s", word);
    }
    printf("\n");
}

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
           "DVB-T options:\n"
           "      --dvbt             With --report: reception of a DVB-T multiplex tuned near its centre:\n"
           "                         mode, TPS, MER, channel and echoes. The sample rate is the one of\n"
           "                         the channel (default with --dvbt: 9.142857 MSPS, 8 MHz).\n"
           "      --watch            With --report --dvbt or --dab, on a board: one line per capture until\n"
           "                         Ctrl-C (MER, margin, level, offset, echo); with --json, one JSON\n"
           "                         object per line.\n"
           "\n"
           "DAB options:\n"
           "      --dab              With --report: reception of a DAB/DAB+ ensemble (mode I) tuned near\n"
           "                         its centre: MER, SNR, carrier offset, channel and echoes. The sample\n"
           "                         rate is 2.048 MSPS (the default with --dab). --watch works too.\n"
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
           "Output options:\n",
           AN_DEFAULT_FREQ_HZ, AN_DEFAULT_SAMPLE_RATE, M2SDR_BUFFER_BYTES, AN_DEFAULT_FFT_LEN, AN_DEFAULT_AVERAGES,
           1 << AN_DEMOD_DEFAULT_LOG2, 1 << AN_PN_DEFAULT_LOG2, AN_TX_TONE_ATT_DB);
    help_views();
    printf("      --report           Measure one capture and print the result, without a display.\n"
           "      --json             With --report: JSON.\n"
           "      --export-csv PATH  With --report: write the spectrum to a CSV file.\n"
           "  -h, --help             Show this help message.\n"
           "\n"
           "Window: left click places the selected marker, the wheel zooms, a right drag moves the\n"
           "span, a double click shows it all. Space: run or stop. P: marker to the peak. F: full\n"
           "span. R: restart the traces.\n");
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
        { "dvbt",            no_argument,       NULL, 44 },
        { "watch",           no_argument,       NULL, 45 },
        { "dab",             no_argument,       NULL, 46 },
        { NULL, 0, NULL, 0 }
    };
    struct m2sdr_cli_device cli_dev;
    struct an_settings set;
    struct an_source src;
    const char *file = NULL, *csv_path = NULL, *trim = NULL, *floor_arg = NULL, *floor_path = NULL;
    static struct an_floor fl;
    static char floor_default[1024];
    bool floor_store = false;
    bool report = false, json = false, watch = false, freq_given = false, rate_given = false, nbits_given = false;
    double offset_s = 0.0;
    int capture_index = 0, view = 0;
    char err[256];
    int c, m, status;

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
            for (view = 0; an_modes[view] && strcmp(optarg, an_modes[view]->key) != 0; view++)
                ;
            if (!an_modes[view]) {
                char names[512];

                m2sdr_cli_invalid_choice("view", optarg, view_names(names, sizeof(names)));
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
        case 44: set.dvbt_on = true; break;
        case 45: watch = true; break;
        case 46: set.dab_on = true; break;
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
    if (watch && (!report || watch_modes(&set, NULL) != 1 || file || csv_path || floor_store)) {
        m2sdr_cli_error("--watch goes with --report and one of --dvbt and --dab, on a board");
        return 1;
    }
    if ((json || csv_path || floor_store) && !report) {
        m2sdr_cli_error("--json, --export-csv and --pn-floor-save go with --report");
        return 1;
    }
    if (floor_store)
        set.pn_on = true;
    /* The mode of the view shown first is measured from the first capture. */
    if (an_mode_flag(an_modes[view], &set))
        *an_mode_flag(an_modes[view], &set) = true;
    /* The sample rate of a mode, unless one is given; a file gives its own. */
    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->default_rate > 0 && an_mode_on(an_modes[m], &set) && !rate_given)
            set.sample_rate = an_modes[m]->default_rate;
    }
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
    if (watch) {
        status = run_watch(&src, &set, json);
    } else if (report) {
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
