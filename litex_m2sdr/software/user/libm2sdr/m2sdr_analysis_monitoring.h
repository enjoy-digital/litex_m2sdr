/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis, monitoring: pulsed signals and EMC pre-compliance.
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

/* EMC */
/*-----*/

#define M2SDR_EMC_MAX_CHANNELS   2048
#define M2SDR_EMC_MAX_LIMIT      32

/* Frequency bands of CISPR 16-1-1 and their measuring receivers. */
enum m2sdr_emc_band {
    M2SDR_EMC_BAND_AUTO = 0,    /* From centre_hz. */
    M2SDR_EMC_BAND_A    = 1,    /* 9 to 150 kHz: 200 Hz, QP 45 ms / 500 ms / 160 ms. */
    M2SDR_EMC_BAND_B    = 2,    /* 0.15 to 30 MHz: 9 kHz, QP 1 ms / 160 ms / 160 ms. */
    M2SDR_EMC_BAND_CD   = 3,    /* 30 to 1000 MHz: 120 kHz, QP 1 ms / 550 ms / 100 ms. */
};

/* A point of a line in frequency: the line is linear in log frequency between its points, as
 * CISPR limits are, and flat beyond its ends. Two points at one frequency make a step: the
 * upper part applies above it. */
struct m2sdr_emc_point {
    double freq_hz;             /* Absolute. */
    double level_db;
};

struct m2sdr_emc_config {
    enum m2sdr_emc_band band;
    double centre_hz;           /* Absolute frequency of the centre of the capture. */
    /* Channels from start_hz to stop_hz (from the centre of the capture) every step_hz. All at 0:
     * the whole capture but 1.5 RBW at each edge, every RBW / 2. */
    double start_hz, stop_hz, step_hz;
    /* The capture is one period of a stationary emission (false), as for a live view: the
     * detectors are in their steady state; or one event (true): they start at zero and the
     * signal is absent after the capture, as for an isolated pulse. */
    bool   single;
    /* Factor added to the dBFS of the detectors to give the unit of the limits (dBuV, dBuV/m):
     * a constant, plus a line when nfactor > 0 (antenna factor, cable loss). */
    double offset_db;
    int    nfactor;
    struct m2sdr_emc_point factor[M2SDR_EMC_MAX_LIMIT];
    /* Limit lines, in the unit of the limits, of the quasi-peak (the peak is checked against it
     * too, as a pre-scan) and of the average detector. 0 points: not checked. */
    int    nqp_limit, nav_limit;
    struct m2sdr_emc_point qp_limit[M2SDR_EMC_MAX_LIMIT];
    struct m2sdr_emc_point av_limit[M2SDR_EMC_MAX_LIMIT];
};

/* EMC pre-compliance: the measuring receiver of CISPR 16-1-1 on a bank of channels.
 *
 * Each channel is the capture through a Gaussian filter of the 6 dB bandwidth of the band
 * (applied in frequency on one FFT of the capture, then brought to baseband at a rate of at
 * least 3 RBW), and its envelope through the three detectors:
 * - peak: the highest envelope;
 * - quasi-peak: a diode on the crests of the IF carrier, its charge and discharge time
 *   constants those of the band (the charge one as CISPR defines it: 63 % of the final output
 *   on a sine applied at once), then the critically damped meter, its reading the highest
 *   deflection. It follows the pulse response curves of CISPR 16-1-1 in the three bands;
 * - average (CISPR-AV): the linear envelope through the same meter, the highest deflection.
 * The levels are in dBFS of the envelope: a CW of power P dBFS in the channel reads P on all
 * three. Calibration, antenna and cable factors are the caller's (offset_db, factor).
 *
 * Not covered: band E (above 1 GHz), the selectivity mask beyond the 6 dB bandwidth, overload
 * of the receiver, the APD and the RMS-average detector. The FFT takes the largest power of two
 * of samples of the capture; in the steady state (single = false) it is taken as periodic, so
 * an impulsive emission should repeat a whole number of times in it. A capture shorter than
 * the time constants gives the steady state of a repeating capture, not a long measurement.
 *
 * Results per channel: frequency, the three levels and, with limits, the margins (limit minus
 * level, negative fails). */
struct m2sdr_emc_result {
    enum m2sdr_emc_band band;
    double rbw_hz;              /* 6 dB bandwidth. */
    double impulse_bw_hz;       /* Of the filter: its integral over its peak. */
    double charge_s, discharge_s, meter_s;   /* Quasi-peak. */
    double av_meter_s;          /* Average. */
    double capture_s;           /* Samples used. */
    double env_rate_hz;         /* Sample rate of the envelopes. */
    int    nchannels;
    double freq_hz[M2SDR_EMC_MAX_CHANNELS];   /* Absolute. */
    float  peak_dbfs[M2SDR_EMC_MAX_CHANNELS];
    float  qp_dbfs[M2SDR_EMC_MAX_CHANNELS];
    float  av_dbfs[M2SDR_EMC_MAX_CHANNELS];
    /* With limits (margins in dB, 999 where there is no limit). */
    bool   limits;
    float  margin_peak_db[M2SDR_EMC_MAX_CHANNELS];   /* Against the quasi-peak limit. */
    float  margin_qp_db[M2SDR_EMC_MAX_CHANNELS];
    float  margin_av_db[M2SDR_EMC_MAX_CHANNELS];
    bool   pass;                /* Quasi-peak and average within their limits. */
    double worst_margin_db;     /* Lowest margin of the quasi-peak and the average... */
    double worst_freq_hz;       /* ...its channel... */
    int    worst_detector;      /* ...and its detector: 1 quasi-peak, 2 average. */
    /* Strongest channel by its peak. */
    double max_peak_dbfs, max_peak_freq_hz;
};

/* Returns 0, or -1 on a bad argument (sample rate, fewer than 4096 samples or not enough for 8
 * bins of the FFT in the RBW, a rate under 3 RBW, a channel within 1.5 RBW of an edge, more
 * than M2SDR_EMC_MAX_CHANNELS channels, a bad line) or an allocation failure. cfg may be NULL:
 * band B, channels by default, no limits. Without a band, the one of centre_hz, B when it is 0.
 * By default, channels every RBW / 2, or wider to keep within M2SDR_EMC_MAX_CHANNELS. */
int m2sdr_emc_analyse(const float complex *x, size_t count, double sample_rate,
                      const struct m2sdr_emc_config *cfg, struct m2sdr_emc_result *res);

/* Level of a line at freq_hz, interpolated in log frequency; NAN with no points. */
double m2sdr_emc_line_db(const struct m2sdr_emc_point *line, int n, double freq_hz);

/* Time constants of a band (charge, discharge, quasi-peak meter, average meter) and its RBW;
 * -1 on a bad band. */
int m2sdr_emc_band_params(enum m2sdr_emc_band band, double *rbw_hz, double *charge_s, double *discharge_s,
                          double *meter_s, double *av_meter_s);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_MONITORING_H */
