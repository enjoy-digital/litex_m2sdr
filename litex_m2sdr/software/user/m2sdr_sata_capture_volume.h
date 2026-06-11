/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SATA Capture Volume helpers.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_SATA_CAPTURE_VOLUME_H
#define M2SDR_SATA_CAPTURE_VOLUME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SATA_CAPTURE_VOLUME_SECTOR      0x800ull
#define SATA_CAPTURE_VOLUME_SECTORS     64u
#define SATA_DATA_START                 0x100000ull
#define SATA_CAPTURE_NAME_MAX           64
#define SATA_CAPTURE_NOTES_MAX          128
#define SATA_CAPTURE_VOLUME_MAX_ENTRIES 64

struct sata_capture_entry {
    bool used;
    char name[SATA_CAPTURE_NAME_MAX];
    uint64_t sector;
    uint32_t nsectors;
    uint64_t bytes;
    uint64_t meta_sector;
    uint32_t meta_nsectors;
    uint64_t meta_bytes;
    int64_t sample_rate;
    char format[16];
    char channel_layout[16];
    uint64_t rx_freq;
    uint64_t tx_freq;
    uint64_t bandwidth;
    int64_t rx_gain;
    int64_t tx_att;
    uint64_t created;
    char notes[SATA_CAPTURE_NOTES_MAX];
};

struct sata_capture_volume {
    bool initialized;
    struct sata_capture_entry entries[SATA_CAPTURE_VOLUME_MAX_ENTRIES];
};

void capture_volume_clear(struct sata_capture_volume *volume);
int  capture_volume_name_valid(const char *name);
int  capture_volume_text_valid(const char *text, size_t max_len);
void capture_volume_copy(char *dst, size_t dst_len, const char *src);

int capture_volume_parse_text(struct sata_capture_volume *volume, char *text);
int capture_volume_format_text(const struct sata_capture_volume *volume, char *buf, size_t buf_len);

struct sata_capture_entry *capture_volume_find(struct sata_capture_volume *volume, const char *name);
uint64_t capture_volume_end_sector(const struct sata_capture_entry *e);
uint64_t capture_volume_meta_end_sector(const struct sata_capture_entry *e);
uint64_t capture_volume_storage_end_sector(const struct sata_capture_entry *e);
bool capture_volume_regions_overlap(uint64_t a_start, uint64_t a_count,
                                    uint64_t b_start, uint64_t b_count);
int capture_volume_validate_new_region(struct sata_capture_volume *volume, const char *name,
                                       uint64_t sector, uint32_t nsectors);
int capture_volume_validate_new_storage(struct sata_capture_volume *volume, const char *name,
                                        uint64_t sector, uint32_t nsectors,
                                        uint64_t meta_sector, uint32_t meta_nsectors);
uint64_t capture_volume_alloc_sector(struct sata_capture_volume *volume, uint32_t nsectors);
void capture_volume_entry_print(const struct sata_capture_entry *e);
int capture_volume_add_entry(struct sata_capture_volume *volume, const struct sata_capture_entry *entry);

#endif /* M2SDR_SATA_CAPTURE_VOLUME_H */
