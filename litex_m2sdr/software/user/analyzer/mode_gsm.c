/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: GSM BCCH carrier (FCCH, SCH: BSIC, frame number, phase error),
 * in a report, a view and --watch.
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

/* Bands */
/*-------*/

/* Downlink of the ARFCNs of a band (TS 45.005 2): first and last ARFCN, frequency of the
 * first, 200 kHz apart. E-GSM 900 also has 975 to 1023 below ARFCN 0. */
static const struct gsm_band {
    const char *name;
    int    first, last;
    double first_hz;
} gsm_bands[] = {
    { "GSM 850",    128, 251,  869.2e6 },
    { "E-GSM 900",    0, 124,  935.0e6 },
    { "DCS 1800",   512, 885, 1805.2e6 },
    { "PCS 1900",   512, 810, 1930.2e6 },
};

#define GSM_BANDS ((int)(sizeof(gsm_bands) / sizeof(gsm_bands[0])))

/* Downlink frequency of an ARFCN of a band; 0 when it is not one of the band. */
static double gsm_arfcn_hz(int band, int arfcn)
{
    const struct gsm_band *b = &gsm_bands[band];

    if (band == 1 && arfcn >= 975 && arfcn <= 1023)
        return b->first_hz + 0.2e6 * (arfcn - 1024);
    if (arfcn < b->first || arfcn > b->last)
        return 0.0;
    return b->first_hz + 0.2e6 * (arfcn - b->first);
}

/* ARFCN of a frequency, its band in *band; -1 off the raster of the bands. DCS 1800 and PCS 1900
 * share their numbers, not their frequencies. */
static int gsm_hz_arfcn(double hz, int *band)
{
    int b, n;

    for (b = 0; b < GSM_BANDS; b++) {
        for (n = gsm_bands[b].first; n <= gsm_bands[b].last; n++)
            if (fabs(gsm_arfcn_hz(b, n) - hz) < 1e3) {
                *band = b;
                return n;
            }
    }
    for (n = 975; n <= 1023; n++)
        if (fabs(gsm_arfcn_hz(1, n) - hz) < 1e3) {
            *band = 1;
            return n;
        }
    return -1;
}

/* Analysis */
/*----------*/

static size_t gsm_samples(const struct an_settings *set)
{
    return (size_t)(AN_GSM_SECONDS * set->sample_rate);
}

static void gsm_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = (size_t)(AN_GSM_SECONDS * set->sample_rate);

    res->gsm_valid = m2sdr_gsm_analyse(x, count < n ? count : n, set->sample_rate, NULL, &res->gsm) == 0;
}

static int gsm_decoded(const struct m2sdr_gsm_result *g)
{
    int i, n = 0;

    for (i = 0; i < g->nsch; i++)
        n += g->sch[i].crc;
    return n;
}

/* Reports */
/*---------*/

static void gsm_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_gsm_result *g = &res->gsm;
    int band = 0, arfcn = gsm_hz_arfcn(set->freq_hz, &band), i;

    printf("\nGSM:\n");
    if (arfcn >= 0)
        printf("  Channel    : ARFCN %d (%s)\n", arfcn, gsm_bands[band].name);
    if (!res->gsm_valid) {
        printf("  The sample rate must be 0.5 to 8 MSPS (1.92 MSPS), with at least two frames of samples.\n");
        return;
    }
    if (!g->fcch) {
        printf("  No frequency correction burst: tune on a BCCH carrier (C0); %.0f ms of samples hold one.\n",
               AN_GSM_SECONDS * 1e3);
        return;
    }
    printf("  FCCH       : %d burst%s, coherence %.2f, carrier %+.1f Hz from the centre\n", g->fcch_bursts,
           g->fcch_bursts == 1 ? "" : "s", g->fcch_quality, g->freq_error_hz);
    if (g->nsch == 0) {
        printf("  SCH        : not in the capture (or its training not found)\n");
        return;
    }
    if (g->found)
        printf("  SCH        : BSIC %d (NCC %d, BCC %d), frame %d, %d of %d decoded\n", g->bsic, g->ncc, g->bcc, g->fn,
               gsm_decoded(g), g->nsch);
    else
        printf("  SCH        : %d found, none decoded (parity): the figures below are against the bits decided\n", g->nsch);
    printf("  Burst      : %.1f %s, phase error %.2f deg RMS, %.1f deg peak, EVM %.2f %% (SNR %.1f dB)\n",
           g->burst_power_db + rep->off, rep->unit, g->phase_err_rms_deg, g->phase_err_peak_deg, 100.0 * g->evm, g->snr_db);
    for (i = 0; i < g->nsch; i++) {
        const struct m2sdr_gsm_sch *s = &res->gsm.sch[i];

        if (s->crc)
            printf("    %8.3f ms  BSIC %2d  FN %7d  %+8.1f Hz  %6.1f %s  %5.2f deg  EVM %5.2f %%\n", 1e3 * s->time_s, s->bsic,
                   s->fn, s->freq_hz, s->power_db + rep->off, rep->unit, s->phase_err_rms_deg, 100.0 * s->evm);
        else
            printf("    %8.3f ms  not decoded  %+8.1f Hz  %6.1f %s\n", 1e3 * s->time_s, s->freq_hz, s->power_db + rep->off,
                   rep->unit);
    }
}

static void gsm_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_gsm_result *g = &res->gsm;
    const bool fcch = res->gsm_valid && g->fcch;
    int band = 0, arfcn = gsm_hz_arfcn(set->freq_hz, &band), i;

    printf("  \"gsm\": {\n");
    if (arfcn >= 0)
        printf("    \"arfcn\": %d,\n    \"band\": \"%s\",\n", arfcn, gsm_bands[band].name);
    printf("    \"fcch\": %s,\n", fcch ? "true" : "false");
    printf("    \"found\": %s%s\n", res->gsm_valid && g->found ? "true" : "false", fcch ? "," : "");
    if (fcch) {
        printf("    \"fcch_bursts\": %d,\n", g->fcch_bursts);
        print_json_number("fcch_quality", g->fcch_quality, false);
        print_json_number("carrier_offset_hz", g->freq_error_hz, false);
        if (g->found)
            printf("    \"bsic\": %d,\n    \"ncc\": %d,\n    \"bcc\": %d,\n    \"fn\": %d,\n", g->bsic, g->ncc, g->bcc, g->fn);
        printf("    \"sch_bursts\": %d,\n    \"sch_decoded\": %d,\n", g->nsch, gsm_decoded(g));
        if (g->nsch > 0) {
            print_json_number("burst_power_db", g->burst_power_db + rep->off, false);
            print_json_number("phase_error_rms_deg", g->phase_err_rms_deg, false);
            print_json_number("phase_error_peak_deg", g->phase_err_peak_deg, false);
            print_json_number("evm", g->evm, false);
            print_json_number("snr_db", g->snr_db, false);
        }
        printf("    \"sch\": [");
        for (i = 0; i < g->nsch; i++) {
            const struct m2sdr_gsm_sch *s = &g->sch[i];

            printf("%s\n      { \"time_s\": %.6f, \"crc\": %s, \"bsic\": %d, \"fn\": %d, \"carrier_offset_hz\": %.1f, "
                   "\"power_db\": %.2f, \"phase_error_rms_deg\": %.3f, \"evm\": %.5f }", i ? "," : "", s->time_s,
                   s->crc ? "true" : "false", s->bsic, s->fn, s->freq_hz, s->power_db + rep->off, s->phase_err_rms_deg, s->evm);
        }
        printf("%s]\n", g->nsch ? "\n    " : "");
    }
    printf("  }");
}

/* Watch */
/*-------*/

static unsigned gsm_watch_lines;

static int gsm_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_gsm_result *g = &res->gsm;

    if (!res->gsm_valid) {
        fprintf(stderr, "The sample rate is not one of GSM (0.5 to 8 MSPS).\n");
        return -1;
    }
    if (json) {
        printf("{\"time_s\": %.3f, \"fcch\": %s, \"found\": %s", now, g->fcch ? "true" : "false", g->found ? "true" : "false");
        if (g->fcch)
            printf(", \"carrier_offset_hz\": %.1f", g->freq_error_hz);
        if (g->found)
            printf(", \"bsic\": %d, \"fn\": %d", g->bsic, g->fn);
        if (g->nsch)
            printf(", \"power_db\": %.2f, \"phase_error_rms_deg\": %.3f, \"evm\": %.5f", g->burst_power_db + set->ref_offset_db,
                   g->phase_err_rms_deg, g->evm);
        printf("}\n");
        return 0;
    }
    if (gsm_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
        printf("%8s %5s %8s %10s %8s %7s %7s\n%8s %5s %8s %10s %8s %7s %7s\n", "time", "BSIC", "FN", "offset", "level",
               "phase", "EVM", "s", "", "", "Hz", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "deg", "%");
    if (g->found)
        printf("%8.2f %5d %8d %+10.1f %8.1f %7.2f %7.2f\n", now, g->bsic, g->fn, g->freq_error_hz,
               g->burst_power_db + set->ref_offset_db, g->phase_err_rms_deg, 100.0 * g->evm);
    else if (g->fcch)
        printf("%8.2f  FCCH at %+.1f Hz, %s\n", now, g->freq_error_hz, g->nsch ? "SCH not decoded" : "no SCH in the capture");
    else
        printf("%8.2f  no FCCH\n", now);
    return 0;
}

/* Command line */
/*--------------*/

static const struct option gsm_options[] = {
    { "gsm", no_argument, NULL, 0 },
    { NULL, 0, NULL, 0 }
};

static int gsm_option(int id, const char *arg, struct an_cli *cli)
{
    (void)id;
    (void)arg;
    cli->set->gsm_on = true;
    return 0;
}

static void gsm_help(void)
{
    printf("GSM options:\n"
           "      --gsm              With --report: a GSM BCCH carrier: FCCH, SCH, BSIC, frame number,\n"
           "                         phase error (default rate 1.92 MSPS). --watch works too.\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

#define GSM_COL_PHASE 0xff40c0ffu

/* Points of a plane with crosses on the ideal ones (n of them, nref crosses). */
static void gsm_points(const char *id, ImVec2 size, const char *label, const float complex *p, int n,
                      const float complex *ref, int nref, ImU32 color)
{
    static float px[M2SDR_GSM_MAX_POINTS], py[M2SDR_GSM_MAX_POINTS];
    struct m2sdr_plot plot;
    int i;

    if (!m2sdr_plot_begin(&plot, id, size, -1.6, 1.6, M2SDR_PLOT_PLAIN, -1.6, 1.6, label))
        return;
    for (i = 0; i < n && i < M2SDR_GSM_MAX_POINTS; i++) {
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

static int gsm_band_sel = 1, gsm_arfcn_sel = 62;

/* Band and ARFCN to tune to; the rate of the analysis. */
static void gsm_controls(struct an_ui *ui, bool *changed)
{
    int band = 0, arfcn = gsm_hz_arfcn(ui->set.freq_hz, &band), i;
    char text[64];

    igTextUnformatted("Band:", NULL);
    igSameLine(0.0f, 8.0f);
    igSetNextItemWidth(120.0f);
    if (igBeginCombo("##gsmband", gsm_bands[gsm_band_sel].name, 0)) {
        for (i = 0; i < GSM_BANDS; i++)
            if (igSelectable_Bool(gsm_bands[i].name, i == gsm_band_sel, 0, (ImVec2){ 0.0f, 0.0f }))
                gsm_band_sel = i;
        igEndCombo();
    }
    igSameLine(0.0f, 16.0f);
    igTextUnformatted("ARFCN:", NULL);
    igSameLine(0.0f, 8.0f);
    igSetNextItemWidth(110.0f);
    igInputInt("##gsmarfcn", &gsm_arfcn_sel, 1, 10, 0);
    igSameLine(0.0f, 8.0f);
    if (gsm_arfcn_hz(gsm_band_sel, gsm_arfcn_sel) > 0) {
        snprintf(text, sizeof(text), "Tune %.1f MHz", gsm_arfcn_hz(gsm_band_sel, gsm_arfcn_sel) / 1e6);
        if (igSmallButton(text))
            view_retune(ui, gsm_arfcn_hz(gsm_band_sel, gsm_arfcn_sel), changed);
    } else {
        igTextDisabled("not an ARFCN of the band");
    }
    if (arfcn >= 0) {
        igSameLine(0.0f, 16.0f);
        igText("Tuned: ARFCN %d (%s)", arfcn, gsm_bands[band].name);
    }
    if (fabs(ui->set.sample_rate - AN_GSM_RATE) > 1.0) {
        igSameLine(0.0f, 16.0f);
        if (igSmallButton("1.92 MSPS"))
            view_rate(ui, AN_GSM_RATE, changed);
    }
}

static void draw_gsm(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_gsm_result *g = &res->gsm;
    static float bits[M2SDR_GSM_BURST_BITS];
    struct m2sdr_plot plot;
    ImVec2 avail, sq;
    float side;
    char text[160];
    int i;

    if (live)
        gsm_controls(ui, changed);
    if (!res->set.gsm_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->gsm_valid) {
        igTextUnformatted("The sample rate must be 0.5 to 8 MSPS (1.92 MSPS).", NULL);
        return;
    }
    igPushTextWrapPos(0.0f);
    if (!g->fcch)
        igText("No frequency correction burst: tune on a BCCH carrier (C0). A capture of %.0f ms holds an FCCH and its SCH.",
               AN_GSM_SECONDS * 1e3);
    else if (!g->nsch)
        igText("FCCH at %+.1f Hz; its SCH is not in the capture.", g->freq_error_hz);
    else if (g->found)
        igText("BSIC %d (NCC %d, BCC %d), frame %d. Phase error %.2f deg RMS, %.1f deg peak, against the GMSK of the bits decoded.",
               g->bsic, g->ncc, g->bcc, g->fn, g->phase_err_rms_deg, g->phase_err_peak_deg);
    else
        igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "SCH found, not decoded: the phase error is against the bits decided.");
    igPopTextWrapPos();
    if (!g->nsch)
        return;

    igGetContentRegionAvail(&avail);
    side = 0.6f * avail.y < 0.4f * avail.x ? 0.6f * avail.y : 0.4f * avail.x;
    sq   = m2sdr_plot_square(side);
    /* Phase error of each bit of the SCH against the ideal GMSK. */
    for (i = 0; i < M2SDR_GSM_BURST_BITS; i++)
        bits[i] = g->phase_err_deg[i];
    snprintf(text, sizeof(text), "Phase error of the SCH (deg) against its bits; RMS %.2f deg", g->phase_err_rms_deg);
    {
        const double lim = g->phase_err_peak_deg > 5.0 ? ceil(g->phase_err_peak_deg / 5.0) * 5.0 : 5.0;

        if (m2sdr_plot_begin(&plot, "##gsmphase", (ImVec2){ avail.x - sq.x - 8.0f, sq.y }, 0.0, M2SDR_GSM_BURST_BITS - 1,
                             M2SDR_PLOT_PLAIN, -lim, lim, text)) {
            m2sdr_plot_band(&plot, 42.0, 105.0, 0x20ffffffu);   /* Training sequence. */
            m2sdr_plot_hline(&plot, 0.0, 0x60ffffffu);
            m2sdr_plot_trace(&plot, bits, M2SDR_GSM_BURST_BITS, 0.0, M2SDR_GSM_BURST_BITS - 1, GSM_COL_PHASE);
            if (plot.hovered) {
                const int b = (int)lround(plot.mouse_x);

                if (b >= 0 && b < M2SDR_GSM_BURST_BITS) {
                    snprintf(text, sizeof(text), "bit %d: %+.2f deg", b, bits[b]);
                    m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
                }
            }
            m2sdr_plot_end(&plot);
        }
    }
    igSameLine(0.0f, 8.0f);
    {
        static const float complex ref[2] = { -1.0f, 1.0f };

        gsm_points("##gsmbits", sq, "SCH bits at their instants, turned onto the real axis", g->points, g->npoints, ref, 2,
                   0x90ffd080u);
    }

    /* The SCH bursts of the capture. */
    if (igBeginTable("##gsmsch", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        igTableSetupColumn("Time", 0, 0.0f, 0);
        igTableSetupColumn("BSIC", 0, 0.0f, 0);
        igTableSetupColumn("Frame", 0, 0.0f, 0);
        igTableSetupColumn("Offset", 0, 0.0f, 0);
        igTableSetupColumn("Power", 0, 0.0f, 0);
        igTableSetupColumn("Phase error", 0, 0.0f, 0);
        igTableSetupColumn("EVM", 0, 0.0f, 0);
        igTableHeadersRow();
        for (i = 0; i < g->nsch; i++) {
            const struct m2sdr_gsm_sch *s = &g->sch[i];

            igTableNextRow(0, 0.0f);
            igTableNextColumn();
            igText("%.3f ms", 1e3 * s->time_s);
            igTableNextColumn();
            if (s->crc)
                igText("%d (%d, %d)", s->bsic, s->ncc, s->bcc);
            else
                igTextDisabled("not decoded");
            igTableNextColumn();
            if (s->crc)
                igText("%d", s->fn);
            igTableNextColumn();
            igText("%+.1f Hz", s->freq_hz);
            igTableNextColumn();
            igText("%.1f dBFS", s->power_db);
            igTableNextColumn();
            igText("%.2f / %.1f deg", s->phase_err_rms_deg, s->phase_err_peak_deg);
            igTableNextColumn();
            igText("%.2f %%", 100.0 * s->evm);
        }
        igEndTable();
    }
}

static void draw_gsm_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_gsm_result *g = &res->gsm;
    int band = 0, arfcn = gsm_hz_arfcn(res->set.freq_hz, &band);

    igSeparatorText("GSM");
    if (!res->gsm_valid || !g->fcch) {
        igTextUnformatted("No FCCH.", NULL);
        return;
    }
    if (igBeginTable("##gsm", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        if (arfcn >= 0)
            label_value("ARFCN", "%d (%s)", arfcn, gsm_bands[band].name);
        label_value("Carrier", "%.6f MHz", (res->set.freq_hz + g->freq_error_hz) / 1e6);
        label_value("Offset", "%+.1f Hz", g->freq_error_hz);
        label_value("FCCH", "%d, coherence %.2f", g->fcch_bursts, g->fcch_quality);
        if (g->found) {
            label_value("BSIC", "%d", g->bsic);
            label_value("NCC, BCC", "%d, %d", g->ncc, g->bcc);
            label_value("Frame number", "%d", g->fn);
        }
        label_value("SCH decoded", "%d of %d", gsm_decoded(g), g->nsch);
        if (g->nsch) {
            label_value("Burst power", "%.1f dBFS", g->burst_power_db);
            label_value("Phase error", "%.2f deg RMS", g->phase_err_rms_deg);
            label_value("", "%.1f deg peak", g->phase_err_peak_deg);
            label_value("EVM", "%.2f %%", 100.0 * g->evm);
            label_value("SNR", "%.1f dB", g->snr_db);
        }
        igEndTable();
    }
    igPushTextWrapPos(0.0f);
    igTextUnformatted("One tap, no equaliser: with multipath the phase error includes the channel.", NULL);
    igPopTextWrapPos();
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_gsm = {
    .key          = "gsm",
    .title        = "GSM",
    .category     = AN_CAT_CELLULAR,
    .on           = offsetof(struct an_settings, gsm_on),
    .valid        = offsetof(struct an_result, gsm_valid),
    .report_rank  = 12,
    .default_rate = AN_GSM_RATE,
    .psd_short    = true,
    .samples      = gsm_samples,
    .analyse      = gsm_analyse,
    .report_text  = gsm_report_text,
    .report_json  = gsm_report_json,
    .watch_line   = gsm_watch_line,
    .options      = gsm_options,
    .option       = gsm_option,
    .help         = gsm_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_gsm,
    .results      = draw_gsm_results,
#endif
};
