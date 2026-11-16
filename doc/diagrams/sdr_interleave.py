#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# 2R2T interleave of the LVDS data port: eight 6-bit words per frame, one per clock edge.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 190)
    x0, w = 115, 78
    words = ["I1 MSB", "Q1 MSB", "I1 LSB", "Q1 LSB", "I2 MSB", "Q2 MSB", "I2 LSB", "Q2 LSB"]
    # DATA_CLK: one word per edge.
    pts = [(x0, 45)]
    for i in range(8):
        y = 25 if i % 2 == 0 else 45
        pts += [(x0 + w*i, y), (x0 + w*(i + 1), y)]
    d.add(Bus(pts, color=COLOR_STROKE))
    # RX_FRAME: high for channel 1, low for channel 2.
    d.add(Bus([(x0, 100), (x0, 80), (x0 + 4*w, 80), (x0 + 4*w, 100), (x0 + 8*w, 100)], color=COLOR_CYAN))
    for i, word in enumerate(words):
        fill = COLOR_ACCENT if i < 4 else COLOR_ACCENT_LIGHT
        d.add(Box(x0 + w*i, 125, w, 36, label=word, radius=0, fill=fill, size=13))
    for y, name in ((35, "DATA_CLK"), (90, "RX_FRAME"), (143, "RX_D[5:0]")):
        d.add(Text(x0 - 12, y, name, size=12, anchor="end"))
    return d
