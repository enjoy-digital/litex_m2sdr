#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# SATA workflows: the commands of m2sdr_sata and the path each one uses.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 300)
    rf   = d.add(Box(20, 40, 130, 60, label="RFIC", fill=COLOR_HARDWARE))
    host = d.add(Box(20, 200, 130, 60, label="Host files\nSigMF, raw", fill=COLOR_HARDWARE))
    d.add(Container(255, 20, 250, 260, title="Gateware", title_pos="br"))
    xbar = d.add(Box(280, 40, 200, 60, label="Stream routing", fill=COLOR_ACCENT_LIGHT))
    sata = d.add(Box(280, 170, 200, 60, label="SATA storage\nLiteSATA", fill=COLOR_ACCENT))
    ssd  = d.add(Box(570, 170, 170, 60, label="SSD\nCapture Volume", fill=COLOR_HARDWARE))
    app  = d.add(Box(570, 40, 170, 60, label="Host RX path\nGQRX, SoapySDR", fill=COLOR_HARDWARE))
    d.add(Arrow((rf, "r"), (xbar, "l"), kind="data", heads="both", label="capture, play", label_pos=(202, 60)))
    d.add(Arrow((xbar, "b"), (sata, "t"), kind="accent", heads="both"))
    d.add(Arrow((sata, "r"), (ssd, "l"), kind="accent", heads="both"))
    d.add(Arrow((xbar, "r"), (app, "l"), kind="data", label="serve", label_side="above"))
    d.add(Arrow((host.x + host.w, 215), (sata.x, 215), kind="data", heads="both", label="import, export",
        label_pos=(202, 232)))
    return d
