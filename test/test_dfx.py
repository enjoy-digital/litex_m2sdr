#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os

import pytest

from litex_m2sdr.gateware.dfx import DEFAULT_DFX_PBLOCK, validate_dfx_pblock


def test_dfx_default_pblock_is_valid():
    assert validate_dfx_pblock(DEFAULT_DFX_PBLOCK) == DEFAULT_DFX_PBLOCK


@pytest.mark.parametrize("pblock", [
    "",
    "SLICE_X150Y100",
    "DSP48_X0Y0:DSP48_X1Y1",
    "SLICE_X150Y100:SLICE_X159Y149 RAMB18_X0Y0:RAMB18_X0Y1",
    "{SLICE_X150Y100:SLICE_X159Y149}",
    "SLICE_X159Y149:SLICE_X150Y100",
])
def test_dfx_rejects_unsupported_pblock_syntax(pblock):
    with pytest.raises(ValueError):
        validate_dfx_pblock(pblock)


def _load_soc_module():
    import importlib.util
    from pathlib import Path
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("litex_m2sdr_soc", root / "litex_m2sdr.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("lanes, oversampling, width", [(1, False, 64), (4, True, 128)])
def test_dfx_region_replaces_the_rfic_hook_at_the_datapath_width(lanes, oversampling, width):
    from litex_m2sdr.gateware.dfx import DFX_CELL_NAME, DFXRFICDatapath
    from litex_m2sdr.gateware.loopback import TXRXLoopback

    soc = _load_soc_module().BaseSoC(variant="m2", with_pcie=True, pcie_lanes=lanes,
        with_rfic_oversampling=oversampling, with_dfx=True)
    assert isinstance(soc.rfic_dfx, DFXRFICDatapath)
    assert isinstance(soc.txrx_loopback, TXRXLoopback)   # Host loopback stays static.
    for name in ("tx_in", "tx_out", "rx_in", "rx_out", "sb_in", "sb_out"):  # Registered boundary.
        assert hasattr(getattr(soc.rfic_dfx, name), "pipe_ready")
    (region,) = [s for s in soc.rfic_dfx._fragment.specials
        if getattr(s, "name_override", None) == DFX_CELL_NAME]
    params = {item.name: item.value.value for item in region.items
        if type(item).__name__ == "Parameter"}
    assert params == {"DATA_WIDTH": width}
    ports = {item.name for item in region.items if type(item).__name__ in ("Input", "Output")}
    assert {"clk", "rst", "sb_sink_data", "sb_source_data", "time_ns", "sb_status"} <= ports
    assert hasattr(soc, "pcie_dma1")                   # --with-dfx reserves DMA1 for the side band.
    assert len(soc.rfic_dfx.sb_source.data) == width
    commands = "\n".join(soc.platform.toolchain.pre_optimize_commands +
        soc.platform.toolchain.additional_commands)
    assert DEFAULT_DFX_PBLOCK in commands
    assert f"-generic DATA_WIDTH={width}" in commands   # Alternate RM matches the static width.


def test_dfx_rm_tcl_implements_only_the_rm_in_the_locked_static():
    from litex_m2sdr.gateware.dfx import dfx_rm_tcl

    tcl = dfx_rm_tcl("img", "mydsp", ["rm.v"], "xc7a200tsbg484-3", 128)
    order = [next(i for i, line in enumerate(tcl) if line.startswith(cmd)) for cmd in
        ("synth_design -mode out_of_context", "open_checkpoint img_static_routed.dcp",
         "read_checkpoint -cell", "route_design", "pr_verify -full_check img_passthrough_routed.dcp",
         "write_bitstream")]
    assert order == sorted(order)                      # Verified before any partial is written.
    assert not any(line.startswith("synth_design -top") for line in tcl)  # No static synthesis.
    assert any("$dfx_width != 128" in line for line in tcl)
    with pytest.raises(ValueError):
        dfx_rm_tcl("img", "../evil", ["rm.v"], "xc7a200tsbg484-3", 128)
    with pytest.raises(ValueError):
        dfx_rm_tcl("img", "mydsp", ["rm.v"], "xc7a200tsbg484-3", 32)


@pytest.mark.parametrize("oversampling, width", [(False, 64), (True, 128)])
def test_dfx_rm_build_reads_the_image_contract(tmp_path, oversampling, width):
    import importlib.util, json
    spec = importlib.util.spec_from_file_location("dfx_rm_build",
        os.path.join(os.path.dirname(__file__), "..", "scripts", "dfx_rm_build.py"))
    script = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(script)

    build = tmp_path / "img"
    (build / "gateware").mkdir(parents=True)
    argv = ["litex_m2sdr.py", "--with-dfx"] + (["--with-rfic-oversampling"] if oversampling else [])
    (build / "build_info.json").write_text(json.dumps(
        {"command": {"argv": argv}, "platform": {"device": "xc7a200tsbg484-3"}}))
    (build / "gateware" / "img.bit").write_bytes(b"\x00\x09img;PARTIAL=FALSE;Version=2024.2\x00")
    with pytest.raises(SystemExit):                    # No locked static checkpoint yet.
        script.image_contract(str(build))
    for suffix in ("static_routed.dcp", "passthrough_routed.dcp"):
        (build / "gateware" / f"img_{suffix}").write_bytes(b"")
    contract = script.image_contract(str(build))
    assert (contract["data_width"], contract["vivado"], contract["part"]) == (width, "2024.2", "xc7a200tsbg484-3")


@pytest.mark.parametrize("width", [64, 128])
def test_dfx_loopback_rm_detector_records_bursts_and_drops_when_busy(tmp_path, width):
    import shutil, subprocess
    if shutil.which("verilator") is None:
        pytest.skip("verilator not installed")
    root = os.path.join(os.path.dirname(__file__), "..")
    subprocess.run(["verilator", "--binary", "--timing", "-Wno-fatal", "-Wno-DECLFILENAME",
        f"-GDATA_WIDTH={width}", "--top-module", "tb", "-Mdir", str(tmp_path),
        os.path.join(root, "test", "dfx", "tb_loopback_detector.sv"),
        os.path.join(root, "litex_m2sdr", "gateware", "dfx", "m2sdr_dfx_datapath_loopback.v")],
        check=True, capture_output=True)
    result = subprocess.run([str(tmp_path / "Vtb")], capture_output=True, text=True, timeout=120)
    assert "PASS" in result.stdout, result.stdout + result.stderr
