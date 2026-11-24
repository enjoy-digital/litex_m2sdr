/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: demodulation of a single carrier with PSK or QAM symbols
 * (constellation, eye diagrams, EVM, MER, I/Q errors).
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
#include <string.h>
#include <strings.h>

#include "m2sdr_cli.h"

#include "analyzer.h"

/* Settings and result */
/*-----------------------*/

extern const struct an_mode an_mode_demod;

/* Its settings: its block of struct an_settings, read with SET(). */
struct demod_set {
    bool   demod_on;               /* Demodulation of the strongest signal. */
    int    demod_log2;             /* Its capture: 2^demod_log2 samples. */
    struct m2sdr_demod_config demod;
};

#define SET(set) ((struct demod_set *)an_mode_settings(&an_mode_demod, (set)))

/* The result of the mode: its block of struct an_result, read with RES(). */
struct demod_res {
    bool   demod_valid;            /* Measured; demod.signal and demod.locked say what was found. */
    struct m2sdr_demod_result demod;
};

#define RES(res) ((struct demod_res *)an_mode_result(&an_mode_demod, (res)))

/* Analysis */
/*----------*/

static size_t demod_samples(const struct an_settings *set)
{
    return (size_t)1 << SET(set)->demod_log2;
}

static void demod_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = count < ((size_t)1 << SET(set)->demod_log2) ? count : (size_t)1 << SET(set)->demod_log2;

    RES(res)->demod_valid = m2sdr_demod(x, n, set->sample_rate, &SET(set)->demod, &RES(res)->demod) == 0;
}

/* Reports */
/*---------*/

static void demod_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const char *unit = rep->unit;
    const struct m2sdr_demod_result *d = &RES(res)->demod;

    printf("\nModulation:\n");
    if (!RES(res)->demod_valid || !d->signal) {
        printf("  No signal with a symbol rate was found: tune on it, with a sample rate of a few\n"
               "  times its bandwidth, or give --symbol-rate.\n");
    } else {
        printf("  Type       : %s%s\n", m2sdr_modulation_name(d->modulation),
               d->locked ? "" : " (NOT LOCKED: the figures below are not valid)");
        printf("  Symbol rate: %.3f symbols/s, roll-off %.2f\n", d->symbol_rate, d->rolloff);
        printf("  Carrier    : %.6f MHz (%+.1f Hz from the centre), %.2f %s\n",
               (set->freq_hz + d->freq_offset_hz) / 1e6, d->freq_offset_hz, d->power_db + off, unit);
        printf("  EVM        : %.2f %% RMS, %.2f %% peak, over %zu symbols\n", 100.0 * d->evm_rms, 100.0 * d->evm_peak, d->symbols);
        printf("  MER        : %.1f dB\n", d->mer_db);
        printf("  Errors     : magnitude %.2f %% RMS, phase %.2f degrees RMS\n", 100.0 * d->mag_err_rms, d->phase_err_deg);
        printf("  I/Q        : origin offset %.1f dB, gain imbalance %+.3f dB, quadrature error %+.3f degrees\n",
               d->iq_offset_db, d->gain_imbalance_db, d->quadrature_deg);
    }
}

static void demod_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const struct m2sdr_demod_result *d = &RES(res)->demod;

    printf("  \"modulation\": {\n");
    printf("    \"signal\": %s,\n", RES(res)->demod_valid && d->signal ? "true" : "false");
    printf("    \"locked\": %s%s\n", RES(res)->demod_valid && d->locked ? "true" : "false",
           RES(res)->demod_valid && d->signal ? "," : "");
    if (RES(res)->demod_valid && d->signal) {
        printf("    \"type\": \"%s\",\n", m2sdr_modulation_name(d->modulation));
        print_json_number("symbol_rate", d->symbol_rate, false);
        print_json_number("rolloff", d->rolloff, false);
        print_json_number("carrier_hz", set->freq_hz + d->freq_offset_hz, false);
        print_json_number("carrier_offset_hz", d->freq_offset_hz, false);
        print_json_number("power_db", d->power_db + off, false);
        printf("    \"symbols\": %zu,\n", d->symbols);
        print_json_number("evm_rms_percent", 100.0 * d->evm_rms, false);
        print_json_number("evm_peak_percent", 100.0 * d->evm_peak, false);
        print_json_number("mer_db", d->mer_db, false);
        print_json_number("magnitude_error_percent", 100.0 * d->mag_err_rms, false);
        print_json_number("phase_error_deg", d->phase_err_deg, false);
        print_json_number("origin_offset_db", d->iq_offset_db, false);
        print_json_number("gain_imbalance_db", d->gain_imbalance_db, false);
        print_json_number("quadrature_deg", d->quadrature_deg, true);
    }
    printf("  }");
}

/* Command line */
/*--------------*/

enum { DEMOD_OPT_ON, DEMOD_OPT_MODULATION, DEMOD_OPT_SYMBOL_RATE, DEMOD_OPT_ROLLOFF, DEMOD_OPT_PULSE, DEMOD_OPT_SAMPLES };

static const struct option demod_options[] = {
    { "demod",           no_argument,       NULL, DEMOD_OPT_ON },
    { "modulation",      required_argument, NULL, DEMOD_OPT_MODULATION },
    { "symbol-rate",     required_argument, NULL, DEMOD_OPT_SYMBOL_RATE },
    { "rolloff",         required_argument, NULL, DEMOD_OPT_ROLLOFF },
    { "pulse",           required_argument, NULL, DEMOD_OPT_PULSE },
    { "demod-samples",   required_argument, NULL, DEMOD_OPT_SAMPLES },
    { NULL, 0, NULL, 0 }
};

static void demod_defaults(struct an_settings *set)
{
    SET(set)->demod_log2 = AN_DEMOD_DEFAULT_LOG2;
}

static int demod_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_settings *set = cli->set;
    double v;
    int n;

    switch (id) {
    case DEMOD_OPT_ON: SET(set)->demod_on = true; break;
    case DEMOD_OPT_MODULATION:
        for (n = M2SDR_MOD_AUTO; n < M2SDR_MOD_COUNT; n++) {
            if (strcasecmp(arg, m2sdr_modulation_name((enum m2sdr_modulation)n)) == 0)
                break;
        }
        if (n == M2SDR_MOD_COUNT) {
            m2sdr_cli_invalid_choice("modulation", arg, "auto, bpsk, qpsk, 8psk, 16qam, 32qam, 64qam, 128qam or 256qam");
            return -1;
        }
        SET(set)->demod.modulation = (enum m2sdr_modulation)n;
        break;
    case DEMOD_OPT_SYMBOL_RATE:
        if (m2sdr_cli_parse_double(arg, &SET(set)->demod.symbol_rate) != 0 || SET(set)->demod.symbol_rate < 0) {
            m2sdr_cli_error("invalid symbol rate '%s'", arg);
            return -1;
        }
        break;
    case DEMOD_OPT_ROLLOFF:
        if (m2sdr_cli_parse_double(arg, &SET(set)->demod.rolloff) != 0 ||
            (SET(set)->demod.rolloff != 0 && (SET(set)->demod.rolloff < 0.05 || SET(set)->demod.rolloff > 1.0))) {
            m2sdr_cli_error("invalid roll-off '%s' (expected 0.05 to 1, or 0 to find it)", arg);
            return -1;
        }
        break;
    case DEMOD_OPT_PULSE:
        if (strcmp(arg, "rrc") == 0) {
            SET(set)->demod.pulse = M2SDR_PULSE_RRC;
        } else if (strcmp(arg, "rc") == 0) {
            SET(set)->demod.pulse = M2SDR_PULSE_RC;
        } else if (strcmp(arg, "rect") == 0) {
            SET(set)->demod.pulse = M2SDR_PULSE_RECT;
        } else {
            m2sdr_cli_invalid_choice("pulse", arg, "rrc, rc or rect");
            return -1;
        }
        break;
    case DEMOD_OPT_SAMPLES:
        if (m2sdr_cli_parse_double(arg, &v) != 0 || v < (double)(1 << AN_DEMOD_MIN_LOG2) || v > (double)(1 << AN_DEMOD_MAX_LOG2)) {
            m2sdr_cli_error("invalid demodulation samples '%s' (expected %d to %d)", arg,
                            1 << AN_DEMOD_MIN_LOG2, 1 << AN_DEMOD_MAX_LOG2);
            return -1;
        }
        for (SET(set)->demod_log2 = AN_DEMOD_MIN_LOG2; (double)((size_t)2 << SET(set)->demod_log2) <= v; SET(set)->demod_log2++)
            ;
        break;
    }
    return 0;
}

static void demod_help(void)
{
    printf("Modulation options:\n"
           "      --demod            With --report: demodulate the strongest signal, a single carrier\n"
           "                         with PSK or QAM: constellation, EVM, MER, I/Q errors.\n"
           "      --modulation NAME  auto, bpsk, qpsk, 8psk, 16qam, 32qam, 64qam, 128qam or 256qam\n"
           "                         (default: auto).\n"
           "      --symbol-rate HZ   Symbols per second (default: found from the signal).\n"
           "      --rolloff A        Roll-off of the pulse (default: found from the signal).\n"
           "      --pulse NAME       Transmit pulse: rrc, rc or rect (default: rrc).\n"
           "      --demod-samples N  Samples of its capture (default: %d).\n"
           "\n", 1 << AN_DEMOD_DEFAULT_LOG2);
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

static void draw_modulation(struct an_ui *ui, bool live, bool *changed)
{
    static const char *const mods[] = { "Auto", "BPSK", "QPSK", "8PSK", "16QAM", "32QAM", "64QAM", "128QAM", "256QAM" };
    static const char *const pulses[] = { "Root raised cosine", "Raised cosine", "Rectangular" };
    static const char *const sizes[] = { "16384", "32768", "65536", "131072", "262144", "524288", "1048576", "2097152", "4194304" };
    static float sx[M2SDR_DEMOD_MAX_SYMBOLS], sy[M2SDR_DEMOD_MAX_SYMBOLS], evm[M2SDR_DEMOD_MAX_SYMBOLS];
    static float complex points[256];
    static float px[256], py[256];
    const struct m2sdr_demod_result *d = &RES(&ui->res)->demod;
    struct m2sdr_plot plot;
    ImVec2 avail, sq;
    float side, lim = 1.0f, far = 0.0f, eye_h;
    int mod = (int)SET(&ui->set)->demod.modulation, pulse = (int)SET(&ui->set)->demod.pulse;
    int idx = SET(&ui->set)->demod_log2 - AN_DEMOD_MIN_LOG2, npoints, i, m, rail;
    float evm_top = 1.0f;
    size_t k;

    (void)live;
    igSetNextItemWidth(90.0f);
    if (igCombo_Str_arr("Type", &mod, mods, M2SDR_MOD_COUNT, -1)) {
        SET(&ui->set)->demod.modulation = (enum m2sdr_modulation)mod;
        *changed = true;
    }
    same_line_or_wrap(12.0f, 215.0f);
    igSetNextItemWidth(170.0f);
    if (igCombo_Str_arr("Pulse", &pulse, pulses, 3, -1)) {
        SET(&ui->set)->demod.pulse = (enum m2sdr_pulse)pulse;
        *changed = true;
    }
    same_line_or_wrap(12.0f, 295.0f);
    if (input_scaled("Symbols/s (0: auto)", &SET(&ui->set)->demod.symbol_rate, 1.0, "%.1f"))
        *changed = true;
    same_line_or_wrap(12.0f, 285.0f);
    {
        double a = SET(&ui->set)->demod.rolloff;
        if (input_scaled("Roll-off (0: auto)", &a, 1.0, "%.2f") && (a == 0 || (a >= 0.05 && a <= 1.0))) {
            SET(&ui->set)->demod.rolloff = a;
            *changed = true;
        }
    }
    igSetNextItemWidth(110.0f);
    if (igCombo_Str_arr("Samples", &idx, sizes, AN_DEMOD_MAX_LOG2 - AN_DEMOD_MIN_LOG2 + 1, -1)) {
        SET(&ui->set)->demod_log2 = idx + AN_DEMOD_MIN_LOG2;
        *changed = true;
    }

    if (!SET(&ui->res.set)->demod_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!RES(&ui->res)->demod_valid || !d->signal) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted("No signal with a symbol rate was found: tune on it, with a sample rate of a few times its "
                          "bandwidth, or give the symbol rate.", NULL);
        igPopTextWrapPos();
        return;
    }
    same_line_or_wrap(16.0f, 150.0f);
    if (d->locked)
        igTextColored((ImVec4){ 0.4f, 1.0f, 0.4f, 1.0f }, "%s locked", m2sdr_modulation_name(d->modulation));
    else
        igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "%s NOT LOCKED: the figures are not valid", m2sdr_modulation_name(d->modulation));

    npoints = m2sdr_constellation(d->modulation, points);
    for (i = 0; i < npoints; i++) {
        px[i] = crealf(points[i]);
        py[i] = cimagf(points[i]);
        if (fabsf(px[i]) > lim)
            lim = fabsf(px[i]);
        if (fabsf(py[i]) > lim)
            lim = fabsf(py[i]);
    }
    lim *= 1.25f;
    for (k = 0; k < d->symbols; k++) {
        sx[k]  = crealf(d->sym[k]);
        sy[k]  = cimagf(d->sym[k]);
        evm[k] = 100.0f * cabsf(d->sym[k] - d->ref[k]);
        if (evm[k] > evm_top)
            evm_top = evm[k];
        if (fabsf(sx[k]) > far)
            far = fabsf(sx[k]);
        if (fabsf(sy[k]) > far)
            far = fabsf(sy[k]);
    }
    /* Symbols that are not on the points (another modulation, no lock) are shown too, up to
     * twice the size of the constellation. */
    if (1.05f * far > lim)
        lim = 1.05f * far < 2.0f * lim ? 1.05f * far : 2.0f * lim;

    igGetContentRegionAvail(&avail);
    side = avail.y - 170.0f < 0.5f * avail.x ? avail.y - 170.0f : 0.5f * avail.x;
    if (side < 120.0f)
        side = 120.0f;
    eye_h = 0.5f * side;
    sq    = m2sdr_plot_square(side);

    /* Constellation: the symbols, and the points they are compared with. */
    if (m2sdr_plot_begin(&plot, "##const", sq, -lim, lim, M2SDR_PLOT_PLAIN, -lim, lim,
                         "Q against I; crosses: the points of the constellation")) {
        m2sdr_plot_points(&plot, sx, sy, d->symbols, 1.0f, 0xc040d0ffu);
        for (i = 0; i < npoints; i++) {
            const float x = m2sdr_plot_x(&plot, px[i]), y = m2sdr_plot_y(&plot, py[i]);
            ImDrawList_AddLine(plot.dl, (ImVec2){ x - 5.0f, y }, (ImVec2){ x + 5.0f, y }, 0xffffffffu, 1.0f);
            ImDrawList_AddLine(plot.dl, (ImVec2){ x, y - 5.0f }, (ImVec2){ x, y + 5.0f }, 0xffffffffu, 1.0f);
        }
        m2sdr_plot_end(&plot);
    }
    igSameLine(0.0f, 8.0f);

    /* Eye diagrams of I and of Q: two symbols, the decisions at the two vertical lines. */
    igBeginGroup();
    for (rail = 0; rail < 2; rail++) {
        if (m2sdr_plot_begin(&plot, rail ? "##eyeq" : "##eyei", (ImVec2){ avail.x - sq.x - 8.0f, eye_h },
                             -0.5, 1.5, M2SDR_PLOT_PLAIN, -lim, lim,
                             rail ? "Eye of Q, in symbols; lines: the decisions" :
                                    "Eye of I, in symbols; lines: the decisions")) {
            static float ex[M2SDR_DEMOD_EYE_POINTS], ey[M2SDR_DEMOD_EYE_POINTS];

            m2sdr_plot_vline(&plot, 0.0, 0x60ffffffu);
            m2sdr_plot_vline(&plot, 1.0, 0x60ffffffu);
            for (i = 0; i < d->eye_traces; i++) {
                for (m = 0; m < M2SDR_DEMOD_EYE_POINTS; m++) {
                    ex[m] = (float)(m - 4) / 8.0f;
                    ey[m] = rail ? cimagf(d->eye[i][m]) : crealf(d->eye[i][m]);
                }
                m2sdr_plot_xy(&plot, ex, ey, M2SDR_DEMOD_EYE_POINTS, rail ? 0x6040a0ffu : 0x60ffa040u);
            }
            m2sdr_plot_end(&plot);
        }
    }
    igEndGroup();

    /* Error of each symbol. */
    if (m2sdr_plot_begin(&plot, "##evm", (ImVec2){ 0.0f, 0.0f }, 0.0, (double)(d->symbols - 1), M2SDR_PLOT_PLAIN,
                         0.0, 1.1 * evm_top, "Error vector of each symbol, % of the RMS of the constellation, against its number; line: RMS")) {
        m2sdr_plot_trace(&plot, evm, d->symbols, 0.0, (double)(d->symbols - 1), 0xff60ff60u);
        m2sdr_plot_hline(&plot, 100.0 * d->evm_rms, 0xc0ffffffu);
        m2sdr_plot_end(&plot);
    }
}

static void draw_modulation_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_demod_result *d = &RES(res)->demod;

    igSeparatorText("Modulation");
    if (!RES(res)->demod_valid || !d->signal) {
        igTextUnformatted("No signal.", NULL);
        return;
    }
    if (igBeginTable("##demod", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Type", "%s%s", m2sdr_modulation_name(d->modulation), d->locked ? "" : " (not locked)");
        label_value("Symbol rate", "%.3f /s", d->symbol_rate);
        label_value("Roll-off", "%.2f", d->rolloff);
        label_value("Carrier", "%.6f MHz", (res->set.freq_hz + d->freq_offset_hz) / 1e6);
        label_value("Carrier offset", "%+.1f Hz", d->freq_offset_hz);
        label_value("Power", "%.2f dBFS", d->power_db);
        label_value("EVM", "%.2f %% RMS", 100.0 * d->evm_rms);
        label_value("EVM peak", "%.2f %%", 100.0 * d->evm_peak);
        label_value("MER", "%.1f dB", d->mer_db);
        label_value("Magnitude error", "%.2f %% RMS", 100.0 * d->mag_err_rms);
        label_value("Phase error", "%.2f deg RMS", d->phase_err_deg);
        label_value("Origin offset", "%.1f dB", d->iq_offset_db);
        label_value("I/Q gain", "%+.3f dB", d->gain_imbalance_db);
        label_value("Quadrature", "%+.3f deg", d->quadrature_deg);
        label_value("Symbols", "%zu", d->symbols);
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_demod = {
    .key           = "modulation",
    .title         = "Modulation",
    .category      = AN_CAT_SIGNAL,
    .settings_size = sizeof(struct demod_set),
    .on            = offsetof(struct demod_set, demod_on),
    .result_size   = sizeof(struct demod_res),
    .valid         = offsetof(struct demod_res, demod_valid),
    .found         = offsetof(struct demod_res, demod.locked),
    .report_rank   = 40,
    .samples       = demod_samples,
    .analyse       = demod_analyse,
    .report_text   = demod_report_text,
    .report_json   = demod_report_json,
    .defaults      = demod_defaults,
    .options       = demod_options,
    .option        = demod_option,
    .help          = demod_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw          = draw_modulation,
    .results       = draw_modulation_results,
#endif
};
