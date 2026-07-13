# SATA Bandwidth Investigation And Plan

Date: 2026-07-13. Branch: `feature/sata-gqrx-shared-streaming`.
Bitstream: `build/litex_m2sdr_baseboard_eth_sata` (Ethernet + SATA baseboard,
Gen2, 125 MHz sys_clk). Transport: Etherbone at `192.168.1.50`, JTAG via
FT4232H for recovery. Drive: WDC WDS120G1G0A-00SS50 (WD Green 120 GB,
DRAM-less SM2258XT, TLC), firmware Z3311000.

## Question

`capture-current` cannot sustain 10 MS/s SC16 1T1R (38.15 MiB/s) without
backpressuring the shared GQRX RX path. Is the ~18-20 MiB/s sustained rate a
drive limit, a gateware limit, or a host-path limit?

## Answer

The sustained-write bottleneck is the SSD itself, in its current state. The
gateware and host paths are exonerated for the ~20 MiB/s cap. Two additional
independent problems were found and root-caused on hardware: a hard deadlock
of the whole Ethernet endpoint when replaying to an unconfigured UDP
destination, and a deliberate-but-costly Etherbone read regression.

## Measurements (2026-07-13 hardware run)

Link and drive state from raw ATA IDENTIFY (drained via
`sata_identify_source_*` CSRs): negotiated speed Gen2, write cache enabled,
look-ahead enabled, NCQ supported (unused by LiteSATA), LBA48, 234441648
sectors.

| Path | Test | Result |
| ---- | ---- | ------ |
| Etherbone burst write (staging RAM only) | `diag etherbone-bench`, 128 words | 139.5 MiB/s |
| Etherbone burst read (staging RAM only) | `diag etherbone-bench`, 128 words | 7.0 MiB/s |
| Host->SATA (Mem2Sector, 128 KiB cmds) | 16 MiB `diag write` after ~5 min idle | 55.0 MiB/s (path ceiling) |
| Host->SATA (Mem2Sector) | same, immediately again | 8.6 then 20.2 MiB/s |
| Host->SATA (Mem2Sector) | 1 GiB `diag write` | 15.1 MiB/s average |
| Host->SATA (Mem2Sector) | 16 x 64 MiB chunks, per-chunk timing | 10.7-24.6 MiB/s, flat |
| Host->SATA (Mem2Sector) | 16 x 16 MiB chunks after ~20 min idle | 7.5-51.4 MiB/s, avg ~13 |
| RF->SATA (Stream2Sectors, 32 MiB cmds) | `capture` 234 MiB @ 30.72 MS/s SC16 (117 MiB/s demand) | 22.8 MiB/s average, flat from first command |
| SATA->host (Sector2Mem + Etherbone read) | 256 MiB `diag read` | 6.4 MiB/s (host path limit, see below) |

Key observations:

- Both write paths (128 KiB commands via the staging buffer, 32 MiB
  streaming commands via `Stream2Sectors`) converge on the same 10-25 MiB/s
  sustained rate. Per-command overhead is therefore not the limiter.
- The same drive absorbs 51-55 MiB/s (the Etherbone-fed path ceiling) for the
  first ~16-32 MiB after idle time, then collapses. This is SLC-cache
  behavior: the effective cache is currently only a few tens of MiB and
  post-cache/GC writes run at ~10-25 MiB/s.
- Reads from never-written LBAs (FTL returns zeros, no NAND access) are as
  slow as reads of written data, proving the 6.4 MiB/s read number is the
  host Etherbone path, not the drive.

## Findings

### 1. Drive sustained write commit rate (primary capture bottleneck)

The WDS120G1G0A is a DRAM-less TLC drive. Field reports for healthy units
show 60-120 MB/s post-SLC-cache sequential writes; this unit sustains only
~10-25 MiB/s with a much smaller-than-expected SLC window. Likely causes:
the volume has never been TRIMmed (LiteSATA has no DATA SET MANAGEMENT
support, so from the FTL's perspective every LBA ever written stays valid),
possible wear (2017-era unit, 40 TBW rating), and bottom-tier sustained
behavior of this model.

Practical envelope with this drive today: sustained captures must stay below
~12-15 MiB/s to keep GQRX smooth. 4 MS/s SC16 1T1R (15.26 MiB/s) is
borderline; 10 MS/s SC16 (38.15 MiB/s) is far above it, matching the
observed 2x-slow backpressured captures.

The `sata-workflows.md` claim that 18-20 MiB/s is "a sustained-drive limit,
not an Ethernet or FIFO-capacity limit" is confirmed, with the refinement
that the limit is drive-state-dependent (8-25 MiB/s observed) and that the
burst ceiling through the same gateware is at least 55 MiB/s.

### 2. Replay to an unconfigured UDP destination wedges the whole board

Reproduced twice, root-caused, and verified on hardware:

- `LiteEthStream2UDPTX` resets `ip_address` to 0.0.0.0
  (`liteeth/frontend/stream.py`); only a SoapySDR/GQRX session programs it
  (`libm2sdr/m2sdr_stream.c`). `m2sdr_sata serve NAME` / `diag replay ... eth`
  do not.
- After a bitstream (re)load with no GQRX session, `serve`/`replay` push a
  UDP stream toward 0.0.0.0. ARP never resolves, the stalled packetizer
  holds LiteEth's shared TX path, and ICMP + Etherbone die with it. The
  board needs a JTAG bitstream reload (`openFPGALoader --cable ft4232 ...`).
- Verification: after reload, `eth_rx_streamer_ip_address` (0x7804) read 0.
  Writing the host IP (0xC0A80164) and re-running the identical
  `serve eth_roundtrip_1m` completed in 0.141 s with the board healthy.

This is a first-class robustness bug for the shared-streaming feature: any
user running `serve` before a GQRX session hard-bricks the Ethernet endpoint
until JTAG/power recovery.

### 3. Etherbone read window regression (deliberate, this branch)

`M2SDR_SATA_ETHERBONE_READ_WINDOW` was reduced 8 -> 1 to keep catalog reads
cooperative with a live GQRX client, and
`eb_read32_bulk_pipeline_checked()` degenerates to stop-and-wait for
window <= 1. Export/read throughput dropped from ~34 MiB/s to ~6.4 MiB/s.
Write bursts are unaffected (139.5 MiB/s raw). This does not affect capture
bandwidth, only `export`/`diag read`.

## Re-Test With A Samsung 850 EVO 250GB (2026-07-13)

Swapping the drive confirmed the diagnosis. Same gateware, same host paths:

| Path | Test | WDS120G1G0A | 850 EVO |
| ---- | ---- | ----------- | ------- |
| Host -> SATA | 1 GiB `diag write` | 15.1 MiB/s | 43.9 MiB/s (path limit) |
| RF -> SATA | 10 MS/s SC16 1T1R, 10 s | ~19 MiB/s, 2x slow | 38.13 MiB/s, real-time |
| RF -> SATA | 30.72 MS/s SC16 1T1R | ~23 MiB/s | 117.15 MiB/s, real-time |
| RF -> SATA | 30.72 MS/s SC16 2T2R, 15 s (3.43 GiB) | - | 232.94 MiB/s, real-time |

The 2T2R run wrote 3.43 GiB, past the EVO's TurboWrite cache, with no fade:
the `Stream2Sectors` path sustains at least 233 MiB/s, close to the practical
Gen2 payload ceiling. The RFIC rate, not SATA, is now the capture limit.

After a drive hot-plug the PHY needs a re-init (`sata_phy_enable` 0 -> 1) and
IDENTIFY can briefly return partial data while the link trains; the identify
hardening below came out of that observation.

## Fixes Implemented (this branch)

- `libm2sdr`: partially drained IDENTIFY data is no longer decoded (it
  reported a bogus LBA28-sized 128 GiB drive during link training).
- `m2sdr_sata`: Ethernet replays program a resolvable UDP destination before
  the RX path is routed to the LiteEth streamer, closing the
  wedge described in finding 2. Verified on hardware: `serve` on a
  freshly-loaded bitstream now completes with the board healthy.
- `m2sdr_sata`: Etherbone catalog reads use the full 8-deep pipeline window
  again when no live client is streaming (6.4 -> 26-30 MiB/s measured,
  readback `cmp`-verified) and keep the cooperative single-burst window
  during a concurrent GQRX session. A 16-deep window measured slower
  (19 MiB/s): in-flight records overrun the gateware's 160-word record
  buffering, so 8 is the sweet spot for the current gateware.

## Remaining Plan

Ordered by leverage:

1. Drive maintenance support (optional, in-tree): add ATA DATA SET
   MANAGEMENT (TRIM) and SMART READ DATA support to LiteSATA +
   `m2sdr_sata trim|health`, so a capture volume can be maintained and
   monitored in place. The degraded WDS120G1G0A showed what an
   unmaintained drive does to sustained rates; a health command would have
   pointed at the drive immediately.

2. LiteEth robustness (upstream): a UDP stream toward an unresolved ARP
   target should time out and drop rather than hold the shared TX arbiter
   forever; the host-side destination fix closes the common path, but the
   gateware can still be wedged by any misprogrammed destination.
   Reproduce with `--with-eth-tx-probe` + LiteScope; fix belongs in LiteEth
   with a test there.

3. Etherbone read concurrency (gateware): pipelined reads scale to
   window=8 (26-30 MiB/s) but regress at window=16 because the shared
   Etherbone record path buffers only one 128-word record
   (`ETH_ETHERBONE_BUFFER_DEPTH = 160`). Deeper record buffering or a
   second in-flight record would raise the export ceiling toward the
   ~56 MiB/s the round-trip time allows.

4. Identify robustness: RESOLVED 2026-07-13. Two combined causes: LiteSATA
   ended IDENTIFY at the first DATA FIS `last` although a drive may split
   the PIO block into several DRQ blocks (fixed in LiteSATA, branch
   `fix-multi-fis-identify`: count the 128-dword block in the command layer
   and identify frontend, accept intermediate PIO Setup FISes), and the
   host fired identify starts into a still-busy engine while draining only
   half the block (fixed in `libm2sdr`: wait for idle, drain the full
   block, retry while a slow transfer is still arriving). The 850 EVO
   stalls identify for hundreds of milliseconds while garbage-collecting,
   which is what exposed both. Back-to-back `info` went from ~60% failures
   to 0/60 on hardware.

5. Observability for future SATA work (per the debugging guide and the
   ai-era-fpga workflow: prefer small CSRs, then narrow probes).
   - CSR counters on the LiteSATA user port: commands issued, data words
     transferred, stall (valid & !ready) cycles, per streamer. Lets
     `m2sdr_sata info` print live throughput and separates drive stalls
     from FIFO starvation in the field without a bench.
   - A drive-independent write benchmark: feed `Stream2Sectors` from a
     gateware counter/PRBS source (crossbar diag route) so the true
     gateware SATA ceiling can be measured; today it can only be bounded
     from below (>= 55 MiB/s) because the drive saturates first.
   - Report negotiated SATA gen and write-cache state in
     `m2sdr_sata info` (already available in IDENTIFY words 77/85).

## Repro Commands

```sh
# Sustained write profile (per-chunk timing, safe raw region):
for i in $(seq 0 15); do
  S=$(printf "0x%x" $((0x2000000 + i*32768)))
  time ./m2sdr_sata -i 192.168.1.50 diag write /tmp/rand_1g.bin $S 32768
done

# Streamer-path write rate (progress lines print cumulative MiB/s):
./m2sdr_sata -i 192.168.1.50 --timeout-ms 90000 capture bench \
    --seconds 2 --sample-rate 30720000 --format sc16 --channel-layout 1t1r \
    --rx-freq 100M --rx-gain 20 --bandwidth 20M
./m2sdr_sata -i 192.168.1.50 delete bench

# LiteEth-endpoint wedge recovery (fixed host-side; the gateware can still
# be wedged by a misprogrammed streamer destination, see Remaining Plan 2):
openFPGALoader --cable ft4232 \
    build/litex_m2sdr_baseboard_eth_sata/gateware/litex_m2sdr_baseboard_eth_sata.bit
```

Note: the installed OpenOCD 0.11 cannot run LiteX's JTAGBone stream script
(`invalid command name "binary"`; needs OpenOCD >= 0.12) and its `ftdi`
command syntax predates the configs in `litex/prog/`. openFPGALoader works
for recovery. Upgrading OpenOCD would restore JTAGBone as an
Ethernet-independent debug path, which this investigation would have used
to inspect the wedged state instead of destroying it with a reload.
