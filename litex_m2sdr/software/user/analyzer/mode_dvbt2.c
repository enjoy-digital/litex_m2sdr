/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: reception of a DVB-T2 multiplex (P1, FFT size, guard interval,
 * pilot pattern, pilot and data MER, constellation, channel, echoes), in a report, a view and
 * --watch. S1/S2 and the L1 signalling are not decoded.
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

#include "analyzer.h"

#define DVBT2_SECONDS       0.25    /* A T2 frame (up to 250 ms): a P1 in most captures. */
#define DVBT2_FAST_SECONDS  0.08    /* Once the mode and pattern are known: 18 symbols of 32k (PP8). */
#define DVBT2_FULL_EVERY    100     /* Live, one capture in this many (about 8 s) is a whole one again. */

static const char *const dvbt2_not_read = "S1/S2 and L1 (code rate, PLPs, cell) are not decoded.";

/* Settings and result */
/*-----------------------*/

extern const struct an_mode an_mode_dvbt2;

/* Its settings: its block of struct an_settings, read with SET(). */
struct dvbt2_set {
    /* DVB-T2 multiplex; set by the worker once its mode and pattern are known: short captures. */
    bool   dvbt2_on;
    bool   dvbt2_fast;
    struct m2sdr_dvbt2_config dvbt2_hint;
};

#define SET(set) ((struct dvbt2_set *)an_mode_settings(&an_mode_dvbt2, (set)))

/* The result of the mode: its block of struct an_result, read with RES(). */
struct dvbt2_res {
    bool   dvbt2_valid;            /* Measured: the sample rate was one of DVB-T2; dvbt2.found says the rest. */
    struct m2sdr_dvbt2_result dvbt2;
};

#define RES(res) ((struct dvbt2_res *)an_mode_result(&an_mode_dvbt2, (res)))

/* Analysis */
/*----------*/

static size_t dvbt2_samples(const struct an_settings *set)
{
    return (size_t)((SET(set)->dvbt2_fast ? DVBT2_FAST_SECONDS : DVBT2_SECONDS) * set->sample_rate);
}

static void dvbt2_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = (size_t)(DVBT2_SECONDS * set->sample_rate);

    RES(res)->dvbt2_valid = m2sdr_dvbt2_analyse(x, count < n ? count : n, set->sample_rate,
                                                SET(set)->dvbt2_hint.fft_len ? &SET(set)->dvbt2_hint : NULL, &RES(res)->dvbt2) == 0;
}

static const char *dvbt2_constellation(const struct m2sdr_dvbt2_result *t, char *text, size_t len)
{
    if (t->modulation != M2SDR_MOD_AUTO)
        snprintf(text, len, "%s%s", m2sdr_modulation_name(t->modulation), t->rotated ? " rotated" : "");
    else
        snprintf(text, len, "%s", t->rotated ? "rotated, order not known" : "not found");
    return text;
}

/* Reports */
/*---------*/

static void dvbt2_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_dvbt2_result *t = &RES(res)->dvbt2;
    char text[64];

    printf("\nDVB-T2:\n");
    if (!RES(res)->dvbt2_valid) {
        printf("  The sample rate must be the one of the channel: 9.142857 MSPS (8 MHz), 8 MSPS (7 MHz)\n"
               "  or 6.857143 MSPS (6 MHz).\n");
        return;
    }
    if (t->dvbt) {
        printf("  Not DVB-T2: a DVB-T multiplex (%dk, guard 1/%d, its continual pilots), see --dvbt.\n",
               t->fft_len / 1024, t->guard_den);
        return;
    }
    if (!t->found) {
        printf("  No multiplex found (guard interval correlation %.3f).\n", t->sync_quality);
        return;
    }
    printf("  Mode       : %dk%s, guard interval %d/%d (%.0f us), %.1f %s\n", t->fft_len / 1024,
           t->extended ? " extended" : "", t->guard_num, t->guard_den, t->guard_us, t->power_dbfs + rep->off, rep->unit);
    printf("  Sync       : guard correlation %.2f (about %.1f dB S/N)\n", t->sync_quality, t->sync_snr_db);
    if (t->p1_count)
        printf("  P1         : %d in the capture (correlation %.2f)%s", t->p1_count, t->p1_quality,
               t->frame_ms > 0 ? "" : "\n");
    else
        printf("  P1         : none in the capture\n");
    if (t->frame_ms > 0)
        printf(", T2 frame %.3f ms\n", t->frame_ms);
    if (!t->pilots) {
        printf("  Pilots     : not found (pattern %.2f, PRBS %.2f): too much noise for the figures below.\n",
               t->pattern_quality, t->pilot_quality);
    } else {
        printf("  Pilots     : PP%d (Dx %d, Dy %d), %d continual pilots\n", t->pattern, t->pattern_dx, t->pattern_dy,
               t->continual_pilots);
        printf("  Multiplex  : %.6f MHz (%+.1f Hz from the centre)\n", (set->freq_hz + t->freq_error_hz) / 1e6, t->freq_error_hz);
        if (t->pilot_mer_valid)
            printf("  Pilot MER  : %.1f dB\n", t->pilot_mer_db);
        printf("  Cells      : %s (fit %.1f)\n", dvbt2_constellation(t, text, sizeof(text)), t->fit);
        if (t->mer_valid)
            printf("  MER        : %.1f dB against %s, over %d symbols\n", t->mer_db, m2sdr_modulation_name(t->modulation),
                   t->symbols);
        else
            printf("  MER        : not measured on the data cells (%s): the pilot MER stands for it\n",
                   t->rotated ? "a rotated constellation is not sliced without deinterleaving" : "constellation not found");
        if (t->echo_level_db > -99.0)
            printf("  Echo       : strongest at %+.2f us, %.1f dB (guard interval %.0f us)\n", t->echo_delay_us,
                   t->echo_level_db, t->guard_us);
    }
    printf("  Not read   : %s\n", dvbt2_not_read);
}

static void dvbt2_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_dvbt2_result *t = &RES(res)->dvbt2;
    const bool ok = RES(res)->dvbt2_valid && t->found;

    printf("  \"dvbt2\": {\n");
    print_json_number("sync_quality", RES(res)->dvbt2_valid ? t->sync_quality : 0.0, false);
    printf("    \"dvbt\": %s,\n", RES(res)->dvbt2_valid && t->dvbt ? "true" : "false");
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (!ok) {
        printf("  }");
        return;
    }
    printf("    \"fft_size\": \"%dk\",\n", t->fft_len / 1024);
    printf("    \"extended\": %s,\n", t->extended ? "true" : "false");
    printf("    \"guard_interval\": \"%d/%d\",\n", t->guard_num, t->guard_den);
    print_json_number("power_db", t->power_dbfs + rep->off, false);
    printf("    \"p1_count\": %d,\n", t->p1_count);
    print_json_number("frame_ms", t->frame_ms > 0 ? t->frame_ms : NAN, false);
    printf("    \"s1s2_decoded\": false,\n");
    printf("    \"l1_decoded\": false,\n");
    printf("    \"pilots\": %s%s\n", t->pilots ? "true" : "false", t->pilots ? "," : "");
    if (t->pilots) {
        printf("    \"pattern\": \"PP%d\",\n", t->pattern);
        print_json_number("carrier_hz", set->freq_hz + t->freq_error_hz, false);
        print_json_number("carrier_offset_hz", t->freq_error_hz, false);
        printf("    \"continual_pilots\": %d,\n", t->continual_pilots);
        print_json_number("pilot_mer_db", t->pilot_mer_valid ? t->pilot_mer_db : NAN, false);
        printf("    \"constellation\": \"%s\",\n", t->modulation != M2SDR_MOD_AUTO ? m2sdr_modulation_name(t->modulation) : "");
        printf("    \"rotated\": %s,\n", t->rotated ? "true" : "false");
        print_json_number("fit", t->fit, false);
        print_json_number("echo_delay_us", t->echo_delay_us, false);
        print_json_number("echo_level_db", t->echo_level_db, false);
        print_json_number("mer_db", t->mer_valid ? t->mer_db : NAN, true);
    }
    printf("  }");
}

/* Watch */
/*-------*/

/* --watch: what the captures read of the multiplex, for short captures once it is known. */
static struct {
    struct m2sdr_dvbt2_result last;
    struct m2sdr_dvbt2_config known;
    unsigned since, lines;
    bool   ok;
} dvbt2_watch;

/* What a result tells of the multiplex for the next captures. */
static void dvbt2_learn(struct m2sdr_dvbt2_config *known, const struct m2sdr_dvbt2_result *t)
{
    known->fft_len        = t->fft_len;
    known->guard_num      = t->guard_num;
    known->guard_den      = t->guard_den;
    known->pattern        = t->pattern;
    known->modulation     = t->modulation;
    known->rotated        = t->rotated && t->modulation != M2SDR_MOD_AUTO;
    known->offset_known   = true;
    known->carrier_offset = t->carrier_offset;
}

static void dvbt2_watch_prepare(struct an_settings *set)
{
    SET(set)->dvbt2_fast = dvbt2_watch.ok && ++dvbt2_watch.since % DVBT2_FULL_EVERY != 0;
    memset(&SET(set)->dvbt2_hint, 0, sizeof(SET(set)->dvbt2_hint));
    if (SET(set)->dvbt2_fast)
        SET(set)->dvbt2_hint = dvbt2_watch.known;
}

static int dvbt2_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_dvbt2_result *t = &RES(res)->dvbt2, *l = &dvbt2_watch.last;
    const bool ok = t->found && t->pilots;
    char text[64];

    if (!RES(res)->dvbt2_valid) {
        fprintf(stderr, "The sample rate is not one of DVB-T2.\n");
        return -1;
    }
    if (ok && !SET(set)->dvbt2_fast) {
        /* The multiplex, when a whole capture reads it differently: the reference of the lines. */
        if (!json && (!dvbt2_watch.ok || l->fft_len != t->fft_len || l->guard_den != t->guard_den ||
                      l->guard_num != t->guard_num || l->pattern != t->pattern || l->extended != t->extended ||
                      l->modulation != t->modulation || l->rotated != t->rotated)) {
            printf("Multiplex %.3f MHz: %dk%s, guard %d/%d, PP%d, %s", (set->freq_hz + t->freq_error_hz) / 1e6,
                   t->fft_len / 1024, t->extended ? " extended" : "", t->guard_num, t->guard_den, t->pattern,
                   dvbt2_constellation(t, text, sizeof(text)));
            if (t->frame_ms > 0)
                printf(", frame %.1f ms", t->frame_ms);
            printf(" (%s)\n", dvbt2_not_read);
            dvbt2_watch.lines = 0;
        }
        dvbt2_watch.last  = *t;
        dvbt2_watch.ok    = true;
        dvbt2_watch.since = 0;
        dvbt2_learn(&dvbt2_watch.known, t);
    } else if (!ok) {
        dvbt2_watch.ok = false;
    }
    if (dvbt2_watch.ok && ok) {
        dvbt2_watch.known.offset_known   = true;
        dvbt2_watch.known.carrier_offset = t->carrier_offset;
    }
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, ok ? "true" : "false");
        if (ok) {
            if (t->mer_valid)
                printf(", \"mer_db\": %.2f", t->mer_db);
            if (t->pilot_mer_valid)
                printf(", \"pilot_mer_db\": %.2f", t->pilot_mer_db);
            printf(", \"power_db\": %.2f, \"sync_quality\": %.3f, \"carrier_offset_hz\": %.1f",
                   t->power_dbfs + set->ref_offset_db, t->sync_quality, t->freq_error_hz);
            if (t->echo_level_db > -99.0)
                printf(", \"echo_delay_us\": %.2f, \"echo_level_db\": %.1f", t->echo_delay_us, t->echo_level_db);
        }
        printf("}\n");
    } else {
        if (dvbt2_watch.lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %7s %7s %8s %6s %10s %8s %6s\n%8s %7s %7s %8s %6s %10s %8s %6s\n",
                   "time", "MER", "pilots", "level", "guard", "offset", "echo", "echo",
                   "s", "dB", "MER dB", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "corr", "Hz", "us", "dB");
        if (ok) {
            printf("%8.2f ", now);
            if (t->mer_valid)
                printf("%7.1f", t->mer_db);
            else
                printf("%7s", "--");
            if (t->pilot_mer_valid)
                printf(" %7.1f", t->pilot_mer_db);
            else
                printf(" %7s", "--");
            printf(" %8.1f %6.2f %+10.1f", t->power_dbfs + set->ref_offset_db, t->sync_quality, t->freq_error_hz);
            if (t->echo_level_db > -99.0)
                printf(" %+8.2f %6.1f", t->echo_delay_us, t->echo_level_db);
            printf("\n");
        } else {
            if (t->dvbt)
                printf("%8.2f  not DVB-T2: a DVB-T multiplex (see --dvbt)\n", now);
            else
                printf("%8.2f  no multiplex (guard correlation %.3f)\n", now, t->sync_quality);
        }
    }
    return 0;
}

/* Command line */
/*--------------*/

static const struct option dvbt2_options[] = {
    { "dvbt2", no_argument, NULL, 0 },
    { NULL, 0, NULL, 0 }
};

static int dvbt2_option(int id, const char *arg, struct an_cli *cli)
{
    (void)id;
    (void)arg;
    SET(cli->set)->dvbt2_on = true;
    return 0;
}

static void dvbt2_help(void)
{
    printf("DVB-T2 options:\n"
           "      --dvbt2            With --report: reception of a DVB-T2 multiplex tuned near its centre:\n"
           "                         P1, FFT size, guard, pilot pattern, pilot and data MER, constellation,\n"
           "                         channel and echoes; S1/S2 and L1 are not decoded. Sample rate as for\n"
           "                         --dvbt (default 9.142857 MSPS, 8 MHz). --watch works too.\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* Worker */
/*--------*/

/* What a whole capture read of the multiplex of a tuning, and the captures since. */
static struct {
    struct an_settings known;
    bool   ok;
    unsigned since;
} dvbt2_track;

static void dvbt2_worker_prepare(struct an_settings *set, bool live)
{
    SET(set)->dvbt2_fast = false;
    memset(&SET(set)->dvbt2_hint, 0, sizeof(SET(set)->dvbt2_hint));
    if (SET(set)->dvbt2_on && dvbt2_track.ok && live && set->freq_hz == dvbt2_track.known.freq_hz &&
        set->sample_rate == dvbt2_track.known.sample_rate && set->channel == dvbt2_track.known.channel) {
        SET(set)->dvbt2_fast = ++dvbt2_track.since % DVBT2_FULL_EVERY != 0;
        if (SET(set)->dvbt2_fast)
            SET(set)->dvbt2_hint = SET(&dvbt2_track.known)->dvbt2_hint;
    }
}

static void dvbt2_worker_result(const struct an_settings *set, const struct an_result *res)
{
    const struct m2sdr_dvbt2_result *t = &RES(res)->dvbt2;

    if (!SET(set)->dvbt2_on || !RES(res)->dvbt2_valid)
        return;
    if (!t->found || !t->pilots) {
        dvbt2_track.ok = false;
        return;
    }
    if (!SET(set)->dvbt2_fast) {
        dvbt2_track.known = *set;
        dvbt2_learn(&SET(&dvbt2_track.known)->dvbt2_hint, t);
        dvbt2_track.ok    = true;
        dvbt2_track.since = 0;
    } else if (set->freq_hz == dvbt2_track.known.freq_hz) {
        /* The offset follows every result of the same tuning. */
        SET(&dvbt2_track.known)->dvbt2_hint.carrier_offset = t->carrier_offset;
    }
}

/* View */
/*------*/

/* State of the view, from a frame to the next (one window). */
static struct {
    /* MER over the results of one tuning, and the last capture with a P1. */
    struct an_trend dvbt2;
    struct m2sdr_dvbt2_result *dvbt2_p1;
} dvbt2_ui;

/* The MER to follow: the one of the data cells, or of the pilots when the data have none
 * (rotated or not found constellation). */
static double dvbt2_mer(const struct m2sdr_dvbt2_result *t)
{
    return t->mer_valid ? t->mer_db : t->pilot_mer_valid ? t->pilot_mer_db : NAN;
}

#define DVBT2_UHF_BASE_HZ  306e6     /* European UHF channel n is centred at 306 + 8 n MHz (21 to 69). */

static void dvbt2_keep(struct an_ui *ui)
{
    static float no_mer[M2SDR_DVBT2_BINS];
    const struct an_result *res = &ui->res;
    const struct m2sdr_dvbt2_result *t = &RES(res)->dvbt2;
    const bool ok = t->found && t->pilots && isfinite(dvbt2_mer(t));
    int i;

    if (!SET(&res->set)->dvbt2_on || !RES(res)->dvbt2_valid)
        return;
    /* No data MER: its trace below the plot. */
    for (i = 0; i < M2SDR_DVBT2_BINS; i++)
        no_mer[i] = -99.0f;
    if (trend_add(&dvbt2_ui.dvbt2, &res->set, ok, ok ? dvbt2_mer(t) : 0.0, t->mer_valid ? t->mer_bin_db : no_mer,
                  t->channel_bin_db, t->bins, t->echo_db, M2SDR_DVBT2_ECHO_BINS) && dvbt2_ui.dvbt2_p1)
        dvbt2_ui.dvbt2_p1->p1_count = 0;
    if (t->p1_count) {
        if (!dvbt2_ui.dvbt2_p1)
            dvbt2_ui.dvbt2_p1 = malloc(sizeof(*dvbt2_ui.dvbt2_p1));
        if (dvbt2_ui.dvbt2_p1)
            *dvbt2_ui.dvbt2_p1 = *t;
    }
}

static void dvbt2_close(struct an_ui *ui)
{
    free(dvbt2_ui.dvbt2_p1);
    dvbt2_ui.dvbt2_p1 = NULL;
}

/* Result whose P1 is shown: this one, or the last one with a P1 (the short captures often have none). */
static const struct m2sdr_dvbt2_result *dvbt2_p1_of(const struct an_ui *ui)
{
    const struct m2sdr_dvbt2_result *t = &RES(ui->shown)->dvbt2;

    return t->p1_count || !dvbt2_ui.dvbt2_p1 || !dvbt2_ui.dvbt2_p1->p1_count ? t : dvbt2_ui.dvbt2_p1;
}

static void draw_dvbt2(struct an_ui *ui, bool live, bool *changed)
{
    static const double rates[3] = { 64e6 / 7.0, 8e6, 48e6 / 7.0 };
    static const char *const rate_names[3] = { "8 MHz", "7 MHz", "6 MHz" };
    const struct an_result *res = &ui->res;
    const struct m2sdr_dvbt2_result *t = &RES(res)->dvbt2, *p1;
    const bool ok = t->found && t->pilots;
    ImVec2 avail, sq;
    float side;
    char text[256], cons[64];
    int i;

    if (live) {
        int ch = (int)lround((ui->set.freq_hz - DVBT2_UHF_BASE_HZ) / 8e6);

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
        if (igInputInt("##uhf2", &ch, 1, 1, 0) && ch >= 21 && ch <= 69)
            view_retune(ui, DVBT2_UHF_BASE_HZ + 8e6 * ch, changed);
        if (ok && RES(res)->dvbt2_valid && fabs(t->freq_error_hz) > 500.0) {
            igSameLine(0.0f, 16.0f);
            snprintf(text, sizeof(text), "Centre on the multiplex (%+.1f kHz)", t->freq_error_hz / 1e3);
            if (igSmallButton(text))
                view_retune(ui, ui->set.freq_hz + 1e3 * round(t->freq_error_hz / 1e3), changed);
        }
    }
    if (!SET(&res->set)->dvbt2_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!RES(res)->dvbt2_valid) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted("The sample rate must be the one of the channel: 9.142857 MSPS for 8 MHz, 8 MSPS for 7 MHz, "
                          "6.857143 MSPS for 6 MHz.", NULL);
        igPopTextWrapPos();
        return;
    }
    if (!t->found) {
        igPushTextWrapPos(0.0f);
        if (t->dvbt)
            igText("Not DVB-T2: a DVB-T multiplex (%dk, guard 1/%d, its continual pilots). See the DVB-T view.",
                   t->fft_len / 1024, t->guard_den);
        else
            igText("No DVB-T2 multiplex (guard interval correlation %.3f): tune near the centre of one.", t->sync_quality);
        igPopTextWrapPos();
    }
    /* The line under the MER: what it is, the mode, and what is not read. */
    p1 = dvbt2_p1_of(ui);
    if (ok) {
        int n = snprintf(text, sizeof(text), "%s, pilot MER ", t->mer_valid ? "Data cells" : "Pilots (no data MER)");

        n += snprintf(text + n, sizeof(text) - (size_t)n, t->pilot_mer_valid ? "%.1f dB" : "--", t->pilot_mer_db);
        n += snprintf(text + n, sizeof(text) - (size_t)n, ". %dk%s, guard %d/%d, PP%d", t->fft_len / 1024,
                      t->extended ? " ext" : "", t->guard_num, t->guard_den, t->pattern);
        if (p1->frame_ms > 0)
            n += snprintf(text + n, sizeof(text) - (size_t)n, ", T2 frame %.1f ms", p1->frame_ms);
        else if (p1->p1_count)
            n += snprintf(text + n, sizeof(text) - (size_t)n, ", P1 seen");
        snprintf(text + n, sizeof(text) - (size_t)n, ". %s", dvbt2_not_read);
    } else {
        snprintf(text, sizeof(text), "%s", dvbt2_not_read);
    }

    igGetContentRegionAvail(&avail);
    side = 0.55f * avail.y < 0.42f * avail.x ? 0.55f * avail.y : 0.42f * avail.x;
    sq   = m2sdr_plot_square(side);
    igBeginGroup();
    draw_trend_mer("##dvbt2mer", &dvbt2_ui.dvbt2, ok && isfinite(dvbt2_mer(t)), ok ? dvbt2_mer(t) : 0.0, NAN, text,
                   avail.x - sq.x - 8.0f, sq.y);
    igEndGroup();
    igSameLine(0.0f, 8.0f);
    igBeginGroup();
    /* A rotated constellation has no grid to draw: its Q is the one of another cell. */
    igText("Cells: %s", ok ? dvbt2_constellation(t, cons, sizeof(cons)) : "--");
    draw_trend_points("##dvbt2const", (ImVec2){ sq.x, sq.y - igGetTextLineHeightWithSpacing() }, t->points,
                      ok ? t->npoints : 0, ok && !t->rotated ? t->modulation : M2SDR_MOD_AUTO);
    igEndGroup();
    if (ok)
        draw_trend_traces("##dvbt2", &dvbt2_ui.dvbt2, t->bins, 0.5 * (t->carriers - 1) * res->set.sample_rate / t->fft_len,
                          t->echo_start_us, t->echo_step_us, M2SDR_DVBT2_ECHO_BINS, t->guard_us, NAN);
}

static void draw_dvbt2_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_dvbt2_result *t = &RES(res)->dvbt2, *p1 = dvbt2_p1_of(ui);
    char cons[64];

    igSeparatorText("DVB-T2");
    if (!RES(res)->dvbt2_valid || !t->found) {
        igTextUnformatted(RES(res)->dvbt2_valid && t->dvbt ? "Not DVB-T2: a DVB-T multiplex." : "No multiplex.", NULL);
        return;
    }
    if (igBeginTable("##dvbt2", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("FFT size", "%dk%s", t->fft_len / 1024, t->extended ? " extended" : "");
        label_value("Guard interval", "%d/%d (%.0f us)", t->guard_num, t->guard_den, t->guard_us);
        label_value("Guard correlation", "%.2f (%.1f dB)", t->sync_quality, t->sync_snr_db);
        label_value("Power", "%.1f dBFS", t->power_dbfs);
        if (p1->p1_count)
            label_value("P1", p1->frame_ms > 0 ? "%d, frame %.3f ms" : "%d", p1->p1_count, p1->frame_ms);
        else
            label_value("P1", "none seen");
        label_value("S1/S2, L1", "not decoded");
        if (t->pilots) {
            label_value("Pattern", "PP%d (Dx %d, Dy %d)", t->pattern, t->pattern_dx, t->pattern_dy);
            label_value("Multiplex", "%.6f MHz", (res->set.freq_hz + t->freq_error_hz) / 1e6);
            label_value("Offset", "%+.1f Hz", t->freq_error_hz);
            label_value("Continual pilots", "%d", t->continual_pilots);
            if (t->pilot_mer_valid)
                label_value("Pilot MER", "%.1f dB", t->pilot_mer_db);
            label_value("Cells", "%s", dvbt2_constellation(t, cons, sizeof(cons)));
            label_value("Fit", "%.1f (1: none)", t->fit);
            if (t->mer_valid)
                label_value("Data MER", "%.1f dB", t->mer_db);
            else
                label_value("Data MER", t->rotated ? "-- (rotated)" : "--");
            label_value("MER shown", "%.1f dB", trend_mer_shown(&dvbt2_ui.dvbt2, dvbt2_mer(t)));
            if (t->echo_level_db > -99.0)
                label_value("Strongest echo", "%+.2f us, %.1f dB", t->echo_delay_us, t->echo_level_db);
            label_value("Symbols", "%d%s", t->symbols, SET(&res->set)->dvbt2_fast ? " (short capture)" : "");
        } else {
            label_value("Pilots", "not found (%.2f, %.2f)", t->pattern_quality, t->pilot_quality);
        }
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_dvbt2 = {
    .key            = "dvbt2",
    .title          = "DVB-T2",
    .category       = AN_CAT_BROADCAST,
    .settings_size  = sizeof(struct dvbt2_set),
    .on             = offsetof(struct dvbt2_set, dvbt2_on),
    .result_size    = sizeof(struct dvbt2_res),
    .valid          = offsetof(struct dvbt2_res, dvbt2_valid),
    .found          = offsetof(struct dvbt2_res, dvbt2.found),
    .report_rank    = 21,
    .default_rate   = AN_DVBT_RATE,
    .psd_short      = true,
    .options        = dvbt2_options,
    .option         = dvbt2_option,
    .help           = dvbt2_help,
    .samples        = dvbt2_samples,
    .analyse        = dvbt2_analyse,
    .report_text    = dvbt2_report_text,
    .report_json    = dvbt2_report_json,
    .watch_prepare  = dvbt2_watch_prepare,
    .watch_line     = dvbt2_watch_line,
#ifdef M2SDR_ANALYZER_GUI
    .worker_prepare = dvbt2_worker_prepare,
    .worker_result  = dvbt2_worker_result,
    .draw           = draw_dvbt2,
    .results        = draw_dvbt2_results,
    .keep           = dvbt2_keep,
    .close          = dvbt2_close,
#endif
};
