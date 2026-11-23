/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis, wireless family: ADS-B (Mode S extended squitter), LoRa and Wi-Fi
 * (802.11a/g/n legacy OFDM).
 *
 * Like m2sdr_analysis.h: functions on a block of complex samples scaled to full scale 1.0, with
 * no state besides their arguments (the ADS-B decoder keeps the aircraft of earlier captures in
 * a state given by the caller).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYSIS_WIRELESS_H
#define M2SDR_ANALYSIS_WIRELESS_H

#include "m2sdr_analysis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ADS-B */
/*-------*/

#define M2SDR_ADSB_MAX_MESSAGES  512
#define M2SDR_ADSB_MAX_AIRCRAFT  256
#define M2SDR_ADSB_MIN_RATE      2.0e6   /* Two samples per bit. */

/* One Mode S extended squitter (DF17, DF18) and what its ME field carries. */
struct m2sdr_adsb_message {
    double   time_s;            /* Start of the preamble: time_s of the capture plus its place in it. */
    uint8_t  data[14];          /* 112 bits, the first one in the MSB of data[0]. */
    int      df;                /* Downlink format: 17 or 18. */
    int      ca;                /* Capability (DF17) or control field (DF18). */
    uint32_t icao;              /* 24-bit address. */
    bool     corrected;         /* One bit was wrong and corrected by the parity. */
    double   level_dbfs;        /* Power of the pulses, the noise taken off. */
    double   snr_db;            /* That power over the noise between the pulses. */
    double   confidence;        /* Least contrast of the two halves of a bit, 0 to 1. */
    int      tc;                /* Type code of the ME field, 0 when it is not read (DF18 CF other than 0, 1 and 6). */
    /* TC 1 to 4: identification. */
    char     callsign[9];       /* Trailing spaces removed; "" without. */
    int      category;
    /* TC 9 to 18: airborne position, barometric altitude. */
    bool     has_altitude;
    int      altitude_ft;       /* Q bit set (25 ft steps) only: the 100 ft Gillham code is not decoded. */
    bool     has_cpr;
    int      cpr_odd;           /* F bit. */
    int      cpr_lat, cpr_lon;  /* 17 bits each. */
    bool     has_position;      /* Decoded with the other frame of the pair, or near the last position. */
    double   lat_deg, lon_deg;
    /* TC 19: airborne velocity. */
    bool     has_velocity;
    int      velocity_subtype;  /* 1, 2: over ground, speed and track; 3, 4: air speed and heading. */
    double   speed_kt;          /* Ground speed, or air speed (airspeed_true tells which). */
    bool     airspeed_true;
    bool     has_track;         /* track_deg valid: the track (subtypes 1, 2) or the heading (3, 4). */
    double   track_deg;
    bool     has_vrate;
    int      vrate_fpm;         /* Positive up. */
};

/* An aircraft, from the messages of its address. */
struct m2sdr_adsb_aircraft {
    uint32_t icao;
    int      messages;
    double   last_s;            /* Time of its last message. */
    double   level_dbfs;        /* Of its last message. */
    char     callsign[9];
    bool     has_altitude;
    int      altitude_ft;
    bool     has_velocity;
    double   speed_kt, track_deg;
    int      vrate_fpm;
    bool     has_position;
    double   lat_deg, lon_deg;
    double   position_s;        /* Time of the position. */
    /* CPR frames kept for the global decoding: the last even and odd one. */
    int      cpr_lat[2], cpr_lon[2];
    double   cpr_s[2];          /* Their time; negative: none. */
};

/* Decoder state across captures: zero it before the first one. The CPR position needs an even
 * and an odd frame of the same aircraft less than 10 s apart, often in two captures. */
struct m2sdr_adsb_state {
    int      aircraft;
    struct m2sdr_adsb_aircraft ac[M2SDR_ADSB_MAX_AIRCRAFT];
};

struct m2sdr_adsb_config {
    bool     no_correction;     /* Do not correct one-bit errors. */
    bool     has_reference;     /* Receiver position: single frames are then decoded near it (180 NM). */
    double   ref_lat_deg, ref_lon_deg;
};

/* Mode S extended squitters at 1090 MHz, tuned on 1090 MHz.
 *
 * The magnitude is integrated over the half bits (chips of 0.5 us), at any rate from 2 MSPS:
 * the preamble (pulses at 0, 1, 3.5 and 4.5 us) is found, its timing refined to an eighth of a
 * sample, and its chips measure how a pulse spreads on its neighbours at that timing. The 112
 * bits are then the most likely sequence of pulses given that spread (a two-state Viterbi), so
 * that pulses across two samples are still told apart. A message is kept when its CRC-24 is
 * right (or right with one bit corrected) and it is a DF17 or DF18. Not decoded: the short
 * (56-bit) replies, the other formats, surface positions (TC 5 to 8), the GNSS altitude (TC 20
 * to 22), the 100 ft (Gillham) altitudes, and the status messages (TC 28, 29, 31).
 *
 * At 2 MSPS a pulse is one sample: half a sample off, it shares its power with the next one,
 * its level reads up to 6 dB low and the bits need about 10 dB more SNR. 2.4 MSPS and more
 * leave less to chance; 4 MSPS is best. */
struct m2sdr_adsb_result {
    int      preambles;         /* Tried. */
    int      messages;          /* Kept (in message[], the first M2SDR_ADSB_MAX_MESSAGES). */
    int      corrected;         /* Of them, with a bit corrected. */
    double   message_rate;      /* Messages per second. */
    double   noise_dbfs;        /* Median power of the samples: the noise when the band is quiet. */
    double   power_dbfs;        /* Of the capture. */
    int      nmessages;
    struct m2sdr_adsb_message message[M2SDR_ADSB_MAX_MESSAGES];
};

/* time_s is the time of the first sample (from any origin, seconds): it dates the messages and
 * the CPR frames. state may be NULL (no aircraft kept, no positions from two captures), so may cfg.
 * Returns 0, or -1 on a bad argument (rate under 2 MSPS, less than 120 us of samples) or an
 * allocation failure. */
int m2sdr_adsb_analyse(const float complex *x, size_t count, double sample_rate, double time_s,
                       const struct m2sdr_adsb_config *cfg, struct m2sdr_adsb_state *state,
                       struct m2sdr_adsb_result *res);

/* Remainder of the CRC-24 of a Mode S message of 56 or 112 bits: 0 for a right DF17/18. */
uint32_t m2sdr_adsb_crc(const uint8_t *msg, int bits);

/* Decodes the fields of a 112-bit message (df to vrate_fpm, not the position). Returns 0, or -1
 * when it is not a DF17 or DF18. The CRC is not checked. */
int m2sdr_adsb_decode(const uint8_t msg[14], struct m2sdr_adsb_message *m);

/* LoRa */
/*------*/

#define M2SDR_LORA_MAX_PACKETS   32
#define M2SDR_LORA_MAX_SYMBOLS   256

struct m2sdr_lora_packet {
    double   time_s;            /* Start of the preamble, from the first sample. */
    int      sf;                /* Spreading factor, 7 to 12. */
    double   bw_hz;             /* 125, 250 or 500 kHz. */
    double   freq_error_hz;     /* Carrier, from the centre of the capture (within +-bw/4). */
    double   rssi_dbfs;         /* Power in the bandwidth over the packet, noise included. */
    double   snr_db;            /* Signal over the noise in the bandwidth, from the preamble: the
                                 * LoRa figure, below 0 dB down to the limit of the SF. */
    int      preamble_symbols;  /* Up-chirps (8 in LoRaWAN). */
    bool     sync;              /* Sync word and down-chirps found: the data symbols follow. */
    int      sync_symbols[2];   /* The two of the sync word. */
    int      sync_word;         /* From them, when both are multiples of 8: 0x34 public (LoRaWAN),
                                 * 0x12 private; -1 otherwise. */
    int      symbols;           /* Data symbols demodulated (the first M2SDR_LORA_MAX_SYMBOLS in symbol[]). */
    bool     complete;          /* The packet ends inside the capture. */
    double   airtime_s;         /* From the start of the preamble to the end of the last symbol. */
    uint16_t symbol[M2SDR_LORA_MAX_SYMBOLS];   /* 0 to 2^sf - 1. */
};

/* Known parameters, to search less: 0 for each found. */
struct m2sdr_lora_config {
    int      sf;
    double   bw_hz;
};

/* LoRa chirp spread spectrum packets (868 MHz ISM band or any), tuned on their channel.
 *
 * Each bandwidth is cut out of the capture (FFT, then the bins of the bandwidth back to time:
 * a sample rate of the bandwidth), which needs sample_rate / bw_hz to be a power of two (1 MSPS
 * takes the three bandwidths). Each spreading factor then dechirps windows of 2^sf samples:
 * four windows or more in a row with the same strong FFT peak are up-chirps of a preamble. The
 * peak of the down-chirps after them gives, with that of the up-chirps, the carrier offset and
 * the timing; the symbols are then read on their own windows, the FFT peak of each. A packet
 * taken by the wrong bandwidth and SF of the same chirp slope (SF + 2 at twice the bandwidth) is
 * left to the right one, the stronger peak.
 *
 * Not decoded: the header and the payload (Gray mapping, interleaving, Hamming code, whitening,
 * CRC): symbol[] holds the FFT bins as received. */
struct m2sdr_lora_result {
    int      packets;           /* Found (in packet[], the first M2SDR_LORA_MAX_PACKETS). */
    double   busy_fraction;     /* Time of the capture with a packet on the air. */
    double   power_dbfs;        /* Of the capture. */
    int      npackets;
    struct m2sdr_lora_packet packet[M2SDR_LORA_MAX_PACKETS];
};

/* cfg may be NULL. Returns 0, or -1 on a bad argument (no bandwidth searched divides the sample
 * rate by a power of two, less than 16 symbols of SF7 at 500 kHz) or an allocation failure.
 * Without a packet, packets = 0. */
int m2sdr_lora_analyse(const float complex *x, size_t count, double sample_rate,
                       const struct m2sdr_lora_config *cfg, struct m2sdr_lora_result *res);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_ANALYSIS_WIRELESS_H */
