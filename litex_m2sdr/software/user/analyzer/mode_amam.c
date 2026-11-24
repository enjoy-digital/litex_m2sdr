/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: AM/AM and AM/PM of a device, with the EVM and the ACLR of its
 * output, from a wideband signal sent by TX (OFDM-like noise or QAM).
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
#include <strings.h>

#include "m2sdr_cli.h"

#include "analyzer.h"

#define AMAM_PERIOD     16384
#define AMAM_PERIODS    4
#define AMAM_LEVEL_DBFS -15.0
#define AMAM_SEED       1           /* The stimulus is the same from capture to capture. */

/* Stimulus */
/*----------*/

static float complex *amam_stimulus(const struct an_settings *set, size_t *period)
{
    const double level = set->lab.amam_level_dbfs != 0.0 ? set->lab.amam_level_dbfs : AMAM_LEVEL_DBFS;
    float complex *x = malloc(AMAM_PERIOD * sizeof(*x));
    int rc;

    if (!x)
        return NULL;
    if (set->lab.amam_stimulus == 1) {
        const enum m2sdr_modulation mod = set->lab.amam_modulation > 0 ? (enum m2sdr_modulation)set->lab.amam_modulation
                                                                       : M2SDR_MOD_16QAM;
        rc = m2sdr_lab_qam(x, AMAM_PERIOD, mod, set->lab.amam_sps > 0 ? (unsigned)set->lab.amam_sps : 8u, 0.35, level,
                           AMAM_SEED, NULL);
    } else {
        rc = m2sdr_lab_noise(x, AMAM_PERIOD, set->sample_rate,
                             set->lab.amam_bw_hz > 0 ? set->lab.amam_bw_hz : 0.2 * set->sample_rate, level, AMAM_SEED, NULL);
    }
    if (rc != 0) {
        free(x);
        return NULL;
    }
    *period = AMAM_PERIOD;
    return x;
}

/* Analysis */
/*----------*/

static size_t amam_samples(const struct an_settings *set)
{
    (void)set;
    return (size_t)AMAM_PERIODS * AMAM_PERIOD;
}

static void amam_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    size_t period;
    float complex *ref = amam_stimulus(set, &period);

    if (!ref)
        return;
    res->amam_valid = m2sdr_amam_analyse(x, count, set->sample_rate, ref, period, NULL, &res->amam) == 0;
    free(ref);
}

/* Reports */
/*---------*/

static const char *amam_stimulus_name(const struct an_settings *set, char *buf, size_t len)
{
    if (set->lab.amam_stimulus == 1)
        snprintf(buf, len, "%s, %d samples per symbol",
                 m2sdr_modulation_name(set->lab.amam_modulation > 0 ? (enum m2sdr_modulation)set->lab.amam_modulation
                                                                    : M2SDR_MOD_16QAM),
                 set->lab.amam_sps > 0 ? set->lab.amam_sps : 8);
    else
        snprintf(buf, len, "OFDM-like noise, %.3f MHz",
                 (set->lab.amam_bw_hz > 0 ? set->lab.amam_bw_hz : 0.2 * set->sample_rate) / 1e6);
    return buf;
}

static void amam_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_amam_result *a = &res->amam;
    const double in_off = lab_input_offset(set);
    const char *iu = lab_input_unit(set);
    char name[96];
    int i;

    printf("\nAM/AM:\n");
    printf("  Stimulus   : %s, %.1f dBFS, TX attenuation %d dB\n", amam_stimulus_name(set, name, sizeof(name)),
           set->lab.amam_level_dbfs != 0.0 ? set->lab.amam_level_dbfs : AMAM_LEVEL_DBFS, set->lab.tx_att);
    if (!res->amam_valid || !a->found) {
        printf("  The stimulus is not in the capture (correlation %.1f dB): TX to RX through the device?\n",
               res->amam_valid ? a->sync_quality : 0.0);
        return;
    }
    printf("  Input      : %.2f %s mean, %.2f %s peak\n", a->input_rms_dbfs + in_off, iu, a->input_peak_dbfs + in_off, iu);
    printf("  Output     : %.2f %s mean, %.2f %s peak\n", a->output_rms_dbfs + rep->off, rep->unit,
           a->output_peak_dbfs + rep->off, rep->unit);
    printf("  Gain       : %.2f dB small-signal, phase %.2f degrees\n", a->gain_db - in_off + rep->off, a->phase_deg);
    printf("  At the top : %.2f dB of compression, %+.2f degrees of AM/PM, at %.2f %s in\n", a->compression_db,
           a->am_pm_deg, a->compression_in_dbfs + in_off, iu);
    printf("  EVM        : %.3f %% (%.1f dB), of the waveform after its gain\n", 100.0 * a->evm_rms, a->evm_db);
    if (a->aclr) {
        printf("  ACLR       : channel %.3f MHz, %.2f %s;", a->channel_bw_hz / 1e6, a->acpr.channel_db + rep->off, rep->unit);
        for (i = 0; i < a->acpr.adjacent; i++)
            printf(" %s%.1f / %.1f dBc", i ? ", alternate " : "adjacent ", a->acpr.lower_dbc[i], a->acpr.upper_dbc[i]);
        printf("\n");
    }
}

static void amam_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_amam_result *a = &res->amam;
    const bool ok = res->amam_valid && a->found;
    const double in_off = lab_input_offset(set);
    double in[M2SDR_AMAM_BINS], gain[M2SDR_AMAM_BINS], phase[M2SDR_AMAM_BINS];
    int b, n = 0, i;

    printf("  \"amam\": {\n");
    print_json_number("sync_quality_db", res->amam_valid ? a->sync_quality : 0.0, false);
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        for (b = 0; b < a->bins; b++) {
            if (a->bin_count[b] < 100)
                continue;
            in[n]    = a->bin_in_dbfs[b] + in_off;
            gain[n]  = a->bin_gain_db[b];
            phase[n] = a->bin_phase_deg[b];
            n++;
        }
        print_json_number("input_rms_db", a->input_rms_dbfs + in_off, false);
        print_json_number("output_rms_db", a->output_rms_dbfs + rep->off, false);
        print_json_number("gain_db", a->gain_db - in_off + rep->off, false);
        print_json_number("phase_deg", a->phase_deg, false);
        print_json_number("compression_db", a->compression_db, false);
        print_json_number("am_pm_deg", a->am_pm_deg, false);
        print_json_number("compression_in_db", a->compression_in_dbfs + in_off, false);
        print_json_number("evm_percent", 100.0 * a->evm_rms, false);
        print_json_number("delay_samples", a->delay_samples, false);
        if (a->aclr) {
            print_json_number("channel_bw_hz", a->channel_bw_hz, false);
            printf("    \"aclr_lower_dbc\": [");
            for (i = 0; i < a->acpr.adjacent; i++)
                printf("%s%.3f", i ? ", " : "", a->acpr.lower_dbc[i]);
            printf("],\n    \"aclr_upper_dbc\": [");
            for (i = 0; i < a->acpr.adjacent; i++)
                printf("%s%.3f", i ? ", " : "", a->acpr.upper_dbc[i]);
            printf("],\n");
        }
        lab_json_array("bin_in_db", in, n, 0.0, false);
        lab_json_array("bin_gain_db", gain, n, 0.0, false);
        lab_json_array("bin_phase_deg", phase, n, 0.0, true);
    }
    printf("  }");
}

/* Command line */
/*--------------*/

enum { AM_OPT_ON, AM_OPT_STIMULUS, AM_OPT_BW, AM_OPT_LEVEL, AM_OPT_MODULATION, AM_OPT_SPS };

static const struct option amam_options[] = {
    { "amam",            no_argument,       NULL, AM_OPT_ON },
    { "amam-stimulus",   required_argument, NULL, AM_OPT_STIMULUS },
    { "amam-bw",         required_argument, NULL, AM_OPT_BW },
    { "amam-level",      required_argument, NULL, AM_OPT_LEVEL },
    { "amam-modulation", required_argument, NULL, AM_OPT_MODULATION },
    { "amam-sps",        required_argument, NULL, AM_OPT_SPS },
    { NULL, 0, NULL, 0 }
};

static int amam_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_lab_settings *lab = &cli->set->lab;
    double v;
    int n;

    switch (id) {
    case AM_OPT_ON: cli->set->amam_on = true; break;
    case AM_OPT_STIMULUS:
        if (strcmp(arg, "noise") == 0) {
            lab->amam_stimulus = 0;
        } else if (strcmp(arg, "qam") == 0) {
            lab->amam_stimulus = 1;
        } else {
            m2sdr_cli_invalid_choice("amam stimulus", arg, "noise or qam");
            return -1;
        }
        break;
    case AM_OPT_BW:
        if (m2sdr_cli_parse_double(arg, &v) != 0 || !(v > 0)) {
            m2sdr_cli_error("invalid value '%s'", arg);
            return -1;
        }
        lab->amam_bw_hz = v;
        break;
    case AM_OPT_LEVEL:
        if (m2sdr_cli_parse_double(arg, &v) != 0 || !(v < 0.0 && v >= -80.0)) {
            m2sdr_cli_error("invalid level '%s' (expected -80 to 0 dBFS, below 0)", arg);
            return -1;
        }
        lab->amam_level_dbfs = v;
        break;
    case AM_OPT_MODULATION:
        for (n = M2SDR_MOD_BPSK; n < M2SDR_MOD_COUNT; n++) {
            if (strcasecmp(arg, m2sdr_modulation_name((enum m2sdr_modulation)n)) == 0)
                break;
        }
        if (n == M2SDR_MOD_COUNT) {
            m2sdr_cli_invalid_choice("amam modulation", arg, "bpsk, qpsk, 8psk, 16qam, 32qam, 64qam, 128qam or 256qam");
            return -1;
        }
        lab->amam_modulation = n;
        break;
    case AM_OPT_SPS:
        if (m2sdr_cli_parse_int_range(arg, 2, 64, &lab->amam_sps) != 0 || (lab->amam_sps & (lab->amam_sps - 1))) {
            m2sdr_cli_error("invalid samples per symbol '%s' (expected a power of two from 2 to 64)", arg);
            return -1;
        }
        break;
    }
    return 0;
}

static void amam_help(void)
{
    printf("AM/AM options:\n"
           "      --amam             With --report: AM/AM, AM/PM, EVM and ACLR of a wideband signal.\n"
           "      --amam-stimulus S  noise (OFDM-like) or qam (default: noise).\n"
           "      --amam-bw HZ       Band of the noise (default: a fifth of the sample rate).\n"
           "      --amam-level DBFS  Mean power (default: -15).\n"
           "      --amam-modulation NAME, --amam-sps N   QAM: constellation and samples per symbol\n"
           "                         (default: 16qam, 8).\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

static void draw_amam(struct an_ui *ui, bool live, bool *changed)
{
    static const char *const kinds[] = { "OFDM-like noise", "QAM" };
    static float bx[M2SDR_AMAM_BINS], bg[M2SDR_AMAM_BINS], bp[M2SDR_AMAM_BINS];
    const struct an_result *res = &ui->res;
    const struct m2sdr_amam_result *a = &res->amam;
    struct m2sdr_plot plot;
    ImVec2 avail;
    double v, xmax = 0, glo = 0, ghi = 0, plo = 0, phi = 0;
    char text[160];
    int kind = ui->set.lab.amam_stimulus, b, n = 0;

    lab_tx_controls(ui, live, changed);
    igSetNextItemWidth(160.0f);
    if (igCombo_Str_arr("Stimulus", &kind, kinds, 2, -1)) {
        ui->set.lab.amam_stimulus = kind;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 230.0f);
    v = ui->set.lab.amam_level_dbfs != 0.0 ? ui->set.lab.amam_level_dbfs : AMAM_LEVEL_DBFS;
    if (input_scaled("Level (dBFS)", &v, 1.0, "%.1f")) {
        ui->set.lab.amam_level_dbfs = v > -6.0 ? -6.0 : v;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 230.0f);
    if (kind == 0) {
        v = ui->set.lab.amam_bw_hz > 0 ? ui->set.lab.amam_bw_hz : 0.2 * ui->set.sample_rate;
        if (input_scaled("Bandwidth (MHz)", &v, 1e6, "%.3f")) {
            ui->set.lab.amam_bw_hz = v;
            *changed = true;
        }
    } else {
        int sps = ui->set.lab.amam_sps > 0 ? ui->set.lab.amam_sps : 8, l2 = 0;
        static const char *const spss[] = { "2", "4", "8", "16", "32" };
        while ((2 << l2) < sps && l2 < 4)
            l2++;
        igSetNextItemWidth(80.0f);
        if (igCombo_Str_arr("Samples/symbol", &l2, spss, 5, -1)) {
            ui->set.lab.amam_sps = 2 << l2;
            *changed = true;
        }
    }

    if (!res->set.amam_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->amam_valid || !a->found) {
        igText("The stimulus is not in the capture (correlation %.1f dB): TX to RX through the device.",
               res->amam_valid ? a->sync_quality : 0.0);
        return;
    }
    for (b = 0; b < a->bins; b++) {
        if (a->bin_count[b] < 100)
            continue;
        bx[n] = (float)pow(10.0, a->bin_in_dbfs[b] / 20.0);
        bg[n] = (float)(bx[n] * pow(10.0, a->bin_gain_db[b] / 20.0));
        bp[n] = a->bin_phase_deg[b];
        glo = fmin(glo, bp[n]);
        ghi = fmax(ghi, bp[n]);
        n++;
    }
    for (b = 0; b < a->npoints; b++)
        xmax = fmax(xmax, a->point_in[b]);
    plo = fmin(glo, -2.0) - 3.0;
    phi = fmax(ghi, 2.0) + 3.0;
    igGetContentRegionAvail(&avail);
    if (m2sdr_plot_begin(&plot, "##amam", (ImVec2){ 0.5f * avail.x - 4.0f, 0.0f }, 0.0, xmax * 1.05, M2SDR_PLOT_PLAIN,
                         0.0, xmax * 1.15, "AM/AM: output over the small-signal gain against the input amplitude")) {
        static const float diag[2] = { 0.0f, 10.0f };
        m2sdr_plot_xy(&plot, diag, diag, 2, 0x40ffffffu);
        m2sdr_plot_points(&plot, a->point_in, a->point_out, (size_t)a->npoints, 1.0f, 0x6040d0ffu);
        m2sdr_plot_xy(&plot, bx, bg, (size_t)n, 0xff40d0ffu);
        snprintf(text, sizeof(text), "%.2f dB of compression at the top, EVM %.2f %%", a->compression_db, 100.0 * a->evm_rms);
        m2sdr_plot_text(&plot, 8.0f, -20.0f, text, 0xffc8c8c8u);
        m2sdr_plot_end(&plot);
    }
    igSameLine(0.0f, 8.0f);
    if (m2sdr_plot_begin(&plot, "##ampm", (ImVec2){ 0.0f, 0.0f }, 0.0, xmax * 1.05, M2SDR_PLOT_PLAIN, plo, phi,
                         "AM/PM: phase from the small-signal one, degrees")) {
        m2sdr_plot_points(&plot, a->point_in, a->point_phase_deg, (size_t)a->npoints, 1.0f, 0x60ffa040u);
        m2sdr_plot_xy(&plot, bx, bp, (size_t)n, 0xffffa040u);
        if (a->aclr) {
            snprintf(text, sizeof(text), "ACLR %.1f / %.1f dBc", a->acpr.lower_dbc[0], a->acpr.upper_dbc[0]);
            m2sdr_plot_text(&plot, 8.0f, -20.0f, text, 0xffc8c8c8u);
        }
        m2sdr_plot_end(&plot);
    }
}

static void draw_amam_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_amam_result *a = &res->amam;
    int i;

    igSeparatorText("AM/AM");
    if (!res->amam_valid || !a->found) {
        igTextUnformatted("No stimulus.", NULL);
        return;
    }
    if (igBeginTable("##amam", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Gain", "%.2f dB, %.1f deg", a->gain_db, a->phase_deg);
        label_value("Compression", "%.2f dB at the top", a->compression_db);
        label_value("AM/PM", "%+.2f deg at the top", a->am_pm_deg);
        label_value("EVM", "%.3f %%", 100.0 * a->evm_rms);
        label_value("Output", "%.2f dBFS mean", a->output_rms_dbfs);
        for (i = 0; a->aclr && i < a->acpr.adjacent; i++) {
            char name[32];
            snprintf(name, sizeof(name), "ACLR %d", i + 1);
            label_value(name, "%.1f / %.1f dBc", a->acpr.lower_dbc[i], a->acpr.upper_dbc[i]);
        }
        label_value("Correlation", "%.1f dB", a->sync_quality);
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_amam = {
    .key          = "amam",
    .title        = "AM/AM",
    .category     = AN_CAT_LAB,
    .on           = offsetof(struct an_settings, amam_on),
    .valid        = offsetof(struct an_result, amam_valid),
    .report_rank  = 43,
    .options      = amam_options,
    .option       = amam_option,
    .help         = amam_help,
    .samples      = amam_samples,
    .analyse      = amam_analyse,
    .stimulus     = amam_stimulus,
    .report_text  = amam_report_text,
    .report_json  = amam_report_json,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_amam,
    .results      = draw_amam_results,
#endif
};
