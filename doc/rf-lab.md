# LiteX-M2SDR RF Lab

LiteX-M2SDR is now positioned not only as an SDR board, but as an open and reproducible RF experimentation platform.

The practical building blocks are already in-tree:

- `m2sdr_record` can emit SigMF captures with board metadata and annotations.
- `m2sdr_play` can replay raw or SigMF-backed captures.
- `m2sdr_check` and `m2sdr_sigmf_info` provide visual and CI-friendly inspection.
- `m2sdr_lab` adds experiment-level organization, replay tracking, verification, headless reports, and portable bundles.

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

Export a bundle for another user or CI job:

```bash
./m2sdr_lab bundle ../../../../my_rf_lab
```

## Suggested public demos

- "Download this SigMF bundle and replay the exact waveform in 3 minutes."
- "Compare the same capture across PCIe hosts and verify metadata/hash compatibility."
- "Capture a timestamped burst, annotate it, replay it, and inspect the result in `m2sdr_check`."
- "Run a replay regression in CI and fail the job when the candidate capture no longer matches the baseline expectations."
- "Turn a bug report into a lab bundle instead of a loose screenshot and a paragraph."
