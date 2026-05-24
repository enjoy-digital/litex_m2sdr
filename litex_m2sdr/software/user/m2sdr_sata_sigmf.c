/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SATA SigMF helpers.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "m2sdr_sata_sigmf.h"
#include "m2sdr_sata_lowlevel.h"

uint32_t m2sdr_sata_bytes_to_sectors(uint64_t bytes)
{
    return (uint32_t)((bytes + SATA_SECTOR_BYTES - 1u) / SATA_SECTOR_BYTES);
}

static const char *catalog_format_to_datatype(const char *format)
{
    if (format && strcmp(format, "sc8") == 0)
        return "ci8";
    return "ci16_le";
}

static const char *datatype_to_catalog_format(const char *datatype)
{
    if (datatype && strcmp(datatype, "ci8") == 0)
        return "sc8";
    if (datatype && strcmp(datatype, "ci16_le") == 0)
        return "sc16";
    return NULL;
}

static unsigned catalog_layout_to_channels(const char *layout)
{
    if (layout && strcmp(layout, "1t1r") == 0)
        return 1;
    return 2;
}

static const char *channels_to_catalog_layout(unsigned channels)
{
    return channels <= 1 ? "1t1r" : "2t2r";
}

static void m2sdr_sata_sigmf_set_paths(struct m2sdr_sigmf_meta *meta, const char *name)
{
    snprintf(meta->data_path, sizeof(meta->data_path), "%s.sigmf-data", name ? name : "capture");
    snprintf(meta->meta_path, sizeof(meta->meta_path), "%s.sigmf-meta", name ? name : "capture");
}

void m2sdr_sata_sigmf_set_storage(struct m2sdr_sigmf_meta *meta,
                                  uint64_t data_sector,
                                  uint32_t data_nsectors,
                                  uint64_t data_bytes,
                                  uint64_t meta_sector,
                                  uint32_t meta_nsectors,
                                  uint64_t meta_bytes,
                                  const char *transport)
{
    if (!meta)
        return;

    meta->m2sdr_sata_data_sector = data_sector;
    meta->m2sdr_sata_data_nsectors = data_nsectors;
    meta->m2sdr_sata_data_bytes = data_bytes;
    meta->m2sdr_sata_meta_sector = meta_sector;
    meta->m2sdr_sata_meta_nsectors = meta_nsectors;
    meta->m2sdr_sata_meta_bytes = meta_bytes;
    meta->has_m2sdr_sata_data_sector = true;
    meta->has_m2sdr_sata_data_nsectors = true;
    meta->has_m2sdr_sata_data_bytes = true;
    meta->has_m2sdr_sata_meta_sector = true;
    meta->has_m2sdr_sata_meta_nsectors = true;
    meta->has_m2sdr_sata_meta_bytes = true;
    if (transport && transport[0])
        snprintf(meta->m2sdr_transport, sizeof(meta->m2sdr_transport), "%s", transport);
}

void m2sdr_sata_sigmf_from_entry(struct m2sdr_sigmf_meta *meta,
                                 const char *name,
                                 const struct sata_capture_entry *entry)
{
    if (!meta || !entry)
        return;

    memset(meta, 0, sizeof(*meta));
    m2sdr_sata_sigmf_set_paths(meta, name ? name : entry->name);
    snprintf(meta->datatype, sizeof(meta->datatype), "%s", catalog_format_to_datatype(entry->format));
    snprintf(meta->recorder, sizeof(meta->recorder), "m2sdr_sata");
    if (entry->notes[0])
        snprintf(meta->description, sizeof(meta->description), "%s", entry->notes);
    else
        snprintf(meta->description, sizeof(meta->description), "M2SDR SATA capture %s", entry->name);

    if (entry->sample_rate > 0) {
        meta->sample_rate = (double)entry->sample_rate;
        meta->has_sample_rate = true;
    }
    meta->num_channels = catalog_layout_to_channels(entry->channel_layout);
    meta->has_num_channels = true;

    meta->capture_count = 1;
    meta->captures[0].sample_start = 0;
    if (entry->rx_freq != 0) {
        meta->center_freq = (double)entry->rx_freq;
        meta->has_center_freq = true;
        meta->captures[0].center_freq = (double)entry->rx_freq;
        meta->captures[0].has_center_freq = true;
    }

    m2sdr_sata_sigmf_set_storage(meta,
        entry->sector, entry->nsectors, entry->bytes,
        entry->meta_sector, entry->meta_nsectors, entry->meta_bytes,
        NULL);
}

int m2sdr_sata_sigmf_entry_from_meta(struct sata_capture_entry *entry,
                                     const char *name,
                                     const struct m2sdr_sigmf_meta *meta,
                                     uint64_t data_sector,
                                     uint32_t data_nsectors,
                                     uint64_t data_bytes,
                                     uint64_t meta_sector,
                                     uint32_t meta_nsectors,
                                     uint64_t meta_bytes)
{
    const char *format;

    if (!entry || !name || !meta)
        return -1;
    format = datatype_to_catalog_format(meta->datatype);
    if (!format)
        return -1;

    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    catalog_copy(entry->name, sizeof(entry->name), name);
    entry->sector = data_sector;
    entry->nsectors = data_nsectors;
    entry->bytes = data_bytes;
    entry->meta_sector = meta_sector;
    entry->meta_nsectors = meta_nsectors;
    entry->meta_bytes = meta_bytes;
    entry->sample_rate = meta->has_sample_rate ? (int64_t)meta->sample_rate : 0;
    catalog_copy(entry->format, sizeof(entry->format), format);
    catalog_copy(entry->channel_layout, sizeof(entry->channel_layout),
                 channels_to_catalog_layout(meta->has_num_channels ? meta->num_channels : 2));
    if (meta->has_center_freq) {
        entry->rx_freq = (uint64_t)meta->center_freq;
        entry->tx_freq = (uint64_t)meta->center_freq;
    }
    entry->created = (uint64_t)time(NULL);
    if (meta->description[0])
        catalog_copy(entry->notes, sizeof(entry->notes), meta->description);
    return 0;
}
