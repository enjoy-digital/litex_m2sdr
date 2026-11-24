/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: pulsed signals (radar, DFS test patterns, pulsed interferers):
 * the pulses, their train (PRI, stagger, duty cycle), in a report, a view and --watch.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_cli.h"

#include "analyzer.h"

#define PULSE_REPORT_LIST  10      /* Pulses listed in the text report. */
#define PULSE_JSON_LIST    32      /* And in the JSON one. */

static const char *const pulse_mod_names[] = { "CW", "LFM", "other" };
static const char *const pulse_mod_keys[]  = { "cw", "lfm", "other" };
static const char *const pri_keys[]        = { "none", "constant", "stagger", "jitter" };

/* Analysis */
/*----------*/

static double pulse_seconds(const struct an_settings *set)
{
    return set->pulse_seconds > 0 ? set->pulse_seconds : AN_PULSE_SECONDS;
}

static size_t pulse_samples(const struct an_settings *set)
{
    return (size_t)(pulse_seconds(set) * set->sample_rate);
}

static void pulse_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t want = pulse_samples(set);
    const size_t n = count < want ? count : want;
    struct m2sdr_pulse_config cfg;
    size_t i, p;

    memset(&cfg, 0, sizeof(cfg));
    cfg.threshold_db = set->pulse_threshold_db;
    res->pulse_valid = m2sdr_pulse_analyse(x, n, set->sample_rate, &cfg, &res->pulse) == 0;
    /* Envelope for the view: the highest power of each point, so that no pulse is lost. */
    res->pulse_env_s = (double)n / set->sample_rate;
    for (p = 0; p < AN_PULSE_ENV_POINTS; p++) {
        const size_t a = p * n / AN_PULSE_ENV_POINTS, b = (p + 1) * n / AN_PULSE_ENV_POINTS;
        float peak = 0.0f;

        for (i = a; i < b; i++) {
            const float v = crealf(x[i] * conjf(x[i]));

            if (v > peak)
                peak = v;
        }
        res->pulse_env_db[p] = 10.0f * log10f(peak + 1e-15f);
    }
}

/* Reports */
/*---------*/

static void pulse_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_pulse_result *p = &res->pulse;
    const double off = rep->off;
    const char *unit = rep->unit;
    int i, k;

    printf("\nPulses:\n");
    if (!res->pulse_valid) {
        printf("  The capture is too short (1024 samples at least).\n");
        return;
    }
    if (!p->found) {
        printf("  No pulses %.1f dB over the noise floor (%.1f %s).\n", p->threshold_dbfs - p->noise_dbfs,
               p->noise_dbfs + off, unit);
        return;
    }
    printf("  Pulses     : %d in %.1f ms (%d listed), noise floor %.1f %s, threshold %.1f %s\n", p->npulses,
           1e3 * res->pulse_env_s, p->nlisted, p->noise_dbfs + off, unit, p->threshold_dbfs + off, unit);
    printf("  Width      : %.3f us, rise %.3f us, fall %.3f us\n", 1e6 * p->width_s, 1e6 * p->rise_s, 1e6 * p->fall_s);
    printf("  Peak       : %.1f %s\n", p->peak_dbfs + off, unit);
    printf("  Frequency  : %.6f MHz (%+.1f kHz from the centre), %s", (set->freq_hz + p->freq_hz) / 1e6, p->freq_hz / 1e3,
           pulse_mod_names[p->modulation]);
    if (p->modulation == M2SDR_PULSE_LFM)
        printf(", chirp %+.3f MHz/ms (%.3f MHz over the width)", p->chirp_hz_per_s / 1e9,
               fabs(p->chirp_hz_per_s) * p->width_s / 1e6);
    printf("\n");
    switch (p->pri_kind) {
    case M2SDR_PRI_NONE:
        printf("  PRI        : fewer than three whole pulses\n");
        break;
    case M2SDR_PRI_CONSTANT:
        printf("  PRI        : constant %.3f us, PRF %.2f Hz, duty cycle %.3f %%\n", 1e6 * p->pri_s[0], p->prf_hz,
               100.0 * p->duty_cycle);
        break;
    case M2SDR_PRI_STAGGER:
        printf("  PRI        : stagger of %d:", p->stagger);
        for (k = 0; k < p->stagger; k++)
            printf("%s %.3f", k ? " /" : "", 1e6 * p->pri_s[k]);
        printf(" us, mean %.3f us, PRF %.2f Hz, duty cycle %.3f %%\n", 1e6 * p->pri_mean_s, p->prf_hz, 100.0 * p->duty_cycle);
        break;
    default:
        printf("  PRI        : no repeating pattern: mean %.3f us (%.3f to %.3f, std %.3f), PRF %.2f Hz\n",
               1e6 * p->pri_mean_s, 1e6 * p->pri_min_s, 1e6 * p->pri_max_s, 1e6 * p->pri_std_s, p->prf_hz);
        break;
    }
    printf("  %4s %12s %10s %8s %8s %8s %8s %10s %10s %6s\n", "#", "start us", "width us", "peak", "mean", "rise us",
           "fall us", "offset kHz", "MHz/ms", "mod");
    for (i = 0; i < p->nlisted && i < PULSE_REPORT_LIST; i++) {
        const struct m2sdr_pulse_info *q = &p->pulses[i];

        printf("  %4d %12.3f %10.3f %8.1f %8.1f %8.3f %8.3f %+10.1f %+10.3f %6s%s\n", i + 1, 1e6 * q->start_s, 1e6 * q->width_s,
               q->peak_dbfs + off, q->mean_dbfs + off, 1e6 * q->rise_s, 1e6 * q->fall_s, q->freq_hz / 1e3,
               q->chirp_hz_per_s / 1e9, pulse_mod_keys[q->modulation], q->truncated ? " cut" : "");
    }
}

static void pulse_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_pulse_result *p = &res->pulse;
    const double off = rep->off;
    const bool ok = res->pulse_valid && p->found;
    int i;

    printf("  \"pulse\": {\n");
    print_json_number("noise_db", res->pulse_valid ? p->noise_dbfs + off : NAN, false);
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (!ok) {
        printf("  }");
        return;
    }
    printf("    \"pulses\": %d,\n", p->npulses);
    print_json_number("threshold_db", p->threshold_dbfs + off, false);
    print_json_number("width_us", 1e6 * p->width_s, false);
    print_json_number("rise_us", 1e6 * p->rise_s, false);
    print_json_number("fall_us", 1e6 * p->fall_s, false);
    print_json_number("peak_db", p->peak_dbfs + off, false);
    print_json_number("freq_hz", set->freq_hz + p->freq_hz, false);
    print_json_number("offset_hz", p->freq_hz, false);
    print_json_number("chirp_hz_per_s", p->chirp_hz_per_s, false);
    printf("    \"modulation\": \"%s\",\n", pulse_mod_keys[p->modulation]);
    printf("    \"pri\": \"%s\",\n", pri_keys[p->pri_kind]);
    printf("    \"stagger\": %d,\n", p->stagger);
    printf("    \"pri_us\": [");
    for (i = 0; i < p->stagger; i++)
        printf("%s%.4f", i ? ", " : "", 1e6 * p->pri_s[i]);
    printf("],\n");
    print_json_number("pri_mean_us", p->pri_kind != M2SDR_PRI_NONE ? 1e6 * p->pri_mean_s : NAN, false);
    print_json_number("prf_hz", p->pri_kind != M2SDR_PRI_NONE ? p->prf_hz : NAN, false);
    print_json_number("duty_cycle", p->pri_kind != M2SDR_PRI_NONE ? p->duty_cycle : NAN, false);
    printf("    \"list\": [");
    for (i = 0; i < p->nlisted && i < PULSE_JSON_LIST; i++) {
        const struct m2sdr_pulse_info *q = &p->pulses[i];

        printf("%s\n      {\"start_us\": %.4f, \"width_us\": %.4f, \"peak_db\": %.2f, \"offset_hz\": %.1f, "
               "\"chirp_hz_per_s\": %.4g, \"modulation\": \"%s\", \"truncated\": %s}", i ? "," : "", 1e6 * q->start_s,
               1e6 * q->width_s, q->peak_dbfs + off, q->freq_hz, q->chirp_hz_per_s, pulse_mod_keys[q->modulation],
               q->truncated ? "true" : "false");
    }
    printf("\n    ]\n");
    printf("  }");
}

/* Command line */
/*--------------*/

/* --pulses: --pulse is the transmit pulse of the demodulation. */
enum { PULSE_OPT_ON, PULSE_OPT_THRESHOLD, PULSE_OPT_SECONDS };

static const struct option pulse_options[] = {
    { "pulses",          no_argument,       NULL, PULSE_OPT_ON },
    { "pulse-threshold", required_argument, NULL, PULSE_OPT_THRESHOLD },
    { "pulse-seconds",   required_argument, NULL, PULSE_OPT_SECONDS },
    { NULL, 0, NULL, 0 }
};

static int pulse_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_settings *set = cli->set;

    switch (id) {
    case PULSE_OPT_ON: set->pulse_on = true; break;
    case PULSE_OPT_THRESHOLD:
        if (m2sdr_cli_parse_double(arg, &set->pulse_threshold_db) != 0 || set->pulse_threshold_db <= 0) {
            m2sdr_cli_error("invalid pulse threshold '%s' (dB, over 0)", arg);
            return -1;
        }
        break;
    case PULSE_OPT_SECONDS:
        if (m2sdr_cli_parse_double(arg, &set->pulse_seconds) != 0 || set->pulse_seconds <= 0 || set->pulse_seconds > 2.0) {
            m2sdr_cli_error("invalid pulse capture '%s' (seconds, up to 2)", arg);
            return -1;
        }
        break;
    default:
        return -1;
    }
    return 0;
}

static void pulse_help(void)
{
    printf("Pulse options:\n"
           "      --pulses           With --report: pulsed signals (radar, DFS patterns): each pulse\n"
           "                         (width, edges, power, frequency, chirp) and the train (PRI,\n"
           "                         stagger, PRF, duty cycle). --watch works too.\n"
           "      --pulse-threshold DB  Detection over the noise floor (default: 10).\n"
           "      --pulse-seconds S  Capture (default: %.2f).\n"
           "\n", AN_PULSE_SECONDS);
}

/* Watch */
/*-------*/

static unsigned pulse_watch_lines;

static int pulse_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_pulse_result *p = &res->pulse;
    const bool ok = res->pulse_valid && p->found;
    const bool pri = ok && p->pri_kind != M2SDR_PRI_NONE;

    if (!res->pulse_valid) {
        fprintf(stderr, "The capture of the pulses is too short.\n");
        return -1;
    }
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, ok ? "true" : "false");
        if (ok) {
            printf(", \"pulses\": %d, \"width_us\": %.4f, \"peak_db\": %.2f, \"offset_hz\": %.1f, \"modulation\": \"%s\", "
                   "\"pri\": \"%s\"", p->npulses, 1e6 * p->width_s, p->peak_dbfs + set->ref_offset_db, p->freq_hz,
                   pulse_mod_keys[p->modulation], pri_keys[p->pri_kind]);
            if (pri)
                printf(", \"pri_mean_us\": %.4f, \"prf_hz\": %.3f, \"stagger\": %d", 1e6 * p->pri_mean_s, p->prf_hz, p->stagger);
        }
        printf("}\n");
        return 0;
    }
    if (pulse_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
        printf("%8s %6s %10s %8s %10s %12s %10s %6s %8s\n%8s %6s %10s %8s %10s %12s %10s %6s %8s\n",
               "time", "pulses", "width", "peak", "offset", "PRI", "PRF", "mod", "train",
               "s", "", "us", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "kHz", "us", "Hz", "", "");
    if (!ok) {
        printf("%8.2f  no pulses (floor %.1f dBFS)\n", now, p->noise_dbfs);
        return 0;
    }
    printf("%8.2f %6d %10.3f %8.1f %+10.1f", now, p->npulses, 1e6 * p->width_s, p->peak_dbfs + set->ref_offset_db,
           p->freq_hz / 1e3);
    if (pri)
        printf(" %12.3f %10.2f", 1e6 * p->pri_mean_s, p->prf_hz);
    else
        printf(" %12s %10s", "-", "-");
    printf(" %6s %8s", pulse_mod_keys[p->modulation], pri_keys[p->pri_kind]);
    if (p->pri_kind == M2SDR_PRI_STAGGER)
        printf(" %d", p->stagger);
    printf("\n");
    return 0;
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

#define PRI_HIST_BINS 64

static void draw_pulse_envelope(const struct an_result *res, float height)
{
    const struct m2sdr_pulse_result *p = &res->pulse;
    struct m2sdr_plot plot;
    double top = -200.0, bottom;
    char text[160];
    int i;

    for (i = 0; i < AN_PULSE_ENV_POINTS; i++)
        if (res->pulse_env_db[i] > top)
            top = res->pulse_env_db[i];
    bottom = 10.0 * floor((p->noise_dbfs - 10.0) / 10.0);
    top    = 10.0 * ceil((top + 5.0) / 10.0);
    if (top < bottom + 20.0)
        top = bottom + 20.0;
    if (!m2sdr_plot_begin(&plot, "##pulseenv", (ImVec2){ 0.0f, height }, 0.0, res->pulse_env_s, M2SDR_PLOT_TIME,
                          bottom, top, "Peak power of the capture, dBFS, with the pulses found"))
        return;
    for (i = 0; i < p->nlisted; i++) {
        const struct m2sdr_pulse_info *q = &p->pulses[i];
        /* Narrow pulses get a pixel at least. */
        const double px = res->pulse_env_s / (double)(plot.p1.x - plot.p0.x);

        m2sdr_plot_band(&plot, q->start_s, q->start_s + fmax(q->width_s, px), q->truncated ? 0x30ff8080u : 0x3040ff40u);
    }
    m2sdr_plot_trace(&plot, res->pulse_env_db, AN_PULSE_ENV_POINTS, 0.0, res->pulse_env_s, 0xff40d0ffu);
    m2sdr_plot_hline(&plot, p->threshold_dbfs, 0xc04040ffu);
    m2sdr_plot_hline(&plot, p->noise_dbfs, 0xc0a0a0a0u);
    snprintf(text, sizeof(text), "green: pulses (red: cut by the capture), red line: threshold, grey: noise floor");
    m2sdr_plot_text(&plot, 8.0f, -20.0f, text, 0xffc8c8c8u);
    if (plot.hovered) {
        m2sdr_plot_format(text, sizeof(text), plot.mouse_x, res->pulse_env_s / 1e4, M2SDR_PLOT_TIME);
        m2sdr_plot_text(&plot, -160.0f, 4.0f, text, 0xffffffffu);
        m2sdr_plot_vline(&plot, plot.mouse_x, 0x40ffffffu);
    }
    m2sdr_plot_end(&plot);
}

/* Histogram of the intervals between the leading edges of the whole pulses, the PRIs of the
 * pattern marked. */
static void draw_pri_histogram(const struct m2sdr_pulse_result *p, ImVec2 size)
{
    double d[M2SDR_PULSE_MAX_PULSES], lo = 1e30, hi = 0, last = -1;
    float count[PRI_HIST_BINS];
    struct m2sdr_plot plot;
    float peak = 1.0f;
    char text[64];
    int i, n = 0;

    for (i = 0; i < p->nlisted; i++) {
        if (p->pulses[i].truncated)
            continue;
        if (last >= 0) {
            d[n] = p->pulses[i].start_s - last;
            if (d[n] < lo)
                lo = d[n];
            if (d[n] > hi)
                hi = d[n];
            n++;
        }
        last = p->pulses[i].start_s;
    }
    if (n == 0) {
        igTextUnformatted("PRI: fewer than two whole pulses.", NULL);
        return;
    }
    /* Range: the intervals and a margin, at least 10 % of their mean wide. */
    {
        const double mid = 0.5 * (lo + hi), half = fmax(0.6 * (hi - lo), 0.05 * mid);

        lo = mid - half;
        hi = mid + half;
    }
    memset(count, 0, sizeof(count));
    for (i = 0; i < n; i++) {
        int b = (int)((d[i] - lo) / (hi - lo) * PRI_HIST_BINS);

        b = b < 0 ? 0 : b >= PRI_HIST_BINS ? PRI_HIST_BINS - 1 : b;
        count[b] += 1.0f;
        if (count[b] > peak)
            peak = count[b];
    }
    if (!m2sdr_plot_begin(&plot, "##prihist", size, lo, hi, M2SDR_PLOT_TIME, 0.0, peak * 1.15,
                          "Intervals between pulses (PRI), count"))
        return;
    for (i = 0; i < PRI_HIST_BINS; i++) {
        const double xa = lo + (hi - lo) * i / PRI_HIST_BINS, xb = lo + (hi - lo) * (i + 1) / PRI_HIST_BINS;

        if (count[i] > 0)
            ImDrawList_AddRectFilled(plot.dl, (ImVec2){ m2sdr_plot_x(&plot, xa), m2sdr_plot_y(&plot, count[i]) },
                                     (ImVec2){ m2sdr_plot_x(&plot, xb) - 1.0f, m2sdr_plot_y(&plot, 0.0) }, 0xff40d0ffu, 0.0f, 0);
    }
    for (i = 0; i < p->stagger; i++) {
        snprintf(text, sizeof(text), "PRI %d", i + 1);
        m2sdr_plot_marker(&plot, p->pri_s[i], peak * 1.05, p->stagger > 1 ? text : "PRI", 0xffc0c0ffu);
    }
    m2sdr_plot_end(&plot);
}

static void draw_pulse_table(const struct m2sdr_pulse_result *p, ImVec2 size)
{
    static const char *const heads[] = { "#", "start us", "width us", "peak dBFS", "mean dBFS", "rise us", "fall us",
                                         "offset kHz", "MHz/ms", "mod" };
    int i, c;

    if (!igBeginTable("##pulses", 10, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                      ImGuiTableFlags_SizingFixedFit, size, 0.0f))
        return;
    igTableSetupScrollFreeze(0, 1);
    for (c = 0; c < 10; c++)
        igTableSetupColumn(heads[c], 0, 0.0f, 0);
    igTableHeadersRow();
    for (i = 0; i < p->nlisted; i++) {
        const struct m2sdr_pulse_info *q = &p->pulses[i];

        igTableNextRow(0, 0.0f);
        igTableNextColumn();
        igText("%d%s", i + 1, q->truncated ? " cut" : "");
        igTableNextColumn();
        igText("%.3f", 1e6 * q->start_s);
        igTableNextColumn();
        igText("%.3f", 1e6 * q->width_s);
        igTableNextColumn();
        igText("%.1f", q->peak_dbfs);
        igTableNextColumn();
        igText("%.1f", q->mean_dbfs);
        igTableNextColumn();
        igText("%.3f", 1e6 * q->rise_s);
        igTableNextColumn();
        igText("%.3f", 1e6 * q->fall_s);
        igTableNextColumn();
        igText("%+.1f", q->freq_hz / 1e3);
        igTableNextColumn();
        igText("%+.3f", q->chirp_hz_per_s / 1e9);
        igTableNextColumn();
        igTextUnformatted(pulse_mod_names[q->modulation], NULL);
    }
    igEndTable();
}

static void draw_pulse(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_pulse_result *p = &res->pulse;
    double ms = 1e3 * pulse_seconds(&ui->set);
    ImVec2 avail;

    (void)live;
    if (input_scaled("Threshold (dB)", &ui->set.pulse_threshold_db, 1.0, "%.1f"))
        *changed = true;
    same_line_or_wrap(16.0f, 230.0f);
    if (input_scaled("Capture (ms)", &ms, 1.0, "%.1f") && ms > 0) {
        ui->set.pulse_seconds = ms / 1e3;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 300.0f);
    igTextDisabled("threshold 0: 10 dB over the floor");

    if (!res->set.pulse_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->pulse_valid) {
        igTextUnformatted("The capture is too short.", NULL);
        return;
    }
    igGetContentRegionAvail(&avail);
    draw_pulse_envelope(res, 0.42f * avail.y);
    if (!p->found) {
        igText("No pulses %.1f dB over the noise floor (%.1f dBFS).", p->threshold_dbfs - p->noise_dbfs, p->noise_dbfs);
        return;
    }
    igGetContentRegionAvail(&avail);
    draw_pri_histogram(p, (ImVec2){ 0.38f * avail.x, avail.y });
    igSameLine(0.0f, 8.0f);
    igGetContentRegionAvail(&avail);
    draw_pulse_table(p, avail);
}

static void draw_pulse_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_pulse_result *p = &res->pulse;
    char text[128];
    int n = 0, k;

    igSeparatorText("Pulses");
    if (!res->pulse_valid || !p->found) {
        igTextUnformatted("No pulses.", NULL);
        return;
    }
    if (igBeginTable("##pulse", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Pulses", "%d in %.1f ms", p->npulses, 1e3 * res->pulse_env_s);
        label_value("Width", "%.3f us", 1e6 * p->width_s);
        label_value("Rise, fall", "%.3f, %.3f us", 1e6 * p->rise_s, 1e6 * p->fall_s);
        label_value("Peak", "%.1f dBFS", p->peak_dbfs);
        label_value("Noise floor", "%.1f dBFS", p->noise_dbfs);
        label_value("Frequency", "%.6f MHz", (res->set.freq_hz + p->freq_hz) / 1e6);
        label_value("Offset", "%+.1f kHz", p->freq_hz / 1e3);
        label_value("Modulation", "%s", pulse_mod_names[p->modulation]);
        if (p->modulation == M2SDR_PULSE_LFM)
            label_value("Chirp", "%+.3f MHz/ms", p->chirp_hz_per_s / 1e9);
        switch (p->pri_kind) {
        case M2SDR_PRI_CONSTANT:
            label_value("PRI", "constant %.3f us", 1e6 * p->pri_s[0]);
            break;
        case M2SDR_PRI_STAGGER:
            for (k = 0; k < p->stagger && n < (int)sizeof(text) - 16; k++)
                n += snprintf(text + n, sizeof(text) - (size_t)n, "%s%.2f", k ? " / " : "", 1e6 * p->pri_s[k]);
            label_value("PRI, stagger", "%s us", text);
            break;
        case M2SDR_PRI_JITTER:
            label_value("PRI", "no pattern, %.2f to %.2f us", 1e6 * p->pri_min_s, 1e6 * p->pri_max_s);
            break;
        default:
            label_value("PRI", "fewer than 3 whole pulses");
            break;
        }
        if (p->pri_kind != M2SDR_PRI_NONE) {
            label_value("Mean PRI", "%.3f us", 1e6 * p->pri_mean_s);
            label_value("PRF", "%.2f Hz", p->prf_hz);
            label_value("Duty cycle", "%.3f %%", 100.0 * p->duty_cycle);
        }
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_pulse = {
    .key          = "pulses",
    .title        = "Pulses",
    .category     = AN_CAT_MONITORING,
    .on           = offsetof(struct an_settings, pulse_on),
    .valid        = offsetof(struct an_result, pulse_valid),
    .found        = offsetof(struct an_result, pulse.found),
    .report_rank  = 80,
    .psd_short    = true,
    .samples      = pulse_samples,
    .analyse      = pulse_analyse,
    .report_text  = pulse_report_text,
    .report_json  = pulse_report_json,
    .watch_line   = pulse_watch_line,
    .options      = pulse_options,
    .option       = pulse_option,
    .help         = pulse_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_pulse,
    .results      = draw_pulse_results,
#endif
};
