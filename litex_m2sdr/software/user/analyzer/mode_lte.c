/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: LTE downlink carrier, FDD with a normal cyclic prefix (cell, MIB,
 * levels, EVM, resource grid, blocks of data).
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
#include <string.h>

#include "m2sdr_cli.h"

#include "analyzer.h"

/* Settings and result */
/*-----------------------*/

extern const struct an_mode an_mode_lte;

/* Its settings: its block of struct an_settings, read with SET(). */
struct lte_set {
    bool   lte_on;                 /* LTE downlink carrier. */
};

#define SET(set) ((struct lte_set *)an_mode_settings(&an_mode_lte, (set)))

/* The result of the mode: its block of struct an_result, read with RES(). */
struct lte_res {
    bool   lte_valid;              /* Measured: the sample rate was one of LTE; lte.found says the rest. */
    struct m2sdr_lte_result lte;
};

#define RES(res) ((struct lte_res *)an_mode_result(&an_mode_lte, (res)))

/* Analysis */
/*----------*/

static size_t lte_samples(const struct an_settings *set)
{
    return (size_t)(AN_LTE_SECONDS * set->sample_rate);
}

static void lte_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = (size_t)(AN_LTE_SECONDS * set->sample_rate);

    RES(res)->lte_valid = m2sdr_lte_analyse(x, count < n ? count : n, set->sample_rate, &RES(res)->lte) == 0;
}

/* Reports */
/*---------*/

static void lte_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const char *unit = rep->unit;
    const struct m2sdr_lte_result *l = &RES(res)->lte;
    static const char *const phich[4] = { "1/6", "1/2", "1", "2" };

    printf("\nLTE downlink:\n");
    if (!RES(res)->lte_valid) {
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

static void lte_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const struct m2sdr_lte_result *l = &RES(res)->lte;
    static const char *const phich[4] = { "1/6", "1/2", "1", "2" };

    printf("  \"lte\": {\n");
    printf("    \"found\": %s%s\n", RES(res)->lte_valid && l->found ? "true" : "false", RES(res)->lte_valid && l->found ? "," : "");
    if (RES(res)->lte_valid && l->found) {
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
    printf("  }");
}

/* Command line */
/*--------------*/

static const struct option lte_options[] = {
    { "lte", no_argument, NULL, 0 },
    { NULL, 0, NULL, 0 }
};

static int lte_option(int id, const char *arg, struct an_cli *cli)
{
    (void)id;
    (void)arg;
    SET(cli->set)->lte_on = true;
    return 0;
}

static void lte_help(void)
{
    printf("LTE options:\n"
           "      --lte              With --report: analyse an LTE downlink carrier, FDD with a normal\n"
           "                         cyclic prefix, tuned on its centre: cell, MIB, EVM, data blocks.\n"
           "                         The sample rate is 1.92, 3.84, 7.68, 15.36 or 30.72 MSPS, for\n"
           "                         6, 15, 25, 50 or 100 resource blocks around the centre.\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

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
#define LTE_NARROW_W       700.0f    /* Under this width of the view, the constellation goes under the grid. */
#define AN_LTE_HOLD        10        /* Captures a frame stays shown without a cell, or its symbols without data. */

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
    const struct m2sdr_lte_result *l = &RES(&ui->res)->lte;
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
    const struct m2sdr_lte_result *l = &RES(&ui->res)->lte;

    if (!SET(&ui->res.set)->lte_on || !RES(&ui->res)->lte_valid)
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

static void lte_close(struct an_ui *ui)
{
    free(ui->lte_rgba);
    free(ui->lte_last);
}

/* The frame shown: the one of the last capture, or the last one with a cell for a few captures. */
static const struct m2sdr_lte_result *lte_shown(const struct an_ui *ui, bool *held)
{
    *held = false;
    if (!SET(&ui->shown->set)->lte_on || !RES(ui->shown)->lte_valid)
        return NULL;
    if (RES(ui->shown)->lte.found)
        return &RES(ui->shown)->lte;
    if (ui->lte_last_ok && ui->seq - ui->lte_last_seq <= AN_LTE_HOLD) {
        *held = true;
        return ui->lte_last;
    }
    return NULL;
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
    bool held, narrow;
    int i;

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
    if (!SET(&ui->res.set)->lte_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!RES(&ui->res)->lte_valid) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted("The sample rate must be 1.92, 3.84, 7.68, 15.36 or 30.72 MSPS: it sets how many resource "
                          "blocks around the centre are analysed.", NULL);
        igPopTextWrapPos();
        return;
    }
    l = lte_shown(ui, &held);
    if (!l) {
        igPushTextWrapPos(0.0f);
        igText("No cell found (PSS quality %.1f): the capture must be tuned on the centre of an FDD carrier with a "
               "normal cyclic prefix.", RES(&ui->res)->lte.sync_quality);
        igPopTextWrapPos();
        return;
    }
    tex._TexData = NULL;
    half_mhz     = 0.5 * 0.015 * (double)l->subcarriers;

    igGetContentRegionAvail(&avail);
    line   = igGetTextLineHeightWithSpacing();
    /* The constellation next to the grid, or under it in a narrow window. */
    narrow = avail.x < LTE_NARROW_W;
    if (narrow) {
        side   = 0.36f * avail.y < avail.x - 40.0f ? 0.36f * avail.y : avail.x - 40.0f;
        sq     = m2sdr_plot_square(side);
        left_w = avail.x;
    } else {
        side   = avail.y - 2.0f * line < 0.45f * avail.x ? avail.y - 2.0f * line : 0.45f * avail.x;
        sq     = m2sdr_plot_square(side);
        left_w = avail.x - sq.x - 8.0f;
    }

    /* Resource grid, and the blocks of data under it on the same time axis. */
    igBeginGroup();
    igGetCursorScreenPos(&pos);
    right = pos.x + left_w;
    igPushTextWrapPos(igGetCursorPosX() + left_w);
    if (held)
        igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "No cell in the last capture (PSS quality %.1f): the last frame read "
                      "is held, with its figures.", RES(&ui->res)->lte.sync_quality);
    else if (l->mib)
        igText("Resource grid of frame %d, MHz from the centre against the time. Colours: dB over the reference signals.", l->sfn);
    else
        igTextUnformatted("Resource grid of a frame, MHz from the centre against the time. Colours: dB over the reference signals.", NULL);
    igPopTextWrapPos();
    if (m2sdr_plot_begin(&plot, "##ltegrid", (ImVec2){ left_w - LTE_SCALE_W, (narrow ? 0.32f : 0.62f) * avail.y }, 0.0, 0.010,
                         M2SDR_PLOT_TIME, -half_mhz, half_mhz, "")) {
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

        igPushTextWrapPos(igGetCursorPosX() + left_w);
        igTextUnformatted("Blocks of data, by slot and resource block of the cell:", NULL);
        igPopTextWrapPos();
        legend_item(lte_mod_colors[M2SDR_MOD_QPSK], "QPSK", right);
        legend_item(lte_mod_colors[M2SDR_MOD_16QAM], "16QAM", right);
        legend_item(lte_mod_colors[M2SDR_MOD_64QAM], "64QAM", right);
        legend_item(lte_mod_colors[M2SDR_MOD_256QAM], "256QAM", right);
        if (l->ports > 1)
            legend_item(LTE_COL_UNREAD, "in use, not read", right);
        legend_item(lte_mod_colors[M2SDR_MOD_AUTO], "empty", right);
        igNewLine();
        if (m2sdr_plot_begin(&plot, "##lteblocks", (ImVec2){ left_w - LTE_SCALE_W, narrow ? 0.18f * avail.y : 0.0f }, 0.0, 0.010,
                             M2SDR_PLOT_TIME, y0, y0 + (double)l->subcarriers / 12.0, "")) {
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
    if (!narrow)
        igSameLine(0.0f, 8.0f);

    /* Equalised symbols: data, PBCH and reference signals. */
    igBeginGroup();
    igGetCursorScreenPos(&pos);
    right = pos.x + (narrow ? avail.x : sq.x);
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
    const struct an_result *res = ui->shown;
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

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_lte = {
    .key           = "lte",
    .title         = "LTE",
    .category      = AN_CAT_CELLULAR,
    .settings_size = sizeof(struct lte_set),
    .on            = offsetof(struct lte_set, lte_on),
    .result_size   = sizeof(struct lte_res),
    .valid         = offsetof(struct lte_res, lte_valid),
    .found         = offsetof(struct lte_res, lte.found),
    .report_rank   = 10,
    .samples       = lte_samples,
    .analyse       = lte_analyse,
    .report_text   = lte_report_text,
    .report_json   = lte_report_json,
    .options       = lte_options,
    .option        = lte_option,
    .help          = lte_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw          = draw_lte,
    .results       = draw_lte_results,
    .keep          = lte_keep,
    .close         = lte_close,
#endif
};
