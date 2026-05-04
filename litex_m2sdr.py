#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import sys
import argparse
import subprocess

from migen import *

from litex.gen import *
from litex.build.generic_platform import Subsignal, Pins

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

from litex.soc.integration.soc_core import *
from litex.soc.integration.builder  import *
from litex.soc.integration.soc      import SoCBusHandler
from litex.soc.integration.soc      import SoCRegion

from litex.soc.cores.clock     import *
from litex.soc.cores.icap      import ICAP
from litex.soc.cores.xadc      import XADC
from litex.soc.cores.dna       import DNA
from litex.soc.cores.gpio      import GPIOOut
from litex.soc.cores.spi_flash import S7SPIFlash

from litex.build.generic_platform import IOStandard

from litepcie.common            import *
from litepcie.phy.s7pciephy     import S7PCIEPHY
from litepcie.frontend.wishbone import LitePCIeWishboneSlave

from liteeth.phy.a7_1000basex import A7_1000BASEX, A7_2500BASEX
from liteeth.frontend.stream  import LiteEthStream2UDPTX, LiteEthUDP2StreamRX
from liteeth.core.ptp         import LiteEthPTP

from litesata.phy import LiteSATAPHY
from litesata.frontend.stream import LiteSATAStream2Sectors, LiteSATASectors2Stream

from litescope import LiteScopeAnalyzer

from litex_m2sdr import Platform, _io_baseboard
from litex_m2sdr.wr_helper import prepare_wr_environment

from litex_m2sdr.gateware.capability       import Capability
from litex_m2sdr.gateware.clock_discipline import MMCMPhaseDiscipline
from litex_m2sdr.gateware.si5351           import SI5351
from litex_m2sdr.gateware.ad9361.core import AD9361RFIC
from litex_m2sdr.gateware.qpll        import SharedQPLL
from litex_m2sdr.gateware.time        import TimeGenerator, TimeNsToPS
from litex_m2sdr.gateware.ptp_discipline import PTPTimeDiscipline, TimeDisciplineCDC
from litex_m2sdr.gateware.ptp_identity   import PTPIdentityTracker
from litex_m2sdr.gateware.pps         import PPSGenerator
from litex_m2sdr.gateware.pcie        import PCIeLinkResetWorkaround
from litex_m2sdr.gateware.header      import TXRXHeader
from litex_m2sdr.gateware.led         import StatusLed
from litex_m2sdr.gateware.measurement import MultiClkMeasurement
from litex_m2sdr.gateware.gpio        import GPIO
from litex_m2sdr.gateware.loopback    import TXRXLoopback
from litex_m2sdr.gateware.rfic        import RFICDataPacketizer
from litex_m2sdr.gateware.vrt         import VRTSignalPacketStreamer

from litex_m2sdr.software import generate_litepcie_software

# CRG ----------------------------------------------------------------------------------------------

class CRG(LiteXModule):
    def __init__(self, platform, sys_clk_freq, with_eth=False, eth_refclk_freq=125e6, with_sata=False, with_white_rabbit=False):
        self.rst              = Signal()
        self.cd_sys           = ClockDomain()
        self.cd_clk10         = ClockDomain()

        self.cd_clk100        = ClockDomain(reset_less=True)
        self.cd_clk200        = ClockDomain()
        self.cd_idelay        = ClockDomain()

        self.cd_refclk_pcie   = ClockDomain()
        self.cd_refclk_eth    = ClockDomain()
        self.cd_refclk_sata   = ClockDomain()

        self.cd_clk_125m_gtp  = ClockDomain()
        self.cd_clk_62m5_dmtd = ClockDomain()
        self.cd_clk10m_in     = ClockDomain()
        self.cd_clk62m5_in    = ClockDomain()

        # # #


        # Clk / Rst.
        # ----------
        clk100 = platform.request("clk100")

        # PLL.
        # ----
        self.pll = pll = S7PLL(speedgrade=-3)
        self.comb += self.pll.reset.eq(self.rst)
        pll.register_clkin(clk100, 100e6)
        pll.create_clkout(self.cd_sys,    sys_clk_freq)
        if not with_white_rabbit:
            # Keep clk10 on a dedicated MMCM so the SI5351C FPGA CLKIN path can be phase-steered later.
            self.clk10_mmcm = clk10_mmcm = S7MMCM(speedgrade=-3, fractional=False)
            self.comb += clk10_mmcm.reset.eq(self.rst)
            clk10_mmcm.register_clkin(clk100, 100e6)
            clk10_mmcm.expose_dps("clk200", with_csr=False)
            clk10_mmcm.create_clkout(self.cd_clk10, 10e6, margin=0)
            clk10_mmcm.params.update(p_CLKOUT0_USE_FINE_PS="TRUE")
        pll.create_clkout(self.cd_idelay, 200e6)
        self.comb += [
            self.cd_clk200.clk.eq(self.cd_idelay.clk),
            self.cd_clk200.rst.eq(self.cd_idelay.rst),
            self.cd_clk100.clk.eq(pll.clkin),
        ]
        # IDelayCtrl.
        # -----------
        self.idelayctrl = S7IDELAYCTRL(self.cd_idelay)

        # Ethernet PLL.
        # -------------
        if with_eth or with_sata or with_white_rabbit:
            self.eth_pll = eth_pll = S7PLL()
            eth_pll.register_clkin(self.cd_sys.clk, sys_clk_freq)
            eth_pll.create_clkout(self.cd_refclk_eth, eth_refclk_freq, margin=0)

        # SATA PLL.
        # ---------
        if with_sata or with_eth:
            self.sata_pll = sata_pll = S7PLL()
            sata_pll.register_clkin(self.cd_sys.clk, sys_clk_freq)
            sata_pll.create_clkout(self.cd_refclk_sata, 150e6, margin=0)

        # White Rabbit MMCMs.
        # -------------------
        if with_white_rabbit:
            # RefClk MMCM (125MHz).
            self.refclk_mmcm = S7MMCM(speedgrade=-3)
            self.comb += self.refclk_mmcm.reset.eq(self.rst)
            self.refclk_mmcm.register_clkin(ClockSignal("clk100"), 100e6)
            self.refclk_mmcm.expose_dps("clk200", with_csr=False)

            self.refclk_mmcm.create_clkout(self.cd_clk_125m_gtp,  125e6, margin=0)
            self.refclk_mmcm.params.update(p_CLKOUT0_USE_FINE_PS="TRUE")

            self.refclk_mmcm.create_clkout(self.cd_refclk_eth,  125e6, margin=0)
            self.refclk_mmcm.params.update(p_CLKOUT1_USE_FINE_PS="TRUE")

            # DMTD MMCM (62.5MHz).
            self.dmtd_mmcm = S7MMCM(speedgrade=-3)
            self.comb += self.dmtd_mmcm.reset.eq(self.rst)
            self.dmtd_mmcm.register_clkin(ClockSignal("clk100"), 100e6)
            self.dmtd_mmcm.create_clkout(self.cd_clk_62m5_dmtd, 62.5e6, margin=0)
            self.dmtd_mmcm.expose_dps("clk200", with_csr=False)
            self.dmtd_mmcm.params.update(p_CLKOUT0_USE_FINE_PS="TRUE")

# BaseSoC ------------------------------------------------------------------------------------------

class BaseSoC(SoCMini):
    SoCCore.csr_map = {
        # SoC.
        "ctrl"             :  0,
        "uart"             :  1,
        "icap"             :  2,
        "flash_cs_n"       :  3,
        "xadc"             :  4,
        "dna"              :  5,
        "flash"            :  6,
        "leds"             :  7,
        "identifier_mem"   :  8,
        "timer0"           :  9,

        # Capability.
        "capability"       : 13,

        # Time.
        "time_gen"         : 17,

        # PCIe.
        "pcie_phy"         : 10,
        "pcie_msi"         : 11,
        "pcie_dma0"        : 12,
        "ptm_requester"    : 34,

        # Eth.
        "eth_phy"          : 14,
        "eth_rx_streamer"  : 15,
        "eth_tx_streamer"  : 16,
        "eth_ptp"          : 37,
        "ptp_discipline"   : 38,
        "ptp_identity"     : 39,

        # SATA.
        "sata_phy"         : 18,
        "sata_core"        : 19,
        "sata_identify"    : 26,
        "sata_mem2sector"  : 27,
        "sata_sector2mem"  : 28,
        "sata_rx_streamer" : 29,
        "sata_tx_streamer" : 32,

        # GPIO.
        "gpio"             : 21,

        # SDR.
        "si5351"           : 20,
        "clk10_discipline" : 40,
        "header"           : 23,
        "ad9361"           : 24,
        "crossbar"         : 25,
        "txrx_loopback"    : 33,

        # Measurements/Analyzer.
        "clk_measurement"  : 30,
        "analyzer"         : 31,
        "eth_rx_mode"      : 35,
        "vrt_streamer"     : 36,
    }

    def __init__(self, variant="m2", sys_clk_freq=int(125e6),
        with_cpu               = False, cpu_variant="standard", integrated_rom_size=0x10000,
        with_pcie              = True,  with_pcie_ptm=False, pcie_gen=2, pcie_lanes=1, with_pcie_reset_workaround=False,
        with_eth               = False, eth_sfp=0, eth_phy="1000basex", eth_local_ip="192.168.1.50", eth_udp_port=2345,
        with_eth_ptp           = False, eth_ptp_p2p=False, eth_ptp_debug=False,
        with_eth_vrt           = False, vrt_dst_ip="239.168.1.100", vrt_dst_port=4991,
        with_sata              = False, sata_gen=2,
        with_white_rabbit      = False, wr_sfp=None, wr_dac_bits=16, wr_firmware=None,
        wr_nic_dir             = None,
        wr_ext_clk10_port      = None,  wr_ext_clk10_period=100.0, wr_ext_clk10_name="wr_ext_clk10",
        with_jtagbone          = True,
        with_gpio              = False,
        with_rfic_oversampling = False,
    ):
        # Platform ---------------------------------------------------------------------------------

        platform = Platform(build_multiboot=True)
        platform.rfic_clk_freq = {
            False : 245.76e6, # Max rfic_clk for  61.44MSPS / 2T2R.
            True  : 491.52e6, # Max rfic_clk for 122.88MSPS / 2T2R (Oversampling).
        }[with_rfic_oversampling]
        if variant == "baseboard":
            platform.add_extension(_io_baseboard)
        if (with_eth or with_sata) and (variant != "baseboard"):
            msg = "Ethernet and SATA are only supported when mounted in the LiteX Acorn Baseboard Mini! "
            msg += "Available here: https://enjoy-digital-shop.myshopify.com/products/litex-acorn-baseboard-mini"
            raise ValueError(msg)

        if with_white_rabbit and (variant != "baseboard"):
            raise ValueError("White Rabbit is only supported with --variant=baseboard (requires baseboard SFP resources).")
        if with_white_rabbit and (wr_sfp is None):
            raise ValueError("White Rabbit SFP must be resolved before BaseSoC initialization.")
        if with_eth_ptp and not with_eth:
            raise ValueError("Ethernet PTP requires --with-eth.")
        if with_eth_ptp and with_white_rabbit:
            raise ValueError("Ethernet PTP and White Rabbit cannot both own the board time generator in the same build yet.")

        # SoC Init ---------------------------------------------------------------------------------

        soc_kwargs = dict(
            ident=f"LiteX-M2SDR SoC / {variant} variant / built on",
            ident_version=True,
            csr_address_width=15,
        )
        if with_cpu:
            SoCCore.__init__(
                self,
                platform,
                sys_clk_freq,
                cpu_type="vexriscv",
                cpu_variant=cpu_variant,
                integrated_rom_size=integrated_rom_size,
                integrated_sram_size=0x10000,
                uart_name="crossover",
                bus_interconnect="crossbar",
                **soc_kwargs,
            )
        else:
            SoCMini.__init__(self, platform, sys_clk_freq, **soc_kwargs)

        # Clocking ---------------------------------------------------------------------------------

        eth_refclk_freq = 156.25e6 if (with_eth and eth_phy == "2500basex") else 125e6

        # General.
        self.crg = CRG(platform, sys_clk_freq,
            with_eth          = with_eth,
            eth_refclk_freq   = eth_refclk_freq,
            with_sata         = with_sata,
            with_white_rabbit = with_white_rabbit,
        )

        # Shared QPLL.
        self.qpll = SharedQPLL(platform,
            with_pcie       = with_pcie,
            with_eth        = with_eth | with_white_rabbit,
            eth_phy         = eth_phy,
            eth_refclk_freq = eth_refclk_freq,
            with_sata       = with_sata,
        )

        # Capability -------------------------------------------------------------------------------

        capability_wr_sfp = 0 if wr_sfp is None else wr_sfp
        self.capability = Capability(
            # API Version.
            api_version_str = "1.0",

            # PCIe Capabilities.
            pcie_enabled    = with_pcie,
            pcie_speed      = {1: "gen1", 2: "gen2"}[pcie_gen],
            pcie_lanes      = pcie_lanes,
            pcie_ptm        = with_pcie_ptm,

            # Ethernet Capabilities.
            eth_enabled     = with_eth,
            eth_speed       = eth_phy,
            eth_ptp         = with_eth_ptp,

            # SATA Capabilities.
            sata_enabled    = with_sata,
            sata_gen        = {1: "gen1", 2: "gen2", 3: "gen3"}[sata_gen],
            sata_mode       = "read+write",

            # GPIO Capabilities.
            gpio_enabled    = with_gpio,

            # White Rabbit Capabilities.
            wr_enabled      = with_white_rabbit,

            # Board.
            variant        = variant,
            jtagbone       = with_jtagbone,
            eth_sfp        = eth_sfp,
            wr_sfp         = capability_wr_sfp,
        )

        # SI5351 Clock Generator -------------------------------------------------------------------

        # SI5351 Control.
        si5351_pads   = platform.request("si5351")
        self.si5351 = SI5351(pads=si5351_pads, i2c_base=self.csr.address_map("si5351", origin=True))
        self.bus.add_master(name="si5351", master=self.si5351.sequencer.bus)

        # SI5351 ClkIn Ext/uFL.
        self.comb += self.si5351.clkin_ufl.eq(platform.request("sync_clk_in"))

        # SI5351 ClkIn/Out.
        si5351_clk0   = platform.request("si5351_clk0")
        si5351_clk1   = platform.request("si5351_clk1")

        # Clk10 Phase Discipline ----------------------------------------------------------------

        if not with_white_rabbit:
            self.clk10_discipline = MMCMPhaseDiscipline(sys_clk_freq=sys_clk_freq)
            self.comb += [
                self.crg.clk10_mmcm.psen.eq(self.clk10_discipline.psen),
                self.crg.clk10_mmcm.psincdec.eq(self.clk10_discipline.psincdec),
                self.clk10_discipline.psdone.eq(self.crg.clk10_mmcm.psdone),
            ]

        # Time Generator ---------------------------------------------------------------------------

        self.time_gen = TimeGenerator(
            clk      = si5351_clk1,
            clk_freq = 100e6,
            with_csr = True,
        )
        self.time_gen.add_cdc()

        # PPS Generator ----------------------------------------------------------------------------

        self.pps_gen = PPSGenerator(
            clk_freq = sys_clk_freq,
            time     = self.time_gen.time,
            reset    = self.time_gen.time_change,
        )
        self.time_s_vrt  = Signal(32)
        self.time_ps_vrt = Signal(64)
        self.time_ns_to_ps = TimeNsToPS(
            time_ns = self.time_gen.time,
            time_s  = self.time_s_vrt,
            time_ps = self.time_ps_vrt,
        )
        self.comb += self.time_s_vrt.eq(self.pps_gen.count)

        # JTAGBone ---------------------------------------------------------------------------------

        if with_jtagbone and not with_cpu:
            self.add_jtagbone()
        elif with_jtagbone and with_cpu:
            print("NOTE: JTAGBone disabled (conflicts with VexRiscv debug JTAG).")

        # ICAP -------------------------------------------------------------------------------------

        self.icap = ICAP()
        self.icap.add_reload()

        # XADC -------------------------------------------------------------------------------------

        self.xadc = XADC()

        # DNA --------------------------------------------------------------------------------------

        self.dna = DNA()

        # SPI Flash --------------------------------------------------------------------------------

        self.flash_cs_n = GPIOOut(platform.request("flash_cs_n"))
        self.flash      = S7SPIFlash(platform.request("flash"), sys_clk_freq, 25e6)
        self.add_config("FLASH_IMAGE_SIZE", platform.image_size)

        # PCIe -------------------------------------------------------------------------------------

        if with_pcie:
            # PHY.
            # ----
            if variant == "baseboard":
                assert pcie_lanes == 1
            pcie_dmas = 1
            self.pcie_phy = S7PCIEPHY(platform, platform.request(f"pcie_x{pcie_lanes}_{variant}"),
                data_width = {1: 64, 2: 64, 4: 128}[pcie_lanes],
                bar0_size  = 0x10_0000,
                with_ptm   = with_pcie_ptm,
                cd         = "sys",
            )
            self.comb += ClockSignal("refclk_pcie").eq(self.pcie_phy.pcie_refclk)
            if variant == "baseboard":
                self.pcie_phy.add_gt_loc_constraints(["GTPE2_CHANNEL_X0Y4"], by_pipe_lane=False)
            self.pcie_phy.update_config({
                "PCIe_Blk_Locn"            : "X0Y0",
                "Base_Class_Menu"          : "Wireless_controller",
                "Sub_Class_Interface_Menu" : "RF_controller",
                "Class_Code_Base"          : "0D",
                "Class_Code_Sub"           : "10",
                "Bar0_Scale"               : "Megabytes",
                "Bar0_Size"                : 1,
                "Link_Speed"               : {1: "2.5_GT/s", 2: "5.0_GT/s"}[pcie_gen],
                "Trgt_Link_Speed"          : {1: "4'h1",     2: "4'h2"}[pcie_gen],
                }
            )

            # Optional workaround for hosts requiring repeated link training attempts.
            if with_pcie_reset_workaround:
                self.pcie_link_reset_workaround = PCIeLinkResetWorkaround(
                    link_up     = self.pcie_phy._link_status.fields.status,
                    sys_clk_freq= sys_clk_freq,
                )
                self.comb += self.pcie_phy.pcie_rst_n.eq(self.pcie_link_reset_workaround.rst_n)

            # MSIs
            # ----
            pcie_msis = {}
            if with_sata:
                pcie_msis.update({
                    "SATA_SECTOR2MEM"  : Signal(),
                    "SATA_MEM2SECTOR"  : Signal(),
                    "SATA_STREAM2SECT" : Signal(),
                    "SATA_SECT2STREAM" : Signal(),
                })

            # Core.
            # -----
            self.add_pcie(phy=self.pcie_phy, address_width=64, ndmas=pcie_dmas, data_width=64,
                with_dma_buffering    = True, dma_buffering_depth=8192,
                with_dma_loopback     = True,
                with_dma_synchronizer = True,
                with_msi              = True, msis = pcie_msis,
                with_ptm              = with_pcie_ptm,
            )
            self.pcie_phy.use_external_qpll(qpll_channel=self.qpll.get_channel("pcie"))
            self.comb += self.pcie_dma0.synchronizer.pps.eq(self.pps_gen.pps_pulse)

            # Host <-> SoC DMA Bus.
            # ---------------------
            if with_sata:
                self.dma_bus = SoCBusHandler(
                    name          = "SoCDMABusHandler",
                    standard      = "wishbone",
                    data_width    = 32,
                    address_width = 32,
                    bursting      = False,
                )
                self.pcie_slave = LitePCIeWishboneSlave(self.pcie_endpoint,
                    address_width = 32,
                    data_width    = 32,
                    addressing    = "byte",
                )
                self.dma_bus.add_slave(name="dma",
                    slave  = self.pcie_slave.bus,
                    region = SoCRegion(origin=0x00000000, size=0x1_0000_0000)
                )

            # PTM.
            # ----
            if with_pcie_ptm:
                # TODO: Test phc2sys Host <-> Board regulation.
                # Checks.
                if pcie_lanes != 1:
                    raise NotImplementedError("PCIe PTM only supported in PCIe Gen2 X1 for now.")

                # Add PCIe PTM support.
                from litex_wr_nic.gateware.soc import LiteXWRNICSoC
                LiteXWRNICSoC.add_pcie_ptm(self)

                # Connect Time Gen's Time to PCIe PTM.
                self.comb += [
                    self.ptm_requester.time_clk.eq(ClockSignal("sys")),
                    self.ptm_requester.time_rst.eq(ResetSignal("sys")),
                    self.ptm_requester.time.eq(self.time_gen.time)
                ]

        # Ethernet ---------------------------------------------------------------------------------

        if with_eth:
            # PHY.
            # ----
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

            # Core + MMAP (Etherbone).
            # ------------------------
            eth_etherbone_kwargs = dict(
                phy        = self.eth_phy,
                ip_address = eth_local_ip,
                data_width = 32,
                arp_entries = 4,
            )
            if with_eth_ptp:
                ptp_igmp_groups = [0xE0000181, 0xE0000182]  # 224.0.1.129, 224.0.1.130.
                if eth_ptp_p2p:
                    ptp_igmp_groups.append(0xE000006B)     # 224.0.0.107.
                eth_etherbone_kwargs.update(
                    with_igmp     = True,
                    igmp_groups   = ptp_igmp_groups,
                    igmp_interval = 2,
                )
            self.add_etherbone(**eth_etherbone_kwargs)

            if with_eth_ptp:
                nominal_time_inc = int(round((1e9/100e6) * (1 << 24)))
                eth_ptp_clock_id = Cat(Constant(eth_sfp + 1, 16), self.dna._id.status)

                self.eth_ptp_event_port   = self.ethcore_etherbone.udp.crossbar.get_port(319, dw=8, cd="sys")
                self.eth_ptp_general_port = self.ethcore_etherbone.udp.crossbar.get_port(320, dw=8, cd="sys")
                self.eth_ptp = LiteEthPTP(
                    self.eth_ptp_event_port,
                    self.eth_ptp_general_port,
                    sys_clk_freq,
                    monitor_debug = eth_ptp_debug,
                )
                self.ptp_discipline = PTPTimeDiscipline(
                    sys_clk_freq      = sys_clk_freq,
                    nominal_time_inc  = nominal_time_inc,
                )
                self.ptp_discipline_cdc = TimeDisciplineCDC(self.time_gen)
                self.ptp_identity = PTPIdentityTracker()
                self.comb += [
                    self.eth_ptp.clock_id.eq(eth_ptp_clock_id),
                    self.eth_ptp.p2p_mode.eq(1 if eth_ptp_p2p else 0),
                    self.ptp_discipline.local_time.eq(self.time_gen.time),
                    self.ptp_discipline.ptp_seconds.eq(self.eth_ptp.tsu.seconds),
                    self.ptp_discipline.ptp_nanoseconds.eq(self.eth_ptp.tsu.nanoseconds),
                    self.ptp_discipline.ptp_locked.eq(self.eth_ptp.locked),
                    self.ptp_discipline_cdc.enable.eq(self.ptp_discipline.discipline_enable),
                    self.ptp_discipline_cdc.time_inc.eq(self.ptp_discipline.discipline_time_inc),
                    self.ptp_discipline_cdc.write.eq(self.ptp_discipline.discipline_write),
                    self.ptp_discipline_cdc.write_time.eq(self.ptp_discipline.discipline_write_time),
                    self.ptp_discipline_cdc.adjust.eq(self.ptp_discipline.discipline_adjust),
                    self.ptp_discipline_cdc.adjust_sign.eq(self.ptp_discipline.discipline_adjust_sign),
                    self.ptp_discipline_cdc.adjustment.eq(self.ptp_discipline.discipline_adjustment),
                    self.ptp_identity.clear.eq(self.ptp_discipline.clear_counters),
                    self.ptp_identity.local_port_id.eq(eth_ptp_clock_id),
                    self.ptp_identity.master_ip.eq(self.eth_ptp.master_ip),
                    self.ptp_identity.event_valid.eq(self.eth_ptp.rx_event.present),
                    self.ptp_identity.event_msg_type.eq(self.eth_ptp.rx_event.msg_type),
                    self.ptp_identity.event_ip.eq(self.eth_ptp_event_port.source.ip_address),
                    self.ptp_identity.event_source_port_id.eq(self.eth_ptp.rx_event.depacketizer.source.source_port_id),
                    self.ptp_identity.general_valid.eq(self.eth_ptp.rx_general.present),
                    self.ptp_identity.general_msg_type.eq(self.eth_ptp.rx_general.msg_type),
                    self.ptp_identity.general_ip.eq(self.eth_ptp_general_port.source.ip_address),
                    self.ptp_identity.general_source_port_id.eq(self.eth_ptp.rx_general.depacketizer.source.source_port_id),
                ]

            # UDP Streamer.
            # -------------
            eth_streamer_port = self.ethcore_etherbone.udp.crossbar.get_port(eth_udp_port, dw=64, cd="sys")

            # RFIC -> UDP TX.
            # ---------------
            self.eth_rx_streamer = LiteEthStream2UDPTX(
                udp_port   = eth_udp_port,
                fifo_depth = 1024//8,
                data_width = 64,
                with_csr   = True,
            )
            self.comb += self.eth_rx_streamer.source.connect(eth_streamer_port.sink)

            # UDP RX -> RFIC.
            # ---------------
            self.eth_tx_streamer = LiteEthUDP2StreamRX(
                udp_port   = eth_udp_port,
                fifo_depth = 1024//8,
                data_width = 64,
                with_csr   = True,
            )
            self.comb += eth_streamer_port.source.connect(self.eth_tx_streamer.sink)

            if with_eth_vrt:
                self.eth_rx_mode = CSRStorage(fields=[
                    CSRField("sel", size=2, offset=0, reset=1, values=[
                        ("``0b00``", "Disable/flush Ethernet RX output."),
                        ("``0b01``", "Route Ethernet RX to raw UDP streamer."),
                        ("``0b10``", "Route Ethernet RX to VRT UDP streamer."),
                    ])
                ])

                self.eth_rx_demux = stream.Demultiplexer(layout=dma_layout(64), n=3, with_csr=False)
                self.comb += self.eth_rx_demux.sel.eq(self.eth_rx_mode.fields.sel)
                self.comb += self.eth_rx_demux.source0.ready.eq(1)  # Flush path.

                self.vrt_rx_conv = stream.Converter(64, 32)
                self.vrt_rx_packetizer = RFICDataPacketizer(data_width=32, data_words=256)
                self.vrt_streamer = VRTSignalPacketStreamer(
                    udp_crossbar = self.ethcore_etherbone.udp.crossbar,
                    ip_address   = vrt_dst_ip,
                    udp_port     = vrt_dst_port,
                    data_width   = 32,
                    with_csr     = True,
                )
                self.comb += [
                    self.eth_rx_demux.source2.connect(self.vrt_rx_conv.sink, omit={"error"}),
                    self.vrt_rx_conv.source.connect(self.vrt_rx_packetizer.sink),
                    self.vrt_rx_packetizer.source.connect(self.vrt_streamer.sink),
                    self.vrt_streamer.sink.stream_id.eq(0xdeadbeef),
                    self.vrt_streamer.sink.timestamp_int.eq(self.time_s_vrt),
                    self.vrt_streamer.sink.timestamp_fra.eq(self.time_ps_vrt),
                ]

        # SATA -------------------------------------------------------------------------------------

        if with_sata:
            # PHY.
            # ----
            self.sata_phy = LiteSATAPHY(platform.device,
                refclk     = ClockSignal("refclk_sata"),
                pads       = platform.request("sata"),
                gen        = {1: "gen1", 2: "gen2", 3: "gen3"}[sata_gen],
                clk_freq   = sys_clk_freq,
                data_width = 16,
                qpll       = self.qpll.get_channel("sata"),
            )

            # Core.
            # -----
            self.add_sata(phy=self.sata_phy, mode="read+write", with_irq=False)
            if with_pcie:
                self.comb += [
                    pcie_msis["SATA_SECTOR2MEM"].eq(self.sata_sector2mem.irq),
                    pcie_msis["SATA_MEM2SECTOR"].eq(self.sata_mem2sector.irq),
                ]

            # Streamers.
            # ----------
            self.sata_rx_streamer = LiteSATAStream2Sectors(port=self.sata_crossbar.get_port())
            self.sata_tx_streamer = LiteSATASectors2Stream(port=self.sata_crossbar.get_port())

            # IRQs.
            # -----
            if with_pcie:
                self.comb += [
                    pcie_msis["SATA_STREAM2SECT"].eq(self.sata_rx_streamer.irq),
                    pcie_msis["SATA_SECT2STREAM"].eq(self.sata_tx_streamer.irq),
                ]

        # AD9361 RFIC ------------------------------------------------------------------------------

        self.ad9361 = AD9361RFIC(
            rfic_pads    = platform.request("ad9361_rfic"),
            spi_pads     = platform.request("ad9361_spi"),
            sys_clk_freq = sys_clk_freq,
        )
        self.ad9361.add_prbs()
        self.ad9361.add_agc()

        # TX/RX Header Extracter/Inserter ----------------------------------------------------------

        self.header = TXRXHeader(data_width=64)
        self.comb += [
            self.header.rx.header.eq(0x5aa5_5aa5_5aa5_5aa5), # Unused for now, arbitrary.
            self.header.rx.timestamp.eq(self.time_gen.time),
        ]

        # TX/RX Datapath ---------------------------------------------------------------------------

        # AD9361 <-> Loopback <-> Header.
        # -------------------------------
        self.txrx_loopback = TXRXLoopback(data_width=64, with_csr=True)

        # Header TX -> Loopback -> RFIC TX.
        self.comb += [
            self.header.tx.source.connect(self.txrx_loopback.tx_sink),
            self.txrx_loopback.tx_source.connect(self.ad9361.sink),
        ]

        # RFIC RX -> Loopback -> Header RX.
        self.comb += [
            self.ad9361.source.connect(self.txrx_loopback.rx_sink),
            self.txrx_loopback.rx_source.connect(self.header.rx.sink),
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
            self.comb += self.sata_tx_streamer.source.connect(self.crossbar.mux.sink2, omit={"error"})
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
            if with_eth_vrt:
                self.comb += [
                    self.crossbar.demux.source1.connect(self.eth_rx_demux.sink, omit={"error"}),
                    self.eth_rx_demux.source1.connect(self.eth_rx_streamer.sink),
                ]
            else:
                self.comb += self.crossbar.demux.source1.connect(self.eth_rx_streamer.sink)
        if with_sata:
            self.comb += self.crossbar.demux.source2.connect(self.sata_rx_streamer.sink, omit={"error"})

        # Leds -------------------------------------------------------------------------------------

        led_pad = platform.request("user_led")
        self.status_leds = StatusLed(sys_clk_freq=sys_clk_freq)

        led_tx_activity = Signal()
        led_rx_activity = Signal()
        self.comb += [
            led_tx_activity.eq((self.pcie_dma0.source.valid & self.pcie_dma0.source.ready if with_pcie else 0) |
                               (self.eth_tx_streamer.source.valid & self.eth_tx_streamer.source.ready if with_eth else 0) |
                               (self.sata_tx_streamer.source.valid & self.sata_tx_streamer.source.ready if with_sata else 0)),
            led_rx_activity.eq((self.pcie_dma0.sink.valid & self.pcie_dma0.sink.ready if with_pcie else 0) |
                               (self.eth_rx_streamer.sink.valid & self.eth_rx_streamer.sink.ready if with_eth else 0) |
                               (self.sata_rx_streamer.sink.valid & self.sata_rx_streamer.sink.ready if with_sata else 0)),
        ]

        self.comb += [
            self.status_leds.time_running.eq(self.time_gen.enable),
            self.status_leds.time_valid.eq(self.time_gen.time != 0),
            self.status_leds.pcie_present.eq(int(with_pcie)),
            self.status_leds.pcie_link_up.eq(
                self.pcie_phy._link_status.fields.status if with_pcie else 0
            ),
            self.status_leds.dma_synced.eq(
                self.pcie_dma0.synchronizer.synced if with_pcie else 0
            ),
            self.status_leds.eth_present.eq(int(with_eth)),
            self.status_leds.eth_link_up.eq(self.eth_phy.link_up if with_eth else 0),
            self.status_leds.tx_activity.eq(led_tx_activity),
            self.status_leds.rx_activity.eq(led_rx_activity),
            self.status_leds.pps_pulse.eq(self.pps_gen.pps_pulse),
            led_pad.eq(self.status_leds.output),
        ]

        # GPIO -------------------------------------------------------------------------------------

        if with_gpio:
            self.gpio = GPIO(
                rx_packer   = self.ad9361.gpio_rx_packer,
                tx_unpacker = self.ad9361.gpio_tx_unpacker,
            )
            self.gpio.connect_to_pads(pads=platform.request("gpios")) # TP1-2.

        # White Rabbit -----------------------------------------------------------------------------

        if with_white_rabbit:
            if wr_firmware is None:
                raise ValueError("White Rabbit enabled but no WR firmware provided. Use --wr-firmware or --wr-nic-dir.")
            wr_firmware = os.path.abspath(wr_firmware)

            # Ensure local/sibling litex_wr_nic checkout is importable in CI and local runs.
            if wr_nic_dir is not None:
                wr_import_paths = [os.path.abspath(wr_nic_dir), os.path.abspath(os.path.dirname(wr_nic_dir))]
                for path in wr_import_paths:
                    if path not in sys.path:
                        sys.path.insert(0, path)

            from litex.soc.cores.uart import UARTPHY, UART

            from litex_wr_nic.gateware.soc  import LiteXWRNICSoC
            from litex_wr_nic.gateware.ps_gen  import PSGen

            # IOs.
            # ----

            _sfp_i2c_io = [
                ("sfp_i2c", 0,
                    Subsignal("sda",  Pins("M2:SMB_DAT")),
                    Subsignal("scl",  Pins("M2:SMB_CLK")),
                    IOStandard("LVCMOS33"),
                ),
            ]
            platform.add_extension(_sfp_i2c_io)

            # UART.
            # -----

            class UARTPads:
                def __init__(self):
                    self.tx = Signal()
                    self.rx = Signal()

            self.uart_xover_pads = UARTPads()
            self.shared_pads     = UARTPads()
            self.uart_xover_phy  = UARTPHY(self.uart_xover_pads, clk_freq=sys_clk_freq, baudrate=115200)
            self.uart_xover      = UART(self.uart_xover_phy, rx_fifo_depth=128, rx_fifo_rx_we=True)

            self.comb += [
                self.uart_xover_pads.rx.eq(self.shared_pads.tx),
                self.shared_pads.rx.eq(self.uart_xover_pads.tx),
            ]

            # Core Instance.
            # --------------
            sfp_i2c_pads = platform.request("sfp_i2c")
            LiteXWRNICSoC.add_wr_core(self,
                # CPU.
                cpu_firmware    = wr_firmware,

                # Board name.
                board_name       = "SAWR",

                # Main/DMTD PLL.
                dac_bits = wr_dac_bits,

                # SFP.
                sfp_pads        = platform.request("sfp", wr_sfp),
                sfp_i2c_pads    = sfp_i2c_pads,
                sfp_tx_polarity = 0, # Inverted on M2SDR and Acorn Baseboard Mini.
                sfp_rx_polarity = 1, # Inverted on M2SDR.

                # QPLL.
                qpll            = self.qpll,
                with_ext_clk    = False,

                # Serial.
                serial_pads     = self.shared_pads,

                # Wishbone Slave.
                wb_slave_origin = 0x0004_0000,
                wb_slave_size   = 0x0004_0000
            )

            LiteXWRNICSoC.add_sources(self)

            # Clk10M Generator.
            # -----------------
            self.syncout_pll = syncout_pll = S7MMCM(speedgrade=-2)
            self.comb += syncout_pll.reset.eq(ResetSignal("wr"))
            syncout_pll.register_clkin(ClockSignal("wr"), 62.5e6)
            syncout_pll.create_clkout(self.crg.cd_clk10, 10e6, margin=0, phase=0)

            # RefClk MMCM Phase Shift.
            # ------------------------
            self.refclk_mmcm_ps_gen = PSGen(
                 cd_psclk    = "clk200",
                 cd_sys      = "wr",
                 ctrl_size   = wr_dac_bits,
                 )
            self.comb += [
                self.refclk_mmcm_ps_gen.ctrl_data.eq(self.dac_refclk_data),
                self.refclk_mmcm_ps_gen.ctrl_load.eq(self.dac_refclk_load),
                self.crg.refclk_mmcm.psen.eq(self.refclk_mmcm_ps_gen.psen),
                self.crg.refclk_mmcm.psincdec.eq(self.refclk_mmcm_ps_gen.psincdec),
            ]

            # DMTD MMCM Phase Shift.
            # ----------------------
            self.dmtd_mmcm_ps_gen = PSGen(
                 cd_psclk    = "clk200",
                 cd_sys      = "wr",
                 ctrl_size   = wr_dac_bits,
                 )
            self.comb += [
                self.dmtd_mmcm_ps_gen.ctrl_data.eq(self.dac_dmtd_data),
                self.dmtd_mmcm_ps_gen.ctrl_load.eq(self.dac_dmtd_load),
                self.crg.dmtd_mmcm.psen.eq(self.dmtd_mmcm_ps_gen.psen),
                self.crg.dmtd_mmcm.psincdec.eq(self.dmtd_mmcm_ps_gen.psincdec),
            ]

            # Timings Constraints.
            # --------------------
            platform.add_platform_command("set_property SEVERITY {{Warning}} [get_drc_checks REQP-123]")
            if wr_ext_clk10_port is not None:
                platform.add_platform_command(
                    f"create_clock -name {wr_ext_clk10_name} -period {wr_ext_clk10_period:.3f} [get_ports {wr_ext_clk10_port}]"
                )
            platform.add_platform_command("create_clock -name wr_txoutclk -period 16.000 [get_pins -hierarchical *gtpe2_i/TXOUTCLK]")
            platform.add_platform_command("create_clock -name wr_rxoutclk -period 16.000 [get_pins -hierarchical *gtpe2_i/RXOUTCLK]")
            platform.add_false_path_constraints(
                "wr_rxoutclk",
                "wr_txoutclk",
                "{{*crg_s7mmcm0_clkout}}",
                "{{*crg_s7mmcm1_clkout}}",
            )

        # Timing Constraints -----------------------------------------------------------------------
        # Explicit root/board clock constraints are handled in Platform.do_finalize().

        def add_guarded_async_clock_groups(clk0, clk1):
            clk0 = "{{" + clk0 + "}}"
            clk1 = "{{" + clk1 + "}}"
            platform.toolchain.pre_placement_commands += [
                f"set _clk0 [get_clocks -quiet {clk0}]",
                f"set _clk1 [get_clocks -quiet {clk1}]",
                "if {{[llength $_clk0] && [llength $_clk1]}} {{",
                "    set_clock_groups -asynchronous -group $_clk0 -group $_clk1",
                "}}",
            ]

        def add_guarded_false_path(clk0, clk1):
            clk0 = "{{" + clk0 + "}}"
            clk1 = "{{" + clk1 + "}}"
            platform.toolchain.pre_placement_commands += [
                f"set _clk0 [get_clocks -quiet {clk0}]",
                f"set _clk1 [get_clocks -quiet {clk1}]",
                "if {{[llength $_clk0] && [llength $_clk1]}} {{",
                "    set_false_path -from $_clk0 -to $_clk1",
                "}}",
            ]

        # JTAG TCK and Async Crossing to sys.
        if with_jtagbone and hasattr(self, "jtagbone"):
            platform.add_period_constraint(self.jtagbone.phy.cd_jtag.clk, 1e9/20e6)
            add_guarded_async_clock_groups("*crg*clkout0*", "jtag_clk")

        # Async Crossings to External RF/PPS Clocks (CDC-only paths).
        for ext_clk in ["clk100", "si5351_clk0", "si5351_clk1", "sync_clk_in", "rfic_clk", "ad9361_rfic_rx_clk_p"]:
            add_guarded_async_clock_groups("*crg*clkout0*", ext_clk)

        # External Async Inputs (CDC/UART/reset/status paths only).
        platform.add_platform_command(
            "set_false_path -from [get_ports -quiet {{"
            "ad9361_rfic_stat* pcie_x1_baseboard_rst_n pcie_x1_m2_rst_n sync_clk_in"
            "}}]"
        )

        # Low-Speed Peripheral Return Inputs (SPI MISO/status, board timing intentionally not modeled).
        platform.add_platform_command(
            "set_false_path -from [get_ports -quiet {{"
            "ad9361_spi_miso flash_miso"
            "}}]"
        )

        # SI5351 10MHz clock select only feeds the external ssen_clkin DDR output, so a
        # non-dedicated BUFG cascade route is acceptable here.
        platform.add_platform_command("set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets -quiet clk10_clk]")

        # Low-Speed Peripheral Control Outputs (registered/static outputs, no external timing budget modeled).
        platform.add_platform_command(
            "set_false_path -to [get_ports -quiet {{"
            "ad9361_rfic_ctrl* ad9361_rfic_en_agc ad9361_rfic_enable ad9361_rfic_rst_n ad9361_rfic_txnrx "
            "ad9361_spi_clk ad9361_spi_cs_n ad9361_spi_mosi "
            "flash_cs_n flash_mosi "
            "si5351_scl si5351_sda si5351_pwm"
            "}}]"
        )

        # ICAP / DNA utility domains (generated from sys_clk).
        self.icap.add_timing_constraints(platform, sys_clk_freq, self.crg.cd_sys.clk)
        self.dna.add_timing_constraints(platform, sys_clk_freq, self.crg.cd_sys.clk)

        # PCIe: keep CRG <-> PCIe pclk asynchronous and ignore 125/250MHz mux alternatives.
        if with_pcie:
            false_paths = [
                ("*crg*clkout0*",  "*s7pciephy_clkout3*"),
                ("*s7pciephy_clkout0*", "*s7pciephy_clkout1*"),
            ]
            for clk0, clk1 in false_paths:
                add_guarded_false_path(clk0, clk1)
                add_guarded_false_path(clk1, clk0)

        # Ethernet transceiver clocks.
        if with_eth:
            platform.add_period_constraint(self.eth_phy.txoutclk, 1e9/(self.eth_phy.tx_clk_freq/2))
            platform.add_period_constraint(self.eth_phy.rxoutclk, 1e9/(self.eth_phy.tx_clk_freq/2))
            platform.add_false_path_constraints(self.eth_phy.txoutclk, self.eth_phy.rxoutclk, self.crg.cd_sys.clk)

        # RFIC clock domain.
        platform.add_period_constraint(self.ad9361.cd_rfic.clk, 1e9/platform.rfic_clk_freq)

        # Clk Measurements -------------------------------------------------------------------------

        self.clk_measurement = MultiClkMeasurement(clks={
            "clk0" : ClockSignal("sys"),
            "clk1" : 0 if not with_pcie else ClockSignal("pcie"),
            "clk2" : si5351_clk0,
            "clk3" : ClockSignal("rfic"),
            "clk4" : si5351_clk1,
        })

    # LiteScope Probes (Debug) ---------------------------------------------------------------------

    # Integrated ROM.
    def add_rom_bus_probe(self, depth=512):
        assert hasattr(self, "rom")
        assert hasattr(self, "cpu")
        assert hasattr(self, "etherbone")

        rom_region       = self.bus.regions["rom"]
        bus_word_bytes   = self.bus.data_width//8
        rom_origin_words = rom_region.origin//bus_word_bytes
        rom_end_words    = rom_origin_words + rom_region.size//bus_word_bytes

        eth_wb  = self.etherbone.wishbone.bus
        rom_wb  = self.rom.bus
        ibus_wb = self.cpu.ibus
        dbus_wb = self.cpu.dbus

        self.rom_probe_sys_reset       = Signal()
        self.rom_probe_eth_rom_access  = Signal()
        self.rom_probe_ibus_rom_access = Signal()
        self.rom_probe_dbus_rom_access = Signal()
        self.rom_probe_rom_read        = Signal()
        self.rom_probe_rom_ack         = Signal()
        self.comb += [
            self.rom_probe_sys_reset.eq(ResetSignal("sys")),
            self.rom_probe_eth_rom_access.eq(
                eth_wb.cyc & eth_wb.stb &
                (eth_wb.adr >= rom_origin_words) &
                (eth_wb.adr <  rom_end_words)
            ),
            self.rom_probe_ibus_rom_access.eq(
                ibus_wb.cyc & ibus_wb.stb &
                (ibus_wb.adr >= rom_origin_words) &
                (ibus_wb.adr <  rom_end_words)
            ),
            self.rom_probe_dbus_rom_access.eq(
                dbus_wb.cyc & dbus_wb.stb &
                (dbus_wb.adr >= rom_origin_words) &
                (dbus_wb.adr <  rom_end_words)
            ),
            self.rom_probe_rom_read.eq(rom_wb.cyc & rom_wb.stb & ~rom_wb.we),
            self.rom_probe_rom_ack.eq(rom_wb.ack),
        ]

        analyzer_signals = [
            # Summary flags / status.
            self.rom_probe_sys_reset,
            self.cpu.reset,
            self.ctrl.cpu_rst,
            self.ctrl.bus_error,
            self.rom_probe_eth_rom_access,
            self.rom_probe_ibus_rom_access,
            self.rom_probe_dbus_rom_access,
            self.rom_probe_rom_read,
            self.rom_probe_rom_ack,

            # Wishbone requesters and integrated ROM slave port.
            eth_wb,
            ibus_wb,
            dbus_wb,
            rom_wb,
        ]
        self.analyzer = LiteScopeAnalyzer(analyzer_signals,
            depth        = depth,
            clock_domain = "sys",
            register     = True,
            csr_csv      = "test/analyzer.csv"
        )

    # PCIe.
    def add_pcie_probe(self, depth=4096):
        self.pcie_phy.add_ltssm_tracer()
        self.pcie_clk_count = Signal(16)
        self.sync.pclk += self.pcie_clk_count.eq(self.pcie_clk_count + 1)
        analyzer_signals = [
            # Rst.
            self.pcie_phy.pcie_rst_n,

            # Clk.
            self.pcie_phy.pcie_refclk,
            self.pcie_clk_count,

            # Link Status.
            self.pcie_phy._link_status.fields.rate,
            self.pcie_phy._link_status.fields.width,
            self.pcie_phy._link_status.fields.ltssm
        ]
        self.analyzer = LiteScopeAnalyzer(analyzer_signals,
            depth        = depth,
            clock_domain = "sys",
            register     = True,
            csr_csv      = "test/analyzer.csv"
        )

    def add_pcie_slave_probe(self, depth=4096):
        assert hasattr(self, "pcie_slave")
        analyzer_signals = [
            self.pcie_slave.bus,
        ]
        self.analyzer = LiteScopeAnalyzer(analyzer_signals,
            depth        = depth,
            clock_domain = "sys",
            register     = True,
            csr_csv      = "test/analyzer.csv"
        )

    def add_pcie_dma_probe(self, depth=1024):
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
            depth        = depth,
            clock_domain = "sys",
            register     = True,
            csr_csv      = "test/analyzer.csv"
        )

    # Clocking.
    def add_si5351_i2c_probe(self, depth=4096):
        analyzer_signals = [
            # I2C SCL.
            self.si5351.i2c.phy.clkgen.scl_o,
            self.si5351.i2c.phy.clkgen.scl_oe,

            # I2C SDA.
            self.si5351.i2c.phy.sda_o,
            self.si5351.i2c.phy.sda_oe,
            self.si5351.i2c.phy.sda_i,

            # I2C Master.
            self.si5351.i2c.master.source,
            self.si5351.i2c.master.sink,

            # I2C Sequencer.
            self.si5351.sequencer.fsm,
            self.si5351.sequencer.done,
            self.si5351.sequencer.bus,
        ]
        self.analyzer = LiteScopeAnalyzer(analyzer_signals,
            depth        = depth,
            clock_domain = "sys",
            register     = True,
            csr_csv      = "test/analyzer.csv"
        )

    # Ethernet.
    def add_eth_tx_probe(self, depth=1024):
        assert hasattr(self, "eth_tx_streamer")
        analyzer_signals = [
            self.eth_tx_streamer.sink,
        ]
        self.analyzer = LiteScopeAnalyzer(analyzer_signals,
            depth        = depth,
            clock_domain = "sys",
            register     = True,
            csr_csv      = "test/analyzer.csv"
        )

    # RFIC.
    def add_ad9361_spi_probe(self, depth=4096):
        analyzer_signals = [self.platform.lookup_request("ad9361_spi")]
        self.analyzer = LiteScopeAnalyzer(analyzer_signals,
            depth        = depth,
            clock_domain = "sys",
            register     = True,
            csr_csv      = "test/analyzer.csv"
        )

    def add_ad9361_data_probe(self, depth=4096):
        analyzer_signals = [
            self.ad9361.phy.sink,   # TX.
            self.ad9361.phy.source, # RX.
            self.ad9361.prbs_rx.fields.synced,
        ]
        self.analyzer = LiteScopeAnalyzer(analyzer_signals,
            depth        = depth,
            clock_domain = "rfic",
            register     = True,
            csr_csv      = "test/analyzer.csv"
        )

# Build --------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="LiteX SoC on LiteX-M2SDR.", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    # Build/Load/Utilities.
    parser.add_argument("--variant",          default="m2",              help="Design variant.", choices=["m2", "baseboard"])
    parser.add_argument("--sys-clk-freq",     default=125e6, type=float, help="System clock frequency in Hz.")
    parser.add_argument("--reset",            action="store_true",       help="Reset the device.")
    parser.add_argument("--build",            action="store_true",       help="Build bitstream.")
    parser.add_argument("--load",             action="store_true",       help="Load bitstream.")
    parser.add_argument("--flash",            action="store_true",       help="Flash bitstream.")
    parser.add_argument("--flash-multiboot",  action="store_true",       help="Flash multiboot bitstreams.")
    parser.add_argument("--rescan",           action="store_true",       help="Execute PCIe Rescan while Loading/Flashing.")
    parser.add_argument("--driver",           action="store_true",       help="Generate PCIe driver from LitePCIe (override local version).")
    parser.add_argument("--without-jtagbone", action="store_true",       help="Disable JTAGBone support.")
    parser.add_argument(
        "--with-cpu",
        action="store_true",
        help="Integrate VexRiscv soft CPU with BIOS + crossover console.",
    )
    parser.add_argument(
        "--cpu-variant",
        default="standard",
        choices=["minimal", "lite", "lite+debug", "standard", "full"],
        help="VexRiscv CPU variant.",
    )
    parser.add_argument("--integrated-rom-size", default=0x10000, type=lambda x: int(x, 0), help="Integrated ROM size used when --with-cpu is enabled.")
    parser.add_argument("--no-integrated-rom-auto-size", action="store_true", help="Keep integrated ROM at the size declared in SoC construction instead of shrinking it to the BIOS binary size.")

    # RFIC parameters.
    parser.add_argument("--with-rfic-oversampling", action="store_true", help="Double the RFIC clock to enable the oversampling mode.")

    # PCIe parameters.
    parser.add_argument("--with-pcie",       action="store_true", help="Enable PCIe Communication.")
    parser.add_argument("--with-pcie-ptm",   action="store_true", help="Enable PCIe PTM.")
    parser.add_argument("--with-pcie-reset-workaround", action="store_true", help="Toggle PCIe reset periodically until link-up.")
    parser.add_argument("--pcie-gen",        default=2, type=int, help="PCIe Generation.", choices=[1, 2])
    parser.add_argument("--pcie-lanes",      default=1, type=int, help="PCIe Lanes.", choices=[1, 2, 4])

    # Ethernet parameters.
    parser.add_argument("--with-eth",        action="store_true",     help="Enable Ethernet Communication.")
    parser.add_argument("--eth-sfp",         default=0, type=int,     help="Ethernet SFP.", choices=[0, 1])
    parser.add_argument("--eth-phy",         default="1000basex",     help="Ethernet PHY.", choices=["1000basex", "2500basex"])
    parser.add_argument("--eth-local-ip",    default="192.168.1.50",  help="Ethernet/Etherbone IP address.")
    parser.add_argument("--eth-udp-port",    default=2345, type=int,  help="Ethernet Remote port.")
    parser.add_argument("--with-eth-ptp",    action="store_true",     help="Enable LiteEth PTP and discipline the board time generator from it.")
    parser.add_argument("--eth-ptp-p2p",     action="store_true",     help="Use PTP peer-to-peer delay mode instead of end-to-end.")
    parser.add_argument("--eth-ptp-debug",   action="store_true",     help="Expose the verbose LiteEth PTP monitor CSRs.")
    parser.add_argument("--with-eth-vrt",    action="store_true",     help="Enable Ethernet RX VRT UDP streamer path.")
    parser.add_argument("--vrt-dst-ip",      default="239.168.1.100", help="VRT destination IP address (when --with-eth-vrt).")
    parser.add_argument("--vrt-dst-port",    default=4991, type=int,  help="VRT destination UDP port (when --with-eth-vrt).")

    # SATA parameters.
    parser.add_argument("--with-sata",       action="store_true", help="Enable SATA Storage.")
    parser.add_argument("--sata-gen",        default=2, type=int, help="SATA Generation.", choices=[1, 2, 3])

    # GPIO parameters.
    parser.add_argument("--with-gpio",       action="store_true",     help="Enable GPIO support.")

    # White Rabbit parameters.
    parser.add_argument("--with-white-rabbit",   action="store_true",                    help="Enable White-Rabbit Support.")
    parser.add_argument("--wr-sfp",              default=None, type=int,                 help="White Rabbit SFP (default: auto-select first available).", choices=[0, 1])
    parser.add_argument("--wr-dac-bits",         default=16, type=int,                   help="White Rabbit MMCM phase-shift control word width (in bits).")
    parser.add_argument("--wr-nic-dir",          default=os.environ.get("LITEX_WR_NIC_DIR"), help="Path to litex_wr_nic checkout (or set LITEX_WR_NIC_DIR).")
    parser.add_argument("--wr-firmware",         default=None,                           help="Path to WR firmware BRAM image (e.g. .../firmware/spec_a7_wrc.bram).")
    parser.add_argument("--wr-firmware-target",  default="acorn",                        help="WR firmware build target passed to build.py (when --build).")
    parser.add_argument("--wr-status",           action="store_true",                    help="Print resolved WR environment status.")
    parser.add_argument("--wr-ext-clk10-port",   default=None,                           help="Vivado port for external 10MHz clock constraint (e.g. clk10m_in).")
    parser.add_argument("--wr-ext-clk10-period", default=100.0, type=float,              help="External 10MHz clock period in ns for constraint.")
    parser.add_argument("--wr-ext-clk10-name",   default="wr_ext_clk10",                 help="External 10MHz clock name for constraint.")

    # Litescope Analyzer Probes.
    probeopts = parser.add_mutually_exclusive_group()
    probeopts.add_argument("--with-pcie-probe",        action="store_true", help="Enable PCIe Probe.")
    probeopts.add_argument("--with-pcie-dma-probe",    action="store_true", help="Enable PCIe DMA Probe.")
    probeopts.add_argument("--with-si5351-i2c-probe",  action="store_true", help="Enable SI5351 I2C Probe.")
    probeopts.add_argument("--with-eth-tx-probe",      action="store_true", help="Enable Ethernet Tx Probe.")
    probeopts.add_argument("--with-ad9361-spi-probe",  action="store_true", help="Enable AD9361 SPI Probe.")
    probeopts.add_argument("--with-ad9361-data-probe", action="store_true", help="Enable AD9361 Data Probe.")
    probeopts.add_argument("--with-rom-bus-probe",     action="store_true", help="Enable integrated ROM Wishbone Probe.")
    parser.add_argument("--rom-bus-probe-depth", default=512, type=int, help="Integrated ROM Wishbone Probe capture depth.")

    args = parser.parse_args()

    this_dir = os.path.dirname(os.path.abspath(__file__))

    wr_env = prepare_wr_environment(
        root_dir          = this_dir,
        variant           = args.variant,
        baseboard_io      = _io_baseboard,
        with_white_rabbit = args.with_white_rabbit,
        wr_sfp            = args.wr_sfp,
        wr_nic_dir        = args.wr_nic_dir,
        wr_firmware       = args.wr_firmware,
        wr_firmware_target= args.wr_firmware_target,
        build             = args.build,
        status            = args.wr_status,
    )
    wr_firmware = wr_env["wr_firmware"]
    wr_sfp      = wr_env["wr_sfp"]

    if args.wr_status and not args.with_white_rabbit:
        return

    # Build SoC.
    soc = BaseSoC(
        # Generic.
        variant       = args.variant,
        sys_clk_freq  = int(args.sys_clk_freq),

        # CPU.
        with_cpu            = args.with_cpu,
        cpu_variant         = args.cpu_variant,
        integrated_rom_size = args.integrated_rom_size,

        # RFIC.
        with_rfic_oversampling = args.with_rfic_oversampling,

        # PCIe.
        with_pcie     = args.with_pcie,
        with_pcie_ptm = args.with_pcie_ptm,
        with_pcie_reset_workaround = args.with_pcie_reset_workaround,
        pcie_gen      = args.pcie_gen,
        pcie_lanes    = args.pcie_lanes,

        # Ethernet.
        with_eth      = args.with_eth,
        eth_sfp       = args.eth_sfp,
        eth_phy       = args.eth_phy,
        eth_local_ip  = args.eth_local_ip,
        eth_udp_port  = args.eth_udp_port,
        with_eth_ptp  = args.with_eth_ptp,
        eth_ptp_p2p   = args.eth_ptp_p2p,
        eth_ptp_debug = args.eth_ptp_debug,
        with_eth_vrt  = args.with_eth_vrt,
        vrt_dst_ip    = args.vrt_dst_ip,
        vrt_dst_port  = args.vrt_dst_port,

        # SATA.
        with_sata     = args.with_sata,
        sata_gen      = args.sata_gen,

        # GPIOs.
        with_gpio     = args.with_gpio,
        with_jtagbone = not args.without_jtagbone,

        # White Rabbit.
        with_white_rabbit = args.with_white_rabbit,
        wr_sfp            = wr_sfp,
        wr_dac_bits       = args.wr_dac_bits,
        wr_firmware       = wr_firmware,
        wr_nic_dir        = wr_env["wr_nic_dir"],
        wr_ext_clk10_port   = args.wr_ext_clk10_port,
        wr_ext_clk10_period = args.wr_ext_clk10_period,
        wr_ext_clk10_name   = args.wr_ext_clk10_name,
    )

    # LiteScope Analyzer Probes.
    if args.with_pcie_probe:
        soc.add_pcie_probe()
    if args.with_pcie_dma_probe:
        soc.add_pcie_dma_probe()
    if args.with_si5351_i2c_probe:
        soc.add_si5351_i2c_probe()
    if args.with_eth_tx_probe:
        soc.add_eth_tx_probe()
    if args.with_ad9361_spi_probe:
        soc.add_ad9361_spi_probe()
    if args.with_ad9361_data_probe:
        soc.add_ad9361_data_probe()
    if args.with_rom_bus_probe:
        soc.add_rom_bus_probe(depth=args.rom_bus_probe_depth)

    # Builder.
    def get_build_name():
        r = f"litex_m2sdr_{args.variant}"
        if int(args.sys_clk_freq) != int(125e6):
            r += f"_sysclk_{int(args.sys_clk_freq)}"
        if args.with_pcie:
            r += f"_pcie_x{args.pcie_lanes}"
        if args.with_eth:
            r += f"_eth"
            if args.with_eth_ptp:
                r += "_ptp"
                if args.eth_ptp_p2p:
                    r += "_p2p"
                if args.eth_ptp_debug:
                    r += "_debug"
            if args.with_eth_vrt:
                r += "_vrt"
        if args.with_sata:
            r += f"_sata"
        if args.with_white_rabbit:
            r += f"_white_rabbit"
        if args.with_rfic_oversampling:
            r += "_rfic_oversampling"
        if args.without_jtagbone:
            r += "_no_jtagbone"
        if args.with_cpu:
            r += f"_cpu_{args.cpu_variant.replace('+', '_')}"
            r += f"_rom_{args.integrated_rom_size:x}"
        return r

    builder = Builder(soc,
        output_dir               = os.path.join("build", get_build_name()),
        csr_csv                  = "scripts/csr.csv",
        integrated_rom_auto_size = not args.no_integrated_rom_auto_size,
    )
    builder.build(build_name=get_build_name(), run=args.build)

    # Generate LitePCIe Driver.
    generate_litepcie_software(soc, "litex_m2sdr/software", use_litepcie_software=args.driver)

    # Reset Device.
    if args.reset:
        prog = soc.platform.create_programmer()
        prog.reset()
    
    # Load Bitstream.
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
