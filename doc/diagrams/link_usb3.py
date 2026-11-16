#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# USB3 host link: USB3-PIPE PHY and device core, EP0 bridge and Bulk streams.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 330)
    host = d.add(Box(20, 130, 100, 70, label="USB3 host\nSFP adapter", fill=COLOR_HARDWARE))
    phy  = d.add(Box(160, 130, 120, 70, label="Transceiver\nUSB3PHY", fill=COLOR_ACCENT_LIGHT))
    core = d.add(Box(320, 40, 130, 250, label="USB3\nDeviceCore", fill=COLOR_ACCENT))
    ep0  = d.add(Box(520,  40, 120, 60, label="EP0 bridge"))
    bin_ = d.add(Box(520, 135, 120, 60, label="Bulk IN\nRX streamer"))
    bout = d.add(Box(520, 230, 120, 60, label="Bulk OUT\nTX streamer"))
    d.add(Arrow((host, "r"), (phy, "l"), kind="data", heads="both"))
    d.add(Arrow((phy, "r"), (320, 165), kind="data", heads="both"))
    d.add(Arrow((450,  70), (ep0,  "l"), kind="control", heads="both"))
    d.add(Arrow((bin_, "l"), (450, 165), kind="accent"))
    d.add(Arrow((450, 260), (bout, "l"), kind="accent"))
    d.add(Arrow((ep0,  "r"), (740,  70), kind="control", heads="both", label="SoC bus"))
    d.add(Arrow((740, 165), (bin_, "r"), kind="accent", label="crossbar"))
    d.add(Arrow((bout, "r"), (740, 260), kind="accent", label="crossbar"))
    return d
