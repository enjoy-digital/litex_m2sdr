#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# One reference clock sets the sample rate, the carrier frequency and the pace of the board time.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 230)
    ref = d.add(Box(20, 90, 120, 50, label="Reference\nclock", fill=COLOR_ACCENT))
    d.add(Bus([(140, 115), (170, 115)]))
    d.add(Bus([(170, 40), (170, 190)]))
    rows = [
        ("Sample clock",   "ADC, DAC",   "sample rate"),
        ("LO synthesizer", "Mixers",     "carrier"),
        ("Time counter",   "Timestamps", "time"),
    ]
    for i, (a, b, label) in enumerate(rows):
        y = 20 + 75*i
        ba = d.add(Box(220, y, 140, 40, label=a, fill=COLOR_ACCENT_LIGHT))
        bb = d.add(Box(540, y, 140, 40, label=b))
        d.add(Arrow((170, y + 20), (ba, "l")))
        d.add(Arrow((ba, "r"), (bb, "l"), label=label))
    return d
