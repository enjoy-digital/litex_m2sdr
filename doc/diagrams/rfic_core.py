#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# RFIC core: the stages between the 64-bit streams of the SoC and the pins of the AD9361.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 250)
    d.add(Container(45, 20, 255, 215, title="sys domain", title_pos="bl"))
    d.add(Container(385, 20, 270, 215, title="rfic domain", title_pos="bl"))
    buf  = d.add(Box(60, 60, 85, 60, label="Buffer"))
    mode = d.add(Box(175, 60, 110, 60, label="Bit mode", fill=COLOR_ACCENT))
    cdc  = d.add(Box(315, 60, 55, 60, label="CDC"))
    fifo = d.add(Box(400, 60, 60, 60, label="FIFO"))
    gpio = d.add(Box(490, 60, 60, 60, label="GPIO\nbits"))
    phy  = d.add(Box(580, 60, 60, 60, label="PHY", fill=COLOR_ACCENT))
    chip = d.add(Box(680, 60, 70, 60, label="AD9361", fill=COLOR_HARDWARE))
    prbs = d.add(Box(570, 160, 80, 44, label="PRBS", fill=COLOR_ACCENT_LIGHT))
    d.add(Arrow((5, 90), (buf, "l"), kind="data", heads="both"))
    d.add(Text(22, 72, "TX, RX", size=FONT_SIZE_SM))
    for a, b in [(buf, mode), (mode, cdc), (cdc, fifo), (fifo, gpio), (gpio, phy), (phy, chip)]:
        d.add(Arrow((a, "r"), (b, "l"), kind="data", heads="both"))
    d.add(Arrow((prbs, "t"), (phy, "b"), kind="control", heads="both"))
    return d
