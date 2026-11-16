#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Two-way time transfer: four timestamps give the offset of the slave and the delay of the path.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 270)
    xm, xs = 220, 540
    d.add(Box(xm - 70, 10, 140, 36, label="Master", fill=COLOR_HARDWARE))
    d.add(Box(xs - 70, 10, 140, 36, label="Slave", fill=COLOR_ACCENT))
    d.add(Bus([(xm, 46), (xm, 255)], dash="4,3"))
    d.add(Bus([(xs, 46), (xs, 255)], dash="4,3"))
    rows = [
        (85,  True,  "Sync",       "t1", "t2"),
        (130, True,  "Follow_Up",  "",   ""),
        (180, False, "Delay_Req",  "t4", "t3"),
        (225, True,  "Delay_Resp", "",   ""),
    ]
    for y, to_slave, name, tm, ts in rows:
        a, b = ((xm, y), (xs, y)) if to_slave else ((xs, y), (xm, y))
        d.add(Arrow(a, b, kind="accent" if tm else "control", label=name))
        if tm:
            d.add(Text(xm - 14, y, tm, size=13, anchor="end"))
            d.add(Text(xs + 14, y, ts, size=13, anchor="start"))
    return d
