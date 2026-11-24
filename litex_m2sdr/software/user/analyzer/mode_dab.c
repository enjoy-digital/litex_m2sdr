/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: reception of a DAB/DAB+ ensemble (MER, SNR, channel, echoes), in a
 * report, a view and --watch.
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

#include "m2sdr_cli.h"

#include "analyzer.h"

/* Settings and result */
/*-----------------------*/

extern const struct an_mode an_mode_dab;

/* Its settings: its block of struct an_settings, read with SET(). */
struct dab_set {
    bool   dab_on;                 /* DAB ensemble. */
};

#define SET(set) ((struct dab_set *)an_mode_settings(&an_mode_dab, (set)))

/* The result of the mode: its block of struct an_result, read with RES(). */
struct dab_res {
    bool   dab_valid;              /* Measured: the sample rate was 2.048 MSPS; dab.found says the rest. */
    struct m2sdr_dab_result dab;
};

#define RES(res) ((struct dab_res *)an_mode_result(&an_mode_dab, (res)))

/* Analysis */
/*----------*/

static size_t dab_samples(const struct an_settings *set)
{
    return (size_t)(AN_DAB_SECONDS * set->sample_rate);
}

static void dab_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = (size_t)(AN_DAB_SECONDS * set->sample_rate);

    RES(res)->dab_valid = m2sdr_dab_analyse(x, count < n ? count : n, set->sample_rate, &RES(res)->dab) == 0;
}

/* Reports */
/*---------*/

static void dab_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const char *unit = rep->unit;
    const struct m2sdr_dab_result *t = &RES(res)->dab;

    printf("\nDAB:\n");
    if (!RES(res)->dab_valid) {
        printf("  The sample rate must be 2.048 MSPS, with at least 110 ms of samples.\n");
    } else if (!t->found) {
        printf("  No ensemble found (null symbol %.1f dB, guard interval correlation %.3f).\n", t->null_depth_db,
               t->sync_quality);
    } else {
        printf("  Sync       : null symbol %.1f dB, guard correlation %.2f, %.1f %s\n", t->null_depth_db, t->sync_quality,
               t->power_dbfs + off, unit);
        if (!t->prs) {
            printf("  Reference  : phase reference symbol not found (%.2f): too much noise for the figures below.\n",
                   t->prs_quality);
        } else {
            printf("  Ensemble   : %.6f MHz (%+.1f Hz from the centre), %d frame%s\n", (set->freq_hz + t->freq_error_hz) / 1e6,
                   t->freq_error_hz, t->frames, t->frames == 1 ? "" : "s");
            printf("  MER        : %.1f dB over %d symbols, SNR on the carriers %.1f dB\n", t->mer_db, t->symbols, t->null_snr_db);
            if (t->echo_level_db > -99.0)
                printf("  Echo       : strongest at %+.2f us, %.1f dB (guard interval %.0f us)\n", t->echo_delay_us,
                       t->echo_level_db, t->guard_us);
        }
    }
}

static void dab_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const struct m2sdr_dab_result *t = &RES(res)->dab;
    const bool ok = RES(res)->dab_valid && t->found;

    printf("  \"dab\": {\n");
    print_json_number("sync_quality", RES(res)->dab_valid ? t->sync_quality : 0.0, false);
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        print_json_number("power_db", t->power_dbfs + off, false);
        print_json_number("null_depth_db", t->null_depth_db, false);
        printf("    \"phase_reference\": %s%s\n", t->prs ? "true" : "false", t->prs ? "," : "");
        if (t->prs) {
            print_json_number("carrier_hz", set->freq_hz + t->freq_error_hz, false);
            print_json_number("carrier_offset_hz", t->freq_error_hz, false);
            printf("    \"frames\": %d,\n", t->frames);
            print_json_number("snr_db", t->null_snr_db, false);
            print_json_number("echo_delay_us", t->echo_delay_us, false);
            print_json_number("echo_level_db", t->echo_level_db, false);
            print_json_number("mer_db", t->mer_db, true);
        }
    }
    printf("  }");
}

/* Watch */
/*-------*/

/* --watch: one line per capture. */
static unsigned dab_watch_lines;

static int dab_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_dab_result *d = &RES(res)->dab;
    const bool dok = RES(res)->dab_valid && d->found && d->prs;

    if (!RES(res)->dab_valid) {
        fprintf(stderr, "The sample rate is not the one of DAB (2.048 MSPS).\n");
        return -1;
    }
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, dok ? "true" : "false");
        if (dok) {
            printf(", \"mer_db\": %.2f, \"snr_db\": %.2f, \"power_db\": %.2f, \"null_depth_db\": %.1f, "
                   "\"carrier_offset_hz\": %.1f", d->mer_db, d->null_snr_db, d->power_dbfs + set->ref_offset_db,
                   d->null_depth_db, d->freq_error_hz);
            if (d->echo_level_db > -99.0)
                printf(", \"echo_delay_us\": %.2f, \"echo_level_db\": %.1f", d->echo_delay_us, d->echo_level_db);
        }
        printf("}\n");
    } else {
        if (dab_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %7s %7s %8s %6s %10s %8s %6s\n%8s %7s %7s %8s %6s %10s %8s %6s\n",
                   "time", "MER", "SNR", "level", "null", "offset", "echo", "echo",
                   "s", "dB", "dB", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "dB", "Hz", "us", "dB");
        if (dok) {
            printf("%8.2f %7.1f %7.1f %8.1f %6.1f %+10.1f", now, d->mer_db, d->null_snr_db, d->power_dbfs + set->ref_offset_db,
                   d->null_depth_db, d->freq_error_hz);
            if (d->echo_level_db > -99.0)
                printf(" %+8.2f %6.1f", d->echo_delay_us, d->echo_level_db);
            printf("\n");
        } else {
            printf("%8.2f  no ensemble (null symbol %.1f dB, guard correlation %.3f)\n", now, d->null_depth_db,
                   d->sync_quality);
        }
    }
    return 0;
}

/* Command line */
/*--------------*/

static const struct option dab_options[] = {
    { "dab", no_argument, NULL, 0 },
    { NULL, 0, NULL, 0 }
};

static int dab_option(int id, const char *arg, struct an_cli *cli)
{
    (void)id;
    (void)arg;
    SET(cli->set)->dab_on = true;
    return 0;
}

static void dab_help(void)
{
    printf("DAB options:\n"
           "      --dab              With --report: reception of a DAB/DAB+ ensemble (mode I) tuned near\n"
           "                         its centre: MER, SNR, carrier offset, channel and echoes. The sample\n"
           "                         rate is 2.048 MSPS (the default with --dab). --watch works too.\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

/* Band III blocks (EN 50248): name and centre, kHz. */
static const struct {
    const char *name;
    int khz;
} dab_blocks[] = {
    { "5A", 174928 }, { "5B", 176640 }, { "5C", 178352 }, { "5D", 180064 }, { "6A", 181936 }, { "6B", 183648 },
    { "6C", 185360 }, { "6D", 187072 }, { "7A", 188928 }, { "7B", 190640 }, { "7C", 192352 }, { "7D", 194064 },
    { "8A", 195936 }, { "8B", 197648 }, { "8C", 199360 }, { "8D", 201072 }, { "9A", 202928 }, { "9B", 204640 },
    { "9C", 206352 }, { "9D", 208064 }, { "10A", 209936 }, { "10N", 210096 }, { "10B", 211648 }, { "10C", 213360 },
    { "10D", 215072 }, { "11A", 216928 }, { "11N", 217088 }, { "11B", 218640 }, { "11C", 220352 }, { "11D", 222064 },
    { "12A", 223936 }, { "12N", 224096 }, { "12B", 225648 }, { "12C", 227360 }, { "12D", 229072 }, { "13A", 230784 },
    { "13B", 232496 }, { "13C", 234208 }, { "13D", 235776 }, { "13E", 237488 }, { "13F", 239200 },
};

static void dab_keep(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_dab_result *t = &RES(res)->dab;

    if (!SET(&res->set)->dab_on || !RES(res)->dab_valid)
        return;
    trend_add(&ui->dab, &res->set, t->found && t->prs, t->mer_db, t->mer_bin_db, t->channel_bin_db, t->bins, t->echo_db,
              M2SDR_DAB_ECHO_BINS);
}

static void draw_dab(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_dab_result *t = &RES(res)->dab;
    const bool ok = t->found && t->prs;
    ImVec2 avail, sq;
    float side;
    char text[160];
    int i;

    if (live) {
        const int khz = (int)lround(ui->set.freq_hz / 1e3);
        int sel = -1;

        for (i = 0; i < (int)(sizeof(dab_blocks) / sizeof(dab_blocks[0])); i++)
            if (abs(dab_blocks[i].khz - khz) < 5)
                sel = i;
        igTextUnformatted("Block:", NULL);
        igSameLine(0.0f, 8.0f);
        igSetNextItemWidth(160.0f);
        if (sel >= 0)
            snprintf(text, sizeof(text), "%s  %.3f MHz", dab_blocks[sel].name, dab_blocks[sel].khz / 1e3);
        else
            snprintf(text, sizeof(text), "%.3f MHz", ui->set.freq_hz / 1e6);
        if (igBeginCombo("##dabblock", text, 0)) {
            for (i = 0; i < (int)(sizeof(dab_blocks) / sizeof(dab_blocks[0])); i++) {
                char item[32];

                snprintf(item, sizeof(item), "%s  %.3f MHz", dab_blocks[i].name, dab_blocks[i].khz / 1e3);
                if (igSelectable_Bool(item, i == sel, 0, (ImVec2){ 0.0f, 0.0f }))
                    view_retune(ui, 1e3 * dab_blocks[i].khz, changed);
            }
            igEndCombo();
        }
        if (fabs(ui->set.sample_rate - M2SDR_DAB_SAMPLE_RATE) > 1.0) {
            igSameLine(0.0f, 16.0f);
            if (igSmallButton("2.048 MSPS"))
                view_rate(ui, M2SDR_DAB_SAMPLE_RATE, changed);
        }
        if (ok && RES(res)->dab_valid && fabs(t->freq_error_hz) > 500.0) {
            igSameLine(0.0f, 16.0f);
            snprintf(text, sizeof(text), "Centre on the ensemble (%+.1f kHz)", t->freq_error_hz / 1e3);
            if (igSmallButton(text))
                view_retune(ui, ui->set.freq_hz + 1e3 * round(t->freq_error_hz / 1e3), changed);
        }
    }
    if (!SET(&res->set)->dab_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!RES(res)->dab_valid) {
        igTextUnformatted("The sample rate must be 2.048 MSPS.", NULL);
        return;
    }
    if (!t->found) {
        igPushTextWrapPos(0.0f);
        igText("No DAB ensemble (null symbol %.1f dB, guard interval correlation %.3f): tune on a block that carries one.",
               t->null_depth_db, t->sync_quality);
        igPopTextWrapPos();
    }
    if (ok)
        snprintf(text, sizeof(text), "SNR on the carriers %.1f dB: the MER is below it by what the transmitter and the receiver add.",
                 t->null_snr_db);
    else
        snprintf(text, sizeof(text), " ");

    igGetContentRegionAvail(&avail);
    side = 0.55f * avail.y < 0.42f * avail.x ? 0.55f * avail.y : 0.42f * avail.x;
    sq   = m2sdr_plot_square(side);
    igBeginGroup();
    draw_trend_mer("##dabmer", &ui->dab, ok, t->mer_db, NAN, text, avail.x - sq.x - 8.0f, sq.y);
    igEndGroup();
    igSameLine(0.0f, 8.0f);
    draw_trend_points("##dabconst", sq, t->points, ok ? t->npoints : 0, M2SDR_MOD_QPSK);
    if (ok)
        draw_trend_traces("##dab", &ui->dab, t->bins, 767.5e3, t->echo_start_us, t->echo_step_us, M2SDR_DAB_ECHO_BINS,
                          t->guard_us, NAN);
}

static void draw_dab_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_dab_result *t = &RES(res)->dab;

    igSeparatorText("DAB");
    if (!RES(res)->dab_valid || !t->found) {
        igTextUnformatted("No ensemble.", NULL);
        return;
    }
    if (igBeginTable("##dab", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Mode", "I, guard %.0f us", t->guard_us);
        label_value("Null symbol", "%.1f dB", t->null_depth_db);
        label_value("Guard correlation", "%.2f", t->sync_quality);
        label_value("Power", "%.1f dBFS", t->power_dbfs);
        if (t->prs) {
            label_value("Ensemble", "%.6f MHz", (res->set.freq_hz + t->freq_error_hz) / 1e6);
            label_value("Offset", "%+.1f Hz", t->freq_error_hz);
            label_value("MER", "%.1f dB (last %.1f)", trend_mer_shown(&ui->dab, t->mer_db), t->mer_db);
            label_value("SNR on the carriers", "%.1f dB", t->null_snr_db);
            if (t->echo_level_db > -99.0)
                label_value("Strongest echo", "%+.2f us, %.1f dB", t->echo_delay_us, t->echo_level_db);
            label_value("Frames, symbols", "%d, %d", t->frames, t->symbols);
        } else {
            label_value("Phase reference", "not found (%.2f)", t->prs_quality);
        }
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_dab = {
    .key           = "dab",
    .title         = "DAB",
    .category      = AN_CAT_BROADCAST,
    .settings_size = sizeof(struct dab_set),
    .on            = offsetof(struct dab_set, dab_on),
    .result_size   = sizeof(struct dab_res),
    .valid         = offsetof(struct dab_res, dab_valid),
    .found         = offsetof(struct dab_res, dab.found),
    .report_rank   = 30,
    .default_rate  = M2SDR_DAB_SAMPLE_RATE,
    .psd_short     = true,
    .samples       = dab_samples,
    .analyse       = dab_analyse,
    .report_text   = dab_report_text,
    .report_json   = dab_report_json,
    .watch_line    = dab_watch_line,
    .options       = dab_options,
    .option        = dab_option,
    .help          = dab_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw          = draw_dab,
    .results       = draw_dab_results,
    .keep          = dab_keep,
#endif
};
