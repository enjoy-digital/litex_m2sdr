/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: phase noise of the strongest carrier (L(f), amplitude noise,
 * jitter, residual FM, spurs), against the floor of the board.
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

#include "m2sdr_cli.h"

#include "analyzer.h"

/* Analysis */
/*----------*/

static size_t pn_samples(const struct an_settings *set)
{
    return (size_t)1 << set->pn_log2;
}

static void pn_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = count < ((size_t)1 << set->pn_log2) ? count : (size_t)1 << set->pn_log2;

    res->pn_valid = m2sdr_phase_noise(x, n, set->sample_rate, set->freq_hz, set->pn_lo_hz, set->pn_hi_hz,
                                      &res->pn) == 0 && res->pn.found;
    /* The jitter needs the RF frequency of the carrier, known once it is found. */
    if (res->pn_valid && set->freq_hz + res->pn.carrier_hz > 0)
        res->pn.jitter_s = res->pn.rms_rad / (2.0 * M_PI * (set->freq_hz + res->pn.carrier_hz));
}

/* Reports */
/*---------*/

/* Value of a trace of phase noise at an offset, between its points; NAN out of them. */
static double pn_interp(const double *offset_hz, const double *value, int points, double at_hz)
{
    int i;

    if (points < 2 || at_hz < offset_hz[0] || at_hz > offset_hz[points - 1])
        return NAN;
    for (i = 1; i < points; i++) {
        if (offset_hz[i] >= at_hz) {
            const double t = log(at_hz / offset_hz[i - 1]) / log(offset_hz[i] / offset_hz[i - 1]);
            return value[i - 1] + t * (value[i] - value[i - 1]);
        }
    }
    return NAN;
}

static const double pn_spots[] = { 100.0, 1e3, 1e4, 1e5, 1e6, 1e7 };
#define PN_SPOTS ((int)(sizeof(pn_spots) / sizeof(pn_spots[0])))

static void pn_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const char *unit = rep->unit;
    const struct an_floor *fl = rep->fl;
    const struct m2sdr_phase_noise *pn = &res->pn;
    int i;

    printf("\nPhase noise:\n");
    if (!res->pn_valid) {
        printf("  No carrier stands %.0f dB above the rest of the capture.\n", 10.0);
        return;
    }
    printf("  Carrier    : %.6f MHz, %.2f %s, %zu samples\n", (set->freq_hz + pn->carrier_hz) / 1e6,
           pn->carrier_db + off, unit, (size_t)1 << set->pn_log2);
    for (i = 0; i < PN_SPOTS; i++) {
        const double l = pn_interp(pn->offset_hz, pn->l_dbc_hz, pn->points, pn_spots[i]);
        const double am = pn_interp(pn->offset_hz, pn->am_dbc_hz, pn->points, pn_spots[i]);
        const double floor = fl && fl->points ? pn_interp(fl->offset_hz, fl->l_dbc_hz, fl->points, pn_spots[i]) : NAN;
        char freq[32];

        if (!isfinite(l))
            continue;
        if (pn_spots[i] >= 1e6)
            snprintf(freq, sizeof(freq), "%.0f MHz", pn_spots[i] / 1e6);
        else if (pn_spots[i] >= 1e3)
            snprintf(freq, sizeof(freq), "%.0f kHz", pn_spots[i] / 1e3);
        else
            snprintf(freq, sizeof(freq), "%.0f Hz", pn_spots[i]);
        printf("  %-10s : %7.1f dBc/Hz (AM %7.1f)", freq, l, am);
        if (isfinite(floor))
            printf(", floor %7.1f%s", floor, l - floor < AN_PN_FLOOR_MARGIN_DB ? ": the reading is the floor of the board" : "");
        printf("\n");
    }
    printf("  Integrated : %.4f degrees RMS from %.0f Hz to %.0f Hz\n", pn->rms_deg, pn->lo_hz, pn->hi_hz);
    printf("  Jitter     : %.3f ps RMS\n", pn->jitter_s * 1e12);
    printf("  Residual FM: %.2f Hz RMS\n", pn->residual_fm_hz);
    for (i = 0; i < pn->nspurs && i < 8; i++)
        printf("  Spur       : %12.1f Hz  %7.1f dBc\n", pn->spur_hz[i], pn->spur_dbc[i]);
    if (!fl || !fl->points)
        printf("  No floor of the board is stored: the reading includes its own phase noise.\n");
}

static void pn_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const struct an_floor *fl = rep->fl;
    const struct m2sdr_phase_noise *pn = &res->pn;
    int i;

    printf("  \"phase_noise\": {\n");
    printf("    \"found\": %s,\n", res->pn_valid ? "true" : "false");
    if (res->pn_valid) {
        print_json_number("carrier_hz", set->freq_hz + pn->carrier_hz, false);
        print_json_number("carrier_db", pn->carrier_db + off, false);
        print_json_number("integrated_from_hz", pn->lo_hz, false);
        print_json_number("integrated_to_hz", pn->hi_hz, false);
        print_json_number("rms_deg", pn->rms_deg, false);
        print_json_number("jitter_s", pn->jitter_s, false);
        print_json_number("residual_fm_hz", pn->residual_fm_hz, false);
        printf("    \"offsets_hz\": [");
        for (i = 0; i < pn->points; i++)
            printf("%s%.3f", i ? ", " : "", pn->offset_hz[i]);
        printf("],\n    \"l_dbc_hz\": [");
        for (i = 0; i < pn->points; i++)
            printf("%s%.3f", i ? ", " : "", pn->l_dbc_hz[i]);
        printf("],\n    \"am_dbc_hz\": [");
        for (i = 0; i < pn->points; i++)
            printf("%s%.3f", i ? ", " : "", pn->am_dbc_hz[i]);
        printf("],\n    \"floor_dbc_hz\": [");
        for (i = 0; fl && fl->points && i < pn->points; i++) {
            const double v = pn_interp(fl->offset_hz, fl->l_dbc_hz, fl->points, pn->offset_hz[i]);
            if (isfinite(v))
                printf("%s%.3f", i ? ", " : "", v);
            else
                printf("%snull", i ? ", " : "");
        }
        printf("],\n    \"spurs\": [");
        for (i = 0; i < pn->nspurs; i++)
            printf("%s{\"offset_hz\": %.3f, \"level_dbc\": %.3f}", i ? ", " : "", pn->spur_hz[i], pn->spur_dbc[i]);
        printf("]\n");
    } else {
        printf("    \"carrier_hz\": null\n");
    }
    printf("  }");
}

/* Command line */
/*--------------*/

enum { PN_OPT_ON, PN_OPT_SAMPLES, PN_OPT_FROM, PN_OPT_TO, PN_OPT_FLOOR, PN_OPT_FLOOR_SAVE };

static const struct option pn_options[] = {
    { "phase-noise",     no_argument,       NULL, PN_OPT_ON },
    { "pn-samples",      required_argument, NULL, PN_OPT_SAMPLES },
    { "pn-from",         required_argument, NULL, PN_OPT_FROM },
    { "pn-to",           required_argument, NULL, PN_OPT_TO },
    { "pn-floor",        required_argument, NULL, PN_OPT_FLOOR },
    { "pn-floor-save",   no_argument,       NULL, PN_OPT_FLOOR_SAVE },
    { NULL, 0, NULL, 0 }
};

static void pn_defaults(struct an_settings *set)
{
    set->pn_log2 = AN_PN_DEFAULT_LOG2;
}

static int pn_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_settings *set = cli->set;
    double v;

    switch (id) {
    case PN_OPT_ON: set->pn_on = true; break;
    case PN_OPT_SAMPLES:
        if (m2sdr_cli_parse_double(arg, &v) != 0 || v < (double)(1 << AN_PN_MIN_LOG2) || v > (double)(1 << AN_PN_MAX_LOG2)) {
            m2sdr_cli_error("invalid phase noise samples '%s' (expected %d to %d)", arg,
                            1 << AN_PN_MIN_LOG2, 1 << AN_PN_MAX_LOG2);
            return -1;
        }
        /* The power of two at or below. */
        for (set->pn_log2 = AN_PN_MIN_LOG2; (double)((size_t)2 << set->pn_log2) <= v; set->pn_log2++)
            ;
        break;
    case PN_OPT_FROM:
        if (m2sdr_cli_parse_double(arg, &set->pn_lo_hz) != 0 || set->pn_lo_hz < 0) {
            m2sdr_cli_error("invalid offset '%s'", arg);
            return -1;
        }
        break;
    case PN_OPT_TO:
        if (m2sdr_cli_parse_double(arg, &set->pn_hi_hz) != 0 || set->pn_hi_hz < 0) {
            m2sdr_cli_error("invalid offset '%s'", arg);
            return -1;
        }
        break;
    case PN_OPT_FLOOR: cli->floor_arg = arg; break;
    case PN_OPT_FLOOR_SAVE:
        /* The phase noise read is stored as the floor. */
        cli->floor_store = true;
        set->pn_on       = true;
        break;
    }
    return 0;
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

static void draw_phase_noise(struct an_ui *ui, bool live, bool *changed)
{
    static const char *const sizes[] = { "65536", "131072", "262144", "524288", "1048576", "2097152", "4194304", "8388608" };
    static float lx[M2SDR_PN_MAX_POINTS], l[M2SDR_PN_MAX_POINTS], am[M2SDR_PN_MAX_POINTS];
    static float fx[M2SDR_PN_MAX_POINTS], fy[M2SDR_PN_MAX_POINTS];
    const struct m2sdr_phase_noise *pn = &ui->res.pn;
    const struct an_floor *fl = ui->fl;
    struct m2sdr_plot plot;
    double top = -200.0, bottom = 0.0;
    int idx = ui->set.pn_log2 - AN_PN_MIN_LOG2, i, nf = 0;
    char text[160];

    (void)live;
    igSetNextItemWidth(110.0f);
    if (igCombo_Str_arr("Samples", &idx, sizes, AN_PN_MAX_LOG2 - AN_PN_MIN_LOG2 + 1, -1)) {
        ui->set.pn_log2 = idx + AN_PN_MIN_LOG2;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 230.0f);
    if (input_scaled("From (Hz)", &ui->set.pn_lo_hz, 1.0, "%.0f"))
        *changed = true;
    same_line_or_wrap(16.0f, 215.0f);
    if (input_scaled("To (Hz)", &ui->set.pn_hi_hz, 1.0, "%.0f"))
        *changed = true;
    same_line_or_wrap(16.0f, 100.0f);
    igCheckbox("AM noise", &ui->pn_am);
    if (ui->floor_path) {
        same_line_or_wrap(16.0f, 115.0f);
        if (igButton("Store as floor", (ImVec2){ 0.0f, 0.0f }) && ui->res.pn_valid) {
            if (floor_save(ui->fl, ui->floor_path, &ui->res) == 0)
                snprintf(ui->message, sizeof(ui->message), "Floor written to %.200s", ui->floor_path);
            else
                snprintf(ui->message, sizeof(ui->message), "Could not write %.200s", ui->floor_path);
        }
    }

    if (!ui->res.set.pn_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!ui->res.pn_valid) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted("No carrier stands 10 dB above the rest of the capture: lower the sample rate around it, "
                          "or tune on it.", NULL);
        igPopTextWrapPos();
        return;
    }
    for (i = 0; i < pn->points; i++) {
        lx[i] = (float)log10(pn->offset_hz[i]);
        l[i]  = (float)pn->l_dbc_hz[i];
        am[i] = (float)pn->am_dbc_hz[i];
        if (l[i] > top)
            top = l[i];
        if (l[i] < bottom)
            bottom = l[i];
        if (ui->pn_am && am[i] < bottom)
            bottom = am[i];
    }
    for (i = 0; fl && i < fl->points; i++) {
        if (fl->offset_hz[i] < pn->offset_hz[0] || fl->offset_hz[i] > pn->offset_hz[pn->points - 1])
            continue;
        fx[nf] = (float)log10(fl->offset_hz[i]);
        fy[nf] = (float)fl->l_dbc_hz[i];
        if (fy[nf] < bottom)
            bottom = fy[nf];
        nf++;
    }
    top    = 10.0 * ceil(top / 10.0) + 10.0;
    bottom = 10.0 * floor(bottom / 10.0) - 10.0;
    if (!m2sdr_plot_begin(&plot, "##pn", (ImVec2){ 0.0f, 0.0f }, lx[0], lx[pn->points - 1], M2SDR_PLOT_LOG_FREQ,
                          bottom, top, "L(f), dBc/Hz, against the offset from the carrier"))
        return;
    /* Integration range. */
    m2sdr_plot_band(&plot, log10(pn->lo_hz), log10(pn->hi_hz), 0x18ffffffu);
    if (nf >= 2)
        m2sdr_plot_xy(&plot, fx, fy, (size_t)nf, 0xffa0a0a0u);
    if (ui->pn_am)
        m2sdr_plot_xy(&plot, lx, am, (size_t)pn->points, 0xffffa040u);
    m2sdr_plot_xy(&plot, lx, l, (size_t)pn->points, 0xff40d0ffu);
    /* Points that are the floor of the board, not the source. */
    for (i = 0; i < pn->points && nf >= 2; i++) {
        const double floor = pn_interp(fl->offset_hz, fl->l_dbc_hz, fl->points, pn->offset_hz[i]);
        if (isfinite(floor) && pn->l_dbc_hz[i] - floor < AN_PN_FLOOR_MARGIN_DB)
            m2sdr_plot_points(&plot, &lx[i], &l[i], 1, 2.5f, 0xff4040ffu);
    }
    for (i = 0; i < pn->nspurs && i < 8; i++) {
        snprintf(text, sizeof(text), "%.0f dBc", pn->spur_dbc[i]);
        m2sdr_plot_marker(&plot, log10(pn->spur_hz[i]), pn_interp(pn->offset_hz, pn->l_dbc_hz, pn->points, pn->spur_hz[i]),
                          text, 0xffc0c0ffu);
    }
    snprintf(text, sizeof(text), "yellow: phase%s%s", ui->pn_am ? ", blue: amplitude" : "",
             nf >= 2 ? ", grey: floor of the board, red: reading at the floor" : ", no floor stored");
    m2sdr_plot_text(&plot, 8.0f, -20.0f, text, 0xffc8c8c8u);
    if (plot.hovered) {
        const double f = pow(10.0, plot.mouse_x);
        snprintf(text, sizeof(text), "%.0f Hz  %.1f dBc/Hz", f, pn_interp(pn->offset_hz, pn->l_dbc_hz, pn->points, f));
        m2sdr_plot_text(&plot, -220.0f, 4.0f, text, 0xffffffffu);
        m2sdr_plot_vline(&plot, plot.mouse_x, 0x40ffffffu);
    }
    m2sdr_plot_end(&plot);
}

static void draw_pn_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_phase_noise *pn = &res->pn;
    const struct an_floor *fl = ui->fl;
    char name[32];
    int i;

    igSeparatorText("Phase noise");
    if (!res->pn_valid) {
        igTextUnformatted("No carrier.", NULL);
        return;
    }
    if (igBeginTable("##pn", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Carrier", "%.6f MHz", (res->set.freq_hz + pn->carrier_hz) / 1e6);
        label_value("Level", "%.2f dBFS", pn->carrier_db);
        for (i = 0; i < PN_SPOTS; i++) {
            const double l = pn_interp(pn->offset_hz, pn->l_dbc_hz, pn->points, pn_spots[i]);
            const double floor = fl && fl->points ? pn_interp(fl->offset_hz, fl->l_dbc_hz, fl->points, pn_spots[i]) : NAN;

            if (!isfinite(l))
                continue;
            m2sdr_plot_format(name, sizeof(name), pn_spots[i], pn_spots[i], M2SDR_PLOT_FREQ);
            if (isfinite(floor))
                label_value(name, "%.1f dBc/Hz (floor %.1f)", l, floor);
            else
                label_value(name, "%.1f dBc/Hz", l);
        }
        label_value("Integrated", "%.4f deg RMS", pn->rms_deg);
        label_value("Jitter", "%.3f ps RMS", pn->jitter_s * 1e12);
        label_value("Residual FM", "%.2f Hz RMS", pn->residual_fm_hz);
        label_value("Range", "%.0f Hz to %.0f Hz", pn->lo_hz, pn->hi_hz);
        for (i = 0; i < pn->nspurs && i < 5; i++) {
            snprintf(name, sizeof(name), "Spur %.0f Hz", pn->spur_hz[i]);
            label_value(name, "%.1f dBc", pn->spur_dbc[i]);
        }
        igEndTable();
    }
    if (!fl || !fl->points) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted("No floor stored: the reading includes the phase noise of the board.", NULL);
        igPopTextWrapPos();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_pn = {
    .key         = "phase-noise",
    .title       = "Phase noise",
    .category    = AN_CAT_SIGNAL,
    .on          = offsetof(struct an_settings, pn_on),
    .valid       = offsetof(struct an_result, pn_valid),
    .report_rank = 50,
    .samples     = pn_samples,
    .analyse     = pn_analyse,
    .report_text = pn_report_text,
    .report_json = pn_report_json,
    .defaults    = pn_defaults,
    .options     = pn_options,
    .option      = pn_option,
#ifdef M2SDR_ANALYZER_GUI
    .draw        = draw_phase_noise,
    .results     = draw_pn_results,
#endif
};
