/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: cellular and professional radio downlinks.
 *
 * - 5G NR: synchronisation signal blocks (3GPP TS 38.211, 38.213, 38.215).
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

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_CELLULAR_H */
