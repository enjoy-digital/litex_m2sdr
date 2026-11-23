/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: cellular and professional radio downlinks.
 *
 * - 5G NR: synchronisation signal blocks (3GPP TS 38.211, 38.213, 38.215).
 * - GSM: frequency correction and synchronisation bursts (3GPP TS 45.002, 45.003, 45.004).
 *
 * As in m2sdr_analysis.h, the functions take a block of samples scaled to full scale 1.0 and
 * keep no state besides their arguments.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYSIS_CELLULAR_H
#define M2SDR_ANALYSIS_CELLULAR_H

#include "m2sdr_analysis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 5G NR synchronisation signal blocks */
/*-------------------------------------*/

#define M2SDR_NR_MAX_SSB        32
#define M2SDR_NR_SSB_SYMBOLS    4
#define M2SDR_NR_SSB_CARRIERS   240
#define M2SDR_NR_MAX_POINTS     2048

/* What is known of the cell. A field at 0 is found or defaulted. */
struct m2sdr_nr_config {
    double scs_hz;              /* Subcarrier spacing of the SSB, 15000 or 30000; 0: 30 kHz, then 15 kHz
                                 * when it finds none (twice the time). */
    double search_hz;           /* SSREF searched within +-search_hz of the centre of the capture; 0:
                                 * wherever an SSB fits in the capture. */
    int    lmax;                /* SSBs of a burst, 4, 8 or 64: how the index reads. 0: 8. */
};

/* One SSB found in the capture. */
struct m2sdr_nr_ssb {
    int    pci;                 /* Physical cell identity, 3 n_id_1 + n_id_2, 0 to 1007. */
    int    n_id_1, n_id_2;
    int    ssb_index;           /* From the PBCH DM-RS: its 3 low bits (L_max 8 or 64), or 0 to 3 (L_max 4). */
    int    half_frame;          /* L_max 4: the half frame bit of the DM-RS; -1 otherwise. */
    size_t start;               /* Sample of the start of the SSB: the cyclic prefix of its PSS symbol. */
    double time_s;              /* The same, in seconds from the start of the capture. */
    double freq_offset_hz;      /* SSREF (subcarrier 120 of the SSB) from the centre of the capture. */
    double pss_metric;          /* Detection: PSS correlation over its noise, 0 for noise up to 127. */
    double sss_match;           /* Of the SSS of the cell, 0 to 1. */
    double dmrs_match;          /* Of the DM-RS of the index, 0 to 1. */
    double rsrp_db;             /* SS-RSRP: mean power of an SSS element, noise taken off, dBFS. */
    double pss_db;              /* Mean power of a PSS element, dBFS. */
    double sinr_db;             /* SS-SINR on the SSS elements. */
    double dmrs_evm;            /* PBCH DM-RS against the channel around them. */
    double pbch_evm;            /* PBCH data, equalised with the DM-RS, against QPSK. */
};

/* The SSBs of a capture, wherever they are in time and frequency (no GSCN raster).
 *
 * The sample rate is the subcarrier spacing times a power of two from 256: 7.68 MSPS for
 * 15 kHz and 15.36 MSPS for 30 kHz are the usual ones. The SSB normally repeats every 20 ms:
 * a capture of 21 ms holds a burst of them wherever it starts.
 *
 * PSS: the three sequences slid over the spectra of windows a quarter of a symbol apart, from
 * the products of neighbouring subcarriers (a timing error within the window does not matter
 * to them). Each candidate is then timed on the samples against the PSS, its carrier offset
 * read from the two halves of the PSS and of the SSS, its cell group from the SSS, and its
 * index from the 8 DM-RS sequences of the PBCH. Each symbol gets its channel from its own
 * reference signals: the phase that the upconversion of TS 38.211 5.4 gives each symbol does
 * not matter.
 *
 * The search loses up to 4 dB when the SSB is half a subcarrier off the bins of the capture,
 * and takes about 60 ms for 21 ms at 7.68 MSPS (110 ms at 15.36 MSPS) for one spacing.
 *
 * Not measured: the MIB is not decoded (polar code), so the system frame number, the
 * numerology of the carrier and the position of the SSB on it are not known. Two cells with
 * the same N_ID_2 at the same time and frequency read as the stronger one. */
struct m2sdr_nr_result {
    bool   found;               /* An SSB with its SSS and its DM-RS. */
    double scs_hz;              /* The spacing of the SSBs found (the last one tried without). */
    int    fft_len;
    double power_dbfs;          /* Of the capture. */
    double freq_error_hz;       /* SSREF of the strongest cell from the centre: the mean over its SSBs. */
    int    nssb;
    struct m2sdr_nr_ssb ssb[M2SDR_NR_MAX_SSB];   /* In time order. */
    int    best;                /* The strongest; -1 without one. */
    int    cells;               /* Distinct PCIs. */
    bool   mib;                 /* Always false: the PBCH payload is not decoded. */
    /* Power of each element of the strongest SSB, dBFS: symbol, then subcarrier from the lowest. */
    float  grid_db[M2SDR_NR_SSB_SYMBOLS][M2SDR_NR_SSB_CARRIERS];
    /* Equalised PBCH data of the SSBs (QPSK, mean power 1) and their DM-RS against the channel
     * around them. */
    int    npoints, ndmrs;
    float complex points[M2SDR_NR_MAX_POINTS];
    float complex dmrs_points[M2SDR_NR_MAX_POINTS];
};

/* Returns 0, or -1 on a bad argument (sample rate, fewer samples than an SSB) or an
 * allocation failure. Without an SSB, found = false. cfg may be NULL. */
int m2sdr_nr_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_nr_config *cfg,
                     struct m2sdr_nr_result *res);

/* PSS (127 values of +-1) of n_id_2, SSS of a cell, and DM-RS of the PBCH (144 values, in the
 * order of their elements) of a cell and of i_SSB + 4 n_hf (L_max 4) or the 3 low bits of the
 * index (L_max 8, 64): for the tests. */
void m2sdr_nr_pss(int n_id_2, float *d);
void m2sdr_nr_sss(int n_id_1, int n_id_2, float *d);
void m2sdr_nr_pbch_dmrs(int pci, int i_ssb_bar, float complex *r);

/* GSM */
/*-----*/

#define M2SDR_GSM_SYMBOL_RATE   (1625000.0 / 6.0)
#define M2SDR_GSM_MAX_SCH       16
#define M2SDR_GSM_BURST_BITS    148
#define M2SDR_GSM_MAX_POINTS    1024

struct m2sdr_gsm_config {
    double max_offset_hz;       /* Carrier searched within this of the centre; 0: 20 kHz. */
};

/* One synchronisation burst. */
struct m2sdr_gsm_sch {
    double start;               /* Sample of its bit 0 (the middle of its phase transition). */
    double time_s;
    bool   crc;                 /* Decoded, its parity right: the fields below are valid. */
    int    bsic, ncc, bcc;
    int    t1, t2, t3;          /* T3 from T3': 1, 11, 21, 31 or 41. */
    int    fn;                  /* TDMA frame number of the burst. */
    double power_db;            /* Mean power over the burst, dBFS. */
    double freq_hz;             /* Carrier over the burst, from the centre of the capture. */
    double phase_err_rms_deg;   /* Against the GMSK of its bits (decoded or decided). */
    double phase_err_peak_deg;
    double evm;                 /* Waveform error over the burst, against the same. */
};

/* A GSM downlink carrier with a BCCH (C0), tuned near its centre: the frequency correction
 * bursts (FCCH) and the synchronisation bursts (SCH) that follow them one frame later.
 *
 * The sample rate is 0.5 to 8 MSPS (1.92 MSPS as m2sdr_cal). The capture is filtered to
 * 170 kHz and resampled to 4 samples a bit. The FCCH is found as in m2sdr_cal_gsm_estimate():
 * a tone 67.7 kHz above the carrier that lasts a burst, here from the square of the turn of
 * the phase over a bit (random bits leave no mean of it), timed and measured over its 148
 * bits against itself 6 bits earlier. The SCH, 1250 bits later, is timed on its 64 bits of
 * training, demodulated
 * coherently (the GMSK taken as its main Laurent pulse: one tap, no equaliser), decoded
 * (convolutional code and parity) and modulated again: the phase error is that of the
 * received burst against this ideal GMSK, after its frequency and its phase are taken off, as
 * in TS 45.005 but through the receive filter. A capture of 55 ms holds an FCCH and its SCH
 * wherever it starts.
 *
 * Not measured: the BCCH and the other channels are not decoded; with multipath (no
 * equaliser) the SCH decodes worse and the phase error includes the channel. */
struct m2sdr_gsm_result {
    bool   found;               /* An SCH decoded with its parity right. */
    bool   fcch;                /* Frequency correction bursts found. */
    int    fcch_bursts;
    double fcch_quality;        /* Coherence of the tone over the burst, 0 to 1. */
    double freq_error_hz;       /* Carrier from the centre of the capture: the SCH, else the FCCH. */
    double power_dbfs;          /* Of the capture. */
    int    nsch;                /* SCH bursts demodulated. */
    struct m2sdr_gsm_sch sch[M2SDR_GSM_MAX_SCH];
    /* Of the first SCH decoded. */
    int    bsic, ncc, bcc, fn;
    /* Over the SCH bursts decoded, else demodulated. */
    double burst_power_db;
    double phase_err_rms_deg, phase_err_peak_deg;
    double evm;
    double snr_db;              /* The one the EVM gives. */
    /* Phase error of each bit of the first burst of the figures, degrees. */
    float  phase_err_deg[M2SDR_GSM_BURST_BITS];
    /* Bits of the SCH bursts at their instants, derotated onto the real axis (+-1). */
    int    npoints;
    float complex points[M2SDR_GSM_MAX_POINTS];
};

/* Returns 0, or -1 on a bad argument (sample rate, less than two TDMA frames of samples) or an
 * allocation failure. Without an FCCH, fcch = false. cfg may be NULL. */
int m2sdr_gsm_analyse(const float complex *x, size_t count, double sample_rate, const struct m2sdr_gsm_config *cfg,
                      struct m2sdr_gsm_result *res);

/* SCH information (BSIC and the reduced frame number of fn) to the 78 coded bits of the burst,
 * and the 148 bits of the burst: for the tests. */
void m2sdr_gsm_sch_encode(int bsic, int fn, uint8_t *coded);
void m2sdr_gsm_sch_burst(const uint8_t *coded, uint8_t *bits);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_CELLULAR_H */
