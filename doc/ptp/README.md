# Ethernet PTP Bring-Up

This note covers the direct-cable M2SDR Ethernet PTP setup used with:

```sh
./litex_m2sdr.py --variant=baseboard --with-eth --with-eth-ptp --eth-sfp=0 --build --load
```

For the optional PTP-disciplined FPGA 10 MHz RFIC reference path, add
`--with-eth-ptp-rfic-clock` to the same Ethernet/PTP build.

## Start ptp4l

For a software-timestamped host NIC:

```sh
sudo ptp4l -i enp5s0 -f doc/ptp/ptp4l-m2sdr-software.cfg -m
```

This is equivalent to:

```sh
sudo ptp4l -i enp5s0 -4 -E -S -m
```

If the host NIC supports hardware timestamping, use:

```sh
sudo ptp4l -i enp5s0 -f doc/ptp/ptp4l-m2sdr-hardware.cfg -m
```

This is equivalent to:

```sh
sudo ptp4l -i enp5s0 -4 -E -H -m
```

## Check Host Timestamping

Before comparing accuracy numbers, check the host NIC:

```sh
ethtool -T enp5s0
```

Use the hardware config only when the output reports a PTP Hardware Clock and hardware transmit/receive timestamp modes. If the output only lists `software-transmit`, `software-receive`, and `software-system-clock`, keep using the software config. In that mode, the board can lock to the host PTP exchange, but the host software timestamp path dominates the observed jitter.

## Validate The Board

Build the Etherbone utility for the Ethernet transport:

```sh
make -C litex_m2sdr/software/user m2sdr_util INTERFACE=USE_LITEETH
```

Run a short smoke test:

```sh
scripts/m2sdr_ptp_check.py smoke --ip 192.168.1.50 --iface enp5s0 --duration 10
```

The smoke test checks board reachability, `m2sdr_util ptp-smoke`, JSON status, and the on-wire PTP message types seen by tcpdump.

Run a longer lock/error soak:

```sh
scripts/m2sdr_ptp_check.py soak \
    --ip 192.168.1.50 \
    --duration 1800 \
    --interval 5 \
    --max-error-ns 1000000 \
    --json-out build/ptp/soak.json \
    --csv-out build/ptp/soak.csv
```

For a software-timestamped host, keep the default `--max-error-ns 1000000` until there is enough data from the target host/NIC. With host hardware timestamping, lower this threshold and compare the soak min/max/mean error against the software-timestamped run.

The JSON report stores the pass/fail result, signed and absolute error statistics, p50/p95/p99 percentiles, counter deltas, final status, and raw samples. The CSV file is easier to plot over long soaks. When `--with-clock10` is used, the report also includes clk10 status/statistics and the CSV writer emits a sibling `*.clock10.csv` file.

## Discipline The RFIC Reference Path

The optional RFIC-reference clock path uses PTP as a slow phase/frequency
reference for the FPGA-generated 10 MHz clock:

```sh
./litex_m2sdr.py \
    --variant=baseboard \
    --with-eth \
    --with-eth-ptp \
    --with-eth-ptp-rfic-clock \
    --eth-sfp=0 \
    --build --load
```

This does not directly create a low-jitter RF reference from packet
timestamps. It monitors the PTP-disciplined PPS/reference pulse against a
10 MHz-derived 1 Hz marker, then optionally steers the `clk10` MMCM with
small Dynamic Phase Shift steps. The clk10 loop uses a low-bandwidth PI rate
servo so it can learn the board's static reference offset without holding a
large permanent phase error. The AD9361 reference path uses it only when
software selects the SI5351C FPGA clock input, for example `clock_source=fpga`
or `--sync fpga` in the user tools.

Start with measurement mode. The loop is disabled by default:

```sh
./litex_m2sdr/software/user/m2sdr_util --ip 192.168.1.50 ptp-clock10-status
./litex_m2sdr/software/user/m2sdr_util --ip 192.168.1.50 ptp-clock10-config
```

When board PTP lock is stable, enable the loop:

```sh
./litex_m2sdr/software/user/m2sdr_util --ip 192.168.1.50 ptp-clock10-config enable on
```

Watch the loop until both the PTP reference and the FPGA 10 MHz marker are
locked:

```sh
./litex_m2sdr/software/user/m2sdr_util --ip 192.168.1.50 --watch ptp-clock10-status
```

If the marker was started before PTP lock, or after changing the reference
source, request a fresh one-shot marker alignment. The request is armed by the
CSR write and applied on the next valid PTP reference edge, so host command
latency does not set the RF reference phase:

```sh
./litex_m2sdr/software/user/m2sdr_util --ip 192.168.1.50 ptp-clock10-config align
```

For RF use, select the FPGA clock input before initializing/calibrating the
AD9361:

```sh
cd litex_m2sdr/software/user
./m2sdr_rf --sync fpga --refclk-freq 38400000
```

With SoapySDR, use the matching FPGA clock-source setting, for example
`clock_source=fpga`.

Useful tuning/status fields:

- `last_error_ns`: signed phase of the clk10 marker relative to the PTP reference pulse.
- `last_rate`: signed MMCM fine phase-step rate, Q0.32 steps per update.
- `update-cycles`: MMCM fine phase-step update interval. The default is
  20 sysclk cycles, which is 200 ns at the 100 MHz system clock.
- `p-gain`: proportional gain from phase error ticks to MMCM step rate.
- `i-gain`: integral gain that learns the steady MMCM step rate required by the board.
- `rate-limit`: maximum absolute MMCM step rate.
- `lock-window-ticks`: clk10 marker lock-reporting window. The default is
  5000 sysclk ticks, which is 50 us at the 100 MHz system clock and tolerates
  software-timestamped PTP jitter. Lower it for hardware-timestamped setups
  after measuring the link.
- `invert`: flips the correction sign if the phase error diverges after enabling.
- `missing_count`, `lock_loss_count`, `saturation_count`: counters to watch during soaks.

The soak helper can include this status in the JSON report:

```sh
scripts/m2sdr_ptp_check.py soak \
    --ip 192.168.1.50 \
    --duration 1800 \
    --interval 5 \
    --with-clock10 \
    --json-out build/ptp/soak-clock10.json \
    --csv-out build/ptp/soak-clock10.csv
```

Add `--require-clock10-lock` after the loop parameters are known-good for the
lab setup:

```sh
scripts/m2sdr_ptp_check.py smoke \
    --ip 192.168.1.50 \
    --iface enp5s0 \
    --duration 10 \
    --with-clock10 \
    --require-clock10-lock
```

A clean run should keep `Reference Locked` and `Clock Locked` asserted, with no
new `Missing Windows` or `Saturations` during the observation window. Host NIC
timestamping and network load dominate a software-timestamped setup, so keep
the accepted error limits tied to the target host and link rather than to a
single bring-up run.

A software-timestamped host can create enough PTP jitter that a very tight
RF-reference loop becomes counterproductive. For RF work, keep the loop
bandwidth low and avoid large corrections while the AD9361 is already
calibrated and streaming.

This provides frequency coherence of the AD9361 reference when the SI5351C is
fed from FPGA `clk10`. Deterministic RF/sample phase alignment between two
boards still requires a deterministic AD9361 synchronization and timestamped
stream-start procedure.

## Tune Time-Lock Stability

The board time-lock state has a small deglitch window: after lock has been
acquired, isolated samples outside `lock-window` are corrected without dropping
the reported state back to `acquire`. A real time-lock loss is reported only
after `unlock-misses` consecutive out-of-window samples, or immediately on PTP
unlock. Runtime coarse realignments while already locked keep the reported
time-lock state asserted, but they are disabled by default after initial
acquisition. Set `coarse-confirm` to a nonzero value to require that many
consecutive discipline samples before a runtime board-clock rewrite is allowed.
Runtime coarse outliers are counted as `Time Lock Misses` and the last accepted
error sample is left unchanged while they are rejected. Runtime coarse errors
very close to one second are treated as LiteEth TSU second-step excursions and
are never copied into the board clock after initial acquisition.

Inspect the live settings with:

```sh
./litex_m2sdr/software/user/m2sdr_util --ip 192.168.1.50 ptp-config
```

The default `lock-window` value is `4096` ns. Phase correction still starts at
the tighter `phase-threshold` value, but the wider lock-reporting window avoids
classifying rare software-timestamp jitter below a few microseconds as board
time unlocks. The default `unlock-misses` value is `64`, about one second with
the default servo update rate. Keep it low enough to catch real servo
divergence, but high enough to avoid reporting host software timestamp jitter
as a lock loss. The default `coarse-confirm` value is `0`,
which disables runtime coarse board-clock rewrites after initial acquisition.
Use a nonzero value only when the board should automatically follow persistent
non-second master steps:

```sh
./litex_m2sdr/software/user/m2sdr_util --ip 192.168.1.50 ptp-config lock-window 4096
./litex_m2sdr/software/user/m2sdr_util --ip 192.168.1.50 ptp-config unlock-misses 64
./litex_m2sdr/software/user/m2sdr_util --ip 192.168.1.50 ptp-config coarse-confirm 0
```

`ptp-status` reports tolerated `Time Lock Misses`, deglitched
`Time Lock Losses`, and `Coarse Steps`, so a soak can distinguish corrected
jitter samples, hard realignments, and actual lock drops.

## Characterize Fixed Offset

Without an external reference, the board can report only residual servo bias against the PTP master. Collect that bias with:

```sh
scripts/m2sdr_ptp_check.py calibrate \
    --ip 192.168.1.50 \
    --duration 300 \
    --interval 1 \
    --json-out build/ptp/calibration.json \
    --csv-out build/ptp/calibration.csv
```

For absolute board/PHY/PPS offset calibration, measure the board PPS or timestamp output against an external timing reference, then pass that measured offset:

```sh
scripts/m2sdr_ptp_check.py calibrate \
    --ip 192.168.1.50 \
    --duration 300 \
    --interval 1 \
    --reference-offset-ns <external-measured-offset-ns> \
    --json-out build/ptp/calibration-with-reference.json
```

The reported median and `calibration.suggested_compensation_ns` are computed from locked, in-limit samples only. They are lab measurement results; do not bake them into gateware until the run has passed and has been repeated with the target host NIC, SFP/cable, and timestamping mode.
