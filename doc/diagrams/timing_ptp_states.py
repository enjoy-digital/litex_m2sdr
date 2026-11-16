#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# States of the PTP board time discipline.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 250)
    man  = d.add(Box(20,  95, 130, 60, label="Manual"))
    acq  = d.add(Box(225, 95, 130, 60, label="Acquire", fill=COLOR_ACCENT_LIGHT))
    lck  = d.add(Box(430, 95, 130, 60, label="Locked", fill=COLOR_ACCENT))
    hld  = d.add(Box(630, 95, 110, 60, label="Holdover", fill=COLOR_ACCENT_LIGHT))
    d.add(Arrow((man, "r"), (acq, "l"), kind="control", label="enable"))
    d.add(Arrow((acq, "r", 0.3), (lck, "l", 0.3), kind="accent", label="in window"))
    d.add(Arrow((lck, "l", 0.7), (acq, "r", 0.7), kind="control", label="misses", label_side="below"))
    d.add(Arrow((lck, "r", 0.3), (hld, "l", 0.3), kind="control", label="PTP lost"))
    d.add(Arrow((hld, "l", 0.7), (lck, "r", 0.7), kind="control", label="in window", label_side="below"))
    d.add(Arrow((hld, "t"), (acq, "t"), kind="feedback", via=[(685, 50), (290, 50)], label="PTP locked again", label_pos=(488, 41)))
    d.add(Arrow((hld, "b"), (man, "b"), kind="feedback", via=[(685, 205), (85, 205)], label="disable, from any state", label_pos=(385, 196)))
    return d
