#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# AD9361 LVDS data port: what the six data pairs carry on each DATA_CLK cycle, in 2R2T and 1R1T.

from svgdiag import *


X0, CW, H = 150, 150, 28


def strip(d, y, name, cells):
    d.add(Text(10, y + H/2, name, anchor="start", size=13))
    x = X0
    for w, label, fill in cells:
        d.add(Box(x, y, w, H, label=label, radius=0, size=13, fill=fill))
        x += w


def group(d, y, title, frame, names):
    d.add(Text(10, y, title, anchor="start", italic=True))
    strip(d, y + 18,       "DATA_CLK cycle", [(CW, str(i), COLOR_SHADE) for i in range(4)])
    strip(d, y + 18 + H,   "Frame",          frame)
    strip(d, y + 18 + 2*H, "Rising edge",    [(CW, f"I {n}", COLOR_ACCENT) for n in names])
    strip(d, y + 18 + 3*H, "Falling edge",   [(CW, f"Q {n}", COLOR_ACCENT_LIGHT) for n in names])


def build():
    d = Diagram(CANVAS_WIDTH, 330)
    group(d, 20, "2R2T mode",
        [(2*CW, "high", COLOR_FILL), (2*CW, "low", COLOR_FILL)],
        ["channel 1 [11:6]", "channel 1 [5:0]", "channel 2 [11:6]", "channel 2 [5:0]"])
    group(d, 180, "1R1T mode",
        [(CW, "high", COLOR_FILL), (CW, "low", COLOR_FILL)]*2,
        ["sample n [11:6]", "sample n [5:0]", "sample n+1 [11:6]", "sample n+1 [5:0]"])
    return d
