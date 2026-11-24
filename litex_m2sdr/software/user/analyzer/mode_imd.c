/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: two-tone intermodulation of a device (IM3, IM5, intercepts,
 * spurs), from two tones sent by TX.
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

#define IMD_PERIOD     65536
#define IMD_PERIODS    4
#define IMD_LEVEL_DBFS -12.0

/* Stimulus */
/*----------*/

struct imd_plan {
    double centre, spacing, level;
};

static void imd_plan(const struct an_settings *set, struct imd_plan *p)
{
    p->centre  = set->lab.tone_offset_hz != 0.0 ? set->lab.tone_offset_hz : 0.1 * set->sample_rate;
    p->spacing = set->lab.imd_spacing_hz > 0 ? set->lab.imd_spacing_hz : 0.01 * set->sample_rate;
    p->level   = set->lab.imd_level_dbfs != 0.0 ? set->lab.imd_level_dbfs : IMD_LEVEL_DBFS;
}

/* The two tones, rounded as the stimulus has them. */
static int imd_tones(const struct an_settings *set, float complex *x, double *f1, double *f2)
{
    struct imd_plan p;
    float complex *tmp = x ? x : malloc(IMD_PERIOD * sizeof(*tmp));
    int rc;

    if (!tmp)
        return -1;
    imd_plan(set, &p);
    rc = m2sdr_lab_two_tone(tmp, IMD_PERIOD, set->sample_rate, p.centre, p.spacing, p.level, f1, f2);
    if (!x)
        free(tmp);
    return rc;
}

static float complex *imd_stimulus(const struct an_settings *set, size_t *period)
{
    float complex *x = malloc(IMD_PERIOD * sizeof(*x));
    double f1, f2;

    if (!x || imd_tones(set, x, &f1, &f2) != 0) {
        free(x);
        return NULL;
    }
    *period = IMD_PERIOD;
    return x;
}

/* Analysis */
/*----------*/

static size_t imd_samples(const struct an_settings *set)
{
    (void)set;
    return (size_t)IMD_PERIODS * IMD_PERIOD;
}

static void imd_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    struct m2sdr_imd_config cfg;
    struct imd_plan p;

    imd_plan(set, &p);
    memset(&cfg, 0, sizeof(cfg));
    if (imd_tones(set, NULL, &cfg.f1_hz, &cfg.f2_hz) != 0)
        return;
    cfg.input_known = true;
    cfg.input_dbfs  = p.level + lab_input_offset(set);
    res->imd_valid  = m2sdr_imd_analyse(x, count, set->sample_rate, &cfg, &res->imd) == 0;
}

/* Reports */
/*---------*/

static void imd_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_imd_result *d = &res->imd;
    const char *iu = lab_input_unit(set);
    const double off = rep->off;
    struct imd_plan p;
    int i;

    imd_plan(set, &p);
    printf("\nIMD:\n");
    printf("  Stimulus   : two tones %.3f kHz apart around %+.3f kHz, %.1f dBFS each, TX attenuation %d dB\n",
           p.spacing / 1e3, p.centre / 1e3, p.level, set->lab.tx_att);
    if (!res->imd_valid) {
        printf("  These settings do not make a two-tone measurement (tones out of the span or too close).\n");
        return;
    }
    if (!d->found) {
        printf("  The two tones are not in the capture: TX to RX through the device?\n");
        return;
    }
    printf("  Tones      : %.2f and %.2f %s at %+.1f and %+.1f Hz (input %.2f %s each)\n", d->tone1_dbfs + off,
           d->tone2_dbfs + off, rep->unit, d->f1_hz, d->f2_hz, p.level + lab_input_offset(set), iu);
    printf("  IM3        : %.2f and %.2f %s, %.1f dBc%s\n", d->im3_low_dbfs + off, d->im3_high_dbfs + off, rep->unit,
           d->im3_dbc, d->im3_measured ? "" : " (under the noise: a bound)");
    printf("  IM5        : %.2f and %.2f %s, %.1f dBc%s\n", d->im5_low_dbfs + off, d->im5_high_dbfs + off, rep->unit,
           d->im5_dbc, d->im5_measured ? "" : " (under the noise: a bound)");
    printf("  Gain       : %.2f dB\n", d->gain_db);
    printf("  IP3        : input %s%.2f %s, output %s%.2f %s\n", d->im3_measured ? "" : ">= ", d->iip3_dbfs, iu,
           d->im3_measured ? "" : ">= ", d->oip3_dbfs + off, rep->unit);
    printf("  IP5        : input %s%.2f %s, output %s%.2f %s\n", d->im5_measured ? "" : ">= ", d->iip5_dbfs, iu,
           d->im5_measured ? "" : ">= ", d->oip5_dbfs + off, rep->unit);
    printf("  Floor      : %.1f %s in the band of a line\n", d->floor_dbfs + off, rep->unit);
    for (i = 0; i < d->nspurs; i++)
        printf("  Spur       : %+.1f Hz, %.2f %s\n", d->spur_hz[i], d->spur_dbfs[i] + off, rep->unit);
}

static void imd_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_imd_result *d = &res->imd;
    const bool ok = res->imd_valid && d->found;
    const double off = rep->off;
    int i;

    printf("  \"imd\": {\n");
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        print_json_number("f1_offset_hz", d->f1_hz, false);
        print_json_number("f2_offset_hz", d->f2_hz, false);
        print_json_number("tone1_db", d->tone1_dbfs + off, false);
        print_json_number("tone2_db", d->tone2_dbfs + off, false);
        print_json_number("im3_low_db", d->im3_low_dbfs + off, false);
        print_json_number("im3_high_db", d->im3_high_dbfs + off, false);
        print_json_number("im5_low_db", d->im5_low_dbfs + off, false);
        print_json_number("im5_high_db", d->im5_high_dbfs + off, false);
        printf("    \"im3_measured\": %s,\n", d->im3_measured ? "true" : "false");
        printf("    \"im5_measured\": %s,\n", d->im5_measured ? "true" : "false");
        print_json_number("im3_dbc", d->im3_dbc, false);
        print_json_number("im5_dbc", d->im5_dbc, false);
        print_json_number("gain_db", d->gain_db, false);
        print_json_number("iip3_db", d->iip3_dbfs, false);
        print_json_number("oip3_db", d->oip3_dbfs + off, false);
        print_json_number("iip5_db", d->iip5_dbfs, false);
        print_json_number("oip5_db", d->oip5_dbfs + off, false);
        print_json_number("floor_db", d->floor_dbfs + off, false);
        printf("    \"spurs\": [");
        for (i = 0; i < d->nspurs; i++)
            printf("%s{\"offset_hz\": %.3f, \"level_db\": %.3f}", i ? ", " : "", d->spur_hz[i], d->spur_dbfs[i] + off);
        printf("]\n");
    }
    printf("  }");
}

/* Command line */
/*--------------*/

enum { IM_OPT_ON, IM_OPT_SPACING, IM_OPT_LEVEL };

static const struct option imd_options[] = {
    { "imd",         no_argument,       NULL, IM_OPT_ON },
    { "imd-spacing", required_argument, NULL, IM_OPT_SPACING },
    { "imd-level",   required_argument, NULL, IM_OPT_LEVEL },
    { NULL, 0, NULL, 0 }
};

/* A level of a stimulus: below 0 dBFS, -80 at the lowest. */
static int lab_level(const char *arg, double *v)
{
    if (m2sdr_cli_parse_double(arg, v) != 0 || !(*v < 0.0 && *v >= -80.0)) {
        m2sdr_cli_error("invalid level '%s' (expected -80 to 0 dBFS, below 0)", arg);
        return -1;
    }
    return 0;
}

static int lab_value(const char *arg, double *v, bool positive)
{
    if (m2sdr_cli_parse_double(arg, v) != 0 || (positive && !(*v > 0))) {
        m2sdr_cli_error("invalid value '%s'", arg);
        return -1;
    }
    return 0;
}

static int imd_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_lab_settings *lab = &cli->set->lab;

    switch (id) {
    case IM_OPT_ON:      cli->set->imd_on = true; return 0;
    case IM_OPT_SPACING: return lab_value(arg, &lab->imd_spacing_hz, true);
    case IM_OPT_LEVEL:   return lab_level(arg, &lab->imd_level_dbfs);
    }
    return 0;
}

static void imd_help(void)
{
    printf("IMD options:\n"
           "      --imd              With --report: two tones, IM3, IM5, IP3, IP5, spurs.\n"
           "      --imd-spacing HZ   Between the two tones (default: a hundredth of the sample rate).\n"
           "      --imd-level DBFS   Of each tone (default: -12). --tone-offset: their centre.\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

static void draw_imd(struct an_ui *ui, bool live, bool *changed)
{
    static float trace[8192];
    const struct an_result *res = &ui->res;
    const struct m2sdr_imd_result *d = &res->imd;
    const struct m2sdr_psd *psd = &res->psd;
    struct m2sdr_plot plot;
    struct imd_plan p;
    double v, lo_hz, hi_hz, top = -200.0;
    size_t k0, k1, k, n;
    char text[160];
    int i;

    lab_tx_controls(ui, live, changed);
    imd_plan(&ui->set, &p);
    v = p.centre;
    if (input_scaled("Centre (kHz)", &v, 1e3, "%.1f")) {
        ui->set.lab.tone_offset_hz = v;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 230.0f);
    v = p.spacing;
    if (input_scaled("Spacing (kHz)", &v, 1e3, "%.2f")) {
        ui->set.lab.imd_spacing_hz = v > 0 ? v : 0.0;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 230.0f);
    v = p.level;
    if (input_scaled("Each tone (dBFS)", &v, 1.0, "%.1f")) {
        ui->set.lab.imd_level_dbfs = v > -6.1 ? -6.1 : v;
        *changed = true;
    }

    if (!res->set.imd_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->imd_valid || !d->found) {
        igTextUnformatted("The two tones are not in the capture: TX to RX through the device.", NULL);
        return;
    }
    if (!psd->power || !psd->bins)
        return;
    /* The spectrum around the products up to the seventh order. */
    lo_hz = d->f1_hz - 3.5 * (d->f2_hz - d->f1_hz);
    hi_hz = d->f2_hz + 3.5 * (d->f2_hz - d->f1_hz);
    k0 = (size_t)fmax(0.0, floor(lo_hz / psd->bin_hz + (double)(psd->bins / 2)));
    k1 = (size_t)fmin((double)psd->bins - 1, ceil(hi_hz / psd->bin_hz + (double)(psd->bins / 2)));
    n  = k1 > k0 ? k1 - k0 + 1 : 0;
    if (n > sizeof(trace) / sizeof(trace[0]))
        n = sizeof(trace) / sizeof(trace[0]);
    for (k = 0; k < n; k++) {
        trace[k] = (float)m2sdr_power_db(psd->power[k0 + k]);
        top = fmax(top, trace[k]);
    }
    if (n < 2)
        return;
    if (!m2sdr_plot_begin(&plot, "##imd", (ImVec2){ 0.0f, 0.0f }, res->set.freq_hz + m2sdr_psd_freq(psd, k0),
                          res->set.freq_hz + m2sdr_psd_freq(psd, k0 + n - 1), M2SDR_PLOT_FREQ,
                          10.0 * floor((d->floor_dbfs - 20.0) / 10.0), 10.0 * ceil(top / 10.0) + 10.0,
                          "Two tones, dBFS (window of the spectrum view)"))
        return;
    m2sdr_plot_trace(&plot, trace, n, res->set.freq_hz + m2sdr_psd_freq(psd, k0),
                     res->set.freq_hz + m2sdr_psd_freq(psd, k0 + n - 1), 0xff40d0ffu);
    m2sdr_plot_marker(&plot, res->set.freq_hz + d->f1_hz, d->tone1_dbfs, "f1", 0xffffffffu);
    m2sdr_plot_marker(&plot, res->set.freq_hz + d->f2_hz, d->tone2_dbfs, "f2", 0xffffffffu);
    snprintf(text, sizeof(text), "IM3 %.1f dBc", d->im3_dbc);
    m2sdr_plot_marker(&plot, res->set.freq_hz + 2.0 * d->f1_hz - d->f2_hz, d->im3_low_dbfs, text, 0xffc0c0ffu);
    m2sdr_plot_marker(&plot, res->set.freq_hz + 2.0 * d->f2_hz - d->f1_hz, d->im3_high_dbfs, "IM3", 0xffc0c0ffu);
    snprintf(text, sizeof(text), "IM5 %.1f dBc", d->im5_dbc);
    m2sdr_plot_marker(&plot, res->set.freq_hz + 3.0 * d->f1_hz - 2.0 * d->f2_hz, d->im5_low_dbfs, text, 0xffa0ffa0u);
    m2sdr_plot_marker(&plot, res->set.freq_hz + 3.0 * d->f2_hz - 2.0 * d->f1_hz, d->im5_high_dbfs, "IM5", 0xffa0ffa0u);
    m2sdr_plot_hline(&plot, d->floor_dbfs, 0x30ffffffu);
    snprintf(text, sizeof(text), "OIP3 %s%.1f dBFS, IIP3 %s%.1f %s", d->im3_measured ? "" : ">= ", d->oip3_dbfs,
             d->im3_measured ? "" : ">= ", d->iip3_dbfs, lab_input_unit(&res->set));
    m2sdr_plot_text(&plot, 8.0f, -20.0f, text, 0xffc8c8c8u);
    for (i = 0; i < d->nspurs; i++)
        if (d->spur_hz[i] >= lo_hz && d->spur_hz[i] <= hi_hz)
            m2sdr_plot_marker(&plot, res->set.freq_hz + d->spur_hz[i], d->spur_dbfs[i], "spur", 0xff4040ffu);
    m2sdr_plot_end(&plot);
}

static void draw_imd_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_imd_result *d = &res->imd;
    const char *iu = lab_input_unit(&res->set);
    int i;

    igSeparatorText("IMD");
    if (!res->imd_valid || !d->found) {
        igTextUnformatted("No tones.", NULL);
        return;
    }
    if (igBeginTable("##imd", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Tones", "%.2f, %.2f dBFS", d->tone1_dbfs, d->tone2_dbfs);
        label_value("IM3", "%.1f dBc%s", d->im3_dbc, d->im3_measured ? "" : " (bound)");
        label_value("IM5", "%.1f dBc%s", d->im5_dbc, d->im5_measured ? "" : " (bound)");
        label_value("Gain", "%.2f dB", d->gain_db);
        label_value("IIP3", "%s%.2f %s", d->im3_measured ? "" : ">= ", d->iip3_dbfs, iu);
        label_value("OIP3", "%s%.2f dBFS", d->im3_measured ? "" : ">= ", d->oip3_dbfs);
        label_value("IIP5", "%s%.2f %s", d->im5_measured ? "" : ">= ", d->iip5_dbfs, iu);
        label_value("Floor", "%.1f dBFS", d->floor_dbfs);
        for (i = 0; i < d->nspurs && i < 4; i++) {
            char name[32];
            snprintf(name, sizeof(name), "Spur %+.0f Hz", d->spur_hz[i]);
            label_value(name, "%.1f dBFS", d->spur_dbfs[i]);
        }
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_imd = {
    .key          = "imd",
    .title        = "IMD",
    .category     = AN_CAT_LAB,
    .on           = offsetof(struct an_settings, imd_on),
    .valid        = offsetof(struct an_result, imd_valid),
    .report_rank  = 42,
    .options      = imd_options,
    .option       = imd_option,
    .help         = imd_help,
    .samples      = imd_samples,
    .analyse      = imd_analyse,
    .stimulus     = imd_stimulus,
    .report_text  = imd_report_text,
    .report_json  = imd_report_json,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_imd,
    .results      = draw_imd_results,
#endif
};
