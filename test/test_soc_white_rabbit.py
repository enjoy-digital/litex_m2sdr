#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import importlib.util
from pathlib import Path

import pytest

from migen import *
from migen.fhdl.structure import _Assign

from litex.gen import LiteXModule

from litex.soc.integration.builder import Builder

# Helpers ------------------------------------------------------------------------------------------

@pytest.fixture
def wr_soc(monkeypatch, tmp_path):
    wr_core = pytest.importorskip("litex_wr_nic.gateware.wr_core")
    root    = Path(__file__).resolve().parents[1]
    spec    = importlib.util.spec_from_file_location("litex_m2sdr_soc", root / "litex_m2sdr.py")
    module  = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    registered = []
    # The board tests elaborate WR as an HDL instance; the dependency's own
    # tests cover source preparation and the VHDL/CPU implementations.
    monkeypatch.setattr(wr_core.WhiteRabbitCore, "add_sources", staticmethod(registered.append))
    firmware = tmp_path / "firmware.bram"
    firmware.write_text("00000013\n", encoding="utf-8")
    firmware.with_suffix(".bin").write_bytes(bytes.fromhex("13000000"))

    def create(**kwargs):
        return module.BaseSoC(
            variant           = "baseboard",
            with_pcie         = False,
            with_jtagbone     = False,
            with_white_rabbit = True,
            wr_firmware       = str(firmware),
            **kwargs,
        )

    return create, registered

# WR Integration Tests -----------------------------------------------------------------------------

@pytest.mark.parametrize("sfp,cpu,memory,boot", [
    (0, "urv",      "private",    "embedded"),
    (1, "urv",      "private",    "embedded"),
    (0, "urv",      "integrated", "host"),
    (0, "vexriscv", "integrated", "embedded"),
    (1, "vexriscv", "integrated", "host"),
])
def test_wr_cpu_memory_console_and_automatic_sources(wr_soc, tmp_path, sfp, cpu, memory, boot):
    create, registered = wr_soc
    soc = create(wr_sfp=sfp, wr_cpu_type=cpu, wr_cpu_memory=memory, wr_cpu_boot=boot)
    assert registered == []
    region = soc.bus.regions["wr_wb_slave"]
    assert (region.origin, region.size, region.cached) == (0x40000, 0x40000, False)
    assert not hasattr(soc.crg, "eth_pll") # The tunable MMCM owns refclk_eth.
    assert soc.wr_core.wr_info.cpu_type.status.reset.value == int(cpu == "vexriscv")
    assert soc.uart.xover.rx_fifo.depth == 4096
    if memory == "integrated":
        assert soc.bus.regions["wr_cpu_mem"].size == 128*1024
        assert "wr_cpu" in soc.bus.masters
    else:
        assert soc.wr_core.cpu_bus is None
    if boot == "host":
        assert soc.wr_cpu_boot._host_ready.storage.reset.value == 0

    Builder(soc,
        output_dir       = tmp_path / "build",
        compile_software = False,
        csr_csv          = str(tmp_path / "csr.csv"),
    ).build(run=False)
    assert registered == [soc.platform]
    csr_csv = (tmp_path / "csr.csv").read_text(encoding="utf-8")
    for name in ("uart_xover_rxtx", "uart_rxlevel", "uart_rxoverflow", "wr_info_magic",
        "wr_time_capture", "refclk_mmcm_ps_gen_status", "dmtd_mmcm_ps_gen_status"):
        assert f"csr_register,{name}," in csr_csv
    if boot == "host":
        assert "csr_register,wr_cpu_boot_host_ready," in csr_csv


def test_wr_tuning_uses_system_clock_and_waits_for_both_mmcm_completions(wr_soc):
    create, _ = wr_soc
    soc = create(wr_sfp=0)
    dut = LiteXModule()
    dut.cd_sys    = ClockDomain("sys")
    dut.cd_wr_sys = ClockDomain("wr_sys")
    dut.cd_clk200 = ClockDomain("clk200")
    backends = [soc.refclk_mmcm_ps_gen, soc.dmtd_mmcm_ps_gen]
    mmcms    = [soc.crg.refclk_mmcm, soc.crg.dmtd_mmcm]
    commands = [soc.wr_core.refclk_tuning, soc.wr_core.dmtd_tuning]
    dut.submodules += backends

    # Simulate the actual board connections around the two real backends.
    # Replace only the FPGA MMCM primitives with delayed PSDONE responses.
    destinations = []
    for backend, mmcm in zip(backends, mmcms):
        destinations += [backend.command.data, backend.command.load, backend.psdone,
            mmcm.psen, mmcm.psincdec]
    connections = [statement for statement in soc._fragment.comb
        if isinstance(statement, _Assign) and any(statement.l is signal for signal in destinations)]
    assert len(connections) == len(destinations)
    dut.comb += connections
    completed = [0, 0]

    def send_commands():
        for _ in range(10):
            yield
        for command in commands:
            yield command.data.eq(0)
            yield command.load.eq(1)
        yield
        for command in commands:
            yield command.load.eq(0)

    def mmcm_model(index, delay):
        mmcm      = mmcms[index]
        backend   = backends[index]
        pending   = 0
        direction = None
        for _ in range(1200):
            if pending:
                assert not (yield mmcm.psen), "A second shift started before PSDONE"
                assert (yield mmcm.psincdec) == direction
                pending -= 1
                if pending == 0:
                    completed[index] += 1
            if (yield mmcm.psen):
                pending   = delay
                direction = yield mmcm.psincdec
            yield mmcm.psdone.eq(pending == 1)
            assert not (yield backend.fault)
            yield

    clocks = {"sys": 8, "wr_sys": 16, "clk200": 5}
    for backend in backends:
        clocks[backend.cdc.input_cd]  = clocks["wr_sys"]
        clocks[backend.cdc.output_cd] = clocks["clk200"]
    run_simulation(dut, {
        "wr_sys" : send_commands(),
        "clk200" : [mmcm_model(0, 23), mmcm_model(1, 37)],
    }, clocks=clocks)
    assert all(count > 2 for count in completed)


@pytest.mark.parametrize("cpu,memory,boot", [
    ("vexriscv", "private",    "embedded"),
    ("urv",      "private",    "host"),
    ("urv",      "hyperram",   "host"),
    ("urv",      "integrated", "spi"),
])
def test_wr_rejects_unsupported_memory_and_boot(wr_soc, cpu, memory, boot):
    create, _ = wr_soc
    with pytest.raises(ValueError):
        create(wr_sfp=0, wr_cpu_type=cpu, wr_cpu_memory=memory, wr_cpu_boot=boot)
