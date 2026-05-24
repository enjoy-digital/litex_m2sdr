/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef M2SDR_SIGMF_H
#define M2SDR_SIGMF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "m2sdr.h"

#define M2SDR_SIGMF_MAX_ANNOTATIONS 64
#define M2SDR_SIGMF_MAX_CAPTURES 64
#define M2SDR_SIGMF_CORE_VERSION "1.2.6"
#define M2SDR_SIGMF_M2SDR_EXTENSION_VERSION "0.1.0"

struct m2sdr_sigmf_capture {
    uint64_t sample_start;
    double center_freq;
    char datetime[64];
    unsigned header_bytes;
    bool has_center_freq;
    bool has_datetime;
    bool has_header_bytes;
};

struct m2sdr_sigmf_annotation {
    uint64_t sample_start;
    uint64_t sample_count;
    double freq_lower_edge;
    double freq_upper_edge;
    char label[64];
    char comment[160];
    bool has_sample_count;
    bool has_freq_lower_edge;
    bool has_freq_upper_edge;
};

struct m2sdr_sigmf_meta {
    char data_path[1024];
    char meta_path[1024];
    char datatype[32];
    char description[256];
    char author[128];
    char hw[128];
    char recorder[128];
    char datetime[64];
    char m2sdr_transport[32];
    double sample_rate;
    double center_freq;
    unsigned num_channels;
    unsigned header_bytes;
    uint64_t m2sdr_sata_data_sector;
    uint64_t m2sdr_sata_data_nsectors;
    uint64_t m2sdr_sata_data_bytes;
    uint64_t m2sdr_sata_meta_sector;
    uint64_t m2sdr_sata_meta_nsectors;
    uint64_t m2sdr_sata_meta_bytes;
    unsigned capture_count;
    struct m2sdr_sigmf_capture captures[M2SDR_SIGMF_MAX_CAPTURES];
    unsigned annotation_count;
    struct m2sdr_sigmf_annotation annotations[M2SDR_SIGMF_MAX_ANNOTATIONS];
    bool has_sample_rate;
    bool has_center_freq;
    bool has_num_channels;
    bool has_header_bytes;
    bool has_datetime;
    bool has_m2sdr_sata_data_sector;
    bool has_m2sdr_sata_data_nsectors;
    bool has_m2sdr_sata_data_bytes;
    bool has_m2sdr_sata_meta_sector;
    bool has_m2sdr_sata_meta_nsectors;
    bool has_m2sdr_sata_meta_bytes;
};

const char *m2sdr_sigmf_datatype_from_format(enum m2sdr_format format);
enum m2sdr_format m2sdr_sigmf_format_from_datatype(const char *datatype);

int m2sdr_sigmf_is_meta_path(const char *path);
int m2sdr_sigmf_is_data_path(const char *path);
int m2sdr_sigmf_derive_paths(const char *input_path,
                             char *data_path,
                             size_t data_path_len,
                             char *meta_path,
                             size_t meta_path_len);

int m2sdr_sigmf_write_text(const struct m2sdr_sigmf_meta *meta, char *buf, size_t buf_len);
int m2sdr_sigmf_read_text(const char *text, size_t text_len,
                          const char *input_path_hint,
                          struct m2sdr_sigmf_meta *meta);
int m2sdr_sigmf_write(const struct m2sdr_sigmf_meta *meta);
int m2sdr_sigmf_read(const char *input_path, struct m2sdr_sigmf_meta *meta);
int m2sdr_sigmf_capture_sample_range(const struct m2sdr_sigmf_meta *meta,
                                     unsigned capture_index,
                                     uint64_t *start_sample,
                                     uint64_t *end_sample);
bool m2sdr_sigmf_timestamp_jump_is_anomalous(uint64_t nominal_dt_ns,
                                             uint64_t dt_ns,
                                             double threshold_pct);
int m2sdr_sigmf_capture_byte_range(const struct m2sdr_sigmf_meta *meta,
                                   unsigned capture_index,
                                   enum m2sdr_format format,
                                   unsigned header_bytes,
                                   unsigned dma_buffer_bytes,
                                   uint64_t *start_offset_bytes,
                                   uint64_t *end_offset_bytes);

#endif /* M2SDR_SIGMF_H */
