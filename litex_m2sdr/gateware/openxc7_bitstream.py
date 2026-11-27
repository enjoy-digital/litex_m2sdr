#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Bitstream fix-ups of the openXC7 build (experimental).

Two things the design needs are not in the Project X-Ray database, so nextpnr-xilinx and
fasm2frames cannot produce them. Both were found on an XC7A200T by comparing with a Vivado
bitstream of the same design (prjxray's bit2fasm --verbose lists the bits it does not know):

- The differential termination of an LVDS_25 input pair of the right IO column (DIFF_TERM=TRUE,
  AD9361 data interface): Vivado does not make the P pad input-only, and sets six bits in its
  RIOB33 tile. Without it the AD9361 data does not come in.
- The fabric reference clock of a GTP PLL (GTGREFCLK0_USED) on the mid-column tiles of the
  XC7A200T: the database has the feature for the edge-column tile only, where it is the same three
  bits 28 frames further (283 of the 284 features the tiles share differ by that offset).
  nextpnr-xilinx only writes the feature for a clock given on GTREFCLK, that it moves to
  GTGREFCLK0 with a port rename that crashes on some netlists: the clock is given on GTGREFCLK0
  instead (see SharedQPLL), and the input is enabled here wherever a clock is routed to it.

The build runs this file twice, around fasm2frames:

    openxc7_bitstream.py fasm   ...   before: known features are changed in the FASM, and what is
                                      not known to the database is taken out of it and noted;
    openxc7_bitstream.py frames ...   after:  the noted bits are set in the frames.
"""

import argparse
import csv
import json
import re

# Bits ---------------------------------------------------------------------------------------------

# <frame offset>_<bit offset> in the tile, as in the segbits files of the database.
DIFF_TERM_BITS      = ["38_100", "38_118", "39_113", "39_119", "39_125", "39_97"]
DIFF_TERM_TILES     = ("RIOB33",)
GTGREFCLK_BITS      = ["00_1438", "00_1439", "01_1438"]
GTGREFCLK_TILES     = ("GTP_COMMON_MID_LEFT", "GTP_COMMON_MID_RIGHT")
GTGREFCLK_FEATURE   = "GTPE2_COMMON.GTGREFCLK0_USED"
GTGREFCLK_ROUTE     = "GTPE2_COMMON_GTGREFCLK0."   # A clock routed to the input.

IOB_IN_ONLY   = "LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVDS_25_LVTTL_SSTL135_SSTL15_TMDS_33.IN_ONLY"
IOB_SLEW_FAST = "LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVTTL.SLEW.FAST"

def tile_type(tile):
    return re.sub(r"_X\d+Y\d+$", "", tile)

# XDC ----------------------------------------------------------------------------------------------

def diff_term_pins(xdc):
    """Package pins of the ports with DIFF_TERM TRUE."""
    loc, term = {}, set()
    for line in open(xdc):
        m = re.match(r"set_property\s+(\S+)\s+(\S+)\s+\[get_ports\s+\{?(.+?)\}?\s*\]$", line.strip())
        if m is None:
            continue
        prop, value, port = m.groups()
        if prop == "LOC":
            loc[port] = value
        elif prop == "DIFF_TERM" and value.upper() == "TRUE":
            term.add(port)
    return sorted(loc[port] for port in term if port in loc)

# FASM ---------------------------------------------------------------------------------------------

def fix_fasm(fasm, xdc, package_pins, notes):
    pin_tile = {row[0]: row[3] for row in csv.reader(open(package_pins)) if len(row) > 3}
    tiles    = sorted({pin_tile[pin] for pin in diff_term_pins(xdc)})
    for tile in tiles:
        if tile_type(tile) not in DIFF_TERM_TILES:
            raise ValueError(f"DIFF_TERM on {tile}: only known for {', '.join(DIFF_TERM_TILES)} tiles.")

    bits  = [(tile, bit) for tile in tiles for bit in DIFF_TERM_BITS]
    lines = []
    gtgrefclk_tiles = []                          # GTP tiles with a clock routed to GTGREFCLK0.
    for line in open(fasm).read().split("\n"):
        feature = line.strip()
        tile, _, name = feature.partition(".")
        # The P pad of a terminated pair is not input-only.
        if tile in tiles and name == f"IOB_Y0.{IOB_IN_ONLY}":
            continue
        if name.startswith(GTGREFCLK_ROUTE) and tile not in gtgrefclk_tiles:
            gtgrefclk_tiles.append(tile)
        if name == GTGREFCLK_FEATURE:
            if tile not in gtgrefclk_tiles:
                gtgrefclk_tiles.append(tile)
            continue
        lines.append(line)
    for tile in gtgrefclk_tiles:
        if tile_type(tile) in GTGREFCLK_TILES:
            # Not in the database for these tiles: set in the frames.
            bits += [(tile, bit) for bit in GTGREFCLK_BITS]
        else:
            lines.append(f"{tile}.{GTGREFCLK_FEATURE}")
    for tile in tiles:
        lines += [
            f"{tile}.IOB_Y0.IN_TERM.NONE",
            f"{tile}.IOB_Y1.IN_TERM.NONE",
            f"{tile}.IOB_Y0.{IOB_SLEW_FAST}",
            f"{tile}.IOB_Y1.{IOB_SLEW_FAST}",
        ]
    open(fasm, "w").write("\n".join(lines).rstrip("\n") + "\n")
    json.dump(bits, open(notes, "w"))
    print(f"openxc7_bitstream: {len(tiles)} terminated LVDS pairs, {len(bits)} bits for the frames.")

# Frames -------------------------------------------------------------------------------------------

def fix_frames(frames, tilegrid, notes):
    grid   = json.load(open(tilegrid))
    wanted = {}                                   # frame address -> [(word, bit)]
    for tile, bit in json.load(open(notes)):
        block = grid[tile]["bits"]["CLB_IO_CLK"]
        frame, offset = (int(v) for v in bit.split("_"))
        if frame >= block["frames"] or offset >= 32 * block["words"]:
            raise ValueError(f"{tile}: bit {bit} is outside the tile.")
        address = int(block["baseaddr"], 16) + frame
        wanted.setdefault(address, []).append((block["offset"] + offset // 32, offset % 32))

    lines = []
    for line in open(frames).read().split("\n"):
        if line.strip():
            address, words = line.split(" ", 1)
            if int(address, 16) in wanted:
                words = [int(w, 16) for w in words.split(",")]
                for word, bit in wanted.pop(int(address, 16)):
                    words[word] |= 1 << bit
                line = address + " " + ",".join("0x%08X" % w for w in words)
        lines.append(line)
    if wanted:
        raise ValueError("frames not in the file: " + ", ".join("0x%08X" % a for a in sorted(wanted)))
    open(frames, "w").write("\n".join(lines))
    print("openxc7_bitstream: frames updated.")

# Main ---------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub    = parser.add_subparsers(dest="step", required=True)
    p = sub.add_parser("fasm")
    p.add_argument("--xdc",          required=True)
    p.add_argument("--package-pins", required=True)
    p.add_argument("--notes",        required=True)
    p.add_argument("fasm")
    p = sub.add_parser("frames")
    p.add_argument("--tilegrid", required=True)
    p.add_argument("--notes",    required=True)
    p.add_argument("frames")
    args = parser.parse_args()
    if args.step == "fasm":
        fix_fasm(args.fasm, args.xdc, args.package_pins, args.notes)
    else:
        fix_frames(args.frames, args.tilegrid, args.notes)

if __name__ == "__main__":
    main()
