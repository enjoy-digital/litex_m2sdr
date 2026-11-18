/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR plot helper: axes, grid, traces and markers on the Dear ImGui draw list.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "m2sdr_plot.h"

#define PLOT_MARGIN_LEFT   58.0f
#define PLOT_MARGIN_BOTTOM 22.0f
#define PLOT_MARGIN_TOP     6.0f
#define PLOT_MARGIN_RIGHT  10.0f

#define COL_FRAME  0xff5a5a5au
#define COL_GRID   0x40ffffffu
#define COL_LABEL  0xffc8c8c8u
#define COL_BACK   0xff141210u
#define COL_PLATE  0xc0141210u   /* Under a text drawn over the traces. */

/* Helpers */
/*---------*/

/* Step of 1, 2 or 5 times a power of ten that gives about `target` divisions of a range. */
static double nice_step(double range, int target)
{
    const double raw = range / (target > 0 ? target : 1);
    double mag, norm;

    if (!(raw > 0))
        return 1.0;
    mag  = pow(10.0, floor(log10(raw)));
    norm = raw / mag;
    if (norm < 1.5)
        return mag;
    if (norm < 3.5)
        return 2.0 * mag;
    if (norm < 7.5)
        return 5.0 * mag;
    return 10.0 * mag;
}

/* Decimals that show a step: none for 2, one for 0.5, two for 0.01. */
static int decimals_for(double step_scaled)
{
    int d;

    if (!(step_scaled > 0))
        return 0;
    d = (int)ceil(-log10(step_scaled) - 1e-9);
    if (d < 0)
        d = 0;
    return d > 9 ? 9 : d;
}

void m2sdr_plot_format(char *buf, size_t len, double value, double step, enum m2sdr_plot_unit unit)
{
    static const struct { double scale; const char *freq; const char *time; } units[] = {
        { 1e9,  " GHz", NULL  },
        { 1e6,  " MHz", NULL  },
        { 1e3,  " kHz", NULL  },
        { 1.0,  " Hz",  " s"  },
        { 1e-3, NULL,   " ms" },
        { 1e-6, NULL,   " us" },
        { 1e-9, NULL,   " ns" },
    };
    const double mag = fabs(value);
    const char *suffix = "";
    double scale = 1.0;
    unsigned i;

    if (unit != M2SDR_PLOT_PLAIN) {
        const double ref = mag > 0 ? mag : fabs(step);
        const char *last = "";
        for (i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
            const char *name = unit == M2SDR_PLOT_FREQ ? units[i].freq : units[i].time;
            if (!name)
                continue;
            scale = units[i].scale;
            last  = name;
            if (ref >= units[i].scale)
                break;
        }
        suffix = last;
    }
    snprintf(buf, len, "%.*f%s", decimals_for(fabs(step) / scale), value / scale, suffix);
}

/* Text over the plot, on a plate that keeps it readable over the traces. */
static void plot_text(struct m2sdr_plot *plot, ImVec2 pos, const char *text, ImU32 color)
{
    ImVec2 ts;

    igCalcTextSize(&ts, text, NULL, false, -1.0f);
    ImDrawList_AddRectFilled(plot->dl, (ImVec2){ pos.x - 3.0f, pos.y - 1.0f }, (ImVec2){ pos.x + ts.x + 3.0f, pos.y + ts.y + 1.0f },
                             COL_PLATE, 0.0f, 0);
    ImDrawList_AddText_Vec2(plot->dl, pos, color, text, NULL);
}

float m2sdr_plot_x(const struct m2sdr_plot *plot, double x)
{
    return plot->p0.x + (float)((x - plot->x0) / (plot->x1 - plot->x0)) * (plot->p1.x - plot->p0.x);
}

float m2sdr_plot_y(const struct m2sdr_plot *plot, double y)
{
    return plot->p1.y - (float)((y - plot->y0) / (plot->y1 - plot->y0)) * (plot->p1.y - plot->p0.y);
}

/* Plot */
/*------*/

ImVec2 m2sdr_plot_square(float height)
{
    return (ImVec2){ height - PLOT_MARGIN_TOP - PLOT_MARGIN_BOTTOM + PLOT_MARGIN_LEFT + PLOT_MARGIN_RIGHT, height };
}

bool m2sdr_plot_begin(struct m2sdr_plot *plot, const char *id, ImVec2 size,
                      double x0, double x1, enum m2sdr_plot_unit x_unit,
                      double y0, double y1, const char *y_label)
{
    ImVec2 pos, avail, mouse, ts;
    double step, v;
    char text[48];
    float edge, tx;
    bool same;

    memset(plot, 0, sizeof(*plot));
    igGetCursorScreenPos(&pos);
    igGetContentRegionAvail(&avail);
    if (size.x <= 0)
        size.x = avail.x;
    if (size.y <= 0)
        size.y = avail.y;
    if (size.x < PLOT_MARGIN_LEFT + PLOT_MARGIN_RIGHT + 40 || size.y < PLOT_MARGIN_TOP + PLOT_MARGIN_BOTTOM + 30)
        return false;
    if (!(x1 > x0))
        x1 = x0 + 1.0;
    if (!(y1 > y0))
        y1 = y0 + 1.0;

    plot->dl = igGetWindowDrawList();
    plot->p0 = (ImVec2){ pos.x + PLOT_MARGIN_LEFT, pos.y + PLOT_MARGIN_TOP };
    plot->p1 = (ImVec2){ pos.x + size.x - PLOT_MARGIN_RIGHT, pos.y + size.y - PLOT_MARGIN_BOTTOM };
    plot->x0 = x0;
    plot->x1 = x1;
    plot->y0 = y0;
    plot->y1 = y1;
    plot->label = y_label;

    igInvisibleButton(id, size, 0);
    igGetMousePos(&mouse);
    plot->hovered = igIsItemHovered(0) &&
                    mouse.x >= plot->p0.x && mouse.x <= plot->p1.x &&
                    mouse.y >= plot->p0.y && mouse.y <= plot->p1.y;
    plot->clicked = plot->hovered && igIsMouseClicked_Bool(0, false);
    plot->mouse_x = x0 + (x1 - x0) * (mouse.x - plot->p0.x) / (plot->p1.x - plot->p0.x);
    plot->mouse_y = y0 + (y1 - y0) * (plot->p1.y - mouse.y) / (plot->p1.y - plot->p0.y);

    ImDrawList_AddRectFilled(plot->dl, plot->p0, plot->p1, COL_BACK, 0.0f, 0);

    /* Vertical axis. The two axes of a plane (the same range on both) share their step. */
    same = x_unit == M2SDR_PLOT_PLAIN && x0 == y0 && x1 == y1;
    step = nice_step(y1 - y0, (int)((plot->p1.y - plot->p0.y) / (same ? 70.0f : 45.0f)));
    for (v = ceil(y0 / step - 1e-9) * step; v <= y1 + 1e-9 * step; v += step) {
        const float y = m2sdr_plot_y(plot, v);
        ImDrawList_AddLine(plot->dl, (ImVec2){ plot->p0.x, y }, (ImVec2){ plot->p1.x, y }, COL_GRID, 1.0f);
        m2sdr_plot_format(text, sizeof(text), fabs(v) < 1e-12 * step ? 0.0 : v, step, M2SDR_PLOT_PLAIN);
        igCalcTextSize(&ts, text, NULL, false, -1.0f);
        ImDrawList_AddText_Vec2(plot->dl, (ImVec2){ plot->p0.x - ts.x - 5.0f, y - 0.5f * ts.y }, COL_LABEL, text, NULL);
    }

    /* Horizontal axis. */
    if (x_unit == M2SDR_PLOT_LOG_FREQ) {
        int decade, m;

        for (decade = (int)floor(x0); decade <= (int)ceil(x1); decade++) {
            for (m = 1; m < 10; m++) {
                const double lx = decade + log10((double)m);
                const float x = m2sdr_plot_x(plot, lx);

                if (lx < x0 - 1e-9 || lx > x1 + 1e-9)
                    continue;
                ImDrawList_AddLine(plot->dl, (ImVec2){ x, plot->p0.y }, (ImVec2){ x, plot->p1.y },
                                   m == 1 ? COL_GRID : (COL_GRID & 0x00ffffffu) | 0x18000000u, 1.0f);
                if (m != 1)
                    continue;
                m2sdr_plot_format(text, sizeof(text), pow(10.0, decade), pow(10.0, decade), M2SDR_PLOT_FREQ);
                igCalcTextSize(&ts, text, NULL, false, -1.0f);
                if (x + 0.5f * ts.x > plot->p1.x + PLOT_MARGIN_RIGHT)
                    continue;
                ImDrawList_AddText_Vec2(plot->dl, (ImVec2){ x - 0.5f * ts.x, plot->p1.y + 4.0f }, COL_LABEL, text, NULL);
            }
        }
        ImDrawList_AddRect(plot->dl, plot->p0, plot->p1, COL_FRAME, 0.0f, 0, 1.0f);
        ImDrawList_PushClipRect(plot->dl, plot->p0, plot->p1, true);
        return true;
    }
    if (!same)
        step = nice_step(x1 - x0, (int)((plot->p1.x - plot->p0.x) / 110.0f));
    edge = plot->p0.x - 0.5f * PLOT_MARGIN_LEFT;
    for (v = ceil(x0 / step - 1e-9) * step; v <= x1 + 1e-9 * step; v += step) {
        const float x = m2sdr_plot_x(plot, v);
        ImDrawList_AddLine(plot->dl, (ImVec2){ x, plot->p0.y }, (ImVec2){ x, plot->p1.y }, COL_GRID, 1.0f);
        m2sdr_plot_format(text, sizeof(text), fabs(v) < 1e-12 * step ? 0.0 : v, step, x_unit);
        igCalcTextSize(&ts, text, NULL, false, -1.0f);
        /* A label is kept in the plot at the right, and left out when it would run over the
         * one before it or over the ones of the vertical axis. */
        tx = x - 0.5f * ts.x;
        if (tx + ts.x > plot->p1.x + PLOT_MARGIN_RIGHT)
            tx = plot->p1.x + PLOT_MARGIN_RIGHT - ts.x;
        if (tx < edge)
            continue;
        ImDrawList_AddText_Vec2(plot->dl, (ImVec2){ tx, plot->p1.y + 4.0f }, COL_LABEL, text, NULL);
        edge = tx + ts.x + 6.0f;
    }

    ImDrawList_AddRect(plot->dl, plot->p0, plot->p1, COL_FRAME, 0.0f, 0, 1.0f);
    ImDrawList_PushClipRect(plot->dl, plot->p0, plot->p1, true);
    return true;
}

void m2sdr_plot_end(struct m2sdr_plot *plot)
{
    /* The label last, over the traces. */
    if (plot->label && plot->label[0])
        plot_text(plot, (ImVec2){ plot->p0.x + 5.0f, plot->p0.y + 3.0f }, plot->label, COL_LABEL);
    ImDrawList_PopClipRect(plot->dl);
}

void m2sdr_plot_trace(struct m2sdr_plot *plot, const float *y, size_t n, double xa, double xb, ImU32 color)
{
    const float width = plot->p1.x - plot->p0.x;
    const double dx = n > 1 ? (xb - xa) / (double)(n - 1) : 0.0;
    size_t first, last, i;

    if (!y || n < 2 || !(xb > xa))
        return;
    /* Values in view, with one on each side. */
    {
        double a = floor((plot->x0 - xa) / dx), b = ceil((plot->x1 - xa) / dx);
        if (a < 0)
            a = 0;
        if (b > (double)(n - 1))
            b = (double)(n - 1);
        if (b <= a)
            return;
        first = (size_t)a;
        last  = (size_t)b;
    }

    if ((double)(last - first) <= 2.0 * width) {
        ImVec2 prev = { m2sdr_plot_x(plot, xa + dx * (double)first), m2sdr_plot_y(plot, y[first]) };
        for (i = first + 1; i <= last; i++) {
            const ImVec2 p = { m2sdr_plot_x(plot, xa + dx * (double)i), m2sdr_plot_y(plot, y[i]) };
            ImDrawList_AddLine(plot->dl, prev, p, color, 1.0f);
            prev = p;
        }
    } else {
        /* One vertical segment per pixel, from the lowest to the highest of its values, joined
         * to the next. */
        const int pixels = (int)width;
        float prev_lo = 0, prev_hi = 0;
        int px;
        size_t k = first;

        for (px = 0; px < pixels; px++) {
            const double x_end = plot->x0 + (plot->x1 - plot->x0) * (double)(px + 1) / (double)pixels;
            float lo = 0, hi = 0, top, bottom;
            bool any = false;

            while (k <= last && xa + dx * (double)k < x_end) {
                if (!any || y[k] < lo)
                    lo = y[k];
                if (!any || y[k] > hi)
                    hi = y[k];
                any = true;
                k++;
            }
            if (!any)
                continue;
            if (px > 0 && prev_hi >= prev_lo) {
                if (prev_lo > hi)
                    hi = prev_lo;
                if (prev_hi < lo)
                    lo = prev_hi;
            }
            top    = m2sdr_plot_y(plot, hi);
            bottom = m2sdr_plot_y(plot, lo);
            if (bottom - top < 1.0f)
                bottom = top + 1.0f;
            ImDrawList_AddLine(plot->dl, (ImVec2){ plot->p0.x + (float)px + 0.5f, top },
                               (ImVec2){ plot->p0.x + (float)px + 0.5f, bottom }, color, 1.0f);
            prev_lo = lo;
            prev_hi = hi;
        }
    }
}

void m2sdr_plot_points(struct m2sdr_plot *plot, const float *x, const float *y, size_t n, float radius, ImU32 color)
{
    size_t i;

    for (i = 0; i < n; i++) {
        const float px = m2sdr_plot_x(plot, x[i]), py = m2sdr_plot_y(plot, y[i]);
        ImDrawList_AddRectFilled(plot->dl, (ImVec2){ px - radius, py - radius },
                                 (ImVec2){ px + radius, py + radius }, color, 0.0f, 0);
    }
}

void m2sdr_plot_band(struct m2sdr_plot *plot, double xa, double xb, ImU32 color)
{
    ImDrawList_AddRectFilled(plot->dl, (ImVec2){ m2sdr_plot_x(plot, xa), plot->p0.y },
                             (ImVec2){ m2sdr_plot_x(plot, xb), plot->p1.y }, color, 0.0f, 0);
}

void m2sdr_plot_vline(struct m2sdr_plot *plot, double x, ImU32 color)
{
    const float px = m2sdr_plot_x(plot, x);
    ImDrawList_AddLine(plot->dl, (ImVec2){ px, plot->p0.y }, (ImVec2){ px, plot->p1.y }, color, 1.0f);
}

void m2sdr_plot_hline(struct m2sdr_plot *plot, double y, ImU32 color)
{
    const float py = m2sdr_plot_y(plot, y);
    ImDrawList_AddLine(plot->dl, (ImVec2){ plot->p0.x, py }, (ImVec2){ plot->p1.x, py }, color, 1.0f);
}

void m2sdr_plot_marker(struct m2sdr_plot *plot, double x, double y, const char *label, ImU32 color)
{
    const float px = m2sdr_plot_x(plot, x);
    float py = m2sdr_plot_y(plot, y);
    ImVec2 ts;

    if (py < plot->p0.y + 14.0f)
        py = plot->p0.y + 14.0f;
    ImDrawList_AddTriangleFilled(plot->dl, (ImVec2){ px, py - 2.0f }, (ImVec2){ px - 5.0f, py - 11.0f },
                                 (ImVec2){ px + 5.0f, py - 11.0f }, color);
    if (label && label[0]) {
        igCalcTextSize(&ts, label, NULL, false, -1.0f);
        ImDrawList_AddText_Vec2(plot->dl, (ImVec2){ px - 0.5f * ts.x, py - 13.0f - ts.y }, color, label, NULL);
    }
}

void m2sdr_plot_text(struct m2sdr_plot *plot, float dx, float dy, const char *text, ImU32 color)
{
    const float x = dx >= 0 ? plot->p0.x + dx : plot->p1.x + dx;
    const float y = dy >= 0 ? plot->p0.y + dy : plot->p1.y + dy;
    plot_text(plot, (ImVec2){ x, y }, text, color);
}

void m2sdr_plot_xy(struct m2sdr_plot *plot, const float *x, const float *y, size_t n, ImU32 color)
{
    size_t i;

    for (i = 1; i < n; i++)
        ImDrawList_AddLine(plot->dl, (ImVec2){ m2sdr_plot_x(plot, x[i - 1]), m2sdr_plot_y(plot, y[i - 1]) },
                           (ImVec2){ m2sdr_plot_x(plot, x[i]), m2sdr_plot_y(plot, y[i]) }, color, 1.5f);
}
