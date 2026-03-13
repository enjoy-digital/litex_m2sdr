# LiteX-M2SDR RF Lab

LiteX-M2SDR is now positioned not only as an SDR board, but as an open and reproducible RF experimentation platform.

The practical building blocks are already in-tree:

- `m2sdr_record` can emit SigMF captures with board metadata and annotations.
- `m2sdr_play` can replay raw or SigMF-backed captures.
- `m2sdr_check` and `m2sdr_sigmf_info` provide visual and CI-friendly inspection.
- `m2sdr_lab` adds experiment-level organization, replay tracking, verification, headless reports, and portable bundles.
- `m2sdr_measure`, `m2sdr_phase`, `m2sdr_timecheck`, `m2sdr_cal`, and `m2sdr_sweep` add offline measurement and regression tooling on top of SigMF captures.
- `m2sdr_lab_gui` provides a native cimgui dashboard to browse runs, reports, measurements, verification results, and sweep artifacts.

## Why this matters

Many SDR platforms let you capture and stream samples. Fewer let you share an experiment and make another developer replay, validate, and compare it with minimal ambiguity.

The LiteX-M2SDR RF lab workflow is aimed at:

- reproducible bug reports
- timestamped loopback or over-the-air regressions
- portable SigMF-based demos
- multi-host validation of the same waveform or capture
- publishable RF experiments that can be replayed by other users

## Typical workflow

Initialize a lab:

```bash
cd litex_m2sdr/software/user
./m2sdr_lab init ../../../../my_rf_lab \
  --title "2.4 GHz loopback regression" \
  --description "Baseline capture for replay/compare checks" \
  --device pcie:/dev/m2sdr0 \
  --sample-rate 30720000 \
  --center-freq 2400000000
```

Capture and register a SigMF dataset:

```bash
./m2sdr_lab capture ../../../../my_rf_lab \
  --name baseline \
  --samples 2000000 \
  --annotation-label burst
```

Replay the exact same dataset later:

```bash
./m2sdr_lab replay ../../../../my_rf_lab baseline --loops 1
```

Compare two runs:

```bash
./m2sdr_lab compare ../../../../my_rf_lab baseline retuned
```

Emit a headless run report:

```bash
./m2sdr_lab report ../../../../my_rf_lab baseline --markdown
```

Verify a new run against a reference:

```bash
./m2sdr_lab verify ../../../../my_rf_lab baseline retuned --fail-on-mismatch
```

Measure the resulting capture:

```bash
./m2sdr_measure ../../../../my_rf_lab/captures/baseline.sigmf-meta \
  --output ../../../../my_rf_lab/reports/baseline-measure.json \
  --plot ../../../../my_rf_lab/reports/baseline-measure.png

./m2sdr_phase ../../../../my_rf_lab/captures/baseline.sigmf-meta
./m2sdr_timecheck ../../../../my_rf_lab/captures/baseline.sigmf-meta
./m2sdr_cal ../../../../my_rf_lab/captures/baseline.sigmf-meta \
  --output ../../../../my_rf_lab/reports/baseline-cal.json

./m2sdr_lab_gui ../../../../my_rf_lab
```

Export a bundle for another user or CI job:

```bash
./m2sdr_lab bundle ../../../../my_rf_lab
```

## Quick tutorial: first capture to GUI

This is the shortest practical path to get a first RF lab dataset and open it in the dashboard.

### 1. Build the user tools

```bash
cd litex_m2sdr/software/user
make
```

If you want the GUI, make sure SDL2/OpenGL development packages are installed and `software/user/cimgui/` is present.

### 2. Check that the board is reachable

```bash
./m2sdr_util info
./m2sdr_record /tmp/rf_lab_smoke.iq 4096
ls -lh /tmp/rf_lab_smoke.iq
```

If the raw capture file is non-zero, the basic RX path is alive.

### 3. Create a standalone SigMF capture first

```bash
./m2sdr_record --sigmf \
  --device pcie:/dev/m2sdr0 \
  --sample-rate 30720000 \
  --center-freq 2400000000 \
  --nchannels 2 \
  /tmp/rf_lab_baseline 2000000
```

This produces:

- `/tmp/rf_lab_baseline.sigmf-data` for the sample payload
- `/tmp/rf_lab_baseline.sigmf-meta` for the SigMF metadata

Check the metadata:

```bash
ls -lh /tmp/rf_lab_baseline.sigmf-data /tmp/rf_lab_baseline.sigmf-meta
./m2sdr_sigmf_info /tmp/rf_lab_baseline.sigmf-meta
```

If `/tmp/rf_lab_baseline.sigmf-data` is missing but `/tmp/rf_lab_baseline` exists, you are likely using an older `m2sdr_record` binary. Rebuild `m2sdr_record` first. As a temporary workaround, rename the payload before ingesting it into the lab:

```bash
mv /tmp/rf_lab_baseline /tmp/rf_lab_baseline.sigmf-data
```

### 4. Create a lab and ingest the capture

```bash
./m2sdr_lab init /tmp/my_rf_lab \
  --title "2.4 GHz loopback regression" \
  --description "First RF lab tutorial" \
  --device pcie:/dev/m2sdr0 \
  --sample-rate 30720000 \
  --center-freq 2400000000

./m2sdr_lab ingest /tmp/my_rf_lab /tmp/rf_lab_baseline.sigmf-meta \
  --name baseline \
  --copy
```

At this point `/tmp/my_rf_lab/lab.json` contains a first tracked run.

Do not add an extra trailing `--` at the end of the command. For example, this is invalid:

```bash
./m2sdr_lab ingest /tmp/my_rf_lab /tmp/rf_lab_baseline.sigmf-meta --name baseline --copy --
```

### 5. Generate artifacts for the dashboard

```bash
./m2sdr_lab report /tmp/my_rf_lab baseline --markdown

./m2sdr_measure /tmp/my_rf_lab/captures/baseline.sigmf-meta \
  --output /tmp/my_rf_lab/reports/baseline-measure.json \
  --plot /tmp/my_rf_lab/reports/baseline-measure.png

./m2sdr_phase /tmp/my_rf_lab/captures/baseline.sigmf-meta \
  > /tmp/my_rf_lab/reports/baseline-phase.json

./m2sdr_timecheck /tmp/my_rf_lab/captures/baseline.sigmf-meta \
  > /tmp/my_rf_lab/reports/baseline-timecheck.json

./m2sdr_cal /tmp/my_rf_lab/captures/baseline.sigmf-meta \
  --output /tmp/my_rf_lab/reports/baseline-cal.json
```

### 6. Open the GUI

```bash
./m2sdr_lab_gui /tmp/my_rf_lab
```

You should now see:

- the `baseline` run in the run list
- the run report in the reports pane
- measurement, phase, timecheck, and calibration artifacts

### 7. Optional: replay and compare

```bash
./m2sdr_lab replay /tmp/my_rf_lab baseline --loops 1
./m2sdr_lab compare /tmp/my_rf_lab baseline baseline
./m2sdr_lab bundle /tmp/my_rf_lab
```

## Troubleshooting

- If `m2sdr_record /tmp/test.iq 4096` fails, debug the basic PCIe/RX path before using the RF lab flow.
- If `m2sdr_lab capture ...` prints `m2sdr_sync_rx failed`, do not trust the registered run blindly; verify that the capture payload file exists and is non-zero.
- If `m2sdr_rf` reports `m2sdr_apply_config failed`, capture may still partially work, but RF configuration is not cleanly applied and results can be misleading.
- If you only want to test the dashboard, `m2sdr_lab ingest` is the safest way to register an already verified SigMF capture.
- `m2sdr_measure`, `m2sdr_phase`, or `m2sdr_cal` failing with `No such file or directory: ... .sigmf-data` means the SigMF metadata points to a missing payload file. Verify the `.sigmf-data` file exists before ingesting the capture.
- If your existing capture was created by an older `m2sdr_record` binary, you may find the payload at `basename` instead of `basename.sigmf-data`. Rebuild `m2sdr_record` and capture again, or rename the payload file before ingestion.
- If `m2sdr_lab ingest --copy` is used with a current tree, the copied metadata is rewritten to point to the copied payload inside the lab as expected.
- If rebuilds fail with `Permission denied` on `.o` or `.d` files, check for root-owned build artifacts in `litex_m2sdr/software/user/` and remove or fix ownership before recompiling.

## Suggested public demos

- "Download this SigMF bundle and replay the exact waveform in 3 minutes."
- "Compare the same capture across PCIe hosts and verify metadata/hash compatibility."
- "Capture a timestamped burst, annotate it, replay it, and inspect the result in `m2sdr_check`."
- "Run a replay regression in CI and fail the job when the candidate capture no longer matches the baseline expectations."
- "Sweep sample rate / gain / frequency combinations and archive the resulting reports as open RF characterization data."
- "Turn a bug report into a lab bundle instead of a loose screenshot and a paragraph."
