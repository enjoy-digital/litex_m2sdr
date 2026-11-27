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


    gateware, verilog = _generate(soc, tmp_path, "top")
    assert "XADC" in verilog

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


def test_openxc7_soc_has_the_primitives_of_the_vivado_soc(tmp_path):
    # openXC7/nextpnr takes the primitives the Vivado build uses: no replacement, no techmap.
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(variant="baseboard", with_pcie=False, with_usb3=True, with_jtagbone=False,
        toolchain="openxc7")

    gateware, verilog = _generate(soc, tmp_path, "top")
    assert "BUFGMUX" in verilog and "BUFGCTRL" not in verilog
    assert "IDDR" in verilog
    common = re.search(r"GTPE2_COMMON #\(.*?\);", verilog, re.S).group(0)
    assert re.search(r"\.GTGREFCLK0\s*\(sys_clk\)", common)
    for port in ["GTREFCLK0", "GTREFCLK1", "GTGREFCLK1"]:
        assert f".{port}" in common
    assert re.search(r"\.PLL0REFCLKSEL\s*\(3'd7\)", common)

    ys = (gateware / "top.ys").read_text()
    assert "techmap -map" not in ys

    # The IOs with their pull resistors as Vivado takes them (openXC7/nextpnr reads PULLUP TRUE).
    xdc = (gateware / "top.xdc").read_text()
    assert "PULLUP TRUE" in xdc


def test_vivado_usb3_keeps_gtgrefclk(tmp_path):
    soc_mod = _load_soc_module()
    soc = soc_mod.BaseSoC(variant="baseboard", with_pcie=False, with_usb3=True, with_jtagbone=False)

    _, verilog = _generate(soc, tmp_path, "top")
    common = re.search(r"GTPE2_COMMON #\(.*?\);", verilog, re.S).group(0)
    assert re.search(r"\.GTGREFCLK0\s*\(sys_clk\)", common)
    for port in ["GTREFCLK0", "GTREFCLK1", "GTGREFCLK1"]:
        assert f".{port}" in common


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
