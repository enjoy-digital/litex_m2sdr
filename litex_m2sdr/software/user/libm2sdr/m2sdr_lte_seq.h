/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: sequences and broadcast channel coding of the LTE downlink
 * (3GPP TS 36.211 and 36.212), private to libm2sdr and its tests.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_LTE_SEQ_H
#define M2SDR_LTE_SEQ_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LTE_SYNC_LEN       62      /* Subcarriers of the PSS and of the SSS. */
#define LTE_PBCH_BITS      1920    /* Coded bits of a BCH block, over four frames. */
#define LTE_PBCH_FRAME     480     /* Of which each frame carries this many. */
#define LTE_MIB_BITS       24
#define LTE_BCH_BITS       40      /* MIB and its CRC. */
#define LTE_MAX_RB         110

/* PSS of N_ID_2 (0 to 2) on its 62 subcarriers, the lowest first. */
void m2sdr_lte_pss(int n_id_2, float complex *d);

/* SSS of a cell on its 62 subcarriers, in subframe 0 or in subframe 5. */
void m2sdr_lte_sss(int n_id_1, int n_id_2, bool subframe5, float *d);

/* Pseudo-random sequence of TS 36.211 7.2: `count` bits from c_init. */
void m2sdr_lte_gold(uint32_t c_init, uint8_t *c, size_t count);

/* Cell reference signal of a symbol: value of index m (0 to 2 * LTE_MAX_RB - 1), for slot ns
 * (0 to 19), symbol l and a normal cyclic prefix. out holds 2 * LTE_MAX_RB values. */
void m2sdr_lte_crs(int cell_id, int ns, int l, float complex *out);

/* Broadcast channel: MIB (24 bits, one per byte) to the 1920 scrambled bits of four frames,
 * for a number of antenna ports (1, 2 or 4) and a cell. */
void m2sdr_lte_bch_encode(const uint8_t *mib, int ports, int cell_id, uint8_t *bits);

/* The reverse, from the soft bits of one frame (positive: 0), 480 of them. Tries the four
 * positions of the frame in the block and the three numbers of ports. Returns 0 with the MIB,
 * the ports and the position (0 to 3), or -1 when no CRC matches. */
int m2sdr_lte_bch_decode(const float *soft, int cell_id, uint8_t *mib, int *ports, int *phase);

#endif /* M2SDR_LTE_SEQ_H */
