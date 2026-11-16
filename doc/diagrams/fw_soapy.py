#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# SoapySDR driver: one module, PCIe DMA or Ethernet UDP selected by the device arguments.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 330)
    apps  = d.add(Box(20, 40, 150, 250, label="GQRX\nGNU Radio\nsrsRAN\nPython, C++", fill=COLOR_HARDWARE))
    d.add(Container(215, 20, 350, 290, title="SoapySDR module", title_pos="br"))
    dev   = d.add(Box(240, 50, 300, 55, label="Device settings\nRF, clock, time, sensors", fill=COLOR_ACCENT))
    strm  = d.add(Box(240, 135, 220, 55, label="Streaming\nCF32, CS16, CS8", fill=COLOR_ACCENT))
    lib   = d.add(Box(240, 220, 300, 50, label="libm2sdr", fill=COLOR_ACCENT_LIGHT))
    pcie  = d.add(Box(610, 60, 130, 70, label="PCIe\nDMA", fill=COLOR_HARDWARE))
    eth   = d.add(Box(610, 200, 130, 70, label="Ethernet\nUDP", fill=COLOR_HARDWARE))
    d.add(Arrow((apps.x + apps.w, dev.cy), (dev, "l"), kind="control", heads="both"))
    d.add(Arrow((apps.x + apps.w, strm.cy), (strm, "l"), kind="accent", heads="both"))
    d.add(Arrow((500, dev.y + dev.h), (500, lib.y), kind="control", heads="both"))
    d.add(Arrow((strm, "b"), (strm.cx, lib.y), kind="data", heads="both"))
    d.add(Arrow((lib.x + lib.w, 235), (pcie, "l"), kind="data", heads="both",
        via=[(pcie.x - 22, 235), (pcie.x - 22, pcie.cy)]))
    d.add(Arrow((lib.x + lib.w, 255), (eth.x, 255), kind="data", heads="both"))
    return d
