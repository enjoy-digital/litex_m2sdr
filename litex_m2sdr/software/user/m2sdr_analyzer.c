/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility.
 *
 * Spectrum, markers, channel power and time-domain measurements on the RX of a board or on a
 * capture file. With SDL2 and cimgui it opens a Dear ImGui window; --report prints the
 * measurements of one capture without a display.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <complex.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef M2SDR_ANALYZER_GUI
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include "scan_ui/m2sdr_plot.h"
#include "scan_ui/imgui_sdl_gl3_bridge.h"
#include "scan_ui/m2sdr_colormaps_gqrx.h"
#endif

#include "m2sdr.h"
#include "m2sdr_analysis.h"
#include "m2sdr_cal.h"
#include "m2sdr_cli.h"
#include "m2sdr_sigmf.h"

#include "analyzer/analyzer.h"

static const char *const window_names[] = { "Hann", "Flat-top", "Blackman-Harris", "Rectangular" };

/* Analysis */
/*----------*/

static size_t capture_samples(const struct an_settings *set)
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

static int analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const double half = 0.5 * set->sample_rate;
    const size_t spectrum = (size_t)set->fft_len * ((size_t)set->averages + 1) / 2;
    const size_t psd_count = psd_short(set) && spectrum >= (size_t)set->fft_len && spectrum < count ? spectrum : count;
    int m;

    res->set = *set;
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

        if (!mode->analyse)
            continue;
        *(bool *)((char *)res + mode->valid) = false;
        if (an_mode_on(mode, set))
            mode->analyse(x, count, set, res);
    }
    res->time_count = count < AN_TIME_POINTS ? count : AN_TIME_POINTS;
    memcpy(res->time, x, res->time_count * sizeof(*x));
    return 0;
}

/* Phase noise floor */
/*-------------------*/

static int floor_default_path(uint64_t dna, char *path, size_t len)
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

static int floor_load(struct an_floor *fl, const char *path)
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

int floor_save(struct an_floor *fl, const char *path, const struct an_result *res)
{
    char dir[1024], *p;
    FILE *f;
    int i;

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
    fprintf(f, "# M2SDR phase noise floor: offset_hz,l_dbc_hz\n");
    fprintf(f, "# freq_hz=%.0f sample_rate=%.0f\n", res->set.freq_hz + res->pn.carrier_hz, res->set.sample_rate);
    for (i = 0; i < res->pn.points; i++)
        fprintf(f, "%.3f,%.3f\n", res->pn.offset_hz[i], res->pn.l_dbc_hz[i]);
    if (fclose(f) != 0)
        return -1;
    return floor_load(fl, path);
}

/* Report */
/*--------*/

/* C/N for quasi error free reception in a Gaussian channel (EN 300 744, annex A), by
 * constellation and code rate: what the MER must at least reach. NAN when not known. */
static double dvbt_needed_db(const struct m2sdr_dvbt_result *t)
{
    static const double cn[3][5] = {
        {  3.1,  4.9,  5.9,  6.9,  7.7 },
        {  8.8, 11.1, 12.5, 13.5, 13.9 },
        { 14.4, 16.5, 18.0, 19.3, 20.1 },
    };
    static const char *const rates[5] = { "1/2", "2/3", "3/4", "5/6", "7/8" };
    int m, r;

    if (!t->tps || t->tps_hierarchy)
        return NAN;
    m = t->tps_modulation == M2SDR_MOD_QPSK ? 0 : t->tps_modulation == M2SDR_MOD_16QAM ? 1 : 2;
    for (r = 0; r < 5; r++)
        if (strcmp(t->code_rate_hp, rates[r]) == 0)
            return cn[m][r];
    return NAN;
}

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

static int export_csv(const char *path, const struct an_result *res)
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

/* Modulation */
/*------------*/

static size_t demod_samples(const struct an_settings *set)
{
    return (size_t)1 << set->demod_log2;
}

static void demod_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = count < ((size_t)1 << set->demod_log2) ? count : (size_t)1 << set->demod_log2;

    res->demod_valid = m2sdr_demod(x, n, set->sample_rate, &set->demod, &res->demod) == 0;
}

static void demod_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const char *unit = rep->unit;
    const struct m2sdr_demod_result *d = &res->demod;

    printf("\nModulation:\n");
    if (!res->demod_valid || !d->signal) {
        printf("  No signal with a symbol rate was found: tune on it, with a sample rate of a few\n"
               "  times its bandwidth, or give --symbol-rate.\n");
    } else {
        printf("  Type       : %s%s\n", m2sdr_modulation_name(d->modulation),
               d->locked ? "" : " (NOT LOCKED: the figures below are not valid)");
        printf("  Symbol rate: %.3f symbols/s, roll-off %.2f\n", d->symbol_rate, d->rolloff);
        printf("  Carrier    : %.6f MHz (%+.1f Hz from the centre), %.2f %s\n",
               (set->freq_hz + d->freq_offset_hz) / 1e6, d->freq_offset_hz, d->power_db + off, unit);
        printf("  EVM        : %.2f %% RMS, %.2f %% peak, over %zu symbols\n", 100.0 * d->evm_rms, 100.0 * d->evm_peak, d->symbols);
        printf("  MER        : %.1f dB\n", d->mer_db);
        printf("  Errors     : magnitude %.2f %% RMS, phase %.2f degrees RMS\n", 100.0 * d->mag_err_rms, d->phase_err_deg);
        printf("  I/Q        : origin offset %.1f dB, gain imbalance %+.3f dB, quadrature error %+.3f degrees\n",
               d->iq_offset_db, d->gain_imbalance_db, d->quadrature_deg);
    }
}

static void demod_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const struct m2sdr_demod_result *d = &res->demod;

    printf("  \"modulation\": {\n");
    printf("    \"signal\": %s,\n", res->demod_valid && d->signal ? "true" : "false");
    printf("    \"locked\": %s%s\n", res->demod_valid && d->locked ? "true" : "false",
           res->demod_valid && d->signal ? "," : "");
    if (res->demod_valid && d->signal) {
        printf("    \"type\": \"%s\",\n", m2sdr_modulation_name(d->modulation));
        print_json_number("symbol_rate", d->symbol_rate, false);
        print_json_number("rolloff", d->rolloff, false);
        print_json_number("carrier_hz", set->freq_hz + d->freq_offset_hz, false);
        print_json_number("carrier_offset_hz", d->freq_offset_hz, false);
        print_json_number("power_db", d->power_db + off, false);
        printf("    \"symbols\": %zu,\n", d->symbols);
        print_json_number("evm_rms_percent", 100.0 * d->evm_rms, false);
        print_json_number("evm_peak_percent", 100.0 * d->evm_peak, false);
        print_json_number("mer_db", d->mer_db, false);
        print_json_number("magnitude_error_percent", 100.0 * d->mag_err_rms, false);
        print_json_number("phase_error_deg", d->phase_err_deg, false);
        print_json_number("origin_offset_db", d->iq_offset_db, false);
        print_json_number("gain_imbalance_db", d->gain_imbalance_db, false);
        print_json_number("quadrature_deg", d->quadrature_deg, true);
    }
    printf("  }");
}

/* LTE */
/*-----*/

static size_t lte_samples(const struct an_settings *set)
{
    return (size_t)(AN_LTE_SECONDS * set->sample_rate);
}

static void lte_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = (size_t)(AN_LTE_SECONDS * set->sample_rate);

    res->lte_valid = m2sdr_lte_analyse(x, count < n ? count : n, set->sample_rate, &res->lte) == 0;
}

static void lte_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const char *unit = rep->unit;
    const struct m2sdr_lte_result *l = &res->lte;
    static const char *const phich[4] = { "1/6", "1/2", "1", "2" };

    printf("\nLTE downlink:\n");
    if (!res->lte_valid) {
        printf("  The sample rate must be 1.92, 3.84, 7.68, 15.36 or 30.72 MSPS.\n");
    } else if (!l->found) {
        printf("  No cell found (PSS quality %.1f): the capture is tuned on the centre of an FDD carrier\n"
               "  with a normal cyclic prefix.\n", l->sync_quality);
    } else {
        printf("  Cell       : identity %d (group %d, sector %d)\n", l->cell_id, l->n_id_1, l->n_id_2);
        printf("  Carrier    : %.6f MHz (%+.1f Hz from the centre)\n", (set->freq_hz + l->freq_error_hz) / 1e6, l->freq_error_hz);
        if (l->mib)
            printf("  MIB        : %d resource blocks, %d antenna port%s, frame %d, PHICH %s, %s\n", l->n_rb, l->ports,
                   l->ports == 1 ? "" : "s", l->sfn, l->phich_duration ? "extended" : "normal", phich[l->phich_resource & 3]);
        else
            printf("  MIB        : not decoded (the PBCH of one and of two antenna ports was tried)\n");
        printf("  Analysed   : %d resource blocks around the centre\n", l->analysed_rb);
        printf("  Levels     : RSRP %.1f %s, RSSI %.1f %s, PSS %.1f, SSS %.1f %s per element\n", l->rsrp_db + off, unit,
               l->rssi_db + off, unit, l->pss_db + off, l->sss_db + off, unit);
        printf("  EVM        : reference signals %.2f %%", 100.0 * l->rs_evm);
        if (l->mib)
            printf(", PBCH %.2f %%", 100.0 * l->pbch_evm);
        printf("\n");
        if (l->data) {
            int used[M2SDR_MOD_COUNT] = { 0 }, ns, rb, m;

            printf("  Data       : %d blocks read, mostly %s, EVM %.2f %% against the nearest points\n", l->data_blocks,
                   l->data_blocks ? m2sdr_modulation_name(l->data_modulation) : "none", 100.0 * l->data_evm);
            for (ns = 0; ns < M2SDR_LTE_FRAME_SLOTS; ns++) {
                for (rb = 0; rb < M2SDR_LTE_MAX_ANALYSED_RB; rb++)
                    used[l->rb_modulation[ns][rb] % M2SDR_MOD_COUNT]++;
            }
            printf("  Blocks     :");
            for (m = 0; m < M2SDR_MOD_COUNT; m++) {
                if (m != M2SDR_MOD_AUTO && used[m])
                    printf(" %s %d", m2sdr_modulation_name((enum m2sdr_modulation)m), used[m]);
            }
            printf("\n");
            if (l->ports > 1)
                printf("  Two ports  : %d blocks in transmit diversity, %d as one precoded layer, %d in use and not read\n"
                       "               (two layers, that one receiver does not separate)\n",
                       l->data_diversity, l->data_precoded, l->data_unknown);
        } else if (l->mib)
            printf("  Data       : not analysed, the cell has %d antenna ports\n", l->ports);
    }
}

static void lte_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const struct m2sdr_lte_result *l = &res->lte;
    static const char *const phich[4] = { "1/6", "1/2", "1", "2" };

    printf("  \"lte\": {\n");
    printf("    \"found\": %s%s\n", res->lte_valid && l->found ? "true" : "false", res->lte_valid && l->found ? "," : "");
    if (res->lte_valid && l->found) {
        printf("    \"cell_id\": %d,\n", l->cell_id);
        print_json_number("carrier_hz", set->freq_hz + l->freq_error_hz, false);
        print_json_number("carrier_offset_hz", l->freq_error_hz, false);
        printf("    \"mib\": %s,\n", l->mib ? "true" : "false");
        if (l->mib) {
            printf("    \"ports\": %d,\n", l->ports);
            printf("    \"n_rb\": %d,\n", l->n_rb);
            printf("    \"sfn\": %d,\n", l->sfn);
            printf("    \"phich_duration\": \"%s\",\n", l->phich_duration ? "extended" : "normal");
            printf("    \"phich_resource\": \"%s\",\n", phich[l->phich_resource & 3]);
        }
        printf("    \"analysed_rb\": %d,\n", l->analysed_rb);
        print_json_number("rsrp_db", l->rsrp_db + off, false);
        print_json_number("rssi_db", l->rssi_db + off, false);
        print_json_number("pss_db", l->pss_db + off, false);
        print_json_number("sss_db", l->sss_db + off, false);
        print_json_number("rs_evm_percent", 100.0 * l->rs_evm, false);
        if (l->mib)
            print_json_number("pbch_evm_percent", 100.0 * l->pbch_evm, false);
        printf("    \"data_analysed\": %s", l->data ? "true" : "false");
        if (l->data) {
            printf(",\n    \"data_blocks\": %d,\n", l->data_blocks);
            if (l->ports > 1) {
                printf("    \"data_blocks_diversity\": %d,\n", l->data_diversity);
                printf("    \"data_blocks_precoded\": %d,\n", l->data_precoded);
                printf("    \"data_blocks_not_read\": %d,\n", l->data_unknown);
            }
            printf("    \"data_modulation\": \"%s\",\n", m2sdr_modulation_name(l->data_modulation));
            print_json_number("data_evm_percent", 100.0 * l->data_evm, true);
        } else {
            printf("\n");
        }
    }
    printf("  }");
}

/* DVB-T */
/*-------*/

static size_t dvbt_samples(const struct an_settings *set)
{
    return (size_t)((set->dvbt_fast ? AN_DVBT_FAST_SECONDS : AN_DVBT_SECONDS) * set->sample_rate);
}

static void dvbt_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = (size_t)(AN_DVBT_SECONDS * set->sample_rate);

    res->dvbt_valid = m2sdr_dvbt_analyse(x, count < n ? count : n, set->sample_rate,
                                         set->dvbt_hint.fft_len ? &set->dvbt_hint : NULL, &res->dvbt) == 0;
}

static void dvbt_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const char *unit = rep->unit;
    const struct m2sdr_dvbt_result *t = &res->dvbt;

    printf("\nDVB-T:\n");
    if (!res->dvbt_valid) {
        printf("  The sample rate must be the one of the channel: 9.142857 MSPS (8 MHz), 8 MSPS (7 MHz)\n"
               "  or 6.857143 MSPS (6 MHz).\n");
    } else if (!t->found) {
        printf("  No multiplex found (guard interval correlation %.3f).\n", t->sync_quality);
    } else {
        printf("  Mode       : %dk, guard interval 1/%d (%.0f us), %.1f %s\n", t->fft_len / 1024, t->guard, t->guard_us,
               t->power_dbfs + off, unit);
        printf("  Sync       : guard correlation %.2f (about %.1f dB S/N)\n", t->sync_quality, t->sync_snr_db);
        if (!t->pilots) {
            printf("  Pilots     : not found (coherence %.2f): too much noise for the figures below.\n", t->pilot_quality);
        } else {
            printf("  Multiplex  : %.6f MHz (%+.1f Hz from the centre)\n", (set->freq_hz + t->freq_error_hz) / 1e6, t->freq_error_hz);
            if (t->tps) {
                printf("  TPS        : %s, hierarchy %s, code rate %s", m2sdr_modulation_name(t->tps_modulation),
                       t->tps_hierarchy ? "yes" : "none", t->code_rate_hp);
                if (t->tps_hierarchy)
                    printf(" (LP %s, alpha %d)", t->code_rate_lp, t->tps_hierarchy);
                if (t->cell_id >= 0)
                    printf(", cell %d", t->cell_id);
                printf("\n");
            } else {
                printf("  TPS        : not read\n");
            }
            printf("  MER        : %.1f dB against %s, over %d symbols", t->mer_db, m2sdr_modulation_name(t->modulation), t->symbols);
            if (isfinite(dvbt_needed_db(t)))
                printf(" (%.1f dB needed, margin %+.1f dB)", dvbt_needed_db(t), t->mer_db - dvbt_needed_db(t));
            printf("\n");
            if (t->echo_level_db > -99.0)
                printf("  Echo       : strongest at %+.2f us, %.1f dB (guard interval %.0f us)\n", t->echo_delay_us,
                       t->echo_level_db, t->guard_us);
        }
    }
}

static void dvbt_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const struct m2sdr_dvbt_result *t = &res->dvbt;
    const bool ok = res->dvbt_valid && t->found;

    printf("  \"dvbt\": {\n");
    print_json_number("sync_quality", res->dvbt_valid ? t->sync_quality : 0.0, false);
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        printf("    \"mode\": \"%dk\",\n", t->fft_len / 1024);
        printf("    \"guard_interval\": \"1/%d\",\n", t->guard);
        print_json_number("power_db", t->power_dbfs + off, false);
        printf("    \"pilots\": %s%s\n", t->pilots ? "true" : "false", t->pilots ? "," : "");
        if (t->pilots) {
            print_json_number("carrier_hz", set->freq_hz + t->freq_error_hz, false);
            print_json_number("carrier_offset_hz", t->freq_error_hz, false);
            printf("    \"tps\": %s,\n", t->tps ? "true" : "false");
            if (t->tps) {
                printf("    \"constellation\": \"%s\",\n", m2sdr_modulation_name(t->tps_modulation));
                printf("    \"hierarchy\": %d,\n", t->tps_hierarchy);
                printf("    \"code_rate_hp\": \"%s\",\n", t->code_rate_hp);
                printf("    \"code_rate_lp\": \"%s\",\n", t->code_rate_lp);
                if (t->cell_id >= 0)
                    printf("    \"cell_id\": %d,\n", t->cell_id);
            }
            printf("    \"mer_against\": \"%s\",\n", m2sdr_modulation_name(t->modulation));
            print_json_number("mer_needed_db", dvbt_needed_db(t), false);
            print_json_number("echo_delay_us", t->echo_delay_us, false);
            print_json_number("echo_level_db", t->echo_level_db, false);
            print_json_number("mer_db", t->mer_db, true);
        }
    }
    printf("  }");
}

/* --watch: the multiplex of the last TPS read and the captures since, for short captures once
 * it is read, as in the view. */
static struct {
    struct m2sdr_dvbt_result tps;
    struct m2sdr_dvbt_config known;
    unsigned since, lines;
    bool   ok;
} dvbt_watch;

static void dvbt_watch_prepare(struct an_settings *set)
{
    set->dvbt_fast = dvbt_watch.ok && ++dvbt_watch.since % AN_DVBT_FULL_EVERY != 0;
    memset(&set->dvbt_hint, 0, sizeof(set->dvbt_hint));
    if (dvbt_watch.ok)
        set->dvbt_hint = dvbt_watch.known;
}

static int dvbt_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_dvbt_result *t = &res->dvbt;
    const struct m2sdr_dvbt_result *tps = &dvbt_watch.tps;
    double needed;

    if (!res->dvbt_valid) {
        fprintf(stderr, "The sample rate is not one of DVB-T.\n");
        return -1;
    }
    if (t->tps) {
        /* The multiplex, each time its TPS is read: the reference of the lines. */
        if (!json && (!tps->tps || tps->tps_modulation != t->tps_modulation || strcmp(tps->code_rate_hp, t->code_rate_hp) ||
                      tps->fft_len != t->fft_len || tps->guard != t->guard || dvbt_watch.since >= AN_DVBT_FULL_EVERY - 1)) {
            printf("Multiplex %.3f MHz: %dk, guard 1/%d, %s, code rate %s", (set->freq_hz + t->freq_error_hz) / 1e6,
                   t->fft_len / 1024, t->guard, m2sdr_modulation_name(t->tps_modulation), t->code_rate_hp);
            if (t->cell_id >= 0)
                printf(", cell %d", t->cell_id);
            if (isfinite(dvbt_needed_db(t)))
                printf(": MER needed %.1f dB", dvbt_needed_db(t));
            printf("\n");
            dvbt_watch.lines = 0;
        }
        dvbt_watch.tps = *t;
        dvbt_watch.known.fft_len    = t->fft_len;
        dvbt_watch.known.guard      = t->guard;
        dvbt_watch.known.modulation = t->tps_modulation;
        dvbt_watch.ok    = true;
        dvbt_watch.since = 0;
    } else if (!t->found || !t->pilots) {
        dvbt_watch.ok = false;
    }
    if (dvbt_watch.ok && t->found && t->pilots) {
        dvbt_watch.known.offset_known   = true;
        dvbt_watch.known.carrier_offset = t->carrier_offset;
    }
    needed = dvbt_needed_db(tps);
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, t->found && t->pilots ? "true" : "false");
        if (t->found && t->pilots) {
            printf(", \"mer_db\": %.2f", t->mer_db);
            if (isfinite(needed))
                printf(", \"margin_db\": %.2f", t->mer_db - needed);
            printf(", \"power_db\": %.2f, \"sync_quality\": %.3f, \"carrier_offset_hz\": %.1f",
                   t->power_dbfs + set->ref_offset_db, t->sync_quality, t->freq_error_hz);
            if (t->echo_level_db > -99.0)
                printf(", \"echo_delay_us\": %.2f, \"echo_level_db\": %.1f", t->echo_delay_us, t->echo_level_db);
        }
        printf("}\n");
    } else {
        /* The names of the columns now and then, the numbers between. */
        if (dvbt_watch.lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %7s %7s %8s %6s %10s %8s %6s\n%8s %7s %7s %8s %6s %10s %8s %6s\n",
                   "time", "MER", "margin", "level", "guard", "offset", "echo", "echo",
                   "s", "dB", "dB", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "corr", "Hz", "us", "dB");
        if (t->found && t->pilots) {
            printf("%8.2f %7.1f ", now, t->mer_db);
            if (isfinite(needed))
                printf("%+7.1f", t->mer_db - needed);
            else
                printf("%7s", "--");
            printf(" %8.1f %6.2f %+10.1f", t->power_dbfs + set->ref_offset_db, t->sync_quality, t->freq_error_hz);
            if (t->echo_level_db > -99.0)
                printf(" %+8.2f %6.1f", t->echo_delay_us, t->echo_level_db);
            printf("\n");
        } else {
            printf("%8.2f  no multiplex (guard correlation %.3f)\n", now, t->sync_quality);
        }
    }
    return 0;
}

/* DAB */
/*-----*/

static size_t dab_samples(const struct an_settings *set)
{
    return (size_t)(AN_DAB_SECONDS * set->sample_rate);
}

static void dab_analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res)
{
    const size_t n = (size_t)(AN_DAB_SECONDS * set->sample_rate);

    res->dab_valid = m2sdr_dab_analyse(x, count < n ? count : n, set->sample_rate, &res->dab) == 0;
}

static void dab_report_text(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const char *unit = rep->unit;
    const struct m2sdr_dab_result *t = &res->dab;

    printf("\nDAB:\n");
    if (!res->dab_valid) {
        printf("  The sample rate must be 2.048 MSPS, with at least 110 ms of samples.\n");
    } else if (!t->found) {
        printf("  No ensemble found (null symbol %.1f dB, guard interval correlation %.3f).\n", t->null_depth_db,
               t->sync_quality);
    } else {
        printf("  Sync       : null symbol %.1f dB, guard correlation %.2f, %.1f %s\n", t->null_depth_db, t->sync_quality,
               t->power_dbfs + off, unit);
        if (!t->prs) {
            printf("  Reference  : phase reference symbol not found (%.2f): too much noise for the figures below.\n",
                   t->prs_quality);
        } else {
            printf("  Ensemble   : %.6f MHz (%+.1f Hz from the centre), %d frame%s\n", (set->freq_hz + t->freq_error_hz) / 1e6,
                   t->freq_error_hz, t->frames, t->frames == 1 ? "" : "s");
            printf("  MER        : %.1f dB over %d symbols, SNR on the carriers %.1f dB\n", t->mer_db, t->symbols, t->null_snr_db);
            if (t->echo_level_db > -99.0)
                printf("  Echo       : strongest at %+.2f us, %.1f dB (guard interval %.0f us)\n", t->echo_delay_us,
                       t->echo_level_db, t->guard_us);
        }
    }
}

static void dab_report_json(const struct an_result *res, const struct an_report *rep)
{
    const struct an_settings *set = &res->set;
    const double off = rep->off;
    const struct m2sdr_dab_result *t = &res->dab;
    const bool ok = res->dab_valid && t->found;

    printf("  \"dab\": {\n");
    print_json_number("sync_quality", res->dab_valid ? t->sync_quality : 0.0, false);
    printf("    \"found\": %s%s\n", ok ? "true" : "false", ok ? "," : "");
    if (ok) {
        print_json_number("power_db", t->power_dbfs + off, false);
        print_json_number("null_depth_db", t->null_depth_db, false);
        printf("    \"phase_reference\": %s%s\n", t->prs ? "true" : "false", t->prs ? "," : "");
        if (t->prs) {
            print_json_number("carrier_hz", set->freq_hz + t->freq_error_hz, false);
            print_json_number("carrier_offset_hz", t->freq_error_hz, false);
            printf("    \"frames\": %d,\n", t->frames);
            print_json_number("snr_db", t->null_snr_db, false);
            print_json_number("echo_delay_us", t->echo_delay_us, false);
            print_json_number("echo_level_db", t->echo_level_db, false);
            print_json_number("mer_db", t->mer_db, true);
        }
    }
    printf("  }");
}

/* --watch: one line per capture. */
static unsigned dab_watch_lines;

static int dab_watch_line(const struct an_settings *set, const struct an_result *res, double now, bool json)
{
    const struct m2sdr_dab_result *d = &res->dab;
    const bool dok = res->dab_valid && d->found && d->prs;

    if (!res->dab_valid) {
        fprintf(stderr, "The sample rate is not the one of DAB (2.048 MSPS).\n");
        return -1;
    }
    if (json) {
        printf("{\"time_s\": %.3f, \"found\": %s", now, dok ? "true" : "false");
        if (dok) {
            printf(", \"mer_db\": %.2f, \"snr_db\": %.2f, \"power_db\": %.2f, \"null_depth_db\": %.1f, "
                   "\"carrier_offset_hz\": %.1f", d->mer_db, d->null_snr_db, d->power_dbfs + set->ref_offset_db,
                   d->null_depth_db, d->freq_error_hz);
            if (d->echo_level_db > -99.0)
                printf(", \"echo_delay_us\": %.2f, \"echo_level_db\": %.1f", d->echo_delay_us, d->echo_level_db);
        }
        printf("}\n");
    } else {
        if (dab_watch_lines++ % AN_WATCH_HEADER_EVERY == 0)
            printf("%8s %7s %7s %8s %6s %10s %8s %6s\n%8s %7s %7s %8s %6s %10s %8s %6s\n",
                   "time", "MER", "SNR", "level", "null", "offset", "echo", "echo",
                   "s", "dB", "dB", set->ref_offset_db != 0.0 ? "dBm" : "dBFS", "dB", "Hz", "us", "dB");
        if (dok) {
            printf("%8.2f %7.1f %7.1f %8.1f %6.1f %+10.1f", now, d->mer_db, d->null_snr_db, d->power_dbfs + set->ref_offset_db,
                   d->null_depth_db, d->freq_error_hz);
            if (d->echo_level_db > -99.0)
                printf(" %+8.2f %6.1f", d->echo_delay_us, d->echo_level_db);
            printf("\n");
        } else {
            printf("%8.2f  no ensemble (null symbol %.1f dB, guard correlation %.3f)\n", now, d->null_depth_db,
                   d->sync_quality);
        }
    }
    return 0;
}

/* Watch */
/*-------*/

static double now_ms(void)
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
static int watch_modes(const struct an_settings *set, const struct an_mode **mode)
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
static int run_watch(struct an_source *src, const struct an_settings *base, bool json)
{
    const struct an_mode *mode = NULL;
    struct an_settings set = *base;
    struct an_result *res = calloc(1, sizeof(*res));
    float complex *x = NULL;
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
            x       = grown;
            x_count = count;
        }
        rc = source_capture(src, &set, x, count, err, sizeof(err));
        if (rc < 0 && watch_stop)
            break;                 /* Ctrl-C during the capture. */
        if (rc < 0) {
            fprintf(stderr, "Capture failed: %s\n", err);
            goto out;
        }
        if (analyse(x, count, &set, res) != 0) {
            fprintf(stderr, "Analysis failed\n");
            goto out;
        }
        if (mode->watch_line(&set, res, now, json) != 0)
            goto out;
        fflush(stdout);
    }
    status = 0;

out:
    if (res) {
        m2sdr_psd_free(&res->psd);
        m2sdr_psd_free(&res->tone_psd);
    }
    free(res);
    free(x);
    return status;
}

static int run_report(struct an_source *src, const struct an_settings *set, bool json, const char *csv_path,
                      struct an_floor *fl, const char *floor_save_path)
{
    const size_t count = capture_samples(set);
    float complex *x = malloc(count * sizeof(*x));
    struct an_result *res = calloc(1, sizeof(*res));
    char err[256];
    int rc, status = 1;

    if (!x || !res) {
        fprintf(stderr, "Out of memory\n");
        goto out;
    }
    rc = source_capture(src, set, x, count, err, sizeof(err));
    if (rc < 0) {
        fprintf(stderr, "Capture failed: %s\n", err);
        goto out;
    }
    res->lost = rc > 0;
    if (analyse(x, count, set, res) != 0) {
        fprintf(stderr, "Analysis failed\n");
        goto out;
    }
    if (json)
        report_json(src, res, fl);
    else
        report_text(src, res, fl);
    if (floor_save_path) {
        if (!res->pn_valid) {
            fprintf(stderr, "No carrier: no floor stored.\n");
            goto out;
        }
        if (floor_save(fl, floor_save_path, res) != 0) {
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
    if (res) {
        m2sdr_psd_free(&res->psd);
        m2sdr_psd_free(&res->tone_psd);
    }
    free(res);
    free(x);
    return status;
}

/* GUI */
/*-----*/

#ifdef M2SDR_ANALYZER_GUI

static const char *const trace_names[UI_TRACES] = { "Live", "Average", "Max hold", "Min hold" };
static const ImU32 trace_colors[UI_TRACES] = { 0xff40d0ffu, 0xff60ff60u, 0xff6060ffu, 0xffffa040u };
static const ImU32 marker_colors[UI_MARKERS] = { 0xffffffffu, 0xff80ffffu, 0xffff80ffu, 0xffffff80u };

/* LTE: colours of the blocks of data and of their symbols by modulation (the first: empty), and
 * the scale of the resource grid, in dB over a reference element. */
static const ImU32 lte_mod_colors[M2SDR_MOD_COUNT] = {
    0xff282828u, 0xff808080u, 0xffff9040u, 0xff808080u, 0xff60ff60u, 0xff808080u, 0xff40d0ffu, 0xff808080u, 0xff4040ffu,
};
#define LTE_COL_UNREAD     0xff808080u
#define LTE_COL_PBCH       0xffff60ffu
#define LTE_COL_RS         0xffffffffu
#define LTE_GRID_TOP_DB    10.0
#define LTE_GRID_BOTTOM_DB (-40.0)
#define LTE_SCALE_W        52.0f     /* Room of the scale, right of the grid. */
#define LTE_NARROW_W       700.0f    /* Under this width of the view, the constellation goes under the grid. */
#define AN_LTE_HOLD        10        /* Captures a frame stays shown without a cell, or its symbols without data. */
#define AN_DVBT_MER_MEAN   10        /* Captures in the MER shown in figures: about 0.5 s. */
#define AN_DVBT_TRACE_AVG  0.125f    /* Weight of a capture in the averaged traces: about 8 captures. */
#define DVBT_UHF_BASE_HZ   306e6     /* European UHF channel n is centred at 306 + 8 n MHz (21 to 69). */
#define DVBT_COL_MER       0xff40c0ffu
#define DVBT_COL_CHANNEL   0xffffb040u
#define DVBT_COL_NEEDED    0xa04040ffu

/* Copy of a result, with its own spectrum. */
static int result_copy(struct an_result *dst, const struct an_result *src)
{
    float *power = dst->psd.power;

    if (dst->psd.bins != src->psd.bins) {
        power = realloc(power, src->psd.bins * sizeof(*power));
        if (!power)
            return -1;
    }
    *dst = *src;
    dst->psd.power = power;
    dst->psd.fft   = NULL;
    dst->psd.win   = NULL;
    dst->psd.work  = NULL;
    memset(&dst->tone_psd, 0, sizeof(dst->tone_psd));
    memcpy(power, src->psd.power, src->psd.bins * sizeof(*power));
    return 0;
}

/* A capture and its analysis. The worker captures into one job while the other is analysed in
 * its own thread: a result every max(capture, analysis) instead of their sum. */
struct an_job {
    pthread_t thread;
    bool   busy;
    float complex *x;
    size_t x_count;                /* Room in x. */
    size_t count;
    struct an_settings set;
    struct an_result *res;
    int    capture_rc;             /* > 0: samples lost. */
    char   err[256];
    double capture_ms, analysis_ms;
    int    rc;
};

static void *job_main(void *arg)
{
    struct an_job *j = arg;
    const double t = now_ms();

    j->rc = analyse(j->x, j->count, &j->set, j->res);
    j->res->lost   = j->capture_rc > 0;
    j->analysis_ms = now_ms() - t;
    return NULL;
}

/* The result of an analysed job to the modes, then to the UI. */
static void job_publish(struct an_shared *sh, struct an_job *j)
{
    int m;

    if (j->rc != 0) {
        pthread_mutex_lock(&sh->lock);
        snprintf(sh->status, sizeof(sh->status), "analysis failed");
        sh->run = sh->single = false;
        pthread_mutex_unlock(&sh->lock);
        return;
    }
    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->worker_result)
            an_modes[m]->worker_result(&j->set, j->res);
    }
    pthread_mutex_lock(&sh->lock);
    if (result_copy(&sh->res, j->res) == 0)
        sh->seq++;
    snprintf(sh->status, sizeof(sh->status), "%s", j->capture_rc > 0 ? j->err : "");
    sh->capture_ms  = j->capture_ms;
    sh->analysis_ms = j->analysis_ms;
    pthread_mutex_unlock(&sh->lock);
}

/* Wait for the analysis of a job, if one runs, and publish it. */
static void job_finish(struct an_shared *sh, struct an_job *j)
{
    if (!j->busy)
        return;
    pthread_join(j->thread, NULL);
    j->busy = false;
    job_publish(sh, j);
}

static void job_free(struct an_job *j)
{
    if (j->res) {
        m2sdr_psd_free(&j->res->psd);
        m2sdr_psd_free(&j->res->tone_psd);
    }
    free(j->res);
    free(j->x);
}

static void *worker_main(void *arg)
{
    struct an_shared *sh = arg;
    struct an_job jobs[2];
    double last = 0;
    int cur = 0, m;

    memset(jobs, 0, sizeof(jobs));
    jobs[0].res = calloc(1, sizeof(*jobs[0].res));
    jobs[1].res = calloc(1, sizeof(*jobs[1].res));
    while (jobs[0].res && jobs[1].res) {
        struct an_job *j = &jobs[cur];
        struct an_settings set;
        double t0;
        bool run, quit;

        pthread_mutex_lock(&sh->lock);
        set  = sh->set;
        run  = sh->run || sh->single;
        quit = sh->quit;
        /* A single capture is taken once; its result arrives when it is analysed. */
        sh->single = false;
        pthread_mutex_unlock(&sh->lock);
        if (quit || !run) {
            job_finish(sh, &jobs[cur ^ 1]);
            if (quit)
                break;
            usleep(20000);
            continue;
        }

        /* What the modes know from the results before (DVB-T: a known multiplex). */
        for (m = 0; an_modes[m]; m++) {
            if (an_modes[m]->worker_prepare)
                an_modes[m]->worker_prepare(&set, sh->src->live);
        }
        j->set   = set;
        j->count = capture_samples(&set);
        if (j->count > j->x_count) {
            float complex *grown = realloc(j->x, j->count * sizeof(*j->x));

            if (!grown) {
                job_finish(sh, &jobs[cur ^ 1]);
                pthread_mutex_lock(&sh->lock);
                snprintf(sh->status, sizeof(sh->status), "out of memory for %zu samples", j->count);
                sh->run = false;
                pthread_mutex_unlock(&sh->lock);
                continue;
            }
            j->x       = grown;
            j->x_count = j->count;
        }

        /* A file is read faster than it can be looked at. */
        if (now_ms() - last < AN_MIN_PERIOD_MS)
            usleep((useconds_t)(1e3 * (AN_MIN_PERIOD_MS - (now_ms() - last))));
        last = now_ms();

        t0 = now_ms();
        j->capture_rc = source_capture(sh->src, &set, j->x, j->count, j->err, sizeof(j->err));
        j->capture_ms = now_ms() - t0;
        /* The analysis of the capture before, then this one in the background. */
        job_finish(sh, &jobs[cur ^ 1]);
        if (j->capture_rc < 0) {
            pthread_mutex_lock(&sh->lock);
            snprintf(sh->status, sizeof(sh->status), "%s", j->err);
            sh->run = false;
            pthread_mutex_unlock(&sh->lock);
            continue;
        }
        if (pthread_create(&j->thread, NULL, job_main, j) != 0) {
            job_main(j);           /* No thread: analysed here, in turn. */
            job_publish(sh, j);
            continue;
        }
        j->busy = true;
        cur ^= 1;
    }
    job_finish(sh, &jobs[0]);
    job_finish(sh, &jobs[1]);
    job_free(&jobs[0]);
    job_free(&jobs[1]);
    return NULL;
}

/* Traces */

static void traces_reset(struct an_ui *ui)
{
    ui->avg_count = 0;
}

static int traces_update(struct an_ui *ui)
{
    const struct m2sdr_psd *psd = &ui->res.psd;
    size_t k;
    int t;

    if (ui->bins != psd->bins) {
        for (t = 0; t < UI_TRACES; t++) {
            float *grown = realloc(ui->trace[t], psd->bins * sizeof(float));
            if (!grown)
                return -1;
            ui->trace[t] = grown;
        }
        {
            double *grown = realloc(ui->avg_lin, psd->bins * sizeof(double));
            if (!grown)
                return -1;
            ui->avg_lin = grown;
        }
        ui->bins      = psd->bins;
        ui->avg_count = 0;
    }
    for (k = 0; k < psd->bins; k++) {
        const float db = (float)(m2sdr_power_db(psd->power[k]) + ui->res.set.ref_offset_db);

        ui->trace[TRACE_LIVE][k] = db;
        if (ui->avg_count == 0) {
            ui->avg_lin[k]          = psd->power[k];
            ui->trace[TRACE_MAX][k] = db;
            ui->trace[TRACE_MIN][k] = db;
        } else {
            const double n = ui->avg_count < ui->avg_target ? ui->avg_count + 1 : ui->avg_target;
            ui->avg_lin[k] += (psd->power[k] - ui->avg_lin[k]) / n;
            if (db > ui->trace[TRACE_MAX][k])
                ui->trace[TRACE_MAX][k] = db;
            if (db < ui->trace[TRACE_MIN][k])
                ui->trace[TRACE_MIN][k] = db;
        }
        ui->trace[TRACE_AVG][k] = (float)(m2sdr_power_db(ui->avg_lin[k]) + ui->res.set.ref_offset_db);
    }
    if (ui->avg_count < ui->avg_target)
        ui->avg_count++;
    return 0;
}

/* Trace read by the markers and the peak search: the first one shown. */
static const float *marker_trace(const struct an_ui *ui)
{
    int t;

    for (t = 0; t < UI_TRACES; t++) {
        if (ui->trace_on[t] && ui->trace[t])
            return ui->trace[t];
    }
    return ui->trace[TRACE_LIVE];
}

static double trace_at(const struct an_ui *ui, const float *trace, double offset_hz)
{
    const struct m2sdr_psd *psd = &ui->res.psd;
    double k = offset_hz / psd->bin_hz + (double)(psd->bins / 2);

    if (!trace || !psd->bins)
        return -200.0;
    if (k < 0)
        k = 0;
    if (k > (double)(psd->bins - 1))
        k = (double)(psd->bins - 1);
    return trace[(size_t)llround(k)];
}

/* Waterfall */

static uint32_t palette_color(int palette, float t)
{
    int idx = (int)(t * 255.0f + 0.5f);
    unsigned r, g, b;

    if (idx < 0)
        idx = 0;
    if (idx > 255)
        idx = 255;
    switch (palette) {
    case 1:
        r = gqrx_plasma[idx][0]; g = gqrx_plasma[idx][1]; b = gqrx_plasma[idx][2];
        break;
    case 2:
        r = (unsigned)(gqrx_viridis[idx][0] * 255.0f);
        g = (unsigned)(gqrx_viridis[idx][1] * 255.0f);
        b = (unsigned)(gqrx_viridis[idx][2] * 255.0f);
        break;
    default:
        r = gqrx_turbo[idx][0]; g = gqrx_turbo[idx][1]; b = gqrx_turbo[idx][2];
        break;
    }
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

static void waterfall_init(struct an_ui *ui)
{
    ui->wf_rgba = calloc((size_t)UI_WF_COLS * UI_WF_ROWS, sizeof(uint32_t));
    glGenTextures(1, &ui->wf_tex);
    glBindTexture(GL_TEXTURE_2D, ui->wf_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, UI_WF_COLS, UI_WF_ROWS, 0, GL_RGBA, GL_UNSIGNED_BYTE, ui->wf_rgba);
}

/* New line at the top, the others move down. A column shows the highest of its bins. */
static void waterfall_push(struct an_ui *ui)
{
    const float *live = ui->trace[TRACE_LIVE];
    float span;
    int col;

    if (!ui->wf_rgba || !live || !ui->bins)
        return;
    if (ui->wf_auto) {
        /* From a little under the noise of a bin to 70 dB above. */
        ui->wf_floor_db = (float)(ui->res.noise_db_hz + 10.0 * log10(ui->res.psd.rbw_hz) + ui->res.set.ref_offset_db - 5.0);
        ui->wf_ceil_db  = ui->wf_floor_db + 70.0f;
        ui->wf_auto     = false;
    }
    span = ui->wf_ceil_db - ui->wf_floor_db;
    memmove(ui->wf_rgba + UI_WF_COLS, ui->wf_rgba, (size_t)UI_WF_COLS * (UI_WF_ROWS - 1) * sizeof(uint32_t));
    for (col = 0; col < UI_WF_COLS; col++) {
        size_t a = (size_t)col * ui->bins / UI_WF_COLS;
        size_t b = (size_t)(col + 1) * ui->bins / UI_WF_COLS;
        float v;

        if (b <= a)
            b = a + 1;
        v = live[a];
        for (a++; a < b && a < ui->bins; a++) {
            if (live[a] > v)
                v = live[a];
        }
        ui->wf_rgba[col] = palette_color(ui->wf_palette, span > 0 ? (v - ui->wf_floor_db) / span : 0.0f);
    }
    glBindTexture(GL_TEXTURE_2D, ui->wf_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, UI_WF_COLS, UI_WF_ROWS, GL_RGBA, GL_UNSIGNED_BYTE, ui->wf_rgba);
}

/* Widgets */

void label_value(const char *label, const char *fmt, ...)
{
    char text[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    igTableNextRow(0, 0.0f);
    igTableNextColumn();
    igTextUnformatted(label, NULL);
    igTableNextColumn();
    igTextUnformatted(text, NULL);
}

/* Number edited as text, in units of `scale`. The value is taken when the edit ends, not at
 * every key: half-typed numbers do not reach the board. */
bool input_scaled(const char *label, double *value, double scale, const char *fmt)
{
    static ImGuiID editing;
    static double edit;
    const ImGuiID id = igGetID_Str(label);
    double v = editing == id ? edit : *value / scale;
    bool done = false;

    igSetNextItemWidth(150.0f);
    igInputDouble(label, &v, 0.0, 0.0, fmt, 0);
    if (igIsItemActive()) {
        editing = id;
        edit    = v;
    }
    if (igIsItemDeactivatedAfterEdit()) {
        *value = (editing == id ? edit : v) * scale;
        done   = true;
    }
    if (!igIsItemActive() && editing == id)
        editing = 0;
    return done;
}

/* The next control on the same line when `width` pixels are left on it, on a new line if not. */
void same_line_or_wrap(float spacing, float width)
{
    ImVec2 avail;

    igSameLine(0.0f, spacing);
    igGetContentRegionAvail(&avail);
    if (avail.x < width)
        igNewLine();
}

static bool input_mhz(const char *label, double *hz)
{
    return input_scaled(label, hz, 1e6, "%.6f");
}

static bool input_khz(const char *label, double *hz)
{
    return input_scaled(label, hz, 1e3, "%.3f");
}

static void view_full(struct an_ui *ui)
{
    ui->view_lo = -0.5 * ui->set.sample_rate;
    ui->view_hi =  0.5 * ui->set.sample_rate;
}

static void view_clamp(struct an_ui *ui)
{
    const double half = 0.5 * ui->set.sample_rate;
    const double min_span = 16.0 * ui->set.sample_rate / (double)ui->set.fft_len;

    if (ui->view_hi - ui->view_lo < min_span) {
        const double mid = 0.5 * (ui->view_lo + ui->view_hi);
        ui->view_lo = mid - 0.5 * min_span;
        ui->view_hi = mid + 0.5 * min_span;
    }
    if (ui->view_hi - ui->view_lo > 2.0 * half) {
        ui->view_lo = -half;
        ui->view_hi = half;
    }
    if (ui->view_lo < -half) {
        ui->view_hi += -half - ui->view_lo;
        ui->view_lo  = -half;
    }
    if (ui->view_hi > half) {
        ui->view_lo -= ui->view_hi - half;
        ui->view_hi  = half;
    }
}

/* Panels */

static void draw_controls(struct an_ui *ui, struct an_shared *sh, bool *changed)
{
    static const char *const fft_names[] = { "256", "1024", "4096", "16384", "65536", "262144", "1048576" };
    static const int fft_values[] = { 256, 1024, 4096, 16384, 65536, 262144, 1048576 };
    static const char *const palettes[] = { "Turbo", "Plasma", "Viridis" };
    const struct an_source *src = sh->src;
    bool run;
    int i, fft_idx = 3;

    pthread_mutex_lock(&sh->lock);
    run = sh->run;
    pthread_mutex_unlock(&sh->lock);

    igSeparatorText(run ? "Acquisition: running" : "Acquisition: stopped");
    if (igButton(run ? "Stop" : "Run", (ImVec2){ 70.0f, 0.0f })) {
        pthread_mutex_lock(&sh->lock);
        sh->run = !run;
        pthread_mutex_unlock(&sh->lock);
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Single", (ImVec2){ 70.0f, 0.0f })) {
        pthread_mutex_lock(&sh->lock);
        sh->run    = false;
        sh->single = true;
        pthread_mutex_unlock(&sh->lock);
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Restart", (ImVec2){ 70.0f, 0.0f }))
        traces_reset(ui);

    igSeparatorText(src->live ? "Board" : "File");
    if (src->live && src->no_rf_init) {
        /* The RFIC is the one of another utility: nothing here would reach it. */
        igText("%.6f MHz, %.3f MSPS", ui->set.freq_hz / 1e6, ui->set.sample_rate / 1e6);
        igPushTextWrapPos(0.0f);
        igTextUnformatted("RFIC left as it is (--no-rf-init): its frequency, rate and gain are set with m2sdr_rf.", NULL);
        igPopTextWrapPos();
        if (igRadioButton_IntPtr("RX1", &ui->set.channel, 0))
            *changed = true;
        igSameLine(0.0f, -1.0f);
        if (igRadioButton_IntPtr("RX2", &ui->set.channel, 1))
            *changed = true;
    } else if (src->live) {
        if (input_mhz("Centre (MHz)", &ui->set.freq_hz)) {
            *changed = true;
            traces_reset(ui);
        }
        {
            double rate = ui->set.sample_rate;
            if (input_scaled("Rate (MSPS)", &rate, 1e6, "%.6f") && rate >= 0.5e6 && rate <= 61.44e6) {
                ui->set.sample_rate = rate;
                ui->set.bandwidth   = 0;
                view_full(ui);
                traces_reset(ui);
                *changed = true;
            }
        }
        if (igCheckbox("AGC", &ui->set.agc))
            *changed = true;
        if (!ui->set.agc) {
            igSetNextItemWidth(150.0f);
            if (igSliderInt("Gain (dB)", &ui->set.rx_gain, 0, 73, "%d", 0))
                *changed = true;
        }
        if (igRadioButton_IntPtr("RX1", &ui->set.channel, 0))
            *changed = true;
        igSameLine(0.0f, -1.0f);
        if (igRadioButton_IntPtr("RX2", &ui->set.channel, 1))
            *changed = true;
    } else {
        igText("%.3f MSPS, %d channel%s", ui->set.sample_rate / 1e6, src->nchannels, src->nchannels == 1 ? "" : "s");
        igText("%.1f %% of %.3f s", src->total_sets ? 100.0 * (double)src->position / (double)src->total_sets : 0.0,
               (double)src->total_sets / ui->set.sample_rate);
        for (i = 0; i < src->nchannels && i < 2; i++) {
            char name[8];
            snprintf(name, sizeof(name), "RX%d", i + 1);
            if (i)
                igSameLine(0.0f, -1.0f);
            if (igRadioButton_IntPtr(name, &ui->set.channel, i))
                *changed = true;
        }
    }

    igSeparatorText("Spectrum");
    for (i = 0; i < (int)(sizeof(fft_values) / sizeof(fft_values[0])); i++) {
        if (fft_values[i] == ui->set.fft_len)
            fft_idx = i;
    }
    igSetNextItemWidth(150.0f);
    if (igCombo_Str_arr("FFT length", &fft_idx, fft_names, (int)(sizeof(fft_values) / sizeof(fft_values[0])), -1)) {
        ui->set.fft_len = fft_values[fft_idx];
        traces_reset(ui);
        *changed = true;
    }
    igSetNextItemWidth(150.0f);
    if (igCombo_Str_arr("Window", &ui->set.window, window_names, 4, -1)) {
        traces_reset(ui);
        *changed = true;
    }
    igSetNextItemWidth(150.0f);
    if (igInputInt("Averages", &ui->set.averages, 1, 8, 0)) {
        if (ui->set.averages < 1)
            ui->set.averages = 1;
        if (ui->set.averages > AN_MAX_AVERAGES)
            ui->set.averages = AN_MAX_AVERAGES;
        *changed = true;
    }
    if (ui->res.psd.bins)
        igText("RBW %.1f Hz", ui->res.psd.rbw_hz);

    igSeparatorText("Traces");
    for (i = 0; i < UI_TRACES; i++) {
        if (i != 0 && i != 2)
            igSameLine(0.0f, -1.0f);
        /* The name of a trace in its colour. */
        igPushStyleColor_U32(ImGuiCol_Text, trace_colors[i]);
        igCheckbox(trace_names[i], &ui->trace_on[i]);
        igPopStyleColor(1);
    }
    igSetNextItemWidth(150.0f);
    if (igInputInt("Trace average", &ui->avg_target, 1, 8, 0)) {
        if (ui->avg_target < 1)
            ui->avg_target = 1;
    }

    igSeparatorText("Scale");
    {
        float ref = (float)ui->ref_db, range = (float)ui->range_db;
        igSetNextItemWidth(150.0f);
        if (igDragFloat("Top (dB)", &ref, 0.5f, -200.0f, 100.0f, "%.0f", 0))
            ui->ref_db = ref;
        igSetNextItemWidth(150.0f);
        if (igDragFloat("Range (dB)", &range, 0.5f, 10.0f, 200.0f, "%.0f", 0))
            ui->range_db = range;
    }
    {
        double off = ui->set.ref_offset_db;
        if (input_scaled("Offset (dB)", &off, 1.0, "%.2f")) {
            ui->set.ref_offset_db = off;
            traces_reset(ui);
            *changed = true;
        }
    }
    if (igButton("Full span", (ImVec2){ 0.0f, 0.0f }))
        view_full(ui);

    igSeparatorText("Channel");
    if (input_khz("Width (kHz)", &ui->set.chan_bw_hz))
        *changed = true;
    if (input_khz("Offset (kHz)", &ui->set.chan_offset_hz))
        *changed = true;
    if (input_khz("Spacing (kHz)", &ui->set.chan_spacing_hz))
        *changed = true;

    igSeparatorText("Waterfall");
    igCheckbox("Show", &ui->wf_on);
    igSameLine(0.0f, -1.0f);
    if (igButton("Auto levels", (ImVec2){ 0.0f, 0.0f }))
        ui->wf_auto = true;
    igSetNextItemWidth(150.0f);
    igCombo_Str_arr("Colours", &ui->wf_palette, palettes, 3, -1);
    igSetNextItemWidth(150.0f);
    igDragFloat("Floor (dB)", &ui->wf_floor_db, 0.5f, -200.0f, 100.0f, "%.0f", 0);
    igSetNextItemWidth(150.0f);
    igDragFloat("Ceiling (dB)", &ui->wf_ceil_db, 0.5f, -200.0f, 100.0f, "%.0f", 0);
}

static void draw_spectrum(struct an_ui *ui, bool live, bool *changed)
{
    const struct m2sdr_psd *psd = &ui->res.psd;
    const double half = 0.5 * ui->res.set.sample_rate;
    const double centre = ui->res.set.freq_hz;
    const char *unit = ui->res.set.ref_offset_db != 0.0 ? "dBm" : "dBFS";
    struct m2sdr_plot plot;
    ImVec2 avail;
    float plot_h;
    char text[96];
    int t, m;

    igGetContentRegionAvail(&avail);
    plot_h = ui->wf_on ? 0.62f * avail.y : avail.y;
    if (!m2sdr_plot_begin(&plot, "##spectrum", (ImVec2){ avail.x, plot_h }, centre + ui->view_lo, centre + ui->view_hi,
                          M2SDR_PLOT_FREQ, ui->ref_db - ui->range_db, ui->ref_db, unit))
        return;

    if (ui->res.set.chan_bw_hz > 0) {
        const double c = centre + ui->res.set.chan_offset_hz, bw = ui->res.set.chan_bw_hz;
        const double spacing = ui->res.set.chan_spacing_hz > 0 ? ui->res.set.chan_spacing_hz : bw;
        int n;

        m2sdr_plot_band(&plot, c - 0.5 * bw, c + 0.5 * bw, 0x3060ff60u);
        for (n = 1; ui->res.acpr_valid && n <= ui->res.acpr.adjacent; n++) {
            m2sdr_plot_band(&plot, c - n * spacing - 0.5 * bw, c - n * spacing + 0.5 * bw, 0x20ffa040u);
            m2sdr_plot_band(&plot, c + n * spacing - 0.5 * bw, c + n * spacing + 0.5 * bw, 0x20ffa040u);
        }
    }
    if (ui->res.obw_valid) {
        m2sdr_plot_vline(&plot, centre + ui->res.obw_lo_hz, 0x80ffff60u);
        m2sdr_plot_vline(&plot, centre + ui->res.obw_hi_hz, 0x80ffff60u);
    }
    if (psd->bins && ui->bins == psd->bins) {
        for (t = UI_TRACES - 1; t >= 0; t--) {
            if (ui->trace_on[t])
                m2sdr_plot_trace(&plot, ui->trace[t], ui->bins, centre - half, centre + half - psd->bin_hz, trace_colors[t]);
        }
        for (m = 0; m < UI_MARKERS; m++) {
            if (!ui->markers[m].on)
                continue;
            snprintf(text, sizeof(text), "M%d", m + 1);
            m2sdr_plot_marker(&plot, centre + ui->markers[m].offset_hz,
                              trace_at(ui, marker_trace(ui), ui->markers[m].offset_hz), text, marker_colors[m]);
        }
    }
    if (plot.hovered) {
        ImGuiIO *io = igGetIO();
        char freq[48];

        m2sdr_plot_format(freq, sizeof(freq), plot.mouse_x, psd->bins ? psd->bin_hz : 1.0, M2SDR_PLOT_FREQ);
        snprintf(text, sizeof(text), "%s  %.1f %s", freq, trace_at(ui, marker_trace(ui), plot.mouse_x - centre), unit);
        m2sdr_plot_text(&plot, -260.0f, 4.0f, text, 0xffffffffu);
        m2sdr_plot_vline(&plot, plot.mouse_x, 0x40ffffffu);

        /* Left click: the selected marker. Wheel: zoom around the mouse. Right drag: pan.
         * Double click: back to the full span. */
        if (plot.clicked) {
            ui->markers[ui->marker_sel].on        = true;
            ui->markers[ui->marker_sel].offset_hz = plot.mouse_x - centre;
        }
        if (io->MouseWheel != 0.0f) {
            const double factor = pow(0.8, io->MouseWheel);
            const double at = plot.mouse_x - centre;
            ui->view_lo = at - (at - ui->view_lo) * factor;
            ui->view_hi = at + (ui->view_hi - at) * factor;
            view_clamp(ui);
        }
        if (igIsMouseDragging(1, 2.0f)) {
            const double per_px = (ui->view_hi - ui->view_lo) / (plot.p1.x - plot.p0.x);
            ui->view_lo -= io->MouseDelta.x * per_px;
            ui->view_hi -= io->MouseDelta.x * per_px;
            view_clamp(ui);
        }
        if (igIsMouseDoubleClicked_Nil(0))
            view_full(ui);
    }
    if (ui->res.lost)
        m2sdr_plot_text(&plot, 8.0f, 22.0f, "Samples lost in the capture", 0xff4040ffu);
    m2sdr_plot_end(&plot);

    if (ui->wf_on) {
        ImTextureRef tex;
        ImVec2 pos;
        const float u0 = (float)((ui->view_lo + half) / (2.0 * half));
        const float u1 = (float)((ui->view_hi + half) / (2.0 * half));

        igGetCursorScreenPos(&pos);
        igGetContentRegionAvail(&avail);
        tex._TexData = NULL;
        tex._TexID   = (ImTextureID)(uintptr_t)ui->wf_tex;
        ImDrawList_AddImage(plot.dl, tex, (ImVec2){ plot.p0.x, pos.y }, (ImVec2){ plot.p1.x, pos.y + avail.y - 4.0f },
                            (ImVec2){ u0, 0.0f }, (ImVec2){ u1, 1.0f }, 0xffffffffu);
        igDummy((ImVec2){ avail.x, avail.y - 4.0f });
    }
    (void)live;
    (void)changed;
}

static void draw_time(struct an_ui *ui, bool live, bool *changed)
{
    static float re[AN_TIME_POINTS], im[AN_TIME_POINTS], mag[AN_TIME_POINTS];
    static int points = 1024;
    const size_t n = ui->res.time_count < (size_t)points ? ui->res.time_count : (size_t)points;
    const double dt = ui->res.set.sample_rate > 0 ? 1.0 / ui->res.set.sample_rate : 1.0;
    struct m2sdr_plot plot;
    ImVec2 avail;
    float peak = 1e-6f;
    size_t i;

    (void)live;
    (void)changed;
    igSetNextItemWidth(200.0f);
    igSliderInt("Samples shown", &points, 64, AN_TIME_POINTS, "%d", ImGuiSliderFlags_Logarithmic);
    if (n < 2)
        return;
    for (i = 0; i < n; i++) {
        re[i]  = crealf(ui->res.time[i]);
        im[i]  = cimagf(ui->res.time[i]);
        mag[i] = (float)(m2sdr_power_db(re[i] * re[i] + im[i] * im[i]) + ui->res.set.ref_offset_db);
        if (fabsf(re[i]) > peak)
            peak = fabsf(re[i]);
        if (fabsf(im[i]) > peak)
            peak = fabsf(im[i]);
    }
    peak *= 1.1f;
    igGetContentRegionAvail(&avail);
    if (m2sdr_plot_begin(&plot, "##iq", (ImVec2){ avail.x, 0.5f * avail.y }, 0.0, dt * (double)(n - 1), M2SDR_PLOT_TIME,
                         -peak, peak, "I (blue), Q (orange), full scale 1")) {
        m2sdr_plot_trace(&plot, re, n, 0.0, dt * (double)(n - 1), 0xffffa040u);
        m2sdr_plot_trace(&plot, im, n, 0.0, dt * (double)(n - 1), 0xff40a0ffu);
        m2sdr_plot_end(&plot);
    }
    if (m2sdr_plot_begin(&plot, "##mag", (ImVec2){ avail.x, 0.0f }, 0.0, dt * (double)(n - 1), M2SDR_PLOT_TIME,
                         ui->res.stats.mean_db + ui->res.set.ref_offset_db - 40.0,
                         ui->res.stats.peak_db + ui->res.set.ref_offset_db + 3.0,
                         ui->res.set.ref_offset_db != 0.0 ? "Power, dBm; line: the mean" : "Power, dBFS; line: the mean")) {
        m2sdr_plot_trace(&plot, mag, n, 0.0, dt * (double)(n - 1), 0xff60ff60u);
        m2sdr_plot_hline(&plot, ui->res.stats.mean_db + ui->res.set.ref_offset_db, 0x80ffffffu);
        m2sdr_plot_end(&plot);
    }
}

static void draw_statistics(struct an_ui *ui, bool live, bool *changed)
{
    static float x[M2SDR_CCDF_POINTS], y[M2SDR_CCDF_POINTS], g[M2SDR_CCDF_POINTS];
    static float cx[AN_TIME_POINTS], cy[AN_TIME_POINTS];
    struct m2sdr_plot plot;
    ImVec2 avail, sq;
    float side, peak = 1e-6f;
    size_t i;
    int k;

    (void)live;
    (void)changed;
    /* CCDF on a log scale, with the one of Gaussian noise as a reference. */
    for (k = 0; k < M2SDR_CCDF_POINTS; k++) {
        const double db = k * M2SDR_CCDF_STEP_DB;
        x[k] = (float)db;
        y[k] = (float)log10(ui->res.stats.ccdf[k] > 1e-7 ? ui->res.stats.ccdf[k] : 1e-7);
        g[k] = (float)(-pow(10.0, db / 10.0) / log(10.0));
    }
    igGetContentRegionAvail(&avail);
    side = avail.y < 0.5f * avail.x ? avail.y : 0.5f * avail.x;
    sq   = m2sdr_plot_square(side);
    if (m2sdr_plot_begin(&plot, "##ccdf", (ImVec2){ avail.x - sq.x - 8.0f, side },
                         0.0, (M2SDR_CCDF_POINTS - 1) * M2SDR_CCDF_STEP_DB, M2SDR_PLOT_PLAIN,
                         -6.0, 0.0, "CCDF, log10, against dB above the mean power")) {
        m2sdr_plot_trace(&plot, g, M2SDR_CCDF_POINTS, x[0], x[M2SDR_CCDF_POINTS - 1], 0x80ffffffu);
        m2sdr_plot_trace(&plot, y, M2SDR_CCDF_POINTS, x[0], x[M2SDR_CCDF_POINTS - 1], 0xff40d0ffu);
        m2sdr_plot_text(&plot, 8.0f, -20.0f, "grey: Gaussian noise", 0xffc8c8c8u);
        m2sdr_plot_end(&plot);
    }
    igSameLine(0.0f, 8.0f);
    for (i = 0; i < ui->res.time_count; i++) {
        cx[i] = crealf(ui->res.time[i]);
        cy[i] = cimagf(ui->res.time[i]);
        if (fabsf(cx[i]) > peak)
            peak = fabsf(cx[i]);
        if (fabsf(cy[i]) > peak)
            peak = fabsf(cy[i]);
    }
    peak *= 1.1f;
    if (m2sdr_plot_begin(&plot, "##iqplane", sq, -peak, peak, M2SDR_PLOT_PLAIN, -peak, peak, "Q against I, full scale 1")) {
        m2sdr_plot_points(&plot, cx, cy, ui->res.time_count, 1.0f, 0xc040d0ffu);
        m2sdr_plot_end(&plot);
    }
}

static void draw_modulation(struct an_ui *ui, bool live, bool *changed)
{
    static const char *const mods[] = { "Auto", "BPSK", "QPSK", "8PSK", "16QAM", "32QAM", "64QAM", "128QAM", "256QAM" };
    static const char *const pulses[] = { "Root raised cosine", "Raised cosine", "Rectangular" };
    static const char *const sizes[] = { "16384", "32768", "65536", "131072", "262144", "524288", "1048576", "2097152", "4194304" };
    static float sx[M2SDR_DEMOD_MAX_SYMBOLS], sy[M2SDR_DEMOD_MAX_SYMBOLS], evm[M2SDR_DEMOD_MAX_SYMBOLS];
    static float complex points[256];
    static float px[256], py[256];
    const struct m2sdr_demod_result *d = &ui->res.demod;
    struct m2sdr_plot plot;
    ImVec2 avail, sq;
    float side, lim = 1.0f, far = 0.0f, eye_h;
    int mod = (int)ui->set.demod.modulation, pulse = (int)ui->set.demod.pulse;
    int idx = ui->set.demod_log2 - AN_DEMOD_MIN_LOG2, npoints, i, m, rail;
    float evm_top = 1.0f;
    size_t k;

    (void)live;
    igSetNextItemWidth(90.0f);
    if (igCombo_Str_arr("Type", &mod, mods, M2SDR_MOD_COUNT, -1)) {
        ui->set.demod.modulation = (enum m2sdr_modulation)mod;
        *changed = true;
    }
    same_line_or_wrap(12.0f, 215.0f);
    igSetNextItemWidth(170.0f);
    if (igCombo_Str_arr("Pulse", &pulse, pulses, 3, -1)) {
        ui->set.demod.pulse = (enum m2sdr_pulse)pulse;
        *changed = true;
    }
    same_line_or_wrap(12.0f, 295.0f);
    if (input_scaled("Symbols/s (0: auto)", &ui->set.demod.symbol_rate, 1.0, "%.1f"))
        *changed = true;
    same_line_or_wrap(12.0f, 285.0f);
    {
        double a = ui->set.demod.rolloff;
        if (input_scaled("Roll-off (0: auto)", &a, 1.0, "%.2f") && (a == 0 || (a >= 0.05 && a <= 1.0))) {
            ui->set.demod.rolloff = a;
            *changed = true;
        }
    }
    igSetNextItemWidth(110.0f);
    if (igCombo_Str_arr("Samples", &idx, sizes, AN_DEMOD_MAX_LOG2 - AN_DEMOD_MIN_LOG2 + 1, -1)) {
        ui->set.demod_log2 = idx + AN_DEMOD_MIN_LOG2;
        *changed = true;
    }

    if (!ui->res.set.demod_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!ui->res.demod_valid || !d->signal) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted("No signal with a symbol rate was found: tune on it, with a sample rate of a few times its "
                          "bandwidth, or give the symbol rate.", NULL);
        igPopTextWrapPos();
        return;
    }
    same_line_or_wrap(16.0f, 150.0f);
    if (d->locked)
        igTextColored((ImVec4){ 0.4f, 1.0f, 0.4f, 1.0f }, "%s locked", m2sdr_modulation_name(d->modulation));
    else
        igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "%s NOT LOCKED: the figures are not valid", m2sdr_modulation_name(d->modulation));

    npoints = m2sdr_constellation(d->modulation, points);
    for (i = 0; i < npoints; i++) {
        px[i] = crealf(points[i]);
        py[i] = cimagf(points[i]);
        if (fabsf(px[i]) > lim)
            lim = fabsf(px[i]);
        if (fabsf(py[i]) > lim)
            lim = fabsf(py[i]);
    }
    lim *= 1.25f;
    for (k = 0; k < d->symbols; k++) {
        sx[k]  = crealf(d->sym[k]);
        sy[k]  = cimagf(d->sym[k]);
        evm[k] = 100.0f * cabsf(d->sym[k] - d->ref[k]);
        if (evm[k] > evm_top)
            evm_top = evm[k];
        if (fabsf(sx[k]) > far)
            far = fabsf(sx[k]);
        if (fabsf(sy[k]) > far)
            far = fabsf(sy[k]);
    }
    /* Symbols that are not on the points (another modulation, no lock) are shown too, up to
     * twice the size of the constellation. */
    if (1.05f * far > lim)
        lim = 1.05f * far < 2.0f * lim ? 1.05f * far : 2.0f * lim;

    igGetContentRegionAvail(&avail);
    side = avail.y - 170.0f < 0.5f * avail.x ? avail.y - 170.0f : 0.5f * avail.x;
    if (side < 120.0f)
        side = 120.0f;
    eye_h = 0.5f * side;
    sq    = m2sdr_plot_square(side);

    /* Constellation: the symbols, and the points they are compared with. */
    if (m2sdr_plot_begin(&plot, "##const", sq, -lim, lim, M2SDR_PLOT_PLAIN, -lim, lim,
                         "Q against I; crosses: the points of the constellation")) {
        m2sdr_plot_points(&plot, sx, sy, d->symbols, 1.0f, 0xc040d0ffu);
        for (i = 0; i < npoints; i++) {
            const float x = m2sdr_plot_x(&plot, px[i]), y = m2sdr_plot_y(&plot, py[i]);
            ImDrawList_AddLine(plot.dl, (ImVec2){ x - 5.0f, y }, (ImVec2){ x + 5.0f, y }, 0xffffffffu, 1.0f);
            ImDrawList_AddLine(plot.dl, (ImVec2){ x, y - 5.0f }, (ImVec2){ x, y + 5.0f }, 0xffffffffu, 1.0f);
        }
        m2sdr_plot_end(&plot);
    }
    igSameLine(0.0f, 8.0f);

    /* Eye diagrams of I and of Q: two symbols, the decisions at the two vertical lines. */
    igBeginGroup();
    for (rail = 0; rail < 2; rail++) {
        if (m2sdr_plot_begin(&plot, rail ? "##eyeq" : "##eyei", (ImVec2){ avail.x - sq.x - 8.0f, eye_h },
                             -0.5, 1.5, M2SDR_PLOT_PLAIN, -lim, lim,
                             rail ? "Eye of Q, in symbols; lines: the decisions" :
                                    "Eye of I, in symbols; lines: the decisions")) {
            static float ex[M2SDR_DEMOD_EYE_POINTS], ey[M2SDR_DEMOD_EYE_POINTS];

            m2sdr_plot_vline(&plot, 0.0, 0x60ffffffu);
            m2sdr_plot_vline(&plot, 1.0, 0x60ffffffu);
            for (i = 0; i < d->eye_traces; i++) {
                for (m = 0; m < M2SDR_DEMOD_EYE_POINTS; m++) {
                    ex[m] = (float)(m - 4) / 8.0f;
                    ey[m] = rail ? cimagf(d->eye[i][m]) : crealf(d->eye[i][m]);
                }
                m2sdr_plot_xy(&plot, ex, ey, M2SDR_DEMOD_EYE_POINTS, rail ? 0x6040a0ffu : 0x60ffa040u);
            }
            m2sdr_plot_end(&plot);
        }
    }
    igEndGroup();

    /* Error of each symbol. */
    if (m2sdr_plot_begin(&plot, "##evm", (ImVec2){ 0.0f, 0.0f }, 0.0, (double)(d->symbols - 1), M2SDR_PLOT_PLAIN,
                         0.0, 1.1 * evm_top, "Error vector of each symbol, % of the RMS of the constellation, against its number; line: RMS")) {
        m2sdr_plot_trace(&plot, evm, d->symbols, 0.0, (double)(d->symbols - 1), 0xff60ff60u);
        m2sdr_plot_hline(&plot, 100.0 * d->evm_rms, 0xc0ffffffu);
        m2sdr_plot_end(&plot);
    }
}

static void draw_modulation_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_demod_result *d = &res->demod;

    igSeparatorText("Modulation");
    if (!res->demod_valid || !d->signal) {
        igTextUnformatted("No signal.", NULL);
        return;
    }
    if (igBeginTable("##demod", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Type", "%s%s", m2sdr_modulation_name(d->modulation), d->locked ? "" : " (not locked)");
        label_value("Symbol rate", "%.3f /s", d->symbol_rate);
        label_value("Roll-off", "%.2f", d->rolloff);
        label_value("Carrier", "%.6f MHz", (res->set.freq_hz + d->freq_offset_hz) / 1e6);
        label_value("Carrier offset", "%+.1f Hz", d->freq_offset_hz);
        label_value("Power", "%.2f dBFS", d->power_db);
        label_value("EVM", "%.2f %% RMS", 100.0 * d->evm_rms);
        label_value("EVM peak", "%.2f %%", 100.0 * d->evm_peak);
        label_value("MER", "%.1f dB", d->mer_db);
        label_value("Magnitude error", "%.2f %% RMS", 100.0 * d->mag_err_rms);
        label_value("Phase error", "%.2f deg RMS", d->phase_err_deg);
        label_value("Origin offset", "%.1f dB", d->iq_offset_db);
        label_value("I/Q gain", "%+.3f dB", d->gain_imbalance_db);
        label_value("Quadrature", "%+.3f deg", d->quadrature_deg);
        label_value("Symbols", "%zu", d->symbols);
        igEndTable();
    }
}

static void lte_texture(GLuint *tex, const uint32_t *rgba, int width, int height)
{
    if (!*tex) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

/* Textures of the frame: a column per symbol or per slot, the highest subcarrier at the top. */
static void lte_textures(struct an_ui *ui)
{
    const struct m2sdr_lte_result *l = &ui->res.lte;
    const int k = l->subcarriers, rb = l->data_rb;
    int q, a, ns, b;

    if (!ui->lte_rgba)
        ui->lte_rgba = malloc((size_t)M2SDR_LTE_FRAME_SYMBOLS * M2SDR_LTE_MAX_SUBCARRIERS * sizeof(uint32_t));
    if (!ui->lte_rgba || k <= 0)
        return;
    for (a = 0; a < k; a++) {
        for (q = 0; q < M2SDR_LTE_FRAME_SYMBOLS; q++)
            ui->lte_rgba[(size_t)(k - 1 - a) * M2SDR_LTE_FRAME_SYMBOLS + (size_t)q] =
                palette_color(ui->wf_palette, (float)((l->grid_db[q][a] - (l->rsrp_db + LTE_GRID_BOTTOM_DB)) /
                                                      (LTE_GRID_TOP_DB - LTE_GRID_BOTTOM_DB)));
    }
    lte_texture(&ui->lte_grid_tex, ui->lte_rgba, M2SDR_LTE_FRAME_SYMBOLS, k);
    if (rb <= 0)
        return;
    for (b = 0; b < rb; b++) {
        for (ns = 0; ns < M2SDR_LTE_FRAME_SLOTS; ns++)
            ui->lte_rgba[(size_t)(rb - 1 - b) * M2SDR_LTE_FRAME_SLOTS + (size_t)ns] =
                l->rb_unknown[ns][b] ? LTE_COL_UNREAD : lte_mod_colors[l->rb_modulation[ns][b] % M2SDR_MOD_COUNT];
    }
    lte_texture(&ui->lte_block_tex, ui->lte_rgba, M2SDR_LTE_FRAME_SLOTS, rb);
}

/* A new result: its frame and its symbols are kept, to be shown while the next captures have no
 * cell, or no block of data that is read. */
static void lte_keep(struct an_ui *ui)
{
    const struct m2sdr_lte_result *l = &ui->res.lte;

    if (!ui->res.set.lte_on || !ui->res.lte_valid)
        return;
    if (ui->res.set.freq_hz != ui->lte_freq_hz || ui->res.set.sample_rate != ui->lte_rate || ui->res.set.channel != ui->lte_channel) {
        ui->lte_freq_hz = ui->res.set.freq_hz;
        ui->lte_rate    = ui->res.set.sample_rate;
        ui->lte_channel = ui->res.set.channel;
        ui->lte_last_ok = false;
        ui->lte_npoints = 0;
    }
    if (!l->found)
        return;
    lte_textures(ui);
    if (!ui->lte_last)
        ui->lte_last = malloc(sizeof(*ui->lte_last));
    if (ui->lte_last) {
        *ui->lte_last    = *l;
        ui->lte_last_seq = ui->seq;
        ui->lte_last_ok  = true;
    }
    if (l->ndata > 0) {
        memcpy(ui->lte_points, l->data_points, (size_t)l->ndata * sizeof(l->data_points[0]));
        memcpy(ui->lte_points_mod, l->data_points_modulation, (size_t)l->ndata);
        ui->lte_npoints    = l->ndata;
        ui->lte_points_sfn = l->sfn;
        ui->lte_points_seq = ui->seq;
    }
}

static void lte_close(struct an_ui *ui)
{
    free(ui->lte_rgba);
    free(ui->lte_last);
}

/* The frame shown: the one of the last capture, or the last one with a cell for a few captures. */
static const struct m2sdr_lte_result *lte_shown(const struct an_ui *ui, bool *held)
{
    *held = false;
    if (!ui->res.set.lte_on || !ui->res.lte_valid)
        return NULL;
    if (ui->res.lte.found)
        return &ui->res.lte;
    if (ui->lte_last_ok && ui->seq - ui->lte_last_seq <= AN_LTE_HOLD) {
        *held = true;
        return ui->lte_last;
    }
    return NULL;
}

/* Entry of a legend: a square of the colour and its name, on the line while it fits before `right`. */
static void legend_item(ImU32 color, const char *name, float right)
{
    const float h = igGetTextLineHeight();
    ImVec2 pos, ts;

    igCalcTextSize(&ts, name, NULL, false, -1.0f);
    igGetCursorScreenPos(&pos);
    if (pos.x + h + ts.x > right) {
        igNewLine();
        igGetCursorScreenPos(&pos);
    }
    ImDrawList_AddRectFilled(igGetWindowDrawList(), (ImVec2){ pos.x, pos.y + 2.0f }, (ImVec2){ pos.x + h - 4.0f, pos.y + h - 2.0f },
                             color, 0.0f, 0);
    igDummy((ImVec2){ h - 2.0f, h });
    igSameLine(0.0f, 2.0f);
    igTextUnformatted(name, NULL);
    igSameLine(0.0f, 12.0f);
}

/* Symbols of the blocks of data, in the colour of the modulation of their block. */
static void lte_data_points(struct m2sdr_plot *plot, const float complex *points, const uint8_t *mod, int n, ImU32 alpha)
{
    static const enum m2sdr_modulation order[] = { M2SDR_MOD_256QAM, M2SDR_MOD_64QAM, M2SDR_MOD_16QAM, M2SDR_MOD_QPSK };
    static float px[M2SDR_LTE_MAX_POINTS], py[M2SDR_LTE_MAX_POINTS];
    unsigned m;
    int i;

    for (m = 0; m < sizeof(order) / sizeof(order[0]); m++) {
        size_t count = 0;

        for (i = 0; i < n; i++) {
            if (mod[i] != order[m])
                continue;
            px[count]   = crealf(points[i]);
            py[count++] = cimagf(points[i]);
        }
        m2sdr_plot_points(plot, px, py, count, 1.0f, (lte_mod_colors[order[m]] & 0x00ffffffu) | alpha);
    }
}

static void draw_lte(struct an_ui *ui, bool live, bool *changed)
{
    static const double rates[5] = { 1.92e6, 3.84e6, 7.68e6, 15.36e6, 30.72e6 };
    static float px[M2SDR_LTE_MAX_POINTS], py[M2SDR_LTE_MAX_POINTS];
    static float complex ref[256];
    const struct m2sdr_lte_result *l;
    struct m2sdr_plot plot;
    ImTextureRef tex;
    ImVec2 avail, pos, sq;
    float side, left_w, line, right;
    double half_mhz;
    char text[200];
    bool held, narrow;
    int i;

    if (live) {
        igTextUnformatted("Sample rate:", NULL);
        for (i = 0; i < 5; i++) {
            snprintf(text, sizeof(text), "%.2f MSPS", rates[i] / 1e6);
            same_line_or_wrap(8.0f, 80.0f);
            if (igSmallButton(text)) {
                ui->set.sample_rate = rates[i];
                ui->set.bandwidth   = 0;
                view_full(ui);
                traces_reset(ui);
                *changed = true;
            }
        }
    }
    if (!ui->res.set.lte_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!ui->res.lte_valid) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted("The sample rate must be 1.92, 3.84, 7.68, 15.36 or 30.72 MSPS: it sets how many resource "
                          "blocks around the centre are analysed.", NULL);
        igPopTextWrapPos();
        return;
    }
    l = lte_shown(ui, &held);
    if (!l) {
        igPushTextWrapPos(0.0f);
        igText("No cell found (PSS quality %.1f): the capture must be tuned on the centre of an FDD carrier with a "
               "normal cyclic prefix.", ui->res.lte.sync_quality);
        igPopTextWrapPos();
        return;
    }
    tex._TexData = NULL;
    half_mhz     = 0.5 * 0.015 * (double)l->subcarriers;

    igGetContentRegionAvail(&avail);
    line   = igGetTextLineHeightWithSpacing();
    /* The constellation next to the grid, or under it in a narrow window. */
    narrow = avail.x < LTE_NARROW_W;
    if (narrow) {
        side   = 0.36f * avail.y < avail.x - 40.0f ? 0.36f * avail.y : avail.x - 40.0f;
        sq     = m2sdr_plot_square(side);
        left_w = avail.x;
    } else {
        side   = avail.y - 2.0f * line < 0.45f * avail.x ? avail.y - 2.0f * line : 0.45f * avail.x;
        sq     = m2sdr_plot_square(side);
        left_w = avail.x - sq.x - 8.0f;
    }

    /* Resource grid, and the blocks of data under it on the same time axis. */
    igBeginGroup();
    igGetCursorScreenPos(&pos);
    right = pos.x + left_w;
    igPushTextWrapPos(igGetCursorPosX() + left_w);
    if (held)
        igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "No cell in the last capture (PSS quality %.1f): the last frame read "
                      "is held, with its figures.", ui->res.lte.sync_quality);
    else if (l->mib)
        igText("Resource grid of frame %d, MHz from the centre against the time. Colours: dB over the reference signals.", l->sfn);
    else
        igTextUnformatted("Resource grid of a frame, MHz from the centre against the time. Colours: dB over the reference signals.", NULL);
    igPopTextWrapPos();
    if (m2sdr_plot_begin(&plot, "##ltegrid", (ImVec2){ left_w - LTE_SCALE_W, (narrow ? 0.32f : 0.62f) * avail.y }, 0.0, 0.010,
                         M2SDR_PLOT_TIME, -half_mhz, half_mhz, "")) {
        static const double marks[] = { LTE_GRID_TOP_DB, 0.0, -20.0, LTE_GRID_BOTTOM_DB };
        const float x = plot.p1.x + 14.0f, h = plot.p1.y - plot.p0.y;
        unsigned n;

        tex._TexID = (ImTextureID)(uintptr_t)ui->lte_grid_tex;
        ImDrawList_AddImage(plot.dl, tex, plot.p0, plot.p1, (ImVec2){ 0.0f, 0.0f }, (ImVec2){ 1.0f, 1.0f }, 0xffffffffu);
        if (plot.hovered) {
            int q = (int)(plot.mouse_x / 0.010 * M2SDR_LTE_FRAME_SYMBOLS);
            int a = (int)((plot.mouse_y + half_mhz) / 0.015);

            q = q < 0 ? 0 : q >= M2SDR_LTE_FRAME_SYMBOLS ? M2SDR_LTE_FRAME_SYMBOLS - 1 : q;
            a = a < 0 ? 0 : a >= l->subcarriers ? l->subcarriers - 1 : a;
            snprintf(text, sizeof(text), "%.2f ms (slot %d, symbol %d), %+.3f MHz: %.1f dBFS, %+.1f dB", 1e3 * plot.mouse_x,
                     q / 7, q % 7, plot.mouse_y, l->grid_db[q][a], l->grid_db[q][a] - l->rsrp_db);
            m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
        }
        m2sdr_plot_end(&plot);
        /* Scale of the colours, in dB over a reference element. */
        for (i = 0; i < (int)h; i++)
            ImDrawList_AddRectFilled(plot.dl, (ImVec2){ x, plot.p0.y + (float)i }, (ImVec2){ x + 12.0f, plot.p0.y + (float)i + 1.0f },
                                     palette_color(ui->wf_palette, 1.0f - (float)i / h), 0.0f, 0);
        for (n = 0; n < sizeof(marks) / sizeof(marks[0]); n++) {
            const float y = plot.p0.y + h * (float)((LTE_GRID_TOP_DB - marks[n]) / (LTE_GRID_TOP_DB - LTE_GRID_BOTTOM_DB));
            ImVec2 ts;

            snprintf(text, sizeof(text), marks[n] != 0.0 ? "%+.0f" : "%.0f", marks[n]);
            igCalcTextSize(&ts, text, NULL, false, -1.0f);
            ImDrawList_AddText_Vec2(plot.dl, (ImVec2){ x + 16.0f, y - (n == 0 ? 0.0f : marks[n] == LTE_GRID_BOTTOM_DB ? ts.y : 0.5f * ts.y) },
                                    0xffc8c8c8u, text, NULL);
        }
    }
    if (!l->data) {
        igPushTextWrapPos(igGetCursorPosX() + left_w);
        igTextUnformatted(l->mib ? "Blocks of data: not analysed with four antenna ports." :
                          "Blocks of data: not analysed, the MIB of the cell is not decoded.", NULL);
        igPopTextWrapPos();
    } else {
        const double y0 = (double)l->data_first_rb - (double)l->data_offset / 12.0;

        igPushTextWrapPos(igGetCursorPosX() + left_w);
        igTextUnformatted("Blocks of data, by slot and resource block of the cell:", NULL);
        igPopTextWrapPos();
        legend_item(lte_mod_colors[M2SDR_MOD_QPSK], "QPSK", right);
        legend_item(lte_mod_colors[M2SDR_MOD_16QAM], "16QAM", right);
        legend_item(lte_mod_colors[M2SDR_MOD_64QAM], "64QAM", right);
        legend_item(lte_mod_colors[M2SDR_MOD_256QAM], "256QAM", right);
        if (l->ports > 1)
            legend_item(LTE_COL_UNREAD, "in use, not read", right);
        legend_item(lte_mod_colors[M2SDR_MOD_AUTO], "empty", right);
        igNewLine();
        if (m2sdr_plot_begin(&plot, "##lteblocks", (ImVec2){ left_w - LTE_SCALE_W, narrow ? 0.18f * avail.y : 0.0f }, 0.0, 0.010,
                             M2SDR_PLOT_TIME, y0, y0 + (double)l->subcarriers / 12.0, "")) {
            tex._TexID = (ImTextureID)(uintptr_t)ui->lte_block_tex;
            ImDrawList_AddImage(plot.dl, tex, (ImVec2){ plot.p0.x, m2sdr_plot_y(&plot, l->data_first_rb + l->data_rb) },
                                (ImVec2){ plot.p1.x, m2sdr_plot_y(&plot, l->data_first_rb) },
                                (ImVec2){ 0.0f, 0.0f }, (ImVec2){ 1.0f, 1.0f }, 0xffffffffu);
            /* Subframes. */
            for (i = 1; i < 10; i++)
                m2sdr_plot_vline(&plot, 0.001 * i, 0x38ffffffu);
            if (plot.hovered) {
                const int ns = (int)(plot.mouse_x / 0.0005), b = (int)floor(plot.mouse_y) - l->data_first_rb;

                if (ns >= 0 && ns < M2SDR_LTE_FRAME_SLOTS && b >= 0 && b < l->data_rb) {
                    if (l->rb_unknown[ns][b])
                        snprintf(text, sizeof(text), "Slot %d, block %d: in use, not read", ns, b + l->data_first_rb);
                    else if (l->rb_modulation[ns][b] == M2SDR_MOD_AUTO)
                        snprintf(text, sizeof(text), "Slot %d, block %d: empty", ns, b + l->data_first_rb);
                    else
                        snprintf(text, sizeof(text), "Slot %d, block %d: %s, EVM %.2f %%", ns, b + l->data_first_rb,
                                 m2sdr_modulation_name((enum m2sdr_modulation)l->rb_modulation[ns][b]), 100.0 * l->rb_evm[ns][b]);
                    m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
                }
            }
            m2sdr_plot_end(&plot);
        }
    }
    igEndGroup();
    if (!narrow)
        igSameLine(0.0f, 8.0f);

    /* Equalised symbols: data, PBCH and reference signals. */
    igBeginGroup();
    igGetCursorScreenPos(&pos);
    right = pos.x + (narrow ? avail.x : sq.x);
    legend_item(lte_mod_colors[M2SDR_MOD_QPSK], "QPSK", right);
    legend_item(lte_mod_colors[M2SDR_MOD_16QAM], "16QAM", right);
    legend_item(lte_mod_colors[M2SDR_MOD_64QAM], "64QAM", right);
    legend_item(lte_mod_colors[M2SDR_MOD_256QAM], "256QAM", right);
    legend_item(LTE_COL_PBCH, "PBCH", right);
    legend_item(LTE_COL_RS, "reference signals", right);
    igNewLine();
    {
        /* The frames without a block that is read keep the symbols of the last one that had some. */
        const bool own = l->ndata > 0;
        const bool kept = !own && l->data && ui->lte_npoints > 0 && ui->seq - ui->lte_points_seq <= AN_LTE_HOLD;
        const enum m2sdr_modulation most = own && l->data_blocks ? l->data_modulation : M2SDR_MOD_QPSK;
        const int nref = m2sdr_constellation(most, ref);
        char label[96];

        snprintf(label, sizeof(label), "Q against I, equalised; crosses: the points of %s", m2sdr_modulation_name(most));
        if (m2sdr_plot_begin(&plot, "##lteconst", sq, -1.6, 1.6, M2SDR_PLOT_PLAIN, -1.6, 1.6, label)) {
            if (own)
                lte_data_points(&plot, l->data_points, l->data_points_modulation, l->ndata, 0xc0000000u);
            else if (kept)
                lte_data_points(&plot, ui->lte_points, ui->lte_points_mod, ui->lte_npoints, 0x50000000u);
            for (i = 0; i < l->npbch; i++) {
                px[i] = crealf(l->pbch_points[i]);
                py[i] = cimagf(l->pbch_points[i]);
            }
            m2sdr_plot_points(&plot, px, py, (size_t)l->npbch, 1.5f, LTE_COL_PBCH);
            for (i = 0; i < l->nrs; i++) {
                px[i] = crealf(l->rs_points[i]);
                py[i] = cimagf(l->rs_points[i]);
            }
            m2sdr_plot_points(&plot, px, py, (size_t)l->nrs, 1.0f, (LTE_COL_RS & 0x00ffffffu) | 0xc0000000u);
            for (i = 0; i < nref; i++) {
                const float x = m2sdr_plot_x(&plot, crealf(ref[i])), y = m2sdr_plot_y(&plot, cimagf(ref[i]));
                ImDrawList_AddLine(plot.dl, (ImVec2){ x - 4.0f, y }, (ImVec2){ x + 5.0f, y }, 0xa0ffffffu, 1.0f);
                ImDrawList_AddLine(plot.dl, (ImVec2){ x, y - 4.0f }, (ImVec2){ x, y + 5.0f }, 0xa0ffffffu, 1.0f);
            }
            if (kept) {
                snprintf(text, sizeof(text), "No data read in this frame: dimmed symbols of frame %d", ui->lte_points_sfn);
                m2sdr_plot_text(&plot, 5.0f, -20.0f, text, 0xff60a0ffu);
            } else if (!own && l->data) {
                m2sdr_plot_text(&plot, 5.0f, -20.0f, "No data read in this frame", 0xff60a0ffu);
            }
            m2sdr_plot_end(&plot);
        }
    }
    igEndGroup();
}

/* OFDM views */
/*------------*/

/* A result of the tuning of `set`; true when the tuning is a new one and the trend restarted. A
 * result without the signal (ok false) counts as 0 dB in the trace. */
static bool trend_add(struct an_trend *tr, const struct an_settings *set, bool ok, double mer, const float *mer_bin,
                      const float *channel_bin, int bins, const float *echo, int echo_bins)
{
    bool fresh = false;
    int i;

    if (set->freq_hz != tr->freq_hz || set->sample_rate != tr->rate || set->channel != tr->channel) {
        tr->freq_hz = set->freq_hz;
        tr->rate    = set->sample_rate;
        tr->channel = set->channel;
        tr->nmer    = 0;
        tr->navg    = 0;
        fresh       = true;
    }
    if (ok) {
        /* The first ones weigh more, so the traces settle at once after a change. */
        const float w = tr->navg < 8 ? 1.0f / (float)(tr->navg + 1) : AN_DVBT_TRACE_AVG;

        for (i = 0; i < bins && i < AN_TREND_BINS; i++) {
            tr->mer_bin[i]     += w * (mer_bin[i] - tr->mer_bin[i]);
            tr->channel_bin[i] += w * (channel_bin[i] - tr->channel_bin[i]);
        }
        for (i = 0; i < echo_bins && i < AN_TREND_BINS; i++)
            tr->echo[i] += w * (echo[i] - tr->echo[i]);
        tr->navg++;
    }
    if (tr->nmer == AN_DVBT_HISTORY) {
        memmove(tr->mer, tr->mer + 1, (AN_DVBT_HISTORY - 1) * sizeof(tr->mer[0]));
        tr->nmer--;
    }
    tr->mer[tr->nmer++] = ok ? (float)mer : 0.0f;
    return fresh;
}

/* MER of the last results, in dB: the figure shown, steadier than one short capture. */
static double trend_mer_shown(const struct an_trend *tr, double last)
{
    const int n = tr->nmer < AN_DVBT_MER_MEAN ? tr->nmer : AN_DVBT_MER_MEAN;
    double sum = 0;
    int i;

    if (n == 0)
        return last;
    for (i = tr->nmer - n; i < tr->nmer; i++)
        sum += tr->mer[i];
    return sum / n;
}

/* The MER, large, a line under it, and its trace over the results: `height` pixels in all. The
 * figure is red under the MER needed when there is one. */
static void draw_trend_mer(const char *id, const struct an_trend *tr, bool ok, double last, double needed, const char *line,
                           float width, float height)
{
    struct m2sdr_plot plot;
    const float y0 = igGetCursorPosY();
    const double mer = trend_mer_shown(tr, last);
    float h;

    igPushFont(NULL, 2.4f * igGetStyle()->FontSizeBase);
    if (ok)
        igTextColored(isfinite(needed) && mer < needed ? (ImVec4){ 1.0f, 0.45f, 0.35f, 1.0f } : (ImVec4){ 0.4f, 1.0f, 0.5f, 1.0f },
                      "MER %.1f dB", mer);
    else
        igTextColored((ImVec4){ 1.0f, 0.45f, 0.35f, 1.0f }, "MER --");
    igPopFont();
    igPushTextWrapPos(igGetCursorPosX() + width);
    igTextUnformatted(line, NULL);
    igPopTextWrapPos();
    h = height - (igGetCursorPosY() - y0);
    if (m2sdr_plot_begin(&plot, id, (ImVec2){ width, h > 80.0f ? h : 80.0f }, -(double)(AN_DVBT_HISTORY - 1), 0.0, M2SDR_PLOT_PLAIN,
                         0.0, 40.0, "MER (dB), one point per result")) {
        if (isfinite(needed))
            m2sdr_plot_hline(&plot, needed, DVBT_COL_NEEDED);
        if (tr->nmer > 0)
            m2sdr_plot_trace(&plot, tr->mer, (size_t)tr->nmer, -(double)(tr->nmer - 1), 0.0, DVBT_COL_MER);
        m2sdr_plot_end(&plot);
    }
}

/* Equalised points, and the crosses of their constellation. */
static void draw_trend_points(const char *id, ImVec2 size, const float complex *points, int n, enum m2sdr_modulation mod)
{
    static float px[4096], py[4096];
    static float complex ref[256];
    struct m2sdr_plot plot;
    const int nref = m2sdr_constellation(mod, ref);
    char label[96];
    int i;

    snprintf(label, sizeof(label), "Q against I, equalised data carriers; crosses: %s", m2sdr_modulation_name(mod));
    if (!m2sdr_plot_begin(&plot, id, size, -1.6, 1.6, M2SDR_PLOT_PLAIN, -1.6, 1.6, label))
        return;
    n = n < 4096 ? n : 4096;
    for (i = 0; i < n; i++) {
        px[i] = crealf(points[i]);
        py[i] = cimagf(points[i]);
    }
    m2sdr_plot_points(&plot, px, py, (size_t)n, 1.0f, 0x90ffd080u);
    for (i = 0; i < nref; i++) {
        const float x = m2sdr_plot_x(&plot, crealf(ref[i])), y = m2sdr_plot_y(&plot, cimagf(ref[i]));
        ImDrawList_AddLine(plot.dl, (ImVec2){ x - 4.0f, y }, (ImVec2){ x + 5.0f, y }, 0xa0ffffffu, 1.0f);
        ImDrawList_AddLine(plot.dl, (ImVec2){ x, y - 4.0f }, (ImVec2){ x, y + 5.0f }, 0xa0ffffffu, 1.0f);
    }
    m2sdr_plot_end(&plot);
}

/* Channel and MER across the band (bins groups from -half_hz to half_hz), next to the echo
 * profile with the guard interval shaded: echoes inside it do no harm by themselves. */
static void draw_trend_traces(const char *id, const struct an_trend *tr, int bins, double half_hz, double echo_start_us,
                              double echo_step_us, int echo_bins, double guard_us, double needed)
{
    struct m2sdr_plot plot;
    ImVec2 avail;
    char key[64], text[160];

    igGetContentRegionAvail(&avail);
    legend_item(DVBT_COL_CHANNEL, "channel (dB from its mean), averaged", avail.x);
    legend_item(DVBT_COL_MER, "MER (dB), averaged", avail.x);
    igNewLine();
    snprintf(key, sizeof(key), "%schan", id);
    if (m2sdr_plot_begin(&plot, key, (ImVec2){ 0.5f * avail.x - 4.0f, 0.0f }, -half_hz, half_hz, M2SDR_PLOT_FREQ, -20.0, 40.0, "")) {
        m2sdr_plot_hline(&plot, 0.0, 0x40ffffffu);
        if (isfinite(needed))
            m2sdr_plot_hline(&plot, needed, DVBT_COL_NEEDED);
        m2sdr_plot_trace(&plot, tr->channel_bin, (size_t)bins, -half_hz, half_hz, DVBT_COL_CHANNEL);
        m2sdr_plot_trace(&plot, tr->mer_bin, (size_t)bins, -half_hz, half_hz, DVBT_COL_MER);
        if (plot.hovered) {
            int b = (int)((plot.mouse_x + half_hz) / (2.0 * half_hz) * bins);

            b = b < 0 ? 0 : b >= bins ? bins - 1 : b;
            snprintf(text, sizeof(text), "%+.3f MHz: channel %+.1f dB, MER %.1f dB", plot.mouse_x / 1e6, tr->channel_bin[b],
                     tr->mer_bin[b]);
            m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
        }
        m2sdr_plot_end(&plot);
    }
    igSameLine(0.0f, 8.0f);
    {
        const double x0 = echo_start_us * 1e-6, x1 = (echo_start_us + echo_bins * echo_step_us) * 1e-6;

        snprintf(key, sizeof(key), "%secho", id);
        if (m2sdr_plot_begin(&plot, key, (ImVec2){ 0.0f, 0.0f }, x0, x1, M2SDR_PLOT_TIME, -40.0, 0.0,
                             "Echoes (dB from the strongest path), averaged")) {
            m2sdr_plot_band(&plot, 0.0, guard_us * 1e-6 < x1 ? guard_us * 1e-6 : x1, 0x2040ff40u);
            m2sdr_plot_trace(&plot, tr->echo, (size_t)echo_bins, x0, x1, DVBT_COL_MER);
            if (plot.hovered) {
                int b = (int)((plot.mouse_x - x0) / (x1 - x0) * echo_bins);

                b = b < 0 ? 0 : b >= echo_bins ? echo_bins - 1 : b;
                snprintf(text, sizeof(text), "%+.2f us (%.0f m): %.1f dB", 1e6 * plot.mouse_x, 299.792458 * 1e6 * plot.mouse_x,
                         tr->echo[b]);
                m2sdr_plot_text(&plot, 8.0f, 4.0f, text, 0xffffffffu);
            }
            m2sdr_plot_end(&plot);
        }
    }
}

static void view_retune(struct an_ui *ui, double freq_hz, bool *changed)
{
    ui->set.freq_hz = freq_hz;
    traces_reset(ui);
    *changed = true;
}

static void view_rate(struct an_ui *ui, double rate, bool *changed)
{
    ui->set.sample_rate = rate;
    ui->set.bandwidth   = 0;
    view_full(ui);
    traces_reset(ui);
    *changed = true;
}

/* DVB-T */
/*-------*/

/* Worker: the multiplex of the last TPS read, and the captures since. */
struct an_dvbt_track {
    struct an_settings known;
    bool   ok;
    unsigned since;
};

static struct an_dvbt_track dvbt_track;

/* A known multiplex: the TPS read again now and then, with its mode given. */
static void dvbt_worker_prepare(struct an_settings *set, bool live)
{
    struct an_dvbt_track *dv = &dvbt_track;

    set->dvbt_fast = false;
    memset(&set->dvbt_hint, 0, sizeof(set->dvbt_hint));
    if (set->dvbt_on && dv->ok && live && set->freq_hz == dv->known.freq_hz &&
        set->sample_rate == dv->known.sample_rate && set->channel == dv->known.channel) {
        set->dvbt_fast = ++dv->since % AN_DVBT_FULL_EVERY != 0;
        set->dvbt_hint = dv->known.dvbt_hint;
    }
}

static void dvbt_worker_result(const struct an_settings *set, const struct an_result *res)
{
    struct an_dvbt_track *dv = &dvbt_track;
    const struct m2sdr_dvbt_result *t = &res->dvbt;

    if (!set->dvbt_on || !res->dvbt_valid)
        return;
    if (t->tps) {
        dv->known = *set;
        dv->known.dvbt_hint.fft_len    = t->fft_len;
        dv->known.dvbt_hint.guard      = t->guard;
        dv->known.dvbt_hint.modulation = t->tps_modulation;
        dv->ok    = true;
        dv->since = 0;
    } else if (!t->found || !t->pilots) {
        dv->ok = false;
    }
    /* The offset follows every result of the same tuning. */
    if (dv->ok && t->found && t->pilots && set->freq_hz == dv->known.freq_hz) {
        dv->known.dvbt_hint.offset_known   = true;
        dv->known.dvbt_hint.carrier_offset = t->carrier_offset;
    }
}

static void dvbt_keep(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_dvbt_result *t = &res->dvbt;

    if (!res->set.dvbt_on || !res->dvbt_valid)
        return;
    if (trend_add(&ui->dvbt, &res->set, t->found && t->pilots, t->mer_db, t->mer_bin_db, t->channel_bin_db, t->bins,
                  t->echo_db, M2SDR_DVBT_ECHO_BINS) && ui->dvbt_tps)
        ui->dvbt_tps->tps = false;
    if (t->tps) {
        if (!ui->dvbt_tps)
            ui->dvbt_tps = malloc(sizeof(*ui->dvbt_tps));
        if (ui->dvbt_tps)
            *ui->dvbt_tps = *t;
    }
}

static void dvbt_close(struct an_ui *ui)
{
    free(ui->dvbt_tps);
}

/* Result whose TPS fields are shown: this one, or the last one that read it. */
static const struct m2sdr_dvbt_result *dvbt_tps_of(const struct an_ui *ui)
{
    const struct m2sdr_dvbt_result *t = &ui->res.dvbt;

    return t->tps || !ui->dvbt_tps || !ui->dvbt_tps->tps ? t : ui->dvbt_tps;
}

static void draw_dvbt(struct an_ui *ui, bool live, bool *changed)
{
    static const double rates[3] = { 64e6 / 7.0, 8e6, 48e6 / 7.0 };
    static const char *const rate_names[3] = { "8 MHz", "7 MHz", "6 MHz" };
    const struct an_result *res = &ui->res;
    const struct m2sdr_dvbt_result *t = &res->dvbt, *tp;
    const bool ok = t->found && t->pilots;
    ImVec2 avail, sq;
    float side;
    double needed;
    char text[160];
    int i;

    if (live) {
        int ch = (int)lround((ui->set.freq_hz - DVBT_UHF_BASE_HZ) / 8e6);

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
        if (igInputInt("##uhf", &ch, 1, 1, 0) && ch >= 21 && ch <= 69)
            view_retune(ui, DVBT_UHF_BASE_HZ + 8e6 * ch, changed);
        if (ok && res->dvbt_valid && fabs(t->freq_error_hz) > 500.0) {
            igSameLine(0.0f, 16.0f);
            snprintf(text, sizeof(text), "Centre on the multiplex (%+.1f kHz)", t->freq_error_hz / 1e3);
            if (igSmallButton(text))
                view_retune(ui, ui->set.freq_hz + 1e3 * round(t->freq_error_hz / 1e3), changed);
        }
    }
    if (!res->set.dvbt_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->dvbt_valid) {
        igPushTextWrapPos(0.0f);
        igTextUnformatted("The sample rate must be the one of the channel: 9.142857 MSPS for 8 MHz, 8 MSPS for 7 MHz, "
                          "6.857143 MSPS for 6 MHz.", NULL);
        igPopTextWrapPos();
        return;
    }
    if (!t->found) {
        igPushTextWrapPos(0.0f);
        igText("No DVB-T multiplex (guard interval correlation %.3f): tune near the centre of one.", t->sync_quality);
        igPopTextWrapPos();
    }
    tp     = dvbt_tps_of(ui);
    needed = dvbt_needed_db(tp);
    if (isfinite(needed))
        snprintf(text, sizeof(text), "Needed for %s %s: %.1f dB (Gaussian channel), margin %+.1f dB",
                 m2sdr_modulation_name(tp->tps_modulation), tp->code_rate_hp, needed,
                 ok ? trend_mer_shown(&ui->dvbt, t->mer_db) - needed : -needed);
    else
        snprintf(text, sizeof(text), "%s", ok ? "TPS not read: no reference for the MER needed." : " ");

    igGetContentRegionAvail(&avail);
    side = 0.55f * avail.y < 0.42f * avail.x ? 0.55f * avail.y : 0.42f * avail.x;
    sq   = m2sdr_plot_square(side);
    igBeginGroup();
    draw_trend_mer("##dvbtmer", &ui->dvbt, ok, t->mer_db, needed, text, avail.x - sq.x - 8.0f, sq.y);
    igEndGroup();
    igSameLine(0.0f, 8.0f);
    draw_trend_points("##dvbtconst", sq, t->points, ok ? t->npoints : 0, ok ? t->modulation : M2SDR_MOD_64QAM);
    if (ok)
        draw_trend_traces("##dvbt", &ui->dvbt, t->bins, 0.5 * (t->carriers - 1) * res->set.sample_rate / t->fft_len,
                          t->echo_start_us, t->echo_step_us, M2SDR_DVBT_ECHO_BINS, t->guard_us, needed);
}

static void draw_dvbt_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_dvbt_result *t = &res->dvbt, *tp = dvbt_tps_of(ui);
    const double needed = dvbt_needed_db(tp);

    igSeparatorText("DVB-T");
    if (!res->dvbt_valid || !t->found) {
        igTextUnformatted("No multiplex.", NULL);
        return;
    }
    if (igBeginTable("##dvbt", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Mode", "%dk, guard 1/%d", t->fft_len / 1024, t->guard);
        label_value("Guard interval", "%.0f us", t->guard_us);
        label_value("Guard correlation", "%.2f (%.1f dB)", t->sync_quality, t->sync_snr_db);
        label_value("Power", "%.1f dBFS", t->power_dbfs);
        if (t->pilots) {
            label_value("Multiplex", "%.6f MHz", (res->set.freq_hz + t->freq_error_hz) / 1e6);
            label_value("Offset", "%+.1f Hz", t->freq_error_hz);
            if (tp->tps) {
                label_value("Constellation", "%s", m2sdr_modulation_name(tp->tps_modulation));
                label_value("Code rate", tp->tps_hierarchy ? "HP %s, LP %s" : "%s", tp->code_rate_hp, tp->code_rate_lp);
                label_value("Hierarchy", tp->tps_hierarchy ? "alpha %d" : "none", tp->tps_hierarchy);
                if (tp->cell_id >= 0)
                    label_value("Cell", "%d", tp->cell_id);
            } else {
                label_value("TPS", "not read");
            }
            label_value("MER", "%.1f dB (last %.1f)", trend_mer_shown(&ui->dvbt, t->mer_db), t->mer_db);
            if (isfinite(needed))
                label_value("Margin", "%+.1f dB", trend_mer_shown(&ui->dvbt, t->mer_db) - needed);
            if (t->echo_level_db > -99.0)
                label_value("Strongest echo", "%+.2f us, %.1f dB", t->echo_delay_us, t->echo_level_db);
            label_value("Symbols", "%d%s", t->symbols, t->tps ? "" : " (TPS of an earlier capture)");
        } else {
            label_value("Pilots", "not found (%.2f)", t->pilot_quality);
        }
        igEndTable();
    }
}

/* DAB */
/*-----*/

/* Band III blocks (EN 50248): name and centre, kHz. */
static const struct {
    const char *name;
    int khz;
} dab_blocks[] = {
    { "5A", 174928 }, { "5B", 176640 }, { "5C", 178352 }, { "5D", 180064 }, { "6A", 181936 }, { "6B", 183648 },
    { "6C", 185360 }, { "6D", 187072 }, { "7A", 188928 }, { "7B", 190640 }, { "7C", 192352 }, { "7D", 194064 },
    { "8A", 195936 }, { "8B", 197648 }, { "8C", 199360 }, { "8D", 201072 }, { "9A", 202928 }, { "9B", 204640 },
    { "9C", 206352 }, { "9D", 208064 }, { "10A", 209936 }, { "10N", 210096 }, { "10B", 211648 }, { "10C", 213360 },
    { "10D", 215072 }, { "11A", 216928 }, { "11N", 217088 }, { "11B", 218640 }, { "11C", 220352 }, { "11D", 222064 },
    { "12A", 223936 }, { "12N", 224096 }, { "12B", 225648 }, { "12C", 227360 }, { "12D", 229072 }, { "13A", 230784 },
    { "13B", 232496 }, { "13C", 234208 }, { "13D", 235776 }, { "13E", 237488 }, { "13F", 239200 },
};

static void dab_keep(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_dab_result *t = &res->dab;

    if (!res->set.dab_on || !res->dab_valid)
        return;
    trend_add(&ui->dab, &res->set, t->found && t->prs, t->mer_db, t->mer_bin_db, t->channel_bin_db, t->bins, t->echo_db,
              M2SDR_DAB_ECHO_BINS);
}

static void draw_dab(struct an_ui *ui, bool live, bool *changed)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_dab_result *t = &res->dab;
    const bool ok = t->found && t->prs;
    ImVec2 avail, sq;
    float side;
    char text[160];
    int i;

    if (live) {
        const int khz = (int)lround(ui->set.freq_hz / 1e3);
        int sel = -1;

        for (i = 0; i < (int)(sizeof(dab_blocks) / sizeof(dab_blocks[0])); i++)
            if (abs(dab_blocks[i].khz - khz) < 5)
                sel = i;
        igTextUnformatted("Block:", NULL);
        igSameLine(0.0f, 8.0f);
        igSetNextItemWidth(160.0f);
        if (sel >= 0)
            snprintf(text, sizeof(text), "%s  %.3f MHz", dab_blocks[sel].name, dab_blocks[sel].khz / 1e3);
        else
            snprintf(text, sizeof(text), "%.3f MHz", ui->set.freq_hz / 1e6);
        if (igBeginCombo("##dabblock", text, 0)) {
            for (i = 0; i < (int)(sizeof(dab_blocks) / sizeof(dab_blocks[0])); i++) {
                char item[32];

                snprintf(item, sizeof(item), "%s  %.3f MHz", dab_blocks[i].name, dab_blocks[i].khz / 1e3);
                if (igSelectable_Bool(item, i == sel, 0, (ImVec2){ 0.0f, 0.0f }))
                    view_retune(ui, 1e3 * dab_blocks[i].khz, changed);
            }
            igEndCombo();
        }
        if (fabs(ui->set.sample_rate - M2SDR_DAB_SAMPLE_RATE) > 1.0) {
            igSameLine(0.0f, 16.0f);
            if (igSmallButton("2.048 MSPS"))
                view_rate(ui, M2SDR_DAB_SAMPLE_RATE, changed);
        }
        if (ok && res->dab_valid && fabs(t->freq_error_hz) > 500.0) {
            igSameLine(0.0f, 16.0f);
            snprintf(text, sizeof(text), "Centre on the ensemble (%+.1f kHz)", t->freq_error_hz / 1e3);
            if (igSmallButton(text))
                view_retune(ui, ui->set.freq_hz + 1e3 * round(t->freq_error_hz / 1e3), changed);
        }
    }
    if (!res->set.dab_on) {
        igTextUnformatted("Waiting for a capture.", NULL);
        return;
    }
    if (!res->dab_valid) {
        igTextUnformatted("The sample rate must be 2.048 MSPS.", NULL);
        return;
    }
    if (!t->found) {
        igPushTextWrapPos(0.0f);
        igText("No DAB ensemble (null symbol %.1f dB, guard interval correlation %.3f): tune on a block that carries one.",
               t->null_depth_db, t->sync_quality);
        igPopTextWrapPos();
    }
    if (ok)
        snprintf(text, sizeof(text), "SNR on the carriers %.1f dB: the MER is below it by what the transmitter and the receiver add.",
                 t->null_snr_db);
    else
        snprintf(text, sizeof(text), " ");

    igGetContentRegionAvail(&avail);
    side = 0.55f * avail.y < 0.42f * avail.x ? 0.55f * avail.y : 0.42f * avail.x;
    sq   = m2sdr_plot_square(side);
    igBeginGroup();
    draw_trend_mer("##dabmer", &ui->dab, ok, t->mer_db, NAN, text, avail.x - sq.x - 8.0f, sq.y);
    igEndGroup();
    igSameLine(0.0f, 8.0f);
    draw_trend_points("##dabconst", sq, t->points, ok ? t->npoints : 0, M2SDR_MOD_QPSK);
    if (ok)
        draw_trend_traces("##dab", &ui->dab, t->bins, 767.5e3, t->echo_start_us, t->echo_step_us, M2SDR_DAB_ECHO_BINS,
                          t->guard_us, NAN);
}

static void draw_dab_results(struct an_ui *ui)
{
    const struct an_result *res = &ui->res;
    const struct m2sdr_dab_result *t = &res->dab;

    igSeparatorText("DAB");
    if (!res->dab_valid || !t->found) {
        igTextUnformatted("No ensemble.", NULL);
        return;
    }
    if (igBeginTable("##dab", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Mode", "I, guard %.0f us", t->guard_us);
        label_value("Null symbol", "%.1f dB", t->null_depth_db);
        label_value("Guard correlation", "%.2f", t->sync_quality);
        label_value("Power", "%.1f dBFS", t->power_dbfs);
        if (t->prs) {
            label_value("Ensemble", "%.6f MHz", (res->set.freq_hz + t->freq_error_hz) / 1e6);
            label_value("Offset", "%+.1f Hz", t->freq_error_hz);
            label_value("MER", "%.1f dB (last %.1f)", trend_mer_shown(&ui->dab, t->mer_db), t->mer_db);
            label_value("SNR on the carriers", "%.1f dB", t->null_snr_db);
            if (t->echo_level_db > -99.0)
                label_value("Strongest echo", "%+.2f us, %.1f dB", t->echo_delay_us, t->echo_level_db);
            label_value("Frames, symbols", "%d, %d", t->frames, t->symbols);
        } else {
            label_value("Phase reference", "not found (%.2f)", t->prs_quality);
        }
        igEndTable();
    }
}

static void draw_lte_results(struct an_ui *ui)
{
    static const char *const phich[4] = { "1/6", "1/2", "1", "2" };
    const struct an_result *res = &ui->res;
    const struct m2sdr_lte_result *l;
    bool held;

    igSeparatorText("LTE downlink");
    l = lte_shown(ui, &held);
    if (!l) {
        igTextUnformatted("No cell.", NULL);
        return;
    }
    if (held) {
        igPushTextWrapPos(0.0f);
        igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "No cell in the last capture: these are the figures of the last frame read.");
        igPopTextWrapPos();
    }
    if (igBeginTable("##lte", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Cell identity", "%d (%d, %d)", l->cell_id, l->n_id_1, l->n_id_2);
        label_value("Carrier", "%.6f MHz", (res->set.freq_hz + l->freq_error_hz) / 1e6);
        label_value("Carrier offset", "%+.1f Hz", l->freq_error_hz);
        if (l->mib) {
            label_value("Resource blocks", "%d", l->n_rb);
            label_value("Antenna ports", "%d", l->ports);
            label_value("Frame number", "%d", l->sfn);
            label_value("PHICH", "%s, %s", l->phich_duration ? "extended" : "normal", phich[l->phich_resource & 3]);
        } else {
            label_value("MIB", "not decoded");
        }
        if (l->data && l->data_rb < l->n_rb)
            label_value("Analysed", "%d blocks, %d to %d", l->analysed_rb, l->data_first_rb, l->data_first_rb + l->data_rb - 1);
        else
            label_value("Analysed", "%d blocks", l->analysed_rb);
        label_value("RSRP", "%.1f dBFS", l->rsrp_db);
        label_value("RSSI", "%.1f dBFS", l->rssi_db);
        label_value("PSS, SSS", "%.1f, %.1f dBFS", l->pss_db, l->sss_db);
        label_value("RS EVM", "%.2f %%", 100.0 * l->rs_evm);
        if (l->mib)
            label_value("PBCH EVM", "%.2f %%", 100.0 * l->pbch_evm);
        if (l->data) {
            int used[M2SDR_MOD_COUNT] = { 0 }, ns, rb, m;
            char text[96] = "";

            for (ns = 0; ns < M2SDR_LTE_FRAME_SLOTS; ns++) {
                for (rb = 0; rb < M2SDR_LTE_MAX_ANALYSED_RB; rb++)
                    used[l->rb_modulation[ns][rb] % M2SDR_MOD_COUNT]++;
            }
            for (m = 0; m < M2SDR_MOD_COUNT; m++) {
                if (m != M2SDR_MOD_AUTO && used[m])
                    snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%s %d", text[0] ? ", " : "",
                             m2sdr_modulation_name((enum m2sdr_modulation)m), used[m]);
            }
            label_value("Data blocks", "%d", l->data_blocks);
            if (l->data_blocks)
                label_value("", "%s", text);
            label_value("Data, mostly", "%s", l->data_blocks ? m2sdr_modulation_name(l->data_modulation) : "none");
            if (l->data_blocks)
                label_value("Data EVM", "%.2f %%", 100.0 * l->data_evm);
            if (l->ports > 1) {
                label_value("Transmit diversity", "%d blocks", l->data_diversity);
                label_value("One precoded layer", "%d blocks", l->data_precoded);
                label_value("In use, not read", "%d blocks", l->data_unknown);
            }
        }
        igEndTable();
    }
    igPushTextWrapPos(0.0f);
    if (l->rs_evm > 1.0)
        igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "The reference signals are in the noise (EVM over 100 %%): the "
                      "levels and the blocks of data are not to be trusted.");
    if (l->data)
        igTextUnformatted("The control channels are not decoded: a block is taken as the constellation it fits best, "
                          "and its EVM is against the nearest points.", NULL);
    if (l->data && l->ports > 1)
        igTextUnformatted("Two antenna ports: a block is read when it is in transmit diversity or holds one layer. "
                          "One receiver does not separate two layers.", NULL);
    else if (l->mib && !l->data)
        igTextUnformatted("Data are not analysed with four antenna ports.", NULL);
    else if (!l->mib)
        igTextUnformatted("The MIB is not decoded: the width of the carrier and its antenna ports are not known, and the "
                          "data are not analysed.", NULL);
    igPopTextWrapPos();
}

static void draw_results(struct an_ui *ui, struct an_shared *sh, bool *changed)
{
    const struct an_result *res = &ui->res;
    const double off = res->set.ref_offset_db;
    const char *unit = off != 0.0 ? "dBm" : "dBFS";
    const float *trace = marker_trace(ui);
    char text[64];
    int m, i;

    /* The figures of the mode shown, or the ones of the spectrum. */
    if (an_modes[ui->tab]->results) {
        an_modes[ui->tab]->results(ui);
        goto status;
    }

    igSeparatorText("Markers");
    for (m = 0; m < UI_MARKERS; m++) {
        snprintf(text, sizeof(text), "M%d", m + 1);
        if (m)
            igSameLine(0.0f, -1.0f);
        igRadioButton_IntPtr(text, &ui->marker_sel, m);
    }
    if (igButton("Peak", (ImVec2){ 0.0f, 0.0f }) && res->npeaks) {
        ui->markers[ui->marker_sel].on        = true;
        ui->markers[ui->marker_sel].offset_hz = res->peaks[0].freq_hz;
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Next", (ImVec2){ 0.0f, 0.0f }) && res->npeaks) {
        /* The peak after the one the marker is on, by level. */
        int at = -1;
        for (i = 0; i < res->npeaks; i++) {
            if (fabs(res->peaks[i].freq_hz - ui->markers[ui->marker_sel].offset_hz) < 2.0 * res->psd.bin_hz)
                at = i;
        }
        ui->markers[ui->marker_sel].on        = true;
        ui->markers[ui->marker_sel].offset_hz = res->peaks[(at + 1) % res->npeaks].freq_hz;
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Off", (ImVec2){ 0.0f, 0.0f }))
        ui->markers[ui->marker_sel].on = false;
    if (sh->src->live && !sh->src->no_rf_init) {
        igSameLine(0.0f, -1.0f);
        if (igButton("To centre", (ImVec2){ 0.0f, 0.0f }) && ui->markers[ui->marker_sel].on) {
            ui->set.freq_hz += ui->markers[ui->marker_sel].offset_hz;
            ui->markers[ui->marker_sel].offset_hz = 0.0;
            traces_reset(ui);
            *changed = true;
        }
    }
    if (igBeginTable("##markers", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        for (m = 0; m < UI_MARKERS; m++) {
            const double level = trace_at(ui, trace, ui->markers[m].offset_hz);
            char name[16];

            if (!ui->markers[m].on)
                continue;
            snprintf(name, sizeof(name), "M%d", m + 1);
            label_value(name, "%.6f MHz", (res->set.freq_hz + ui->markers[m].offset_hz) / 1e6);
            label_value("", "%.2f %s, %.1f %s/Hz", level, unit, level - 10.0 * log10(res->psd.rbw_hz > 0 ? res->psd.rbw_hz : 1.0), unit);
            if (m > 0 && ui->markers[0].on)
                label_value("", "M1 %+.3f kHz, %+.2f dB", (ui->markers[m].offset_hz - ui->markers[0].offset_hz) / 1e3,
                            level - trace_at(ui, trace, ui->markers[0].offset_hz));
        }
        igEndTable();
    }

    igSeparatorText("Peaks");
    if (igBeginTable("##peaks", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        for (i = 0; i < res->npeaks; i++) {
            snprintf(text, sizeof(text), "%.6f MHz", (res->set.freq_hz + res->peaks[i].freq_hz) / 1e6);
            label_value(text, "%.2f %s", res->peaks[i].power_db + off, unit);
        }
        igEndTable();
    }

    igSeparatorText("Strongest line, as a tone");
    if (res->tone_valid && igBeginTable("##tone", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Frequency", "%.6f MHz", (res->set.freq_hz + res->tone.freq_hz) / 1e6);
        label_value("Level", "%.2f %s", res->tone.level_db + off, unit);
        label_value("SNR", "%.1f dB", res->tone.snr_db);
        label_value("SINAD", "%.1f dB", res->tone.sinad_db);
        label_value("SFDR", "%.1f dBc", res->tone.sfdr_dbc);
        label_value("THD", "%.1f dBc", res->tone.thd_dbc);
        label_value("Harmonics 2, 3", "%.1f, %.1f dBc", res->tone.harmonic_dbc[0], res->tone.harmonic_dbc[1]);
        igEndTable();
    }

    igSeparatorText("Channel");
    if (igBeginTable("##chan", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        if (res->chan_valid) {
            label_value("Power", "%.2f %s", res->chan_power_db + off, unit);
            for (i = 0; res->acpr_valid && i < res->acpr.adjacent; i++) {
                snprintf(text, sizeof(text), "ACPR %d", i + 1);
                label_value(text, "%.1f / %.1f dBc", res->acpr.lower_dbc[i], res->acpr.upper_dbc[i]);
            }
        } else if (res->set.chan_bw_hz > 0) {
            label_value("Power", "out of the span");
        }
        if (res->obw_valid) {
            label_value("OBW (99 %)", "%.3f kHz", (res->obw_hi_hz - res->obw_lo_hz) / 1e3);
            label_value("OBW centre", "%+.3f kHz", 0.5 * (res->obw_lo_hz + res->obw_hi_hz) / 1e3);
        }
        label_value("Noise", "%.1f %s/Hz", res->noise_db_hz + off, unit);
        igEndTable();
    }

    igSeparatorText("Time domain");
    if (igBeginTable("##time", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0.0f, 0.0f }, 0.0f)) {
        label_value("Mean power", "%.2f %s", res->stats.mean_db + off, unit);
        label_value("Peak power", "%.2f %s", res->stats.peak_db + off, unit);
        label_value("PAPR", "%.2f dB", res->stats.papr_db);
        label_value("DC offset", "%.1f %s", res->stats.dc_db + off, unit);
        label_value("I/Q gain", "%+.3f dB", res->stats.gain_imbalance_db);
        label_value("Quadrature", "%+.3f deg", res->stats.quadrature_deg);
        label_value("Clipped", "%.4f %%", 100.0 * res->stats.clipped);
        igEndTable();
    }

status:
    igSeparatorText("Status");
    {
        double capture_ms, analysis_ms;
        char status[256];

        pthread_mutex_lock(&sh->lock);
        capture_ms  = sh->capture_ms;
        analysis_ms = sh->analysis_ms;
        snprintf(status, sizeof(status), "%s", sh->status);
        pthread_mutex_unlock(&sh->lock);
        igText("Capture %.0f ms, analysis %.0f ms", capture_ms, analysis_ms);
        igText("%zu samples, %u segments", capture_samples(&res->set), res->psd.segments);
        if (status[0]) {
            igPushTextWrapPos(0.0f);
            igTextColored((ImVec4){ 1.0f, 0.4f, 0.3f, 1.0f }, "%s", status);
            igPopTextWrapPos();
        }
        if (ui->message[0]) {
            igPushTextWrapPos(0.0f);
            igTextUnformatted(ui->message, NULL);
            igPopTextWrapPos();
        }
    }
    if (igButton("Export CSV", (ImVec2){ 0.0f, 0.0f }) && res->psd.bins) {
        char path[64];
        const time_t now = time(NULL);
        struct tm tm;

        localtime_r(&now, &tm);
        strftime(path, sizeof(path), "m2sdr_analyzer_%Y%m%d_%H%M%S.csv", &tm);
        if (export_csv(path, res) == 0)
            snprintf(ui->message, sizeof(ui->message), "Trace written to %s", path);
        else
            snprintf(ui->message, sizeof(ui->message), "Could not write %s", path);
    }
}

static int run_gui(struct an_source *src, const struct an_settings *set, int first_view, struct an_floor *fl,
                   const char *floor_path)
{
    const char *glsl_version = "#version 130";
    struct an_shared sh;
    struct an_ui ui;
    SDL_Window *window;
    SDL_GLContext gl_context;
    pthread_t worker;
    bool quit = false;
    char title[1100];
    int t, m;

    memset(&sh, 0, sizeof(sh));
    memset(&ui, 0, sizeof(ui));
    pthread_mutex_init(&sh.lock, NULL);
    sh.set = *set;
    sh.src = src;
    sh.run = true;
    ui.set = *set;
    ui.trace_on[TRACE_LIVE] = true;
    ui.avg_target  = 16;
    ui.ref_db      = 0.0 + set->ref_offset_db;
    ui.range_db    = 130.0;
    ui.wf_on       = true;
    ui.wf_auto     = true;
    ui.fl          = fl;
    ui.floor_path  = floor_path;
    ui.tab         = first_view;
    view_full(&ui);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s (use --report without a display)\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    snprintf(title, sizeof(title), "M2SDR Analyzer - %s", src->name);
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 960,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    igCreateContext(NULL);
    igStyleColorsDark(NULL);
    igGetIO()->IniFilename = NULL;
    if (!m2sdr_imgui_sdl2_init_for_opengl(window, gl_context) || !m2sdr_imgui_opengl3_init(glsl_version)) {
        fprintf(stderr, "ImGui backend init failed\n");
        igDestroyContext(NULL);
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    waterfall_init(&ui);
    if (pthread_create(&worker, NULL, worker_main, &sh) != 0) {
        fprintf(stderr, "Could not start the worker thread\n");
        quit = true;
    }

    while (!quit) {
        bool changed = false;
        SDL_Event e;

        while (SDL_PollEvent(&e)) {
            m2sdr_imgui_sdl2_process_event(&e);
            if (e.type == SDL_QUIT)
                quit = true;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window))
                quit = true;
            if (e.type == SDL_KEYDOWN && e.key.repeat == 0 && !igGetIO()->WantTextInput) {
                if (e.key.keysym.sym == SDLK_SPACE) {
                    pthread_mutex_lock(&sh.lock);
                    sh.run = !sh.run;
                    pthread_mutex_unlock(&sh.lock);
                } else if (e.key.keysym.sym == SDLK_p && ui.res.npeaks) {
                    ui.markers[ui.marker_sel].on        = true;
                    ui.markers[ui.marker_sel].offset_hz = ui.res.peaks[0].freq_hz;
                } else if (e.key.keysym.sym == SDLK_f) {
                    view_full(&ui);
                } else if (e.key.keysym.sym == SDLK_r) {
                    traces_reset(&ui);
                }
            }
        }

        /* New result of the worker. */
        pthread_mutex_lock(&sh.lock);
        if (sh.seq != ui.seq && result_copy(&ui.res, &sh.res) == 0) {
            ui.seq = sh.seq;
            pthread_mutex_unlock(&sh.lock);
            if (traces_update(&ui) == 0)
                waterfall_push(&ui);
            for (m = 0; an_modes[m]; m++) {
                if (an_modes[m]->keep)
                    an_modes[m]->keep(&ui);
            }
        } else {
            pthread_mutex_unlock(&sh.lock);
        }

        m2sdr_imgui_opengl3_new_frame();
        m2sdr_imgui_sdl2_new_frame();
        igNewFrame();
        {
            ImGuiIO *io = igGetIO();
            igSetNextWindowPos((ImVec2){ 0, 0 }, ImGuiCond_Always, (ImVec2){ 0, 0 });
            igSetNextWindowSize(io->DisplaySize, ImGuiCond_Always);
        }
        igBegin("##M2SDRAnalyzerMain", NULL,
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
        if (igBeginChild_Str("##controls", (ImVec2){ 280.0f, 0.0f }, 0, 0))
            draw_controls(&ui, &sh, &changed);
        igEndChild();
        igSameLine(0.0f, -1.0f);
        {
            ImVec2 avail;
            igGetContentRegionAvail(&avail);
            if (igBeginChild_Str("##views", (ImVec2){ avail.x - 330.0f, 0.0f }, 0, 0)) {
                if (igBeginTabBar("##tabs", 0)) {
                    /* The view asked on the command line is selected once. */
                    const int select = first_view;

                    first_view = -1;
                    int tab = ui.tab;

                    for (m = 0; an_modes[m]; m++) {
                        if (igBeginTabItem(an_modes[m]->title, NULL, select == m ? ImGuiTabItemFlags_SetSelected : 0)) {
                            tab = m;
                            an_modes[m]->draw(&ui, src->live && !src->no_rf_init, &changed);
                            igEndTabItem();
                        }
                    }
                    /* A mode is measured while its view is shown: some need their own
                     * captures. */
                    for (m = 0; select < 0 && an_modes[m]; m++) {
                        bool *on = an_mode_flag(an_modes[m], &ui.set);

                        if (on && *on != (tab == m)) {
                            *on     = tab == m;
                            changed = true;
                        }
                    }
                    if (select < 0)
                        ui.tab = tab;
                    igEndTabBar();
                }
            }
            igEndChild();
        }
        igSameLine(0.0f, -1.0f);
        if (igBeginChild_Str("##results", (ImVec2){ 0.0f, 0.0f }, 0, 0))
            draw_results(&ui, &sh, &changed);
        igEndChild();
        igEnd();

        if (changed) {
            pthread_mutex_lock(&sh.lock);
            sh.set = ui.set;
            pthread_mutex_unlock(&sh.lock);
        }

        igRender();
        {
            ImGuiIO *io = igGetIO();
            glViewport(0, 0, (int)io->DisplaySize.x, (int)io->DisplaySize.y);
            glClearColor(0.07f, 0.08f, 0.09f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            m2sdr_imgui_opengl3_render_draw_data(igGetDrawData());
        }
        SDL_GL_SwapWindow(window);
    }

    pthread_mutex_lock(&sh.lock);
    sh.quit = true;
    pthread_mutex_unlock(&sh.lock);
    pthread_join(worker, NULL);

    m2sdr_imgui_opengl3_shutdown();
    m2sdr_imgui_sdl2_shutdown();
    igDestroyContext(NULL);
    glDeleteTextures(1, &ui.wf_tex);
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    for (t = 0; t < UI_TRACES; t++)
        free(ui.trace[t]);
    free(ui.avg_lin);
    free(ui.wf_rgba);
    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->close)
            an_modes[m]->close(&ui);
    }
    free(ui.res.psd.power);
    free(sh.res.psd.power);
    pthread_mutex_destroy(&sh.lock);
    return 0;
}

#endif /* M2SDR_ANALYZER_GUI */

/* Modes */
/*-------*/

/* Views of the spectrum: measured with every capture. */
const struct an_mode an_mode_spectrum = {
    .key      = "spectrum",
    .title    = "Spectrum",
    .category = AN_CAT_SPECTRUM,
    .on       = AN_NONE,
    .valid    = AN_NONE,
#ifdef M2SDR_ANALYZER_GUI
    .draw     = draw_spectrum,
#endif
};

const struct an_mode an_mode_time = {
    .key      = "time",
    .title    = "Time",
    .category = AN_CAT_SPECTRUM,
    .on       = AN_NONE,
    .valid    = AN_NONE,
#ifdef M2SDR_ANALYZER_GUI
    .draw     = draw_time,
#endif
};

const struct an_mode an_mode_statistics = {
    .key      = "statistics",
    .title    = "Statistics",
    .category = AN_CAT_SPECTRUM,
    .on       = AN_NONE,
    .valid    = AN_NONE,
#ifdef M2SDR_ANALYZER_GUI
    .draw     = draw_statistics,
#endif
};

const struct an_mode an_mode_demod = {
    .key         = "modulation",
    .title       = "Modulation",
    .category    = AN_CAT_SIGNAL,
    .on          = offsetof(struct an_settings, demod_on),
    .valid       = offsetof(struct an_result, demod_valid),
    .report_rank = 40,
    .samples     = demod_samples,
    .analyse     = demod_analyse,
    .report_text = demod_report_text,
    .report_json = demod_report_json,
#ifdef M2SDR_ANALYZER_GUI
    .draw        = draw_modulation,
    .results     = draw_modulation_results,
#endif
};

const struct an_mode an_mode_lte = {
    .key         = "lte",
    .title       = "LTE",
    .category    = AN_CAT_CELLULAR,
    .on          = offsetof(struct an_settings, lte_on),
    .valid       = offsetof(struct an_result, lte_valid),
    .report_rank = 10,
    .samples     = lte_samples,
    .analyse     = lte_analyse,
    .report_text = lte_report_text,
    .report_json = lte_report_json,
#ifdef M2SDR_ANALYZER_GUI
    .draw        = draw_lte,
    .results     = draw_lte_results,
    .keep        = lte_keep,
    .close       = lte_close,
#endif
};

const struct an_mode an_mode_dvbt = {
    .key            = "dvbt",
    .title          = "DVB-T",
    .category       = AN_CAT_BROADCAST,
    .on             = offsetof(struct an_settings, dvbt_on),
    .valid          = offsetof(struct an_result, dvbt_valid),
    .report_rank    = 20,
    .default_rate   = AN_DVBT_RATE,
    .psd_short      = true,
    .samples        = dvbt_samples,
    .analyse        = dvbt_analyse,
    .report_text    = dvbt_report_text,
    .report_json    = dvbt_report_json,
    .watch_prepare  = dvbt_watch_prepare,
    .watch_line     = dvbt_watch_line,
#ifdef M2SDR_ANALYZER_GUI
    .worker_prepare = dvbt_worker_prepare,
    .worker_result  = dvbt_worker_result,
    .draw           = draw_dvbt,
    .results        = draw_dvbt_results,
    .keep           = dvbt_keep,
    .close          = dvbt_close,
#endif
};

const struct an_mode an_mode_dab = {
    .key          = "dab",
    .title        = "DAB",
    .category     = AN_CAT_BROADCAST,
    .on           = offsetof(struct an_settings, dab_on),
    .valid        = offsetof(struct an_result, dab_valid),
    .report_rank  = 30,
    .default_rate = M2SDR_DAB_SAMPLE_RATE,
    .psd_short    = true,
    .samples      = dab_samples,
    .analyse      = dab_analyse,
    .report_text  = dab_report_text,
    .report_json  = dab_report_json,
    .watch_line   = dab_watch_line,
#ifdef M2SDR_ANALYZER_GUI
    .draw         = draw_dab,
    .results      = draw_dab_results,
    .keep         = dab_keep,
#endif
};

/* Registry: the tabs, in their order. */
const struct an_mode *const an_modes[] = {
    &an_mode_spectrum,
    &an_mode_time,
    &an_mode_statistics,
    &an_mode_pn,
    &an_mode_demod,
    &an_mode_lte,
    &an_mode_dvbt,
    &an_mode_dab,
    NULL
};

/* Help */
/*------*/

#define AN_HELP_COLUMNS 94         /* The longest line of the help. */

/* The names of the views, as "a, b or c". */
static char *view_names(char *text, size_t len)
{
    size_t n = 0;
    int m;

    text[0] = '\0';
    for (m = 0; an_modes[m] && n < len; m++)
        n += (size_t)snprintf(text + n, len - n, "%s%s", m == 0 ? "" : an_modes[m + 1] ? ", " : " or ", an_modes[m]->key);
    return text;
}

/* --view: the names of the views, on lines as long as the other ones of the help. */
static void help_views(void)
{
    char names[512], *word;
    int col;

    view_names(names, sizeof(names) - 1);
    strcat(names, ".");
    col = printf("      --view NAME        View shown first:");
    for (word = strtok(names, " "); word; word = strtok(NULL, " ")) {
        if (col + 1 + (int)strlen(word) > AN_HELP_COLUMNS)
            col = printf("\n%25s%s", "", word) - 1;
        else
            col += printf(" %s", word);
    }
    printf("\n");
}

static void help(void)
{
    printf("M2SDR Signal Analyzer Utility.\n"
           "usage: m2sdr_analyzer [options]                  Board RX\n"
           "       m2sdr_analyzer [options] --file PATH      SigMF or raw capture\n"
           "\n"
           "Spectrum with markers, channel power, occupied bandwidth and ACPR, the figures of a\n"
           "tone, time-domain statistics and phase noise. It only receives, unless --tx-tone is given.\n"
           "\n");
    m2sdr_cli_print_device_help();
    printf("\n"
           "Board options:\n"
           "      --freq HZ          Centre frequency (default: %.0f).\n"
           "      --sample-rate HZ   Sample rate (default: %.0f).\n"
           "      --bandwidth HZ     RF bandwidth (default: 0.9 x the sample rate).\n"
           "      --rx-gain DB       Manual RX gain (default: slow AGC).\n"
           "      --channel N        RX channel, 1 or 2 (default: 1).\n"
           "      --refclk-ppm V     Reference trim in ppm, 'stored' or 'none' (default: the\n"
           "                         calibration record of the board when it has one, see m2sdr_cal).\n"
           "      --sync MODE        Clock source: internal, external or fpga (default: internal).\n"
           "      --refclk-freq HZ   Reference frequency in Hz (default: 38400000).\n"
           "\n"
           "File options:\n"
           "      --file PATH        SigMF metadata or data file, or raw interleaved samples.\n"
           "      --format FMT       Raw file: sc16 or sc8 (default: sc16).\n"
           "      --nbits N          Raw file: significant bits of a sample (default: 12, 8 for sc8).\n"
           "      --nchannels N      Raw file: channels interleaved (default: 2).\n"
           "      --frame-header     Raw file: a 16-byte header starts every frame.\n"
           "      --frame-size N     Raw file: bytes of a frame with its header (default: %u).\n"
           "      --capture-index N  SigMF: capture to open (default: 0).\n"
           "      --offset S         Start, in seconds from the start of the file (default: 0).\n"
           "      --freq HZ, --sample-rate HZ   When the file does not give them.\n"
           "\n"
           "Measurement options:\n"
           "      --fft-len N        FFT length, a power of two (default: %d).\n"
           "      --window NAME      hann, flattop, blackman-harris or rect (default: hann).\n"
           "      --averages N       Segments averaged in a capture (default: %d).\n"
           "      --channel-bw HZ    Channel measured: power, ACPR, occupied bandwidth.\n"
           "      --channel-offset HZ   Its centre, from the centre of the capture (default: 0).\n"
           "      --channel-spacing HZ  Spacing of the adjacent channels (default: the bandwidth).\n"
           "      --ref-offset DB    Added to the levels, to read dBm at the connector: the board\n"
           "                         has no absolute level calibration.\n"
           "\n"
           "Modulation options:\n"
           "      --demod            With --report: demodulate the strongest signal, a single carrier\n"
           "                         with PSK or QAM: constellation, EVM, MER, I/Q errors.\n"
           "      --modulation NAME  auto, bpsk, qpsk, 8psk, 16qam, 32qam, 64qam, 128qam or 256qam\n"
           "                         (default: auto).\n"
           "      --symbol-rate HZ   Symbols per second (default: found from the signal).\n"
           "      --rolloff A        Roll-off of the pulse (default: found from the signal).\n"
           "      --pulse NAME       Transmit pulse: rrc, rc or rect (default: rrc).\n"
           "      --demod-samples N  Samples of its capture (default: %d).\n"
           "      --no-rf-init       Board: leave the RFIC as it is, set by m2sdr_rf, and only receive;\n"
           "                         --freq and --sample-rate then only label the results.\n"
           "\n"
           "LTE options:\n"
           "      --lte              With --report: analyse an LTE downlink carrier, FDD with a normal\n"
           "                         cyclic prefix, tuned on its centre: cell, MIB, EVM, data blocks.\n"
           "                         The sample rate is 1.92, 3.84, 7.68, 15.36 or 30.72 MSPS, for\n"
           "                         6, 15, 25, 50 or 100 resource blocks around the centre.\n"
           "\n"
           "DVB-T options:\n"
           "      --dvbt             With --report: reception of a DVB-T multiplex tuned near its centre:\n"
           "                         mode, TPS, MER, channel and echoes. The sample rate is the one of\n"
           "                         the channel (default with --dvbt: 9.142857 MSPS, 8 MHz).\n"
           "      --watch            With --report --dvbt or --dab, on a board: one line per capture until\n"
           "                         Ctrl-C (MER, margin, level, offset, echo); with --json, one JSON\n"
           "                         object per line.\n"
           "\n"
           "DAB options:\n"
           "      --dab              With --report: reception of a DAB/DAB+ ensemble (mode I) tuned near\n"
           "                         its centre: MER, SNR, carrier offset, channel and echoes. The sample\n"
           "                         rate is 2.048 MSPS (the default with --dab). --watch works too.\n"
           "\n"
           "Phase noise options:\n"
           "      --phase-noise      With --report: phase noise of the strongest carrier.\n"
           "      --pn-samples N     Samples of its capture (default: %d): more reach lower offsets.\n"
           "      --pn-from HZ, --pn-to HZ   Integration limits (default: the offsets measured).\n"
           "      --pn-floor PATH    File of the floor of the board (default: one per board,\n"
           "                         ~/.config/litex_m2sdr/pn_floor_<dna>.csv).\n"
           "      --pn-floor-save    With --report: store the phase noise read as this floor.\n"
           "      --tx-tone          Board: send the test tone of the RFIC on TX, at the centre plus a\n"
           "                         thirty-second of the sample rate. For a cable from TX to RX.\n"
           "      --tx-att DB        Attenuation of the test tone (default: %d).\n"
           "\n"
           "Output options:\n",
           AN_DEFAULT_FREQ_HZ, AN_DEFAULT_SAMPLE_RATE, M2SDR_BUFFER_BYTES, AN_DEFAULT_FFT_LEN, AN_DEFAULT_AVERAGES,
           1 << AN_DEMOD_DEFAULT_LOG2, 1 << AN_PN_DEFAULT_LOG2, AN_TX_TONE_ATT_DB);
    help_views();
    printf("      --report           Measure one capture and print the result, without a display.\n"
           "      --json             With --report: JSON.\n"
           "      --export-csv PATH  With --report: write the spectrum to a CSV file.\n"
           "  -h, --help             Show this help message.\n"
           "\n"
           "Window: left click places the selected marker, the wheel zooms, a right drag moves the\n"
           "span, a double click shows it all. Space: run or stop. P: marker to the peak. F: full\n"
           "span. R: restart the traces.\n");
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    static struct option options[] = {
        { "help",            no_argument,       NULL, 'h' },
        { "device",          required_argument, NULL, 'd' },
        { "freq",            required_argument, NULL, 1 },
        { "sample-rate",     required_argument, NULL, 2 },
        { "bandwidth",       required_argument, NULL, 3 },
        { "rx-gain",         required_argument, NULL, 4 },
        { "channel",         required_argument, NULL, 5 },
        { "refclk-ppm",      required_argument, NULL, 6 },
        { "sync",            required_argument, NULL, 7 },
        { "refclk-freq",     required_argument, NULL, 8 },
        { "file",            required_argument, NULL, 9 },
        { "format",          required_argument, NULL, 10 },
        { "nbits",           required_argument, NULL, 11 },
        { "nchannels",       required_argument, NULL, 12 },
        { "frame-header",    no_argument,       NULL, 13 },
        { "frame-size",      required_argument, NULL, 14 },
        { "capture-index",   required_argument, NULL, 15 },
        { "offset",          required_argument, NULL, 16 },
        { "fft-len",         required_argument, NULL, 17 },
        { "window",          required_argument, NULL, 18 },
        { "averages",        required_argument, NULL, 19 },
        { "channel-bw",      required_argument, NULL, 20 },
        { "channel-offset",  required_argument, NULL, 21 },
        { "channel-spacing", required_argument, NULL, 22 },
        { "ref-offset",      required_argument, NULL, 23 },
        { "report",          no_argument,       NULL, 24 },
        { "json",            no_argument,       NULL, 25 },
        { "export-csv",      required_argument, NULL, 26 },
        { "view",            required_argument, NULL, 27 },
        { "phase-noise",     no_argument,       NULL, 28 },
        { "pn-samples",      required_argument, NULL, 29 },
        { "pn-from",         required_argument, NULL, 30 },
        { "pn-to",           required_argument, NULL, 31 },
        { "pn-floor",        required_argument, NULL, 32 },
        { "pn-floor-save",   no_argument,       NULL, 33 },
        { "tx-tone",         no_argument,       NULL, 34 },
        { "tx-att",          required_argument, NULL, 35 },
        { "demod",           no_argument,       NULL, 36 },
        { "modulation",      required_argument, NULL, 37 },
        { "symbol-rate",     required_argument, NULL, 38 },
        { "rolloff",         required_argument, NULL, 39 },
        { "pulse",           required_argument, NULL, 40 },
        { "demod-samples",   required_argument, NULL, 41 },
        { "no-rf-init",      no_argument,       NULL, 42 },
        { "lte",             no_argument,       NULL, 43 },
        { "dvbt",            no_argument,       NULL, 44 },
        { "watch",           no_argument,       NULL, 45 },
        { "dab",             no_argument,       NULL, 46 },
        { NULL, 0, NULL, 0 }
    };
    struct m2sdr_cli_device cli_dev;
    struct an_settings set;
    struct an_source src;
    const char *file = NULL, *csv_path = NULL, *trim = NULL, *floor_arg = NULL, *floor_path = NULL;
    static struct an_floor fl;
    static char floor_default[1024];
    bool floor_store = false;
    bool report = false, json = false, watch = false, freq_given = false, rate_given = false, nbits_given = false;
    double offset_s = 0.0;
    int capture_index = 0, view = 0;
    char err[256];
    int c, m, status;

    m2sdr_cli_handle_version(argc, argv);
    m2sdr_cli_device_init(&cli_dev);
    memset(&set, 0, sizeof(set));
    memset(&src, 0, sizeof(src));
    set.freq_hz      = AN_DEFAULT_FREQ_HZ;
    set.sample_rate  = AN_DEFAULT_SAMPLE_RATE;
    set.agc          = true;
    set.rx_gain      = 40;
    set.fft_len      = AN_DEFAULT_FFT_LEN;
    set.window       = M2SDR_WINDOW_HANN;
    set.averages     = AN_DEFAULT_AVERAGES;
    src.sample_bytes = 2;
    src.nbits        = 12;
    src.nchannels    = 2;
    src.frame_size   = M2SDR_BUFFER_BYTES;
    src.loop         = true;
    src.tx_att       = AN_TX_TONE_ATT_DB;
    set.pn_log2      = AN_PN_DEFAULT_LOG2;
    set.demod_log2   = AN_DEMOD_DEFAULT_LOG2;

    while ((c = getopt_long(argc, argv, "hd:c:i:p:", options, NULL)) != -1) {
        double v;
        int n;

        switch (c) {
        case 'h':
            help();
            return 0;
        case 'd': case 'c': case 'i': case 'p':
            if (m2sdr_cli_handle_device_option(&cli_dev, c, optarg) != 0)
                return 1;
            break;
        case 1:
            if (m2sdr_cli_parse_double(optarg, &set.freq_hz) != 0 || set.freq_hz < 0) {
                m2sdr_cli_error("invalid frequency '%s'", optarg);
                return 1;
            }
            freq_given = true;
            break;
        case 2:
            if (m2sdr_cli_parse_double(optarg, &set.sample_rate) != 0 || !(set.sample_rate > 0)) {
                m2sdr_cli_error("invalid sample rate '%s'", optarg);
                return 1;
            }
            rate_given = true;
            break;
        case 3:
            if (m2sdr_cli_parse_double(optarg, &set.bandwidth) != 0 || set.bandwidth < 0) {
                m2sdr_cli_error("invalid bandwidth '%s'", optarg);
                return 1;
            }
            break;
        case 4:
            if (m2sdr_cli_parse_int_range(optarg, -10, 77, &set.rx_gain) != 0) {
                m2sdr_cli_error("invalid RX gain '%s'", optarg);
                return 1;
            }
            set.agc = false;
            break;
        case 5:
            if (m2sdr_cli_parse_int_range(optarg, 1, 2, &n) != 0) {
                m2sdr_cli_error("invalid channel '%s' (expected 1 or 2)", optarg);
                return 1;
            }
            set.channel = n - 1;
            break;
        case 6:  trim = optarg; break;
        case 7:  src.sync = optarg; break;
        case 8:
            if (m2sdr_cli_parse_double(optarg, &v) != 0 || !(v > 0)) {
                m2sdr_cli_error("invalid refclk frequency '%s'", optarg);
                return 1;
            }
            src.refclk_freq = (int64_t)v;
            break;
        case 9:  file = optarg; break;
        case 10:
            if (strcmp(optarg, "sc16") == 0) {
                src.sample_bytes = 2;
            } else if (strcmp(optarg, "sc8") == 0) {
                src.sample_bytes = 1;
                if (!nbits_given)
                    src.nbits = 8;
            } else {
                m2sdr_cli_invalid_choice("format", optarg, "sc16 or sc8");
                return 1;
            }
            break;
        case 11:
            if (m2sdr_cli_parse_int_range(optarg, 2, 16, &src.nbits) != 0) {
                m2sdr_cli_error("invalid nbits '%s' (expected 2 to 16)", optarg);
                return 1;
            }
            nbits_given = true;
            break;
        case 12:
            if (m2sdr_cli_parse_int_range(optarg, 1, 8, &src.nchannels) != 0) {
                m2sdr_cli_error("invalid nchannels '%s' (expected 1 to 8)", optarg);
                return 1;
            }
            break;
        case 13: src.frame_header = true; break;
        case 14:
            if (m2sdr_cli_parse_int_range(optarg, 32, 1 << 26, &n) != 0) {
                m2sdr_cli_error("invalid frame size '%s'", optarg);
                return 1;
            }
            src.frame_size = (size_t)n;
            break;
        case 15:
            if (m2sdr_cli_parse_int_range(optarg, 0, 1 << 20, &capture_index) != 0) {
                m2sdr_cli_error("invalid capture index '%s'", optarg);
                return 1;
            }
            break;
        case 16:
            if (m2sdr_cli_parse_double(optarg, &offset_s) != 0 || offset_s < 0) {
                m2sdr_cli_error("invalid offset '%s'", optarg);
                return 1;
            }
            break;
        case 17:
            if (m2sdr_cli_parse_int_range(optarg, AN_MIN_FFT_LEN, AN_MAX_FFT_LEN, &set.fft_len) != 0 ||
                (set.fft_len & (set.fft_len - 1))) {
                m2sdr_cli_error("invalid FFT length '%s' (expected a power of two from %d to %d)", optarg,
                                AN_MIN_FFT_LEN, AN_MAX_FFT_LEN);
                return 1;
            }
            break;
        case 18:
            if (strcmp(optarg, "hann") == 0) {
                set.window = M2SDR_WINDOW_HANN;
            } else if (strcmp(optarg, "flattop") == 0) {
                set.window = M2SDR_WINDOW_FLATTOP;
            } else if (strcmp(optarg, "blackman-harris") == 0) {
                set.window = M2SDR_WINDOW_BLACKMAN_HARRIS;
            } else if (strcmp(optarg, "rect") == 0) {
                set.window = M2SDR_WINDOW_RECTANGULAR;
            } else {
                m2sdr_cli_invalid_choice("window", optarg, "hann, flattop, blackman-harris or rect");
                return 1;
            }
            break;
        case 19:
            if (m2sdr_cli_parse_int_range(optarg, 1, AN_MAX_AVERAGES, &set.averages) != 0) {
                m2sdr_cli_error("invalid averages '%s' (expected 1 to %d)", optarg, AN_MAX_AVERAGES);
                return 1;
            }
            break;
        case 20:
            if (m2sdr_cli_parse_double(optarg, &set.chan_bw_hz) != 0 || set.chan_bw_hz < 0) {
                m2sdr_cli_error("invalid channel bandwidth '%s'", optarg);
                return 1;
            }
            break;
        case 21:
            if (m2sdr_cli_parse_double(optarg, &set.chan_offset_hz) != 0) {
                m2sdr_cli_error("invalid channel offset '%s'", optarg);
                return 1;
            }
            break;
        case 22:
            if (m2sdr_cli_parse_double(optarg, &set.chan_spacing_hz) != 0 || set.chan_spacing_hz < 0) {
                m2sdr_cli_error("invalid channel spacing '%s'", optarg);
                return 1;
            }
            break;
        case 23:
            if (m2sdr_cli_parse_double(optarg, &set.ref_offset_db) != 0) {
                m2sdr_cli_error("invalid reference offset '%s'", optarg);
                return 1;
            }
            break;
        case 24: report = true; break;
        case 25: json = true; break;
        case 26: csv_path = optarg; break;
        case 27:
            for (view = 0; an_modes[view] && strcmp(optarg, an_modes[view]->key) != 0; view++)
                ;
            if (!an_modes[view]) {
                char names[512];

                m2sdr_cli_invalid_choice("view", optarg, view_names(names, sizeof(names)));
                return 1;
            }
            break;
        case 28: set.pn_on = true; break;
        case 29:
            if (m2sdr_cli_parse_double(optarg, &v) != 0 || v < (double)(1 << AN_PN_MIN_LOG2) || v > (double)(1 << AN_PN_MAX_LOG2)) {
                m2sdr_cli_error("invalid phase noise samples '%s' (expected %d to %d)", optarg,
                                1 << AN_PN_MIN_LOG2, 1 << AN_PN_MAX_LOG2);
                return 1;
            }
            /* The power of two at or below. */
            for (set.pn_log2 = AN_PN_MIN_LOG2; (double)((size_t)2 << set.pn_log2) <= v; set.pn_log2++)
                ;
            break;
        case 30:
            if (m2sdr_cli_parse_double(optarg, &set.pn_lo_hz) != 0 || set.pn_lo_hz < 0) {
                m2sdr_cli_error("invalid offset '%s'", optarg);
                return 1;
            }
            break;
        case 31:
            if (m2sdr_cli_parse_double(optarg, &set.pn_hi_hz) != 0 || set.pn_hi_hz < 0) {
                m2sdr_cli_error("invalid offset '%s'", optarg);
                return 1;
            }
            break;
        case 32: floor_arg = optarg; break;
        case 33: floor_store = true; break;
        case 36: set.demod_on = true; break;
        case 37:
            for (n = M2SDR_MOD_AUTO; n < M2SDR_MOD_COUNT; n++) {
                if (strcasecmp(optarg, m2sdr_modulation_name((enum m2sdr_modulation)n)) == 0)
                    break;
            }
            if (n == M2SDR_MOD_COUNT) {
                m2sdr_cli_invalid_choice("modulation", optarg, "auto, bpsk, qpsk, 8psk, 16qam, 32qam, 64qam, 128qam or 256qam");
                return 1;
            }
            set.demod.modulation = (enum m2sdr_modulation)n;
            break;
        case 38:
            if (m2sdr_cli_parse_double(optarg, &set.demod.symbol_rate) != 0 || set.demod.symbol_rate < 0) {
                m2sdr_cli_error("invalid symbol rate '%s'", optarg);
                return 1;
            }
            break;
        case 39:
            if (m2sdr_cli_parse_double(optarg, &set.demod.rolloff) != 0 ||
                (set.demod.rolloff != 0 && (set.demod.rolloff < 0.05 || set.demod.rolloff > 1.0))) {
                m2sdr_cli_error("invalid roll-off '%s' (expected 0.05 to 1, or 0 to find it)", optarg);
                return 1;
            }
            break;
        case 40:
            if (strcmp(optarg, "rrc") == 0) {
                set.demod.pulse = M2SDR_PULSE_RRC;
            } else if (strcmp(optarg, "rc") == 0) {
                set.demod.pulse = M2SDR_PULSE_RC;
            } else if (strcmp(optarg, "rect") == 0) {
                set.demod.pulse = M2SDR_PULSE_RECT;
            } else {
                m2sdr_cli_invalid_choice("pulse", optarg, "rrc, rc or rect");
                return 1;
            }
            break;
        case 41:
            if (m2sdr_cli_parse_double(optarg, &v) != 0 || v < (double)(1 << AN_DEMOD_MIN_LOG2) || v > (double)(1 << AN_DEMOD_MAX_LOG2)) {
                m2sdr_cli_error("invalid demodulation samples '%s' (expected %d to %d)", optarg,
                                1 << AN_DEMOD_MIN_LOG2, 1 << AN_DEMOD_MAX_LOG2);
                return 1;
            }
            for (set.demod_log2 = AN_DEMOD_MIN_LOG2; (double)((size_t)2 << set.demod_log2) <= v; set.demod_log2++)
                ;
            break;
        case 42: src.no_rf_init = true; break;
        case 43: set.lte_on = true; break;
        case 44: set.dvbt_on = true; break;
        case 45: watch = true; break;
        case 46: set.dab_on = true; break;
        case 34: src.tx_tone = true; break;
        case 35:
            if (m2sdr_cli_parse_int_range(optarg, 0, 89, &src.tx_att) != 0) {
                m2sdr_cli_error("invalid TX attenuation '%s' (expected 0 to 89)", optarg);
                return 1;
            }
            break;
        default:
            return 1;
        }
    }
    if (optind < argc) {
        m2sdr_cli_error("unexpected argument '%s'", argv[optind]);
        return 1;
    }
    if (watch && (!report || watch_modes(&set, NULL) != 1 || file || csv_path || floor_store)) {
        m2sdr_cli_error("--watch goes with --report and one of --dvbt and --dab, on a board");
        return 1;
    }
    if ((json || csv_path || floor_store) && !report) {
        m2sdr_cli_error("--json, --export-csv and --pn-floor-save go with --report");
        return 1;
    }
    if (floor_store)
        set.pn_on = true;
    /* The mode of the view shown first is measured from the first capture. */
    if (an_mode_flag(an_modes[view], &set))
        *an_mode_flag(an_modes[view], &set) = true;
    /* The sample rate of a mode, unless one is given; a file gives its own. */
    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->default_rate > 0 && an_mode_on(an_modes[m], &set) && !rate_given)
            set.sample_rate = an_modes[m]->default_rate;
    }
    if (src.no_rf_init && (file || src.tx_tone)) {
        m2sdr_cli_error("--no-rf-init is for a board, without --tx-tone");
        return 1;
    }
    if (src.tx_tone && file) {
        m2sdr_cli_error("--tx-tone is for a board");
        return 1;
    }

    if (file) {
        if (src.frame_header && src.frame_size <= AN_FRAME_HEADER_BYTES) {
            m2sdr_cli_error("the frame size must be above %d bytes", AN_FRAME_HEADER_BYTES);
            return 1;
        }
        if (file_open(&src, file, capture_index, err, sizeof(err)) != 0) {
            fprintf(stderr, "Could not open the capture: %s\n", err);
            source_close(&src);
            return 1;
        }
        if (!rate_given && src.file_rate > 0)
            set.sample_rate = src.file_rate;
        if (!freq_given)
            set.freq_hz = src.file_freq;
        if (!rate_given && !(src.file_rate > 0))
            fprintf(stderr, "The file does not give its sample rate: %.0f assumed (--sample-rate).\n", set.sample_rate);
        if (set.channel >= src.nchannels) {
            fprintf(stderr, "The file has %d channel%s.\n", src.nchannels, src.nchannels == 1 ? "" : "s");
            source_close(&src);
            return 1;
        }
        src.position = (uint64_t)(offset_s * set.sample_rate);
        if (src.position >= src.total_sets)
            src.position = 0;
        if (report)
            src.loop = false;
    } else {
        struct m2sdr_cal_record rec;

        if (!m2sdr_cli_finalize_device(&cli_dev))
            return 1;
        if (m2sdr_open(&src.dev, m2sdr_cli_device_id(&cli_dev)) != 0) {
            fprintf(stderr, "Could not open device: %s\n", m2sdr_cli_device_id(&cli_dev));
            return 1;
        }
        src.live = true;
        if (json)
            (void)m2sdr_set_log_enabled(false);     /* The JSON is alone on the output. */
        snprintf(src.name, sizeof(src.name), "%s", m2sdr_cli_device_id(&cli_dev));

        /* Reference trim: the calibration record of the board unless told otherwise. */
        if (trim && strcmp(trim, "none") == 0) {
            src.trim_ppm = 0.0;
        } else if (trim && strcmp(trim, "stored") != 0) {
            if (m2sdr_cli_parse_double(trim, &src.trim_ppm) != 0 || fabs(src.trim_ppm) > M2SDR_REFCLK_PPM_MAX) {
                m2sdr_cli_error("invalid refclk ppm '%s' (expected 'stored', 'none' or -%g to %g)", trim,
                                (double)M2SDR_REFCLK_PPM_MAX, (double)M2SDR_REFCLK_PPM_MAX);
                source_close(&src);
                return 1;
            }
        } else if (m2sdr_cal_load(src.dev, M2SDR_CAL_FLASH_OFFSET, &rec) != M2SDR_CAL_SOURCE_NONE) {
            src.trim_ppm = rec.refclk_ppm;
            fprintf(stderr, "RefClk trim %+.3f ppm, from the calibration record of the board.\n", rec.refclk_ppm);
        } else if (trim) {
            fprintf(stderr, "No calibration record for this board: run m2sdr_cal measure --store.\n");
            source_close(&src);
            return 1;
        }
    }

    /* Floor of the phase noise: the file given, or the one of the board. */
    if (floor_arg) {
        floor_path = floor_arg;
    } else if (src.live && m2sdr_get_fpga_dna(src.dev, &src.dna) == M2SDR_ERR_OK &&
               floor_default_path(src.dna, floor_default, sizeof(floor_default)) == 0) {
        floor_path = floor_default;
    }
    if (floor_path && floor_load(&fl, floor_path) != 0 && floor_arg && !floor_store)
        fprintf(stderr, "No phase noise floor in %s.\n", floor_path);
    if (floor_store && !floor_path) {
        m2sdr_cli_error("--pn-floor-save needs a board or --pn-floor PATH");
        source_close(&src);
        return 1;
    }

    (void)view;
    if (watch) {
        status = run_watch(&src, &set, json);
    } else if (report) {
        status = run_report(&src, &set, json, csv_path, &fl, floor_store ? floor_path : NULL);
    } else {
#ifdef M2SDR_ANALYZER_GUI
        status = run_gui(&src, &set, view, &fl, floor_path);
#else
        fprintf(stderr, "This build has no window (SDL2 or cimgui was missing): use --report.\n");
        status = 1;
#endif
    }
    source_close(&src);
    return status;
}
