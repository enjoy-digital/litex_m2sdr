/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR reference clock calibration: carrier frequency offset of cellular downlink signals.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_CAL_DSP_H
#define M2SDR_CAL_DSP_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sample rate of the captures given to the estimators: the central 1.08 MHz of an LTE carrier
 * (PSS, SSS, PBCH) and a GSM carrier with its two neighbours fit in it. */
#define M2SDR_CAL_SAMPLE_RATE 1920000.0

/* GSM: the frequency correction burst is a tone 1625/24 kHz above the carrier. */
#define M2SDR_CAL_GSM_FCCH_HZ (1625000.0 / 24.0)

enum m2sdr_cal_signal {
    M2SDR_CAL_SIGNAL_LTE = 0,
    M2SDR_CAL_SIGNAL_GSM = 1,
};

struct m2sdr_cal_estimate {
    bool   found;         /* A signal of the expected kind was identified. */
    double cfo_hz;        /* Received carrier minus tuned frequency, in Hz. */
    double spread_hz;     /* Standard deviation of the partial estimates. */
    double quality;       /* LTE: PSS peak over mean. GSM: share of the FCCH gaps on the frame grid. */
    int    count;         /* LTE: blocks used. GSM: FCCH bursts used. */
    int    id;            /* LTE: PSS root (25, 29, 34), N_ID_2 = 0, 1, 2. GSM: unused. */
    double power_db;      /* Mean power of the capture, dB full scale. */
};

/* Interleaved I/Q of one channel to complex, scaled to full scale 1.0. `bits` is 12 for SC16
 * captures of the AD9361. `stride` is the number of int16 between two samples of the channel
 * (4 in 2T2R, 2 in 1T1R). The mean is removed. */
void m2sdr_cal_iq_to_complex(const int16_t *iq, size_t count, unsigned stride, unsigned bits,
                             float complex *x);

/* LTE downlink, FDD or TDD, normal cyclic prefix, tuned on the centre of the carrier. The PSS
 * identifies the signal and gives the offset within +-max_cfo_hz, the cyclic prefix refines it.
 * Returns 0, or -1 on a bad argument or allocation failure; a signal that is not found gives
 * found = false. */
int m2sdr_cal_lte_estimate(const float complex *x, size_t count, double max_cfo_hz,
                           struct m2sdr_cal_estimate *est);

/* GSM downlink, tuned on a carrier with a BCCH: frequency of the FCCH bursts. */
int m2sdr_cal_gsm_estimate(const float complex *x, size_t count, double max_cfo_hz,
                           struct m2sdr_cal_estimate *est);

/* Reference clock error, in ppm, for a signal received cfo_hz away from freq_hz: positive when
 * the clock runs fast, which lowers the received frequency. This is the value that
 * m2sdr_set_refclk_ppm() compensates. */
double m2sdr_cal_refclk_ppm(double cfo_hz, double freq_hz);

/* Downlink centre frequency of an LTE EARFCN or of a GSM ARFCN (P-GSM, E-GSM, DCS 1800), in Hz.
 * 0 when the number is not in a supported band. */
double m2sdr_cal_earfcn_to_hz(unsigned earfcn);
double m2sdr_cal_arfcn_to_hz(unsigned arfcn);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_CAL_DSP_H */
