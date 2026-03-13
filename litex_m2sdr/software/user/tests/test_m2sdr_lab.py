#!/usr/bin/env python3

# SPDX-License-Identifier: BSD-2-Clause

import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parent.parent / "m2sdr_lab"


def run_cmd(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        check=True,
        text=True,
        capture_output=True,
    )


def write_sigmf(base, sample_rate, center_freq, payload):
    meta_path = base.with_suffix(".sigmf-meta")
    data_path = base.with_suffix(".sigmf-data")
    meta = {
        "global": {
            "core:version": "1.2.5",
            "core:datatype": "ci16_le",
            "core:dataset": data_path.name,
            "core:sample_rate": sample_rate,
            "core:frequency": center_freq,
            "core:num_channels": 2,
            "core:hw": "LiteX-M2SDR",
        },
        "captures": [{"core:sample_start": 0}],
        "annotations": [{"core:sample_start": 0, "core:sample_count": 16, "core:label": "burst"}],
    }
    meta_path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    data_path.write_bytes(payload)
    return meta_path


class TestM2SDRLab(unittest.TestCase):
    def test_init_ingest_compare_verify_report_bundle_and_replay(self):
        with tempfile.TemporaryDirectory(prefix="m2sdr_lab_test_") as tmpdir:
            root = Path(tmpdir)
            lab = root / "lab"
            source_a = root / "source_a"
            source_b = root / "source_b"
            source_c = root / "source_c"

            run_cmd(
                "init",
                str(lab),
                "--title",
                "RF regression",
                "--description",
                "Loopback reproducibility check",
            )
            self.assertTrue((lab / "lab.json").exists())
            self.assertTrue((lab / "README.md").exists())

            meta_a = write_sigmf(source_a, 30720000, 2400000000, b"\x00\x01" * 32)
            meta_b = write_sigmf(source_b, 30720000, 2450000000, b"\x02\x03" * 32)
            meta_c = write_sigmf(source_c, 30720000, 2400000000, b"\x00\x01" * 32)

            run_cmd("ingest", str(lab), str(meta_a), "--name", "baseline", "--copy")
            run_cmd("ingest", str(lab), str(meta_b), "--name", "variant", "--copy")
            run_cmd("ingest", str(lab), str(meta_c), "--name", "baseline-copy", "--copy")

            listed = run_cmd("list", str(lab)).stdout
            self.assertIn("baseline", listed)
            self.assertIn("variant", listed)

            compare = run_cmd("compare", str(lab), "baseline", "variant").stdout
            self.assertIn('"center_freq": false', compare)
            self.assertIn('"compatible_for_replay": false', compare)

            report = run_cmd("report", str(lab), "baseline", "--markdown").stdout
            self.assertIn('"sample_rate": 30720000', report)
            self.assertTrue((lab / "reports" / "report-baseline.json").exists())
            self.assertTrue((lab / "reports" / "report-baseline.md").exists())

            verify_ok = run_cmd(
                "verify",
                str(lab),
                "baseline",
                "baseline-copy",
            ).stdout
            self.assertIn('"passed": true', verify_ok)
            self.assertIn('"exact_match": false', verify_ok)

            verify_fail = subprocess.run(
                [sys.executable, str(SCRIPT), "verify", str(lab), "baseline", "baseline-copy", "--exact", "--fail-on-mismatch"],
                text=True,
                capture_output=True,
            )
            self.assertEqual(verify_fail.returncode, 1)
            self.assertIn('"passed": false', verify_fail.stdout)

            replay = run_cmd(
                "replay",
                str(lab),
                "baseline",
                "--dry-run",
                "--name",
                "baseline-replay",
                "--play-bin",
                "./m2sdr_play",
            ).stdout
            self.assertIn("m2sdr_play", replay)

            capture = run_cmd(
                "capture",
                str(lab),
                "--dry-run",
                "--name",
                "planned-capture",
                "--samples",
                "4096",
                "--record-bin",
                "./m2sdr_record",
            ).stdout
            self.assertIn("m2sdr_record", capture)

            bundle_path = lab / "bundles" / "rf-regression.tar.gz"
            run_cmd("bundle", str(lab), "--meta-only", "--output", str(bundle_path))
            self.assertTrue(bundle_path.exists())
            with tarfile.open(bundle_path, "r:gz") as tar:
                members = tar.getnames()
            self.assertIn("lab.json", members)
            self.assertIn("captures/baseline.sigmf-meta", members)
            self.assertIn("reports/report-baseline.json", members)
            self.assertIn("reports/report-baseline.md", members)
            self.assertNotIn("captures/baseline.sigmf-data", members)


if __name__ == "__main__":
    unittest.main()
