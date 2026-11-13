#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Direct conversion (zero-IF) receiver: the RF signal is mixed down to two baseband signals.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 190)
    ant  = d.add(Box(10, 72, 75, 40, label="Antenna", fill=COLOR_HARDWARE))
    lna  = d.add(Box(110, 72, 60, 40, label="LNA"))
    lo   = d.add(Box(215, 75, 75, 34, label="LO", fill=COLOR_ACCENT))
    d.add(Arrow((ant, "r"), (lna, "l"), kind="data"))
    d.add(Bus([(170, 92), (192, 92)], kind="data"))
    d.add(Bus([(192, 35), (192, 150)], kind="data"))
    dig  = d.add(Box(550, 15, 100, 155, label="Digital\nfilters", fill=COLOR_ACCENT_LIGHT))
    for name, y, phase, side in (("I", 15, "0°", "t"), ("Q", 130, "90°", "b")):
        mix = d.add(Box(215, y, 75, 40, label="Mixer", fill=COLOR_ACCENT))
        lpf = d.add(Box(330, y, 85, 40, label="Low-pass"))
        adc = d.add(Box(455, y, 55, 40, label="ADC"))
        d.add(Arrow((192, y + 20), (mix, "l"), kind="data"))
        d.add(Arrow((mix, "r"), (lpf, "l"), kind="data"))
        d.add(Arrow((lpf, "r"), (adc, "l"), kind="data"))
        d.add(Arrow((adc, "r"), (550, y + 20), kind="data"))
        d.add(Arrow((650, y + 20), (745, y + 20), kind="accent", label=name + " samples"))
        if side == "t":
            d.add(Arrow((lo, "t"), (mix, "b"), label=phase, label_side="right"))
        else:
            d.add(Arrow((lo, "b"), (mix, "t"), label=phase, label_side="right"))
    return d
