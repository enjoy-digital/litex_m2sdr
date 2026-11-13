#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Release flow: builds, timing check, dated archives, GitHub release, release flasher.

from svgdiag import *


def build():
    d = Diagram(CANVAS_WIDTH, 250)
    rel = d.add(Box( 15,  40, 100, 60, label="release.py", fill=COLOR_ACCENT))
    bld = d.add(Box(140,  40,  95, 60, label="Vivado\nbuilds", stack=2))
    tim = d.add(Box(265,  40,  95, 60, label="Timing\ncheck", fill=COLOR_ACCENT_LIGHT))
    arc = d.add(Box(390,  40,  95, 60, label="Dated\narchives", fill=COLOR_ACCENT_LIGHT))
    pub = d.add(Box(520,  40, 150, 60, label="github_release.py", fill=COLOR_ACCENT))
    ghr = d.add(Box(520, 160, 150, 60, label="GitHub release", fill=COLOR_HARDWARE))
    flr = d.add(Box(300, 160, 150, 60, label="flash_release.py", fill=COLOR_ACCENT))
    brd = d.add(Box(140, 160,  95, 60, label="Board", fill=COLOR_HARDWARE))
    d.add(Arrow((rel, "r"), (bld, "l"), kind="data"))
    d.add(Arrow((bld, "r"), (tim, "l"), kind="data"))
    d.add(Arrow((tim, "r"), (arc, "l"), kind="data"))
    d.add(Arrow((arc, "r"), (pub, "l"), kind="data"))
    d.add(Arrow((pub, "b"), (ghr, "t"), kind="data", label="tag, upload", label_side="right", label_anchor="start"))
    d.add(Arrow((ghr, "l"), (flr, "r"), kind="data", label="download"))
    d.add(Arrow((flr, "l"), (brd, "r"), kind="accent", label="JTAG"))
    return d
