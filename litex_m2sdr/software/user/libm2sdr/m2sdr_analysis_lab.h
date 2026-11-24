/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis, lab instrument: stimuli for the transmitter and analyses of what the
 * receiver gets back through a device under test (or a cable).
 *
 * The M2SDR transmits a known stimulus, the device under test sits between its TX and RX ports,
 * and these functions compare the capture with the stimulus. They are stateless, like the ones
 * of m2sdr_analysis.h: samples scaled to full scale 1.0, levels in dBFS (a tone of amplitude 1
 * is 0 dBFS, a wideband signal is given by its mean power). The caller sets the radio (gains,
 * attenuation, frequencies), loops the stimulus on TX and retunes for spans wider than the
 * sample rate.
 *
 * The stimuli are periodic: a period of a power of two samples, looped on TX, so that the
 * analyses use transforms of one period without windows. TX and RX of the M2SDR share their
 * clock, so a capture is periodic too; with clocks that differ, the magnitudes stay right and
 * the phases and the ACLR degrade.
 *
 * Families: sweep (scalar network analysis, with the group delay), compression (gain against
 * the input level, P1dB), imd (two tones: IM3, IM5, IP3) and amam (AM/AM, AM/PM, EVM and ACLR
 * of a wideband signal).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYSIS_LAB_H
#define M2SDR_ANALYSIS_LAB_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "m2sdr_analysis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stimuli */
/*---------*/

/* Tone of `count` samples that loops without a step: its frequency is rounded to a whole number
 * of cycles in the buffer, and given back in *freq_out (may be NULL). Level: of the tone, dBFS.
 * Returns 0, or -1 on a bad argument. */
int m2sdr_lab_tone(float complex *x, size_t count, double sample_rate, double freq_hz, double level_dbfs,
                   double *freq_out);

/* Two tones of the same level (each, dBFS) at centre_hz -+ spacing_hz / 2, rounded like
 * m2sdr_lab_tone(); f1 below f2. The peak amplitude is twice that of a tone. */
int m2sdr_lab_two_tone(float complex *x, size_t count, double sample_rate, double centre_hz, double spacing_hz,
                       double level_dbfs, double *f1_out, double *f2_out);

/* OFDM-like noise: the carriers of the transform of `period` samples (a power of two) between
 * -bw_hz / 2 and bw_hz / 2, DC left empty, all of one level and of random phases. Its spectrum
 * is flat and ends sharply, and its amplitude is near Rayleigh: a PAPR of 10 to 12 dB, a
 * stimulus for AM/AM over the whole range of levels. Level: mean power, dBFS. *papr_db (may be
 * NULL) is its peak to average ratio. Returns 0, or -1 on a bad argument or allocation failure. */
int m2sdr_lab_noise(float complex *x, size_t period, double sample_rate, double bw_hz, double level_dbfs,
                    uint32_t seed, double *papr_db);

/* PSK/QAM of random symbols under a root raised cosine (as `m2sdr_gen --signal qam`), periodic:
 * sps samples per symbol, a power of two from 2, period / sps symbols, the pulse applied in
 * frequency so that the spectrum ends at (1 + rolloff) / (2 sps) of the sample rate. Level:
 * mean power, dBFS. */
int m2sdr_lab_qam(float complex *x, size_t period, enum m2sdr_modulation modulation, unsigned sps,
                  double rolloff, double level_dbfs, uint32_t seed, double *papr_db);

/* Sweep: scalar network analysis over one span */
/*-----------------------------------------------*/

#define M2SDR_SWEEP_MAX_POINTS 1024

enum m2sdr_sweep_stimulus {
    M2SDR_SWEEP_STEPPED = 0,  /* A tone per point, period / points samples each: dynamic range. */
    M2SDR_SWEEP_CHIRP   = 1,  /* A linear chirp over the period, a little wider than the span: speed. */
};

/* The stimulus and its analysis take the same configuration. A field at 0 takes the default. */
struct m2sdr_sweep_config {
    enum m2sdr_sweep_stimulus stimulus;
    double start_hz, stop_hz; /* From the centre of the capture, within -+0.49 of the sample rate. */
    int    points;            /* 2 to M2SDR_SWEEP_MAX_POINTS; 0: 101. */
    size_t period;            /* Samples, a power of two; 0: 65536. Longer than the delays measured. */
    double level_dbfs;        /* Of the tones, or of the chirp. 0 dBFS is taken as given. */
    size_t settle;            /* Stepped: samples at the start of each tone left out; 0: a quarter of the tone. */
    /* Timing of the capture: its first sample was received while the stimulus sample
     * capture_offset (modulo the period) was sent, from the timestamps of TX and RX. Without it,
     * the delay through the device is not known: the phase is given from its linear part and the
     * group delay from its mean. */
    bool   timing_known;
    size_t capture_offset;
};

/* Stimulus of one period (cfg->period samples, defaults applied). Returns 0, or -1 on a bad
 * argument. */
int m2sdr_sweep_stimulus(const struct m2sdr_sweep_config *cfg, double sample_rate, float complex *x);

/* What a thru (the cable, or the path without the device) gives: the response of the radio,
 * removed from the later sweeps of the same configuration. */
struct m2sdr_sweep_cal {
    bool   valid;
    double start_hz, stop_hz; /* Of the points: a stepped thru calibrates a chirp sweep too. */
    int    points;
    bool   timing_known;
    double delay_samples;
    float complex h[M2SDR_SWEEP_MAX_POINTS];
};

/* Response of the path between TX and RX, at `points` frequencies evenly spaced from start_hz to
 * stop_hz. The capture is aligned on the stimulus by their circular correlation, then each point
 * is the complex ratio of the capture to the stimulus: the correlation with its tone (stepped),
 * or the least-squares ratio of the transforms of the periods over the bins of the point
 * (chirp). The periods of the capture are averaged.
 *
 * Without a calibration, S21 holds the gains of the transmitter and of the receiver; with one,
 * it is that of the device alone, and the delay that of the device. The phase holds the turn of
 * the LOs: it is relative to the thru only while the LOs are not retuned. A frequency offset
 * between the TX and the RX LOs turns the phase from tone to tone: TX and RX are tuned to the
 * same frequency from the same reference. Harmonics and mixing products of the device are not
 * measured (the response at the frequency sent only). */
struct m2sdr_sweep_result {
    bool   found;             /* The stimulus stands out of the correlation. */
    bool   calibrated;        /* S21 over that of the thru. */
    bool   delay_valid;       /* Timing known (of the thru too): delay_s, phase and group delay absolute. */
    double sync_quality;      /* Correlation peak over its mean power, dB; given without a stimulus too. */
    int    periods;           /* Averaged. */
    int    points;
    double delay_s;           /* Through the path (or the device, calibrated), when delay_valid. */
    double freq_hz[M2SDR_SWEEP_MAX_POINTS];
    double mag_db[M2SDR_SWEEP_MAX_POINTS];         /* |S21|. */
    double phase_deg[M2SDR_SWEEP_MAX_POINTS];      /* Wrapped; without the timing, its deviation
                                                    * from a straight line, unwrapped. */
    double group_delay_s[M2SDR_SWEEP_MAX_POINTS];  /* Without the timing, from its mean. */
    double snr_db[M2SDR_SWEEP_MAX_POINTS];         /* Of each point, from the spread over the
                                                    * periods; 0 with a single period. */
    double mag_min_db, mag_max_db;
    /* Raw response, for the calibration: relative to the stimulus delayed by raw_delay samples
     * (the alignment, plus the capture offset with the timing). */
    float complex raw[M2SDR_SWEEP_MAX_POINTS];
    double raw_delay;
    bool   raw_timing;
    size_t period;
    double sample_rate;
};

/* cal may be NULL (raw S21). Returns 0, or -1 on a bad argument (configuration, less than two
 * periods of samples, a calibration of another configuration) or an allocation failure. Without
 * a stimulus, found = false. */
int m2sdr_sweep_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_sweep_config *cfg,
                        const struct m2sdr_sweep_cal *cal, struct m2sdr_sweep_result *res);

/* Stores a sweep of the thru as the calibration. Returns 0, or -1 when it is not found. */
int m2sdr_sweep_cal_store(const struct m2sdr_sweep_result *thru, struct m2sdr_sweep_cal *cal);

/* Computes the figures of res from its raw response, over the calibration (NULL: raw S21). Done
 * by m2sdr_sweep_analyse(); given to apply another calibration to a result. Returns 0, or -1 on
 * a calibration of another configuration. */
int m2sdr_sweep_cal_apply(const struct m2sdr_sweep_cal *cal, struct m2sdr_sweep_result *res);

/* Compression: gain against the input level */
/*--------------------------------------------*/

#define M2SDR_COMPRESSION_MAX_STEPS 256

/* One level of the stimulus: the tone at the input of the device (dBFS of the stimulus less the
 * attenuation the caller set, or a level of its own scale) and the capture of the output. */
struct m2sdr_compression_step {
    double input_dbfs;
    const float complex *x;
    size_t count;
};

/* Gain of the device against the level of a tone. Each capture gives the level of the tone (a
 * flat-top spectrum, the noise in its band removed), the steps are sorted by input level, the
 * small-signal gain is the mean of the gains of the lowest three, and the 1 dB and 3 dB compression points
 * are interpolated between the steps around them, the compression linear in dB. The input
 * levels are trusted: an error of the attenuator steps is an error of the gain. */
struct m2sdr_compression_result {
    bool   found;             /* A tone in two steps at least: the gain is known. */
    int    steps;             /* With a tone, sorted by input level. */
    double input_dbfs[M2SDR_COMPRESSION_MAX_STEPS];
    double output_dbfs[M2SDR_COMPRESSION_MAX_STEPS];
    double gain_db[M2SDR_COMPRESSION_MAX_STEPS];
    double compression_db[M2SDR_COMPRESSION_MAX_STEPS];  /* Small-signal gain less the gain. */
    double small_signal_gain_db;
    double max_compression_db;
    double max_output_dbfs;   /* Highest output: the saturated power when it flattens. */
    bool   p1db;              /* Reached within the steps. */
    double p1db_in_dbfs, p1db_out_dbfs;
    bool   p3db;
    double p3db_in_dbfs, p3db_out_dbfs;
};

/* Level of the tone near freq_hz (searched within 1 % of the sample rate), dBFS. Returns 0,
 * -1 on a bad argument or an allocation failure, 1 without a tone (10 dB over the noise of
 * its band). */
int m2sdr_compression_tone(const float complex *x, size_t count, double sample_rate, double freq_hz,
                           double *level_dbfs);

/* From the levels already measured (output_dbfs[i] for input_dbfs[i]); n up to
 * M2SDR_COMPRESSION_MAX_STEPS. Returns 0, or -1 on a bad argument. */
int m2sdr_compression_fit(const double *input_dbfs, const double *output_dbfs, int n,
                          struct m2sdr_compression_result *res);

/* Both: the level of each step, then the fit. Steps without a tone are left out. */
int m2sdr_compression_analyse(const struct m2sdr_compression_step *steps, int n, double sample_rate, double freq_hz,
                              struct m2sdr_compression_result *res);

/* IMD: two tones */
/*----------------*/

#define M2SDR_IMD_MAX_SPURS 16

struct m2sdr_imd_config {
    double f1_hz, f2_hz;      /* The tones (m2sdr_lab_two_tone()), from the centre of the capture. */
    bool   input_known;       /* input_dbfs given: the input intercepts and the gain. */
    double input_dbfs;        /* Of each tone at the input of the device. */
    size_t fft_len;           /* 0: the spacing over 64 bins at least. */
    double spur_excursion_db; /* 0: 10 dB. */
};

/* Two-tone intermodulation. Blackman-Harris spectrum; the level of each line is the power of
 * its main lobe, the noise of that band removed. The products are those of odd order next to
 * the tones: IM3 at 2 f1 - f2 and 2 f2 - f1, IM5 at 3 f1 - 2 f2 and 3 f2 - 2 f1. The intercepts
 * are those of the mean tone level and of the stronger product of each order:
 * OIP3 = Pout + (Pout - PIM3) / 2, OIP5 = Pout + (Pout - PIM5) / 4, the input ones less the
 * gain. They are those of a small-signal device: measured where the tones are not compressed.
 * A product below the noise gives an intercept that is a lower bound (imN_measured false).
 * The spurs are the other lines, without the DC line of the receiver and the odd products up
 * to the ninth order (and the lines within 11 bins of those, not told from their skirts). The harmonics of the tones are not in a complex baseband capture. */
struct m2sdr_imd_result {
    bool   found;             /* Both tones stand out. */
    double f1_hz, f2_hz;      /* Measured. */
    double tone1_dbfs, tone2_dbfs;
    double floor_dbfs;        /* Noise in the band of a line. */
    double im3_low_dbfs, im3_high_dbfs;
    double im5_low_dbfs, im5_high_dbfs;
    bool   im3_measured, im5_measured;
    double im3_dbc, im5_dbc;  /* Stronger product, from the mean tone level. */
    double oip3_dbfs, oip5_dbfs;
    bool   input_known;
    double gain_db;
    double iip3_dbfs, iip5_dbfs;
    int    nspurs;            /* Strongest first. */
    double spur_hz[M2SDR_IMD_MAX_SPURS];
    double spur_dbfs[M2SDR_IMD_MAX_SPURS];
};

/* Returns 0, or -1 on a bad argument (tones out of the span or too close for the transform,
 * count below the transform) or an allocation failure. Without the tones, found = false. */
int m2sdr_imd_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_imd_config *cfg,
                      struct m2sdr_imd_result *res);

/* AM/AM, AM/PM: a wideband signal against its reference */
/*-------------------------------------------------------*/

#define M2SDR_AMAM_BINS       64
#define M2SDR_AMAM_MAX_POINTS 4096

struct m2sdr_amam_config {
    double channel_bw_hz;      /* ACLR: 0: the band of the reference. */
    double channel_spacing_hz; /* 0: the channel bandwidth. */
    double linear_rms;         /* Input amplitude, over the RMS of the reference, below which the
                                * device is linear: the small-signal gain; 0: 0.5. */
};

/* The device between the transmitter of a known periodic signal (m2sdr_lab_noise(),
 * m2sdr_lab_qam(), one period of a power of two samples) and the receiver.
 *
 * The capture is aligned on the reference: whole samples from their circular correlation, the
 * fraction from the slope of the phase of their cross-spectrum, the reference then delayed by
 * it. The input amplitudes are binned from 0 to the peak of the reference; in each bin, the
 * least-squares complex gain of the output over the input gives the AM/AM and the AM/PM: free
 * of the bias of the noise. They are relative to the small-signal gain, the bins below
 * linear_rms extrapolated to zero amplitude (a gain linear in the square of the amplitude).
 *
 * The model is memoryless: the linear response of the path (filters of the radio and of the
 * device) and the memory of the device spread the points and add to the EVM. The EVM is that
 * of the waveform after the least-squares gain, not that of demodulated symbols (m2sdr_demod()
 * for a QAM reference). The ACLR is from the transforms of the periods: no window, the
 * channel the band of the reference, and the adjacent channels that fit in the span. */
struct m2sdr_amam_result {
    bool   found;              /* The reference stands out of the correlation. */
    double sync_quality;       /* Correlation peak over its mean power, dB. */
    int    periods;
    double delay_samples;      /* Of the capture on the reference, modulo the period. */
    double gain_db, phase_deg; /* Small-signal. */
    double input_rms_dbfs, input_peak_dbfs;
    double output_rms_dbfs, output_peak_dbfs;
    double evm_rms;            /* Fraction: 0.02 is 2 %. */
    double evm_db;
    double compression_db;     /* At the highest bin of 100 samples or more. */
    double am_pm_deg;          /* There. */
    double compression_in_dbfs;
    /* Per bin of input amplitude: its RMS amplitude (dBFS of the reference), the output (dBFS of
     * the capture, over the small-signal gain: equal to the input for a linear device), the gain
     * and the phase from the small-signal ones, and its samples. */
    int    bins;
    float  bin_in_dbfs[M2SDR_AMAM_BINS];
    float  bin_out_dbfs[M2SDR_AMAM_BINS];
    float  bin_gain_db[M2SDR_AMAM_BINS];
    float  bin_phase_deg[M2SDR_AMAM_BINS];
    uint32_t bin_count[M2SDR_AMAM_BINS];
    /* ACLR of the output. */
    bool   aclr;
    double channel_bw_hz, channel_spacing_hz;
    struct m2sdr_acpr acpr;
    /* Samples, every so many: input amplitude, output amplitude over the small-signal gain, and
     * phase from it. */
    int    npoints;
    float  point_in[M2SDR_AMAM_MAX_POINTS];
    float  point_out[M2SDR_AMAM_MAX_POINTS];
    float  point_phase_deg[M2SDR_AMAM_MAX_POINTS];
};

/* ref holds one period of the reference (a power of two samples, 64 at least), cfg may be NULL.
 * Returns 0, or -1 on a bad argument (less than two periods of samples) or an allocation
 * failure. Without the reference in the capture, found = false. */
int m2sdr_amam_analyse(const float complex *x, size_t count, double sample_rate, const float complex *ref,
                       size_t period, const struct m2sdr_amam_config *cfg, struct m2sdr_amam_result *res);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_LAB_H */
