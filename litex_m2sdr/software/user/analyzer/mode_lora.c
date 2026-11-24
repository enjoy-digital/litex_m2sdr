/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: LoRa packets (chirp spread spectrum, 868 MHz band or any): SF,
 * bandwidth, offset, level, SNR, symbols and the airtime of the channel, in a report, a view and
 * --watch.
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

#define LORA_HISTORY       300       /* Results of the busy trace. */
#define LORA_LIST          100       /* Packets of the list of the view. */

/* Analysis */
/*----------*/

static size_t lora_samples(const struct an_settings *set)
{
    return (size_t)(AN_LORA_SECONDS * set->sample_rate);
}

static void lora_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = lora_samples(set);

    res->lora_valid = m2sdr_lora_analyse(x, count < n ? count : n, set->sample_rate, NULL, &res->lora) == 0;
}

static const char *sync_text(const struct m2sdr_lora_packet *p, char *text, size_t len)
{
    if (!p->sync)
        snprintf(text, len, "-");
    else if (p->sync_word >= 0)
        snprintf(text, len, "0x%02X%s", p->sync_word, p->sync_word == 0x34 ? " LoRaWAN" : p->sync_word == 0x12 ? " private" : "");
    else
        snprintf(text, len, "%d,%d", p->sync_symbols[0], p->sync_symbols[1]);
    return text;
}

/* Reports */
/*---------*/

static void lora_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_lora_result *l = &res->lora;
    char sw[32];
    int i;

    printf("\nLoRa:\n");
    if (!res->lora_valid) {
        printf("  The sample rate must be 125, 250 or 500 kHz times a power of two (1 MSPS by default).\n");
        return;
    }
    printf("  Packets    : %d, channel busy %.1f %% of the capture\n", l->packets, 100.0 * l->busy_fraction);
    if (l->npackets == 0)
        return;
    printf("  %9s %4s %5s %10s %8s %7s %-14s %4s %7s %9s\n", "time ms", "SF", "kHz", "offset Hz", rep->unit, "SNR",
           "sync word", "pre", "symbols", "airtime");
    for (i = 0; i < l->npackets; i++) {
        const struct m2sdr_lora_packet *p = &l->packet[i];

        printf("  %9.2f %4d %5.0f %+10.1f %8.1f %7.1f %-14s %4d %7d %7.1f ms%s\n", p->time_s * 1e3, p->sf, p->bw_hz / 1e3,
               p->freq_error_hz, p->rssi_dbfs + rep->off, p->snr_db, sync_text(p, sw, sizeof(sw)), p->preamble_symbols,
               p->symbols, p->airtime_s * 1e3, p->complete ? "" : " (cut)");
    }
}

static void lora_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_lora_result *l = &res->lora;
    int i, k;

    printf("  \"lora\": {\n");
    printf("    \"valid\": %s%s\n", res->lora_valid ? "true" : "false", res->lora_valid ? "," : "");
    if (res->lora_valid) {
        printf("    \"packets\": %d,\n", l->packets);
        print_json_number("busy_fraction", l->busy_fraction, false);
        printf("    \"packet_list\": [");
        for (i = 0; i < l->npackets; i++) {
            const struct m2sdr_lora_packet *p = &l->packet[i];

            printf("%s\n      {\"time_ms\": %.3f, \"sf\": %d, \"bw_hz\": %.0f, \"carrier_offset_hz\": %.1f, "
                   "\"rssi_db\": %.2f, \"snr_db\": %.2f, \"preamble_symbols\": %d, \"sync\": %s, \"sync_word\": %d, "
                   "\"symbols\": %d, \"complete\": %s, \"airtime_ms\": %.3f, \"symbol\": [", i ? "," : "", p->time_s * 1e3,
                   p->sf, p->bw_hz, p->freq_error_hz, p->rssi_dbfs + rep->off, p->snr_db, p->preamble_symbols,
                   p->sync ? "true" : "false", p->sync_word, p->symbols, p->complete ? "true" : "false", p->airtime_s * 1e3);
            for (k = 0; k < p->symbols && k < M2SDR_LORA_MAX_SYMBOLS; k++)
                printf("%s%u", k ? ", " : "", p->symbol[k]);
            printf("]}");
        }
        printf("%s]\n", l->npackets ? "\n    " : "");
    }
    printf("  }");
}

/* Watch */
/*-------*/

/* --watch: one line per packet. */
static unsigned lora_watch_lines;

static int lora_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_lora_result *l = &res->lora;
    char sw[32];
    int i;

    if (!res->lora_valid) {
        fprintf(stderr, "The sample rate must be 125, 250 or 500 kHz times a power of two for LoRa.\n");
        return -1;
    }
    for (i = 0; i < l->npackets; i++) {
        const struct m2sdr_lora_packet *p = &l->packet[i];

        if (json) {
            printf("{\"time_s\": %.3f, \"sf\": %d, \"bw_hz\": %.0f, \"carrier_offset_hz\": %.1f, \"rssi_db\": %.2f, "
                   "\"snr_db\": %.2f, \"sync_word\": %d, \"symbols\": %d, \"airtime_ms\": %.3f}\n", now + p->time_s, p->sf,
                   p->bw_hz, p->freq_error_hz, p->rssi_dbfs + set->ref_offset_db, p->snr_db, p->sync_word, p->symbols,
                   p->airtime_s * 1e3);
            continue;
        }
        if (lora_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %4s %5s %10s %8s %7s %-14s %7s %9s\n", "time", "SF", "kHz", "offset Hz",
                   set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "SNR", "sync word", "symbols", "airtime");
        printf("%8.2f %4d %5.0f %+10.1f %8.1f %7.1f %-14s %7d %6.1f ms\n", now + p->time_s, p->sf, p->bw_hz / 1e3,
               p->freq_error_hz, p->rssi_dbfs + set->ref_offset_db, p->snr_db, sync_text(p, sw, sizeof(sw)), p->symbols,
               p->airtime_s * 1e3);
    }
    return 0;
}

/* Command line */
/*--------------*/

static const struct option lora_options[] = {
    { "lora", no_argument, NULL, 0 },
    { NULL, 0, NULL, 0 }
};

static int lora_option(int id, const char *arg, struct an_cli *cli)
{
    (void)id;
    (void)arg;
    cli->set->lora_on = true;
    return 0;
}

static void lora_help(void)
{
    printf("LoRa options:\n"
           "      --lora             With --report: LoRa packets, SF 7 to 12 at 125, 250 and 500 kHz:\n"
           "                         offset, level, SNR, sync word, symbols, airtime. Default 1 MSPS\n"
           "                         at 868.1 MHz. --watch: a line per packet.\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

/* Packets over the results, the busy trace and the last packet with symbols. UI only. */
struct lora_seen {
    struct m2sdr_lora_packet p;
    double at_s;               /* Clock time of its start. */
};

static struct lora_seen lora_list[LORA_LIST];
static int lora_nlist;
static float lora_busy[LORA_HISTORY];
static int lora_nbusy;
static struct m2sdr_lora_packet lora_last;
static bool lora_have_last;

static void lora_keep(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_lora_result *l = &res->lora;
    const double now = now_ms() / 1e3, span = AN_LORA_SECONDS;
    int i;

    if (!res->set.lora_on || !res->lora_valid)
        return;
    for (i = 0; i < l->npackets; i++) {
        if (lora_nlist == LORA_LIST) {
            memmove(lora_list, lora_list + 1, (LORA_LIST - 1) * sizeof(lora_list[0]));
            lora_nlist--;
        }
        lora_list[lora_nlist].p    = l->packet[i];
        lora_list[lora_nlist].at_s = now - span + l->packet[i].time_s;
        lora_nlist++;
        if (l->packet[i].symbols > 0) {
            lora_last      = l->packet[i];
            lora_have_last = true;
        }
    }
    if (lora_nbusy == LORA_HISTORY) {
        memmove(lora_busy, lora_busy + 1, (LORA_HISTORY - 1) * sizeof(lora_busy[0]));
        lora_nbusy--;
    }
    lora_busy[lora_nbusy++] = (float)(100.0 * l->busy_fraction);
}

static void draw_lora(struct an_ui *ui, bool live, bool *changed)
{
    static const double channels[3] = { 868.1e6, 868.3e6, 868.5e6 };
    static float sx[M2SDR_LORA_MAX_SYMBOLS], sy[M2SDR_LORA_MAX_SYMBOLS];
    const struct an_result *res = &ui->res;
    const double now = now_ms() / 1e3;
    struct m2sdr_plot plot;
    ImVec2 avail;
    char text[64];
    int i;

    if (live) {
        igTextUnformatted("Channel:", NULL);
        for (i = 0; i < 3; i++) {
            igSameLine(0.0f, 8.0f);
            snprintf(text, sizeof(text), "%s%.1f MHz", fabs(ui->set.freq_hz - channels[i]) < 1.0 ? "> " : "",
                     channels[i] / 1e6);
            if (igSmallButton(text))
                view_retune(ui, channels[i], changed);
        }
        if (fabs(ui->set.sample_rate - AN_LORA_SAMPLE_RATE) > 1.0) {
            igSameLine(0.0f, 16.0f);
            if (igSmallButton("1 MSPS"))
                view_rate(ui, AN_LORA_SAMPLE_RATE, changed);
        }
    }
    if (!res->set.lora_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->lora_valid) {
        igTextUnformatted("The sample rate must be 125, 250 or 500 kHz times a power of two (1 MSPS takes all three).", NULL);
        return;
    }
    igGetContentRegionAvail(&avail);
    {
        float top = 10.0f;

        for (i = 0; i < lora_nbusy; i++)
            top = lora_busy[i] > top ? lora_busy[i] : top;
        if (m2sdr_plot_begin(&plot, "##lorabusy", (ImVec2){ 0.5f * avail.x - 4.0f, 0.4f * avail.y },
                             -(double)(LORA_HISTORY - 1), 0.0, M2SDR_PLOT_PLAIN, 0.0, top > 80.0f ? 100.0 : 1.25 * top,
                             "Channel busy (%), one point per capture")) {
            if (lora_nbusy > 0)
                m2sdr_plot_trace(&plot, lora_busy, (size_t)lora_nbusy, -(double)(lora_nbusy - 1), 0.0, 0xffffb040u);
            m2sdr_plot_end(&plot);
        }
    }
    igSameLine(0.0f, 8.0f);
    {
        const int n = lora_have_last ? (lora_last.symbols < M2SDR_LORA_MAX_SYMBOLS ? lora_last.symbols : M2SDR_LORA_MAX_SYMBOLS) : 0;
        const double top = lora_have_last ? (double)(1 << lora_last.sf) : 128.0;

        if (lora_have_last)
            snprintf(text, sizeof(text), "Last packet: symbols (FFT peak), SF%d %.0f kHz", lora_last.sf, lora_last.bw_hz / 1e3);
        else
            snprintf(text, sizeof(text), "Last packet: symbols");
        if (m2sdr_plot_begin(&plot, "##lorasym", (ImVec2){ 0.0f, 0.4f * avail.y }, 0.0, n > 1 ? n - 1 : 1, M2SDR_PLOT_PLAIN,
                             0.0, top, text)) {
            for (i = 0; i < n; i++) {
                sx[i] = (float)i;
                sy[i] = (float)lora_last.symbol[i];
            }
            m2sdr_plot_points(&plot, sx, sy, (size_t)n, 2.0f, 0xff40c0ffu);
            m2sdr_plot_end(&plot);
        }
    }
    if (igBeginTable("##lorapk", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                     ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        static const char *const cols[10] = { "Seen", "SF", "BW kHz", "Offset Hz", "RSSI dBFS", "SNR dB", "Sync word",
                                              "Preamble", "Symbols", "Airtime ms" };

        igTableSetupScrollFreeze(0, 1);
        for (i = 0; i < 10; i++)
            igTableSetupColumn(cols[i], 0, 0.0f, 0);
        igTableHeadersRow();
        for (i = lora_nlist - 1; i >= 0; i--) {
            const struct m2sdr_lora_packet *p = &lora_list[i].p;

            igTableNextRow(0, 0.0f);
            igTableNextColumn();
            igText("%.1f s ago", now - lora_list[i].at_s > 0.0 ? now - lora_list[i].at_s : 0.0);
            igTableNextColumn();
            igText("%d", p->sf);
            igTableNextColumn();
            igText("%.0f", p->bw_hz / 1e3);
            igTableNextColumn();
            igText("%+.0f", p->freq_error_hz);
            igTableNextColumn();
            igText("%.1f", p->rssi_dbfs);
            igTableNextColumn();
            igText("%.1f", p->snr_db);
            igTableNextColumn();
            igTextUnformatted(sync_text(p, text, sizeof(text)), NULL);
            igTableNextColumn();
            igText("%d", p->preamble_symbols);
            igTableNextColumn();
            igText("%d%s", p->symbols, p->complete ? "" : " (cut)");
            igTableNextColumn();
            igText("%.1f", p->airtime_s * 1e3);
        }
        igEndTable();
    }
}

static void draw_lora_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_lora_result *l = &res->lora;

    igSeparatorText("LoRa");
    if (!res->lora_valid) {
        igTextUnformatted("No analysis.", NULL);
        return;
    }
    if (igBeginTable("##lora", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Packets", "%d in the capture, %d listed", l->packets, lora_nlist);
        label_value("Channel busy", "%.1f %%", 100.0 * l->busy_fraction);
        if (l->npackets > 0) {
            const struct m2sdr_lora_packet *p = &l->packet[l->npackets - 1];

            label_value("Last", "SF%d, %.0f kHz", p->sf, p->bw_hz / 1e3);
            label_value("Its level", "%.1f dBFS, SNR %.1f dB", p->rssi_dbfs, p->snr_db);
            label_value("Its offset", "%+.0f Hz", p->freq_error_hz);
        }
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_lora = {
    .key          = "lora",
    .title        = "LoRa",
    .category     = AN_CAT_MONITORING,
    .on           = offsetof(struct an_settings, lora_on),
    .valid        = offsetof(struct an_result, lora_valid),
    .report_rank  = 42,
    .default_rate = AN_LORA_SAMPLE_RATE,
    .default_freq = 868.1e6,
    .psd_short    = true,
    .samples      = lora_samples,
    .analyse      = lora_analyse,
    .report_text  = lora_report_text,
    .report_json  = lora_report_json,
    .watch_line   = lora_watch_line,
    .options      = lora_options,
    .option       = lora_option,
    .help         = lora_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_lora,
    .results      = draw_lora_results,
    .keep         = lora_keep,
#endif
};
