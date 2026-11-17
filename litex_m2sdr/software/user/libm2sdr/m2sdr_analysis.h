/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: measurements on a block of complex samples.
 *
 * The functions take samples scaled to full scale 1.0 (m2sdr_cal_iq_to_complex()) and have no
 * state besides their arguments: no device, no display.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYSIS_H
#define M2SDR_ANALYSIS_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spectrum */
/*----------*/

enum m2sdr_window {
    M2SDR_WINDOW_HANN            = 0,   /* General use. */
    M2SDR_WINDOW_FLATTOP         = 1,   /* Level of a tone within 0.01 dB, wide. */
    M2SDR_WINDOW_BLACKMAN_HARRIS = 2,   /* Sidelobes 92 dB down: weak signals next to strong ones. */
    M2SDR_WINDOW_RECTANGULAR     = 3,
};

/* Averaged power spectrum (Welch, segments half overlapped).
 *
 * power[k] is linear, for the frequency m2sdr_psd_freq(k): from -sample_rate/2 upwards, relative
 * to the centre of the capture. A tone of full-scale amplitude reads 1.0 (0 dBFS) at its bin.
 * The power of a band is the sum of its bins over enbw_bins, and a noise density is a bin over
 * rbw_hz. Zero the struct before its first use; m2sdr_psd_compute() can be called again on it,
 * and m2sdr_psd_free() releases it. */
struct m2sdr_psd {
    size_t   bins;          /* FFT length. */
    double   sample_rate;   /* Hz. */
    double   bin_hz;        /* Spacing of the bins. */
    double   enbw_bins;     /* Equivalent noise bandwidth of the window, in bins. */
    double   rbw_hz;        /* Resolution bandwidth: enbw_bins * bin_hz. */
    unsigned segments;      /* Segments averaged. */
    float   *power;
    /* Private: plan and window kept between calls. */
    void    *fft;
    float   *win;
    float complex *work;
    int      win_kind;
};

/* Returns 0, or -1 on a bad argument (fft_len not a power of two, count < fft_len) or an
 * allocation failure. */
int  m2sdr_psd_compute(const float complex *x, size_t count, double sample_rate, size_t fft_len,
                       enum m2sdr_window window, struct m2sdr_psd *psd);
void m2sdr_psd_free(struct m2sdr_psd *psd);

static inline double m2sdr_psd_freq(const struct m2sdr_psd *psd, size_t k)
{
    return ((double)k - (double)(psd->bins / 2)) * psd->bin_hz;
}

/* Linear power to dB, with a floor of -200 dB. */
double m2sdr_power_db(double power);

struct m2sdr_psd_peak {
    double freq_hz;         /* Interpolated between the bins. */
    double power_db;        /* dBFS. */
};

/* Highest bin between lo_hz and hi_hz. Returns 0, or -1 on an empty range. */
int m2sdr_psd_peak(const struct m2sdr_psd *psd, double lo_hz, double hi_hz, struct m2sdr_psd_peak *peak);

/* Peaks that stand excursion_db above the floor on both of their sides, strongest first.
 * Returns their number, max at most. */
int m2sdr_psd_peaks(const struct m2sdr_psd *psd, double lo_hz, double hi_hz, double excursion_db,
                    struct m2sdr_psd_peak *peaks, int max);

/* Power of the band lo_hz..hi_hz, dBFS. */
double m2sdr_psd_band_power_db(const struct m2sdr_psd *psd, double lo_hz, double hi_hz);

/* Noise density of the band, dBFS/Hz, from the median of its bins: signals narrower than half
 * of the band do not raise it. */
double m2sdr_psd_noise_density_db(const struct m2sdr_psd *psd, double lo_hz, double hi_hz);

/* Occupied bandwidth: the band that holds `fraction` (0.99) of the power found between lo_hz
 * and hi_hz, the rest split evenly below and above. Returns 0, or -1 on a bad argument. */
int m2sdr_psd_obw(const struct m2sdr_psd *psd, double lo_hz, double hi_hz, double fraction,
                  double *obw_lo_hz, double *obw_hi_hz);

#define M2SDR_ACPR_MAX_ADJACENT 2

struct m2sdr_acpr {
    double channel_db;                          /* Power of the channel, dBFS. */
    int    adjacent;                            /* Adjacent channels that fit in the span. */
    double lower_dbc[M2SDR_ACPR_MAX_ADJACENT];  /* Channels below, the nearest first. */
    double upper_dbc[M2SDR_ACPR_MAX_ADJACENT];
};

/* Adjacent channel power: channels bw_hz wide, spacing_hz apart, the main one at center_hz. */
int m2sdr_psd_acpr(const struct m2sdr_psd *psd, double center_hz, double bw_hz, double spacing_hz,
                   struct m2sdr_acpr *acpr);

#define M2SDR_TONE_HARMONICS 5   /* Second to sixth. */

/* Single tone: the strongest line is the fundamental. The harmonics are taken where they fold
 * in the capture. The DC line is left out of every figure. The skirts of the window count as
 * noise and spurs: a Blackman-Harris spectrum is the one to give for a tone of more than
 * 40 dB of SNR. */
struct m2sdr_tone {
    double freq_hz;
    double level_db;                            /* dBFS. */
    double snr_db;                              /* Fundamental over noise, harmonics left out. */
    double sinad_db;                            /* Fundamental over everything else. */
    double thd_dbc;                             /* Harmonics over fundamental. */
    double sfdr_dbc;                            /* Fundamental over the strongest other line. */
    double spur_freq_hz;                        /* That line. */
    double noise_db_hz;                         /* dBFS/Hz. */
    double harmonic_dbc[M2SDR_TONE_HARMONICS];
    double harmonic_hz[M2SDR_TONE_HARMONICS];
};

int m2sdr_psd_tone(const struct m2sdr_psd *psd, struct m2sdr_tone *tone);

/* Time domain */
/*-------------*/

#define M2SDR_CCDF_POINTS  64
#define M2SDR_CCDF_STEP_DB 0.25

struct m2sdr_iq_stats {
    double mean_db;             /* Mean power, dBFS. */
    double peak_db;             /* Peak power, dBFS. */
    double papr_db;
    double dc_i, dc_q;          /* Mean of each rail, full scale 1.0. */
    double dc_db;               /* Power of the mean, dBFS. */
    double gain_imbalance_db;   /* I over Q. */
    double quadrature_deg;      /* Error from 90 degrees between the rails. */
    double clipped;             /* Share of the samples at the rails, for full scale `clip`. */
    /* Share of the samples whose power is k * M2SDR_CCDF_STEP_DB or more above the mean. */
    double ccdf[M2SDR_CCDF_POINTS];
};

/* `clip` is the level counted as clipped (0.999 for samples scaled by their converter range). */
int m2sdr_iq_stats(const float complex *x, size_t count, double clip, struct m2sdr_iq_stats *stats);

/* Phase noise */
/*-------------*/

#define M2SDR_PN_POINTS_PER_DECADE 20
#define M2SDR_PN_MAX_POINTS        160
#define M2SDR_PN_MAX_SPURS         16

/* Phase noise of the strongest carrier of a capture.
 *
 * The carrier is brought to zero frequency, its frequency offset and its mean phase are
 * removed, and the signal is split into its phase and its amplitude. L(f) is the spectral
 * density of the phase, single sideband, in dBc/Hz; the density of the relative amplitude is
 * given next to it. Offsets are spaced evenly on a log scale, with a longer transform for the
 * lower ones.
 *
 * The centre of the capture is not searched: it holds the DC offset and the LO leakage of the
 * receiver, so the capture is tuned a little off the carrier.
 *
 * What is measured is the sum of the source and of the receiver: its local oscillator and its
 * reference add their own phase noise, and its noise figure and its converters add a floor,
 * half of it as phase and half as amplitude. */
struct m2sdr_phase_noise {
    bool   found;                                 /* A carrier stands out of the capture. */
    double carrier_hz;                            /* From the centre of the capture. */
    double carrier_db;                            /* dBFS. */
    int    points;
    double offset_hz[M2SDR_PN_MAX_POINTS];
    double l_dbc_hz[M2SDR_PN_MAX_POINTS];         /* Phase noise. */
    double am_dbc_hz[M2SDR_PN_MAX_POINTS];        /* Amplitude noise. */
    /* Integrated between lo_hz and hi_hz, both sidebands. */
    double lo_hz, hi_hz;
    double rms_rad;                               /* Phase error. */
    double rms_deg;
    double jitter_s;                              /* 0 when the carrier frequency is not given. */
    double residual_fm_hz;                        /* RMS frequency deviation. */
    /* Discrete lines of the phase, the strongest first; they are part of L(f) above. */
    int    nspurs;
    double spur_hz[M2SDR_PN_MAX_SPURS];
    double spur_dbc[M2SDR_PN_MAX_SPURS];
};

/* carrier_freq_hz is the RF frequency of the carrier, for the jitter (0: not computed).
 * The integration goes from lo_hz to hi_hz, clamped to the offsets measured (0, 0: all of
 * them). Returns 0, or -1 on a bad argument or allocation failure; without a carrier,
 * found = false. A capture of 2^20 samples or more gives five decades of offsets. */
int m2sdr_phase_noise(const float complex *x, size_t count, double sample_rate, double carrier_freq_hz,
                      double lo_hz, double hi_hz, struct m2sdr_phase_noise *pn);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_H */
