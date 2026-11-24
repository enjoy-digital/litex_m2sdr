/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: TETRA base station carrier (slots, MER, SYNC PDU), in a report,
 * a view and --watch.
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

#include "analyzer.h"

static const char *const tetra_burst_names[4] = { "none", "other", "normal", "sync" };

/* Analysis */
/*----------*/

static size_t tetra_samples(const struct an_settings *set)
{
    return (size_t)(AN_TETRA_SECONDS * set->sample_rate);
}

static void tetra_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = (size_t)(AN_TETRA_SECONDS * set->sample_rate);

    res->tetra_valid = m2sdr_tetra_analyse(x, count < n ? count : n, set->sample_rate, &res->tetra) == 0;
}

/* Timeslot (1 to 4) of slot s of the capture, from the SYNC PDU; 0 without it. */
static int tetra_tn(const struct m2sdr_tetra_result *t, int s)
{
    if (!t->sync)
        return 0;
    return ((t->tn - 1 + s - t->sync_slot) % 4 + 4) % 4 + 1;
}

/* Reports */
/*---------*/

static void tetra_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_tetra_result *t = &res->tetra;
    int s;

    printf("\nTETRA:\n");
    if (!res->tetra_valid) {
        printf("  The sample rate must be 144 kSPS to 4 MSPS, with at least three slots of samples.\n");
        return;
    }
    if (!t->found) {
        printf("  No slot boundaries (training correlation %.3f): tune on a base station carrier.\n", t->sync_quality);
        return;
    }
    printf("  Slots      : %d, training correlation %.2f, carrier %+.1f Hz from the centre\n", t->nslots, t->sync_quality,
           t->freq_error_hz);
    printf("  Bursts     : %d normal, %d synchronisation\n", t->normal_bursts, t->sync_bursts);
    printf("  MER        : %.1f dB, EVM %.2f %% RMS, %.1f %% peak\n", t->mer_db, 100.0 * t->evm_rms, 100.0 * t->evm_peak);
    if (t->sync) {
        printf("  SYNC PDU   : colour code %d, MCC %d, MNC %d, system code %d\n", t->colour_code, t->mcc, t->mnc, t->system_code);
        printf("               timeslot %d, frame %d, multiframe %d (slot %d of the capture), %d decoded\n", t->tn, t->fn, t->mn,
               t->sync_slot, t->sync_decoded);
        printf("               sharing %d, reserved frames %d, U-plane DTX %d, frame 18 extension %d, neighbour broadcast %d, "
               "cell load %d, late entry %d\n", t->sharing_mode, t->ts_reserved_frames, t->u_plane_dtx, t->frame18_ext,
               t->neighbour_broadcast, t->cell_load, t->late_entry);
    } else {
        printf("  SYNC PDU   : %s\n", t->sync_bursts ? "synchronisation burst found, its BSCH not decoded (CRC)" :
                                                    "no synchronisation burst (once a multiframe, 1.02 s)");
    }
    for (s = 0; s < t->nslots; s++) {
        printf("    slot %2d", s);
        if (t->sync)
            printf("  TN %d", tetra_tn(t, s));
        printf("  %-7s %6.1f %s  MER %5.1f dB\n", tetra_burst_names[t->slot_burst[s] & 3], t->slot_power_db[s] + rep->off,
               rep->unit, t->slot_mer_db[s]);
    }
}

static void tetra_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_tetra_result *t = &res->tetra;
    const bool ok = res->tetra_valid && t->found;
    int s;

    printf("  \"tetra\": {\n");
    print_json_number("sync_quality", res->tetra_valid ? t->sync_quality : 0.0, false);
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        print_json_number("power_db", t->power_dbfs + rep->off, false);
        print_json_number("carrier_offset_hz", t->freq_error_hz, false);
        printf("    \"slots\": %d,\n    \"normal_bursts\": %d,\n    \"sync_bursts\": %d,\n", t->nslots, t->normal_bursts,
               t->sync_bursts);
        print_json_number("mer_db", t->mer_db, false);
        print_json_number("evm", t->evm_rms, false);
        printf("    \"sync\": %s,\n", t->sync ? "true" : "false");
        if (t->sync)
            printf("    \"colour_code\": %d,\n    \"mcc\": %d,\n    \"mnc\": %d,\n    \"system_code\": %d,\n    \"tn\": %d,\n"
                   "    \"fn\": %d,\n    \"mn\": %d,\n    \"sync_slot\": %d,\n", t->colour_code, t->mcc, t->mnc,
                   t->system_code, t->tn, t->fn, t->mn, t->sync_slot);
        printf("    \"slot\": [");
        for (s = 0; s < t->nslots; s++)
            printf("%s\n      { \"burst\": \"%s\", \"training\": %d, \"power_db\": %.2f, \"mer_db\": %.2f }", s ? "," : "",
                   tetra_burst_names[t->slot_burst[s] & 3], t->slot_training[s], t->slot_power_db[s] + rep->off,
                   t->slot_mer_db[s]);
        printf("%s]\n", t->nslots ? "\n    " : "");
    }
    printf("  }");
}

/* Watch */
/*-------*/

static unsigned tetra_watch_lines;

static int tetra_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_tetra_result *t = &res->tetra;

    if (!res->tetra_valid) {
        fprintf(stderr, "The sample rate is not one of TETRA (144 kSPS to 4 MSPS).\n");
        return -1;
    }
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, t->found ? "true" : "false");
        if (t->found)
            printf(", \"mer_db\": %.2f, \"power_db\": %.2f, \"carrier_offset_hz\": %.1f, \"sync_bursts\": %d", t->mer_db,
                   t->power_dbfs + set->ref_offset_db, t->freq_error_hz, t->sync_bursts);
        if (t->sync)
            printf(", \"colour_code\": %d, \"mcc\": %d, \"mnc\": %d", t->colour_code, t->mcc, t->mnc);
        printf("}\n");
        return 0;
    }
    if (tetra_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
        printf("%8s %7s %8s %10s %5s %5s %6s %4s\n%8s %7s %8s %10s %5s %5s %6s %4s\n", "time", "MER", "level", "offset",
               "MCC", "MNC", "colour", "sync", "s", "dB", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "Hz", "", "", "", "");
    if (!t->found) {
        printf("%8.2f  no slots (training correlation %.3f)\n", now, t->sync_quality);
    } else if (t->sync) {
        printf("%8.2f %7.1f %8.1f %+10.1f %5d %5d %6d %4d\n", now, t->mer_db, t->power_dbfs + set->ref_offset_db,
               t->freq_error_hz, t->mcc, t->mnc, t->colour_code, t->sync_bursts);
    } else {
        printf("%8.2f %7.1f %8.1f %+10.1f %5s %5s %6s %4d\n", now, t->mer_db, t->power_dbfs + set->ref_offset_db,
               t->freq_error_hz, "-", "-", "-", t->sync_bursts);
    }
    return 0;
}

/* Command line */
/*--------------*/

static const struct option tetra_options[] = {
    { "tetra", no_argument, NULL, 0 },
    { NULL, 0, NULL, 0 }
};

static int tetra_option(int id, const char *arg, struct an_cli *cli)
{
    (void)id;
    (void)arg;
    cli->set->tetra_on = true;
    return 0;
}

static void tetra_help(void)
{
    printf("TETRA options:\n"
           "      --tetra            With --report: a TETRA base station carrier: slots, MER, SYNC PDU\n"
           "                         (colour code, MCC, MNC); captures of 1.1 s (default rate 1 MSPS).\n"
           "                         --watch works too.\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

/* Points of a plane with crosses on the ideal ones (n of them, nref crosses). */
static void tetra_points(const char *id, ImVec2 size, const char *label, const float complex *p, int n,
                      const float complex *ref, int nref, ImU32 color)
{
    static float px[M2SDR_TETRA_MAX_POINTS], py[M2SDR_TETRA_MAX_POINTS];
    struct m2sdr_plot plot;
    int i;

    if (!m2sdr_plot_begin(&plot, id, size, -1.6, 1.6, M2SDR_PLOT_PLAIN, -1.6, 1.6, label))
        return;
    for (i = 0; i < n && i < M2SDR_TETRA_MAX_POINTS; i++) {
        px[i] = crealf(p[i]);
        py[i] = cimagf(p[i]);
    }
    m2sdr_plot_points(&plot, px, py, (size_t)i, 1.0f, color);
    for (i = 0; i < nref; i++) {
        const float x = m2sdr_plot_x(&plot, crealf(ref[i])), y = m2sdr_plot_y(&plot, cimagf(ref[i]));

        ImDrawList_AddLine(plot.dl, (ImVec2){ x - 4.0f, y }, (ImVec2){ x + 5.0f, y }, 0xa0ffffffu, 1.0f);
        ImDrawList_AddLine(plot.dl, (ImVec2){ x, y - 4.0f }, (ImVec2){ x, y + 5.0f }, 0xa0ffffffu, 1.0f);
    }
    m2sdr_plot_end(&plot);
}

/* SYNC PDU of the last capture that had one: one comes every 1.02 s. */
static struct m2sdr_tetra_result tetra_last_sync;
static bool tetra_have_sync;
static double tetra_sync_freq_hz;

static void tetra_keep(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;

    if (!res->set.tetra_on || !res->tetra_valid)
        return;
    if (res->set.freq_hz != tetra_sync_freq_hz)
        tetra_have_sync = false;
    if (res->tetra.sync) {
        tetra_last_sync    = res->tetra;
        tetra_have_sync    = true;
        tetra_sync_freq_hz = res->set.freq_hz;
    }
}

static void draw_tetra(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_tetra_result *t = &res->tetra;
    static float pw[M2SDR_TETRA_MAX_SLOTS], mer[M2SDR_TETRA_MAX_SLOTS];
    struct m2sdr_plot plot;
    ImVec2 avail, sq;
    float side;
    char text[160];
    int s;

    if (live && fabs(ui->set.sample_rate - AN_TETRA_RATE) > 1.0) {
        if (igSmallButton("1 MSPS"))
            view_rate(ui, AN_TETRA_RATE, changed);
        igSameLine(0.0f, 16.0f);
    }
    igPushTextWrapPos(0.0f);
    igText("Captures of %.1f s: the synchronisation burst (BSCH) comes once a multiframe, 1.02 s.", AN_TETRA_SECONDS);
    igPopTextWrapPos();
    if (!res->set.tetra_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->tetra_valid) {
        igTextUnformatted("The sample rate must be 144 kSPS to 4 MSPS (1 MSPS).", NULL);
        return;
    }
    if (!t->found) {
        igText("No slot boundaries (training correlation %.3f): tune on a base station carrier.", t->sync_quality);
        return;
    }
    if (live && fabs(t->freq_error_hz) > 1000.0) {
        snprintf(text, sizeof(text), "Centre on the carrier (%+.2f kHz)", t->freq_error_hz / 1e3);
        if (igSmallButton(text))
            view_retune(ui, ui->set.freq_hz + 1e3 * round(t->freq_error_hz / 1e3), changed);
    }

    igGetContentRegionAvail(&avail);
    side = 0.55f * avail.y < 0.4f * avail.x ? 0.55f * avail.y : 0.4f * avail.x;
    sq   = m2sdr_plot_square(side);
    /* Power and MER of each slot. */
    for (s = 0; s < t->nslots; s++) {
        pw[s]  = t->slot_power_db[s];
        mer[s] = t->slot_burst[s] == M2SDR_TETRA_BURST_NONE ? NAN : t->slot_mer_db[s];
    }
    snprintf(text, sizeof(text), "Slots: power (dBFS, orange) and MER (dB, blue); MER %.1f dB", t->mer_db);
    if (m2sdr_plot_begin(&plot, "##tetraslots", (ImVec2){ avail.x - sq.x - 8.0f, sq.y }, -0.5, t->nslots - 0.5,
                         M2SDR_PLOT_PLAIN, -60.0, 50.0, text)) {
        for (s = 0; s < t->nslots; s++) {
            const ImU32 col = t->slot_burst[s] == M2SDR_TETRA_BURST_SYNC ? 0x6040ff40u :
                              t->slot_burst[s] == M2SDR_TETRA_BURST_NONE ? 0x00000000u : 0x18ffffffu;

            m2sdr_plot_band(&plot, s - 0.45, s + 0.45, col);
        }
        m2sdr_plot_hline(&plot, 0.0, 0x60ffffffu);
        m2sdr_plot_trace(&plot, pw, (size_t)t->nslots, 0.0, t->nslots - 1, 0xff40a0ffu);
        m2sdr_plot_trace(&plot, mer, (size_t)t->nslots, 0.0, t->nslots - 1, 0xffffc040u);
        if (plot.hovered) {
            s = (int)lround(plot.mouse_x);
            if (s >= 0 && s < t->nslots) {
                snprintf(text, sizeof(text), "slot %d: %s, %.1f dBFS, MER %.1f dB", s, tetra_burst_names[t->slot_burst[s] & 3],
                         t->slot_power_db[s], t->slot_mer_db[s]);
                m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
            }
        }
        m2sdr_plot_end(&plot);
    }
    igSameLine(0.0f, 8.0f);
    {
        static float complex ref[256];
        const int nref = m2sdr_constellation(M2SDR_MOD_QPSK, ref);

        tetra_points("##tetraconst", sq, "Symbols turned back by pi/4 each, equalised", t->points, t->npoints, ref, nref,
                     0x90ffd080u);
    }

    /* Slot table. */
    if (igBeginTable("##tetraslottab", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                     (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        igTableSetupColumn("Slot", 0, 0.0f, 0);
        igTableSetupColumn("TN", 0, 0.0f, 0);
        igTableSetupColumn("Burst", 0, 0.0f, 0);
        igTableSetupColumn("Training", 0, 0.0f, 0);
        igTableSetupColumn("Power", 0, 0.0f, 0);
        igTableSetupColumn("MER", 0, 0.0f, 0);
        igTableHeadersRow();
        for (s = 0; s < t->nslots; s++) {
            igTableNextRow(0, 0.0f);
            igTableNextColumn();
            igText("%d", s);
            igTableNextColumn();
            if (t->sync)
                igText("%d", tetra_tn(t, s));
            igTableNextColumn();
            igTextUnformatted(tetra_burst_names[t->slot_burst[s] & 3], NULL);
            igTableNextColumn();
            if (t->slot_training[s])
                igText("%d", t->slot_training[s]);
            igTableNextColumn();
            igText("%.1f dBFS", t->slot_power_db[s]);
            igTableNextColumn();
            if (t->slot_burst[s] != M2SDR_TETRA_BURST_NONE)
                igText("%.1f dB", t->slot_mer_db[s]);
        }
        igEndTable();
    }
}

static void draw_tetra_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_tetra_result *t = &res->tetra;
    const struct m2sdr_tetra_result *p = t->sync ? t : tetra_have_sync ? &tetra_last_sync : NULL;

    igSeparatorText("TETRA");
    if (!res->tetra_valid || !t->found) {
        igTextUnformatted("No carrier.", NULL);
        return;
    }
    if (igBeginTable("##tetra", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Carrier", "%.6f MHz", (res->set.freq_hz + t->freq_error_hz) / 1e6);
        label_value("Offset", "%+.1f Hz", t->freq_error_hz);
        label_value("Power", "%.1f dBFS", t->power_dbfs);
        label_value("Slots", "%d (%.2f)", t->nslots, t->sync_quality);
        label_value("Bursts", "%d normal, %d sync", t->normal_bursts, t->sync_bursts);
        label_value("MER", "%.1f dB", t->mer_db);
        label_value("EVM", "%.2f %% RMS, %.1f %% peak", 100.0 * t->evm_rms, 100.0 * t->evm_peak);
        igEndTable();
    }
    igSeparatorText(p == t ? "SYNC PDU" : p ? "SYNC PDU (last capture with one)" : "SYNC PDU");
    if (!p) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted(t->sync_bursts ? "Synchronisation burst found, its BSCH not decoded (CRC)." :
                          "No synchronisation burst yet.", NULL);
        igPopTextWrapPos();
        return;
    }
    if (igBeginTable("##tetrasync", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("MCC, MNC", "%d, %d", p->mcc, p->mnc);
        label_value("Colour code", "%d", p->colour_code);
        label_value("System code", "%d", p->system_code);
        label_value("TN, FN, MN", "%d, %d, %d", p->tn, p->fn, p->mn);
        label_value("Sharing mode", "%d", p->sharing_mode);
        label_value("Reserved frames", "%d", p->ts_reserved_frames);
        label_value("U-plane DTX", "%d", p->u_plane_dtx);
        label_value("Frame 18 ext.", "%d", p->frame18_ext);
        label_value("Neighbour bcast", "%d", p->neighbour_broadcast);
        label_value("Cell load", "%d", p->cell_load);
        label_value("Late entry", "%d", p->late_entry);
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_tetra = {
    .key          = "tetra",
    .title        = "TETRA",
    .category     = AN_CAT_CELLULAR,
    .on           = offsetof(struct an_settings, tetra_on),
    .valid        = offsetof(struct an_result, tetra_valid),
    .report_rank  = 13,
    .default_rate = AN_TETRA_RATE,
    .psd_short    = true,
    .samples      = tetra_samples,
    .analyse      = tetra_analyse,
    .report_text  = tetra_report_text,
    .report_json  = tetra_report_json,
    .watch_line   = tetra_watch_line,
    .options      = tetra_options,
    .option       = tetra_option,
    .help         = tetra_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_tetra,
    .results      = draw_tetra_results,
    .keep         = tetra_keep,
#endif
};
