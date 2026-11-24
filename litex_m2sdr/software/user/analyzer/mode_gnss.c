/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: GPS L1 C/A acquisition (satellites seen, C/N0, Doppler, code
 * phase) and jamming of the band, in a report, a view and --watch.
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

#define GNSS_RATE 2048000.0

/* Analysis */
/*----------*/

static int gnss_ms(const struct an_settings *set)
{
    return set->gnss_ms > 0 ? set->gnss_ms : AN_GNSS_DEFAULT_MS;
}

static size_t gnss_samples(const struct an_settings *set)
{
    return (size_t)ceil(1e-3 * gnss_ms(set) * set->sample_rate);
}

static void gnss_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    struct m2sdr_gnss_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.ms = gnss_ms(set);
    res->gnss_valid = m2sdr_gnss_analyse(x, count, set->sample_rate, &cfg, &res->gnss) == 0;
}

/* Reports */
/*---------*/

static void gnss_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_gnss_result *g = &res->gnss;
    const struct m2sdr_gnss_jamming *j = &g->jamming;
    int i;

    printf("\nGPS L1 C/A:\n");
    if (!res->gnss_valid) {
        printf("  The sample rate must be 2.048 or 4.096 MSPS, with %d ms of samples.\n", gnss_ms(&res->set));
        return;
    }
    if (fabs(res->set.freq_hz - M2SDR_GNSS_L1_HZ) > 10e3)
        printf("  Tuned to %.6f MHz, not to L1 (1575.42 MHz).\n", res->set.freq_hz / 1e6);
    printf("  Search     : %d PRNs, %d Doppler bins of %.0f Hz, %d ms, threshold %.1f dB%s\n", g->nprn, g->ndopplers,
           g->doppler_step_hz, g->ms, g->threshold_db, g->excised ? ", narrowband lines notched out first" : "");
    if (!g->found)
        printf("  Satellites : none detected (an active antenna needs a bias-T: the M2SDR has none)\n");
    else
        printf("  Satellites : %d detected\n  %4s %10s %10s %12s %8s\n", g->detected, "PRN", "C/N0", "Doppler", "code phase",
               "metric");
    for (i = 0; i < g->nprn; i++) {
        const struct m2sdr_gnss_prn *p = &g->prn[i];

        if (p->detected)
            printf("  %4d %7.1f dB-Hz %+8.1f Hz %8.2f chips %5.1f dB\n", p->prn, p->cn0_dbhz, p->doppler_hz,
                   p->code_phase_chips, p->metric_db);
    }
    printf("  Band       : %.1f %s, noise %.1f dBFS/Hz, flatness %.2f dB, J/N %.1f dB%s\n", j->power_dbfs + rep->off,
           rep->unit, j->noise_dbfs_hz, j->flatness_db, j->jn_db, j->jammed ? ", JAMMED" : "");
    for (i = 0; i < j->njammers; i++)
        printf("  Line       : %+.1f kHz, %.1f %s, J/N %.1f dB\n", j->jammer_hz[i] / 1e3, j->jammer_dbfs[i] + rep->off,
               rep->unit, j->jammer_jn_db[i]);
}

static void gnss_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_gnss_result *g = &res->gnss;
    const struct m2sdr_gnss_jamming *j = &g->jamming;
    int i, n = 0;

    printf("  \"gnss\": {\n");
    printf("    \"valid\": %s%s\n", res->gnss_valid ? "true" : "false", res->gnss_valid ? "," : "");
    if (!res->gnss_valid) {
        printf("  }");
        return;
    }
    printf("    \"found\": %s,\n    \"detected\": %d,\n    \"ms\": %d,\n", g->found ? "true" : "false", g->detected, g->ms);
    print_json_number("threshold_db", g->threshold_db, false);
    printf("    \"excised\": %d,\n", g->excised);
    printf("    \"prns\": [");
    for (i = 0; i < g->nprn; i++) {
        const struct m2sdr_gnss_prn *p = &g->prn[i];

        if (!p->detected)
            continue;
        printf("%s{\"prn\": %d, \"cn0_dbhz\": %.2f, \"doppler_hz\": %.1f, \"code_phase_chips\": %.3f, \"metric_db\": %.2f}",
               n++ ? ", " : "", p->prn, p->cn0_dbhz, p->doppler_hz, p->code_phase_chips, p->metric_db);
    }
    printf("],\n    \"jamming\": {\"jammed\": %s, \"power_db\": %.2f, \"noise_db_hz\": %.2f, \"flatness_db\": %.3f, "
           "\"jn_db\": %.2f, \"lines\": [", j->jammed ? "true" : "false", j->power_dbfs + rep->off, j->noise_dbfs_hz,
           j->flatness_db, j->jn_db);
    for (i = 0; i < j->njammers; i++)
        printf("%s{\"freq_hz\": %.1f, \"power_db\": %.2f, \"jn_db\": %.2f}", i ? ", " : "", j->jammer_hz[i],
               j->jammer_dbfs[i] + rep->off, j->jammer_jn_db[i]);
    printf("]}\n  }");
}

/* Watch */
/*-------*/

static unsigned gnss_watch_lines;

static int gnss_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_gnss_result *g = &res->gnss;
    const struct m2sdr_gnss_jamming *j = &g->jamming;
    const struct m2sdr_gnss_prn *best = g->best >= 0 ? &g->prn[g->best] : NULL;
    int i, n = 0;

    if (!res->gnss_valid) {
        fprintf(stderr, "The sample rate is not one of GPS L1 C/A here (2.048 or 4.096 MSPS).\n");
        return -1;
    }
    if (json) {
        printf("{\"time_s\": %.3f, \"detected\": %d, \"jammed\": %s, \"power_db\": %.2f, \"flatness_db\": %.3f, "
               "\"jn_db\": %.2f, \"prns\": [", now, g->detected, j->jammed ? "true" : "false",
               j->power_dbfs + set->ref_offset_db, j->flatness_db, j->jn_db);
        for (i = 0; i < g->nprn; i++)
            if (g->prn[i].detected)
                printf("%s{\"prn\": %d, \"cn0_dbhz\": %.1f, \"doppler_hz\": %.0f}", n++ ? ", " : "",
                       g->prn[i].prn, g->prn[i].cn0_dbhz, g->prn[i].doppler_hz);
        printf("]}\n");
    } else {
        if (gnss_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %4s %10s %8s %8s %6s %s\n%8s %4s %10s %8s %8s %6s\n", "time", "sats", "best C/N0", "level",
                   "flatness", "J/N", "PRN:C/N0", "s", "", "dB-Hz", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "dB", "dB");
        printf("%8.2f %4d", now, g->detected);
        if (best)
            printf(" %10.1f", best->cn0_dbhz);
        else
            printf(" %10s", "-");
        printf(" %8.1f %8.2f %6.1f", j->power_dbfs + set->ref_offset_db, j->flatness_db, j->jn_db);
        for (i = 0; i < g->nprn; i++)
            if (g->prn[i].detected)
                printf(" %d:%.0f", g->prn[i].prn, g->prn[i].cn0_dbhz);
        if (j->jammed)
            printf("  JAMMED");
        printf("\n");
    }
    return 0;
}

/* Command line */
/*--------------*/

static const struct option gnss_options[] = {
    { "gnss",    no_argument,       NULL, 0 },
    { "gnss-ms", required_argument, NULL, 1 },
    { NULL, 0, NULL, 0 }
};

static int gnss_option(int id, const char *arg, struct an_cli *cli)
{
    switch (id) {
    case 0:
        cli->set->gnss_on = true;
        return 0;
    case 1:
        if (m2sdr_cli_parse_int_range(arg, 1, 100, &cli->set->gnss_ms) != 0) {
            m2sdr_cli_error("invalid GNSS integration '%s' (expected 1 to 100 ms)", arg);
            return -1;
        }
        return 0;
    default:
        return -1;
    }
}

static void gnss_help(void)
{
    printf("GPS L1 options:\n"
           "      --gnss             With --report: GPS L1 C/A acquisition (PRN, C/N0, Doppler, code\n"
           "                         phase) and jamming of the band, tuned to 1575.42 MHz at 2.048 MSPS\n"
           "                         (the default with --gnss) or 4.096 MSPS. --watch works too.\n"
           "      --gnss-ms N        Milliseconds integrated (default: %d).\n"
           "\n", AN_GNSS_DEFAULT_MS);
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

static void draw_gnss(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_gnss_result *g = &res->gnss;
    const struct m2sdr_gnss_jamming *j = &g->jamming;
    static float spec[M2SDR_GNSS_SPECTRUM];
    struct m2sdr_plot plot;
    ImVec2 avail;
    char text[160], title[160];
    int i;

    if (live) {
        if (fabs(ui->set.freq_hz - M2SDR_GNSS_L1_HZ) > 1.0) {
            if (igSmallButton("L1 1575.42 MHz"))
                view_retune(ui, M2SDR_GNSS_L1_HZ, changed);
            igSameLine(0.0f, 16.0f);
        }
        if (fabs(ui->set.sample_rate - GNSS_RATE) > 1.0 && fabs(ui->set.sample_rate - 2.0 * GNSS_RATE) > 1.0) {
            if (igSmallButton("2.048 MSPS"))
                view_rate(ui, GNSS_RATE, changed);
            igSameLine(0.0f, 16.0f);
        }
    }
    {
        int ms = gnss_ms(&ui->set);

        igSetNextItemWidth(100.0f);
        if (igInputInt("ms integrated", &ms, 5, 10, 0)) {
            ui->set.gnss_ms = ms < 1 ? 1 : ms > 100 ? 100 : ms;
            *changed = true;
        }
    }
    igSameLine(0.0f, 16.0f);
    igTextDisabled("An active antenna needs a bias-T: the M2SDR has none.");
    if (!res->set.gnss_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->gnss_valid) {
        igTextUnformatted("The sample rate must be 2.048 or 4.096 MSPS.", NULL);
        return;
    }
    igGetContentRegionAvail(&avail);

    /* C/N0 of each PRN searched: detected ones in colour, the others at their metric, grey. */
    if (m2sdr_plot_begin(&plot, "##gnsscn0", (ImVec2){ 0.62f * avail.x, 0.36f * avail.y }, 0.5, g->nprn + 0.5,
                         M2SDR_PLOT_PLAIN, 20.0, 55.0, "C/N0 (dB-Hz) per PRN searched; grey: not detected")) {
        for (i = 0; i < g->nprn; i++) {
            const struct m2sdr_gnss_prn *p = &g->prn[i];
            /* An undetected PRN at the C/N0 its peak would give: r = 1 + C/N0 * 1 ms, r the peak over the floor. */
            const double r = pow(10.0, p->metric_db / 10.0);
            const double v = p->detected ? p->cn0_dbhz : 10.0 * log10(fmax(r - 1.0, 1e-3) / 1e-3);
            const ImVec2 a = { m2sdr_plot_x(&plot, i + 0.65), m2sdr_plot_y(&plot, fmin(fmax(v, 20.0), 55.0)) };
            const ImVec2 b = { m2sdr_plot_x(&plot, i + 1.35), m2sdr_plot_y(&plot, 20.0) };

            ImDrawList_AddRectFilled(plot.dl, a, b, p->detected ? 0xff40d040u : 0x60a0a0a0u, 0.0f, 0);
            snprintf(text, sizeof(text), "%d", p->prn);
            ImDrawList_AddText_Vec2(plot.dl, (ImVec2){ a.x - 2.0f, b.y - 16.0f }, 0xffffffffu, text, NULL);
        }
        m2sdr_plot_end(&plot);
    }
    igSameLine(0.0f, 8.0f);
    /* Table of the detected PRNs. */
    igBeginChild_Str("##gnsstable", (ImVec2){ 0.0f, 0.36f * avail.y }, ImGuiChildFlags_None, 0);
    if (igBeginTable("##gnssprn", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        igTableSetupColumn("PRN", 0, 0.0f, 0);
        igTableSetupColumn("C/N0 dB-Hz", 0, 0.0f, 0);
        igTableSetupColumn("Doppler Hz", 0, 0.0f, 0);
        igTableSetupColumn("Code chips", 0, 0.0f, 0);
        igTableHeadersRow();
        for (i = 0; i < g->nprn; i++) {
            const struct m2sdr_gnss_prn *p = &g->prn[i];

            if (!p->detected)
                continue;
            igTableNextRow(0, 0.0f);
            igTableNextColumn();
            igText("%d", p->prn);
            igTableNextColumn();
            igText("%.1f", p->cn0_dbhz);
            igTableNextColumn();
            igText("%+.0f", p->doppler_hz);
            igTableNextColumn();
            igText("%.2f", p->code_phase_chips);
        }
        igEndTable();
    }
    if (!g->found)
        igTextWrapped("No satellite detected (threshold %.1f dB over %d ms).", g->threshold_db, g->ms);
    igEndChild();

    /* The strongest: its correlation against the code phase and the Doppler. */
    if (g->best >= 0) {
        const struct m2sdr_gnss_prn *p = &g->prn[g->best];

        snprintf(text, sizeof(text), "PRN %d: correlation (dB over the floor) against the code phase, chips", p->prn);
        if (m2sdr_plot_begin(&plot, "##gnsscode", (ImVec2){ 0.62f * avail.x, 0.27f * avail.y }, 0.0,
                             M2SDR_GNSS_CA_CHIPS, M2SDR_PLOT_PLAIN, -5.0, fmax(20.0, p->metric_db + 3.0), text)) {
            m2sdr_plot_trace(&plot, g->code_db, M2SDR_GNSS_CA_CHIPS, 0.0, M2SDR_GNSS_CA_CHIPS - 1, 0xff40d0ffu);
            m2sdr_plot_hline(&plot, g->threshold_db, 0x80ff8080u);
            m2sdr_plot_end(&plot);
        }
        igSameLine(0.0f, 8.0f);
        if (m2sdr_plot_begin(&plot, "##gnssdop", (ImVec2){ 0.0f, 0.27f * avail.y }, g->doppler_start_hz,
                             g->doppler_start_hz + (g->ndopplers - 1) * g->doppler_step_hz, M2SDR_PLOT_FREQ, -5.0,
                             fmax(20.0, p->metric_db + 3.0), "against the Doppler")) {
            m2sdr_plot_trace(&plot, g->doppler_db, (size_t)g->ndopplers, g->doppler_start_hz,
                             g->doppler_start_hz + (g->ndopplers - 1) * g->doppler_step_hz, 0xff40d0ffu);
            m2sdr_plot_vline(&plot, p->doppler_hz, 0x80ffffffu);
            m2sdr_plot_end(&plot);
        }
    }

    /* Jamming: the spectrum of the band over its noise floor, the lines found. */
    for (i = 0; i < j->spectrum_points; i++)
        spec[i] = j->spectrum_db[i];
    snprintf(title, sizeof(title), "Band, dB over the noise floor: flatness %.2f dB, J/N %.1f dB%s", j->flatness_db, j->jn_db,
             j->jammed ? " - JAMMED" : "");
    if (m2sdr_plot_begin(&plot, "##gnssjam", (ImVec2){ 0.0f, 0.0f }, j->spectrum_start_hz,
                         j->spectrum_start_hz + (j->spectrum_points - 1) * j->spectrum_step_hz, M2SDR_PLOT_FREQ, -6.0,
                         j->njammers ? fmax(15.0, j->jammer_jn_db[0] + 30.0) : 15.0, title)) {
        m2sdr_plot_band(&plot, -0.5 * j->band_hz, 0.5 * j->band_hz, 0x10ffffffu);
        m2sdr_plot_trace(&plot, spec, (size_t)j->spectrum_points, j->spectrum_start_hz,
                         j->spectrum_start_hz + (j->spectrum_points - 1) * j->spectrum_step_hz,
                         j->jammed ? 0xff4040ffu : 0xff40d040u);
        for (i = 0; i < j->njammers; i++) {
            snprintf(text, sizeof(text), "J/N %.1f dB", j->jammer_jn_db[i]);
            m2sdr_plot_marker(&plot, j->jammer_hz[i], j->spectrum_db[(int)((j->jammer_hz[i] - j->spectrum_start_hz) /
                              j->spectrum_step_hz + 0.5) % j->spectrum_points], text, 0xffc0c0ffu);
        }
        m2sdr_plot_end(&plot);
    }
}

static void draw_gnss_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_gnss_result *g = &res->gnss;
    const struct m2sdr_gnss_jamming *j = &g->jamming;

    igSeparatorText("GPS L1 C/A");
    if (!res->gnss_valid) {
        igTextUnformatted("Not measured.", NULL);
        return;
    }
    if (igBeginTable("##gnss", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Satellites", "%d of %d", g->detected, g->nprn);
        if (g->best >= 0)
            label_value("Strongest", "PRN %d, %.1f dB-Hz", g->prn[g->best].prn, g->prn[g->best].cn0_dbhz);
        label_value("Integration", "%d ms, threshold %.1f dB", g->ms, g->threshold_db);
        label_value("Level", "%.1f dBFS", j->power_dbfs);
        label_value("Noise", "%.1f dBFS/Hz", j->noise_dbfs_hz);
        label_value("Flatness", "%.2f dB", j->flatness_db);
        label_value("J/N", "%.1f dB", j->jn_db);
        label_value("Lines", "%d%s", j->njammers, g->excised ? ", notched out" : "");
        label_value("Jamming", "%s", j->jammed ? "yes" : "no");
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_gnss = {
    .key          = "gnss",
    .title        = "GPS L1",
    .category     = AN_CAT_TIMING,
    .on           = offsetof(struct an_settings, gnss_on),
    .valid        = offsetof(struct an_result, gnss_valid),
    .found        = offsetof(struct an_result, gnss.found),
    .report_rank  = 71,
    .default_rate = GNSS_RATE,
    .default_freq = 1575.42e6,
    .options      = gnss_options,
    .option       = gnss_option,
    .help         = gnss_help,
    .samples      = gnss_samples,
    .analyse      = gnss_analyse,
    .report_text  = gnss_report_text,
    .report_json  = gnss_report_json,
    .watch_line   = gnss_watch_line,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_gnss,
    .results      = draw_gnss_results,
#endif
};
