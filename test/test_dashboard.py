#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import time
import threading
import argparse

from litex import RemoteClient

# Constants ----------------------------------------------------------------------------------------

XADC_WINDOW_SECONDS = 10

# Define the clocks to measure.
CLOCKS = {
    "clk0": "Sys Clk",
    "clk1": "PCIe Clk",
    "clk2": "AD9361 Ref Clk",
    "clk3": "AD9361 Dat Clk",
    "clk4": "Time Ref Clk",
}

# Update interval (in seconds) for clock frequency measurement.
clock_update_interval = 1.0

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

    # Initialize clock measurement variables if available.
    if with_clks:
        def latch_all():
            for clk in CLOCKS:
                reg_name = f"clk_measurement_{clk}_latch"
                getattr(bus.regs, reg_name).write(1)
        def read_all():
            ret = {}
            for clk in CLOCKS:
                reg_name = f"clk_measurement_{clk}_value"
                ret[clk] = getattr(bus.regs, reg_name).read()
            return ret
        latch_all()
        prev_clk_values = read_all()
        prev_clk_time   = time.time()
        last_clk_update = prev_clk_time
    else:
        prev_clk_values = None
        prev_clk_time   = None
        last_clk_update = None

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

    if with_xadc:
        def get_xadc_temp():
            return bus.regs.xadc_temperature.read() * 503.975 / 4096 - 273.15

        def get_xadc_vccint():
            return bus.regs.xadc_vccint.read() * 3 / 4096

        def get_xadc_vccaux():
            return bus.regs.xadc_vccaux.read() * 3 / 4096

        def get_xadc_vccbram():
            return bus.regs.xadc_vccbram.read() * 3 / 4096

        def gen_xadc_data(get_cls, n):
            xadc_data = [get_cls()] * n
            while True:
                xadc_data.pop(-1)
                xadc_data.insert(0, get_cls())
                yield xadc_data

    # Create Main Window.
    dpg.create_context()
    dpg.create_viewport(title="LiteX M2SDR Dashboard", width=1920, height=1080, always_on_top=True)
    dpg.setup_dearpygui()

    # CSR Registers Window.
    with dpg.window(label="LiteX M2SDR CSR Registers", autosize=True):
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

    # Peripherals Window.
    with dpg.window(label="LiteX M2SDR Peripherals", autosize=True, pos=(550, 0)):
        dpg.add_text("SoC")
        dpg.add_button(label="Reboot", callback=reboot)
        if with_identifier:
            dpg.add_text(f"Identifier: {get_identifier()}")

    # Clocks Window.
    if with_clks:
        with dpg.window(label="LiteX M2SDR Clocks", autosize=True, pos=(550, 300)):
            # Create a text widget for each clock.
            for clk, desc in CLOCKS.items():
                dpg.add_text(f"{desc}: -- MHz", tag=f"clock_{clk}")

    # XADC Window.
    if with_xadc:
        with dpg.window(label="LiteX M2SDR XADC", width=600, height=600, pos=(1000, 0)):
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

    # GUI Timer Callback.
    def timer_callback(refresh=0.1):
        nonlocal last_clk_update, prev_clk_values, prev_clk_time
        if with_xadc:
            xadc_points = int(XADC_WINDOW_SECONDS / refresh)
            temp_gen    = gen_xadc_data(get_xadc_temp, n=xadc_points)
            vccint_gen  = gen_xadc_data(get_xadc_vccint, n=xadc_points)
            vccaux_gen  = gen_xadc_data(get_xadc_vccaux, n=xadc_points)
            vccbram_gen = gen_xadc_data(get_xadc_vccbram, n=xadc_points)

        while dpg.is_dearpygui_running():
            # Update CSR Registers.
            for name, reg in bus.regs.__dict__.items():
                dpg.set_value(item=name, value=f"0x{reg.read():x}")

            # Update XADC data.
            if with_xadc:
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
                    dpg.set_value(name, [datax, datay])
                    dpg.set_item_label(name, name)
                    dpg.set_axis_limits_auto(f"{name}_x")
                    dpg.fit_axis_data(f"{name}_x")

            # Update Clock Frequencies.
            if with_clks:
                now = time.time()
                if now - last_clk_update >= clock_update_interval:
                    latch_all()
                    current_values = read_all()
                    elapsed = now - prev_clk_time
                    for clk in CLOCKS:
                        delta = current_values[clk] - prev_clk_values[clk]
                        freq = delta / (elapsed * 1e6)  # Frequency in MHz.
                        dpg.set_value(f"clock_{clk}", f"{CLOCKS[clk]}: {freq:.2f} MHz")
                    prev_clk_values = current_values
                    prev_clk_time = now
                    last_clk_update = now

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
        description     = "LiteX M2SDR Dashboard",
        formatter_class = argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--csr-csv", default="csr.csv",   help="CSR configuration file")
    parser.add_argument("--host",    default="localhost", help="Host IP address")
    parser.add_argument("--port",    default="1234",      help="Host bind port")
    args = parser.parse_args()

    run_gui(host=args.host, csr_csv=args.csr_csv, port=int(args.port))

if __name__ == "__main__":
    main()
