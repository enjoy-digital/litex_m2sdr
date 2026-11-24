/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: reception of a DVB-T multiplex (mode, TPS, MER, channel, echoes),
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
#include <string.h>

#include "m2sdr_cli.h"

#include "analyzer.h"

/* Analysis */
/*----------*/

static size_t dvbt_samples(const struct an_settings *set)
{
    return (size_t)((set->dvbt_fast ? AN_DVBT_FAST_SECONDS : AN_DVBT_SECONDS) * set->sample_rate);
}

static void dvbt_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = (size_t)(AN_DVBT_SECONDS * set->sample_rate);

    res->dvbt_valid = m2sdr_dvbt_analyse(x, count < n ? count : n, set->sample_rate,
                                         set->dvbt_hint.fft_len ? &set->dvbt_hint : NULL, &res->dvbt) == 0;
}

/* Reports */
/*---------*/

/* C/N for quasi error free reception in a Gaussian channel (EN 300 744, annex A), by
 * constellation and code rate: what the MER must at least reach. NAN when not known. */
static double dvbt_needed_db(const struct m2sdr_dvbt_result *t)
{
    static const double cn[3][5] = {
        {  3.1,  4.9,  5.9,  6.9,  7.7 },
        {  8.8, 11.1, 12.5, 13.5, 13.9 },
        { 14.4, 16.5, 18.0, 19.3, 20.1 },
    };
    static const char *const rates[5] = { "1/2", "2/3", "3/4", "5/6", "7/8" };
    int m, r;

    if (!t->tps || t->tps_hierarchy)
        return NAN;
    m = t->tps_modulation == M2SDR_MOD_QPSK ? 0 : t->tps_modulation == M2SDR_MOD_16QAM ? 1 : 2;
    for (r = 0; r < 5; r++)
        if (strcmp(t->code_rate_hp, rates[r]) == 0)
            return cn[m][r];
    return NAN;
}

static void dvbt_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const char *unit = rep->unit;
    const struct m2sdr_dvbt_result *t = &res->dvbt;

    printf("\nDVB-T:\n");
    if (!res->dvbt_valid) {
        printf("  The sample rate must be the one of the channel: 9.142857 MSPS (8 MHz), 8 MSPS (7 MHz)\n"
               "  or 6.857143 MSPS (6 MHz).\n");
    } else if (!t->found) {
        printf("  No multiplex found (guard interval correlation %.3f).\n", t->sync_quality);
    } else {
        printf("  Mode       : %dk, guard interval 1/%d (%.0f us), %.1f %s\n", t->fft_len / 1024, t->guard, t->guard_us,
               t->power_dbfs + off, unit);
        printf("  Sync       : guard correlation %.2f (about %.1f dB S/N)\n", t->sync_quality, t->sync_snr_db);
        if (!t->pilots) {
            printf("  Pilots     : not found (coherence %.2f): too much noise for the figures below.\n", t->pilot_quality);
        } else {
            printf("  Multiplex  : %.6f MHz (%+.1f Hz from the centre)\n", (set->freq_hz + t->freq_error_hz) / 1e6, t->freq_error_hz);
            if (t->tps) {
                printf("  TPS        : %s, hierarchy %s, code rate %s", m2sdr_modulation_name(t->tps_modulation),
                       t->tps_hierarchy ? "yes" : "none", t->code_rate_hp);
                if (t->tps_hierarchy)
                    printf(" (LP %s, alpha %d)", t->code_rate_lp, t->tps_hierarchy);
                if (t->cell_id >= 0)
                    printf(", cell %d", t->cell_id);
                printf("\n");
            } else {
                printf("  TPS        : not read\n");
            }
            printf("  MER        : %.1f dB against %s, over %d symbols", t->mer_db, m2sdr_modulation_name(t->modulation), t->symbols);
            if (isfinite(dvbt_needed_db(t)))
                printf(" (%.1f dB needed, margin %+.1f dB)", dvbt_needed_db(t), t->mer_db - dvbt_needed_db(t));
            printf("\n");
            if (t->echo_level_db > -99.0)
                printf("  Echo       : strongest at %+.2f us, %.1f dB (guard interval %.0f us)\n", t->echo_delay_us,
                       t->echo_level_db, t->guard_us);
        }
    }
}

static void dvbt_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const struct m2sdr_dvbt_result *t = &res->dvbt;
    const bool ok = res->dvbt_valid && t->found;

    printf("  \"dvbt\": {\n");
    print_json_number("sync_quality", res->dvbt_valid ? t->sync_quality : 0.0, false);
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        printf("    \"mode\": \"%dk\",\n", t->fft_len / 1024);
        printf("    \"guard_interval\": \"1/%d\",\n", t->guard);
        print_json_number("power_db", t->power_dbfs + off, false);
        printf("    \"pilots\": %s%s\n", t->pilots ? "true" : "false", t->pilots ? "," : "");
        if (t->pilots) {
            print_json_number("carrier_hz", set->freq_hz + t->freq_error_hz, false);
            print_json_number("carrier_offset_hz", t->freq_error_hz, false);
            printf("    \"tps\": %s,\n", t->tps ? "true" : "false");
            if (t->tps) {
                printf("    \"constellation\": \"%s\",\n", m2sdr_modulation_name(t->tps_modulation));
                printf("    \"hierarchy\": %d,\n", t->tps_hierarchy);
                printf("    \"code_rate_hp\": \"%s\",\n", t->code_rate_hp);
                printf("    \"code_rate_lp\": \"%s\",\n", t->code_rate_lp);
                if (t->cell_id >= 0)
                    printf("    \"cell_id\": %d,\n", t->cell_id);
            }
            printf("    \"mer_against\": \"%s\",\n", m2sdr_modulation_name(t->modulation));
            print_json_number("mer_needed_db", dvbt_needed_db(t), false);
            print_json_number("echo_delay_us", t->echo_delay_us, false);
            print_json_number("echo_level_db", t->echo_level_db, false);
            print_json_number("mer_db", t->mer_db, true);
        }
    }
    printf("  }");
}

/* Watch */
/*-------*/

/* --watch: the multiplex of the last TPS read and the captures since, for short captures once
 * it is read, as in the view. */
static struct {
    struct m2sdr_dvbt_result tps;
    struct m2sdr_dvbt_config known;
    unsigned since, lines;
    bool   ok;
} dvbt_watch;

static void dvbt_watch_prepare(struct an_settings *set)
{
    set->dvbt_fast = dvbt_watch.ok && ++dvbt_watch.since % AN_DVBT_FULL_EVERY != 0;
    memset(&set->dvbt_hint, 0, sizeof(set->dvbt_hint));
    if (dvbt_watch.ok)
        set->dvbt_hint = dvbt_watch.known;
}

static int dvbt_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_dvbt_result *t = &res->dvbt;
    const struct m2sdr_dvbt_result *tps = &dvbt_watch.tps;
    double needed;

    if (!res->dvbt_valid) {
        fprintf(stderr, "The sample rate is not one of DVB-T.\n");
        return -1;
    }
    if (t->tps) {
        /* The multiplex, each time its TPS is read: the reference of the lines. */
        if (!json && (!tps->tps || tps->tps_modulation != t->tps_modulation || strcmp(tps->code_rate_hp, t->code_rate_hp) ||
                      tps->fft_len != t->fft_len || tps->guard != t->guard || dvbt_watch.since >= AN_DVBT_FULL_EVERY - 1)) {
            printf("Multiplex %.3f MHz: %dk, guard 1/%d, %s, code rate %s", (set->freq_hz + t->freq_error_hz) / 1e6,
                   t->fft_len / 1024, t->guard, m2sdr_modulation_name(t->tps_modulation), t->code_rate_hp);
            if (t->cell_id >= 0)
                printf(", cell %d", t->cell_id);
            if (isfinite(dvbt_needed_db(t)))
                printf(": MER needed %.1f dB", dvbt_needed_db(t));
            printf("\n");
            dvbt_watch.lines = 0;
        }
        dvbt_watch.tps = *t;
        dvbt_watch.known.fft_len    = t->fft_len;
        dvbt_watch.known.guard      = t->guard;
        dvbt_watch.known.modulation = t->tps_modulation;
        dvbt_watch.ok    = true;
        dvbt_watch.since = 0;
    } else if (!t->found || !t->pilots) {
        dvbt_watch.ok = false;
    }
    if (dvbt_watch.ok && t->found && t->pilots) {
        dvbt_watch.known.offset_known   = true;
        dvbt_watch.known.carrier_offset = t->carrier_offset;
    }
    needed = dvbt_needed_db(tps);
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, t->found && t->pilots ? "true" : "false");
        if (t->found && t->pilots) {
            printf(", \"mer_db\": %.2f", t->mer_db);
            if (isfinite(needed))
                printf(", \"margin_db\": %.2f", t->mer_db - needed);
            printf(", \"power_db\": %.2f, \"sync_quality\": %.3f, \"carrier_offset_hz\": %.1f",
                   t->power_dbfs + set->ref_offset_db, t->sync_quality, t->freq_error_hz);
            if (t->echo_level_db > -99.0)
                printf(", \"echo_delay_us\": %.2f, \"echo_level_db\": %.1f", t->echo_delay_us, t->echo_level_db);
        }
        printf("}\n");
    } else {
        /* The names of the columns now and then, the numbers between. */
        if (dvbt_watch.lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %7s %7s %8s %6s %10s %8s %6s\n%8s %7s %7s %8s %6s %10s %8s %6s\n",
                   "time", "MER", "margin", "level", "guard", "offset", "echo", "echo",
                   "s", "dB", "dB", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "corr", "Hz", "us", "dB");
        if (t->found && t->pilots) {
            printf("%8.2f %7.1f ", now, t->mer_db);
            if (isfinite(needed))
                printf("%+7.1f", t->mer_db - needed);
            else
                printf("%7s", "--");
            printf(" %8.1f %6.2f %+10.1f", t->power_dbfs + set->ref_offset_db, t->sync_quality, t->freq_error_hz);
            if (t->echo_level_db > -99.0)
                printf(" %+8.2f %6.1f", t->echo_delay_us, t->echo_level_db);
            printf("\n");
        } else {
            printf("%8.2f  no multiplex (guard correlation %.3f)\n", now, t->sync_quality);
        }
    }
    return 0;
}

/* Command line */
/*--------------*/

static const struct option dvbt_options[] = {
    { "dvbt", no_argument, NULL, 0 },
    { NULL, 0, NULL, 0 }
};

static int dvbt_option(int id, const char *arg, struct an_cli *cli)
{
    (void)id;
    (void)arg;
    cli->set->dvbt_on = true;
    return 0;
}

static void dvbt_help(void)
{
    printf("DVB-T options:\n"
           "      --dvbt             With --report: reception of a DVB-T multiplex tuned near its centre:\n"
           "                         mode, TPS, MER, channel and echoes. The sample rate is the one of\n"
           "                         the channel (default with --dvbt: 9.142857 MSPS, 8 MHz).\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* Worker */
/*--------*/

/* The multiplex of the last TPS read, and the captures since. */
struct an_dvbt_track {
    struct an_settings known;
    bool   ok;
    unsigned since;
};

static struct an_dvbt_track dvbt_track;

/* A known multiplex: the TPS read again now and then, with its mode given. */
static void dvbt_worker_prepare(struct an_settings *set, bool live)
{
    struct an_dvbt_track *dv = &dvbt_track;

    set->dvbt_fast = false;
    memset(&set->dvbt_hint, 0, sizeof(set->dvbt_hint));
    if (set->dvbt_on && dv->ok && live && set->freq_hz == dv->known.freq_hz &&
        set->sample_rate == dv->known.sample_rate && set->channel == dv->known.channel) {
        set->dvbt_fast = ++dv->since % AN_DVBT_FULL_EVERY != 0;
        set->dvbt_hint = dv->known.dvbt_hint;
    }
}

static void dvbt_worker_result(const struct an_settings *set, const struct an_result *res)
{
    struct an_dvbt_track *dv = &dvbt_track;
    const struct m2sdr_dvbt_result *t = &res->dvbt;

    if (!set->dvbt_on || !res->dvbt_valid)
        return;
    if (t->tps) {
        dv->known = *set;
        dv->known.dvbt_hint.fft_len    = t->fft_len;
        dv->known.dvbt_hint.guard      = t->guard;
        dv->known.dvbt_hint.modulation = t->tps_modulation;
        dv->ok    = true;
        dv->since = 0;
    } else if (!t->found || !t->pilots) {
        dv->ok = false;
    }
    /* The offset follows every result of the same tuning. */
    if (dv->ok && t->found && t->pilots && set->freq_hz == dv->known.freq_hz) {
        dv->known.dvbt_hint.offset_known   = true;
        dv->known.dvbt_hint.carrier_offset = t->carrier_offset;
    }
}

/* View */
/*------*/

#define DVBT_UHF_BASE_HZ   306e6     /* European UHF channel n is centred at 306 + 8 n MHz (21 to 69). */

static void dvbt_keep(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_dvbt_result *t = &res->dvbt;

    if (!res->set.dvbt_on || !res->dvbt_valid)
        return;
    if (trend_add(&ui->dvbt, &res->set, t->found && t->pilots, t->mer_db, t->mer_bin_db, t->channel_bin_db, t->bins,
                  t->echo_db, M2SDR_DVBT_ECHO_BINS) && ui->dvbt_tps)
        ui->dvbt_tps->tps = false;
    if (t->tps) {
        if (!ui->dvbt_tps)
            ui->dvbt_tps = malloc(sizeof(*ui->dvbt_tps));
        if (ui->dvbt_tps)
            *ui->dvbt_tps = *t;
    }
}

static void dvbt_close(struct an_ui *ui)
{
    free(ui->dvbt_tps);
}

/* Result whose TPS fields are shown: this one, or the last one that read it. */
static const struct m2sdr_dvbt_result *dvbt_tps_of(const struct an_ui *ui)
{
    const struct m2sdr_dvbt_result *t = &ui->res.dvbt;

    return t->tps || !ui->dvbt_tps || !ui->dvbt_tps->tps ? t : ui->dvbt_tps;
}

static void draw_dvbt(struct an_ui *ui, bool live, bool *changed)
{
    static const double rates[3] = { 64e6 / 7.0, 8e6, 48e6 / 7.0 };
    static const char *const rate_names[3] = { "8 MHz", "7 MHz", "6 MHz" };
    const struct an_result *res = &ui->res;
    const struct m2sdr_dvbt_result *t = &res->dvbt, *tp;
    const bool ok = t->found && t->pilots;
    ImVec2 avail, sq;
    float side;
    double needed;
    char text[160];
    int i;

    if (live) {
        int ch = (int)lround((ui->set.freq_hz - DVBT_UHF_BASE_HZ) / 8e6);

        igTextUnformatted("Channel:", NULL);
        for (i = 0; i < 3; i++) {
            igSameLine(0.0f, 8.0f);
            if (igSmallButton(rate_names[i]))
                view_rate(ui, rates[i], changed);
        }
        igSameLine(0.0f, 16.0f);
        igTextUnformatted("UHF", NULL);
        igSameLine(0.0f, 4.0f);
        igSetNextItemWidth(100.0f);
        if (igInputInt("##uhf", &ch, 1, 1, 0) && ch >= 21 && ch <= 69)
            view_retune(ui, DVBT_UHF_BASE_HZ + 8e6 * ch, changed);
        if (ok && res->dvbt_valid && fabs(t->freq_error_hz) > 500.0) {
            igSameLine(0.0f, 16.0f);
            snprintf(text, sizeof(text), "Centre on the multiplex (%+.1f kHz)", t->freq_error_hz / 1e3);
            if (igSmallButton(text))
                view_retune(ui, ui->set.freq_hz + 1e3 * round(t->freq_error_hz / 1e3), changed);
        }
    }
    if (!res->set.dvbt_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->dvbt_valid) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted("The sample rate must be the one of the channel: 9.142857 MSPS for 8 MHz, 8 MSPS for 7 MHz, "
                          "6.857143 MSPS for 6 MHz.", NULL);
        igPopTextWrapPos();
        return;
    }
    if (!t->found) {
        igPushTextWrapPos(0.0f);
        igText("No DVB-T multiplex (guard interval correlation %.3f): tune near the centre of one.", t->sync_quality);
        igPopTextWrapPos();
    }
    tp     = dvbt_tps_of(ui);
    needed = dvbt_needed_db(tp);
    if (isfinite(needed))
        snprintf(text, sizeof(text), "Needed for %s %s: %.1f dB (Gaussian channel), margin %+.1f dB",
                 m2sdr_modulation_name(tp->tps_modulation), tp->code_rate_hp, needed,
                 ok ? trend_mer_shown(&ui->dvbt, t->mer_db) - needed : -needed);
    else
        snprintf(text, sizeof(text), "%s", ok ? "TPS not read: no reference for the MER needed." : " ");

    igGetContentRegionAvail(&avail);
    side = 0.55f * avail.y < 0.42f * avail.x ? 0.55f * avail.y : 0.42f * avail.x;
    sq   = m2sdr_plot_square(side);
    igBeginGroup();
    draw_trend_mer("##dvbtmer", &ui->dvbt, ok, t->mer_db, needed, text, avail.x - sq.x - 8.0f, sq.y);
    igEndGroup();
    igSameLine(0.0f, 8.0f);
    draw_trend_points("##dvbtconst", sq, t->points, ok ? t->npoints : 0, ok ? t->modulation : M2SDR_MOD_64QAM);
    if (ok)
        draw_trend_traces("##dvbt", &ui->dvbt, t->bins, 0.5 * (t->carriers - 1) * res->set.sample_rate / t->fft_len,
                          t->echo_start_us, t->echo_step_us, M2SDR_DVBT_ECHO_BINS, t->guard_us, needed);
}

static void draw_dvbt_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_dvbt_result *t = &res->dvbt, *tp = dvbt_tps_of(ui);
    const double needed = dvbt_needed_db(tp);

    igSeparatorText("DVB-T");
    if (!res->dvbt_valid || !t->found) {
        igTextUnformatted("No multiplex.", NULL);
        return;
    }
    if (igBeginTable("##dvbt", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Mode", "%dk, guard 1/%d", t->fft_len / 1024, t->guard);
        label_value("Guard interval", "%.0f us", t->guard_us);
        label_value("Guard correlation", "%.2f (%.1f dB)", t->sync_quality, t->sync_snr_db);
        label_value("Power", "%.1f dBFS", t->power_dbfs);
        if (t->pilots) {
            label_value("Multiplex", "%.6f MHz", (res->set.freq_hz + t->freq_error_hz) / 1e6);
            label_value("Offset", "%+.1f Hz", t->freq_error_hz);
            if (tp->tps) {
                label_value("Constellation", "%s", m2sdr_modulation_name(tp->tps_modulation));
                label_value("Code rate", tp->tps_hierarchy ? "HP %s, LP %s" : "%s", tp->code_rate_hp, tp->code_rate_lp);
                label_value("Hierarchy", tp->tps_hierarchy ? "alpha %d" : "none", tp->tps_hierarchy);
                if (tp->cell_id >= 0)
                    label_value("Cell", "%d", tp->cell_id);
            } else {
                label_value("TPS", "not read");
            }
            label_value("MER", "%.1f dB (last %.1f)", trend_mer_shown(&ui->dvbt, t->mer_db), t->mer_db);
            if (isfinite(needed))
                label_value("Margin", "%+.1f dB", trend_mer_shown(&ui->dvbt, t->mer_db) - needed);
            if (t->echo_level_db > -99.0)
                label_value("Strongest echo", "%+.2f us, %.1f dB", t->echo_delay_us, t->echo_level_db);
            label_value("Symbols", "%d%s", t->symbols, t->tps ? "" : " (TPS of an earlier capture)");
        } else {
            label_value("Pilots", "not found (%.2f)", t->pilot_quality);
        }
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_dvbt = {
    .key            = "dvbt",
    .title          = "DVB-T",
    .category       = AN_CAT_BROADCAST,
    .on             = offsetof(struct an_settings, dvbt_on),
    .valid          = offsetof(struct an_result, dvbt_valid),
    .report_rank    = 20,
    .default_rate   = AN_DVBT_RATE,
    .psd_short      = true,
    .samples        = dvbt_samples,
    .analyse        = dvbt_analyse,
    .report_text    = dvbt_report_text,
    .report_json    = dvbt_report_json,
    .watch_prepare  = dvbt_watch_prepare,
    .watch_line     = dvbt_watch_line,
    .options        = dvbt_options,
    .option         = dvbt_option,
    .help           = dvbt_help,
#ifdef M2SDR_ANALYZER_GUI
    .worker_prepare = dvbt_worker_prepare,
    .worker_result  = dvbt_worker_result,
    .draw           = draw_dvbt,
    .results        = draw_dvbt_results,
    .keep           = dvbt_keep,
    .close          = dvbt_close,
#endif
};
