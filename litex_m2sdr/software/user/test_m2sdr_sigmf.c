/* SPDX-License-Identifier: BSD-2-Clause */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "m2sdr_sigmf.h"

static int expect_true(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return -1;
    }
    return 0;
}

static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        return -1;
    if (fwrite(content, 1, strlen(content), f) != strlen(content)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int test_derive_paths(void)
{
    char data[128];
    char meta[128];

    if (m2sdr_sigmf_derive_paths("capture", data, sizeof(data), meta, sizeof(meta)) != 0)
        return expect_true(0, "derive paths from basename");
    if (expect_true(strcmp(data, "capture.sigmf-data") == 0, "basename data path") != 0 ||
        expect_true(strcmp(meta, "capture.sigmf-meta") == 0, "basename meta path") != 0)
        return -1;

    if (m2sdr_sigmf_derive_paths("capture.sigmf-meta", data, sizeof(data), meta, sizeof(meta)) != 0)
        return expect_true(0, "derive from meta path");
    if (expect_true(strcmp(data, "capture.sigmf-data") == 0, "meta -> data path") != 0 ||
        expect_true(strcmp(meta, "capture.sigmf-meta") == 0, "meta -> meta path") != 0)
        return -1;

    if (m2sdr_sigmf_derive_paths("capture.sigmf-data", data, sizeof(data), meta, sizeof(meta)) != 0)
        return expect_true(0, "derive from data path");
    if (expect_true(strcmp(data, "capture.sigmf-data") == 0, "data path preserved") != 0 ||
        expect_true(strcmp(meta, "capture.sigmf-meta") == 0, "data -> meta path") != 0)
        return -1;

    return 0;
}

static int test_roundtrip(const char *dir)
{
    struct m2sdr_sigmf_meta meta;
    struct m2sdr_sigmf_meta parsed;
    char base[512];

    memset(&meta, 0, sizeof(meta));
    snprintf(base, sizeof(base), "%s/roundtrip", dir);
    if (m2sdr_sigmf_derive_paths(base, meta.data_path, sizeof(meta.data_path),
                                 meta.meta_path, sizeof(meta.meta_path)) != 0)
        return expect_true(0, "derive roundtrip paths");

    snprintf(meta.datatype, sizeof(meta.datatype), "ci16_le");
    snprintf(meta.description, sizeof(meta.description), "loopback");
    snprintf(meta.author, sizeof(meta.author), "tester");
    snprintf(meta.hw, sizeof(meta.hw), "m2sdr");
    snprintf(meta.recorder, sizeof(meta.recorder), "test_m2sdr_sigmf");
    meta.sample_rate = 30.72e6;
    meta.has_sample_rate = true;
    meta.num_channels = 2;
    meta.has_num_channels = true;
    meta.center_freq = 2.45e9;
    meta.has_center_freq = true;
    meta.datetime[0] = '\0';
    meta.annotation_count = 2;
    meta.annotations[0].sample_start = 0;
    meta.annotations[0].sample_count = 1024;
    meta.annotations[0].has_sample_count = true;
    snprintf(meta.annotations[0].label, sizeof(meta.annotations[0].label), "burst-a");
    meta.annotations[1].sample_start = 2048;
    meta.annotations[1].sample_count = 256;
    meta.annotations[1].has_sample_count = true;
    meta.annotations[1].freq_lower_edge = 2.4495e9;
    meta.annotations[1].freq_upper_edge = 2.4505e9;
    meta.annotations[1].has_freq_lower_edge = true;
    meta.annotations[1].has_freq_upper_edge = true;
    snprintf(meta.annotations[1].comment, sizeof(meta.annotations[1].comment), "narrowband");

    if (m2sdr_sigmf_write(&meta) != 0)
        return expect_true(0, "write roundtrip metadata");
    if (m2sdr_sigmf_read(meta.meta_path, &parsed) != 0)
        return expect_true(0, "read roundtrip metadata");

    if (expect_true(strcmp(parsed.datatype, "ci16_le") == 0, "roundtrip datatype") != 0 ||
        expect_true(parsed.has_sample_rate && parsed.sample_rate == meta.sample_rate, "roundtrip sample rate") != 0 ||
        expect_true(parsed.has_num_channels && parsed.num_channels == 2, "roundtrip channel count") != 0 ||
        expect_true(parsed.has_center_freq && parsed.center_freq == meta.center_freq, "roundtrip center frequency") != 0 ||
        expect_true(parsed.annotation_count == 2, "roundtrip annotation count") != 0 ||
        expect_true(strcmp(parsed.annotations[0].label, "burst-a") == 0, "roundtrip annotation label") != 0 ||
        expect_true(parsed.annotations[1].has_freq_lower_edge, "roundtrip annotation freq low") != 0 ||
        expect_true(strcmp(parsed.annotations[1].comment, "narrowband") == 0, "roundtrip annotation comment") != 0)
        return -1;

    return 0;
}

static int test_manual_metadata(const char *dir)
{
    struct m2sdr_sigmf_meta parsed;
    char meta_path[512];
    char data_dir[512];
    const char *json =
        "{\n"
        "  \"global\": {\n"
        "    \"core:version\": \"1.2.5\",\n"
        "    \"core:datatype\": \"ci8\",\n"
        "    \"core:dataset\": \"datasets/capture.sigmf-data\",\n"
        "    \"core:sample_rate\": 30720000,\n"
        "    \"core:num_channels\": 1,\n"
        "    \"core:description\": \"manual\"\n"
        "  },\n"
        "  \"captures\": [\n"
        "    {\"core:sample_start\": 0, \"core:frequency\": 915000000, \"core:datetime\": \"2026-03-12T12:00:00.000000000Z\", \"core:header_bytes\": 16},\n"
        "    {\"core:sample_start\": 4096, \"core:frequency\": 916000000}\n"
        "  ],\n"
        "  \"annotations\": [\n"
        "    {\"core:sample_start\": 128, \"core:sample_count\": 256, \"core:label\": \"packet\"},\n"
        "    {\"core:sample_start\": 8192, \"core:freq_lower_edge\": 914900000, \"core:freq_upper_edge\": 915100000, \"core:comment\": \"channel\"}\n"
        "  ]\n"
        "}\n";

    snprintf(data_dir, sizeof(data_dir), "%s/datasets", dir);
    if (mkdir(data_dir, 0700) != 0)
        return expect_true(errno == EEXIST, "create datasets dir");

    snprintf(meta_path, sizeof(meta_path), "%s/manual.sigmf-meta", dir);
    if (write_file(meta_path, json) != 0)
        return expect_true(0, "write manual metadata");
    if (m2sdr_sigmf_read(meta_path, &parsed) != 0)
        return expect_true(0, "read manual metadata");

    if (expect_true(strcmp(parsed.data_path + strlen(parsed.data_path) - strlen("datasets/capture.sigmf-data"),
                           "datasets/capture.sigmf-data") == 0, "relative dataset resolution") != 0 ||
        expect_true(parsed.capture_count == 2, "capture count") != 0 ||
        expect_true(parsed.captures[1].has_center_freq && parsed.captures[1].center_freq == 916000000.0,
                    "second capture frequency") != 0 ||
        expect_true(parsed.has_header_bytes && parsed.header_bytes == 16, "header bytes propagated") != 0 ||
        expect_true(parsed.annotation_count == 2, "manual annotation count") != 0 ||
        expect_true(strcmp(parsed.annotations[0].label, "packet") == 0, "manual annotation label") != 0 ||
        expect_true(parsed.annotations[1].has_freq_upper_edge, "manual annotation freq upper edge") != 0)
        return -1;

    return 0;
}

static int test_multi_capture_multi_annotation(const char *dir)
{
    struct m2sdr_sigmf_meta parsed;
    char meta_path[512];
    const char *json =
        "{\n"
        "  \"global\": {\n"
        "    \"core:version\": \"1.2.5\",\n"
        "    \"core:datatype\": \"ci16_le\",\n"
        "    \"core:dataset\": \"multi.sigmf-data\",\n"
        "    \"core:sample_rate\": 30720000,\n"
        "    \"core:num_channels\": 2\n"
        "  },\n"
        "  \"captures\": [\n"
        "    {\"core:sample_start\": 0, \"core:frequency\": 2400000000},\n"
        "    {\"core:sample_start\": 4096, \"core:frequency\": 2410000000},\n"
        "    {\"core:sample_start\": 8192, \"core:frequency\": 2420000000}\n"
        "  ],\n"
        "  \"annotations\": [\n"
        "    {\"core:sample_start\": 128, \"core:sample_count\": 512, \"core:label\": \"cap0-a\"},\n"
        "    {\"core:sample_start\": 4608, \"core:sample_count\": 256, \"core:label\": \"cap1-a\", \"core:comment\": \"middle\"},\n"
        "    {\"core:sample_start\": 8704, \"core:sample_count\": 128, \"core:freq_lower_edge\": 2419000000, \"core:freq_upper_edge\": 2421000000, \"core:label\": \"cap2-a\"}\n"
        "  ]\n"
        "}\n";

    snprintf(meta_path, sizeof(meta_path), "%s/multi.sigmf-meta", dir);
    if (write_file(meta_path, json) != 0)
        return expect_true(0, "write multi capture metadata");
    if (m2sdr_sigmf_read(meta_path, &parsed) != 0)
        return expect_true(0, "read multi capture metadata");

    if (expect_true(parsed.capture_count == 3, "multi capture count") != 0 ||
        expect_true(parsed.annotation_count == 3, "multi annotation count") != 0 ||
        expect_true(parsed.captures[2].has_center_freq && parsed.captures[2].center_freq == 2420000000.0,
                    "multi capture frequency") != 0 ||
        expect_true(strcmp(parsed.annotations[1].comment, "middle") == 0, "multi annotation comment") != 0 ||
        expect_true(parsed.annotations[2].has_freq_lower_edge && parsed.annotations[2].has_freq_upper_edge,
                    "multi annotation frequency edges") != 0)
        return -1;

    return 0;
}

static int test_capture_sample_range(void)
{
    struct m2sdr_sigmf_meta meta;
    uint64_t start_sample = 0;
    uint64_t end_sample = 0;

    memset(&meta, 0, sizeof(meta));
    meta.capture_count = 3;
    meta.captures[0].sample_start = 0;
    meta.captures[1].sample_start = 4096;
    meta.captures[2].sample_start = 8192;

    if (m2sdr_sigmf_capture_sample_range(&meta, 1, &start_sample, &end_sample) != 0)
        return expect_true(0, "capture range helper succeeds");
    if (expect_true(start_sample == 4096, "capture range start") != 0 ||
        expect_true(end_sample == 8192, "capture range end") != 0)
        return -1;

    if (m2sdr_sigmf_capture_sample_range(&meta, 2, &start_sample, &end_sample) != 0)
        return expect_true(0, "last capture range helper succeeds");
    if (expect_true(start_sample == 8192, "last capture start") != 0 ||
        expect_true(end_sample == 0, "last capture open-ended end") != 0)
        return -1;

    return 0;
}

static int test_timestamp_jump_detection(void)
{
    if (expect_true(!m2sdr_sigmf_timestamp_jump_is_anomalous(1000, 1030, 5.0),
                    "small timestamp delta stays nominal") != 0 ||
        expect_true(m2sdr_sigmf_timestamp_jump_is_anomalous(1000, 1200, 5.0),
                    "large timestamp delta is anomalous") != 0 ||
        expect_true(!m2sdr_sigmf_timestamp_jump_is_anomalous(0, 1200, 5.0),
                    "zero nominal timestamp delta is ignored") != 0)
        return -1;
    return 0;
}

static int test_capture_byte_range(void)
{
    struct m2sdr_sigmf_meta meta;
    uint64_t start_offset = 0;
    uint64_t end_offset = 0;

    memset(&meta, 0, sizeof(meta));
    meta.capture_count = 2;
    meta.captures[0].sample_start = 0;
    meta.captures[1].sample_start = 4;

    if (m2sdr_sigmf_capture_byte_range(&meta, 1, M2SDR_FORMAT_SC16_Q11, 0, 8192,
                                       &start_offset, &end_offset) != 0)
        return expect_true(0, "headerless capture byte range succeeds");
    if (expect_true(start_offset == 16, "headerless capture start offset") != 0 ||
        expect_true(end_offset == 0, "headerless capture end offset") != 0)
        return -1;

    meta.captures[1].sample_start = 2044;
    if (m2sdr_sigmf_capture_byte_range(&meta, 1, M2SDR_FORMAT_SC16_Q11, 16, 8192,
                                       &start_offset, &end_offset) != 0)
        return expect_true(0, "headered aligned capture byte range succeeds");
    if (expect_true(start_offset == 8192, "headered aligned capture start offset") != 0)
        return -1;

    meta.captures[1].sample_start = 100;
    if (expect_true(m2sdr_sigmf_capture_byte_range(&meta, 1, M2SDR_FORMAT_SC16_Q11, 16, 8192,
                                                   &start_offset, &end_offset) != 0,
                    "headered misaligned capture byte range fails") != 0)
        return -1;

    return 0;
}

int main(void)
{
    char template[] = "/tmp/m2sdr_sigmf_testXXXXXX";
    char *dir;

    dir = mkdtemp(template);
    if (!dir) {
        perror("mkdtemp");
        return 1;
    }

    if (test_derive_paths() != 0 ||
        test_roundtrip(dir) != 0 ||
        test_manual_metadata(dir) != 0 ||
        test_multi_capture_multi_annotation(dir) != 0 ||
        test_capture_sample_range() != 0 ||
        test_timestamp_jump_detection() != 0 ||
        test_capture_byte_range() != 0)
        return 1;

    printf("test_m2sdr_sigmf: ok\n");
    return 0;
}
