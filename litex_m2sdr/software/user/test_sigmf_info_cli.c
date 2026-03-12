/* SPDX-License-Identifier: BSD-2-Clause */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        return -1;
    if (fwrite(text, 1, strlen(text), f) != strlen(text)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int write_bytes(const char *path, const void *data, size_t len)
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

static int run_and_check(const char *cmd, const char *expect_text, int expect_exit)
{
    FILE *p;
    char buf[1024];
    size_t len = 0;
    int rc;

    p = popen(cmd, "r");
    if (!p)
        return -1;
    while (fgets(buf + len, (int)(sizeof(buf) - len), p) != NULL) {
        len = strlen(buf);
        if (len >= sizeof(buf) - 1)
            break;
    }
    rc = pclose(p);
    if (!WIFEXITED(rc) || WEXITSTATUS(rc) != expect_exit)
        return -1;
    return strstr(buf, expect_text) ? 0 : -1;
}

int main(void)
{
    char template[] = "/tmp/m2sdr_sigmf_cliXXXXXX";
    char *dir;
    char ok_meta[512];
    char bad_meta[512];
    char freq_meta[512];
    char dup_meta[512];
    char ok_data[512];
    char cmd[1024];
    const char *ok_json =
        "{\n"
        "  \"global\": {\n"
        "    \"core:version\": \"1.2.5\",\n"
        "    \"core:datatype\": \"ci16_le\",\n"
        "    \"core:dataset\": \"ok.sigmf-data\",\n"
        "    \"core:sample_rate\": 1000000,\n"
        "    \"core:num_channels\": 1\n"
        "  },\n"
        "  \"captures\": [\n"
        "    {\"core:sample_start\": 0}\n"
        "  ]\n"
        "}\n";
    const char *bad_json =
        "{\n"
        "  \"global\": {\n"
        "    \"core:version\": \"1.2.5\",\n"
        "    \"core:datatype\": \"ci16_le\",\n"
        "    \"core:dataset\": \"ok.sigmf-data\",\n"
        "    \"core:sample_rate\": 1000000,\n"
        "    \"core:num_channels\": 1\n"
        "  },\n"
        "  \"captures\": [\n"
        "    {\"core:sample_start\": 100},\n"
        "    {\"core:sample_start\": 200}\n"
        "  ],\n"
        "  \"annotations\": [\n"
        "    {\"core:sample_start\": 50, \"core:sample_count\": 300, \"core:label\": \"bad\"}\n"
        "  ]\n"
        "}\n";
    const char *freq_json =
        "{\n"
        "  \"global\": {\n"
        "    \"core:version\": \"1.2.5\",\n"
        "    \"core:datatype\": \"ci16_le\",\n"
        "    \"core:dataset\": \"ok.sigmf-data\",\n"
        "    \"core:sample_rate\": 1000000,\n"
        "    \"core:num_channels\": 1\n"
        "  },\n"
        "  \"captures\": [\n"
        "    {\"core:sample_start\": 0}\n"
        "  ],\n"
        "  \"annotations\": [\n"
        "    {\"core:sample_start\": 0, \"core:sample_count\": 16, \"core:freq_lower_edge\": 2000, \"core:freq_upper_edge\": 1000, \"core:label\": \"bad-freq\"}\n"
        "  ]\n"
        "}\n";
    const char *dup_json =
        "{\n"
        "  \"global\": {\n"
        "    \"core:version\": \"1.2.5\",\n"
        "    \"core:datatype\": \"ci16_le\",\n"
        "    \"core:dataset\": \"ok.sigmf-data\",\n"
        "    \"core:sample_rate\": 1000000,\n"
        "    \"core:num_channels\": 1\n"
        "  },\n"
        "  \"captures\": [\n"
        "    {\"core:sample_start\": 0},\n"
        "    {\"core:sample_start\": 0}\n"
        "  ]\n"
        "}\n";

    dir = mkdtemp(template);
    if (!dir) {
        perror("mkdtemp");
        return 1;
    }

    snprintf(ok_meta, sizeof(ok_meta), "%s/ok.sigmf-meta", dir);
    snprintf(bad_meta, sizeof(bad_meta), "%s/bad.sigmf-meta", dir);
    snprintf(freq_meta, sizeof(freq_meta), "%s/freq.sigmf-meta", dir);
    snprintf(dup_meta, sizeof(dup_meta), "%s/dup.sigmf-meta", dir);
    snprintf(ok_data, sizeof(ok_data), "%s/ok.sigmf-data", dir);

    if (write_text(ok_meta, ok_json) != 0 ||
        write_text(bad_meta, bad_json) != 0 ||
        write_text(freq_meta, freq_json) != 0 ||
        write_text(dup_meta, dup_json) != 0 ||
        write_bytes(ok_data, "\0\0\0\0", 4) != 0) {
        perror("write");
        return 1;
    }

    snprintf(cmd, sizeof(cmd), "./m2sdr_sigmf_info --validate --strict --ci %s", ok_meta);
    if (run_and_check(cmd, "VALIDATION status=ok", 0) != 0) {
        fprintf(stderr, "ok validation CLI check failed\n");
        return 1;
    }

    snprintf(cmd, sizeof(cmd), "./m2sdr_sigmf_info --validate --strict --ci %s", bad_meta);
    if (run_and_check(cmd, "VALIDATION status=fail", 1) != 0) {
        fprintf(stderr, "bad validation CLI check failed\n");
        return 1;
    }

    snprintf(cmd, sizeof(cmd), "./m2sdr_sigmf_info --validate --strict --ci %s", freq_meta);
    if (run_and_check(cmd, "VALIDATION status=fail", 1) != 0) {
        fprintf(stderr, "freq validation CLI check failed\n");
        return 1;
    }

    snprintf(cmd, sizeof(cmd), "./m2sdr_sigmf_info --validate --strict --ci %s", dup_meta);
    if (run_and_check(cmd, "VALIDATION status=fail", 1) != 0) {
        fprintf(stderr, "duplicate capture validation CLI check failed\n");
        return 1;
    }

    printf("test_sigmf_info_cli: ok\n");
    return 0;
}
