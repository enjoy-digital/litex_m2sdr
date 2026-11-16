#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Layout of a buffer with a header, and of a 64-bit sample word in SC16 and SC8.

from svgdiag import *


X0, H = 20, 34


def strip(d, y, cells, notes):
    x = X0
    for (w, label, fill), note in zip(cells, notes):
        d.add(Box(x, y, w, H, label=label, radius=0, size=13, fill=fill))
        if note:
            d.add(Text(x + w/2, y + H + 12, note, size=FONT_SIZE_SM))
        x += w


def build():
    d = Diagram(CANVAS_WIDTH, 300)
    d.add(Text(X0, 20, "Buffer with a header (8192 bytes by default)", anchor="start", italic=True))
    strip(d, 35, [(150, "Sync word", COLOR_ACCENT), (150, "Timestamp", COLOR_ACCENT),
        (420, "Samples", COLOR_FILL)], ["bytes 0 to 7", "bytes 8 to 15", "bytes 16 to 8191"])
    d.add(Text(X0, 120, "Sample word, SC16 (8 bytes)", anchor="start", italic=True))
    strip(d, 135, [(180, n, COLOR_FILL) for n in ["I1", "Q1", "I2", "Q2"]],
        ["bytes 0, 1", "bytes 2, 3", "bytes 4, 5", "bytes 6, 7"])
    d.add(Text(X0, 220, "Sample word, SC8 (8 bytes)", anchor="start", italic=True))
    strip(d, 235, [(90, n, f) for f in (COLOR_FILL, COLOR_ACCENT_LIGHT) for n in ["I1", "Q1", "I2", "Q2"]],
        ["", "sample n", "", "", "", "sample n+1", "", ""])
    return d
