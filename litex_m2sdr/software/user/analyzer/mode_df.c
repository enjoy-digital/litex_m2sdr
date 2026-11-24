/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: direction finding with two antennas on RX1 and RX2 (angle of
 * arrival, candidates, beamscan and MUSIC), a calibration stored per board, in a report, a view
 * and --watch.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <complex.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "m2sdr_cal.h"

#include "m2sdr_cli.h"

#include "analyzer.h"

#define DF_C 299792458.0

/* Result */
/*--------*/

/* The result of the mode: its block of struct an_result, read with RES(). */
struct df_res {
    bool   df_valid;
    struct m2sdr_df_result df;
    double df_spacing_m;           /* Used. */
    double df_cal_deg;             /* Used, */
    int    df_cal_source;          /* from: 0 none, 1 the command line or the view, 2 the store. */
    double df_cal_freq_hz;         /* The frequency of the stored one. */
};

extern const struct an_mode an_mode_df;
#define RES(res) ((struct df_res *)an_mode_result(&an_mode_df, (res)))

/* Calibration */
/*-------------*/

/* Phase of RX2 against RX1 of the board and its cables, read on a source at broadside, per
 * frequency: the cables and the filters make it change with the frequency. The window stores
 * while the worker reads: a lock. */
static struct {
    pthread_mutex_t lock;
    char   path[1024];
    bool   path_given;             /* --df-cal-file. */
    int    n;
    double freq_hz[AN_DF_CAL_MAX];
    double phase_deg[AN_DF_CAL_MAX];
} df_cal = { PTHREAD_MUTEX_INITIALIZER, "", false, 0, { 0 }, { 0 } };

/* The file of --df-cal-file, or the one of the board. Returns -1 when there is neither. */
static int df_cal_init(uint64_t dna)
{
    char cal[1024], line[256], *slash;
    FILE *f;

    df_cal.n = 0;
    if (df_cal.path_given) {
        /* The path of the option. */
    } else if (dna && m2sdr_cal_default_path(dna, cal, sizeof(cal)) == 0 && (slash = strrchr(cal, '/'))) {
        *slash = '\0';
        snprintf(df_cal.path, sizeof(df_cal.path), "%.900s/df_cal_%014llx.csv", cal, (unsigned long long)dna);
    } else {
        return -1;
    }
    f = fopen(df_cal.path, "r");
    if (!f)
        return 0;                  /* Nothing stored yet. */
    while (fgets(line, sizeof(line), f) && df_cal.n < AN_DF_CAL_MAX) {
        double a, b;

        if (line[0] != '#' && sscanf(line, "%lf,%lf", &a, &b) == 2) {
            df_cal.freq_hz[df_cal.n]   = a;
            df_cal.phase_deg[df_cal.n] = b;
            df_cal.n++;
        }
    }
    fclose(f);
    return 0;
}

/* The calibrations of the board, once the source is open: --df-cal-store needs a file. */
static int df_opened(struct an_cli *cli)
{
    struct an_source *src = cli->src;

    if (src->live && !src->dna)
        (void)m2sdr_get_fpga_dna(src->dev, &src->dna);
    if (df_cal_init(src->live ? src->dna : 0) != 0 && cli->set->df_cal_store) {
        m2sdr_cli_error("Could not store the calibration (no board, no --df-cal-file)");
        return -1;
    }
    return 0;
}

const char *df_cal_file(void)
{
    return df_cal.path[0] ? df_cal.path : NULL;
}

/* The calibration nearest freq_hz, within AN_DF_CAL_SPAN_HZ. */
bool df_cal_lookup(double freq_hz, double *phase_deg, double *at_hz)
{
    int i, best = -1;

    pthread_mutex_lock(&df_cal.lock);
    for (i = 0; i < df_cal.n; i++) {
        if (fabs(df_cal.freq_hz[i] - freq_hz) <= AN_DF_CAL_SPAN_HZ &&
            (best < 0 || fabs(df_cal.freq_hz[i] - freq_hz) < fabs(df_cal.freq_hz[best] - freq_hz)))
            best = i;
    }
    if (best >= 0) {
        *phase_deg = df_cal.phase_deg[best];
        *at_hz     = df_cal.freq_hz[best];
    }
    pthread_mutex_unlock(&df_cal.lock);
    return best >= 0;
}

/* Stores the calibration of a frequency, in place of one within 1 kHz of it, and writes the file. */
int df_cal_store(double freq_hz, double phase_deg)
{
    char dir[1024], *p;
    FILE *f;
    int i, rc = -1;

    pthread_mutex_lock(&df_cal.lock);
    if (!df_cal.path[0])
        goto out;
    for (i = 0; i < df_cal.n && fabs(df_cal.freq_hz[i] - freq_hz) > 1e3; i++)
        ;
    if (i == df_cal.n) {
        if (df_cal.n == AN_DF_CAL_MAX)
            goto out;
        df_cal.n++;
    }
    df_cal.freq_hz[i]   = freq_hz;
    df_cal.phase_deg[i] = phase_deg;
    snprintf(dir, sizeof(dir), "%s", df_cal.path);
    for (p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(dir, 0755);
            *p = '/';
        }
    }
    f = fopen(df_cal.path, "w");
    if (!f)
        goto out;
    fprintf(f, "# M2SDR direction finding: phase of RX2 against RX1, source at broadside\n# freq_hz,phase_deg\n");
    for (i = 0; i < df_cal.n; i++)
        fprintf(f, "%.0f,%.4f\n", df_cal.freq_hz[i], df_cal.phase_deg[i]);
    rc = fclose(f) == 0 ? 0 : -1;

out:
    pthread_mutex_unlock(&df_cal.lock);
    return rc;
}

/* Command line */
/*--------------*/

static const struct option df_options[] = {
    { "df",           no_argument,       NULL, 0 },
    { "df-spacing",   required_argument, NULL, 1 },
    { "df-freq",      required_argument, NULL, 2 },
    { "df-bw",        required_argument, NULL, 3 },
    { "df-cal",       required_argument, NULL, 4 },
    { "df-cal-store", no_argument,       NULL, 5 },
    { "df-cal-file",  required_argument, NULL, 6 },
    { NULL, 0, NULL, 0 }
};

static int df_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_settings *set = cli->set;

    switch (id) {
    case 0:
        set->df_on = true;
        break;
    case 1:
        if (m2sdr_cli_parse_double(arg, &set->df_spacing_m) != 0 || !(set->df_spacing_m > 0)) {
            m2sdr_cli_error("invalid antenna spacing '%s'", arg);
            return -1;
        }
        break;
    case 2:
        if (m2sdr_cli_parse_double(arg, &set->df_freq_hz) != 0) {
            m2sdr_cli_error("invalid signal offset '%s'", arg);
            return -1;
        }
        break;
    case 3:
        if (m2sdr_cli_parse_double(arg, &set->df_bw_hz) != 0 || set->df_bw_hz < 0) {
            m2sdr_cli_error("invalid signal bandwidth '%s'", arg);
            return -1;
        }
        break;
    case 4:
        if (m2sdr_cli_parse_double(arg, &set->df_cal_deg) != 0) {
            m2sdr_cli_error("invalid calibration '%s'", arg);
            return -1;
        }
        set->df_cal_given = true;
        break;
    case 5:
        set->df_cal_store = set->df_on = true;
        break;
    case 6:
        snprintf(df_cal.path, sizeof(df_cal.path), "%.1023s", arg);
        df_cal.path_given = true;
        break;
    }
    return 0;
}

static void df_help(void)
{
    printf("Direction finding options (RX1 and RX2 together; a file needs two channels):\n"
           "      --df               With --report: angle of arrival of the strongest signal on two\n"
           "                         antennas, with every candidate beyond lambda/2. --watch works too.\n"
           "      --df-spacing M     Spacing of the antennas, metres (default: half the wavelength).\n"
           "      --df-freq HZ       The signal, from the centre (default: the strongest).\n"
           "      --df-bw HZ         Its bandwidth (default: the main lobe of its peak).\n"
           "      --df-cal DEG       Phase of RX2 against RX1 of the board and cables (default: the one\n"
           "                         stored for the frequency, ~/.config/litex_m2sdr/df_cal_<dna>.csv).\n"
           "      --df-cal-store     With --report --df: store the phase read, a source at broadside.\n"
           "      --df-cal-file PATH File of the calibrations (default: one per board).\n"
           "\n");
}

/* Analysis */
/*----------*/

static size_t df_samples(const struct an_settings *set)
{
    (void)set;
    return (size_t)1 << AN_TWO_LOG2;
}

static void df_analyse(const float complex *x1, const float complex *x2, size_t count, const struct an_settings *set,
                       struct an_result *res)
{
    struct m2sdr_df_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    RES(res)->df_cal_source = 0;
    RES(res)->df_cal_deg    = 0.0;
    if (set->df_cal_given) {
        RES(res)->df_cal_deg    = set->df_cal_deg;
        RES(res)->df_cal_source = 1;
    } else if (df_cal_lookup(set->freq_hz, &RES(res)->df_cal_deg, &RES(res)->df_cal_freq_hz)) {
        RES(res)->df_cal_source = 2;
    }
    /* Without a spacing, half the wavelength of the centre: no ambiguity. */
    RES(res)->df_spacing_m = set->df_spacing_m > 0 ? set->df_spacing_m : set->freq_hz > 0 ? 0.5 * DF_C / set->freq_hz : 0.0;
    cfg.spacing_m     = RES(res)->df_spacing_m;
    cfg.rf_freq_hz    = set->freq_hz;
    cfg.freq_hz       = set->df_freq_hz;
    cfg.bandwidth_hz  = set->df_bw_hz;
    cfg.cal_phase_deg = RES(res)->df_cal_deg;
    RES(res)->df_valid = m2sdr_df_analyse(x1, x2, count, set->sample_rate, &cfg, &RES(res)->df) == 0;
}

/* Reports */
/*---------*/

static const char *df_cal_name(const struct an_result *res, char *text, size_t len)
{
    if (RES(res)->df_cal_source == 1)
        snprintf(text, len, "%+.2f deg, given", RES(res)->df_cal_deg);
    else if (RES(res)->df_cal_source == 2)
        snprintf(text, len, "%+.2f deg, stored at %.3f MHz", RES(res)->df_cal_deg, RES(res)->df_cal_freq_hz / 1e6);
    else
        snprintf(text, len, "none");
    return text;
}

/* --report --df-cal-store: the phase read, a source at broadside, as the calibration of the
 * frequency. The command fails when nothing is stored. */
static int df_report_done(const struct an_result *res)
{
    double cal;

    if (!res->set.df_cal_store)
        return 0;
    if (!RES(res)->df_valid || !RES(res)->df.found) {
        fprintf(stderr, "No signal: no direction finding calibration stored.\n");
        return -1;
    }
    cal = m2sdr_df_calibration(&RES(res)->df, 0.0);
    if (df_cal_store(res->set.freq_hz, cal) != 0) {
        fprintf(stderr, "Could not store the calibration%s%s.\n", df_cal_file() ? " in " : " (no board, no --df-cal-file)",
                df_cal_file() ? df_cal_file() : "");
        return -1;
    }
    fprintf(stderr, "Calibration %+.3f degrees at %.6f MHz stored in %s.\n", cal, res->set.freq_hz / 1e6, df_cal_file());
    return 0;
}

static void df_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_df_result *d = &RES(res)->df;
    char text[96];
    int i;

    printf("\nDirection finding (RX1, RX2):\n");
    if (!res->two_channels) {
        printf("  Needs both channels: a board, or a file of two channels.\n");
    } else if (!RES(res)->df_valid) {
        printf("  Not measured: the centre frequency (--freq) and the spacing must be known, the signal in the capture.\n");
    } else if (!d->found) {
        printf("  No signal stands out on both channels (SNR %.1f dB, coherence %.3f).\n", d->snr_db, d->coherence);
    } else {
        printf("  Signal     : %.6f MHz (%+.1f Hz), %.1f %s, SNR %.1f dB, coherence %.4f\n",
               (res->set.freq_hz + d->freq_hz) / 1e6, d->freq_hz, d->level_db + rep->off, rep->unit, d->snr_db, d->coherence);
        printf("  Antennas   : %.4f m apart, %.3f wavelengths (%.4f m)\n", RES(res)->df_spacing_m, d->spacing_wl, d->wavelength_m);
        printf("  Phase      : %+.3f deg as received, calibration %s: %+.3f deg (+-%.3f)\n", d->phase_raw_deg,
               df_cal_name(res, text, sizeof(text)), d->phase_deg, d->phase_std_deg);
        printf("  Gain       : %+.2f dB (RX2 over RX1)\n", d->gain_db);
        printf("  Angle      : %+.2f deg from broadside, towards RX2 (+-%.2f), MUSIC peak %+.2f deg\n", d->angle_deg,
               d->angle_std_deg, d->music_peak_deg);
        if (d->ambiguous) {
            printf("  Ambiguous  : spaced more than lambda/2, %d angles give this phase:", d->candidates);
            for (i = 0; i < d->candidates; i++)
                printf(" %+.2f", d->candidate_deg[i]);
            printf(" deg\n");
        }
    }
}

static void df_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_df_result *d = &RES(res)->df;
    const bool ok = res->two_channels && RES(res)->df_valid && d->found;
    int i;

    printf("  \"df\": {\n");
    printf("    \"two_channels\": %s,\n", res->two_channels ? "true" : "false");
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        print_json_number("freq_hz", res->set.freq_hz + d->freq_hz, false);
        print_json_number("offset_hz", d->freq_hz, false);
        print_json_number("level_db", d->level_db + rep->off, false);
        print_json_number("snr_db", d->snr_db, false);
        print_json_number("coherence", d->coherence, false);
        print_json_number("gain_db", d->gain_db, false);
        print_json_number("spacing_m", RES(res)->df_spacing_m, false);
        print_json_number("spacing_wavelengths", d->spacing_wl, false);
        print_json_number("phase_raw_deg", d->phase_raw_deg, false);
        print_json_number("calibration_deg", RES(res)->df_cal_deg, false);
        printf("    \"calibration\": \"%s\",\n", RES(res)->df_cal_source == 1 ? "given" : RES(res)->df_cal_source == 2 ? "stored" : "none");
        print_json_number("phase_deg", d->phase_deg, false);
        print_json_number("phase_std_deg", d->phase_std_deg, false);
        print_json_number("angle_deg", d->angle_deg, false);
        print_json_number("angle_std_deg", d->angle_std_deg, false);
        print_json_number("music_peak_deg", d->music_peak_deg, false);
        printf("    \"ambiguous\": %s,\n", d->ambiguous ? "true" : "false");
        printf("    \"candidates_deg\": [");
        for (i = 0; i < d->candidates; i++)
            printf("%s%.3f", i ? ", " : "", d->candidate_deg[i]);
        printf("]\n");
    }
    printf("  }");
}

/* Watch */
/*-------*/

static unsigned df_watch_lines;

static int df_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_df_result *d = &RES(res)->df;
    const bool ok = res->two_channels && RES(res)->df_valid && d->found;
    int i;

    (void)set;
    if (!RES(res)->df_valid) {
        fprintf(stderr, "Direction finding not measured: check --freq and --df-spacing.\n");
        return -1;
    }
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, ok ? "true" : "false");
        if (ok) {
            printf(", \"angle_deg\": %.3f, \"angle_std_deg\": %.3f, \"phase_deg\": %.3f, \"snr_db\": %.1f, \"coherence\": %.4f, "
                   "\"candidates_deg\": [", d->angle_deg, d->angle_std_deg, d->phase_deg, d->snr_db, d->coherence);
            for (i = 0; i < d->candidates; i++)
                printf("%s%.3f", i ? ", " : "", d->candidate_deg[i]);
            printf("]");
        }
        printf("}\n");
    } else {
        if (df_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %8s %6s %9s %6s %9s  %s\n%8s %8s %6s %9s %6s %9s\n", "time", "angle", "+-", "phase", "SNR", "coherence",
                   "candidates", "s", "deg", "deg", "deg", "dB", "");
        if (ok) {
            printf("%8.2f %+8.2f %6.2f %+9.2f %6.1f %9.4f ", now, d->angle_deg, d->angle_std_deg, d->phase_deg, d->snr_db,
                   d->coherence);
            for (i = 0; d->ambiguous && i < d->candidates; i++)
                printf(" %+.1f", d->candidate_deg[i]);
            printf("\n");
        } else {
            printf("%8.2f  no signal (SNR %.1f dB, coherence %.3f)\n", now, d->snr_db, d->coherence);
        }
    }
    return 0;
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

#define DF_COL_BEAM   0xff40d0ffu
#define DF_COL_MUSIC  0xff60e060u
#define DF_COL_CAND   0xa0a0a0ffu
#define DF_COL_ANGLE  0xff4060ffu
#define DF_COL_GRID   0x40ffffffu
#define DF_RANGE_DB   40.0

/* Pseudo-spectra on a half circle: broadside up, RX2 on the right, DF_RANGE_DB at the centre. */
static void df_polar(const struct m2sdr_df_result *d, float side)
{
    ImDrawList *dl = igGetWindowDrawList();
    ImVec2 pos, pts[M2SDR_DF_POINTS];
    const float r = 0.5f * side - 24.0f;
    float cx, cy;
    char text[32];
    int i, k;

    igGetCursorScreenPos(&pos);
    cx = pos.x + 0.5f * side;
    cy = pos.y + 0.5f * side + r * 0.5f;
    /* Rings every 10 dB, rays every 30 degrees. */
    for (k = 1; k <= 4; k++) {
        ImDrawList_PathArcTo(dl, (ImVec2){ cx, cy }, r * k / 4.0f, (float)M_PI, (float)(2.0 * M_PI), 48);
        ImDrawList_PathStroke(dl, DF_COL_GRID, 0, 1.0f);
    }
    for (i = -90; i <= 90; i += 30) {
        const float a = (float)(i * M_PI / 180.0);

        ImDrawList_AddLine(dl, (ImVec2){ cx, cy }, (ImVec2){ cx + r * sinf(a), cy - r * cosf(a) }, DF_COL_GRID, 1.0f);
        snprintf(text, sizeof(text), "%+d", i);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ cx + (r + 6.0f) * sinf(a) - 10.0f, cy - (r + 6.0f) * cosf(a) - 14.0f },
                                0xffc8c8c8u, text, NULL);
    }
    ImDrawList_AddText_Vec2(dl, (ImVec2){ cx - r, cy + 4.0f }, 0xffc8c8c8u, "RX1 side", NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ cx + r - 56.0f, cy + 4.0f }, 0xffc8c8c8u, "RX2 side", NULL);
    if (d) {
        for (k = 0; k < 2; k++) {
            const float *db = k ? d->music_db : d->beamscan_db;

            for (i = 0; i < M2SDR_DF_POINTS; i++) {
                const float a = (float)((i - 90) * M_PI / 180.0);
                float v = (float)((db[i] + DF_RANGE_DB) / DF_RANGE_DB);

                v      = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
                pts[i] = (ImVec2){ cx + r * v * sinf(a), cy - r * v * cosf(a) };
            }
            ImDrawList_AddPolyline(dl, pts, M2SDR_DF_POINTS, k ? DF_COL_MUSIC : DF_COL_BEAM, 0, 2.0f);
        }
        for (i = 0; i < d->candidates; i++) {
            const float a = (float)(d->candidate_deg[i] * M_PI / 180.0);
            const bool main = d->candidate_deg[i] == d->angle_deg;

            ImDrawList_AddLine(dl, (ImVec2){ cx, cy }, (ImVec2){ cx + r * sinf(a), cy - r * cosf(a) },
                               main ? DF_COL_ANGLE : DF_COL_CAND, main ? 2.5f : 1.5f);
        }
    }
    igDummy((ImVec2){ side, 0.5f * side + r * 0.5f + 24.0f });
}

static void df_linear(const struct m2sdr_df_result *d, ImVec2 size)
{
    static float angle[M2SDR_DF_POINTS];
    struct m2sdr_plot plot;
    char text[96];
    int i;

    for (i = 0; i < M2SDR_DF_POINTS; i++)
        angle[i] = (float)(i - 90);
    if (!m2sdr_plot_begin(&plot, "##dflin", size, -90.0, 90.0, M2SDR_PLOT_PLAIN, -DF_RANGE_DB, 3.0,
                          "Beamscan and MUSIC, dB, against the angle (degrees)"))
        return;
    if (d) {
        for (i = 0; i < d->candidates; i++)
            m2sdr_plot_vline(&plot, d->candidate_deg[i], d->candidate_deg[i] == d->angle_deg ? DF_COL_ANGLE : DF_COL_CAND);
        m2sdr_plot_xy(&plot, angle, d->beamscan_db, M2SDR_DF_POINTS, DF_COL_BEAM);
        m2sdr_plot_xy(&plot, angle, d->music_db, M2SDR_DF_POINTS, DF_COL_MUSIC);
        if (plot.hovered) {
            i = (int)lround(plot.mouse_x) + 90;
            i = i < 0 ? 0 : i >= M2SDR_DF_POINTS ? M2SDR_DF_POINTS - 1 : i;
            snprintf(text, sizeof(text), "%+d deg: beamscan %.1f dB, MUSIC %.1f dB", i - 90, d->beamscan_db[i], d->music_db[i]);
            m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
        }
    }
    m2sdr_plot_end(&plot);
}

static void draw_df(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_df_result *d = &RES(res)->df;
    const bool ok = res->two_channels && RES(res)->df_valid && d->found;
    double cm = ui->set.df_spacing_m, khz = ui->set.df_freq_hz;
    ImVec2 avail;
    char text[128];
    float side;

    (void)live;
    if (input_scaled("Spacing (cm, 0: lambda/2)##dfsp", &cm, 0.01, "%.2f")) {
        ui->set.df_spacing_m = cm > 0 ? cm : 0.0;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 300.0f);
    if (input_scaled("Signal (kHz, 0: strongest)##dff", &khz, 1e3, "%.3f")) {
        ui->set.df_freq_hz = khz;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 400.0f);
    if (igButton("Calibrate: source at broadside", (ImVec2){ 0.0f, 0.0f }) && ok) {
        const double cal = m2sdr_df_calibration(d, 0.0);

        if (df_cal_store(res->set.freq_hz, cal) == 0)
            snprintf(ui->message, sizeof(ui->message), "Calibration %+.2f deg at %.3f MHz stored in %.150s", cal,
                     res->set.freq_hz / 1e6, df_cal_file());
        else
            snprintf(ui->message, sizeof(ui->message), "Calibration %+.2f deg used, not stored (no board file)", cal);
        ui->set.df_cal_given = df_cal_file() == NULL;
        ui->set.df_cal_deg   = cal;
        *changed = true;
    }
    if (ui->set.df_cal_given) {
        igSameLine(0.0f, 8.0f);
        if (igSmallButton("Use the stored one")) {
            ui->set.df_cal_given = false;
            *changed = true;
        }
    }
    if (ui->set.agc) {
        igTextColored((ImVec4){ 1.0f, 0.75f, 0.3f, 1.0f }, "AGC on: a gain change of one RX can move its phase; a manual gain keeps the calibration.");
    }
    if (!res->set.df_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->two_channels) {
        igTextUnformatted("Needs both channels: a board, or a file of two channels.", NULL);
        return;
    }
    if (!RES(res)->df_valid) {
        igTextUnformatted("Not measured: the centre frequency and the spacing must be known, the signal in the capture.", NULL);
        return;
    }
    if (ok) {
        snprintf(text, sizeof(text), "Angle %+.1f deg (+-%.1f)%s", d->angle_deg, d->angle_std_deg,
                 d->ambiguous ? ", ambiguous: grey lines" : "");
        igTextColored((ImVec4){ 0.4f, 1.0f, 0.5f, 1.0f }, "%s", text);
    } else {
        igText("No signal stands out on both channels (SNR %.1f dB, coherence %.3f).", d->snr_db, d->coherence);
    }
    igSameLine(0.0f, 16.0f);
    igText("Calibration: %s", df_cal_name(res, text, sizeof(text)));
    igGetContentRegionAvail(&avail);
    legend_item(DF_COL_BEAM, "beamscan", avail.x);
    legend_item(DF_COL_MUSIC, "MUSIC", avail.x);
    legend_item(DF_COL_ANGLE, "angle", avail.x);
    legend_item(DF_COL_CAND, "other candidates", avail.x);
    igNewLine();
    igGetContentRegionAvail(&avail);
    side = 0.45f * avail.x < 1.6f * avail.y ? 0.45f * avail.x : 1.6f * avail.y;
    igBeginGroup();
    df_polar(ok ? d : NULL, side);
    igEndGroup();
    igSameLine(0.0f, 8.0f);
    df_linear(ok ? d : NULL, (ImVec2){ 0.0f, 0.0f });
}

static void draw_df_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_df_result *d = &RES(res)->df;
    char text[96];
    int i, n = 0;

    igSeparatorText("Direction finding");
    if (!res->two_channels || !RES(res)->df_valid || !d->found) {
        igTextUnformatted("No signal.", NULL);
        return;
    }
    if (igBeginTable("##df", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Angle", "%+.2f deg (+-%.2f)", d->angle_deg, d->angle_std_deg);
        label_value("MUSIC peak", "%+.2f deg", d->music_peak_deg);
        for (i = 0; d->ambiguous && i < d->candidates && n < (int)sizeof(text) - 8; i++)
            n += snprintf(text + n, sizeof(text) - (size_t)n, "%s%+.1f", i ? " " : "", d->candidate_deg[i]);
        if (d->ambiguous)
            label_value("Candidates", "%s", text);
        label_value("Signal", "%+.1f kHz, %.1f dBFS", d->freq_hz / 1e3, d->level_db);
        label_value("SNR, coherence", "%.1f dB, %.4f", d->snr_db, d->coherence);
        label_value("Phase", "%+.2f deg (raw %+.2f)", d->phase_deg, d->phase_raw_deg);
        label_value("Calibration", "%s", df_cal_name(res, text, sizeof(text)));
        label_value("Spacing", "%.2f cm, %.3f lambda", 100.0 * RES(res)->df_spacing_m, d->spacing_wl);
        label_value("Gain RX2/RX1", "%+.2f dB", d->gain_db);
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_df = {
    .key          = "df",
    .title        = "Direction",
    .category     = AN_CAT_TWO_CHANNEL,
    .on           = offsetof(struct an_settings, df_on),
    .result_size  = sizeof(struct df_res),
    .valid        = offsetof(struct df_res, df_valid),
    .found        = offsetof(struct df_res, df.found),
    .report_rank  = 71,
    .samples      = df_samples,
    .two_channels = true,
    .analyse_two  = df_analyse,
    .report_text  = df_report_text,
    .report_json  = df_report_json,
    .report_done  = df_report_done,
    .watch_line   = df_watch_line,
    .options      = df_options,
    .option       = df_option,
    .help         = df_help,
    .opened       = df_opened,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_df,
    .results      = draw_df_results,
#endif
};
