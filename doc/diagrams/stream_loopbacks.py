#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Loopback points of the sample path, from the host to the antenna.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 215)
    y, h, w = 40, 120, 80
    names = ["Host", "DMA", "Crossbar,\nheader", "RFIC\ncore", "AD9361"]
    fills = [COLOR_HARDWARE, COLOR_FILL, COLOR_FILL, COLOR_FILL, COLOR_HARDWARE]
    boxes = [d.add(Box(10 + 150*i, y, w, h, label=n, fill=f)) for i, (n, f) in enumerate(zip(names, fills))]
    for a, b in zip(boxes, boxes[1:]):
        d.add(Arrow((a, "r", 0.25), (b, "l", 0.25), kind="data"))
        d.add(Arrow((b, "l", 0.75), (a, "r", 0.75), kind="data"))
    for x, name in [(275, "DMA\nloopback"), (425, "Stream\nloopback"), (575, "PHY\nloopback")]:
        d.add(Arrow((x, 70), (x, 130), kind="accent"))
        d.add(Text(x, 185, name, size=FONT_SIZE_SM))
    d.add(Arrow((690, 70), (690, 130), via=[(725, 70), (725, 130)], kind="accent"))
    d.add(Text(722, 185, "RFIC\nloopback", size=FONT_SIZE_SM))
    return d
