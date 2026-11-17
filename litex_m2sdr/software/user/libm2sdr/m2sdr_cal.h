/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR reference clock calibration: carrier frequency offset of cellular downlink signals,
 * and the record of the result in a file or in the SPI flash of a board.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_CAL_H
#define M2SDR_CAL_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Estimators */
/*------------*/

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
    double cp_quality;    /* LTE: peak of the cyclic prefix correlation over its median. */
    double match;         /* LTE: share of the energy under the PSS that correlates with it, 0 to 1. */
};

/* Interleaved I/Q of one channel to complex, scaled to full scale 1.0. `bits` is 12 for SC16
 * captures of the AD9361. `stride` is the number of int16 between two samples of the channel
 * (4 in 2T2R, 2 in 1T1R). The mean is removed. */
void m2sdr_cal_iq_to_complex(const int16_t *iq, size_t count, unsigned stride, unsigned bits,
                             float complex *x);

/* LTE downlink, FDD or TDD, normal cyclic prefix, tuned on the centre of the carrier.
 *
 * The frequency must be the centre. From a raster point next to it, the PSS still correlates,
 * weaker, through the ambiguity of its Zadoff-Chu sequence between frequency and time, and the
 * offset read is wrong by a third or two thirds of the subcarrier spacing (5 or 10 kHz). The
 * scan of the m2sdr_cal utility keeps the strongest of neighbouring detections for this reason. The PSS
 * cyclic prefix gives the offset modulo the subcarrier spacing, the PSS identifies the signal
 * and selects the offset within +-max_cfo_hz.
 * Returns 0, or -1 on a bad argument or allocation failure; a signal that is not found gives
 * found = false. */
int m2sdr_cal_lte_estimate(const float complex *x, size_t count, double max_cfo_hz,
                           struct m2sdr_cal_estimate *est);

/* GSM downlink, tuned on a carrier with a BCCH: frequency of the FCCH bursts. */
int m2sdr_cal_gsm_estimate(const float complex *x, size_t count, double max_cfo_hz,
                           struct m2sdr_cal_estimate *est);

/* Scan: a capture M2SDR_CAL_SCAN_DECIM times as fast holds several carriers. This extracts the
 * one offset_hz from its centre, at M2SDR_CAL_SAMPLE_RATE, for the estimators. y holds
 * count / M2SDR_CAL_SCAN_DECIM samples at most; *out_count is the number written. */
#define M2SDR_CAL_SCAN_DECIM 4
int m2sdr_cal_channelize(const float complex *x, size_t count, double offset_hz,
                         float complex *y, size_t *out_count);

/* Reference clock error, in ppm, for a signal received cfo_hz away from freq_hz: positive when
 * the clock runs fast, which lowers the received frequency. This is the value that
 * m2sdr_set_refclk_ppm() compensates. */
double m2sdr_cal_refclk_ppm(double cfo_hz, double freq_hz);

/* Downlink centre frequency of an LTE EARFCN or of a GSM ARFCN (P-GSM, E-GSM, DCS 1800), in Hz.
 * 0 when the number is not in a supported band. */
double m2sdr_cal_earfcn_to_hz(unsigned earfcn);
double m2sdr_cal_arfcn_to_hz(unsigned arfcn);
/* The reverse: number of a downlink frequency on the raster of a supported band, -1 otherwise.
 * *band (optional) is the LTE band. */
int m2sdr_cal_hz_to_earfcn(double freq_hz, unsigned *band);
int m2sdr_cal_hz_to_arfcn(double freq_hz);

/* Record */
/*--------*/

/* Size of the record, in a file or in the flash. */
#define M2SDR_CAL_RECORD_BYTES 128
#define M2SDR_CAL_NOTE_BYTES   64

/* Default place of the record in the SPI flash: the last 64 KiB of the operational slot
 * (8 MiB at 0x00800000). The bitstreams are compressed and end far below it, and a gateware
 * update only erases the blocks that its image covers. */
#define M2SDR_CAL_FLASH_OFFSET 0x00ff0000u

struct m2sdr_cal_record {
    double   refclk_ppm;                 /* Reference clock error (positive: fast). */
    double   spread_ppm;                 /* Spread of the measurements behind it. */
    double   temperature_c;              /* FPGA temperature at the measurement. */
    uint64_t unix_time;                  /* Date of the measurement, seconds. */
    uint64_t fpga_dna;                   /* Board the record belongs to. */
    uint32_t signals;                    /* Number of signals measured. */
    char     note[M2SDR_CAL_NOTE_BYTES]; /* Free text: the signals used. */
};

/* Record to and from its M2SDR_CAL_RECORD_BYTES bytes (little-endian, CRC-32 at the end).
 * The decode returns 0, or -1 for anything else than a valid record: erased flash included. */
void m2sdr_cal_record_encode(const struct m2sdr_cal_record *rec, uint8_t *buf);
int  m2sdr_cal_record_decode(const uint8_t *buf, struct m2sdr_cal_record *rec);

/* Record in a file. Return 0, or -1 with errno set (ENOENT: no record yet, EINVAL: not a
 * valid record). */
int m2sdr_cal_file_save(const char *path, const struct m2sdr_cal_record *rec);
int m2sdr_cal_file_load(const char *path, struct m2sdr_cal_record *rec);

/* Default file of a board: $XDG_CONFIG_HOME/litex_m2sdr/cal_<dna>.bin, or under ~/.config. */
int m2sdr_cal_default_path(uint64_t fpga_dna, char *path, size_t len);

/* Record of a board */
/*-------------------*/

struct m2sdr_dev;

enum m2sdr_cal_source {
    M2SDR_CAL_SOURCE_NONE  = 0,
    M2SDR_CAL_SOURCE_FLASH = 1,
    M2SDR_CAL_SOURCE_FILE  = 2,
};

/* Record in the SPI flash of the board, at flash_offset (M2SDR_CAL_FLASH_OFFSET by default).
 * The save erases and writes the erase block of the offset, and reads it back. Return 0, or
 * -1 (load: no valid record there). */
int m2sdr_cal_flash_load(struct m2sdr_dev *dev, uint32_t flash_offset, struct m2sdr_cal_record *rec);
int m2sdr_cal_flash_save(struct m2sdr_dev *dev, uint32_t flash_offset, const struct m2sdr_cal_record *rec);
int m2sdr_cal_flash_erase(struct m2sdr_dev *dev, uint32_t flash_offset);

/* Record of an open board: the flash first, then the default file of the board. A record of
 * another board (FPGA DNA) is refused. Returns the source, M2SDR_CAL_SOURCE_NONE without one. */
enum m2sdr_cal_source m2sdr_cal_load(struct m2sdr_dev *dev, uint32_t flash_offset,
                                     struct m2sdr_cal_record *rec);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_CAL_H */
