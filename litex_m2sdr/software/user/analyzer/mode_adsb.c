/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: ADS-B at 1090 MHz (Mode S extended squitters): the aircraft
 * around, their messages, level and SNR, in a report, a view and --watch.
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

#define ADSB_FREQ_HZ       1090e6
#define ADSB_HISTORY       300       /* Results of the message rate trace. */

/* Result */
/*--------*/

/* The result of the mode: its block of struct an_result, read with RES(). */
struct adsb_res {
    bool   adsb_valid;             /* Measured: the sample rate was 2 MSPS or more. */
    struct m2sdr_adsb_result adsb;
    struct m2sdr_adsb_state adsb_state;   /* Aircraft of the captures so far. */
};

extern const struct an_mode an_mode_adsb;
#define RES(res) ((struct adsb_res *)an_mode_result(&an_mode_adsb, (res)))

/* Analysis */
/*----------*/

/* The aircraft of the captures so far: the CPR positions need an even and an odd frame, often
 * in two captures. Kept by the worker only; each result carries a copy. */
static struct m2sdr_adsb_state adsb_state;

static size_t adsb_samples(const struct an_settings *set)
{
    return (size_t)(AN_ADSB_SECONDS * set->sample_rate);
}

static void adsb_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = adsb_samples(set);

    /* Dated by the clock: the captures of a board follow each other, the pairs of CPR frames are
     * taken within 10 s. */
    RES(res)->adsb_valid = m2sdr_adsb_analyse(x, count < n ? count : n, set->sample_rate, now_ms() / 1e3, &set->adsb,
                                              &adsb_state, &RES(res)->adsb) == 0;
    RES(res)->adsb_state = adsb_state;
}

/* Reports */
/*---------*/

static void adsb_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_adsb_result *a = &RES(res)->adsb;
    const struct m2sdr_adsb_state *st = &RES(res)->adsb_state;
    int i;

    printf("\nADS-B:\n");
    if (!RES(res)->adsb_valid) {
        printf("  The sample rate must be 2 MSPS or more (2.4 MSPS by default).\n");
        return;
    }
    printf("  Messages   : %d (%d with a bit corrected), %.1f per second, %d preambles tried\n", a->messages,
           a->corrected, a->message_rate, a->preambles);
    printf("  Noise      : %.1f %s\n", a->noise_dbfs + rep->off, rep->unit);
    if (st->aircraft == 0)
        return;
    printf("  %-6s %-8s %8s %6s %6s %7s %10s %11s %5s %7s\n", "ICAO", "callsign", "alt ft", "kt", "track", "ft/min",
           "lat", "lon", "msgs", rep->unit);
    for (i = 0; i < st->aircraft; i++) {
        const struct m2sdr_adsb_aircraft *ac = &st->ac[i];
        char alt[16] = "", spd[16] = "", trk[16] = "", vr[16] = "", lat[16] = "", lon[16] = "";

        if (ac->has_altitude)
            snprintf(alt, sizeof(alt), "%d", ac->altitude_ft);
        if (ac->has_velocity) {
            snprintf(spd, sizeof(spd), "%.0f", ac->speed_kt);
            snprintf(trk, sizeof(trk), "%.0f", ac->track_deg);
            snprintf(vr, sizeof(vr), "%+d", ac->vrate_fpm);
        }
        if (ac->has_position) {
            snprintf(lat, sizeof(lat), "%.5f", ac->lat_deg);
            snprintf(lon, sizeof(lon), "%.5f", ac->lon_deg);
        }
        printf("  %06X %-8s %8s %6s %6s %7s %10s %11s %5d %7.1f\n", (unsigned)ac->icao, ac->callsign, alt, spd, trk, vr,
               lat, lon, ac->messages, ac->level_dbfs + rep->off);
    }
}

static void adsb_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_adsb_result *a = &RES(res)->adsb;
    const struct m2sdr_adsb_state *st = &RES(res)->adsb_state;
    int i;

    printf("  \"adsb\": {\n");
    printf("    \"valid\": %s%s\n", RES(res)->adsb_valid ? "true" : "false", RES(res)->adsb_valid ? "," : "");
    if (RES(res)->adsb_valid) {
        printf("    \"messages\": %d,\n", a->messages);
        printf("    \"corrected\": %d,\n", a->corrected);
        print_json_number("message_rate", a->message_rate, false);
        print_json_number("noise_db", a->noise_dbfs + rep->off, false);
        printf("    \"aircraft\": [");
        for (i = 0; i < st->aircraft; i++) {
            const struct m2sdr_adsb_aircraft *ac = &st->ac[i];

            printf("%s\n      {\"icao\": \"%06X\", \"callsign\": \"%s\", \"messages\": %d, \"level_db\": %.2f",
                   i ? "," : "", (unsigned)ac->icao, ac->callsign, ac->messages, ac->level_dbfs + rep->off);
            if (ac->has_altitude)
                printf(", \"altitude_ft\": %d", ac->altitude_ft);
            if (ac->has_velocity)
                printf(", \"speed_kt\": %.1f, \"track_deg\": %.2f, \"vrate_fpm\": %d", ac->speed_kt, ac->track_deg,
                       ac->vrate_fpm);
            if (ac->has_position)
                printf(", \"lat_deg\": %.6f, \"lon_deg\": %.6f", ac->lat_deg, ac->lon_deg);
            printf("}");
        }
        printf("%s]\n", st->aircraft ? "\n    " : "");
    }
    printf("  }");
}

/* Watch */
/*-------*/

/* --watch: one line per message, with what is known of its aircraft. */
static unsigned adsb_watch_lines;

static const struct m2sdr_adsb_aircraft *aircraft_find(const struct m2sdr_adsb_state *st, uint32_t icao)
{
    int i;

    for (i = 0; i < st->aircraft; i++)
        if (st->ac[i].icao == icao)
            return &st->ac[i];
    return NULL;
}

static int adsb_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_adsb_result *a = &RES(res)->adsb;
    int i;

    if (!RES(res)->adsb_valid) {
        fprintf(stderr, "The sample rate must be 2 MSPS or more for ADS-B.\n");
        return -1;
    }
    for (i = 0; i < a->nmessages; i++) {
        const struct m2sdr_adsb_message *m = &a->message[i];
        const struct m2sdr_adsb_aircraft *ac = aircraft_find(&RES(res)->adsb_state, m->icao);
        const char *cs = m->callsign[0] ? m->callsign : ac ? ac->callsign : "";

        if (json) {
            printf("{\"time_s\": %.3f, \"icao\": \"%06X\", \"df\": %d, \"tc\": %d, \"callsign\": \"%s\", "
                   "\"level_db\": %.2f, \"snr_db\": %.2f, \"corrected\": %s", now, (unsigned)m->icao, m->df, m->tc, cs,
                   m->level_dbfs + set->ref_offset_db, m->snr_db, m->corrected ? "true" : "false");
            if (m->has_altitude)
                printf(", \"altitude_ft\": %d", m->altitude_ft);
            if (m->has_velocity)
                printf(", \"speed_kt\": %.1f", m->speed_kt);
            if (m->has_track)
                printf(", \"track_deg\": %.2f", m->track_deg);
            if (m->has_vrate)
                printf(", \"vrate_fpm\": %d", m->vrate_fpm);
            if (m->has_position)
                printf(", \"lat_deg\": %.6f, \"lon_deg\": %.6f", m->lat_deg, m->lon_deg);
            printf("}\n");
            continue;
        }
        if (adsb_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %-6s %-8s %3s %7s %6s %6s %7s %10s %11s %7s %5s\n", "time", "ICAO", "callsign", "TC", "alt ft",
                   "kt", "track", "ft/min", "lat", "lon", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "SNR");
        printf("%8.2f %06X %-8s %3d", now, (unsigned)m->icao, cs, m->tc);
        if (m->has_altitude)
            printf(" %7d", m->altitude_ft);
        else
            printf(" %7s", "");
        if (m->has_velocity)
            printf(" %6.0f", m->speed_kt);
        else
            printf(" %6s", "");
        if (m->has_track)
            printf(" %6.1f", m->track_deg);
        else
            printf(" %6s", "");
        if (m->has_vrate)
            printf(" %+7d", m->vrate_fpm);
        else
            printf(" %7s", "");
        if (m->has_position)
            printf(" %10.5f %11.5f", m->lat_deg, m->lon_deg);
        else
            printf(" %10s %11s", "", "");
        printf(" %7.1f %5.1f%s\n", m->level_dbfs + set->ref_offset_db, m->snr_db, m->corrected ? " (corrected)" : "");
    }
    return 0;
}

/* Command line */
/*--------------*/

static const struct option adsb_options[] = {
    { "adsb",     no_argument,       NULL, 0 },
    { "adsb-ref", required_argument, NULL, 1 },
    { NULL, 0, NULL, 0 }
};

static int adsb_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_settings *set = cli->set;

    if (id == 0) {
        set->adsb_on = true;
        return 0;
    }
    if (sscanf(arg, "%lf,%lf", &set->adsb.ref_lat_deg, &set->adsb.ref_lon_deg) != 2 ||
        fabs(set->adsb.ref_lat_deg) > 90.0 || fabs(set->adsb.ref_lon_deg) > 180.0) {
        m2sdr_cli_error("invalid position '%s' (expected LAT,LON in degrees)", arg);
        return -1;
    }
    set->adsb.has_reference = true;
    return 0;
}

static void adsb_help(void)
{
    printf("ADS-B options:\n"
           "      --adsb             With --report: ADS-B at 1090 MHz (the default with --adsb): aircraft,\n"
           "                         messages, level, SNR. Default 2.4 MSPS: pulses across two samples\n"
           "                         decode with less SNR than at 2 MSPS. --watch: a line per message.\n"
           "      --adsb-ref LAT,LON Receiver position: single position frames are placed near it.\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

static float adsb_rate_hist[ADSB_HISTORY];
static int adsb_nrate;

static void adsb_keep(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;

    if (!res->set.adsb_on || !RES(res)->adsb_valid)
        return;
    if (adsb_nrate == ADSB_HISTORY) {
        memmove(adsb_rate_hist, adsb_rate_hist + 1, (ADSB_HISTORY - 1) * sizeof(adsb_rate_hist[0]));
        adsb_nrate--;
    }
    adsb_rate_hist[adsb_nrate++] = (float)RES(res)->adsb.message_rate;
}

static int cmp_last_seen(const void *a, const void *b)
{
    const double x = (*(const struct m2sdr_adsb_aircraft *const *)a)->last_s;
    const double y = (*(const struct m2sdr_adsb_aircraft *const *)b)->last_s;

    return (x < y) - (x > y);
}

static void draw_adsb(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_adsb_state *st = &RES(res)->adsb_state;
    static const struct m2sdr_adsb_aircraft *order[M2SDR_ADSB_MAX_AIRCRAFT];
    const double now = now_ms() / 1e3;
    struct m2sdr_plot plot;
    ImVec2 avail;
    float top = 1.0f;
    int i;

    if (live) {
        if (fabs(ui->set.freq_hz - ADSB_FREQ_HZ) > 1.0) {
            if (igSmallButton("1090 MHz"))
                view_retune(ui, ADSB_FREQ_HZ, changed);
            igSameLine(0.0f, 16.0f);
        }
        if (fabs(ui->set.sample_rate - AN_ADSB_SAMPLE_RATE) > 1.0) {
            if (igSmallButton("2.4 MSPS"))
                view_rate(ui, AN_ADSB_SAMPLE_RATE, changed);
            igSameLine(0.0f, 16.0f);
        }
    }
    /* Receiver position: single CPR frames are then placed near it. */
    if (igCheckbox("Receiver position", &ui->set.adsb.has_reference))
        *changed = true;
    if (ui->set.adsb.has_reference) {
        igSameLine(0.0f, 8.0f);
        igSetNextItemWidth(110.0f);
        if (igInputDouble("lat##adsbref", &ui->set.adsb.ref_lat_deg, 0.0, 0.0, "%.4f", 0))
            *changed = true;
        igSameLine(0.0f, 8.0f);
        igSetNextItemWidth(110.0f);
        if (igInputDouble("lon##adsbref", &ui->set.adsb.ref_lon_deg, 0.0, 0.0, "%.4f", 0))
            *changed = true;
    }
    if (!res->set.adsb_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!RES(res)->adsb_valid) {
        igTextUnformatted("The sample rate must be 2 MSPS or more: 2.4 MSPS lets pulses across two samples decode.", NULL);
        return;
    }
    igGetContentRegionAvail(&avail);
    for (i = 0; i < adsb_nrate; i++)
        top = adsb_rate_hist[i] > top ? adsb_rate_hist[i] : top;
    if (m2sdr_plot_begin(&plot, "##adsbrate", (ImVec2){ avail.x, 0.25f * avail.y }, -(double)(ADSB_HISTORY - 1), 0.0,
                         M2SDR_PLOT_PLAIN, 0.0, 1.2 * top, "Messages per second, one point per capture")) {
        if (adsb_nrate > 0)
            m2sdr_plot_trace(&plot, adsb_rate_hist, (size_t)adsb_nrate, -(double)(adsb_nrate - 1), 0.0, 0xff40c0ffu);
        m2sdr_plot_end(&plot);
    }
    for (i = 0; i < st->aircraft; i++)
        order[i] = &st->ac[i];
    qsort(order, (size_t)st->aircraft, sizeof(order[0]), cmp_last_seen);
    if (igBeginTable("##adsbac", 11, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                     ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        static const char *const cols[11] = { "ICAO", "Callsign", "Altitude ft", "Speed kt", "Track", "ft/min",
                                              "Latitude", "Longitude", "Last seen", "Messages", "Level dBFS" };

        igTableSetupScrollFreeze(0, 1);
        for (i = 0; i < 11; i++)
            igTableSetupColumn(cols[i], 0, 0.0f, 0);
        igTableHeadersRow();
        for (i = 0; i < st->aircraft; i++) {
            const struct m2sdr_adsb_aircraft *ac = order[i];

            igTableNextRow(0, 0.0f);
            igTableNextColumn();
            igText("%06X", (unsigned)ac->icao);
            igTableNextColumn();
            igTextUnformatted(ac->callsign, NULL);
            igTableNextColumn();
            if (ac->has_altitude)
                igText("%d", ac->altitude_ft);
            igTableNextColumn();
            if (ac->has_velocity)
                igText("%.0f", ac->speed_kt);
            igTableNextColumn();
            if (ac->has_velocity)
                igText("%.0f", ac->track_deg);
            igTableNextColumn();
            if (ac->has_velocity)
                igText("%+d", ac->vrate_fpm);
            igTableNextColumn();
            if (ac->has_position)
                igText("%.5f", ac->lat_deg);
            igTableNextColumn();
            if (ac->has_position)
                igText("%.5f", ac->lon_deg);
            igTableNextColumn();
            igText("%.0f s", now - ac->last_s > 0.0 ? now - ac->last_s : 0.0);
            igTableNextColumn();
            igText("%d", ac->messages);
            igTableNextColumn();
            igText("%.1f", ac->level_dbfs + res->set.ref_offset_db);
        }
        igEndTable();
    }
}

static void draw_adsb_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_adsb_result *a = &RES(res)->adsb;
    int i, pos = 0;

    igSeparatorText("ADS-B");
    if (!RES(res)->adsb_valid) {
        igTextUnformatted("No analysis.", NULL);
        return;
    }
    for (i = 0; i < RES(res)->adsb_state.aircraft; i++)
        pos += RES(res)->adsb_state.ac[i].has_position;
    if (igBeginTable("##adsb", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Messages", "%d (%d corrected)", a->messages, a->corrected);
        label_value("Message rate", "%.1f /s", a->message_rate);
        label_value("Preambles tried", "%d", a->preambles);
        label_value("Noise", "%.1f dBFS", a->noise_dbfs);
        label_value("Aircraft", "%d, %d with a position", RES(res)->adsb_state.aircraft, pos);
        if (a->nmessages > 0)
            label_value("Last message", "%06X, %.1f dBFS, SNR %.1f dB", (unsigned)a->message[a->nmessages - 1].icao,
                        a->message[a->nmessages - 1].level_dbfs, a->message[a->nmessages - 1].snr_db);
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_adsb = {
    .key          = "adsb",
    .title        = "ADS-B",
    .category     = AN_CAT_MONITORING,
    .on           = offsetof(struct an_settings, adsb_on),
    .result_size  = sizeof(struct adsb_res),
    .valid        = offsetof(struct adsb_res, adsb_valid),
    .report_rank  = 41,
    .default_rate = AN_ADSB_SAMPLE_RATE,
    .default_freq = 1090e6,
    .psd_short    = true,
    .samples      = adsb_samples,
    .analyse      = adsb_analyse,
    .report_text  = adsb_report_text,
    .report_json  = adsb_report_json,
    .watch_line   = adsb_watch_line,
    .options      = adsb_options,
    .option       = adsb_option,
    .help         = adsb_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_adsb,
    .results      = draw_adsb_results,
    .keep         = adsb_keep,
#endif
};
