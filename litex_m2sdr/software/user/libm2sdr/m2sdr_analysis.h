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

/* Demodulation */
/*--------------*/

enum m2sdr_modulation {
    M2SDR_MOD_AUTO   = 0,     /* Found among the ones below. */
    M2SDR_MOD_BPSK   = 1,
    M2SDR_MOD_QPSK   = 2,
    M2SDR_MOD_8PSK   = 3,
    M2SDR_MOD_16QAM  = 4,
    M2SDR_MOD_32QAM  = 5,
    M2SDR_MOD_64QAM  = 6,
    M2SDR_MOD_128QAM = 7,
    M2SDR_MOD_256QAM = 8,
    M2SDR_MOD_COUNT  = 9,
};

enum m2sdr_pulse {
    M2SDR_PULSE_RRC  = 0,     /* Root raised cosine at the transmitter: matched filter here. */
    M2SDR_PULSE_RC   = 1,     /* Raised cosine at the transmitter: no filter here. */
    M2SDR_PULSE_RECT = 2,     /* Rectangular symbols: integration over a symbol. */
};

#define M2SDR_DEMOD_MAX_SYMBOLS 8192
#define M2SDR_DEMOD_EYE_TRACES  128
#define M2SDR_DEMOD_EYE_POINTS  17      /* Two symbols, eight points per symbol. */

struct m2sdr_demod_config {
    enum m2sdr_modulation modulation;
    enum m2sdr_pulse pulse;
    double symbol_rate;       /* Symbols per second; 0: found from the signal. */
    double rolloff;           /* Of the pulse, 0.05 to 1; 0: found from the signal. */
    size_t max_symbols;       /* Symbols analysed; 0: M2SDR_DEMOD_MAX_SYMBOLS. */
};

/* Single-carrier PSK or QAM signal: the strongest signal of the capture.
 *
 * The symbols are compared with the nearest points of the constellation, not with the symbols
 * sent: the EVM is right while symbol errors are rare, and reads low once the points merge.
 * `locked` tells whether the EVM is small enough against the spacing of the constellation for
 * the figures to mean something. The carrier phase is followed by a loop a hundredth of the
 * symbol rate wide: slower phase noise of the source or of the receiver is not in the EVM.
 * The rotation of the constellation is not resolved: a multiple of its symmetry remains. */
struct m2sdr_demod_result {
    bool   signal;            /* A signal and its symbol rate were found. */
    bool   locked;            /* Carrier and symbols recovered: the figures are valid. */
    enum m2sdr_modulation modulation;
    double symbol_rate;       /* Used, Hz. */
    double rolloff;           /* Used. */
    double freq_offset_hz;    /* Carrier, from the centre of the capture. */
    double power_db;          /* Of the signal in its band, dBFS. */
    size_t symbols;
    double evm_rms;           /* Fractions of the RMS of the constellation: 0.02 is 2 %. */
    double evm_peak;
    double mer_db;
    double mag_err_rms;       /* Fraction. */
    double phase_err_deg;     /* RMS. */
    double iq_offset_db;      /* Origin offset over the RMS of the constellation. */
    double gain_imbalance_db; /* I over Q. */
    double quadrature_deg;
    /* Received symbols and the points they were compared with, the constellation scaled to a
     * mean power of 1. */
    float complex sym[M2SDR_DEMOD_MAX_SYMBOLS];
    float complex ref[M2SDR_DEMOD_MAX_SYMBOLS];
    /* Eye diagram: traces of two symbols after the receive filter, the symbol instants at
     * points 4 and 12. */
    int    eye_traces;
    float complex eye[M2SDR_DEMOD_EYE_TRACES][M2SDR_DEMOD_EYE_POINTS];
};

const char *m2sdr_modulation_name(enum m2sdr_modulation modulation);

/* Points of a constellation, scaled to a mean power of 1; returns their number (0 for AUTO).
 * points holds 256 entries. */
int m2sdr_constellation(enum m2sdr_modulation modulation, float complex *points);

/* Returns 0, or -1 on a bad argument or allocation failure. Without a signal, signal = false;
 * with one that does not demodulate, locked = false and the figures are what was read. */
int m2sdr_demod(const float complex *x, size_t count, double sample_rate,
                const struct m2sdr_demod_config *cfg, struct m2sdr_demod_result *res);

/* LTE downlink */
/*--------------*/

#define M2SDR_LTE_FRAME_SYMBOLS 140     /* Of a frame, normal cyclic prefix. */
#define M2SDR_LTE_FRAME_SLOTS   20
#define M2SDR_LTE_MAX_ANALYSED_RB 100
#define M2SDR_LTE_MAX_SUBCARRIERS (12 * M2SDR_LTE_MAX_ANALYSED_RB)
#define M2SDR_LTE_MAX_POINTS    4096

/* One frame of an LTE downlink carrier, FDD with a normal cyclic prefix, tuned on its centre.
 *
 * The sample rate is 1.92 MSPS times 1, 2, 4, 8 or 16, and sets how much of the carrier is
 * analysed: 6, 15, 25, 50 or 100 resource blocks around its centre, the whole carrier when it
 * is not wider.
 *
 * The control channels are not decoded, so the allocation and the modulation of the data are
 * not known: a block of data is taken as the constellation it fits best, and its EVM is
 * against the nearest points. This is a diagnostic, not the EVM of a conformance test.
 *
 * With two antenna ports, a block is tried as transmit diversity and as one layer with each of
 * the precoders, and is read when it fits one of them. A block of two layers fits none: one
 * receiver does not separate them, and it is counted as in use and not read. With four
 * antenna ports, the blocks of data are not analysed. */
struct m2sdr_lte_result {
    bool   found;               /* PSS and SSS found, and the MIB or the reference signals of the cell. */
    int    cell_id;             /* Physical cell identity, 0 to 503. */
    int    n_id_1, n_id_2;
    double freq_error_hz;       /* Carrier, from the centre of the capture. */
    double sync_quality;        /* PSS peak over the mean of its correlation; given without a cell too. */
    size_t frame_offset;        /* Sample of the start of the frame analysed. */
    bool   mib;                 /* PBCH decoded, its CRC right: the four fields below are valid. */
    int    ports;               /* Antenna ports of the cell: 1, 2 or 4. */
    int    n_rb;                /* Resource blocks of the carrier. */
    int    sfn;                 /* System frame number of the frame analysed. */
    int    phich_duration;      /* 0: normal, 1: extended. */
    int    phich_resource;      /* 0 to 3: one sixth, a half, one, two. */
    int    analysed_rb;
    int    subcarriers;         /* 12 * analysed_rb. */
    double rsrp_db;             /* Mean power of a reference signal element of port 0, dBFS. */
    double pss_db, sss_db;      /* Mean power of their elements, dBFS. */
    double rssi_db;             /* Power of the band analysed, dBFS. */
    double rs_evm;              /* Reference signals of port 0 against the channel around them. */
    double pbch_evm;            /* PBCH symbols against QPSK. */
    bool   data;                /* Data blocks analysed (one or two antenna ports). */
    double data_evm;            /* Over the blocks in use. */
    int    data_blocks;         /* Resource blocks in use and read in the frame, of 20 slots each. */
    /* Two antenna ports: blocks read as transmit diversity, as one precoded layer, and blocks in
     * use that fit neither (two layers, mostly). */
    int    data_diversity, data_precoded, data_unknown;
    enum m2sdr_modulation data_modulation;   /* The most used. */
    /* The blocks below are the ones of the cell that are whole in the band analysed: data_rb of
     * them, the first one block data_first_rb of the cell, data_offset subcarriers (0 or 6)
     * above the lowest one of grid_db. */
    int    data_rb, data_first_rb, data_offset;
    /* Per slot and resource block: modulation (M2SDR_MOD_AUTO: empty or not analysed) and EVM. */
    uint8_t rb_modulation[M2SDR_LTE_FRAME_SLOTS][M2SDR_LTE_MAX_ANALYSED_RB];
    float   rb_evm[M2SDR_LTE_FRAME_SLOTS][M2SDR_LTE_MAX_ANALYSED_RB];
    /* Per slot and resource block: 1 when the block is in use and not read (two antenna ports). */
    uint8_t rb_unknown[M2SDR_LTE_FRAME_SLOTS][M2SDR_LTE_MAX_ANALYSED_RB];
    /* Power of each resource element, dBFS, the lowest subcarrier first. */
    float   grid_db[M2SDR_LTE_FRAME_SYMBOLS][M2SDR_LTE_MAX_SUBCARRIERS];
    /* Equalised symbols: data in use, reference signals and PBCH, the constellations scaled to
     * a mean power of 1. */
    int    ndata, nrs, npbch;
    float complex data_points[M2SDR_LTE_MAX_POINTS];
    uint8_t data_points_modulation[M2SDR_LTE_MAX_POINTS];   /* Of the block of each point. */
    float complex rs_points[M2SDR_LTE_MAX_POINTS];
    float complex pbch_points[240];
};

/* Returns 0, or -1 on a bad argument (sample rate, less than 25 ms of samples) or an
 * allocation failure. Without a cell, found = false. */
int m2sdr_lte_analyse(const float complex *x, size_t count, double sample_rate, struct m2sdr_lte_result *res);

/* DVB-T */
/*-------*/

#define M2SDR_DVBT_BINS        512     /* Groups of carriers of the per-carrier traces. */
#define M2SDR_DVBT_ECHO_BINS   512     /* Points of the echo profile. */
#define M2SDR_DVBT_MAX_POINTS  4096

/* A DVB-T multiplex (EN 300 744), 2k or 8k, tuned near its centre: a reception check, as a
 * TV installer's meter does, not a decoder. The error correction and the transport stream
 * are not looked at.
 *
 * The sample rate is the elementary rate of the channel, 8/7 of its bandwidth: 64/7 MSPS
 * (9.142857 MSPS) for 8 MHz, 8 MSPS for 7 MHz, 48/7 MSPS for 6 MHz. The multiplex may be off
 * the centre of the capture by up to about half the gap between its band and the sample
 * rate, enough for the 1/6 MHz offsets of some networks.
 *
 * The mode and the guard interval come from the correlation of the guard intervals, the
 * integer carrier offset from the continual pilots, the channel from the scattered pilots
 * of four symbols, and the transmission parameters from the TPS. The MER is that of the
 * equalised data carriers against the constellation of the TPS, or the one they fit best
 * without a TPS. */
struct m2sdr_dvbt_result {
    bool   found;               /* Guard intervals found: an OFDM signal of DVB-T. */
    int    fft_len;             /* 2048 (2k) or 8192 (8k). */
    int    carriers;            /* 1705 or 6817. */
    int    guard;               /* Guard interval: 1/guard of the useful part, 4, 8, 16 or 32. */
    double guard_us;
    double sync_quality;        /* Correlation of the guard intervals, 0 to 1; given without a signal too. */
    double sync_snr_db;         /* The signal to noise ratio it gives: a rough figure. */
    double power_dbfs;          /* Of the capture. */
    bool   pilots;              /* Continual pilots found: the figures below are valid. */
    double pilot_quality;       /* Their coherence from symbol to symbol, 0 to 1. */
    double freq_error_hz;       /* Centre of the multiplex, from the centre of the capture. */
    int    carrier_offset;      /* Its whole carriers. */
    int    symbols;             /* Analysed. */
    bool   tps;                 /* TPS sync word found: the parameters below are read. */
    int    tps_fft_len, tps_guard;
    enum m2sdr_modulation tps_modulation;   /* QPSK, 16QAM or 64QAM. */
    int    tps_hierarchy;       /* 0: none, 1, 2 or 4: alpha. */
    char   code_rate_hp[4], code_rate_lp[4];  /* "1/2" to "7/8". */
    int    cell_id;             /* -1 when the TPS does not carry it or it was not read. */
    enum m2sdr_modulation modulation;       /* The MER is against it. */
    double mer_db;
    /* Per group of carriers, the lowest first: MER, and power of the channel in dB from its
     * mean. bins groups of carriers_per_bin carriers each. */
    int    bins;
    double carriers_per_bin;
    float  mer_bin_db[M2SDR_DVBT_BINS];
    float  channel_bin_db[M2SDR_DVBT_BINS];
    /* Echo profile: impulse response of the channel in dB from its strongest path, point i at
     * echo_start_us + i * echo_step_us from that path. */
    float  echo_db[M2SDR_DVBT_ECHO_BINS];
    double echo_start_us, echo_step_us;
    double echo_delay_us;       /* The strongest other path: its delay and its level. */
    double echo_level_db;
    /* Equalised data carriers, mean power 1. */
    int    npoints;
    float complex points[M2SDR_DVBT_MAX_POINTS];
};

/* What is already known of a multiplex, from an earlier capture with its TPS: shorter
 * captures then give the MER, the channel and the echoes. A field at 0 is found. */
struct m2sdr_dvbt_config {
    int    fft_len;             /* 2048 or 8192, with guard. */
    int    guard;
    enum m2sdr_modulation modulation;   /* Of the MER when the capture has no TPS. */
    bool   offset_known;        /* carrier_offset of an earlier result: searched around it only. */
    int    carrier_offset;
};

/* Returns 0, or -1 on a bad argument (sample rate, less than the time of eight 8k symbols)
 * or an allocation failure. Without a multiplex, found = false. 150 ms of samples carry the
 * TPS of a frame wherever it starts; with cfg (may be NULL), 40 ms give the rest. */
int m2sdr_dvbt_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_dvbt_config *cfg,
                       struct m2sdr_dvbt_result *res);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_H */
