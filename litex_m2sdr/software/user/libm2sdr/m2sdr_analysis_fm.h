/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: FM broadcast (ITU-R BS.450, BS.412), stereo multiplex and RDS
 * (IEC 62106 / EN 50067).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYSIS_FM_H
#define M2SDR_ANALYSIS_FM_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "m2sdr_analysis.h"
#include "m2sdr_fm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FM broadcast */
/*--------------*/

#define M2SDR_FM_AN_MIN_RATE     240000.0   /* Lowest I/Q rate: the channel is +-120 kHz. */
#define M2SDR_FM_AN_MIN_SECONDS  0.05       /* Shortest capture. */
#define M2SDR_FM_BS412_REF_HZ    19000.0    /* 0 dBr: the power of a sine of +-19 kHz deviation. */
#define M2SDR_FM_MPX_BINS        400        /* Spectrum of the multiplex, 0 to 100 kHz. */
#define M2SDR_FM_MPX_BIN_HZ      250.0
#define M2SDR_FM_DEV_BINS        300        /* Histogram of the deviation, 0 to 150 kHz. */
#define M2SDR_FM_DEV_BIN_HZ      500.0
#define M2SDR_FM_MAX_POINTS      1024       /* RDS bit decisions. */

/* One FM station near the centre of the capture (within +-25 kHz, or around
 * cfg->offset_hz), any I/Q rate from 240 kSPS: 0.96 or 1.92 MSPS on the AD9361. A channel
 * filter (+-100 kHz flat, 60 dB down from +-140 kHz) selects the station and decimates to
 * mpx_rate (240 to 480 kHz), where the discriminator gives the multiplex in Hz of deviation.
 * The response of the discriminator (it averages the frequency over a sample) is compensated:
 * flat within 0.15 dB to 60 kHz at 240 kHz, and the levels of the pilot, the RDS and the test
 * tone are corrected for what remains.
 *
 * Measured, on this capture alone:
 * - carrier: offset (mean frequency, Hann weighted; the error of the receiver's clock is in
 *   it), level, and carrier to noise ratio in the channel filter from the second and fourth
 *   moments of the samples (the noise is what is not of constant envelope: multipath and
 *   adjacent channels count as noise; a clean signal of 75 kHz deviation reads 60 dB);
 * - deviation of the multiplex low-passed at 60 kHz (the receiver noise above is left out):
 *   peak, the levels exceeded by 1 % and 0.1 % of the samples (the kind of percentile of
 *   ITU-R SM.1268, whose exact measurement time and percentile are not reproduced here), and
 *   their histogram;
 * - multiplex power (ITU-R BS.412): mean square deviation over the capture, in dB from the
 *   power of a sine of +-19 kHz deviation. The standard integrates over 60 s: the caller
 *   averages mpx_mean_square_hz2 (linear) over captures that cover 60 s, then takes
 *   10 log10(mean / (19000^2 / 2));
 * - pilot (19 kHz): deviation (amplitude in Hz), frequency error from the phase of 10 ms
 *   pieces (against the sample clock: 1 ppm of it is 0.019 Hz), and its coherence;
 * - stereo: pilot present; RMS deviation of L+R (30 Hz to 15 kHz) and of L-R, from its
 *   sidebands at 23 to 53 kHz scaled to the demodulated L-R. Stereo separation needs a known
 *   test signal: with cfg->tone_hz, the tone is measured in L+R and in L-R (demodulated with
 *   the pilot phase) and separation_db compares the two channels. On programme content
 *   separation cannot be measured;
 * - RDS: decoded by the decoder of m2sdr_fm.h (PI, PTY, TP/TA, PS, RadioText, clock time),
 *   blocks, block errors and groups by type, and its level: twice the RMS of the 57 kHz band,
 *   i.e. the peak deviation of a stream of identical bits of the same power (the level
 *   convention of m2sdr_fm_tx). PS and RT take 0.35 s and up to 1.4 s of groups: pass a
 *   persistent decoder in cfg->rds to build them over several captures;
 * - audio signal to noise ratio (mono, de-emphasis cfg->tau, 15 kHz, unweighted, from a sine of
 *   +-75 kHz deviation): the noise is measured between the audio and the pilot (16 to
 *   18.5 kHz), where the programme leaves nothing, and extrapolated with the f^2 spectrum
 *   of the noise of an FM receiver. Noise of the transmitter with another spectrum is not
 *   extrapolated right: the figure is the one the received carrier allows, and it means
 *   nothing below the FM threshold (CNR under 12 dB, clicks).
 * RDS needs about 16 dB of CNR (C/N0 70 dB-Hz) for no block errors; the decoder takes 2 to 3
 * blocks to sync at the start of each capture.
 * Not measured: audio content (levels in dBu, loudness), harmonics, AM noise of the
 * transmitter, deviation of SCA subcarriers, RDS phase against the pilot. */
struct m2sdr_fm_analysis_config {
    double offset_hz;           /* Station frequency from the centre of the capture. */
    double tau;                 /* De-emphasis of the audio SNR, s: 50e-6 (Europe) or 75e-6. */
    double tone_hz;             /* Known test tone of the stereo separation; 0: none. */
    struct m2sdr_fm_rds_rx *rds;/* Decoder kept between captures (rate: res->mpx_rate of an
                                 * earlier result, or m2sdr_fm_analysis_mpx_rate()); NULL:
                                 * one for this capture only. */
};

struct m2sdr_fm_result {
    bool   found;               /* A carrier: CNR of 6 dB or more in the channel filter. */
    double mpx_rate;            /* Rate of the multiplex, Hz. */
    double duration_s;          /* Of the multiplex analysed. */
    double power_dbfs;          /* Of the capture. */

    /* Carrier. */
    double carrier_offset_hz;   /* From the centre of the capture. */
    double carrier_dbfs;
    double cnr_db;              /* In noise_bw_hz. */
    double cn0_dbhz;
    double noise_bw_hz;         /* Noise bandwidth of the channel filter. */

    /* Deviation of the multiplex, Hz. */
    double peak_dev_hz;
    double dev_p99_hz;          /* Exceeded by 1 % of the samples. */
    double dev_p999_hz;         /* By 0.1 %. */
    float  dev_hist[M2SDR_FM_DEV_BINS];  /* Share of the samples per bin of |deviation|. */

    /* Multiplex power, ITU-R BS.412. */
    double mpx_mean_square_hz2;
    double mpx_power_dbr;

    /* Pilot. */
    bool   pilot;               /* 1 kHz of deviation or more, coherence 0.9 or more. */
    double pilot_dev_hz;
    double pilot_freq_hz;       /* Measured frequency... */
    double pilot_freq_error_hz; /* ...and its error from 19 kHz. */
    double pilot_coherence;     /* 1 for a steady tone. */

    /* Stereo. */
    bool   stereo;              /* The pilot. */
    double mono_rms_hz;         /* L+R, RMS deviation. */
    double stereo_rms_hz;       /* L-R, as demodulated. */
    double stereo_ratio_db;     /* L-R over L+R. */
    /* Test tone, with cfg->tone_hz: amplitudes (Hz of deviation) of the tone in L+R, L-R, and
     * in the left and right channels ((L+R) +- (L-R)), and the separation: the stronger
     * channel over the weaker. Valid with the pilot. */
    bool   tone;
    double tone_mono_hz, tone_stereo_hz;
    double tone_left_hz, tone_right_hz;
    double separation_db;

    /* RDS. */
    bool   rds;                 /* Groups decoded in this capture. */
    double rds_dev_hz;          /* Level, see above. */
    uint64_t rds_blocks;        /* This capture: blocks received in sync... */
    uint64_t rds_block_errors;  /* ...those with a wrong checkword... */
    uint64_t rds_groups;        /* ...and good groups. */
    uint64_t rds_type_groups[32];   /* Good groups by type, as in struct m2sdr_fm_rds_info. */
    double rds_bler;            /* Block error rate of this capture. */
    struct m2sdr_fm_rds_info rds_info;  /* What the decoder holds (over the captures it saw). */
    /* Bit decisions while in sync, data on the real axis at +-1 (mean of |re|), and their
     * MER. */
    double rds_mer_db;
    int    npoints;
    float complex points[M2SDR_FM_MAX_POINTS];

    /* Audio. */
    double audio_snr_db;
    double guard_noise_dbr_hz;  /* Noise density at 17.25 kHz, dB from the BS.412 reference per Hz. */

    /* Spectrum of the multiplex: bin i at i * M2SDR_FM_MPX_BIN_HZ, peak of the spectrum
     * (resolution mpx_rbw_hz) over the bin, in dB from the BS.412 reference: a tone reads
     * its power, the pilot at 7.5 kHz -8.1 dB. */
    float  mpx_db[M2SDR_FM_MPX_BINS];
    double mpx_rbw_hz;
};

/* Rate of the multiplex for an I/Q rate: for the persistent RDS decoder. */
double m2sdr_fm_analysis_mpx_rate(double sample_rate);

/* Returns 0, or -1 on a bad argument (sample rate below M2SDR_FM_AN_MIN_RATE, less than
 * M2SDR_FM_AN_MIN_SECONDS of samples, the channel of offset_hz out of the capture, tone_hz
 * outside the audio band, tau out of 0..1 ms) or an allocation failure. A cfg->rds decoder
 * made for another rate is not detected: it decodes nothing. Without a carrier, found =
 * false and only power_dbfs and the carrier figures are set. cfg may be NULL (tau 50 us). */
int m2sdr_fm_analyse(const float complex *x, size_t count, double sample_rate,
                     const struct m2sdr_fm_analysis_config *cfg, struct m2sdr_fm_result *res);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_FM_H */
