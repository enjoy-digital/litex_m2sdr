/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR plot helper: axes, grid, traces and markers on the Dear ImGui draw list.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_PLOT_H
#define M2SDR_PLOT_H

#include <stdbool.h>
#include <stddef.h>

#ifndef CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#endif
#include "cimgui/cimgui.h"

/* Labels of an axis. */
enum m2sdr_plot_unit {
    M2SDR_PLOT_PLAIN = 0,   /* The number. */
    M2SDR_PLOT_FREQ  = 1,   /* Hz, shown in Hz, kHz, MHz or GHz. */
    M2SDR_PLOT_TIME  = 2,   /* Seconds, shown in s, ms, us or ns. */
    M2SDR_PLOT_LOG_FREQ = 3,   /* log10 of Hz: a line per decade, labelled in Hz, kHz or MHz. */
};

struct m2sdr_plot {
    ImDrawList *dl;
    ImVec2 p0, p1;          /* Corners of the plot area, in screen pixels. */
    double x0, x1;          /* Range of the horizontal axis. */
    double y0, y1;          /* Range of the vertical axis, y1 at the top. */
    bool   hovered;         /* The mouse is over the plot area. */
    bool   clicked;         /* Left click on it in this frame. */
    double mouse_x;         /* Position of the mouse on the axes. */
    double mouse_y;
};

/* Value to its text, with the unit of an axis: "1.8525 GHz", "250 us". `step` is the smallest
 * difference that must show. */
void m2sdr_plot_format(char *buf, size_t len, double value, double step, enum m2sdr_plot_unit unit);

/* Start a plot of `size` pixels (0: what is left of the window) at the cursor. It draws the
 * frame, the grid and the labels, and takes the mouse. Returns false when it is not visible;
 * m2sdr_plot_end() is only called after a true. */
bool m2sdr_plot_begin(struct m2sdr_plot *plot, const char *id, ImVec2 size,
                      double x0, double x1, enum m2sdr_plot_unit x_unit,
                      double y0, double y1, const char *y_label);
void m2sdr_plot_end(struct m2sdr_plot *plot);

float m2sdr_plot_x(const struct m2sdr_plot *plot, double x);
float m2sdr_plot_y(const struct m2sdr_plot *plot, double y);

/* Trace of n values spread evenly from xa to xb. With more values than pixels, each pixel shows
 * the range of its values. */
void m2sdr_plot_trace(struct m2sdr_plot *plot, const float *y, size_t n, double xa, double xb, ImU32 color);

/* Line through n points of any spacing. */
void m2sdr_plot_xy(struct m2sdr_plot *plot, const float *x, const float *y, size_t n, ImU32 color);

/* Points, as dots. */
void m2sdr_plot_points(struct m2sdr_plot *plot, const float *x, const float *y, size_t n, float radius, ImU32 color);

/* Vertical band and lines, and a marker with its label. */
void m2sdr_plot_band(struct m2sdr_plot *plot, double xa, double xb, ImU32 color);
void m2sdr_plot_vline(struct m2sdr_plot *plot, double x, ImU32 color);
void m2sdr_plot_hline(struct m2sdr_plot *plot, double y, ImU32 color);
void m2sdr_plot_marker(struct m2sdr_plot *plot, double x, double y, const char *label, ImU32 color);
void m2sdr_plot_text(struct m2sdr_plot *plot, float dx, float dy, const char *text, ImU32 color);

#endif /* M2SDR_PLOT_H */
