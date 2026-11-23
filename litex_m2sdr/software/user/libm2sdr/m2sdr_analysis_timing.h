/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis, timing family: frequency stability of a carrier (Allan deviation) and
 * GPS L1 C/A acquisition with a jamming indicator.
 *
 * Like the functions of m2sdr_analysis.h, they take samples scaled to full scale 1.0 and keep
 * no state besides their arguments; the series of the Allan deviation is a state the caller
 * owns.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYSIS_TIMING_H
#define M2SDR_ANALYSIS_TIMING_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "m2sdr_analysis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allan deviation */
/*-----------------*/

#define M2SDR_ADEV_MAX_TAUS    40      /* Octaves of tau. */
#define M2SDR_ADEV_MAX_BLOCKS  1024    /* Sub-blocks of a capture for its short-tau deviation. */

/* Overlapping Allan deviation and modified Allan deviation of a fractional frequency, at
 * tau = m tau0 for m = 1, 2, 4... while at least one second difference (three for the
 * modified one) fits. terms[i] is the number of second differences averaged for adev[i]:
 * the deviation is known to about 1 / sqrt(terms) for white noises. mdev[i] is 0 when the
 * series is too short for it. */
struct m2sdr_adev {
    int    ntau;
    double tau_s[M2SDR_ADEV_MAX_TAUS];
    double adev[M2SDR_ADEV_MAX_TAUS];
    double mdev[M2SDR_ADEV_MAX_TAUS];
    int    terms[M2SDR_ADEV_MAX_TAUS];
};

/* What is known of the carrier. All fields may be 0. */
struct m2sdr_carrier_config {
    double carrier_hz;     /* Nominal RF frequency of the carrier: fractional frequencies are
                            * offsets over it. 0: they are offsets in Hz. */
    double offset_hz;      /* Where the carrier is expected, from the centre of the capture. */
    double search_hz;      /* Half span searched around offset_hz; 0: the whole capture. */
    int    blocks;         /* Sub-blocks of the fine estimate, 8 to M2SDR_ADEV_MAX_BLOCKS;
                            * 0: chosen from the SNR of the carrier. */
};

/* One carrier in one capture: a fine estimate of its frequency and phase.
 *
 * The strongest line of the search span is found on a windowed FFT, brought to zero frequency,
 * and the capture is cut into sub-blocks; the phase of each sub-block is unwrapped and a
 * straight line fitted to them gives the frequency (least squares: the Cramer-Rao bound for a
 * tone in white noise) and the phase at the centre of the capture. The phase of the first and
 * of the last sub-block give the mean frequency over the capture: this one, not the fitted
 * one, is what the Allan deviation over captures wants (the fit weighs the middle of the
 * capture more and reads the white FM 10 % high).
 *
 * The sub-block phases also give the Allan deviation inside the capture, at tau from one
 * sub-block to a third of the capture. Its short taus are bounded by the white phase noise of
 * the additive noise: adev = sqrt(3) sigma_x / tau, sigma_x the phase error of a sub-block in
 * seconds.
 *
 * What is measured is the carrier against the receiver: the LO and the sample clock of the
 * board come from its reference, so a frequency offset is the one of the carrier from the
 * board's reference, and the deviation holds both instabilities. A carrier that moves by more
 * than half a sub-block rate between sub-blocks (strong FM) is not followed. */
struct m2sdr_carrier_result {
    bool   found;              /* A line stands out of the search span. */
    double timestamp;          /* Sample index of the centre of the capture: t0 + (count - 1) / 2. */
    double freq_hz;            /* Fitted frequency, from the centre of the capture. */
    double freq_mean_hz;       /* Mean frequency over the capture, from its end phases. */
    double freq_std_hz;        /* Cramer-Rao bound of freq_hz for the measured SNR. */
    double phase_rad;          /* At the timestamp, -pi to pi. */
    double offset_ppb;         /* (freq_mean_hz - cfg offset_hz) / carrier_hz; 0 without carrier_hz. */
    double level_dbfs;         /* Of the carrier. */
    double snr_db;             /* Carrier over the noise of the whole capture bandwidth. */
    double cn0_dbhz;           /* Carrier over noise density. */
    double phase_rms_rad;      /* Of the sub-block phases around the fitted line. */
    int    blocks;             /* Sub-blocks used, each block_len samples. */
    size_t block_len;
    struct m2sdr_adev short_tau;
};

/* t0 is the sample index of x[0] in the stream, for the timestamp. cfg may be NULL.
 * Returns 0, or -1 on a bad argument (fewer than 1024 samples) or an allocation failure.
 * Without a carrier, found = false. */
int m2sdr_carrier_estimate(const float complex *x, size_t count, double sample_rate, int64_t t0,
                           const struct m2sdr_carrier_config *cfg, struct m2sdr_carrier_result *res);

/* A series of carrier frequencies over captures, the newest `capacity` kept.
 *
 * The captures are taken to be evenly spaced: tau0 is the mean spacing of their timestamps, and
 * `regular` says whether each spacing is within 1 % of it (a lost capture breaks the series:
 * reset it). With dead time between the captures (a capture shorter than tau0), the deviation
 * of white FM reads sqrt(tau0 / capture time) high, that of random walk FM about right. */
struct m2sdr_adev_series {
    double  sample_rate;
    double  carrier_hz;    /* Nominal RF frequency: the frequencies are offsets over it. */
    double  offset_hz;     /* Expected frequency from the centre of the capture. */
    size_t  capacity;
    size_t  count;         /* Points kept. */
    size_t  head;          /* Next point written. */
    double *timestamp;     /* Samples. */
    double *freq_hz;
};

struct m2sdr_adev_result {
    size_t points;
    double tau0_s;
    double span_s;             /* From the first point to the last. */
    bool   regular;
    double offset_ppb;         /* Mean fractional frequency offset. */
    double drift_ppb_s;        /* Slope of a straight line fitted to the fractional frequency. */
    double freq_hz;            /* Mean frequency from the centre of the capture. */
    struct m2sdr_adev dev;
};

/* carrier_hz must be over 0; capacity of 3 or more points. Returns 0, or -1 on a bad argument
 * or an allocation failure. */
int  m2sdr_adev_series_init(struct m2sdr_adev_series *s, double sample_rate, double carrier_hz,
                            double offset_hz, size_t capacity);
void m2sdr_adev_series_free(struct m2sdr_adev_series *s);
void m2sdr_adev_series_reset(struct m2sdr_adev_series *s);
/* A capture: its timestamp (samples) and its mean frequency (freq_mean_hz of the estimate). */
void m2sdr_adev_series_add(struct m2sdr_adev_series *s, double timestamp, double freq_hz);
/* Returns 0, or -1 with fewer than 3 points or an allocation failure. */
int  m2sdr_adev_series_compute(const struct m2sdr_adev_series *s, struct m2sdr_adev_result *res);

/* Allan deviations of a phase series x (seconds of time error), points tau0_s apart. */
void m2sdr_adev_from_phase(const double *x, size_t n, double tau0_s, struct m2sdr_adev *dev);

/* GNSS */
/*------*/

#define M2SDR_GNSS_L1_HZ        1575.42e6
#define M2SDR_GNSS_CA_CHIPS     1023
#define M2SDR_GNSS_CA_RATE      1.023e6
#define M2SDR_GNSS_PRNS         32
#define M2SDR_GNSS_MAX_DOPPLERS 81
#define M2SDR_GNSS_MAX_POINTS   256
#define M2SDR_GNSS_MAX_JAMMERS  8
#define M2SDR_GNSS_SPECTRUM     256    /* Points of the spectrum trace of the jamming indicator. */

/* C/A code of a PRN (1 to 32): 1023 chips, 0 or 1 (ICD-GPS-200, G1 xor the G2 taps of the
 * PRN). Returns 0, or -1 on a bad PRN. */
int m2sdr_gnss_ca_code(int prn, uint8_t chips[M2SDR_GNSS_CA_CHIPS]);

/* What the search covers. All fields may be 0. */
struct m2sdr_gnss_config {
    uint32_t prn_mask;         /* Bit p - 1 for PRN p; 0: all 32. */
    double   doppler_hz;       /* Searched from -doppler_hz to +doppler_hz; 0: 5000. */
    double   doppler_step_hz;  /* 0: 500, the coherent time of 1 ms losing at most 0.9 dB. */
    int      ms;               /* Milliseconds integrated, non-coherently; 0: 10. */
    double   pfa;              /* False alarm probability over the whole search; 0: 1e-3. */
};

struct m2sdr_gnss_prn {
    int    prn;
    bool   detected;
    double metric_db;          /* Peak of the search over its noise floor. */
    /* Valid when detected. */
    double cn0_dbhz;
    double doppler_hz;         /* Carrier frequency from the centre of the capture. */
    double code_phase_chips;   /* Delay of the start of a code epoch from x[0], 0 to 1023 chips. */
    size_t code_phase_samples; /* The same, in whole samples (0 to 1 ms). */
};

/* Interference in the band. GPS is below the thermal noise: the spectrum holds noise and
 * interference only, so a flat spectrum and a steady level are the normal state. A wideband
 * jammer raises the whole floor: only the level over time (power_dbfs at a fixed gain) shows
 * it. The centre bins (within one bin of DC, the offset of the receiver) are left out. */
struct m2sdr_gnss_jamming {
    double power_dbfs;         /* Of the capture. */
    double noise_dbfs_hz;      /* Noise density from the median of the spectrum. */
    double flatness_db;        /* Geometric over arithmetic mean of the spectrum, 0 dB when flat. */
    double jn_db;              /* Power above the noise floor over the noise, in the band (-30 dB floor). */
    int    njammers;           /* Narrowband lines 10 dB or more above the floor, strongest first. */
    double jammer_hz[M2SDR_GNSS_MAX_JAMMERS];
    double jammer_dbfs[M2SDR_GNSS_MAX_JAMMERS];  /* Power of the line. */
    double jammer_jn_db[M2SDR_GNSS_MAX_JAMMERS]; /* Over the noise power of the band. */
    bool   jammed;             /* A narrowband line, or 3 dB or more of J/N. */
    double band_hz;            /* Band looked at: 80 % of the sample rate, centred. */
    int    spectrum_points;
    double spectrum_start_hz, spectrum_step_hz;
    float  spectrum_db[M2SDR_GNSS_SPECTRUM];     /* dB over the noise floor. */
};

/* GPS L1 C/A acquisition, tuned to 1575.42 MHz, at 2.048 or 4.096 MSPS.
 *
 * For each PRN and Doppler bin, the code phase is searched in parallel (FFT correlation of
 * each millisecond at 2.048 MSPS, 4.096 MSPS summed by pairs), and the power of `ms`
 * milliseconds is summed. Integer kHz Doppler steps are rotations of the spectrum of a
 * millisecond, so one spectrum per fractional step is computed. The detection threshold is
 * set from the floor of the search for the false alarm probability asked over all the cells.
 * A detected PRN is then refined at the full rate: Doppler from the turn of its prompt
 * correlation from one millisecond to the next, code phase from early, prompt and late
 * correlations half a chip apart (a triangle), C/N0 from the prompt power over the noise of
 * the search.
 *
 * Sensitivity: a carrier between two Doppler bins loses 0.9 dB, a code half a sample from
 * the search grid 2.5 dB; 10 ms detect about 40 dB-Hz in that worst case, 20 ms 35 dB-Hz.
 * The search of 32 PRNs over +-5 kHz takes about 0.25 s for 10 ms (linear in ms, PRNs and
 * Doppler bins).
 *
 * Not done: tracking, the navigation message, positions, the time. The data bits (20 ms) are
 * not removed: non-coherent sums only, and a bit edge inside a millisecond lowers it. Doppler
 * is the sum of the satellite's and of the receiver's frequency error (1 ppm of the reference
 * is 1575 Hz): a search wider than +-5 kHz is needed for a reference off by more than about
 * 1 ppm. */
struct m2sdr_gnss_result {
    bool   found;                  /* At least one PRN detected. */
    int    detected;
    int    ms;                     /* Integrated. */
    double threshold_db;           /* Detection threshold of metric_db. */
    int    nprn;                   /* Searched, by ascending PRN. */
    struct m2sdr_gnss_prn prn[M2SDR_GNSS_PRNS];
    /* The strongest PRN: its search at its Doppler, dB over the floor, one point per chip,
     * and over the Doppler bins at its code phase. */
    int    best;                   /* Index into prn, -1 without a detection. */
    float  code_db[M2SDR_GNSS_CA_CHIPS];
    int    ndopplers;
    double doppler_start_hz, doppler_step_hz;
    float  doppler_db[M2SDR_GNSS_MAX_DOPPLERS];
    /* Prompt correlations of the strongest PRN, one per millisecond, turned onto the real axis
     * and scaled to a mean power of 1: the data bits, BPSK. */
    int    npoints;
    float complex points[M2SDR_GNSS_MAX_POINTS];
    struct m2sdr_gnss_jamming jamming;
};

/* cfg may be NULL. Returns 0, or -1 on a bad argument (sample rate, fewer samples than `ms`
 * milliseconds, ms over 200) or an allocation failure. */
int m2sdr_gnss_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_gnss_config *cfg,
                       struct m2sdr_gnss_result *res);

/* The jamming indicator alone (part of m2sdr_gnss_analyse): any sample rate, 4096 samples or
 * more. Returns 0, or -1 on a bad argument or an allocation failure. */
int m2sdr_gnss_jamming(const float complex *x, size_t count, double sample_rate, struct m2sdr_gnss_jamming *jam);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_TIMING_H */
