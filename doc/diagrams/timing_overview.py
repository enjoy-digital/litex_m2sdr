#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Board time: who sets it and who uses it.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 380)
    sw   = d.add(Box(20,  40, 170, 60, label="Host software\nregister write"))
    ptm  = d.add(Box(20, 125, 170, 60, label="PCIe PTM\nhost clock"))
    ptp  = d.add(Box(20, 210, 170, 60, label="Ethernet PTP\ndiscipline"))
    clk  = d.add(Box(20, 295, 170, 60, label="SI5351\n100 MHz clock", fill=COLOR_HARDWARE))
    gen  = d.add(Box(295, 40, 150, 315, label="TimeGenerator\nboard time", fill=COLOR_ACCENT))
    hdr_ = d.add(Box(560,  40, 180, 60, label="RX and TX headers\ntimed TX"))
    pps  = d.add(Box(560, 145, 180, 60, label="PPSGenerator\ninternal PPS"))
    vrt  = d.add(Box(560, 250, 180, 60, label="VRT timestamps"))
    d.add(Arrow((sw,  "r"), (295,  70), kind="control", label="write, adjust"))
    d.add(Arrow((ptm, "r"), (295, 155), kind="control", label="PHC, phc2sys"))
    d.add(Arrow((ptp, "r"), (295, 240), kind="accent", label="step, trim, rate"))
    d.add(Arrow((clk, "r"), (295, 325), kind="feedback", label="time domain"))
    d.add(Arrow((445,  70), (hdr_, "l"), kind="data", label="64-bit ns"))
    d.add(Arrow((445, 175), (pps,  "l"), kind="data"))
    d.add(Arrow((445, 280), (vrt,  "l"), kind="data"))
    return d
