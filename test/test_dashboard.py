#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import time
import threading
import argparse

from litex import RemoteClient

from test_clks   import ClkDriver, CLOCKS
from test_xadc   import XADCDriver
from test_header import HeaderDriver
from test_time   import TimeDriver, unix_to_datetime
from test_agc    import AGCDriver

# Constants ----------------------------------------------------------------------------------------

XADC_WINDOW_DURATION = 10

# GUI ----------------------------------------------------------------------------------------------

def run_gui(host="localhost", csr_csv="csr.csv", port=1234):
    import dearpygui.dearpygui as dpg

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

    # Create Main Window.
    main_title = f"LiteX M2SDR Dashboard on '{get_identifier()[:-1]}'"
    dpg.create_context()
    dpg.create_viewport(title=main_title, width=1920, height=1080, always_on_top=True)
    dpg.setup_dearpygui()

    # Registers Window.
    with dpg.window(label="LiteX M2SDR Registers", autosize=True, pos=(1230, 0)):
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
    with dpg.window(label="LiteX M2SDR Clks/Time", autosize=True, pos=(0, 0)):
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
        with dpg.window(label="LiteX M2SDR DMAs", autosize=True, pos=(0, 450)):
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
        with dpg.window(label="LiteX M2SDR XADC", width=600, height=500, pos=(625, 510)):
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
    with dpg.window(label="LiteX M2SDR RF AGC", autosize=True, pos=(935, 0)):
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
    with dpg.window(label="LiteX M2SDR System Overview", width=600, height=400, pos=(260, 0)):
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

    def timer_callback(refresh=0.1):
        if with_xadc and xadc_driver:
            xadc_points = int(XADC_WINDOW_DURATION / refresh)
            temp_gen    = xadc_driver.gen_data("temp", n=xadc_points)
            vccint_gen  = xadc_driver.gen_data("vccint", n=xadc_points)
            vccaux_gen  = xadc_driver.gen_data("vccaux", n=xadc_points)
            vccbram_gen = xadc_driver.gen_data("vccbram", n=xadc_points)

        nonlocal writer_prev_loops, writer_prev_time
        nonlocal reader_prev_loops, reader_prev_time

        while dpg.is_dearpygui_running():
            # Update CSR Registers.
            for name, reg in bus.regs.__dict__.items():
                dpg.set_value(item=name, value=f"0x{reg.read():x}")

            # Update XADC data.
            if with_xadc and xadc_driver:
                now = time.time()
                relative_now = now - start_time
                for name, gen_data in [
                    ("temp",    temp_gen),
                    ("vccint",  vccint_gen),
                    ("vccaux",  vccaux_gen),
                    ("vccbram", vccbram_gen)
                ]:
                    datay    = next(gen_data)
                    n_points = len(datay)
                    datax    = [relative_now - (n_points - 1 - i) * refresh for i in range(n_points)]
                    dpg.set_value(name, [datax, datay[::-1]])
                    dpg.set_item_label(name, name)
                    dpg.set_axis_limits_auto(f"{name}_x")
                    dpg.fit_axis_data(f"{name}_x")

            # Update Clock Frequencies and Sample Rate.
            if with_clks and clk_drivers:
                for clk, driver in clk_drivers.items():
                    freq = driver.update()
                    dpg.set_value(f"clock_{clk}_freq", f"{freq:.2f}")
                # Update RFIC clock frequency.
                ref_clk_freq     = clk_drivers["clk2"].update()
                ref_clk_freq_str = f"{ref_clk_freq:.2f}".rjust(8)
                data_clk_freq    = clk_drivers["clk3"].update()
                sample_rate      = data_clk_freq / 2
                sample_rate_str  = f"{sample_rate:.2f}".rjust(8)
                dpg.set_value("rfic_clock_freq", f"Clock Freq: {ref_clk_freq_str} MHz")
                dpg.set_value("rfic_sample_rate", f"Sample Rate: {sample_rate_str} MHz")

            # Update Time.
            if with_time and time_driver:
                current_time_ns = time_driver.read_ns()
                time_str = unix_to_datetime(current_time_ns)
                dpg.set_value("time_ns", f"Time: {current_time_ns} ns")
                dpg.set_value("time_str", f"Date/Time: {time_str}")

            # Update DMA Header & Timestamps.
            if with_header_reg:
                tx_header    = header_driver.last_tx_header.read()
                rx_header    = header_driver.last_rx_header.read()
                tx_timestamp = header_driver.last_tx_timestamp.read()
                rx_timestamp = header_driver.last_rx_timestamp.read()
                tx_datetime  = unix_to_datetime(tx_timestamp) if tx_timestamp else "N/A"
                rx_datetime  = unix_to_datetime(rx_timestamp) if rx_timestamp else "N/A"
                dpg.set_value("dma_tx_header", f"{tx_header:016x}")
                dpg.set_value("dma_tx_timestamp", f"{tx_timestamp:016x}")
                dpg.set_value("dma_tx_datetime", f"{tx_datetime}")
                dpg.set_value("dma_rx_header", f"{rx_header:016x}")
                dpg.set_value("dma_rx_timestamp", f"{rx_timestamp:016x}")
                dpg.set_value("dma_rx_datetime", f"{rx_datetime}")

            # Update DMA Info and Nodes.
            if hasattr(bus.regs, "pcie_dma0_writer_enable"):
                now = time.time()
                writer_enable = bus.regs.pcie_dma0_writer_enable.read()
                dpg.set_value("dma_writer_enable", str(writer_enable))
                dpg.set_value("node_dma_writer_enable", f"Writer Enable: {str(writer_enable).rjust(1)}")
                writer_table_level = bus.regs.pcie_dma0_writer_table_level.read()
                loops_w = (writer_table_level >> 16) & 0xFFFF
                count_w = writer_table_level & 0xFFFF
                dpg.set_value("dma_writer_table_level", f"Loops: {loops_w}, Count: {count_w}")
                writer_loop_status = bus.regs.pcie_dma0_writer_table_loop_status.read()
                loops_ws = (writer_loop_status >> 16) & 0xFFFF
                count_ws = writer_loop_status & 0xFFFF
                dpg.set_value("dma_writer_table_loop_status", f"Loops: {loops_ws}, Count: {count_ws}")

                # Compute Writer Loops/s and update node.
                loops_diff = loops_ws - writer_prev_loops
                time_diff = now - writer_prev_time
                writer_speed = 0.0
                if time_diff > 0 and loops_diff >= 0:
                    writer_speed = loops_diff / time_diff
                writer_speed_str = f"{writer_speed:.2f}".rjust(8)
                dpg.set_value("dma_writer_loops_speed", f"{writer_speed:.2f}")
                dpg.set_value("node_dma_writer_loops", f"Writer Loops/s: {writer_speed_str}")
                dpg.set_value("dma_writer_loops_count", f"Writer Loops: {str(loops_w).rjust(8)}")
                dpg.set_value("dma_writer_count", f"Writer Count: {str(count_w).rjust(8)}")
                writer_prev_loops = loops_ws
                writer_prev_time = now

                reader_enable = bus.regs.pcie_dma0_reader_enable.read()
                dpg.set_value("dma_reader_enable", str(reader_enable))
                dpg.set_value("node_dma_reader_enable", f"Reader Enable: {str(reader_enable).rjust(1)}")
                reader_table_level = bus.regs.pcie_dma0_reader_table_level.read()
                loops_r = (reader_table_level >> 16) & 0xFFFF
                count_r = reader_table_level & 0xFFFF
                dpg.set_value("dma_reader_table_level", f"Loops: {loops_r}, Count: {count_r}")
                reader_loop_status = bus.regs.pcie_dma0_reader_table_loop_status.read()
                loops_rs = (reader_loop_status >> 16) & 0xFFFF
                count_rs = reader_loop_status & 0xFFFF
                dpg.set_value("dma_reader_table_loop_status", f"Loops: {loops_rs}, Count: {count_rs}")

                # Compute Reader Loops/s and update node.
                loops_diff_r = loops_rs - reader_prev_loops
                time_diff_r = now - reader_prev_time
                reader_speed = 0.0
                if time_diff_r > 0 and loops_diff_r >= 0:
                    reader_speed = loops_diff_r / time_diff_r
                reader_speed_str = f"{reader_speed:.2f}".rjust(8)
                dpg.set_value("dma_reader_loops_speed", f"{reader_speed:.2f}")
                dpg.set_value("node_dma_reader_loops", f"Reader Loops/s: {reader_speed_str}")
                dpg.set_value("dma_reader_loops_count", f"Reader Loops: {str(loops_r).rjust(8)}")
                dpg.set_value("dma_reader_count", f"Reader Count: {str(count_r).rjust(8)}")
                reader_prev_loops = loops_rs
                reader_prev_time = now

                # Update Loopback and Synchronizer status in node.
                loopback_enable = bus.regs.pcie_dma0_loopback_enable.read()
                dpg.set_value("dma_loopback_enable", str(loopback_enable))
                dpg.set_value("node_dma_loopback", f"Loopback: {str(loopback_enable).rjust(1)}")

                sync_bypass = bus.regs.pcie_dma0_synchronizer_bypass.read()
                dpg.set_value("dma_sync_bypass", str(sync_bypass))
                dpg.set_value("node_dma_sync_bypass", f"Sync Bypass: {str(sync_bypass).rjust(1)}")

                sync_enable = bus.regs.pcie_dma0_synchronizer_enable.read()
                dpg.set_value("dma_sync_enable", str(sync_enable))
                dpg.set_value("node_dma_sync_enable", f"Sync Enable: {str(sync_enable).rjust(1)}")

            # Update RF AGC Panel and RFIC Node.
            for inst in ["rx1_low", "rx1_high", "rx2_low", "rx2_high"]:
                count = agc_drivers[inst].read_count()
                count_str = str(count).rjust(10)
                dpg.set_value(f"agc_{inst}_count", f"Saturation Count: {count}")
                dpg.set_value(f"agc_{inst}", f"{inst.upper()} AGC: {count_str}")
                if agc_auto_clear.get(inst, False):
                    agc_drivers[inst].clear()

            time.sleep(refresh)

    timer_thread = threading.Thread(target=timer_callback)
    timer_thread.start()

    dpg.show_viewport()
    try:
        while dpg.is_dearpygui_running():
            dpg.render_dearpygui_frame()
    except KeyboardInterrupt:
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
