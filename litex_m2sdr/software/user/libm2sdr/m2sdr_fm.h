/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR broadcast FM: modulator and demodulator between audio and I/Q samples, mono or stereo
 * (pilot-tone multiplex), with the Radio Data System (RDS, EN 50067 / IEC 62106).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_FM_H
#define M2SDR_FM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Constants */
/*-----------*/

#define M2SDR_FM_DEVIATION_HZ  75000.0  /* Peak deviation of broadcast FM. */
#define M2SDR_FM_TAU_EU        50e-6    /* Emphasis time constant, Europe and most countries. */
#define M2SDR_FM_TAU_US        75e-6    /* Emphasis time constant, Americas and South Korea. */
#define M2SDR_FM_AUDIO_HZ      15000.0  /* Upper edge of the audio band. */
#define M2SDR_FM_PILOT_HZ      19000.0  /* Stereo pilot; the L-R subcarrier is at twice this. */

/* Share of the peak deviation taken by the pilot, in stereo. The audio takes the rest. */
#define M2SDR_FM_PILOT_LEVEL   0.1

/* Lowest I/Q sample rates: the multiplex goes up to 15 kHz in mono, 53 kHz in stereo and
 * 59.4 kHz with RDS. */
#define M2SDR_FM_MIN_RATE_MONO    64000.0
#define M2SDR_FM_MIN_RATE_STEREO  128000.0
#define M2SDR_FM_MIN_RATE_RDS     128000.0

/* RDS */
/*-----*/

#define M2SDR_FM_RDS_HZ        57000.0  /* Subcarrier: three times the pilot. */
#define M2SDR_FM_RDS_BITRATE   1187.5   /* The subcarrier divided by 48. */

/* Share of the peak deviation taken by the RDS subcarrier: 3 kHz of 75 kHz. As in the
 * standard, this is the level of a stream of identical bits; data peaks are higher. */
#define M2SDR_FM_RDS_LEVEL     0.04

#define M2SDR_FM_RDS_PS_LEN    8        /* Programme Service name. */
#define M2SDR_FM_RDS_RT_LEN    64       /* RadioText. */

/* What the modulator broadcasts: groups 0A (PI, PTY, flags, PS), 2A (RT) when the text is
 * not empty, and 4A (clock time) at each minute when `ct` is set. */
struct m2sdr_fm_rds_config {
    bool     enable;       /* Modulator: add the RDS subcarrier. Demodulator: decode it, and
                            * scale the audio for a signal that gives RDS its share. */
    uint16_t pi;           /* Programme Identification. */
    uint8_t  pty;          /* Programme Type, 0 to 31. */
    bool     tp, ta;       /* Traffic Programme, Traffic Announcement. */
    bool     music;        /* Music/Speech switch. */
    char     ps[M2SDR_FM_RDS_PS_LEN + 1];  /* Padded with spaces. */
    char     rt[M2SDR_FM_RDS_RT_LEN + 1];
    bool     ct;           /* Send the clock time. */
    int64_t  ct_unix_time; /* UTC time of the first sample, in seconds since 1970. */
    int      ct_offset;    /* Local time minus UTC, in half hours, -31 to 31. */
};

/* What the demodulator has decoded so far. */
struct m2sdr_fm_rds_info {
    bool     synced;       /* Locked on the sequence of blocks. */
    uint64_t blocks;       /* Blocks received while synced... */
    uint64_t block_errors; /* ...and those with a wrong checkword (they are dropped, not corrected). */
    uint64_t groups;       /* Groups received with their four blocks right. */
    unsigned updates;      /* Incremented each time a field below changes. */
    bool     pi_valid;     /* A group was received: pi, pty and tp are set. */
    uint16_t pi;
    uint8_t  pty;
    bool     tp, ta, music;
    bool     ps_valid;     /* The eight characters were received. */
    char     ps[M2SDR_FM_RDS_PS_LEN + 1];
    bool     rt_valid;     /* The whole text was received. */
    char     rt[M2SDR_FM_RDS_RT_LEN + 1];
    bool     ct_valid;     /* A clock time group was received. */
    int64_t  ct_unix_time; /* UTC, in seconds since 1970. */
    int      ct_offset;    /* Local time minus UTC, in half hours. */
};

/* Blocks: 16 information bits, then a 10-bit checkword (generator polynomial
 * x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1) to which the offset word of the block is added. */
enum m2sdr_fm_rds_offset {
    M2SDR_FM_RDS_OFFSET_A  = 0,
    M2SDR_FM_RDS_OFFSET_B  = 1,
    M2SDR_FM_RDS_OFFSET_C  = 2,
    M2SDR_FM_RDS_OFFSET_CP = 3,  /* C', third block of the version B groups. */
    M2SDR_FM_RDS_OFFSET_D  = 4,
};

/* The 26-bit block of an information word, first transmitted bit in bit 25. */
uint32_t m2sdr_fm_rds_encode_block(uint16_t data, enum m2sdr_fm_rds_offset offset);
/* The offset word of a received 26-bit block and its information word, or -1 when the
 * checkword matches no offset word. */
int m2sdr_fm_rds_decode_block(uint32_t block, uint16_t *data);

/* Name of a Programme Type in the RDS table (Europe); "" above 31. */
const char *m2sdr_fm_rds_pty_name(unsigned pty);

struct m2sdr_fm_config {
    double   sample_rate;  /* I/Q sample rate, in Hz. */
    double   deviation;    /* Deviation of full-scale audio, in Hz (0.4 x sample_rate at most). */
    double   tau;          /* Emphasis time constant, in seconds. 0: no emphasis. */
    bool     stereo;       /* Pilot-tone stereo multiplex instead of mono. */
    unsigned bits;         /* Modulator: significant bits of the I/Q samples, 2 to 16
                            * (amplitude 2^(bits-1) - 1). Unused by the demodulator. */
    struct m2sdr_fm_rds_config rds;
};

/* Sample rate 1 MSPS, 75 kHz deviation, 50 us emphasis, mono, 12 bits, no RDS. */
void m2sdr_fm_config_init(struct m2sdr_fm_config *cfg);

/* Modulator */
/*-----------*/

/* Audio -> pre-emphasis -> limiter -> 15 kHz low-pass -> stereo multiplex and RDS
 * -> interpolation -> FM. The RDS subcarrier is locked on the third harmonic of the pilot,
 * in phase, and takes its share of the deviation from the audio. The audio is given at m2sdr_fm_tx_audio_rate(), an integer fraction of the I/Q
 * rate, with full scale +-1.0: the caller resamples its source to this rate. */
struct m2sdr_fm_tx;

/* NULL on a bad configuration or allocation failure. */
struct m2sdr_fm_tx *m2sdr_fm_tx_new(const struct m2sdr_fm_config *cfg);
void m2sdr_fm_tx_free(struct m2sdr_fm_tx *tx);

/* Rate of the audio input, in Hz, and number of I/Q samples made from one audio frame. */
double   m2sdr_fm_tx_audio_rate(const struct m2sdr_fm_tx *tx);
unsigned m2sdr_fm_tx_interpolation(const struct m2sdr_fm_tx *tx);

/* Modulates `frames` audio frames (one float in mono, left then right in stereo) into
 * frames * interpolation I/Q samples (I then Q). Returns the number of I/Q samples written. */
size_t m2sdr_fm_tx_process(struct m2sdr_fm_tx *tx, const float *audio, size_t frames, int16_t *iq);

/* Audio samples limited to full scale so far, after the pre-emphasis: the input was too loud
 * for the deviation. */
uint64_t m2sdr_fm_tx_clipped(const struct m2sdr_fm_tx *tx);

/* Demodulator */
/*-------------*/

/* I/Q -> channel filter and decimation -> discriminator -> pilot PLL and stereo decoder
 * -> de-emphasis -> 15 kHz low-pass and decimation -> audio. The audio comes out at
 * m2sdr_fm_rx_audio_rate(), an integer fraction of the I/Q rate: the caller resamples it to
 * the rate of its sink. */
struct m2sdr_fm_rx;

struct m2sdr_fm_rx *m2sdr_fm_rx_new(const struct m2sdr_fm_config *cfg);
void m2sdr_fm_rx_free(struct m2sdr_fm_rx *rx);

/* Rate of the audio output, in Hz, and number of I/Q samples behind one audio frame. */
double   m2sdr_fm_rx_audio_rate(const struct m2sdr_fm_rx *rx);
unsigned m2sdr_fm_rx_decimation(const struct m2sdr_fm_rx *rx);

/* Demodulates `count` I/Q samples (I then Q, any amplitude) into audio frames (one float in
 * mono, left then right in stereo; full scale +-1.0 at the configured deviation, not limited).
 * `audio` holds count / decimation + 1 frames at least. Returns the number of frames written. */
size_t m2sdr_fm_rx_process(struct m2sdr_fm_rx *rx, const int16_t *iq, size_t count, float *audio);

/* Stereo: the pilot is received and the two channels are decoded. Without a pilot the two
 * channels carry the mono signal. */
bool m2sdr_fm_rx_pilot_locked(const struct m2sdr_fm_rx *rx);

/* RDS decoded so far, when enabled in the configuration: the 57 kHz subcarrier is recovered
 * on its own, so this also works in mono and without a pilot. */
void m2sdr_fm_rx_rds(const struct m2sdr_fm_rx *rx, struct m2sdr_fm_rds_info *info);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_FM_H */
