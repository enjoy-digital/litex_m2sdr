/* SPDX-License-Identifier: BSD-2-Clause */

#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "m2sdr_sigmf.h"

static int write_bytes(const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        return -1;
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int write_text(const char *path, const char *text)
{
    return write_bytes(path, (const unsigned char *)text, strlen(text));
}

static int check_capture_payload(const char *data_path, uint64_t start_sample, uint64_t end_sample)
{
    unsigned char buf[32];
    FILE *f;
    size_t len;
    size_t sample_size = 4;

    f = fopen(data_path, "rb");
    if (!f)
        return -1;
    if (fseeko(f, (off_t)(start_sample * sample_size), SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    len = fread(buf, 1, (size_t)((end_sample - start_sample) * sample_size), f);
    fclose(f);
    if (len != (size_t)((end_sample - start_sample) * sample_size))
        return -1;

    return (buf[0] == 0x10 && buf[1] == 0x11 && buf[2] == 0x12 && buf[3] == 0x13) ? 0 : -1;
}

int main(void)
{
    char template[] = "/tmp/m2sdr_sigmf_smokeXXXXXX";
    char *dir;
    char meta_path[512];
    char data_path[512];
    struct m2sdr_sigmf_meta meta;
    uint64_t start_sample = 0;
    uint64_t end_sample = 0;
    unsigned char payload[32];
    const char *json =
        "{\n"
        "  \"global\": {\n"
        "    \"core:version\": \"1.2.5\",\n"
        "    \"core:datatype\": \"ci16_le\",\n"
        "    \"core:dataset\": \"capture.sigmf-data\",\n"
        "    \"core:sample_rate\": 1000000,\n"
        "    \"core:num_channels\": 1\n"
        "  },\n"
        "  \"captures\": [\n"
        "    {\"core:sample_start\": 0},\n"
        "    {\"core:sample_start\": 4}\n"
        "  ]\n"
        "}\n";
    int i;

    dir = mkdtemp(template);
    if (!dir) {
        perror("mkdtemp");
        return 1;
    }

    for (i = 0; i < 32; i++)
        payload[i] = (unsigned char)i;

    snprintf(meta_path, sizeof(meta_path), "%s/capture.sigmf-meta", dir);
    snprintf(data_path, sizeof(data_path), "%s/capture.sigmf-data", dir);

    if (write_bytes(data_path, payload, sizeof(payload)) != 0 ||
        write_text(meta_path, json) != 0) {
        perror("write");
        return 1;
    }
    if (m2sdr_sigmf_read(meta_path, &meta) != 0) {
        fprintf(stderr, "Could not read generated SigMF metadata\n");
        return 1;
    }
    if (m2sdr_sigmf_capture_sample_range(&meta, 1, &start_sample, &end_sample) != 0) {
        fprintf(stderr, "Could not derive capture sample range\n");
        return 1;
    }
    if (start_sample != 4 || end_sample != 0) {
        fprintf(stderr, "Unexpected capture range: start=%" PRIu64 " end=%" PRIu64 "\n",
                start_sample, end_sample);
        return 1;
    }
    end_sample = 8;
    if (check_capture_payload(data_path, start_sample, end_sample) != 0) {
        fprintf(stderr, "Capture payload selection smoke test failed\n");
        return 1;
    }

    printf("test_sigmf_capture_smoke: ok\n");
    return 0;
}
