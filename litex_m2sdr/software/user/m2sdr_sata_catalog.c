/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SATA named capture catalog helpers.
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

#include "m2sdr_sata_catalog.h"
#include "m2sdr_cli.h"

void catalog_clear(struct sata_catalog *cat)
{
    memset(cat, 0, sizeof(*cat));
    cat->initialized = true;
}

int catalog_name_valid(const char *name)
{
    if (!name || !name[0] || strlen(name) >= SATA_CAPTURE_NAME_MAX)
        return 0;
    for (const char *p = name; *p; p++) {
        if (*p == '|' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ' ')
            return 0;
    }
    return 1;
}

int catalog_text_valid(const char *text, size_t max_len)
{
    if (!text)
        return 1;
    if (strlen(text) >= max_len)
        return 0;
    for (const char *p = text; *p; p++) {
        if (*p == '|' || *p == '\n' || *p == '\r')
            return 0;
    }
    return 1;
}

void catalog_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;
    if (!src)
        src = "";
    snprintf(dst, dst_len, "%s", src);
}

static char *catalog_next_field(char **save)
{
    char *field = *save;
    char *sep;

    if (!field)
        return NULL;
    sep = strchr(field, '|');
    if (sep) {
        *sep = '\0';
        *save = sep + 1;
    } else {
        *save = NULL;
    }
    return field;
}

static int catalog_parse_entry(struct sata_catalog *cat, char *line)
{
    char *save = line;
    char *field;
    struct sata_capture_entry e;
    int slot = -1;

    memset(&e, 0, sizeof(e));
    field = catalog_next_field(&save);
    if (!field || strcmp(field, "entry") != 0)
        return 0;

    field = catalog_next_field(&save);
    if (!catalog_name_valid(field))
        return -1;
    catalog_copy(e.name, sizeof(e.name), field);

    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.sector) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u32(field, &e.nsectors) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.bytes) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_int64(field, &e.sample_rate) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!catalog_text_valid(field, sizeof(e.format)))
        return -1;
    catalog_copy(e.format, sizeof(e.format), field);
    field = catalog_next_field(&save);
    if (!catalog_text_valid(field, sizeof(e.channel_layout)))
        return -1;
    catalog_copy(e.channel_layout, sizeof(e.channel_layout), field);
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.rx_freq) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.tx_freq) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.bandwidth) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_int64(field, &e.rx_gain) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_int64(field, &e.tx_att) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!field || m2sdr_cli_parse_u64(field, &e.created) != 0)
        return -1;
    field = catalog_next_field(&save);
    if (!catalog_text_valid(field, sizeof(e.notes)))
        return -1;
    catalog_copy(e.notes, sizeof(e.notes), field ? field : "");
    field = catalog_next_field(&save);
    if (field) {
        if (m2sdr_cli_parse_u64(field, &e.meta_sector) != 0)
            return -1;
        field = catalog_next_field(&save);
        if (!field || m2sdr_cli_parse_u32(field, &e.meta_nsectors) != 0)
            return -1;
        field = catalog_next_field(&save);
        if (!field || m2sdr_cli_parse_u64(field, &e.meta_bytes) != 0)
            return -1;
    }

    e.used = true;
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        if (!cat->entries[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;
    cat->entries[slot] = e;
    return 0;
}

int catalog_parse_text(struct sata_catalog *cat, char *text)
{
    char *line;
    char *save;

    memset(cat, 0, sizeof(*cat));
    line = strtok_r(text, "\n", &save);
    if (!line || strcmp(line, "M2SDR_SATA_CATALOG_V1") != 0)
        return 0;

    cat->initialized = true;
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        if (strncmp(line, "entry|", 6) == 0 && catalog_parse_entry(cat, line) != 0)
            return -1;
    }
    return 0;
}

static int catalog_appendf(char *buf, size_t buf_len, size_t *used, const char *fmt, ...)
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

int catalog_format_text(const struct sata_catalog *cat, char *buf, size_t buf_len)
{
    size_t used = 0;

    if (catalog_appendf(buf, buf_len, &used,
            "M2SDR_SATA_CATALOG_V1\n"
            "catalog_sector=%" PRIu64 "\n"
            "catalog_sectors=%u\n"
            "data_start=%" PRIu64 "\n",
            (uint64_t)SATA_CATALOG_SECTOR, SATA_CATALOG_SECTORS,
            (uint64_t)SATA_DATA_START) != 0)
        return -1;

    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        const struct sata_capture_entry *e = &cat->entries[i];
        if (!e->used)
            continue;
        if (catalog_appendf(buf, buf_len, &used,
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

struct sata_capture_entry *catalog_find(struct sata_catalog *cat, const char *name)
{
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        if (cat->entries[i].used && strcmp(cat->entries[i].name, name) == 0)
            return &cat->entries[i];
    }
    return NULL;
}

static int catalog_first_free(struct sata_catalog *cat)
{
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        if (!cat->entries[i].used)
            return i;
    }
    return -1;
}

uint64_t catalog_end_sector(const struct sata_capture_entry *e)
{
    return e->sector + (uint64_t)e->nsectors;
}

uint64_t catalog_meta_end_sector(const struct sata_capture_entry *e)
{
    return e->meta_sector + (uint64_t)e->meta_nsectors;
}

uint64_t catalog_storage_end_sector(const struct sata_capture_entry *e)
{
    uint64_t end = catalog_end_sector(e);
    uint64_t meta_end = catalog_meta_end_sector(e);

    return meta_end > end ? meta_end : end;
}

bool catalog_regions_overlap(uint64_t a_start, uint64_t a_count,
                             uint64_t b_start, uint64_t b_count)
{
    uint64_t a_end = a_start + a_count;
    uint64_t b_end = b_start + b_count;

    return a_start < b_end && b_start < a_end;
}

static bool catalog_entry_overlaps_region(const struct sata_capture_entry *e,
                                          uint64_t sector, uint32_t nsectors)
{
    if (catalog_regions_overlap(sector, nsectors, e->sector, e->nsectors))
        return true;
    if (e->meta_nsectors != 0 &&
        catalog_regions_overlap(sector, nsectors, e->meta_sector, e->meta_nsectors))
        return true;
    return false;
}

int catalog_validate_new_storage(struct sata_catalog *cat, const char *name,
                                 uint64_t sector, uint32_t nsectors,
                                 uint64_t meta_sector, uint32_t meta_nsectors)
{
    if (!catalog_name_valid(name)) {
        fprintf(stderr, "Invalid capture name. Use a short name without spaces or '|'.\n");
        return 1;
    }
    if (catalog_find(cat, name)) {
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
    if (meta_nsectors != 0 && catalog_regions_overlap(sector, nsectors, meta_sector, meta_nsectors)) {
        fprintf(stderr, "Capture data and metadata regions overlap.\n");
        return 1;
    }
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        const struct sata_capture_entry *e = &cat->entries[i];
        if (!e->used)
            continue;
        if (catalog_entry_overlaps_region(e, sector, nsectors)) {
            fprintf(stderr, "Capture overlaps '%s' at 0x%016" PRIx64 "..0x%016" PRIx64 ".\n",
                e->name, e->sector, catalog_storage_end_sector(e));
            return 1;
        }
        if (meta_nsectors != 0 && catalog_entry_overlaps_region(e, meta_sector, meta_nsectors)) {
            fprintf(stderr, "Capture metadata overlaps '%s' at 0x%016" PRIx64 "..0x%016" PRIx64 ".\n",
                e->name, e->sector, catalog_storage_end_sector(e));
            return 1;
        }
    }
    return 0;
}

int catalog_validate_new_region(struct sata_catalog *cat, const char *name,
                                uint64_t sector, uint32_t nsectors)
{
    return catalog_validate_new_storage(cat, name, sector, nsectors, 0, 0);
}

uint64_t catalog_alloc_sector(struct sata_catalog *cat, uint32_t nsectors)
{
    uint64_t sector = SATA_DATA_START;

    for (;;) {
        bool moved = false;

        for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
            const struct sata_capture_entry *e = &cat->entries[i];
            if (!e->used)
                continue;
            if (catalog_regions_overlap(sector, nsectors, e->sector, e->nsectors)) {
                sector = catalog_end_sector(e);
                moved = true;
            }
            if (e->meta_nsectors != 0 &&
                catalog_regions_overlap(sector, nsectors, e->meta_sector, e->meta_nsectors)) {
                sector = catalog_meta_end_sector(e);
                moved = true;
            }
        }
        if (!moved)
            return sector;
    }
}

void catalog_entry_print(const struct sata_capture_entry *e)
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

int catalog_add_entry(struct sata_catalog *cat, const struct sata_capture_entry *entry)
{
    int slot = catalog_first_free(cat);

    if (slot < 0) {
        fprintf(stderr, "SATA catalog is full.\n");
        return 1;
    }
    cat->entries[slot] = *entry;
    cat->entries[slot].used = true;
    return 0;
}
