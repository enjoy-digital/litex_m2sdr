/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SATA SigMF helpers.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_SATA_SIGMF_H
#define M2SDR_SATA_SIGMF_H

#include <stdint.h>

#include "m2sdr_sata_catalog.h"
#include "m2sdr_sigmf.h"

#define M2SDR_SATA_SIGMF_META_SECTORS 64u

uint32_t m2sdr_sata_bytes_to_sectors(uint64_t bytes);

void m2sdr_sata_sigmf_set_storage(struct m2sdr_sigmf_meta *meta,
                                  uint64_t data_sector,
                                  uint32_t data_nsectors,
                                  uint64_t data_bytes,
                                  uint64_t meta_sector,
                                  uint32_t meta_nsectors,
                                  uint64_t meta_bytes,
                                  const char *transport);
void m2sdr_sata_sigmf_from_entry(struct m2sdr_sigmf_meta *meta,
                                 const char *name,
                                 const struct sata_capture_entry *entry);
int m2sdr_sata_sigmf_entry_from_meta(struct sata_capture_entry *entry,
                                     const char *name,
                                     const struct m2sdr_sigmf_meta *meta,
                                     uint64_t data_sector,
                                     uint32_t data_nsectors,
                                     uint64_t data_bytes,
                                     uint64_t meta_sector,
                                     uint32_t meta_nsectors,
                                     uint64_t meta_bytes);

#endif /* M2SDR_SATA_SIGMF_H */
