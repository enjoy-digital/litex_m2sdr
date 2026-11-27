#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""USB3 bring-up design for the LiteX-M2SDR baseboard: USB3 only.

The clocks, the GTP PLL and the USB3 SerDes of the SoC, with the bench SoC of USB3-PIPE (PHY, Device
Core example design, debug registers) and a JTAGBone bridge: no RFIC, no SI5351. For the
toolchains where the USB3 of the complete SoC does not work yet (openXC7), to validate it step by
step: SerDes, link training, enumeration, Bulk loopback.

    ./bench/usb3_bringup.py --usb3-pipe <usb3_pipe checkout> --sys-clk-freq 62.5e6 --build

--sys-clk-freq 125e6: PIPE/LTSSM/Core in sys; lower: PIPE on a 125MHz clock, LTSSM/Core in sys on
64-bit words. Host scripts: bench/usb3_*.py of USB3-PIPE, with litex_server --jtag.
"""

import os
import sys
import argparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

def usb3_pipe_bench_path(argv):
    path = os.environ.get("USB3_PIPE", "")
    for i, arg in enumerate(argv):
        if arg == "--usb3-pipe" and i + 1 < len(argv):
            path = argv[i + 1]
        elif arg.startswith("--usb3-pipe="):
            path = arg.split("=", 1)[1]
    if not path:
        import usb3_pipe
        path = os.path.dirname(os.path.dirname(os.path.abspath(usb3_pipe.__file__)))
    return os.path.join(os.path.expanduser(path), "bench")

sys.path.insert(0, usb3_pipe_bench_path(sys.argv))

import common # USB3-PIPE bench: its checkout on the path.

from migen import *

from litex.gen import *

from litex.soc.cores.clock import S7PLL

from usb3_pipe import A7USB3SerDes

from usb3_soc import USB3BaseSoC, add_usb3_device, usb3_analyzer
from usb3_soc import usb3_soc_args, usb3_soc_argdict, usb3_soc_build

from litex_m2sdr_platform import Platform, _io_baseboard

from litex_m2sdr.gateware.qpll         import SharedQPLL
from litex_m2sdr.gateware.usb3         import USB3QPLLChannel, get_usb3_sfp_pads
from litex_m2sdr.gateware.openxc7_flow import add_openxc7_flow

# CRG ----------------------------------------------------------------------------------------------

class _CRG(LiteXModule):
    def __init__(self, platform, sys_clk_freq, usb3_clk_freq=125e6):
        self.rst    = Signal()
        self.cd_sys = ClockDomain()
        self.cd_oob = ClockDomain()

        # # #

        self.pll = pll = S7PLL(speedgrade=-3)
        self.comb += pll.reset.eq(self.rst)
        pll.register_clkin(platform.request("clk100"), 100e6)
        pll.create_clkout(self.cd_sys, sys_clk_freq, margin=0)
        if sys_clk_freq != usb3_clk_freq:
            self.cd_usb3 = ClockDomain()
            pll.create_clkout(self.cd_usb3, usb3_clk_freq, margin=0)
        pll.create_clkout(self.cd_oob, usb3_clk_freq/8, margin=0)

# USB3SoC ------------------------------------------------------------------------------------------

class USB3SoC(USB3BaseSoC):
    def __init__(self, platform, sys_clk_freq=int(125e6), sfp=0, with_analyzer=False,
        analyzer_group="link", analyzer_csv="analyzer.csv", usb3_kwargs={}, **kwargs):
        usb3_clk_freq = 125e6
        phy_cd        = None if sys_clk_freq == usb3_clk_freq else "usb3"
        openxc7       = platform.toolchain_name != "vivado"

        # CRG --------------------------------------------------------------------------------------
        self.crg = _CRG(platform, sys_clk_freq)

        # USB3BaseSoC ------------------------------------------------------------------------------
        USB3BaseSoC.__init__(self, platform, sys_clk_freq, **kwargs)

        # GTP PLL (as the SoC) ---------------------------------------------------------------------
        self.qpll = SharedQPLL(platform,
            with_usb3      = True,
            usb3_refclk_cd = "sys" if phy_cd is None else phy_cd)

        # USB3 SerDes (as the SoC) -----------------------------------------------------------------
        tx_pads, rx_pads = get_usb3_sfp_pads(platform.request("sfp", sfp))
        usb3_serdes = A7USB3SerDes(platform,
            sys_clk      = self.crg.cd_sys.clk if phy_cd is None else self.crg.cd_usb3.clk,
            sys_clk_freq = usb3_clk_freq,
            refclk_pads  = None,
            refclk_freq  = usb3_clk_freq,
            tx_pads      = tx_pads,
            rx_pads      = rx_pads,
            pll          = USB3QPLLChannel(self.qpll.get_channel("usb3"), usb3_clk_freq),
            # Yosys/nextpnr-xilinx: 8b/10b checker lookups in block ROMs (250MHz RX clock).
            **(dict(rx_rom_style="block") if openxc7 else {}))
        if phy_cd is not None:
            usb3_serdes = ClockDomainsRenamer({"sys": phy_cd})(usb3_serdes)
            usb3_kwargs = dict(usb3_kwargs,
                phy_cd       = phy_cd,
                phy_clk_freq = usb3_clk_freq,
                link_words   = max(usb3_kwargs.get("link_words", 1), 2),
                with_profile = False)

        # USB3 (PHY, Debug, Core) ------------------------------------------------------------------
        add_usb3_device(self, usb3_serdes, **usb3_kwargs)

        # Analyzer ---------------------------------------------------------------------------------
        if with_analyzer:
            self.analyzer = usb3_analyzer(self, group=analyzer_group, csv=analyzer_csv)

        # openXC7 ----------------------------------------------------------------------------------
        if openxc7:
            # nextpnr-xilinx does not derive the clocks through the PLL.
            clocks = [(self.crg.cd_sys.clk, sys_clk_freq), (self.crg.cd_oob.clk, usb3_clk_freq/8)]
            if phy_cd is not None:
                clocks.append((self.crg.cd_usb3.clk, usb3_clk_freq))
            for clk, freq in clocks:
                platform.add_period_constraint(clk, 1e9/freq)
            add_openxc7_flow(platform)

# Build --------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="USB3 bring-up design for the LiteX-M2SDR baseboard.")
    parser.add_argument("--usb3-pipe",    default=None,              help="USB3-PIPE checkout (default: the installed one, or $USB3_PIPE).")
    parser.add_argument("--toolchain",    default="openxc7",         help="FPGA toolchain.", choices=["vivado", "openxc7"])
    parser.add_argument("--sys-clk-freq", default=62.5e6, type=float, help="System clock frequency (125e6: PIPE/Core in sys).")
    parser.add_argument("--sfp",          default=0, type=int,       help="Baseboard SFP.", choices=[0, 1])
    parser.add_argument("--seed",         default=1, type=int,       help="nextpnr seed.")
    usb3_soc_args(parser, output_dir="build/usb3_bringup", with_uartbone=False)
    args = parser.parse_args()

    soc_args = usb3_soc_argdict(args)
    platform = Platform(toolchain=args.toolchain)
    platform.add_extension(_io_baseboard)
    soc = USB3SoC(platform,
        sys_clk_freq   = int(args.sys_clk_freq),
        sfp            = args.sfp,
        with_uartbone  = False,
        with_jtagbone  = True,
        cpu_type       = soc_args["cpu_type"],
        with_analyzer  = soc_args["with_analyzer"],
        analyzer_group = soc_args["analyzer_group"],
        analyzer_csv   = soc_args["analyzer_csv"],
        usb3_kwargs    = soc_args["usb3_kwargs"],
    )
    usb3_soc_build(parser, args, soc, **(dict(seed=args.seed) if args.toolchain == "openxc7" else {}))

if __name__ == "__main__":
    main()
