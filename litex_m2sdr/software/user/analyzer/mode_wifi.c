/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: a Wi-Fi channel (802.11 OFDM, 20 MHz): its networks, packets,
 * levels, EVM and airtime, in a report, a view and --watch.
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

#define WIFI_HISTORY       300       /* Results of the airtime trace. */
#define WIFI_SKIP_FULL     10        /* Captures without the data after one too slow to decode. */
#define WIFI_REPORT_PACKETS 32       /* Packets listed in the reports. */

/* Result */
/*--------*/

/* The result of the mode: its block of struct an_result, read with RES(). */
struct wifi_res {
    bool   wifi_valid;             /* Measured: the sample rate was 20 MSPS. */
    struct m2sdr_wifi_result wifi;
    bool   wifi_no_psdu;           /* The data was not decoded: it took longer than the capture. */
};

extern const struct an_mode an_mode_wifi;
#define RES(res) ((struct wifi_res *)an_mode_result(&an_mode_wifi, (res)))

/* Analysis */
/*----------*/

static size_t wifi_samples(const struct an_settings *set)
{
    return (size_t)(AN_WIFI_SECONDS * set->sample_rate);
}

/* A capture full of long packets takes longer to decode than it lasts: the next ones are then
 * read without their data (L-SIG and HT-SIG only), a full one now and then. Worker only. */
static int wifi_skip;

static void wifi_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = wifi_samples(set);
    struct m2sdr_wifi_config cfg = { .no_psdu = wifi_skip > 0 };
    const double t0 = now_ms();

    if (count > n)
        count = n;
    RES(res)->wifi_valid   = m2sdr_wifi_analyse(x, count, set->sample_rate, &cfg, &RES(res)->wifi) == 0;
    RES(res)->wifi_no_psdu = cfg.no_psdu;
    if (wifi_skip > 0)
        wifi_skip--;
    else if (now_ms() - t0 > 1e3 * (double)count / set->sample_rate)
        wifi_skip = WIFI_SKIP_FULL;
}

static const char *wifi_format(const struct m2sdr_wifi_packet *p, char *text, size_t len)
{
    if (!p->signal_ok)
        snprintf(text, len, "L-SIG wrong");
    else if (p->format == M2SDR_WIFI_HT)
        snprintf(text, len, "HT MCS%d", p->ht_mcs);
    else if (p->format == M2SDR_WIFI_VHT)
        snprintf(text, len, "VHT+");
    else
        snprintf(text, len, "%d Mbps", p->rate_mbps);
    return text;
}

static const char *wifi_kind(const struct m2sdr_wifi_packet *p)
{
    static const char *const mgmt[16] = { "assoc req", "assoc resp", "reassoc req", "reassoc resp", "probe req",
                                          "probe resp", "mgmt", "mgmt", "beacon", "ATIM", "disassoc", "auth", "deauth",
                                          "action", "action", "mgmt" };
    static const char *const ctrl[16] = { "ctrl", "ctrl", "ctrl", "ctrl", "ctrl", "ctrl", "ctrl", "ctrl", "BAR", "BA",
                                          "PS-poll", "RTS", "CTS", "ACK", "CF-end", "CF-end" };

    if (!p->fcs_ok)
        return "";
    return p->type == 0 ? mgmt[p->subtype & 15] : p->type == 1 ? ctrl[p->subtype & 15] : p->type == 2 ? "data" : "ext";
}

static void mac_text(const uint8_t *a, char *text, size_t len)
{
    snprintf(text, len, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* Reports */
/*---------*/

static void wifi_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_wifi_result *w = &RES(res)->wifi;
    char fmt[32], mac[24];
    int i;

    printf("\nWi-Fi:\n");
    if (!RES(res)->wifi_valid) {
        printf("  The sample rate must be 20 MSPS.\n");
        return;
    }
    printf("  Packets    : %d, %d with a right L-SIG, %d FCS right%s\n", w->packets, w->signal_ok, w->fcs_ok,
           RES(res)->wifi_no_psdu ? " (data not decoded: it took longer than the capture)" : "");
    printf("  Airtime    : %.1f %% from the packets, %.1f %% of 4 us slots 6 dB over the floor\n", 100.0 * w->busy_fraction,
           100.0 * w->energy_busy_fraction);
    printf("  Noise      : %.1f %s in 20 MHz\n", w->noise_dbfs + rep->off, rep->unit);
    for (i = 0; i < w->nbss; i++) {
        mac_text(w->bss[i].bssid, mac, sizeof(mac));
        printf("  Network    : %s \"%s\", %d beacon%s, %.1f %s, SNR %.1f dB\n", mac, w->bss[i].ssid, w->bss[i].beacons,
               w->bss[i].beacons == 1 ? "" : "s", w->bss[i].rssi_dbfs + rep->off, rep->unit, w->bss[i].snr_db);
    }
    if (w->npackets == 0)
        return;
    printf("  %10s %-11s %5s %8s %6s %6s %8s %-10s %-17s\n", "time us", "rate", "bytes", rep->unit, "SNR", "EVM", "kHz",
           "frame", "transmitter");
    for (i = 0; i < w->npackets && i < WIFI_REPORT_PACKETS; i++) {
        const struct m2sdr_wifi_packet *p = &w->packet[i];

        mac[0] = '\0';
        if (p->fcs_ok && p->naddr >= 2)
            mac_text(p->addr2, mac, sizeof(mac));
        printf("  %10.1f %-11s %5d %8.1f %6.1f %6.1f %+8.1f %-10s %-17s\n", p->time_s * 1e6, wifi_format(p, fmt, sizeof(fmt)),
               p->format == M2SDR_WIFI_HT ? p->ht_length : p->length, p->rssi_dbfs + rep->off, p->snr_db, p->evm_db,
               p->freq_error_hz / 1e3, wifi_kind(p), mac);
    }
    if (w->npackets > WIFI_REPORT_PACKETS)
        printf("  ... %d more\n", w->npackets - WIFI_REPORT_PACKETS);
}

static void wifi_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_wifi_result *w = &RES(res)->wifi;
    char mac[24], fmt[32];
    int i;

    printf("  \"wifi\": {\n");
    printf("    \"valid\": %s%s\n", RES(res)->wifi_valid ? "true" : "false", RES(res)->wifi_valid ? "," : "");
    if (RES(res)->wifi_valid) {
        printf("    \"packets\": %d,\n", w->packets);
        printf("    \"signal_ok\": %d,\n", w->signal_ok);
        printf("    \"fcs_ok\": %d,\n", w->fcs_ok);
        printf("    \"data_decoded\": %s,\n", RES(res)->wifi_no_psdu ? "false" : "true");
        print_json_number("busy_fraction", w->busy_fraction, false);
        print_json_number("energy_busy_fraction", w->energy_busy_fraction, false);
        print_json_number("noise_db", w->noise_dbfs + rep->off, false);
        printf("    \"networks\": [");
        for (i = 0; i < w->nbss; i++) {
            mac_text(w->bss[i].bssid, mac, sizeof(mac));
            printf("%s\n      {\"bssid\": \"%s\", \"ssid\": \"%s\", \"beacons\": %d, \"rssi_db\": %.2f, \"snr_db\": %.2f}",
                   i ? "," : "", mac, w->bss[i].ssid, w->bss[i].beacons, w->bss[i].rssi_dbfs + rep->off, w->bss[i].snr_db);
        }
        printf("%s],\n", w->nbss ? "\n    " : "");
        printf("    \"packet_list\": [");
        for (i = 0; i < w->npackets && i < WIFI_REPORT_PACKETS; i++) {
            const struct m2sdr_wifi_packet *p = &w->packet[i];

            printf("%s\n      {\"time_us\": %.2f, \"format\": \"%s\", \"length\": %d, \"rssi_db\": %.2f, \"snr_db\": %.2f, "
                   "\"evm_db\": %.2f, \"carrier_offset_hz\": %.1f, \"duration_us\": %.1f, \"fcs_ok\": %s",
                   i ? "," : "", p->time_s * 1e6, wifi_format(p, fmt, sizeof(fmt)),
                   p->format == M2SDR_WIFI_HT ? p->ht_length : p->length, p->rssi_dbfs + rep->off, p->snr_db, p->evm_db,
                   p->freq_error_hz, p->duration_s * 1e6, p->fcs_ok ? "true" : "false");
            if (p->fcs_ok)
                printf(", \"frame\": \"%s\"", wifi_kind(p));
            printf("}");
        }
        printf("%s]\n", w->npackets ? "\n    " : "");
    }
    printf("  }");
}

/* Watch */
/*-------*/

/* --watch: one line per capture. */
static unsigned wifi_watch_lines;

static int wifi_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_wifi_result *w = &RES(res)->wifi;
    const struct m2sdr_wifi_packet *best = NULL;
    int i, beacons = 0;

    if (!RES(res)->wifi_valid) {
        fprintf(stderr, "The sample rate must be 20 MSPS for Wi-Fi.\n");
        return -1;
    }
    for (i = 0; i < w->npackets; i++) {
        const struct m2sdr_wifi_packet *p = &w->packet[i];

        beacons += p->fcs_ok && p->type == 0 && p->subtype == 8;
        if (p->signal_ok && (!best || p->rssi_dbfs > best->rssi_dbfs))
            best = p;
    }
    if (json) {
        printf("{\"time_s\": %.3f, \"packets\": %d, \"signal_ok\": %d, \"fcs_ok\": %d, \"beacons\": %d, "
               "\"busy_fraction\": %.4f, \"energy_busy_fraction\": %.4f, \"noise_db\": %.2f", now, w->packets, w->signal_ok,
               w->fcs_ok, beacons, w->busy_fraction, w->energy_busy_fraction, w->noise_dbfs + set->ref_offset_db);
        if (best)
            printf(", \"strongest\": {\"rssi_db\": %.2f, \"snr_db\": %.2f, \"evm_db\": %.2f, \"carrier_offset_hz\": %.1f}",
                   best->rssi_dbfs + set->ref_offset_db, best->snr_db, best->evm_db, best->freq_error_hz);
        printf("}\n");
        return 0;
    }
    if (wifi_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
        printf("%8s %7s %5s %5s %7s %7s %7s %8s %7s %7s %9s\n%8s %7s %5s %5s %7s %7s %7s %8s %7s %7s %9s\n",
               "time", "packets", "FCS", "bcn", "airtime", "energy", "noise", "best", "SNR", "EVM", "offset",
               "s", "", "", "", "%", "%", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", set->ref_offset_db != 0.0 ? "dBm" : "dBFS",
               "dB", "dB", "kHz");
    printf("%8.2f %7d %5d %5d %7.1f %7.1f %7.1f", now, w->packets, w->fcs_ok, beacons, 100.0 * w->busy_fraction,
           100.0 * w->energy_busy_fraction, w->noise_dbfs + set->ref_offset_db);
    if (best)
        printf(" %8.1f %7.1f %7.1f %+9.1f", best->rssi_dbfs + set->ref_offset_db, best->snr_db, best->evm_db,
               best->freq_error_hz / 1e3);
    printf("\n");
    return 0;
}

/* Command line */
/*--------------*/

static const struct option wifi_options[] = {
    { "wifi", no_argument, NULL, 0 },
    { NULL, 0, NULL, 0 }
};

static int wifi_option(int id, const char *arg, struct an_cli *cli)
{
    (void)id;
    (void)arg;
    cli->set->wifi_on = true;
    return 0;
}

static void wifi_help(void)
{
    printf("Wi-Fi options:\n"
           "      --wifi             With --report: a Wi-Fi channel (802.11 OFDM, 20 MHz): networks,\n"
           "                         packets (rate, level, SNR, EVM, MAC header of legacy frames) and\n"
           "                         airtime. The sample rate is 20 MSPS (the default with --wifi,\n"
           "                         at 2412 MHz). --watch works too: a line per capture.\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

/* Channels: 2.4 GHz 1 to 13, and the 20 MHz channels of 5 GHz most used. */
static const int wifi_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
};

static double wifi_channel_hz(int ch)
{
    return ch <= 14 ? 2407e6 + 5e6 * ch : 5000e6 + 5e6 * ch;
}

/* Networks over the captures of a tuning, and the airtime over the results. UI only. */
struct wifi_net {
    struct m2sdr_wifi_bss bss;
    double last_s;
};

static struct wifi_net wifi_nets[M2SDR_WIFI_MAX_BSS];
static int wifi_nnets;
static double wifi_nets_freq;
static float wifi_busy[WIFI_HISTORY], wifi_energy[WIFI_HISTORY];
static int wifi_nbusy;

static void wifi_keep(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_wifi_result *w = &RES(res)->wifi;
    int i, b;

    if (!res->set.wifi_on || !RES(res)->wifi_valid)
        return;
    if (res->set.freq_hz != wifi_nets_freq) {
        wifi_nets_freq = res->set.freq_hz;
        wifi_nnets     = 0;
        wifi_nbusy     = 0;
    }
    for (i = 0; i < w->nbss; i++) {
        for (b = 0; b < wifi_nnets && memcmp(wifi_nets[b].bss.bssid, w->bss[i].bssid, 6) != 0; b++)
            ;
        if (b == wifi_nnets) {
            if (wifi_nnets == M2SDR_WIFI_MAX_BSS)
                continue;
            memset(&wifi_nets[b], 0, sizeof(wifi_nets[b]));
            wifi_nnets++;
        }
        {
            const int total = wifi_nets[b].bss.beacons + w->bss[i].beacons;

            wifi_nets[b].bss         = w->bss[i];
            wifi_nets[b].bss.beacons = total;
            wifi_nets[b].last_s      = now_ms() / 1e3;
        }
    }
    if (wifi_nbusy == WIFI_HISTORY) {
        memmove(wifi_busy, wifi_busy + 1, (WIFI_HISTORY - 1) * sizeof(wifi_busy[0]));
        memmove(wifi_energy, wifi_energy + 1, (WIFI_HISTORY - 1) * sizeof(wifi_energy[0]));
        wifi_nbusy--;
    }
    wifi_busy[wifi_nbusy]   = (float)(100.0 * w->busy_fraction);
    wifi_energy[wifi_nbusy] = (float)(100.0 * w->energy_busy_fraction);
    wifi_nbusy++;
}

static void draw_wifi_channel(struct an_ui *ui, bool *changed)
{
    char text[64];
    int i, sel = -1;

    for (i = 0; i < (int)(sizeof(wifi_channels) / sizeof(wifi_channels[0])); i++)
        if (fabs(wifi_channel_hz(wifi_channels[i]) - ui->set.freq_hz) < 1e3)
            sel = i;
    igTextUnformatted("Channel:", NULL);
    igSameLine(0.0f, 8.0f);
    igSetNextItemWidth(170.0f);
    if (sel >= 0)
        snprintf(text, sizeof(text), "%d  %.0f MHz", wifi_channels[sel], wifi_channel_hz(wifi_channels[sel]) / 1e6);
    else
        snprintf(text, sizeof(text), "%.3f MHz", ui->set.freq_hz / 1e6);
    if (igBeginCombo("##wifich", text, 0)) {
        for (i = 0; i < (int)(sizeof(wifi_channels) / sizeof(wifi_channels[0])); i++) {
            char item[32];

            snprintf(item, sizeof(item), "%d  %.0f MHz", wifi_channels[i], wifi_channel_hz(wifi_channels[i]) / 1e6);
            if (igSelectable_Bool(item, i == sel, 0, (ImVec2){ 0.0f, 0.0f }))
                view_retune(ui, wifi_channel_hz(wifi_channels[i]), changed);
        }
        igEndCombo();
    }
    if (fabs(ui->set.sample_rate - M2SDR_WIFI_SAMPLE_RATE) > 1.0) {
        igSameLine(0.0f, 16.0f);
        if (igSmallButton("20 MSPS"))
            view_rate(ui, M2SDR_WIFI_SAMPLE_RATE, changed);
    }
}

static void draw_wifi_tables(const struct an_result *res, float height)
{
    const struct m2sdr_wifi_result *w = &RES(res)->wifi;
    const double now = now_ms() / 1e3;
    char text[32];
    int i;

    if (igBeginTable("##wifinets", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                     ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.35f * height }, 0.0f)) {
        static const char *const cols[6] = { "BSSID", "SSID", "Beacons", "RSSI dBFS", "SNR dB", "Last seen" };

        igTableSetupScrollFreeze(0, 1);
        for (i = 0; i < 6; i++)
            igTableSetupColumn(cols[i], 0, 0.0f, 0);
        igTableHeadersRow();
        for (i = 0; i < wifi_nnets; i++) {
            const struct wifi_net *nt = &wifi_nets[i];

            igTableNextRow(0, 0.0f);
            igTableNextColumn();
            mac_text(nt->bss.bssid, text, sizeof(text));
            igTextUnformatted(text, NULL);
            igTableNextColumn();
            igTextUnformatted(nt->bss.ssid[0] ? nt->bss.ssid : "(hidden)", NULL);
            igTableNextColumn();
            igText("%d", nt->bss.beacons);
            igTableNextColumn();
            igText("%.1f", nt->bss.rssi_dbfs);
            igTableNextColumn();
            igText("%.1f", nt->bss.snr_db);
            igTableNextColumn();
            igText("%.0f s", now - nt->last_s > 0.0 ? now - nt->last_s : 0.0);
        }
        igEndTable();
    }
    if (igBeginTable("##wifipk", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                     ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.65f * height - 8.0f }, 0.0f)) {
        static const char *const cols[9] = { "Time us", "Rate", "Bytes", "RSSI", "SNR", "EVM", "Offset kHz", "Frame",
                                             "Transmitter" };

        igTableSetupScrollFreeze(0, 1);
        for (i = 0; i < 9; i++)
            igTableSetupColumn(cols[i], 0, 0.0f, 0);
        igTableHeadersRow();
        for (i = 0; i < w->npackets; i++) {
            const struct m2sdr_wifi_packet *p = &w->packet[i];

            igTableNextRow(0, 0.0f);
            igTableNextColumn();
            igText("%.1f", p->time_s * 1e6);
            igTableNextColumn();
            igTextUnformatted(wifi_format(p, text, sizeof(text)), NULL);
            igTableNextColumn();
            igText("%d", p->format == M2SDR_WIFI_HT ? p->ht_length : p->length);
            igTableNextColumn();
            igText("%.1f", p->rssi_dbfs);
            igTableNextColumn();
            igText("%.1f", p->snr_db);
            igTableNextColumn();
            igText("%.1f", p->evm_db);
            igTableNextColumn();
            igText("%+.1f", p->freq_error_hz / 1e3);
            igTableNextColumn();
            igTextUnformatted(p->fcs_ok ? wifi_kind(p) : p->decoded ? "FCS wrong" : "", NULL);
            igTableNextColumn();
            if (p->fcs_ok && p->naddr >= 2) {
                mac_text(p->addr2, text, sizeof(text));
                igTextUnformatted(text, NULL);
            }
        }
        igEndTable();
    }
}

static void draw_wifi(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_wifi_result *w = &RES(res)->wifi;
    struct m2sdr_plot plot;
    ImVec2 avail, sq;
    float side;

    if (live)
        draw_wifi_channel(ui, changed);
    if (!res->set.wifi_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!RES(res)->wifi_valid) {
        igTextUnformatted("The sample rate must be 20 MSPS.", NULL);
        return;
    }
    if (RES(res)->wifi_no_psdu)
        igTextColored((ImVec4){ 1.0f, 0.8f, 0.3f, 1.0f },
                      "Data not decoded: the last full analysis took longer than the capture; one in %d is full.",
                      WIFI_SKIP_FULL + 1);
    igGetContentRegionAvail(&avail);
    side = 0.42f * avail.y < 0.32f * avail.x ? 0.42f * avail.y : 0.32f * avail.x;
    sq   = m2sdr_plot_square(side);
    /* Airtime trend, then the traces and points of the strongest packet. */
    igBeginGroup();
    legend_item(0xffffb040u, "airtime of the packets (%)", avail.x - 2.0f * sq.x - 16.0f);
    legend_item(0xff40a0ffu, "energy (%)", avail.x - 2.0f * sq.x - 16.0f);
    igNewLine();
    if (m2sdr_plot_begin(&plot, "##wifibusy", (ImVec2){ avail.x - 2.0f * sq.x - 16.0f, sq.y - igGetFrameHeightWithSpacing() },
                         -(double)(WIFI_HISTORY - 1), 0.0, M2SDR_PLOT_PLAIN, 0.0, 100.0, "")) {
        if (wifi_nbusy > 0) {
            m2sdr_plot_trace(&plot, wifi_energy, (size_t)wifi_nbusy, -(double)(wifi_nbusy - 1), 0.0, 0xff40a0ffu);
            m2sdr_plot_trace(&plot, wifi_busy, (size_t)wifi_nbusy, -(double)(wifi_nbusy - 1), 0.0, 0xffffb040u);
        }
        m2sdr_plot_end(&plot);
    }
    igEndGroup();
    igSameLine(0.0f, 8.0f);
    igBeginGroup();
    legend_item(0xffffb040u, "channel (dB)", sq.x);
    legend_item(0xff40a0ffu, "EVM (dB), strongest packet", sq.x);
    igNewLine();
    if (m2sdr_plot_begin(&plot, "##wificar", (ImVec2){ sq.x, sq.y - igGetFrameHeightWithSpacing() }, -26.0, 26.0,
                         M2SDR_PLOT_PLAIN, -40.0, 10.0, "")) {
        if (w->trace_packet >= 0 || w->npoints > 0) {
            m2sdr_plot_hline(&plot, 0.0, 0x40ffffffu);
            m2sdr_plot_trace(&plot, w->channel_db, M2SDR_WIFI_CARRIERS, -26.0, 26.0, 0xffffb040u);
            m2sdr_plot_trace(&plot, w->evm_carrier_db, M2SDR_WIFI_CARRIERS, -26.0, 26.0, 0xff40a0ffu);
        }
        m2sdr_plot_end(&plot);
    }
    igEndGroup();
    igSameLine(0.0f, 8.0f);
    draw_trend_points("##wificonst", sq, w->points, w->npoints, w->npoints ? w->points_modulation : M2SDR_MOD_BPSK);
    igGetContentRegionAvail(&avail);
    draw_wifi_tables(res, avail.y);
}

static void draw_wifi_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_wifi_result *w = &RES(res)->wifi;

    igSeparatorText("Wi-Fi");
    if (!RES(res)->wifi_valid) {
        igTextUnformatted("No analysis.", NULL);
        return;
    }
    if (igBeginTable("##wifi", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Packets", "%d", w->packets);
        label_value("L-SIG right", "%d", w->signal_ok);
        label_value("FCS right", "%d%s", w->fcs_ok, RES(res)->wifi_no_psdu ? " (data skipped)" : "");
        label_value("Airtime", "%.1f %%", 100.0 * w->busy_fraction);
        label_value("Energy airtime", "%.1f %%", 100.0 * w->energy_busy_fraction);
        label_value("Noise", "%.1f dBFS", w->noise_dbfs);
        label_value("Networks", "%d here, %d seen", w->nbss, wifi_nnets);
        if (w->trace_packet >= 0) {
            const struct m2sdr_wifi_packet *p = &w->packet[w->trace_packet];

            label_value("Strongest", "%.1f dBFS, SNR %.1f dB", p->rssi_dbfs, p->snr_db);
            label_value("Its EVM", "%.1f dB", p->evm_db);
            label_value("Its offset", "%+.1f kHz", p->freq_error_hz / 1e3);
        }
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_wifi = {
    .key          = "wifi",
    .title        = "Wi-Fi",
    .category     = AN_CAT_WIRELESS,
    .on           = offsetof(struct an_settings, wifi_on),
    .result_size  = sizeof(struct wifi_res),
    .valid        = offsetof(struct wifi_res, wifi_valid),
    .report_rank  = 40,
    .default_rate = M2SDR_WIFI_SAMPLE_RATE,
    .default_freq = 2412e6,
    .psd_short    = true,
    .samples      = wifi_samples,
    .analyse      = wifi_analyse,
    .report_text  = wifi_report_text,
    .report_json  = wifi_report_json,
    .watch_line   = wifi_watch_line,
    .options      = wifi_options,
    .option       = wifi_option,
    .help         = wifi_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_wifi,
    .results      = draw_wifi_results,
    .keep         = wifi_keep,
#endif
};
