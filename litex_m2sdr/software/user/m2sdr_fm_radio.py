#!/usr/bin/env python3
#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import os
import time
import signal
import argparse
import threading
import subprocess

import numpy as np

# Constants ----------------------------------------------------------------------------------------

IQ_SAMPLERATE   = 1_000_000
AUDIO_RATE      = 44_100
BLOCK_SIZE      = 1024
WAVEFORM_LEN    = 512

CHAN_DEFAULT    = "1t1r"
FREQ_STEP_MHZ   = 0.1

FREQ_MIN_MHZ    = 88.0
FREQ_MAX_MHZ    = 108.0
GAIN_MIN_DB     = 0
GAIN_MAX_DB     = 73

FM_DEVIATION    = 75_000     # -d 75000
FM_BW_KHZ       = 12         # -b 12

DEBOUNCE_DELAY  = 0.5

# GUI ----------------------------------------------------------------------------------------------

def run_gui(
    samplerate        = IQ_SAMPLERATE,
    audio_rate        = AUDIO_RATE,
    default_freq_mhz  = 100.0,
    default_volume    = 1.0,
    default_gain_db   = 0,
    fm_region         = "eu",
):
    import dearpygui.dearpygui as dpg

    # State ----------------------------------------------------------------------------------------

    pcm_proc         = None   # m2sdr_record | m2sdr_fm_rx (stdout = PCM s16le)
    ffmpeg_proc      = None   # ffmpeg sink to ALSA (stdin = PCM)
    pipe_thread      = None
    running          = False

    volume_target    = float(max(0.0, min(1.0, default_volume)))
    volume_current   = volume_target

    current_mode     = "stereo"
    current_channels = 2

    current_freq_mhz = float(max(FREQ_MIN_MHZ, min(FREQ_MAX_MHZ, default_freq_mhz)))
    current_gain_db  = int(max(GAIN_MIN_DB, min(GAIN_MAX_DB, default_gain_db)))

    rf_dirty         = False
    last_change_time = 0.0

    waveform_data    = [0.0] * WAVEFORM_LEN
    dragging_title   = False

    # Helpers --------------------------------------------------------------------------------------

    def _clamp(v, lo, hi):
        return lo if v < lo else hi if v > hi else v

    def _rf_update_now():
        rx_freq_hz = int(current_freq_mhz * 1e6)
        cmd = [
            "./m2sdr_rf",
            "-samplerate", str(samplerate),
            "-rx_freq",    str(rx_freq_hz),
            "-rx_gain",    str(current_gain_db),
            "-chan",       str(CHAN_DEFAULT),
        ]
        try:
            subprocess.run(cmd, check=False)
        except Exception:
            pass

    def _spawn_pcm_proc():
        fm_cmd = (
            f"./m2sdr_record -q - | "
            f"./m2sdr_fm_rx -s {samplerate} -d {FM_DEVIATION} -b {FM_BW_KHZ} -e {fm_region} "
            f"-m {current_mode} - -"
        )
        try:
            return subprocess.Popen(
                fm_cmd,
                shell      = True,
                stdout     = subprocess.PIPE,
                bufsize    = 0,
                preexec_fn = os.setsid,
            )
        except Exception:
            return None

    def _spawn_ffmpeg_proc():
        args = [
            "ffmpeg",
            "-hide_banner", "-loglevel", "error", "-nostats",
            "-f", "s16le",
            "-ac", str(current_channels),
            "-ar", str(audio_rate),
            "-i", "-",
            "-f", "alsa", "default",
        ]
        try:
            return subprocess.Popen(
                args,
                stdin      = subprocess.PIPE,
                bufsize    = 0,
                preexec_fn = os.setsid,
            )
        except Exception:
            return None

    def _start_reception():
        nonlocal pcm_proc, ffmpeg_proc, pipe_thread, running, current_channels
        if running:
            return

        _rf_update_now()
        current_channels = 2 if current_mode == "stereo" else 1

        pcm_proc    = _spawn_pcm_proc()
        ffmpeg_proc = _spawn_ffmpeg_proc()
        if not (pcm_proc and ffmpeg_proc and pcm_proc.stdout and ffmpeg_proc.stdin):
            _stop_reception()
            return

        running     = True
        pipe_thread = threading.Thread(target=_pipe_worker, daemon=True)
        pipe_thread.start()

    def _stop_reception():
        nonlocal pcm_proc, ffmpeg_proc, pipe_thread, running
        running = False

        if pipe_thread and pipe_thread.is_alive():
            pipe_thread.join(timeout=1.0)
        pipe_thread = None

        if ffmpeg_proc:
            try:  os.killpg(ffmpeg_proc.pid, signal.SIGTERM)
            except Exception:
                try:  ffmpeg_proc.terminate()
                except Exception:  pass
            try:  ffmpeg_proc.wait(timeout=2)
            except Exception:
                try:  os.killpg(ffmpeg_proc.pid, signal.SIGKILL)
                except Exception:  pass
            ffmpeg_proc = None

        if pcm_proc:
            try:  os.killpg(pcm_proc.pid, signal.SIGTERM)
            except Exception:
                try:  pcm_proc.terminate()
                except Exception:  pass
            try:  pcm_proc.wait(timeout=2)
            except Exception:
                try:  os.killpg(pcm_proc.pid, signal.SIGKILL)
                except Exception:  pass
            pcm_proc = None

    def _pipe_worker():
        nonlocal volume_current
        bytes_per_samp = 2
        frame_bytes    = current_channels * bytes_per_samp
        read_size      = max(BLOCK_SIZE * frame_bytes, WAVEFORM_LEN * frame_bytes)

        try:
            while running and pcm_proc and ffmpeg_proc and pcm_proc.stdout and ffmpeg_proc.stdin:
                raw = pcm_proc.stdout.read(read_size)
                if not raw:
                    break

                usable = len(raw) - (len(raw) % frame_bytes)
                if usable <= 0:
                    continue

                s = np.frombuffer(raw[:usable], dtype=np.int16).astype(np.float32) / 32768.0
                n = usable // frame_bytes
                s = s.reshape(n, current_channels, order="C")

                # Smooth volume ramp to avoid clicks.
                vt = float(volume_target)
                if abs(volume_current - vt) < 1e-6:
                    s *= volume_current
                else:
                    ramp = np.linspace(volume_current, vt, num=n, dtype=np.float32)[:, None]
                    s *= ramp
                    volume_current = vt

                # Waveform (post-gain), first channel only.
                try:
                    seg = s[:WAVEFORM_LEN, 0].flatten()
                    waveform_data[:len(seg)] = seg.tolist()
                    if len(seg) < WAVEFORM_LEN:
                        waveform_data[len(seg):] = [0.0] * (WAVEFORM_LEN - len(seg))
                    dpg.set_value("waveform", waveform_data)
                except Exception:
                    pass

                s   = np.clip(s, -1.0, 1.0)
                out = (s * 32767.0).astype(np.int16).tobytes()
                try:
                    ffmpeg_proc.stdin.write(out)
                except Exception:
                    break
        finally:
            try:
                if ffmpeg_proc and ffmpeg_proc.stdin:
                    ffmpeg_proc.stdin.close()
            except Exception:
                pass

    # Debounce Thread ------------------------------------------------------------------------------

    def _debounce_worker():
        nonlocal rf_dirty, last_change_time
        while True:
            now = time.time()
            if rf_dirty and (now - last_change_time) >= DEBOUNCE_DELAY:
                _rf_update_now()
                rf_dirty = False
            time.sleep(0.1)

    threading.Thread(target=_debounce_worker, daemon=True).start()

    # Signal Handling ------------------------------------------------------------------------------

    def _signal_handler(sig, frame):
        _stop_reception()
        try:
            dpg.stop_dearpygui()
        except Exception:
            pass

    signal.signal(signal.SIGINT, _signal_handler)

    # GUI Setup ------------------------------------------------------------------------------------

    dpg.create_context()
    dpg.create_viewport(
        title        = "M2 SDR FM Radio",
        width        = 520,
        height       = 250,
        small_icon   = "icon.ico",
        always_on_top= True,
    )
    dpg.setup_dearpygui()

    # Theme ----------------------------------------------------------------------------------------

    with dpg.theme() as green_theme:
        with dpg.theme_component(dpg.mvAll):
            dpg.add_theme_color(dpg.mvThemeCol_WindowBg,       (0,  0,   0, 255))
            dpg.add_theme_color(dpg.mvThemeCol_Text,           (0, 255,  0, 255))
            dpg.add_theme_color(dpg.mvThemeCol_Button,         (0,  64,  0, 255))
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered,  (0, 128,  0, 255))
            dpg.add_theme_color(dpg.mvThemeCol_ButtonActive,   (0, 192,  0, 255))
            dpg.add_theme_color(dpg.mvThemeCol_SliderGrab,     (0, 255,  0, 255))
            dpg.add_theme_color(dpg.mvThemeCol_FrameBg,        (20, 40, 20, 255))
            dpg.add_theme_color(dpg.mvThemeCol_FrameBgHovered, (0, 128,  0, 255))
            dpg.add_theme_color(dpg.mvThemeCol_FrameBgActive,  (0, 192,  0, 255))
            dpg.add_theme_color(dpg.mvThemeCol_PopupBg,        (0,  0,   0, 255))
            dpg.add_theme_color(dpg.mvThemeCol_PlotLines,      (0, 255,  0, 255))
            dpg.add_theme_style(dpg.mvStyleVar_WindowPadding, 10, 10)
            dpg.add_theme_style(dpg.mvStyleVar_ItemSpacing,    5,  5)

    # Fonts ----------------------------------------------------------------------------------------

    font_path   = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
    large_font  = None
    medium_font = None
    if os.path.exists(font_path):
        with dpg.font_registry():
            large_font  = dpg.add_font(font_path, 30)  # Frequency input
            medium_font = dpg.add_font(font_path, 24)  # Title

    # Drag-to-move (title only) --------------------------------------------------------------------

    def _titlebar_pressed(sender, app_data):
        nonlocal dragging_title
        dragging_title = True

    def _global_mouse_release(sender, app_data):
        nonlocal dragging_title
        dragging_title = False

    def _global_mouse_drag(sender, app_data):
        if not dragging_title:
            return
        dx, dy = app_data[1], app_data[2]
        vx, vy = dpg.get_viewport_pos()
        dpg.set_viewport_pos([vx + dx, vy + dy])

    # Callbacks ------------------------------------------------------------------------------------

    def _freq_changed(sender):
        nonlocal current_freq_mhz, rf_dirty, last_change_time
        val = float(dpg.get_value(sender))
        val = _clamp(val, FREQ_MIN_MHZ, FREQ_MAX_MHZ)
        current_freq_mhz = round(val * 10.0) / 10.0
        dpg.set_value(sender, current_freq_mhz)
        rf_dirty         = True
        last_change_time = time.time()

    def _gain_changed(sender):
        nonlocal current_gain_db, rf_dirty, last_change_time
        current_gain_db  = int(dpg.get_value(sender))
        rf_dirty         = True
        last_change_time = time.time()

    def _inc_freq():
        nonlocal current_freq_mhz, rf_dirty, last_change_time
        current_freq_mhz = min(current_freq_mhz + FREQ_STEP_MHZ, FREQ_MAX_MHZ)
        dpg.set_value("freq_display", current_freq_mhz)
        rf_dirty         = True
        last_change_time = time.time()

    def _dec_freq():
        nonlocal current_freq_mhz, rf_dirty, last_change_time
        current_freq_mhz = max(current_freq_mhz - FREQ_STEP_MHZ, FREQ_MIN_MHZ)
        dpg.set_value("freq_display", current_freq_mhz)
        rf_dirty         = True
        last_change_time = time.time()

    def _update_volume(sender, app_data):
        nonlocal volume_target
        volume_target = float(max(0.0, min(1.0, app_data)))

    def _toggle_reception():
        if not running:
            _start_reception()
            dpg.configure_item("toggle_btn", label="OFF")
        else:
            _stop_reception()
            dpg.configure_item("toggle_btn", label="ON")

    def _toggle_mode():
        nonlocal current_mode, current_channels
        current_mode     = "stereo" if current_mode == "mono" else "mono"
        current_channels = 2 if current_mode == "stereo" else 1
        dpg.configure_item("mode_btn", label=current_mode.upper())
        if running:
            _stop_reception()
            _start_reception()

    def _exit_app():
        _stop_reception()
        dpg.stop_dearpygui()

    # Windows --------------------------------------------------------------------------------------

    with dpg.window(label="M2 SDR FM Radio", width=520, height=250, no_title_bar=True, no_move=True) as main_window:
        dpg.bind_item_theme(main_window, green_theme)

        with dpg.group(horizontal=True, tag="title_row"):
            dpg.add_spacer(tag="title_spacer", width=0, height=1)
            dpg.add_text("M2 SDR FM Radio", tag="title_bar", color=(0, 255, 0, 255))
        if medium_font:
            dpg.bind_item_font("title_bar", medium_font)

        with dpg.child_window(width=500, height=48, border=False, no_scrollbar=True):
            with dpg.group(horizontal=True):
                dpg.add_spacer(width=40)
                dpg.add_button(tag="toggle_btn", label="ON", callback=_toggle_reception, width=70, height=40)
                dpg.add_button(label="-",                    callback=_dec_freq,         width=40, height=40)
                dpg.add_input_double(
                    tag           = "freq_display",
                    default_value = current_freq_mhz,
                    min_value     = FREQ_MIN_MHZ,
                    max_value     = FREQ_MAX_MHZ,
                    callback      = _freq_changed,
                    format        = "%.1f",
                    width         = 80,
                    step          = 0
                )
                dpg.add_button(label="+", callback=_inc_freq, width=40, height=40)
                dpg.add_text("FM")
                dpg.add_button(tag="mode_btn", label=current_mode.upper(), callback=_toggle_mode, width=80, height=40)
                dpg.add_button(label="Exit", callback=_exit_app, width=60, height=40)

        if large_font:
            dpg.bind_item_font("freq_display", large_font)

        dpg.add_slider_float(
            tag="vol_slider",
            default_value=volume_target, min_value=0.0, max_value=1.0,
            callback=_update_volume, width=500, format="Vol %.2f"
        )
        dpg.add_slider_int(
            tag="rx_gain",
            default_value=current_gain_db, min_value=GAIN_MIN_DB, max_value=GAIN_MAX_DB,
            callback=_gain_changed, width=500, format="Gain %d dB"
        )
        dpg.add_simple_plot(label="", tag="waveform", default_value=waveform_data, height=80, width=500)

    # Handlers -------------------------------------------------------------------------------------

    with dpg.item_handler_registry(tag="titlebar_handlers"):
        dpg.add_item_clicked_handler(callback=_titlebar_pressed, button=0)
    dpg.bind_item_handler_registry("title_bar", "titlebar_handlers")

    with dpg.handler_registry():
        dpg.add_mouse_drag_handler(button=0, callback=_global_mouse_drag)
        dpg.add_mouse_release_handler(button=0, callback=_global_mouse_release)

    # Main Loop ------------------------------------------------------------------------------------

    def _center_title():
        total_width = 500  # content width
        text_w, _   = dpg.get_text_size("M2 SDR FM Radio")
        dpg.configure_item("title_spacer", width=max(0, (total_width - text_w)//2))

    dpg.show_viewport()
    dpg.set_frame_callback(1, _center_title)
    try:
        dpg.start_dearpygui()
    finally:
        _stop_reception()
        dpg.destroy_context()

# Run ----------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description     = "LiteX M2SDR FM Radio (Simple GUI: m2sdr_record | m2sdr_fm_rx | ffmpeg ALSA)",
        formatter_class = argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--samplerate",      type=int,   default=IQ_SAMPLERATE, help="RF IQ samplerate (Hz)")
    parser.add_argument("--audio-rate",      type=int,   default=AUDIO_RATE,    help="Audio output samplerate (Hz)")
    parser.add_argument("--freq",            type=float, default=100.0,         help="Default FM frequency (MHz)")
    parser.add_argument("--volume",          type=float, default=1.0,           help="Default volume [0.0–1.0]")
    parser.add_argument("--gain",            type=int,   default=0,             help="Default RX gain (dB)")
    parser.add_argument("--fm-region",       type=str,   default="eu",          help="De-emphasis region (eu/us/jp)")
    args = parser.parse_args()

    run_gui(
        samplerate       = args.samplerate,
        audio_rate       = args.audio_rate,
        default_freq_mhz = args.freq,
        default_volume   = args.volume,
        default_gain_db  = args.gain,
        fm_region        = args.fm_region,
    )

if __name__ == "__main__":
    main()
