/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: sweep, scalar network analysis from TX to RX through a device
 * (|S21|, phase, group delay), with a thru calibration kept per board; and what the lab modes
 * share.
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

#define SWEEP_PERIOD       65536
#define SWEEP_PERIODS      4          /* Captured: three whole ones wherever the capture starts. */
#define SWEEP_POINTS       201
#define SWEEP_LEVEL_DBFS   -10.0

/* Lab, shared */
/*-------------*/

void lab_json_array(const char *name, const double *v, int n, double add, bool last)
{
    int i;

    printf("    \"%s\": [", name);
    for (i = 0; i < n; i++)
        printf("%s%.4f", i ? ", " : "", isfinite(v[i]) ? v[i] + add : 0.0);
    printf("]%s\n", last ? "" : ",");
}

/* Configuration */
/*---------------*/

static void sweep_config(const struct an_settings *set, struct m2sdr_sweep_config *cfg)
{
    double span = set->lab.sweep_span_hz > 0 ? set->lab.sweep_span_hz : 0.8 * set->sample_rate;

    if (span > 0.98 * set->sample_rate)
        span = 0.98 * set->sample_rate;
    memset(cfg, 0, sizeof(*cfg));
    cfg->stimulus   = set->lab.sweep_stimulus == M2SDR_SWEEP_CHIRP ? M2SDR_SWEEP_CHIRP : M2SDR_SWEEP_STEPPED;
    cfg->start_hz   = -0.5 * span;
    cfg->stop_hz    = 0.5 * span;
    cfg->points     = set->lab.sweep_points > 0 ? set->lab.sweep_points : SWEEP_POINTS;
    cfg->period     = SWEEP_PERIOD;
    cfg->level_dbfs = set->lab.sweep_level_dbfs != 0.0 ? set->lab.sweep_level_dbfs : SWEEP_LEVEL_DBFS;
    /* The TX loop is not timed: the delay through the device is not known, see the manual. */
    cfg->timing_known = false;
}

/* Calibration */
/*-------------*/

/* The thru of the board: written by the window, read by the worker. */
static pthread_mutex_t cal_lock = PTHREAD_MUTEX_INITIALIZER;
static struct m2sdr_sweep_cal cal;
static char cal_path[1024];
static bool sweep_stored;          /* --sweep-thru-save: done once. */
static bool sweep_store_failed;    /* It was asked and not done: --report then fails. */

int sweep_cal_default_path(uint64_t dna, char *path, size_t len)
{
    char dir[1024], *slash;

    if (m2sdr_cal_default_path(dna, dir, sizeof(dir)) != 0)
        return -1;
    slash = strrchr(dir, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    return snprintf(path, len, "%.900s/sweep_thru_%014llx.csv", dir, (unsigned long long)dna) < (int)len ? 0 : -1;
}

int sweep_cal_load(const char *path)
{
    struct m2sdr_sweep_cal c;
    char line[256];
    FILE *f;
    int timing = 0, n = 0;

    snprintf(cal_path, sizeof(cal_path), "%.1023s", path);
    f = fopen(path, "r");
    if (!f)
        return -1;
    memset(&c, 0, sizeof(c));
    while (fgets(line, sizeof(line), f)) {
        double a, b, d;

        if (sscanf(line, "# points=%d start_hz=%lf stop_hz=%lf timing=%d delay=%lf", &c.points, &c.start_hz, &c.stop_hz,
                   &timing, &c.delay_samples) == 5)
            continue;
        if (line[0] != '#' && sscanf(line, "%lf,%lf,%lf", &a, &b, &d) == 3 && n < M2SDR_SWEEP_MAX_POINTS)
            c.h[n++] = (float)b + I * (float)d;
    }
    fclose(f);
    c.timing_known = timing != 0;
    c.valid        = c.points >= 2 && c.points <= M2SDR_SWEEP_MAX_POINTS && n == c.points;
    pthread_mutex_lock(&cal_lock);
    cal = c;
    pthread_mutex_unlock(&cal_lock);
    return c.valid ? 0 : -1;
}

int sweep_cal_save(const char *path)
{
    struct m2sdr_sweep_cal c;
    char dir[1024], *p;
    FILE *f;
    int i;

    if (!path)
        path = cal_path;
    pthread_mutex_lock(&cal_lock);
    c = cal;
    pthread_mutex_unlock(&cal_lock);
    if (!c.valid || !path[0])
        return -1;
    /* Directories of the path. */
    if (snprintf(dir, sizeof(dir), "%s", path) < (int)sizeof(dir)) {
        for (p = dir + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                (void)mkdir(dir, 0755);
                *p = '/';
            }
        }
    }
    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "# M2SDR sweep thru calibration: offset_hz,re,im\n");
    fprintf(f, "# points=%d start_hz=%.6f stop_hz=%.6f timing=%d delay=%.6f\n", c.points, c.start_hz, c.stop_hz,
            c.timing_known ? 1 : 0, c.delay_samples);
    for (i = 0; i < c.points; i++)
        fprintf(f, "%.3f,%.9g,%.9g\n", c.start_hz + i * (c.stop_hz - c.start_hz) / (c.points - 1),
                crealf(c.h[i]), cimagf(c.h[i]));
    if (fclose(f) != 0)
        return -1;
    if (path != cal_path)
        snprintf(cal_path, sizeof(cal_path), "%.1023s", path);
    return 0;
}

int sweep_cal_store(const struct m2sdr_sweep_result *thru)
{
    struct m2sdr_sweep_cal c;

    if (m2sdr_sweep_cal_store(thru, &c) != 0)
        return -1;
    pthread_mutex_lock(&cal_lock);
    cal = c;
    pthread_mutex_unlock(&cal_lock);
    return 0;
}

void sweep_cal_clear(void)
{
    pthread_mutex_lock(&cal_lock);
    memset(&cal, 0, sizeof(cal));
    pthread_mutex_unlock(&cal_lock);
}

bool sweep_cal_valid(void)
{
    bool v;

    pthread_mutex_lock(&cal_lock);
    v = cal.valid;
    pthread_mutex_unlock(&cal_lock);
    return v;
}

/* Stimulus and analysis */
/*-----------------------*/

static float complex *sweep_stimulus(const struct an_settings *set, size_t *period)
{
    struct m2sdr_sweep_config cfg;
    float complex *x;

    sweep_config(set, &cfg);
    x = malloc(cfg.period * sizeof(*x));
    if (!x || m2sdr_sweep_stimulus(&cfg, set->sample_rate, x) != 0) {
        free(x);
        return NULL;
    }
    *period = cfg.period;
    return x;
}

static size_t sweep_samples(const struct an_settings *set)
{
    (void)set;
    return (size_t)SWEEP_PERIODS * SWEEP_PERIOD;
}

static void sweep_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    struct m2sdr_sweep_config cfg;
    struct m2sdr_sweep_cal *c = NULL;

    sweep_config(set, &cfg);
    if (!set->lab.sweep_raw) {
        c = malloc(sizeof(*c));
        if (c) {
            pthread_mutex_lock(&cal_lock);
            *c = cal;
            pthread_mutex_unlock(&cal_lock);
            if (!c->valid) {
                free(c);
                c = NULL;
            }
        }
    }
    /* A thru of other points or another span is not applied. */
    res->sweep_valid = m2sdr_sweep_analyse(x, count, set->sample_rate, &cfg, c, &res->sweep) == 0 ||
                       (c && m2sdr_sweep_analyse(x, count, set->sample_rate, &cfg, NULL, &res->sweep) == 0);
    free(c);
    /* --sweep-thru-save: the first sweep found, from its raw response, calibrated or not. */
    if (set->lab.sweep_store && !sweep_stored) {
        sweep_stored = true;
        sweep_store_failed = true;
        if (!res->sweep_valid || !res->sweep.found)
            fprintf(stderr, "No stimulus in the capture: no thru stored.\n");
        else if (sweep_cal_store(&res->sweep) != 0 || sweep_cal_save(NULL) != 0)
            fprintf(stderr, "Could not write the thru to %s.\n", cal_path);
        else {
            fprintf(stderr, "Sweep stored as the thru in %s.\n", cal_path);
            sweep_store_failed = false;
        }
    }
}

/* --report --sweep-thru-save: the command fails when the thru was not stored. */
static int sweep_report_done(const struct an_result *res)
{
    (void)res;
    return sweep_store_failed ? -1 : 0;
}

/* Command line */
/*--------------*/

/* The options of the lab modes in general are here, with the ones of the sweep. */
enum {
    SW_OPT_ON, SW_OPT_TX_ATT, SW_OPT_TX_REF, SW_OPT_STIMULUS_OUT, SW_OPT_STIMULUS, SW_OPT_SPAN, SW_OPT_POINTS,
    SW_OPT_LEVEL, SW_OPT_THRU, SW_OPT_THRU_SAVE, SW_OPT_RAW,
};

static const struct option sweep_options[] = {
    { "sweep",            no_argument,       NULL, SW_OPT_ON },
    { "lab-tx-att",       required_argument, NULL, SW_OPT_TX_ATT },
    { "lab-tx-ref",       required_argument, NULL, SW_OPT_TX_REF },
    { "lab-stimulus-out", required_argument, NULL, SW_OPT_STIMULUS_OUT },
    { "sweep-stimulus",   required_argument, NULL, SW_OPT_STIMULUS },
    { "sweep-span",       required_argument, NULL, SW_OPT_SPAN },
    { "sweep-points",     required_argument, NULL, SW_OPT_POINTS },
    { "sweep-level",      required_argument, NULL, SW_OPT_LEVEL },
    { "sweep-thru",       required_argument, NULL, SW_OPT_THRU },
    { "sweep-thru-save",  no_argument,       NULL, SW_OPT_THRU_SAVE },
    { "sweep-raw",        no_argument,       NULL, SW_OPT_RAW },
    { NULL, 0, NULL, 0 }
};

static const char *stimulus_out, *thru_arg;

static void sweep_defaults(struct an_settings *set)
{
    set->lab.tx_att = AN_LAB_TX_ATT_DB;
}

static int sweep_option(int id, const char *arg, struct an_cli *cli)
{
    struct an_settings *set = cli->set;
    double v;

    switch (id) {
    case SW_OPT_ON: set->sweep_on = true; break;
    case SW_OPT_TX_ATT:
        if (m2sdr_cli_parse_int_range(arg, 0, 89, &set->lab.tx_att) != 0) {
            m2sdr_cli_error("invalid TX attenuation '%s' (expected 0 to 89)", arg);
            return -1;
        }
        break;
    case SW_OPT_TX_REF:
        if (m2sdr_cli_parse_double(arg, &set->lab.tx_ref_db) != 0) {
            m2sdr_cli_error("invalid level '%s'", arg);
            return -1;
        }
        break;
    case SW_OPT_STIMULUS_OUT: stimulus_out = arg; break;
    case SW_OPT_STIMULUS:
        if (strcmp(arg, "stepped") == 0) {
            set->lab.sweep_stimulus = M2SDR_SWEEP_STEPPED;
        } else if (strcmp(arg, "chirp") == 0) {
            set->lab.sweep_stimulus = M2SDR_SWEEP_CHIRP;
        } else {
            m2sdr_cli_invalid_choice("sweep stimulus", arg, "stepped or chirp");
            return -1;
        }
        break;
    case SW_OPT_SPAN:
        if (m2sdr_cli_parse_double(arg, &v) != 0 || !(v > 0)) {
            m2sdr_cli_error("invalid value '%s'", arg);
            return -1;
        }
        set->lab.sweep_span_hz = v;
        break;
    case SW_OPT_POINTS:
        if (m2sdr_cli_parse_int_range(arg, 2, M2SDR_SWEEP_MAX_POINTS, &set->lab.sweep_points) != 0) {
            m2sdr_cli_error("invalid points '%s' (expected 2 to %d)", arg, M2SDR_SWEEP_MAX_POINTS);
            return -1;
        }
        break;
    case SW_OPT_LEVEL:
        if (m2sdr_cli_parse_double(arg, &v) != 0 || !(v < 0.0 && v >= -80.0)) {
            m2sdr_cli_error("invalid level '%s' (expected -80 to 0 dBFS, below 0)", arg);
            return -1;
        }
        set->lab.sweep_level_dbfs = v;
        break;
    case SW_OPT_THRU: thru_arg = arg; break;
    case SW_OPT_THRU_SAVE: set->lab.sweep_store = true; break;
    case SW_OPT_RAW: set->lab.sweep_raw = true; break;
    }
    return 0;
}

/* --lab-stimulus-out: the stimulus only depends on the settings, written before any source. */
static int sweep_start(struct an_cli *cli)
{
    if (cli->set->lab.sweep_store && !cli->set->sweep_on) {
        m2sdr_cli_error("--sweep-thru-save goes with --sweep");
        return -1;
    }
    if (!stimulus_out)
        return 0;
    if (lab_stimulus_write(cli->set, stimulus_out) != 0) {
        m2sdr_cli_error("no stimulus written: give one lab mode (--sweep, --compression, --imd or --amam)");
        return -1;
    }
    return 1;
}

/* Thru of the sweep: the file given, or the one of the board. */
static int sweep_opened(struct an_cli *cli)
{
    static char thru_default[1024];
    struct an_source *src = cli->src;
    const bool store = cli->set->lab.sweep_store;

    if (thru_arg) {
        if (sweep_cal_load(thru_arg) != 0 && !store && cli->set->sweep_on)
            fprintf(stderr, "No thru calibration in %s: raw S21.\n", thru_arg);
    } else if (src->live && (src->dna || m2sdr_get_fpga_dna(src->dev, &src->dna) == M2SDR_ERR_OK) &&
               sweep_cal_default_path(src->dna, thru_default, sizeof(thru_default)) == 0) {
        (void)sweep_cal_load(thru_default);
    } else if (store) {
        m2sdr_cli_error("--sweep-thru-save needs a board or --sweep-thru PATH");
        return -1;
    }
    return 0;
}

static void sweep_help(void)
{
    printf("Lab options (the board transmits a stimulus: a cable and attenuators to the device under\n"
           "test, never an antenna):\n"
           "      --lab-tx-att DB    TX attenuation of the stimulus (default: %d).\n"
           "      --lab-tx-ref DBM   Power at the input of the device of a 0 dBFS tone at 0 dB of\n"
           "                         attenuation: input levels in dBm (default: dBFS of TX).\n"
           "      --lab-stimulus-out PATH   Write the stimulus of the lab mode given (one period, SC16,\n"
           "                         two channels) and exit.\n"
           "\n"
           "Sweep options:\n"
           "      --sweep            With --report: |S21|, phase and group delay over the span.\n"
           "      --sweep-stimulus S stepped or chirp (default: stepped).\n"
           "      --sweep-span HZ    Span, centred (default: 0.8 x the sample rate).\n"
           "      --sweep-points N   Points of the span (default: 201).\n"
           "      --sweep-level DBFS Level of the stimulus (default: -10).\n"
           "      --sweep-thru PATH  Thru calibration (default: one per board,\n"
           "                         ~/.config/litex_m2sdr/sweep_thru_<dna>.csv).\n"
           "      --sweep-thru-save  With --report --sweep: store this sweep as the thru.\n"
           "      --sweep-raw        The thru is not applied.\n"
           "\n", AN_LAB_TX_ATT_DB);
}

/* Group delay: its mean and its peak to peak over the span. */
static void sweep_gd_stats(const struct m2sdr_sweep_result *s, double *mean, double *pp)
{
    double lo = 1e9, hi = -1e9, sum = 0;
    int k;

    for (k = 1; k < s->points - 1; k++) {
        sum += s->group_delay_s[k];
        lo   = fmin(lo, s->group_delay_s[k]);
        hi   = fmax(hi, s->group_delay_s[k]);
    }
    *mean = s->points > 2 ? sum / (s->points - 2) : 0.0;
    *pp   = s->points > 2 ? hi - lo : 0.0;
}

/* Reports */
/*---------*/

static void sweep_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_sweep_result *s = &res->sweep;
    struct m2sdr_sweep_config cfg;
    double mean, pp;
    int k, kmin = 0, kmax = 0;

    (void)rep;
    sweep_config(set, &cfg);
    printf("\nSweep:\n");
    printf("  Stimulus   : %s, %d points from %+.3f to %+.3f MHz, %.1f dBFS, TX attenuation %d dB\n",
           cfg.stimulus == M2SDR_SWEEP_CHIRP ? "chirp" : "stepped tones", cfg.points, cfg.start_hz / 1e6, cfg.stop_hz / 1e6,
           cfg.level_dbfs, set->lab.tx_att);
    if (!res->sweep_valid) {
        printf("  These settings do not make a sweep (span, points or capture).\n");
        return;
    }
    if (!s->found) {
        printf("  The stimulus is not in the capture (correlation %.1f dB): TX to RX through the device?\n", s->sync_quality);
        return;
    }
    for (k = 0; k < s->points; k++) {
        if (s->mag_db[k] < s->mag_db[kmin])
            kmin = k;
        if (s->mag_db[k] > s->mag_db[kmax])
            kmax = k;
    }
    sweep_gd_stats(s, &mean, &pp);
    printf("  Sync       : correlation %.1f dB, %d periods averaged\n", s->sync_quality, s->periods);
    printf("  S21        : %s\n", s->calibrated ? "over the thru stored" : "raw: the gains of TX and RX included (no thru stored)");
    printf("  |S21|      : %.2f dB at the centre, from %.2f dB (%.6f MHz) to %.2f dB (%.6f MHz)\n", s->mag_db[s->points / 2],
           s->mag_min_db, (set->freq_hz + s->freq_hz[kmin]) / 1e6, s->mag_max_db, (set->freq_hz + s->freq_hz[kmax]) / 1e6);
    printf("  Group delay: %s, %.2f ns peak to peak over the span\n",
           s->delay_valid ? "absolute" : "from its mean (the delay is not measured)", pp * 1e9);
    printf("  SNR        : %.1f dB at the centre point\n", s->snr_db[s->points / 2]);
}

static void sweep_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct m2sdr_sweep_result *s = &res->sweep;
    const bool ok = res->sweep_valid && s->found;
    double mean, pp, gd_ns[M2SDR_SWEEP_MAX_POINTS];
    int k;

    (void)rep;
    printf("  \"sweep\": {\n");
    print_json_number("sync_quality_db", res->sweep_valid ? s->sync_quality : 0.0, false);
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        sweep_gd_stats(s, &mean, &pp);
        for (k = 0; k < s->points; k++)
            gd_ns[k] = s->group_delay_s[k] * 1e9;
        printf("    \"calibrated\": %s,\n", s->calibrated ? "true" : "false");
        printf("    \"delay_valid\": %s,\n", s->delay_valid ? "true" : "false");
        printf("    \"points\": %d,\n", s->points);
        printf("    \"periods\": %d,\n", s->periods);
        print_json_number("mag_centre_db", s->mag_db[s->points / 2], false);
        print_json_number("mag_min_db", s->mag_min_db, false);
        print_json_number("mag_max_db", s->mag_max_db, false);
        print_json_number("group_delay_pp_ns", pp * 1e9, false);
        lab_json_array("offset_hz", s->freq_hz, s->points, 0.0, false);
        lab_json_array("mag_db", s->mag_db, s->points, 0.0, false);
        lab_json_array("phase_deg", s->phase_deg, s->points, 0.0, false);
        lab_json_array("group_delay_ns", gd_ns, s->points, 0.0, true);
    }
    printf("  }");
}

#ifdef M2SDR_ANALYZER_GUI

/* Lab, shared: TX controls */
/*--------------------------*/

void lab_tx_controls(struct an_ui *ui, bool live, bool *changed)
{
    igSetNextItemWidth(140.0f);
    if (igSliderInt("TX att (dB)", &ui->set.lab.tx_att, 0, 89, "%d", 0))
        *changed = true;
    same_line_or_wrap(16.0f, 230.0f);
    if (input_scaled("TX ref (dBm)", &ui->set.lab.tx_ref_db, 1.0, "%.1f"))
        *changed = true;
    if (live) {
        igSameLine(0.0f, 16.0f);
        igTextColored((ImVec4){ 1.0f, 0.55f, 0.3f, 1.0f }, "Transmits: a cable and attenuators to the device, never an antenna.");
    }
}

/* View */
/*------*/

static void draw_sweep(struct an_ui *ui, bool live, bool *changed)
{
    static const char *const kinds[] = { "Stepped tones", "Chirp" };
    static float fx[M2SDR_SWEEP_MAX_POINTS], fy[M2SDR_SWEEP_MAX_POINTS];
    const struct an_result *res = &ui->res;
    const struct m2sdr_sweep_result *s = &res->sweep;
    struct m2sdr_plot plot;
    ImVec2 avail;
    double span = ui->set.lab.sweep_span_hz > 0 ? ui->set.lab.sweep_span_hz : 0.8 * ui->set.sample_rate;
    char text[160];
    int k, kind = ui->set.lab.sweep_stimulus, plot_i;

    lab_tx_controls(ui, live, changed);
    igSetNextItemWidth(140.0f);
    if (igCombo_Str_arr("Stimulus", &kind, kinds, 2, -1)) {
        ui->set.lab.sweep_stimulus = kind;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 230.0f);
    if (input_scaled("Span (MHz)", &span, 1e6, "%.3f")) {
        ui->set.lab.sweep_span_hz = span;
        *changed = true;
    }
    same_line_or_wrap(16.0f, 200.0f);
    {
        int points = ui->set.lab.sweep_points > 0 ? ui->set.lab.sweep_points : SWEEP_POINTS;
        igSetNextItemWidth(100.0f);
        if (igInputInt("Points", &points, 10, 100, 0)) {
            ui->set.lab.sweep_points = points < 2 ? 2 : points > 1024 ? 1024 : points;
            *changed = true;
        }
    }
    same_line_or_wrap(16.0f, 230.0f);
    {
        double level = ui->set.lab.sweep_level_dbfs != 0.0 ? ui->set.lab.sweep_level_dbfs : SWEEP_LEVEL_DBFS;
        if (input_scaled("Level (dBFS)", &level, 1.0, "%.1f")) {
            ui->set.lab.sweep_level_dbfs = level < -60.0 ? -60.0 : level > -1.0 ? -1.0 : level;
            *changed = true;
        }
    }
    if (igButton("Store as thru", (ImVec2){ 0.0f, 0.0f }) && res->sweep_valid && s->found) {
        /* From the raw response of the result, calibrated or not. */
        if (sweep_cal_store(s) == 0) {
            if (sweep_cal_save(NULL) == 0)
                snprintf(ui->message, sizeof(ui->message), "Thru stored and written");
            else
                snprintf(ui->message, sizeof(ui->message), "Thru stored (not written to a file)");
        }
    }
    igSameLine(0.0f, 8.0f);
    if (igButton("Clear thru", (ImVec2){ 0.0f, 0.0f }))
        sweep_cal_clear();
    igSameLine(0.0f, 16.0f);
    {
        bool apply = !ui->set.lab.sweep_raw;
        if (igCheckbox("Apply the thru", &apply)) {
            ui->set.lab.sweep_raw = !apply;
            *changed = true;
        }
    }
    igSameLine(0.0f, 16.0f);
    igTextUnformatted(sweep_cal_valid() ? "thru stored" : "no thru stored: S21 holds the radio", NULL);

    if (!res->set.sweep_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->sweep_valid || !s->found) {
        igPushTextWrapPos(0.0f);
        igText("No stimulus in the capture (correlation %.1f dB): TX to RX through the device, the TX attenuation low "
               "enough, the RX gain high enough.", res->sweep_valid ? s->sync_quality : 0.0);
        igPopTextWrapPos();
        return;
    }

    igGetContentRegionAvail(&avail);
    for (k = 0; k < s->points; k++)
        fx[k] = (float)(res->set.freq_hz + s->freq_hz[k]);
    for (plot_i = 0; plot_i < 3; plot_i++) {
        double lo = 1e9, hi = -1e9, pad;
        const char *label;

        for (k = 0; k < s->points; k++) {
            fy[k] = (float)(plot_i == 0 ? s->mag_db[k] : plot_i == 1 ? s->phase_deg[k] : s->group_delay_s[k] * 1e9);
            if (k > 0 && k < s->points - 1) {
                lo = fmin(lo, fy[k]);
                hi = fmax(hi, fy[k]);
            }
        }
        pad = plot_i == 0 ? fmax(1.0, 0.1 * (hi - lo)) : fmax(plot_i == 1 ? 5.0 : 0.5, 0.1 * (hi - lo));
        label = plot_i == 0 ? (s->calibrated ? "|S21|, dB (over the thru)" : "|S21|, dB (raw)")
              : plot_i == 1 ? (s->delay_valid ? "Phase, degrees" : "Phase from its linear part, degrees")
                            : (s->delay_valid ? "Group delay, ns" : "Group delay from its mean, ns");
        snprintf(text, sizeof(text), "##sweep%d", plot_i);
        if (!m2sdr_plot_begin(&plot, text, (ImVec2){ 0.0f, plot_i == 0 ? 0.42f * avail.y : 0.27f * avail.y },
                              fx[0], fx[s->points - 1], M2SDR_PLOT_FREQ, lo - pad, hi + pad, label))
            continue;
        m2sdr_plot_xy(&plot, fx, fy, (size_t)s->points, plot_i == 0 ? 0xff40d0ffu : plot_i == 1 ? 0xffffa040u : 0xff60ff60u);
        if (plot.hovered) {
            int best = 0;
            for (k = 1; k < s->points; k++)
                if (fabs(fx[k] - plot.mouse_x) < fabs(fx[best] - plot.mouse_x))
                    best = k;
            snprintf(text, sizeof(text), "%.6f MHz  %.2f", fx[best] / 1e6, fy[best]);
            m2sdr_plot_text(&plot, -230.0f, 4.0f, text, 0xffffffffu);
            m2sdr_plot_vline(&plot, fx[best], 0x40ffffffu);
        }
        m2sdr_plot_end(&plot);
    }
}

static void draw_sweep_results(struct an_ui *ui)
{
    const struct an_result *res = ui->shown;
    const struct m2sdr_sweep_result *s = &res->sweep;
    double mean, pp;

    igSeparatorText("Sweep");
    if (!res->sweep_valid || !s->found) {
        igTextUnformatted("No stimulus.", NULL);
        return;
    }
    sweep_gd_stats(s, &mean, &pp);
    if (igBeginTable("##sweep", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("S21", "%s", s->calibrated ? "over the thru" : "raw");
        label_value("At the centre", "%.2f dB", s->mag_db[s->points / 2]);
        label_value("Min, max", "%.2f, %.2f dB", s->mag_min_db, s->mag_max_db);
        label_value("Group delay", "%.2f ns p-p", pp * 1e9);
        label_value("Correlation", "%.1f dB", s->sync_quality);
        label_value("SNR, centre", "%.1f dB", s->snr_db[s->points / 2]);
        label_value("Periods", "%d", s->periods);
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_sweep = {
    .key          = "sweep",
    .title        = "Sweep",
    .category     = AN_CAT_LAB,
    .on           = offsetof(struct an_settings, sweep_on),
    .valid        = offsetof(struct an_result, sweep_valid),
    .report_rank  = 40,
    .psd_short    = true,
    .defaults     = sweep_defaults,
    .options      = sweep_options,
    .option       = sweep_option,
    .help         = sweep_help,
    .start        = sweep_start,
    .opened       = sweep_opened,
    .samples      = sweep_samples,
    .analyse      = sweep_analyse,
    .stimulus     = sweep_stimulus,
    .report_text  = sweep_report_text,
    .report_json  = sweep_report_json,
    .report_done  = sweep_report_done,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_sweep,
    .results      = draw_sweep_results,
#endif
};
