import importlib.util
import json
from pathlib import Path


def _load_ptp_check():
    path = Path(__file__).resolve().parents[1] / "scripts" / "m2sdr_ptp_check.py"
    spec = importlib.util.spec_from_file_location("m2sdr_ptp_check", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _status(**overrides):
    last_error_ns = overrides.get("last_error_ns", 10)
    last_local_time_ns = overrides.get("last_local_time_ns", 1778760000000000000)
    status = {
        "enabled": True,
        "active": True,
        "ptp_locked": True,
        "time_locked": True,
        "holdover": False,
        "state": 2,
        "state_name": "locked",
        "refresh_time_unix": 1778760000,
        "master_ip": "192.168.1.125",
        "master_port": {"identity": "74563cfffe4f4c6d:1"},
        "last_error_ns": last_error_ns,
        "last_ptp_time_ns": last_local_time_ns + last_error_ns,
        "last_local_time_ns": last_local_time_ns,
        "coarse_steps": 0,
        "phase_steps": 0,
        "rate_updates": 0,
        "ptp_lock_losses": 0,
        "time_lock_misses": 0,
        "time_lock_miss_count": 0,
        "time_lock_losses": 0,
    }
    status.update(overrides)
    return status


def _clock10_status(**overrides):
    status = {
        "refresh_time_unix": 1778760000,
        "enabled": True,
        "active": True,
        "reference_locked": True,
        "clock_locked": True,
        "holdover": False,
        "aligned": True,
        "rate_limited": False,
        "phase_tick_ps": 8000,
        "last_error_ticks": 1,
        "last_error_ns": 8,
        "last_rate": -1048576,
        "sample_count": 10,
        "reference_count": 10,
        "clk10_count": 10,
        "missing_count": 0,
        "align_count": 1,
        "lock_loss_count": 0,
        "rate_update_count": 9,
        "saturation_count": 0,
    }
    status.update(overrides)
    return status


def test_status_evaluator_accepts_locked_ptp_state():
    ptp_check = _load_ptp_check()

    assert ptp_check.evaluate_status(_status(), max_error_ns=1000) == []


def test_etherbone_process_settle_only_applies_to_direct_ethernet(monkeypatch):
    ptp_check = _load_ptp_check()
    calls = []

    class Args:
        device = None

    monkeypatch.setattr(ptp_check.time, "sleep", lambda seconds: calls.append(seconds))

    ptp_check.settle_after_util_process(Args)
    assert calls == [ptp_check.ETHERBONE_PROCESS_SETTLE_SECONDS]

    Args.device = "/dev/litex/liteeth_ptp"
    ptp_check.settle_after_util_process(Args)
    assert calls == [ptp_check.ETHERBONE_PROCESS_SETTLE_SECONDS]


def test_status_evaluator_reports_lock_and_error_failures():
    ptp_check = _load_ptp_check()

    failures = ptp_check.evaluate_status(
        _status(ptp_locked=False, holdover=True, last_error_ns=-2000),
        max_error_ns=1000,
    )

    assert "PTP servo is not locked" in failures
    assert "board is in holdover" in failures
    assert "last error -2000 ns exceeds 1000 ns" in failures


def test_signed_error_corrects_zero_extended_negative_raw_error():
    ptp_check = _load_ptp_check()
    status = _status(
        last_error_ns=4294967289,
        last_ptp_time_ns=1000,
        last_local_time_ns=1007,
    )

    assert ptp_check.signed_error_ns(status) == -7
    assert ptp_check.abs_error_ns(status) == 7
    assert ptp_check.evaluate_status(status, max_error_ns=1000) == []


def test_signed_error_keeps_raw_error_for_non_matching_timestamp_pair():
    ptp_check = _load_ptp_check()
    status = _status(
        last_error_ns=-1,
        last_ptp_time_ns=1_000_000,
        last_local_time_ns=2_000_000,
    )

    assert ptp_check.signed_error_ns(status) == -1
    assert ptp_check.abs_error_ns(status) == 1


def test_signed_error_falls_back_to_raw_error_without_timestamps():
    ptp_check = _load_ptp_check()
    status = _status(last_error_ns=-12)
    del status["last_ptp_time_ns"]
    del status["last_local_time_ns"]

    assert ptp_check.signed_error_ns(status) == -12


def test_counter_evaluator_reports_discipline_deltas():
    ptp_check = _load_ptp_check()
    first = _status()
    last = _status(coarse_steps=2, rate_updates=5)

    failures, discipline = ptp_check.evaluate_counter_deltas(first, last)

    assert failures == []
    assert discipline["coarse_steps"] == 2
    assert discipline["rate_updates"] == 5


def test_tcpdump_analyzer_requires_all_ptp_message_types():
    ptp_check = _load_ptp_check()
    output = """
    PTPv2, msg type : sync msg
    PTPv2, msg type : follow up msg
    PTPv2, msg type : delay req msg
    PTPv2, msg type : delay resp msg
    """

    present, missing = ptp_check.analyze_tcpdump_output(output)

    assert all(present.values())
    assert missing == []


def test_tcpdump_analyzer_reports_missing_message_types():
    ptp_check = _load_ptp_check()

    _, missing = ptp_check.analyze_tcpdump_output("PTPv2, msg type : sync msg")

    assert missing == ["follow_up", "delay_req", "delay_resp"]


def test_percentile_interpolates_between_samples():
    ptp_check = _load_ptp_check()

    assert ptp_check.percentile([0, 10, 20], 50) == 10
    assert ptp_check.percentile([0, 100], 95) == 95


def test_numeric_stats_reports_signed_error_percentiles():
    ptp_check = _load_ptp_check()

    stats = ptp_check.numeric_stats([-10, 0, 30, 40])

    assert stats["count"] == 4
    assert stats["min"] == -10
    assert stats["max"] == 40
    assert stats["mean"] == 15
    assert stats["p50"] == 15
    assert stats["p95"] == 38.5


def test_sample_quality_counts_lock_and_error_failures():
    ptp_check = _load_ptp_check()

    quality = ptp_check.sample_quality(
        [
            _status(last_error_ns=10),
            _status(ptp_locked=False, time_locked=False, last_error_ns=2000),
            _status(holdover=True, last_error_ns=-5),
        ],
        max_error_ns=1000,
    )

    assert quality == {
        "ptp_unlocked_count": 1,
        "time_unlocked_count": 1,
        "holdover_count": 1,
        "over_error_limit_count": 1,
    }


def test_csv_samples_include_time_lock_miss_counters(tmp_path):
    ptp_check = _load_ptp_check()
    path = tmp_path / "samples.csv"

    ptp_check.write_csv_samples(path, [_status()])
    header = path.read_text().splitlines()[0].split(",")

    assert "time_lock_misses" in header
    assert "time_lock_miss_count" in header


def test_json_report_includes_raw_clock10_samples(tmp_path):
    ptp_check = _load_ptp_check()
    path = tmp_path / "report.json"

    ptp_check.write_json_report(
        path,
        {"result": "PASS"},
        [_status()],
        [_clock10_status(last_error_ns=24)],
    )
    payload = json.loads(path.read_text())

    assert payload["raw_samples"][0]["state_name"] == "locked"
    assert payload["raw_clock10_samples"][0]["last_error_ns"] == 24


def test_calibration_error_samples_use_locked_in_limit_samples():
    ptp_check = _load_ptp_check()

    errors = ptp_check.calibration_error_samples(
        [
            _status(last_error_ns=-8),
            _status(time_locked=False, last_error_ns=20),
            _status(last_error_ns=2000),
            _status(holdover=True, last_error_ns=30),
            _status(last_error_ns=4),
        ],
        max_error_ns=1000,
    )

    assert errors == [-8, 4]


def test_clock10_evaluator_and_report_accept_locked_marker():
    ptp_check = _load_ptp_check()
    first = _clock10_status(sample_count=10, rate_update_count=9)
    samples = [
        _clock10_status(last_error_ns=-16, sample_count=11, rate_update_count=10),
        _clock10_status(last_error_ns=24, sample_count=12, rate_update_count=11),
    ]

    assert ptp_check.evaluate_clock10_status(samples[-1], require_lock=True) == []

    report = ptp_check.clock10_report(first, samples)

    assert report["samples"]["count"] == 2
    assert report["error_ns"]["signed"]["min"] == -16
    assert report["error_ns"]["absolute"]["max"] == 24
    assert report["counter_deltas"]["sample_count"] == 2
    assert report["counter_deltas"]["rate_update_count"] == 2


def test_clock10_evaluator_reports_missing_windows_and_unlock():
    ptp_check = _load_ptp_check()

    failures = ptp_check.evaluate_clock10_status(
        _clock10_status(reference_locked=False, clock_locked=False, missing_count=1),
        require_lock=True,
    )

    assert "PTP clk10 reference is not locked" in failures
    assert "PTP clk10 marker is not locked" in failures
    assert "PTP clk10 phase detector has missing windows" in failures


def test_build_report_contains_discipline_deltas():
    ptp_check = _load_ptp_check()
    first = _status()
    samples = [
        _status(last_error_ns=-10, refresh_time_unix=1),
        _status(
            last_error_ns=30,
            refresh_time_unix=2,
            coarse_steps=2,
            phase_steps=11,
            rate_updates=12,
            time_lock_misses=1,
        ),
    ]

    class Args:
        device = None
        ip = "192.168.1.50"
        port = 1234
        iface = "enp5s0"
        max_error_ns = 1000

    failures, discipline_deltas = ptp_check.evaluate_counter_deltas(first, samples[-1])
    report = ptp_check.build_report(
        "soak",
        Args,
        first,
        samples,
        discipline_deltas,
        None,
        failures,
    )

    assert report["result"] == "PASS"
    assert report["samples"]["count"] == 2
    assert report["error_ns"]["signed"]["min"] == -10
    assert report["error_ns"]["absolute"]["max"] == 30
    assert report["sample_quality"]["over_error_limit_count"] == 0
    assert report["counter_deltas"]["discipline"]["coarse_steps"] == 2
    assert report["counter_deltas"]["discipline"]["time_lock_misses"] == 1
