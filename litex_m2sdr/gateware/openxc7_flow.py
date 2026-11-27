#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Synthesis and bitstream steps of the openXC7 build (Yosys + nextpnr-xilinx + Project X-Ray,
experimental), shared by the SoC and the bring-up designs."""

import os
import glob

def add_openxc7_flow(platform, fabric_iddr=False):
    """Adapt the LiteX openxc7 toolchain of the platform to the design.

    fabric_iddr: IDDRs in SAME_EDGE_PIPELINED mode (AD9361 data interface) replaced by a capture in
    fabric flip-flops (openxc7_techmap/iddr_pipelined.v), for the archived nextpnr-xilinx (0.8.x),
    which does not configure this IDDR mode. The capture timing then depends on the placement
    (narrower AD9361 delay windows). openXC7/nextpnr (nextpnr-himbaechel, xilinx) configures it as
    Vivado does: no replacement.
    """
    from litex.build.yosys_wrapper import YosysWrapper
    toolchain = platform.toolchain
    gateware  = os.path.dirname(os.path.abspath(__file__))

    # Primitives nextpnr-xilinx does not take as Yosys or LiteX give them are replaced after the
    # synthesis (openxc7_techmap): the single-port LUT RAMs small memories are mapped to, and with
    # fabric_iddr the IDDR mode of the DDR inputs.
    template = list(YosysWrapper._default_template)
    for techmap in sorted(glob.glob(os.path.join(gateware, "openxc7_techmap", "*.v"))):
        if os.path.basename(techmap) == "iddr_pipelined.v" and not fabric_iddr:
            continue
        template.insert(len(template) - 1, f"techmap -map {techmap}")
    toolchain._yosys_template = template

    # The 8b/10b checker ROMs of USB3-PIPE in block RAMs (250MHz RX clock, see USB3): Yosys only
    # does it for such small ROMs with the attribute.
    toolchain._yosys_cmds += [
        "hierarchy -top {build_name}",
        "setattr -set rom_style \"block\" m:*rx_8b10b_checker_rom*",
    ]

    # Bits the design needs and the Project X-Ray database does not have (differential termination,
    # fabric reference clock of the GTP PLL) are set around fasm2frames: see openxc7_bitstream.py.
    fixup             = os.path.join(gateware, "openxc7_bitstream.py")
    bitstream_options = toolchain._build_openxc7_bitstream_options
    def bitstream_options_with_fixup(prjxray_db_arg_dir):
        bitstream_options(prjxray_db_arg_dir)
        if toolchain._pre_packer_cmd != ["fasm2frames"]:
            raise NotImplementedError("openXC7 bitstream fix-ups need the fasm2frames/xc7frames2bit flow.")
        db_root = os.path.join(prjxray_db_arg_dir, toolchain._xc7family)
        top     = toolchain._build_name
        fasm, frames = f"python3 {fixup} fasm", f"python3 {fixup} frames"
        toolchain._pre_packer_cmd = [fasm, "fasm2frames", frames]
        toolchain._pre_packer_opts[fasm] = (
            f"--xdc {top}.xdc --package-pins {db_root}/{toolchain.openxc7_part}/package_pins.csv "
            f"--notes {top}.fixup.json {top}.fasm")
        toolchain._pre_packer_opts[frames] = (
            f"--tilegrid {db_root}/{toolchain.openxc7_die}/tilegrid.json "
            f"--notes {top}.fixup.json {top}.frames")
    toolchain._build_openxc7_bitstream_options = bitstream_options_with_fixup
