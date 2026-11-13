#
# This file is part of LiteX-M2SDR project.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Register appendix of the manual: which designs tools/csr_doc.py elaborates and which register
# banks it documents (the engine is generic, part of the manual kit).

import importlib.util
import json
import os
import sys
import tempfile

# Configurations: name -> command line of litex_m2sdr.py (no --build: the SoC is elaborated, no
# toolchain runs). PCIe, Ethernet and SATA cannot be enabled together (the shared QPLL has two
# channels) and the Ethernet PHYs have different registers, so three configurations are needed to
# elaborate the register banks of the target:
# - m2_pcie: PCIe with PTM, the oversampling RFIC PHY, the GPIO core and the PCIe LiteScope probe
#   (analyzer bank, LTSSM tracer of the PCIe PHY).
# - baseboard_eth_sata: Ethernet (1000BASE-X) with PTP, the PTP-disciplined RFIC reference and the
#   VRT streamer, with SATA.
# - baseboard_eth_5g: Ethernet with the 5GBASE-R PHY, for its diagnostics registers.
# White Rabbit is not elaborated: it needs the external litex_wr_nic package and its sources.
CONFIGS = {
    "m2_pcie"            : ["--variant=m2", "--with-pcie", "--with-pcie-ptm",
        "--with-rfic-oversampling", "--with-gpio", "--with-pcie-probe"],
    "baseboard_eth_sata" : ["--variant=baseboard", "--with-eth", "--with-eth-ptp",
        "--with-eth-ptp-rfic-clock", "--with-eth-vrt", "--with-sata"],
    "baseboard_eth_5g"   : ["--variant=baseboard", "--with-eth", "--eth-phy=5000baser"],
}

# Banks shown under a label in one configuration: (configuration, bank) -> label. The same bank name
# holds other registers in another configuration (the appendix documents a bank once), so the label
# keeps both; the note of the label says which bank of csr.csv it is.
LABELS = {
    ("baseboard_eth_5g", "eth_phy") : "eth_phy_5g",
}

# Banks documented in the appendix (prefix match on the bank name), in order, with their title and
# the Python class that defines them. The longest matching prefix owns a bank.
BANKS = [
    # SoC.
    ("capability",           "Capability",                        "Capability",            "litex_m2sdr/gateware/capability.py"),
    ("ctrl",                 "SoC controller (LiteX)",            "SoCController",         "litex/soc/integration/soc.py"),
    ("icap",                 "ICAP (LiteX)",                      "ICAP",                  "litex/soc/cores/icap.py"),
    ("flash_cs_n",           "SPI flash chip select (LiteX)",     "GPIOOut",               "litex/soc/cores/gpio.py"),
    ("flash",                "SPI flash (LiteX)",                 "S7SPIFlash",            "litex/soc/cores/spi_flash.py"),
    ("xadc",                 "XADC (LiteX)",                      "S7SystemMonitor",       "litex/soc/cores/xadc.py"),
    ("dna",                  "Device DNA (LiteX)",                "S7DNA",                 "litex/soc/cores/dna.py"),
    ("leds",                 "Status LED",                        "StatusLed",             "litex_m2sdr/gateware/led.py"),
    ("gpio",                 "GPIO",                              "GPIO",                  "litex_m2sdr/gateware/gpio.py"),
    # Clocking and time.
    ("si5351",               "SI5351 clock generator",            "SI5351",                "litex_m2sdr/gateware/si5351.py"),
    ("clk_measurement",      "Clock measurement",                 "MultiClkMeasurement",   "litex_m2sdr/gateware/measurement.py"),
    ("time_gen",             "Board time",                        "TimeGenerator",         "litex_m2sdr/gateware/time.py"),
    ("pps_in",               "PPS input",                         "PPSInput",              "litex_m2sdr/gateware/pps.py"),
    ("clk10_discipline",     "10 MHz clock phase discipline",     "MMCMPhaseDiscipline",   "litex_m2sdr/gateware/clock_discipline.py"),
    # RFIC and streaming.
    ("ad9361",               "AD9361 RFIC",                       "AD9361RFIC",            "litex_m2sdr/gateware/ad9361/core.py"),
    ("header",               "TX/RX header",                      "TXRXHeader",            "litex_m2sdr/gateware/header.py"),
    ("txrx_loopback",        "TX/RX loopback",                    "TXRXLoopback",          "litex_m2sdr/gateware/loopback.py"),
    ("crossbar",             "Stream crossbar (LiteX)",           "Crossbar",              "litex/soc/interconnect/stream.py"),
    # PCIe.
    ("pcie_phy",             "PCIe PHY (LitePCIe)",               "S7PCIEPHY",             "litepcie/phy/s7pciephy.py"),
    ("pcie_msi",             "PCIe MSI (LitePCIe)",               "LitePCIeMSI",           "litepcie/core/msi.py"),
    ("pcie_dma0",            "PCIe DMA (LitePCIe)",               "LitePCIeDMA",           "litepcie/frontend/dma.py"),
    ("ptm_requester",        "PCIe PTM requester (LitePCIe)",     "PTMRequester",          "litepcie/frontend/ptm.py"),
    # Ethernet.
    ("eth_phy",              "Ethernet PHY, 1000BASE-X and 2500BASE-X (LiteEth)", "A7_1000BASEX", "liteeth/phy/a7_1000basex.py"),
    ("eth_phy_5g",           "Ethernet PHY, 5GBASE-R (LiteEth)",  "A7_GTP_5G_BASER",       "liteeth/phy/serial/baser/wrappers/a7_gtp.py"),
    ("eth_rx_streamer",      "Ethernet RX streamer (LiteEth)",    "LiteEthStream2UDPTX",   "liteeth/frontend/stream.py"),
    ("eth_tx_streamer",      "Ethernet TX streamer (LiteEth)",    "LiteEthUDP2StreamRX",   "liteeth/frontend/stream.py"),
    ("eth_rx_mode",          "Ethernet RX mode",                  "BaseSoC",               "litex_m2sdr.py"),
    ("vrt_streamer",         "VRT streamer",                      "VRTSignalPacketStreamer", "litex_m2sdr/gateware/vrt.py"),
    ("eth_ptp",              "Ethernet PTP (LiteEth)",            "LiteEthPTP",            "liteeth/core/ptp.py"),
    ("ptp_discipline",       "PTP board time discipline",         "PTPTimeDiscipline",     "litex_m2sdr/gateware/ptp_discipline.py"),
    ("ptp_identity",         "PTP identity",                      "PTPIdentityTracker",    "litex_m2sdr/gateware/ptp_identity.py"),
    ("ptp_clk10_discipline", "PTP 10 MHz clock discipline",       "PTPClock10Discipline",  "litex_m2sdr/gateware/clock_discipline.py"),
    # SATA.
    ("sata_phy",             "SATA PHY (LiteSATA)",               "LiteSATAPHY",           "litesata/phy/__init__.py"),
    ("sata_identify",        "SATA identify (LiteSATA)",          "LiteSATAIdentifyCSR",   "litesata/frontend/identify.py"),
    ("sata_sector2mem",      "SATA sectors to memory",            "M2SDRLiteSATASector2MemDMA", "litex_m2sdr/gateware/sata.py"),
    ("sata_mem2sector",      "SATA memory to sectors",            "M2SDRLiteSATAMem2SectorDMA", "litex_m2sdr/gateware/sata.py"),
    ("sata_rx_streamer",     "SATA RX streamer",                  "M2SDRLiteSATAStream2Sectors", "litex_m2sdr/gateware/sata.py"),
    ("sata_tx_streamer",     "SATA TX streamer",                  "M2SDRLiteSATASectors2Stream", "litex_m2sdr/gateware/sata.py"),
    ("sata_streamer_control", "SATA streamer control",            "BaseSoC",               "litex_m2sdr.py"),
    # Debug.
    ("analyzer",             "LiteScope analyzer (LiteScope)",    "LiteScopeAnalyzer",     "litescope/core.py"),
]

# Access of registers that the engine cannot infer: the receive/transmit register of LiteI2C is a
# plain CSR that software writes (transmit) and reads (receive).
ACCESS = {
    "si5351_i2c_master_rxtx" : "rw",
}

# Descriptions of the registers and fields that the gateware declares without one. A description
# declared in the gateware takes precedence; a field declared with a list of values and no
# description is described by its values. Keys: (bank, register) or (bank, register, field).
_CLOCKS = {
    "clk0" : "the system clock",
    "clk1" : "the PCIe clock (0 without PCIe)",
    "clk2" : "the AD9361 reference clock (SI5351 output 0)",
    "clk3" : "the AD9361 data clock",
    "clk4" : "the board time reference clock (SI5351 output 1)",
    "clk5" : "the FPGA 10 MHz clock",
}

DESCRIPTIONS = {
    # LiteI2C master of the SI5351.
    ("si5351", "i2c_phy_speed_mode") : "I2C speed: 0: 100 kHz, 1: 400 kHz, 2: 1 MHz.",
    ("si5351", "i2c_master_active")  : "Write 1 to enable the I2C master.",
    ("si5351", "i2c_master_addr")    : "I2C address of the next transfer.",
    ("si5351", "i2c_master_rxtx")    : "Write: push the transmit data. Read: pop the received data.",
    # Board time.
    ("time_gen", "control", "read")  : "Write 1: latch the board time into `read_time` (pulse).",
    ("time_gen", "control", "write") : "Write 1: load `write_time` into the board time (pulse).",
    # AD9361 SPI.
    ("ad9361", "spi_mosi") : "SPI data to send to the AD9361.",
    ("ad9361", "spi_miso") : "SPI data received from the AD9361.",
    # PCIe PTM.
    ("ptm_requester", "control", "trigger") : "Write 1: issue a PTM request (pulse).",
    # SATA.
    ("sata_phy", "enable")             : "1: SATA PHY enabled, 0: transceiver and initialization held in reset.",
    ("sata_identify", "start")         : "Write 1: send the IDENTIFY DEVICE command.",
    ("sata_identify", "done")          : "1: the IDENTIFY DEVICE command is complete.",
    ("sata_identify", "data_width")    : "Width in bits of the identify data port.",
    ("sata_identify", "source_valid")  : "1: a word of identify data is available in `source_data`.",
    ("sata_identify", "source_ready")  : "Write 1: take the word of `source_data` and move to the next.",
    ("sata_identify", "source_data")   : "Word of identify data.",
    ("sata_identify", "error")         : "1: the IDENTIFY DEVICE command failed.",
    ("sata_sector2mem", "start")       : "Write: start the transfer from the sectors to the memory.",
    ("sata_mem2sector", "start")       : "Write: start the transfer from the memory to the sectors.",
    ("sata_rx_streamer", "start")      : "Write: start the recording.",
    ("sata_rx_streamer", "stop")       : "Write: request the end of the recording.",
    ("sata_tx_streamer", "start")      : "Write: start the playback.",
    ("sata_tx_streamer", "stop")       : "Write: request the end of the playback.",
    # LiteScope.
    ("analyzer", "mux_value")          : "Selected group of signals.",
    ("analyzer", "trigger_enable")     : "1: trigger armed.",
    ("analyzer", "trigger_done")       : "1: the trigger has fired.",
    ("analyzer", "trigger_mem_write")  : "Write: push `trigger_mem_mask` and `trigger_mem_value` as one trigger condition.",
    ("analyzer", "trigger_mem_mask")   : "Mask of the trigger condition to push.",
    ("analyzer", "trigger_mem_value")  : "Value of the trigger condition to push.",
    ("analyzer", "trigger_mem_full")   : "1: the memory of trigger conditions is full.",
    ("analyzer", "subsampler_value")   : "Subsampling ratio minus one.",
    ("analyzer", "storage_enable")     : "1: capture enabled.",
    ("analyzer", "storage_done")       : "1: the capture is complete.",
    ("analyzer", "storage_length")     : "Number of samples to capture.",
    ("analyzer", "storage_offset")     : "Number of samples kept before the trigger.",
    ("analyzer", "storage_mem_level")  : "Number of samples available in the capture memory.",
    ("analyzer", "storage_mem_data")   : "Captured sample; each read moves to the next one.",
    ("clk_measurement", "latch_all")   : "Write: latch the six counters at the same time.",
}
for _name, _what in _CLOCKS.items():
    DESCRIPTIONS[("clk_measurement", f"{_name}_latch")] = f"Write: latch the counter of {_what} into `{_name}_value`."
    DESCRIPTIONS[("clk_measurement", f"{_name}_value")] = f"Latched count of cycles of {_what}."

# Notes printed after the title of a bank family: what the register descriptions do not say.
NOTES = {
    "capability": [
        "Read-only description of the build: software reads it first to know which host links and",
        "options the bitstream holds ([The SoC](#soc)).",
    ],
    "ctrl": [
        "Registers of the LiteX SoC controller. `scratch` is the register to read and write to",
        "check a host bridge ([Debugging](#debugging)).",
    ],
    "icap": [
        "Access to the configuration logic of the FPGA; the utilities use it to reload the FPGA",
        "from the SPI flash after an update ([Building the gateware](#building-gateware)).",
    ],
    "si5351": [
        "The bank holds the registers of the LiteI2C master (`i2c_*`), of the PWM that tunes the",
        "oscillator of the SI5351B version (`pwm_*`) and of the SI5351 control",
        "([Time synchronisation](#timing)).",
    ],
    "clk_measurement": [
        "Each clock has a free-running 64-bit counter. Software latches the counters twice and",
        "divides the difference by the elapsed time to get a frequency, as `scripts/test_clks.py`",
        "does ([Debugging](#debugging)).",
    ],
    "time_gen": [
        "The board time is a 64-bit count of nanoseconds ([Streaming](#streaming)). The field",
        "`sync_enable` of `control` has no effect: the SoC does not connect the PPS input to the",
        "board time.",
    ],
    "pps_in": [
        "The PPS input is only monitored: these registers report it, and nothing sets the board",
        "time from it.",
    ],
    "gpio": [
        "The core is 4 bits wide; only bits 0 and 1 reach a pin, the test points TP1 and TP2",
        "([Hardware](#hardware)).",
    ],
    "clk10_discipline": [
        "Present in the builds without White Rabbit.",
    ],
    "ad9361": [
        "The `phy_rx_deskew_*` registers of the oversampling PHY are present with",
        "`--with-rfic-oversampling` only ([Oversampling mode](#rfic-oversampling)).",
    ],
    "pcie_phy": [
        "Registers of the LitePCIe PHY, documented in the LitePCIe manual. `ltssm_tracer_history` is",
        "present with `--with-pcie-probe` only. `link_status` is the first register to read when a",
        "board is not enumerated ([Debugging](#debugging)).",
    ],
    "pcie_dma0": [
        "Registers of the LitePCIe DMA channel 0, which carries the samples; the kernel driver",
        "owns them. A build with `--pcie-dmas` above 1 has one such bank per channel",
        "(`pcie_dma1` and the next ones). The PPS input of the start synchroniser is connected",
        "on channel 0 only.",
    ],
    "ptm_requester": [
        "Present with `--with-pcie-ptm`.",
    ],
    "eth_phy": [
        "Bank `eth_phy` of a build with `--eth-phy=1000basex` or `--eth-phy=2500basex`.",
    ],
    "eth_phy_5g": [
        "Bank `eth_phy` of a build with `--eth-phy=5000baser`: `eth_phy_5g` is a label of this",
        "appendix, not a bank of `csr.csv`, and the registers are named `eth_phy_reset`,",
        "`eth_phy_control`, `eth_phy_status` and `eth_phy_rx_prbs_errors`.",
    ],
    "eth_rx_streamer": [
        "RX is seen from the antenna: this streamer sends the RX stream to the host in UDP",
        "datagrams.",
    ],
    "eth_tx_streamer": [
        "This streamer receives the TX stream from the host in UDP datagrams.",
    ],
    "eth_rx_mode": [
        "Present with `--with-eth-vrt`. The shipped software does not write this register: the",
        "VRT route is selected by hand ([VRT streaming](#flow-eth-vrt)).",
    ],
    "vrt_streamer": [
        "Present with `--with-eth-vrt`.",
    ],
    "eth_ptp": [
        "Present with `--with-eth-ptp`, as the two PTP banks that follow.",
    ],
    "ptp_clk10_discipline": [
        "Present with `--with-eth-ptp-rfic-clock`.",
    ],
    "sata_phy": [
        "The SATA banks are present with `--with-sata`.",
    ],
    "analyzer": [
        "Present with one of the probe options ([Build options](#build-options)). `litescope_cli`",
        "drives these registers; the LiteScope manual describes them.",
    ],
}

# Opening paragraphs of the appendix and the sentence that ends the access paragraph.
INTRO = ["This appendix lists the registers of the LiteX-M2SDR gateware, bank by bank. It is generated",
         "from the register definitions of the gateware: the tool runs `litex_m2sdr.py` without",
         "`--build` for the configurations of the table below, which elaborates the SoC without",
         "running Vivado.",
         "",
         "One build cannot hold every bank, because PCIe, Ethernet and SATA cannot be enabled",
         "together and the Ethernet PHYs have different registers. The three configurations are",
         "chosen to elaborate every bank once, not as builds to use. White Rabbit builds add the",
         "registers of the White Rabbit core, which this appendix does not list: they need the",
         "external `litex_wr_nic` package.",
         "",
         "Offsets are in bytes from the base of the bank. The banks named in `BaseSoC.csr_map`",
         "(`litex_m2sdr.py`) keep the same base address in every build ([The SoC](#soc)); LiteX",
         "places the others. The addresses are in the `csr.csv` and `csr.h` of a build."]
USAGE = ["",
         "The utilities and the library use these names through the generated `csr.h`, and the",
         "scripts through `csr.csv`, so they never need the offsets."]
MAIN_BANK = "Register of the top-level `main` bank of the SoC."


def _describe(soc):
    """Attach the descriptions of DESCRIPTIONS to the registers that have none in the gateware."""
    for bank, region in soc.csr.regions.items():
        for csr in region.obj:
            desc = DESCRIPTIONS.get((bank, csr.name))
            if desc and not (getattr(csr, "description", None) or "").strip():
                csr.description = desc
            fields = getattr(csr, "fields", None)
            for f in (fields.fields if fields else []):
                desc = DESCRIPTIONS.get((bank, csr.name, f.name))
                if not desc and f.values:
                    # "``0b0``" (reStructuredText) -> "`0b0`"; "`Minimal`" -> "minimal".
                    desc = " ".join(f"{str(v[0]).replace('``', '`')}: {str(v[1]).replace('`', '').replace('  ', ' ')}"
                        for v in f.values)
                if desc and not (f.description or "").strip():
                    f.description = desc


def _load(repo_root):
    """Import the target script under a module name of its own."""
    spec = importlib.util.spec_from_file_location("manual_litex_m2sdr", os.path.join(repo_root, "litex_m2sdr.py"))
    mod  = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def elaborate(name, repo_root):
    """Run the main() of the target with its Builder replaced by one that elaborates without the
    toolchain, from a temporary directory (the target writes build/, scripts/csr.csv, the LiteScope
    test/analyzer.csv and the software headers relative to the working directory); return
    (soc, csr_json, module, argv)."""
    argv = CONFIGS[name]
    sys.path.insert(0, repo_root)
    from litex.soc.integration.builder import Builder
    captured = {}
    old_cwd  = os.getcwd()
    old_argv = sys.argv

    with tempfile.TemporaryDirectory() as tmp:
        class CaptureBuilder(Builder):
            def __init__(self, soc, *args, **kwargs):
                kwargs.update(output_dir=os.path.join(tmp, "build"), csr_csv=os.path.join(tmp, "csr.csv"),
                    csr_json=os.path.join(tmp, "csr.json"), compile_software=False,
                    compile_gateware=False)
                Builder.__init__(self, soc, **kwargs)
                captured["soc"] = soc

            def build(self, *args, **kwargs):
                kwargs["run"] = False
                return Builder.build(self, *args, **kwargs)

        mod = _load(repo_root)
        mod.Builder = CaptureBuilder
        # The software headers of the repository are not regenerated.
        mod.generate_litepcie_software = lambda *args, **kwargs: None
        for d in ("scripts", "test"):
            os.makedirs(os.path.join(tmp, d))
        os.chdir(tmp)
        sys.argv = ["litex_m2sdr.py"] + list(argv)
        try:
            mod.main()
        finally:
            sys.argv = old_argv
            os.chdir(old_cwd)
        captured["json"] = json.load(open(os.path.join(tmp, "csr.json")))

    # Memories mapped on the CSR bus have no register definitions: keep the register banks only.
    soc, csr_json = captured["soc"], captured["json"]
    for bank, region in list(soc.csr.regions.items()):
        if not isinstance(region.obj, list):
            del soc.csr.regions[bank]
    for (config, bank), label in LABELS.items():
        if config == name and bank in soc.csr.regions:
            region = soc.csr.regions.pop(bank)
            soc.csr.regions[label] = region
            csr_json["csr_bases"][label] = csr_json["csr_bases"][bank]
            for csr in region.obj:
                csr_json["csr_registers"][f"{label}_{csr.name}"] = csr_json["csr_registers"][f"{bank}_{csr.name}"]
    _describe(soc)
    return soc, csr_json, "litex_m2sdr.py", list(argv)
