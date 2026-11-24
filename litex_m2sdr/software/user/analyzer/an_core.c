/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: capture length, analysis of a capture by the spectrum and the modes
 * on, floor of the phase noise, text and JSON reports, --report and --watch.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <complex.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "m2sdr_cal.h"

#include "analyzer.h"

/* Settings */
/*----------*/

const char *const window_names[] = { "Hann", "Flat-top", "Blackman-Harris", "Rectangular" };

/* Results */
/*---------*/

static void *empty_result;         /* Zeroed, of the largest result_size: the result of a mode not measured. */

int an_registry_init(void)
{
    size_t size = 1;
    int m;

    for (m = 0; an_modes[m]; m++) {
        if (m >= AN_MODES_MAX) {
            fprintf(stderr, "More than %d views and modes in analyzer/modes.h.\n", AN_MODES_MAX);
            return -1;
        }
        if ((an_modes[m]->analyse || an_modes[m]->analyse_two) && !an_modes[m]->result_size) {
            fprintf(stderr, "The mode %s analyses without a result_size.\n", an_modes[m]->key);
            return -1;
        }
        if (an_modes[m]->result_size > size)
            size = an_modes[m]->result_size;
    }
    empty_result = calloc(1, size);
    if (!empty_result) {
        fprintf(stderr, "Out of memory\n");
        return -1;
    }
    return 0;
}

int an_mode_index(const struct an_mode *m)
{
    int i;

    for (i = 0; an_modes[i]; i++) {
        if (an_modes[i] == m)
            return i;
    }
    return -1;
}

void *an_mode_result(const struct an_mode *m, const struct an_result *res)
{
    const int i = an_mode_index(m);

    return i >= 0 && res->mode[i] ? res->mode[i] : empty_result;
}

bool an_mode_result_flag(const struct an_mode *m, const struct an_result *res, size_t off)
{
    return *(const bool *)((const char *)an_mode_result(m, res) + off);
}

int an_result_copy(struct an_result *dst, const struct an_result *src)
{
    float *power = dst->psd.power;
    void *mode[AN_MODES_MAX];
    int m;

    /* The blocks of the modes of src, before anything of dst changes. */
    for (m = 0; an_modes[m]; m++) {
        if (src->mode[m] && !dst->mode[m] && !(dst->mode[m] = calloc(1, an_modes[m]->result_size)))
            return -1;
    }
    if (dst->psd.bins != src->psd.bins) {
        power = realloc(power, src->psd.bins * sizeof(*power));
        if (!power)
            return -1;
    }
    memcpy(mode, dst->mode, sizeof(mode));
    *dst = *src;
    dst->psd.power = power;
    dst->psd.fft   = NULL;
    dst->psd.win   = NULL;
    dst->psd.work  = NULL;
    memset(&dst->tone_psd, 0, sizeof(dst->tone_psd));
    memcpy(power, src->psd.power, src->psd.bins * sizeof(*power));
    for (m = 0; an_modes[m]; m++) {
        dst->mode[m] = src->mode[m] ? mode[m] : NULL;
        if (src->mode[m])
            memcpy(mode[m], src->mode[m], an_modes[m]->result_size);
        else
            free(mode[m]);
    }
    return 0;
}

void an_result_free(struct an_result *res)
{
    int m;

    m2sdr_psd_free(&res->psd);
    m2sdr_psd_free(&res->tone_psd);
    for (m = 0; m < AN_MODES_MAX; m++) {
        free(res->mode[m]);
        res->mode[m] = NULL;
    }
}

/* Analysis */
/*----------*/

size_t capture_samples(const struct an_settings *set)
{
    size_t count = (size_t)set->fft_len * ((size_t)set->averages + 1) / 2;
    int m;

    if (count < (size_t)set->fft_len)
        count = (size_t)set->fft_len;
    /* The longest capture of the modes on. */
    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->samples && an_mode_on(an_modes[m], set) && count < an_modes[m]->samples(set))
            count = an_modes[m]->samples(set);
    }
    return count < AN_TIME_POINTS ? AN_TIME_POINTS : count;
}

/* A mode on has a long capture: the spectrum keeps its own averages of it, for speed. */
static bool psd_short(const struct an_settings *set)
{
    int m;

    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->psd_short && an_mode_on(an_modes[m], set))
            return true;
    }
    return false;
}

/* A mode of two channels is on: both are captured. */
bool capture_two_channels(const struct an_settings *set)
{
    int m;

    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->two_channels && an_mode_on(an_modes[m], set))
            return true;
    }
    return false;
}

int analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    return analyse_two(x, NULL, count, set, res);
}

int analyse_two(const float complex *x, const float complex *x2, size_t count, const struct an_settings *set,
                struct an_result *res)
{
    const double half = 0.5 * set->sample_rate;
    const size_t spectrum = (size_t)set->fft_len * ((size_t)set->averages + 1) / 2;
    const size_t psd_count = psd_short(set) && spectrum >= (size_t)set->fft_len && spectrum < count ? spectrum : count;
    int m;

    res->set          = *set;
    res->two_channels = x2 != NULL;
    if (m2sdr_psd_compute(x, psd_count, set->sample_rate, (size_t)set->fft_len, (enum m2sdr_window)set->window, &res->psd) != 0)
        return -1;
    (void)m2sdr_iq_stats(x, count, 0.999, &res->stats);
    res->noise_db_hz = m2sdr_psd_noise_density_db(&res->psd, -half, half);
    /* Peaks and tone: only what stands out of the noise. On a wide signal, the highest bins are
     * its own ripple. */
    {
        const double floor_db = res->noise_db_hz + 10.0 * log10(res->psd.rbw_hz) + AN_PEAK_MIN_ABOVE_DB;
        int n = m2sdr_psd_peaks(&res->psd, -half, half, AN_PEAK_EXCURSION_DB, res->peaks, AN_MAX_PEAKS);

        while (n > 0 && res->peaks[n - 1].power_db < floor_db)
            n--;
        res->npeaks     = n;
        struct m2sdr_psd_peak top;

        res->tone_valid = m2sdr_psd_peak(&res->psd, -half, half, &top) == 0 && top.power_db >= floor_db;
        if (res->tone_valid) {
            /* The skirts of a Hann or flat-top window would count as spurs and noise next to a
             * strong tone: its figures are taken with a Blackman-Harris window, whatever the
             * one of the trace. */
            const struct m2sdr_psd *psd = &res->psd;

            if (set->window != M2SDR_WINDOW_BLACKMAN_HARRIS) {
                psd = &res->tone_psd;
                if (m2sdr_psd_compute(x, psd_count, set->sample_rate, (size_t)set->fft_len,
                                      M2SDR_WINDOW_BLACKMAN_HARRIS, &res->tone_psd) != 0)
                    psd = NULL;
            }
            res->tone_valid = psd && m2sdr_psd_tone(psd, &res->tone) == 0;
        }
    }

    res->chan_valid = res->acpr_valid = res->obw_valid = false;
    if (set->chan_bw_hz > 0) {
        const double lo = set->chan_offset_hz - 0.5 * set->chan_bw_hz;
        const double hi = set->chan_offset_hz + 0.5 * set->chan_bw_hz;
        const double spacing = set->chan_spacing_hz > 0 ? set->chan_spacing_hz : set->chan_bw_hz;

        if (lo >= -half && hi <= half) {
            res->chan_valid    = true;
            res->chan_power_db = m2sdr_psd_band_power_db(&res->psd, lo, hi);
            res->acpr_valid    = m2sdr_psd_acpr(&res->psd, set->chan_offset_hz, set->chan_bw_hz, spacing, &res->acpr) == 0;
            /* The occupied bandwidth is searched in the channel and half of it on each side. */
            res->obw_valid     = m2sdr_psd_obw(&res->psd, lo - 0.5 * set->chan_bw_hz, hi + 0.5 * set->chan_bw_hz,
                                               0.99, &res->obw_lo_hz, &res->obw_hi_hz) == 0;
        }
    } else {
        res->obw_valid = m2sdr_psd_obw(&res->psd, -half, half, 0.99, &res->obw_lo_hz, &res->obw_hi_hz) == 0;
    }
    /* The modes on. */
    for (m = 0; an_modes[m]; m++) {
        const struct an_mode *mode = an_modes[m];

        if (!mode->analyse && !mode->analyse_two)
            continue;
        /* Its block while it is on, kept from a capture to the next; none while it is off. */
        if (!an_mode_on(mode, set)) {
            free(res->mode[m]);
            res->mode[m] = NULL;
            continue;
        }
        if (!res->mode[m] && !(res->mode[m] = calloc(1, mode->result_size)))
            return -1;
        *(bool *)((char *)res->mode[m] + mode->valid) = false;
        if (mode->analyse)
            mode->analyse(x, count, set, res);
        /* RX1 and RX2, whichever the spectrum shows. */
        if (mode->analyse_two && x2)
            mode->analyse_two(set->channel ? x2 : x, set->channel ? x : x2, count, set, res);
    }
    res->time_count = count < AN_TIME_POINTS ? count : AN_TIME_POINTS;
    memcpy(res->time, x, res->time_count * sizeof(*x));
    return 0;
}

/* Phase noise floor */
/*-------------------*/

int floor_default_path(uint64_t dna, char *path, size_t len)
{
    char cal[1024], *slash;

    if (m2sdr_cal_default_path(dna, cal, sizeof(cal)) != 0)
        return -1;
    slash = strrchr(cal, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    return snprintf(path, len, "%.900s/pn_floor_%014llx.csv", cal, (unsigned long long)dna) < (int)len ? 0 : -1;
}

int floor_load(struct an_floor *fl, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];

    fl->points = 0;
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        double a, b;

        if (sscanf(line, "# freq_hz=%lf sample_rate=%lf", &a, &b) == 2) {
            fl->freq_hz     = a;
            fl->sample_rate = b;
        } else if (sscanf(line, "%lf,%lf", &a, &b) == 2 && a > 0 && fl->points < M2SDR_PN_MAX_POINTS) {
            fl->offset_hz[fl->points] = a;
            fl->l_dbc_hz[fl->points]  = b;
            fl->points++;
        }
    }
    fclose(f);
    snprintf(fl->path, sizeof(fl->path), "%.1023s", path);
    return fl->points >= 2 ? 0 : -1;
}

/* Report */
/*--------*/

/* The modes in the order of their sections in the reports. */
static int report_modes(const struct an_mode **order)
{
    int m, n = 0, k;

    for (m = 0; an_modes[m] && n < AN_MODES_MAX; m++, n++) {
        for (k = n; k > 0 && order[k - 1]->report_rank > an_modes[m]->report_rank; k--)
            order[k] = order[k - 1];
        order[k] = an_modes[m];
    }
    return n;
}

void print_json_number(const char *name, double value, bool last)
{
    if (isfinite(value))
        printf("    \"%s\": %.6f%s\n", name, value, last ? "" : ",");
    else
        printf("    \"%s\": null%s\n", name, last ? "" : ",");
}

static void report_json(const struct an_source *src, const struct an_result *res, const struct an_floor *fl)
{
    const struct an_settings *set = &res->set;
    const double off = set->ref_offset_db;
    const struct an_report rep = { off, off != 0.0 ? "dBm" : "dBFS", fl };
    const struct an_mode *order[AN_MODES_MAX];
    int i, k, n;

    printf("{\n");
    printf("  \"source\": {\n");
    printf("    \"kind\": \"%s\",\n", src->live ? "board" : "file");
    print_json_number("center_hz", set->freq_hz, false);
    print_json_number("sample_rate", set->sample_rate, false);
    printf("    \"channel\": %d,\n", set->channel + 1);
    printf("    \"samples_lost\": %s\n", res->lost ? "true" : "false");
    printf("  },\n");
    printf("  \"spectrum\": {\n");
    printf("    \"fft_len\": %d,\n", set->fft_len);
    printf("    \"window\": \"%s\",\n", window_names[set->window]);
    printf("    \"segments\": %u,\n", res->psd.segments);
    print_json_number("rbw_hz", res->psd.rbw_hz, false);
    print_json_number("noise_db_hz", res->noise_db_hz + off, false);
    printf("    \"peaks\": [");
    for (i = 0; i < res->npeaks; i++)
        printf("%s{\"freq_hz\": %.3f, \"offset_hz\": %.3f, \"level_db\": %.3f}", i ? ", " : "",
               set->freq_hz + res->peaks[i].freq_hz, res->peaks[i].freq_hz, res->peaks[i].power_db + off);
    printf("]\n");
    printf("  },\n");
    if (res->tone_valid) {
        printf("  \"tone\": {\n");
        print_json_number("freq_hz", set->freq_hz + res->tone.freq_hz, false);
        print_json_number("offset_hz", res->tone.freq_hz, false);
        print_json_number("level_db", res->tone.level_db + off, false);
        print_json_number("snr_db", res->tone.snr_db, false);
        print_json_number("sinad_db", res->tone.sinad_db, false);
        print_json_number("thd_dbc", res->tone.thd_dbc, false);
        print_json_number("sfdr_dbc", res->tone.sfdr_dbc, false);
        print_json_number("spur_offset_hz", res->tone.spur_freq_hz, false);
        printf("    \"harmonics_dbc\": [");
        for (i = 0; i < M2SDR_TONE_HARMONICS; i++)
            printf("%s%.3f", i ? ", " : "", res->tone.harmonic_dbc[i]);
        printf("]\n");
        printf("  },\n");
    }
    printf("  \"channel\": {\n");
    if (res->chan_valid) {
        print_json_number("offset_hz", set->chan_offset_hz, false);
        print_json_number("bandwidth_hz", set->chan_bw_hz, false);
        print_json_number("power_db", res->chan_power_db + off, false);
        if (res->acpr_valid) {
            printf("    \"acpr_lower_dbc\": [");
            for (i = 0; i < res->acpr.adjacent; i++)
                printf("%s%.3f", i ? ", " : "", res->acpr.lower_dbc[i]);
            printf("],\n    \"acpr_upper_dbc\": [");
            for (i = 0; i < res->acpr.adjacent; i++)
                printf("%s%.3f", i ? ", " : "", res->acpr.upper_dbc[i]);
            printf("],\n");
        }
    }
    if (res->obw_valid) {
        print_json_number("obw_hz", res->obw_hi_hz - res->obw_lo_hz, false);
        print_json_number("obw_center_offset_hz", 0.5 * (res->obw_lo_hz + res->obw_hi_hz), true);
    } else {
        printf("    \"obw_hz\": null\n");
    }
    printf("  },\n");
    printf("  \"time\": {\n");
    print_json_number("mean_db", res->stats.mean_db + off, false);
    print_json_number("peak_db", res->stats.peak_db + off, false);
    print_json_number("papr_db", res->stats.papr_db, false);
    print_json_number("dc_db", res->stats.dc_db + off, false);
    print_json_number("gain_imbalance_db", res->stats.gain_imbalance_db, false);
    print_json_number("quadrature_deg", res->stats.quadrature_deg, false);
    print_json_number("clipped", res->stats.clipped, true);
    printf("  }");
    /* The sections of the modes on, each after a comma. */
    n = report_modes(order);
    for (k = 0; k < n; k++) {
        if (order[k]->report_json && an_mode_on(order[k], set)) {
            printf(",\n");
            order[k]->report_json(res, &rep);
        }
    }
    printf("\n");
    printf("}\n");
}

static void report_text(const struct an_source *src, const struct an_result *res, const struct an_floor *fl)
{
    const struct an_settings *set = &res->set;
    const double off = set->ref_offset_db;
    const char *unit = off != 0.0 ? "dBm" : "dBFS";
    const struct an_report rep = { off, unit, fl };
    const struct an_mode *order[AN_MODES_MAX];
    int i, k, n;

    printf("Source     : %s, centre %.6f MHz, %.3f MSPS, RX%d\n", src->live ? "board" : src->name,
           set->freq_hz / 1e6, set->sample_rate / 1e6, set->channel + 1);
    printf("Spectrum   : FFT %d, %s window, RBW %.1f Hz, %u segments averaged\n",
           set->fft_len, window_names[set->window], res->psd.rbw_hz, res->psd.segments);
    if (res->lost)
        printf("Warning    : samples were lost in the capture; the figures are not reliable.\n");
    printf("Noise      : %.1f %s/Hz (median of the span)\n", res->noise_db_hz + off, unit);

    printf("\nPeaks (%.0f dB above the noise of a bin):\n", AN_PEAK_MIN_ABOVE_DB);
    for (i = 0; i < res->npeaks; i++)
        printf("  %14.6f MHz  %+12.1f Hz  %8.2f %s\n", (set->freq_hz + res->peaks[i].freq_hz) / 1e6,
               res->peaks[i].freq_hz, res->peaks[i].power_db + off, unit);
    if (!res->npeaks)
        printf("  none\n");

    if (res->tone_valid) {
        printf("\nStrongest line, as a tone:\n");
        printf("  Frequency  : %.6f MHz (%+.1f Hz from the centre)\n", (set->freq_hz + res->tone.freq_hz) / 1e6, res->tone.freq_hz);
        printf("  Level      : %.2f %s\n", res->tone.level_db + off, unit);
        printf("  SNR        : %.1f dB, SINAD %.1f dB\n", res->tone.snr_db, res->tone.sinad_db);
        printf("  SFDR       : %.1f dBc (other line at %+.1f Hz)\n", res->tone.sfdr_dbc, res->tone.spur_freq_hz);
        printf("  THD        : %.1f dBc; harmonics 2 to %d:", res->tone.thd_dbc, M2SDR_TONE_HARMONICS + 1);
        for (i = 0; i < M2SDR_TONE_HARMONICS; i++)
            printf(" %.1f", res->tone.harmonic_dbc[i]);
        printf(" dBc\n");
    } else {
        printf("\nNo line stands out of the noise: no tone figures.\n");
    }

    printf("\nChannel:\n");
    if (res->chan_valid) {
        printf("  Channel    : %.3f kHz wide at %+.3f kHz\n", set->chan_bw_hz / 1e3, set->chan_offset_hz / 1e3);
        printf("  Power      : %.2f %s\n", res->chan_power_db + off, unit);
        for (i = 0; res->acpr_valid && i < res->acpr.adjacent; i++)
            printf("  ACPR %d     : lower %.1f dBc, upper %.1f dBc\n", i + 1, res->acpr.lower_dbc[i], res->acpr.upper_dbc[i]);
    } else if (set->chan_bw_hz > 0) {
        printf("  The channel does not fit in the span.\n");
    }
    if (res->obw_valid)
        printf("  OBW (99 %%)  : %.3f kHz, centred at %+.3f kHz\n", (res->obw_hi_hz - res->obw_lo_hz) / 1e3,
               0.5 * (res->obw_lo_hz + res->obw_hi_hz) / 1e3);

    printf("\nTime domain:\n");
    printf("  Mean power : %.2f %s, peak %.2f %s, PAPR %.2f dB\n", res->stats.mean_db + off, unit,
           res->stats.peak_db + off, unit, res->stats.papr_db);
    printf("  DC offset  : %.1f %s (I %+.5f, Q %+.5f)\n", res->stats.dc_db + off, unit, res->stats.dc_i, res->stats.dc_q);
    printf("  I/Q        : gain imbalance %+.3f dB, quadrature error %+.3f degrees\n",
           res->stats.gain_imbalance_db, res->stats.quadrature_deg);
    printf("  Clipping   : %.4f %% of the samples\n", 100.0 * res->stats.clipped);

    /* The sections of the modes on. */
    n = report_modes(order);
    for (k = 0; k < n; k++) {
        if (order[k]->report_text && an_mode_on(order[k], set))
            order[k]->report_text(res, &rep);
    }
}

int export_csv(const char *path, const struct an_result *res)
{
    FILE *f = fopen(path, "w");
    size_t k;

    if (!f)
        return -1;
    fprintf(f, "frequency_hz,offset_hz,level_db\n");
    for (k = 0; k < res->psd.bins; k++)
        fprintf(f, "%.3f,%.3f,%.3f\n", res->set.freq_hz + m2sdr_psd_freq(&res->psd, k), m2sdr_psd_freq(&res->psd, k),
                m2sdr_power_db(res->psd.power[k]) + res->set.ref_offset_db);
    return fclose(f) == 0 ? 0 : -1;
}

int run_report(struct an_source *src, const struct an_settings *set, bool json, const char *csv_path,
               struct an_floor *fl, const char *floor_save_path)
{
    const size_t count = capture_samples(set);
    const bool two = capture_two_channels(set);
    float complex *x = malloc(count * sizeof(*x)), *x2 = two ? malloc(count * sizeof(*x2)) : NULL;
    struct an_result *res = calloc(1, sizeof(*res));
    char err[256];
    int rc, m, status = 1;

    if (!x || !res || (two && !x2)) {
        fprintf(stderr, "Out of memory\n");
        goto out;
    }
    rc = source_capture_two(src, set, x, x2, count, err, sizeof(err));
    if (rc < 0) {
        fprintf(stderr, "Capture failed: %s\n", err);
        goto out;
    }
    res->lost    = rc > 0;
    res->capture = src->capture;
    if (analyse_two(x, x2, count, set, res) != 0) {
        fprintf(stderr, "Analysis failed\n");
        goto out;
    }
    if (json)
        report_json(src, res, fl);
    else
        report_text(src, res, fl);
    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->report_done && an_modes[m]->report_done(res) < 0)
            goto out;
    }
    if (floor_save_path) {
        rc = floor_save(fl, floor_save_path, res);
        if (rc == -2) {
            fprintf(stderr, "No carrier: no floor stored.\n");
            goto out;
        }
        if (rc != 0) {
            fprintf(stderr, "Could not write %s: %s\n", floor_save_path, strerror(errno));
            goto out;
        }
        fprintf(stderr, "Phase noise stored as the floor of the board in %s.\n", floor_save_path);
    }
    if (csv_path && export_csv(csv_path, res) != 0) {
        fprintf(stderr, "Could not write %s: %s\n", csv_path, strerror(errno));
        goto out;
    }
    status = 0;

out:
    if (res)
        an_result_free(res);
    free(res);
    free(x);
    free(x2);
    return status;
}
/* Watch */
/*-------*/

double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return 1e3 * (double)ts.tv_sec + 1e-6 * (double)ts.tv_nsec;
}

static volatile sig_atomic_t watch_stop;

static void watch_signal(int sig)
{
    (void)sig;
    watch_stop = 1;
}

/* The modes on that can be watched: the one of --watch, of which main checks there is one. */
int watch_modes(const struct an_settings *set, const struct an_mode **mode)
{
    int m, n = 0;

    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->watch_line && an_mode_on(an_modes[m], set)) {
            if (mode)
                *mode = an_modes[m];
            n++;
        }
    }
    return n;
}

/* A mode from a board, one line per capture until Ctrl-C (DVB-T, DAB): the figures to follow
 * when placing an antenna, without a display. */
int run_watch(struct an_source *src, const struct an_settings *base, bool json)
{
    const struct an_mode *mode = NULL;
    struct an_settings set = *base;
    struct an_result *res = calloc(1, sizeof(*res));
    float complex *x = NULL, *x2 = NULL;
    size_t x_count = 0;
    const double start = now_ms();
    int status = 1;

    (void)watch_modes(&set, &mode);
    if (!res || !mode) {
        fprintf(stderr, "Out of memory\n");
        goto out;
    }
    signal(SIGINT, watch_signal);
    while (!watch_stop) {
        const double now = (now_ms() - start) / 1e3;
        char err[256];
        size_t count;
        int rc;

        if (mode->watch_prepare)
            mode->watch_prepare(&set);
        count = capture_samples(&set);
        if (count > x_count) {
            float complex *grown = realloc(x, count * sizeof(*x));

            if (!grown) {
                fprintf(stderr, "Out of memory\n");
                goto out;
            }
            x = grown;
            if (capture_two_channels(&set)) {
                grown = realloc(x2, count * sizeof(*x2));
                if (!grown) {
                    fprintf(stderr, "Out of memory\n");
                    goto out;
                }
                x2 = grown;
            }
            x_count = count;
        }
        rc = source_capture_two(src, &set, x, capture_two_channels(&set) ? x2 : NULL, count, err, sizeof(err));
        if (rc < 0 && watch_stop)
            break;                 /* Ctrl-C during the capture. */
        if (rc < 0) {
            fprintf(stderr, "Capture failed: %s\n", err);
            goto out;
        }
        res->capture = src->capture;
        if (analyse_two(x, capture_two_channels(&set) ? x2 : NULL, count, &set, res) != 0) {
            fprintf(stderr, "Analysis failed\n");
            goto out;
        }
        if (mode->watch_line(&set, res, now, json) != 0)
            goto out;
        fflush(stdout);
    }
    status = 0;

out:
    if (res)
        an_result_free(res);
    free(res);
    free(x);
    free(x2);
    return status;
}
