#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# A LiteX stream: data moves on the cycles where valid and ready are both high.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 150)
    src = d.add(Box(120, 20, 150, 110, label="Source", fill=COLOR_ACCENT_LIGHT))
    snk = d.add(Box(490, 20, 150, 110, label="Sink", fill=COLOR_ACCENT_LIGHT))
    d.add(Arrow((270, 45), (490, 45), label="valid"))
    d.add(Arrow((270, 77), (490, 77), kind="data", label="data, last"))
    d.add(Arrow((490, 110), (270, 110), label="ready"))
    return d
