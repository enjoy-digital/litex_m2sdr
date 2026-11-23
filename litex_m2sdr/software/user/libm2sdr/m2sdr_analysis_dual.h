/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: measurements on the two coherent receive channels of the AD9361.
 *
 * RX1 and RX2 share their local oscillator and their sample clock: the gain, the phase and the
 * delay between them are those of the board, of the cables and of the antennas, and stay put
 * while the LO stays tuned. The functions take one block of samples of each channel, captured
 * together and scaled to full scale 1.0, and have no state besides their arguments.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYSIS_DUAL_H
#define M2SDR_ANALYSIS_DUAL_H

#include "m2sdr_analysis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cross spectrum */
/*----------------*/

/* Averaged spectra of two channels and their cross spectrum (Welch, Hann window, segments half
 * overlapped). Bin k is at m2sdr_cross_freq(k), from -sample_rate/2 upwards, scaled as
 * struct m2sdr_psd: a full-scale tone reads 1.0 at its bin. s21 is the mean of X2 conj(X1):
 * its phase is that of RX2 against RX1. Zero the struct before its first use; it can be
 * computed again, and m2sdr_cross_free() releases it. */
struct m2sdr_cross {
    size_t   bins;
    double   sample_rate;
    double   bin_hz;
    unsigned segments;
    float   *s11, *s22;
    float complex *s21;
    /* Private: plan and window kept between calls. */
    void    *fft;
    float   *win;
    float complex *work;
};

/* Returns 0, or -1 on a bad argument (fft_len not a power of two, count < fft_len) or an
 * allocation failure. */
int  m2sdr_cross_compute(const float complex *x1, const float complex *x2, size_t count, double sample_rate,
                         size_t fft_len, struct m2sdr_cross *cs);
void m2sdr_cross_free(struct m2sdr_cross *cs);

static inline double m2sdr_cross_freq(const struct m2sdr_cross *cs, size_t k)
{
    return ((double)k - (double)(cs->bins / 2)) * cs->bin_hz;
}

/* Coherence */
/*-----------*/

#define M2SDR_COHERENCE_MAX_BINS 8192

/* Self-check of the two receivers, of their cables and of their antennas: the same signal on
 * both (a splitter, or two antennas side by side on a strong signal).
 *
 * Per bin: the magnitude-squared coherence |S21|^2 / (S11 S22), the gain of RX2 over RX1 and
 * their phase. Over the band, from the coherent bins only (coherence above min_coherence):
 *   - the delay of RX2 behind RX1, from the slope of the phase over frequency, each bin
 *     weighted by the inverse of the variance of its phase, (1 - C) / (2 K C) for K segments;
 *   - the phase at the centre of the capture once that delay is taken off;
 *   - the gain, sqrt(sum S22 / sum S11): the noise of each receiver raises it a little when
 *     their signal to noise ratios differ;
 *   - the ripple of the gain and of the phase around these figures: what is left over
 *     frequency (filters, cables, a reflection), RMS.
 * The DC bin and its two neighbours hold the LO leakage of the receivers and are left out.
 * One tone gives the gain and the phase but no delay: delay_valid tells whether the coherent
 * bins spread enough for it. A delay of more than a quarter of the FFT length is not
 * measured. The coherence of noise alone is about 1/K: a few segments give a high one. */
struct m2sdr_coherence_config {
    size_t fft_len;             /* Power of two, 16 to M2SDR_COHERENCE_MAX_BINS; 0: 1024. */
    double lo_hz, hi_hz;        /* Band, from the centre of the capture; both 0: all of it. */
    double min_coherence;       /* Of the bins used for the figures; 0: 0.5. */
};

struct m2sdr_coherence_result {
    bool   found;               /* Enough coherent bins: the figures below are valid. */
    int    bins;                /* FFT length; bin k at m2sdr_coherence_freq(). */
    double bin_hz;
    unsigned segments;
    int    band_bins;           /* Bins of the band, */
    int    coherent_bins;       /* and coherent ones among them. */
    double coherence;           /* Mean over the band. */
    double coherence_used;      /* Mean over the coherent bins. */
    double rx1_db, rx2_db;      /* Power of each channel in the band, dBFS. */
    double gain_db;             /* RX2 over RX1. */
    double phase_deg;           /* RX2 against RX1 at the centre of the capture, delay taken off. */
    bool   delay_valid;
    double delay_s;             /* RX2 behind RX1: positive when it receives later. */
    double delay_samples;
    double gain_ripple_db;      /* RMS over the coherent bins. */
    double phase_ripple_deg;
    /* Per bin, the lowest frequency first. */
    float  msc[M2SDR_COHERENCE_MAX_BINS];
    float  gain_bin_db[M2SDR_COHERENCE_MAX_BINS];
    float  phase_bin_deg[M2SDR_COHERENCE_MAX_BINS];     /* As received, -180 to 180. */
    float  residual_bin_deg[M2SDR_COHERENCE_MAX_BINS];  /* Less the phase and the delay. */
    float  rx1_bin_db[M2SDR_COHERENCE_MAX_BINS];        /* dBFS per bin. */
    float  rx2_bin_db[M2SDR_COHERENCE_MAX_BINS];
};

static inline double m2sdr_coherence_freq(const struct m2sdr_coherence_result *res, int k)
{
    return (double)(k - res->bins / 2) * res->bin_hz;
}

/* cfg may be NULL. Returns 0, or -1 on a bad argument (fewer than eight segments of samples)
 * or an allocation failure. Without coherent bins, found = false. */
int m2sdr_coherence_analyse(const float complex *x1, const float complex *x2, size_t count, double sample_rate,
                            const struct m2sdr_coherence_config *cfg, struct m2sdr_coherence_result *res);

/* Direction finding */
/*-------------------*/

#define M2SDR_DF_POINTS         181     /* Angles of the pseudo-spectra: -90 to 90 degrees, 1 degree apart. */
#define M2SDR_DF_MAX_CANDIDATES 16

/* Angle of arrival of the strongest signal (or of the one at freq_hz) on two antennas spaced
 * spacing_m apart: RX1 and RX2.
 *
 * The angle is from broadside, positive towards RX2: a plane wave from there reaches RX2
 * first, and RX2 leads RX1 by phi = 2 pi d sin(angle) / lambda. phi is the phase of the cross
 * spectrum summed over the bins of the signal, less the phase of the receivers and their
 * cables (cal_phase_deg, measured on a source at a known angle: m2sdr_df_calibration()).
 * lambda is that of rf_freq_hz + freq: the bandwidth of the signal is taken as small against
 * its frequency.
 *
 * Spaced more than lambda / 2, the antennas give several angles for one phase: they are all in
 * candidate_deg, and angle_deg is the one nearest broadside. Two antennas on a line do not tell
 * the front from the back either: the angle a and 180 - a give the same phase.
 *
 * The pseudo-spectra over the angle are those of the 2x2 covariance of the signal: beamscan
 * a^H R a, and MUSIC 1 / |e^H a|^2 with e the eigenvector of the smaller eigenvalue. With two
 * antennas, MUSIC finds one source, the angle of the phase above, more sharply drawn. Several
 * signals at the same frequency mix into one angle. */
struct m2sdr_df_config {
    double spacing_m;           /* Between the phase centres of the antennas. */
    double rf_freq_hz;          /* Of the centre of the capture. */
    double freq_hz;             /* Signal, from the centre of the capture; 0: the strongest one. */
    double bandwidth_hz;        /* Bins used around it; 0: the main lobe of its peak. */
    double cal_phase_deg;       /* Phase of RX2 against RX1 of the receivers and cables, taken off. */
    size_t fft_len;             /* Power of two, 64 to 65536; 0: 4096. */
};

struct m2sdr_df_result {
    bool   found;               /* A signal stands above the noise, coherent on both channels. */
    double freq_hz;             /* Used, from the centre of the capture. */
    double level_db;            /* Its power on RX1, dBFS. */
    double snr_db;              /* Over the noise in the same bins. */
    double coherence;           /* Of the bins used, 0 to 1. */
    double gain_db;             /* RX2 over RX1. */
    double phase_raw_deg;       /* RX2 against RX1, as received. */
    double phase_deg;           /* Less cal_phase_deg: the one of the geometry, -180 to 180. */
    double phase_std_deg;       /* Its standard deviation, from the coherence. */
    double wavelength_m;
    double spacing_wl;          /* Spacing over the wavelength. */
    bool   ambiguous;           /* Several candidates. */
    int    candidates;
    double candidate_deg[M2SDR_DF_MAX_CANDIDATES];   /* The lowest first. */
    double angle_deg;           /* The candidate nearest broadside. */
    double angle_std_deg;       /* From phase_std_deg. */
    double music_peak_deg;      /* Highest point of the MUSIC pseudo-spectrum, interpolated. */
    /* dB from their highest point, angle -90 + i degrees. */
    float  beamscan_db[M2SDR_DF_POINTS];
    float  music_db[M2SDR_DF_POINTS];
};

/* Returns 0, or -1 on a bad argument (spacing or frequency not positive, fewer than eight
 * segments of samples) or an allocation failure. Without a signal, found = false. */
int m2sdr_df_analyse(const float complex *x1, const float complex *x2, size_t count, double sample_rate,
                     const struct m2sdr_df_config *cfg, struct m2sdr_df_result *res);

/* cal_phase_deg that turns the result of a source at known_angle_deg into that angle: the
 * measured phase less the one of the geometry, -180 to 180. Broadside (0) needs no spacing. */
double m2sdr_df_calibration(const struct m2sdr_df_result *res, double known_angle_deg);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_DUAL_H */
