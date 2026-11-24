/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: 2x2 channel of an LTE downlink cell of two antenna ports on RX1
 * and RX2 (singular values, condition number, rank, path powers per resource block), in a
 * report, a view and --watch.
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

/* Command line */
/*--------------*/

static const struct option mimo_options[] = {
    { "mimo", no_argument, NULL, 0 },
    { NULL, 0, NULL, 0 }
};

static int mimo_option(int id, const char *arg, struct an_cli *cli)
{
    (void)id;
    (void)arg;
    cli->set->mimo_on = true;
    return 0;
}

static void mimo_help(void)
{
    printf("LTE 2x2 options (RX1 and RX2 together; a file needs two channels):\n"
           "      --mimo             With --report: 2x2 channel of an LTE cell of two antenna ports from\n"
           "                         its reference signals: condition number, singular values, rank and\n"
           "                         path powers per resource block. Sample rates as --lte. --watch works.\n"
           "\n");
}

/* Analysis */
/*----------*/

/* The capture of the LTE mode: a whole frame wherever it starts. */
static size_t mimo_samples(const struct an_settings *set)
{
    return (size_t)(AN_LTE_SECONDS * set->sample_rate);
}

static void mimo_analyse(const float complex *x1, const float complex *x2, size_t count, const struct an_settings *set,
                         struct an_result *res)
{
    const size_t n = (size_t)(AN_LTE_SECONDS * set->sample_rate);

    res->mimo_valid = m2sdr_mimo_analyse(x1, x2, count < n ? count : n, set->sample_rate, &res->mimo) == 0;
}

/* Reports */
/*---------*/

static void mimo_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_mimo_result *m = &res->mimo;
    int r, p;

    printf("\nLTE 2x2 channel (RX1, RX2):\n");
    if (!res->two_channels) {
        printf("  Needs both channels: a board, or a file of two channels.\n");
    } else if (!res->mimo_valid) {
        printf("  The sample rate must be 1.92, 3.84, 7.68, 15.36 or 30.72 MSPS, with at least 25 ms of samples.\n");
    } else if (!m->found) {
        printf("  No cell found on RX1 nor on RX2.\n");
    } else {
        printf("  Cell       : %d, found on RX%d, %d antenna port%s%s, %d blocks (%d measured from block %d), offset %+.1f Hz\n",
               m->cell_id, m->rx + 1, m->ports, m->ports == 1 ? "" : "s", m->ports ? "" : " (MIB not read)", m->n_rb, m->rb,
               m->first_rb, m->freq_error_hz);
        printf("  Paths      :");
        for (r = 0; r < 2; r++) {
            for (p = 0; p < (m->mimo ? 2 : 1); p++)
                printf(" RX%d-port%d %.1f", r + 1, p, m->path_db[r][p] + rep->off);
        }
        printf(" %s\n", rep->unit);
        printf("  Pilot SNR  : %.1f dB\n", m->snr_db);
        if (!m->mimo) {
            printf("  One antenna port: no 2x2 figures.\n");
            return;
        }
        printf("  Singular   : s1^2 %.1f %s, s2^2 %.1f %s\n", m->sv_db[0] + rep->off, rep->unit, m->sv_db[1] + rep->off,
               rep->unit);
        printf("  Condition  : %.1f dB mean, %.1f dB worst block\n", m->cond_db, m->cond_max_db);
        printf("  Rank       : %d (%d of %d blocks pick two layers), capacity %.2f / %.2f bit/s/Hz with one / two layers\n",
               m->rank, m->rank2_rb, m->rb, m->capacity[0], m->capacity[1]);
        printf("  Correlation: receive %.3f, transmit %.3f\n", m->correlation_rx, m->correlation_tx);
    }
}

static void mimo_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_mimo_result *m = &res->mimo;
    const bool ok = res->two_channels && res->mimo_valid && m->found;
    int b;

    printf("  \"mimo\": {\n");
    printf("    \"two_channels\": %s,\n", res->two_channels ? "true" : "false");
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        printf("    \"cell_id\": %d,\n", m->cell_id);
        printf("    \"rx\": %d,\n", m->rx + 1);
        printf("    \"ports\": %d,\n", m->ports);
        printf("    \"n_rb\": %d,\n", m->n_rb);
        printf("    \"rb\": %d,\n", m->rb);
        printf("    \"first_rb\": %d,\n", m->first_rb);
        print_json_number("carrier_offset_hz", m->freq_error_hz, false);
        print_json_number("snr_db", m->snr_db, false);
        printf("    \"path_db\": [[%.3f, %.3f], [%.3f, %.3f]],\n", m->path_db[0][0] + rep->off, m->path_db[0][1] + rep->off,
               m->path_db[1][0] + rep->off, m->path_db[1][1] + rep->off);
        printf("    \"mimo\": %s%s\n", m->mimo ? "true" : "false", m->mimo ? "," : "");
        if (m->mimo) {
            print_json_number("s1_db", m->sv_db[0] + rep->off, false);
            print_json_number("s2_db", m->sv_db[1] + rep->off, false);
            print_json_number("condition_db", m->cond_db, false);
            print_json_number("condition_max_db", m->cond_max_db, false);
            printf("    \"rank\": %d,\n", m->rank);
            printf("    \"rank2_rb\": %d,\n", m->rank2_rb);
            print_json_number("capacity1", m->capacity[0], false);
            print_json_number("capacity2", m->capacity[1], false);
            print_json_number("correlation_rx", m->correlation_rx, false);
            print_json_number("correlation_tx", m->correlation_tx, false);
            printf("    \"condition_rb_db\": [");
            for (b = 0; b < m->rb; b++)
                printf("%s%.2f", b ? ", " : "", m->cond_rb_db[b]);
            printf("]\n");
        }
    }
    printf("  }");
}

/* Watch */
/*-------*/

static unsigned mimo_watch_lines;

static int mimo_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_mimo_result *m = &res->mimo;
    const bool ok = res->two_channels && res->mimo_valid && m->found && m->mimo;

    (void)set;
    if (!res->mimo_valid) {
        fprintf(stderr, "The sample rate is not one of LTE (1.92 to 30.72 MSPS).\n");
        return -1;
    }
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, ok ? "true" : "false");
        if (ok)
            printf(", \"cell_id\": %d, \"condition_db\": %.2f, \"rank\": %d, \"snr_db\": %.1f, \"path_db\": [[%.2f, %.2f], [%.2f, %.2f]]",
                   m->cell_id, m->cond_db, m->rank, m->snr_db, m->path_db[0][0], m->path_db[0][1], m->path_db[1][0],
                   m->path_db[1][1]);
        printf("}\n");
    } else {
        if (mimo_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %5s %6s %6s %4s %6s %7s %7s %7s %7s\n%8s %5s %6s %6s %4s %6s %7s %7s %7s %7s\n", "time", "cell", "cond",
                   "worst", "rank", "SNR", "H11", "H12", "H21", "H22", "s", "", "dB", "dB", "", "dB", "dB", "dB", "dB", "dB");
        if (ok)
            printf("%8.2f %5d %6.1f %6.1f %4d %6.1f %7.1f %7.1f %7.1f %7.1f\n", now, m->cell_id, m->cond_db, m->cond_max_db,
                   m->rank, m->snr_db, m->path_db[0][0], m->path_db[0][1], m->path_db[1][0], m->path_db[1][1]);
        else if (res->mimo_valid && m->found)
            printf("%8.2f %5d  one antenna port: no 2x2 channel\n", now, m->cell_id);
        else
            printf("%8.2f  no cell\n", now);
    }
    return 0;
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

#define MIMO_COL_S1    0xff40d0ffu
#define MIMO_COL_S2    0xffffa040u
#define MIMO_COL_COND  0xff60e060u

static const ImU32 mimo_path_colors[2][2] = { { 0xff40d0ffu, 0xff4080ffu }, { 0xffffa040u, 0xffff60c0u } };

/* Per block against the block of the cell. */
static void mimo_plot_blocks(const char *id, ImVec2 size, const struct m2sdr_mimo_result *m, double y0, double y1,
                             const char *label, int what)
{
    static float bx[M2SDR_MIMO_MAX_RB], by[M2SDR_MIMO_MAX_RB];
    struct m2sdr_plot plot;
    char text[128];
    int b, r, p;

    if (!m2sdr_plot_begin(&plot, id, size, m->first_rb - 0.5, m->first_rb + m->rb - 0.5, M2SDR_PLOT_PLAIN, y0, y1, label))
        return;
    for (b = 0; b < m->rb; b++)
        bx[b] = (float)(m->first_rb + b);
    if (what == 0) {
        for (b = 0; b < m->rb; b++)
            by[b] = m->cond_rb_db[b];
        m2sdr_plot_xy(&plot, bx, by, (size_t)m->rb, MIMO_COL_COND);
        m2sdr_plot_points(&plot, bx, by, (size_t)m->rb, 2.0f, MIMO_COL_COND);
        m2sdr_plot_hline(&plot, m->cond_db, 0x60ffffffu);
    } else if (what == 1) {
        for (r = 0; r < 2; r++) {
            for (b = 0; b < m->rb; b++)
                by[b] = m->sv_rb_db[b][r];
            m2sdr_plot_xy(&plot, bx, by, (size_t)m->rb, r ? MIMO_COL_S2 : MIMO_COL_S1);
        }
    } else {
        for (r = 0; r < 2; r++) {
            for (p = 0; p < 2; p++) {
                for (b = 0; b < m->rb; b++)
                    by[b] = m->path_rb_db[b][r][p];
                m2sdr_plot_xy(&plot, bx, by, (size_t)m->rb, mimo_path_colors[r][p]);
            }
        }
    }
    if (plot.hovered) {
        b = (int)lround(plot.mouse_x) - m->first_rb;
        b = b < 0 ? 0 : b >= m->rb ? m->rb - 1 : b;
        snprintf(text, sizeof(text), "block %d: condition %.1f dB, s1^2 %.1f, s2^2 %.1f dBFS, rank %d", m->first_rb + b,
                 m->cond_rb_db[b], m->sv_rb_db[b][0], m->sv_rb_db[b][1], m->rank_rb[b]);
        m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
        m2sdr_plot_vline(&plot, m->first_rb + b, 0x40ffffffu);
    }
    m2sdr_plot_end(&plot);
}

/* The four paths as a 2x2 table of their power. */
static void mimo_matrix(const struct m2sdr_mimo_result *m)
{
    int r, p;

    if (!igBeginTable("##mimoh", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit, (ImVec2){ 200.0f, 0.0f }, 0.0f))
        return;
    igTableNextRow(0, 0.0f);
    igTableNextColumn();
    igTextUnformatted("dBFS", NULL);
    for (p = 0; p < 2; p++) {
        igTableNextColumn();
        igText("port %d", p);
    }
    for (r = 0; r < 2; r++) {
        igTableNextRow(0, 0.0f);
        igTableNextColumn();
        igText("RX%d", r + 1);
        for (p = 0; p < 2; p++) {
            const ImU32 c = mimo_path_colors[r][p];

            igTableNextColumn();
            if (p == 1 && !m->mimo)
                igTextUnformatted("--", NULL);
            else
                igTextColored((ImVec4){ (c & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, ((c >> 16) & 0xff) / 255.0f, 1.0f },
                              "%7.1f", m->path_db[r][p]);
        }
    }
    igEndTable();
}

static void draw_mimo(struct an_ui *ui, bool live, bool *changed)
{
    static const double rates[5] = { 1.92e6, 3.84e6, 7.68e6, 15.36e6, 30.72e6 };
    const struct an_result *res = &ui->res;
    const struct m2sdr_mimo_result *m = &res->mimo;
    ImVec2 avail, half;
    double top = -200.0, bottom = 0.0;
    char text[64];
    int i, b;

    if (live) {
        igTextUnformatted("Sample rate:", NULL);
        for (i = 0; i < 5; i++) {
            snprintf(text, sizeof(text), "%.2f MSPS", rates[i] / 1e6);
            same_line_or_wrap(8.0f, 80.0f);
            if (igSmallButton(text)) {
                ui->set.sample_rate = rates[i];
                ui->set.bandwidth   = 0;
                view_full(ui);
                traces_reset(ui);
                *changed = true;
            }
        }
    }
    igTextDisabled("The cell search of LTE takes about 0.2 s a capture: a few results a second.");
    if (!res->set.mimo_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->two_channels) {
        igTextUnformatted("Needs both channels: a board, or a file of two channels.", NULL);
        return;
    }
    if (!res->mimo_valid) {
        igTextUnformatted("The sample rate must be 1.92, 3.84, 7.68, 15.36 or 30.72 MSPS.", NULL);
        return;
    }
    if (!m->found) {
        igTextUnformatted("No cell found on RX1 nor RX2: tune on the centre of an FDD carrier.", NULL);
        return;
    }
    igBeginGroup();
    mimo_matrix(m);
    igEndGroup();
    igSameLine(0.0f, 16.0f);
    igBeginGroup();
    igText("Cell %d on RX%d, %d port%s, blocks %d to %d of %d", m->cell_id, m->rx + 1, m->ports, m->ports == 1 ? "" : "s",
           m->first_rb, m->first_rb + m->rb - 1, m->n_rb);
    if (m->mimo)
        igText("Condition %.1f dB (worst %.1f), rank %d, pilot SNR %.1f dB", m->cond_db, m->cond_max_db, m->rank, m->snr_db);
    else
        igText("One antenna port: no 2x2 channel. Pilot SNR %.1f dB", m->snr_db);
    igEndGroup();
    if (!m->mimo)
        return;
    for (b = 0; b < m->rb; b++) {
        if (m->sv_rb_db[b][0] > top)
            top = m->sv_rb_db[b][0];
        if (m->sv_rb_db[b][1] < bottom)
            bottom = m->sv_rb_db[b][1];
    }
    top    = 10.0 * ceil(top / 10.0) + 5.0;
    bottom = bottom < top - 60.0 ? top - 60.0 : 10.0 * floor(bottom / 10.0) - 5.0;
    igGetContentRegionAvail(&avail);
    legend_item(MIMO_COL_COND, "condition number", avail.x);
    legend_item(MIMO_COL_S1, "s1^2", avail.x);
    legend_item(MIMO_COL_S2, "s2^2", avail.x);
    igNewLine();
    igGetContentRegionAvail(&avail);
    half = (ImVec2){ 0.5f * avail.x - 4.0f, 0.5f * avail.y - 4.0f };
    mimo_plot_blocks("##mimocond", half, m, 0.0, m->cond_max_db > 30.0 ? m->cond_max_db + 3.0 : 33.0,
                     "Condition number per block, dB", 0);
    igSameLine(0.0f, 8.0f);
    mimo_plot_blocks("##mimosv", (ImVec2){ 0.0f, half.y }, m, bottom, top, "Singular values per block, dBFS", 1);
    mimo_plot_blocks("##mimopath", (ImVec2){ 0.0f, 0.0f }, m, bottom, top,
                     "Path power per block, dBFS (colours of the table)", 2);
}

static void draw_mimo_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_mimo_result *m = &res->mimo;

    igSeparatorText("LTE 2x2");
    if (!res->two_channels || !res->mimo_valid || !m->found) {
        igTextUnformatted("No cell.", NULL);
        return;
    }
    if (igBeginTable("##mimo", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Cell", "%d (RX%d)", m->cell_id, m->rx + 1);
        label_value("Ports", m->ports ? "%d" : "%d (no MIB)", m->ports);
        label_value("Offset", "%+.1f Hz", m->freq_error_hz);
        label_value("Pilot SNR", "%.1f dB", m->snr_db);
        if (m->mimo) {
            label_value("Condition", "%.1f dB (worst %.1f)", m->cond_db, m->cond_max_db);
            label_value("s1^2, s2^2", "%.1f, %.1f dBFS", m->sv_db[0], m->sv_db[1]);
            label_value("Rank", "%d (%d of %d blocks)", m->rank, m->rank2_rb, m->rb);
            label_value("Capacity 1, 2", "%.2f, %.2f bit/s/Hz", m->capacity[0], m->capacity[1]);
            label_value("Correlation RX, TX", "%.3f, %.3f", m->correlation_rx, m->correlation_tx);
        }
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_mimo = {
    .key          = "mimo",
    .title        = "LTE 2x2",
    .category     = AN_CAT_TWO_CHANNEL,
    .on           = offsetof(struct an_settings, mimo_on),
    .valid        = offsetof(struct an_result, mimo_valid),
    .found        = offsetof(struct an_result, mimo.found),
    .report_rank  = 72,
    .default_rate = 7680000.0,
    .psd_short    = true,
    .samples      = mimo_samples,
    .two_channels = true,
    .analyse_two  = mimo_analyse,
    .report_text  = mimo_report_text,
    .report_json  = mimo_report_json,
    .watch_line   = mimo_watch_line,
    .options      = mimo_options,
    .option       = mimo_option,
    .help         = mimo_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_mimo,
    .results      = draw_mimo_results,
#endif
};
