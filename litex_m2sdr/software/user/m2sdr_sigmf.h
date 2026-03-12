/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef M2SDR_SIGMF_H
#define M2SDR_SIGMF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "m2sdr.h"

#define M2SDR_SIGMF_MAX_ANNOTATIONS 64
#define M2SDR_SIGMF_MAX_CAPTURES 64

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
    double sample_rate;
    double center_freq;
    unsigned num_channels;
    unsigned header_bytes;
    unsigned capture_count;
    struct m2sdr_sigmf_capture captures[M2SDR_SIGMF_MAX_CAPTURES];
    unsigned annotation_count;
    struct m2sdr_sigmf_annotation annotations[M2SDR_SIGMF_MAX_ANNOTATIONS];
    bool has_sample_rate;
    bool has_center_freq;
    bool has_num_channels;
    bool has_header_bytes;
    bool has_datetime;
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

int m2sdr_sigmf_write(const struct m2sdr_sigmf_meta *meta);
int m2sdr_sigmf_read(const char *input_path, struct m2sdr_sigmf_meta *meta);

#endif /* M2SDR_SIGMF_H */
