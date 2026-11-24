/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: coherence, gain, phase and delay between RX1 and RX2 (a
 * self-check of the board, its cables and its antennas), in a report, a view and --watch.
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

#include "m2sdr_cli.h"

#include "analyzer.h"

/* Settings and result */
/*-----------------------*/

extern const struct an_mode an_mode_coherence;

/* Its settings: its block of struct an_settings, read with SET(). */
struct coherence_set {
    bool   coherence_on;           /* Coherence, gain, phase and delay between RX1 and RX2. */
    int    coherence_fft_len;
};

#define SET(set) ((struct coherence_set *)an_mode_settings(&an_mode_coherence, (set)))

/* The result of the mode: its block of struct an_result, read with RES(). */
struct coherence_res {
    bool   coherence_valid;
    struct m2sdr_coherence_result coherence;
};

#define RES(res) ((struct coherence_res *)an_mode_result(&an_mode_coherence, (res)))

/* Command line */
/*--------------*/

static const struct option coherence_options[] = {
    { "coherence",     no_argument,       NULL, 0 },
    { "coherence-fft", required_argument, NULL, 1 },
    { NULL, 0, NULL, 0 }
};

static int coherence_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_settings *set = cli->set;

    switch (id) {
    case 0:
        SET(set)->coherence_on = true;
        break;
    case 1:
        if (m2sdr_cli_parse_int_range(arg, 256, M2SDR_COHERENCE_MAX_BINS, &SET(set)->coherence_fft_len) != 0 ||
            (SET(set)->coherence_fft_len & (SET(set)->coherence_fft_len - 1))) {
            m2sdr_cli_error("invalid coherence FFT length '%s' (expected a power of two from 256 to %d)", arg,
                            M2SDR_COHERENCE_MAX_BINS);
            return -1;
        }
        break;
    }
    return 0;
}

static void coherence_help(void)
{
    printf("Coherence options (RX1 and RX2 together; a file needs two channels):\n"
           "      --coherence        With --report: coherence, gain, phase and delay of RX2 against RX1\n"
           "                         over frequency: the board, cables and antennas on one source.\n"
           "                         The band is the one of --channel-bw when given. --watch works too.\n"
           "      --coherence-fft N  Its FFT length, 256 to 8192 (default: %d).\n"
           "\n", AN_COHERENCE_FFT_LEN);
}

/* Analysis */
/*----------*/

static size_t coherence_samples(const struct an_settings *set)
{
    (void)set;
    return (size_t)1 << AN_TWO_LOG2;
}

static void coherence_analyse(const float complex *x1, const float complex *x2, size_t count, const struct an_settings *set,
                              struct an_result *res)
{
    struct m2sdr_coherence_config cfg = { 0 };

    cfg.fft_len = SET(set)->coherence_fft_len > 0 ? (size_t)SET(set)->coherence_fft_len : AN_COHERENCE_FFT_LEN;
    /* The channel of the spectrum, when one is set, is the band of the figures. */
    if (set->chan_bw_hz > 0) {
        cfg.lo_hz = set->chan_offset_hz - 0.5 * set->chan_bw_hz;
        cfg.hi_hz = set->chan_offset_hz + 0.5 * set->chan_bw_hz;
    }
    RES(res)->coherence_valid = m2sdr_coherence_analyse(x1, x2, count, set->sample_rate, &cfg, &RES(res)->coherence) == 0;
}

/* Reports */
/*---------*/

static void coherence_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_coherence_result *c = &RES(res)->coherence;

    printf("\nCoherence of RX2 against RX1:\n");
    if (!res->two_channels) {
        printf("  Needs both channels: a board, or a file of two channels.\n");
    } else if (!RES(res)->coherence_valid) {
        printf("  Not measured: the FFT length or the band does not fit the capture.\n");
    } else if (!c->found) {
        printf("  No signal common to both channels (mean coherence %.3f over %d bins, %u segments).\n", c->coherence,
               c->band_bins, c->segments);
    } else {
        printf("  Coherence  : %.4f over the coherent bins (%d of %d), %.4f over the band\n", c->coherence_used,
               c->coherent_bins, c->band_bins, c->coherence);
        printf("  Levels     : RX1 %.2f %s, RX2 %.2f %s\n", c->rx1_db + rep->off, rep->unit, c->rx2_db + rep->off, rep->unit);
        printf("  Gain       : %+.3f dB (RX2 over RX1), ripple %.3f dB RMS\n", c->gain_db, c->gain_ripple_db);
        printf("  Phase      : %+.3f degrees at the centre, ripple %.3f degrees RMS\n", c->phase_deg, c->phase_ripple_deg);
        if (c->delay_valid)
            printf("  Delay      : %+.3f ns (%+.4f samples), RX2 behind RX1\n", c->delay_s * 1e9, c->delay_samples);
        else
            printf("  Delay      : not measured, the coherent bins are too few or too close (a tone)\n");
    }
}

static void coherence_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_coherence_result *c = &RES(res)->coherence;
    const bool ok = res->two_channels && RES(res)->coherence_valid && c->found;

    printf("  \"coherence\": {\n");
    printf("    \"two_channels\": %s,\n", res->two_channels ? "true" : "false");
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        printf("    \"fft_len\": %d,\n", c->bins);
        printf("    \"segments\": %u,\n", c->segments);
        printf("    \"band_bins\": %d,\n", c->band_bins);
        printf("    \"coherent_bins\": %d,\n", c->coherent_bins);
        print_json_number("coherence", c->coherence_used, false);
        print_json_number("coherence_band", c->coherence, false);
        print_json_number("rx1_db", c->rx1_db + rep->off, false);
        print_json_number("rx2_db", c->rx2_db + rep->off, false);
        print_json_number("gain_db", c->gain_db, false);
        print_json_number("gain_ripple_db", c->gain_ripple_db, false);
        print_json_number("phase_deg", c->phase_deg, false);
        print_json_number("phase_ripple_deg", c->phase_ripple_deg, false);
        printf("    \"delay_valid\": %s,\n", c->delay_valid ? "true" : "false");
        print_json_number("delay_ns", c->delay_valid ? c->delay_s * 1e9 : NAN, false);
        print_json_number("delay_samples", c->delay_valid ? c->delay_samples : NAN, true);
    }
    printf("  }");
}

/* Watch */
/*-------*/

static unsigned coherence_watch_lines;

static int coherence_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_coherence_result *c = &RES(res)->coherence;
    const bool ok = res->two_channels && RES(res)->coherence_valid && c->found;

    (void)set;
    if (!RES(res)->coherence_valid) {
        fprintf(stderr, "The coherence was not measured: check the FFT length.\n");
        return -1;
    }
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, ok ? "true" : "false");
        if (ok) {
            printf(", \"coherence\": %.4f, \"gain_db\": %.3f, \"phase_deg\": %.3f", c->coherence_used, c->gain_db, c->phase_deg);
            if (c->delay_valid)
                printf(", \"delay_ns\": %.3f", c->delay_s * 1e9);
        }
        printf("}\n");
    } else {
        if (coherence_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %9s %8s %9s %9s\n%8s %9s %8s %9s %9s\n", "time", "coherence", "gain", "phase", "delay",
                   "s", "", "dB", "deg", "ns");
        if (ok) {
            printf("%8.2f %9.4f %+8.3f %+9.3f", now, c->coherence_used, c->gain_db, c->phase_deg);
            if (c->delay_valid)
                printf(" %+9.3f", c->delay_s * 1e9);
            printf("\n");
        } else {
            printf("%8.2f  no common signal (coherence %.3f)\n", now, c->coherence);
        }
    }
    return 0;
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

#define COH_COL_RX1   0xff40d0ffu
#define COH_COL_RX2   0xffffa040u
#define COH_COL_MSC   0xff60e060u
#define COH_COL_RES   0xffa0a0a0u

/* A per-bin trace against frequency, with its value under the mouse. */
static void coherence_plot(const char *id, ImVec2 size, const struct m2sdr_coherence_result *c, const float *a, const float *b,
                           ImU32 ca, ImU32 cb, double y0, double y1, const char *label, const char *unit)
{
    struct m2sdr_plot plot;
    const double xa = m2sdr_coherence_freq(c, 0), xb = m2sdr_coherence_freq(c, c->bins - 1);
    char text[96];

    if (!m2sdr_plot_begin(&plot, id, size, xa, xb, M2SDR_PLOT_FREQ, y0, y1, label))
        return;
    if (b)
        m2sdr_plot_trace(&plot, b, (size_t)c->bins, xa, xb, cb);
    m2sdr_plot_trace(&plot, a, (size_t)c->bins, xa, xb, ca);
    if (plot.hovered) {
        int k = (int)lround((plot.mouse_x - xa) / c->bin_hz);

        k = k < 0 ? 0 : k >= c->bins ? c->bins - 1 : k;
        snprintf(text, sizeof(text), "%+.3f MHz: %.3f%s (coherence %.3f)", m2sdr_coherence_freq(c, k) / 1e6, a[k], unit,
                 c->msc[k]);
        m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
        m2sdr_plot_vline(&plot, plot.mouse_x, 0x40ffffffu);
    }
    m2sdr_plot_end(&plot);
}

static void draw_coherence(struct an_ui *ui, bool live, bool *changed)
{
    static const int lens[] = { 256, 512, 1024, 2048, 4096, 8192 };
    const struct an_result *res = &ui->res;
    const struct m2sdr_coherence_result *c = &RES(res)->coherence;
    const int len = SET(&ui->set)->coherence_fft_len > 0 ? SET(&ui->set)->coherence_fft_len : AN_COHERENCE_FFT_LEN;
    ImVec2 avail, half;
    char text[64];
    double top = -200.0;
    int i, k;

    (void)live;
    igTextUnformatted("FFT:", NULL);
    igSameLine(0.0f, 8.0f);
    igSetNextItemWidth(100.0f);
    snprintf(text, sizeof(text), "%d", len);
    if (igBeginCombo("##cohfft", text, 0)) {
        for (i = 0; i < (int)(sizeof(lens) / sizeof(lens[0])); i++) {
            snprintf(text, sizeof(text), "%d", lens[i]);
            if (igSelectable_Bool(text, lens[i] == len, 0, (ImVec2){ 0.0f, 0.0f })) {
                SET(&ui->set)->coherence_fft_len = lens[i];
                *changed = true;
            }
        }
        igEndCombo();
    }
    igSameLine(0.0f, 16.0f);
    igTextUnformatted(ui->set.chan_bw_hz > 0 ? "Band: the channel of the spectrum view." : "Band: the whole capture.", NULL);
    if (ui->set.agc) {
        igSameLine(0.0f, 16.0f);
        igTextColored((ImVec4){ 1.0f, 0.75f, 0.3f, 1.0f }, "AGC on: each RX sets its own gain; a manual gain compares them.");
    }
    if (!SET(&res->set)->coherence_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->two_channels) {
        igTextUnformatted("Needs both channels: a board, or a file of two channels.", NULL);
        return;
    }
    if (!RES(res)->coherence_valid) {
        igTextUnformatted("Not measured: the FFT length or the band does not fit the capture.", NULL);
        return;
    }
    if (!c->found) {
        igPushTextWrapPos(0.0f);
        igText("No signal common to both channels (mean coherence %.3f): feed both from one source, through a splitter, "
               "or two antennas on a strong signal.", c->coherence);
        igPopTextWrapPos();
    }
    for (k = 0; k < c->bins; k++) {
        if (c->rx1_bin_db[k] > top)
            top = c->rx1_bin_db[k];
        if (c->rx2_bin_db[k] > top)
            top = c->rx2_bin_db[k];
    }
    top = 10.0 * ceil(top / 10.0);
    igGetContentRegionAvail(&avail);
    legend_item(COH_COL_RX1, "RX1", avail.x);
    legend_item(COH_COL_RX2, "RX2", avail.x);
    legend_item(COH_COL_MSC, "coherence", avail.x);
    legend_item(COH_COL_RES, "phase less delay and phase", avail.x);
    igNewLine();
    igGetContentRegionAvail(&avail);
    half = (ImVec2){ 0.5f * avail.x - 4.0f, 0.5f * avail.y - 4.0f };
    coherence_plot("##cohpow", half, c, c->rx1_bin_db, c->rx2_bin_db, COH_COL_RX1, COH_COL_RX2, top - 100.0, top,
                   "Level of each channel, dBFS per bin", " dBFS");
    igSameLine(0.0f, 8.0f);
    coherence_plot("##cohmsc", half, c, c->msc, NULL, COH_COL_MSC, 0, 0.0, 1.05, "Coherence", "");
    coherence_plot("##cohgain", half, c, c->gain_bin_db, NULL, COH_COL_RX2, 0, c->found ? c->gain_db - 6.0 : -20.0,
                   c->found ? c->gain_db + 6.0 : 20.0, "Gain of RX2 over RX1, dB", " dB");
    igSameLine(0.0f, 8.0f);
    coherence_plot("##cohphase", (ImVec2){ 0.0f, half.y }, c, c->phase_bin_deg, c->residual_bin_deg, COH_COL_RX1, COH_COL_RES,
                   -180.0, 180.0, "Phase of RX2 against RX1, degrees", " deg");
}

static void draw_coherence_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_coherence_result *c = &RES(res)->coherence;

    igSeparatorText("Coherence");
    if (!res->two_channels || !RES(res)->coherence_valid || !c->found) {
        igTextUnformatted("No common signal.", NULL);
        return;
    }
    if (igBeginTable("##coh", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Coherence", "%.4f", c->coherence_used);
        label_value("Coherent bins", "%d of %d", c->coherent_bins, c->band_bins);
        label_value("RX1, RX2", "%.1f, %.1f dBFS", c->rx1_db, c->rx2_db);
        label_value("Gain", "%+.3f dB", c->gain_db);
        label_value("Gain ripple", "%.3f dB RMS", c->gain_ripple_db);
        label_value("Phase", "%+.2f deg", c->phase_deg);
        label_value("Phase ripple", "%.2f deg RMS", c->phase_ripple_deg);
        if (c->delay_valid)
            label_value("Delay", "%+.3f ns (%+.3f samples)", c->delay_s * 1e9, c->delay_samples);
        else
            label_value("Delay", "not measured (a tone)");
        label_value("Segments", "%u of %d", c->segments, c->bins);
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_coherence = {
    .key           = "coherence",
    .title         = "Coherence",
    .category      = AN_CAT_TWO_CHANNEL,
    .settings_size = sizeof(struct coherence_set),
    .on            = offsetof(struct coherence_set, coherence_on),
    .result_size   = sizeof(struct coherence_res),
    .valid         = offsetof(struct coherence_res, coherence_valid),
    .found         = offsetof(struct coherence_res, coherence.found),
    .report_rank   = 70,
    .samples       = coherence_samples,
    .two_channels  = true,
    .analyse_two   = coherence_analyse,
    .report_text   = coherence_report_text,
    .report_json   = coherence_report_json,
    .watch_line    = coherence_watch_line,
    .options       = coherence_options,
    .option        = coherence_option,
    .help          = coherence_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw          = draw_coherence,
    .results       = draw_coherence_results,
#endif
};
