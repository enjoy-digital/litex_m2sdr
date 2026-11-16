#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Timed TX gate: states of the TX header extractor for a 64-bit datapath.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 262)
    h = 40
    head  = d.add(Box(15,  110, 100, h, label="HEADER"))
    stamp = d.add(Box(150, 110, 110, h, label="TIMESTAMP"))
    gate  = d.add(Box(295, 110, 90,  h, label="GATE", fill=COLOR_ACCENT))
    frame = d.add(Box(470, 30,  110, h, label="FRAME", fill=COLOR_ACCENT_LIGHT))
    wait  = d.add(Box(470, 110, 110, h, label="WAIT"))
    drop  = d.add(Box(470, 190, 110, h, label="DROP"))
    d.add(Arrow((head, "r"), (stamp, "l"), kind="data"))
    d.add(Arrow((stamp, "r"), (gate, "l"), kind="data"))
    d.add(Arrow((gate, "r"), (wait, "l"), kind="data", label="early", label_side="above"))
    d.add(Arrow((gate, "t"), (frame, "l"), kind="data", label="untimed, on time",
        label_pos=(405, 40)))
    d.add(Arrow((gate, "b"), (drop, "l"), kind="data", label="late", label_pos=(405, 200)))
    d.add(Arrow((wait, "t"), (frame, "b"), kind="data", label="time reached", label_side="right"))
    d.add(Arrow((frame, "r"), (head, "t"), via=[(660, 50), (660, 12), (65, 12)], kind="control",
        label="end of frame", label_pos=(200, 25)))
    d.add(Arrow((drop, "r"), (head, "b"), via=[(660, 210), (660, 250), (65, 250)], kind="control",
        label="frame dropped, underflow counted", label_pos=(220, 238)))
    return d
