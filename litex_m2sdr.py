#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import time
import argparse

from migen import *

from litex.gen import *
from litex.gen.genlib.cdc import BusSynchronizer

from litex.build.generic_platform import Subsignal, Pins
from litex_m2sdr_platform import Platform

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

from litex.soc.integration.soc_core import *
from litex.soc.integration.builder  import *

from litex.soc.cores.clock     import *
from litex.soc.cores.led       import LedChaser
from litex.soc.cores.icap      import ICAP
from litex.soc.cores.xadc      import XADC
from litex.soc.cores.dna       import DNA
from litex.soc.cores.gpio      import GPIOOut
from litex.soc.cores.spi_flash import S7SPIFlash

from litex.build.generic_platform import IOStandard, Subsignal, Pins

from litepcie.common        import *
from litepcie.phy.s7pciephy import S7PCIEPHY

from liteeth.common           import convert_ip
from liteeth.phy.a7_1000basex import A7_1000BASEX, A7_2500BASEX
from liteeth.frontend.stream  import LiteEthStream2UDPTX, LiteEthUDP2StreamRX

from litesata.phy import LiteSATAPHY

from litescope import LiteScopeAnalyzer

from litex_m2sdr.gateware.si5351      import SI5351
from litex_m2sdr.gateware.si5351_i2c  import SI5351I2C, i2c_program_si5351
from litex_m2sdr.gateware.ad9361.core import AD9361RFIC
from litex_m2sdr.gateware.qpll        import SharedQPLL
from litex_m2sdr.gateware.time        import TimeGenerator
from litex_m2sdr.gateware.pps         import PPSGenerator
from litex_m2sdr.gateware.header      import TXRXHeader
from litex_m2sdr.gateware.measurement import MultiClkMeasurement

from litex_m2sdr.software import generate_litepcie_software

# CRG ----------------------------------------------------------------------------------------------

class CRG(LiteXModule):
    def __init__(self, platform, sys_clk_freq, with_eth=False, with_sata=False):
        self.rst            = Signal()
        self.cd_sys         = ClockDomain()
        self.cd_clk10       = ClockDomain()
        self.cd_idelay      = ClockDomain()
        self.cd_refclk_pcie = ClockDomain()
        self.cd_refclk_eth  = ClockDomain()
        self.cd_refclk_sata = ClockDomain()

        # # #

        # Clk / Rst.
        clk100 = platform.request("clk100")

        # PLL.
        self.pll = pll = S7PLL(speedgrade=-3)
        self.comb += self.pll.reset.eq(self.rst)
        pll.register_clkin(clk100, 100e6)
        pll.create_clkout(self.cd_sys,    sys_clk_freq)
        pll.create_clkout(self.cd_clk10,  10e6)
        pll.create_clkout(self.cd_idelay, 200e6)

        # IDelayCtrl.
        self.idelayctrl = S7IDELAYCTRL(self.cd_idelay)

        # Ethernet PLL.
        if with_eth or with_sata:
            self.eth_pll = eth_pll = S7PLL()
            eth_pll.register_clkin(self.cd_sys.clk, sys_clk_freq)
            eth_pll.create_clkout(self.cd_refclk_eth, 156.25e6, margin=0)

        # SATA PLL.
        if with_sata or with_eth:
            self.sata_pll = sata_pll = S7PLL()
            sata_pll.register_clkin(self.cd_sys.clk, sys_clk_freq)
            sata_pll.create_clkout(self.cd_refclk_sata, 150e6, margin=0)

# BaseSoC ------------------------------------------------------------------------------------------

class BaseSoC(SoCMini):
    SoCCore.csr_map = {
        # SoC.
        "ctrl"            : 0,
        "uart"            : 1,
        "icap"            : 2,
        "flash_cs_n"      : 3,
        "xadc"            : 4,
        "dna"             : 5,
        "flash"           : 6,
        "leds"            : 7,
        "identifier_mem"  : 8,
        "timer0"          : 9,

        # PCIe.
        "pcie_phy"        : 10,
        "pcie_msi"        : 11,
        "pcie_dma0"       : 12,

        # Eth.
        "eth_phy"         : 14,
        "eth_rx_streamer" : 15,
        "eth_tx_streamer" : 16,

        # SATA.
        "sata_phy"        : 18,
        "sata_core"       : 19,

        # SDR.
        "si5351"          : 20,
        "time"            : 21,
        "header"          : 23,
        "ad9361"          : 24,
        "crossbar"        : 25,

        # Measurements/Analyzer.
        "clk_measurement" : 30,
        "analyzer"        : 31,
    }

    def __init__(self, variant="m2", sys_clk_freq=int(125e6),
        with_pcie     = True,  pcie_lanes=1,
        with_eth      = False, eth_sfp=0, eth_phy="1000basex", eth_local_ip="192.168.1.50", eth_udp_port=2345,
        with_sata     = False, sata_gen="gen1",
        with_jtagbone = True,
        with_rfic_oversampling = True,
    ):
        # Platform ---------------------------------------------------------------------------------

        platform = Platform(build_multiboot=True)
        if (with_eth or with_sata) and (variant != "baseboard"):
            msg = "Ethernet and SATA are only supported when mounted in the LiteX Acorn Baseboard Mini! "
            msg += "Available here: https://enjoy-digital-shop.myshopify.com/products/litex-acorn-baseboard-mini"
            raise ValueError(msg)

        # SoCMini ----------------------------------------------------------------------------------

        SoCMini.__init__(self, platform, sys_clk_freq,
            ident         = f"LiteX-M2SDR SoC / {variant} variant / built on",
            ident_version = True,
        )

        # Clocking ---------------------------------------------------------------------------------

        # General.
        self.crg = CRG(platform, sys_clk_freq,
            with_eth  = with_eth,
            with_sata = with_sata,
        )

        # Shared QPLL.
        self.qpll = SharedQPLL(platform,
            with_pcie = with_pcie,
            with_eth  = with_eth,
            eth_phy   = eth_phy,
            with_sata = with_sata,
        )

        # SI5351 Clock Generator -------------------------------------------------------------------

        self.si5351 = SI5351(platform, sys_clk_freq=sys_clk_freq, clk_in=platform.request("sync_clk_in"))
        si5351_clk0 = platform.request("si5351_clk0")
        si5351_clk1 = platform.request("si5351_clk1")

        # Time Generator ---------------------------------------------------------------------------

        self.time_gen = TimeGenerator(
            clk        = si5351_clk1,
            clk_freq   = 100e6,
            with_csr   = True,
        )

        # FIXME: Try to avoid CDC, change sys_clk?
        time_sys = Signal(64)
        self.time_sync = BusSynchronizer(
            width   = 64,
            idomain = "time",
            odomain = "sys",
        )
        self.comb += [
            self.time_sync.i.eq(self.time_gen.time),
            time_sys.eq(self.time_sync.o),
        ]

        # PPS Generator ----------------------------------------------------------------------------

        self.pps_gen = ClockDomainsRenamer("time")(PPSGenerator(
            clk_freq = 100e6,
            time     = self.time_gen.time,
            reset    = self.time_gen.time_change,
        ))
        # FIXME: Improve.
        pps_sys   = Signal()
        pps_sys_d = Signal()
        pps_rise  = Signal()
        self.specials += MultiReg(self.pps_gen.pps, pps_sys)
        self.sync += pps_sys_d.eq(pps_sys)
        self.comb += pps_rise.eq(pps_sys & ~pps_sys_d)

        # JTAGBone ---------------------------------------------------------------------------------

        if with_jtagbone:
            self.add_jtagbone()
            platform.add_period_constraint(self.jtagbone_phy.cd_jtag.clk, 1e9/20e6)

        # Leds -------------------------------------------------------------------------------------

        self.leds = LedChaser(
            pads         = platform.request_all("user_led"),
            sys_clk_freq = sys_clk_freq
        )

        # ICAP -------------------------------------------------------------------------------------

        self.icap = ICAP()
        self.icap.add_reload()
        self.icap.add_timing_constraints(platform, sys_clk_freq, self.crg.cd_sys.clk)

        # XADC -------------------------------------------------------------------------------------

        self.xadc = XADC()

        # DNA --------------------------------------------------------------------------------------

        self.dna = DNA()
        self.dna.add_timing_constraints(platform, sys_clk_freq, self.crg.cd_sys.clk)

        # SPI Flash --------------------------------------------------------------------------------

        self.flash_cs_n = GPIOOut(platform.request("flash_cs_n"))
        self.flash      = S7SPIFlash(platform.request("flash"), sys_clk_freq, 25e6)
        self.add_config("FLASH_IMAGE_SIZE", platform.image_size)

        # PCIe -------------------------------------------------------------------------------------

        if with_pcie:
            if variant == "baseboard":
                assert pcie_lanes == 1
            pcie_dmas = 1
            self.pcie_phy = S7PCIEPHY(platform, platform.request(f"pcie_x{pcie_lanes}_{variant}"),
                data_width  = {1: 64, 2: 64, 4: 128}[pcie_lanes],
                bar0_size   = 0x20000,
                cd          = "sys",
            )
            self.comb += ClockSignal("refclk_pcie").eq(self.pcie_phy.pcie_refclk)
            if variant == "baseboard":
                platform.toolchain.pre_placement_commands.append("reset_property LOC [get_cells -hierarchical -filter {{NAME=~pcie_s7/*gtp_channel.gtpe2_channel_i}}]")
                platform.toolchain.pre_placement_commands.append("set_property LOC GTPE2_CHANNEL_X0Y4 [get_cells -hierarchical -filter {{NAME=~pcie_s7/*gtp_channel.gtpe2_channel_i}}]")
            self.pcie_phy.update_config({
                "Base_Class_Menu"          : "Wireless_controller",
                "Sub_Class_Interface_Menu" : "RF_controller",
                "Class_Code_Base"          : "0D",
                "Class_Code_Sub"           : "10",
                }
            )
            self.add_pcie(phy=self.pcie_phy, address_width=64, ndmas=pcie_dmas, data_width=64,
                with_dma_buffering    = True, dma_buffering_depth=8192,
                with_dma_loopback     = True,
                with_dma_synchronizer = True,
                with_msi              = True
            )
            self.pcie_phy.use_external_qpll(qpll_channel=self.qpll.get_channel("pcie"))
            self.comb += self.pcie_dma0.synchronizer.pps.eq(pps_rise)

        # Ethernet ---------------------------------------------------------------------------------

        if with_eth:
            # PHY.
            eth_phy_cls = {
                "1000basex" : A7_1000BASEX,
                "2500basex" : A7_2500BASEX,
            }[eth_phy]
            self.eth_phy = eth_phy_cls(
                qpll_channel = self.qpll.get_channel("eth"),
                data_pads    = self.platform.request("sfp", eth_sfp),
                sys_clk_freq = sys_clk_freq,
                rx_polarity  = 1, # Inverted on M2SDR.
                tx_polarity  = 0, # Inverted on M2SDR and Acorn Baseboard Mini.
            )
            platform.add_period_constraint(self.eth_phy.txoutclk, 1e9/(self.eth_phy.tx_clk_freq/2))
            platform.add_period_constraint(self.eth_phy.rxoutclk, 1e9/(self.eth_phy.tx_clk_freq/2))

            # Core + MMAP (Etherbone).
            self.add_etherbone(phy=self.eth_phy, ip_address=eth_local_ip, data_width=32, arp_entries=4)

            # UDP Streamer.
            eth_streamer_port = self.ethcore_etherbone.udp.crossbar.get_port(eth_udp_port, dw=64, cd="sys")

            # RFIC -> UDP TX.
            self.eth_rx_streamer = LiteEthStream2UDPTX(
                udp_port   = eth_udp_port,
                fifo_depth = 1024//8,
                data_width = 64,
                with_csr   = True,
            )
            self.comb += self.eth_rx_streamer.source.connect(eth_streamer_port.sink)

            # UDP RX -> RFIC.
            self.eth_tx_streamer = LiteEthUDP2StreamRX(
                udp_port   = eth_udp_port,
                fifo_depth = 1024//8,
                data_width = 64,
                with_csr   = True,
            )
            self.comb += eth_streamer_port.source.connect(self.eth_tx_streamer.sink)

        # SATA -------------------------------------------------------------------------------------

        if with_sata:
            # IOs
            _sata_io = [
                ("sata", 0,
                    # Inverted on M2SDR.
                    Subsignal("tx_p",  Pins("D7")),
                    Subsignal("tx_n",  Pins("C7")),
                    # Inverted on M2SDR.
                    Subsignal("rx_p",  Pins("D9")),
                    Subsignal("rx_n",  Pins("C9")),
                ),
            ]
            platform.add_extension(_sata_io)

            # PHY
            self.sata_phy = LiteSATAPHY(platform.device,
                refclk     = ClockSignal("refclk_sata"),
                pads       = platform.request("sata"),
                gen        = sata_gen,
                clk_freq   = sys_clk_freq,
                data_width = 16,
                qpll       = self.qpll.get_channel("sata"),
            )
            sata_clk_freq = {"gen1": 75e6, "gen2": 150e6, "gen3": 300e6}[sata_gen]
            platform.add_period_constraint(self.sata_phy.crg.cd_sata_tx.clk, 1e9/sata_clk_freq)
            platform.add_period_constraint(self.sata_phy.crg.cd_sata_rx.clk, 1e9/sata_clk_freq)
            self.platform.add_false_path_constraints(
                self.crg.cd_sys.clk,
                self.sata_phy.crg.cd_sata_tx.clk,
                self.sata_phy.crg.cd_sata_rx.clk,
            )

            # Core
            self.add_sata(phy=self.sata_phy, mode="read+write")

        # AD9361 RFIC ------------------------------------------------------------------------------

        self.ad9361 = AD9361RFIC(
            rfic_pads    = platform.request("ad9361_rfic"),
            spi_pads     = platform.request("ad9361_spi"),
            sys_clk_freq = sys_clk_freq,
        )
        self.ad9361.add_prbs()
        self.ad9361.add_agc()
        rfic_clk_freq = {
            False : 245.76e6, # Max rfic_clk for  61.44MSPS / 2T2R.
            True  : 491.52e6, # Max rfic_clk for 122.88MSPS / 2T2R (Oversampling).
        }[with_rfic_oversampling]
        self.platform.add_period_constraint(self.ad9361.cd_rfic.clk, 1e9/rfic_clk_freq)

        # TX/RX Header Extracter/Inserter ----------------------------------------------------------

        self.header = TXRXHeader(data_width=64)
        self.comb += [
            self.header.rx.header.eq(0x5aa5_5aa5_5aa5_5aa5), # Unused for now, arbitrary.
            self.header.rx.timestamp.eq(time_sys),
        ]

        # TX/RX Datapath ---------------------------------------------------------------------------

        # AD9361 <-> Header.
        # ------------------
        self.comb += [
            self.header.tx.source.connect(self.ad9361.sink),
            self.ad9361.source.connect(self.header.rx.sink),
        ]

        # Crossbar.
        # ---------
        self.crossbar = stream.Crossbar(layout=dma_layout(64), n=3, with_csr=True)

        # TX: Comms -> Crossbar -> Header.
        # --------------------------------
        if with_pcie:
            self.comb += [
                self.pcie_dma0.source.connect(self.crossbar.mux.sink0),
                If(self.crossbar.mux.sel == 0,
                    self.header.tx.reset.eq(~self.pcie_dma0.synchronizer.synced)
                )
            ]
        if with_eth:
            self.comb += self.eth_tx_streamer.source.connect(self.crossbar.mux.sink1, omit={"error"})
        if with_sata:
            pass # TODO.
        self.comb += self.crossbar.mux.source.connect(self.header.tx.sink)

        # RX: Header -> Crossbar -> Comms.
        # --------------------------------
        self.comb += self.header.rx.source.connect(self.crossbar.demux.sink)
        if with_pcie:
            self.comb += [
                self.crossbar.demux.source0.connect(self.pcie_dma0.sink),
                If(self.crossbar.demux.sel == 0,
                    self.header.rx.reset.eq(~self.pcie_dma0.synchronizer.synced)
                )
            ]
        if with_eth:
            self.comb += self.crossbar.demux.source1.connect(self.eth_rx_streamer.sink)
        if with_sata:
            pass # TODO.

        # Timing Constraints/False Paths -----------------------------------------------------------

        platform.add_false_path_constraints(
            # PCIe.
            #"main_s7pciephy_clkout0", # FIXME.
            #"main_s7pciephy_clkout1", # FIXME.
            #"main_s7pciephy_clkout2", # FIXME.
            #"main_s7pciephy_clkout3", # FIXME.

            # CRG.
            "main_crg_clkout0",
            "main_crg_clkout1",

            # RFIC.
            "rfic_clk",

            # Internal Primitives.
            "dna_clk",
            "jtag_clk",
            "icap_clk",

            # Sync.
            "si5351_clk0",
            "si5351_clk1",
            "sync_clk_in",
        )

        # Clk Measurements -------------------------------------------------------------------------

        self.clk_measurement = MultiClkMeasurement(clks={
            "clk0" : ClockSignal("sys"),
            "clk1" : 0 if not with_pcie else ClockSignal("pcie"),
            "clk2" : si5351_clk0,
            "clk3" : ClockSignal("rfic"),
            "clk4" : si5351_clk1,
            "clk5" : ClockSignal("refclk_sata"),
        })

    # LiteScope Probes (Debug) ---------------------------------------------------------------------

    def add_ad9361_spi_probe(self):
        analyzer_signals = [self.platform.lookup_request("ad9361_spi")]
        self.analyzer = LiteScopeAnalyzer(analyzer_signals,
            depth        = 4096,
            clock_domain = "sys",
            register     = True,
            csr_csv      = "analyzer.csv"
        )

    def add_ad96361_data_probe(self):
        analyzer_signals = [
            self.ad9361.phy.sink,   # TX.
            self.ad9361.phy.source, # RX.
            self.ad9361.prbs_rx.fields.synced,
        ]
        self.analyzer = LiteScopeAnalyzer(analyzer_signals,
            depth        = 4096,
            clock_domain = "rfic",
            register     = True,
            csr_csv      = "analyzer.csv"
        )

    def add_pcie_dma_probe(self):
        assert hasattr(self, "pcie_dma0")
        analyzer_signals = [
            self.pps_gen.pps,      # PPS.
            self.pcie_dma0.sink,   # RX.
            self.pcie_dma0.source, # TX.
            self.pcie_dma0.synchronizer.synced,
            self.header.rx.reset,
            self.header.tx.reset,
        ]
        self.analyzer = LiteScopeAnalyzer(analyzer_signals,
            depth        = 1024,
            clock_domain = "sys",
            register     = True,
            csr_csv      = "analyzer.csv"
        )

    def add_eth_tx_probe(self):
        assert hasattr(self, "eth_streamer")
        analyzer_signals = [
            self.eth_streamer.sink,
        ]
        self.analyzer = LiteScopeAnalyzer(analyzer_signals,
            depth        = 1024,
            clock_domain = "sys",
            register     = True,
            csr_csv      = "analyzer.csv"
        )

# Build --------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="LiteX SoC on LiteX-M2SDR.", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    # Build/Load/Utilities.
    parser.add_argument("--variant",         default="m2",        help="Design variant.", choices=["m2", "baseboard"])
    parser.add_argument("--reset",           action="store_true", help="Reset the device.")
    parser.add_argument("--build",           action="store_true", help="Build bitstream.")
    parser.add_argument("--load",            action="store_true", help="Load bitstream.")
    parser.add_argument("--flash",           action="store_true", help="Flash bitstream.")
    parser.add_argument("--flash-multiboot", action="store_true", help="Flash multiboot bitstreams.")
    parser.add_argument("--rescan",          action="store_true", help="Execute PCIe Rescan while Loading/Flashing.")
    parser.add_argument("--driver",          action="store_true", help="Generate PCIe driver from LitePCIe (override local version).")

    # PCIe parameters.
    parser.add_argument("--with-pcie",       action="store_true", help="Enable PCIe Communication.")
    parser.add_argument("--pcie-lanes",      default=1, type=int, help="PCIe Lanes.", choices=[1, 2, 4])

    # Ethernet parameters.
    parser.add_argument("--with-eth",        action="store_true",     help="Enable Ethernet Communication.")
    parser.add_argument("--eth-sfp",         default=0, type=int,     help="Ethernet SFP.", choices=[0, 1])
    parser.add_argument("--eth-phy",         default="1000basex",     help="Ethernet PHY.", choices=["1000basex", "2500basex"])
    parser.add_argument("--eth-local-ip",    default="192.168.1.50",  help="Ethernet/Etherbone IP address.")
    parser.add_argument("--eth-udp-port",    default=2345, type=int,  help="Ethernet Remote port.")

    # SATA parameters.
    parser.add_argument("--with-sata",       action="store_true",     help="Enable SATA Storage.")

    # Litescope Analyzer Probes.
    probeopts = parser.add_mutually_exclusive_group()
    probeopts.add_argument("--with-ad9361-spi-probe",      action="store_true", help="Enable AD9361 SPI Probe.")
    probeopts.add_argument("--with-ad9361-data-probe",     action="store_true", help="Enable AD9361 Data Probe.")
    probeopts.add_argument("--with-pcie-dma-probe",        action="store_true", help="Enable PCIe DMA Probe.")
    probeopts.add_argument("--with-eth-tx-probe",          action="store_true", help="Enable Ethernet Tx Probe.")

    args = parser.parse_args()

    # Build SoC.
    soc = BaseSoC(
        # Generic.
        variant       = args.variant,

        # PCIe.
        with_pcie     = args.with_pcie,
        pcie_lanes    = args.pcie_lanes,

        # Ethernet.
        with_eth      = args.with_eth,
        eth_sfp       = args.eth_sfp,
        eth_phy       = args.eth_phy,
        eth_local_ip  = args.eth_local_ip,
        eth_udp_port  = args.eth_udp_port,

        # SATA.
        with_sata     = args.with_sata,
    )

    # LiteScope Analyzer Probes.
    if args.with_ad9361_spi_probe:
        soc.add_ad9361_spi_probe()
    if args.with_ad9361_data_probe:
        soc.add_ad96361_data_probe()
    if args.with_pcie_dma_probe:
        soc.add_pcie_dma_probe()
    if args.with_eth_tx_probe:
        soc.add_eth_tx_probe()

    # Builder.
    def get_build_name():
        r = f"litex_m2sdr_{args.variant}"
        if args.with_pcie:
            r += f"_pcie_x{args.pcie_lanes}"
        if args.with_eth:
            r += f"_eth"
        return r

    builder = Builder(soc, output_dir=os.path.join("build", get_build_name()), csr_csv="csr.csv")
    builder.build(build_name=get_build_name(), run=args.build)

    # Generate LitePCIe Driver.
    generate_litepcie_software(soc, "litex_m2sdr/software", use_litepcie_software=args.driver)

    # Reset Device.
    if args.reset:
        prog = soc.platform.create_programmer()
        prog.reset()
    
    # Load Bistream.
    if args.load:
        prog = soc.platform.create_programmer()
        prog.load_bitstream(os.path.join(builder.gateware_dir, soc.build_name + ".bit"))

    # Flash Bitstream.
    if args.flash:
        prog = soc.platform.create_programmer()
        prog.flash(0, os.path.join(builder.gateware_dir, soc.build_name + ".bin"))

    # Flash Multiboot Bitstreams.
    if args.flash_multiboot:
        prog = soc.platform.create_programmer()
        prog.flash(            0x0000_0000,  builder.get_bitstream_filename(mode="flash").replace(".bin", "_fallback.bin"),    verify=True)
        prog.flash(soc.platform.image_size,  builder.get_bitstream_filename(mode="flash").replace(".bin", "_operational.bin"), verify=True)

    # Rescan PCIe Bus.
    if args.rescan:
        subprocess.run("sudo sh -c 'cd litex_m2sdr/software && ./rescan.py'", shell=True)

if __name__ == "__main__":
    main()
