/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: frequency stability of a carrier (offset, drift, Allan
 * deviation), in a report, a view and --watch.
 *
 * Each capture gives a fine frequency and phase of the carrier, and the Allan deviation inside
 * it from the phase of its sub-blocks. The mean frequencies of the captures of one tuning are
 * kept in a series, with the time of each capture (an_capture_time): from a file, its sample
 * count, and consecutive captures are contiguous; from a board, its timestamps or else the host
 * clock, and the captures have dead time between them (the deviation of white FM then reads
 * sqrt(tau0 / capture) high).
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

#define ADEV_MIN_SEARCH_HZ  2000.0   /* Around a nominal carrier given. */
#define ADEV_SEARCH_PPM     20.0     /* ... or this much of it, if more. */
#define ADEV_GAP            3.0      /* A spacing this many times the last one: a pause, the series restarts. */

/* Series */
/*--------*/

/* The series of the captures of one tuning. The analyses run one after the other (the window
 * captures the next one while one is analysed, never analyses two at once), so it is kept
 * here, without a lock. */
static struct {
    struct m2sdr_adev_series s;
    bool   init;
    double freq_hz, rate, carrier_hz, nominal_hz;
    int    channel;
    enum an_clock clock;
    double first_t, last_t, last_dt;
} adev_state;

static volatile int adev_reset_wanted;     /* From the view. */

static void adev_series_restart(const struct an_settings *set, enum an_clock clock, double nominal_hz)
{
    if (adev_state.init)
        m2sdr_adev_series_free(&adev_state.s);
    adev_state.init = m2sdr_adev_series_init(&adev_state.s, set->sample_rate, nominal_hz, nominal_hz - set->freq_hz,
                                             AN_ADEV_HISTORY) == 0;
    adev_state.freq_hz    = set->freq_hz;
    adev_state.rate       = set->sample_rate;
    adev_state.channel    = set->channel;
    adev_state.carrier_hz = set->adev_carrier_hz;
    adev_state.nominal_hz = nominal_hz;
    adev_state.clock      = clock;
    adev_state.last_dt    = 0.0;
}

/* Analysis */
/*----------*/

static size_t adev_samples(const struct an_settings *set)
{
    (void)set;
    return AN_ADEV_SAMPLES;
}

static void adev_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    struct m2sdr_carrier_config cfg;
    const size_t n = count < AN_ADEV_SAMPLES ? count : AN_ADEV_SAMPLES;
    const double t_s = res->capture.t_s;
    const int64_t t0 = (int64_t)llround(t_s * set->sample_rate);
    struct m2sdr_carrier_result *c = &res->adev;
    bool restart;
    size_t k;

    memset(&cfg, 0, sizeof(cfg));
    if (set->adev_carrier_hz > 0.0) {
        cfg.carrier_hz = set->adev_carrier_hz;
        cfg.offset_hz  = set->adev_carrier_hz - set->freq_hz;
        cfg.search_hz  = fmax(ADEV_MIN_SEARCH_HZ, ADEV_SEARCH_PPM * 1e-6 * set->adev_carrier_hz);
    }
    res->adev_series_ok = false;
    res->adev_npoints   = 0;
    res->adev_valid     = m2sdr_carrier_estimate(x, n, set->sample_rate, t0, cfg.carrier_hz > 0.0 ? &cfg : NULL, c) == 0;
    if (!res->adev_valid || !c->found)
        return;

    /* A new series on a new tuning, a pause (or a file read again from its start), or when asked. */
    restart = !adev_state.init || adev_reset_wanted || adev_state.freq_hz != set->freq_hz ||
              adev_state.rate != set->sample_rate || adev_state.channel != set->channel ||
              adev_state.carrier_hz != set->adev_carrier_hz || adev_state.clock != res->capture.clock ||
              t_s <= adev_state.last_t ||
              (adev_state.last_dt > 0.0 && t_s - adev_state.last_t > ADEV_GAP * adev_state.last_dt);
    if (restart) {
        /* Without a nominal frequency, the first carrier found is the reference of the offsets. */
        const double nominal = set->adev_carrier_hz > 0.0 ? set->adev_carrier_hz : set->freq_hz + c->freq_mean_hz;

        adev_reset_wanted = 0;
        adev_series_restart(set, res->capture.clock, nominal);
        adev_state.first_t = t_s;
    } else {
        adev_state.last_dt = t_s - adev_state.last_t;
    }
    adev_state.last_t = t_s;
    res->adev_nominal_hz    = adev_state.nominal_hz;
    res->adev_nominal_given = set->adev_carrier_hz > 0.0;
    if (!adev_state.init)
        return;
    m2sdr_adev_series_add(&adev_state.s, c->timestamp, c->freq_mean_hz);
    res->adev_series_ok = m2sdr_adev_series_compute(&adev_state.s, &res->adev_series) == 0;

    /* History, oldest first. */
    {
        const struct m2sdr_adev_series *s = &adev_state.s;
        const size_t first = s->count == s->capacity ? s->head : 0;
        const double t_first = s->timestamp[first];

        for (k = 0; k < s->count && k < AN_ADEV_HISTORY; k++) {
            const size_t i = (first + k) % s->capacity;

            res->adev_t_s[k] = (float)((s->timestamp[i] - t_first) / set->sample_rate);
            res->adev_ppb[k] = (float)((s->freq_hz[i] - s->offset_hz) / s->carrier_hz * 1e9);
        }
        res->adev_npoints = (int)k;
    }
}

/* ADEV of the series at the octave of tau nearest to tau_s, within a factor sqrt(2); NAN if none. */
static double adev_at(const struct m2sdr_adev *d, double tau_s, double *tau_found)
{
    int i;

    for (i = 0; i < d->ntau; i++) {
        if (fabs(log2(d->tau_s[i] / tau_s)) <= 0.5) {
            if (tau_found)
                *tau_found = d->tau_s[i];
            return d->adev[i];
        }
    }
    return NAN;
}

static const char *adev_clock_name(enum an_clock clock)
{
    switch (clock) {
    case AN_CLOCK_SAMPLES: return "sample count of the file, contiguous captures";
    case AN_CLOCK_BOARD:   return "timestamps of the board, dead time between the captures";
    case AN_CLOCK_HOST:    return "host clock, dead time between the captures";
    default:               return "none";
    }
}

/* Reports */
/*---------*/

static void adev_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_carrier_result *c = &res->adev;
    const struct m2sdr_adev *d = &c->short_tau;
    int i;

    printf("\nCarrier stability:\n");
    if (!res->adev_valid) {
        printf("  The capture is too short (1024 samples at least).\n");
        return;
    }
    if (!c->found) {
        printf("  No carrier found%s.\n", set->adev_carrier_hz > 0.0 ? " near the nominal frequency" : "");
        return;
    }
    printf("  Carrier    : %.6f MHz (%+.4f Hz from the centre), %.1f %s, SNR %.1f dB, C/N0 %.1f dB-Hz\n",
           (set->freq_hz + c->freq_hz) / 1e6, c->freq_hz, c->level_dbfs + rep->off, rep->unit, c->snr_db,
           c->cn0_dbhz);
    printf("  Frequency  : fitted %+.6f Hz (+-%.2g Hz), mean over the capture %+.6f Hz, phase %+.4f rad\n", c->freq_hz, c->freq_std_hz,
           c->freq_mean_hz, c->phase_rad);
    if (set->adev_carrier_hz > 0.0)
        printf("  Offset     : %+.3f ppb from %.6f MHz, against the reference of the board\n", c->offset_ppb,
               set->adev_carrier_hz / 1e6);
    printf("  Short tau  : %d sub-blocks of %zu samples (phase error %.2g rad RMS)\n", c->blocks, c->block_len,
           c->phase_rms_rad);
    printf("  %12s %12s %12s\n", "tau (s)", "ADEV", "MDEV");
    for (i = 0; i < d->ntau; i++)
        printf("  %12.4g %12.3e %12.3e\n", d->tau_s[i], d->adev[i], d->mdev[i]);
    if (res->adev_series_ok) {
        const struct m2sdr_adev_result *r = &res->adev_series;

        printf("  Series     : %zu captures, tau0 %.4g s, %.1f s, offset %+.3f ppb, drift %+.4f ppb/s\n", r->points,
               r->tau0_s, r->span_s, r->offset_ppb, r->drift_ppb_s);
        for (i = 0; i < r->dev.ntau; i++)
            printf("  %12.4g %12.3e %12.3e\n", r->dev.tau_s[i], r->dev.adev[i], r->dev.mdev[i]);
    }
}

static void print_json_dev(const char *name, const struct m2sdr_adev *d, bool last)
{
    int i;

    printf("    \"%s\": [", name);
    for (i = 0; i < d->ntau; i++)
        printf("%s{\"tau_s\": %.6g, \"adev\": %.6g, \"mdev\": %.6g, \"terms\": %d}", i ? ", " : "", d->tau_s[i], d->adev[i],
               d->mdev[i], d->terms[i]);
    printf("]%s\n", last ? "" : ",");
}

static void adev_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const struct m2sdr_carrier_result *c = &res->adev;
    const bool ok = res->adev_valid && c->found;

    printf("  \"adev\": {\n");
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        print_json_number("carrier_hz", set->freq_hz + c->freq_hz, false);
        print_json_number("carrier_offset_hz", c->freq_hz, false);
        print_json_number("freq_mean_hz", c->freq_mean_hz, false);
        print_json_number("freq_std_hz", c->freq_std_hz, false);
        print_json_number("phase_rad", c->phase_rad, false);
        print_json_number("timestamp", c->timestamp, false);
        print_json_number("level_db", c->level_dbfs + rep->off, false);
        print_json_number("snr_db", c->snr_db, false);
        print_json_number("cn0_dbhz", c->cn0_dbhz, false);
        if (set->adev_carrier_hz > 0.0) {
            print_json_number("nominal_hz", set->adev_carrier_hz, false);
            print_json_number("offset_ppb", c->offset_ppb, false);
        }
        printf("    \"block_len\": %zu,\n", c->block_len);
        if (res->adev_series_ok) {
            const struct m2sdr_adev_result *r = &res->adev_series;

            printf("    \"series_points\": %zu,\n", r->points);
            print_json_number("series_tau0_s", r->tau0_s, false);
            print_json_number("series_offset_ppb", r->offset_ppb, false);
            print_json_number("series_drift_ppb_s", r->drift_ppb_s, false);
            print_json_dev("series", &r->dev, false);
        }
        print_json_dev("short_tau", &c->short_tau, true);
    }
    printf("  }");
}

/* Watch */
/*-------*/

static unsigned adev_watch_lines;

static int adev_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    static const double taus[] = { 1.0, 10.0, 100.0 };
    const struct m2sdr_carrier_result *c = &res->adev;
    const bool ok = res->adev_valid && c->found;
    const double ppb = ok ? (set->freq_hz + c->freq_hz - res->adev_nominal_hz) / res->adev_nominal_hz * 1e9 : 0.0;
    double a[3];
    unsigned i;

    for (i = 0; i < 3; i++)
        a[i] = res->adev_series_ok ? adev_at(&res->adev_series.dev, taus[i], NULL) : NAN;
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, ok ? "true" : "false");
        if (ok) {
            printf(", \"carrier_offset_hz\": %.6f, \"offset_ppb\": %.4f, \"nominal_given\": %s, \"snr_db\": %.1f",
                   c->freq_hz, ppb, res->adev_nominal_given ? "true" : "false", c->snr_db);
            if (res->adev_series_ok)
                printf(", \"drift_ppb_s\": %.5f, \"points\": %zu", res->adev_series.drift_ppb_s, res->adev_series.points);
            for (i = 0; i < 3; i++)
                if (isfinite(a[i]))
                    printf(", \"adev_%gs\": %.4g", taus[i], a[i]);
        }
        printf("}\n");
    } else {
        if (adev_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %14s %10s %10s %6s %10s %10s %10s\n%8s %14s %10s %10s %6s %10s %10s %10s\n",
                   "time", "offset", "offset", "drift", "SNR", "ADEV", "ADEV", "ADEV",
                   "s", "Hz", res->adev_nominal_given ? "ppb" : "ppb rel.", "ppb/s", "dB", "1 s", "10 s", "100 s");
        if (ok) {
            printf("%8.2f %+14.6f %+10.3f", now, c->freq_hz, ppb);
            if (res->adev_series_ok)
                printf(" %+10.4f", res->adev_series.drift_ppb_s);
            else
                printf(" %10s", "-");
            printf(" %6.1f", c->snr_db);
            for (i = 0; i < 3; i++) {
                if (isfinite(a[i]))
                    printf(" %10.3e", a[i]);
                else
                    printf(" %10s", "-");
            }
            printf("\n");
        } else {
            printf("%8.2f  no carrier\n", now);
        }
    }
    return 0;
}

/* Command line */
/*--------------*/

static const struct option adev_options[] = {
    { "adev",         no_argument,       NULL, 0 },
    { "adev-carrier", required_argument, NULL, 1 },
    { NULL, 0, NULL, 0 }
};

static int adev_option(int id, const char *arg, struct an_cli *cli)
{
    switch (id) {
    case 0:
        cli->set->adev_on = true;
        return 0;
    case 1:
        if (m2sdr_cli_parse_double(arg, &cli->set->adev_carrier_hz) != 0 || cli->set->adev_carrier_hz < 0) {
            m2sdr_cli_error("invalid carrier frequency '%s'", arg);
            return -1;
        }
        return 0;
    default:
        return -1;
    }
}

static void adev_help(void)
{
    printf("Allan deviation options:\n"
           "      --adev             With --report: frequency, phase and Allan deviation of the strongest\n"
           "                         carrier (inside the capture; over the captures with --watch).\n"
           "      --adev-carrier HZ  Its nominal RF frequency: offsets in ppb from it, searched near it.\n"
           "\n");
}

#ifdef M2SDR_ANALYZER_GUI

/* View */
/*------*/

/* Deviation traces in log10, with the extent of what is drawn. */
static int dev_log(const struct m2sdr_adev *d, const double *dev, float *lx, float *ly, double *ymin, double *ymax,
                   double *xmin, double *xmax)
{
    int i, n = 0;

    for (i = 0; i < d->ntau; i++) {
        if (!(dev[i] > 0.0))
            continue;
        lx[n] = (float)log10(d->tau_s[i]);
        ly[n] = (float)log10(dev[i]);
        *xmin = fmin(*xmin, lx[n]);
        *xmax = fmax(*xmax, lx[n]);
        *ymin = fmin(*ymin, ly[n]);
        *ymax = fmax(*ymax, ly[n]);
        n++;
    }
    return n;
}

static void draw_adev(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_carrier_result *c = &res->adev;
    const bool ok = res->adev_valid && c->found;
    static float sx[M2SDR_ADEV_MAX_TAUS], sy[M2SDR_ADEV_MAX_TAUS], ax[M2SDR_ADEV_MAX_TAUS], ay[M2SDR_ADEV_MAX_TAUS];
    static float mx[M2SDR_ADEV_MAX_TAUS], my[M2SDR_ADEV_MAX_TAUS];
    struct m2sdr_plot plot;
    ImVec2 avail;
    char text[200];
    int ns = 0, na = 0, nm = 0, i;

    (void)live;
    if (input_scaled("Nominal carrier (MHz)", &ui->set.adev_carrier_hz, 1e6, "%.6f"))
        *changed = true;
    same_line_or_wrap(16.0f, 140.0f);
    if (igButton("Restart series", (ImVec2){ 0.0f, 0.0f }))
        adev_reset_wanted = 1;
    if (ok && ui->set.adev_carrier_hz <= 0.0) {
        same_line_or_wrap(16.0f, 200.0f);
        snprintf(text, sizeof(text), "Nominal: %.3f MHz", 1e-6 * round(res->set.freq_hz + c->freq_mean_hz));
        if (igSmallButton(text)) {
            ui->set.adev_carrier_hz = round(res->set.freq_hz + c->freq_mean_hz);
            *changed = true;
        }
    }
    if (!res->set.adev_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!ok) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted(res->set.adev_carrier_hz > 0.0
                          ? "No carrier near the nominal frequency: check the tuning, or clear the nominal to search the capture."
                          : "No carrier stands out of the capture (the centre is left out): tune a little off the carrier.",
                          NULL);
        igPopTextWrapPos();
        return;
    }
    igPushTextWrapPos(0.0f);
    igTextDisabled("Against the reference of the board: the offset and the deviation hold both. Time: %s%s.",
                   adev_clock_name(res->capture.clock),
                   res->adev_series_ok && !res->adev_series.regular ? ", spacing irregular" : "");
    igPopTextWrapPos();

    igGetContentRegionAvail(&avail);
    /* Offset over time. */
    if (res->adev_npoints >= 2) {
        double lo = 1e300, hi = -1e300, pad;

        for (i = 0; i < res->adev_npoints; i++) {
            lo = fmin(lo, res->adev_ppb[i]);
            hi = fmax(hi, res->adev_ppb[i]);
        }
        pad = fmax(0.1 * (hi - lo), 1e-3);
        snprintf(text, sizeof(text), "Frequency offset, ppb %s, against time",
                 res->adev_nominal_given ? "from the nominal" : "from the first capture");
        if (m2sdr_plot_begin(&plot, "##adevppb", (ImVec2){ avail.x, 0.35f * avail.y }, 0.0,
                             fmax(res->adev_t_s[res->adev_npoints - 1], 1e-3), M2SDR_PLOT_TIME, lo - pad, hi + pad, text)) {
            m2sdr_plot_xy(&plot, res->adev_t_s, res->adev_ppb, (size_t)res->adev_npoints, 0xff40d0ffu);
            m2sdr_plot_end(&plot);
        }
    } else {
        igTextUnformatted("Offset over time: from the third capture.", NULL);
    }

    /* Deviations, log-log, with the slopes of the power-law noises through the first point. */
    {
        double ymin = 1e300, ymax = -1e300, xmin = 1e300, xmax = -1e300;

        ns = dev_log(&c->short_tau, c->short_tau.adev, sx, sy, &ymin, &ymax, &xmin, &xmax);
        if (res->adev_series_ok) {
            na = dev_log(&res->adev_series.dev, res->adev_series.dev.adev, ax, ay, &ymin, &ymax, &xmin, &xmax);
            nm = dev_log(&res->adev_series.dev, res->adev_series.dev.mdev, mx, my, &ymin, &ymax, &xmin, &xmax);
        }
        if (ns + na == 0)
            return;
        xmin = floor(xmin) - 0.2;
        xmax = ceil(xmax) + 0.2;
        ymin = floor(ymin) - 0.5;
        ymax = ceil(ymax) + 0.5;
        if (!m2sdr_plot_begin(&plot, "##adevdev", (ImVec2){ 0.0f, 0.0f }, xmin, xmax, M2SDR_PLOT_PLAIN, ymin, ymax,
                              "log10 deviation against log10 tau (s)"))
            return;
        {
            static const struct { double slope; const char *name; } guides[] = {
                { -1.0, "-1 white PM" }, { -0.5, "-1/2 white FM" }, { 0.0, "0 flicker FM" }, { 0.5, "+1/2 RW FM" },
            };
            const float x0 = na ? ax[0] : sx[0], y0 = na ? ay[0] : sy[0];
            unsigned g;

            for (g = 0; g < sizeof(guides) / sizeof(guides[0]); g++) {
                const double k = guides[g].slope;
                /* To the right edge, or to the top or the bottom if it gets there first. */
                const double xe = k > 0.0 ? fmin(xmax, x0 + (ymax - y0) / k) : k < 0.0 ? fmin(xmax, x0 + (ymin - y0) / k) : xmax;
                const float gx[2] = { x0, (float)xe };
                const float gy[2] = { y0, (float)(y0 + k * (xe - x0)) };
                const double xl = x0 + 0.7 * (xe - x0);

                m2sdr_plot_xy(&plot, gx, gy, 2, 0x40ffffffu);
                ImDrawList_AddText_Vec2(plot.dl, (ImVec2){ m2sdr_plot_x(&plot, xl) + 4.0f,
                                        m2sdr_plot_y(&plot, y0 + k * (xl - x0)) - 16.0f }, 0x90c8c8c8u, guides[g].name, NULL);
            }
        }
        if (ns >= 1) {
            m2sdr_plot_xy(&plot, sx, sy, (size_t)ns, 0xffffa040u);
            m2sdr_plot_points(&plot, sx, sy, (size_t)ns, 2.5f, 0xffffa040u);
        }
        if (nm >= 1)
            m2sdr_plot_xy(&plot, mx, my, (size_t)nm, 0xff80ff80u);
        if (na >= 1) {
            m2sdr_plot_xy(&plot, ax, ay, (size_t)na, 0xff40d0ffu);
            m2sdr_plot_points(&plot, ax, ay, (size_t)na, 3.0f, 0xff40d0ffu);
        }
        m2sdr_plot_text(&plot, 8.0f, -20.0f, "yellow: ADEV over captures, green: MDEV, blue: ADEV inside the capture",
                        0xffc8c8c8u);
        if (plot.hovered) {
            snprintf(text, sizeof(text), "tau %.3g s  %.3g", pow(10.0, plot.mouse_x), pow(10.0, plot.mouse_y));
            m2sdr_plot_text(&plot, -220.0f, 4.0f, text, 0xffffffffu);
        }
        m2sdr_plot_end(&plot);
    }
}

static void draw_adev_results(struct an_ui *ui)
{
    static const double taus[] = { 0.1, 1.0, 10.0, 100.0 };
    const struct an_result *res = &ui->res;
    const struct m2sdr_carrier_result *c = &res->adev;
    unsigned i;

    igSeparatorText("Carrier stability");
    if (!res->adev_valid || !c->found) {
        igTextUnformatted("No carrier.", NULL);
        return;
    }
    if (igBeginTable("##adev", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Carrier", "%.6f MHz", (res->set.freq_hz + c->freq_hz) / 1e6);
        label_value("From the centre", "%+.4f Hz", c->freq_hz);
        label_value("Level", "%.1f dBFS", c->level_dbfs);
        label_value("SNR, C/N0", "%.1f dB, %.1f dB-Hz", c->snr_db, c->cn0_dbhz);
        label_value("Resolution", "%.2g Hz", c->freq_std_hz);
        label_value("Offset", "%+.3f ppb%s", (res->set.freq_hz + c->freq_hz - res->adev_nominal_hz) /
                    res->adev_nominal_hz * 1e9, res->adev_nominal_given ? "" : " (from the first)");
        if (res->adev_series_ok) {
            const struct m2sdr_adev_result *r = &res->adev_series;

            label_value("Drift", "%+.4f ppb/s", r->drift_ppb_s);
            label_value("Captures", "%zu over %.1f s", r->points, r->span_s);
            label_value("tau0", "%.4g s%s", r->tau0_s, r->regular ? "" : ", irregular");
            for (i = 0; i < sizeof(taus) / sizeof(taus[0]); i++) {
                double tau = 0.0;
                const double a = adev_at(&r->dev, taus[i], &tau);
                char name[32];

                if (!isfinite(a))
                    continue;
                snprintf(name, sizeof(name), "ADEV %.3g s", tau);
                label_value(name, "%.3e", a);
            }
        }
        if (c->short_tau.ntau > 0)
            label_value("ADEV inside", "%.3e at %.3g s", c->short_tau.adev[0], c->short_tau.tau_s[0]);
        igEndTable();
    }
}

#endif /* M2SDR_ANALYZER_GUI */

/* Mode */
/*------*/

const struct an_mode an_mode_adev = {
    .key          = "adev",
    .title        = "Allan deviation",
    .category     = AN_CAT_TIMING,
    .on           = offsetof(struct an_settings, adev_on),
    .valid        = offsetof(struct an_result, adev_valid),
    .report_rank  = 70,
    .options      = adev_options,
    .option       = adev_option,
    .help         = adev_help,
    .samples      = adev_samples,
    .analyse      = adev_analyse,
    .report_text  = adev_report_text,
    .report_json  = adev_report_json,
    .watch_line   = adev_watch_line,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_adev,
    .results      = draw_adev_results,
#endif
};
