import importlib.util
from pathlib import Path


def _load_time_sync():
    path = Path(__file__).resolve().parents[1] / "scripts" / "m2sdr_pcie_time_sync.py"
    spec = importlib.util.spec_from_file_location("m2sdr_pcie_time_sync", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _ptp(sys_class_ptp, name, clock_name):
    path = sys_class_ptp / name
    path.mkdir(parents=True)
    (path / "clock_name").write_text(clock_name, encoding="ascii")
    return path


def test_discover_phcs_finds_m2sdr_clock_name(tmp_path):
    time_sync = _load_time_sync()
    sys_class_ptp = tmp_path / "sys" / "class" / "ptp"
    dev_root = tmp_path / "dev"

    _ptp(sys_class_ptp, "ptp3", "other")
    _ptp(sys_class_ptp, "ptp1", "m2sdr")

    assert time_sync.discover_phcs(sys_class_ptp, dev_root) == [dev_root / "ptp1"]


def test_select_phc_reports_multiple_boards(tmp_path):
    time_sync = _load_time_sync()
    sys_class_ptp = tmp_path / "sys" / "class" / "ptp"

    _ptp(sys_class_ptp, "ptp0", "m2sdr")
    _ptp(sys_class_ptp, "ptp1", "m2sdr")
    args = time_sync.parse_args([
        "--sys-class-ptp", str(sys_class_ptp),
        "--dev-root", str(tmp_path / "dev"),
    ])

    try:
        time_sync.select_phc(args)
    except time_sync.TimeSyncError as e:
        assert "multiple M2SDR PHCs found" in str(e)
    else:
        assert False, "expected TimeSyncError"


def test_build_phc2sys_command_uses_host_as_source(tmp_path):
    time_sync = _load_time_sync()
    phc2sys = tmp_path / "phc2sys"
    phc2sys.write_text("#!/bin/sh\n", encoding="ascii")
    args = time_sync.parse_args([
        "--phc2sys", str(phc2sys),
        "--rate", "4",
        "--readings", "7",
        "--stdout",
        "--no-syslog",
    ])

    cmd = time_sync.build_phc2sys_command(args, Path("/dev/ptp4"))

    assert cmd[:9] == [
        str(phc2sys),
        "-s", "CLOCK_REALTIME",
        "-c", "/dev/ptp4",
        "-O", "0.0",
        "-R", "4.0",
    ]
    assert "-N" in cmd
    assert "7" in cmd
    assert "-m" in cmd
    assert "-q" in cmd


def test_main_dry_run_prints_command_without_exec(tmp_path, capsys):
    time_sync = _load_time_sync()
    phc = tmp_path / "ptp2"
    phc.touch()
    phc2sys = tmp_path / "phc2sys"
    phc2sys.write_text("#!/bin/sh\n", encoding="ascii")
    exec_calls = []

    rc = time_sync.main([
        "--phc", str(phc),
        "--phc2sys", str(phc2sys),
        "--dry-run",
    ], exec_func=lambda *args: exec_calls.append(args))

    assert rc == 0
    assert exec_calls == []
    assert "-s CLOCK_REALTIME -c {}".format(phc) in capsys.readouterr().out
