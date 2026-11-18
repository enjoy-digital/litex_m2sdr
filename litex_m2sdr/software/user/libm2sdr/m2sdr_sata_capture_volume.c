/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SATA Capture Volume helpers.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

#include "m2sdr_sata_capture_volume.h"
#include "m2sdr_cli.h"

/*
 * Keep the original V1 marker and serialized field names so existing disks
 * remain readable after the user-facing rename from catalog to Capture Volume.
 */
#define SATA_CAPTURE_VOLUME_MAGIC_V1 "M2SDR_SATA_CATALOG_V1"

/* Each entry is one '|'-separated record per line; the separator is therefore
 * forbidden inside names and free-text fields. */
#define SATA_CAPTURE_VOLUME_FIELD_SEP '|'

void capture_volume_clear(struct sata_capture_volume *volume)
{
    memset(volume, 0, sizeof(*volume));
    volume->initialized = true;
}

int capture_volume_name_valid(const char *name)
{
    if (!name || !name[0] || strlen(name) >= SATA_CAPTURE_NAME_MAX)
        return 0;
    for (const char *p = name; *p; p++) {
        if (*p == SATA_CAPTURE_VOLUME_FIELD_SEP ||
            *p == '\n' || *p == '\r' || *p == '\t' || *p == ' ')
            return 0;
    }
    return 1;
}

int capture_volume_text_valid(const char *text, size_t max_len)
{
    if (!text)
        return 1;
    if (strlen(text) >= max_len)
        return 0;
    for (const char *p = text; *p; p++) {
        if (*p == SATA_CAPTURE_VOLUME_FIELD_SEP || *p == '\n' || *p == '\r')
            return 0;
    }
    return 1;
}

void capture_volume_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;
    if (!src)
        src = "";
    snprintf(dst, dst_len, "%s", src);
}

static char *capture_volume_next_field(char **save)
{
    char *field = *save;
    char *sep;

    if (!field)
        return NULL;
    sep = strchr(field, SATA_CAPTURE_VOLUME_FIELD_SEP);
    if (sep) {
        *sep = '\0';
        *save = sep + 1;
    } else {
        *save = NULL;
    }
    return field;
}

static int capture_volume_parse_entry(struct sata_capture_volume *volume, char *line)
{
    char *save = line;
    char *field;
    struct sata_capture_entry e;
    int slot = -1;

    memset(&e, 0, sizeof(e));
    field = capture_volume_next_field(&save);
    if (!field || strcmp(field, "entry") != 0)
        return 0;

    field = capture_volume_next_field(&save);
    if (!capture_volume_name_valid(field))
        return -1;
    capture_volume_copy(e.name, sizeof(e.name), field);

    field = capture_volume_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.sector) != 0)
        return -1;
    field = capture_volume_next_field(&save);
    if (!field || m2sdr_cli_parse_u32(field, &e.nsectors) != 0)
        return -1;
    field = capture_volume_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.bytes) != 0)
        return -1;
    field = capture_volume_next_field(&save);
    if (!field || m2sdr_cli_parse_int64(field, &e.sample_rate) != 0)
        return -1;
    field = capture_volume_next_field(&save);
    if (!capture_volume_text_valid(field, sizeof(e.format)))
        return -1;
    capture_volume_copy(e.format, sizeof(e.format), field);
    field = capture_volume_next_field(&save);
    if (!capture_volume_text_valid(field, sizeof(e.channel_layout)))
        return -1;
    capture_volume_copy(e.channel_layout, sizeof(e.channel_layout), field);
    field = capture_volume_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.rx_freq) != 0)
        return -1;
    field = capture_volume_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.tx_freq) != 0)
        return -1;
    field = capture_volume_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.bandwidth) != 0)
        return -1;
    field = capture_volume_next_field(&save);
    if (!field || m2sdr_cli_parse_int64(field, &e.rx_gain) != 0)
        return -1;
    field = capture_volume_next_field(&save);
    if (!field || m2sdr_cli_parse_int64(field, &e.tx_att) != 0)
        return -1;
    field = capture_volume_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.created) != 0)
        return -1;
    field = capture_volume_next_field(&save);
    if (!capture_volume_text_valid(field, sizeof(e.notes)))
        return -1;
    capture_volume_copy(e.notes, sizeof(e.notes), field ? field : "");
    field = capture_volume_next_field(&save);
    if (field) {
        if (m2sdr_cli_parse_u64(field, &e.meta_sector) != 0)
            return -1;
        field = capture_volume_next_field(&save);
        if (!field || m2sdr_cli_parse_u32(field, &e.meta_nsectors) != 0)
            return -1;
        field = capture_volume_next_field(&save);
        if (!field || m2sdr_cli_parse_u64(field, &e.meta_bytes) != 0)
            return -1;
    }

    e.used = true;
    for (int i = 0; i < SATA_CAPTURE_VOLUME_MAX_ENTRIES; i++) {
        if (!volume->entries[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;
    volume->entries[slot] = e;
    return 0;
}

int capture_volume_parse_text(struct sata_capture_volume *volume, char *text)
{
    char *line;
    char *save;

    memset(volume, 0, sizeof(*volume));
    line = strtok_r(text, "\n", &save);
    if (!line || strcmp(line, SATA_CAPTURE_VOLUME_MAGIC_V1) != 0)
        return 0;

    volume->initialized = true;
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        if (strncmp(line, "entry|", 6) == 0 && capture_volume_parse_entry(volume, line) != 0)
            return -1;
    }
    return 0;
}

static int capture_volume_appendf(char *buf, size_t buf_len, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*used >= buf_len)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(buf + *used, buf_len - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= buf_len - *used)
        return -1;
    *used += (size_t)n;
    return 0;
}

int capture_volume_format_text(const struct sata_capture_volume *volume, char *buf, size_t buf_len)
{
    size_t used = 0;

    if (capture_volume_appendf(buf, buf_len, &used,
            SATA_CAPTURE_VOLUME_MAGIC_V1 "\n"
            "catalog_sector=%" PRIu64 "\n"
            "catalog_sectors=%u\n"
            "data_start=%" PRIu64 "\n",
            (uint64_t)SATA_CAPTURE_VOLUME_SECTOR, SATA_CAPTURE_VOLUME_SECTORS,
            (uint64_t)SATA_DATA_START) != 0)
        return -1;

    for (int i = 0; i < SATA_CAPTURE_VOLUME_MAX_ENTRIES; i++) {
        const struct sata_capture_entry *e = &volume->entries[i];
        if (!e->used)
            continue;
        if (capture_volume_appendf(buf, buf_len, &used,
                "entry|%s|%" PRIu64 "|%" PRIu32 "|%" PRIu64 "|%" PRId64
                "|%s|%s|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%" PRId64
                "|%" PRId64 "|%" PRIu64 "|%s|%" PRIu64 "|%" PRIu32 "|%" PRIu64 "\n",
                e->name, e->sector, e->nsectors, e->bytes, e->sample_rate,
                e->format, e->channel_layout, e->rx_freq, e->tx_freq,
                e->bandwidth, e->rx_gain, e->tx_att, e->created, e->notes,
                e->meta_sector, e->meta_nsectors, e->meta_bytes) != 0)
            return -1;
    }
    return 0;
}

struct sata_capture_entry *capture_volume_find(struct sata_capture_volume *volume, const char *name)
{
    for (int i = 0; i < SATA_CAPTURE_VOLUME_MAX_ENTRIES; i++) {
        if (volume->entries[i].used && strcmp(volume->entries[i].name, name) == 0)
            return &volume->entries[i];
    }
    return NULL;
}

static int capture_volume_first_free(struct sata_capture_volume *volume)
{
    for (int i = 0; i < SATA_CAPTURE_VOLUME_MAX_ENTRIES; i++) {
        if (!volume->entries[i].used)
            return i;
    }
    return -1;
}

uint64_t capture_volume_end_sector(const struct sata_capture_entry *e)
{
    return e->sector + (uint64_t)e->nsectors;
}

uint64_t capture_volume_meta_end_sector(const struct sata_capture_entry *e)
{
    return e->meta_sector + (uint64_t)e->meta_nsectors;
}

uint64_t capture_volume_storage_end_sector(const struct sata_capture_entry *e)
{
    uint64_t end = capture_volume_end_sector(e);
    uint64_t meta_end = capture_volume_meta_end_sector(e);

    return meta_end > end ? meta_end : end;
}

bool capture_volume_regions_overlap(uint64_t a_start, uint64_t a_count,
                             uint64_t b_start, uint64_t b_count)
{
    uint64_t a_end = a_start + a_count;
    uint64_t b_end = b_start + b_count;

    return a_start < b_end && b_start < a_end;
}

static bool capture_volume_entry_overlaps_region(const struct sata_capture_entry *e,
                                          uint64_t sector, uint32_t nsectors)
{
    if (capture_volume_regions_overlap(sector, nsectors, e->sector, e->nsectors))
        return true;
    if (e->meta_nsectors != 0 &&
        capture_volume_regions_overlap(sector, nsectors, e->meta_sector, e->meta_nsectors))
        return true;
    return false;
}

int capture_volume_validate_new_storage(struct sata_capture_volume *volume, const char *name,
                                 uint64_t sector, uint32_t nsectors,
                                 uint64_t meta_sector, uint32_t meta_nsectors)
{
    if (!capture_volume_name_valid(name)) {
        fprintf(stderr, "Invalid capture name. Use a short name without spaces or '|'.\n");
        return 1;
    }
    if (capture_volume_find(volume, name)) {
        fprintf(stderr, "Capture '%s' already exists.\n", name);
        return 1;
    }
    if (nsectors == 0) {
        fprintf(stderr, "Capture sector count must be greater than zero.\n");
        return 1;
    }
    if (sector < SATA_DATA_START) {
        fprintf(stderr, "Capture sector 0x%016" PRIx64 " is before data start 0x%016" PRIx64 ".\n",
            sector, (uint64_t)SATA_DATA_START);
        return 1;
    }
    if (meta_nsectors != 0 && meta_sector < SATA_DATA_START) {
        fprintf(stderr, "Metadata sector 0x%016" PRIx64 " is before data start 0x%016" PRIx64 ".\n",
            meta_sector, (uint64_t)SATA_DATA_START);
        return 1;
    }
    if (meta_nsectors != 0 && capture_volume_regions_overlap(sector, nsectors, meta_sector, meta_nsectors)) {
        fprintf(stderr, "Capture data and metadata regions overlap.\n");
        return 1;
    }
    for (int i = 0; i < SATA_CAPTURE_VOLUME_MAX_ENTRIES; i++) {
        const struct sata_capture_entry *e = &volume->entries[i];
        if (!e->used)
            continue;
        if (capture_volume_entry_overlaps_region(e, sector, nsectors)) {
            fprintf(stderr, "Capture overlaps '%s' at 0x%016" PRIx64 "..0x%016" PRIx64 ".\n",
                e->name, e->sector, capture_volume_storage_end_sector(e));
            return 1;
        }
        if (meta_nsectors != 0 && capture_volume_entry_overlaps_region(e, meta_sector, meta_nsectors)) {
            fprintf(stderr, "Capture metadata overlaps '%s' at 0x%016" PRIx64 "..0x%016" PRIx64 ".\n",
                e->name, e->sector, capture_volume_storage_end_sector(e));
            return 1;
        }
    }
    return 0;
}

int capture_volume_validate_new_region(struct sata_capture_volume *volume, const char *name,
                                uint64_t sector, uint32_t nsectors)
{
    return capture_volume_validate_new_storage(volume, name, sector, nsectors, 0, 0);
}

uint64_t capture_volume_alloc_sector(struct sata_capture_volume *volume, uint32_t nsectors)
{
    uint64_t sector = SATA_DATA_START;

    for (;;) {
        bool moved = false;

        for (int i = 0; i < SATA_CAPTURE_VOLUME_MAX_ENTRIES; i++) {
            const struct sata_capture_entry *e = &volume->entries[i];
            if (!e->used)
                continue;
            if (capture_volume_regions_overlap(sector, nsectors, e->sector, e->nsectors)) {
                sector = capture_volume_end_sector(e);
                moved = true;
            }
            if (e->meta_nsectors != 0 &&
                capture_volume_regions_overlap(sector, nsectors, e->meta_sector, e->meta_nsectors)) {
                sector = capture_volume_meta_end_sector(e);
                moved = true;
            }
        }
        if (!moved)
            return sector;
    }
}

void capture_volume_entry_print(const struct sata_capture_entry *e)
{
    printf("Name           : %s\n", e->name);
    printf("Sector         : 0x%016" PRIx64 "\n", e->sector);
    printf("Sectors        : %" PRIu32 "\n", e->nsectors);
    printf("Bytes          : %" PRIu64 "\n", e->bytes);
    if (e->meta_nsectors != 0) {
        printf("SigMF Sector   : 0x%016" PRIx64 "\n", e->meta_sector);
        printf("SigMF Sectors  : %" PRIu32 "\n", e->meta_nsectors);
        printf("SigMF Bytes    : %" PRIu64 "\n", e->meta_bytes);
    }
    printf("Sample Rate    : %" PRId64 "\n", e->sample_rate);
    printf("Format         : %s\n", e->format);
    printf("Channel Layout : %s\n", e->channel_layout);
    printf("RX Frequency   : %" PRIu64 "\n", e->rx_freq);
    printf("TX Frequency   : %" PRIu64 "\n", e->tx_freq);
    printf("Bandwidth      : %" PRIu64 "\n", e->bandwidth);
    printf("RX Gain        : %" PRId64 "\n", e->rx_gain);
    printf("TX Attenuation : %" PRId64 "\n", e->tx_att);
    printf("Created        : %" PRIu64 "\n", e->created);
    if (e->notes[0])
        printf("Notes          : %s\n", e->notes);
}

int capture_volume_add_entry(struct sata_capture_volume *volume, const struct sata_capture_entry *entry)
{
    int slot = capture_volume_first_free(volume);

    if (slot < 0) {
        fprintf(stderr, "SATA Capture Volume is full.\n");
        return 1;
    }
    volume->entries[slot] = *entry;
    volume->entries[slot].used = true;
    return 0;
}
