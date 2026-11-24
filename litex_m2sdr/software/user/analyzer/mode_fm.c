/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: an FM broadcast station (deviation, MPX power per ITU-R BS.412,
 * pilot, stereo, RDS, audio SNR), in a report, a view and --watch.
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

#define FM_DEFAULT_RATE   960000.0  /* 240 kHz of multiplex after a decimation by 4. */
#define FM_BS412_WINDOW_S 60.0      /* BS.412 integrates the MPX power over 60 s. */
#define FM_HISTORY        512       /* Results kept for the 60 s mean and the trend. */

/* Analysis */
/*----------*/

/* RDS decoder kept between the captures of one tuning, so that PS and RadioText build up over
 * them; a new one for another frequency, rate or channel. The captures are analysed one at a
 * time (worker, report, --watch), never two at once. */
static struct m2sdr_fm_rds_rx *fm_rds;
static double fm_rds_freq, fm_rds_rate;
static int    fm_rds_channel;

static size_t fm_samples(const struct an_settings *set)
{
    return (size_t)(AN_FM_SECONDS * set->sample_rate);
}

static void fm_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = fm_samples(set);
    struct m2sdr_fm_analysis_config cfg;

    if (!fm_rds || set->freq_hz != fm_rds_freq || set->sample_rate != fm_rds_rate || set->channel != fm_rds_channel) {
        m2sdr_fm_rds_rx_free(fm_rds);
        fm_rds         = m2sdr_fm_rds_rx_new(m2sdr_fm_analysis_mpx_rate(set->sample_rate));
        fm_rds_freq    = set->freq_hz;
        fm_rds_rate    = set->sample_rate;
        fm_rds_channel = set->channel;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.tau     = set->fm_deemph_us < 0 ? 0.0 : (set->fm_deemph_us == 0 ? 50 : set->fm_deemph_us) * 1e-6;
    cfg.tone_hz = set->fm_tone_hz;
    cfg.rds     = fm_rds;
    res->fm_valid = m2sdr_fm_analyse(x, count < n ? count : n, set->sample_rate, &cfg, &res->fm) == 0;
}

/* Running mean of the MPX power over the last 60 s (BS.412), from the mean square of each
 * capture: kept by --watch and by the view. */
struct fm_bs412 {
    double t[FM_HISTORY];          /* Time of each result, s. */
    float  ms[FM_HISTORY];         /* Its mean square, Hz^2. */
    float  dbr[FM_HISTORY];        /* Its power, dBr, and the 60 s mean at that time. */
    float  mean_dbr[FM_HISTORY];
    int    n;
};

static double fm_bs412_add(struct fm_bs412 *b, double t, double ms)
{
    const double ref = M2SDR_FM_BS412_REF_HZ * M2SDR_FM_BS412_REF_HZ / 2.0;
    double sum = 0.0;
    int i, k = 0;

    if (b->n == FM_HISTORY) {
        memmove(b->t, b->t + 1, (FM_HISTORY - 1) * sizeof(b->t[0]));
        memmove(b->ms, b->ms + 1, (FM_HISTORY - 1) * sizeof(b->ms[0]));
        memmove(b->dbr, b->dbr + 1, (FM_HISTORY - 1) * sizeof(b->dbr[0]));
        memmove(b->mean_dbr, b->mean_dbr + 1, (FM_HISTORY - 1) * sizeof(b->mean_dbr[0]));
        b->n--;
    }
    b->t[b->n]  = t;
    b->ms[b->n] = (float)ms;
    for (i = b->n; i >= 0 && t - b->t[i] < FM_BS412_WINDOW_S; i--, k++)
        sum += b->ms[i];
    b->dbr[b->n]      = (float)(10.0 * log10(ms / ref + 1e-30));
    b->mean_dbr[b->n] = (float)(10.0 * log10(sum / k / ref + 1e-30));
    b->n++;
    return b->mean_dbr[b->n - 1];
}

#ifdef M2SDR_ANALYZER_GUI
/* Seconds covered by the 60 s mean, from the first result in it to the last. */
static double fm_bs412_span(const struct fm_bs412 *b)
{
    int i;

    if (b->n == 0)
        return 0.0;
    for (i = b->n - 1; i > 0 && b->t[b->n - 1] - b->t[i - 1] < FM_BS412_WINDOW_S; i--)
        ;
    return b->t[b->n - 1] - b->t[i];
}
#endif

/* Reports */
/*---------*/

/* RDS text in JSON: quotes, backslashes and the bytes out of printable ASCII escaped. */
static void fm_json_string(const char *name, const char *s, bool last)
{
    printf("    \"%s\": \"", name);
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;

        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c < 0x20 || c >= 0x7f)
            printf("\\u%04x", c);
        else
            putchar(c);
    }
    printf("\"%s\n", last ? "" : ",");
}

static void fm_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_fm_result *f = &res->fm;
    const struct m2sdr_fm_rds_info *r = &f->rds_info;

    printf("\nFM:\n");
    if (!res->fm_valid) {
        printf("  The sample rate must be 240 kSPS or more, with at least 50 ms of samples.\n");
        return;
    }
    if (!f->found) {
        printf("  No FM carrier (CNR %.1f dB in the channel).\n", f->cnr_db);
        return;
    }
    printf("  Carrier    : %.4f MHz (%+.1f Hz from the centre), %.1f %s, CNR %.1f dB (C/N0 %.1f dB-Hz)\n",
           (set->freq_hz + f->carrier_offset_hz) / 1e6, f->carrier_offset_hz, f->carrier_dbfs + rep->off, rep->unit,
           f->cnr_db, f->cn0_dbhz);
    printf("  Deviation  : peak %.1f kHz, exceeded by 0.1 %% %.1f kHz, by 1 %% %.1f kHz (%.2f s)\n",
           f->peak_dev_hz / 1e3, f->dev_p999_hz / 1e3, f->dev_p99_hz / 1e3, f->duration_s);
    printf("  MPX power  : %+.2f dBr over this capture (BS.412 averages it over 60 s; limit 0 dBr)\n", f->mpx_power_dbr);
    if (f->pilot)
        printf("  Pilot      : %.2f kHz deviation, %.4f Hz (%+.4f Hz)\n", f->pilot_dev_hz / 1e3, f->pilot_freq_hz,
               f->pilot_freq_error_hz);
    else
        printf("  Pilot      : none (mono)\n");
    printf("  Audio      : L+R %.1f kHz RMS, L-R %.1f kHz RMS (%+.1f dB), mono SNR %.1f dB\n", f->mono_rms_hz / 1e3,
           f->stereo_rms_hz / 1e3, f->stereo_ratio_db, f->audio_snr_db);
    if (f->tone)
        printf("  Test tone  : %.0f Hz: left %.2f kHz, right %.2f kHz, separation %.1f dB\n", set->fm_tone_hz,
               f->tone_left_hz / 1e3, f->tone_right_hz / 1e3, f->separation_db);
    if (f->rds || r->pi_valid) {
        printf("  RDS        : %.2f kHz deviation, %llu blocks, %llu errors (BLER %.2f %%), %llu groups, MER %.1f dB\n",
               f->rds_dev_hz / 1e3, (unsigned long long)f->rds_blocks, (unsigned long long)f->rds_block_errors,
               100.0 * f->rds_bler, (unsigned long long)f->rds_groups, f->rds_mer_db);
        if (r->pi_valid)
            printf("  RDS fields : PI %04X, PTY %u (%s), TP %d, TA %d, %s\n", r->pi, r->pty,
                   m2sdr_fm_rds_pty_name(r->pty), r->tp, r->ta, r->music ? "music" : "speech");
        printf("  PS         : %s\n", r->ps_valid ? r->ps : "(not complete)");
        printf("  RadioText  : %s\n", r->rt_valid ? r->rt : "(not complete)");
        {
            int t;

            printf("  Groups     :");
            for (t = 0; t < 32; t++)
                if (f->rds_type_groups[t])
                    printf(" %d%c %llu", t / 2, t & 1 ? 'B' : 'A', (unsigned long long)f->rds_type_groups[t]);
            printf("\n");
        }
    } else {
        printf("  RDS        : none\n");
    }
}

static void fm_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_fm_result *f = &res->fm;
    const struct m2sdr_fm_rds_info *r = &f->rds_info;
    const bool ok = res->fm_valid && f->found;

    printf("  \"fm\": {\n");
    print_json_number("cnr_db", res->fm_valid ? f->cnr_db : 0.0, false);
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        char pi[8];

        print_json_number("carrier_hz", set->freq_hz + f->carrier_offset_hz, false);
        print_json_number("carrier_offset_hz", f->carrier_offset_hz, false);
        print_json_number("carrier_db", f->carrier_dbfs + rep->off, false);
        print_json_number("cn0_dbhz", f->cn0_dbhz, false);
        print_json_number("duration_s", f->duration_s, false);
        print_json_number("peak_dev_hz", f->peak_dev_hz, false);
        print_json_number("dev_p999_hz", f->dev_p999_hz, false);
        print_json_number("dev_p99_hz", f->dev_p99_hz, false);
        print_json_number("mpx_power_dbr", f->mpx_power_dbr, false);
        print_json_number("mpx_mean_square_hz2", f->mpx_mean_square_hz2, false);
        printf("    \"pilot\": %s,\n", f->pilot ? "true" : "false");
        print_json_number("pilot_dev_hz", f->pilot_dev_hz, false);
        print_json_number("pilot_freq_error_hz", f->pilot_freq_error_hz, false);
        print_json_number("pilot_coherence", f->pilot_coherence, false);
        print_json_number("mono_rms_hz", f->mono_rms_hz, false);
        print_json_number("stereo_rms_hz", f->stereo_rms_hz, false);
        print_json_number("audio_snr_db", f->audio_snr_db, false);
        if (f->tone) {
            print_json_number("tone_left_hz", f->tone_left_hz, false);
            print_json_number("tone_right_hz", f->tone_right_hz, false);
            print_json_number("separation_db", f->separation_db, false);
        }
        printf("    \"rds\": %s,\n", f->rds ? "true" : "false");
        print_json_number("rds_dev_hz", f->rds_dev_hz, false);
        printf("    \"rds_blocks\": %llu,\n", (unsigned long long)f->rds_blocks);
        printf("    \"rds_block_errors\": %llu,\n", (unsigned long long)f->rds_block_errors);
        printf("    \"rds_groups\": %llu,\n", (unsigned long long)f->rds_groups);
        print_json_number("rds_bler", f->rds_bler, false);
        print_json_number("rds_mer_db", f->rds_mer_db, false);
        if (r->pi_valid) {
            snprintf(pi, sizeof(pi), "%04X", r->pi);
            fm_json_string("pi", pi, false);
            printf("    \"pty\": %u,\n    \"tp\": %s,\n    \"ta\": %s,\n", r->pty, r->tp ? "true" : "false",
                   r->ta ? "true" : "false");
        }
        if (r->ps_valid)
            fm_json_string("ps", r->ps, false);
        if (r->rt_valid)
            fm_json_string("rt", r->rt, false);
        printf("    \"mpx_power_note\": \"one capture; BS.412 averages mpx_mean_square_hz2 over 60 s\"\n");
    }
    printf("  }");
}

/* Watch */
/*-------*/

static unsigned fm_watch_lines;
static struct fm_bs412 fm_watch_bs412;

static int fm_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_fm_result *f = &res->fm;
    const struct m2sdr_fm_rds_info *r = &f->rds_info;
    const bool ok = res->fm_valid && f->found;
    double mean = 0.0;

    if (!res->fm_valid) {
        fprintf(stderr, "The sample rate is below 240 kSPS: no FM analysis.\n");
        return -1;
    }
    if (ok)
        mean = fm_bs412_add(&fm_watch_bs412, now, f->mpx_mean_square_hz2);
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, ok ? "true" : "false");
        if (ok) {
            printf(", \"carrier_offset_hz\": %.1f, \"carrier_db\": %.2f, \"cnr_db\": %.1f, \"peak_dev_hz\": %.0f, "
                   "\"dev_p99_hz\": %.0f, \"mpx_power_dbr\": %.2f, \"mpx_power_60s_dbr\": %.2f, \"pilot_dev_hz\": %.0f, "
                   "\"pilot_freq_error_hz\": %.3f, \"rds_dev_hz\": %.0f, \"rds_bler\": %.4f",
                   f->carrier_offset_hz, f->carrier_dbfs + set->ref_offset_db, f->cnr_db, f->peak_dev_hz, f->dev_p99_hz,
                   f->mpx_power_dbr, mean, f->pilot ? f->pilot_dev_hz : 0.0, f->pilot ? f->pilot_freq_error_hz : 0.0,
                   f->rds_dev_hz, f->rds_bler);
            if (r->pi_valid)
                printf(", \"pi\": \"%04X\"", r->pi);
        }
        printf("}\n");
        return 0;
    }
    if (fm_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
        printf("%8s %10s %7s %5s %6s %6s %6s %6s %6s %7s %5s %6s %4s %-8s %s\n"
               "%8s %10s %7s %5s %6s %6s %6s %6s %6s %7s %5s %6s %4s %-8s %s\n",
               "time", "offset", "level", "CNR", "peak", "p99", "MPX", "MPX60", "pilot", "pilot", "RDS", "BLER", "PI", "PS",
               "RadioText", "s", "Hz", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "dB", "kHz", "kHz", "dBr", "dBr", "kHz",
               "err Hz", "kHz", "%", "", "", "");
    if (!ok) {
        printf("%8.2f  no FM carrier (CNR %.1f dB)\n", now, f->cnr_db);
        return 0;
    }
    printf("%8.2f %+10.1f %7.1f %5.1f %6.1f %6.1f %+6.2f %+6.2f", now, f->carrier_offset_hz,
           f->carrier_dbfs + set->ref_offset_db, f->cnr_db, f->peak_dev_hz / 1e3, f->dev_p99_hz / 1e3, f->mpx_power_dbr, mean);
    if (f->pilot)
        printf(" %6.2f %+7.3f", f->pilot_dev_hz / 1e3, f->pilot_freq_error_hz);
    else
        printf(" %6s %7s", "mono", "-");
    if (f->rds_blocks)
        printf(" %5.2f %6.2f", f->rds_dev_hz / 1e3, 100.0 * f->rds_bler);
    else
        printf(" %5s %6s", "-", "-");
    if (r->pi_valid)
        printf(" %04X", r->pi);
    else
        printf(" %4s", "-");
    printf(" %-8s %s\n", r->ps_valid ? r->ps : "", r->rt_valid ? r->rt : "");
    return 0;
}

/* Command line */
/*--------------*/

enum { FM_OPT_FM, FM_OPT_TONE, FM_OPT_DEEMPH };

static const struct option fm_options[] = {
    { "fm",        no_argument,       NULL, FM_OPT_FM },
    { "fm-tone",   required_argument, NULL, FM_OPT_TONE },
    { "fm-deemph", required_argument, NULL, FM_OPT_DEEMPH },
    { NULL, 0, NULL, 0 }
};

static int fm_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_settings *set = cli->set;

    switch (id) {
    case FM_OPT_FM:
        set->fm_on = true;
        break;
    case FM_OPT_TONE:
        if (m2sdr_cli_parse_double(arg, &set->fm_tone_hz) != 0 || !(set->fm_tone_hz > 0.0 && set->fm_tone_hz < 15000.0)) {
            m2sdr_cli_error("invalid FM test tone '%s' (expected 0 to 15000 Hz)", arg);
            return -1;
        }
        break;
    case FM_OPT_DEEMPH:
        if (strcmp(arg, "50") == 0 || strcmp(arg, "75") == 0) {
            set->fm_deemph_us = arg[0] == '5' ? 50 : 75;
        } else if (strcmp(arg, "0") == 0) {
            set->fm_deemph_us = -1;
        } else {
            m2sdr_cli_invalid_choice("fm-deemph", arg, "50, 75 or 0");
            return -1;
        }
        break;
    default:
        return -1;
    }
    return 0;
}

static void fm_help(void)
{
    printf("FM options:\n"
           "      --fm               With --report: FM broadcast station tuned near its centre:\n"
           "                         deviation, MPX power (BS.412), pilot, stereo, RDS, audio SNR. Any\n"
           "                         sample rate from 240 kSPS (default with --fm: 0.96 MSPS).\n"
           "                         --watch works too.\n"
           "      --fm-tone HZ       Known test tone on one channel: stereo separation.\n"
           "      --fm-deemph US     De-emphasis of the audio SNR: 50, 75 or 0 (default: 50).\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

#define FM_COL_MPX     0xff40c0ffu
#define FM_COL_MEAN    0xffffb040u
#define FM_COL_LIMIT   0xa04040ffu
#define FM_COL_BAND    0x18ffffffu

/* What the view keeps over the results of one tuning. */
struct an_fm_ui {
    struct fm_bs412 bs412;
    double freq_hz, rate;
    int    channel;
    double start_ms;
    float  mpx_avg[M2SDR_FM_MPX_BINS];  /* Spectrum of the multiplex, averaged (power). */
    int    navg;
};

static void fm_keep(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_fm_result *f = &res->fm;
    struct an_fm_ui *v;
    int i;

    if (!res->set.fm_on || !res->fm_valid)
        return;
    if (!ui->fm) {
        ui->fm = calloc(1, sizeof(*ui->fm));
        if (!ui->fm)
            return;
        ui->fm->start_ms = now_ms();
    }
    v = ui->fm;
    if (res->set.freq_hz != v->freq_hz || res->set.sample_rate != v->rate || res->set.channel != v->channel) {
        memset(&v->bs412, 0, sizeof(v->bs412));
        v->freq_hz = res->set.freq_hz;
        v->rate    = res->set.sample_rate;
        v->channel = res->set.channel;
        v->navg    = 0;
    }
    if (!f->found)
        return;
    fm_bs412_add(&v->bs412, (now_ms() - v->start_ms) / 1e3, f->mpx_mean_square_hz2);
    {
        /* Average of the powers: the first ones weigh more, then about 8 results. */
        const float w = v->navg < 8 ? 1.0f / (float)(v->navg + 1) : 0.125f;

        for (i = 0; i < M2SDR_FM_MPX_BINS; i++) {
            const float p = powf(10.0f, f->mpx_db[i] / 10.0f);
            const float a = v->navg ? powf(10.0f, v->mpx_avg[i] / 10.0f) : p;

            v->mpx_avg[i] = 10.0f * log10f(a + w * (p - a) + 1e-30f);
        }
        v->navg++;
    }
}

static void fm_close(struct an_ui *ui)
{
    free(ui->fm);
    ui->fm = NULL;
}

/* Station: frequency in MHz, steps of 100 and 50 kHz, and the centre on the carrier. */
static void fm_station(struct an_ui *ui, const struct m2sdr_fm_result *f, bool ok, bool *changed)
{
    static const double steps[] = { -100e3, -50e3, 50e3, 100e3 };
    static const char *names[] = { "-100k", "-50k", "+50k", "+100k" };
    double mhz = ui->set.freq_hz;
    char text[96];
    unsigned i;

    igTextUnformatted("Station:", NULL);
    igSameLine(0.0f, 8.0f);
    if (input_scaled("MHz##fmfreq", &mhz, 1e6, "%.3f"))
        view_retune(ui, mhz, changed);
    for (i = 0; i < 4; i++) {
        igSameLine(0.0f, 6.0f);
        if (igSmallButton(names[i]))
            view_retune(ui, 1e3 * round((ui->set.freq_hz + steps[i]) / 1e3), changed);
    }
    if (ui->set.sample_rate < M2SDR_FM_AN_MIN_RATE || ui->set.sample_rate > 4e6) {
        igSameLine(0.0f, 16.0f);
        if (igSmallButton("0.96 MSPS"))
            view_rate(ui, FM_DEFAULT_RATE, changed);
    }
    if (ok && fabs(f->carrier_offset_hz) > 2000.0) {
        igSameLine(0.0f, 16.0f);
        snprintf(text, sizeof(text), "Centre on the carrier (%+.1f kHz)", f->carrier_offset_hz / 1e3);
        if (igSmallButton(text))
            view_retune(ui, ui->set.freq_hz + 100.0 * round(f->carrier_offset_hz / 100.0), changed);
    }
}

/* Spectrum of the multiplex with its bands: L+R, pilot, L-R, RDS. */
static void fm_draw_mpx(const struct an_fm_ui *v, const struct m2sdr_fm_result *f, ImVec2 size)
{
    struct m2sdr_plot plot;
    char text[128];

    if (!m2sdr_plot_begin(&plot, "##fmmpx", size, 0.0, M2SDR_FM_MPX_BINS * M2SDR_FM_MPX_BIN_HZ, M2SDR_PLOT_FREQ, -90.0, 10.0,
                          "Multiplex (dBr per bin, averaged; a tone reads its power)"))
        return;
    m2sdr_plot_band(&plot, 30.0, 15000.0, FM_COL_BAND);
    m2sdr_plot_band(&plot, 23000.0, 53000.0, FM_COL_BAND);
    m2sdr_plot_band(&plot, 54600.0, 59400.0, FM_COL_BAND);
    m2sdr_plot_vline(&plot, M2SDR_FM_PILOT_HZ, 0x40ffffffu);
    m2sdr_plot_trace(&plot, v && v->navg ? v->mpx_avg : f->mpx_db, M2SDR_FM_MPX_BINS, 0.0,
                     (M2SDR_FM_MPX_BINS - 1) * M2SDR_FM_MPX_BIN_HZ, FM_COL_MPX);
    if (plot.hovered) {
        int b = (int)lround(plot.mouse_x / M2SDR_FM_MPX_BIN_HZ);

        b = b < 0 ? 0 : b >= M2SDR_FM_MPX_BINS ? M2SDR_FM_MPX_BINS - 1 : b;
        snprintf(text, sizeof(text), "%.2f kHz: %.1f dBr", plot.mouse_x / 1e3, v && v->navg ? v->mpx_avg[b] : f->mpx_db[b]);
        m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
    }
    m2sdr_plot_end(&plot);
}

/* Histogram of |deviation|, log scale, with the 75 kHz of the standard. */
static void fm_draw_deviation(const struct m2sdr_fm_result *f, ImVec2 size)
{
    struct m2sdr_plot plot;
    static float h[M2SDR_FM_DEV_BINS];
    char text[128];
    int i;

    for (i = 0; i < M2SDR_FM_DEV_BINS; i++)
        h[i] = f->dev_hist[i] > 0.0f ? log10f(f->dev_hist[i]) : -7.0f;
    if (!m2sdr_plot_begin(&plot, "##fmdev", size, 0.0, M2SDR_FM_DEV_BINS * M2SDR_FM_DEV_BIN_HZ, M2SDR_PLOT_FREQ, -6.0, 0.0,
                          "Deviation: log10 of the share of the samples"))
        return;
    m2sdr_plot_vline(&plot, M2SDR_FM_DEVIATION_HZ, FM_COL_LIMIT);
    m2sdr_plot_vline(&plot, f->peak_dev_hz, 0x80ffffffu);
    m2sdr_plot_vline(&plot, f->dev_p999_hz, FM_COL_MEAN);
    m2sdr_plot_trace(&plot, h, M2SDR_FM_DEV_BINS, 0.5 * M2SDR_FM_DEV_BIN_HZ, (M2SDR_FM_DEV_BINS - 0.5) * M2SDR_FM_DEV_BIN_HZ,
                     FM_COL_MPX);
    snprintf(text, sizeof(text), "peak %.1f kHz, p99.9 %.1f kHz, p99 %.1f kHz", f->peak_dev_hz / 1e3, f->dev_p999_hz / 1e3,
             f->dev_p99_hz / 1e3);
    m2sdr_plot_text(&plot, 8.0f, 20.0f, text, 0xffffffffu);
    m2sdr_plot_end(&plot);
}

/* MPX power of each result and its 60 s mean, with the limit of BS.412. */
static void fm_draw_power(const struct an_fm_ui *v, ImVec2 size)
{
    struct m2sdr_plot plot;
    const int n = v ? v->bs412.n : 0;
    const double t1 = n ? v->bs412.t[n - 1] : 0.0;
    static float x[FM_HISTORY];
    int i;

    if (!m2sdr_plot_begin(&plot, "##fmpow", size, -120.0, 0.0, M2SDR_PLOT_PLAIN, -12.0, 6.0,
                          "MPX power (dBr) over the last 120 s: each capture, and the 60 s mean"))
        return;
    m2sdr_plot_hline(&plot, 0.0, FM_COL_LIMIT);
    for (i = 0; i < n; i++)
        x[i] = (float)(v->bs412.t[i] - t1);
    if (n > 0) {
        m2sdr_plot_points(&plot, x, v->bs412.dbr, (size_t)n, 1.5f, FM_COL_MPX);
        m2sdr_plot_xy(&plot, x, v->bs412.mean_dbr, (size_t)n, FM_COL_MEAN);
    }
    m2sdr_plot_end(&plot);
}

static void draw_fm(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_fm_result *f = &res->fm;
    const struct m2sdr_fm_rds_info *r = &f->rds_info;
    const struct an_fm_ui *v = ui->fm;
    const bool ok = res->fm_valid && f->found;
    ImVec2 avail, sq;
    float side, w;

    if (live)
        fm_station(ui, f, ok, changed);
    if (!res->set.fm_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->fm_valid) {
        igTextUnformatted("The sample rate must be 240 kSPS or more (0.96 MSPS).", NULL);
        return;
    }
    if (!f->found) {
        igText("No FM carrier (CNR %.1f dB in the channel): tune on a station.", f->cnr_db);
        return;
    }

    /* Station name and text, large. */
    igPushFont(NULL, 2.4f * igGetStyle()->FontSizeBase);
    if (r->ps_valid)
        igTextColored((ImVec4){ 0.4f, 1.0f, 0.5f, 1.0f }, "%s", r->ps);
    else
        igTextColored((ImVec4){ 0.6f, 0.6f, 0.6f, 1.0f }, "%s", r->pi_valid ? "--------" : "no RDS");
    igPopFont();
    igSameLine(0.0f, 24.0f);
    igBeginGroup();
    if (r->pi_valid)
        igText("PI %04X   PTY %u %s   %s%s%s", r->pi, r->pty, m2sdr_fm_rds_pty_name(r->pty), r->tp ? "TP " : "",
               r->ta ? "TA " : "", r->music ? "music" : "speech");
    else
        igTextUnformatted(" ", NULL);
    igText("%s", r->rt_valid ? r->rt : "");
    igEndGroup();
    {
        const int n = v ? v->bs412.n : 0;
        char pilot[48], bler[32];

        if (f->pilot)
            snprintf(pilot, sizeof(pilot), "%.2f kHz, %+.3f Hz", f->pilot_dev_hz / 1e3, f->pilot_freq_error_hz);
        else
            snprintf(pilot, sizeof(pilot), "none (mono)");
        if (f->rds_blocks)
            snprintf(bler, sizeof(bler), "%.2f %%", 100.0 * f->rds_bler);
        else
            snprintf(bler, sizeof(bler), "-");
        igText("MPX power %+.2f dBr, 60 s mean %+.2f dBr (over %.0f s)    peak %.1f kHz    pilot %s    RDS BLER %s",
               f->mpx_power_dbr, n ? v->bs412.mean_dbr[n - 1] : f->mpx_power_dbr, v ? fm_bs412_span(&v->bs412) : 0.0,
               f->peak_dev_hz / 1e3, pilot, bler);
    }

    igGetContentRegionAvail(&avail);
    side = 0.5f * avail.y < 0.3f * avail.x ? 0.5f * avail.y : 0.3f * avail.x;
    sq   = m2sdr_plot_square(side);
    w    = avail.x - sq.x - 8.0f;
    fm_draw_mpx(v, f, (ImVec2){ w, sq.y });
    igSameLine(0.0f, 8.0f);
    draw_trend_points("##fmrds", sq, f->points, f->npoints, M2SDR_MOD_BPSK);
    fm_draw_power(v, (ImVec2){ 0.5f * avail.x - 4.0f, 0.0f });
    igSameLine(0.0f, 8.0f);
    fm_draw_deviation(f, (ImVec2){ 0.0f, 0.0f });
}

static void draw_fm_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_fm_result *f = &res->fm;
    const struct m2sdr_fm_rds_info *r = &f->rds_info;
    const struct an_fm_ui *v = ui->fm;

    igSeparatorText("FM");
    if (!res->fm_valid || !f->found) {
        igTextUnformatted("No station.", NULL);
        return;
    }
    if (igBeginTable("##fm", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Carrier", "%.4f MHz", (res->set.freq_hz + f->carrier_offset_hz) / 1e6);
        label_value("Offset", "%+.1f Hz", f->carrier_offset_hz);
        label_value("Level", "%.1f dBFS", f->carrier_dbfs);
        label_value("CNR", "%.1f dB", f->cnr_db);
        label_value("Peak deviation", "%.1f kHz", f->peak_dev_hz / 1e3);
        label_value("Deviation p99.9, p99", "%.1f, %.1f kHz", f->dev_p999_hz / 1e3, f->dev_p99_hz / 1e3);
        label_value("MPX power", "%+.2f dBr", f->mpx_power_dbr);
        if (v && v->bs412.n)
            label_value("MPX power, 60 s", "%+.2f dBr", v->bs412.mean_dbr[v->bs412.n - 1]);
        if (f->pilot) {
            label_value("Pilot", "%.2f kHz", f->pilot_dev_hz / 1e3);
            label_value("Pilot error", "%+.3f Hz", f->pilot_freq_error_hz);
        } else {
            label_value("Pilot", "none");
        }
        label_value("L+R, L-R RMS", "%.1f, %.1f kHz", f->mono_rms_hz / 1e3, f->stereo_rms_hz / 1e3);
        if (f->tone)
            label_value("Separation", "%.1f dB", f->separation_db);
        label_value("Audio SNR", "%.1f dB", f->audio_snr_db);
        if (f->rds_blocks) {
            label_value("RDS level", "%.2f kHz", f->rds_dev_hz / 1e3);
            label_value("RDS BLER", "%.2f %% (%llu blocks)", 100.0 * f->rds_bler, (unsigned long long)f->rds_blocks);
            label_value("RDS MER", "%.1f dB", f->rds_mer_db);
            label_value("RDS groups", "%llu (total %llu)", (unsigned long long)f->rds_groups, (unsigned long long)r->groups);
        }
        if (r->pi_valid) {
            label_value("PI", "%04X", r->pi);
            label_value("PTY", "%u %s", r->pty, m2sdr_fm_rds_pty_name(r->pty));
        }
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_fm = {
    .key          = "fm",
    .title        = "FM",
    .category     = AN_CAT_BROADCAST,
    .on           = offsetof(struct an_settings, fm_on),
    .valid        = offsetof(struct an_result, fm_valid),
    .found        = offsetof(struct an_result, fm.found),
    .report_rank  = 40,
    .default_rate = FM_DEFAULT_RATE,
    .psd_short    = true,
    .samples      = fm_samples,
    .analyse      = fm_analyse,
    .report_text  = fm_report_text,
    .report_json  = fm_report_json,
    .watch_line   = fm_watch_line,
    .options      = fm_options,
    .option       = fm_option,
    .help         = fm_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_fm,
    .results      = draw_fm_results,
    .keep         = fm_keep,
    .close        = fm_close,
#endif
};
