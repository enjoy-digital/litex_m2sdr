#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import time
import threading
import argparse
import copy
import json
import os

from litex import RemoteClient

from test_clks   import ClkDriver, CLOCKS
from test_xadc   import XADCDriver
from test_header import HeaderDriver
from test_time   import TimeDriver, unix_to_datetime
from test_agc    import AGCDriver

# Constants ----------------------------------------------------------------------------------------

XADC_WINDOW_DURATION = 10
DASHBOARD_SETTINGS_PATH = os.path.expanduser("~/.litex_m2sdr_dashboard.json")


def load_dashboard_settings():
    default = {
        "refresh": 0.1,
        "freeze_plots": False,
        "windows": {},
    }
    try:
        with open(DASHBOARD_SETTINGS_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict):
            return default
        default.update({k: v for k, v in data.items() if k in default})
        if not isinstance(default["windows"], dict):
            default["windows"] = {}
        return default
    except Exception:
        return default


def save_dashboard_settings(settings):
    try:
        with open(DASHBOARD_SETTINGS_PATH, "w", encoding="utf-8") as f:
            json.dump(settings, f, indent=2, sort_keys=True)
    except Exception as e:
        print(f"Unable to save dashboard settings: {e}")

# GUI ----------------------------------------------------------------------------------------------

def run_gui(host="localhost", csr_csv="csr.csv", port=1234):
    import dearpygui.dearpygui as dpg

    default_window_pos = {
        "win_status":    (625, 0),
        "win_registers": (1230, 0),
        "win_clks_time": (0, 0),
        "win_dmas":      (0, 450),
        "win_xadc":      (625, 510),
        "win_rf_agc":    (935, 0),
        "win_overview":  (260, 0),
    }

    dashboard_settings = load_dashboard_settings()
    refresh_default = max(0.02, float(dashboard_settings.get("refresh", 0.1)))
    freeze_default = bool(dashboard_settings.get("freeze_plots", False))

    bus = RemoteClient(host=host, csr_csv=csr_csv, port=port)
    bus.open()

    # Record the start time.
    start_time = time.time()

    # Board capabilities.
    with_identifier = hasattr(bus.bases, "identifier_mem")
    with_xadc       = hasattr(bus.regs, "xadc_temperature")
    with_clks       = hasattr(bus.regs, "clk_measurement_clk0_value")
    with_header_reg = hasattr(bus.regs, "header_last_tx_header")
    with_time       = hasattr(bus.regs, "time_gen_read_time")

    # Initialize ClkDriver if available.
    if with_clks:
        clk_drivers = {clk: ClkDriver(bus, clk, CLOCKS[clk]) for clk in CLOCKS}
    else:
        clk_drivers = None

    # Initialize XADCDriver if available.
    if with_xadc:
        xadc_driver = XADCDriver(bus)
    else:
        xadc_driver = None

    # Initialize DMA Header driver if available.
    if with_header_reg:
        header_driver = HeaderDriver(bus, "header")

    # Initialize TimeDriver if available.
    if with_time:
        time_driver = TimeDriver(bus, "time_gen")
    else:
        time_driver = None

    # Create AGC driver instances for each RF AGC module.
    rf_agc_instances = ["rx1_low", "rx1_high", "rx2_low", "rx2_high"]
    agc_drivers = {inst: AGCDriver(bus, name=f"ad9361_agc_count_{inst}") for inst in rf_agc_instances}
    # Dictionary to hold auto_clear state for each instance.
    agc_auto_clear = {inst: False for inst in rf_agc_instances}

    # Board functions.
    def reboot():
        bus.regs.ctrl_reset.write(1)
        bus.regs.ctrl_reset.write(0)

    if with_identifier:
        def get_identifier():
            identifier = ""
            for i in range(256):
                c = chr(bus.read(bus.bases.identifier_mem + 4 * i) & 0xff)
                identifier += c
                if c == "\0":
                    break
            return identifier
    else:
        def get_identifier():
            return "LiteX-M2SDR"

    ui_state_lock = threading.Lock()
    ui_state = {
        "refresh": refresh_default,
        "freeze_plots": freeze_default,
    }
    clear_counters_event = threading.Event()

    def get_window_kwargs(tag, label, default_pos=None, default_size=None, autosize=False):
        kwargs = {"label": label, "tag": tag}
        win_cfg = dashboard_settings.get("windows", {}).get(tag, {})
        pos = win_cfg.get("pos", default_pos)
        if pos is not None:
            kwargs["pos"] = tuple(pos)
        if autosize:
            kwargs["autosize"] = True
        else:
            size = win_cfg.get("size", default_size)
            if size is not None:
                kwargs["width"] = int(size[0])
                kwargs["height"] = int(size[1])
        return kwargs

    # Create Main Window.
    board_name = get_identifier().rstrip("\0")
    main_title = f"LiteX M2SDR Dashboard on '{board_name}'"
    dpg.create_context()
    dpg.create_viewport(title=main_title, width=1920, height=1080, always_on_top=True)
    dpg.setup_dearpygui()

    def set_status_badge(tag, state, label):
        colors = {
            "green":  [80, 220, 120, 255],
            "yellow": [235, 200, 80, 255],
            "red":    [240, 90, 90, 255],
        }
        dpg.set_value(tag, f"{label}: {state.upper()}")
        dpg.configure_item(tag, color=colors[state])

    def on_refresh_changed(sender, app_data, user_data):
        with ui_state_lock:
            ui_state["refresh"] = max(0.02, float(app_data))

    def on_freeze_plots_changed(sender, app_data, user_data):
        with ui_state_lock:
            ui_state["freeze_plots"] = bool(app_data)

    def on_clear_counters(sender, app_data, user_data):
        clear_counters_event.set()

    def reset_layout(sender=None, app_data=None, user_data=None):
        dashboard_settings["windows"] = {}
        for tag, pos in default_window_pos.items():
            if dpg.does_item_exist(tag):
                dpg.set_item_pos(tag, list(pos))

    with dpg.window(**get_window_kwargs("win_status", "LiteX M2SDR Status/Controls", default_pos=(625, 0), default_size=(300, 240))):
        dpg.add_text("Status Badges")
        dpg.add_text("DMA Enabled: --", tag="status_dma_enabled")
        dpg.add_text("Loopback: --", tag="status_loopback")
        dpg.add_text("Synchronizer: --", tag="status_sync")
        dpg.add_text("AGC Saturation: --", tag="status_agc")
        dpg.add_separator()
        dpg.add_slider_float(label="Refresh (s)", min_value=0.02, max_value=1.0, default_value=refresh_default, callback=on_refresh_changed)
        dpg.add_checkbox(label="Freeze XADC Plots", default_value=freeze_default, callback=on_freeze_plots_changed)
        dpg.add_button(label="Clear Counters", callback=on_clear_counters)
        dpg.add_button(label="Reset Layout", callback=reset_layout)
        dpg.add_button(label="Reboot FPGA", callback=lambda: reboot())
        dpg.add_text("", tag="status_error_text")

    # Registers Window.
    with dpg.window(**get_window_kwargs("win_registers", "LiteX M2SDR Registers", default_pos=(1230, 0), autosize=True)):
        dpg.add_text("Control/Status")
        def filter_callback(sender, filter_str):
            dpg.set_value("csr_filter", filter_str)
        dpg.add_input_text(label="CSR Filter (inc, -exc)", callback=filter_callback)
        dpg.add_text("CSR Registers:")
        with dpg.filter_set(tag="csr_filter"):
            def reg_callback(tag, data):
                for name, reg in bus.regs.__dict__.items():
                    if tag == name:
                        try:
                            reg.write(int(data, 0))
                        except Exception as e:
                            print(e)
            for name, reg in bus.regs.__dict__.items():
                dpg.add_input_text(
                    indent=16,
                    label=f"0x{reg.addr:08x} - {name}",
                    tag=name,
                    filter_key=name,
                    callback=reg_callback,
                    on_enter=True,
                    width=200
                )

    # Clks and Time Window.
    with dpg.window(**get_window_kwargs("win_clks_time", "LiteX M2SDR Clks/Time", default_pos=(0, 0), autosize=True)):
        with dpg.collapsing_header(label="Clks", default_open=True):
            with dpg.table(header_row=True, tag="clocks_table", resizable=False, policy=dpg.mvTable_SizingFixedFit):
                dpg.add_table_column(label="Name")
                dpg.add_table_column(label="Frequency (MHz)")
                for clk, name in CLOCKS.items():
                    with dpg.table_row():
                        dpg.add_text(name)
                        dpg.add_text("--", tag=f"clock_{clk}_freq")

        with dpg.collapsing_header(label="Time", default_open=True):
            dpg.add_text("Time: -- ns", tag="time_ns")
            dpg.add_text("Date/Time: --", tag="time_str")

    # DMA Header & Timestamps Window.
    if with_header_reg:
        with dpg.window(**get_window_kwargs("win_dmas", "LiteX M2SDR DMAs", default_pos=(0, 450), autosize=True)):
            with dpg.collapsing_header(label="DMA Info", default_open=True):
                with dpg.table(header_row=True, tag="dma_info_table", resizable=False, policy=dpg.mvTable_SizingFixedFit, width=600):
                    dpg.add_table_column(label="Register", width=250)
                    dpg.add_table_column(label="Value", width=350)
                    with dpg.table_row():
                        dpg.add_text("Writer Enable")
                        dpg.add_text("", tag="dma_writer_enable")
                    with dpg.table_row():
                        dpg.add_text("Writer Table Level")
                        dpg.add_text("", tag="dma_writer_table_level")
                    with dpg.table_row():
                        dpg.add_text("Writer Table Loop Status")
                        dpg.add_text("", tag="dma_writer_table_loop_status")
                    with dpg.table_row():
                        dpg.add_text("Writer Loops/s")
                        dpg.add_text("", tag="dma_writer_loops_speed")
                    with dpg.table_row():
                        dpg.add_text("Reader Enable")
                        dpg.add_text("", tag="dma_reader_enable")
                    with dpg.table_row():
                        dpg.add_text("Reader Table Level")
                        dpg.add_text("", tag="dma_reader_table_level")
                    with dpg.table_row():
                        dpg.add_text("Reader Table Loop Status")
                        dpg.add_text("", tag="dma_reader_table_loop_status")
                    with dpg.table_row():
                        dpg.add_text("Reader Loops/s")
                        dpg.add_text("", tag="dma_reader_loops_speed")
                    with dpg.table_row():
                        dpg.add_text("Loopback Enable")
                        dpg.add_text("", tag="dma_loopback_enable")
                    with dpg.table_row():
                        dpg.add_text("Synchronizer Bypass")
                        dpg.add_text("", tag="dma_sync_bypass")
                    with dpg.table_row():
                        dpg.add_text("Synchronizer Enable")
                        dpg.add_text("", tag="dma_sync_enable")

            with dpg.collapsing_header(label="TX DMA Header", default_open=True):
                with dpg.table(header_row=True, tag="dma_tx_table", resizable=False, policy=dpg.mvTable_SizingFixedFit, width=400):
                    dpg.add_table_column(label="Field", width=150)
                    dpg.add_table_column(label="Value", width=430)
                    with dpg.table_row():
                        dpg.add_text("Header")
                        dpg.add_text("", tag="dma_tx_header")
                    with dpg.table_row():
                        dpg.add_text("Timestamp")
                        dpg.add_text("", tag="dma_tx_timestamp")
                    with dpg.table_row():
                        dpg.add_text("Date/Time")
                        dpg.add_text("", tag="dma_tx_datetime")

            with dpg.collapsing_header(label="RX DMA Header", default_open=True):
                with dpg.table(header_row=True, tag="dma_rx_table", resizable=False, policy=dpg.mvTable_SizingFixedFit, width=400):
                    dpg.add_table_column(label="Field", width=150)
                    dpg.add_table_column(label="Value", width=430)
                    with dpg.table_row():
                        dpg.add_text("Header")
                        dpg.add_text("", tag="dma_rx_header")
                    with dpg.table_row():
                        dpg.add_text("Timestamp")
                        dpg.add_text("", tag="dma_rx_timestamp")
                    with dpg.table_row():
                        dpg.add_text("Date/Time")
                        dpg.add_text("", tag="dma_rx_datetime")

    # XADC Window.
    if with_xadc:
        with dpg.window(**get_window_kwargs("win_xadc", "LiteX M2SDR XADC", default_pos=(625, 510), default_size=(600, 500))):
            with dpg.subplots(2, 2, label="", width=-1, height=-1):
                # Temperature Plot.
                with dpg.plot(label="Temperature (°C)"):
                    dpg.add_plot_axis(dpg.mvXAxis, tag="temp_x", label="Time (s)")
                    with dpg.plot_axis(dpg.mvYAxis, tag="temp_y"):
                        dpg.add_line_series([], [], label="temp", tag="temp")
                    dpg.set_axis_limits("temp_y", 0, 100)
                # VCCInt Plot.
                with dpg.plot(label="VCCInt (V)"):
                    dpg.add_plot_axis(dpg.mvXAxis, tag="vccint_x", label="Time (s)")
                    with dpg.plot_axis(dpg.mvYAxis, tag="vccint_y"):
                        dpg.add_line_series([], [], label="vccint", tag="vccint")
                    dpg.set_axis_limits("vccint_y", 0, 1.8)
                # VCCAux Plot.
                with dpg.plot(label="VCCAux (V)"):
                    dpg.add_plot_axis(dpg.mvXAxis, tag="vccaux_x", label="Time (s)")
                    with dpg.plot_axis(dpg.mvYAxis, tag="vccaux_y"):
                        dpg.add_line_series([], [], label="vccaux", tag="vccaux")
                    dpg.set_axis_limits("vccaux_y", 0, 2.5)
                # VCCBRAM Plot.
                with dpg.plot(label="VCCBRAM (V)"):
                    dpg.add_plot_axis(dpg.mvXAxis, tag="vccbram_x", label="Time (s)")
                    with dpg.plot_axis(dpg.mvYAxis, tag="vccbram_y"):
                        dpg.add_line_series([], [], label="vccbram", tag="vccbram")
                    dpg.set_axis_limits("vccbram_y", 0, 1.8)

    # RF AGC Panel.
    with dpg.window(**get_window_kwargs("win_rf_agc", "LiteX M2SDR RF AGC", default_pos=(935, 0), autosize=True)):
        for inst in rf_agc_instances:
            with dpg.collapsing_header(label=f"AGC {inst.upper()}", default_open=True):
                dpg.add_text("Saturation Count: --", tag=f"agc_{inst}_count")
                def make_slider_callback(inst):
                    def slider_callback(sender, app_data, user_data):
                        agc_drivers[inst].set_threshold(app_data)
                    return slider_callback

                dpg.add_slider_int(
                    label         = "Threshold",
                    default_value = 1000,
                    min_value     = 0,
                    max_value     = 65535,
                    tag           = f"agc_{inst}_threshold",
                    callback      = make_slider_callback(inst)
                )
                def make_checkbox_callback(inst):
                    def checkbox_callback(sender, app_data, user_data):
                        if app_data:
                            agc_drivers[inst].enable()
                        else:
                            agc_drivers[inst].disable()
                    return checkbox_callback

                dpg.add_checkbox(
                    label    = "Enable",
                    tag      = f"agc_{inst}_enable",
                    callback = make_checkbox_callback(inst)
                )
                def make_autoclear_callback(inst):
                    def autoclear_callback(sender, app_data, user_data):
                        agc_auto_clear[inst] = bool(app_data)
                    return autoclear_callback

                dpg.add_checkbox(
                    label    = "Auto Clear",
                    tag      = f"agc_{inst}_autoclear",
                    callback = make_autoclear_callback(inst)
                )
                dpg.add_separator()

    # System Overview Window..
    with dpg.window(**get_window_kwargs("win_overview", "LiteX M2SDR System Overview", default_pos=(260, 0), default_size=(600, 400))):
        with dpg.node_editor():
            # Host Node
            host_node = dpg.add_node(label="Host", draggable=True)
            with dpg.node_attribute(label="PCIe RX", attribute_type=dpg.mvNode_Attr_Output, parent=host_node) as host_tx_data:
                dpg.add_text("PCIe RX")
            with dpg.node_attribute(label="PCIe TX", attribute_type=dpg.mvNode_Attr_Output, parent=host_node) as host_rx_data:
                dpg.add_text("PCIe TX")
            dpg.set_item_pos(host_node, [10, 10])

            # LitePCIe Node
            litepcie_node = dpg.add_node(label="LitePCIe Core", draggable=True)
            with dpg.node_attribute(label="PCIe RX", attribute_type=dpg.mvNode_Attr_Input, parent=litepcie_node) as dma_host_tx:
                dpg.add_text("PCIe RX")
            with dpg.node_attribute(label="PCIe TX", attribute_type=dpg.mvNode_Attr_Input, parent=litepcie_node) as dma_host_rx:
                dpg.add_text("PCIe TX")
            with dpg.node_attribute(label="DMA TX", attribute_type=dpg.mvNode_Attr_Output, parent=litepcie_node) as dma_fpga_tx:
                dpg.add_text("                   DMA TX")
            with dpg.node_attribute(label="DMA RX", attribute_type=dpg.mvNode_Attr_Output, parent=litepcie_node) as dma_fpga_rx:
                dpg.add_text("                   DMA RX")
            with dpg.node_attribute(attribute_type=dpg.mvNode_Attr_Static, parent=litepcie_node):
                dpg.add_text("Writer Loops/s:       0.00", tag="node_dma_writer_loops")
                dpg.add_text("Reader Loops/s:       0.00", tag="node_dma_reader_loops")
                dpg.add_text("Writer Loops:         0",    tag="dma_writer_loops_count")
                dpg.add_text("Writer Count:         0",    tag="dma_writer_count")
                dpg.add_text("Reader Loops:         0",    tag="dma_reader_loops_count")
                dpg.add_text("Reader Count:         0",    tag="dma_reader_count")
                dpg.add_text("Loopback:             0",    tag="node_dma_loopback")
                dpg.add_text("Sync Bypass:          0",    tag="node_dma_sync_bypass")
                dpg.add_text("Sync Enable:          0",    tag="node_dma_sync_enable")
                dpg.add_text("Writer Enable:        0",    tag="node_dma_writer_enable")
                dpg.add_text("Reader Enable:        0",    tag="node_dma_reader_enable")
            dpg.set_item_pos(litepcie_node, [100, 10])

            # RFIC Node
            rfic_node = dpg.add_node(label="AD9361 RFIC Core", draggable=True)
            with dpg.node_attribute(label="RF TX", attribute_type=dpg.mvNode_Attr_Input, parent=rfic_node) as rfic_rf_tx:
                dpg.add_text("RF TX")
            with dpg.node_attribute(label="RF RX", attribute_type=dpg.mvNode_Attr_Input, parent=rfic_node) as rfic_rf_rx:
                dpg.add_text("RF RX")
            with dpg.node_attribute(attribute_type=dpg.mvNode_Attr_Static, parent=rfic_node):
                dpg.add_text("Clock Freq:     0.00 MHz", tag="rfic_clock_freq")
                dpg.add_text("Sample Rate:    0.00 MHz", tag="rfic_sample_rate")
                dpg.add_text("RX1 Low AGC:         0",   tag="agc_rx1_low")
                dpg.add_text("RX1 High AGC:        0",   tag="agc_rx1_high")
                dpg.add_text("RX2 Low AGC:         0",   tag="agc_rx2_low")
                dpg.add_text("RX2 High AGC:        0",   tag="agc_rx2_high")
            dpg.set_item_pos(rfic_node, [350, 10])

            # Create Static Links.
            dpg.add_node_link(host_tx_data,  dma_host_tx)
            dpg.add_node_link(dma_fpga_tx,   rfic_rf_tx)
            dpg.add_node_link(rfic_rf_rx,    dma_fpga_rx)
            dpg.add_node_link(dma_host_rx,   host_rx_data)

    # GUI Timer Callback.
    writer_prev_loops = 0
    writer_prev_time = time.time()
    reader_prev_loops = 0
    reader_prev_time = time.time()
    stop_event = threading.Event()
    snapshot_lock = threading.Lock()
    shared_snapshot = {"error": None}

    def collect_snapshot():
        nonlocal writer_prev_loops, writer_prev_time
        nonlocal reader_prev_loops, reader_prev_time

        last_refresh = None
        local_xadc = {}

        while not stop_event.is_set():
            try:
                with ui_state_lock:
                    refresh = ui_state["refresh"]

                if with_xadc and xadc_driver and (last_refresh is None or abs(refresh - last_refresh) > 1e-9):
                    xadc_points = max(2, int(XADC_WINDOW_DURATION / refresh))
                    local_xadc = {
                        "temp":    xadc_driver.gen_data("temp", n=xadc_points),
                        "vccint":  xadc_driver.gen_data("vccint", n=xadc_points),
                        "vccaux":  xadc_driver.gen_data("vccaux", n=xadc_points),
                        "vccbram": xadc_driver.gen_data("vccbram", n=xadc_points),
                    }
                    last_refresh = refresh

                now = time.time()
                snap = {"error": None, "csr": {}, "xadc": {}, "clks": {}, "time": None, "headers": None, "dma": None, "agc": {}}

                if clear_counters_event.is_set():
                    for inst in rf_agc_instances:
                        agc_drivers[inst].clear()
                    clear_counters_event.clear()

                # Snapshot CSR registers.
                for name, reg in bus.regs.__dict__.items():
                    snap["csr"][name] = reg.read()

                # Snapshot XADC data.
                if with_xadc and xadc_driver:
                    relative_now = now - start_time
                    for name, gen_data in local_xadc.items():
                        datay    = next(gen_data)
                        n_points = len(datay)
                        datax    = [relative_now - (n_points - 1 - i) * refresh for i in range(n_points)]
                        snap["xadc"][name] = [datax, datay[::-1]]

                # Snapshot clocks.
                if with_clks and clk_drivers:
                    for clk, driver in clk_drivers.items():
                        snap["clks"][clk] = driver.update()

                # Snapshot time.
                if with_time and time_driver:
                    current_time_ns = time_driver.read_ns()
                    snap["time"] = {
                        "ns": current_time_ns,
                        "str": unix_to_datetime(current_time_ns),
                    }

                # Snapshot headers.
                if with_header_reg:
                    tx_header    = header_driver.last_tx_header.read()
                    rx_header    = header_driver.last_rx_header.read()
                    tx_timestamp = header_driver.last_tx_timestamp.read()
                    rx_timestamp = header_driver.last_rx_timestamp.read()
                    snap["headers"] = {
                        "tx_header": tx_header,
                        "rx_header": rx_header,
                        "tx_timestamp": tx_timestamp,
                        "rx_timestamp": rx_timestamp,
                        "tx_datetime": unix_to_datetime(tx_timestamp) if tx_timestamp else "N/A",
                        "rx_datetime": unix_to_datetime(rx_timestamp) if rx_timestamp else "N/A",
                    }

                # Snapshot DMA info.
                if hasattr(bus.regs, "pcie_dma0_writer_enable"):
                    writer_enable = bus.regs.pcie_dma0_writer_enable.read()
                    writer_table_level = bus.regs.pcie_dma0_writer_table_level.read()
                    loops_w = (writer_table_level >> 16) & 0xFFFF
                    count_w = writer_table_level & 0xFFFF
                    writer_loop_status = bus.regs.pcie_dma0_writer_table_loop_status.read()
                    loops_ws = (writer_loop_status >> 16) & 0xFFFF
                    count_ws = writer_loop_status & 0xFFFF

                    loops_diff = loops_ws - writer_prev_loops
                    time_diff = now - writer_prev_time
                    writer_speed = 0.0
                    if time_diff > 0 and loops_diff >= 0:
                        writer_speed = loops_diff / time_diff
                    writer_prev_loops = loops_ws
                    writer_prev_time = now

                    reader_enable = bus.regs.pcie_dma0_reader_enable.read()
                    reader_table_level = bus.regs.pcie_dma0_reader_table_level.read()
                    loops_r = (reader_table_level >> 16) & 0xFFFF
                    count_r = reader_table_level & 0xFFFF
                    reader_loop_status = bus.regs.pcie_dma0_reader_table_loop_status.read()
                    loops_rs = (reader_loop_status >> 16) & 0xFFFF
                    count_rs = reader_loop_status & 0xFFFF

                    loops_diff_r = loops_rs - reader_prev_loops
                    time_diff_r = now - reader_prev_time
                    reader_speed = 0.0
                    if time_diff_r > 0 and loops_diff_r >= 0:
                        reader_speed = loops_diff_r / time_diff_r
                    reader_prev_loops = loops_rs
                    reader_prev_time = now

                    snap["dma"] = {
                        "writer_enable": writer_enable,
                        "reader_enable": reader_enable,
                        "writer_table_level": (loops_w, count_w),
                        "writer_loop_status": (loops_ws, count_ws),
                        "reader_table_level": (loops_r, count_r),
                        "reader_loop_status": (loops_rs, count_rs),
                        "writer_speed": writer_speed,
                        "reader_speed": reader_speed,
                        "loopback_enable": bus.regs.pcie_dma0_loopback_enable.read(),
                        "sync_bypass": bus.regs.pcie_dma0_synchronizer_bypass.read(),
                        "sync_enable": bus.regs.pcie_dma0_synchronizer_enable.read(),
                    }

                # Snapshot AGC.
                for inst in rf_agc_instances:
                    count = agc_drivers[inst].read_count()
                    snap["agc"][inst] = count
                    if agc_auto_clear.get(inst, False):
                        agc_drivers[inst].clear()

                with snapshot_lock:
                    shared_snapshot.clear()
                    shared_snapshot.update(snap)
            except Exception as e:
                with snapshot_lock:
                    shared_snapshot.clear()
                    shared_snapshot.update({"error": str(e)})
            time.sleep(refresh)

    timer_thread = threading.Thread(target=collect_snapshot, daemon=True)
    timer_thread.start()

    dpg.show_viewport()

    # Recover windows restored outside visible area (multi-screen/layout changes).
    viewport_w, viewport_h = 1920, 1080
    for tag, pos in default_window_pos.items():
        if dpg.does_item_exist(tag):
            x, y = dpg.get_item_pos(tag)
            if (x < 0) or (y < 0) or (x > viewport_w - 80) or (y > viewport_h - 80):
                dpg.set_item_pos(tag, list(pos))

    agc_prev_counts = {}
    try:
        while dpg.is_dearpygui_running():
            with snapshot_lock:
                snap = copy.deepcopy(shared_snapshot)
            with ui_state_lock:
                freeze_plots = ui_state["freeze_plots"]

            if snap.get("error"):
                dpg.set_value("status_error_text", f"Error: {snap['error']}")
                print(f"Dashboard update error: {snap['error']}")
            else:
                dpg.set_value("status_error_text", "")
                # Update CSR Registers.
                for name, value in snap.get("csr", {}).items():
                    dpg.set_value(item=name, value=f"0x{value:x}")

                # Update XADC data.
                if not freeze_plots:
                    for name, series in snap.get("xadc", {}).items():
                        dpg.set_value(name, series)
                        dpg.set_item_label(name, name)
                        dpg.set_axis_limits_auto(f"{name}_x")
                        dpg.fit_axis_data(f"{name}_x")

                # Update Clock Frequencies and Sample Rate.
                clks = snap.get("clks", {})
                if with_clks and clks:
                    for clk, freq in clks.items():
                        dpg.set_value(f"clock_{clk}_freq", f"{freq:.2f}")
                    ref_clk_freq = clks.get("clk2", 0.0)
                    data_clk_freq = clks.get("clk3", 0.0)
                    sample_rate = data_clk_freq / 2
                    dpg.set_value("rfic_clock_freq", f"Clock Freq: {f'{ref_clk_freq:.2f}'.rjust(8)} MHz")
                    dpg.set_value("rfic_sample_rate", f"Sample Rate: {f'{sample_rate:.2f}'.rjust(8)} MHz")

                # Update Time.
                if snap.get("time"):
                    dpg.set_value("time_ns", f"Time: {snap['time']['ns']} ns")
                    dpg.set_value("time_str", f"Date/Time: {snap['time']['str']}")

                # Update DMA Header & Timestamps.
                if snap.get("headers"):
                    h = snap["headers"]
                    dpg.set_value("dma_tx_header", f"{h['tx_header']:016x}")
                    dpg.set_value("dma_tx_timestamp", f"{h['tx_timestamp']:016x}")
                    dpg.set_value("dma_tx_datetime", f"{h['tx_datetime']}")
                    dpg.set_value("dma_rx_header", f"{h['rx_header']:016x}")
                    dpg.set_value("dma_rx_timestamp", f"{h['rx_timestamp']:016x}")
                    dpg.set_value("dma_rx_datetime", f"{h['rx_datetime']}")

                # Update DMA Info and Nodes.
                if snap.get("dma"):
                    d = snap["dma"]
                    dpg.set_value("dma_writer_enable", str(d["writer_enable"]))
                    dpg.set_value("node_dma_writer_enable", f"Writer Enable: {str(d['writer_enable']).rjust(1)}")
                    loops_w, count_w = d["writer_table_level"]
                    loops_ws, count_ws = d["writer_loop_status"]
                    loops_r, count_r = d["reader_table_level"]
                    loops_rs, count_rs = d["reader_loop_status"]
                    dpg.set_value("dma_writer_table_level", f"Loops: {loops_w}, Count: {count_w}")
                    dpg.set_value("dma_writer_table_loop_status", f"Loops: {loops_ws}, Count: {count_ws}")
                    dpg.set_value("dma_writer_loops_speed", f"{d['writer_speed']:.2f}")
                    writer_speed_str = f"{d['writer_speed']:.2f}".rjust(8)
                    dpg.set_value("node_dma_writer_loops", f"Writer Loops/s: {writer_speed_str}")
                    dpg.set_value("dma_writer_loops_count", f"Writer Loops: {str(loops_w).rjust(8)}")
                    dpg.set_value("dma_writer_count", f"Writer Count: {str(count_w).rjust(8)}")

                    dpg.set_value("dma_reader_enable", str(d["reader_enable"]))
                    dpg.set_value("node_dma_reader_enable", f"Reader Enable: {str(d['reader_enable']).rjust(1)}")
                    dpg.set_value("dma_reader_table_level", f"Loops: {loops_r}, Count: {count_r}")
                    dpg.set_value("dma_reader_table_loop_status", f"Loops: {loops_rs}, Count: {count_rs}")
                    dpg.set_value("dma_reader_loops_speed", f"{d['reader_speed']:.2f}")
                    reader_speed_str = f"{d['reader_speed']:.2f}".rjust(8)
                    dpg.set_value("node_dma_reader_loops", f"Reader Loops/s: {reader_speed_str}")
                    dpg.set_value("dma_reader_loops_count", f"Reader Loops: {str(loops_r).rjust(8)}")
                    dpg.set_value("dma_reader_count", f"Reader Count: {str(count_r).rjust(8)}")

                    dpg.set_value("dma_loopback_enable", str(d["loopback_enable"]))
                    dpg.set_value("node_dma_loopback", f"Loopback: {str(d['loopback_enable']).rjust(1)}")
                    dpg.set_value("dma_sync_bypass", str(d["sync_bypass"]))
                    dpg.set_value("node_dma_sync_bypass", f"Sync Bypass: {str(d['sync_bypass']).rjust(1)}")
                    dpg.set_value("dma_sync_enable", str(d["sync_enable"]))
                    dpg.set_value("node_dma_sync_enable", f"Sync Enable: {str(d['sync_enable']).rjust(1)}")

                    dma_enabled_state = "green" if d["writer_enable"] and d["reader_enable"] else ("yellow" if (d["writer_enable"] or d["reader_enable"]) else "red")
                    loopback_state = "green" if d["loopback_enable"] else "yellow"
                    sync_state = "green" if (d["sync_enable"] and not d["sync_bypass"]) else ("yellow" if d["sync_enable"] else "red")
                    set_status_badge("status_dma_enabled", dma_enabled_state, "DMA Enabled")
                    set_status_badge("status_loopback", loopback_state, "Loopback")
                    set_status_badge("status_sync", sync_state, "Synchronizer")

                # Update RF AGC Panel and RFIC Node.
                agc_increase = False
                agc_nonzero = False
                for inst, count in snap.get("agc", {}).items():
                    count_str = str(count).rjust(10)
                    dpg.set_value(f"agc_{inst}_count", f"Saturation Count: {count}")
                    dpg.set_value(f"agc_{inst}", f"{inst.upper()} AGC: {count_str}")
                    agc_nonzero |= (count > 0)
                    if count > agc_prev_counts.get(inst, count):
                        agc_increase = True
                    agc_prev_counts[inst] = count

                agc_state = "red" if agc_increase else ("yellow" if agc_nonzero else "green")
                set_status_badge("status_agc", agc_state, "AGC Saturation")

            dpg.render_dearpygui_frame()
    except KeyboardInterrupt:
        pass

    stop_event.set()
    timer_thread.join(timeout=1.0)
    with ui_state_lock:
        saved_settings = {
            "refresh": ui_state["refresh"],
            "freeze_plots": ui_state["freeze_plots"],
            "windows": {},
        }
    for tag in ["win_status", "win_registers", "win_clks_time", "win_dmas", "win_xadc", "win_rf_agc", "win_overview"]:
        if dpg.does_item_exist(tag):
            pos = dpg.get_item_pos(tag)
            saved_settings["windows"][tag] = {
                "pos": [int(pos[0]), int(pos[1])],
                "size": [int(dpg.get_item_width(tag)), int(dpg.get_item_height(tag))],
            }
    save_dashboard_settings(saved_settings)
    dpg.destroy_context()
    bus.close()

# Run ----------------------------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="LiteX M2SDR Dashboard",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--csr-csv", default="csr.csv", help="CSR configuration file")
    parser.add_argument("--host", default="localhost", help="Host IP address")
    parser.add_argument("--port", default="1234", help="Host bind port")
    args = parser.parse_args()

    run_gui(host=args.host, csr_csv=args.csr_csv, port=int(args.port))

if __name__ == "__main__":
    main()
