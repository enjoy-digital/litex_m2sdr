#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# DFX build: the reconfigurable partition takes the place of the user processing hook; DMA1 and
# the ICAP loader reach it besides the sample path.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 290)
    d.add(Container(100, 20, 560, 255, title="FPGA", title_pos="br"))
    host = d.add(Box(10, 40, 70, 210, label="Host", fill=COLOR_HARDWARE))
    dma0 = d.add(Box(120, 40, 80, 50, label="DMA0"))
    dma1 = d.add(Box(120, 120, 80, 50, label="DMA1", fill=COLOR_ACCENT_LIGHT))
    icap = d.add(Box(120, 200, 80, 50, label="ICAP\nloader", fill=COLOR_ACCENT_LIGHT))
    path = d.add(Box(240, 40, 100, 50, label="Header,\nloopback"))
    part = d.add(Box(390, 40, 130, 210, label="Reconfigurable\npartition", fill=COLOR_ACCENT))
    rfic = d.add(Box(560, 40, 80, 50, label="RFIC\ncore"))
    chip = d.add(Box(680, 40, 70, 50, label="AD9361", fill=COLOR_HARDWARE))
    d.add(Arrow((80, 65), (dma0, "l"), kind="data", heads="both"))
    d.add(Arrow((80, 145), (dma1, "l"), kind="data", heads="both"))
    d.add(Arrow((80, 225), (icap, "l"), kind="control"))
    d.add(Arrow((dma0, "r"), (path, "l"), kind="data", heads="both"))
    d.add(Arrow((path, "r"), (390, 65), kind="data", heads="both"))
    d.add(Arrow((dma1, "r"), (390, 145), kind="accent", heads="both", label="Side band"))
    d.add(Arrow((icap, "r"), (390, 225), kind="control", label="Partial load"))
    d.add(Arrow((520, 65), (rfic, "l"), kind="data", heads="both"))
    d.add(Arrow((rfic, "r"), (chip, "l"), kind="data", heads="both"))
    return d
