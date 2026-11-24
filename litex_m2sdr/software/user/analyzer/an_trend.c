/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: what the OFDM views share (DVB-T, DAB): the MER and the traces
 * averaged over the results of one tuning, followed when moving an antenna.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "analyzer.h"

#define AN_DVBT_MER_MEAN   10        /* Captures in the MER shown in figures: about 0.5 s. */
#define AN_DVBT_TRACE_AVG  0.125f    /* Weight of a capture in the averaged traces: about 8 captures. */
#define DVBT_COL_MER       0xff40c0ffu
#define DVBT_COL_CHANNEL   0xffffb040u
#define DVBT_COL_NEEDED    0xa04040ffu

/* Trend */
/*-------*/

/* A result of the tuning of `set`; true when the tuning is a new one and the trend restarted. A
 * result without the signal (ok false) counts as 0 dB in the trace. */
bool trend_add(struct an_trend *tr, const struct an_settings *set, bool ok, double mer, const float *mer_bin,
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
double trend_mer_shown(const struct an_trend *tr, double last)
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
void draw_trend_mer(const char *id, const struct an_trend *tr, bool ok, double last, double needed, const char *line,
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
void draw_trend_points(const char *id, ImVec2 size, const float complex *points, int n, enum m2sdr_modulation mod)
{
    static float px[4096], py[4096];
    static float complex ref[256];
    struct m2sdr_plot plot;
    const int nref = m2sdr_constellation(mod, ref);
    char label[96];
    int i;

    snprintf(label, sizeof(label), "Q against I, equalised symbols; crosses: %s", m2sdr_modulation_name(mod));
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
void draw_trend_traces(const char *id, const struct an_trend *tr, int bins, double half_hz, double echo_start_us,
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
