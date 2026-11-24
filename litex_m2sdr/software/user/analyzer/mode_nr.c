/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: 5G NR synchronisation signal blocks (PCI, SSB index, SS-RSRP,
 * SS-SINR, EVM), in a report, a view and --watch.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "m2sdr_cli.h"

#include "analyzer.h"

/* Analysis */
/*----------*/

/* The spacing found on a tuning: the next captures of it search that one only (half the
 * time when it is not given). */
static struct {
    bool   ok;
    double freq_hz, rate, scs;
    int    channel;
} nr_track;

static size_t nr_samples(const struct an_settings *set)
{
    return (size_t)(AN_NR_SECONDS * set->sample_rate);
}

static void nr_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = (size_t)(AN_NR_SECONDS * set->sample_rate);
    struct m2sdr_nr_config cfg;

    /* L_max (TS 38.213 4.1): 4 up to 3 GHz, 8 above (FR1). */
    cfg.scs_hz    = set->nr_scs_hz > 0 ? set->nr_scs_hz : set->nr_scs_known;
    cfg.search_hz = 0;
    cfg.lmax      = set->freq_hz <= 3e9 ? 4 : 8;
    res->nr_valid = m2sdr_nr_analyse(x, count < n ? count : n, set->sample_rate, &cfg, &res->nr) == 0;
}

static void nr_track_prepare(struct an_settings *set)
{
    set->nr_scs_known = 0;
    if (set->nr_on && nr_track.ok && set->freq_hz == nr_track.freq_hz && set->sample_rate == nr_track.rate &&
        set->channel == nr_track.channel)
        set->nr_scs_known = nr_track.scs;
}

static void nr_track_result(const struct an_settings *set, const struct an_result *res)
{
    if (!set->nr_on || !res->nr_valid)
        return;
    nr_track.ok = res->nr.found;
    if (res->nr.found) {
        nr_track.freq_hz = set->freq_hz;
        nr_track.rate    = set->sample_rate;
        nr_track.channel = set->channel;
        nr_track.scs     = res->nr.scs_hz;
    }
}

/* Reports */
/*---------*/

static void nr_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_nr_result *r = &res->nr;
    int i;

    printf("\n5G NR SSB:\n");
    if (!res->nr_valid) {
        printf("  The sample rate must be the subcarrier spacing (15 or 30 kHz) times a power of two from 256\n"
               "  (7.68, 15.36 or 30.72 MSPS), with at least an SSB of samples.\n");
        return;
    }
    if (!r->found) {
        printf("  No SSB found (tried %.0f kHz last): %.0f ms of samples hold a burst of SSBs every 20 ms.\n", r->scs_hz / 1e3,
               AN_NR_SECONDS * 1e3);
        return;
    }
    printf("  SSBs       : %d at %.0f kHz (FFT %d), %d cell%s, SSREF %.6f MHz (%+.1f Hz from the centre)\n", r->nssb,
           r->scs_hz / 1e3, r->fft_len, r->cells, r->cells == 1 ? "" : "s", (set->freq_hz + r->freq_error_hz) / 1e6,
           r->freq_error_hz);
    if (r->best >= 0) {
        const struct m2sdr_nr_ssb *b = &r->ssb[r->best];

        printf("  Strongest  : PCI %d (%d, %d), index %d, SS-RSRP %.1f %s, SS-SINR %.1f dB, DM-RS EVM %.2f %%, PBCH EVM %.2f %%\n",
               b->pci, b->n_id_1, b->n_id_2, b->ssb_index, b->rsrp_db + rep->off, rep->unit, b->sinr_db,
               100.0 * b->dmrs_evm, 100.0 * b->pbch_evm);
    }
    printf("  MIB        : not decoded (polar code)\n");
    for (i = 0; i < r->nssb; i++) {
        const struct m2sdr_nr_ssb *b = &r->ssb[i];

        printf("    %8.3f ms  PCI %4d  index %d%s  %+11.1f Hz  RSRP %6.1f %s  SINR %5.1f dB  DM-RS %5.2f %%  PBCH %5.2f %%\n",
               1e3 * b->time_s, b->pci, b->ssb_index, b->half_frame > 0 ? "'" : " ", b->freq_offset_hz,
               b->rsrp_db + rep->off, rep->unit, b->sinr_db, 100.0 * b->dmrs_evm, 100.0 * b->pbch_evm);
    }
}

static void nr_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_nr_result *r = &res->nr;
    const bool ok = res->nr_valid && r->found;
    int i;

    printf("  \"nr\": {\n");
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        const struct m2sdr_nr_ssb *b = &r->ssb[r->best];

        print_json_number("scs_hz", r->scs_hz, false);
        printf("    \"fft_len\": %d,\n    \"cells\": %d,\n    \"ssb_count\": %d,\n", r->fft_len, r->cells, r->nssb);
        print_json_number("ssref_hz", set->freq_hz + r->freq_error_hz, false);
        print_json_number("carrier_offset_hz", r->freq_error_hz, false);
        printf("    \"pci\": %d,\n    \"ssb_index\": %d,\n", b->pci, b->ssb_index);
        print_json_number("rsrp_db", b->rsrp_db + rep->off, false);
        print_json_number("sinr_db", b->sinr_db, false);
        print_json_number("dmrs_evm", b->dmrs_evm, false);
        print_json_number("pbch_evm", b->pbch_evm, false);
        printf("    \"mib\": false,\n");
        printf("    \"ssb\": [");
        for (i = 0; i < r->nssb; i++) {
            const struct m2sdr_nr_ssb *s = &r->ssb[i];

            printf("%s\n      { \"time_s\": %.6f, \"start\": %zu, \"pci\": %d, \"ssb_index\": %d, \"half_frame\": %d, "
                   "\"carrier_offset_hz\": %.1f, \"rsrp_db\": %.2f, \"sinr_db\": %.2f, \"dmrs_evm\": %.5f, \"pbch_evm\": %.5f }",
                   i ? "," : "", s->time_s, s->start, s->pci, s->ssb_index, s->half_frame, s->freq_offset_hz,
                   s->rsrp_db + rep->off, s->sinr_db, s->dmrs_evm, s->pbch_evm);
        }
        printf("\n    ]\n");
    }
    printf("  }");
}

/* Watch */
/*-------*/

static unsigned nr_watch_lines;

static int nr_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_nr_result *r = &res->nr;
    const struct m2sdr_nr_ssb *b = r->best >= 0 ? &r->ssb[r->best] : NULL;

    if (!res->nr_valid) {
        fprintf(stderr, "The sample rate is not one of NR (15 or 30 kHz times a power of two).\n");
        return -1;
    }
    nr_track_result(set, res);
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, r->found ? "true" : "false");
        if (r->found && b)
            printf(", \"pci\": %d, \"ssb_index\": %d, \"ssbs\": %d, \"rsrp_db\": %.2f, \"sinr_db\": %.2f, \"dmrs_evm\": %.5f, "
                   "\"carrier_offset_hz\": %.1f", b->pci, b->ssb_index, r->nssb, b->rsrp_db + set->ref_offset_db, b->sinr_db,
                   b->dmrs_evm, r->freq_error_hz);
        printf("}\n");
        return 0;
    }
    if (nr_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
        printf("%8s %5s %5s %4s %8s %6s %7s %10s\n%8s %5s %5s %4s %8s %6s %7s %10s\n", "time", "PCI", "index", "SSBs",
               "RSRP", "SINR", "DM-RS", "offset", "s", "", "", "", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "dB", "%", "Hz");
    if (r->found && b)
        printf("%8.2f %5d %5d %4d %8.1f %6.1f %7.2f %+10.1f\n", now, b->pci, b->ssb_index, r->nssb,
               b->rsrp_db + set->ref_offset_db, b->sinr_db, 100.0 * b->dmrs_evm, r->freq_error_hz);
    else
        printf("%8.2f  no SSB\n", now);
    return 0;
}

/* Command line */
/*--------------*/

static const struct option nr_options[] = {
    { "nr",     no_argument,       NULL, 0 },
    { "nr-scs", required_argument, NULL, 1 },
    { NULL, 0, NULL, 0 }
};

static int nr_option(int id, const char *arg, struct an_cli *cli)
{
    double v;

    if (id == 0) {
        cli->set->nr_on = true;
        return 0;
    }
    if (m2sdr_cli_parse_double(arg, &v) != 0 || (v != 15000.0 && v != 30000.0)) {
        m2sdr_cli_error("invalid SSB spacing '%s' (expected 15000 or 30000)", arg);
        return -1;
    }
    cli->set->nr_scs_hz = v;
    return 0;
}

static void nr_help(void)
{
    printf("5G NR options:\n"
           "      --nr               With --report: the 5G NR SSBs of the capture: PCI, SSB index, SS-RSRP,\n"
           "                         SS-SINR, EVM, offset (default rate 15.36 MSPS; 7.68 or 30.72 too).\n"
           "                         --watch works too.\n"
           "      --nr-scs HZ        Spacing of the SSBs, 15000 or 30000 (default: found, 30 kHz first).\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

#define NR_GRID_TOP_DB    10.0
#define NR_GRID_BOTTOM_DB (-40.0)
#define NR_COL_PBCH       0xc0ffd080u
#define NR_COL_DMRS       0xffff60ffu

static void nr_worker_prepare(struct an_settings *set, bool live)
{
    (void)live;
    nr_track_prepare(set);
}

/* Grid of the strongest SSB: 4 symbols, 240 subcarriers, the highest at the top. */
static void nr_keep(struct an_ui *ui)
{
    const struct m2sdr_nr_result *r = &ui->res.nr;
    static uint32_t rgba[M2SDR_NR_SSB_CARRIERS * M2SDR_NR_SSB_SYMBOLS];
    double ref;
    int l, k;

    if (!ui->res.set.nr_on || !ui->res.nr_valid || !r->found || r->best < 0)
        return;
    ref = r->ssb[r->best].rsrp_db;
    for (k = 0; k < M2SDR_NR_SSB_CARRIERS; k++)
        for (l = 0; l < M2SDR_NR_SSB_SYMBOLS; l++)
            rgba[(M2SDR_NR_SSB_CARRIERS - 1 - k) * M2SDR_NR_SSB_SYMBOLS + l] =
                palette_color(ui->wf_palette, (float)((r->grid_db[l][k] - (ref + NR_GRID_BOTTOM_DB)) /
                                                      (NR_GRID_TOP_DB - NR_GRID_BOTTOM_DB)));
    if (!ui->nr_grid_tex) {
        glGenTextures(1, &ui->nr_grid_tex);
        glBindTexture(GL_TEXTURE_2D, ui->nr_grid_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, ui->nr_grid_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, M2SDR_NR_SSB_SYMBOLS, M2SDR_NR_SSB_CARRIERS, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

static void nr_controls(struct an_ui *ui, bool *changed)
{
    static const double rates[3] = { 7.68e6, 15.36e6, 30.72e6 };
    static const double spacings[3] = { 0.0, 15e3, 30e3 };
    static const char *const names[3] = { "auto", "15 kHz", "30 kHz" };
    char text[32];
    int i;

    igTextUnformatted("SSB spacing:", NULL);
    for (i = 0; i < 3; i++) {
        igSameLine(0.0f, 8.0f);
        if (igRadioButton_Bool(names[i], ui->set.nr_scs_hz == spacings[i])) {
            ui->set.nr_scs_hz = spacings[i];
            *changed = true;
        }
    }
    igSameLine(0.0f, 24.0f);
    igTextUnformatted("Sample rate:", NULL);
    for (i = 0; i < 3; i++) {
        snprintf(text, sizeof(text), "%.2f MSPS", rates[i] / 1e6);
        same_line_or_wrap(8.0f, 80.0f);
        if (igSmallButton(text))
            view_rate(ui, rates[i], changed);
    }
}

static void nr_points(struct m2sdr_plot *plot, const float complex *p, int n, float radius, ImU32 color)
{
    static float px[M2SDR_NR_MAX_POINTS], py[M2SDR_NR_MAX_POINTS];
    int i;

    for (i = 0; i < n && i < M2SDR_NR_MAX_POINTS; i++) {
        px[i] = crealf(p[i]);
        py[i] = cimagf(p[i]);
    }
    m2sdr_plot_points(plot, px, py, (size_t)i, radius, color);
}

static void draw_nr(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_nr_result *r = &res->nr;
    struct m2sdr_plot plot;
    ImTextureRef tex;
    ImVec2 avail, sq;
    float side, gw;
    char text[200];
    int i;

    if (live)
        nr_controls(ui, changed);
    if (!res->set.nr_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->nr_valid) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted("The sample rate must be the SSB spacing (15 or 30 kHz) times a power of two from 256: 7.68, "
                          "15.36 or 30.72 MSPS.", NULL);
        igPopTextWrapPos();
        return;
    }
    if (!r->found) {
        igPushTextWrapPos(0.0f);
        igText("No SSB at %.0f kHz in %.0f ms: tune near the SSREF of a cell (its GSCN), the SSBs repeat every 20 ms.",
               r->scs_hz / 1e3, AN_NR_SECONDS * 1e3);
        igPopTextWrapPos();
        return;
    }
    if (live && fabs(r->freq_error_hz) > 0.5 * r->scs_hz) {
        snprintf(text, sizeof(text), "Centre on the SSB (%+.1f kHz)", r->freq_error_hz / 1e3);
        if (igSmallButton(text))
            view_retune(ui, ui->set.freq_hz + 1e3 * round(r->freq_error_hz / 1e3), changed);
    }

    /* The grid and the constellation, the SSBs under them. */
    igGetContentRegionAvail(&avail);
    side = 0.55f * avail.y < 0.45f * avail.x ? 0.55f * avail.y : 0.45f * avail.x;
    sq   = m2sdr_plot_square(side);
    gw   = avail.x - sq.x - 8.0f;
    tex._TexData = NULL;
    /* Grid of the strongest SSB. */
    igBeginGroup();
    snprintf(text, sizeof(text), "SSB of PCI %d, index %d: MHz from SSREF by symbol, dB over SS-RSRP", r->ssb[r->best].pci, r->ssb[r->best].ssb_index);
    if (m2sdr_plot_begin(&plot, "##nrgrid", (ImVec2){ gw, sq.y }, -0.5, 3.5, M2SDR_PLOT_PLAIN,
                         -120.0 * r->scs_hz / 1e6, 120.0 * r->scs_hz / 1e6, text)) {
        tex._TexID = (ImTextureID)(uintptr_t)ui->nr_grid_tex;
        if (ui->nr_grid_tex)
            ImDrawList_AddImage(plot.dl, tex, plot.p0, plot.p1, (ImVec2){ 0.0f, 0.0f }, (ImVec2){ 1.0f, 1.0f }, 0xffffffffu);
        if (plot.hovered) {
            const int l = (int)lround(plot.mouse_x), k = (int)floor(plot.mouse_y * 1e6 / r->scs_hz) + 120;

            if (l >= 0 && l < 4 && k >= 0 && k < M2SDR_NR_SSB_CARRIERS) {
                snprintf(text, sizeof(text), "symbol %d, subcarrier %d: %+.1f dB", l, k,
                         r->grid_db[l][k] - r->ssb[r->best].rsrp_db);
                m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
            }
        }
        m2sdr_plot_end(&plot);
    }
    igEndGroup();
    igSameLine(0.0f, 8.0f);
    /* PBCH data and DM-RS, equalised. */
    if (m2sdr_plot_begin(&plot, "##nrconst", sq, -1.6, 1.6, M2SDR_PLOT_PLAIN, -1.6, 1.6, "Q against I: PBCH (blue), DM-RS (magenta)")) {
        nr_points(&plot, r->dmrs_points, r->ndmrs, 1.0f, NR_COL_DMRS);
        nr_points(&plot, r->points, r->npoints, 1.0f, NR_COL_PBCH);
        m2sdr_plot_end(&plot);
    }
    /* SSBs of the capture. */
    if (igBeginTable("##nrssb", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                     (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        igTableSetupColumn("Time", 0, 0.0f, 0);
        igTableSetupColumn("PCI", 0, 0.0f, 0);
        igTableSetupColumn("Index", 0, 0.0f, 0);
        igTableSetupColumn("RSRP", 0, 0.0f, 0);
        igTableSetupColumn("SINR", 0, 0.0f, 0);
        igTableSetupColumn("DM-RS EVM", 0, 0.0f, 0);
        igTableSetupColumn("PBCH EVM", 0, 0.0f, 0);
        igTableSetupColumn("Offset", 0, 0.0f, 0);
        igTableHeadersRow();
        for (i = 0; i < r->nssb; i++) {
            const struct m2sdr_nr_ssb *b = &r->ssb[i];

            igTableNextRow(0, 0.0f);
            igTableNextColumn();
            igText("%.3f ms", 1e3 * b->time_s);
            igTableNextColumn();
            igText("%d", b->pci);
            igTableNextColumn();
            if (b->half_frame >= 0)
                igText("%d (half %d)", b->ssb_index, b->half_frame);
            else
                igText("%d", b->ssb_index);
            igTableNextColumn();
            igText("%.1f dBFS", b->rsrp_db);
            igTableNextColumn();
            igText("%.1f dB", b->sinr_db);
            igTableNextColumn();
            igText("%.2f %%", 100.0 * b->dmrs_evm);
            igTableNextColumn();
            igText("%.2f %%", 100.0 * b->pbch_evm);
            igTableNextColumn();
            igText("%+.2f kHz", b->freq_offset_hz / 1e3);
        }
        igEndTable();
    }
    igPushTextWrapPos(0.0f);
    igTextUnformatted("The MIB is not decoded (polar code): the system frame number and the place of the SSB on the "
                      "carrier are not known.", NULL);
    igPopTextWrapPos();
}

static void draw_nr_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_nr_result *r = &res->nr;
    const struct m2sdr_nr_ssb *b;

    igSeparatorText("5G NR SSB");
    if (!res->nr_valid || !r->found || r->best < 0) {
        igTextUnformatted("No SSB.", NULL);
        return;
    }
    b = &r->ssb[r->best];
    if (igBeginTable("##nr", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("PCI", "%d (%d, %d)", b->pci, b->n_id_1, b->n_id_2);
        label_value("SSB index", "%d", b->ssb_index);
        label_value("Spacing", "%.0f kHz", r->scs_hz / 1e3);
        label_value("SSREF", "%.6f MHz", (res->set.freq_hz + r->freq_error_hz) / 1e6);
        label_value("Offset", "%+.1f Hz", r->freq_error_hz);
        label_value("SSBs, cells", "%d, %d", r->nssb, r->cells);
        label_value("SS-RSRP", "%.1f dBFS", b->rsrp_db);
        label_value("PSS", "%.1f dBFS", b->pss_db);
        label_value("SS-SINR", "%.1f dB", b->sinr_db);
        label_value("DM-RS EVM", "%.2f %%", 100.0 * b->dmrs_evm);
        label_value("PBCH EVM", "%.2f %%", 100.0 * b->pbch_evm);
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

static void nr_watch_prepare(struct an_settings *set)
{
    nr_track_prepare(set);
}

/* Mode */
/*------*/

const struct an_mode an_mode_nr = {
    .key            = "nr",
    .title          = "5G NR",
    .category       = AN_CAT_CELLULAR,
    .on             = offsetof(struct an_settings, nr_on),
    .valid          = offsetof(struct an_result, nr_valid),
    .found          = offsetof(struct an_result, nr.found),
    .report_rank    = 11,
    .default_rate   = AN_NR_RATE,
    .psd_short      = true,
    .samples        = nr_samples,
    .analyse        = nr_analyse,
    .report_text    = nr_report_text,
    .report_json    = nr_report_json,
    .watch_prepare  = nr_watch_prepare,
    .watch_line     = nr_watch_line,
    .options        = nr_options,
    .option         = nr_option,
    .help           = nr_help,
#ifdef M2SDR_ANALYZER_GUI
    .worker_prepare = nr_worker_prepare,
    .worker_result  = nr_track_result,
    .draw           = draw_nr,
    .results        = draw_nr_results,
    .keep           = nr_keep,
#endif
};
