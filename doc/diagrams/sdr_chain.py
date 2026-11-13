#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Digital chain of the AD9361: three half-band stages and a programmable FIR on each side.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 200)
    d.add(Container(10, 10, 740, 180, title="AD9361", title_pos="bl"))
    port = d.add(Box(620, 30, 110, 115, label="Data port", fill=COLOR_ACCENT))
    rx = ["ADC", "HB3", "HB2", "HB1", "RX FIR"]
    tx = ["DAC", "HB3", "HB2", "HB1", "TX FIR"]
    for labels, y, to_port in ((rx, 30, True), (tx, 105, False)):
        boxes = []
        for i, label in enumerate(labels):
            fill = COLOR_HARDWARE if i == 0 else COLOR_ACCENT_LIGHT
            boxes.append(d.add(Box(30 + 118*i, y, 85, 40, label=label, fill=fill)))
        for a, b in zip(boxes, boxes[1:]):
            if to_port:
                d.add(Arrow((a, "r"), (b, "l"), kind="data"))
            else:
                d.add(Arrow((b, "l"), (a, "r"), kind="data"))
        if to_port:
            d.add(Arrow((boxes[-1], "r"), (620, y + 20), kind="data"))
        else:
            d.add(Arrow((620, y + 20), (boxes[-1], "r"), kind="data"))
    return d
