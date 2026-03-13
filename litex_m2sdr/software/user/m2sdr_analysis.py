#!/usr/bin/env python3

# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import json
import math
from pathlib import Path

import numpy as np


SUPPORTED_DTYPES = {
    "ci16_le": (np.int16, 32767.0),
    "ci8": (np.int8, 127.0),
    "cf32_le": (np.float32, 1.0),
}


def read_json(path):
    with Path(path).open("r", encoding="utf-8") as f:
        return json.load(f)


def write_json(path, data):
    with Path(path).open("w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")


def resolve_sigmf_pair(path):
    path = Path(path).resolve()
    if path.suffix == ".sigmf-meta":
        meta_path = path
    elif path.suffix == ".sigmf-data":
        meta_path = path.with_suffix(".sigmf-meta")
    else:
        meta_path = path.with_suffix(".sigmf-meta")
    meta = read_json(meta_path)
    dataset = meta.get("global", {}).get("core:dataset", meta_path.with_suffix(".sigmf-data").name)
    data_path = (meta_path.parent / dataset).resolve()
    return meta_path, data_path, meta


def load_complex_samples(sigmf_path, max_samples=None):
    meta_path, data_path, meta = resolve_sigmf_pair(sigmf_path)
    global_meta = meta.get("global", {})
    datatype = global_meta.get("core:datatype")
    if datatype not in SUPPORTED_DTYPES:
        raise ValueError(f"Unsupported datatype: {datatype}")
    dtype, scale = SUPPORTED_DTYPES[datatype]
    nchannels = int(global_meta.get("core:num_channels", 1))
    if nchannels < 1:
        raise ValueError("core:num_channels must be >= 1")

    raw = np.fromfile(data_path, dtype=dtype)
    if datatype.startswith("cf32"):
        words_per_complex = 2
    else:
        words_per_complex = 2
    stride = words_per_complex * nchannels
    if raw.size < stride:
        samples = np.empty((0, nchannels), dtype=np.complex64)
    else:
        raw = raw[: (raw.size // stride) * stride]
        if max_samples is not None:
            raw = raw[: max_samples * stride]
        reshaped = raw.reshape(-1, stride)
        channels = []
        for channel in range(nchannels):
            i = reshaped[:, channel * 2].astype(np.float32)
            q = reshaped[:, channel * 2 + 1].astype(np.float32)
            channels.append((i + 1j * q) / scale)
        samples = np.stack(channels, axis=1).astype(np.complex64, copy=False)

    return {
        "meta_path": meta_path,
        "data_path": data_path,
        "meta": meta,
        "sample_rate": float(global_meta.get("core:sample_rate", 0.0)),
        "center_freq": global_meta.get("core:frequency"),
        "datatype": datatype,
        "nchannels": nchannels,
        "full_scale": scale,
        "samples": samples,
    }


def clipping_rate(channel_data):
    if channel_data.size == 0:
        return 0.0
    return float(np.mean((np.abs(channel_data.real) >= 0.999) | (np.abs(channel_data.imag) >= 0.999)))


def crest_factor_db(channel_data):
    if channel_data.size == 0:
        return 0.0
    rms = np.sqrt(np.mean(np.abs(channel_data) ** 2))
    if rms == 0:
        return 0.0
    return float(20.0 * np.log10(np.max(np.abs(channel_data)) / rms))


def estimate_frequency_hz(channel_data, sample_rate):
    if channel_data.size < 2 or sample_rate <= 0:
        return 0.0
    phase = np.angle(np.mean(np.conj(channel_data[:-1]) * channel_data[1:]))
    return float(phase * sample_rate / (2.0 * math.pi))


def estimate_iq_imbalance(channel_data):
    if channel_data.size == 0:
        return {"gain_db": 0.0, "phase_deg": 0.0}
    i = channel_data.real
    q = channel_data.imag
    rms_i = np.sqrt(np.mean(i**2))
    rms_q = np.sqrt(np.mean(q**2))
    gain_db = 0.0 if rms_q == 0 else float(20.0 * np.log10(max(rms_i, 1e-12) / max(rms_q, 1e-12)))
    denom = np.mean(i**2 + q**2)
    phase_err = 0.0 if denom == 0 else math.degrees(math.asin(np.clip(2.0 * np.mean(i * q) / denom, -1.0, 1.0)))
    return {"gain_db": gain_db, "phase_deg": float(phase_err)}


def fft_summary(channel_data, sample_rate, fft_size=4096):
    if channel_data.size == 0:
        return {
            "fft_size": 0,
            "peak_freq_hz": 0.0,
            "peak_dbfs": 0.0,
            "noise_floor_dbfs": 0.0,
            "sfdr_db": 0.0,
            "snr_db": 0.0,
        }
    size = min(int(fft_size), int(channel_data.size))
    if size < 8:
        size = int(channel_data.size)
    window = np.hanning(size).astype(np.float32)
    segment = channel_data[:size] * window
    spectrum = np.fft.fftshift(np.fft.fft(segment, n=size))
    mags = np.abs(spectrum) / max(np.sum(window), 1.0)
    mags_db = 20.0 * np.log10(np.maximum(mags, 1e-12))
    freqs = np.fft.fftshift(np.fft.fftfreq(size, d=1.0 / sample_rate)) if sample_rate > 0 else np.zeros(size)
    peak_idx = int(np.argmax(mags))
    peak_db = float(mags_db[peak_idx])
    peak_freq = float(freqs[peak_idx])
    mask = np.ones(size, dtype=bool)
    lo = max(0, peak_idx - 1)
    hi = min(size, peak_idx + 2)
    mask[lo:hi] = False
    spur_db = float(np.max(mags_db[mask])) if np.any(mask) else peak_db
    noise_floor = float(np.median(mags_db[mask])) if np.any(mask) else peak_db
    linear = mags**2
    signal_power = float(linear[peak_idx])
    noise_power = float(np.sum(linear[mask])) if np.any(mask) else 0.0
    snr_db = 0.0 if noise_power <= 0 else float(10.0 * np.log10(signal_power / noise_power))
    return {
        "fft_size": size,
        "peak_freq_hz": peak_freq,
        "peak_dbfs": peak_db,
        "noise_floor_dbfs": noise_floor,
        "sfdr_db": float(peak_db - spur_db),
        "snr_db": snr_db,
    }


def measure_channel(channel_data, sample_rate, fft_size=4096):
    if channel_data.size == 0:
        return {
            "sample_count": 0,
            "power_dbfs": 0.0,
            "rms": 0.0,
            "rms_i": 0.0,
            "rms_q": 0.0,
            "dc_i": 0.0,
            "dc_q": 0.0,
            "clipping_rate": 0.0,
            "crest_factor_db": 0.0,
            "freq_offset_hz": 0.0,
            "iq_gain_imbalance_db": 0.0,
            "iq_phase_error_deg": 0.0,
            "fft": fft_summary(channel_data, sample_rate, fft_size=fft_size),
        }
    power = float(np.mean(np.abs(channel_data) ** 2))
    rms = math.sqrt(power)
    iq = estimate_iq_imbalance(channel_data)
    metrics = {
        "sample_count": int(channel_data.size),
        "power_dbfs": float(10.0 * np.log10(max(power, 1e-15))),
        "rms": float(rms),
        "rms_i": float(np.sqrt(np.mean(channel_data.real**2))),
        "rms_q": float(np.sqrt(np.mean(channel_data.imag**2))),
        "dc_i": float(np.mean(channel_data.real)),
        "dc_q": float(np.mean(channel_data.imag)),
        "clipping_rate": clipping_rate(channel_data),
        "crest_factor_db": crest_factor_db(channel_data),
        "freq_offset_hz": estimate_frequency_hz(channel_data, sample_rate),
        "iq_gain_imbalance_db": iq["gain_db"],
        "iq_phase_error_deg": iq["phase_deg"],
        "fft": fft_summary(channel_data, sample_rate, fft_size=fft_size),
    }
    return metrics


def measure_capture(sigmf_path, max_samples=None, fft_size=4096):
    capture = load_complex_samples(sigmf_path, max_samples=max_samples)
    samples = capture["samples"]
    channels = {
        f"ch{idx}": measure_channel(samples[:, idx], capture["sample_rate"], fft_size=fft_size)
        for idx in range(capture["nchannels"])
    }
    capture["measurements"] = channels
    capture["duration_s"] = 0.0 if capture["sample_rate"] <= 0 else float(samples.shape[0] / capture["sample_rate"])
    return capture


def phase_compare(sigmf_path, reference=0, candidate=1, max_samples=None):
    capture = load_complex_samples(sigmf_path, max_samples=max_samples)
    samples = capture["samples"]
    if capture["nchannels"] <= max(reference, candidate):
        raise ValueError("Requested channel index is out of range")
    a = samples[:, reference]
    b = samples[:, candidate]
    if a.size == 0:
        corr = 0j
    else:
        corr = np.mean(np.conj(a) * b)
    amp_a = np.sqrt(np.mean(np.abs(a) ** 2)) if a.size else 0.0
    amp_b = np.sqrt(np.mean(np.abs(b) ** 2)) if b.size else 0.0
    gain_db = 0.0 if amp_a == 0 or amp_b == 0 else float(20.0 * np.log10(amp_b / amp_a))
    coherence = 0.0 if amp_a == 0 or amp_b == 0 else float(np.abs(corr) / max(amp_a * amp_b, 1e-12))
    return {
        "reference_channel": reference,
        "candidate_channel": candidate,
        "sample_count": int(a.size),
        "gain_delta_db": gain_db,
        "phase_delta_deg": float(np.degrees(np.angle(corr))) if a.size else 0.0,
        "coherence": coherence,
        "sample_rate": capture["sample_rate"],
    }


def timecheck_summary(sigmf_path):
    meta_path, _, meta = resolve_sigmf_pair(sigmf_path)
    captures = meta.get("captures", [])
    annotations = meta.get("annotations", [])
    starts = [int(capture.get("core:sample_start", 0)) for capture in captures]
    sorted_ok = starts == sorted(starts)
    deltas = [starts[idx + 1] - starts[idx] for idx in range(len(starts) - 1)]
    discontinuities = [delta for delta in deltas if delta <= 0]
    label_hits = [
        annotation.get("core:label", "")
        for annotation in annotations
        if "jump" in annotation.get("core:label", "").lower() or "timestamp" in annotation.get("core:label", "").lower()
    ]
    global_meta = meta.get("global", {})
    return {
        "meta_path": str(meta_path),
        "capture_count": len(captures),
        "annotation_count": len(annotations),
        "capture_starts": starts,
        "sorted": sorted_ok,
        "delta_samples": deltas,
        "discontinuities": discontinuities,
        "header_bytes": int(global_meta.get("core:header_bytes", 0) or 0),
        "sample_rate": float(global_meta.get("core:sample_rate", 0.0) or 0.0),
        "timestamp_annotations": label_hits,
        "continuous_capture_layout": sorted_ok and all(delta > 0 for delta in deltas),
    }


def calibration_profile_from_measure(sigmf_path, reference=0, candidate=1, max_samples=None):
    capture = measure_capture(sigmf_path, max_samples=max_samples)
    phase = phase_compare(sigmf_path, reference=reference, candidate=candidate, max_samples=max_samples)
    reference_metrics = capture["measurements"].get(f"ch{reference}", {})
    profile = {
        "schema": "m2sdr-calibration-profile/v1",
        "generated_from": str(Path(sigmf_path)),
        "generated_at": None,
        "rx_iq_correction": {
            "channel": reference,
            "gain_db": -float(reference_metrics.get("iq_gain_imbalance_db", 0.0)),
            "phase_deg": -float(reference_metrics.get("iq_phase_error_deg", 0.0)),
        },
        "channel_match": {
            "reference_channel": reference,
            "candidate_channel": candidate,
            "gain_db": -float(phase.get("gain_delta_db", 0.0)),
            "phase_deg": -float(phase.get("phase_delta_deg", 0.0)),
            "coherence": float(phase.get("coherence", 0.0)),
        },
    }
    return profile
