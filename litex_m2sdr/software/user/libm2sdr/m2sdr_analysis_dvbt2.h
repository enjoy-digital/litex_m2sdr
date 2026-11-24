/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: DVB-T2 reception (EN 302 755).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYSIS_DVBT2_H
#define M2SDR_ANALYSIS_DVBT2_H

#include "m2sdr_analysis.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M2SDR_DVBT2_BINS        512     /* Groups of carriers of the per-carrier traces. */
#define M2SDR_DVBT2_ECHO_BINS   512     /* Points of the echo profile. */
#define M2SDR_DVBT2_MAX_POINTS  4096
#define M2SDR_DVBT2_MAX_P1      8

/* A DVB-T2 multiplex (EN 302 755), SISO, tuned near its centre: a reception check, as a TV
 * installer's meter does, not a decoder. The sample rate is the elementary rate of the channel,
 * 8/7 of its bandwidth, as for DVB-T: 64/7 MSPS for 8 MHz, 8 MSPS for 7 MHz, 48/7 MSPS for 6 MHz.
 *
 * Measured, without any signalling:
 * - the P1 symbols, from their C-A-B structure (the two frequency shifted copies of the main part):
 *   their place, the T2 frame length when two are in the capture, and a fractional frequency;
 * - the FFT size (1k to 32k) and the guard interval (1/128 to 1/4), from the correlation of the
 *   guard intervals; normal or extended carriers from the width of the spectrum;
 * - the scattered pilot pattern PP1 to PP8, from the carriers that repeat every Dy symbols, and
 *   the integer carrier offset from the reference sequence (PRBS) of those pilots;
 * - the channel (scattered pilots of the last Dy symbols, linear in frequency), its echoes;
 * - the noise on the continual and edge pilots (found as the carriers that stay the same from
 *   symbol to symbol), against the channel: a pilot MER that does not depend on the constellation;
 * - the constellation of the data cells, the one their I and Q fit best among QPSK, 16, 64 and
 *   256QAM, rotated or not, and the MER of the data cells against it.
 *
 * Not measured, and why:
 * - S1/S2 of the P1 are not decoded: that needs the carrier distribution and the modulation
 *   sequences of the standard (its tables), which this file does not reproduce. The P1 is
 *   detected from its structure only.
 * - L1-pre and L1-post (LDPC/BCH coded) are not decoded: no code rate, PLP, cell identity...
 *   The P2 symbols after a P1 are left out of the analysis.
 * - A rotated constellation carries the Q of a cell in another cell (cyclic Q delay, then the
 *   interleavers): it cannot be sliced without deinterleaving. It is detected, and no data MER is
 *   given then (mer_valid false): the pilot MER stands for it. A rotated 64 or 256QAM spreads its
 *   I and Q almost uniformly: it is told from a square one only by the absence of a grid where the
 *   pilot MER says one should show; its order is then not known.
 * - MISO (two transmitters with opposite pilots) is not resolved: the channel reads wrong.
 * - Cells that are not data are not told apart besides the pilots: several PLPs of different
 *   constellations, dummy cells, the carriers of tone reservation all count in the data MER.
 * - FEF parts (S1) are not identified: a capture with them may read no multiplex.
 *
 * A capture needs at least Dy + 2 symbols after or before a P1 (PP8: 18 symbols), and more for
 * the continual pilots (8): 150 ms suit every mode. */
struct m2sdr_dvbt2_result {
    bool   found;               /* Guard intervals found: an OFDM signal. */
    int    fft_len;             /* 1024 to 32768. */
    bool   extended;            /* Extended carrier mode (8k, 16k, 32k). */
    int    carriers;
    int    guard_num, guard_den;   /* Guard interval: guard_num / guard_den of the useful part. */
    double guard_us;
    double sync_quality;        /* Correlation of the guard intervals, 0 to 1; given without a signal too. */
    double sync_snr_db;         /* The signal to noise ratio it gives: a rough figure. */
    double power_dbfs;          /* Of the capture. */
    /* P1 symbols. */
    int    p1_count;
    size_t p1_sample[M2SDR_DVBT2_MAX_P1];   /* Their first sample. */
    double p1_quality;          /* Correlation of the C-A-B parts of the best one, 0 to 1. */
    double p1_freq_hz;          /* Frequency from it, modulo sample_rate / 1024. */
    double frame_ms;            /* From one P1 to the next; 0 with fewer than two. */
    bool   s1s2;                /* S1/S2 decoded: always false (see above). */
    /* Pilots. */
    bool   pilots;              /* Pattern and offset found: the figures below are valid. */
    int    pattern;             /* 1 to 8: PP1 to PP8; 0 not found. */
    int    pattern_dx, pattern_dy;
    double pattern_quality;     /* Coherence of the pilots from Dy symbols to the next, 0 to 1. */
    double pilot_quality;       /* Coherence of neighbour pilots with their PRBS, 0 to 1. */
    double freq_error_hz;       /* Centre of the multiplex, from the centre of the capture. */
    int    carrier_offset;      /* Its whole carriers. */
    int    symbols;             /* Analysed: data symbols, the P2 left out. */
    int    continual_pilots;    /* Found (edge pilots included). */
    bool   pilot_mer_valid;
    double pilot_mer_db;        /* Noise on the continual pilots over the data power. */
    /* Constellation of the data cells. */
    enum m2sdr_modulation modulation;   /* QPSK to 256QAM; AUTO: not found. */
    bool   rotated;             /* Rotated constellation (with modulation AUTO: order not known). */
    double fit;                 /* Of the constellation found: 1 for none, more for a better fit. */
    bool   mer_valid;           /* A square constellation found or given: mer_db is valid. */
    double mer_db;
    /* Per group of carriers, the lowest first: MER (when valid), and power of the channel in dB
     * from its mean. bins groups of carriers_per_bin carriers each. */
    int    bins;
    double carriers_per_bin;
    float  mer_bin_db[M2SDR_DVBT2_BINS];
    float  channel_bin_db[M2SDR_DVBT2_BINS];
    /* Echo profile: impulse response of the channel in dB from its strongest path, point i at
     * echo_start_us + i * echo_step_us from that path. */
    float  echo_db[M2SDR_DVBT2_ECHO_BINS];
    double echo_start_us, echo_step_us;
    double echo_delay_us;       /* The strongest other path: its delay and its level. */
    double echo_level_db;
    /* Equalised data cells, mean power 1. */
    int    npoints;
    float complex points[M2SDR_DVBT2_MAX_POINTS];
};

/* What is already known of a multiplex, from an earlier result or its signalling: a field at 0
 * (AUTO, false) is found. */
struct m2sdr_dvbt2_config {
    int    fft_len;             /* With guard_num and guard_den. */
    int    guard_num, guard_den;
    int    pattern;             /* 1 to 8. */
    enum m2sdr_modulation modulation;   /* The data MER against it, with rotated. */
    bool   rotated;
    bool   offset_known;        /* carrier_offset of an earlier result: searched around it only. */
    int    carrier_offset;
};

/* Returns 0, or -1 on a bad argument (sample rate, fewer than 10240 samples) or an allocation
 * failure. Without a multiplex, found = false. cfg may be NULL. */
int m2sdr_dvbt2_analyse(const float complex *x, size_t count, double sample_rate,
                        const struct m2sdr_dvbt2_config *cfg, struct m2sdr_dvbt2_result *res);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_DVBT2_H */
