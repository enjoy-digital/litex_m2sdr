#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Getting started path: from the software installation to a first SoapySDR application.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 110)
    labels = ["Install\nsoftware", "Load\ndriver", "Check\nboard", "Configure\nRFIC", "Record\nand play",
        "SoapySDR\napplication"]
    boxes = []
    for i, label in enumerate(labels):
        fill = COLOR_ACCENT if i in (2, 3, 4) else COLOR_FILL
        boxes.append(d.add(Box(15 + 125*i, 25, 105, 60, label=label, fill=fill)))
    for a, b in zip(boxes, boxes[1:]):
        d.add(Arrow((a, "r"), (b, "l"), kind="data"))
    return d
