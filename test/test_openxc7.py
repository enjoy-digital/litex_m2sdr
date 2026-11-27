#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import importlib.util
import re
from pathlib import Path

import pytest

from litex.soc.integration.builder import Builder


def _load_soc_module():
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("litex_m2sdr_soc", root / "litex_m2sdr.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _generate(soc, tmp_path, build_name):
    builder = Builder(
        soc,
        output_dir       = tmp_path / "build",
        compile_software = False,
        csr_csv          = str(tmp_path / "csr.csv"),
    )
    builder.build(build_name=build_name, run=False)
    gateware = tmp_path / "build" / "gateware"
    return gateware, (gateware / f"{build_name}.v").read_text()


def test_openxc7_soc_generates_without_vivado_only_content(tmp_path):
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(variant="m2", with_pcie=False, toolchain="openxc7")

    # Primitives the open-source flow cannot place are left out.
    assert not hasattr(soc, "xadc")

    gateware, verilog = _generate(soc, tmp_path, "top")
    assert "BUFGMUX" not in verilog
    assert "BUFGCTRL" in verilog
    assert "XADC" not in verilog

    # nextpnr supports CLKFBOUT_MULT_F up to 63.
    mults = [int(m) for m in re.findall(r"\.CLKFBOUT_MULT_F\s*\(\d+'d(\d+)\)", verilog)]
    assert mults and max(mults) <= 63

    # Yosys/nextpnr build script and constraints, without Vivado Tcl.
    assert (gateware / "top.ys").exists()
    assert not (gateware / "top.tcl").exists()
    xdc = (gateware / "top.xdc").read_text()
    assert "create_clock" in xdc
    for vivado_only in ["set_false_path", "set_clock_groups", "get_clocks", "get_nets", "BITSTREAM"]:
        assert vivado_only not in xdc
    assert "nextpnr-xilinx" in (gateware / "build_top.sh").read_text()


def test_vivado_soc_keeps_its_primitives_and_bitstream_commands(tmp_path):
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(variant="m2", with_pcie=False)

    assert soc.platform.toolchain_name == "vivado"
    assert hasattr(soc, "xadc")
    assert any("write_cfgmem" in c for c in soc.platform.toolchain.additional_commands)
    assert any("COMPRESS" in c for c in soc.platform.toolchain.bitstream_commands)

    gateware, verilog = _generate(soc, tmp_path, "top")
    assert "BUFGMUX" in verilog
    assert "BUFGCTRL" not in verilog
    assert "XADC" in verilog
    tcl = (gateware / "top.tcl").read_text()
    assert "set_clock_groups" in tcl
    assert "write_cfgmem" in tcl


def test_openxc7_usb3_gtp_refclk_and_techmaps(tmp_path):
    # nextpnr-xilinx takes the fabric reference clock of the GTP PLL on GTGREFCLK0 (enabled in the
    # bitstream by the build) and no constant on the other reference clock ports; the primitives it does not take are replaced by
    # techmaps after the synthesis.
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(variant="baseboard", with_pcie=False, with_usb3=True, with_jtagbone=False,
        toolchain="openxc7")

    gateware, verilog = _generate(soc, tmp_path, "top")
    common = re.search(r"GTPE2_COMMON #\(.*?\);", verilog, re.S).group(0)
    assert re.search(r"\.GTGREFCLK0\s*\(sys_clk\)", common)
    for port in ["GTREFCLK0", "GTREFCLK1", "GTGREFCLK1"]:
        assert f".{port}" not in common
    assert re.search(r"\.PLL0REFCLKSEL\s*\(3'd7\)", common)

    ys = (gateware / "top.ys").read_text().splitlines()
    synth = next(i for i, line in enumerate(ys) if line.startswith("synth_xilinx"))
    write = next(i for i, line in enumerate(ys) if line.startswith("write_json"))
    techmaps = [line for line in ys[synth + 1:write] if line.startswith("techmap -map ")]
    assert any(line.endswith("lutram_sp.v") for line in techmaps)
    assert any(line.endswith("iddr_pipelined.v") for line in techmaps)

    # The IOs with the pull resistors and the registered tristates as nextpnr-xilinx takes them.
    xdc = (gateware / "top.xdc").read_text()
    assert "PULLTYPE PULLUP" in xdc and "PULLUP TRUE" not in xdc


def test_vivado_usb3_keeps_gtgrefclk(tmp_path):
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(variant="baseboard", with_pcie=False, with_usb3=True, with_jtagbone=False)

    _, verilog = _generate(soc, tmp_path, "top")
    common = re.search(r"GTPE2_COMMON #\(.*?\);", verilog, re.S).group(0)
    assert re.search(r"\.GTGREFCLK0\s*\(sys_clk\)", common)
    for port in ["GTREFCLK0", "GTREFCLK1", "GTGREFCLK1"]:
        assert f".{port}" in common


def test_openxc7_refuses_two_fabric_gtp_refclks():
    # Ethernet 1000BASE-X and SATA both feed their PLL of the GTP quad from the fabric.
    soc_mod = _load_soc_module()
    with pytest.raises(ValueError, match="single fabric reference clock"):
        soc_mod.BaseSoC(variant="baseboard", with_pcie=False, with_eth=True, with_sata=True,
            with_jtagbone=False, toolchain="openxc7")


def test_openxc7_pcie_requires_ip_sources():
    soc_mod = _load_soc_module()
    with pytest.raises(ValueError, match="pcie_ip_sources"):
        soc_mod.BaseSoC(variant="m2", with_pcie=True, toolchain="openxc7")


def test_openxc7_pcie_uses_pregenerated_ip_sources(tmp_path):
    ip_dir = tmp_path / "pcie_s7"
    (ip_dir / "synth").mkdir(parents=True)
    (ip_dir / "source").mkdir()
    (ip_dir / "synth"  / "pcie_s7.v").write_text("// IP wrapper.\n")
    (ip_dir / "source" / "pcie_s7_core_top.v").write_text("// IP source.\n")

    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(variant="m2", with_pcie=True, toolchain="openxc7", pcie_ip_sources=str(ip_dir))

    assert soc.pcie_phy.external_hard_ip
    sources = {Path(filename).name for filename, _, _ in soc.platform.sources}
    assert {"pcie_s7.v", "pcie_s7_core_top.v", "xpm_cdc_single.v", "bram_tdp_macro.v"} <= sources


@pytest.mark.parametrize("flash_option", ["--flash", "--flash-multiboot"])
def test_main_refuses_to_flash_openxc7_bitstreams(monkeypatch, flash_option):
    soc_mod = _load_soc_module()
    monkeypatch.setattr("sys.argv", ["litex_m2sdr.py", "--toolchain=openxc7", flash_option])
    with pytest.raises(SystemExit):
        soc_mod.main()


def test_openxc7_bitstream_fixups(tmp_path):
    # The step around fasm2frames: a terminated LVDS pair is configured as Vivado does (the P pad
    # is not input-only), and the bits the database does not know are set in the frames.
    import json
    from litex_m2sdr.gateware import openxc7_bitstream as fixup

    (tmp_path / "top.xdc").write_text(
        "set_property LOC U6 [get_ports {rx_data_p[0]}]\n"
        "set_property DIFF_TERM TRUE [get_ports {rx_data_p[0]}]\n"
        "set_property LOC V5 [get_ports {rx_data_n[0]}]\n"
        "set_property DIFF_TERM TRUE [get_ports {rx_data_n[0]}]\n"
        "set_property LOC A1 [get_ports {led}]\n")
    (tmp_path / "package_pins.csv").write_text(
        "pin,bank,site,tile,pin_function\n"
        "U6,34,IOB_X1Y118,RIOB33_X105Y117,IO_L16P_T2_34\n"
        "V5,34,IOB_X1Y117,RIOB33_X105Y117,IO_L16N_T2_34\n"
        "A1,35,IOB_X1Y190,RIOB33_X105Y189,IO_L1P_T0_35\n")
    in_only = "RIOB33_X105Y117.IOB_Y0." + fixup.IOB_IN_ONLY
    (tmp_path / "top.fasm").write_text("\n".join([
        in_only,
        "RIOB33_X105Y117.IOB_Y1." + fixup.IOB_IN_ONLY,
        "RIOB33_X105Y189.IOB_Y0." + fixup.IOB_IN_ONLY,
        # A clock routed to GTGREFCLK0, that nextpnr-xilinx does not enable by itself.
        "GTP_COMMON_MID_LEFT_X103Y231.GTPE2_COMMON_GTGREFCLK0.GTPE2_CLK0_5",
        "GTP_COMMON_MID_LEFT_X103Y231.GTPE2_COMMON.GTREFCLK0_USED",
    ]) + "\n")
    notes = tmp_path / "top.fixup.json"
    fixup.fix_fasm(str(tmp_path / "top.fasm"), str(tmp_path / "top.xdc"), str(tmp_path / "package_pins.csv"), str(notes))

    fasm = (tmp_path / "top.fasm").read_text().split("\n")
    assert in_only not in fasm
    assert "RIOB33_X105Y117.IOB_Y1." + fixup.IOB_IN_ONLY in fasm          # The N pad stays input-only,
    assert "RIOB33_X105Y189.IOB_Y0." + fixup.IOB_IN_ONLY in fasm          # as a pad without termination.
    assert "GTP_COMMON_MID_LEFT_X103Y231.GTPE2_COMMON.GTGREFCLK0_USED" not in fasm
    assert "GTP_COMMON_MID_LEFT_X103Y231.GTPE2_COMMON.GTREFCLK0_USED" in fasm
    bits = json.loads(notes.read_text())
    assert sorted(b for t, b in bits if t == "RIOB33_X105Y117") == sorted(fixup.DIFF_TERM_BITS)
    assert sorted(b for t, b in bits if t.startswith("GTP_COMMON")) == sorted(fixup.GTGREFCLK_BITS)

    # Frames: bit <frame>_<offset> of a tile is bit offset % 32 of word (tile offset + offset // 32)
    # of the frame (tile base + frame).
    (tmp_path / "tilegrid.json").write_text(json.dumps({
        "RIOB33_X105Y117":              {"bits": {"CLB_IO_CLK": {"baseaddr": "0x00400000", "frames": 42, "offset": 10, "words": 4}}},
        "GTP_COMMON_MID_LEFT_X103Y231": {"bits": {"CLB_IO_CLK": {"baseaddr": "0x00401000", "frames": 32, "offset": 20, "words": 50}}},
    }))
    words = ",".join(["0x00000000"] * 101)
    addresses = [0x00400000 + 38, 0x00400000 + 39, 0x00401000, 0x00401001, 0x00402000]
    (tmp_path / "top.frames").write_text("".join("0x%08X %s\n" % (a, words) for a in addresses))
    fixup.fix_frames(str(tmp_path / "top.frames"), str(tmp_path / "tilegrid.json"), str(notes))
    frames = {int(l.split(" ")[0], 16): [int(w, 16) for w in l.split(" ")[1].split(",")]
        for l in (tmp_path / "top.frames").read_text().split("\n") if l.strip()}
    assert frames[0x00400000 + 38][10 + 100 // 32] == (1 << (100 % 32)) | (1 << (118 % 32))
    assert frames[0x00400000 + 39][10 + 97 // 32] == sum(1 << (b % 32) for b in (97, 113, 119, 125))
    assert frames[0x00401000][20 + 1438 // 32] == (1 << (1438 % 32)) | (1 << (1439 % 32))
    assert frames[0x00401001][20 + 1438 // 32] == 1 << (1438 % 32)
    assert not any(frames[0x00402000])

    # A terminated pair on a tile whose bits are not known is refused.
    (tmp_path / "package_pins.csv").write_text("pin,bank,site,tile,pin_function\nU6,14,IOB_X0Y118,LIOB33_X0Y117,IO\nV5,14,IOB_X0Y117,LIOB33_X0Y117,IO\n")
    with pytest.raises(ValueError, match="only known for"):
        fixup.fix_fasm(str(tmp_path / "top.fasm"), str(tmp_path / "top.xdc"), str(tmp_path / "package_pins.csv"), str(notes))
