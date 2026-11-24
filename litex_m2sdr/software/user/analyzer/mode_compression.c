/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: compression of a device, gain against the input level and its
 * 1 dB and 3 dB compression points, from a tone sent by TX.
 *
 * The levels are stepped in the stimulus itself: one period holds a staircase of the tone, a
 * step every COMP_STEP samples from the lowest level to the highest, so one capture gives every
 * step. The TX attenuation stays where it is set and only moves all the levels together; the
 * TX chain of the board is taken as linear over the staircase (the steps are digital).
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

#define COMP_STEP       8192       /* Samples of a step. */
#define COMP_BLOCKS     16         /* Blocks of a step, to find where the staircase starts. */
#define COMP_EDGE       (COMP_STEP / 8)   /* Samples at each end of a step left out. */
#define COMP_TOP_DBFS   -1.0
#define COMP_RANGE_DB   30.0
#define COMP_STEP_DB    1.0

/* Stimulus */
/*----------*/

struct comp_plan {
    int    steps;
    double top, step_db, freq;
};

static void comp_plan(const struct an_settings *set, struct comp_plan *p)
{
    const double range = set->lab.comp_range_db > 0 ? set->lab.comp_range_db : COMP_RANGE_DB;
    const double offset = set->lab.tone_offset_hz != 0.0 ? set->lab.tone_offset_hz : 0.1 * set->sample_rate;

    p->top     = set->lab.comp_top_dbfs != 0.0 ? set->lab.comp_top_dbfs : COMP_TOP_DBFS;
    p->step_db = set->lab.comp_step_db > 0 ? set->lab.comp_step_db : COMP_STEP_DB;
    p->steps   = (int)floor(range / p->step_db + 0.5) + 1;
    if (p->steps > M2SDR_COMPRESSION_MAX_STEPS)
        p->steps = M2SDR_COMPRESSION_MAX_STEPS;
    if (p->steps < 3)
        p->steps = 3;
    /* Whole cycles in a step: the tone goes on from step to step without a jump of phase. */
    p->freq = round(offset * COMP_STEP / set->sample_rate) * set->sample_rate / COMP_STEP;
}

static double comp_level(const struct comp_plan *p, int k)
{
    return p->top - (p->steps - 1 - k) * p->step_db;
}

static float complex *comp_stimulus(const struct an_settings *set, size_t *period)
{
    struct comp_plan p;
    float complex *x;
    int k;

    comp_plan(set, &p);
    if (!(fabs(p.freq) < 0.45 * set->sample_rate))
        return NULL;
    x = malloc((size_t)p.steps * COMP_STEP * sizeof(*x));
    if (!x)
        return NULL;
    for (k = 0; k < p.steps; k++)
        (void)m2sdr_lab_tone(x + (size_t)k * COMP_STEP, COMP_STEP, set->sample_rate, p.freq, comp_level(&p, k), NULL);
    *period = (size_t)p.steps * COMP_STEP;
    return x;
}

/* Analysis */
/*----------*/

static size_t comp_samples(const struct an_settings *set)
{
    struct comp_plan p;

    comp_plan(set, &p);
    return 2 * (size_t)p.steps * COMP_STEP + COMP_STEP;
}

/* Start of the lowest step: the largest fall of the power from one block to the next, from the
 * highest step back to the lowest, within the first period. */
static size_t comp_start(const float complex *x, size_t count, size_t period)
{
    const size_t block = COMP_STEP / COMP_BLOCKS;
    double prev = -1, best = 0;
    size_t b, start = 0;

    for (b = 0; (b + 1) * block <= count && b * block <= period + block; b++) {
        double p = 0;
        size_t i;

        for (i = b * block; i < (b + 1) * block; i++)
            p += crealf(x[i]) * crealf(x[i]) + cimagf(x[i]) * cimagf(x[i]);
        if (prev > 0 && p > 0 && prev / p > best) {
            best  = prev / p;
            start = b * block;
        }
        prev = p;
    }
    return start % period;
}

static void comp_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    struct m2sdr_compression_step steps[M2SDR_COMPRESSION_MAX_STEPS];
    struct comp_plan p;
    size_t period, start;
    int k;

    comp_plan(set, &p);
    period = (size_t)p.steps * COMP_STEP;
    res->compression_freq_hz = p.freq;
    if (count < period + COMP_STEP)
        return;
    start = comp_start(x, count, period);
    if (start + period > count)
        start = start >= COMP_STEP ? start - COMP_STEP : start;
    for (k = 0; k < p.steps; k++) {
        steps[k].input_dbfs = comp_level(&p, k) + lab_input_offset(set);
        steps[k].x          = x + start + (size_t)k * COMP_STEP + COMP_EDGE;
        steps[k].count      = COMP_STEP - 2 * COMP_EDGE;
    }
    res->compression_valid = m2sdr_compression_analyse(steps, p.steps, set->sample_rate, p.freq, &res->compression) == 0;
}

/* Reports */
/*---------*/

static void comp_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_compression_result *c = &res->compression;
    const char *iu = lab_input_unit(set);
    struct comp_plan p;

    comp_plan(set, &p);
    printf("\nCompression:\n");
    printf("  Stimulus   : tone at %+.3f kHz, %d steps of %.1f dB up to %.1f dBFS, TX attenuation %d dB\n", p.freq / 1e3,
           p.steps, p.step_db, p.top, set->lab.tx_att);
    if (!res->compression_valid || !c->found) {
        printf("  No tone in the steps: TX to RX through the device, the TX attenuation low enough?\n");
        return;
    }
    printf("  Gain       : %.2f dB small-signal (output %s over input %s), %d steps with a tone\n", c->small_signal_gain_db,
           rep->unit, iu, c->steps);
    if (c->p1db)
        printf("  P1dB       : input %.2f %s, output %.2f %s\n", c->p1db_in_dbfs, iu, c->p1db_out_dbfs + rep->off, rep->unit);
    else
        printf("  P1dB       : not reached (%.2f dB of compression at most)\n", c->max_compression_db);
    if (c->p3db)
        printf("  P3dB       : input %.2f %s, output %.2f %s\n", c->p3db_in_dbfs, iu, c->p3db_out_dbfs + rep->off, rep->unit);
    printf("  Highest out: %.2f %s\n", c->max_output_dbfs + rep->off, rep->unit);
}

static void comp_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_compression_result *c = &res->compression;
    const bool ok = res->compression_valid && c->found;

    printf("  \"compression\": {\n");
    print_json_number("tone_offset_hz", res->compression_freq_hz, false);
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        printf("    \"steps\": %d,\n", c->steps);
        print_json_number("small_signal_gain_db", c->small_signal_gain_db, false);
        printf("    \"p1db\": %s,\n", c->p1db ? "true" : "false");
        if (c->p1db) {
            print_json_number("p1db_in_db", c->p1db_in_dbfs, false);
            print_json_number("p1db_out_db", c->p1db_out_dbfs + rep->off, false);
        }
        printf("    \"p3db\": %s,\n", c->p3db ? "true" : "false");
        if (c->p3db) {
            print_json_number("p3db_in_db", c->p3db_in_dbfs, false);
            print_json_number("p3db_out_db", c->p3db_out_dbfs + rep->off, false);
        }
        print_json_number("max_compression_db", c->max_compression_db, false);
        print_json_number("max_output_db", c->max_output_dbfs + rep->off, false);
        lab_json_array("input_db", c->input_dbfs, c->steps, 0.0, false);
        lab_json_array("output_db", c->output_dbfs, c->steps, rep->off, false);
        lab_json_array("gain_db", c->gain_db, c->steps, 0.0, true);
    }
    printf("  }");
}

/* Command line */
/*--------------*/

enum { CP_OPT_ON, CP_OPT_TONE, CP_OPT_TOP, CP_OPT_RANGE, CP_OPT_STEP };

static const struct option comp_options[] = {
    { "compression", no_argument,       NULL, CP_OPT_ON },
    { "tone-offset", required_argument, NULL, CP_OPT_TONE },
    { "comp-top",    required_argument, NULL, CP_OPT_TOP },
    { "comp-range",  required_argument, NULL, CP_OPT_RANGE },
    { "comp-step",   required_argument, NULL, CP_OPT_STEP },
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

static int comp_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_lab_settings *lab = &cli->set->lab;

    switch (id) {
    case CP_OPT_ON:    cli->set->compression_on = true; return 0;
    case CP_OPT_TONE:  return lab_value(arg, &lab->tone_offset_hz, false);
    case CP_OPT_TOP:   return lab_level(arg, &lab->comp_top_dbfs);
    case CP_OPT_RANGE: return lab_value(arg, &lab->comp_range_db, true);
    case CP_OPT_STEP:  return lab_value(arg, &lab->comp_step_db, true);
    }
    return 0;
}

static void comp_help(void)
{
    printf("Compression options:\n"
           "      --compression      With --report: gain against the input level, P1dB, P3dB.\n"
           "      --tone-offset HZ   Compression tone, centre of the two tones of --imd (default: a\n"
           "                         tenth of the sample rate).\n"
           "      --comp-top DBFS, --comp-range DB, --comp-step DB   Steps of the compression tone\n"
           "                         (default: -1, 30 and 1).\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

static void draw_compression(struct an_ui *ui, bool live, bool *changed)
{
    static float fx[M2SDR_COMPRESSION_MAX_STEPS], fg[M2SDR_COMPRESSION_MAX_STEPS], fo[M2SDR_COMPRESSION_MAX_STEPS];
    const struct an_result *res = &ui->res;
    const struct m2sdr_compression_result *c = &res->compression;
    struct m2sdr_plot plot;
    struct comp_plan p;
    ImVec2 avail;
    double v, lo = 1e9, hi = -1e9;
    char text[160];
    int k;

    lab_tx_controls(ui, live, changed);
    comp_plan(&ui->set, &p);
    v = p.top;
    if (input_scaled("Top (dBFS)", &v, 1.0, "%.1f")) {
        ui->set.lab.comp_top_dbfs = v > -0.1 ? -0.1 : v;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 230.0f);
    v = ui->set.lab.comp_range_db > 0 ? ui->set.lab.comp_range_db : COMP_RANGE_DB;
    if (input_scaled("Range (dB)", &v, 1.0, "%.1f")) {
        ui->set.lab.comp_range_db = v < 3.0 ? 3.0 : v > 60.0 ? 60.0 : v;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 230.0f);
    v = p.step_db;
    if (input_scaled("Step (dB)", &v, 1.0, "%.2f")) {
        ui->set.lab.comp_step_db = v < 0.1 ? 0.1 : v > 10.0 ? 10.0 : v;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 230.0f);
    v = p.freq;
    if (input_scaled("Tone (kHz)", &v, 1e3, "%.1f")) {
        ui->set.lab.tone_offset_hz = v;
        *changed = true;
    }

    if (!res->set.compression_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->compression_valid || !c->found) {
        igTextUnformatted("No tone in the steps: TX to RX through the device.", NULL);
        return;
    }
    for (k = 0; k < c->steps; k++) {
        fx[k] = (float)c->input_dbfs[k];
        fg[k] = (float)c->gain_db[k];
        fo[k] = (float)(c->output_dbfs[k] - c->small_signal_gain_db);
        lo = fmin(lo, fg[k]);
        hi = fmax(hi, fg[k]);
    }
    igGetContentRegionAvail(&avail);
    snprintf(text, sizeof(text), "Gain, dB, against the input, %s", lab_input_unit(&res->set));
    if (m2sdr_plot_begin(&plot, "##compgain", (ImVec2){ 0.0f, 0.5f * avail.y }, fx[0], fx[c->steps - 1], M2SDR_PLOT_PLAIN,
                         fmin(lo, c->small_signal_gain_db - 4.0) - 1.0, hi + 1.0, text)) {
        m2sdr_plot_hline(&plot, c->small_signal_gain_db, 0x40ffffffu);
        m2sdr_plot_hline(&plot, c->small_signal_gain_db - 1.0, 0x30a0a0ffu);
        m2sdr_plot_xy(&plot, fx, fg, (size_t)c->steps, 0xff40d0ffu);
        m2sdr_plot_points(&plot, fx, fg, (size_t)c->steps, 2.5f, 0xff40d0ffu);
        if (c->p1db) {
            snprintf(text, sizeof(text), "P1dB in %.2f", c->p1db_in_dbfs);
            m2sdr_plot_marker(&plot, c->p1db_in_dbfs, c->small_signal_gain_db - 1.0, text, 0xffc0c0ffu);
        }
        if (c->p3db) {
            snprintf(text, sizeof(text), "P3dB in %.2f", c->p3db_in_dbfs);
            m2sdr_plot_marker(&plot, c->p3db_in_dbfs, c->small_signal_gain_db - 3.0, text, 0xffc0c0ffu);
        }
        m2sdr_plot_end(&plot);
    }
    /* Output against input, both on the input scale: a line of slope one while linear. */
    if (m2sdr_plot_begin(&plot, "##compout", (ImVec2){ 0.0f, 0.0f }, fx[0], fx[c->steps - 1], M2SDR_PLOT_PLAIN,
                         fx[0] - 3.0, fx[c->steps - 1] + 3.0, "Output less the small-signal gain, against the input")) {
        m2sdr_plot_xy(&plot, fx, fx, (size_t)c->steps, 0x40ffffffu);
        m2sdr_plot_xy(&plot, fx, fo, (size_t)c->steps, 0xff60ff60u);
        m2sdr_plot_end(&plot);
    }
}

static void draw_compression_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_compression_result *c = &res->compression;
    const char *iu = lab_input_unit(&res->set);

    igSeparatorText("Compression");
    if (!res->compression_valid || !c->found) {
        igTextUnformatted("No tone.", NULL);
        return;
    }
    if (igBeginTable("##comp", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Small-signal gain", "%.2f dB", c->small_signal_gain_db);
        if (c->p1db) {
            label_value("P1dB in", "%.2f %s", c->p1db_in_dbfs, iu);
            label_value("P1dB out", "%.2f dBFS", c->p1db_out_dbfs);
        } else {
            label_value("P1dB", "not reached (%.2f dB)", c->max_compression_db);
        }
        if (c->p3db)
            label_value("P3dB in", "%.2f %s", c->p3db_in_dbfs, iu);
        label_value("Highest output", "%.2f dBFS", c->max_output_dbfs);
        label_value("Steps", "%d", c->steps);
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_compression = {
    .key          = "compression",
    .title        = "Compression",
    .category     = AN_CAT_LAB,
    .on           = offsetof(struct an_settings, compression_on),
    .valid        = offsetof(struct an_result, compression_valid),
    .report_rank  = 41,
    .psd_short    = true,
    .options      = comp_options,
    .option       = comp_option,
    .help         = comp_help,
    .samples      = comp_samples,
    .analyse      = comp_analyse,
    .stimulus     = comp_stimulus,
    .report_text  = comp_report_text,
    .report_json  = comp_report_json,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_compression,
    .results      = draw_compression_results,
#endif
};
