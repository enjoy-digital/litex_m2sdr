/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: EMC pre-compliance, the measuring receiver of CISPR 16-1-1 over
 * a span (peak, quasi-peak, average) against limit lines, in a report, a view and --watch.
 *
 * Pre-compliance only: the levels are dBFS unless an offset and factors are given, and the
 * CISPR constants of libm2sdr are to be checked against CISPR 16-1-1 and a calibrated
 * generator (see m2sdr_analysis_monitoring.h).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <complex.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "m2sdr_cli.h"

#include "analyzer.h"

#define EMC_REPORT_WORST  10       /* Channels of the lowest margins listed. */

static const char *const emc_band_names[] = { "auto", "A", "B", "C/D" };
static const char *const emc_band_keys[]  = { "auto", "a", "b", "cd" };

/* Settings and result */
/*-----------------------*/

extern const struct an_mode an_mode_emc;

/* Its settings: its block of struct an_settings, read with SET(). */
struct emc_set {
    bool   emc_on;                 /* EMC pre-compliance. */
    int    emc_log2;               /* Its capture: 2^emc_log2 samples; 0: the default. */
    double emc_span_hz;            /* Channels over this span around the centre; 0: the default, < 0: all. */
    struct m2sdr_emc_config emc;   /* Band, unit offset, factors and limit lines (centre and channels set per capture). */
};

#define SET(set) ((struct emc_set *)an_mode_settings(&an_mode_emc, (set)))

/* The result of the mode: its block of struct an_result, read with RES(). */
struct emc_res {
    bool   emc_valid;              /* Measured: the band fits the sample rate and the capture. */
    struct m2sdr_emc_result emc;
};

#define RES(res) ((struct emc_res *)an_mode_result(&an_mode_emc, (res)))

/* Files of the lines, for the view. */
static char emc_paths[3][512];

/* Settings */
/*----------*/

/* A line of SET(set)->emc from a text file, a frequency in Hz and a level in dB per line ('#'
 * comments), in ascending frequency: the quasi-peak limit, the average limit or the factors.
 * NULL or "" clears it. Returns 0, or -1 with err set. */
enum { AN_EMC_QP_LIMIT = 0, AN_EMC_AV_LIMIT = 1, AN_EMC_FACTORS = 2 };

static int emc_load_line(struct an_settings *set, int which, const char *path, char *err, size_t err_len)
{
    struct m2sdr_emc_point pts[M2SDR_EMC_MAX_LIMIT];
    char line[256];
    FILE *f;
    int n = 0, lineno = 0;

    if (which < AN_EMC_QP_LIMIT || which > AN_EMC_FACTORS) {
        snprintf(err, err_len, "bad line");
        return -1;
    }
    if (path && path[0]) {
        f = fopen(path, "r");
        if (!f) {
            snprintf(err, err_len, "%s: %s", path, strerror(errno));
            return -1;
        }
        while (fgets(line, sizeof(line), f)) {
            char *p = strchr(line, '#');
            double fr, db;
            int got;

            lineno++;
            if (p)
                *p = '\0';
            got = sscanf(line, "%lf %lf", &fr, &db);
            if (got <= 0)
                continue;          /* Blank or comment. */
            if (got != 2 || !(fr > 0) || !isfinite(db) || (n > 0 && fr < pts[n - 1].freq_hz) || n == M2SDR_EMC_MAX_LIMIT) {
                snprintf(err, err_len, "%s, line %d: expected a frequency in Hz and a level in dB, in ascending frequency, "
                         "%d lines at most", path, lineno, M2SDR_EMC_MAX_LIMIT);
                fclose(f);
                return -1;
            }
            pts[n].freq_hz  = fr;
            pts[n].level_db = db;
            n++;
        }
        fclose(f);
        if (n == 0) {
            snprintf(err, err_len, "%s: no points", path);
            return -1;
        }
    }
    switch (which) {
    case AN_EMC_QP_LIMIT:
        memcpy(SET(set)->emc.qp_limit, pts, (size_t)n * sizeof(pts[0]));
        SET(set)->emc.nqp_limit = n;
        break;
    case AN_EMC_AV_LIMIT:
        memcpy(SET(set)->emc.av_limit, pts, (size_t)n * sizeof(pts[0]));
        SET(set)->emc.nav_limit = n;
        break;
    default:
        memcpy(SET(set)->emc.factor, pts, (size_t)n * sizeof(pts[0]));
        SET(set)->emc.nfactor = n;
        break;
    }
    snprintf(emc_paths[which], sizeof(emc_paths[which]), "%s", path ? path : "");
    return 0;
}

/* Band of the settings: given, or the one of the centre. */
static enum m2sdr_emc_band emc_band(const struct an_settings *set)
{
    if (SET(set)->emc.band != M2SDR_EMC_BAND_AUTO)
        return SET(set)->emc.band;
    if (!(set->freq_hz > 0))
        return M2SDR_EMC_BAND_B;   /* A file that does not give it. */
    return set->freq_hz < 150e3 ? M2SDR_EMC_BAND_A : set->freq_hz < 30e6 ? M2SDR_EMC_BAND_B : M2SDR_EMC_BAND_CD;
}

static double emc_rbw(const struct an_settings *set)
{
    double rbw = 120e3;

    (void)m2sdr_emc_band_params(emc_band(set), &rbw, NULL, NULL, NULL, NULL);
    return rbw;
}

/* Unit of the levels: dBFS, or what the offset and the factors make of them. */
static const char *emc_unit(const struct m2sdr_emc_config *cfg)
{
    return cfg->nfactor > 0 ? "dBuV/m" : cfg->offset_db != 0.0 ? "dBuV" : "dBFS";
}

/* Level of a channel in the unit of the limits. */
static double emc_level(const struct m2sdr_emc_config *cfg, double freq_hz, double dbfs)
{
    return dbfs + cfg->offset_db + (cfg->nfactor > 0 ? m2sdr_emc_line_db(cfg->factor, cfg->nfactor, freq_hz) : 0.0);
}

/* Analysis */
/*----------*/

/* 2^emc_log2 samples, more when the RBW needs 8 bins of the FFT of the capture. */
static size_t emc_samples(const struct an_settings *set)
{
    const double rbw = emc_rbw(set);
    int log2 = SET(set)->emc_log2 > 0 ? SET(set)->emc_log2 : AN_EMC_DEFAULT_LOG2;

    while (log2 < AN_EMC_MAX_LOG2 && set->sample_rate / (double)((size_t)1 << log2) > rbw / 8.0)
        log2++;
    return (size_t)1 << log2;
}

static void emc_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    struct m2sdr_emc_config cfg = SET(set)->emc;
    const double rbw = emc_rbw(set);
    const double edge = 0.5 * set->sample_rate - 1.5 * rbw;
    size_t n = 1;

    cfg.centre_hz = set->freq_hz;
    cfg.band      = emc_band(set);
    /* Span: AN_EMC_DEFAULT_CHANNELS channels by default, for a live speed; all of the capture
     * with a span < 0. */
    cfg.start_hz = cfg.stop_hz = cfg.step_hz = 0.0;
    if (SET(set)->emc_span_hz >= 0 && edge > 0) {
        double half = SET(set)->emc_span_hz > 0 ? 0.5 * SET(set)->emc_span_hz : 0.25 * (AN_EMC_DEFAULT_CHANNELS - 1) * rbw;

        if (half > edge)
            half = edge;
        cfg.step_hz  = 0.5 * rbw;
        if (2.0 * half / cfg.step_hz + 1 > M2SDR_EMC_MAX_CHANNELS)
            cfg.step_hz = 2.0 * half / (M2SDR_EMC_MAX_CHANNELS - 1);
        cfg.start_hz = -half;
        cfg.stop_hz  = half;
    }
    while (n * 2 <= count && n * 2 <= emc_samples(set))
        n *= 2;
    RES(res)->emc_valid = m2sdr_emc_analyse(x, n, set->sample_rate, &cfg, &RES(res)->emc) == 0;
}

/* Reports */
/*---------*/

/* Channels by their lowest margin, the lowest first: up to max of them. */
static float emc_margin(const struct m2sdr_emc_result *e, int c)
{
    return fminf(e->margin_qp_db[c], e->margin_av_db[c]);
}

static int emc_worst(const struct m2sdr_emc_result *e, int *idx, int max)
{
    int c, k, n = 0;

    for (c = 0; c < e->nchannels; c++) {
        const float m = emc_margin(e, c);

        if (n == max && m >= emc_margin(e, idx[n - 1]))
            continue;
        if (n < max)
            n++;
        for (k = n - 1; k > 0 && emc_margin(e, idx[k - 1]) > m; k--)
            idx[k] = idx[k - 1];
        idx[k] = c;
    }
    return n;
}

static void emc_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_emc_config *cfg = &SET(&res->set)->emc;
    const struct m2sdr_emc_result *e = &RES(res)->emc;
    const char *unit = emc_unit(cfg);
    int idx[EMC_REPORT_WORST], n, i;

    (void)rep;
    printf("\nEMC (pre-compliance, CISPR 16-1-1 receiver%s):\n", cfg->offset_db == 0.0 && cfg->nfactor == 0 ? ", uncalibrated" : "");
    if (!RES(res)->emc_valid) {
        printf("  The band does not fit the capture: its sample rate must be 3 RBW at least, with 8 FFT bins in the RBW.\n");
        return;
    }
    printf("  Band       : %s, RBW %.0f Hz, quasi-peak %.0f / %.0f / %.0f ms, average meter %.0f ms\n",
           emc_band_names[e->band], e->rbw_hz, 1e3 * e->charge_s, 1e3 * e->discharge_s, 1e3 * e->meter_s, 1e3 * e->av_meter_s);
    printf("  Channels   : %d from %.6f to %.6f MHz, capture %.1f ms as one period of a steady emission\n", e->nchannels,
           e->freq_hz[0] / 1e6, e->freq_hz[e->nchannels - 1] / 1e6, 1e3 * e->capture_s);
    for (i = 0; i < e->nchannels && e->freq_hz[i] != e->max_peak_freq_hz; i++)
        ;
    if (i < e->nchannels)
        printf("  Strongest  : %.6f MHz: peak %.1f, quasi-peak %.1f, average %.1f %s\n", e->freq_hz[i] / 1e6,
               emc_level(cfg, e->freq_hz[i], e->peak_dbfs[i]), emc_level(cfg, e->freq_hz[i], e->qp_dbfs[i]),
               emc_level(cfg, e->freq_hz[i], e->av_dbfs[i]), unit);
    if (!e->limits) {
        printf("  Limits     : none given (--emc-limit-qp, --emc-limit-av)\n");
        return;
    }
    printf("  Limits     : %s, worst margin %.1f dB at %.6f MHz (%s)\n", e->pass ? "PASS" : "FAIL", e->worst_margin_db,
           e->worst_freq_hz / 1e6, e->worst_detector == 2 ? "average" : "quasi-peak");
    n = emc_worst(e, idx, EMC_REPORT_WORST);
    printf("  %14s %8s %8s %8s %10s %10s %10s\n", "MHz", "peak", "QP", "AV", "margin pk", "margin QP", "margin AV");
    for (i = 0; i < n; i++) {
        const int c = idx[i];

        char m[3][16];
        int k;

        for (k = 0; k < 3; k++) {
            const float v = k == 0 ? e->margin_peak_db[c] : k == 1 ? e->margin_qp_db[c] : e->margin_av_db[c];

            snprintf(m[k], sizeof(m[k]), v < 900.0f ? "%.1f" : "-", v);
        }
        printf("  %14.6f %8.1f %8.1f %8.1f %10s %10s %10s\n", e->freq_hz[c] / 1e6, emc_level(cfg, e->freq_hz[c], e->peak_dbfs[c]),
               emc_level(cfg, e->freq_hz[c], e->qp_dbfs[c]), emc_level(cfg, e->freq_hz[c], e->av_dbfs[c]), m[0], m[1], m[2]);
    }
}

static void emc_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_emc_config *cfg = &SET(&res->set)->emc;
    const struct m2sdr_emc_result *e = &RES(res)->emc;
    int c;

    (void)rep;
    printf("  \"emc\": {\n");
    printf("    \"valid\": %s%s\n", RES(res)->emc_valid ? "true" : "false", RES(res)->emc_valid ? "," : "");
    if (!RES(res)->emc_valid) {
        printf("  }");
        return;
    }
    printf("    \"band\": \"%s\",\n", emc_band_keys[e->band]);
    printf("    \"unit\": \"%s\",\n", emc_unit(cfg));
    print_json_number("rbw_hz", e->rbw_hz, false);
    print_json_number("capture_s", e->capture_s, false);
    printf("    \"nchannels\": %d,\n", e->nchannels);
    print_json_number("max_peak_hz", e->max_peak_freq_hz, false);
    print_json_number("max_peak_db", emc_level(cfg, e->max_peak_freq_hz, e->max_peak_dbfs), false);
    printf("    \"limits\": %s,\n", e->limits ? "true" : "false");
    if (e->limits) {
        printf("    \"pass\": %s,\n", e->pass ? "true" : "false");
        print_json_number("worst_margin_db", e->worst_margin_db, false);
        print_json_number("worst_freq_hz", e->worst_freq_hz, false);
        printf("    \"worst_detector\": \"%s\",\n", e->worst_detector == 2 ? "average" : "quasi-peak");
    }
    printf("    \"channels\": [");
    for (c = 0; c < e->nchannels; c++) {
        const double f = e->freq_hz[c];

        printf("%s\n      {\"freq_hz\": %.1f, \"peak_db\": %.2f, \"qp_db\": %.2f, \"av_db\": %.2f", c ? "," : "", f,
               emc_level(cfg, f, e->peak_dbfs[c]), emc_level(cfg, f, e->qp_dbfs[c]), emc_level(cfg, f, e->av_dbfs[c]));
        if (e->limits && e->margin_qp_db[c] < 900.0f)
            printf(", \"margin_qp_db\": %.2f", e->margin_qp_db[c]);
        if (e->limits && e->margin_av_db[c] < 900.0f)
            printf(", \"margin_av_db\": %.2f", e->margin_av_db[c]);
        printf("}");
    }
    printf("\n    ]\n");
    printf("  }");
}

/* Command line */
/*--------------*/

enum { EMC_OPT_ON, EMC_OPT_BAND, EMC_OPT_SPAN, EMC_OPT_OFFSET, EMC_OPT_LIMIT_QP, EMC_OPT_LIMIT_AV, EMC_OPT_FACTORS,
       EMC_OPT_SAMPLES };

static const struct option emc_options[] = {
    { "emc",             no_argument,       NULL, EMC_OPT_ON },
    { "emc-band",        required_argument, NULL, EMC_OPT_BAND },
    { "emc-span",        required_argument, NULL, EMC_OPT_SPAN },
    { "emc-offset",      required_argument, NULL, EMC_OPT_OFFSET },
    { "emc-limit-qp",    required_argument, NULL, EMC_OPT_LIMIT_QP },
    { "emc-limit-av",    required_argument, NULL, EMC_OPT_LIMIT_AV },
    { "emc-factors",     required_argument, NULL, EMC_OPT_FACTORS },
    { "emc-samples",     required_argument, NULL, EMC_OPT_SAMPLES },
    { NULL, 0, NULL, 0 }
};

static int emc_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_settings *set = cli->set;
    char err[256];
    double v;

    switch (id) {
    case EMC_OPT_ON: SET(set)->emc_on = true; break;
    case EMC_OPT_BAND:
        if (strcmp(arg, "auto") == 0) {
            SET(set)->emc.band = M2SDR_EMC_BAND_AUTO;
        } else if (strcasecmp(arg, "a") == 0) {
            SET(set)->emc.band = M2SDR_EMC_BAND_A;
        } else if (strcasecmp(arg, "b") == 0) {
            SET(set)->emc.band = M2SDR_EMC_BAND_B;
        } else if (strcasecmp(arg, "cd") == 0 || strcasecmp(arg, "c") == 0 || strcasecmp(arg, "d") == 0) {
            SET(set)->emc.band = M2SDR_EMC_BAND_CD;
        } else {
            m2sdr_cli_invalid_choice("EMC band", arg, "auto, a, b or cd");
            return -1;
        }
        break;
    case EMC_OPT_SPAN:
        if (m2sdr_cli_parse_double(arg, &SET(set)->emc_span_hz) != 0) {
            m2sdr_cli_error("invalid EMC span '%s'", arg);
            return -1;
        }
        break;
    case EMC_OPT_OFFSET:
        if (m2sdr_cli_parse_double(arg, &SET(set)->emc.offset_db) != 0) {
            m2sdr_cli_error("invalid EMC offset '%s'", arg);
            return -1;
        }
        break;
    case EMC_OPT_LIMIT_QP:
    case EMC_OPT_LIMIT_AV:
    case EMC_OPT_FACTORS:
        if (emc_load_line(set, id == EMC_OPT_LIMIT_QP ? AN_EMC_QP_LIMIT : id == EMC_OPT_LIMIT_AV ? AN_EMC_AV_LIMIT : AN_EMC_FACTORS,
                          arg, err, sizeof(err)) != 0) {
            m2sdr_cli_error("%s", err);
            return -1;
        }
        break;
    case EMC_OPT_SAMPLES:
        if (m2sdr_cli_parse_double(arg, &v) != 0 || v < (double)(1 << AN_EMC_MIN_LOG2) || v > (double)(1 << AN_EMC_MAX_LOG2)) {
            m2sdr_cli_error("invalid EMC samples '%s' (expected %d to %d)", arg, 1 << AN_EMC_MIN_LOG2, 1 << AN_EMC_MAX_LOG2);
            return -1;
        }
        for (SET(set)->emc_log2 = AN_EMC_MIN_LOG2; (double)((size_t)2 << SET(set)->emc_log2) <= v; SET(set)->emc_log2++)
            ;
        break;
    default:
        return -1;
    }
    return 0;
}

static void emc_help(void)
{
    printf("EMC options:\n"
           "      --emc              With --report: EMC pre-compliance, the CISPR 16-1-1 receiver over a\n"
           "                         span: peak, quasi-peak and average per channel, against limit\n"
           "                         lines. Uncalibrated: dBFS unless --emc-offset. --watch works too.\n"
           "      --emc-band NAME    auto, a (200 Hz RBW), b (9 kHz) or cd (120 kHz) (default: auto,\n"
           "                         from --freq).\n"
           "      --emc-span HZ      Span of the channels (default: 100 RBW, %d channels, for speed;\n"
           "                         -1: the whole capture).\n"
           "      --emc-samples N    Samples of its capture (default: %d, more when the RBW needs).\n"
           "      --emc-offset DB    Added to dBFS to read dBuV at the input (calibration).\n"
           "      --emc-limit-qp PATH, --emc-limit-av PATH   Limit lines: a frequency in Hz and a level\n"
           "                         in dB per line, in the unit of the levels.\n"
           "      --emc-factors PATH Antenna and cable factors, same format, added to the levels.\n"
           "\n", AN_EMC_DEFAULT_CHANNELS, 1 << AN_EMC_DEFAULT_LOG2);
}

/* Watch */
/*-------*/

static unsigned emc_watch_lines;

static int emc_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_emc_config *cfg = &SET(set)->emc;
    const struct m2sdr_emc_result *e = &RES(res)->emc;
    double qp = NAN, av = NAN;
    int c;

    if (!RES(res)->emc_valid) {
        fprintf(stderr, "The EMC band does not fit the sample rate.\n");
        return -1;
    }
    for (c = 0; c < e->nchannels; c++)
        if (e->freq_hz[c] == e->max_peak_freq_hz) {
            qp = emc_level(cfg, e->freq_hz[c], e->qp_dbfs[c]);
            av = emc_level(cfg, e->freq_hz[c], e->av_dbfs[c]);
        }
    if (json) {
        printf("{\"time_s\": %.3f, \"max_peak_hz\": %.1f, \"peak_db\": %.2f, \"qp_db\": %.2f, \"av_db\": %.2f", now,
               e->max_peak_freq_hz, emc_level(cfg, e->max_peak_freq_hz, e->max_peak_dbfs), qp, av);
        if (e->limits)
            printf(", \"pass\": %s, \"worst_margin_db\": %.2f, \"worst_freq_hz\": %.1f", e->pass ? "true" : "false",
                   e->worst_margin_db, e->worst_freq_hz);
        printf("}\n");
        return 0;
    }
    if (emc_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
        printf("%8s %14s %8s %8s %8s %8s %14s %5s\n%8s %14s %8s %8s %8s %8s %14s %5s\n",
               "time", "strongest", "peak", "QP", "AV", "margin", "at", "",
               "s", "MHz", emc_unit(cfg), emc_unit(cfg), emc_unit(cfg), "dB", "MHz", "");
    printf("%8.2f %14.6f %8.1f %8.1f %8.1f", now, e->max_peak_freq_hz / 1e6, emc_level(cfg, e->max_peak_freq_hz, e->max_peak_dbfs),
           qp, av);
    if (e->limits)
        printf(" %8.1f %14.6f %5s\n", e->worst_margin_db, e->worst_freq_hz / 1e6, e->pass ? "pass" : "FAIL");
    else
        printf(" %8s\n", "-");
    return 0;
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

static void draw_emc_files(struct an_ui *ui, bool *changed)
{
    static const char *const labels[3] = { "QP limit", "AV limit", "Factors" };
    static char edit[3][512];
    static bool init;
    char id[32], err[256];
    int k;

    if (!init) {
        memcpy(edit, emc_paths, sizeof(edit));
        init = true;
    }
    for (k = 0; k < 3; k++) {
        if (k)
            same_line_or_wrap(16.0f, 330.0f);
        snprintf(id, sizeof(id), "%s##emcf%d", labels[k], k);
        igSetNextItemWidth(200.0f);
        igInputText(id, edit[k], sizeof(edit[k]), 0, NULL, NULL);
        igSameLine(0.0f, 4.0f);
        snprintf(id, sizeof(id), "Load##emcl%d", k);
        if (igSmallButton(id)) {
            if (emc_load_line(&ui->set, k, edit[k], err, sizeof(err)) == 0) {
                *changed = true;
                snprintf(ui->message, sizeof(ui->message), "%s: %.200s", labels[k], edit[k][0] ? edit[k] : "cleared");
            } else {
                snprintf(ui->message, sizeof(ui->message), "%.250s", err);
            }
        }
    }
}

static void draw_emc_plot(const struct an_result *res, ImVec2 size)
{
    static float fx[M2SDR_EMC_MAX_CHANNELS], pk[M2SDR_EMC_MAX_CHANNELS], qp[M2SDR_EMC_MAX_CHANNELS], av[M2SDR_EMC_MAX_CHANNELS];
    static float lq[M2SDR_EMC_MAX_CHANNELS], la[M2SDR_EMC_MAX_CHANNELS];
    const struct m2sdr_emc_config *cfg = &SET(&res->set)->emc;
    const struct m2sdr_emc_result *e = &RES(res)->emc;
    const int n = e->nchannels;
    double top = -300.0, bottom = 300.0;
    struct m2sdr_plot plot;
    char text[192], title[128];
    int c;

    for (c = 0; c < n; c++) {
        fx[c] = (float)e->freq_hz[c];
        pk[c] = (float)emc_level(cfg, e->freq_hz[c], e->peak_dbfs[c]);
        qp[c] = (float)emc_level(cfg, e->freq_hz[c], e->qp_dbfs[c]);
        av[c] = (float)emc_level(cfg, e->freq_hz[c], e->av_dbfs[c]);
        lq[c] = cfg->nqp_limit > 0 ? (float)m2sdr_emc_line_db(cfg->qp_limit, cfg->nqp_limit, e->freq_hz[c]) : NAN;
        la[c] = cfg->nav_limit > 0 ? (float)m2sdr_emc_line_db(cfg->av_limit, cfg->nav_limit, e->freq_hz[c]) : NAN;
        top    = fmax(top, fmax(pk[c], isfinite(lq[c]) ? lq[c] : -300.0));
        bottom = fmin(bottom, av[c]);
    }
    top    = 10.0 * ceil(top / 10.0) + 10.0;
    bottom = 10.0 * floor(bottom / 10.0) - 10.0;
    /* Kept until m2sdr_plot_end(), which draws it. */
    snprintf(title, sizeof(title), "Levels, %s, against the frequency (RBW %.0f Hz)", emc_unit(cfg), e->rbw_hz);
    if (!m2sdr_plot_begin(&plot, "##emc", size, fx[0], fx[n - 1] > fx[0] ? fx[n - 1] : fx[0] + 1.0f, M2SDR_PLOT_FREQ,
                          bottom, top, title))
        return;
    if (n > 1) {
        if (cfg->nqp_limit > 0)
            m2sdr_plot_xy(&plot, fx, lq, (size_t)n, 0xff4040ffu);
        if (cfg->nav_limit > 0)
            m2sdr_plot_xy(&plot, fx, la, (size_t)n, 0xff40a0ffu);
        m2sdr_plot_xy(&plot, fx, pk, (size_t)n, 0xff40d0ffu);
        m2sdr_plot_xy(&plot, fx, av, (size_t)n, 0xffffc040u);
        m2sdr_plot_xy(&plot, fx, qp, (size_t)n, 0xff60ff60u);
    } else {
        m2sdr_plot_points(&plot, fx, pk, 1, 3.0f, 0xff40d0ffu);
        m2sdr_plot_points(&plot, fx, qp, 1, 3.0f, 0xff60ff60u);
        m2sdr_plot_points(&plot, fx, av, 1, 3.0f, 0xffffc040u);
    }
    if (e->limits && !e->pass)
        m2sdr_plot_vline(&plot, e->worst_freq_hz, 0x804040ffu);
    m2sdr_plot_text(&plot, 8.0f, -20.0f, "yellow: peak, green: quasi-peak, blue: average, red: QP limit, orange: AV limit",
                    0xffc8c8c8u);
    if (plot.hovered && n > 1) {
        int i = (int)lround((plot.mouse_x - fx[0]) / ((double)fx[n - 1] - fx[0]) * (n - 1));
        char f[32];

        i = i < 0 ? 0 : i >= n ? n - 1 : i;
        m2sdr_plot_format(f, sizeof(f), e->freq_hz[i], e->rbw_hz / 10.0, M2SDR_PLOT_FREQ);
        snprintf(text, sizeof(text), "%s  pk %.1f  QP %.1f  AV %.1f", f, pk[i], qp[i], av[i]);
        m2sdr_plot_text(&plot, -330.0f, 4.0f, text, 0xffffffffu);
        m2sdr_plot_vline(&plot, e->freq_hz[i], 0x40ffffffu);
    }
    m2sdr_plot_end(&plot);
}

static void draw_emc_margins(const struct an_result *res, ImVec2 size)
{
    static const char *const heads[] = { "MHz", "peak", "QP", "AV", "margin QP", "margin AV" };
    const struct m2sdr_emc_config *cfg = &SET(&res->set)->emc;
    const struct m2sdr_emc_result *e = &RES(res)->emc;
    int idx[64], n, i, c;

    if (!e->limits) {
        igTextUnformatted("No limit line: load one (a frequency in Hz and a level in dB per line).", NULL);
        return;
    }
    n = emc_worst(e, idx, 64);
    if (!igBeginTable("##emcm", 6, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                      ImGuiTableFlags_SizingFixedFit, size, 0.0f))
        return;
    igTableSetupScrollFreeze(0, 1);
    for (c = 0; c < 6; c++)
        igTableSetupColumn(heads[c], 0, 0.0f, 0);
    igTableHeadersRow();
    for (i = 0; i < n; i++) {
        const int k = idx[i];
        const double f = e->freq_hz[k];
        const ImVec4 bad = { 1.0f, 0.45f, 0.35f, 1.0f }, good = { 0.8f, 0.8f, 0.8f, 1.0f };

        igTableNextRow(0, 0.0f);
        igTableNextColumn();
        igText("%.6f", f / 1e6);
        igTableNextColumn();
        igText("%.1f", emc_level(cfg, f, e->peak_dbfs[k]));
        igTableNextColumn();
        igText("%.1f", emc_level(cfg, f, e->qp_dbfs[k]));
        igTableNextColumn();
        igText("%.1f", emc_level(cfg, f, e->av_dbfs[k]));
        igTableNextColumn();
        if (e->margin_qp_db[k] < 900.0f)
            igTextColored(e->margin_qp_db[k] < 0 ? bad : good, "%.1f", e->margin_qp_db[k]);
        igTableNextColumn();
        if (e->margin_av_db[k] < 900.0f)
            igTextColored(e->margin_av_db[k] < 0 ? bad : good, "%.1f", e->margin_av_db[k]);
    }
    igEndTable();
}

static void draw_emc(struct an_ui *ui, bool live, bool *changed)
{
    static const char *const bands[] = { "Auto", "A: 9-150 kHz", "B: 0.15-30 MHz", "C/D: 30-1000 MHz" };
    const struct an_result *res = &ui->res;
    const struct m2sdr_emc_result *e = &RES(res)->emc;
    int band = (int)SET(&ui->set)->emc.band;
    double span_mhz = SET(&ui->set)->emc_span_hz / 1e6;
    ImVec2 avail;

    (void)live;
    igSetNextItemWidth(170.0f);
    if (igCombo_Str_arr("Band", &band, bands, 4, -1)) {
        SET(&ui->set)->emc.band = (enum m2sdr_emc_band)band;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 230.0f);
    if (input_scaled("Span (MHz)", &span_mhz, 1.0, "%.3f")) {
        SET(&ui->set)->emc_span_hz = span_mhz * 1e6;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 230.0f);
    if (input_scaled("Offset (dB)", &SET(&ui->set)->emc.offset_db, 1.0, "%.1f"))
        *changed = true;
    draw_emc_files(ui, changed);
    igPushTextWrapPos(0.0f);
    igTextDisabled("Pre-compliance: levels in dBFS unless an offset (dBFS to dBuV) and factors (antenna, cable) are given; "
                   "the CISPR 16-1-1 constants come from memory and are to be checked against the standard and a calibrated "
                   "generator. Span 0: %d channels (100 RBW) for a live speed, < 0: the whole capture.", AN_EMC_DEFAULT_CHANNELS);
    igPopTextWrapPos();

    if (!SET(&res->set)->emc_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!RES(res)->emc_valid) {
        igTextUnformatted("The band does not fit the capture: the sample rate must be 3 RBW at least.", NULL);
        return;
    }
    if (e->limits) {
        if (e->pass)
            igTextColored((ImVec4){ 0.4f, 1.0f, 0.5f, 1.0f }, "PASS: worst margin %.1f dB at %.6f MHz (%s)", e->worst_margin_db,
                          e->worst_freq_hz / 1e6, e->worst_detector == 2 ? "average" : "quasi-peak");
        else
            igTextColored((ImVec4){ 1.0f, 0.45f, 0.35f, 1.0f }, "FAIL: worst margin %.1f dB at %.6f MHz (%s)", e->worst_margin_db,
                          e->worst_freq_hz / 1e6, e->worst_detector == 2 ? "average" : "quasi-peak");
    }
    igGetContentRegionAvail(&avail);
    draw_emc_plot(res, (ImVec2){ avail.x, e->limits ? 0.62f * avail.y : avail.y });
    if (e->limits) {
        igGetContentRegionAvail(&avail);
        draw_emc_margins(res, avail);
    }
}

static void draw_emc_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_emc_result *e = &RES(res)->emc;
    const struct m2sdr_emc_config *cfg = &SET(&res->set)->emc;

    igSeparatorText("EMC");
    if (!RES(res)->emc_valid) {
        igTextUnformatted("Band out of the capture.", NULL);
        return;
    }
    if (igBeginTable("##emcr", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Band", "%s, RBW %.0f Hz", emc_band_names[e->band], e->rbw_hz);
        label_value("Quasi-peak", "%.0f / %.0f / %.0f ms", 1e3 * e->charge_s, 1e3 * e->discharge_s, 1e3 * e->meter_s);
        label_value("Channels", "%d", e->nchannels);
        label_value("Capture", "%.1f ms", 1e3 * e->capture_s);
        label_value("Strongest", "%.6f MHz", e->max_peak_freq_hz / 1e6);
        label_value("Its peak", "%.1f %s", emc_level(cfg, e->max_peak_freq_hz, e->max_peak_dbfs), emc_unit(cfg));
        if (e->limits) {
            label_value("Result", "%s", e->pass ? "PASS" : "FAIL");
            label_value("Worst margin", "%.1f dB (%s)", e->worst_margin_db, e->worst_detector == 2 ? "AV" : "QP");
            label_value("At", "%.6f MHz", e->worst_freq_hz / 1e6);
        }
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_emc = {
    .key           = "emc",
    .title         = "EMC",
    .category      = AN_CAT_MONITORING,
    .settings_size = sizeof(struct emc_set),
    .on            = offsetof(struct emc_set, emc_on),
    .result_size   = sizeof(struct emc_res),
    .valid         = offsetof(struct emc_res, emc_valid),
    .report_rank   = 81,
    .psd_short     = true,
    .samples       = emc_samples,
    .analyse       = emc_analyse,
    .report_text   = emc_report_text,
    .report_json   = emc_report_json,
    .watch_line    = emc_watch_line,
    .options       = emc_options,
    .option        = emc_option,
    .help          = emc_help,
#ifdef M2SDR_ANALYZER_GUI
    .draw          = draw_emc,
    .results       = draw_emc_results,
#endif
};
