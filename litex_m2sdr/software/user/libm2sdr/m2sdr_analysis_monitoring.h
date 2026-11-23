/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis, monitoring: pulsed signals.
 *
 * Like the functions of m2sdr_analysis.h: samples scaled to full scale 1.0, no state besides
 * the arguments.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYSIS_MONITORING_H
#define M2SDR_ANALYSIS_MONITORING_H

#include "m2sdr_analysis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pulses */
/*--------*/

#define M2SDR_PULSE_MAX_PULSES   512
#define M2SDR_PULSE_MAX_STAGGER  8

enum m2sdr_pulse_modulation {
    M2SDR_PULSE_CW    = 0,      /* One frequency. */
    M2SDR_PULSE_LFM   = 1,      /* Linear FM chirp: chirp_hz_per_s. */
    M2SDR_PULSE_OTHER = 2,      /* Neither: phase code, nonlinear FM, noise. Not identified further. */
};

enum m2sdr_pri_kind {
    M2SDR_PRI_NONE     = 0,     /* Fewer than three whole pulses. */
    M2SDR_PRI_CONSTANT = 1,
    M2SDR_PRI_STAGGER  = 2,     /* stagger PRIs repeated in turn. */
    M2SDR_PRI_JITTER   = 3,     /* No repeating pattern: jitter, agility, a missed pulse or two emitters. */
};

struct m2sdr_pulse_info {
    double start_s;             /* Leading edge at 50 % of the amplitude, from the first sample. */
    double width_s;             /* Between the 50 % amplitude points. */
    double peak_dbfs;           /* Highest sample power. */
    double mean_dbfs;           /* Mean power of the top, between the 90 % points (the 50 % ones when short). */
    double rise_s, fall_s;      /* 10 % to 90 % of the amplitude, and back. */
    double freq_hz;             /* At the middle of the pulse, from the centre of the capture. */
    double chirp_hz_per_s;      /* Linear frequency slope. */
    double coherence;           /* 0 to 1: how much of the pulse the CW or LFM model holds. */
    enum m2sdr_pulse_modulation modulation;
    bool   truncated;           /* Cut by an end of the capture: start or width not valid. */
};

/* Pulsed signals: radar, DFS test patterns, pulsed interferers. One emitter at a time is
 * assumed for the PRI analysis.
 *
 * The pulses are found on the power envelope averaged over smooth_s, above the noise floor by
 * threshold_db, with 3 dB of hysteresis. The noise floor is a low percentile of the power of
 * blocks of the capture: adaptive to the level of the capture, right while the pulses fill
 * less than about two thirds of it. Each pulse is then measured on the raw amplitude: its top
 * is the median amplitude over the detection, its edges the 50 % crossings, its rise and fall
 * the 10 % and 90 % crossings (meaningful above about 25 dB of SNR, and when longer than a
 * few samples). Inside the pulse, between the 90 % points, the chirp rate comes from the lag
 * product of the lag-1 products, the frequency from the lag-1 product of the dechirped samples.
 *
 * The train: the intervals between the leading edges of the whole pulses are searched for the
 * shortest pattern that repeats, within max(2 samples, 0.5 %). Constant or staggered PRI, and
 * the duty cycle: mean width over mean PRI. A missed pulse breaks the pattern (JITTER). */
struct m2sdr_pulse_result {
    bool   found;               /* At least one pulse. */
    double noise_dbfs;          /* Noise floor, power per sample. */
    double threshold_dbfs;
    double power_dbfs;          /* Of the capture. */
    int    npulses;             /* Found; the first M2SDR_PULSE_MAX_PULSES are listed and analysed. */
    int    nlisted;
    struct m2sdr_pulse_info pulses[M2SDR_PULSE_MAX_PULSES];
    /* Summary of the whole pulses. */
    double width_s;             /* Means. */
    double peak_dbfs;
    double rise_s, fall_s;
    double freq_hz;
    double chirp_hz_per_s;
    enum m2sdr_pulse_modulation modulation;   /* Of most pulses. */
    /* Train. */
    enum m2sdr_pri_kind pri_kind;
    int    stagger;             /* PRIs of the pattern: 1 constant, 2 to M2SDR_PULSE_MAX_STAGGER staggered. */
    double pri_s[M2SDR_PULSE_MAX_STAGGER];    /* In the order of the train, from its first pulse. */
    double pri_mean_s;          /* Mean interval: of the pattern when there is one. */
    double pri_min_s, pri_max_s, pri_std_s;   /* Of the intervals. */
    double prf_hz;              /* 1 / pri_mean_s. */
    double duty_cycle;          /* 0 to 1. */
};

/* Known or wanted parameters; NULL or a field at 0 for the default. */
struct m2sdr_pulse_config {
    double threshold_db;        /* Detection over the noise floor: 10 dB. */
    double smooth_s;            /* Averaging of the detection envelope: 4 samples. */
    double min_width_s;         /* Shorter detections are ignored: none. */
};

/* Returns 0, or -1 on a bad argument (sample rate, fewer than 1024 samples) or an
 * allocation failure. Without pulses, found = false. */
int m2sdr_pulse_analyse(const float complex *x, size_t count, double sample_rate,
                        const struct m2sdr_pulse_config *cfg, struct m2sdr_pulse_result *res);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_MONITORING_H */
