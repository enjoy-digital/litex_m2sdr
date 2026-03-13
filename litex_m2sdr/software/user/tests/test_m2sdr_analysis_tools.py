#!/usr/bin/env python3

# SPDX-License-Identifier: BSD-2-Clause

import csv
import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np


USER_DIR = Path(__file__).resolve().parent.parent


def run_tool(tool, *args):
    return subprocess.run(
        [sys.executable, str(USER_DIR / tool), *args],
        check=True,
        text=True,
        capture_output=True,
    )


def write_sigmf_capture(base_path):
    sample_rate = 1_000_000
    tone_hz = 50_000
    nsamples = 4096
    n = np.arange(nsamples, dtype=np.float32)
    ch0 = 0.7 * np.exp(1j * 2.0 * np.pi * tone_hz * n / sample_rate)
    ch1 = (0.7 * 10 ** (1.5 / 20.0)) * np.exp(1j * (2.0 * np.pi * tone_hz * n / sample_rate + np.deg2rad(15.0)))

    interleaved = np.empty((nsamples, 4), dtype=np.int16)
    interleaved[:, 0] = np.clip(np.round(ch0.real * 32767.0), -32768, 32767).astype(np.int16)
    interleaved[:, 1] = np.clip(np.round(ch0.imag * 32767.0), -32768, 32767).astype(np.int16)
    interleaved[:, 2] = np.clip(np.round(ch1.real * 32767.0), -32768, 32767).astype(np.int16)
    interleaved[:, 3] = np.clip(np.round(ch1.imag * 32767.0), -32768, 32767).astype(np.int16)

    meta = {
        "global": {
            "core:version": "1.2.5",
            "core:datatype": "ci16_le",
            "core:dataset": base_path.with_suffix(".sigmf-data").name,
            "core:sample_rate": sample_rate,
            "core:frequency": 2_400_000_000,
            "core:num_channels": 2,
            "core:hw": "LiteX-M2SDR",
        },
        "captures": [
            {"core:sample_start": 0},
            {"core:sample_start": 4096},
            {"core:sample_start": 8192},
        ],
        "annotations": [
            {"core:sample_start": 1024, "core:sample_count": 256, "core:label": "timestamp-jump-check"},
        ],
    }
    base_path.with_suffix(".sigmf-meta").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    interleaved.tofile(base_path.with_suffix(".sigmf-data"))
    return sample_rate, tone_hz


class TestM2SDRAnalysisTools(unittest.TestCase):
    def test_measure_phase_timecheck_cal_and_sweep(self):
        with tempfile.TemporaryDirectory(prefix="m2sdr_analysis_") as tmpdir:
            root = Path(tmpdir)
            base = root / "tone_capture"
            sample_rate, tone_hz = write_sigmf_capture(base)

            measure_json = root / "measure.json"
            measure_png = root / "measure.png"
            measured = run_tool(
                "m2sdr_measure",
                str(base.with_suffix(".sigmf-meta")),
                "--output",
                str(measure_json),
                "--plot",
                str(measure_png),
            )
            self.assertTrue(measure_json.exists())
            self.assertTrue(measure_png.exists())
            measure_report = json.loads(measured.stdout)
            ch0 = measure_report["capture"]["channels"]["ch0"]
            self.assertAlmostEqual(ch0["freq_offset_hz"], tone_hz, delta=1_000)
            self.assertLess(ch0["clipping_rate"], 0.01)
            self.assertAlmostEqual(ch0["fft"]["peak_freq_hz"], tone_hz, delta=1_000)

            phased = run_tool("m2sdr_phase", str(base.with_suffix(".sigmf-meta")))
            phase_report = json.loads(phased.stdout)["result"]
            self.assertAlmostEqual(phase_report["phase_delta_deg"], 15.0, delta=2.0)
            self.assertAlmostEqual(phase_report["gain_delta_db"], 1.5, delta=0.5)

            timed = run_tool("m2sdr_timecheck", str(base.with_suffix(".sigmf-meta")))
            time_report = json.loads(timed.stdout)["result"]
            self.assertTrue(time_report["continuous_capture_layout"])
            self.assertEqual(len(time_report["timestamp_annotations"]), 1)

            cal_json = root / "cal.json"
            calibrated = run_tool("m2sdr_cal", str(base.with_suffix(".sigmf-meta")), "--output", str(cal_json))
            self.assertTrue(cal_json.exists())
            cal_report = json.loads(calibrated.stdout)
            self.assertIn("rx_iq_correction", cal_report)
            self.assertAlmostEqual(cal_report["channel_match"]["phase_deg"], -15.0, delta=2.0)

            sweep_json = root / "sweep.json"
            sweep_csv = root / "sweep.csv"
            swept = run_tool(
                "m2sdr_sweep",
                "--var",
                "freq=2400000000,2450000000",
                "--var",
                "gain=0,10",
                "--command",
                "/bin/echo {freq} {gain}",
                "--output",
                str(sweep_json),
                "--csv",
                str(sweep_csv),
            )
            sweep_report = json.loads(swept.stdout)
            self.assertEqual(len(sweep_report["points"]), 4)
            self.assertTrue(all(point["returncode"] == 0 for point in sweep_report["points"]))
            with sweep_csv.open("r", encoding="utf-8") as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 4)
            self.assertEqual(rows[0]["returncode"], "0")


if __name__ == "__main__":
    unittest.main()
